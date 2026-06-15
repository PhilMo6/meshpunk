--[[
  Code Editor for MeshPunk T-Deck
  Browse, edit, save, and run Lua files on the device.

  Features:
  - File/folder browser for LittleFS
  - Full text editor with physical keyboard input
  - Save and Run (pcall with error display)
  - Basic syntax highlighting in preview mode (LVGL label recolor)
  - Trackball moves cursor with auto-scroll (built-in LVGL behavior)
  - Touch tap positions cursor (built-in LVGL Textarea behavior)
  - Scrollbar responds to touch (built-in LVGL scrollable behavior)
  - New file creation

  NOTES:
  - For full file listing, add _list_all() C++ bridge (see project notes)
  - Without _list_all, browser probes for known filenames as fallback
  - Tab key moves LVGL focus between widgets (platform default)
  - Preview highlighting uses LVGL label recolor (#RRGGBB text#)
  - Max file size for editing: 8KB (ESP32 memory constraint)
]]

local lvgl = require("lvgl")
local apps = require("lib/apps")

-- ── Screen constants ──
local W = lvgl.HOR_RES()
local H = lvgl.VER_RES()
local HDR_H = 26
local STS_H = 20
local BODY_H = H - HDR_H - STS_H
-- [COMMENTED OUT] limit removed, let the device handle what it can
-- local MAX_FILE = 8000 -- max editable file size in bytes

-- ── App state ──
local cur_path = "/lua"
local cur_file = nil
local content = ""

-- ── Root container ──
local root = lvgl.Object()
root:set {
    w = W, h = H,
    bg_color = "#16161e",
    pad_all = 0,
    border_width = 0,
}
root:clear_flag(lvgl.FLAG.SCROLLABLE)

-- Current view reference (deleted on navigation)
local vw = nil

-- Forward declarations for mutual navigation
local show_browser, show_editor, show_newfile

-- ══════════════════════════════════════════════════
-- ██ UTILITY FUNCTIONS
-- ══════════════════════════════════════════════════

local function fexists(p)
    local f = io.open(p, "r")
    if f then f:close() return true end
    return false
end

local function fread(p)
    local f = io.open(p, "r")
    if not f then return nil end
    local t = f:read("*a")
    f:close()
    return t or ""
end

local function fwrite(p, c)
    local f = io.open(p, "w")
    if not f then return false end
    f:write(c)
    f:close()
    return true
end

local function bname(p)
    return p:match("([^/]+)$") or p
end

local function pdir(p)
    return p:match("(.+)/[^/]+$") or "/"
end

-- Clear active view for navigation transitions
local function clr()
    if vw then vw:delete() vw = nil end
end

-- Brief notification overlay
local function notify(parent, msg, col)
    col = col or "#a9b1d6"
    local n = parent:Object {
        bg_color = "#24283b",
        border_color = col,
        border_width = 1,
        radius = 4,
        pad_all = 4,
        w = 280,
        h = lvgl.SIZE_CONTENT,
        align = lvgl.ALIGN.BOTTOM_MID,
        y = -6,
    }
    n:Label { text = msg, text_color = col, align = lvgl.ALIGN.CENTER }
    lvgl.Timer.create(function()
        pcall(function() n:delete() end)
    end, 2500, 1)
end

-- ══════════════════════════════════════════════════
-- ██ DIRECTORY LISTING
-- ══════════════════════════════════════════════════

local function ls(dir)
    local out = {}

    -- Best case: _list_all returns files + dirs with type field
    -- Add this C++ function for full file browser support
    if type(_list_all) == "function" then
        local ok, items = pcall(_list_all, dir)
        if ok and type(items) == "table" then
            table.sort(items, function(a, b)
                if a.type ~= b.type then return a.type == "dir" end
                return a.name < b.name
            end)
            return items
        end
    end

    -- Fallback: _list_dir for subdirectories only
    local ok, dirs = pcall(_list_dir, dir)
    if ok and type(dirs) == "table" then
        for _, d in ipairs(dirs) do
            table.insert(out, { name = d, type = "dir" })
        end
    end

    -- Probe for known filenames (workaround without _list_all)
    -- This list covers the default project files
    local probes = {
        "main.lua", "init.lua", "launcher.lua", "utils.lua",
        "nav.lua", "toml.lua", "wifi.lua", "messages.lua",
        "config.lua", "notes.txt", "apps.toml", "README.md",
    }
    for _, fn in ipairs(probes) do
        if fexists(dir .. "/" .. fn) then
            table.insert(out, { name = fn, type = "file" })
        end
    end

    table.sort(out, function(a, b)
        if a.type ~= b.type then return a.type == "dir" end
        return a.name < b.name
    end)
    return out
end

-- ══════════════════════════════════════════════════
-- ██ SYNTAX HIGHLIGHTING (preview mode)
-- ══════════════════════════════════════════════════
-- Uses LVGL label recolor format: #RRGGBB text#
-- ## displays a literal # character

local kw_tbl = {}
for _, w in ipairs({
    "local", "function", "end", "if", "then", "else", "elseif",
    "for", "while", "do", "repeat", "until", "return", "break",
    "in", "not", "and", "or", "true", "false", "nil",
    "require", "pcall", "self",
}) do
    kw_tbl[w] = true
end

-- Highlight a single line of Lua code
local function hl_line(ln)
    -- Step 1: Escape all # to ## (literal # in LVGL recolor)
    ln = ln:gsub("#", "##")

    -- Step 2: Split at first -- to separate code from comment
    local code, cmt = ln:match("^(.-)(%-%-.*)$")
    if not code then
        code = ln
        cmt = nil
    end

    -- Step 3: Color keywords in code portion
    code = code:gsub("([%a_][%w_]*)", function(w)
        if kw_tbl[w] then
            return "#bb9af7" .. w .. "#"
        end
        return w
    end)

    -- Step 4: Color comment portion in gray
    if cmt then
        return code .. "#565f89" .. cmt .. "#"
    end
    return code
end

-- Highlight entire source (with safety limits)
local function highlight(src)
    if not src or #src == 0 then return "" end
    if #src > 4000 then return src end -- skip for large files

    local ok, result = pcall(function()
        local lines = {}
        for line in (src .. "\n"):gmatch("(.-)\n") do
            table.insert(lines, hl_line(line))
        end
        return table.concat(lines, "\n")
    end)
    return ok and result or src
end

-- ══════════════════════════════════════════════════
-- ██ FILE BROWSER VIEW
-- ══════════════════════════════════════════════════

show_browser = function()
    clr()

    vw = root:Object()
    vw:set {
        w = W, h = H,
        bg_color = "#16161e",
        pad_all = 0, border_width = 0,
    }
    vw:clear_flag(lvgl.FLAG.SCROLLABLE)

    -- ── Header ──
    local hdr = vw:Object {
        w = W, h = HDR_H, y = 0,
        bg_color = "#1a1b26",
        border_width = 0,
        pad_all = 2, pad_left = 6,
    }
    hdr:clear_flag(lvgl.FLAG.SCROLLABLE)

    -- Truncate long paths
    local dp = cur_path
    if #dp > 25 then dp = "..." .. dp:sub(-22) end

    hdr:Label {
        text = dp,
        text_color = "#7aa2f7",
        align = lvgl.ALIGN.LEFT_MID,
        w = 220,
    }

    hdr:Label {
        text = "#",
        text_color = "#565f89",
        align = lvgl.ALIGN.RIGHT_MID,
        w = 20,
    }

    -- ── Scrollable file list ──
    local list = vw:Object {
        w = W, h = BODY_H,
        y = HDR_H,
        bg_color = "#16161e",
        border_width = 0,
        pad_all = 2,
        flex = {
            flex_direction = "column",
        },
    }
    _nav_setup(list, GRIDNAV_ROLLOVER + GRIDNAV_SCROLL_FIRST)

    -- Home button at top of list
    local home_btn = list:Button {
        w = lvgl.PCT(100), h = 22,
        bg_color = "#1a1b26",
    }
    home_btn:Label {
        text = "< Home",
        text_color = "#7aa2f7",
        align = lvgl.ALIGN.LEFT_MID,
    }
    home_btn:onClicked(function()
        root:delete()
        apps.refresh()   -- pick up any files created/edited this session
        local launcher = require("launcher")
        launcher.create()
    end)

    -- Parent directory entry
    if cur_path ~= "/" and cur_path ~= "" then
        local up = list:Button {
            w = lvgl.PCT(100), h = 22,
            bg_color = "#1a1b26",
        }
        up:Label {
            text = ".. (up)",
            text_color = "#565f89",
            align = lvgl.ALIGN.LEFT_MID,
        }
        up:onClicked(function()
            cur_path = pdir(cur_path)
            show_browser()
        end)
    end

    -- List all entries
    local entries = ls(cur_path)

    for _, e in ipairs(entries) do
        local btn = list:Button {
            w = lvgl.PCT(100), h = 22,
            bg_color = "#24283b",
        }

        local icon = e.type == "dir" and "> " or "  "
        local col = e.type == "dir" and "#7aa2f7" or "#a9b1d6"
        local sfx = e.type == "dir" and "/" or ""

        btn:Label {
            text = icon .. e.name .. sfx,
            text_color = col,
            align = lvgl.ALIGN.LEFT_MID,
        }

        btn:onClicked(function()
            if e.type == "dir" then
                cur_path = cur_path .. "/" .. e.name
                show_browser()
            else
                show_editor(cur_path .. "/" .. e.name)
            end
        end)
    end

    if #entries == 0 then
        list:Label {
            text = "(empty or no files found)",
            text_color = "#565f89",
            w = lvgl.PCT(100), h = 30,
            align = lvgl.ALIGN.CENTER,
        }
    end

    -- New File button at end of list
    local new_btn = list:Button {
        w = lvgl.PCT(100), h = 22,
        bg_color = "#24283b",
    }
    new_btn:Label { text = "+ New File", text_color = "#9ece6a", align = lvgl.ALIGN.LEFT_MID }
    new_btn:onClicked(function()
        show_newfile()
    end)

    -- ── Footer ──
    local ftr = vw:Object {
        w = W, h = STS_H,
        y = H - STS_H,
        bg_color = "#1a1b26",
        border_width = 0,
        pad_all = 2, pad_left = 6,
    }
    ftr:clear_flag(lvgl.FLAG.SCROLLABLE)

    ftr:Label {
        text = #entries .. " items",
        text_color = "#565f89",
        align = lvgl.ALIGN.RIGHT_MID,
    }
end

-- ══════════════════════════════════════════════════
-- ██ NEW FILE DIALOG
-- ══════════════════════════════════════════════════

show_newfile = function()
    clr()

    vw = root:Object()
    vw:set {
        w = W, h = H,
        bg_color = "#16161e",
        pad_all = 0, border_width = 0,
    }
    vw:clear_flag(lvgl.FLAG.SCROLLABLE)

    -- ── Header ──
    local hdr = vw:Object {
        w = W, h = HDR_H, y = 0,
        bg_color = "#1a1b26",
        border_width = 0, pad_all = 2, pad_left = 6,
    }
    hdr:clear_flag(lvgl.FLAG.SCROLLABLE)
    hdr:Label {
        text = "New File",
        text_color = "#7aa2f7",
        align = lvgl.ALIGN.LEFT_MID,
    }

    -- ── Body ──
    local body = vw:Object {
        w = W, h = H - HDR_H,
        y = HDR_H,
        bg_color = "#16161e",
        border_width = 0,
        pad_all = 10,
        flex = { flex_direction = "row", flex_wrap = "wrap" },
    }
    body:clear_flag(lvgl.FLAG.SCROLLABLE)
    _nav_setup(body, GRIDNAV_ROLLOVER)

    body:Label {
        text = "Dir: " .. cur_path,
        text_color = "#565f89",
        w = lvgl.PCT(100), h = 18,
    }

    body:Label {
        text = "Filename (.lua added if no ext):",
        text_color = "#a9b1d6",
        w = lvgl.PCT(100), h = 18,
    }

    local name_ta = body:Textarea {
        one_line = true,
        text = "",
        text_color = "#c0caf5",
        bg_color = "#24283b",
        border_color = "#3b4261",
        border_width = 1,
        w = lvgl.PCT(100), h = 34,
        pad_all = 4,
    }

    local status = body:Label {
        text = "",
        text_color = "#f7768e",
        w = lvgl.PCT(100), h = 18,
    }

    -- Create handler
    local function do_create()
        local fname = name_ta.text
        if not fname or #fname == 0 then
            status.text = "Enter a filename"
            return
        end
        -- Add .lua extension if no extension present
        if not fname:match("%.") then
            fname = fname .. ".lua"
        end
        local fpath = cur_path .. "/" .. fname
        if fexists(fpath) then
            status.text = "File already exists!"
            return
        end
        if fwrite(fpath, "-- " .. fname .. "\n") then
            show_editor(fpath)
        else
            status.text = "Failed to create file"
        end
    end

    local create_btn = body:Button { w = 80, h = 26 }
    create_btn:Label { text = "Create", align = lvgl.ALIGN.CENTER }
    create_btn:onClicked(do_create)

    local cancel_btn = body:Button { w = 70, h = 26 }
    cancel_btn:Label { text = "Cancel", align = lvgl.ALIGN.CENTER }
    cancel_btn:onClicked(function()
        show_browser()
    end)

    -- Enter key submits
    name_ta:onevent(lvgl.EVENT.KEY, function(obj, code)
        local indev = lvgl.indev.get_act()
        local key = indev:get_key()
        if key == lvgl.KEY.ENTER then
            do_create()
        end
    end)
end

-- ══════════════════════════════════════════════════
-- ██ CODE EDITOR VIEW
-- ══════════════════════════════════════════════════

show_editor = function(filepath)
    clr()
    cur_file = filepath

    -- Read file content
    content = fread(filepath)
    if not content then
        notify(root, "Cannot read: " .. bname(filepath), "#f7768e")
        show_browser()
        return
    end

    -- [COMMENTED OUT] file size guard removed per user request
    -- if #content > MAX_FILE then
    --     notify(root, "File too large (" .. #content .. "b)", "#f7768e")
    --     show_browser()
    --     return
    -- end

    -- Editor state
    local previewing = false
    local body_container = nil
    local ta = nil
    local view_lbl = nil
    local status_lbl = nil

    -- ── Build view ──
    vw = root:Object()
    vw:set {
        w = W, h = H,
        bg_color = "#16161e",
        pad_all = 0, border_width = 0,
    }
    vw:clear_flag(lvgl.FLAG.SCROLLABLE)

    -- ── Header with action buttons ──
    local hdr = vw:Object {
        w = W, h = HDR_H, y = 0,
        bg_color = "#1a1b26",
        border_width = 0, pad_all = 1, pad_left = 4,
        flex = {
            flex_direction = "row",
            align_items = "center",
        },
    }
    hdr:clear_flag(lvgl.FLAG.SCROLLABLE)
    _nav_setup(hdr, GRIDNAV_ROLLOVER)

    -- Filename display (truncated if needed)
    local fn = bname(filepath)
    if #fn > 13 then fn = fn:sub(1, 12) .. "~" end
    hdr:Label {
        text = fn,
        text_color = "#9ece6a",
        w = 95,
    }

    -- Save button
    local save_btn = hdr:Button { w = 38, h = 20 }
    save_btn:Label { text = "Sav", align = lvgl.ALIGN.CENTER }
    save_btn:onClicked(function()
        if previewing then return end
        content = ta.text or content
        if fwrite(filepath, content) then
            status_lbl.text = "Saved " .. #content .. "b"
        else
            status_lbl.text = "Save FAILED"
        end
    end)

    -- Run button: saves, closes editor, executes the file
    -- If run errors, shows error screen with back nav
    local run_btn = hdr:Button { w = 36, h = 20 }
    run_btn:Label { text = "Run", align = lvgl.ALIGN.CENTER }
    run_btn:onClicked(function()
        -- Grab latest content from textarea
        if not previewing and ta then
            content = ta.text or content
        end
        -- Save before running
        fwrite(filepath, content)

        -- Destroy editor UI completely
        root:delete()

        -- Attempt to run the file
        local ok, err = pcall(dofile, filepath)

        if not ok then
            -- Show error screen with back navigation
            local err_root = lvgl.Object()
            err_root:set {
                w = W, h = H,
                bg_color = "#16161e",
                pad_all = 10, border_width = 0,
            }
            err_root:clear_flag(lvgl.FLAG.SCROLLABLE)
            _nav_setup(err_root, GRIDNAV_ROLLOVER)

            err_root:Label {
                text = "Runtime Error",
                text_color = "#f7768e",
                align = lvgl.ALIGN.TOP_MID,
                y = 10,
            }

            -- Show truncated error message
            local err_msg = tostring(err)
            if #err_msg > 300 then
                err_msg = err_msg:sub(1, 300) .. "..."
            end
            err_root:Label {
                text = err_msg,
                text_color = "#a9b1d6",
                align = lvgl.ALIGN.TOP_MID,
                y = 35,
                w = 290,
            }

            local back = err_root:Button {
                w = 100, h = 30,
                align = lvgl.ALIGN.BOTTOM_MID,
                y = -10,
            }
            back:Label { text = "Back Home", align = lvgl.ALIGN.CENTER }
            back:onClicked(function()
                err_root:delete()
                local launcher = require("launcher")
                launcher.create()
            end)
        end
        -- If ok, the executed file has taken over the screen
    end)

    -- View/Edit toggle (switches between textarea and highlighted label)
    local view_btn = hdr:Button { w = 40, h = 20 }
    view_lbl = view_btn:Label { text = "View", align = lvgl.ALIGN.CENTER }

    -- Back to browser
    local back_btn = hdr:Button { w = 28, h = 20 }
    back_btn:Label { text = "<", align = lvgl.ALIGN.CENTER }
    back_btn:onClicked(function()
        -- Save current content from textarea
        if not previewing and ta then
            content = ta.text or content
        end
        -- Navigate to file's parent directory
        cur_path = pdir(filepath)
        show_browser()
    end)

    -- ── Body container (holds editor textarea OR preview label) ──
    body_container = vw:Object {
        w = W, h = BODY_H,
        y = HDR_H,
        bg_color = "#1a1b26",
        border_width = 0,
        pad_all = 0,
    }
    body_container:clear_flag(lvgl.FLAG.SCROLLABLE)

    -- Create the editable textarea
    local function make_editor()
        ta = body_container:Textarea {
            text = content,
            one_line = false,
            text_color = "#c0caf5",
            bg_color = "#1a1b26",
            border_width = 0,
            pad_all = 4,
            w = W,
            h = BODY_H,
        }
        -- Textarea auto-scrolls when cursor moves past visible area
        -- Trackball keys move cursor; touch tap positions cursor
        -- Both are built-in LVGL Textarea behaviors
    end

    -- Create the syntax-highlighted preview (read-only)
    local function make_preview()
        -- Scrollable container for preview content
        local scroll = body_container:Object {
            w = W, h = BODY_H,
            bg_color = "#1a1b26",
            border_width = 0,
            pad_all = 4,
        }
        -- Attempt syntax highlighting via LVGL recolor
        local hl_text = highlight(content)
        scroll:Label {
            text = hl_text,
            text_color = "#a9b1d6",
            text_recolor = true,
            w = W - 16,
        }
    end

    -- Start in edit mode
    make_editor()

    -- Toggle between edit and preview modes
    view_btn:onClicked(function()
        if previewing then
            -- Switch to edit mode
            body_container:clean()
            make_editor()
            view_lbl.text = "Edit"
            previewing = false
        else
            -- Save textarea content before switching
            if ta then
                content = ta.text or content
            end
            body_container:clean()
            ta = nil
            make_preview()
            view_lbl.text = "Back"
            previewing = true
        end
    end)

    -- ── Status bar ──
    local sts = vw:Object {
        w = W, h = STS_H,
        y = H - STS_H,
        bg_color = "#24283b",
        border_width = 0,
        pad_all = 2, pad_left = 6,
    }
    sts:clear_flag(lvgl.FLAG.SCROLLABLE)

    status_lbl = sts:Label {
        text = #content .. "b",
        text_color = "#565f89",
        align = lvgl.ALIGN.LEFT_MID,
    }

    sts:Label {
        text = bname(filepath),
        text_color = "#565f89",
        align = lvgl.ALIGN.RIGHT_MID,
    }
end

-- ══════════════════════════════════════════════════
-- ██ START THE APP
-- ══════════════════════════════════════════════════

show_browser()

return root
