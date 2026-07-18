-- lib/emoji_popup.lua — the shared emoji picker popup.
--
-- Two consumers:
--   * Settings > Emoji calls M.open{} in assign mode (pick sets a key, then
--     the popup closes; a "None" extra button clears the key).
--   * Alt+mic while typing fires M.on_shortcut() (dispatch_emoji_popup in
--     loop()): the same picker opens over the running app as an INSERT popup —
--     each pick inserts into the textarea the firmware captured at press time
--     (_emoji_popup_insert), and it stays open until Close / a second alt+mic.
--
-- Modal overlay pattern (see the Messenger popups / topbar panel): CLICKABLE
-- overlay blocks the page below, every focusable is a DIRECT child of the
-- pushed box (gridnav invariant), nav.pop() runs BEFORE overlay:delete().
local lvgl = require("lvgl")
local nav  = require("lib/nav")

local M = {}

local PAGE = 24

-- Manual UTF-8 encode (the sandbox may not expose the utf8 lib). Handles the
-- 3-byte PUA range and 4-byte emoji planes.
function M.ucp(cp)
    if cp < 0x80 then return string.char(cp) end
    if cp < 0x800 then
        return string.char(0xC0 + math.floor(cp / 0x40), 0x80 + cp % 0x40)
    end
    if cp < 0x10000 then
        return string.char(0xE0 + math.floor(cp / 0x1000),
                           0x80 + math.floor(cp / 0x40) % 0x40,
                           0x80 + cp % 0x40)
    end
    return string.char(0xF0 + math.floor(cp / 0x40000),
                       0x80 + math.floor(cp / 0x1000) % 0x40,
                       0x80 + math.floor(cp / 0x40) % 0x40,
                       0x80 + cp % 0x40)
end

-- M.open{ parent=?, title=?, on_pick=fn(cp), close_on_pick=?, extra=?,
--         close_label=?, on_close=? } -> close fn, or nil if the blob is empty.
--   parent        overlay parent; nil = parentless over everything (topbar
--                 panel pattern), pass the app root to die with the app.
--   on_pick(cp)   called on a cell tap; return false to flag a failure in the
--                 page counter (nil/true proceed).
--   close_on_pick close the popup after a successful pick.
--   extra         { label=, cb= } optional fourth button; cb then close.
--   on_close      runs after the popup is deleted (any close path).
function M.open(opts)
    opts = opts or {}
    local total = _emoji_blob_count and _emoji_blob_count() or 0
    if total == 0 then return nil end

    local W = lvgl.HOR_RES()
    local H = lvgl.VER_RES()

    local ovargs = {
        w = W, h = H, x = 0, y = 0,
        bg_color = "#000000", bg_opa = 128, border_width = 0, pad_all = 0,
    }
    local overlay = opts.parent and opts.parent:Object(ovargs) or lvgl.Object(ovargs)
    overlay:clear_flag(lvgl.FLAG.SCROLLABLE)
    overlay:add_flag(lvgl.FLAG.CLICKABLE)   -- modal: swallow taps on the dim area
    if not opts.parent then pcall(_obj_move_foreground, overlay) end

    local box = overlay:Object {
        w = W - 20, h = lvgl.SIZE_CONTENT, align = lvgl.ALIGN.CENTER,
        bg_color = "#333333", radius = 6,
        border_width = 1, border_color = "#555555", pad_all = 6, pad_row = 2,
        -- Explicit pad_column packs the 42px cells 6-per-row (theme default
        -- gave 5); max_height + native scroll is the fallback if a theme font
        -- ever grows the rows past the screen again.
        pad_column = 4, max_height = H - 6,
        flex = { flex_direction = "row", flex_wrap = "wrap" },
    }
    nav.push(box)

    local function close()
        nav.pop()
        overlay:delete()
        if opts.on_close then pcall(opts.on_close) end
    end

    -- Single header row: title + search + Go + page counter. (A separate title
    -- row pushed the box past the 240px screen once search was added.)
    box:Label { text = opts.title or "", w = lvgl.PCT(10), h = 28 }

    -- Fixed pool of PAGE cells, created once; paging only rewrites labels and
    -- the cps[] slots (handlers registered once — never re-bound per page).
    local start = 1
    local show_page   -- forward decl (header row sits above the cells)

    -- Codepoint search: jump the grid to a hex codepoint (nearest entry at or
    -- after it). The index is codepoint-sorted, so this doubles as a category
    -- jump: 1F300 weather, 1F400 animals, 1F600 smileys, E000 sequences.
    -- Lua-side binary search over _emoji_blob_list — no firmware call per cell.
    local function blob_lower_bound(cp)
        local lo, hi = 1, total + 1
        while lo < hi do
            local mid = math.floor((lo + hi) / 2)
            local v = (_emoji_blob_list(mid, 1) or {})[1]
            if not v then break end
            if v < cp then lo = mid + 1 else hi = mid end
        end
        if lo > total then lo = total end
        return lo
    end

    local search_ta = box:Textarea {
        password_mode = false, one_line = true,
        placeholder_text = "hex 1F600",
        w = lvgl.PCT(38), h = 28,
    }
    search_ta:clear_flag(lvgl.FLAG.SCROLLABLE)

    local go_b = box:Button { w = lvgl.PCT(15), h = 28 }
    go_b:Label { text = "Go", align = lvgl.ALIGN.CENTER }

    local page_lbl = box:Label { text = "", w = lvgl.PCT(32), h = 28 }

    local function do_jump()
        local q = (search_ta.text or ""):gsub("%s+", ""):gsub("^[Uu]%+?", "")
        local cp = tonumber(q, 16)
        if not cp or cp <= 0 then
            page_lbl:set { text = "bad hex" }
            return
        end
        local idx = blob_lower_bound(cp)
        start = idx - ((idx - 1) % PAGE)   -- align to the page holding it
        show_page()
    end
    go_b:onClicked(do_jump)
    search_ta:onevent(lvgl.EVENT.KEY, function()
        if lvgl.indev.get_act():get_key() == lvgl.KEY.ENTER then do_jump() end
    end)

    local cps = {}
    local cells = {}
    for i = 1, PAGE do
        local b = box:Button { w = 42, h = 30 }
        local l = b:Label { text = "", align = lvgl.ALIGN.CENTER }
        cells[i] = { btn = b, lbl = l }
        b:onClicked(function()
            local cp = cps[i]
            if not cp then return end
            local ok = true
            if opts.on_pick then ok = opts.on_pick(cp) end
            if ok == false then
                page_lbl:set { text = "failed" }
                return
            end
            if opts.close_on_pick then close() end
        end)
    end

    show_page = function()
        local list = _emoji_blob_list(start, PAGE)
        for i = 1, PAGE do
            local cp = list[i]
            cps[i] = cp
            if cp then
                pcall(_emoji_preload, cp)
                cells[i].lbl:set { text = M.ucp(cp) }
                cells[i].btn:clear_flag(lvgl.FLAG.HIDDEN)
            else
                cells[i].btn:add_flag(lvgl.FLAG.HIDDEN)
            end
        end
        local last = math.min(start + PAGE - 1, total)
        page_lbl:set { text = start .. "-" .. last .. "/" .. total }
    end

    local bw = opts.extra and 23 or 31   -- 4 buttons at 23%, 3 at 31%

    local prev_b = box:Button { w = lvgl.PCT(bw), h = 26 }
    prev_b:Label { text = "< Prev", align = lvgl.ALIGN.CENTER }
    prev_b:onClicked(function()
        start = math.max(1, start - PAGE)
        show_page()
    end)

    local next_b = box:Button { w = lvgl.PCT(bw), h = 26 }
    next_b:Label { text = "Next >", align = lvgl.ALIGN.CENTER }
    next_b:onClicked(function()
        if start + PAGE <= total then start = start + PAGE end
        show_page()
    end)

    if opts.extra then
        local ex_b = box:Button { w = lvgl.PCT(bw), h = 26 }
        ex_b:Label { text = opts.extra.label, align = lvgl.ALIGN.CENTER }
        ex_b:onClicked(function()
            if opts.extra.cb then opts.extra.cb() end
            close()
        end)
    end

    local close_b = box:Button { w = lvgl.PCT(bw), h = 26 }
    close_b:Label { text = opts.close_label or "Cancel", align = lvgl.ALIGN.CENTER }
    close_b:onClicked(close)

    show_page()
    return close
end

-- ── Alt+mic insert popup ────────────────────────────────────────────────────
-- The firmware only dispatches while a textarea is focused, and has already
-- captured it as the insert target. A second alt+mic closes the popup: the
-- press re-captures the popup's own hex-search box as the target, which is
-- harmless — close throws it away via _emoji_popup_release.
local insert_close = nil   -- close fn while the insert popup is up

-- Close the insert popup if open (no-op otherwise). The overlay is parentless,
-- so an app teardown (apps.home_shortcut) must close it explicitly or it
-- would linger over the launcher.
function M.close()
    if not insert_close then return end
    local c = insert_close
    insert_close = nil
    pcall(c)
end

function M.on_shortcut()
    if insert_close then
        M.close()
        return
    end
    if not _emoji_popup_insert then return end
    insert_close = M.open {
        close_on_pick = false,
        close_label = "Close",
        on_pick = function(cp) return _emoji_popup_insert(cp) end,
        on_close = function()
            insert_close = nil
            pcall(_emoji_popup_release)
        end,
    }
end

return M
