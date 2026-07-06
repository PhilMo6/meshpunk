local lvgl = require("lvgl")
local apps = require("lib/apps")
local fileman = require("lib/fileman")

local app_dir = ...

local W = lvgl.HOR_RES()
local H = lvgl.VER_RES()

-- ============================================================
-- Game Boy / Game Boy Color (gnuboy) Launcher
-- ============================================================

-- Derive SD-side mirror of app directory:
-- "L:/lua/apps/Games/GameBoy" -> "S:/lua/apps/Games/GameBoy"
local sd_app_dir = app_dir:gsub("^L:", "S:")

-- Convert firmware path prefixes to VFS mount points for the ELF module's fopen
local function to_vfs_path(path)
    if path:sub(1, 2) == "S:" then return "/sd" .. path:sub(3) end
    if path:sub(1, 2) == "L:" then return "/littlefs" .. path:sub(3) end
    return path
end

-- Search for a file: check app dir, then SD mirror, then legacy S:/gb/
local function find_file(name)
    local search = { app_dir, sd_app_dir, "S:/gb" }
    for _, dir in ipairs(search) do
        local path = dir .. "/" .. name
        local f = io.open(path, "r")
        if f then f:close(); return path end
    end
    return nil
end

local CFG_PATH = app_dir .. "/controls.cfg"
local ELF_PATH = find_file("gameboy.app.elf") or sd_app_dir .. "/gameboy.app.elf"

local found_roms = {}   -- { {name, path}, ... }
local seen_lower = {}
local selected_rom = 1
local selected_rom_name = nil
local scr = nil

-- Stable, manager-registered root (same pattern as the PICO-8/Doom launchers).
local root = apps.new_root({
    w = W, h = H,
    bg_color = "#000000", bg_opa = lvgl.OPA(255),
    border_width = 0, pad_all = 0,
})
root:clear_flag(lvgl.FLAG.SCROLLABLE)

-- fileman routes the drive from the L:/S: prefix itself; sizes=false skips
-- the per-entry size lookup, so huge ROM folders list fast (watchdog-safe).
local function scan_dir_for_roms(dir_path)
    local entries = fileman.list(dir_path, {
        sizes = false,
        filter = function(e)
            return e.type == "file"
                and (e.name:lower():match("%.gb$") or e.name:lower():match("%.gbc$"))
        end,
    }) or {}
    for _, e in ipairs(entries) do
        local low = e.name:lower()
        if not seen_lower[low] then
            seen_lower[low] = true
            found_roms[#found_roms + 1] = {
                name = e.name,
                path = dir_path .. "/" .. e.name,
            }
        end
    end
end

-- ============================================================
-- Keymap / controls system
-- ============================================================

-- Canonical Game Boy button codes: these are the key codes map_key() inside
-- the ELF understands. The launcher maps physical keys to these via the
-- host's keymap system.
local GB = {
    UP     = 0x77,  -- 'w'
    DOWN   = 0x73,  -- 's'
    LEFT   = 0x61,  -- 'a'
    RIGHT  = 0x64,  -- 'd'
    A      = 0x6D,  -- 'm'
    B      = 0x6E,  -- 'n'
    START  = 0x0D,  -- Enter
    SELECT = 0x20,  -- Space
}

-- Physical key names -> hex codes for the keymap string.
local KEYS = {
    -- Letters
    a=0x61, b=0x62, c=0x63, d=0x64, e=0x65, f=0x66, g=0x67, h=0x68,
    i=0x69, j=0x6A, k=0x6B, l=0x6C, m=0x6D, n=0x6E, o=0x6F, p=0x70,
    q=0x71, r=0x72, s=0x73, t=0x74, u=0x75, v=0x76, w=0x77, x=0x78,
    z=0x7A,
    -- Special keys
    Space  = 0x20,
    Enter  = 0x0D,
    BkSpc  = 0x08,
    Shift  = 0x80,
    -- Trackball
    TrkUp  = 0x81,
    TrkDn  = 0x82,
    TrkLt  = 0x83,
    TrkRt  = 0x84,
    TrkClk = 0x85,
}

-- Reverse lookup: hex code -> display name
local KEY_NAMES = {}
for name, code in pairs(KEYS) do KEY_NAMES[code] = name end

-- Action definitions: {id, label, gb_code, default_key1, default_key2}
-- NOTE: ids must stay underscore-free OR the config parser must accept "_"
-- (we use [%w_] below, so both are fine).
local ACTIONS = {
    { id="up",     label="Up",     gb=GB.UP,     key1=KEYS.w,     key2=KEYS.TrkUp  },
    { id="down",   label="Down",   gb=GB.DOWN,   key1=KEYS.s,     key2=KEYS.TrkDn  },
    { id="left",   label="Left",   gb=GB.LEFT,   key1=KEYS.a,     key2=KEYS.TrkLt  },
    { id="right",  label="Right",  gb=GB.RIGHT,  key1=KEYS.d,     key2=KEYS.TrkRt  },
    { id="btn_a",  label="A btn",  gb=GB.A,      key1=KEYS.m,     key2=KEYS.TrkClk },
    { id="btn_b",  label="B btn",  gb=GB.B,      key1=KEYS.n,     key2=nil         },
    { id="start",  label="Start",  gb=GB.START,  key1=KEYS.Enter, key2=nil         },
    { id="select", label="Select", gb=GB.SELECT, key1=KEYS.Space, key2=nil         },
}

-- Working copy of bindings
local bindings = {}  -- bindings[action_id] = {gb=N, key1=N|nil, key2=N|nil}

local function load_defaults()
    bindings = {}
    for _, a in ipairs(ACTIONS) do
        bindings[a.id] = { gb = a.gb, key1 = a.key1, key2 = a.key2 }
    end
end

-- Build the -keymap hex string from current bindings
local function build_keymap_string()
    local parts = {}
    for _, a in ipairs(ACTIONS) do
        local b = bindings[a.id]
        if b and (b.key1 or b.key2) then
            local s = string.format("%02X=", b.gb)
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

-- Trackball momentum settings
local trk_momentum = true
local trk_impulse  = 15       -- impulse * 10 (1.5 -> 15)
local trk_friction = 82       -- friction * 100 (0.82 -> 82)
local trk_thresh   = 4        -- threshold * 10 (0.4 -> 4)

local function build_trkball_string()
    return string.format("%d,%d,%d,%d",
        trk_momentum and 1 or 0, trk_impulse, trk_friction, trk_thresh)
end

-- ============================================================
-- Emulator settings
-- ============================================================

-- DMG colorization palettes (gb_palette_t indices in the ELF).
-- Only affects original Game Boy games; GBC games use their own colors.
local PALETTES = {
    { label = "GBC auto",   value = 35 }, -- per-game colorization, like a real GBC
    { label = "DMG green",  value = 32 },
    { label = "Pocket",     value = 33 },
    { label = "Light",      value = 34 },
    { label = "SGB",        value = 36 },
}
local SCALES = {
    { label = "Fit (240x216)",    value = "fit"  },
    { label = "Native (160x144)", value = "1x"   },
    { label = "Fullscreen",       value = "full" },
}
local sel_palette = 1
local sel_scale   = 1
local resume_on   = false

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
    f:write(string.format("trk_momentum=%d\n", trk_momentum and 1 or 0))
    f:write(string.format("trk_impulse=%d\n", trk_impulse))
    f:write(string.format("trk_friction=%d\n", trk_friction))
    f:write(string.format("trk_thresh=%d\n", trk_thresh))
    f:write(string.format("pal=%d\n", sel_palette))
    f:write(string.format("scale=%d\n", sel_scale))
    f:write(string.format("resume=%d\n", resume_on and 1 or 0))
    if #found_roms > 0 then
        f:write("rom=" .. found_roms[selected_rom].name .. "\n")
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
        local pal = line:match("^pal=(%d+)$")
        if pal then
            pal = tonumber(pal)
            if pal >= 1 and pal <= #PALETTES then sel_palette = pal end
        end
        local sc = line:match("^scale=(%d+)$")
        if sc then
            sc = tonumber(sc)
            if sc >= 1 and sc <= #SCALES then sel_scale = sc end
        end
        local res = line:match("^resume=([01])$")
        if res then resume_on = (res == "1") end
        local rname = line:match("^rom=(.+)$")
        if rname then selected_rom_name = rname end
    end
    return true
end

local function key_display(code)
    if not code then return "---" end
    return KEY_NAMES[code] or string.format("0x%02X", code)
end

-- All available keys for binding (sorted for display)
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
    -- Title
    scr:Label{
        text = "GAME BOY",
        text_font = lvgl.BUILTIN_FONT.MONTSERRAT_22,
        text_color = "#9BBC0F",
        align = { type = lvgl.ALIGN.TOP_MID, y_ofs = 6 },
    }

    -- ROM selector — dropdown over all found ROMs
    local rom_opts = "No ROMs found"
    if #found_roms > 0 then
        local names = {}
        for i, r in ipairs(found_roms) do names[i] = r.name end
        rom_opts = table.concat(names, "\n")
    end

    local romDd = scr:Dropdown{
        options = rom_opts,
        w = 260, h = 28,
        align = { type = lvgl.ALIGN.TOP_MID, y_ofs = 32 },
    }
    if #found_roms > 0 then
        romDd:set{ selected = selected_rom - 1 }
    end
    romDd:onevent(lvgl.EVENT.VALUE_CHANGED, function()
        if #found_roms == 0 then return end
        selected_rom = romDd:get("selected") + 1
        save_config()
    end)

    -- Status
    local has_roms = #found_roms > 0
    local status = scr:Label{
        text = has_roms and "Ready to play"
               or "Place .gb/.gbc ROMs in S:/gb/",
        text_font = lvgl.BUILTIN_FONT.MONTSERRAT_14,
        text_color = has_roms and "#888888" or "#FF6666",
        align = { type = lvgl.ALIGN.TOP_MID, y_ofs = 62 },
    }

    -- Action buttons
    local btnBox = scr:Object{
        w = 140, h = lvgl.SIZE_CONTENT,
        align = { type = lvgl.ALIGN.TOP_MID, y_ofs = 80 },
        bg_opa = 0, border_width = 0, pad_all = 4,
        flex = {
            flex_direction = "column",
            row_gap = 6,
        },
    }
    btnBox:clear_flag(lvgl.FLAG.SCROLLABLE)

    local launchBtn = btnBox:Button{ w = lvgl.PCT(100), h = 34 }
    launchBtn:Label{ text = "Play", align = lvgl.ALIGN.CENTER }
    launchBtn:onClicked(function()
        if #found_roms == 0 then
            status:set{ text = "No .gb/.gbc ROM found!" }
            return
        end
        status:set{ text = "Loading..." }
        local km = build_keymap_string()
        lvgl.Timer{
            period = 50,
            cb = function(t)
                t:delete()
                local r = found_roms[selected_rom]
                -- Deferred launch: the firmware tears Lua down, runs the
                -- module, then recreates Lua and returns to the launcher.
                _launch_elf(ELF_PATH, to_vfs_path(r.path),
                    "-pal", tostring(PALETTES[sel_palette].value),
                    "-scale", SCALES[sel_scale].value,
                    "-resume", resume_on and "1" or "0",
                    "-keymap", km,
                    "-trkball", build_trkball_string())
            end
        }
    end)

    local ctrlBtn = btnBox:Button{ w = lvgl.PCT(100), h = 34 }
    ctrlBtn:Label{ text = "Controls", align = lvgl.ALIGN.CENTER }
    ctrlBtn:onClicked(function() create_controls_screen() end)

    local setBtn = btnBox:Button{ w = lvgl.PCT(100), h = 34 }
    setBtn:Label{ text = "Settings", align = lvgl.ALIGN.CENTER }
    setBtn:onClicked(function() create_settings_screen() end)

    local quitBtn = btnBox:Button{ w = lvgl.PCT(100), h = 34 }
    quitBtn:Label{ text = "Quit", align = lvgl.ALIGN.CENTER }
    quitBtn:onClicked(function()
        apps.go_home()
    end)

    lvgl.group.get_default():add_obj(romDd)
    _gridnav_add(btnBox, GRIDNAV_ROLLOVER)
    lvgl.group.get_default():add_obj(btnBox)
end

-- ============================================================
-- Settings screen (palette / scale / resume)
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
        text_color = "#9BBC0F",
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
            on_click()
            valLbl:set{ text = get_text() }
            save_config()
        end)
        return valBtn
    end

    -- DMG palette (GBC games ignore this)
    setting_row(list, "GB palette",
        function() return "< " .. PALETTES[sel_palette].label .. " >" end,
        function()
            sel_palette = sel_palette + 1
            if sel_palette > #PALETTES then sel_palette = 1 end
        end
    )

    -- Screen scale
    setting_row(list, "Screen",
        function() return "< " .. SCALES[sel_scale].label .. " >" end,
        function()
            sel_scale = sel_scale + 1
            if sel_scale > #SCALES then sel_scale = 1 end
        end
    )

    -- Resume toggle
    setting_row(list, "Resume session",
        function() return resume_on and "< ON >" or "< OFF >" end,
        function() resume_on = not resume_on end
    )

    -- Info label
    list:Label{
        text = "GB palette: colors for original GB games\n"
             .. "Resume: save/restore full state on exit\n"
             .. "Battery saves (.sav) always work",
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
        text_color = "#9BBC0F",
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
            w = 65,
        }

        -- Primary key button
        local k1btn = row:Button{ w = 75, h = 20 }
        k1btn:Label{
            text = key_display(b.key1),
            text_font = font,
            align = lvgl.ALIGN.CENTER,
        }
        k1btn:onClicked(function() create_bind_screen(idx, 1) end)

        -- Alt key button
        local k2btn = row:Button{ w = 75, h = 20 }
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
        text_color = "#9BBC0F",
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
        text_color = "#9BBC0F",
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
            on_click()
            valLbl:set{ text = get_text() }
            save_config()
        end)
        return valBtn
    end

    -- Momentum toggle
    setting_row(list, "Momentum",
        function() return trk_momentum and "< ON >" or "< OFF >" end,
        function() trk_momentum = not trk_momentum end
    )

    -- Impulse (sensitivity): 5..30, step 1
    setting_row(list, "Sensitivity",
        function() return string.format("< %.1f >", trk_impulse / 10) end,
        function()
            trk_impulse = trk_impulse + 1
            if trk_impulse > 30 then trk_impulse = 5 end
        end
    )

    -- Friction: 50..95, step 2
    setting_row(list, "Friction",
        function() return string.format("< %.2f >", trk_friction / 100) end,
        function()
            trk_friction = trk_friction + 2
            if trk_friction > 95 then trk_friction = 50 end
        end
    )

    -- Threshold: 2..10, step 1
    setting_row(list, "Dead Zone",
        function() return string.format("< %.1f >", trk_thresh / 10) end,
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
local init_phase = 0
return function()
    init_phase = init_phase + 1

    -- Phase 1-3: directory scanning
    if init_phase == 1 then
        scan_dir_for_roms(app_dir)
        return false
    elseif init_phase == 2 then
        scan_dir_for_roms(sd_app_dir)
        return false
    elseif init_phase == 3 then
        if sd_app_dir ~= "S:/gb" then
            scan_dir_for_roms("S:/gb")
        end
        return false
    end

    -- Final phase: sort ROMs, load config, show UI
    table.sort(found_roms, function(a, b)
        return a.name:lower() < b.name:lower()
    end)
    load_config()
    -- Restore ROM selection by name
    if selected_rom_name then
        for i, r in ipairs(found_roms) do
            if r.name == selected_rom_name then selected_rom = i; break end
        end
    end
    create_main_screen()
    return true
end
