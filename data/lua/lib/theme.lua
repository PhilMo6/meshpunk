--[[
  Theme manager for MeshPunk.

  A theme returns { name, apply(t) }:

      return {
        name  = "Hotpink Punk",
        apply = function(t)  -- runs ONCE when the theme is selected
          t.set_palette{ scr=.., card=.., text=.., grey=.., accent=.., btn_text=.., dark=true }
          t.background.procedural(function(canvas, w, h) ... end)  -- or .image / .fill
          t.set_font(t.dir .. "/font.ttf")            -- optional runtime UI font
          -- or: t.set_font{ file = t.dir .. "/font.ttf", size = 16 }
        end,
      }

  Fonts: two roles. "ui" is the interface font everything inherits; "text" is
  the reading font used by objects that opted in (text_font =
  lvgl.Font("text", 16) — e.g. Messenger chat bubbles; 16 = the standard UI
  size, selecting the role's chain head). set_font with a string
  sets the ui role only; a table sets either/both: { ui = path, text = path }
  (each also accepts { file=, size= }). Per role: the theme's font wins while
  the theme is active; otherwise the user's default from Settings > Fonts
  applies (factory: bundled Noto Sans, wide Latin/Cyrillic/Greek coverage).
  Emoji rendering and FontAwesome symbols are unaffected — they sit above/
  below the TTF in the font chain. .ttf files with glyf outlines ONLY —
  CFF-flavored .otf is rejected (theme still applies, font stays default).
  Apps can request role fonts at other sizes: pcall(lvgl.Font, "text", 22).

  A theme can live two ways inside a themes folder, both valid:
    * a flat file   <id>.lua
    * a folder      <id>/theme.lua   (tidier for image-heavy themes — bundle the
                                      wallpaper images beside theme.lua)
  Both are scanned in two locations:
    * internal : /lua/themes
    * SD card  : /meshpunk/themes
  An internal theme wins over an SD theme of the same id, and a flat file wins
  over a folder of the same id.

  A folder theme gets its own directory via t.dir (LVGL drive-prefixed, e.g.
  "L:/lua/themes/<id>" or "S:/meshpunk/themes/<id>"), so it can load bundled
  images with t.background.image(t.dir .. "/wall.bin"). Flat themes typically use
  procedural/solid backgrounds or an absolute image path.

  Because apply() is plain Lua it can compute colors at run time — branch on the
  month (t.month()), randomize, etc. — so "the theme is the whole file".

  Theme modules are run FULLY TRANSIENTLY: internal ones are required then dropped
  from package.loaded; SD ones run via _dofile_sd (never cached). Nothing a theme
  allocates (palette tables, draw closures) stays resident; only the small
  selected record is kept (so ensure_background doesn't rescan the filesystem).

  Split of responsibilities:
    * palette  -> pushed to the C LVGL theme (_theme_apply_palette). Lives in the
                  theme styles, survives app launches, costs no PSRAM.
    * background-> drawn by lib/background. Freed on every app launch (so heavy
                  apps keep the PSRAM); the launcher redraws it on return home,
                  and any lightweight app may opt in via M.show_background().
]]

local lvgl = require("lvgl")
local background = require("lib/background")
local utils = require("lib/utils")

local M = {}

local INTERNAL_DIR = "/lua/themes"
local SD_DIR = "/meshpunk/themes"
local DEFAULT_ID = "default"

local current_id = nil    -- id of the last applied theme
local current_rec = nil   -- its resolved record, so ensure_background needn't rescan

-- ── Helpers handed to themes via the toolkit ────────────────────────────────

-- HSV (h 0-360, s/v 0-1) -> "#rrggbb". Convenient for rainbow / procedural fills.
local function hsv(h, s, v)
    h = (h % 360 + 360) % 360
    local c = v * s
    local x = c * (1 - math.abs((h / 60) % 2 - 1))
    local m = v - c
    local r, g, b = 0, 0, 0
    if     h < 60  then r, g, b = c, x, 0
    elseif h < 120 then r, g, b = x, c, 0
    elseif h < 180 then r, g, b = 0, c, x
    elseif h < 240 then r, g, b = 0, x, c
    elseif h < 300 then r, g, b = x, 0, c
    else                r, g, b = c, 0, x end
    return string.format("#%02x%02x%02x",
        math.floor((r + m) * 255 + 0.5),
        math.floor((g + m) * 255 + 0.5),
        math.floor((b + m) * 255 + 0.5))
end

-- Local epoch (device RX/GPS clock + tz offset). Mirrors lib/topbar.
local function local_now()
    local ok, ts = pcall(_rtc_time)
    local epoch = (ok and ts) or 0
    local ok2, off = pcall(_rtc_tz_offset_minutes)
    local off_min = (ok2 and off) or 0
    return epoch + off_min * 60
end

-- Civil (y, m, d) from an epoch, via Howard Hinnant's algorithm (same as topbar).
local function civil_from_epoch(ts)
    local days = math.floor(ts / 86400) + 719468
    local era = math.floor(days / 146097)
    local doe = days - era * 146097
    local yoe = math.floor((doe - math.floor(doe / 1460) + math.floor(doe / 36524) - math.floor(doe / 146096)) / 365)
    local y = yoe + era * 400
    local doy = doe - (365 * yoe + math.floor(yoe / 4) - math.floor(yoe / 100))
    local mp = math.floor((5 * doy + 2) / 153)
    local d = doy - math.floor((153 * mp + 2) / 5) + 1
    local m = mp + (mp < 10 and 3 or -9)
    if m <= 2 then y = y + 1 end
    return y, m, d
end

-- Build the toolkit passed to a theme's apply().
local function make_toolkit(asset_dir)
    return {
        w = lvgl.HOR_RES(),
        h = lvgl.VER_RES(),
        dir = asset_dir,   -- this theme's own folder (drive-prefixed) for bundled
                           -- images; nil-safe to use as a prefix only when set
        hsv = hsv,
        now = local_now,
        -- Current month 1-12 (local clock). 0 if the clock isn't set yet.
        month = function()
            local ts = local_now()
            if ts < 1 then return 0 end
            local _, m = civil_from_epoch(ts)
            return m
        end,
        -- Push the chrome palette to the C theme (live, no reboot). Missing keys
        -- fall back to the current dark defaults; btn_text falls back to text.
        set_palette = function(p)
            p = p or {}
            _theme_apply_palette(
                p.scr or "#15171A",
                p.card or "#282b30",
                p.text or "#e6e6e6",
                p.grey or "#2f3237",
                p.accent or "#ff00aa",
                p.btn_text or p.text or "#ffffff",
                p.dark ~= false)   -- default dark unless explicitly dark = false
        end,
        -- Optional runtime fonts (TTF), two roles: "ui" (interface chrome —
        -- what everything inherits) and "text" (reading content — objects
        -- that opted in with text_font = lvgl.Font("text", 16), e.g. chat
        -- bubbles). A string sets the ui role only; a table sets either:
        --   t.set_font(t.dir .. "/font.ttf")                       -- ui only
        --   t.set_font{ ui = "...ttf", text = { file = "...ttf", size = 16 } }
        -- Unset roles keep the user/bundled default. Idempotent C-side
        -- (show_background re-runs apply()); pcall-guarded so themes still
        -- load on firmware without TTF support. Failure keeps current fonts.
        set_font = function(spec)
            if type(spec) == "string" then spec = { ui = spec } end
            if type(spec) ~= "table" then return end
            if spec.file then spec = { ui = spec } end   -- old {file=,size=} form
            local function one(role, v)
                if type(v) == "string" then v = { file = v } end
                if type(v) ~= "table" or not v.file then return end
                pcall(_theme_font_set, role, v.file, v.size or 0)
            end
            one("ui", spec.ui)
            one("text", spec.text)
        end,
        -- One-shot background helpers (lib/background). Draw whenever the theme's
        -- apply() runs — at home, and inside any app that opts in by calling
        -- theme.show_background(). Heavy apps simply never ask, and their launch
        -- already freed the surface, so they keep the PSRAM.
        background = {
            fill       = function(color)   background.fill(color)        end,
            image      = function(src)     background.image(src)         end,
            procedural = function(draw_fn) background.procedural(draw_fn) end,
        },
    }
end

-- ── Discovery / loading ─────────────────────────────────────────────────────

-- LVGL drive-letter prefix for image paths (lv_conf): internal = L, SD = S.
local function drive(source) return source == "sd" and "S:" or "L:" end

-- Record a discovered theme. is_folder => "<id>/theme.lua" (assets live beside
-- it); otherwise a flat "<id>.lua". First seen wins, so callers add the
-- preferred sources/kinds first.
local function add_rec(seen, out, source, dir, id, is_folder)
    if not id or id == "" or seen[id] then return end
    local rec = { id = id, source = source, is_folder = is_folder }
    if is_folder then
        rec.asset_dir = drive(source) .. dir .. "/" .. id
        if source == "sd" then rec.sd_path = dir .. "/" .. id .. "/theme.lua"
        else                   rec.require_path = "themes/" .. id .. "/theme" end
    else
        rec.asset_dir = drive(source) .. dir
        if source == "sd" then rec.sd_path = dir .. "/" .. id .. ".lua"
        else                   rec.require_path = "themes/" .. id end
    end
    seen[id] = rec
    out[#out + 1] = rec
end

local function folder_has_theme(source, dir, id)
    local sub = dir .. "/" .. id .. "/theme.lua"
    if source == "sd" then
        local ok, present = pcall(_file_exists_sd, sub)
        return ok and present
    end
    return utils.file_exists(sub)
end

-- Scan one location's entries (from _list_all / _list_all_sd): flat *.lua files
-- and <id>/theme.lua folders. Files are taken before folders so a flat file
-- wins over a same-named folder.
local function collect(entries, source, dir, seen, out)
    if type(entries) ~= "table" then return end
    for _, e in ipairs(entries) do
        if type(e.name) == "string" and e.type == "file" then
            add_rec(seen, out, source, dir, e.name:match("^(.+)%.lua$"), false)
        end
    end
    for _, e in ipairs(entries) do
        if type(e.name) == "string" and e.type == "dir"
           and folder_has_theme(source, dir, e.name) then
            add_rec(seen, out, source, dir, e.name, true)
        end
    end
end

-- Discover all themes. Returns (list, by_id). Internal wins over SD on a clash.
local function discover()
    local seen, out = {}, {}
    local oki, ei = pcall(_list_all, INTERNAL_DIR)
    if oki then collect(ei, "internal", INTERNAL_DIR, seen, out) end
    local oks, es = pcall(_list_all_sd, SD_DIR)
    if oks then collect(es, "sd", SD_DIR, seen, out) end
    return out, seen
end

local function resolve(id)
    if not id or id == "" then return nil end
    local _, by_id = discover()
    return by_id[id]
end

-- Load a theme record's module table transiently (never keeps a reference).
-- Internal: require (flat "themes/<id>" or folder "themes/<id>/theme").
-- SD: _dofile_sd on the resolved path (returns the theme table).
local function load_theme(rec)
    if not rec then return nil end
    local ok, mod
    if rec.source == "sd" then
        ok, mod = pcall(_dofile_sd, rec.sd_path)
    else
        local path = rec.require_path
        package.loaded[path] = nil
        ok, mod = pcall(require, path)
        package.loaded[path] = nil
    end
    if ok and type(mod) == "table" and type(mod.apply) == "function" then
        return mod
    end
    if not ok then
        print("[theme] load error (" .. tostring(rec.id) .. "): " .. tostring(mod))
    end
    return nil
end

-- Run a resolved record's apply() transiently.
local function run_rec(rec)
    local mod = load_theme(rec)
    if not mod then return false end
    local t = make_toolkit(rec.asset_dir)
    local ok, err = pcall(mod.apply, t)
    if not ok then
        print("[theme] apply error (" .. tostring(rec.id) .. "): " .. tostring(err))
    end
    mod, t = nil, nil
    collectgarbage("collect")
    return true
end

-- ── Public API ──────────────────────────────────────────────────────────────

-- Available themes: { { id =, name =, source = }, ... } sorted by name.
function M.list()
    local recs = discover()
    local out = {}
    for _, rec in ipairs(recs) do
        local mod = load_theme(rec)
        if mod then out[#out + 1] = { id = rec.id, name = mod.name or rec.id, source = rec.source } end
        mod = nil
    end
    collectgarbage("collect")
    table.sort(out, function(a, b) return a.name < b.name end)
    return out
end

-- Id of the last applied theme (for the picker highlight).
function M.current()
    return current_id
end

-- Apply a theme by id: resolve it, run its apply() once, persist, release. Falls
-- back to the default theme if the id is unknown.
function M.apply(id)
    if not id or id == "" then id = DEFAULT_ID end
    local rec = resolve(id)
    if not rec then
        if id ~= DEFAULT_ID then return M.apply(DEFAULT_ID) end
        return false
    end
    -- Drop the previous theme's font BEFORE apply so a theme without set_font
    -- lands on the bundled default. Only here — show_background()'s re-run of
    -- apply() must NOT clear (set_font is idempotent C-side, so a font theme
    -- re-applying is a no-op, not a reload).
    pcall(_theme_font_clear)
    if not run_rec(rec) then
        if id ~= DEFAULT_ID then return M.apply(DEFAULT_ID) end
        return false
    end
    current_id, current_rec = id, rec
    pcall(_theme_pref_set, id)
    return true
end

-- Redraw the current theme's background only if it was freed (an app ran). Uses
-- the cached record, so no filesystem rescan on return home. The palette already
-- lives in the C theme (re-pushing it is a no-op there). Used by the launcher.
function M.ensure_background()
    if background.present() then return end
    if current_rec then
        run_rec(current_rec)
    else
        M.apply(DEFAULT_ID)
    end
end

-- Draw the current theme's background NOW. For lightweight apps (Settings,
-- Messenger, ...) that want the wallpaper behind their UI: call this at startup,
-- and make the app's containers transparent (bg_opa = 0) where the wallpaper
-- should show. The launch teardown freed the surface, so this is its first draw.
-- Do NOT call this from PSRAM-heavy apps (PICO-8/Doom/Map) — they need that PSRAM.
function M.show_background()
    if current_rec then run_rec(current_rec) end
end

return M
