local lvgl = require("lvgl")
local apps = require("lib/apps")
local fileman = require("lib/fileman")

local app_dir = ...

local W = lvgl.HOR_RES()
local H = lvgl.VER_RES()

-- ============================================================
-- Doom Launcher with configurable controls
-- ============================================================

-- Derive SD-side mirror of app directory:
-- "L:/lua/apps/Games/Doom" → "S:/lua/apps/Games/Doom"
local sd_app_dir = app_dir:gsub("^L:", "S:")

-- Convert firmware path prefixes to VFS mount points for the ELF module's fopen
local function to_vfs_path(path)
    if path:sub(1, 2) == "S:" then return "/sd" .. path:sub(3) end
    if path:sub(1, 2) == "L:" then return "/littlefs" .. path:sub(3) end
    return path
end

-- Search for a file: check app dir, then SD mirror, then legacy S:/doom/
-- Returns the firmware-convention path (S:/L:) — caller converts to VFS if needed
local function find_file(name)
    local search = { app_dir, sd_app_dir, "S:/doom" }
    for _, dir in ipairs(search) do
        local path = dir .. "/" .. name
        local f = io.open(path, "r")
        if f then f:close(); return path end
    end
    return nil
end

local CFG_PATH = app_dir .. "/controls.cfg"
local ELF_PATH = find_file("doom.app.elf") or sd_app_dir .. "/doom.app.elf"

-- Check WAD type by reading the 4-byte header: "IWAD" or "PWAD"
local function wad_type(path)
    local f = io.open(path, "r")
    if not f then return "unknown" end
    local hdr = f:read(4)
    f:close()
    if not hdr then return "unknown" end
    if hdr == "IWAD" then return "iwad"
    elseif hdr == "PWAD" then return "pwad"
    else return "unknown" end
end

local found_wads = {}  -- { {name, path, wtype="iwad"|"pwad"}, ... }
local seen_lower = {}

-- fileman routes the drive from the L:/S: prefix itself; sizes=false skips
-- the per-entry size lookup, so huge WAD folders list fast (watchdog-safe).
local function scan_dir_for_wads(dir_path)
    local entries = fileman.list(dir_path, {
        sizes = false,
        filter = function(e)
            return e.type == "file" and e.name:lower():match("%.wad$")
        end,
    }) or {}
    for _, e in ipairs(entries) do
        local low = e.name:lower()
        if not seen_lower[low] then
            seen_lower[low] = true
            found_wads[#found_wads + 1] = {
                name = e.name,
                path = dir_path .. "/" .. e.name,
                wtype = "unknown",  -- classified one-per-tick in deferred init
            }
        end
    end
end

local iwad_list = {}
local selected_wad = 1
local selected_base = 1   -- index into iwad_list
local selected_wad_name = nil   -- persisted in config
local selected_base_name = nil  -- persisted in config

-- Doom keycodes
local DK = {
    UP        = 0xAD,
    DOWN      = 0xAF,
    LEFT      = 0xAC,
    RIGHT     = 0xAE,
    STRAFEL   = 0xA0,
    STRAFER   = 0xA1,
    FIRE      = 0xA3,
    USE       = 0xA2,  -- KEY_USE in Doom
    RUN       = 0xB6,  -- rshift
    ENTER     = 0x0D,
    ESCAPE    = 0x1B,
}

-- Physical key names → hex codes for the keymap string.
-- Printable ASCII uses their char code; specials use pseudo-codes.
local KEYS = {
    -- Letters
    a=0x61, b=0x62, c=0x63, d=0x64, e=0x65, f=0x66, g=0x67, h=0x68,
    i=0x69, j=0x6A, k=0x6B, l=0x6C, m=0x6D, n=0x6E, o=0x6F, p=0x70,
    q=0x71, r=0x72, s=0x73, t=0x74, u=0x75, v=0x76, w=0x77, x=0x78,
    z=0x7A,
    -- Special keys
    Space  = 0x20,
    Enter  = 0x0D,
    BkSpc  = 0x08,  -- backspace key (host produces raw BS code)
    Shift  = 0x80,
    -- Trackball
    TrkUp  = 0x81,
    TrkDn  = 0x82,
    TrkLt  = 0x83,
    TrkRt  = 0x84,
    TrkClk = 0x85,
}

-- Reverse lookup: hex code → display name
local KEY_NAMES = {}
for name, code in pairs(KEYS) do KEY_NAMES[code] = name end

-- Action definitions: {id, label, doom_keycode, default_key1, default_key2}
local ACTIONS = {
    { id="fwd",     label="Forward",      doom=DK.UP,      key1=KEYS.w,     key2=KEYS.TrkUp  },
    { id="back",    label="Backward",     doom=DK.DOWN,    key1=KEYS.s,     key2=KEYS.TrkDn  },
    { id="sleft",   label="Strafe Left",  doom=DK.STRAFEL, key1=KEYS.a,     key2=nil         },
    { id="sright",  label="Strafe Right", doom=DK.STRAFER, key1=KEYS.d,     key2=nil         },
    { id="tleft",   label="Turn Left",    doom=DK.LEFT,    key1=KEYS.j,     key2=KEYS.TrkLt  },
    { id="tright",  label="Turn Right",   doom=DK.RIGHT,   key1=KEYS.l,     key2=KEYS.TrkRt  },
    { id="fire",    label="Fire",         doom=DK.FIRE,    key1=KEYS.Space, key2=KEYS.TrkClk },
    { id="use",     label="Use / Open",   doom=DK.USE,     key1=KEYS.e,     key2=nil         },
    { id="run",     label="Run",          doom=DK.RUN,     key1=KEYS.Shift, key2=nil         },
    { id="enter",   label="Menu OK",      doom=DK.ENTER,   key1=KEYS.Enter, key2=nil         },
    { id="esc",     label="Menu / ESC",   doom=DK.ESCAPE,  key1=KEYS.BkSpc, key2=nil         },
    -- Weapon select (bottom row z-m + t,g for easy access)
    { id="wp1",     label="Weapon 1",     doom=0x31,       key1=KEYS.z,     key2=nil         },
    { id="wp2",     label="Weapon 2",     doom=0x32,       key1=KEYS.x,     key2=nil         },
    { id="wp3",     label="Weapon 3",     doom=0x33,       key1=KEYS.c,     key2=nil         },
    { id="wp4",     label="Weapon 4",     doom=0x34,       key1=KEYS.v,     key2=nil         },
    { id="wp5",     label="Weapon 5",     doom=0x35,       key1=KEYS.b,     key2=nil         },
    { id="wp6",     label="Weapon 6",     doom=0x36,       key1=KEYS.n,     key2=nil         },
    { id="wp7",     label="Weapon 7",     doom=0x37,       key1=KEYS.m,     key2=nil         },
    { id="wp8",     label="Weapon 8",     doom=0x38,       key1=KEYS.t,     key2=nil         },
    { id="wp9",     label="Weapon 9",     doom=0x39,       key1=KEYS.g,     key2=nil         },
}

-- Working copy of bindings (populated from defaults or config file)
local bindings = {}  -- bindings[action_id] = {doom=N, key1=N|nil, key2=N|nil}

local function load_defaults()
    bindings = {}
    for _, a in ipairs(ACTIONS) do
        bindings[a.id] = { doom = a.doom, key1 = a.key1, key2 = a.key2 }
    end
end

-- Build the -keymap hex string from current bindings
local function build_keymap_string()
    local parts = {}
    for _, a in ipairs(ACTIONS) do
        local b = bindings[a.id]
        if b and (b.key1 or b.key2) then
            local s = string.format("%02X=", b.doom)
            if b.key1 then
                s = s .. string.format("%02X", b.key1)
                if b.key2 then
                    s = s .. string.format("+%02X", b.key2)
                end
            elseif b.key2 then
                s = s .. string.format("%02X", b.key2)
            end
            parts[#parts + 1] = s
        end
    end
    return table.concat(parts, ",")
end

-- Audio toggle state
local sfx_enabled = true
local music_enabled = true

-- Trackball momentum settings
local trk_momentum = true     -- enable momentum mode
local trk_impulse  = 15       -- impulse * 10 (1.5 → 15)
local trk_friction = 82       -- friction * 100 (0.82 → 82)
local trk_thresh   = 4        -- threshold * 10 (0.4 → 4)

-- Build the -trkball argument string
local function build_trkball_string()
    return string.format("%d,%d,%d,%d",
        trk_momentum and 1 or 0, trk_impulse, trk_friction, trk_thresh)
end

-- Save bindings + settings to config file
local function save_config()
    local f = io.open(CFG_PATH, "w")
    if not f then return end
    for _, a in ipairs(ACTIONS) do
        local b = bindings[a.id]
        local k1 = b.key1 and string.format("%02X", b.key1) or "--"
        local k2 = b.key2 and string.format("%02X", b.key2) or "--"
        f:write(a.id .. "=" .. k1 .. "," .. k2 .. "\n")
    end
    f:write(string.format("sfx=%d\n", sfx_enabled and 1 or 0))
    f:write(string.format("music=%d\n", music_enabled and 1 or 0))
    f:write(string.format("trk_momentum=%d\n", trk_momentum and 1 or 0))
    f:write(string.format("trk_impulse=%d\n", trk_impulse))
    f:write(string.format("trk_friction=%d\n", trk_friction))
    f:write(string.format("trk_thresh=%d\n", trk_thresh))
    if #found_wads > 0 then
        f:write("wad=" .. found_wads[selected_wad].name .. "\n")
    end
    if #iwad_list > 0 then
        f:write("basewad=" .. iwad_list[selected_base].name .. "\n")
    end
    f:close()
end

-- Load bindings + settings from config file
local function load_config()
    load_defaults()
    local f = io.open(CFG_PATH, "r")
    if not f then return false end
    local text = f:read("*a")
    f:close()
    if not text then return false end
    for line in text:gmatch("[^\r\n]+") do
        local id, k1s, k2s = line:match("^(%w+)=(%S+),(%S+)$")
        if id and bindings[id] then
            bindings[id].key1 = (k1s ~= "--") and tonumber(k1s, 16) or nil
            bindings[id].key2 = (k2s ~= "--") and tonumber(k2s, 16) or nil
        end
        local setting, val = line:match("^(%a+)=([01])$")
        if setting == "sfx" then sfx_enabled = (val == "1") end
        if setting == "music" then music_enabled = (val == "1") end
        if setting == "trk_momentum" then trk_momentum = (val == "1") end
        local trk_key, trk_val = line:match("^(trk_%a+)=(%d+)$")
        if trk_key == "trk_impulse" then trk_impulse = tonumber(trk_val) end
        if trk_key == "trk_friction" then trk_friction = tonumber(trk_val) end
        if trk_key == "trk_thresh" then trk_thresh = tonumber(trk_val) end
        local wname = line:match("^wad=(.+)$")
        if wname then selected_wad_name = wname end
        local bname = line:match("^basewad=(.+)$")
        if bname then selected_base_name = bname end
    end
    -- Migrate old backspace code: host used to produce 0x1B for backspace,
    -- now produces 0x08. Convert any saved bindings that reference 0x1B as
    -- a physical key to 0x08 so ESC/menu still works after the update.
    for _, a in ipairs(ACTIONS) do
        local b = bindings[a.id]
        if b then
            if b.key1 == 0x1B then b.key1 = 0x08 end
            if b.key2 == 0x1B then b.key2 = 0x08 end
        end
    end
    -- Restore selections by name
    if selected_wad_name then
        for i, w in ipairs(found_wads) do
            if w.name == selected_wad_name then selected_wad = i; break end
        end
    end
    if selected_base_name then
        for i, b in ipairs(iwad_list) do
            if b.name == selected_base_name then selected_base = i; break end
        end
    end
    return true
end

local function key_display(code)
    if not code then return "---" end
    return KEY_NAMES[code] or string.format("0x%02X", code)
end

-- ============================================================
-- Screen management
-- ============================================================
-- Stable, manager-registered root. The menu/controls/keymap views swap by
-- replacing `scr` (a CHILD of this root); the root itself is never deleted, so
-- apps.go_home() (and a future home/back key) can tear the app down cleanly.
local root = apps.new_root({
    w = W, h = H,
    bg_color = "#000000", bg_opa = lvgl.OPA(255),
    border_width = 0, pad_all = 0,
})
root:clear_flag(lvgl.FLAG.SCROLLABLE)

local scr

local function create_main_screen() end
local function create_controls_screen() end
local function create_bind_screen(action_idx, slot) end
local function create_input_screen() end

-- All available keys for binding (sorted for display)
local BINDABLE_KEYS = {}
for name, code in pairs(KEYS) do
    BINDABLE_KEYS[#BINDABLE_KEYS + 1] = { name = name, code = code }
end
table.sort(BINDABLE_KEYS, function(a, b) return a.name < b.name end)

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

    -- Title
    scr:Label{
        text = "DOOM",
        text_font = lvgl.BUILTIN_FONT.MONTSERRAT_22,
        text_color = "#FF4444",
        align = { type = lvgl.ALIGN.TOP_MID, y_ofs = 10 },
    }

    -- WAD selector — dropdown over all found WADs (mods tagged)
    local wad_opts = "No WADs found"
    if #found_wads > 0 then
        local names = {}
        for i, w in ipairs(found_wads) do
            names[i] = w.name .. (w.wtype == "pwad" and " (mod)" or "")
        end
        wad_opts = table.concat(names, "\n")
    end

    local wadDd = scr:Dropdown{
        options = wad_opts,
        w = 240, h = 28,
        align = { type = lvgl.ALIGN.TOP_MID, y_ofs = 44 },
    }
    if #found_wads > 0 then
        wadDd:set{ selected = selected_wad - 1 }
    end

    -- Base IWAD selector (shown only when a PWAD is selected)
    local base_row = scr:Object{
        w = 240, h = 28,
        align = { type = lvgl.ALIGN.TOP_MID, y_ofs = 76 },
        bg_opa = 0, border_width = 0, pad_all = 0,
        flex = { flex_direction = "row", cross_place = "center" },
    }
    base_row:clear_flag(lvgl.FLAG.SCROLLABLE)
    base_row:Label{
        text = "Base:",
        text_color = "#AAAAAA",
        w = 50,
    }
    local base_opts = "No base game"
    if #iwad_list > 0 then
        local names = {}
        for i, b in ipairs(iwad_list) do names[i] = b.name end
        base_opts = table.concat(names, "\n")
    end
    local baseDd = base_row:Dropdown{
        options = base_opts,
        w = 190, h = 28,
    }
    if #iwad_list > 0 then
        baseDd:set{ selected = selected_base - 1 }
    end

    local function update_base_visibility()
        local is_pwad = #found_wads > 0 and found_wads[selected_wad].wtype == "pwad"
        if is_pwad then
            base_row:clear_flag(lvgl.FLAG.HIDDEN)
        else
            base_row:add_flag(lvgl.FLAG.HIDDEN)
        end
    end
    update_base_visibility()

    wadDd:onevent(lvgl.EVENT.VALUE_CHANGED, function()
        if #found_wads == 0 then return end
        selected_wad = wadDd:get("selected") + 1
        update_base_visibility()
        save_config()
    end)

    baseDd:onevent(lvgl.EVENT.VALUE_CHANGED, function()
        if #iwad_list == 0 then return end
        selected_base = baseDd:get("selected") + 1
        save_config()
    end)

    -- Status
    local has_wads = #found_wads > 0
    local status = scr:Label{
        text = has_wads and "Ready to launch"
               or "Place a .WAD in " .. sd_app_dir,
        text_font = lvgl.BUILTIN_FONT.MONTSERRAT_14,
        text_color = has_wads and "#888888" or "#FF6666",
        align = { type = lvgl.ALIGN.TOP_MID, y_ofs = 110 },
    }

    -- Options row: SFX + Music toggles
    local optBox = scr:Object{
        w = 260, h = lvgl.SIZE_CONTENT,
        align = { type = lvgl.ALIGN.TOP_MID, y_ofs = 138 },
        bg_opa = 0, border_width = 0, pad_all = 0,
        flex = {
            flex_direction = "row",
            justify_content = "center",
            column_gap = 8,
        },
    }
    optBox:clear_flag(lvgl.FLAG.SCROLLABLE)

    local sfxBtn = optBox:Button{ w = 100, h = 28 }
    local sfxLbl = sfxBtn:Label{
        text = sfx_enabled and "SFX: ON" or "SFX: OFF",
        align = lvgl.ALIGN.CENTER,
    }
    sfxBtn:onClicked(function()
        sfx_enabled = not sfx_enabled
        sfxLbl:set{ text = sfx_enabled and "SFX: ON" or "SFX: OFF" }
        save_config()
    end)

    local musBtn = optBox:Button{ w = 100, h = 28 }
    local musLbl = musBtn:Label{
        text = music_enabled and "Music: ON" or "Music: OFF",
        align = lvgl.ALIGN.CENTER,
    }
    musBtn:onClicked(function()
        music_enabled = not music_enabled
        musLbl:set{ text = music_enabled and "Music: ON" or "Music: OFF" }
        save_config()
    end)

    -- Action buttons
    local btnBox = scr:Object{
        w = 260, h = lvgl.SIZE_CONTENT,
        align = { type = lvgl.ALIGN.TOP_MID, y_ofs = 175 },
        bg_opa = 0, border_width = 0, pad_all = 4,
        flex = {
            flex_direction = "row",
            flex_wrap = "wrap",
            justify_content = "center",
            column_gap = 8,
        },
    }
    btnBox:clear_flag(lvgl.FLAG.SCROLLABLE)

    local launchBtn = btnBox:Button{ w = 75, h = 34 }
    launchBtn:Label{ text = "Play", align = lvgl.ALIGN.CENTER }
    launchBtn:onClicked(function()
        if #found_wads == 0 then
            status:set{ text = "No WAD file found!" }
            return
        end
        status:set{ text = "Loading Doom..." }
        local wad = found_wads[selected_wad].path
        local km = build_keymap_string()
        lvgl.Timer{
            period = 50,
            cb = function(t)
                t:delete()
                local w = found_wads[selected_wad]
                local vfs_wad = to_vfs_path(w.path)
                local wad_dir = vfs_wad:match("^(.*)/") or "."
                local args = {}

                if w.wtype == "pwad" then
                    -- PWADs need a base IWAD
                    if #iwad_list == 0 then
                        status:set{ text = "No base IWAD found!" }
                        return
                    end
                    local base = iwad_list[selected_base]
                    local vfs_base = to_vfs_path(base.path)
                    args = {ELF_PATH, "-iwad", vfs_base,
                            "-file", vfs_wad,
                            "-configdir", wad_dir}
                else
                    args = {ELF_PATH, "-iwad", vfs_wad,
                            "-configdir", wad_dir}
                end
                -- Independent audio flags: -nosfx keeps music alive (the
                -- module pumps it via the music Poll), unlike -nosound.
                if not sfx_enabled then
                    args[#args + 1] = "-nosfx"
                end
                if not music_enabled then
                    args[#args + 1] = "-nomusic"
                end
                args[#args + 1] = "-keymap"
                args[#args + 1] = km
                args[#args + 1] = "-trkball"
                args[#args + 1] = build_trkball_string()
                -- Deferred launch: the firmware tears Lua down, runs Doom, then
                -- recreates Lua and returns to the launcher home. _launch_elf only
                -- queues the request, so there's no result to handle here.
                _launch_elf(table.unpack(args))
            end
        }
    end)

    local ctrlBtn = btnBox:Button{ w = 95, h = 34 }
    ctrlBtn:Label{ text = "Controls", align = lvgl.ALIGN.CENTER }
    ctrlBtn:onClicked(function() create_controls_screen() end)

    local quitBtn = btnBox:Button{ w = 75, h = 34 }
    quitBtn:Label{ text = "Quit", align = lvgl.ALIGN.CENTER }
    quitBtn:onClicked(function()
        apps.go_home()   -- manager tears down the stable root (and its current view)
    end)

    lvgl.group.get_default():add_obj(wadDd)
    lvgl.group.get_default():add_obj(baseDd)
    _gridnav_add(optBox, GRIDNAV_ROLLOVER)
    lvgl.group.get_default():add_obj(optBox)
    _gridnav_add(btnBox, GRIDNAV_ROLLOVER)
    lvgl.group.get_default():add_obj(btnBox)
end

-- ============================================================
-- Controls overview screen
-- ============================================================
create_controls_screen = function()
    if scr then scr:delete() end

    scr = root:Object({
        w = W, h = H,
        bg_color = "#000000", bg_opa = lvgl.OPA(255),
        border_width = 0, pad_all = 0,
    })
    scr:clear_flag(lvgl.FLAG.SCROLLABLE)

    scr:Label{
        text = "CONTROLS",
        text_font = lvgl.BUILTIN_FONT.MONTSERRAT_14,
        text_color = "#FF4444",
        align = { type = lvgl.ALIGN.TOP_MID, y_ofs = 4 },
    }

    -- Scrollable list of actions
    local list = scr:Object{
        w = W - 4, h = H - 54,
        align = { type = lvgl.ALIGN.TOP_MID, y_ofs = 22 },
        bg_opa = 0, border_width = 0,
        pad_left = 4, pad_right = 4, pad_top = 2, pad_bottom = 2,
        flex = { flex_direction = "column" },
    }

    local font = lvgl.BUILTIN_FONT.MONTSERRAT_12

    for idx, a in ipairs(ACTIONS) do
        local b = bindings[a.id]
        local row = list:Object{
            w = lvgl.PCT(100), h = 22,
            bg_opa = 0, border_width = 0, pad_all = 0,
            flex = { flex_direction = "row", cross_place = "center" },
        }
        row:clear_flag(lvgl.FLAG.SCROLLABLE)

        row:Label{
            text = a.label,
            text_font = font,
            text_color = "#CCCCCC",
            w = 95,
        }

        -- Primary key button
        local k1btn = row:Button{ w = 65, h = 20 }
        k1btn:Label{
            text = key_display(b.key1),
            text_font = font,
            align = lvgl.ALIGN.CENTER,
        }
        k1btn:onClicked(function() create_bind_screen(idx, 1) end)

        -- Alt key button
        local k2btn = row:Button{ w = 65, h = 20 }
        k2btn:Label{
            text = key_display(b.key2),
            text_font = font,
            align = lvgl.ALIGN.CENTER,
        }
        k2btn:onClicked(function() create_bind_screen(idx, 2) end)
    end

    -- Bottom buttons
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

    local defBtn = btnBar:Button{ w = 70, h = 26 }
    defBtn:Label{ text = "Defaults", text_font = font, align = lvgl.ALIGN.CENTER }
    defBtn:onClicked(function()
        load_defaults()
        save_config()
        create_controls_screen()
    end)

    local inputBtn = btnBar:Button{ w = 60, h = 26 }
    inputBtn:Label{ text = "Input", text_font = font, align = lvgl.ALIGN.CENTER }
    inputBtn:onClicked(function() create_input_screen() end)

    local backBtn = btnBar:Button{ w = 50, h = 26 }
    backBtn:Label{ text = "Back", text_font = font, align = lvgl.ALIGN.CENTER }
    backBtn:onClicked(function() create_main_screen() end)

    _gridnav_add(list, GRIDNAV_ROLLOVER)
    _gridnav_add(btnBar, GRIDNAV_ROLLOVER)
    local grp = lvgl.group.get_default()
    grp:add_obj(list)
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
        text_color = "#FF4444",
        align = { type = lvgl.ALIGN.TOP_MID, y_ofs = 4 },
    }

    -- Scrollable key list
    local list = scr:Object{
        w = W - 4, h = H - 54,
        align = { type = lvgl.ALIGN.TOP_MID, y_ofs = 22 },
        bg_opa = 0, border_width = 0,
        pad_left = 4, pad_right = 4, pad_top = 2, pad_bottom = 2,
        flex = { flex_direction = "column" },
    }

    local font = lvgl.BUILTIN_FONT.MONTSERRAT_12
    local current = (slot == 1) and b.key1 or b.key2

    -- "Clear" option
    local clrBtn = list:Button{ w = lvgl.PCT(100), h = 22 }
    clrBtn:Label{ text = "--- (clear)", text_font = font, align = lvgl.ALIGN.CENTER }
    clrBtn:onClicked(function()
        if slot == 1 then b.key1 = nil else b.key2 = nil end
        save_config()
        create_controls_screen()
    end)

    -- Key options
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

    -- Cancel button
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
-- Input settings screen (trackball momentum tuning)
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
        text_color = "#FF4444",
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

    -- Helper: create a row with label + value button (< value >)
    local function setting_row(parent, label, get_text, on_left, on_right)
        local row = parent:Object{
            w = lvgl.PCT(100), h = 26,
            bg_opa = 0, border_width = 0, pad_all = 0,
            flex = { flex_direction = "row", cross_place = "center" },
        }
        row:clear_flag(lvgl.FLAG.SCROLLABLE)

        row:Label{
            text = label,
            text_font = font,
            text_color = "#CCCCCC",
            w = 120,
        }

        local valBtn = row:Button{ w = 130, h = 24 }
        local valLbl = valBtn:Label{
            text = get_text(),
            text_font = font,
            align = lvgl.ALIGN.CENTER,
        }
        valBtn:onClicked(function()
            on_right()
            valLbl:set{ text = get_text() }
            save_config()
        end)
        -- Long press cycles backward (not all LVGL builds support this, but it's there)
        return valBtn
    end

    -- Momentum toggle
    setting_row(list, "Momentum",
        function() return trk_momentum and "< ON >" or "< OFF >" end,
        function() trk_momentum = not trk_momentum end,
        function() trk_momentum = not trk_momentum end
    )

    -- Impulse (sensitivity): 5..30, step 1 → displayed as x/10
    setting_row(list, "Sensitivity",
        function() return string.format("< %.1f >", trk_impulse / 10) end,
        function() trk_impulse = math.max(5, trk_impulse - 1) end,
        function()
            trk_impulse = trk_impulse + 1
            if trk_impulse > 30 then trk_impulse = 5 end
        end
    )

    -- Friction: 50..95, step 2 → displayed as x/100
    setting_row(list, "Friction",
        function() return string.format("< %.2f >", trk_friction / 100) end,
        function() trk_friction = math.max(50, trk_friction - 2) end,
        function()
            trk_friction = trk_friction + 2
            if trk_friction > 95 then trk_friction = 50 end
        end
    )

    -- Threshold: 2..10, step 1 → displayed as x/10
    setting_row(list, "Dead Zone",
        function() return string.format("< %.1f >", trk_thresh / 10) end,
        function() trk_thresh = math.max(2, trk_thresh - 1) end,
        function()
            trk_thresh = trk_thresh + 1
            if trk_thresh > 10 then trk_thresh = 2 end
        end
    )

    -- Info label
    list:Label{
        text = "Sensitivity: impulse per tick\n"
             .. "Friction: decay rate (lower=faster stop)\n"
             .. "Dead Zone: min velocity to register",
        text_font = font,
        text_color = "#666666",
        w = lvgl.PCT(100),
    }

    -- Bottom buttons
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
-- Startup: load config (or defaults) and show main screen
-- ============================================================
load_defaults()
-- Return a deferred init function. The sublauncher calls this once per event
-- loop tick (inside its loadingPopUpAdd), keeping the watchdog fed between steps.
local init_phase = 0
local classify_idx = 0
return function()
    init_phase = init_phase + 1

    -- Phases 1-3: directory scanning (no file I/O per WAD)
    if init_phase == 1 then
        scan_dir_for_wads(app_dir)
        return false
    elseif init_phase == 2 then
        scan_dir_for_wads(sd_app_dir)
        return false
    elseif init_phase == 3 then
        if sd_app_dir ~= "S:/doom" then
            scan_dir_for_wads("S:/doom")
        end
        classify_idx = 0
        return false
    end

    -- Phases 4..4+N: classify one WAD header per tick (one SD open each)
    classify_idx = classify_idx + 1
    if classify_idx <= #found_wads then
        local w = found_wads[classify_idx]
        w.wtype = wad_type(w.path)
        return false
    end

    -- Final phase: sort, build IWAD list, config, screen
    table.sort(found_wads, function(a, b)
        if a.wtype ~= b.wtype then return a.wtype == "iwad" end
        return a.name:lower() < b.name:lower()
    end)
    for i, w in ipairs(found_wads) do
        if w.wtype == "iwad" then
            iwad_list[#iwad_list + 1] = { idx = i, name = w.name, path = w.path }
        end
    end
    load_config()
    create_main_screen()
    return true
end
