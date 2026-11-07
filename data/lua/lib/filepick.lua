--[[
  lib/filepick — the shared file / folder picker.

  A full-screen, folder-navigating list that an app opens over its current
  view and that hands back ONE path. Drives come from lib/fileman, so it
  browses L:, S: and U: alike and returns drive-prefixed paths ("S:/x/y").

    local filepick = require("lib/filepick")
    filepick.open(root, {
        mode      = "file",             -- "file" (default) or "dir"
        title     = "Choose an image",  -- header line
        start     = "S:/meshpunk",      -- first folder; nil or unlistable ->
                                        -- the drive list
        exts      = { "bin" },          -- files shown, by lowercase extension
        filter    = function(entry, dir) return true end,  -- or your own test
        on_pick   = function(path, entry) ... end,
        on_cancel = function() ... end, -- optional
    })

  The picker is a child of `root` (the app's root from apps.new_root), so an
  app teardown removes it with everything else, and it runs as its own nav
  scope (nav.push) so the view underneath keeps its focus state. on_pick /
  on_cancel run AFTER the picker has closed and popped its scope, which is
  what lets them swap the app's own view.

  Folders are always listed and tapping one enters it. In "file" mode a file
  tap is the pick. In "dir" mode files are shown as plain text and the pick
  is the "Choose this folder" button. Up at a drive root returns to the drive
  list; Cancel closes without a pick.
]]

local lvgl    = require("lvgl")
local apps    = require("lib/apps")
local nav     = require("lib/nav")
local fileman = require("lib/fileman")

local M = {}

local MAX_SHOW = 150   -- rows built per folder (same cap as Tools/Files)

function M.open(root, opts)
    opts = opts or {}
    local mode = opts.mode == "dir" and "dir" or "file"
    local W, H = lvgl.HOR_RES(), lvgl.VER_RES()

    local exts
    if opts.exts then
        exts = {}
        for _, e in ipairs(opts.exts) do exts[e:lower()] = true end
    end

    local function shown(entry, dir)
        if entry.type == "dir" then return true end
        if exts and not exts[fileman.ext(entry.name) or ""] then return false end
        if opts.filter and not opts.filter(entry, dir) then return false end
        return true
    end

    local overlay = root:Object {
        w = W, h = H, x = 0, y = 0,
        bg_opa = 255, border_width = 0, pad_all = 0, radius = 0,
    }
    overlay:clear_flag(lvgl.FLAG.SCROLLABLE)
    overlay:add_flag(lvgl.FLAG.CLICKABLE)   -- taps never reach the view below

    local content = nil
    local closed = false
    local cur = nil                          -- folder shown; nil = drive list

    local function close()
        if closed then return end
        closed = true
        nav.pop()
        overlay:delete()
    end

    local function finish(path, entry)
        close()
        if opts.on_pick then opts.on_pick(path, entry) end
    end

    local function cancel()
        close()
        if opts.on_cancel then opts.on_cancel() end
    end

    local show_drives, show_folder

    -- A fresh flat flex-wrap scope per folder, swapped pop-before-delete:
    -- nav points at the new one before the old one goes.
    local function new_content()
        local old = content
        content = overlay:Object {
            w = W, h = H, x = 0, y = 0,
            bg_opa = 0, border_width = 0, pad_all = 4,
            flex = { flex_direction = "row", flex_wrap = "wrap" },
        }
        if old then
            nav.replace(content, { flags = nav.ROLLOVER + nav.SCROLL_FIRST })
            apps.delete_view(old)
        else
            nav.push(content, { flags = nav.ROLLOVER + nav.SCROLL_FIRST })
        end
        return content
    end

    local function tool(c, text, width, fn)
        local b = c:Button { w = width, h = 24 }
        b:Label { text = text, align = lvgl.ALIGN.CENTER }
        b:onClicked(fn)
        return b
    end

    show_drives = function()
        cur = nil
        local c = new_content()
        c:Label { text = opts.title or "Choose a drive", w = lvgl.PCT(100), h = 18 }
        for _, d in ipairs(fileman.drives()) do
            if d.mounted then
                local b = c:Button { w = lvgl.PCT(100), h = 30 }
                b:Label { text = d.label .. "  (" .. d.id .. ":)", align = lvgl.ALIGN.LEFT_MID }
                local drive_root = d.root
                b:onClicked(function() show_folder(drive_root) end)
            else
                c:Label { text = d.label .. "  (" .. d.id .. ":)  not mounted",
                          w = lvgl.PCT(100), h = 24 }
            end
        end
        tool(c, "Cancel", lvgl.PCT(100), cancel)
    end

    show_folder = function(path)
        cur = fileman.normalize(path)
        local entries = fileman.list(cur)
        if not entries then
            show_drives()
            return
        end
        local c = new_content()

        if opts.title then c:Label { text = opts.title, w = lvgl.PCT(100), h = 18 } end
        local disp = cur
        if #disp > 36 then disp = "..." .. disp:sub(-33) end
        c:Label { text = disp, w = lvgl.PCT(100), h = 16 }

        tool(c, "Up", 46, function()
            local p = fileman.parent(cur)
            if p then show_folder(p) else show_drives() end
        end)
        tool(c, "Drives", 60, show_drives)
        tool(c, "Cancel", 60, cancel)
        if mode == "dir" then
            tool(c, "Choose this folder", lvgl.PCT(100), function()
                finish(cur, { name = fileman.basename(cur), type = "dir" })
            end)
        end

        local count = 0
        for _, e in ipairs(entries) do
            if count >= MAX_SHOW then break end
            if shown(e, cur) then
                count = count + 1
                local name = e.name
                if #name > 30 then name = name:sub(1, 29) .. "~" end
                local entry = e
                local full = fileman.join(cur, e.name)
                if e.type == "dir" then
                    local row = c:Button { w = lvgl.PCT(100), h = 24 }
                    row:Label { text = name .. "/", align = lvgl.ALIGN.LEFT_MID }
                    row:onClicked(function() show_folder(full) end)
                elseif mode == "file" then
                    local row = c:Button { w = lvgl.PCT(100), h = 24 }
                    row:Label { text = name, align = lvgl.ALIGN.LEFT_MID }
                    row:Label { text = fileman.size_str(e.size), align = lvgl.ALIGN.RIGHT_MID }
                    row:onClicked(function() finish(full, entry) end)
                else
                    c:Label { text = "   " .. name, w = lvgl.PCT(100), h = 20 }
                end
            end
        end
        if count == 0 then
            c:Label { text = "(nothing here)", w = lvgl.PCT(100), h = 24 }
        end
    end

    if opts.start then show_folder(opts.start) else show_drives() end

    return { close = cancel }
end

return M
