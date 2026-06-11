local lvgl = require("lvgl")
local messages = require("lib/mesh/messages")

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
-- Map preferences (LittleFS so they survive without an SD card)
-- ---------------------------------------------------------------------------
local PREFS_PATH = "L:/map_prefs"

-- Prefs file: key=value lines (anim=0/1, arch=0/1)
local function load_map_prefs()
    local prefs = { anim = true, archived = false }
    local f = io.open(PREFS_PATH, "r")
    if not f then return prefs end
    local txt = f:read("*a") or ""
    f:close()
    if string.find(txt, "anim=0", 1, true) then prefs.anim = false end
    if string.find(txt, "arch=1", 1, true) then prefs.archived = true end
    return prefs
end

local function save_map_prefs(anim_on, archived_on)
    local f = io.open(PREFS_PATH, "w")
    if not f then return end
    f:write((anim_on and "anim=1" or "anim=0") .. "\n" ..
            (archived_on and "arch=1" or "arch=0"))
    f:close()
end

local map_prefs = load_map_prefs()

-- ---------------------------------------------------------------------------
-- State
-- ---------------------------------------------------------------------------
local map = {
    running = true,
    zoom = 14,
    cx = 0, cy = 0,
    timers = {},
    download_queue = {},
    tooltip = nil,
    drag = nil,
    vx = 0, vy = 0,
    marker_ref_vl = nil,
    marker_ref_vt = nil,
    base_tx = nil,
    base_ty = nil,
    canvas_zoom = nil,
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

local function tile_bin_path(z, tx, ty)
    return CACHE_ROOT .. "/" .. z .. "/" .. tx .. "/" .. ty .. ".bin"
end

local function tile_img_src(z, tx, ty)
    return "S:" .. tile_bin_path(z, tx, ty)
end

local function tile_cached(z, tx, ty)
    local key = z .. "/" .. tx .. "/" .. ty
    if bin_cache[key] ~= nil then return bin_cache[key] end
    -- Tile conversion uses atomic write (.tmp → .bin rename), so any .bin
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

local tile_imgs = {}  -- forward-declare; populated in UI setup
local tile_srcs = {}  -- current source path per widget (avoids redundant set_src)
-- Cached margin tiles (in the 4x4 grid but off-screen) waiting to be loaded.
-- Visible tiles load immediately in refresh_tiles; these trickle in via
-- dl_timer (1-2 per tick) so each ~50ms SD read never stalls a pan frame.
local margin_pending = {}
-- Tile fetches running on the Core-1 worker (_tile_fetch_start/_tile_fetch_poll).
-- pending_fetches[key] = { kind = "map"|"pc", z, tx, ty, retried }
local pending_fetches = {}
local map_inflight = 0        -- worker fetches owned by the map view
local fetch_outstanding = 0   -- total worker fetches in flight (map + precache)
local poll_fetch_results      -- forward-declare; defined after pre-cache helpers

-- Packet path animation layer state. Each queue entry is a waypoint list
-- (lat/lon, travel order: sender -> repeaters -> us) built when a channel
-- message arrives; one animation plays at a time.
local anim = {
    enabled = map_prefs.anim,
    queue = {},
    active = nil,
}

-- Include archived contacts (evicted from / removed out of the live mesh
-- table) in markers, tap targets and path resolution. Settings toggle.
local show_archived = map_prefs.archived

-- Own position: GPS fix first, node prefs as fallback. nil when unknown.
local function own_position()
    local gps_ok, _, _, gps_has_loc, gps_lat, gps_lon = pcall(_gps_info)
    local prefs = _mesh_get_node_info()
    local lat = (gps_ok and gps_has_loc and gps_lat) or (prefs and prefs.lat) or 0
    local lon = (gps_ok and gps_has_loc and gps_lon) or (prefs and prefs.lon) or 0
    if lat == 0 and lon == 0 then return nil end
    return lat, lon
end
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

-- Show a tile on its grid widget if it falls inside the current grid.
local function show_tile_if_on_grid(z, tx, ty)
    if map.canvas_zoom ~= z or not map.base_tx then return end
    local c = tx - map.base_tx
    local r = ty - map.base_ty
    if c >= 0 and c < GRID and r >= 0 and r < GRID then
        set_tile_widget(r * GRID + c + 1, z, tx, ty)
    end
end

-- Feed the Core-1 fetch worker from the view's download queue, keeping up to
-- two requests in flight so the worker's keep-alive connection stays hot.
-- Results come back through poll_fetch_results().
local MAX_MAP_INFLIGHT = 2

local function feed_tile_fetches()
    if not map.wifi_ok then return end
    while #map.download_queue > 0 and map_inflight < MAX_MAP_INFLIGHT do
        local item = map.download_queue[1]
        if tile_cached(item.z, item.tx, item.ty) then
            table.remove(map.download_queue, 1)
            show_tile_if_on_grid(item.z, item.tx, item.ty)
        elseif pending_fetches[item.key] then
            table.remove(map.download_queue, 1)  -- already in flight
        else
            ensure_tile_dirs(item.z, item.tx)
            local url = TILE_URL .. "/" .. item.z .. "/" .. item.tx .. "/" .. item.ty .. ".png"
            if not _tile_fetch_start(url, "S:" .. tile_bin_path(item.z, item.tx, item.ty), item.key) then
                break  -- worker queue full — retry next tick
            end
            pending_fetches[item.key] = { kind = "map", z = item.z, tx = item.tx, ty = item.ty }
            map_inflight = map_inflight + 1
            fetch_outstanding = fetch_outstanding + 1
            table.remove(map.download_queue, 1)
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

-- Moving packet dot for the path animation (above the marker canvas, below
-- the HUD). One widget repositioned per tick — far cheaper than repainting
-- the canvas every frame.
local anim_dot = root:Object({
    w = 10, h = 10, x = -20, y = -20,
    bg_color = "#00e0ff", bg_opa = 255, radius = 5,
    border_color = "#ffffff", border_width = 1,
})
anim_dot:add_flag(lvgl.FLAG.HIDDEN)
anim_dot:clear_flag(lvgl.FLAG.CLICKABLE)
anim_dot:clear_flag(lvgl.FLAG.SCROLLABLE)

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

-- Settings button (bottom left, right of home/center buttons). Always
-- visible — tile pre-cache moved into the settings page, which validates
-- SD/WiFi itself.
local settings_btn = root:Button({ w = 36, h = 36, x = 50, y = H - 44 })
pcall(_emoji_preload, 0x2699)
settings_btn:Label({ text = "\xE2\x9A\x99", align = lvgl.ALIGN.CENTER })
settings_btn:clear_flag(lvgl.FLAG.CLICK_FOCUSABLE)

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

local redraw_markers   -- forward declaration (defined after refresh_tiles)
local update_status    -- forward declaration (defined after refresh_tiles)

local function refresh_tiles()
    if not map.running then return end

    -- Discard stale queues — only the current view matters
    map.download_queue = {}
    margin_pending = {}

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

    -- Visible-first: the viewport only ever intersects 3×2 of the 4×4 grid.
    -- Slots actually on screen load immediately (≤6 SD reads, mostly LVGL
    -- cache hits when panning); cached margin slots go to margin_pending and
    -- trickle in via dl_timer so they're warm before they scroll on-screen.
    -- A margin slot that already shows the right tile is kept as-is; one with
    -- stale content is hidden so wrong imagery never slides into view.
    for r = 0, GRID - 1 do
        for c = 0, GRID - 1 do
            local idx = r * GRID + c + 1
            local tx = base_tx + c
            local ty = base_ty + r
            if tx >= 0 and ty >= 0 and tx <= max_tile and ty <= max_tile then
                local sx = c * TILE_SIZE + off_x
                local sy = r * TILE_SIZE + off_y
                local on_screen = sx < W and sx + TILE_SIZE > 0
                              and sy < H and sy + TILE_SIZE > 0
                if tile_cached(map.zoom, tx, ty) then
                    if on_screen or tile_srcs[idx] == tile_img_src(map.zoom, tx, ty) then
                        set_tile_widget(idx, map.zoom, tx, ty)
                    else
                        tile_imgs[idx]:add_flag(lvgl.FLAG.HIDDEN)
                        tile_srcs[idx] = nil
                        margin_pending[#margin_pending + 1] =
                            { idx = idx, z = map.zoom, tx = tx, ty = ty }
                    end
                else
                    tile_imgs[idx]:add_flag(lvgl.FLAG.HIDDEN)
                    tile_srcs[idx] = nil
                    enqueue_download(map.zoom, tx, ty, on_screen)
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

    redraw_markers()
    update_status()
    update_dl_status()
end

-- ---------------------------------------------------------------------------
-- Markers (drawn onto canvas — 1 widget vs 262)
-- ---------------------------------------------------------------------------

-- Draw the active packet path polyline onto the marker canvas (canvas
-- coordinates follow map.marker_ref_vl/vt, so canvas slides keep it aligned).
-- Segments touching a synthesized waypoint (repeater with unknown position)
-- are dashed; real repeater hops get a small node square.
local function draw_active_path()
    if not anim.active or not map.marker_ref_vl then return end
    local pts = anim.active.points
    local prev_x, prev_y, prev_real
    for i, p in ipairs(pts) do
        local wx, wy = lat_lon_to_world_px(p.lat, p.lon, map.zoom)
        local cx = wx - map.marker_ref_vl + MARKER_PAD
        local cy = wy - map.marker_ref_vt + MARKER_PAD
        if prev_x then
            local certain = p.real and prev_real
            marker_canvas:draw_line({
                p1 = { x = prev_x, y = prev_y },
                p2 = { x = cx, y = cy },
                color = "#00e0ff", width = 2, opa = 150,
                dash_width = certain and 0 or 6,
                dash_gap = certain and 0 or 5,
                round_start = 1, round_end = 1,
            })
        end
        if p.real and i > 1 and i < #pts then
            marker_canvas:draw_rect({
                x1 = cx - 3, y1 = cy - 3, x2 = cx + 2, y2 = cy + 2,
                bg_color = "#00e0ff", bg_opa = 220, radius = 3,
            })
        end
        prev_x, prev_y, prev_real = cx, cy, p.real
    end
end

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

    -- Contact markers (archived ones render gray)
    local ok, contacts = pcall(_mesh_get_contacts, show_archived)
    if ok and contacts then
        for _, c in ipairs(contacts) do
            if c.lat and c.lon and (c.lat ~= 0 or c.lon ~= 0) then
                local px, py = lat_lon_to_world_px(c.lat, c.lon, map.zoom)
                local cx = px - view_left + MARKER_PAD
                local cy = py - view_top + MARKER_PAD
                if cx >= -4 and cx < MCANVAS_W + 4 and cy >= -4 and cy < MCANVAS_H + 4 then
                    marker_canvas:draw_rect({
                        x1 = cx - 4, y1 = cy - 4, x2 = cx + 3, y2 = cy + 3,
                        bg_color = c.archived and "#888888" or "#ff6644",
                        bg_opa = 255, radius = 4,
                        border_color = "#ffffff", border_width = 1, border_opa = 255,
                    })
                end
            end
        end
    end

    -- Active packet path (if an animation is playing)
    draw_active_path()
end

-- ---------------------------------------------------------------------------
-- Packet path animation — resolve & playback
-- ---------------------------------------------------------------------------

-- Squared distance in degrees, longitude weighted by latitude so ranking is
-- roughly metric. Only used to compare candidates — units don't matter.
local function geo_dist2(lat1, lon1, lat2, lon2)
    local dlat = lat1 - lat2
    local dlon = (lon1 - lon2) * math.cos(math.rad(lat1))
    return dlat * dlat + dlon * dlon
end

-- Resolve a received channel message into animation waypoints.
-- msg.path is in travel order (repeaters append their hash as they forward):
-- path[1] = first repeater after the sender, path[#path] = last before us.
--
-- Rules:
--  * hash matches one positioned contact          -> that position
--  * hash matches several (collision)             -> the candidate nearest the
--    midpoint of the previous position and the next known one; resolving
--    left-to-right makes each pick the anchor for the following hop, which
--    settles consecutive collisions
--  * hash matches nothing (repeater not in contacts / no GPS) -> synthetic
--    waypoint at the midpoint of the previous position and the next known one
local function resolve_path_waypoints(msg)
    local own_lat, own_lon = own_position()
    if not own_lat then return nil end  -- no end point — nothing to animate to

    -- Archived repeaters (when enabled) count as positioned candidates too —
    -- a repeater the mesh evicted can still anchor a hop on the animation.
    local ok, contacts = pcall(_mesh_get_contacts, show_archived)
    if not ok or not contacts then return nil end

    -- Sender position (start point), matched by name
    local start_lat, start_lon
    for _, c in ipairs(contacts) do
        if c.name == msg.from and c.lat and c.lon and (c.lat ~= 0 or c.lon ~= 0) then
            start_lat, start_lon = c.lat, c.lon
            break
        end
    end

    -- Candidate contacts per hop hash (positioned, repeaters preferred)
    local hops = {}
    local n = 0
    for _, hash in ipairs(msg.path or {}) do
        local hl = string.lower(hash)
        local cands, reps = {}, {}
        for _, c in ipairs(contacts) do
            if c.pubkey and string.lower(string.sub(c.pubkey, 1, #hash)) == hl
               and c.lat and c.lon and (c.lat ~= 0 or c.lon ~= 0) then
                cands[#cands + 1] = c
                if c.type_name and string.find(string.lower(c.type_name), "repeater", 1, true) then
                    reps[#reps + 1] = c
                end
            end
        end
        if #reps > 0 then cands = reps end
        n = n + 1
        hops[n] = { cands = cands }
    end

    -- Pass 1: unambiguous hops
    for i = 1, n do
        if #hops[i].cands == 1 then
            local c = hops[i].cands[1]
            hops[i].lat, hops[i].lon, hops[i].real = c.lat, c.lon, true
        end
    end

    -- Next known position after hop i (resolved hop, else our own position)
    local function next_anchor(i)
        for j = i + 1, n do
            if hops[j].lat then return hops[j].lat, hops[j].lon end
        end
        return own_lat, own_lon
    end

    -- Pass 2: collisions — nearest candidate to the prev/next midpoint
    local prev_lat, prev_lon = start_lat, start_lon
    for i = 1, n do
        local h = hops[i]
        if h.lat then
            prev_lat, prev_lon = h.lat, h.lon
        elseif #h.cands > 1 then
            local na_lat, na_lon = next_anchor(i)
            local ref_lat, ref_lon
            if prev_lat then
                ref_lat, ref_lon = (prev_lat + na_lat) / 2, (prev_lon + na_lon) / 2
            else
                ref_lat, ref_lon = na_lat, na_lon  -- no anchor before this hop yet
            end
            local best, best_d
            for _, c in ipairs(h.cands) do
                local d = geo_dist2(c.lat, c.lon, ref_lat, ref_lon)
                if not best_d or d < best_d then best, best_d = c, d end
            end
            h.lat, h.lon, h.real = best.lat, best.lon, true
            prev_lat, prev_lon = h.lat, h.lon
        end
    end

    -- Pass 3: unknown repeaters — synthetic midpoint waypoints
    prev_lat, prev_lon = start_lat, start_lon
    for i = 1, n do
        local h = hops[i]
        if h.lat then
            prev_lat, prev_lon = h.lat, h.lon
        elseif prev_lat then
            local na_lat, na_lon = next_anchor(i)
            h.lat = (prev_lat + na_lat) / 2
            h.lon = (prev_lon + na_lon) / 2
            h.real = false
            prev_lat, prev_lon = h.lat, h.lon
        end
        -- no previous anchor and unknown sender: hop stays unresolved (skipped)
    end

    -- Assemble: sender -> hops -> us. Need at least one segment.
    local points = {}
    if start_lat then
        points[#points + 1] = { lat = start_lat, lon = start_lon, real = true }
    end
    for i = 1, n do
        if hops[i].lat then
            points[#points + 1] = { lat = hops[i].lat, lon = hops[i].lon, real = hops[i].real }
        end
    end
    points[#points + 1] = { lat = own_lat, lon = own_lon, real = true }
    if #points < 2 then return nil end
    return points
end

-- Playback: the dot eases along each segment, dwells briefly on real
-- repeater hops, then pulses out on arrival. Positions are recomputed from
-- lat/lon every tick, so panning and zooming mid-animation stay glued.
local ANIM_HOP_MS = 500
local ANIM_PAUSE_MS = 180
local ANIM_ARRIVE_MS = 350

local function anim_world_to_screen(lat, lon)
    local px, py = lat_lon_to_world_px(lat, lon, map.zoom)
    return px - (map.cx - math.floor(W / 2)), py - (map.cy - math.floor(H / 2))
end

local function anim_place_dot(lat, lon, size)
    size = size or 10
    local sx, sy = anim_world_to_screen(lat, lon)
    anim_dot:set({
        w = size, h = size, radius = math.floor(size / 2),
        x = sx - math.floor(size / 2), y = sy - math.floor(size / 2),
    })
end

local function anim_stop()
    anim.active = nil
    anim_dot:add_flag(lvgl.FLAG.HIDDEN)
    anim_dot:set({ w = 10, h = 10, radius = 5, bg_opa = 255 })
    redraw_markers()  -- wipes the path polyline
end

local function anim_start_next()
    anim.active = table.remove(anim.queue, 1)
    if not anim.active then return end
    local a = anim.active
    a.seg = 1        -- animating points[seg] -> points[seg+1]
    a.t = 0
    a.phase = "move"
    anim_place_dot(a.points[1].lat, a.points[1].lon)
    anim_dot:set({ bg_opa = 255 })
    anim_dot:clear_flag(lvgl.FLAG.HIDDEN)
    redraw_markers()  -- draws the path polyline
end

local function anim_tick(period)
    if not anim.active then
        if anim.enabled and #anim.queue > 0 then anim_start_next() end
        return
    end
    local a = anim.active
    local pts = a.points
    a.t = a.t + period

    if a.phase == "move" then
        local f = math.min(1, a.t / ANIM_HOP_MS)
        local e = f * f * (3 - 2 * f)  -- smoothstep ease
        local p1, p2 = pts[a.seg], pts[a.seg + 1]
        anim_place_dot(p1.lat + (p2.lat - p1.lat) * e,
                       p1.lon + (p2.lon - p1.lon) * e)
        if f >= 1 then
            a.t = 0
            if a.seg + 1 >= #pts then
                a.phase = "arrive"
            else
                a.seg = a.seg + 1
                a.phase = pts[a.seg].real and "pause" or "move"
            end
        end
    elseif a.phase == "pause" then
        anim_place_dot(pts[a.seg].lat, pts[a.seg].lon)
        if a.t >= ANIM_PAUSE_MS then
            a.t = 0
            a.phase = "move"
        end
    elseif a.phase == "arrive" then
        local f = math.min(1, a.t / ANIM_ARRIVE_MS)
        local last = pts[#pts]
        anim_place_dot(last.lat, last.lon, 10 + math.floor(22 * f))
        anim_dot:set({ bg_opa = math.floor(255 * (1 - f)) })
        if f >= 1 then anim_stop() end
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
    local ok, contacts = pcall(_mesh_get_contacts, show_archived)
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

    -- Archived contacts are no longer in the live mesh table
    if contact.archived then
        info_row("Status", "Archived")
    end

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

    -- Re-add an archived contact to the live mesh table
    if contact.archived and contact.pubkey then
        local readd_btn = box:Button({ w = W - 40, h = 30 })
        readd_btn:Label({ text = "Re-add to mesh", align = lvgl.ALIGN.CENTER })
        readd_btn:onClicked(function()
            local ok = _mesh_readd_contact(contact.pubkey)
            tooltip_label:set({ text = ok and "Contact re-added"
                                           or "Re-add failed (table full?)" })
            map.tooltip:clear_flag(lvgl.FLAG.HIDDEN)
            close_contact_popup()
            redraw_markers()
        end)
    end

    -- Close button
    local close_btn = box:Button({ w = W - 40, h = 30 })
    close_btn:Label({ text = "Close", align = lvgl.ALIGN.CENTER })
    close_btn:onClicked(function() close_contact_popup() end)

    _nav_setup(box, GRIDNAV_ROLLOVER)
end

update_dl_status = function()
    if not map.sd_ok or not map.wifi_ok then return end  -- other label already shown
    local pending = #map.download_queue + map_inflight
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
        -- WiFi came back: clear offline indicator
        dl_label:add_flag(lvgl.FLAG.HIDDEN)
        -- Re-enqueue tiles for any visible gaps
        refresh_tiles()
    elseif not map.wifi_ok and was_ok then
        -- WiFi dropped: show offline, flush queue
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
    inflight = 0,     -- worker fetches owned by the pre-cache run
    fail_streak = 0,  -- consecutive network failures (WiFi-loss detector)
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

-- Collect finished Core-1 fetches and route them by key: map tiles display
-- on the grid, pre-cache tiles drive the progress UI. A "frag" failure means
-- PSRAM was too fragmented to decode — drop the LVGL image cache here on the
-- LVGL thread (decoded tiles reload from their .bin) and retry the tile once.
poll_fetch_results = function()
    while true do
        local key, ok, stage = _tile_fetch_poll()
        if key == nil then break end
        local p = pending_fetches[key]
        if p then
            pending_fetches[key] = nil
            fetch_outstanding = math.max(0, fetch_outstanding - 1)
            if p.kind == "map" then
                map_inflight = math.max(0, map_inflight - 1)
            else
                pc.inflight = math.max(0, pc.inflight - 1)
            end

            if ok then
                bin_cache[key] = true
            elseif stage == "frag" and not p.retried then
                pcall(_lvgl_image_cache_drop)
                local url = TILE_URL .. "/" .. p.z .. "/" .. p.tx .. "/" .. p.ty .. ".png"
                if _tile_fetch_start(url, "S:" .. tile_bin_path(p.z, p.tx, p.ty), key) then
                    p.retried = true
                    pending_fetches[key] = p
                    fetch_outstanding = fetch_outstanding + 1
                    if p.kind == "map" then
                        map_inflight = map_inflight + 1
                    else
                        pc.inflight = pc.inflight + 1
                    end
                end
            end

            if pending_fetches[key] == nil then
                -- Fetch concluded (success or final failure)
                if p.kind == "map" then
                    if ok then show_tile_if_on_grid(p.z, p.tx, p.ty) end
                else
                    pc.completed = pc.completed + 1
                    if ok then
                        pc.fail_streak = 0
                    elseif stage == "wifi" or stage == "http" or stage == "truncated" then
                        pc.fail_streak = pc.fail_streak + 1
                    else
                        pc.fail_streak = 0  -- local failure — network is fine
                    end
                    update_progress_ui()
                end
            end
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
    pc.fail_streak = 0
    _nav_clear()  -- remove gridnav before delete (avoids use-after-free)
    if pc_overlay then pc_overlay:delete(); pc_overlay = nil end
    pc_confirm_group = nil
    pc_progress_group = nil
    pc_bar_fill = nil
    pc_counter_lbl = nil
    pc_zoom_lbl = nil
    pc_eta_lbl = nil
    pc_progress_title = nil
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

        -- Block Download until the estimate finishes — clicking mid-calc would
        -- run build_precache_list's cache checks synchronously (SD stat storm
        -- on uncached tiles, possible watchdog reset on big areas).
        download_btn:add_state(lvgl.STATE.DISABLED)

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
                -- ~0.5s/tile estimate: Core-1 pipeline with keep-alive
                eta_lbl:set({ text = format_time(uncached * 0.5) .. "  " .. size_str })
                download_btn:clear_state(lvgl.STATE.DISABLED)
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
        if pc.calc_timer then return end  -- estimate still running
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

        pc.fail_streak = 0
        pc.inflight = 0
        pc.timer = lvgl.Timer({
            period = 100,
            cb = function(t)
                poll_fetch_results()

                if #pc.queue == 0 and pc.inflight == 0 then
                    t:delete()
                    pc.timer = nil
                    show_completion()
                    return
                end

                -- Abort if WiFi dropped (3 consecutive network failures)
                if pc.fail_streak >= 3 then
                    t:delete()
                    pc.timer = nil
                    pc_progress_title:set({ text = "Download Failed" })
                    pc_counter_lbl:set({ text = pc.completed .. " / " .. pc.total .. " tiles" })
                    pc_eta_lbl:set({ text = "WiFi connection lost" })
                    return
                end

                -- Keep the Core-1 worker fed (two in flight)
                while #pc.queue > 0 and pc.inflight < 2 do
                    local tile = pc.queue[1]
                    local key = tile.z .. "/" .. tile.tx .. "/" .. tile.ty
                    if tile_cached(tile.z, tile.tx, tile.ty) or pending_fetches[key] then
                        -- already on SD, or the map view is fetching it
                        table.remove(pc.queue, 1)
                        pc.completed = pc.completed + 1
                        update_progress_ui()
                    else
                        ensure_tile_dirs(tile.z, tile.tx)
                        local url = TILE_URL .. "/" .. tile.z .. "/" .. tile.tx .. "/" .. tile.ty .. ".png"
                        if not _tile_fetch_start(url, "S:" .. tile_bin_path(tile.z, tile.tx, tile.ty), key) then
                            break  -- worker queue full — retry next tick
                        end
                        pending_fetches[key] = { kind = "pc", z = tile.z, tx = tile.tx, ty = tile.ty }
                        pc.inflight = pc.inflight + 1
                        fetch_outstanding = fetch_outstanding + 1
                        table.remove(pc.queue, 1)
                    end
                end
            end,
        })
    end)

    -- Initial estimate
    recalc_estimate()

    -- Navigation for trackball
    _nav_setup(pc_overlay, GRIDNAV_ROLLOVER)
end

-- ---------------------------------------------------------------------------
-- Settings screen
-- ---------------------------------------------------------------------------
local settings_overlay = nil

local function close_settings_screen()
    if settings_overlay then
        _nav_clear()  -- remove gridnav before delete (avoids use-after-free)
        settings_overlay:delete()
        settings_overlay = nil
        lvgl.group.focus_obj(root)
    end
end

local function show_settings_screen()
    if settings_overlay then return end

    settings_overlay = root:Object({
        w = W, h = H, x = 0, y = 0,
        bg_color = "#1a1a2e", bg_opa = 255,
        pad_all = 8, border_width = 0,
        flex = { flex_direction = "column", flex_wrap = "nowrap" },
    })
    settings_overlay:clear_flag(lvgl.FLAG.SCROLLABLE)

    settings_overlay:Label({
        text = "Map Settings",
        text_color = "#FFFFFF",
        text_font = lvgl.BUILTIN_FONT.MONTSERRAT_14,
        w = lvgl.PCT(100), h = 24,
    })

    -- Packet path animation toggle
    local function anim_toggle_text()
        return (anim.enabled and "[x]" or "[ ]") .. " Packet path animation"
    end
    local anim_btn = settings_overlay:Button({ w = W - 16, h = 32 })
    local anim_lbl = anim_btn:Label({ text = anim_toggle_text(), align = lvgl.ALIGN.LEFT_MID })
    anim_btn:onClicked(function()
        anim.enabled = not anim.enabled
        save_map_prefs(anim.enabled, show_archived)
        anim_lbl:set({ text = anim_toggle_text() })
        if not anim.enabled then
            anim.queue = {}
            if anim.active then anim_stop() end
        end
    end)

    -- Archived contacts toggle (history of contacts the mesh dropped)
    local function arch_toggle_text()
        return (show_archived and "[x]" or "[ ]") .. " Show archived contacts"
    end
    local arch_btn = settings_overlay:Button({ w = W - 16, h = 32 })
    local arch_lbl = arch_btn:Label({ text = arch_toggle_text(), align = lvgl.ALIGN.LEFT_MID })
    arch_btn:onClicked(function()
        show_archived = not show_archived
        save_map_prefs(anim.enabled, show_archived)
        arch_lbl:set({ text = arch_toggle_text() })
        redraw_markers()  -- reflect immediately
    end)

    -- Tile pre-cache download (validates SD/WiFi on its own screen)
    local dl_btn = settings_overlay:Button({ w = W - 16, h = 32 })
    dl_btn:Label({ text = "Download map tiles...", align = lvgl.ALIGN.LEFT_MID })
    dl_btn:onClicked(function()
        close_settings_screen()
        show_precache_screen()
    end)

    -- Back to map
    local back_btn = settings_overlay:Button({ w = W - 16, h = 32 })
    back_btn:Label({ text = "Close", align = lvgl.ALIGN.CENTER })
    back_btn:onClicked(function() close_settings_screen() end)

    _nav_setup(settings_overlay, GRIDNAV_ROLLOVER)
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
    pcall(function() messages:onAnyMessage(nil) end)  -- release the hub slot
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
        if settings_overlay then
            close_settings_screen()
        elseif contact_popup then
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
-- The contact popup opens on a deliberate stationary hold. LVGL's built-in
-- LONG_PRESSED fires after only 400ms even while the finger is moving (this
-- layer never scrolls, so LVGL doesn't recognize the pan), which kept
-- opening the popup mid-swipe — so hold detection is done here instead:
-- ~0.8s of press with under HOLD_MOVE_PX of finger travel.
local HOLD_TICKS = 24      -- PRESSING events (~33ms each) ≈ 0.8s hold
local HOLD_MOVE_PX = 12    -- finger travel that cancels the hold
local hold = { x = 0, y = 0, ticks = 0, moved = false, fired = false }

touch_layer:onevent(lvgl.EVENT.PRESSED, function()
    if not map.running then return end
    map.vx = 0
    map.vy = 0
    map.drag = true
    map.tooltip:add_flag(lvgl.FLAG.HIDDEN)
    local indev = lvgl.indev.get_act()
    local sx, sy = indev:get_point()
    hold.x, hold.y = sx, sy
    hold.ticks = 0
    hold.moved = false
    hold.fired = false
end)

touch_layer:onevent(lvgl.EVENT.PRESSING, function()
    if not map.running or not map.drag then return end
    local indev = lvgl.indev.get_act()
    local vx, vy = indev:get_vect()
    map.vx = -vx
    map.vy = -vy

    -- Stationary-hold detection
    local sx, sy = indev:get_point()
    if math.abs(sx - hold.x) > HOLD_MOVE_PX
       or math.abs(sy - hold.y) > HOLD_MOVE_PX then
        hold.moved = true
    end
    if not hold.moved and not hold.fired then
        hold.ticks = hold.ticks + 1
        if hold.ticks >= HOLD_TICKS then
            hold.fired = true
            map.vx = 0
            map.vy = 0
            map.drag = false
            local half_w = math.floor(W / 2)
            local half_h = math.floor(H / 2)
            local contact = find_nearest_contact(map.cx - half_w + sx,
                                                 map.cy - half_h + sy)
            if contact then
                show_contact_popup(contact)
            end
        end
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

settings_btn:onevent(lvgl.EVENT.CLICKED, function()
    if map.running then show_settings_screen() end
end)

-- ---------------------------------------------------------------------------
-- Tile download timer
-- ---------------------------------------------------------------------------

-- The Core-1 fetch worker self-manages its keep-alive connection (it closes
-- it after 10s of queue idle), so this timer only routes work: collect
-- finished fetches, keep the worker fed, and trickle-load margin tiles.
local dl_timer = lvgl.Timer({
    period = 200,
    cb = function(t)
        if not map.running then t:delete(); return end
        poll_fetch_results()
        feed_tile_fetches()
        update_dl_status()

        -- Trickle-load cached margin tiles once downloads are quiet and the
        -- map isn't moving fast. Each load is a ~50ms SD read; during a fast
        -- fling the next boundary refresh re-targets them anyway, while slow
        -- drags (≤8 px/tick) keep trickling so edges are warm when they
        -- scroll into view.
        local speed = math.max(math.abs(map.vx), math.abs(map.vy))
        if #map.download_queue == 0 and fetch_outstanding == 0
           and pc.timer == nil and speed <= 8 then
            for _ = 1, 2 do
                local m = table.remove(margin_pending, 1)
                if not m then break end
                set_tile_widget(m.idx, m.z, m.tx, m.ty)
            end
        end
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

-- Packet path animation ticker
local ANIM_TICK_MS = 30
local anim_timer = lvgl.Timer({
    period = ANIM_TICK_MS,
    cb = function(t)
        if not map.running then t:delete(); return end
        anim_tick(ANIM_TICK_MS)
    end,
})
table.insert(map.timers, anim_timer)

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

    refresh_tiles()
end

-- Subscribe to live channel traffic for the path animation. DMs are ignored
-- for now; own local-echo messages (hops 0, from = us) are skipped too.
local own_name
do
    local ok, info = pcall(_mesh_get_node_info)
    own_name = ok and info and info.name or nil
end

messages:onAnyMessage(function(msg)
    if not map.running or not anim.enabled then return end
    if msg.is_dm then return end
    if own_name and msg.from == own_name then return end
    local points = resolve_path_waypoints(msg)
    if not points then return end
    if #anim.queue >= 3 then table.remove(anim.queue, 1) end
    anim.queue[#anim.queue + 1] = { points = points }
end)

_nav_clear()
root:add_flag(lvgl.FLAG.CLICKABLE)
root:add_flag(lvgl.FLAG.CLICK_FOCUSABLE)
local group = lvgl.group.get_default()
group:add_obj(root)
lvgl.group.focus_obj(root)
init_view()
print("[Map] ready")
