--[[
  Images — image viewer for MeshPunk.

  Browsing is lib/fileman (same drives, same listing, same rules as Tools/Files,
  filtered to the decodable formats: PNG, baseline JPEG and RGB565 .bin).
  Display is lib/imgview, which owns the two view modes:

    Fit   whole image scaled to the screen (box-filtered on load, not at
          draw time — no per-frame cost)
    1:1   native pixels, panned with drag / arrows / trackball

  Tap the image (or press M) for the toolbar; ESC steps back a level.
]]

local lvgl    = require("lvgl")
local apps    = require("lib/apps")
local nav     = require("lib/nav")
local theme   = require("lib/theme")
local utils   = require("lib/utils")
local fileman = require("lib/fileman")
local imgview = require("lib/imgview")

local W = lvgl.HOR_RES()
local H = lvgl.VER_RES()

local MAX_SHOW = 150      -- listing rows built per directory (UI cap)
local PAN_STEP = 40       -- pixels per arrow-key / trackball nudge
local EXT = { png = true, jpg = true, jpeg = true, bin = true }

-- ── App state ────────────────────────────────────────────────────────────────
local cur_path = nil      -- current directory, nil = drive picker
local shots = {}          -- image entries of cur_path, in listing order
local idx = 0             -- index into shots being viewed
local view = nil          -- live imgview (viewer only)
local bar = nil           -- toolbar overlay (viewer only)

-- Reclaim any buffer a previous run leaked (a crash between open and close);
-- the slots are C-side and outlive the Lua state.
pcall(_img_close)

local root = apps.new_root()
root:set { w = W, h = H, pad_all = 0, border_width = 0, bg_opa = 0 }
root:clear_flag(lvgl.FLAG.SCROLLABLE)
theme.show_background()

local vw = nil            -- current view container

local show_drives, show_browser, show_viewer, hide_bar   -- forward declarations

local function toast(msg)
    pcall(utils.createNotification, root, tostring(msg), 2500)
end

-- Drop the viewer's PSRAM buffers before anything can delete its widgets:
-- nothing may draw from a buffer being freed.
local function drop_view()
    hide_bar()
    if view then
        view:close()
        view = nil
    end
end

-- Build a fresh full-screen view container, tear the old one down safely.
local function swap_view(builder)
    local old = vw
    vw = root:Object {
        w = W, h = H, x = 0, y = 0,
        bg_opa = 0, border_width = 0, pad_all = 0, radius = 0,
    }
    vw:clear_flag(lvgl.FLAG.SCROLLABLE)
    builder(vw)
    if old then apps.delete_view(old) end
end

local function quit_app()
    drop_view()
    apps.go_home()
end

-- Structural safety net: the home chord tears the app down without running
-- quit_app, and the image buffers are C-side (nothing else frees them).
apps.set_on_close(drop_view)

-- ── Browser ──────────────────────────────────────────────────────────────────

show_drives = function()
    cur_path = nil
    drop_view()
    swap_view(function(v)
        local col = v:Object {
            w = W, h = H, x = 0, y = 0,
            bg_opa = 0, border_width = 0, pad_all = 8,
            flex = { flex_direction = "column", flex_wrap = "nowrap" },
        }
        nav.replace(col, { flags = nav.ROLLOVER })

        col:Label { text = "Images", w = lvgl.PCT(100), h = 22 }

        for _, d in ipairs(fileman.drives()) do
            local sub
            if not d.mounted then
                sub = "not mounted"
            elseif d.total then
                sub = fileman.size_str(d.used or 0) .. " / "
                    .. fileman.size_str(d.total) .. " used"
            else
                sub = "mounted"
            end
            local b = col:Button { w = lvgl.PCT(100), h = 52 }
            b:Label { text = d.label .. "  (" .. d.id .. ":)", align = lvgl.ALIGN.TOP_LEFT }
            b:Label { text = sub, align = lvgl.ALIGN.BOTTOM_LEFT }
            local drive_root = d.root
            if d.mounted then
                b:onClicked(function() show_browser(drive_root) end)
            else
                b:onClicked(function() toast(d.label .. " not mounted") end)
            end
        end

        local quit_btn = col:Button { w = lvgl.PCT(100), h = 30 }
        quit_btn:Label { text = "Quit", align = lvgl.ALIGN.CENTER }
        quit_btn:onClicked(quit_app)
    end)
end

show_browser = function(path)
    cur_path = fileman.normalize(path)
    drop_view()

    -- Folders stay visible for navigation; files are filtered to what the
    -- decoders actually handle, so a photo folder isn't buried in .txt rows.
    local entries, list_err = fileman.list(cur_path, {
        filter = function(e)
            return e.type == "dir" or EXT[fileman.ext(e.name) or ""] == true
        end,
    })

    shots = {}
    for _, e in ipairs(entries or {}) do
        if e.type == "file" then shots[#shots + 1] = e end
    end

    swap_view(function(v)
        -- One flat flex-wrap scope: toolbar buttons and rows are all direct
        -- children, so a single gridnav covers everything (Files-style).
        local content = v:Object {
            w = W, h = H, x = 0, y = 0,
            bg_opa = 0, border_width = 0, pad_all = 4,
            flex = { flex_direction = "row", flex_wrap = "wrap" },
        }
        nav.replace(content, { flags = nav.ROLLOVER + nav.SCROLL_FIRST })

        local disp = cur_path
        if #disp > 36 then disp = "..." .. disp:sub(-33) end
        content:Label { text = disp, w = lvgl.PCT(100), h = 16 }

        local function tool(text, width, fn)
            local b = content:Button { w = width, h = 24 }
            b:Label { text = text, align = lvgl.ALIGN.CENTER }
            b:onClicked(fn)
            return b
        end

        tool("Up", 44, function()
            local p = fileman.parent(cur_path)
            if p then show_browser(p) else show_drives() end
        end)
        tool("Drives", 60, show_drives)
        tool("Quit", 50, quit_app)

        if not entries then
            content:Label {
                text = "Cannot list: " .. tostring(list_err),
                w = lvgl.PCT(100), h = 40,
            }
            entries = {}
        end

        local shown, shot_no = 0, 0
        for _, e in ipairs(entries) do
            if shown >= MAX_SHOW then break end
            shown = shown + 1

            local row = content:Button { w = lvgl.PCT(100), h = 24 }
            local name = e.name
            if #name > 28 then name = name:sub(1, 27) .. "~" end
            row:Label {
                text = (e.type == "dir") and (name .. "/") or name,
                align = lvgl.ALIGN.LEFT_MID,
            }
            if e.type == "file" then
                row:Label { text = fileman.size_str(e.size), align = lvgl.ALIGN.RIGHT_MID }
                shot_no = shot_no + 1
                local n = shot_no
                row:onClicked(function() show_viewer(n) end)
            else
                local sub = fileman.join(cur_path, e.name)
                row:onClicked(function() show_browser(sub) end)
            end
        end

        if #entries == 0 then
            content:Label { text = "(no images here)", w = lvgl.PCT(100), h = 24 }
        elseif #entries > MAX_SHOW then
            content:Label {
                text = "(+" .. (#entries - MAX_SHOW) .. " more not shown)",
                w = lvgl.PCT(100), h = 20,
            }
        end

        content:Label {
            text = #shots .. " image" .. (#shots == 1 and "" or "s"),
            w = lvgl.PCT(100), h = 16,
        }
    end)
end

-- ── Viewer ───────────────────────────────────────────────────────────────────

local function back_to_browser()
    drop_view()
    show_browser(cur_path)
end

-- Load shots[n] into the live viewer. Wraps around at both ends.
local function load_shot(n)
    if #shots == 0 then return end
    idx = ((n - 1) % #shots) + 1
    local entry = shots[idx]
    local ok, err = view:load(fileman.join(cur_path, entry.name))
    if not ok then toast(err or "Cannot open") end
end

local function show_info()
    local i = view:info()
    if not i.loaded then return end
    local name = shots[idx] and shots[idx].name or "?"
    toast(name .. "  " .. i.w .. "x" .. i.h
        .. ((i.div and i.div > 1) and ("  1/" .. i.div .. " scale") or "")
        .. "  [" .. i.mode .. "]  " .. idx .. "/" .. #shots)
end

local show_bar

hide_bar = function()
    if not bar then return end
    nav.pop()
    bar:delete()
    bar = nil
end

show_bar = function(keyscope)
    if bar then
        hide_bar()
        return
    end
    bar = vw:Object {
        w = W, h = 30, x = 0, y = H - 30,
        bg_opa = 220, border_width = 0, pad_all = 2, radius = 0,
        flex = { flex_direction = "row", flex_wrap = "nowrap" },
    }
    bar:clear_flag(lvgl.FLAG.SCROLLABLE)

    local function item(text, width, fn)
        local b = bar:Button { w = width, h = 26 }
        b:Label { text = text, align = lvgl.ALIGN.CENTER }
        b:onClicked(fn)
        return b
    end

    item(view.mode == "fit" and "1:1" or "Fit", 46, function()
        view:toggle()
        hide_bar()
        lvgl.group.focus_obj(keyscope)
    end)
    item("<", 34, function() load_shot(idx - 1) end)
    item(">", 34, function() load_shot(idx + 1) end)
    item("Info", 52, show_info)
    item("Back", 52, back_to_browser)
    item("X", 34, quit_app)

    nav.push(bar)
    -- ESC closes the toolbar rather than the app: the bar owns the focus while
    -- it is up, so the viewer's own key handler never sees the press.
    bar:onevent(lvgl.EVENT.KEY, function()
        local key = lvgl.indev.get_act():get_key()
        if key == lvgl.KEY.ESC or key == 27 or key == 113 then
            hide_bar()
            lvgl.group.focus_obj(keyscope)
        end
    end)
end

show_viewer = function(n)
    swap_view(function(v)
        view = imgview.new(v, { w = W, h = H, bg = "#000000" })

        -- Transparent, NON-clickable key sink stacked over the image: it holds
        -- the nav focus (so keys land here) while taps fall through to the
        -- imgview container beneath it. It has no children, so gridnav has
        -- nothing to steal the arrows for.
        local keyscope = v:Object {
            w = W, h = H, x = 0, y = 0,
            bg_opa = 0, border_width = 0, pad_all = 0, radius = 0,
        }
        keyscope:clear_flag(lvgl.FLAG.SCROLLABLE)
        keyscope:clear_flag(lvgl.FLAG.CLICKABLE)
        nav.replace(keyscope, { flags = nav.NONE })

        view:enable_drag(function() show_bar(keyscope) end)

        keyscope:onevent(lvgl.EVENT.KEY, function()
            local key = lvgl.indev.get_act():get_key()
            local full = view.mode == "full"
            if key == lvgl.KEY.LEFT then
                if full then view:pan(-PAN_STEP, 0) else load_shot(idx - 1) end
            elseif key == lvgl.KEY.RIGHT then
                if full then view:pan(PAN_STEP, 0) else load_shot(idx + 1) end
            elseif key == lvgl.KEY.UP then
                if full then view:pan(0, -PAN_STEP) end
            elseif key == lvgl.KEY.DOWN then
                if full then view:pan(0, PAN_STEP) end
            elseif key == lvgl.KEY.ENTER or key == 102 then      -- enter / f
                view:toggle()
            elseif key == 44 then                                 -- ,
                load_shot(idx - 1)
            elseif key == 46 then                                 -- .
                load_shot(idx + 1)
            elseif key == 105 then                                -- i
                show_info()
            elseif key == 99 then                                 -- c
                view:center()
            elseif key == 109 then                                -- m
                show_bar(keyscope)
            elseif key == lvgl.KEY.ESC or key == 27 or key == 113 then
                back_to_browser()
            end
        end)

        load_shot(n)
    end)
end

-- ── Start ────────────────────────────────────────────────────────────────────

show_drives()

return root
