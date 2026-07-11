local lvgl = require("lvgl")
local messages = require("lib/mesh/messages")
local apps = require("lib/apps")
local utils = require("lib/utils")  -- utils.now() = device RTC (os.time() isn't RTC-synced)

print("[Map] starting")

local W = lvgl.HOR_RES()
local H = lvgl.VER_RES()
local TILE_SIZE = 256
local CACHE_ROOT = "/meshpunk/map_cache"
local TILE_URL = "https://tile.openstreetmap.org"
local MIN_ZOOM = 4   -- z4 ≈ continent/region view, for taking in a large mesh
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

-- Prefs file: key=value lines (anim, arch, trail, hashes, color, hop, halo_w,
-- halo_opa, and the meshprint mp1/mp2/mptri/mpalgo/mp2nd keys)
local function load_map_prefs()
    local prefs = {
        anim = true,           -- live packet path animation on/off
        archived = false,      -- show archived contacts
        anim_color = "#ff00cc",-- packet path / dot color
        anim_hop = 500,        -- ms the dot travels per hop
        halo_w = 5,            -- dark halo width under the path (0 = off)
        halo_opa = 140,        -- halo opacity 0-255
        trail = true,          -- reveal the path hop by hop behind the dot
        hashes = true,         -- label animated waypoints with their path hash
        -- Meshprint (sender triangulation) layer:
        mp_c1 = "#00e0ff",     -- 1st-hop repeater markers (cyan)
        mp_c2 = "#ffe000",     -- 2nd-hop repeater markers (yellow)
        mp_tri = "#ff00cc",    -- triangulated sender point (magenta)
        mp_algo = 1,           -- 1=weighted centroid, 2=plain centroid, 3=geometric median
        mp_second = false,     -- also collect/show 2nd-hop repeaters
        mp_second_calc = false,-- feed 2nd-hop repeaters into triangulation + cull
        mp_cull = 3,           -- final-cull multiplier (0=off, 2/3/4×): drop outliers
    }
    local f = io.open(PREFS_PATH, "r")
    if not f then return prefs end
    local txt = f:read("*a") or ""
    f:close()
    if string.find(txt, "anim=0", 1, true) then prefs.anim = false end
    if string.find(txt, "arch=1", 1, true) then prefs.archived = true end
    if string.find(txt, "trail=0", 1, true) then prefs.trail = false end
    if string.find(txt, "hashes=0", 1, true) then prefs.hashes = false end
    if string.find(txt, "mp2nd=1", 1, true) then prefs.mp_second = true end
    if string.find(txt, "mp2calc=1", 1, true) then prefs.mp_second_calc = true end
    local c = string.match(txt, "color=(#%x%x%x%x%x%x)")
    if c then prefs.anim_color = c end
    local m1 = string.match(txt, "mp1=(#%x%x%x%x%x%x)");   if m1 then prefs.mp_c1 = m1 end
    local m2 = string.match(txt, "mp2=(#%x%x%x%x%x%x)");   if m2 then prefs.mp_c2 = m2 end
    local mt = string.match(txt, "mptri=(#%x%x%x%x%x%x)"); if mt then prefs.mp_tri = mt end
    local ma = tonumber(string.match(txt, "mpalgo=(%d+)") or "")
    if ma and ma >= 1 and ma <= 3 then prefs.mp_algo = ma end
    local mcl = tonumber(string.match(txt, "mpcull=(%d+)") or "")
    if mcl and (mcl == 0 or (mcl >= 2 and mcl <= 4)) then prefs.mp_cull = mcl end
    local hop = tonumber(string.match(txt, "hop=(%d+)") or "")
    if hop and hop >= 100 and hop <= 5000 then prefs.anim_hop = hop end
    local hw = tonumber(string.match(txt, "halo_w=(%d+)") or "")
    if hw and hw >= 0 and hw <= 12 then prefs.halo_w = hw end
    local ho = tonumber(string.match(txt, "halo_opa=(%d+)") or "")
    if ho and ho >= 0 and ho <= 255 then prefs.halo_opa = ho end
    return prefs
end

local map_prefs = load_map_prefs()

local function save_map_prefs()
    local f = io.open(PREFS_PATH, "w")
    if not f then return end
    f:write(table.concat({
        map_prefs.anim and "anim=1" or "anim=0",
        map_prefs.archived and "arch=1" or "arch=0",
        map_prefs.trail and "trail=1" or "trail=0",
        map_prefs.hashes and "hashes=1" or "hashes=0",
        "color=" .. map_prefs.anim_color,
        "hop=" .. map_prefs.anim_hop,
        "halo_w=" .. map_prefs.halo_w,
        "halo_opa=" .. map_prefs.halo_opa,
        map_prefs.mp_second and "mp2nd=1" or "mp2nd=0",
        map_prefs.mp_second_calc and "mp2calc=1" or "mp2calc=0",
        "mp1=" .. map_prefs.mp_c1,
        "mp2=" .. map_prefs.mp_c2,
        "mptri=" .. map_prefs.mp_tri,
        "mpalgo=" .. map_prefs.mp_algo,
        "mpcull=" .. map_prefs.mp_cull,
    }, "\n"))
    f:close()
end

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
    marker_ref_vl = nil,  -- view at last marker redraw (oversized canvas slide ref)
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
local bin_cache_n = 0           -- approx entry count (bounds memory growth)
local BIN_CACHE_CAP = 4000      -- panning a wide area would otherwise grow forever

local function tile_bin_path(z, tx, ty)
    return CACHE_ROOT .. "/" .. z .. "/" .. tx .. "/" .. ty .. ".bin"
end

local function tile_img_src(z, tx, ty)
    return "S:" .. tile_bin_path(z, tx, ty)
end

local function tile_cached(z, tx, ty)
    local key = z .. "/" .. tx .. "/" .. ty
    if bin_cache[key] ~= nil then return bin_cache[key] end
    -- Bound the cache: panning/precaching a wide area would otherwise add an
    -- entry per tile forever. It's only a perf cache (SD-existence memo), so
    -- wiping it just re-stats tiles as they're revisited.
    if bin_cache_n >= BIN_CACHE_CAP then bin_cache = {}; bin_cache_n = 0 end
    -- Tile conversion uses atomic write (.tmp → .bin rename), so any .bin
    -- that exists on SD is guaranteed complete. Simple existence check.
    local ok, exists = pcall(_file_exists_sd, tile_bin_path(z, tx, ty))
    local result = (ok and exists) and true or false
    bin_cache[key] = result
    bin_cache_n = bin_cache_n + 1
    return result
end

local dirs_created = {}
local dirs_created_n = 0
local function ensure_tile_dirs(z, tx)
    local key = z .. "/" .. tx
    if dirs_created[key] then return end
    if dirs_created_n >= 2000 then dirs_created = {}; dirs_created_n = 0 end  -- bound growth
    _mkdir_sd(CACHE_ROOT)
    _mkdir_sd(CACHE_ROOT .. "/" .. z)
    _mkdir_sd(CACHE_ROOT .. "/" .. z .. "/" .. tx)
    dirs_created[key] = true
    dirs_created_n = dirs_created_n + 1
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

-- Own node name — used to skip our local-echo messages in animation/replay.
local own_name
do
    local ok, info = pcall(_mesh_get_node_info)
    own_name = ok and info and info.name or nil
end

-- Path replay: persisted channel-message paths played back one at a time,
-- oldest first (fed into the animation queue by anim_tick). paused freezes
-- the playback mid-flight (transport buttons; settings auto-pauses).
local replay = { list = {}, idx = 0, total = 0, active = false, paused = false }
local stop_replay  -- forward-declare; defined with the playback engine
local update_replay_buttons  -- forward-declare; defined with the engine

-- Meshprint: sender-triangulation layer on its own screen-sized canvas.
-- reps1/reps2 are {lat,lon,pubkey,count} of the resolved 1st/2nd-hop repeaters;
-- tri is the estimated sender point. Redrawn against the current view on
-- pan/zoom (like the marker/anim canvases). Persists until explicitly cleared.
local meshprint = { active = false, reps1 = {}, reps2 = {}, tri = nil }
local redraw_meshprint_canvas  -- forward-declare; defined after redraw_markers
local clear_meshprint          -- forward-declare; defined with the engine
local run_meshprint            -- forward-declare
local show_meshprint_screen    -- forward-declare; defined after the replay screen

-- Packet path color, halo and speed live in map_prefs (user-tunable from
-- the replay screen). The dark halo is what keeps any color readable on
-- light OSM tiles — magenta default since OSM cartography never uses it.

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
    local src = tile_img_src(z, tx, ty)   -- string key for per-widget dedup
    if tile_srcs[idx] == src then
        tile_imgs[idx]:clear_flag(lvgl.FLAG.HIDDEN)
        return true
    end
    -- Load the .bin into the fixed tile pool slot `idx` and point the widget at
    -- that slot's in-memory RGB565 descriptor (LVGL draws it directly). No LVGL
    -- image cache, so no scattered 128KB decode buffers fragmenting PSRAM.
    local ok, shown = pcall(_tile_show, tile_imgs[idx], idx, tile_bin_path(z, tx, ty))
    if ok and shown then
        tile_imgs[idx]:clear_flag(lvgl.FLAG.HIDDEN)
        tile_srcs[idx] = src
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
            -- About to start a download burst (nothing in flight): the worker
            -- will decode PNGs into the shared PSRAM heap, which needs a big
            -- contiguous block. Compact the heap first so the decode has room.
            -- Once-per-burst (fetch_outstanding stays >0 while it runs).
            if fetch_outstanding == 0 then collectgarbage("collect") end
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

-- Build flat list of candidate tile coordinates (fast, no SD I/O). Each carries
-- its squared distance from the zoom's center so the estimate can hand the
-- uncached subset straight to the downloader (zoom asc, then nearest-first).
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
                    coords[#coords + 1] = { z = z, tx = tx, ty = ty, dist = dx * dx + dy * dy }
                end
            end
        end
    end
    return coords
end

-- ---------------------------------------------------------------------------
-- UI setup
-- ---------------------------------------------------------------------------

local root = apps.new_root({
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

-- Marker canvas is OVERSIZED (W+2·PAD) and SLID on pan — drawn once, then just
-- repositioned each frame, with a full redraw only when the slide nears the
-- margin. Redrawing hundreds of marker rects every frame (screen-sized) was far
-- too laggy. The anim canvas stays SCREEN-SIZED + redrawn (its path is a handful
-- of elements, cheap, and only while a packet animates). ~915KB marker (freed
-- during a meshprint) + ~307KB anim. create_base_canvases re-establishes draw
-- order tile_layer < marker < anim < (meshprint/HUD) via move_background (pushes
-- to index 0 → call in reverse order). Alloc is pcall-guarded (failure → both
-- nil, nil-guards keep the app alive with markers hidden, never crashes).
local MARKER_PAD = 100
local MCANVAS_W = W + 2 * MARKER_PAD
local MCANVAS_H = H + 2 * MARKER_PAD
local marker_canvas, anim_canvas  -- created by create_base_canvases()
local function create_base_canvases()
    if marker_canvas and anim_canvas then return end
    -- Reclaim freed PSRAM (a meshprint frees ~1.8MB) before the ~1.2MB marker+anim
    -- re-alloc, so it lands in contiguous space and leaves headroom for the draw
    -- descriptors the first repaint needs. Runs only when actually (re)creating.
    collectgarbage("collect")
    local function mk(w, h, x, y)
        local function try()
            local okc, cv = pcall(function()
                return root:Canvas({ w = w, h = h, cf = lvgl.COLOR_FORMAT.ARGB8888, x = x, y = y })
            end)
            return okc and cv or nil
        end
        local cv = try()
        if not cv then
            -- A big ARGB canvas (the ~915KB oversized marker layer especially) needs
            -- a large CONTIGUOUS block. The LVGL tile image cache fragments PSRAM
            -- below that — more so now that tiles download during a meshprint, which
            -- fills the cache (it's global, survives the app closing). Drop it (tiles
            -- transparently re-decode from their .bin) + GC, then retry once.
            pcall(_lvgl_image_cache_drop)
            collectgarbage("collect")
            cv = try()
        end
        return cv
    end
    -- Each canvas is (re)created independently: a meshprint frees ONLY the marker
    -- canvas (the anim canvas is reused as the meshprint dot layer and stays alive),
    -- so we must not clobber a surviving anim_canvas. nil-guards in the redraw paths
    -- keep the app alive if either alloc fails.
    if not marker_canvas then
        local mc = mk(MCANVAS_W, MCANVAS_H, -MARKER_PAD, -MARKER_PAD)  -- oversized
        if mc then
            marker_canvas = mc
            marker_canvas:fill_bg("#000000", 0)
            marker_canvas:clear_flag(lvgl.FLAG.CLICKABLE)
            marker_canvas:clear_flag(lvgl.FLAG.SCROLLABLE)
        else
            print("[Map] marker canvas alloc FAILED")
        end
    end
    if not anim_canvas then
        local ac = mk(W, H, 0, 0)  -- screen-sized; doubles as the meshprint dot layer
        if ac then
            anim_canvas = ac
            anim_canvas:fill_bg("#000000", 0)
            anim_canvas:clear_flag(lvgl.FLAG.CLICKABLE)
            anim_canvas:clear_flag(lvgl.FLAG.SCROLLABLE)
            anim_canvas:add_flag(lvgl.FLAG.HIDDEN)  -- hidden = skipped by renderer
        else
            print("[Map] anim canvas alloc FAILED")
        end
    end

    -- tile_layer (bottom) < marker_canvas < anim_canvas < everything above.
    if anim_canvas then pcall(_obj_move_background, anim_canvas) end
    if marker_canvas then pcall(_obj_move_background, marker_canvas) end
    pcall(_obj_move_background, tile_layer)
end
-- A meshprint HIDES (never frees) the marker canvas. The marker canvas is the big
-- ~915KB oversized layer; freeing it and re-allocating after a meshprint is exactly
-- the trap that fails — tiles/TLS/in-flight fetches move into the freed hole and a
-- clean 915KB block is no longer there. Kept allocated at its app-start address it
-- never has to move. Tiles still decode during a meshprint (they coexist with the
-- marker on first open, using OTHER free PSRAM, not the marker's block).
local function hide_marker_layer()
    if marker_canvas then pcall(function() marker_canvas:add_flag(lvgl.FLAG.HIDDEN) end) end
end
-- Show the marker canvas again after a meshprint. It stayed allocated, so this is
-- just clearing HIDDEN; only re-create if the app-start alloc had failed.
local function show_marker_layer()
    if marker_canvas then
        pcall(function() marker_canvas:clear_flag(lvgl.FLAG.HIDDEN) end)
    else
        create_base_canvases()
    end
end

-- Allocate the contiguous tile pool (one 2MB block, 16 fixed slots) FIRST, before
-- the canvases, while the heap is cleanest — the biggest contiguous request gets
-- first pick. Tiles draw from this pool instead of LVGL's scattered image cache.
collectgarbage("collect")
if not _tile_pool_alloc() then
    print("[Map] tile pool alloc FAILED - tiles will not display this session")
end
create_base_canvases()
-- Meshprint sender-triangulation dots are drawn onto the ANIM canvas (reused
-- while a meshprint is open — animation is stopped then), so a meshprint never
-- allocates a separate ~307KB layer into a fragmented heap. hide_meshprint_layer
-- clears + hides those dots from the anim canvas (on clear, or when a scan finds
-- nothing); animation re-shows/redraws the canvas on its own when it resumes.
local function hide_meshprint_layer()
    if anim_canvas then
        pcall(function() anim_canvas:fill_bg("#000000", 0) end)
        pcall(function() anim_canvas:add_flag(lvgl.FLAG.HIDDEN) end)
    end
end

-- Moving packet dot for the path animation (above both canvases, below
-- the HUD). One widget repositioned per tick — far cheaper than repainting
-- the canvas every frame.
local anim_dot = root:Object({
    w = 12, h = 12, x = -20, y = -20,
    bg_color = map_prefs.anim_color, bg_opa = 255, radius = 6,
    border_color = "#ffffff", border_width = 2,
})
anim_dot:add_flag(lvgl.FLAG.HIDDEN)
anim_dot:clear_flag(lvgl.FLAG.CLICKABLE)
anim_dot:clear_flag(lvgl.FLAG.SCROLLABLE)

-- Archived-contacts paging: the cycle handler + its button updater are defined
-- with the arch_load engine below; forward-declared here so the button can bind
-- them.
local arch_cycle, update_arch_button

-- On-map "Clear MP" button (top-right): visible only while a meshprint is on
-- screen; clears the meshprint layer. clear_meshprint is forward-declared.
local mp_clear_btn = root:Button({ w = 80, h = 28, x = W - 86, y = 24 })
mp_clear_btn:Label({ text = "Clear MP", align = lvgl.ALIGN.CENTER })
mp_clear_btn:add_flag(lvgl.FLAG.HIDDEN)
mp_clear_btn:clear_flag(lvgl.FLAG.SCROLLABLE)
mp_clear_btn:clear_flag(lvgl.FLAG.CLICK_FOCUSABLE)  -- tap-only, like the other overlay buttons (don't trap gridnav focus)
mp_clear_btn:onClicked(function() if clear_meshprint then clear_meshprint() end end)

-- On-map archive cycle button (top-right, just below the status bar): visible
-- while the archived display is on and no meshprint owns the layer. Each tap
-- loads the next ARCH_PAGE_SIZE-record window of the archive, wrapping back to
-- the first. Label shows the 1-based page; " >" = more pages, " <" = wraps to 1.
-- Shares the corner with Clear MP (y=24) — they're never visible at once (the
-- arch button hides during a meshprint).
local arch_cycle_btn = root:Button({ w = 92, h = 28, x = W - 98, y = 24 })
local arch_cycle_lbl = arch_cycle_btn:Label({ text = "Arch 1", align = lvgl.ALIGN.CENTER })
arch_cycle_btn:add_flag(lvgl.FLAG.HIDDEN)
arch_cycle_btn:clear_flag(lvgl.FLAG.SCROLLABLE)
arch_cycle_btn:clear_flag(lvgl.FLAG.CLICK_FOCUSABLE)  -- tap-only, like the other overlay buttons (don't trap gridnav focus)
arch_cycle_btn:onClicked(function() if arch_cycle then arch_cycle() end end)

-- Replay info popup (top-left corner under the status bar): who the
-- currently replayed packet is from, plus progress through the replay.
-- (Hop hashes are drawn on the map itself, next to the animated waypoints.)
local replay_box = root:Object({
    w = 170, h = 40, x = 4, y = 24,
    bg_color = "#000000", bg_opa = 200,
    border_width = 1, border_color = "#666688", radius = 4,
    pad_all = 3,
})
replay_box:clear_flag(lvgl.FLAG.SCROLLABLE)
replay_box:clear_flag(lvgl.FLAG.CLICKABLE)
-- NOTE: no label in this app sets text_font. They all inherit the screen
-- default — the emoji font with montserrat fallback — so names and message
-- text with emojis render correctly. Setting MONTSERRAT_14 explicitly is
-- what produces tofu; don't reintroduce it.
local replay_from_lbl = replay_box:Label({
    text = "",
    text_color = map_prefs.anim_color,
    align = lvgl.ALIGN.TOP_LEFT,
})
local replay_prog_lbl = replay_box:Label({
    text = "",
    text_color = "#888888",
    align = lvgl.ALIGN.BOTTOM_LEFT,
})
replay_box:add_flag(lvgl.FLAG.HIDDEN)

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
    align = lvgl.ALIGN.LEFT_MID,
})

local zoom_label = status_bar:Label({
    text = "z14",
    text_color = "#AAAAAA",
    align = lvgl.ALIGN.RIGHT_MID,
})

local dl_label = status_bar:Label({
    text = "",
    text_color = "#FFB020",
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

-- Replay transport (right of the gear; visible only while a replay runs).
-- Emoji glyphs (the FontAwesome symbols don't render through the emoji
-- font's fallback), with ASCII fallbacks when the blob lacks one —
-- same preload pattern as the topbar.
local ok_pl, has_pl = pcall(_emoji_preload, 0x25B6)  -- ▶ play
local ok_pa, has_pa = pcall(_emoji_preload, 0x23F8)  -- ⏸ pause
local ok_st, has_st = pcall(_emoji_preload, 0x23F9)  -- ⏹ stop
local GLYPH_PLAY  = (ok_pl and has_pl) and "\xE2\x96\xB6" or ">"
local GLYPH_PAUSE = (ok_pa and has_pa) and "\xE2\x8F\xB8" or "||"
local GLYPH_STOP  = (ok_st and has_st) and "\xE2\x8F\xB9" or "[]"

local rstop_btn = root:Button({ w = 36, h = 36, x = 92, y = H - 44 })
rstop_btn:Label({ text = GLYPH_STOP, align = lvgl.ALIGN.CENTER })
rstop_btn:clear_flag(lvgl.FLAG.CLICK_FOCUSABLE)
rstop_btn:add_flag(lvgl.FLAG.HIDDEN)

local rpp_btn = root:Button({ w = 36, h = 36, x = 134, y = H - 44 })
local rpp_lbl = rpp_btn:Label({ text = GLYPH_PAUSE, align = lvgl.ALIGN.CENTER })
rpp_btn:clear_flag(lvgl.FLAG.CLICK_FOCUSABLE)
rpp_btn:add_flag(lvgl.FLAG.HIDDEN)

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
    align = lvgl.ALIGN.CENTER,
})
tooltip:add_flag(lvgl.FLAG.HIDDEN)
map.tooltip = tooltip

-- ---------------------------------------------------------------------------
-- Tile rendering
-- ---------------------------------------------------------------------------

-- Vestigial. Tiles no longer go through LVGL's image cache: they are loaded into
-- the fixed contiguous tile pool (16 slots, 1:1 with the grid widgets) and drawn
-- from in-memory RGB565 descriptors. A tile that scrolls off-screen simply has its
-- slot overwritten by the next on-screen tile, so there is nothing to evict and no
-- per-frame cache churn to compact. Kept as a no-op so existing callers are unchanged.
local function evict_offscreen_tiles()
end

local redraw_markers      -- forward declaration (defined after refresh_tiles)
local redraw_anim_canvas  -- forward declaration (defined after refresh_tiles)
local update_status       -- forward declaration (defined after refresh_tiles)

-- Contact-marker projection cache. A marker's world-pixel position depends only
-- on (lat/lon, zoom) — NOT on pan — so we project every contact once and reuse
-- it across the per-frame pan redraws (a fling no longer re-projects hundreds of
-- contacts with trig every 30ms). Rebuilt only when marked dirty: zoom change,
-- contact-set change (refresh_tiles on settle/boundary, archive toggle), or the
-- meshprint restoring the layer. Own position stays live (GPS moves).
-- pts is reused in place (entries updated, count in n) so a rebuild doesn't
-- churn ~hundreds of Lua tables through the (PSRAM) heap each time.
local marker_cache = { zoom = nil, dirty = true, pts = {}, n = 0 }
local function invalidate_markers() marker_cache.dirty = true end

-- Progressive archived-marker loader. When "show archived" is on, archived
-- contacts are streamed from the disk log in batches (one per timer tick) and
-- drawn onto the marker canvas as they arrive — the live map stays interactive
-- the whole time. pts[i] = { c = <archived contact table>, px, py } (px/py
-- projected at draw_zoom; re-projected by redraw_markers on zoom). No dedup yet
-- (Noah: draw all incl. doubles for now). redraw_markers draws these in gray;
-- find_nearest also searches them so taps on a gray dot open its re-add popup.
-- Archived markers load in pages of ARCH_PAGE_SIZE records (the cycle button
-- steps through them, wrapping at the end) so the working set — Lua tables plus
-- their canvas draws — is bounded; an unbounded load starved PSRAM and
-- hard-faulted LVGL's draw path. ARCH_MIN_FREE is the largest-free-block floor
-- enforced BEFORE each read/draw batch (display) and before each archive batch
-- the meshprint streams, so no allocation is attempted without headroom.
local ARCH_PAGE_SIZE = 1000
local ARCH_MIN_FREE = 700 * 1024
local arch_load = {
    offset = 0,       -- streaming cursor (byte offset) within the current page
    page = 0,         -- current page index (0-based)
    page_start = 0,   -- byte offset where the current page begins
    next_off = 0,     -- byte offset where the next page begins (when has_next)
    read = 0,         -- records pulled into the current page so far
    has_next = false, -- more records exist on disk past this page
    pts = {},
    draw_zoom = nil,
    timer = nil,
}

local function refresh_tiles()
    if not map.running then return end
    invalidate_markers()  -- settle/boundary/zoom: refresh contact projections

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
    if anim.active then redraw_anim_canvas() end  -- re-anchor path at new view/zoom
    if meshprint.active then redraw_meshprint_canvas() end  -- reposition dots at new zoom
    update_status()
    update_dl_status()
end

-- ---------------------------------------------------------------------------
-- Markers (drawn onto canvas — 1 widget vs 262)
-- ---------------------------------------------------------------------------

-- Largest-free-block floor for a single canvas repaint. Each draw_rect/line/label
-- allocates a transient LVGL draw descriptor; if that malloc returns NULL (PSRAM
-- starved) the draw path writes to address 0 and hard-faults (StoreProhibited).
-- A meshprint frees+restores ~1.8MB, so the frames just after one can land tight.
-- Returns false -> skip this repaint (the layer keeps its last contents and
-- repaints once memory frees). Smaller than ARCH_MIN_FREE: this is one frame's
-- descriptors, not a 300-record batch. GCs once before giving up (the Lua heap
-- IS PSRAM and its incremental GC lags on churn, so a low reading is often just
-- uncollected garbage).
local DRAW_MIN_FREE = 128 * 1024
local function draw_heap_ok()
    local okh, _, largest = pcall(_heap_info)
    if okh and largest and largest < DRAW_MIN_FREE then
        collectgarbage("collect")
        okh, _, largest = pcall(_heap_info)
    end
    return (not okh) or (not largest) or largest >= DRAW_MIN_FREE
end

-- Draw the active packet path polyline onto the ANIMATION canvas, positioned
-- against the current view (screen-sized canvas; redrawn on pan/zoom).
-- Segments touching a synthesized waypoint (repeater with unknown position)
-- are dashed; real repeater hops get a small node square.
local function draw_active_path()
    if not anim.active then return end
    local a = anim.active
    local pts = a.points
    local view_left = map.cx - math.floor(W / 2)
    local view_top  = map.cy - math.floor(H / 2)
    -- Trail mode: only segments the dot has finished are drawn, so the path
    -- reveals itself hop by hop instead of appearing all at once (anim_tick
    -- triggers a canvas redraw at each hop transition).
    local reached = #pts
    if map_prefs.trail then
        reached = (a.phase == "arrive") and #pts or (a.seg or 1)
    end
    local prev_x, prev_y, prev_real
    for i, p in ipairs(pts) do
        local wx, wy = lat_lon_to_world_px(p.lat, p.lon, map.zoom)
        local cx = wx - view_left
        local cy = wy - view_top
        if prev_x and i <= reached then
            local certain = p.real and prev_real
            -- Dark halo first, bright line on top — keeps the path readable
            -- on light tiles and dark tiles alike. Width 0 disables it.
            if map_prefs.halo_w > 0 then
                anim_canvas:draw_line({
                    p1 = { x = prev_x, y = prev_y },
                    p2 = { x = cx, y = cy },
                    color = "#000000",
                    width = map_prefs.halo_w, opa = map_prefs.halo_opa,
                    dash_width = certain and 0 or 6,
                    dash_gap = certain and 0 or 5,
                    round_start = 1, round_end = 1,
                })
            end
            anim_canvas:draw_line({
                p1 = { x = prev_x, y = prev_y },
                p2 = { x = cx, y = cy },
                color = map_prefs.anim_color, width = 2, opa = 235,
                dash_width = certain and 0 or 6,
                dash_gap = certain and 0 or 5,
                round_start = 1, round_end = 1,
            })
        end
        if p.real and i > 1 and i < #pts and i <= reached then
            anim_canvas:draw_rect({
                x1 = cx - 3, y1 = cy - 3, x2 = cx + 2, y2 = cy + 2,
                bg_color = map_prefs.anim_color, bg_opa = 255, radius = 3,
                border_color = "#ffffff", border_width = 1, border_opa = 255,
            })
        end
        prev_x, prev_y, prev_real = cx, cy, p.real
    end

    -- Hash chips: label every waypoint of the active path with its repeater
    -- hash. Drawn for the WHOLE path from animation start (not gated by the
    -- trail reveal) so the route can be read before the dot travels it.
    -- Second pass so the text sits on top of the lines.
    if map_prefs.hashes then
        for _, p in ipairs(pts) do
            if p.hash then
                local wx, wy = lat_lon_to_world_px(p.lat, p.lon, map.zoom)
                local cx = wx - view_left
                local cy = wy - view_top
                local tw = 4 + #p.hash * 8
                local bx, by = cx + 6, cy - 20
                anim_canvas:draw_rect({
                    x1 = bx, y1 = by, x2 = bx + tw, y2 = by + 16,
                    bg_color = "#000000", bg_opa = 170, radius = 3,
                })
                anim_canvas:draw_label({
                    text = p.hash,
                    color = "#ffffff", opa = 255,
                    x1 = bx + 3, y1 = by + 1, x2 = bx + tw, y2 = by + 16,
                })
            end
        end
    end
end

-- Repaint the animation canvas against the current view (clears when no path).
redraw_anim_canvas = function()
    if not map.running then return end
    if not anim_canvas then return end  -- freed while a meshprint is active
    if not anim.active then
        anim_canvas:add_flag(lvgl.FLAG.HIDDEN)  -- zero render cost while idle
        return
    end
    if not draw_heap_ok() then return end  -- low PSRAM: keep last frame, don't risk a draw-descriptor NULL
    anim_canvas:clear_flag(lvgl.FLAG.HIDDEN)
    anim_canvas:set({ x = 0, y = 0 })
    anim_canvas:fill_bg("#000000", 0)
    draw_active_path()
end

redraw_markers = function()
    if not map.running then return end
    if meshprint.active then return end   -- marker layer is hidden during a meshprint
    if not marker_canvas then return end
    if not draw_heap_ok() then return end  -- low PSRAM: keep last frame, don't risk a draw-descriptor NULL

    local view_left = map.cx - math.floor(W / 2)
    local view_top  = map.cy - math.floor(H / 2)
    -- Oversized canvas: anchor it at its home offset and record the view so
    -- reposition_tiles can slide it (cheap) until the slide nears the margin.
    map.marker_ref_vl = view_left
    map.marker_ref_vt = view_top
    marker_canvas:set({ x = -MARKER_PAD, y = -MARKER_PAD })
    marker_canvas:fill_bg("#000000", 0)

    -- Rebuild the projected-position cache only when dirty or the zoom changed;
    -- panning reuses it (cheap subtraction, no trig, no _mesh_get_contacts).
    -- Entries are updated in place (only grows the array when contacts grow),
    -- so a rebuild allocates nothing in steady state. LIVE contacts only —
    -- archived are drawn separately from arch_load (progressive loader).
    if marker_cache.dirty or marker_cache.zoom ~= map.zoom then
        local pts = marker_cache.pts
        local n = 0
        local ok, contacts = pcall(_mesh_get_contacts, false)
        if ok and contacts then
            for _, c in ipairs(contacts) do
                if c.lat and c.lon and (c.lat ~= 0 or c.lon ~= 0) then
                    local px, py = lat_lon_to_world_px(c.lat, c.lon, map.zoom)
                    n = n + 1
                    local p = pts[n]
                    if p then
                        p.px, p.py, p.archived = px, py, c.archived
                    else
                        pts[n] = { px = px, py = py, archived = c.archived }
                    end
                end
            end
        end
        for i = #pts, n + 1, -1 do pts[i] = nil end  -- drop leftover tail entries
        marker_cache.n = n
        marker_cache.zoom = map.zoom
        marker_cache.dirty = false
    end

    -- Own position (live — GPS moves; a single projection is negligible)
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

    -- Live contact markers (red) from the cache. Drawn across the whole oversized
    -- canvas (the PAD margin) so they're already there when the canvas slides in.
    local pts = marker_cache.pts
    for i = 1, marker_cache.n do
        local p = pts[i]
        local cx = p.px - view_left + MARKER_PAD
        local cy = p.py - view_top + MARKER_PAD
        if cx >= -4 and cx < MCANVAS_W + 4 and cy >= -4 and cy < MCANVAS_H + 4 then
            marker_canvas:draw_rect({
                x1 = cx - 4, y1 = cy - 4, x2 = cx + 3, y2 = cy + 3,
                bg_color = "#ff6644", bg_opa = 255, radius = 4,
                border_color = "#ffffff", border_width = 1, border_opa = 255,
            })
        end
    end

    -- Archived contact markers (gray), progressively loaded from disk. Re-project
    -- on zoom change; positions stay in arch_load so pan-redraws/slides keep them.
    if show_archived and #arch_load.pts > 0 then
        if arch_load.draw_zoom ~= map.zoom then
            for _, p in ipairs(arch_load.pts) do
                p.px, p.py = lat_lon_to_world_px(p.lat, p.lon, map.zoom)
            end
            arch_load.draw_zoom = map.zoom
        end
        for _, p in ipairs(arch_load.pts) do
            local cx = p.px - view_left + MARKER_PAD
            local cy = p.py - view_top + MARKER_PAD
            if cx >= -4 and cx < MCANVAS_W + 4 and cy >= -4 and cy < MCANVAS_H + 4 then
                marker_canvas:draw_rect({
                    x1 = cx - 4, y1 = cy - 4, x2 = cx + 3, y2 = cy + 3,
                    bg_color = "#888888", bg_opa = 255, radius = 4,
                    border_color = "#ffffff", border_width = 1, border_opa = 255,
                })
            end
        end
    end
end

-- ── Progressive archived-marker loader (paged) ──────────────────────────────
local function arch_load_stop()
    if arch_load.timer then arch_load.timer:delete(); arch_load.timer = nil end
end

-- Refresh the on-map cycle button: label = 1-based page (" >" = more pages on
-- disk, " <" = next tap wraps to page 1). Shown only while the archived display
-- is on and no meshprint owns the layer.
update_arch_button = function()
    if not arch_cycle_btn then return end
    if show_archived and not meshprint.active then
        arch_cycle_lbl:set({ text = "Arch " .. (arch_load.page + 1)
                                    .. (arch_load.has_next and " >" or " <") })
        arch_cycle_btn:clear_flag(lvgl.FLAG.HIDDEN)
    else
        arch_cycle_btn:add_flag(lvgl.FLAG.HIDDEN)
    end
end

-- One batch of the current page: pre-check PSRAM, pull up to 300 archived
-- records, project + additively draw the positioned ones, advance the cursor.
-- Stops (keeping what's drawn) at the page cap (ARCH_PAGE_SIZE records), at EOF,
-- or when PSRAM headroom drops below ARCH_MIN_FREE. Stays interactive between
-- ticks and never blocks a draw.
local function arch_load_step()
    if not map.running or not show_archived then arch_load_stop(); return end

    -- Pre-check: a draw_rect allocates an LVGL draw descriptor; under a starved
    -- heap that malloc returns NULL and the draw path hard-faults
    -- (StoreProhibited). Never read/draw a batch without headroom — stop the
    -- page here and keep what's already shown.
    local okh, _, largest = pcall(_heap_info)
    if okh and largest and largest < ARCH_MIN_FREE then
        -- The Lua heap IS PSRAM and its incremental GC lags badly on churn
        -- (each page drops ~1000 lean tables). A low reading here is usually
        -- just uncollected garbage — force a full GC and re-measure before
        -- giving up, so a transient dip can't permanently wedge loading.
        collectgarbage("collect")
        okh, _, largest = pcall(_heap_info)
    end
    if okh and largest and largest < ARCH_MIN_FREE then
        print("[Map] archive page stopped at " .. #arch_load.pts
              .. " largest=" .. tostring(largest) .. " (low PSRAM)")
        arch_load_stop()
        arch_load.has_next = true   -- unknown remainder on disk; allow cycling on
        arch_load.next_off = arch_load.offset
        update_arch_button()
        return
    end

    local want = math.min(300, ARCH_PAGE_SIZE - arch_load.read)
    if want < 1 then arch_load_stop(); return end

    local ok, batch, next_off, done = pcall(_mesh_archive_read, arch_load.offset, want)
    if not ok then arch_load_stop(); return end
    if next_off then arch_load.offset = next_off end
    if type(batch) == "table" then arch_load.read = arch_load.read + #batch end

    -- Draw against the canvas's current slide reference (set by redraw_markers);
    -- fall back to the live view before the first redraw.
    local vl = map.marker_ref_vl or (map.cx - math.floor(W / 2))
    local vt = map.marker_ref_vt or (map.cy - math.floor(H / 2))
    arch_load.draw_zoom = map.zoom  -- new pts are projected at the current zoom

    if type(batch) == "table" then
        for _, c in ipairs(batch) do
            if c.lat and c.lon and (c.lat ~= 0 or c.lon ~= 0) then
                local px, py = lat_lon_to_world_px(c.lat, c.lon, map.zoom)
                -- One table per archived contact: stash px/py as fields on the lean
                -- contact table itself (no per-entry wrapper) to halve table count.
                c.px = px; c.py = py
                arch_load.pts[#arch_load.pts + 1] = c
                if marker_canvas then
                    local cx = px - vl + MARKER_PAD
                    local cy = py - vt + MARKER_PAD
                    if cx >= -4 and cx < MCANVAS_W + 4 and cy >= -4 and cy < MCANVAS_H + 4 then
                        marker_canvas:draw_rect({
                            x1 = cx - 4, y1 = cy - 4, x2 = cx + 3, y2 = cy + 3,
                            bg_color = "#888888", bg_opa = 255, radius = 4,
                            border_color = "#ffffff", border_width = 1, border_opa = 255,
                        })
                    end
                end
            end
        end
    end

    -- Page full: stop and remember whether the disk holds more beyond it.
    if arch_load.read >= ARCH_PAGE_SIZE then
        arch_load_stop()
        arch_load.has_next = not done
        arch_load.next_off = arch_load.offset
        update_arch_button()
        return
    end
    -- EOF before the cap: last page.
    if done then
        arch_load_stop()
        arch_load.has_next = false
        update_arch_button()
        return
    end
end

-- Stream the CURRENT page (from page_start), clearing the prior page's counters
-- first. Used by both the initial load and the cycle button. arch_load_step
-- must be defined above so this closure can call it.
local function arch_load_begin_page()
    arch_load_stop()
    arch_load.pts = {}
    arch_load.read = 0
    arch_load.offset = arch_load.page_start
    arch_load.draw_zoom = nil
    -- Reclaim the page we just dropped (its ~1000 lean tables are now garbage)
    -- before streaming the next one — the incremental GC won't keep up on its
    -- own, so the largest-free floor would otherwise climb across cycles.
    collectgarbage("collect")
    if not show_archived then return end
    arch_load.timer = lvgl.Timer({ period = 100, cb = function(t) arch_load_step() end })
end

-- Full reset back to page 0 (toggling "show archived" on / map start).
local function arch_load_reset()
    arch_load_stop()
    arch_load.offset = 0
    arch_load.page = 0
    arch_load.page_start = 0
    arch_load.next_off = 0
    arch_load.read = 0
    arch_load.has_next = false
    arch_load.pts = {}
    arch_load.draw_zoom = nil
end

-- (Re)start the paged load from page 0. Called when "show archived" turns on and
-- at map start if it's already on.
local function arch_load_start()
    arch_load_reset()
    if show_archived then arch_load_begin_page() end
    update_arch_button()
end

-- Advance to the next page (or wrap to the first) and stream it in, clearing the
-- prior page's gray markers.
arch_cycle = function()
    if not show_archived then return end
    if arch_load.has_next then
        arch_load.page = arch_load.page + 1
        arch_load.page_start = arch_load.next_off or 0
    else
        arch_load.page = 0
        arch_load.page_start = 0
    end
    arch_load.has_next = false
    invalidate_markers()
    arch_load_begin_page()  -- clears pts + starts streaming the new page
    redraw_markers()        -- wipe old gray dots + repaint live (new page draws on top)
    update_arch_button()
end

-- Repaint the screen-sized meshprint canvas: draw the resolved repeaters (2nd
-- hops under 1st hops) plus the triangulated sender point on top, each in its
-- configured color, positioned against the CURRENT view (no slide). Called on
-- every pan/zoom; cheap since it's only a handful of dots. Hidden when idle.
redraw_meshprint_canvas = function()
    if not map.running then return end
    if not meshprint.active then return end  -- not our layer now; clear/scan hides it
    if not anim_canvas then return end        -- anim canvas alloc failed at startup
    if not draw_heap_ok() then return end  -- low PSRAM: keep last frame, don't risk a draw-descriptor NULL
    anim_canvas:clear_flag(lvgl.FLAG.HIDDEN)
    anim_canvas:set({ x = 0, y = 0 })
    anim_canvas:fill_bg("#000000", 0)

    local view_left = map.cx - math.floor(W / 2)
    local view_top  = map.cy - math.floor(H / 2)

    local function draw_pt(lat, lon, color, size, border)
        local px, py = lat_lon_to_world_px(lat, lon, map.zoom)
        local cx = px - view_left
        local cy = py - view_top
        if cx >= -size and cx < W + size and cy >= -size and cy < H + size then
            anim_canvas:draw_rect({
                x1 = cx - size, y1 = cy - size, x2 = cx + size - 1, y2 = cy + size - 1,
                bg_color = color, bg_opa = 255, radius = size,
                border_color = "#ffffff", border_width = border, border_opa = 255,
            })
        end
    end

    for _, r in ipairs(meshprint.reps2) do draw_pt(r.lat, r.lon, map_prefs.mp_c2, 5, 1) end
    for _, r in ipairs(meshprint.reps1) do draw_pt(r.lat, r.lon, map_prefs.mp_c1, 5, 1) end
    if meshprint.tri then draw_pt(meshprint.tri.lat, meshprint.tri.lon, map_prefs.mp_tri, 8, 2) end
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
local function resolve_path_waypoints(msg, fallback_to_own)
    -- Endpoint = where WE were when the message arrived (saved per-message), not
    -- the live position. A replayed message with no saved location has NO
    -- endpoint, so the path stops at the last resolved repeater; live traffic
    -- (no saved loc) falls back to the current position.
    local end_lat, end_lon
    if msg.lat and msg.lon and (msg.lat ~= 0 or msg.lon ~= 0) then
        end_lat, end_lon = msg.lat, msg.lon
    elseif fallback_to_own then
        end_lat, end_lon = own_position()
        if not end_lat then return nil end  -- nothing to animate to
    end

    -- Live contacts only: path resolution no longer reads the archive (it's
    -- disk-only now, and a per-message file read would be far too costly). An
    -- evicted repeater simply won't anchor a hop until it re-adverts.
    local ok, contacts = pcall(_mesh_get_contacts, false)
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
        hops[n] = { cands = cands, hash = hash }
    end

    -- Pass 1: unambiguous hops
    for i = 1, n do
        if #hops[i].cands == 1 then
            local c = hops[i].cands[1]
            hops[i].lat, hops[i].lon, hops[i].real = c.lat, c.lon, true
        end
    end

    -- Next known position after hop i (resolved hop, else the endpoint, which
    -- may be nil when this message has no saved location).
    local function next_anchor(i)
        for j = i + 1, n do
            if hops[j].lat then return hops[j].lat, hops[j].lon end
        end
        return end_lat, end_lon
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
            if prev_lat and na_lat then
                ref_lat, ref_lon = (prev_lat + na_lat) / 2, (prev_lon + na_lon) / 2
            elseif prev_lat then
                ref_lat, ref_lon = prev_lat, prev_lon  -- no next anchor (no endpoint)
            elseif na_lat then
                ref_lat, ref_lon = na_lat, na_lon       -- no anchor before this hop yet
            end
            if ref_lat then
                local best, best_d
                for _, c in ipairs(h.cands) do
                    local d = geo_dist2(c.lat, c.lon, ref_lat, ref_lon)
                    if not best_d or d < best_d then best, best_d = c, d end
                end
                h.lat, h.lon, h.real = best.lat, best.lon, true
                prev_lat, prev_lon = h.lat, h.lon
            end
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
            if na_lat then
                h.lat = (prev_lat + na_lat) / 2
                h.lon = (prev_lon + na_lon) / 2
                h.real = false
                prev_lat, prev_lon = h.lat, h.lon
            end
            -- no next anchor (no endpoint, no later hop): leave unresolved
        end
        -- no previous anchor and unknown sender: hop stays unresolved (skipped)
    end

    -- Assemble: sender -> hops -> (endpoint, only if this message has one).
    -- With no endpoint the path simply stops at the last resolved repeater.
    local points = {}
    if start_lat then
        points[#points + 1] = { lat = start_lat, lon = start_lon, real = true }
    end
    for i = 1, n do
        if hops[i].lat then
            points[#points + 1] = { lat = hops[i].lat, lon = hops[i].lon,
                                    real = hops[i].real, hash = hops[i].hash }
        end
    end
    if end_lat then
        points[#points + 1] = { lat = end_lat, lon = end_lon, real = true }
    end
    if #points < 2 then return nil end
    return points
end

-- Playback: the dot eases along each segment, dwells briefly on real
-- repeater hops, then pulses out on arrival. Positions are recomputed from
-- lat/lon every tick, so panning and zooming mid-animation stay glued.
local ANIM_PAUSE_MS = 180
local ANIM_ARRIVE_MS = 350

local function anim_world_to_screen(lat, lon)
    local px, py = lat_lon_to_world_px(lat, lon, map.zoom)
    return px - (map.cx - math.floor(W / 2)), py - (map.cy - math.floor(H / 2))
end

local function anim_place_dot(lat, lon, size)
    size = size or 12
    local sx, sy = anim_world_to_screen(lat, lon)
    anim_dot:set({
        w = size, h = size, radius = math.floor(size / 2),
        x = sx - math.floor(size / 2), y = sy - math.floor(size / 2),
    })
    -- Remember where the dot sits so a paused replay stays glued to the
    -- map while panning/zooming.
    local a = anim.active
    if a then a.cur_lat, a.cur_lon = lat, lon end
end

local function anim_stop()
    anim.active = nil
    anim_dot:add_flag(lvgl.FLAG.HIDDEN)
    anim_dot:set({ w = 12, h = 12, radius = 6, bg_opa = 255 })
    redraw_anim_canvas()  -- clears the path layer (markers untouched)
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
    if a.replay then
        replay_from_lbl:set({ text = "From: " .. utils.emojiText(a.from or "?") })
        replay_prog_lbl:set({ text = a.num .. " / " .. replay.total })
        replay_box:clear_flag(lvgl.FLAG.HIDDEN)
    end
    redraw_anim_canvas()  -- draws the path polyline (markers untouched)
end

local function anim_tick(period)
    if meshprint.active then return end  -- anim canvas is freed during a meshprint
    if not anim.active then
        -- Replay feeder: when idle, pull the next replayable message
        -- (skipping ones whose path can't be resolved any more).
        if replay.active and replay.paused then return end  -- frozen: no feeding
        if replay.active and #anim.queue == 0 then
            local tries = 0
            while replay.idx < replay.total and tries < 5 do
                replay.idx = replay.idx + 1
                tries = tries + 1
                local m = replay.list[replay.idx]
                -- Replay: end at the saved receive location, or stop at the last
                -- repeater if the message has none (no current-position fallback).
                local points = resolve_path_waypoints(m, false)
                if points then
                    anim.queue[#anim.queue + 1] = {
                        points = points, replay = true,
                        from = m.from, num = replay.idx,
                    }
                    break
                end
            end
            if replay.idx >= replay.total and #anim.queue == 0 then
                stop_replay()  -- exhausted (or nothing left resolvable)
            end
        end
        -- Replay entries play even when live animations are toggled off —
        -- the user asked for them explicitly.
        if #anim.queue > 0 and (anim.enabled or anim.queue[1].replay) then
            anim_start_next()
        end
        return
    end
    local a = anim.active
    if a.replay and replay.paused then
        -- Frozen mid-flight: no time advance, but keep the dot glued to the
        -- map so panning/zooming doesn't strand it.
        if a.cur_lat then anim_place_dot(a.cur_lat, a.cur_lon) end
        return
    end
    local pts = a.points
    a.t = a.t + period

    if a.phase == "move" then
        local f = math.min(1, a.t / map_prefs.anim_hop)
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
            -- Trail mode: reveal the just-completed segment on the path
            -- layer — no marker repaint involved anymore
            if map_prefs.trail then redraw_anim_canvas() end
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
        anim_place_dot(last.lat, last.lon, 12 + math.floor(22 * f))
        anim_dot:set({ bg_opa = math.floor(255 * (1 - f)) })
        if f >= 1 then anim_stop() end
    end
end

-- Transport buttons mirror the replay state: hidden when idle, pause
-- button swaps its glyph with the paused flag.
update_replay_buttons = function()
    if replay.active then
        rstop_btn:clear_flag(lvgl.FLAG.HIDDEN)
        rpp_btn:clear_flag(lvgl.FLAG.HIDDEN)
        rpp_lbl:set({ text = replay.paused and GLYPH_PLAY or GLYPH_PAUSE })
    else
        rstop_btn:add_flag(lvgl.FLAG.HIDDEN)
        rpp_btn:add_flag(lvgl.FLAG.HIDDEN)
    end
end

stop_replay = function()
    replay.active = false
    replay.paused = false
    replay.list = {}
    replay.idx = 0
    replay.total = 0
    -- Drop queued replay animations (live ones stay), stop a playing one
    local i = 1
    while i <= #anim.queue do
        if anim.queue[i].replay then table.remove(anim.queue, i) else i = i + 1 end
    end
    if anim.active and anim.active.replay then anim_stop() end
    replay_box:add_flag(lvgl.FLAG.HIDDEN)
    update_replay_buttons()
end

-- Gather persisted channel messages from `window_secs` ago until now
-- (0 = everything), optionally only those from a sender whose name starts
-- with `name_filter`, oldest first. Returns how many will be replayed.
local REPLAY_MAX = 50  -- newest N when the window matches more

local function start_replay(window_secs, name_filter, skip_1byte)
    stop_replay()

    local cutoff = (window_secs and window_secs > 0) and (utils.now() - window_secs) or 0
    local filter = name_filter and string.lower(name_filter) or ""
    filter = filter:gsub("^%s+", ""):gsub("%s+$", "")
    if filter == "" then filter = nil end

    -- Routing store, windowed by `cutoff` (since_ts). The query matches senders
    -- exactly, so the prefix `filter` is applied here in Lua. Records are channel
    -- traffic only (no DMs) carrying { from, timestamp, lat, lon, path }.
    local list = {}
    do
        local ok, recs = pcall(_mesh_routing_query, nil, cutoff, 0)
        if ok and type(recs) == "table" then
            for _, m in ipairs(recs) do
                -- All hashes in one packet's path share a size; 1-byte
                -- hashes are 2 hex chars. Zero-hop (empty path) messages
                -- contain no 1-byte hashes, so the skip leaves them in.
                local one_byte = m.path and #m.path > 0 and #m.path[1] <= 2
                if m.from
                   and (not own_name or m.from ~= own_name)
                   and (not filter or string.lower(m.from):sub(1, #filter) == filter)
                   and not (skip_1byte and one_byte)
                then
                    list[#list + 1] = m
                end
            end
        end
    end

    table.sort(list, function(a, b) return (a.timestamp or 0) < (b.timestamp or 0) end)

    if #list > REPLAY_MAX then
        local trimmed = {}
        for i = #list - REPLAY_MAX + 1, #list do trimmed[#trimmed + 1] = list[i] end
        list = trimmed
    end

    if #list == 0 then return 0 end

    replay.list = list
    replay.total = #list
    replay.idx = 0
    replay.active = true
    replay.paused = false
    update_replay_buttons()
    return #list
end

-- ---------------------------------------------------------------------------
-- Meshprint: triangulate a sender from the first repeater of each of its msgs
-- ---------------------------------------------------------------------------
-- Tally a resolved repeater into list `into` (frequency keyed by pubkey).
local function meshprint_tally(into, c)
    for _, e in ipairs(into) do
        if e.pubkey == c.pubkey then e.count = e.count + 1; return end
    end
    into[#into + 1] = { lat = c.lat, lon = c.lon, pubkey = c.pubkey, count = 1 }
end

-- Estimate the sender from the 1st-hop repeaters. algo: 1=weighted centroid
-- (by frequency), 2=plain centroid, 3=geometric median (Weiszfeld iteration).
local function meshprint_triangulate(reps, algo)
    if #reps == 0 then return nil end
    if #reps == 1 then return { lat = reps[1].lat, lon = reps[1].lon } end
    if algo == 2 then
        local slat, slon = 0, 0
        for _, r in ipairs(reps) do slat = slat + r.lat; slon = slon + r.lon end
        return { lat = slat / #reps, lon = slon / #reps }
    end
    -- weighted centroid (also the seed for the geometric median)
    local slat, slon, sw = 0, 0, 0
    for _, r in ipairs(reps) do
        slat = slat + r.lat * r.count
        slon = slon + r.lon * r.count
        sw = sw + r.count
    end
    local cx, cy = slat / sw, slon / sw
    if algo ~= 3 then return { lat = cx, lon = cy } end
    for _ = 1, 40 do  -- Weiszfeld, count-weighted, geo_dist2 as the metric
        local nx, ny, wsum = 0, 0, 0
        for _, r in ipairs(reps) do
            local d = math.sqrt(geo_dist2(cx, cy, r.lat, r.lon))
            if d < 1e-9 then d = 1e-9 end
            local w = r.count / d
            nx = nx + r.lat * w; ny = ny + r.lon * w; wsum = wsum + w
        end
        if wsum == 0 then break end
        nx, ny = nx / wsum, ny / wsum
        if math.abs(nx - cx) < 1e-7 and math.abs(ny - cy) < 1e-7 then cx, cy = nx, ny; break end
        cx, cy = nx, ny
    end
    return { lat = cx, lon = cy }
end

-- Final cull: before triangulating, drop 1st-hop repeaters that sit way off the
-- pack — almost always a bad hash resolution (collision picked the wrong distant
-- node). For each node we compare its mean distance to the others against the
-- mean pairwise distance AMONG the others (leave-one-out, so one far outlier
-- can't inflate its own baseline). A node further than `mult`× that baseline is
-- removed. Needs ≥3 nodes to judge an outlier and never culls below 2 survivors.
-- Returns a (possibly shorter) list; the originals are left untouched.
local function meshprint_cull(reps, mult)
    local n = #reps
    if not mult or mult < 2 or n < 3 then return reps end  -- mult<2 (incl. 0/Off): no cull
    -- O(n²) pairwise sums (n = distinct repeaters, small). row[i] = Σ dist(i, j≠i);
    -- total = Σ over unordered pairs.
    local row = {}
    for i = 1, n do row[i] = 0 end
    local total = 0
    for i = 1, n - 1 do
        for j = i + 1, n do
            local d = math.sqrt(geo_dist2(reps[i].lat, reps[i].lon, reps[j].lat, reps[j].lon))
            row[i] = row[i] + d
            row[j] = row[j] + d
            total = total + d
        end
    end
    local pairs_excl = (n - 1) * (n - 2) / 2  -- pairs not involving a given node (n≥3 ⇒ ≥1)
    local kept = {}
    for i = 1, n do
        local mean_i = row[i] / (n - 1)              -- node i's mean distance to the others
        local base = (total - row[i]) / pairs_excl   -- mean pairwise distance among the others
        if base <= 0 or mean_i <= mult * base then
            kept[#kept + 1] = reps[i]
        end
    end
    if #kept >= 2 then return kept end
    return reps  -- degenerate geometry (collinear/coincident): keep all, don't void the estimate
end

-- Position-sample set for triangulation: the (culled) 1st-hop repeaters, plus the
-- 2nd-hop repeaters when BOTH "map" and "calculate" 2nd-hop toggles are on. Reads
-- the stored meshprint.reps* so the run and the live Method re-triangulate agree.
local function meshprint_tri_input()
    local r1 = meshprint.reps1 or {}
    if map_prefs.mp_second and map_prefs.mp_second_calc
       and meshprint.reps2 and #meshprint.reps2 > 0 then
        local t = {}
        for _, r in ipairs(r1) do t[#t + 1] = r end
        for _, r in ipairs(meshprint.reps2) do t[#t + 1] = r end
        return t
    end
    return r1
end

-- Run a meshprint for `node_name`. ONE batched scan over a timer (watchdog-safe
-- AND memory-flat: messages are resolved INLINE and only the tallied result is
-- kept — no per-message route storage, which previously piled up thousands of
-- tables and OOM'd). For each message the node sent, the backward cull resolves
-- its path to a single contact per hop, and the 1st (and optional 2nd) hop are
-- tallied by frequency, then triangulated. on_progress(done,total,phase);
-- on_done(#reps1); on_done(-1) = out of memory for the canvas layer.
run_meshprint = function(node_name, want_second, algo, skip_1byte, cull_mult, calc_second, on_progress, on_done)
    local nl = node_name and node_name:lower():gsub("^%s+", ""):gsub("%s+$", "") or ""
    if nl == "" then if on_done then on_done(0) end return end
    -- Live contacts only here. When "Show archived" is on, the FULL archive is
    -- streamed into the LUT below (not the 1000-cap union _mesh_get_contacts(true)
    -- returns) so every archived repeater can anchor a hop.
    local ok, contacts = pcall(_mesh_get_contacts, false)
    if not ok or not contacts then
        if on_done then on_done(0) end return
    end
    if meshprint.scan_timer then meshprint.scan_timer:delete(); meshprint.scan_timer = nil end
    collectgarbage("collect")  -- maximize free heap before building the LUT

    -- Free the big marker canvas (~915KB) for scan headroom; the map then shows
    -- tiles + meshprint dots only (the marker canvas is restored in clear_meshprint
    -- or on the failure paths below). The anim canvas is NOT freed — it's reused as
    -- the meshprint dot layer. Allocated at app start in a clean heap, it sits at a
    -- stable address, so reusing it avoids a fresh ~307KB alloc fragmenting PSRAM
    -- and starving tile decodes. Stop any animation first so the layer is free.
    if anim.active then anim_stop() end
    anim.queue = {}
    arch_load_stop()  -- suspend archived-contact loader to free PSRAM for the scan
    -- HIDE (don't free) the marker canvas. Freeing the 915KB layer and re-allocating
    -- it after a meshprint is the trap that fails; kept resident it never has to move
    -- and the scan/tiles use the rest of free PSRAM (marker + tiles coexist already).
    hide_marker_layer()
    -- Drop any previous run's dots so a stale layer doesn't show during this scan
    -- (the anim canvas is live now, unlike the old separate lazily-allocated layer).
    meshprint.reps1, meshprint.reps2, meshprint.tri = {}, {}, nil
    -- Mark active now (before the scan) so anim_tick early-returns for the whole
    -- run (the meshprint owns the anim canvas). Cleared again on any failure.
    meshprint.active = true
    update_arch_button()  -- hide the archive cycle button while the MP owns the layer
    collectgarbage("collect")

    -- Integer value of the first n hex chars of s, read via string.byte so NO
    -- substring is interned. The LUT used to key on pk:sub(1,hlen) strings, which
    -- interned ~1800 short strings for 500 contacts and overflowed Lua's global
    -- string table; its bucket array then realloc'd mid-heap and pinned there (in
    -- place on later shrink) — the PSRAM wall that halved the largest free block
    -- and starved Doom's zone. Integer keys never touch the string table.
    -- Case-insensitive; a non-hex byte (or past end of s) counts as 0.
    local function hex_prefix_val(s, n)
        local v = 0
        for i = 1, n do
            local b = s:byte(i)
            local d = 0
            if b then
                if     b >= 48 and b <= 57  then d = b - 48   -- '0'-'9'
                elseif b >= 97 and b <= 102 then d = b - 87   -- 'a'-'f'
                elseif b >= 65 and b <= 70  then d = b - 55   -- 'A'-'F'
                end
            end
            v = v * 16 + d
        end
        return v
    end

    -- prefix value -> { positioned candidate contacts }, nested by hash length
    -- (1-4 bytes = 2/4/6/8 hex chars). INTEGER keys (see hex_prefix_val) so the
    -- build doesn't grow the string table. Keeps colliding contacts so the cull
    -- can pick between them. Buckets reference the existing contact subtables (no
    -- copies — Lua keeps them alive; the C cache won't free them under us).
    local prefix_lut = { [2] = {}, [4] = {}, [6] = {}, [8] = {} }
    for _, c in ipairs(contacts) do
        if c.pubkey and c.lat and c.lon and (c.lat ~= 0 or c.lon ~= 0) then
            local pk = c.pubkey
            for _, hlen in ipairs({2, 4, 6, 8}) do
                if #pk >= hlen then
                    local v = hex_prefix_val(pk, hlen)
                    local sub = prefix_lut[hlen]
                    local bucket = sub[v]
                    if not bucket then bucket = {}; sub[v] = bucket end
                    bucket[#bucket + 1] = c
                end
            end
        end
    end

    -- Repeater-preferred candidate set for a path-hop hash (repeaters if any
    -- match, else all matches). Keyed by integer prefix value nested per hash
    -- length — no interned hash strings. Cached arrays are read-only/shared.
    local cand_cache = { [2] = {}, [4] = {}, [6] = {}, [8] = {} }
    local function cands_for(hash)
        local hlen = #hash
        local sub = prefix_lut[hlen]
        if not sub then return nil end          -- hop hash not a 1-4 byte prefix
        local v = hex_prefix_val(hash, hlen)
        local ccache = cand_cache[hlen]
        local cached = ccache[v]
        if cached ~= nil then return cached or nil end
        local bucket = sub[v]
        if not bucket then ccache[v] = false; return nil end
        local reps = nil
        for _, c in ipairs(bucket) do
            if c.type_name and c.type_name:lower():find("repeater", 1, true) then
                reps = reps or {}; reps[#reps + 1] = c
            end
        end
        local result = reps or bucket
        ccache[v] = result
        return result
    end

    -- Scan phase: resolve this node's stored messages against the LUT, tally its
    -- 1st/2nd-hop repeaters, then triangulate. Wrapped in a closure so the
    -- archive phase below can finish building the LUT first, then hand off.
    local function start_scan()
    -- Pull only this node's records from the routing store (sender-indexed) —
    -- no more scanning every channel's full history. Records carry the same
    -- { from, timestamp, lat, lon, path } shape the scan loop expects.
    local all_msgs = {}
    do
        local okm, recs = pcall(_mesh_routing_query, nl, 0, 0)  -- nl: trimmed, lowercased
        if okm and type(recs) == "table" then all_msgs = recs end
    end

    local total = #all_msgs
    if on_progress then on_progress(0, total, "scan") end

    local of_lat, of_lon = own_position()  -- fallback receiver location

    -- Backward cull for one message's path: walk from the receiver end (last
    -- hop, nearest us) toward the sender, anchor starting at our receive
    -- location. Each conflicting hop resolves to whichever candidate is closest
    -- to the anchor (the next hop toward us), then becomes the new anchor. A
    -- lone candidate is taken as-is; nothing is dropped for a conflict. Returns
    -- the resolved 1st and 2nd hop contacts (or nil). No per-call allocation.
    local function cull(path, alat, alon)
        local p1, p2
        for j = #path, 1, -1 do
            local cs = cands_for(path[j])
            local pick
            if cs and #cs > 0 then
                if #cs == 1 then
                    pick = cs[1]
                elseif alat then
                    local best, bd
                    for _, c in ipairs(cs) do
                        local d = geo_dist2(alat, alon, c.lat, c.lon)
                        if not bd or d < bd then bd = d; best = c end
                    end
                    pick = best
                else
                    pick = cs[1]  -- no anchor (no GPS at all): never drop
                end
                if pick then alat, alon = pick.lat, pick.lon end
            end
            if j == 1 then p1 = pick elseif j == 2 then p2 = pick end
        end
        return p1, p2
    end

    local reps1, reps2 = {}, {}

    local idx = 1
    local BATCH = 20
    meshprint.scan_timer = lvgl.Timer({ period = 30, cb = function(t)
        if not map.running then t:delete(); meshprint.scan_timer = nil; return end
        local stop = math.min(idx + BATCH - 1, total)
        for i = idx, stop do
            local m = all_msgs[i]
            -- this node's channel msgs only: skip DMs, pathless msgs, and (when
            -- skip_1byte is on) 1-byte-hash paths.
            if m.from and m.from:lower() == nl and not m.is_dm
               and m.path and #m.path > 0
               and not (skip_1byte and #m.path[1] <= 2) then
                local alat, alon = of_lat, of_lon
                if m.lat and m.lon and (m.lat ~= 0 or m.lon ~= 0) then
                    alat, alon = m.lat, m.lon
                end
                local p1, p2 = cull(m.path, alat, alon)
                if p1 then meshprint_tally(reps1, p1) end
                if want_second and p2 then meshprint_tally(reps2, p2) end
            end
        end
        idx = stop + 1
        if on_progress then on_progress(math.min(idx - 1, total), total, "scan") end
        if idx > total then
            t:delete(); meshprint.scan_timer = nil
            if #reps1 == 0 then
                all_msgs = nil; prefix_lut = nil; cand_cache = nil; contacts = nil
                meshprint.active = false
                hide_meshprint_layer()  -- clear/hide the anim-canvas dot layer
                collectgarbage("collect")
                show_marker_layer()     -- un-hide the (still-allocated) marker canvas
                invalidate_markers()
                redraw_markers()
                if show_archived then arch_load_start() end
                if on_done then on_done(0) end return
            end
            -- Drop everything the scan built (message list, prefix LUT, candidate
            -- cache, the contacts array) so the canvas has contiguous PSRAM.
            -- These are scan-only; reps1/reps2 hold copies of lat/lon/pubkey, so
            -- nothing we still need is referenced. cull/cands_for aren't called
            -- again past this point.
            all_msgs = nil
            prefix_lut = nil
            cand_cache = nil
            contacts = nil
            collectgarbage("collect")
            -- The meshprint dots are drawn onto the (already-alive) anim canvas, so
            -- there's no separate layer to allocate here; redraw_meshprint_canvas
            -- nil-guards a missing anim canvas.
            -- Final cull: remove outliers (bad hash resolutions) before triangulating.
            -- The culled sets also feed the displayed markers, so a node judged bogus
            -- doesn't show as a repeater either. 1st and 2nd hops are culled SEPARATELY
            -- (each against its own ring) so a legitimately-further 2nd hop isn't
            -- dropped just for being a ring out from the 1st hops.
            reps1 = meshprint_cull(reps1, cull_mult)
            if want_second and calc_second then
                reps2 = meshprint_cull(reps2, cull_mult)
            end
            meshprint.reps1 = reps1
            meshprint.reps2 = want_second and reps2 or {}
            -- Triangulate from 1st hops (+ 2nd hops when "calculate" is on).
            meshprint.tri = meshprint_triangulate(meshprint_tri_input(), algo)
            meshprint.active = true
            redraw_meshprint_canvas()
            mp_clear_btn:clear_flag(lvgl.FLAG.HIDDEN)
            -- Collect the scan's transient interned strings before reporting done.
            collectgarbage("collect")
            if on_done then on_done(#reps1) end
        end
    end })
    end  -- start_scan

    -- Fold one lean archived contact into the prefix LUT under each hash size
    -- (same integer-keyed shape the live-contacts loop builds above).
    local function fold_into_lut(c)
        local pk = c.pubkey
        for _, hlen in ipairs({2, 4, 6, 8}) do
            if #pk >= hlen then
                local v = hex_prefix_val(pk, hlen)
                local sub = prefix_lut[hlen]
                local bucket = sub[v]
                if not bucket then bucket = {}; sub[v] = bucket end
                bucket[#bucket + 1] = c
            end
        end
    end

    -- Archive phase: with "Show archived" on, stream the FULL on-disk archive
    -- into prefix_lut before scanning, so repeaters since evicted to disk can
    -- still anchor a hop. Deduped by pubkey (newest-wins), positioned entries
    -- only, batched + memory-guarded — a low-PSRAM stop just proceeds best-effort
    -- with what was gathered (never aborts the run for memory). The base canvases
    -- are freed by now (~1.2MB headroom), so this is the safe moment. The timer
    -- reuses meshprint.scan_timer so shutdown/clear tear it down too.
    if not show_archived then
        start_scan()
    else
        local seen, nseen, aoff = {}, 0, 0
        if on_progress then on_progress(0, 0, "archive") end
        meshprint.scan_timer = lvgl.Timer({ period = 20, cb = function(t)
            if not map.running then t:delete(); meshprint.scan_timer = nil; return end
            local okh, _, largest = pcall(_heap_info)
            if okh and largest and largest < ARCH_MIN_FREE then
                collectgarbage("collect")  -- drop lagged garbage before judging low
                okh, _, largest = pcall(_heap_info)
            end
            local low = okh and largest and largest < ARCH_MIN_FREE
            local batch, next_off, done
            if not low then
                local okr
                okr, batch, next_off, done = pcall(_mesh_archive_read, aoff, 300)
                if not okr then done = true end
            end
            if type(batch) == "table" then
                for _, c in ipairs(batch) do
                    if c.pubkey and c.lat and c.lon and (c.lat ~= 0 or c.lon ~= 0) then
                        local k = c.pubkey:lower()
                        if seen[k] == nil then nseen = nseen + 1 end
                        seen[k] = c   -- newest-wins (a later disk line supersedes)
                    end
                end
            end
            if next_off then aoff = next_off end
            if on_progress then on_progress(nseen, 0, "archive") end
            if low or done or type(batch) ~= "table" then
                t:delete(); meshprint.scan_timer = nil
                for _, c in pairs(seen) do fold_into_lut(c) end
                seen = nil
                collectgarbage("collect")
                start_scan()
            end
        end })
    end
end

clear_meshprint = function()
    if meshprint.scan_timer then meshprint.scan_timer:delete(); meshprint.scan_timer = nil end
    meshprint.active = false
    meshprint.reps1, meshprint.reps2, meshprint.tri = {}, {}, nil
    -- Clear + hide the meshprint dots from the (reused) anim canvas, un-hide the
    -- (still-allocated) marker canvas, and repaint the markers.
    hide_meshprint_layer()
    show_marker_layer()
    invalidate_markers()  -- contacts may have changed during the meshprint
    redraw_markers()
    if show_archived then arch_load_start() end
    mp_clear_btn:add_flag(lvgl.FLAG.HIDDEN)
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

-- Hit-test contact markers: find the nearest contact to a world-pixel position.
-- Live contacts from the mesh table + the progressively-loaded archived ones, so
-- a tap on a gray archived dot opens its popup (and "Re-add to mesh") too.
local HIT_RADIUS = 20  -- px tolerance for tap/center selection
local function find_nearest_contact(wx, wy)
    local best, best_dist = nil, HIT_RADIUS * HIT_RADIUS + 0.0
    local function consider(c)
        if c.lat and c.lon and (c.lat ~= 0 or c.lon ~= 0) then
            local px, py = lat_lon_to_world_px(c.lat, c.lon, map.zoom)
            local dx, dy = (px - wx) + 0.0, (py - wy) + 0.0  -- float to avoid int32 overflow
            local d2 = dx * dx + dy * dy
            if d2 < best_dist then best = c; best_dist = d2 end
        end
    end
    local ok, contacts = pcall(_mesh_get_contacts, false)
    if ok and contacts then
        for _, c in ipairs(contacts) do consider(c) end
    end
    if show_archived then
        for _, p in ipairs(arch_load.pts) do consider(p) end
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

    -- Sizes to content but never beyond the screen; archived contacts add
    -- rows (Status + Re-add) that overflow 240px, so the box stays
    -- scrollable and gridnav scrolls the focused button into view.
    local box = contact_popup:Object({
        w = W - 20, h = lvgl.SIZE_CONTENT,
        max_height = H - 16,
        align = lvgl.ALIGN.CENTER,
        bg_color = "#1a1a2e", radius = 8,
        border_width = 1, border_color = "#444466",
        pad_all = 10,
        flex = { flex_direction = "column", flex_wrap = "nowrap" },
    })

    local function info_row(label, value)
        local row = box:Object({
            w = W - 40, h = 18,
            bg_opa = 0, border_width = 0, pad_all = 0,
        })
        row:clear_flag(lvgl.FLAG.SCROLLABLE)
        row:Label({
            text = label,
            text_color = "#888888",
            align = lvgl.ALIGN.LEFT_MID,
        })
        row:Label({
            text = value,
            text_color = "#FFFFFF",
            align = lvgl.ALIGN.RIGHT_MID,
        })
    end

    -- Title (display-composed; identity uses raw contact.name everywhere else)
    box:Label({
        text = utils.emojiText(contact.name or "Unknown"),
        text_color = "#FFFFFF",
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
        local now = utils.now()
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

    _nav_setup(box, GRIDNAV_ROLLOVER + GRIDNAV_SCROLL_FIRST)
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
    est_tiles = nil,  -- uncached tiles collected by the (batched) estimate; the
                      -- Download reuses these so it never re-scans SD synchronously
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
    pc.est_tiles = nil
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
        w = 55, h = 30,
    })
    local area_dd = area_row:Dropdown({
        options = "Small\nMedium\nLarge\nHuge",
        w = 110, h = 30,
    })
    local area_desc = area_row:Label({
        text = "",
        text_color = "#888888",
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
        w = lvgl.PCT(100), h = 20,
    })
    local eta_lbl = pc_confirm_group:Label({
        text = "",
        text_color = "#FFFFFF",
        w = lvgl.PCT(100), h = 20,
    })
    local warn_lbl = pc_confirm_group:Label({
        text = "",
        text_color = "#FF4444",
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
        w = lvgl.PCT(100), h = 20,
    })
    pc_zoom_lbl = pc_progress_group:Label({
        text = "",
        text_color = "#AAAAAA",
        w = lvgl.PCT(100), h = 20,
    })
    pc_eta_lbl = pc_progress_group:Label({
        text = "",
        text_color = "#AAAAAA",
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

        -- Block Download until the estimate finishes: the estimate is what does
        -- the SD stat-storm safely (batched below) AND collects the uncached
        -- tile list the downloader reuses, so Download never re-scans SD itself.
        download_btn:add_state(lvgl.STATE.DISABLED)
        pc.est_tiles = nil

        -- Cancel previous calculation
        if pc.calc_timer then pcall(function() pc.calc_timer:delete() end); pc.calc_timer = nil end

        -- Build coordinate list (fast, no SD I/O)
        local coords = build_tile_coords(lat, lon, min_z, max_z, area.radius)
        local check_idx = 1
        local uncached_tiles = {}

        -- Check cache status in batches to avoid watchdog timeout; collect the
        -- uncached tiles (cheap append) so the Download can use them directly.
        pc.calc_timer = lvgl.Timer({ period = 10, cb = function(t)
            local end_idx = math.min(check_idx + CALC_BATCH - 1, #coords)
            for i = check_idx, end_idx do
                local c = coords[i]
                if not tile_cached(c.z, c.tx, c.ty) then
                    uncached_tiles[#uncached_tiles + 1] = c
                end
            end
            check_idx = end_idx + 1

            if check_idx > #coords then
                t:delete()
                pc.calc_timer = nil
                -- Download priority: zoom ascending, then nearest-first per zoom.
                table.sort(uncached_tiles, function(a, b)
                    if a.z ~= b.z then return a.z < b.z end
                    return a.dist < b.dist
                end)
                pc.est_tiles = uncached_tiles
                local uncached = #uncached_tiles
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

        -- Reuse the uncached list the estimate already gathered (batched, SD-safe)
        -- instead of re-scanning SD synchronously. Download is gated on the
        -- estimate completing, so this always matches the current selection.
        local queue = pc.est_tiles
        if not queue or #queue == 0 then
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
-- Path replay screen
-- ---------------------------------------------------------------------------
local replay_overlay = nil

local function close_replay_screen()
    if not replay_overlay then return end
    local ov = replay_overlay
    replay_overlay = nil

    -- This overlay holds textareas, and it closes from inside its own
    -- buttons' click events. Tearing down a focused textarea synchronously
    -- mid-event-chain crashed once on hardware — so: drop gridnav, move
    -- focus off the overlay, hide it now, and delete it on the next timer
    -- tick once the event chain has fully unwound.
    _nav_clear()
    lvgl.group.focus_obj(root)
    ov:add_flag(lvgl.FLAG.HIDDEN)
    lvgl.Timer({
        period = 20,
        cb = function(t)
            t:delete()
            -- pcall: the map may have shut down (root:delete) in the gap
            pcall(function() ov:delete() end)
        end,
    })
end

local function show_replay_screen()
    if replay_overlay then return end

    replay_overlay = root:Object({
        w = W, h = H, x = 0, y = 0,
        bg_color = "#1a1a2e", bg_opa = 255,
        pad_all = 8, border_width = 0,
        flex = { flex_direction = "column", flex_wrap = "nowrap" },
    })
    -- Content is taller than the screen — keep the overlay scrollable so the
    -- lower buttons are reachable (gridnav scrolls the focused one into view).

    replay_overlay:Label({
        text = "Replay Packet Paths",
        text_color = "#FFFFFF",
        w = lvgl.PCT(100), h = 24,
    })

    -- Time window: minutes back from now, 0 = entire history
    local win_row = replay_overlay:Object({
        w = W - 16, h = 34,
        bg_opa = 0, border_width = 0, pad_all = 0,
        flex = { flex_direction = "row", flex_wrap = "nowrap" },
    })
    win_row:clear_flag(lvgl.FLAG.SCROLLABLE)
    win_row:Label({
        text = "Minutes back (0=all): ",
        text_color = "#AAAAAA",
        w = 165, h = 30,
    })
    local win_ta = win_row:Textarea({
        password_mode = false, one_line = true,
        text = "60",
        w = W - 16 - 169, h = 30,
    })
    win_ta:clear_flag(lvgl.FLAG.SCROLLABLE)

    -- Sender filter
    local name_row = replay_overlay:Object({
        w = W - 16, h = 34,
        bg_opa = 0, border_width = 0, pad_all = 0,
        flex = { flex_direction = "row", flex_wrap = "nowrap" },
    })
    name_row:clear_flag(lvgl.FLAG.SCROLLABLE)
    name_row:Label({
        text = "From: ",
        text_color = "#AAAAAA",
        w = 75, h = 30,
    })
    local name_ta = name_row:Textarea({
        password_mode = false, one_line = true,
        text = "",
        w = W - 16 - 79, h = 30,
    })
    name_ta:clear_flag(lvgl.FLAG.SCROLLABLE)

    replay_overlay:Label({
        text = "Leave name empty to replay everyone",
        text_color = "#888888",
        w = lvgl.PCT(100), h = 18,
    })

    -- 1-byte path hashes collide easily (midpoint guesses); this skips
    -- messages whose path uses them, keeping only precise multi-byte paths.
    local skip_1b = false
    local function skip_1b_text()
        return (skip_1b and "[x]" or "[ ]") .. " Skip 1-byte hash paths"
    end
    local skip_btn = replay_overlay:Button({ w = W - 16, h = 32 })
    local skip_lbl = skip_btn:Label({ text = skip_1b_text(), align = lvgl.ALIGN.LEFT_MID })
    skip_btn:onClicked(function()
        skip_1b = not skip_1b
        skip_lbl:set({ text = skip_1b_text() })
    end)

    local warn_lbl = replay_overlay:Label({
        text = "",
        text_color = "#FF4444",
        w = lvgl.PCT(100), h = 18,
    })
    warn_lbl:add_flag(lvgl.FLAG.HIDDEN)

    -- Start / Stop / Close
    local start_btn = replay_overlay:Button({ w = W - 16, h = 32 })
    start_btn:Label({ text = "Start replay", align = lvgl.ALIGN.CENTER })
    start_btn:onClicked(function()
        local mins = tonumber(win_ta.text)
        if not mins or mins < 0 then
            warn_lbl:set({ text = "Enter minutes >= 0", text_color = "#FF4444" })
            warn_lbl:clear_flag(lvgl.FLAG.HIDDEN)
            return
        end
        local n = start_replay(math.floor(mins) * 60, name_ta.text, skip_1b)
        if n == 0 then
            warn_lbl:set({ text = "No matching messages", text_color = "#FF4444" })
            warn_lbl:clear_flag(lvgl.FLAG.HIDDEN)
            return
        end
        close_replay_screen()  -- back to the map to watch
    end)

    local stop_btn = replay_overlay:Button({ w = W - 16, h = 32 })
    stop_btn:Label({ text = "Stop replay", align = lvgl.ALIGN.CENTER })
    stop_btn:onClicked(function()
        stop_replay()
        close_replay_screen()
    end)

    -- ── Animation settings (shared by live + replay animations) ──
    replay_overlay:Label({
        text = "Animation",
        text_color = "#FFFFFF",
        w = lvgl.PCT(100), h = 22,
    })

    -- Color (applies + saves immediately)
    local COLOR_VALUES = { "#ff00cc", "#00e0ff", "#ffe000", "#ff8800", "#00ff66", "#ffffff" }
    local color_row = replay_overlay:Object({
        w = W - 16, h = 34,
        bg_opa = 0, border_width = 0, pad_all = 0,
        flex = { flex_direction = "row", flex_wrap = "nowrap" },
    })
    color_row:clear_flag(lvgl.FLAG.SCROLLABLE)
    color_row:Label({
        text = "Color: ",
        text_color = "#AAAAAA",
        w = 75, h = 30,
    })
    local color_dd = color_row:Dropdown({
        options = "Magenta\nCyan\nYellow\nOrange\nGreen\nWhite",
        w = 130, h = 30,
    })
    local color_sel = 1
    for i, v in ipairs(COLOR_VALUES) do
        if v == map_prefs.anim_color then color_sel = i break end
    end
    color_dd:set({ selected = color_sel - 1 })
    color_dd:onevent(lvgl.EVENT.VALUE_CHANGED, function()
        local i = color_dd:get("selected") + 1
        map_prefs.anim_color = COLOR_VALUES[i] or COLOR_VALUES[1]
        save_map_prefs()
        anim_dot:set({ bg_color = map_prefs.anim_color })
        replay_from_lbl:set({ text_color = map_prefs.anim_color })
    end)

    -- Trail mode: reveal the path hop by hop vs draw it all up front
    local function trail_toggle_text()
        return (map_prefs.trail and "[x]" or "[ ]") .. " Reveal path hop by hop"
    end
    local trail_btn = replay_overlay:Button({ w = W - 16, h = 32 })
    local trail_lbl = trail_btn:Label({ text = trail_toggle_text(), align = lvgl.ALIGN.LEFT_MID })
    trail_btn:onClicked(function()
        map_prefs.trail = not map_prefs.trail
        save_map_prefs()
        trail_lbl:set({ text = trail_toggle_text() })
    end)

    -- Hash chips on the animated waypoints
    local function hashes_toggle_text()
        return (map_prefs.hashes and "[x]" or "[ ]") .. " Show hop hashes"
    end
    local hashes_btn = replay_overlay:Button({ w = W - 16, h = 32 })
    local hashes_lbl = hashes_btn:Label({ text = hashes_toggle_text(), align = lvgl.ALIGN.LEFT_MID })
    hashes_btn:onClicked(function()
        map_prefs.hashes = not map_prefs.hashes
        save_map_prefs()
        hashes_lbl:set({ text = hashes_toggle_text() })
    end)

    -- Numeric settings (validated + saved by the Apply button)
    local function num_row(label_text, value)
        local row = replay_overlay:Object({
            w = W - 16, h = 34,
            bg_opa = 0, border_width = 0, pad_all = 0,
            flex = { flex_direction = "row", flex_wrap = "nowrap" },
        })
        row:clear_flag(lvgl.FLAG.SCROLLABLE)
        row:Label({
            text = label_text,
            text_color = "#AAAAAA",
            w = 165, h = 30,
        })
        local ta = row:Textarea({
            password_mode = false, one_line = true,
            text = tostring(value),
            w = W - 16 - 169, h = 30,
        })
        ta:clear_flag(lvgl.FLAG.SCROLLABLE)
        return ta
    end
    local hop_ta = num_row("Hop time (ms): ", map_prefs.anim_hop)
    local hw_ta  = num_row("Halo width (0=off): ", map_prefs.halo_w)
    local ho_ta  = num_row("Halo opacity: ", map_prefs.halo_opa)

    local apply_btn = replay_overlay:Button({ w = W - 16, h = 32 })
    apply_btn:Label({ text = "Apply animation settings", align = lvgl.ALIGN.CENTER })
    apply_btn:onClicked(function()
        local hop = tonumber(hop_ta.text)
        local hw  = tonumber(hw_ta.text)
        local ho  = tonumber(ho_ta.text)
        if not hop or not hw or not ho then
            warn_lbl:set({ text = "Enter numbers in all fields", text_color = "#FF4444" })
            warn_lbl:clear_flag(lvgl.FLAG.HIDDEN)
            return
        end
        map_prefs.anim_hop = math.max(100, math.min(5000, math.floor(hop)))
        map_prefs.halo_w   = math.max(0, math.min(12, math.floor(hw)))
        map_prefs.halo_opa = math.max(0, math.min(255, math.floor(ho)))
        save_map_prefs()
        -- reflect clamped values back into the fields
        hop_ta.text = tostring(map_prefs.anim_hop)
        hw_ta.text  = tostring(map_prefs.halo_w)
        ho_ta.text  = tostring(map_prefs.halo_opa)
        warn_lbl:set({ text = "Animation settings saved", text_color = "#24ba24" })
        warn_lbl:clear_flag(lvgl.FLAG.HIDDEN)
    end)

    local back_btn = replay_overlay:Button({ w = W - 16, h = 32 })
    back_btn:Label({ text = "Close", align = lvgl.ALIGN.CENTER })
    back_btn:onClicked(function() close_replay_screen() end)

    _nav_setup(replay_overlay, GRIDNAV_ROLLOVER + GRIDNAV_SCROLL_FIRST)
end

-- ---------------------------------------------------------------------------
-- Meshprint screen
-- ---------------------------------------------------------------------------
local meshprint_overlay = nil

local function close_meshprint_screen()
    if not meshprint_overlay then return end
    local ov = meshprint_overlay
    meshprint_overlay = nil
    -- Same deferred teardown as the replay screen (textareas closing from a
    -- click event): drop gridnav, move focus off, hide now, delete next tick.
    _nav_clear()
    lvgl.group.focus_obj(root)
    ov:add_flag(lvgl.FLAG.HIDDEN)
    lvgl.Timer({ period = 20, cb = function(t)
        t:delete()
        pcall(function() ov:delete() end)
    end })
end

show_meshprint_screen = function()
    if meshprint_overlay then return end
    meshprint_overlay = root:Object({
        w = W, h = H, x = 0, y = 0,
        bg_color = "#1a1a2e", bg_opa = 255,
        pad_all = 8, border_width = 0,
        flex = { flex_direction = "column", flex_wrap = "nowrap" },
    })

    meshprint_overlay:Label({
        text = "Meshprint Node", text_color = "#FFFFFF", w = lvgl.PCT(100), h = 24,
    })
    meshprint_overlay:Label({
        text = "Estimate a sender from the first repeater of each message it sent.",
        text_color = "#888888", w = lvgl.PCT(100), h = 30,
    })

    -- Find target by search (like the messenger contacts search). Type, Find,
    -- tap a match to set the target. With "Scan messages" on, after the known
    -- contacts the search also walks all stored messages (watchdog-safe batched,
    -- like the meshprint scan) for sender names in `from`, surfacing nodes you've
    -- never added as a contact.
    local selected_node = nil
    local scan_msgs = false
    local search_scan_timer = nil
    local search_ta, target_lbl, list_holder  -- assigned below; used by refresh_list

    -- Build the tappable result list from an array of names (+ optional note).
    local function render_results(names, note)
        if not meshprint_overlay then return end
        list_holder:clean()
        for _, cn in ipairs(names) do
            local b = list_holder:Button({ w = W - 16, h = 28 })
            b:Label({ text = utils.emojiText(cn), align = lvgl.ALIGN.LEFT_MID })
            b:onClicked(function()
                selected_node = cn   -- raw: feeds the routing-store scan by name
                target_lbl:set({ text = "Target: " .. utils.emojiText(cn), text_color = "#24ba24" })
            end)
        end
        if note then
            list_holder:Label({ text = note, text_color = "#888888", w = lvgl.PCT(100), h = 18 })
        elseif #names == 0 then
            list_holder:Label({ text = "No matches", text_color = "#888888", w = lvgl.PCT(100), h = 18 })
        end
        _nav_setup(meshprint_overlay, GRIDNAV_ROLLOVER + GRIDNAV_SCROLL_FIRST)
    end

    -- Batch-scan every channel's messages for sender names matching `q` that
    -- aren't already in `seen`; append unique finds to `names`, then re-render.
    -- Self-cancels if the screen closes; superseded if a new search starts.
    local function scan_message_senders(q, names, seen)
        -- Sender names come straight from the routing index: _mesh_routing_senders
        -- streams the .idx files in C (one at a time, dropped before the next) and
        -- returns only distinct matching names — no message bodies are ever pulled
        -- into RAM (the old "load every channel's full history" path was a multi-MB
        -- PSRAM spike). Deferred a tick so the "Scanning..." note paints first.
        if search_scan_timer then search_scan_timer:delete() end
        search_scan_timer = lvgl.Timer({ period = 10, cb = function(t)
            t:delete(); search_scan_timer = nil
            if not meshprint_overlay then return end
            local okm, more = pcall(_mesh_routing_senders, q, 40)
            if okm and type(more) == "table" then
                for _, nm in ipairs(more) do
                    local lk = nm:lower()
                    if not seen[lk] then
                        seen[lk] = true
                        names[#names + 1] = nm
                    end
                end
            end
            table.sort(names, function(a, b) return a:lower() < b:lower() end)
            render_results(names)
        end })
    end

    local function refresh_list()
        if search_scan_timer then search_scan_timer:delete(); search_scan_timer = nil end
        local q = (search_ta.text or ""):lower():gsub("^%s+", ""):gsub("%s+$", "")
        if q == "" then
            render_results({}, "Type a name, then Find")
            return
        end
        -- Matching contact names (live + archived) come back from C already
        -- filtered + deduped, so the full ~1500-entry union table is never built
        -- in Lua (that build was the worst single PSRAM fragmenter on a busy mesh).
        local cap = scan_msgs and 60 or 12
        local names, seen = {}, {}
        local ok, matches = pcall(_mesh_search_contact_names, q, show_archived, cap)
        if ok and type(matches) == "table" then
            for _, nm in ipairs(matches) do
                local lk = nm:lower()
                if not seen[lk] then
                    seen[lk] = true
                    names[#names + 1] = nm
                end
            end
        end        table.sort(names, function(a, b) return a:lower() < b:lower() end)
        if scan_msgs then
            render_results(names, "Scanning messages...")
            scan_message_senders(q, names, seen)
        else
            render_results(names)
        end
    end

    local search_row = meshprint_overlay:Object({
        w = W - 16, h = 34, bg_opa = 0, border_width = 0, pad_all = 0,
        flex = { flex_direction = "row", flex_wrap = "nowrap" },
    })
    search_row:clear_flag(lvgl.FLAG.SCROLLABLE)
    search_row:Label({ text = "Find: ", text_color = "#AAAAAA", w = 50, h = 30 })
    search_ta = search_row:Textarea({
        password_mode = false, one_line = true, text = "",
        placeholder_text = "node name", w = W - 16 - 104, h = 30,
    })
    search_ta:clear_flag(lvgl.FLAG.SCROLLABLE)
    local find_btn = search_row:Button({ w = 50, h = 30 })
    find_btn:Label({ text = "Find", align = lvgl.ALIGN.CENTER })

    -- Toggle: also scan message history for senders who aren't contacts.
    local function scan_text()
        return (scan_msgs and "[x]" or "[ ]") .. " Scan messages for senders"
    end
    local scan_btn = meshprint_overlay:Button({ w = W - 16, h = 30 })
    local scan_lbl = scan_btn:Label({ text = scan_text(), align = lvgl.ALIGN.LEFT_MID })
    scan_btn:onClicked(function()
        scan_msgs = not scan_msgs
        scan_lbl:set({ text = scan_text() })
    end)

    target_lbl = meshprint_overlay:Label({
        text = "Target: (none)", text_color = "#AAAAAA", w = lvgl.PCT(100), h = 18,
    })

    list_holder = meshprint_overlay:Object({
        w = W - 16, h = lvgl.SIZE_CONTENT, bg_opa = 0, border_width = 0, pad_all = 0,
        flex = { flex_direction = "column", flex_wrap = "nowrap" },
    })
    list_holder:clear_flag(lvgl.FLAG.SCROLLABLE)

    local function do_search()
        selected_node = nil
        target_lbl:set({ text = "Target: (none)", text_color = "#AAAAAA" })
        if search_scan_timer then search_scan_timer:delete(); search_scan_timer = nil end
        -- Immediate "Searching..." so the press is acknowledged; defer the real
        -- work one tick so the indicator paints before any synchronous filter.
        list_holder:clean()
        list_holder:Label({ text = "Searching...", text_color = "#cccccc", w = lvgl.PCT(100), h = 18 })
        _nav_setup(meshprint_overlay, GRIDNAV_ROLLOVER + GRIDNAV_SCROLL_FIRST)
        lvgl.Timer({ period = 10, cb = function(t)
            t:delete()
            if meshprint_overlay then refresh_list() end
        end })
    end
    find_btn:onClicked(do_search)
    search_ta:onevent(lvgl.EVENT.KEY, function()
        if lvgl.indev.get_act():get_key() == lvgl.KEY.ENTER then do_search() end
    end)

    -- Also collect/show 2nd-hop repeaters
    local second_calc_btn  -- created just below; visible only while mp_second is on
    local function second_text()
        return (map_prefs.mp_second and "[x]" or "[ ]") .. " Also map 2nd-hop repeaters"
    end
    local second_btn = meshprint_overlay:Button({ w = W - 16, h = 32 })
    local second_lbl = second_btn:Label({ text = second_text(), align = lvgl.ALIGN.LEFT_MID })
    second_btn:onClicked(function()
        map_prefs.mp_second = not map_prefs.mp_second
        save_map_prefs()
        second_lbl:set({ text = second_text() })
        -- The "calculate" toggle is only meaningful when 2nd hops are mapped.
        if map_prefs.mp_second then
            second_calc_btn:clear_flag(lvgl.FLAG.HIDDEN)
        else
            second_calc_btn:add_flag(lvgl.FLAG.HIDDEN)
        end
        _nav_setup(meshprint_overlay, GRIDNAV_ROLLOVER + GRIDNAV_SCROLL_FIRST)
    end)

    -- Feed the 2nd-hop repeaters into the triangulation + final cull (applies on
    -- the next Run). Hidden unless "Also map 2nd-hop repeaters" is on.
    local function second_calc_text()
        return (map_prefs.mp_second_calc and "[x]" or "[ ]") .. " Calculate 2nd-hop repeaters"
    end
    second_calc_btn = meshprint_overlay:Button({ w = W - 16, h = 32 })
    local second_calc_lbl = second_calc_btn:Label({ text = second_calc_text(), align = lvgl.ALIGN.LEFT_MID })
    second_calc_btn:onClicked(function()
        map_prefs.mp_second_calc = not map_prefs.mp_second_calc
        save_map_prefs()
        second_calc_lbl:set({ text = second_calc_text() })
    end)
    if not map_prefs.mp_second then second_calc_btn:add_flag(lvgl.FLAG.HIDDEN) end

    local mp_skip_1b = false
    local function skip_1b_text()
        return (mp_skip_1b and "[x]" or "[ ]") .. " Skip 1-byte hash paths"
    end
    local skip_btn = meshprint_overlay:Button({ w = W - 16, h = 32 })
    local skip_lbl = skip_btn:Label({ text = skip_1b_text(), align = lvgl.ALIGN.LEFT_MID })
    skip_btn:onClicked(function()
        mp_skip_1b = not mp_skip_1b
        skip_lbl:set({ text = skip_1b_text() })
    end)

    -- Triangulation method
    local algo_row = meshprint_overlay:Object({
        w = W - 16, h = 34, bg_opa = 0, border_width = 0, pad_all = 0,
        flex = { flex_direction = "row", flex_wrap = "nowrap" },
    })
    algo_row:clear_flag(lvgl.FLAG.SCROLLABLE)
    algo_row:Label({ text = "Method: ", text_color = "#AAAAAA", w = 75, h = 30 })
    local algo_dd = algo_row:Dropdown({
        options = "Weighted centroid\nPlain centroid\nGeometric median",
        w = 180, h = 30,
    })
    algo_dd:set({ selected = (map_prefs.mp_algo or 1) - 1 })
    algo_dd:onevent(lvgl.EVENT.VALUE_CHANGED, function()
        map_prefs.mp_algo = algo_dd:get("selected") + 1
        save_map_prefs()
        -- Re-triangulate the current result live with the new method (same sample
        -- set the run used, incl. 2nd hops when "calculate" is on).
        if meshprint.active then
            meshprint.tri = meshprint_triangulate(meshprint_tri_input(), map_prefs.mp_algo)
            redraw_meshprint_canvas()
        end
    end)

    -- Final cull: how far past the pack a 1st-hop node may sit before it's
    -- dropped as a bad hash resolution. Applies on the next Run.
    local cull_row = meshprint_overlay:Object({
        w = W - 16, h = 34, bg_opa = 0, border_width = 0, pad_all = 0,
        flex = { flex_direction = "row", flex_wrap = "nowrap" },
    })
    cull_row:clear_flag(lvgl.FLAG.SCROLLABLE)
    cull_row:Label({ text = "Final cull: ", text_color = "#AAAAAA", w = 90, h = 30 })
    local CULL_VALUES = { 0, 2, 3, 4 }  -- 0 = Off
    local cull_dd = cull_row:Dropdown({ options = "Off\n2x\n3x\n4x", w = 90, h = 30 })
    local cull_sel = 3  -- default 3x
    for i, v in ipairs(CULL_VALUES) do if v == map_prefs.mp_cull then cull_sel = i; break end end
    cull_dd:set({ selected = cull_sel - 1 })
    cull_dd:onevent(lvgl.EVENT.VALUE_CHANGED, function()
        map_prefs.mp_cull = CULL_VALUES[cull_dd:get("selected") + 1] or 3
        save_map_prefs()
    end)

    -- Color pickers (same selector style as the replay screen)
    local COLOR_VALUES = { "#ff00cc", "#00e0ff", "#ffe000", "#ff8800", "#00ff66", "#ffffff" }
    local COLOR_NAMES = "Magenta\nCyan\nYellow\nOrange\nGreen\nWhite"
    local function color_picker(label, get, set)
        local row = meshprint_overlay:Object({
            w = W - 16, h = 34, bg_opa = 0, border_width = 0, pad_all = 0,
            flex = { flex_direction = "row", flex_wrap = "nowrap" },
        })
        row:clear_flag(lvgl.FLAG.SCROLLABLE)
        row:Label({ text = label, text_color = "#AAAAAA", w = 110, h = 30 })
        local dd = row:Dropdown({ options = COLOR_NAMES, w = 120, h = 30 })
        local sel = 1
        for i, v in ipairs(COLOR_VALUES) do if v == get() then sel = i break end end
        dd:set({ selected = sel - 1 })
        dd:onevent(lvgl.EVENT.VALUE_CHANGED, function()
            set(COLOR_VALUES[dd:get("selected") + 1] or COLOR_VALUES[1])
            save_map_prefs()
            if meshprint.active then redraw_meshprint_canvas() end
        end)
    end
    color_picker("1st repeater: ", function() return map_prefs.mp_c1 end,
                                   function(v) map_prefs.mp_c1 = v end)
    color_picker("2nd repeater: ", function() return map_prefs.mp_c2 end,
                                   function(v) map_prefs.mp_c2 = v end)
    color_picker("Sender point: ", function() return map_prefs.mp_tri end,
                                   function(v) map_prefs.mp_tri = v end)

    local warn_lbl = meshprint_overlay:Label({
        text = "", text_color = "#FF4444", w = lvgl.PCT(100), h = 18,
    })
    warn_lbl:add_flag(lvgl.FLAG.HIDDEN)

    -- Run
    local run_btn = meshprint_overlay:Button({ w = W - 16, h = 32 })
    run_btn:Label({ text = "Run meshprint", align = lvgl.ALIGN.CENTER })
    run_btn:onClicked(function()
        local name = selected_node or search_ta.text or ""
        if name:gsub("%s+", "") == "" then
            warn_lbl:set({ text = "Search and pick a node", text_color = "#FF4444" })
            warn_lbl:clear_flag(lvgl.FLAG.HIDDEN)
            return
        end
        -- Show "Searching..." immediately, then defer the heavy run_meshprint
        -- setup (free canvases, LUT build, message gather) one tick so the label
        -- paints before the synchronous work blocks the UI.
        warn_lbl:set({ text = "Searching...", text_color = "#AAAAAA" })
        warn_lbl:clear_flag(lvgl.FLAG.HIDDEN)
        lvgl.Timer({ period = 10, cb = function(t)
            t:delete()
            if not meshprint_overlay then return end
            run_meshprint(name, map_prefs.mp_second, map_prefs.mp_algo, mp_skip_1b, map_prefs.mp_cull,
                map_prefs.mp_second_calc,
                function(done, total, phase)
                    if not meshprint_overlay then return end  -- screen closed mid-scan
                    if phase == "archive" then
                        warn_lbl:set({ text = "Loading archive " .. done .. "...",
                                       text_color = "#AAAAAA" })
                    else
                        warn_lbl:set({ text = "Scanning " .. done .. "/" .. total .. "...",
                                       text_color = "#AAAAAA" })
                    end
                end,
                function(n)
                    if not meshprint_overlay then return end  -- screen closed mid-scan
                    if n == -1 then
                        warn_lbl:set({ text = "Out of memory for meshprint layer",
                                       text_color = "#FF4444" })
                    elseif n == 0 then
                        warn_lbl:set({ text = "No positioned first-hop repeaters found",
                                       text_color = "#FF4444" })
                    else
                        close_meshprint_screen()
                    end
                end)
        end })
    end)

    -- Clear the meshprint layer
    local clear_btn = meshprint_overlay:Button({ w = W - 16, h = 32 })
    clear_btn:Label({ text = "Clear meshprint", align = lvgl.ALIGN.CENTER })
    clear_btn:onClicked(function()
        clear_meshprint()
        close_meshprint_screen()
    end)

    -- Close
    local back_btn = meshprint_overlay:Button({ w = W - 16, h = 32 })
    back_btn:Label({ text = "Close", align = lvgl.ALIGN.CENTER })
    back_btn:onClicked(function() close_meshprint_screen() end)

    _nav_setup(meshprint_overlay, GRIDNAV_ROLLOVER + GRIDNAV_SCROLL_FIRST)
end

-- ---------------------------------------------------------------------------
-- Settings screen
-- ---------------------------------------------------------------------------
-- ---------------------------------------------------------------------------
-- Controls / help screen (text mirrors the README's "Map App" section)
-- ---------------------------------------------------------------------------
local help_overlay = nil

local function close_map_help()
    if help_overlay then
        _nav_clear()  -- remove gridnav before delete (avoids use-after-free)
        help_overlay:delete()
        help_overlay = nil
        lvgl.group.focus_obj(root)
    end
end

local function show_map_help()
    if help_overlay then return end

    help_overlay = root:Object({
        w = W, h = H, x = 0, y = 0,
        bg_color = "#1a1a2e", bg_opa = 255,
        pad_all = 8, border_width = 0,
        flex = { flex_direction = "column", flex_wrap = "nowrap" },
    })

    -- Close first so gridnav focuses it at the top (the screen opens scrolled
    -- to the top); the help text below scrolls with touch.
    local back_btn = help_overlay:Button({ w = W - 16, h = 30 })
    back_btn:Label({ text = "Close", align = lvgl.ALIGN.CENTER })
    back_btn:onClicked(function() close_map_help() end)

    help_overlay:Label({
        text = "Map Controls", text_color = "#FFFFFF", w = lvgl.PCT(100), h = 22,
    })

    local function section(title, body)
        help_overlay:Label({ text = title, text_color = "#88AAFF", w = lvgl.PCT(100) })
        help_overlay:Label({ text = body, text_color = "#CCCCCC", w = lvgl.PCT(100) })
    end

    section("Keys",
        "h  -  center on home (own GPS)\n" ..
        "o / +  -  zoom in\n" ..
        "i / -  -  zoom out\n" ..
        "Space  -  stop scrolling\n" ..
        "Enter  -  select contact / stop scrolling\n" ..
        "c  -  cycle archived-contact page\n" ..
        "q  -  quit (closes a popup first)\n" ..
        "Trackball  -  pan the map")

    section("Contacts",
        "Long-press a marker (touch), or center the trackball on it and " ..
        "press Enter, to see its name, type, distance, hop count and last seen.")

    section("Meshprint",
        "Run a meshprint on a message's sender to capture its 1st/2nd-hop " ..
        "repeaters and triangulate the sender's rough location. The more mesh " ..
        "data you have, the better the result.")

    section("Offline tiles",
        "In Settings, download map tiles for offline use - pick an area size " ..
        "and zoom range. Tiles are cached to the SD card.")

    -- 'q' / ESC closes the help. While this overlay is the gridnav scope, root
    -- (which owns the map's key handler) isn't focused — so handle the key on
    -- the scope itself, the same idiom the messenger's nav.list uses.
    help_overlay:onevent(lvgl.EVENT.KEY, function()
        local k = lvgl.indev.get_act():get_key()
        if k == 113 or k == 27 then close_map_help() end
    end)
    _nav_setup(help_overlay, GRIDNAV_ROLLOVER)
end

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

    -- A running replay auto-pauses while the settings cover the map
    -- (stays paused on return — resume with the play button).
    if replay.active and not replay.paused then
        replay.paused = true
        update_replay_buttons()
    end

    settings_overlay = root:Object({
        w = W, h = H, x = 0, y = 0,
        bg_color = "#1a1a2e", bg_opa = 255,
        pad_all = 8, border_width = 0,
        flex = { flex_direction = "column", flex_wrap = "nowrap" },
    })

    settings_overlay:Label({
        text = "Map Settings",
        text_color = "#FFFFFF",
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
        map_prefs.anim = anim.enabled
        save_map_prefs()
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
        map_prefs.archived = show_archived
        save_map_prefs()
        arch_lbl:set({ text = arch_toggle_text() })
        if show_archived then
            arch_load_start()       -- begin streaming archived markers from disk
        else
            arch_load_reset()       -- drop them + stop the loader
        end
        update_arch_button()        -- show/hide the cycle button to match
        invalidate_markers()
        redraw_markers()  -- reflect immediately (archived fill in progressively)
    end)

    -- Replay packet paths from message history
    local replay_btn = settings_overlay:Button({ w = W - 16, h = 32 })
    replay_btn:Label({ text = "Replay packet paths...", align = lvgl.ALIGN.LEFT_MID })
    replay_btn:onClicked(function()
        close_settings_screen()
        show_replay_screen()
    end)

    -- Meshprint: triangulate a sender from its messages' first repeaters
    local meshprint_btn = settings_overlay:Button({ w = W - 16, h = 32 })
    meshprint_btn:Label({ text = "Meshprint node...", align = lvgl.ALIGN.LEFT_MID })
    meshprint_btn:onClicked(function()
        close_settings_screen()
        show_meshprint_screen()
    end)

    -- Tile pre-cache download (validates SD/WiFi on its own screen)
    local dl_btn = settings_overlay:Button({ w = W - 16, h = 32 })
    dl_btn:Label({ text = "Download map tiles...", align = lvgl.ALIGN.LEFT_MID })
    dl_btn:onClicked(function()
        close_settings_screen()
        show_precache_screen()
    end)

    -- Controls / help (info mirrors the README)
    local help_btn = settings_overlay:Button({ w = W - 16, h = 32 })
    help_btn:Label({ text = "Controls / help...", align = lvgl.ALIGN.LEFT_MID })
    help_btn:onClicked(function()
        close_settings_screen()
        show_map_help()
    end)

    -- Back to map
    local back_btn = settings_overlay:Button({ w = W - 16, h = 32 })
    back_btn:Label({ text = "Close", align = lvgl.ALIGN.CENTER })
    back_btn:onClicked(function() close_settings_screen() end)

    -- 'q' / ESC closes the menu (the gridnav scope owns the keys while it's
    -- open, so root's key handler can't see them).
    settings_overlay:onevent(lvgl.EVENT.KEY, function()
        local k = lvgl.indev.get_act():get_key()
        if k == 113 or k == 27 then close_settings_screen() end
    end)
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
    -- Slide the oversized marker canvas (cheap) and only redraw when the slide
    -- nears its margin — this is what keeps panning smooth with many markers.
    -- (nil while a meshprint has it freed.)
    if marker_canvas and map.marker_ref_vl then
        local dx = map.marker_ref_vl - view_left
        local dy = map.marker_ref_vt - view_top
        marker_canvas:set({ x = dx - MARKER_PAD, y = dy - MARKER_PAD })
        if math.abs(dx) > MARKER_PAD - 20 or math.abs(dy) > MARKER_PAD - 20 then
            redraw_markers()
        end
    end
    -- anim/meshprint are screen-sized (few elements): redraw against the new view.
    if anim.active and anim_canvas then redraw_anim_canvas() end
    if meshprint.active and anim_canvas then redraw_meshprint_canvas() end
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
    map.running = false                               -- stops timer-tick work
    arch_load_stop()                                  -- untracked timer — stop explicitly
    -- The meshprint scan timer is untracked and can outlive a closed MP screen
    -- (it keeps running to draw on the map). Stop it here or it fires into the
    -- freed root after apps.go_home() deletes everything.
    if meshprint.scan_timer then
        meshprint.scan_timer:delete()
        meshprint.scan_timer = nil
    end
    pcall(function() messages:onAnyMessage(nil) end)  -- release the hub slot
    -- Free the contiguous 2MB tile pool. HIDE the tile widgets first so nothing
    -- draws from the pool memory between the free and go_home's (deferred) widget
    -- deletion. _tile_pool_free also drops the image cache (icons re-decode).
    for _, img in ipairs(tile_imgs) do
        pcall(function() img:add_flag(lvgl.FLAG.HIDDEN) end)
    end
    pcall(_tile_pool_free)
    -- Release the cached 500-contact Lua table (~388KB, pinned in the registry by
    -- _mesh_get_contacts). The Map is one of only two consumers; dropping it on
    -- close stops it lingering and fragmenting the heap for the next heavy app.
    -- go_home's collectgarbage reclaims it. Rebuilt on demand next time it's used.
    pcall(_mesh_drop_contacts_cache)
    -- Tear down the Core-1 tile worker: close its keep-alive TLS session AND free
    -- its 16KB INTERNAL stack (the task self-deletes). That 16KB is what drops the
    -- largest contiguous internal block below what a heavy app (Doom) needs for
    -- its own task stack right after the Map. Recreated on the next tile fetch.
    pcall(_tile_fetch_close)
    apps.go_home()   -- manager: _nav_clear, delete tracked timers, then the root
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
        if help_overlay then
            close_map_help()
        elseif replay_overlay then
            close_replay_screen()
        elseif settings_overlay then
            close_settings_screen()
        elseif meshprint_overlay then
            close_meshprint_screen()  -- close the overlay, NOT the app (scan keeps drawing)
        elseif pc_overlay then
            hide_precache_screen()    -- close the overlay (also stops its dl/calc timers)
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
    elseif key == 99 then -- c: cycle the archive page (same as the on-map button)
        if show_archived and not meshprint.active and arch_cycle then arch_cycle() end
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

-- Replay transport
rstop_btn:onevent(lvgl.EVENT.CLICKED, function()
    if map.running then stop_replay() end
end)
rpp_btn:onevent(lvgl.EVENT.CLICKED, function()
    if map.running and replay.active then
        replay.paused = not replay.paused
        update_replay_buttons()
    end
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
apps.track_timer(dl_timer)

-- Periodic WiFi status check (every 3s)
local wifi_timer = lvgl.Timer({
    period = 3000,
    cb = function(t)
        if not map.running then t:delete(); return end
        update_wifi_status()
    end,
})
apps.track_timer(wifi_timer)

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
apps.track_timer(momentum_timer)

-- Auto-hide tooltip after 3 seconds
local tooltip_timer = lvgl.Timer({
    period = 3000,
    cb = function(t)
        if not map.running then t:delete(); return end
        map.tooltip:add_flag(lvgl.FLAG.HIDDEN)
    end,
})
apps.track_timer(tooltip_timer)

-- Packet path animation ticker
local ANIM_TICK_MS = 30
local anim_timer = lvgl.Timer({
    period = ANIM_TICK_MS,
    cb = function(t)
        if not map.running then t:delete(); return end
        anim_tick(ANIM_TICK_MS)
    end,
})
apps.track_timer(anim_timer)

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

    -- Tiles need WiFi and the firmware no longer retries in the background
    -- (bounded connect rounds): kick one round now — async no-op when already
    -- connected/disabled — and update_wifi_status flips wifi_ok when it lands.
    pcall(_wifi_auto_connect)
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
messages:onAnyMessage(function(msg)
    if not map.running or not anim.enabled then return end
    if msg.is_dm then return end
    if own_name and msg.from == own_name then return end
    -- Resolving a path per message (a full _mesh_get_contacts fetch + per-hop
    -- iteration) is the dominant Lua-heap garbage source on a busy mesh. Only
    -- resolve when the animator is idle: paths still animate one at a time, but a
    -- message storm can't churn the (PSRAM-backed) heap into an OOM.
    if anim.active or #anim.queue > 0 then return end
    -- Live traffic: end at the saved location if present, else the current
    -- position (the message is arriving here, now).
    local points = resolve_path_waypoints(msg, true)
    if not points then return end
    anim.queue[#anim.queue + 1] = { points = points }
end)

_nav_clear()
root:add_flag(lvgl.FLAG.CLICKABLE)
root:add_flag(lvgl.FLAG.CLICK_FOCUSABLE)
local group = lvgl.group.get_default()
group:add_obj(root)
lvgl.group.focus_obj(root)
init_view()
if show_archived then arch_load_start() end  -- stream archived markers if enabled
print("[Map] ready")
