-- lib/osk.lua — full-screen on-screen keyboard for touch boards.
--
-- Opened by the firmware (dispatch_osk in loop()) when a textarea gains
-- focus or is re-tapped, on any touch board whose touch mode is not OFF.
-- Full-screen modal: a one-line PREVIEW textarea on top (seeded with the
-- target field's current text), and below it either the PunkKeyboard widget
-- (typing mode) or an embedded emoji grid (emoji mode) — the :)/abc button
-- swaps the two in place, the same way the keyboard's own number key swaps
-- key sets. The keyboard's check key commits the preview text into the app's
-- textarea via _osk_commit (which re-validates the captured target) and
-- closes the modal; it is the only key that closes it. The keyboard key
-- beside it toggles the widget's big-key layout and stays in the modal.
--
-- Emoji mode shows the USER'S emoji set first: the per-key alt-layer map
-- from Settings > Emoji (_kb_emoji_get, the same assignments the T-Deck
-- types with alt+key), laid out in the qwerty arrangement so muscle memory
-- transfers. A "More" key swaps to the full blob browser (pages of 24 via
-- _emoji_blob_count/_emoji_blob_list) and "Set" returns. Every pick is
-- gated on _emoji_preload so no tofu is committed.
--
-- Modal overlay pattern per the house rules: CLICKABLE overlay swallows
-- taps, teardown is pcall-guarded, and the DELETE event releases the
-- firmware-side capture even when the app underneath tears down first.

local lvgl = require("lvgl")
local emoji_popup = require("lib/emoji_popup")   -- for ucp() only

local M = {}

local PAGE = 24   -- emoji cells per page (6 x 4)

local overlay = nil   -- modal root; nil = closed

local function teardown()
    if not overlay then return end
    local ov = overlay
    overlay = nil
    pcall(_osk_release)
    pcall(function() ov:delete() end)
end

function M.open()
    if overlay then return end
    _osk_set_active(true)

    local W = lvgl.HOR_RES()
    local H = lvgl.VER_RES()

    overlay = lvgl.Object {
        w = W, h = H, x = 0, y = 0,
        bg_color = "#000000", bg_opa = 255, border_width = 0,
        pad_all = 0, pad_row = 4,
        flex = { flex_direction = "column" },
    }
    overlay:clear_flag(lvgl.FLAG.SCROLLABLE)
    overlay:add_flag(lvgl.FLAG.CLICKABLE)
    pcall(_obj_move_foreground, overlay)

    -- The firmware capture must not outlive the modal, whatever kills it
    -- (OK, cancel, or the app tearing down the screen under us).
    overlay:onevent(lvgl.EVENT.DELETE, function()
        if overlay then
            overlay = nil
            pcall(_osk_release)
        end
    end)

    -- Top row: mode toggle + preview textarea.
    local top = overlay:Object {
        w = W, h = 40, bg_opa = 0, border_width = 0,
        pad_all = 4, pad_column = 4,
        flex = { flex_direction = "row" },
    }
    top:clear_flag(lvgl.FLAG.SCROLLABLE)

    local mode_b = top:Button { w = 44, h = 32 }
    local mode_lbl = mode_b:Label { text = ":)", align = lvgl.ALIGN.CENTER }

    local preview = top:Textarea {
        one_line = true, w = W - 60, h = 32,
        text = _osk_initial_text() or "",
    }
    preview:clear_flag(lvgl.FLAG.SCROLLABLE)

    -- Typing mode: the firmware's own keyboard widget, typing into the
    -- preview. Its check key fires READY (commit + close); its keyboard key
    -- swaps the normal and big-key layouts inside the widget and sends
    -- nothing here. Nothing emits CANCEL any more, so check is the way out.
    local kb = overlay:PunkKeyboard { w = W, h = H - 48 }
    kb:set_textarea(preview)

    kb:onevent(lvgl.EVENT.READY, function()
        pcall(_osk_commit, preview.text or "")
        teardown()
    end)

    -- Shared pick handler: gate on the active blob, append to the preview.
    local function pick(cp)
        if not cp or cp <= 0 then return end
        if _emoji_preload and not _emoji_preload(cp) then return end
        preview:set { text = (preview.text or "") .. emoji_popup.ucp(cp) }
    end

    -- Emoji layer A: the user's per-key emoji set (Settings > Emoji) in the
    -- qwerty arrangement — the alt-layer map the T-Deck types with alt+key.
    local ROWS = { "qwertyuiop", "asdfghjkl", "zxcvbnm$" }
    local upanel = nil

    local function build_user_panel(on_more)
        upanel = overlay:Object {
            w = W, h = H - 48, bg_opa = 0, border_width = 0,
            pad_all = 2, pad_row = 6,
            flex = { flex_direction = "column" },
        }
        upanel:clear_flag(lvgl.FLAG.SCROLLABLE)

        for r = 1, #ROWS do
            local row = upanel:Object {
                w = W - 8, h = 42, bg_opa = 0, border_width = 0,
                pad_all = 0, pad_column = 3,
                flex = { flex_direction = "row" },
            }
            row:clear_flag(lvgl.FLAG.SCROLLABLE)
            local keys = ROWS[r]
            for k = 1, #keys do
                local ch = keys:sub(k, k)
                local cp = _kb_emoji_get and _kb_emoji_get(ch) or 0
                if cp and cp > 0 then
                    local c = row:Button { w = 28, h = 40 }
                    c:Label { text = emoji_popup.ucp(cp), align = lvgl.ALIGN.CENTER }
                    c:onClicked(function() pick(cp) end)
                end
            end
            -- The last row carries the browser switch.
            if r == #ROWS then
                local more_b = row:Button { w = 62, h = 40 }
                more_b:Label { text = "More", align = lvgl.ALIGN.CENTER }
                more_b:onClicked(on_more)
            end
        end
    end

    -- Emoji layer B: the full blob browser — paged grid, fixed pool of PAGE
    -- cells, paging rewrites labels only (the shared picker's pattern).
    local total = _emoji_blob_count and _emoji_blob_count() or 0
    local epanel = nil
    local cells, cps = {}, {}
    local start = 1
    local page_lbl = nil

    local function show_page()
        local list = _emoji_blob_list(start, PAGE) or {}
        for i = 1, PAGE do
            local cp = list[i]
            cps[i] = cp
            if cp then
                cells[i]:set { text = emoji_popup.ucp(cp) }
                cells[i]:clear_flag(lvgl.FLAG.HIDDEN)
            else
                cells[i]:add_flag(lvgl.FLAG.HIDDEN)
            end
        end
        local last = math.min(start + PAGE - 1, total)
        page_lbl:set { text = start .. "-" .. last .. "/" .. total }
    end

    local function build_browser_panel(on_back)
        epanel = overlay:Object {
            w = W, h = H - 48, bg_opa = 0, border_width = 0,
            pad_all = 4, pad_row = 2, pad_column = 4,
            flex = { flex_direction = "row", flex_wrap = "wrap" },
        }
        epanel:clear_flag(lvgl.FLAG.SCROLLABLE)

        for i = 1, PAGE do
            local c = epanel:Button { w = 46, h = 34 }
            local l = c:Label { text = "", align = lvgl.ALIGN.CENTER }
            cells[i] = l
            c:onClicked(function() pick(cps[i]) end)
        end

        local back_b = epanel:Button { w = 56, h = 30 }
        back_b:Label { text = "Set", align = lvgl.ALIGN.CENTER }
        back_b:onClicked(on_back)

        local prev_b = epanel:Button { w = 50, h = 30 }
        prev_b:Label { text = "<", align = lvgl.ALIGN.CENTER }
        prev_b:onClicked(function()
            start = start - PAGE
            if start < 1 then
                start = total - ((total - 1) % PAGE)   -- wrap to last page
                if start < 1 then start = 1 end
            end
            show_page()
        end)

        page_lbl = epanel:Label { text = "", w = 100, h = 30 }

        local next_b = epanel:Button { w = 50, h = 30 }
        next_b:Label { text = ">", align = lvgl.ALIGN.CENTER }
        next_b:onClicked(function()
            start = start + PAGE
            if start > total then start = 1 end
            show_page()
        end)

        show_page()
    end

    -- Mode plumbing: typing <-> emoji (layer A default; B via More/Set).
    local emoji_mode = false

    local function show_browser()
        if not epanel then build_browser_panel(function()
            epanel:add_flag(lvgl.FLAG.HIDDEN)
            upanel:clear_flag(lvgl.FLAG.HIDDEN)
        end) end
        upanel:add_flag(lvgl.FLAG.HIDDEN)
        epanel:clear_flag(lvgl.FLAG.HIDDEN)
    end

    mode_b:onClicked(function()
        emoji_mode = not emoji_mode
        if emoji_mode then
            if not upanel then build_user_panel(show_browser) end
            kb:add_flag(lvgl.FLAG.HIDDEN)
            if epanel then epanel:add_flag(lvgl.FLAG.HIDDEN) end
            upanel:clear_flag(lvgl.FLAG.HIDDEN)
            mode_lbl:set { text = "abc" }
        else
            if upanel then upanel:add_flag(lvgl.FLAG.HIDDEN) end
            if epanel then epanel:add_flag(lvgl.FLAG.HIDDEN) end
            kb:clear_flag(lvgl.FLAG.HIDDEN)
            mode_lbl:set { text = ":)" }
        end
    end)
end

-- Home-shortcut / app-teardown hook (parentless-popup close convention).
function M.close()
    teardown()
end

return M
