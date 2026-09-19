-- Map tile-source module: switch between the downloaded .bin cache and a
-- user folder of 256x256 z/x/y PNG tiles on SD, plus the picker/source
-- screens. Lives in its own chunk because main.lua's main chunk is near
-- Lua's 200-local limit.
--
-- Loaded by main.lua as: loadfile(app_dir .. "/tilesource.lua")(ctx) with
-- ctx = { W, H, map_prefs, save_map_prefs, tile_bin_path }. UI dependencies
-- arrive later through M.bind_ui (main builds them after this module loads).
local lvgl = require("lvgl")
local fileman = require("lib/fileman")

local ctx = ...
local W, H = ctx.W, ctx.H
local map_prefs = ctx.map_prefs
local save_map_prefs = ctx.save_map_prefs
local tile_bin_path = ctx.tile_bin_path

-- Set by M.bind_ui: { root, dl_label, refresh_tiles, update_wifi_status,
-- show_precache_screen, reset_tile_caches }
local ui = nil

local M = {}

-- User tile folder as a bare SD path — "S:" stripped, trailing slash
-- stripped — or nil when tiles come from the download cache.
local folder_bare = nil
local function set_folder(dir)
    if dir and dir ~= "" then
        folder_bare = dir:gsub("^[Ss]:", ""):gsub("/+$", "")
    else
        folder_bare = nil
    end
end

-- nil = download cache; else the bare SD folder path.
function M.folder()
    return folder_bare
end

-- Bare SD path of the tile file for (z,tx,ty): the user folder's z/x/y.png
-- when one is selected, else the download cache's .bin.
function M.tile_path(z, tx, ty)
    if folder_bare then
        return folder_bare .. "/" .. z .. "/" .. tx .. "/" .. ty .. ".png"
    end
    return tile_bin_path(z, tx, ty)
end

-- Short source name for the settings label.
function M.source_label()
    return map_prefs.tile_dir and fileman.basename(map_prefs.tile_dir)
                               or "Downloaded (OSM)"
end

local dir_overlay = nil     -- folder picker
local source_overlay = nil  -- tile source home

local function close_dir_screen()
    if dir_overlay then
        _nav_clear()  -- remove gridnav before delete (avoids use-after-free)
        dir_overlay:delete()
        dir_overlay = nil
        lvgl.group.focus_obj(ui.root)
    end
end

local function close_source_screen()
    if source_overlay then
        _nav_clear()  -- remove gridnav before delete (avoids use-after-free)
        source_overlay:delete()
        source_overlay = nil
        lvgl.group.focus_obj(ui.root)
    end
end

-- True while either screen is up (main's ESC chain asks before closing).
function M.screen_open()
    return (source_overlay or dir_overlay) ~= nil
end

function M.close_screens()
    close_source_screen()
    close_dir_screen()
end

-- First all-digit subfolder of dir, or nil.
local function first_digit_dir(dir)
    local entries = fileman.list(dir, { sizes = false })
    for _, e in ipairs(entries or {}) do
        if e.type == "dir" and e.name:match("^%d+$") then
            return fileman.join(dir, e.name)
        end
    end
    return nil
end

-- Validate dir as a z/x/y PNG tileset by reading one sample tile's header.
-- Returns true, or nil and a user-facing message. PNG width/height are
-- big-endian u32s at bytes 17..24 (1-indexed).
local function probe_tile_folder(dir)
    local zdir = first_digit_dir(dir)
    local xdir = zdir and first_digit_dir(zdir)
    local sample = nil
    if xdir then
        local files = fileman.list(xdir, { sizes = false })
        for _, e in ipairs(files or {}) do
            if e.type == "file" and e.name:match("%.png$") then
                sample = fileman.join(xdir, e.name)
                break
            end
        end
    end
    if not sample then return nil, "No z/x/y .png tiles found here" end
    local f = io.open(sample, "r")
    if not f then return nil, "Cannot read " .. fileman.basename(sample) end
    local data = f:read("*a") or ""
    f:close()
    if #data < 24 then return nil, "Bad tile: " .. fileman.basename(sample) end
    local b = { string.byte(data, 1, 24) }
    if b[1] ~= 0x89 or b[2] ~= 0x50 or b[3] ~= 0x4E or b[4] ~= 0x47 then
        return nil, "Not PNG tiles: " .. fileman.basename(sample)
    end
    local w = b[17] * 16777216 + b[18] * 65536 + b[19] * 256 + b[20]
    local h = b[21] * 16777216 + b[22] * 65536 + b[23] * 256 + b[24]
    if w ~= 256 or h ~= 256 then
        return nil, "Tiles are " .. w .. "x" .. h .. " - need 256x256"
    end
    return true
end

-- Switch the tile source and reset per-source state. dir = "S:/..." folder
-- of z/x/y PNG tiles, or nil for the download cache.
local function apply_tile_source(dir)
    map_prefs.tile_dir = dir
    save_map_prefs()
    set_folder(dir)
    ui.reset_tile_caches()
    close_dir_screen()
    close_source_screen()
    if folder_bare then
        ui.dl_label:add_flag(lvgl.FLAG.HIDDEN)
    else
        -- init_view skips the WiFi kick in folder mode and the firmware
        -- never reconnects on its own — kick one round now.
        pcall(_wifi_auto_connect)
        ui.update_wifi_status()
    end
    ui.refresh_tiles()
end

local function show_tile_dir_screen(path)
    if dir_overlay then close_dir_screen() end
    path = fileman.normalize(path)

    dir_overlay = ui.root:Object({
        w = W, h = H, x = 0, y = 0,
        bg_color = "#1a1a2e", bg_opa = 255,
        pad_all = 8, border_width = 0,
        flex = { flex_direction = "column", flex_wrap = "nowrap" },
    })

    dir_overlay:Label({
        text = "Tile folder", text_color = "#FFFFFF", w = lvgl.PCT(100), h = 22,
    })
    local disp = path
    if #disp > 36 then disp = "..." .. disp:sub(-33) end
    dir_overlay:Label({
        text = disp, text_color = "#88AAFF", w = lvgl.PCT(100), h = 18,
    })

    local msg_lbl = dir_overlay:Label({
        text = "", text_color = "#FF8888", w = lvgl.PCT(100), h = 18,
    })
    msg_lbl:add_flag(lvgl.FLAG.HIDDEN)

    local use_btn = dir_overlay:Button({ w = W - 16, h = 32 })
    use_btn:Label({ text = "Use this folder", align = lvgl.ALIGN.LEFT_MID })
    use_btn:onClicked(function()
        local ok, msg = probe_tile_folder(path)
        if ok then
            apply_tile_source(path)
        else
            msg_lbl:set({ text = msg })
            msg_lbl:clear_flag(lvgl.FLAG.HIDDEN)
        end
    end)

    local up_btn = dir_overlay:Button({ w = W - 16, h = 32 })
    up_btn:Label({ text = "Up", align = lvgl.ALIGN.LEFT_MID })
    up_btn:onClicked(function()
        local p = fileman.parent(path)
        if p then show_tile_dir_screen(p) end
    end)

    local cancel_btn = dir_overlay:Button({ w = W - 16, h = 32 })
    cancel_btn:Label({ text = "Cancel", align = lvgl.ALIGN.LEFT_MID })
    cancel_btn:onClicked(function() close_dir_screen() end)

    local entries, list_err = fileman.list(path, {
        sizes = false,
        filter = function(e) return e.type == "dir" end,
    })
    if entries then
        local MAX_DIRS = 150
        for i, e in ipairs(entries) do
            if i > MAX_DIRS then
                dir_overlay:Label({
                    text = "(+" .. (#entries - MAX_DIRS) .. " more)",
                    w = lvgl.PCT(100), h = 20,
                })
                break
            end
            local row = dir_overlay:Button({ w = W - 16, h = 28 })
            local name = e.name
            if #name > 30 then name = name:sub(1, 29) .. "~" end
            row:Label({ text = name .. "/", align = lvgl.ALIGN.LEFT_MID })
            local sub = fileman.join(path, e.name)
            row:onClicked(function() show_tile_dir_screen(sub) end)
        end
    else
        dir_overlay:Label({
            text = "Cannot list: " .. tostring(list_err),
            w = lvgl.PCT(100), h = 40,
        })
    end

    -- 'q' / ESC closes the picker (the gridnav scope owns the keys while
    -- it's open, so root's key handler can't see them).
    dir_overlay:onevent(lvgl.EVENT.KEY, function()
        local k = lvgl.indev.get_act():get_key()
        if k == 113 or k == 27 then close_dir_screen() end
    end)
    _nav_setup(dir_overlay, GRIDNAV_ROLLOVER)
end

-- Tile source home: current source + the download / folder / switch actions.
function M.show_source_screen()
    if source_overlay then return end

    source_overlay = ui.root:Object({
        w = W, h = H, x = 0, y = 0,
        bg_color = "#1a1a2e", bg_opa = 255,
        pad_all = 8, border_width = 0,
        flex = { flex_direction = "column", flex_wrap = "nowrap" },
    })

    source_overlay:Label({
        text = "Map Tiles", text_color = "#FFFFFF", w = lvgl.PCT(100), h = 24,
    })
    local src = map_prefs.tile_dir or "Downloaded (OSM)"
    if #src > 36 then src = "..." .. src:sub(-33) end
    source_overlay:Label({
        text = "Source: " .. src,
        text_color = "#AAAAAA", w = lvgl.PCT(100), h = 18,
    })

    -- Pre-cache downloads write the .bin cache — offered only while that
    -- cache is the active source.
    if not folder_bare then
        local dl_btn = source_overlay:Button({ w = W - 16, h = 32 })
        dl_btn:Label({ text = "Download tiles for offline use...",
                       align = lvgl.ALIGN.LEFT_MID })
        dl_btn:onClicked(function()
            close_source_screen()
            ui.show_precache_screen()
        end)
    end

    local folder_btn = source_overlay:Button({ w = W - 16, h = 32 })
    folder_btn:Label({
        text = folder_bare and "Use a different tile folder..."
                            or "Use a tile folder from SD...",
        align = lvgl.ALIGN.LEFT_MID,
    })
    folder_btn:onClicked(function()
        close_source_screen()
        show_tile_dir_screen(map_prefs.tile_dir or "S:/")
    end)

    if folder_bare then
        local dlmode_btn = source_overlay:Button({ w = W - 16, h = 32 })
        dlmode_btn:Label({ text = "Use downloaded tiles", align = lvgl.ALIGN.LEFT_MID })
        dlmode_btn:onClicked(function() apply_tile_source(nil) end)
    end

    local back_btn = source_overlay:Button({ w = W - 16, h = 32 })
    back_btn:Label({ text = "Close", align = lvgl.ALIGN.CENTER })
    back_btn:onClicked(function() close_source_screen() end)

    -- 'q' / ESC closes this screen (the gridnav scope owns the keys while
    -- it's open, so root's key handler can't see them).
    source_overlay:onevent(lvgl.EVENT.KEY, function()
        local k = lvgl.indev.get_act():get_key()
        if k == 113 or k == 27 then close_source_screen() end
    end)
    _nav_setup(source_overlay, GRIDNAV_ROLLOVER)
end

-- Late UI wiring — called by main.lua once root, the status label and the
-- shared view functions exist.
function M.bind_ui(t)
    ui = t
end

set_folder(map_prefs.tile_dir)
return M
