local lvgl = require("lvgl")
local apps = require("lib/apps")
local fileman = require("lib/fileman")

local app_dir = ...

local W = lvgl.HOR_RES()
local H = lvgl.VER_RES()

-- ============================================================
-- PC-XT (Faux86) Launcher
-- ============================================================

local sd_app_dir = app_dir:gsub("^L:", "S:")

local function to_vfs_path(path)
    if path:sub(1, 2) == "S:" then return "/sd" .. path:sub(3) end
    if path:sub(1, 2) == "L:" then return "/littlefs" .. path:sub(3) end
    return path
end

local function find_file(name)
    local search = { app_dir, sd_app_dir, "S:/dos" }
    for _, dir in ipairs(search) do
        local path = dir .. "/" .. name
        local f = io.open(path, "r")
        if f then f:close(); return path end
    end
    return nil
end

local CFG_PATH = app_dir .. "/controls.cfg"
local ELF_PATH = find_file("pcxt.app.elf") or sd_app_dir .. "/pcxt.app.elf"

local found_imgs = {}    -- { {name, path}, ... }
local seen_lower = {}
local found_folders = {} -- { {name, path}, ... } folder-backed C: candidates
local seen_folder_lower = {}
local sel_fda = 1        -- index into 1="None", 2.. = found_imgs[i-1]
local sel_hda = 1        -- index into 1="None", 2.. = hda_choices[i-1]
local hda_choices = {}   -- imgs then folders: { {kind="img"|"folder", name, path}, ... }
local sel_fda_name, sel_hda_name = nil, nil
local sel_hda_folder_name = nil
local scr = nil

local root = apps.new_root({
    w = W, h = H,
    bg_color = "#000000", bg_opa = lvgl.OPA(255),
    border_width = 0, pad_all = 0,
})
root:clear_flag(lvgl.FLAG.SCROLLABLE)

-- fileman routes the drive from the L:/S: prefix itself; sizes=false skips
-- the per-entry size lookup, so huge image folders list fast (watchdog-safe).
local function scan_dir_for_imgs(dir_path)
    local entries = fileman.list(dir_path, {
        sizes = false,
        filter = function(e)
            return e.type == "file"
                and (e.name:lower():match("%.img$") or e.name:lower():match("%.raw$"))
        end,
    }) or {}
    for _, e in ipairs(entries) do
        local low = e.name:lower()
        if not seen_lower[low] then
            seen_lower[low] = true
            found_imgs[#found_imgs + 1] = {
                name = e.name,
                path = dir_path .. "/" .. e.name,
            }
        end
    end
end

-- Subfolders double as C: drives (module synthesizes a FAT16 disk from them).
local function scan_dir_for_folders(dir_path)
    local entries = fileman.list(dir_path, {
        sizes = false,
        filter = function(e) return e.type == "dir" end,
    }) or {}
    for _, e in ipairs(entries) do
        local low = e.name:lower()
        if not seen_folder_lower[low] then
            seen_folder_lower[low] = true
            found_folders[#found_folders + 1] = {
                name = e.name,
                path = dir_path .. "/" .. e.name,
            }
        end
    end
end

-- FolderDisk manifest: line 1 = folder VFS root, then one line per entry
-- "relpath<TAB>size<TAB>isdir". The module resolves each entry's parent by
-- prefix lookup, so a directory must appear before anything inside it
-- (pre-order walk). Rewritten fresh at every boot.
local MANIFEST_PATH = app_dir .. "/cdrive.man"
local MANIFEST_MAX_ENTRIES = 1024
local function write_cdrive_manifest(folder_path)
    local f = io.open(MANIFEST_PATH, "w")
    if not f then return nil end
    f:write(to_vfs_path(folder_path) .. "\n")
    local count = 0
    local function walk(dir, rel, depth)
        if depth > 8 or count >= MANIFEST_MAX_ENTRIES then return end
        local entries = fileman.list(dir, { sizes = true }) or {}
        for _, e in ipairs(entries) do
            if count >= MANIFEST_MAX_ENTRIES then return end
            local erel = (rel == "") and e.name or (rel .. "/" .. e.name)
            count = count + 1
            if e.type == "dir" then
                f:write(erel .. "\t0\t1\n")
                walk(dir .. "/" .. e.name, erel, depth + 1)
            else
                f:write(erel .. "\t" .. tostring(e.size or 0) .. "\t0\n")
            end
        end
    end
    walk(folder_path, "", 1)
    f:close()
    return to_vfs_path(MANIFEST_PATH)
end

-- ============================================================
-- Keymap / controls system
-- ============================================================

-- Module extension codes understood by pcxt's ascii_to_scan() (main_tdeck.cpp)
local PC = {
    ESC   = 0x1B,
    F1    = 0xB0, F2 = 0xB1, F3 = 0xB2, F4 = 0xB3, F5 = 0xB4,
    F6    = 0xB5, F7 = 0xB6, F8 = 0xB7, F9 = 0xB8, F10 = 0xB9,
    CTRL  = 0x96,
    ALT   = 0x97,
    DEL   = 0x98,
    TAB   = 0x99,
    UP    = 0x91, DOWN = 0x92, LEFT = 0x93, RIGHT = 0x94,
    RMOUSE = 0x95,
}

local KEYS = {
    a=0x61, b=0x62, c=0x63, d=0x64, e=0x65, f=0x66, g=0x67, h=0x68,
    i=0x69, j=0x6A, k=0x6B, l=0x6C, m=0x6D, n=0x6E, o=0x6F, p=0x70,
    q=0x71, r=0x72, s=0x73, t=0x74, u=0x75, v=0x76, w=0x77, x=0x78,
    z=0x7A,
    Space  = 0x20,
    Enter  = 0x0D,
    BkSpc  = 0x08,
    Shift  = 0x80,
    TrkUp  = 0x81,
    TrkDn  = 0x82,
    TrkLt  = 0x83,
    TrkRt  = 0x84,
    TrkClk = 0x85,
}

local KEY_NAMES = {}
for name, code in pairs(KEYS) do KEY_NAMES[code] = name end

-- DOS needs keys the T-Deck doesn't have. All unbound by default — binding a
-- physical key STEALS it from typing, so users bind only what a game needs.
local ACTIONS = {
    { id="esc",    label="Esc",    pc=PC.ESC    },
    { id="up",     label="Up",     pc=PC.UP     },
    { id="down",   label="Down",   pc=PC.DOWN   },
    { id="left",   label="Left",   pc=PC.LEFT   },
    { id="right",  label="Right",  pc=PC.RIGHT  },
    { id="ctrl",   label="Ctrl",   pc=PC.CTRL   },
    { id="alt",    label="Alt",    pc=PC.ALT    },
    { id="tab",    label="Tab",    pc=PC.TAB    },
    { id="del",    label="Del",    pc=PC.DEL    },
    { id="f1",     label="F1",     pc=PC.F1     },
    { id="f2",     label="F2",     pc=PC.F2     },
    { id="f3",     label="F3",     pc=PC.F3     },
    { id="f4",     label="F4",     pc=PC.F4     },
    { id="f5",     label="F5",     pc=PC.F5     },
    { id="f6",     label="F6",     pc=PC.F6     },
    { id="f7",     label="F7",     pc=PC.F7     },
    { id="f8",     label="F8",     pc=PC.F8     },
    { id="f9",     label="F9",     pc=PC.F9     },
    { id="f10",    label="F10",    pc=PC.F10    },
    { id="rmouse", label="R.Mouse", pc=PC.RMOUSE },
}

local bindings = {}

local function load_defaults()
    bindings = {}
    for _, a in ipairs(ACTIONS) do
        bindings[a.id] = { pc = a.pc, key1 = nil, key2 = nil }
    end
end

-- Build the -keymap hex string. Returns nil when nothing is bound (then we
-- omit -keymap entirely = full passthrough). The firmware keymap is a pure
-- remapper (unmapped codes pass through unchanged), so only the actual
-- bindings are emitted — no identity entries needed.
local function build_keymap_string()
    local parts = {}
    for _, a in ipairs(ACTIONS) do
        local b = bindings[a.id]
        if b and (b.key1 or b.key2) then
            local s = string.format("%02X=", b.pc)
            if b.key1 then
                s = s .. string.format("%02X", b.key1)
                if b.key2 then s = s .. string.format("+%02X", b.key2) end
            else
                s = s .. string.format("%02X", b.key2)
            end
            parts[#parts + 1] = s
        end
    end
    if #parts == 0 then return nil end
    return table.concat(parts, ",")
end

-- Trackball momentum settings (used in arrow-keys mode, i.e. mouse off)
local trk_momentum = true
local trk_impulse  = 15
local trk_friction = 82
local trk_thresh   = 4

local function build_trkball_string()
    return string.format("%d,%d,%d,%d",
        trk_momentum and 1 or 0, trk_impulse, trk_friction, trk_thresh)
end

-- ============================================================
-- Emulator settings
-- ============================================================
local MHZ_STEPS = { 5, 8, 10, 12, 16, 20 }
local sel_mhz   = 4      -- 12 MHz default
local audio_on  = true
local mouse_speed = 3    -- 0 = off (trackball = arrow keys), 1..5 = mouse
local BOOTS = { "auto", "a", "c" }
local sel_boot = 1

local function save_config()
    local f = io.open(CFG_PATH, "w")
    if not f then return end
    for _, a in ipairs(ACTIONS) do
        local b = bindings[a.id]
        local k1 = b.key1 and string.format("%02X", b.key1) or "--"
        local k2 = b.key2 and string.format("%02X", b.key2) or "--"
        f:write(a.id .. "=" .. k1 .. "," .. k2 .. "\n")
    end
    f:write(string.format("trk_momentum=%d\n", trk_momentum and 1 or 0))
    f:write(string.format("trk_impulse=%d\n", trk_impulse))
    f:write(string.format("trk_friction=%d\n", trk_friction))
    f:write(string.format("trk_thresh=%d\n", trk_thresh))
    f:write(string.format("mhz=%d\n", sel_mhz))
    f:write(string.format("audio=%d\n", audio_on and 1 or 0))
    f:write(string.format("mousespd=%d\n", mouse_speed))
    f:write(string.format("bootsel=%d\n", sel_boot))
    if sel_fda > 1 then f:write("fda=" .. found_imgs[sel_fda - 1].name .. "\n") end
    if sel_hda > 1 then
        local c = hda_choices[sel_hda - 1]
        if c and c.kind == "folder" then
            f:write("hdafolder=" .. c.name .. "\n")
        elseif c then
            f:write("hda=" .. c.name .. "\n")
        end
    end
    f:close()
end

local function load_config()
    load_defaults()
    local f = io.open(CFG_PATH, "r")
    if not f then return false end
    local text = f:read("*a")
    f:close()
    if not text then return false end
    for line in text:gmatch("[^\r\n]+") do
        local id, k1s, k2s = line:match("^([%w_]+)=(%S+),(%S+)$")
        if id and bindings[id] then
            bindings[id].key1 = (k1s ~= "--") and tonumber(k1s, 16) or nil
            bindings[id].key2 = (k2s ~= "--") and tonumber(k2s, 16) or nil
        end
        local val = line:match("^trk_momentum=([01])$")
        if val then trk_momentum = (val == "1") end
        local trk_key, trk_val = line:match("^(trk_%a+)=(%d+)$")
        if trk_key == "trk_impulse" then trk_impulse = tonumber(trk_val) end
        if trk_key == "trk_friction" then trk_friction = tonumber(trk_val) end
        if trk_key == "trk_thresh" then trk_thresh = tonumber(trk_val) end
        local v = line:match("^mhz=(%d+)$")
        if v then
            v = tonumber(v)
            if v >= 1 and v <= #MHZ_STEPS then sel_mhz = v end
        end
        v = line:match("^audio=([01])$")
        if v then audio_on = (v == "1") end
        v = line:match("^mousespd=(%d+)$")
        if v then
            v = tonumber(v)
            if v >= 0 and v <= 5 then mouse_speed = v end
        end
        v = line:match("^bootsel=(%d+)$")
        if v then
            v = tonumber(v)
            if v >= 1 and v <= #BOOTS then sel_boot = v end
        end
        local fname = line:match("^fda=(.+)$")
        if fname then sel_fda_name = fname end
        fname = line:match("^hda=(.+)$")
        if fname then sel_hda_name = fname end
        fname = line:match("^hdafolder=(.+)$")
        if fname then sel_hda_folder_name = fname end
    end
    return true
end

local function key_display(code)
    if not code then return "---" end
    return KEY_NAMES[code] or string.format("0x%02X", code)
end

local BINDABLE_KEYS = {}
for name, code in pairs(KEYS) do
    BINDABLE_KEYS[#BINDABLE_KEYS + 1] = { name = name, code = code }
end
table.sort(BINDABLE_KEYS, function(a, b) return a.name < b.name end)

-- ============================================================
-- Screen management
-- ============================================================
local function create_main_screen() end
local function create_controls_screen() end
local function create_bind_screen(action_idx, slot) end
local function create_input_screen() end
local function create_settings_screen() end

-- ============================================================
-- Main screen
-- ============================================================
create_main_screen = function()
    if scr then scr:delete() end

    scr = root:Object({
        w = W, h = H,
        bg_color = "#000000", bg_opa = lvgl.OPA(255),
        border_width = 0, pad_all = 0,
    })
    scr:Label{
        text = "PC-XT",
        text_font = lvgl.BUILTIN_FONT.MONTSERRAT_22,
        text_color = "#55AAFF",
        align = { type = lvgl.ALIGN.TOP_MID, y_ofs = 4 },
    }

    local img_opts = "None"
    if #found_imgs > 0 then
        local names = { "None" }
        for i, r in ipairs(found_imgs) do names[i + 1] = r.name end
        img_opts = table.concat(names, "\n")
    end

    -- C: accepts images and folders
    local hda_names = { "None" }
    for _, c in ipairs(hda_choices) do
        hda_names[#hda_names + 1] =
            (c.kind == "folder") and ("Folder: " .. c.name) or c.name
    end
    local hda_opts = table.concat(hda_names, "\n")

    local font12 = lvgl.BUILTIN_FONT.MONTSERRAT_12

    scr:Label{
        text = "A:",
        text_font = font12, text_color = "#CCCCCC",
        align = { type = lvgl.ALIGN.TOP_LEFT, x_ofs = 8, y_ofs = 36 },
    }
    local fdaDd = scr:Dropdown{
        options = img_opts,
        w = 280, h = 26,
        align = { type = lvgl.ALIGN.TOP_LEFT, x_ofs = 30, y_ofs = 30 },
    }
    fdaDd:set{ selected = sel_fda - 1 }
    fdaDd:onevent(lvgl.EVENT.VALUE_CHANGED, function()
        sel_fda = fdaDd:get("selected") + 1
        save_config()
    end)

    scr:Label{
        text = "C:",
        text_font = font12, text_color = "#CCCCCC",
        align = { type = lvgl.ALIGN.TOP_LEFT, x_ofs = 8, y_ofs = 66 },
    }
    local hdaDd = scr:Dropdown{
        options = hda_opts,
        w = 280, h = 26,
        align = { type = lvgl.ALIGN.TOP_LEFT, x_ofs = 30, y_ofs = 60 },
    }
    hdaDd:set{ selected = sel_hda - 1 }
    hdaDd:onevent(lvgl.EVENT.VALUE_CHANGED, function()
        sel_hda = hdaDd:get("selected") + 1
        save_config()
    end)

    local has_imgs = #found_imgs > 0 or #hda_choices > 0
    local status = scr:Label{
        text = has_imgs and "Ready" or "Put .img disks / game folders in S:/dos/",
        text_font = font12,
        text_color = has_imgs and "#888888" or "#FF6666",
        align = { type = lvgl.ALIGN.TOP_MID, y_ofs = 92 },
    }

    local btnBox = scr:Object{
        w = 220, h = lvgl.SIZE_CONTENT,
        align = { type = lvgl.ALIGN.TOP_MID, y_ofs = 108 },
        bg_opa = 0, border_width = 0, pad_all = 2,
        flex = {
            flex_direction = "row",
            flex_wrap = "wrap",
            justify_content = "center",
            column_gap = 6, row_gap = 6,
        },
    }
    btnBox:clear_flag(lvgl.FLAG.SCROLLABLE)

    local launchBtn = btnBox:Button{ w = 100, h = 30 }
    launchBtn:Label{ text = "Boot", align = lvgl.ALIGN.CENTER }
    launchBtn:onClicked(function()
        if sel_fda == 1 and sel_hda == 1 then
            status:set{ text = "Select a boot disk first!" }
            return
        end
        local hda_choice = (sel_hda > 1) and hda_choices[sel_hda - 1] or nil
        if hda_choice and hda_choice.kind == "folder" and sel_fda == 1 then
            -- Folder C: is a data drive; DOS itself must come off a floppy.
            status:set{ text = "Folder C: needs a boot floppy in A:" }
            return
        end
        status:set{ text = "Booting..." }
        lvgl.Timer{
            period = 50,
            cb = function(t)
                t:delete()
                local args = { ELF_PATH }
                if sel_fda > 1 then
                    args[#args + 1] = "-fda"
                    args[#args + 1] = to_vfs_path(found_imgs[sel_fda - 1].path)
                end
                if hda_choice and hda_choice.kind == "folder" then
                    local man = write_cdrive_manifest(hda_choice.path)
                    if man then
                        args[#args + 1] = "-cfolder"
                        args[#args + 1] = man
                    end
                elseif hda_choice then
                    args[#args + 1] = "-hda"
                    args[#args + 1] = to_vfs_path(hda_choice.path)
                end
                if hda_choice and hda_choice.kind == "folder" then
                    -- Folder C: is never bootable — always start from A:
                    args[#args + 1] = "-boot"
                    args[#args + 1] = "a"
                elseif BOOTS[sel_boot] ~= "auto" then
                    args[#args + 1] = "-boot"
                    args[#args + 1] = BOOTS[sel_boot]
                end
                args[#args + 1] = "-mhz"
                args[#args + 1] = tostring(MHZ_STEPS[sel_mhz])
                if not audio_on then
                    args[#args + 1] = "-audio"
                    args[#args + 1] = "0"
                end
                local km = build_keymap_string()
                if km then
                    args[#args + 1] = "-keymap"
                    args[#args + 1] = km
                end
                if mouse_speed > 0 then
                    -- Mouse mode: module reads raw deltas; no -trkball needed
                    args[#args + 1] = "-mouse"
                    args[#args + 1] = tostring(mouse_speed)
                else
                    -- Arrow-keys mode: trackball momentum drives 0x81-0x84
                    args[#args + 1] = "-trkball"
                    args[#args + 1] = build_trkball_string()
                end
                _launch_elf(table.unpack(args))
            end
        }
    end)

    local ctrlBtn = btnBox:Button{ w = 100, h = 30 }
    ctrlBtn:Label{ text = "Help", align = lvgl.ALIGN.CENTER }
    ctrlBtn:onClicked(function() create_controls_screen() end)

    local setBtn = btnBox:Button{ w = 100, h = 30 }
    setBtn:Label{ text = "Settings", align = lvgl.ALIGN.CENTER }
    setBtn:onClicked(function() create_settings_screen() end)

    local quitBtn = btnBox:Button{ w = 100, h = 30 }
    quitBtn:Label{ text = "Quit", align = lvgl.ALIGN.CENTER }
    quitBtn:onClicked(function() apps.go_home() end)

    local grp = lvgl.group.get_default()
    grp:add_obj(fdaDd)
    grp:add_obj(hdaDd)
    _gridnav_add(btnBox, GRIDNAV_ROLLOVER)
    grp:add_obj(btnBox)
end

-- ============================================================
-- Settings screen
-- ============================================================
create_settings_screen = function()
    if scr then scr:delete() end

    scr = root:Object({
        w = W, h = H,
        bg_color = "#000000", bg_opa = lvgl.OPA(255),
        border_width = 0, pad_all = 0,
    })
    scr:clear_flag(lvgl.FLAG.SCROLLABLE)

    scr:Label{
        text = "SETTINGS",
        text_font = lvgl.BUILTIN_FONT.MONTSERRAT_14,
        text_color = "#55AAFF",
        align = { type = lvgl.ALIGN.TOP_MID, y_ofs = 4 },
    }

    local font = lvgl.BUILTIN_FONT.MONTSERRAT_12
    local list = scr:Object{
        w = W - 4, h = H - 54,
        align = { type = lvgl.ALIGN.TOP_MID, y_ofs = 22 },
        bg_opa = 0, border_width = 0,
        pad_left = 4, pad_right = 4, pad_top = 2, pad_bottom = 2,
        flex = { flex_direction = "column", row_gap = 4 },
    }

    local function setting_row(parent, label, get_text, on_click)
        local row = parent:Object{
            w = lvgl.PCT(100), h = 26,
            bg_opa = 0, border_width = 0, pad_all = 0,
            flex = { flex_direction = "row", cross_place = "center" },
        }
        row:clear_flag(lvgl.FLAG.SCROLLABLE)
        row:Label{
            text = label,
            text_font = font, text_color = "#CCCCCC",
            w = 120,
        }
        local valBtn = row:Button{ w = 130, h = 24 }
        local valLbl = valBtn:Label{
            text = get_text(),
            text_font = font,
            align = lvgl.ALIGN.CENTER,
        }
        valBtn:onClicked(function()
            on_click()
            valLbl:set{ text = get_text() }
            save_config()
        end)
        return valBtn
    end

    setting_row(list, "CPU speed",
        function() return string.format("< %d MHz >", MHZ_STEPS[sel_mhz]) end,
        function()
            sel_mhz = sel_mhz + 1
            if sel_mhz > #MHZ_STEPS then sel_mhz = 1 end
        end
    )

    setting_row(list, "Boot drive",
        function()
            local names = { "< Auto >", "< A: floppy >", "< C: hard disk >" }
            return names[sel_boot]
        end,
        function()
            sel_boot = sel_boot + 1
            if sel_boot > #BOOTS then sel_boot = 1 end
        end
    )

    setting_row(list, "Audio",
        function() return audio_on and "< ON >" or "< OFF >" end,
        function() audio_on = not audio_on end
    )

    setting_row(list, "Trackball",
        function()
            if mouse_speed == 0 then return "< Arrow keys >" end
            return string.format("< Mouse x%d >", mouse_speed)
        end,
        function()
            mouse_speed = mouse_speed + 1
            if mouse_speed > 5 then mouse_speed = 0 end
        end
    )

    list:Label{
        text = "Mouse mode needs MOUSE.COM in DOS\n"
             .. "(serial mouse on COM2).\n"
             .. "Lower CPU MHz for speed-sensitive games.",
        text_font = font,
        text_color = "#666666",
        w = lvgl.PCT(100),
    }

    local btnBar = scr:Object{
        w = W, h = 28,
        align = { type = lvgl.ALIGN.BOTTOM_MID, y_ofs = 0 },
        bg_opa = 0, border_width = 0, pad_all = 0,
        flex = {
            flex_direction = "row",
            justify_content = "center",
            column_gap = 8,
        },
    }
    btnBar:clear_flag(lvgl.FLAG.SCROLLABLE)

    local backBtn = btnBar:Button{ w = 60, h = 26 }
    backBtn:Label{ text = "Back", text_font = font, align = lvgl.ALIGN.CENTER }
    backBtn:onClicked(function() create_main_screen() end)

    _gridnav_add(list, GRIDNAV_ROLLOVER)
    _gridnav_add(btnBar, GRIDNAV_ROLLOVER)
    local grp = lvgl.group.get_default()
    grp:add_obj(list)
    grp:add_obj(btnBar)
end

-- ============================================================
-- Keyboard help screen
-- ============================================================
-- Numbers, symbols, F-keys and backslash are reached natively via the T-Deck
-- SYM and ALT modifiers (decoded in the firmware's ELF keyboard path + the
-- module's ascii_to_scan). This screen documents that scheme.
create_controls_screen = function()
    if scr then scr:delete() end

    scr = root:Object({
        w = W, h = H,
        bg_color = "#000000", bg_opa = lvgl.OPA(255),
        border_width = 0, pad_all = 0,
    })
    scr:clear_flag(lvgl.FLAG.SCROLLABLE)

    scr:Label{
        text = "KEYBOARD HELP",
        text_font = lvgl.BUILTIN_FONT.MONTSERRAT_14,
        text_color = "#55AAFF",
        align = { type = lvgl.ALIGN.TOP_MID, y_ofs = 4 },
    }

    local font = lvgl.BUILTIN_FONT.MONTSERRAT_12
    local list = scr:Object{
        w = W - 4, h = H - 54,
        align = { type = lvgl.ALIGN.TOP_MID, y_ofs = 22 },
        bg_opa = 0, border_width = 0,
        pad_left = 6, pad_right = 6, pad_top = 2, pad_bottom = 2,
        flex = { flex_direction = "column", row_gap = 1 },
    }
    -- content outgrew one screen with the USB section — let it scroll

    local function head(t)
        list:Label{ text = t, text_font = font, text_color = "#55AAFF",
                    w = lvgl.PCT(100) }
    end
    local function body(t)
        list:Label{ text = t, text_font = font, text_color = "#CCCCCC",
                    w = lvgl.PCT(100) }
    end

    head("Numbers & symbols")
    body("Hold SYM + key.  SYM+1 = 1, SYM+! = !")

    head("Function keys F1-F10")
    body("Hold ALT + a number key.\nALT+1 = F1  ...  ALT+0 = F10")

    head("Backslash  \\  (DOS paths)")
    body("ALT + / key (the G key).   e.g. cd \\dos")

    head("USB keyboard (Tools/USB host on)")
    body("Everything is native: arrows, F1-F12,\nCtrl, Alt, Home/End/PgUp/PgDn/Ins/Del,\nCapsLock. NumLock inert: keypad = digits,\narrow keys always navigate.")

    head("Quit to launcher")
    body("Hold Backspace about 1.5 seconds.")

    local btnBar = scr:Object{
        w = W, h = 28,
        align = { type = lvgl.ALIGN.BOTTOM_MID, y_ofs = 0 },
        bg_opa = 0, border_width = 0, pad_all = 0,
        flex = {
            flex_direction = "row",
            justify_content = "center",
            column_gap = 8,
        },
    }
    btnBar:clear_flag(lvgl.FLAG.SCROLLABLE)

    local trkBtn = btnBar:Button{ w = 80, h = 26 }
    trkBtn:Label{ text = "Trackball", text_font = font, align = lvgl.ALIGN.CENTER }
    trkBtn:onClicked(function() create_input_screen() end)

    local backBtn = btnBar:Button{ w = 60, h = 26 }
    backBtn:Label{ text = "Back", text_font = font, align = lvgl.ALIGN.CENTER }
    backBtn:onClicked(function() create_main_screen() end)

    _gridnav_add(btnBar, GRIDNAV_ROLLOVER)
    local grp = lvgl.group.get_default()
    grp:add_obj(btnBar)
end

-- ============================================================
-- Key binding picker screen
-- ============================================================
create_bind_screen = function(action_idx, slot)
    local a = ACTIONS[action_idx]
    local b = bindings[a.id]
    if scr then scr:delete() end

    scr = root:Object({
        w = W, h = H,
        bg_color = "#000000", bg_opa = lvgl.OPA(255),
        border_width = 0, pad_all = 0,
    })
    scr:clear_flag(lvgl.FLAG.SCROLLABLE)

    local slot_name = (slot == 1) and "Primary" or "Alt"
    scr:Label{
        text = a.label .. " - " .. slot_name,
        text_font = lvgl.BUILTIN_FONT.MONTSERRAT_14,
        text_color = "#55AAFF",
        align = { type = lvgl.ALIGN.TOP_MID, y_ofs = 4 },
    }

    local list = scr:Object{
        w = W - 4, h = H - 54,
        align = { type = lvgl.ALIGN.TOP_MID, y_ofs = 22 },
        bg_opa = 0, border_width = 0,
        pad_left = 4, pad_right = 4, pad_top = 2, pad_bottom = 2,
        flex = { flex_direction = "column" },
    }

    local font = lvgl.BUILTIN_FONT.MONTSERRAT_12
    local current = (slot == 1) and b.key1 or b.key2

    local clrBtn = list:Button{ w = lvgl.PCT(100), h = 22 }
    clrBtn:Label{ text = "--- (clear)", text_font = font, align = lvgl.ALIGN.CENTER }
    clrBtn:onClicked(function()
        if slot == 1 then b.key1 = nil else b.key2 = nil end
        save_config()
        create_controls_screen()
    end)

    for _, k in ipairs(BINDABLE_KEYS) do
        local btn = list:Button{ w = lvgl.PCT(100), h = 22 }
        local lbl = k.name
        if k.code == current then lbl = "> " .. lbl .. " <" end
        btn:Label{ text = lbl, text_font = font, align = lvgl.ALIGN.CENTER }
        btn:onClicked(function()
            if slot == 1 then b.key1 = k.code else b.key2 = k.code end
            save_config()
            create_controls_screen()
        end)
    end

    local cancelBtn = scr:Button{
        w = 80, h = 26,
        align = { type = lvgl.ALIGN.BOTTOM_MID, y_ofs = -2 },
    }
    cancelBtn:Label{ text = "Cancel", text_font = font, align = lvgl.ALIGN.CENTER }
    cancelBtn:onClicked(function() create_controls_screen() end)

    _gridnav_add(list, GRIDNAV_ROLLOVER)
    local grp = lvgl.group.get_default()
    grp:add_obj(list)
    grp:add_obj(cancelBtn)
end

-- ============================================================
-- Input settings screen (trackball tuning for arrow-keys mode)
-- ============================================================
create_input_screen = function()
    if scr then scr:delete() end

    scr = root:Object({
        w = W, h = H,
        bg_color = "#000000", bg_opa = lvgl.OPA(255),
        border_width = 0, pad_all = 0,
    })
    scr:clear_flag(lvgl.FLAG.SCROLLABLE)

    scr:Label{
        text = "INPUT SETTINGS",
        text_font = lvgl.BUILTIN_FONT.MONTSERRAT_14,
        text_color = "#55AAFF",
        align = { type = lvgl.ALIGN.TOP_MID, y_ofs = 4 },
    }

    local font = lvgl.BUILTIN_FONT.MONTSERRAT_12
    local list = scr:Object{
        w = W - 4, h = H - 54,
        align = { type = lvgl.ALIGN.TOP_MID, y_ofs = 22 },
        bg_opa = 0, border_width = 0,
        pad_left = 4, pad_right = 4, pad_top = 2, pad_bottom = 2,
        flex = { flex_direction = "column", row_gap = 4 },
    }

    local function setting_row(parent, label, get_text, on_click)
        local row = parent:Object{
            w = lvgl.PCT(100), h = 26,
            bg_opa = 0, border_width = 0, pad_all = 0,
            flex = { flex_direction = "row", cross_place = "center" },
        }
        row:clear_flag(lvgl.FLAG.SCROLLABLE)
        row:Label{
            text = label,
            text_font = font, text_color = "#CCCCCC",
            w = 120,
        }
        local valBtn = row:Button{ w = 130, h = 24 }
        local valLbl = valBtn:Label{
            text = get_text(),
            text_font = font,
            align = lvgl.ALIGN.CENTER,
        }
        valBtn:onClicked(function()
            on_click()
            valLbl:set{ text = get_text() }
            save_config()
        end)
        return valBtn
    end

    setting_row(list, "Momentum",
        function() return trk_momentum and "< ON >" or "< OFF >" end,
        function() trk_momentum = not trk_momentum end
    )

    setting_row(list, "Sensitivity",
        function() return string.format("< %.1f >", trk_impulse / 10) end,
        function()
            trk_impulse = trk_impulse + 1
            if trk_impulse > 30 then trk_impulse = 5 end
        end
    )

    setting_row(list, "Friction",
        function() return string.format("< %.2f >", trk_friction / 100) end,
        function()
            trk_friction = trk_friction + 2
            if trk_friction > 95 then trk_friction = 50 end
        end
    )

    setting_row(list, "Dead Zone",
        function() return string.format("< %.1f >", trk_thresh / 10) end,
        function()
            trk_thresh = trk_thresh + 1
            if trk_thresh > 10 then trk_thresh = 2 end
        end
    )

    list:Label{
        text = "These apply in Arrow-keys trackball mode\n"
             .. "(Settings -> Trackball -> Arrow keys).",
        text_font = font,
        text_color = "#666666",
        w = lvgl.PCT(100),
    }

    local btnBar = scr:Object{
        w = W, h = 28,
        align = { type = lvgl.ALIGN.BOTTOM_MID, y_ofs = 0 },
        bg_opa = 0, border_width = 0, pad_all = 0,
        flex = {
            flex_direction = "row",
            justify_content = "center",
            column_gap = 8,
        },
    }
    btnBar:clear_flag(lvgl.FLAG.SCROLLABLE)

    local resetBtn = btnBar:Button{ w = 65, h = 26 }
    resetBtn:Label{ text = "Reset", text_font = font, align = lvgl.ALIGN.CENTER }
    resetBtn:onClicked(function()
        trk_momentum = true
        trk_impulse = 15
        trk_friction = 82
        trk_thresh = 4
        save_config()
        create_input_screen()
    end)

    local backBtn = btnBar:Button{ w = 60, h = 26 }
    backBtn:Label{ text = "Back", text_font = font, align = lvgl.ALIGN.CENTER }
    backBtn:onClicked(function() create_controls_screen() end)

    _gridnav_add(list, GRIDNAV_ROLLOVER)
    _gridnav_add(btnBar, GRIDNAV_ROLLOVER)
    local grp = lvgl.group.get_default()
    grp:add_obj(list)
    grp:add_obj(btnBar)
end

-- ============================================================
-- Startup
-- ============================================================
load_defaults()
local init_phase = 0
return function()
    init_phase = init_phase + 1

    if init_phase == 1 then
        scan_dir_for_imgs(app_dir)
        return false
    elseif init_phase == 2 then
        scan_dir_for_imgs(sd_app_dir)
        scan_dir_for_folders(sd_app_dir)
        return false
    elseif init_phase == 3 then
        if sd_app_dir ~= "S:/dos" then
            scan_dir_for_imgs("S:/dos")
            scan_dir_for_folders("S:/dos")
        end
        return false
    end

    table.sort(found_imgs, function(a, b)
        return a.name:lower() < b.name:lower()
    end)
    table.sort(found_folders, function(a, b)
        return a.name:lower() < b.name:lower()
    end)
    for _, r in ipairs(found_imgs) do
        hda_choices[#hda_choices + 1] = { kind = "img", name = r.name, path = r.path }
    end
    for _, r in ipairs(found_folders) do
        hda_choices[#hda_choices + 1] = { kind = "folder", name = r.name, path = r.path }
    end
    load_config()
    -- Restore selections by name
    if sel_fda_name then
        for i, r in ipairs(found_imgs) do
            if r.name == sel_fda_name then sel_fda = i + 1; break end
        end
    end
    if sel_hda_name or sel_hda_folder_name then
        for i, c in ipairs(hda_choices) do
            if (c.kind == "img" and c.name == sel_hda_name)
                or (c.kind == "folder" and c.name == sel_hda_folder_name) then
                sel_hda = i + 1
                break
            end
        end
    end
    create_main_screen()
    return true
end
