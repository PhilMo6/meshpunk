--[[
  Files — full file manager for MeshPunk.

  Built entirely on lib/fileman (the reusable file-management library); this
  app is also its reference user. Features:

    * Drive picker (Internal LittleFS / SD card) with used/total space
    * Browser: folders first, sizes, [..] up, entry cap for huge directories
    * Copy / Cut / Paste across drives (fileman tasks driven from a timer,
      progress modal with cancel — watchdog-safe for whole trees)
    * Delete (recursive) with confirmation, extra warning on system paths
    * Rename, New folder, New file, file Info, text Preview
    * Select mode: tapping ANY entry (folders too) opens the action menu

  Navigation: one flat flex-wrap scope per view (Settings-style), modals via
  nav.push/pop (Messenger-style overlays).
]]

local lvgl    = require("lvgl")
local apps    = require("lib/apps")
local nav     = require("lib/nav")
local theme   = require("lib/theme")
local utils   = require("lib/utils")
local fileman = require("lib/fileman")

local W = lvgl.HOR_RES()
local H = lvgl.VER_RES()

local MAX_SHOW = 150          -- listing rows built per directory (UI cap)
local PREVIEW_MAX = 8 * 1024  -- bytes; larger files get Info instead

-- ── App state ────────────────────────────────────────────────────────────────
local cur_path = nil          -- current directory ("L:/...", nil = drive picker)
local clip = nil              -- { op = "copy"|"cut", src = path, name = }
local select_mode = false     -- true: tapping any entry opens the action menu
local dirty = false           -- something changed -> apps.refresh() on quit

local root = apps.new_root()
root:set { w = W, h = H, pad_all = 0, border_width = 0, bg_opa = 0 }
root:clear_flag(lvgl.FLAG.SCROLLABLE)
theme.show_background()

local vw = nil                -- current view container

local show_drives, show_browser  -- forward declarations

local function toast(msg)
    pcall(utils.createNotification, root, tostring(msg), 2500)
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

-- ── Modal helpers ────────────────────────────────────────────────────────────
-- Dimmed full-screen overlay + centered box pushed as its own nav scope.
-- `build(box, close)` adds the content. Returns the close function.
local function modal(box_opts, build)
    local overlay = root:Object {
        w = W, h = H, x = 0, y = 0,
        bg_color = "#000000", bg_opa = 140, border_width = 0, pad_all = 0,
        radius = 0,
    }
    overlay:clear_flag(lvgl.FLAG.SCROLLABLE)
    overlay:add_flag(lvgl.FLAG.CLICKABLE)   -- swallow taps behind the box

    local box = overlay:Object {
        w = box_opts.w or (W - 70),
        h = box_opts.h or lvgl.SIZE_CONTENT,
        align = lvgl.ALIGN.CENTER,
        radius = 6, border_width = 1, pad_all = 8,
        flex = { flex_direction = "column", flex_wrap = "nowrap" },
    }
    nav.push(box)

    local closed = false
    local function close()
        if closed then return end
        closed = true
        nav.pop()
        overlay:delete()
    end
    build(box, close)
    return close
end

-- Text-input modal. cb(value) returns an error string to keep the dialog
-- open, or nil (+ optional follow-up function) to accept. The follow-up runs
-- AFTER the dialog's nav scope is popped — rebuilding a view from inside the
-- cb would nav.replace() the prompt's own scope and crash on the pop.
local function prompt(title, initial, cb)
    modal({}, function(box, close)
        box:Label { text = title, w = lvgl.PCT(100), h = 18 }
        local ta = box:Textarea {
            one_line = true, text = initial or "",
            w = lvgl.PCT(100), h = 32,
        }
        ta:clear_flag(lvgl.FLAG.SCROLLABLE)
        local err_lbl = box:Label { text = "", w = lvgl.PCT(100), h = 16 }

        local function submit()
            local err, after = cb(ta.text or "")
            if err then
                err_lbl.text = tostring(err)
            else
                close()
                if after then after() end
            end
        end

        local ok_btn = box:Button { w = lvgl.PCT(100), h = 26 }
        ok_btn:Label { text = "OK", align = lvgl.ALIGN.CENTER }
        ok_btn:onevent(lvgl.EVENT.RELEASED, submit)

        local cancel_btn = box:Button { w = lvgl.PCT(100), h = 26 }
        cancel_btn:Label { text = "Cancel", align = lvgl.ALIGN.CENTER }
        cancel_btn:onevent(lvgl.EVENT.RELEASED, close)

        ta:onevent(lvgl.EVENT.KEY, function()
            if lvgl.indev.get_act():get_key() == lvgl.KEY.ENTER then submit() end
        end)
    end)
end

-- Yes/no confirmation. `warn` (optional) is an extra red-flag line.
local function confirm(title, warn, on_yes)
    modal({}, function(box, close)
        box:Label { text = title, w = lvgl.PCT(100) }
        if warn then
            box:Label { text = warn, text_color = "#ff5555", w = lvgl.PCT(100) }
        end
        local yes = box:Button { w = lvgl.PCT(100), h = 26 }
        yes:Label { text = "Yes", align = lvgl.ALIGN.CENTER }
        yes:onevent(lvgl.EVENT.RELEASED, function()
            close()
            on_yes()
        end)
        local no = box:Button { w = lvgl.PCT(100), h = 26 }
        no:Label { text = "No", align = lvgl.ALIGN.CENTER }
        no:onevent(lvgl.EVENT.RELEASED, close)
    end)
end

-- Drive a fileman task from a timer behind a progress modal with Cancel.
-- on_done(err) runs after the modal closes (err nil on success).
local function run_task(task, title, on_done)
    local overlay = root:Object {
        w = W, h = H, x = 0, y = 0,
        bg_color = "#000000", bg_opa = 140, border_width = 0, pad_all = 0,
        radius = 0,
    }
    overlay:clear_flag(lvgl.FLAG.SCROLLABLE)
    overlay:add_flag(lvgl.FLAG.CLICKABLE)

    local box = overlay:Object {
        w = 240, h = lvgl.SIZE_CONTENT, align = lvgl.ALIGN.CENTER,
        radius = 6, border_width = 1, pad_all = 8,
        flex = { flex_direction = "column", flex_wrap = "nowrap" },
    }
    nav.push(box)

    box:Label { text = title, w = lvgl.PCT(100), h = 18 }
    local cur_lbl = box:Label { text = "...", w = lvgl.PCT(100), h = 18 }
    local cnt_lbl = box:Label { text = "", w = lvgl.PCT(100), h = 16 }
    local cancel_btn = box:Button { w = lvgl.PCT(100), h = 26 }
    cancel_btn:Label { text = "Cancel", align = lvgl.ALIGN.CENTER }
    cancel_btn:onevent(lvgl.EVENT.RELEASED, function()
        task.cancelled = true
    end)

    apps.add_timer { period = 15, cb = function(t)
        local done, err
        for _ = 1, 3 do   -- a few bounded steps per tick keeps small jobs snappy
            done, err = task.step()
            if done then break end
        end
        if not done then
            local name = tostring(task.current or "")
            if #name > 24 then name = name:sub(1, 23) .. "~" end
            cur_lbl.text = name
            cnt_lbl.text = (task.files_done or 0) .. " / " .. (task.files_total or "?")
            return
        end
        t:delete()
        nav.pop()
        overlay:delete()
        on_done(err)
    end }
end

-- ── Entry actions ────────────────────────────────────────────────────────────

local function refresh()
    if cur_path then show_browser(cur_path) else show_drives() end
end

local function do_paste()
    if not clip then
        toast("Clipboard empty")
        return
    end
    if not fileman.exists(clip.src) then
        toast("Source is gone")
        clip = nil
        refresh()
        return
    end
    if clip.op == "cut" and fileman.parent(clip.src) == cur_path then
        toast("Already here")
        return
    end
    local dst = fileman.unique_path(cur_path, clip.name)
    if not dst then
        toast("No free name here")
        return
    end
    local task, title
    if clip.op == "cut" then
        task, title = fileman.task_move(clip.src, dst), "Moving..."
    else
        task, title = fileman.task_copy(clip.src, dst), "Copying..."
    end
    run_task(task, title, function(err)
        if err then
            toast(err)
        else
            dirty = true
            if clip and clip.op == "cut" then clip = nil end
        end
        refresh()
    end)
end

local function do_delete(path, name)
    local warn = fileman.is_protected(path)
        and "SYSTEM path! Deleting this can\nbrick the firmware or reset\nyour mesh identity." or nil
    confirm('Delete "' .. name .. '"?', warn, function()
        run_task(fileman.task_remove(path), "Deleting...", function(err)
            if err then toast(err) else dirty = true end
            if clip and clip.src == path then clip = nil end
            refresh()
        end)
    end)
end

local function do_rename(path, name)
    prompt("Rename:", name, function(value)
        if value == "" then return "Enter a name" end
        if value:find("/", 1, true) then return "No / in names" end
        if value == name then return nil end
        local dst = fileman.join(fileman.parent(path), value)
        if fileman.exists(dst) then return "Already exists" end
        local ok, err = fileman.rename(path, dst)
        if not ok then return err or "Rename failed" end
        dirty = true
        if clip and clip.src == path then clip = nil end
        return nil, refresh
    end)
end

local function show_info(path, entry)
    modal({}, function(box, close)
        box:Label { text = entry.name, w = lvgl.PCT(100) }
        box:Label { text = "Type: " .. entry.type, w = lvgl.PCT(100), h = 16 }
        if entry.type == "file" then
            box:Label {
                text = "Size: " .. fileman.size_str(entry.size)
                    .. string.format(" (%.0fB)", tonumber(entry.size) or 0),
                w = lvgl.PCT(100), h = 16,
            }
        else
            local items = fileman.list(path)
            box:Label {
                text = "Contains: " .. (items and #items or "?") .. " entries",
                w = lvgl.PCT(100), h = 16,
            }
        end
        box:Label { text = path, w = lvgl.PCT(100) }
        local close_btn = box:Button { w = lvgl.PCT(100), h = 26 }
        close_btn:Label { text = "Close", align = lvgl.ALIGN.CENTER }
        close_btn:onevent(lvgl.EVENT.RELEASED, close)
    end)
end

local function show_preview(path, entry)
    if (tonumber(entry.size) or 0) > PREVIEW_MAX then
        toast("Too large to preview")
        show_info(path, entry)
        return
    end
    local data, err = fileman.read(path)
    if not data then
        toast(err or "Read failed")
        return
    end
    modal({ w = W - 20, h = H - 20 }, function(box, close)
        box:Label { text = entry.name, w = lvgl.PCT(100), h = 18 }
        local body = box:Object {
            w = lvgl.PCT(100), h = H - 90,
            border_width = 1, pad_all = 4, radius = 0,
        }
        body:Label { text = data ~= "" and data or "(empty file)", w = lvgl.PCT(100) }
        local close_btn = box:Button { w = lvgl.PCT(100), h = 26 }
        close_btn:Label { text = "Close", align = lvgl.ALIGN.CENTER }
        close_btn:onevent(lvgl.EVENT.RELEASED, close)
    end)
end

-- Action menu for one entry (folder or file).
local function action_menu(entry)
    local path = fileman.join(cur_path, entry.name)
    modal({ w = 190 }, function(box, close)
        local title = entry.name
        if #title > 20 then title = title:sub(1, 19) .. "~" end
        box:Label { text = title, w = lvgl.PCT(100), h = 18 }

        local function item(text, fn)
            local b = box:Button { w = lvgl.PCT(100), h = 24 }
            b:Label { text = text, align = lvgl.ALIGN.CENTER }
            b:onevent(lvgl.EVENT.RELEASED, function()
                close()
                fn()
            end)
        end

        if entry.type == "dir" then
            item("Open", function() show_browser(path) end)
        else
            item("Preview", function() show_preview(path, entry) end)
        end
        item("Copy", function()
            clip = { op = "copy", src = path, name = entry.name }
            toast("Copied: " .. entry.name)
            refresh()
        end)
        item("Cut", function()
            clip = { op = "cut", src = path, name = entry.name }
            toast("Cut: " .. entry.name)
            refresh()
        end)
        item("Rename", function() do_rename(path, entry.name) end)
        item("Delete", function() do_delete(path, entry.name) end)
        item("Info", function() show_info(path, entry) end)

        local cancel_btn = box:Button { w = lvgl.PCT(100), h = 24 }
        cancel_btn:Label { text = "Cancel", align = lvgl.ALIGN.CENTER }
        cancel_btn:onevent(lvgl.EVENT.RELEASED, close)
    end)
end

local function new_menu()
    modal({ w = 190 }, function(box, close)
        box:Label { text = "Create new", w = lvgl.PCT(100), h = 18 }

        local folder_btn = box:Button { w = lvgl.PCT(100), h = 26 }
        folder_btn:Label { text = "Folder", align = lvgl.ALIGN.CENTER }
        folder_btn:onevent(lvgl.EVENT.RELEASED, function()
            close()
            prompt("New folder:", "", function(value)
                if value == "" then return "Enter a name" end
                if value:find("/", 1, true) then return "No / in names" end
                local dst = fileman.join(cur_path, value)
                if fileman.exists(dst) then return "Already exists" end
                if not fileman.mkdir(dst) then return "mkdir failed" end
                dirty = true
                return nil, refresh
            end)
        end)

        local file_btn = box:Button { w = lvgl.PCT(100), h = 26 }
        file_btn:Label { text = "Empty file", align = lvgl.ALIGN.CENTER }
        file_btn:onevent(lvgl.EVENT.RELEASED, function()
            close()
            prompt("New file:", "", function(value)
                if value == "" then return "Enter a name" end
                if value:find("/", 1, true) then return "No / in names" end
                local dst = fileman.join(cur_path, value)
                if fileman.exists(dst) then return "Already exists" end
                local ok, err = fileman.write(dst, "")
                if not ok then return err or "Create failed" end
                dirty = true
                return nil, refresh
            end)
        end)

        local cancel_btn = box:Button { w = lvgl.PCT(100), h = 26 }
        cancel_btn:Label { text = "Cancel", align = lvgl.ALIGN.CENTER }
        cancel_btn:onevent(lvgl.EVENT.RELEASED, close)
    end)
end

local function quit_app()
    if dirty then pcall(apps.refresh) end   -- pick up app installs/removals
    apps.go_home()
end

-- ── Views ────────────────────────────────────────────────────────────────────

show_drives = function()
    cur_path = nil
    swap_view(function(v)
        local col = v:Object {
            w = W, h = H, x = 0, y = 0,
            bg_opa = 0, border_width = 0, pad_all = 8,
            flex = { flex_direction = "column", flex_wrap = "nowrap" },
        }
        nav.replace(col, { flags = nav.ROLLOVER })

        col:Label { text = "File Manager", w = lvgl.PCT(100), h = 22 }

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
                b:onClicked(function() toast("SD card not mounted") end)
            end
        end

        local quit_btn = col:Button { w = lvgl.PCT(100), h = 30 }
        quit_btn:Label { text = "Quit", align = lvgl.ALIGN.CENTER }
        quit_btn:onClicked(quit_app)
    end)
end

show_browser = function(path)
    cur_path = fileman.normalize(path)
    local entries, list_err = fileman.list(cur_path)

    swap_view(function(v)
        -- One flat flex-wrap scope: toolbar buttons + rows are all direct
        -- children (Settings-style), so one gridnav covers everything.
        local content = v:Object {
            w = W, h = H, x = 0, y = 0,
            bg_opa = 0, border_width = 0, pad_all = 4,
            flex = { flex_direction = "row", flex_wrap = "wrap" },
        }
        nav.replace(content, { flags = nav.ROLLOVER + nav.SCROLL_FIRST })

        -- Path header
        local disp = cur_path
        if #disp > 36 then disp = "..." .. disp:sub(-33) end
        content:Label { text = disp, w = lvgl.PCT(100), h = 16 }

        -- Toolbar
        local function tool(text, width, fn)
            local b = content:Button { w = width, h = 24 }
            b:Label { text = text, align = lvgl.ALIGN.CENTER }
            b:onClicked(fn)
            return b
        end

        tool("Up", 40, function()
            local p = fileman.parent(cur_path)
            if p then show_browser(p) else show_drives() end
        end)
        tool("New", 46, new_menu)
        tool(clip and "Paste*" or "Paste", 58, do_paste)
        tool(select_mode and "Sel:ON" or "Sel", 52, function()
            select_mode = not select_mode
            refresh()
        end)
        tool("Drives", 56, show_drives)
        tool("Quit", 46, quit_app)

        -- Listing
        if not entries then
            content:Label {
                text = "Cannot list: " .. tostring(list_err),
                w = lvgl.PCT(100), h = 40,
            }
            entries = {}
        end

        local shown = 0
        for _, e in ipairs(entries) do
            if shown >= MAX_SHOW then break end
            shown = shown + 1

            local row = content:Button { w = lvgl.PCT(100), h = 24 }
            local name = e.name
            if #name > 28 then name = name:sub(1, 27) .. "~" end
            local marked = clip and clip.src == fileman.join(cur_path, e.name)
            row:Label {
                text = (e.type == "dir" and (name .. "/") or name)
                    .. (marked and "  *" or ""),
                align = lvgl.ALIGN.LEFT_MID,
            }
            if e.type == "file" then
                row:Label {
                    text = fileman.size_str(e.size),
                    align = lvgl.ALIGN.RIGHT_MID,
                }
            end
            local entry = e
            -- Short tap enters a folder (or opens the menu for a file / when
            -- select mode is on); a long press ALWAYS opens the action menu, so
            -- a folder can be selected without navigating into it. SHORT_CLICKED
            -- (not CLICKED) is deliberate: LVGL suppresses it after a long press,
            -- so the two gestures never both fire on one row. Applies to the
            -- trackball's enter too (same long_pr_sent gate in the keypad path).
            row:onevent(lvgl.EVENT.SHORT_CLICKED, function()
                if not select_mode and entry.type == "dir" then
                    show_browser(fileman.join(cur_path, entry.name))
                else
                    action_menu(entry)
                end
            end)
            row:onevent(lvgl.EVENT.LONG_PRESSED, function()
                action_menu(entry)
            end)
        end

        if #entries == 0 then
            content:Label { text = "(empty)", w = lvgl.PCT(100), h = 24 }
        elseif #entries > MAX_SHOW then
            content:Label {
                text = "(+" .. (#entries - MAX_SHOW) .. " more not shown)",
                w = lvgl.PCT(100), h = 20,
            }
        end

        -- Footer: count, free space, clipboard
        local drive = fileman.split(cur_path)
        local total, used = fileman.df(drive)
        local free = (total and used) and fileman.size_str(total - used) .. " free" or ""
        local clip_note = ""
        if clip then
            local n = clip.name
            if #n > 12 then n = n:sub(1, 11) .. "~" end
            clip_note = "  |  clip: " .. n .. " (" .. clip.op .. ")"
        end
        content:Label {
            text = #entries .. " items  " .. free .. clip_note,
            w = lvgl.PCT(100), h = 16,
        }
    end)
end

-- ── Start ────────────────────────────────────────────────────────────────────

show_drives()

return root
