local lvgl = require("lvgl")

print("[Map] starting")

local W = lvgl.HOR_RES()
local H = lvgl.VER_RES()
local TILE_SIZE = 256
local CACHE_ROOT = "/meshpunk/map_cache"
local TILE_URL = "https://tile.openstreetmap.org"
local MIN_ZOOM = 5
local MAX_ZOOM = 16
local GRID = 4          -- 16 tiles × 128KB = 2MB (fits 3MB cache)
local FETCH_GRID = 6    -- pre-cache buffer ring around visible

local AREA_PRESETS = {
    { name = "Small",  radius = 3  },
    { name = "Medium", radius = 6  },
    { name = "Large",  radius = 12 },
    { name = "Huge",   radius = 20 },
}

local app_dir = ...

-- ---------------------------------------------------------------------------
-- State
-- ---------------------------------------------------------------------------
local map = {
    running = true,
    zoom = 14,
    cx = 0, cy = 0,
    timers = {},
    download_queue = {},
    downloading = false,
    tooltip = nil,
    drag = nil,
    vx = 0, vy = 0,
    marker_ref_vl = nil,
    marker_ref_vt = nil,
    base_tx = nil,
    base_ty = nil,
    canvas_zoom = nil,
    pending_display = false,  -- visible tiles converted, awaiting batch display
    sd_ok = false,
    wifi_ok = false,
}

-- ---------------------------------------------------------------------------
-- Tile math  (OSM slippy map conventions)
-- ---------------------------------------------------------------------------
local function lat_lon_to_world_px(lat, lon, zoom)
    local n = 2 ^ zoom
    local x = ((lon + 180) / 360) * n * TILE_SIZE
    local lat_rad = math.rad(lat)
    local y = (1 - math.log(math.tan(lat_rad) + 1 / math.cos(lat_rad)) / math.pi) / 2 * n * TILE_SIZE
    return math.floor(x), math.floor(y)
end

local function world_px_to_lat_lon(px, py, zoom)
    local n = 2 ^ zoom
    local lon = px / (n * TILE_SIZE) * 360 - 180
    local a = math.pi * (1 - 2 * py / (n * TILE_SIZE))
    local lat_rad = math.atan((math.exp(a) - math.exp(-a)) / 2)
    return math.deg(lat_rad), lon
end

-- ---------------------------------------------------------------------------
-- Tile cache
-- ---------------------------------------------------------------------------
local bin_cache = {}  -- in-memory set of tiles known to have .bin on SD
local loaded_srcs = {}  -- src paths currently decoded in the LVGL image cache
local function tile_sd_path(z, tx, ty)
    return CACHE_ROOT .. "/" .. z .. "/" .. tx .. "/" .. ty .. ".png"
end

local function tile_bin_path(z, tx, ty)
    return CACHE_ROOT .. "/" .. z .. "/" .. tx .. "/" .. ty .. ".bin"
end

local function tile_img_src(z, tx, ty)
    return "S:" .. tile_bin_path(z, tx, ty)
end

local function tile_cached(z, tx, ty)
    local key = z .. "/" .. tx .. "/" .. ty
    if bin_cache[key] ~= nil then return bin_cache[key] end
    -- _png_to_bin uses atomic write (.tmp → .bin rename), so any .bin
    -- that exists on SD is guaranteed complete. Simple existence check.
    local ok, exists = pcall(_file_exists_sd, tile_bin_path(z, tx, ty))
    if ok and exists then
        bin_cache[key] = true
        return true
    end
    bin_cache[key] = false
    return false
end

local dirs_created = {}
local function ensure_tile_dirs(z, tx)
    local key = z .. "/" .. tx
    if dirs_created[key] then return end
    _mkdir_sd(CACHE_ROOT)
    _mkdir_sd(CACHE_ROOT .. "/" .. z)
    _mkdir_sd(CACHE_ROOT .. "/" .. z .. "/" .. tx)
    dirs_created[key] = true
end

local function enqueue_download(z, tx, ty, visible)
    if not map.sd_ok or not map.wifi_ok then return end
    local max_tile = 2 ^ z - 1
    if tx < 0 or ty < 0 or tx > max_tile or ty > max_tile then return end
    if tile_cached(z, tx, ty) then return end
    local key = z .. "/" .. tx .. "/" .. ty
    for _, q in ipairs(map.download_queue) do
        if q.key == key then
            if visible then q.visible = true end
            return
        end
    end
    table.insert(map.download_queue, { z = z, tx = tx, ty = ty, key = key, visible = visible })
end

-- True if any queued tile is needed in the current on-screen grid.
-- Display is deferred until none remain, so the transient 256KB RGBA decode
-- buffers never interleave with the permanent 131KB tile cache entries
-- (that interleaving fragments PSRAM and breaks lodepng decoding).
local function any_visible_pending()
    for _, q in ipairs(map.download_queue) do
        if q.visible then return true end
    end
    return false
end

local tile_imgs = {}  -- forward-declare; populated in UI setup
local tile_srcs = {}  -- current source path per widget (avoids redundant set_src)
local update_dl_status  -- forward-declare; defined after UI setup
local update_wifi_status  -- forward-declare; defined after UI setup
local show_precache_screen  -- forward-declare; defined after UI setup
local hide_precache_screen  -- forward-declare; defined after UI setup

local function set_tile_widget(idx, z, tx, ty)
    if not tile_imgs[idx] then return false end
    local src = tile_img_src(z, tx, ty)
    if tile_srcs[idx] == src then
        tile_imgs[idx]:clear_flag(lvgl.FLAG.HIDDEN)
        loaded_srcs[src] = true
        return true
    end
    local ok, err = pcall(function()
        tile_imgs[idx]:set_src(src)
        tile_imgs[idx]:clear_flag(lvgl.FLAG.HIDDEN)
    end)
    if ok then
        tile_srcs[idx] = src
        loaded_srcs[src] = true  -- track for off-screen eviction
        return true
    end
    tile_imgs[idx]:add_flag(lvgl.FLAG.HIDDEN)
    tile_srcs[idx] = nil
    return false
end

local function process_download_queue()
    if not map.wifi_ok then return end
    if map.downloading then return end
    if #map.download_queue == 0 then return end
    local item = table.remove(map.download_queue, 1)
    if tile_cached(item.z, item.tx, item.ty) then
        if item.visible and map.canvas_zoom == item.z and map.base_tx then
            local c = item.tx - map.base_tx
            local r = item.ty - map.base_ty
            if c >= 0 and c < GRID and r >= 0 and r < GRID then
                set_tile_widget(r * GRID + c + 1, item.z, item.tx, item.ty)
            end
        end
        return
    end
    map.downloading = true
    ensure_tile_dirs(item.z, item.tx)
    local url = TILE_URL .. "/" .. item.z .. "/" .. item.tx .. "/" .. item.ty .. ".png"
    local path = "S:" .. tile_sd_path(item.z, item.tx, item.ty)
    local result = _wifi_download_file(url, path)
    map.downloading = false
    if result and result.success and result.size and result.size > 0 then
        local bin_path = "S:" .. tile_bin_path(item.z, item.tx, item.ty)
        if not _png_to_bin(path, bin_path) then return end
        bin_cache[item.z .. "/" .. item.tx .. "/" .. item.ty] = true
        if item.visible and map.canvas_zoom == item.z and map.base_tx then
            local c = item.tx - map.base_tx
            local r = item.ty - map.base_ty
            if c >= 0 and c < GRID and r >= 0 and r < GRID then
                set_tile_widget(r * GRID + c + 1, item.z, item.tx, item.ty)
            end
        end
    end
end

-- ---------------------------------------------------------------------------
-- Pre-cache helpers
-- ---------------------------------------------------------------------------
local function format_time(seconds)
    if seconds < 60 then return "<1 min" end
    if seconds < 3600 then return "~" .. math.ceil(seconds / 60) .. " min" end
    local h = math.floor(seconds / 3600)
    local m = math.ceil((seconds % 3600) / 60)
    return "~" .. h .. "h " .. m .. "m"
end

local function radius_at_zoom(radius_z14, z)
    if z <= 14 then
        return math.max(1, math.ceil(radius_z14 / (2 ^ (14 - z))))
    else
        local max_high = { [15] = 8, [16] = 4, [17] = 2 }
        local scaled = radius_z14 * (2 ^ (z - 14))
        return math.min(scaled, max_high[z] or 2)
    end
end

-- Build flat list of candidate tile coordinates (fast, no SD I/O)
local function build_tile_coords(lat, lon, min_z, max_z, radius_z14)
    local coords = {}
    for z = min_z, max_z do
        local px, py = lat_lon_to_world_px(lat, lon, z)
        local ctx = math.floor(px / TILE_SIZE)
        local cty = math.floor(py / TILE_SIZE)
        local r = radius_at_zoom(radius_z14, z)
        local max_tile = 2 ^ z - 1
        for dy = -r, r do
            for dx = -r, r do
                local tx = ctx + dx
                local ty = cty + dy
                if tx >= 0 and ty >= 0 and tx <= max_tile and ty <= max_tile then
                    coords[#coords + 1] = { z = z, tx = tx, ty = ty }
                end
            end
        end
    end
    return coords
end

local function build_precache_list(lat, lon, min_z, max_z, radius_z14)
    local tiles = {}
    for z = min_z, max_z do
        local px, py = lat_lon_to_world_px(lat, lon, z)
        local ctx = math.floor(px / TILE_SIZE)
        local cty = math.floor(py / TILE_SIZE)
        local r = radius_at_zoom(radius_z14, z)
        local max_tile = 2 ^ z - 1
        local zoom_tiles = {}
        for dy = -r, r do
            for dx = -r, r do
                local tx = ctx + dx
                local ty = cty + dy
                if tx >= 0 and ty >= 0 and tx <= max_tile and ty <= max_tile then
                    if not tile_cached(z, tx, ty) then
                        table.insert(zoom_tiles, {
                            z = z, tx = tx, ty = ty,
                            dist = dx * dx + dy * dy,
                        })
                    end
                end
            end
        end
        table.sort(zoom_tiles, function(a, b) return a.dist < b.dist end)
        for _, t in ipairs(zoom_tiles) do
            table.insert(tiles, t)
        end
    end
    return tiles
end

-- ---------------------------------------------------------------------------
-- UI setup
-- ---------------------------------------------------------------------------

local root = lvgl.Object({
    w = W, h = H, x = 0, y = 0,
    pad_all = 0, border_width = 0,
    bg_color = "#1a1a2e",
    clip_corner = true,
})
root:add_flag(lvgl.FLAG.CLICKABLE)
root:clear_flag(lvgl.FLAG.SCROLLABLE)

-- Tile layer — container for Image widgets, repositioned as a unit on scroll
local tile_layer = root:Object({
    w = GRID * TILE_SIZE, h = GRID * TILE_SIZE,
    x = 0, y = 0,
    pad_all = 0, border_width = 0,
    bg_opa = 0,
})
tile_layer:clear_flag(lvgl.FLAG.SCROLLABLE)
tile_layer:clear_flag(lvgl.FLAG.CLICKABLE)

-- Create GRID×GRID Image widgets inside tile_layer
for r = 0, GRID - 1 do
    for c = 0, GRID - 1 do
        local img = tile_layer:Image({
            x = c * TILE_SIZE, y = r * TILE_SIZE,
            w = TILE_SIZE, h = TILE_SIZE,
        })
        img:clear_flag(lvgl.FLAG.CLICKABLE)
        table.insert(tile_imgs, img)
    end
end

-- Touch surface (screen-sized, transparent, captures drag events)
local touch_layer = root:Object({
    w = W, h = H, x = 0, y = 0,
    bg_opa = 0, border_width = 0,
})
touch_layer:add_flag(lvgl.FLAG.CLICKABLE)
touch_layer:clear_flag(lvgl.FLAG.SCROLLABLE)

-- Marker canvas (oversized for scroll buffering, redrawn when edge nears viewport)
local MARKER_PAD = 100
local MCANVAS_W = W + 2 * MARKER_PAD
local MCANVAS_H = H + 2 * MARKER_PAD
local marker_canvas = root:Canvas({
    w = MCANVAS_W, h = MCANVAS_H,
    cf = lvgl.COLOR_FORMAT.ARGB8888,
    x = -MARKER_PAD, y = -MARKER_PAD,
})
marker_canvas:fill_bg("#000000", 0)
marker_canvas:clear_flag(lvgl.FLAG.CLICKABLE)
marker_canvas:clear_flag(lvgl.FLAG.SCROLLABLE)

-- Status bar at top
local status_bar = root:Object({
    w = W, h = 20, x = 0, y = 0,
    bg_color = "#000000", bg_opa = 180,
    pad_left = 4, pad_right = 4,
    border_width = 0,
})
status_bar:clear_flag(lvgl.FLAG.SCROLLABLE)
status_bar:clear_flag(lvgl.FLAG.CLICKABLE)

local status_label = status_bar:Label({
    text = "Map",
    text_color = "#AAAAAA",
    text_font = lvgl.BUILTIN_FONT.MONTSERRAT_14,
    align = lvgl.ALIGN.LEFT_MID,
})

local zoom_label = status_bar:Label({
    text = "z14",
    text_color = "#AAAAAA",
    text_font = lvgl.BUILTIN_FONT.MONTSERRAT_14,
    align = lvgl.ALIGN.RIGHT_MID,
})

local dl_label = status_bar:Label({
    text = "",
    text_color = "#FFB020",
    text_font = lvgl.BUILTIN_FONT.MONTSERRAT_14,
    align = lvgl.ALIGN.CENTER,
})
dl_label:add_flag(lvgl.FLAG.HIDDEN)

-- Zoom buttons (bottom right, for touch)
local zoom_in_btn = root:Button({ w = 36, h = 36, x = W - 44, y = H - 84 })
zoom_in_btn:Label({ text = "+", align = lvgl.ALIGN.CENTER })
zoom_in_btn:clear_flag(lvgl.FLAG.CLICK_FOCUSABLE)

local zoom_out_btn = root:Button({ w = 36, h = 36, x = W - 44, y = H - 44 })
zoom_out_btn:Label({ text = "-", align = lvgl.ALIGN.CENTER })
zoom_out_btn:clear_flag(lvgl.FLAG.CLICK_FOCUSABLE)

-- Close button (bottom left, top of stack)
local close_btn = root:Button({ w = 36, h = 36, x = 8, y = H - 84 })
close_btn:Label({ text = "X", align = lvgl.ALIGN.CENTER })
close_btn:clear_flag(lvgl.FLAG.CLICK_FOCUSABLE)

-- Center-on-self button (bottom left)
local center_btn = root:Button({ w = 36, h = 36, x = 8, y = H - 44 })
pcall(_emoji_preload, 0x1F3E0)
center_btn:Label({ text = "\xF0\x9F\x8F\xA0", align = lvgl.ALIGN.CENTER })
center_btn:clear_flag(lvgl.FLAG.CLICK_FOCUSABLE)

-- Pre-cache download button (bottom left, right of home/center buttons)
local dl_btn = root:Button({ w = 36, h = 36, x = 50, y = H - 44 })
dl_btn:Label({ text = "DL", align = lvgl.ALIGN.CENTER })
dl_btn:clear_flag(lvgl.FLAG.CLICK_FOCUSABLE)
dl_btn:add_flag(lvgl.FLAG.HIDDEN)  -- shown when WiFi + SD are both OK

-- Tooltip for marker info
local tooltip = root:Object({
    w = 200, h = 24, x = (W - 200) / 2, y = H - 30,
    bg_color = "#000000", bg_opa = 200,
    border_width = 1, border_color = "#666688",
    pad_all = 2,
})
tooltip:clear_flag(lvgl.FLAG.SCROLLABLE)
tooltip:clear_flag(lvgl.FLAG.CLICKABLE)
local tooltip_label = tooltip:Label({
    text = "",
    text_color = "#FFFFFF",
    text_font = lvgl.BUILTIN_FONT.MONTSERRAT_14,
    align = lvgl.ALIGN.CENTER,
})
tooltip:add_flag(lvgl.FLAG.HIDDEN)
map.tooltip = tooltip

-- ---------------------------------------------------------------------------
-- Tile rendering
-- ---------------------------------------------------------------------------

-- Evict from the LVGL image cache every tile we've decoded that is no longer in
-- the on-screen grid. Decoded tiles are ~131KB each; left to LVGL's LRU they
-- accumulate to the 3MB cap (old zoom levels, far-scrolled tiles) and fragment
-- PSRAM so the 256KB PNG decode buffer can't be allocated. Bounding the cache to
-- the visible set keeps a large contiguous region free. Zoom changes are handled
-- for free: old-zoom tiles have different paths, so none match the new grid.
local function evict_offscreen_tiles()
    if not map.base_tx or not map.canvas_zoom then return end
    local max_tile = 2 ^ map.canvas_zoom - 1
    local visible = {}
    for r = 0, GRID - 1 do
        for c = 0, GRID - 1 do
            local tx = map.base_tx + c
            local ty = map.base_ty + r
            if tx >= 0 and ty >= 0 and tx <= max_tile and ty <= max_tile then
                visible[tile_img_src(map.canvas_zoom, tx, ty)] = true
            end
        end
    end
    local dropped = 0
    for src in pairs(loaded_srcs) do
        if not visible[src] then
            pcall(_lvgl_image_cache_drop, src)
            loaded_srcs[src] = nil
            dropped = dropped + 1
        end
    end
end

-- Batch display pass: set every cached on-screen tile widget at once.
-- Called only when no visible tiles are pending conversion, so the run of
-- permanent 131KB cache allocations happens contiguously, not interleaved
-- with the transient 256KB conversion buffers.
local function display_visible_tiles()
    if not map.base_tx or not map.canvas_zoom then return end
    local max_tile = 2 ^ map.canvas_zoom - 1
    local shown = 0
    for r = 0, GRID - 1 do
        for c = 0, GRID - 1 do
            local idx = r * GRID + c + 1
            local tx = map.base_tx + c
            local ty = map.base_ty + r
            if tx >= 0 and ty >= 0 and tx <= max_tile and ty <= max_tile
               and tile_cached(map.canvas_zoom, tx, ty) then
                if set_tile_widget(idx, map.canvas_zoom, tx, ty) then
                    shown = shown + 1
                end
            else
                tile_imgs[idx]:add_flag(lvgl.FLAG.HIDDEN)
                tile_srcs[idx] = nil
            end
        end
    end
end

local redraw_markers   -- forward declaration (defined after refresh_tiles)
local update_status    -- forward declaration (defined after refresh_tiles)

local function refresh_tiles()
    if not map.running then return end

    -- Discard stale download queue — only current view matters
    map.download_queue = {}

    local half_w = math.floor(W / 2)
    local half_h = math.floor(H / 2)

    local view_left = map.cx - half_w
    local view_top  = map.cy - half_h

    local base_tx = math.floor(view_left / TILE_SIZE)
    local base_ty = math.floor(view_top / TILE_SIZE)

    map.base_tx = base_tx
    map.base_ty = base_ty
    map.canvas_zoom = map.zoom

    -- Release tiles that just left the view (or the whole previous zoom layer)
    -- before converting new ones, so the decode buffer has contiguous PSRAM.
    evict_offscreen_tiles()

    local off_x = base_tx * TILE_SIZE - view_left
    local off_y = base_ty * TILE_SIZE - view_top

    -- Position the tile layer so the grid aligns with the viewport
    tile_layer:set({ x = off_x, y = off_y })

    local max_tile = 2 ^ map.zoom - 1

    -- Classify on-screen tiles: cached ones are flagged for the batch display
    -- pass; uncached ones are queued (visible=true) so we know to wait for them
    -- before displaying. Widgets keep their current content until the batch
    -- pass to avoid flicker.
    for r = 0, GRID - 1 do
        for c = 0, GRID - 1 do
            local idx = r * GRID + c + 1
            local tx = base_tx + c
            local ty = base_ty + r
            if tx >= 0 and ty >= 0 and tx <= max_tile and ty <= max_tile then
                if tile_cached(map.zoom, tx, ty) then
                    map.pending_display = true
                else
                    tile_imgs[idx]:add_flag(lvgl.FLAG.HIDDEN)
                    tile_srcs[idx] = nil
                    enqueue_download(map.zoom, tx, ty, true)
                end
            else
                tile_imgs[idx]:add_flag(lvgl.FLAG.HIDDEN)
                tile_srcs[idx] = nil
            end
        end
    end

    -- Enqueue downloads for the wider fetch grid (pre-cache buffer ring)
    -- Skip during active scrolling to avoid wasted SD I/O on tiles we'll scroll past
    if map.vx == 0 and map.vy == 0 then
        local fetch_pad = math.floor((FETCH_GRID - GRID) / 2)
        for r = -fetch_pad, GRID - 1 + fetch_pad do
            for c = -fetch_pad, GRID - 1 + fetch_pad do
                if c < 0 or c >= GRID or r < 0 or r >= GRID then
                    local tx = base_tx + c
                    local ty = base_ty + r
                    if tx >= 0 and ty >= 0 and tx <= max_tile and ty <= max_tile then
                        if not tile_cached(map.zoom, tx, ty) then
                            enqueue_download(map.zoom, tx, ty, false)
                        end
                    end
                end
            end
        end
    end

    -- Sort queue: visible tiles first, then by distance from view center
    local center_tx = map.cx / TILE_SIZE
    local center_ty = map.cy / TILE_SIZE
    table.sort(map.download_queue, function(a, b)
        if a.visible ~= b.visible then return a.visible == true end
        local da = (a.tx + 0.5 - center_tx)^2 + (a.ty + 0.5 - center_ty)^2
        local db = (b.tx + 0.5 - center_tx)^2 + (b.ty + 0.5 - center_ty)^2
        return da < db
    end)

    -- If every on-screen tile is already cached (e.g. revisiting an area or
    -- scrolling within pre-fetched tiles), show them immediately in one batch.
    -- Otherwise dl_timer runs the batch once the last visible tile converts.
    if map.pending_display and not any_visible_pending() then
        display_visible_tiles()
        map.pending_display = false
    end

    redraw_markers()
    update_status()
    update_dl_status()
end

-- ---------------------------------------------------------------------------
-- Markers (drawn onto canvas — 1 widget vs 262)
-- ---------------------------------------------------------------------------
redraw_markers = function()
    if not map.running then return end

    local half_w = math.floor(W / 2)
    local half_h = math.floor(H / 2)
    local view_left = map.cx - half_w
    local view_top  = map.cy - half_h

    -- Store reference for canvas sliding
    map.marker_ref_vl = view_left
    map.marker_ref_vt = view_top
    marker_canvas:set({ x = -MARKER_PAD, y = -MARKER_PAD })
    marker_canvas:fill_bg("#000000", 0)

    -- Own position
    local prefs = _mesh_get_node_info()
    local gps_ok, _, _, gps_has_loc, gps_lat, gps_lon = pcall(_gps_info)
    local own_lat = (gps_ok and gps_has_loc and gps_lat) or (prefs and prefs.lat) or 0
    local own_lon = (gps_ok and gps_has_loc and gps_lon) or (prefs and prefs.lon) or 0

    if own_lat ~= 0 or own_lon ~= 0 then
        local px, py = lat_lon_to_world_px(own_lat, own_lon, map.zoom)
        local cx = px - view_left + MARKER_PAD
        local cy = py - view_top + MARKER_PAD
        marker_canvas:draw_rect({
            x1 = cx - 6, y1 = cy - 6, x2 = cx + 5, y2 = cy + 5,
            bg_color = "#00ff88", bg_opa = 255, radius = 6,
            border_color = "#ffffff", border_width = 2, border_opa = 255,
        })
    end

    -- Contact markers
    local ok, contacts = pcall(_mesh_get_contacts)
    if ok and contacts then
        for _, c in ipairs(contacts) do
            if c.lat and c.lon and (c.lat ~= 0 or c.lon ~= 0) then
                local px, py = lat_lon_to_world_px(c.lat, c.lon, map.zoom)
                local cx = px - view_left + MARKER_PAD
                local cy = py - view_top + MARKER_PAD
                if cx >= -4 and cx < MCANVAS_W + 4 and cy >= -4 and cy < MCANVAS_H + 4 then
                    marker_canvas:draw_rect({
                        x1 = cx - 4, y1 = cy - 4, x2 = cx + 3, y2 = cy + 3,
                        bg_color = "#ff6644", bg_opa = 255, radius = 4,
                        border_color = "#ffffff", border_width = 1, border_opa = 255,
                    })
                end
            end
        end
    end
end

-- ---------------------------------------------------------------------------
-- Status / HUD
-- ---------------------------------------------------------------------------
update_status = function()
    if not map.running then return end
    local lat, lon = world_px_to_lat_lon(map.cx, map.cy, map.zoom)
    status_label:set({ text = string.format("%.4f, %.4f", lat, lon) })
    zoom_label:set({ text = "z" .. map.zoom })
end

-- Hit-test contact markers: find the nearest contact to a world-pixel position
local HIT_RADIUS = 20  -- px tolerance for tap/center selection
local function find_nearest_contact(wx, wy)
    local ok, contacts = pcall(_mesh_get_contacts)
    if not ok or not contacts then return nil end
    local best, best_dist = nil, HIT_RADIUS * HIT_RADIUS + 0.0
    for _, c in ipairs(contacts) do
        if c.lat and c.lon and (c.lat ~= 0 or c.lon ~= 0) then
            local px, py = lat_lon_to_world_px(c.lat, c.lon, map.zoom)
            local dx, dy = (px - wx) + 0.0, (py - wy) + 0.0  -- float to avoid int32 overflow
            local d2 = dx * dx + dy * dy
            if d2 < best_dist then
                best = c
                best_dist = d2
            end
        end
    end
    return best
end

local contact_popup = nil
local function close_contact_popup()
    if contact_popup then
        _nav_clear()  -- remove gridnav before delete (avoids use-after-free)
        contact_popup:delete()
        contact_popup = nil
        lvgl.group.focus_obj(root)
    end
end

local function show_contact_popup(contact)
    if not contact then return end
    if contact_popup then close_contact_popup() end

    contact_popup = root:Object({
        w = W, h = H, x = 0, y = 0,
        bg_color = "#000000", bg_opa = 160,
        border_width = 0, pad_all = 0,
    })
    contact_popup:clear_flag(lvgl.FLAG.SCROLLABLE)

    local box = contact_popup:Object({
        w = W - 20, h = lvgl.SIZE_CONTENT,
        align = lvgl.ALIGN.CENTER,
        bg_color = "#1a1a2e", radius = 8,
        border_width = 1, border_color = "#444466",
        pad_all = 10,
        flex = { flex_direction = "column", flex_wrap = "nowrap" },
    })
    box:clear_flag(lvgl.FLAG.SCROLLABLE)

    local function info_row(label, value)
        local row = box:Object({
            w = W - 40, h = 18,
            bg_opa = 0, border_width = 0, pad_all = 0,
        })
        row:clear_flag(lvgl.FLAG.SCROLLABLE)
        row:Label({
            text = label,
            text_color = "#888888",
            text_font = lvgl.BUILTIN_FONT.MONTSERRAT_14,
            align = lvgl.ALIGN.LEFT_MID,
        })
        row:Label({
            text = value,
            text_color = "#FFFFFF",
            text_font = lvgl.BUILTIN_FONT.MONTSERRAT_14,
            align = lvgl.ALIGN.RIGHT_MID,
        })
    end

    -- Title
    box:Label({
        text = contact.name or "Unknown",
        text_color = "#FFFFFF",
        text_font = lvgl.BUILTIN_FONT.MONTSERRAT_14,
        w = W - 40, h = 22,
    })

    -- Type
    info_row("Type", contact.type_name or "?")

    -- ID (first 8 hex chars of public key)
    if contact.pubkey then
        info_row("ID", contact.pubkey:sub(1, 8) .. "…")
    end

    -- Position
    if contact.lat and contact.lon and (contact.lat ~= 0 or contact.lon ~= 0) then
        info_row("Position", string.format("%.5f, %.5f", contact.lat, contact.lon))
    end

    -- Distance from own position
    local gps_ok, _, _, gps_has_loc, gps_lat, gps_lon = pcall(_gps_info)
    local prefs = _mesh_get_node_info()
    local own_lat = (gps_ok and gps_has_loc and gps_lat) or (prefs and prefs.lat) or 0
    local own_lon = (gps_ok and gps_has_loc and gps_lon) or (prefs and prefs.lon) or 0
    if (own_lat ~= 0 or own_lon ~= 0) and contact.lat and contact.lon
       and (contact.lat ~= 0 or contact.lon ~= 0) then
        local dlat = math.rad(contact.lat - own_lat)
        local dlon = math.rad(contact.lon - own_lon)
        local a = math.sin(dlat / 2) ^ 2
            + math.cos(math.rad(own_lat)) * math.cos(math.rad(contact.lat))
            * math.sin(dlon / 2) ^ 2
        local dist_km = 6371 * 2 * math.atan(math.sqrt(a), math.sqrt(1 - a))
        if dist_km < 1 then
            info_row("Distance", string.format("%.0f m", dist_km * 1000))
        else
            info_row("Distance", string.format("%.1f km", dist_km))
        end
    end

    -- Hops
    if contact.path_len then
        local hops = contact.path_len >= 0 and (contact.path_len .. " hops") or "flood"
        info_row("Path", hops)
    end

    -- Last seen
    if contact.last_seen and contact.last_seen > 0 then
        local now = os.time()
        local ago = now - contact.last_seen
        local ago_text
        if ago < 60 then
            ago_text = "just now"
        elseif ago < 3600 then
            ago_text = math.floor(ago / 60) .. "m ago"
        elseif ago < 86400 then
            ago_text = math.floor(ago / 3600) .. "h ago"
        else
            ago_text = math.floor(ago / 86400) .. "d ago"
        end
        info_row("Last seen", ago_text)
    end

    -- Close button
    local close_btn = box:Button({ w = W - 40, h = 30 })
    close_btn:Label({ text = "Close", align = lvgl.ALIGN.CENTER })
    close_btn:onClicked(function() close_contact_popup() end)

    _nav_setup(box, GRIDNAV_ROLLOVER)
end

update_dl_status = function()
    if not map.sd_ok or not map.wifi_ok then return end  -- other label already shown
    local pending = #map.download_queue + (map.downloading and 1 or 0)
    if pending > 0 then
        dl_label:set({ text = "DL " .. pending })
        dl_label:clear_flag(lvgl.FLAG.HIDDEN)
    else
        dl_label:add_flag(lvgl.FLAG.HIDDEN)
    end
end

update_wifi_status = function()
    local st = _wifi_status()
    local was_ok = map.wifi_ok
    map.wifi_ok = (st == "connected")

    if not map.sd_ok then return end  -- "No SD" takes priority

    if map.wifi_ok and not was_ok then
        -- WiFi came back: show DL button, clear offline indicator
        dl_btn:clear_flag(lvgl.FLAG.HIDDEN)
        dl_label:add_flag(lvgl.FLAG.HIDDEN)
        -- Re-enqueue tiles for any visible gaps
        refresh_tiles()
    elseif not map.wifi_ok and was_ok then
        -- WiFi dropped: hide DL button, show offline, flush queue
        dl_btn:add_flag(lvgl.FLAG.HIDDEN)
        dl_label:set({ text = "Offline" })
        dl_label:clear_flag(lvgl.FLAG.HIDDEN)
        map.download_queue = {}
    elseif not map.wifi_ok then
        -- Still offline — keep label (might have been overwritten)
        dl_label:set({ text = "Offline" })
        dl_label:clear_flag(lvgl.FLAG.HIDDEN)
    end
end

-- ---------------------------------------------------------------------------
-- Pre-cache download screen
-- ---------------------------------------------------------------------------
local pc = {
    timer = nil,
    calc_timer = nil,
    done_timer = nil,
    queue = {},
    total = 0,
    completed = 0,
    start_time = nil,
}

local pc_overlay = nil
local pc_confirm_group = nil
local pc_progress_group = nil
local pc_bar_fill = nil
local pc_counter_lbl = nil
local pc_zoom_lbl = nil
local pc_eta_lbl = nil
local pc_progress_title = nil
local BAR_W = W - 60

local function update_progress_ui()
    if not pc_counter_lbl then return end
    local pct = pc.total > 0 and pc.completed / pc.total or 0
    pc_bar_fill:set({ w = math.max(1, math.floor(BAR_W * pct)) })
    pc_counter_lbl:set({ text = pc.completed .. " / " .. pc.total .. " tiles" })

    if #pc.queue > 0 then
        pc_zoom_lbl:set({ text = "Zoom " .. pc.queue[1].z })
    end

    if pc.completed > 0 and pc.start_time then
        local elapsed = os.time() - pc.start_time
        if elapsed > 0 then
            local avg = elapsed / pc.completed
            local remaining = math.floor((pc.total - pc.completed) * avg)
            pc_eta_lbl:set({ text = format_time(remaining) .. " remaining" })
        end
    end
end

local function show_completion()
    pc_progress_title:set({ text = "Download Complete!" })
    pc_counter_lbl:set({ text = pc.completed .. " tiles cached" })
    pc_bar_fill:set({ w = BAR_W })
    pc_zoom_lbl:add_flag(lvgl.FLAG.HIDDEN)
    pc_eta_lbl:set({ text = "" })
    pc.done_timer = lvgl.Timer({ period = 2000, cb = function(t)
        t:delete()
        pc.done_timer = nil
        hide_precache_screen()
    end })
end

hide_precache_screen = function()
    if pc.timer then pcall(function() pc.timer:delete() end); pc.timer = nil end
    if pc.calc_timer then pcall(function() pc.calc_timer:delete() end); pc.calc_timer = nil end
    if pc.done_timer then pcall(function() pc.done_timer:delete() end); pc.done_timer = nil end
    pc.queue = {}
    pc.total = 0
    pc.completed = 0
    pc.start_time = nil
    _nav_clear()  -- remove gridnav before delete (avoids use-after-free)
    if pc_overlay then pc_overlay:delete(); pc_overlay = nil end
    pc_confirm_group = nil
    pc_progress_group = nil
    pc_bar_fill = nil
    pc_counter_lbl = nil
    pc_zoom_lbl = nil
    pc_eta_lbl = nil
    pc_progress_title = nil
    bin_cache = {}
    refresh_tiles()
    lvgl.group.focus_obj(root)
end

show_precache_screen = function()
    local lat, lon = world_px_to_lat_lon(map.cx, map.cy, map.zoom)
    local tile_km = 40075 * math.cos(math.rad(lat)) / (2 ^ 14)

    -- Full-screen overlay
    pc_overlay = root:Object({
        w = W, h = H, x = 0, y = 0,
        bg_color = "#1a1a2e", bg_opa = 255,
        pad_all = 8, border_width = 0,
        flex = { flex_direction = "column", flex_wrap = "nowrap" },
    })
    pc_overlay:clear_flag(lvgl.FLAG.SCROLLABLE)

    -- ===== CONFIRM GROUP =====
    pc_confirm_group = pc_overlay:Object({
        w = W - 16, h = lvgl.SIZE_CONTENT,
        bg_opa = 0, border_width = 0, pad_all = 0,
        flex = { flex_direction = "column", flex_wrap = "nowrap" },
    })
    pc_confirm_group:clear_flag(lvgl.FLAG.SCROLLABLE)

    -- Title
    pc_confirm_group:Label({
        text = "Download Map Tiles",
        text_color = "#FFFFFF",
        text_font = lvgl.BUILTIN_FONT.MONTSERRAT_14,
        w = lvgl.PCT(100), h = 24,
    })

    -- Area row
    local area_row = pc_confirm_group:Object({
        w = W - 16, h = 34,
        bg_opa = 0, border_width = 0, pad_all = 0,
        flex = { flex_direction = "row", flex_wrap = "nowrap" },
    })
    area_row:clear_flag(lvgl.FLAG.SCROLLABLE)

    area_row:Label({
        text = "Area: ",
        text_color = "#AAAAAA",
        text_font = lvgl.BUILTIN_FONT.MONTSERRAT_14,
        w = 55, h = 30,
    })
    local area_dd = area_row:Dropdown({
        options = "Small\nMedium\nLarge\nHuge",
        w = 110, h = 30,
    })
    local area_desc = area_row:Label({
        text = "",
        text_color = "#888888",
        text_font = lvgl.BUILTIN_FONT.MONTSERRAT_14,
        w = 120, h = 30,
    })

    -- Zoom row (min + max on same line)
    local zoom_row = pc_confirm_group:Object({
        w = W - 16, h = 34,
        bg_opa = 0, border_width = 0, pad_all = 0,
        flex = { flex_direction = "row", flex_wrap = "nowrap" },
    })
    zoom_row:clear_flag(lvgl.FLAG.SCROLLABLE)

    -- Build zoom options string
    local zoom_opts = ""
    for z = MIN_ZOOM, MAX_ZOOM do
        if z > MIN_ZOOM then zoom_opts = zoom_opts .. "\n" end
        zoom_opts = zoom_opts .. z
    end

    zoom_row:Label({
        text = "Min z:",
        text_color = "#AAAAAA",
        text_font = lvgl.BUILTIN_FONT.MONTSERRAT_14,
        w = 55, h = 30,
    })
    local minz_dd = zoom_row:Dropdown({
        options = zoom_opts,
        w = 60, h = 30,
    })
    minz_dd:set({ selected = 10 - MIN_ZOOM })
    zoom_row:Label({
        text = " Max z:",
        text_color = "#AAAAAA",
        text_font = lvgl.BUILTIN_FONT.MONTSERRAT_14,
        w = 65, h = 30,
    })
    local maxz_dd = zoom_row:Dropdown({
        options = zoom_opts,
        w = 60, h = 30,
    })
    maxz_dd:set({ selected = 14 - MIN_ZOOM })

    -- Info labels
    local count_lbl = pc_confirm_group:Label({
        text = "",
        text_color = "#FFFFFF",
        text_font = lvgl.BUILTIN_FONT.MONTSERRAT_14,
        w = lvgl.PCT(100), h = 20,
    })
    local eta_lbl = pc_confirm_group:Label({
        text = "",
        text_color = "#FFFFFF",
        text_font = lvgl.BUILTIN_FONT.MONTSERRAT_14,
        w = lvgl.PCT(100), h = 20,
    })
    local warn_lbl = pc_confirm_group:Label({
        text = "",
        text_color = "#FF4444",
        text_font = lvgl.BUILTIN_FONT.MONTSERRAT_14,
        w = lvgl.PCT(100), h = 20,
    })
    warn_lbl:add_flag(lvgl.FLAG.HIDDEN)

    -- Buttons
    local btn_row = pc_confirm_group:Object({
        w = W - 16, h = 38,
        bg_opa = 0, border_width = 0, pad_all = 0,
        flex = { flex_direction = "row", flex_wrap = "nowrap" },
    })
    btn_row:clear_flag(lvgl.FLAG.SCROLLABLE)

    local BTN_CANCEL_W = 100
    local BTN_DL_W = 130
    local cancel_btn = btn_row:Button({ w = BTN_CANCEL_W, h = 32 })
    cancel_btn:Label({ text = "Cancel", align = lvgl.ALIGN.CENTER })

    local spacer_w = math.max(4, W - 16 - BTN_CANCEL_W - BTN_DL_W)
    btn_row:Object({ w = spacer_w, h = 1, bg_opa = 0, border_width = 0 })

    local download_btn = btn_row:Button({ w = BTN_DL_W, h = 32 })
    download_btn:Label({ text = "Download", align = lvgl.ALIGN.CENTER })

    -- ===== PROGRESS GROUP (hidden initially) =====
    pc_progress_group = pc_overlay:Object({
        w = W - 16, h = lvgl.SIZE_CONTENT,
        bg_opa = 0, border_width = 0, pad_all = 0,
        flex = { flex_direction = "column", flex_wrap = "nowrap" },
    })
    pc_progress_group:clear_flag(lvgl.FLAG.SCROLLABLE)
    pc_progress_group:add_flag(lvgl.FLAG.HIDDEN)

    pc_progress_title = pc_progress_group:Label({
        text = "Downloading Tiles",
        text_color = "#FFFFFF",
        text_font = lvgl.BUILTIN_FONT.MONTSERRAT_14,
        w = lvgl.PCT(100), h = 24,
    })

    -- Progress bar (nested Objects since no Bar widget in Lua bindings)
    local bar_bg = pc_progress_group:Object({
        w = BAR_W, h = 14,
        bg_color = "#333333", radius = 4,
        border_width = 0, pad_all = 0,
    })
    bar_bg:clear_flag(lvgl.FLAG.SCROLLABLE)
    pc_bar_fill = bar_bg:Object({
        w = 1, h = 14,
        bg_color = "#FFB020", radius = 4,
        border_width = 0, pad_all = 0,
        x = 0, y = 0,
    })
    pc_bar_fill:clear_flag(lvgl.FLAG.SCROLLABLE)

    pc_counter_lbl = pc_progress_group:Label({
        text = "0 / 0 tiles",
        text_color = "#FFFFFF",
        text_font = lvgl.BUILTIN_FONT.MONTSERRAT_14,
        w = lvgl.PCT(100), h = 20,
    })
    pc_zoom_lbl = pc_progress_group:Label({
        text = "",
        text_color = "#AAAAAA",
        text_font = lvgl.BUILTIN_FONT.MONTSERRAT_14,
        w = lvgl.PCT(100), h = 20,
    })
    pc_eta_lbl = pc_progress_group:Label({
        text = "",
        text_color = "#AAAAAA",
        text_font = lvgl.BUILTIN_FONT.MONTSERRAT_14,
        w = lvgl.PCT(100), h = 20,
    })

    local pc_cancel_btn = pc_progress_group:Button({ w = 100, h = 32 })
    pc_cancel_btn:Label({ text = "Cancel", align = lvgl.ALIGN.CENTER })
    pc_cancel_btn:onClicked(function() hide_precache_screen() end)

    -- ===== WIRING =====

    local CALC_BATCH = 100  -- tiles checked per timer tick (keeps watchdog happy)
    local function recalc_estimate()
        local ai = area_dd:get("selected") + 1
        local area = AREA_PRESETS[ai]
        local min_z = MIN_ZOOM + minz_dd:get("selected")
        local max_z = MIN_ZOOM + maxz_dd:get("selected")

        if min_z > max_z then
            max_z = min_z
            maxz_dd:set({ selected = min_z - MIN_ZOOM })
        end

        local radius_km = math.floor(area.radius * tile_km + 0.5)
        area_desc:set({ text = " ~" .. radius_km .. "km" })
        count_lbl:set({ text = "Calculating..." })
        eta_lbl:set({ text = "" })
        warn_lbl:add_flag(lvgl.FLAG.HIDDEN)

        -- Cancel previous calculation
        if pc.calc_timer then pcall(function() pc.calc_timer:delete() end); pc.calc_timer = nil end

        -- Build coordinate list (fast, no SD I/O)
        local coords = build_tile_coords(lat, lon, min_z, max_z, area.radius)
        local check_idx = 1
        local uncached = 0

        -- Check cache status in batches to avoid watchdog timeout
        pc.calc_timer = lvgl.Timer({ period = 10, cb = function(t)
            local end_idx = math.min(check_idx + CALC_BATCH - 1, #coords)
            for i = check_idx, end_idx do
                local c = coords[i]
                if not tile_cached(c.z, c.tx, c.ty) then
                    uncached = uncached + 1
                end
            end
            check_idx = end_idx + 1

            if check_idx > #coords then
                t:delete()
                pc.calc_timer = nil
                count_lbl:set({ text = "Tiles to download: " .. uncached })
                local est_mb = uncached * 128 / 1024  -- 128KB per tile (256x256 RGB565 .bin)
                local size_str
                if est_mb < 1 then
                    size_str = string.format("~%.0f KB", uncached * 128)
                else
                    size_str = string.format("~%.0f MB", est_mb)
                end
                eta_lbl:set({ text = format_time(uncached * 2.0) .. "  " .. size_str })
            else
                count_lbl:set({ text = "Calculating... " .. math.floor(check_idx / #coords * 100) .. "%" })
            end
        end })
    end

    area_dd:onevent(lvgl.EVENT.VALUE_CHANGED, recalc_estimate)
    minz_dd:onevent(lvgl.EVENT.VALUE_CHANGED, recalc_estimate)
    maxz_dd:onevent(lvgl.EVENT.VALUE_CHANGED, recalc_estimate)

    cancel_btn:onClicked(function() hide_precache_screen() end)

    download_btn:onClicked(function()
        if not map.sd_ok then
            warn_lbl:set({ text = "No SD card!" })
            warn_lbl:clear_flag(lvgl.FLAG.HIDDEN)
            return
        end
        local st = _wifi_status()
        if st ~= "connected" then
            warn_lbl:set({ text = "WiFi not connected!" })
            warn_lbl:clear_flag(lvgl.FLAG.HIDDEN)
            return
        end

        local ai = area_dd:get("selected") + 1
        local area = AREA_PRESETS[ai]
        local min_z = MIN_ZOOM + minz_dd:get("selected")
        local max_z = MIN_ZOOM + maxz_dd:get("selected")
        if min_z > max_z then max_z = min_z end

        local queue = build_precache_list(lat, lon, min_z, max_z, area.radius)
        if #queue == 0 then
            warn_lbl:set({ text = "All tiles already cached!" })
            warn_lbl:clear_flag(lvgl.FLAG.HIDDEN)
            return
        end

        -- Switch to progress view
        pc_confirm_group:add_flag(lvgl.FLAG.HIDDEN)
        pc_progress_group:clear_flag(lvgl.FLAG.HIDDEN)

        pc.queue = queue
        pc.total = #queue
        pc.completed = 0
        pc.start_time = os.time()

        pc_counter_lbl:set({ text = "0 / " .. pc.total .. " tiles" })
        pc_bar_fill:set({ w = 1 })

        local fail_streak = 0
        pc.timer = lvgl.Timer({
            period = 100,
            cb = function(t)
                if #pc.queue == 0 then
                    t:delete()
                    pc.timer = nil
                    show_completion()
                    return
                end

                -- Abort if WiFi dropped (3 consecutive failures = lost connection)
                if fail_streak >= 3 then
                    t:delete()
                    pc.timer = nil
                    pc_progress_title:set({ text = "Download Failed" })
                    pc_counter_lbl:set({ text = pc.completed .. " / " .. pc.total .. " tiles" })
                    pc_eta_lbl:set({ text = "WiFi connection lost" })
                    return
                end

                local tile = table.remove(pc.queue, 1)

                if tile_cached(tile.z, tile.tx, tile.ty) then
                    pc.completed = pc.completed + 1
                    update_progress_ui()
                    return
                end

                ensure_tile_dirs(tile.z, tile.tx)
                local url = TILE_URL .. "/" .. tile.z .. "/" .. tile.tx .. "/" .. tile.ty .. ".png"
                local png_path = "S:" .. tile_sd_path(tile.z, tile.tx, tile.ty)
                local result = _wifi_download_file(url, png_path)

                if result and result.success and result.size and result.size > 0 then
                    local bin_path = "S:" .. tile_bin_path(tile.z, tile.tx, tile.ty)
                    _png_to_bin(png_path, bin_path)
                    bin_cache[tile.z .. "/" .. tile.tx .. "/" .. tile.ty] = true
                    fail_streak = 0
                else
                    fail_streak = fail_streak + 1
                end

                pc.completed = pc.completed + 1
                update_progress_ui()
            end,
        })
    end)

    -- Initial estimate
    recalc_estimate()

    -- Navigation for trackball
    _nav_setup(pc_overlay, GRIDNAV_ROLLOVER)
end

-- ---------------------------------------------------------------------------
-- Navigation
-- ---------------------------------------------------------------------------
local function reposition_tiles()
    local half_w = math.floor(W / 2)
    local half_h = math.floor(H / 2)
    local view_left = map.cx - half_w
    local view_top = map.cy - half_h

    -- If the view has scrolled past the current tile grid, load new tiles
    if map.base_tx then
        local new_base_tx = math.floor(view_left / TILE_SIZE)
        local new_base_ty = math.floor(view_top / TILE_SIZE)
        if new_base_tx ~= map.base_tx or new_base_ty ~= map.base_ty then
            refresh_tiles()
            return
        end
    end

    -- Move the tile layer (1 container, all children move with it)
    if map.base_tx then
        tile_layer:set({
            x = map.base_tx * TILE_SIZE - view_left,
            y = map.base_ty * TILE_SIZE - view_top,
        })
    end
    -- Slide marker canvas
    if map.marker_ref_vl then
        local dx = map.marker_ref_vl - view_left
        local dy = map.marker_ref_vt - view_top
        marker_canvas:set({ x = dx - MARKER_PAD, y = dy - MARKER_PAD })
        -- Recenter canvas when edge nears viewport
        if math.abs(dx) > MARKER_PAD - 20 or math.abs(dy) > MARKER_PAD - 20 then
            redraw_markers()
        end
    end
end

-- Momentum constants (shared by trackball and touch)
local IMPULSE = 6        -- px/tick added per trackball input
local FRICTION = 0.88    -- velocity multiplier per momentum tick (applied when not dragging)
local STOP_THRESH = 0.5  -- below this, snap to zero
local MAX_VEL = 20       -- px/tick cap — universal max map speed (touch + trackball)

-- Inputs (trackball + touch) only feed raw velocity into map.vx/vy.
-- The momentum controller is the single authority that caps the speed.
local function trackball_impulse(dx, dy)
    map.vx = map.vx + dx
    map.vy = map.vy + dy
end

local function brake()
    map.vx = 0
    map.vy = 0
    refresh_tiles()
end

local function set_zoom(z)
    local new_z = math.max(MIN_ZOOM, math.min(MAX_ZOOM, z))
    if new_z == map.zoom then return end
    map.vx = 0
    map.vy = 0
    bin_cache = {}

    local lat, lon = world_px_to_lat_lon(map.cx, map.cy, map.zoom)
    map.zoom = new_z
    map.cx, map.cy = lat_lon_to_world_px(lat, lon, map.zoom)
    refresh_tiles()
end

local function center_on_self()
    map.vx = 0
    map.vy = 0
    local gps_ok, _, _, gps_has_loc, gps_lat, gps_lon = pcall(_gps_info)
    local prefs = _mesh_get_node_info()
    local lat = (gps_ok and gps_has_loc and gps_lat) or (prefs and prefs.lat) or 0
    local lon = (gps_ok and gps_has_loc and gps_lon) or (prefs and prefs.lon) or 0
    if lat == 0 and lon == 0 then
        tooltip_label:set({ text = "No GPS fix" })
        map.tooltip:clear_flag(lvgl.FLAG.HIDDEN)
        return
    end
    map.cx, map.cy = lat_lon_to_world_px(lat, lon, map.zoom)
    refresh_tiles()
end

local function shutdown()
    map.running = false
    _nav_clear()  -- remove gridnav before delete (avoids use-after-free)
    for _, t in ipairs(map.timers) do pcall(function() t:delete() end) end
    map.timers = {}
    root:delete()
    local launcher = require("launcher")
    launcher.create()
end

-- ---------------------------------------------------------------------------
-- Input handlers
-- ---------------------------------------------------------------------------

-- Keyboard / trackball
root:onevent(lvgl.EVENT.KEY, function()
    if not map.running then return end
    local indev = lvgl.indev.get_act()
    local key = indev:get_key()
    if key == lvgl.KEY.UP then
        trackball_impulse(0, -IMPULSE)
    elseif key == lvgl.KEY.DOWN then
        trackball_impulse(0, IMPULSE)
    elseif key == lvgl.KEY.LEFT then
        trackball_impulse(-IMPULSE, 0)
    elseif key == lvgl.KEY.RIGHT then
        trackball_impulse(IMPULSE, 0)
    elseif key == lvgl.KEY.ENTER then
        if map.vx ~= 0 or map.vy ~= 0 then
            brake()
        else
            local contact = find_nearest_contact(map.cx, map.cy)
            if contact then
                show_contact_popup(contact)
            end
        end
    elseif key == lvgl.KEY.ESC or key == 27 or key == 113 then -- ESC / q
        if contact_popup then
            close_contact_popup()
        else
            shutdown()
        end
    elseif key == 43 or key == 111 then -- + / o
        set_zoom(map.zoom + 1)
    elseif key == 45 or key == 105 then -- - / i
        set_zoom(map.zoom - 1)
    elseif key == 104 then -- h
        center_on_self()
    elseif key == 32 then -- space
        brake()
    end
end)

-- Touch input — feeds into shared momentum system
touch_layer:onevent(lvgl.EVENT.PRESSED, function()
    if not map.running then return end
    map.vx = 0
    map.vy = 0
    map.drag = true
    map.tooltip:add_flag(lvgl.FLAG.HIDDEN)
end)

touch_layer:onevent(lvgl.EVENT.PRESSING, function()
    if not map.running or not map.drag then return end
    local indev = lvgl.indev.get_act()
    local vx, vy = indev:get_vect()
    map.vx = -vx
    map.vy = -vy
end)

touch_layer:onevent(lvgl.EVENT.LONG_PRESSED, function()
    if not map.running then return end
    map.vx = 0
    map.vy = 0
    map.drag = false
    local indev = lvgl.indev.get_act()
    local sx, sy = indev:get_point()
    -- Convert screen position to world pixels
    local half_w = math.floor(W / 2)
    local half_h = math.floor(H / 2)
    local wx = map.cx - half_w + sx
    local wy = map.cy - half_h + sy
    local contact = find_nearest_contact(wx, wy)
    if contact then
        show_contact_popup(contact)
    end
end)

touch_layer:onevent(lvgl.EVENT.RELEASED, function()
    map.drag = false
    if map.vx == 0 and map.vy == 0 then
        refresh_tiles()
    end
end)

touch_layer:onevent(lvgl.EVENT.PRESS_LOST, function()
    map.drag = false
    if map.vx == 0 and map.vy == 0 then
        refresh_tiles()
    end
end)

-- Close button
close_btn:onevent(lvgl.EVENT.CLICKED, function()
    shutdown()
end)

-- Zoom buttons
zoom_in_btn:onevent(lvgl.EVENT.CLICKED, function()
    if map.running then set_zoom(map.zoom + 1) end
    lvgl.group.focus_obj(root)
end)
zoom_out_btn:onevent(lvgl.EVENT.CLICKED, function()
    if map.running then set_zoom(map.zoom - 1) end
    lvgl.group.focus_obj(root)
end)
center_btn:onevent(lvgl.EVENT.CLICKED, function()
    if map.running then center_on_self() end
    lvgl.group.focus_obj(root)
end)

dl_btn:onevent(lvgl.EVENT.CLICKED, function()
    if map.running then show_precache_screen() end
end)

-- ---------------------------------------------------------------------------
-- Tile download timer
-- ---------------------------------------------------------------------------

local dl_timer = lvgl.Timer({
    period = 200,
    cb = function(t)
        if not map.running then t:delete(); return end
        process_download_queue()
        update_dl_status()
    end,
})
table.insert(map.timers, dl_timer)

-- Periodic WiFi status check (every 3s)
local wifi_timer = lvgl.Timer({
    period = 3000,
    cb = function(t)
        if not map.running then t:delete(); return end
        update_wifi_status()
    end,
})
table.insert(map.timers, wifi_timer)

-- Momentum timer — single movement engine for both touch and trackball
local momentum_timer = lvgl.Timer({
    period = 30,
    cb = function(t)
        if not map.running then t:delete(); return end
        if map.vx == 0 and map.vy == 0 then return end

        -- Universal speed cap: whatever the inputs fed in (touch fling/drag or
        -- trackball roll), clamp it here so nothing can move the map faster than
        -- MAX_VEL. Single source of truth for max movement speed.
        map.vx = math.max(-MAX_VEL, math.min(MAX_VEL, map.vx))
        map.vy = math.max(-MAX_VEL, math.min(MAX_VEL, map.vy))

        -- Apply velocity to position
        local max_px = 2 ^ map.zoom * TILE_SIZE
        map.cx = math.max(0, math.min(max_px - 1, map.cx + map.vx))
        map.cy = math.max(0, math.min(max_px - 1, map.cy + map.vy))

        -- Friction only when coasting (not during touch drag — finger controls velocity)
        if not map.drag then
            map.vx = map.vx * FRICTION
            map.vy = map.vy * FRICTION

            if math.abs(map.vx) < STOP_THRESH and math.abs(map.vy) < STOP_THRESH then
                map.vx = 0
                map.vy = 0
                refresh_tiles()
                return
            end
        end

        reposition_tiles()
    end,
})
table.insert(map.timers, momentum_timer)

-- Auto-hide tooltip after 3 seconds
local tooltip_timer = lvgl.Timer({
    period = 3000,
    cb = function(t)
        if not map.running then t:delete(); return end
        map.tooltip:add_flag(lvgl.FLAG.HIDDEN)
    end,
})
table.insert(map.timers, tooltip_timer)

-- ---------------------------------------------------------------------------
-- Initial view: center on own position or default
-- ---------------------------------------------------------------------------
local function init_view()
    local gps_ok, _, _, gps_has_loc, gps_lat, gps_lon = pcall(_gps_info)
    local prefs = _mesh_get_node_info()

    local lat = (gps_ok and gps_has_loc and gps_lat) or (prefs and prefs.lat) or 0
    local lon = (gps_ok and gps_has_loc and gps_lon) or (prefs and prefs.lon) or 0

    if lat == 0 and lon == 0 then
        local ok, contacts = pcall(_mesh_get_contacts)
        if ok and contacts then
            for _, c in ipairs(contacts) do
                if c.lat and c.lon and (c.lat ~= 0 or c.lon ~= 0) then
                    lat, lon = c.lat, c.lon
                    break
                end
            end
        end
    end

    if lat == 0 and lon == 0 then
        lat, lon = 39.8283, -98.5795
    end

    map.cx, map.cy = lat_lon_to_world_px(lat, lon, map.zoom)

    local wstatus = _wifi_status()
    map.wifi_ok = (wstatus == "connected")

    map.sd_ok = pcall(_file_exists_sd, "/meshpunk")

    -- Status bar indicator: SD takes priority over WiFi
    if not map.sd_ok then
        dl_label:set({ text = "No SD" })
        dl_label:clear_flag(lvgl.FLAG.HIDDEN)
    elseif not map.wifi_ok then
        dl_label:set({ text = "Offline" })
        dl_label:clear_flag(lvgl.FLAG.HIDDEN)
    end

    -- Only show DL button when both SD and WiFi are available
    if map.sd_ok and map.wifi_ok then
        dl_btn:clear_flag(lvgl.FLAG.HIDDEN)
    end

    refresh_tiles()
end

_nav_clear()
root:add_flag(lvgl.FLAG.CLICKABLE)
root:add_flag(lvgl.FLAG.CLICK_FOCUSABLE)
local group = lvgl.group.get_default()
group:add_obj(root)
lvgl.group.focus_obj(root)
init_view()
print("[Map] ready")
