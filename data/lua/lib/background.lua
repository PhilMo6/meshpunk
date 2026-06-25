--[[
  Home-screen background layer for MeshPunk themes.

  A single full-screen surface that sits BEHIND the launcher body and the topbar.
  It exists only on the home screen: launching an app frees it (returning its
  PSRAM) and returning home redraws it. This matters because a full-screen
  surface is expensive on this device — an RGB565 canvas is ~150KB and an ARGB
  image decode parks a draw buffer in PSRAM — exactly the memory the heavy apps
  (PICO-8 / Doom / Map) need. So the wallpaper must never be resident while an
  app runs (see lib/apps.launch -> background.free()).

  There is no redraw timer. Each theme apply draws once. The draw helpers below
  are one-shot and are called by a theme module's apply() via lib/theme's
  toolkit.

  Layout: a base lvgl.Object is the screen-level layer (so _obj_move_background
  can push it to the very back); the image / canvas is a child of that base. We
  use a base wrapper rather than a bare top-level Canvas/Image so placement on
  the active screen is guaranteed regardless of which widget kinds expose a
  top-level constructor.
]]

local lvgl = require("lvgl")

local M = {}

local layer = nil           -- base object holding the current background, or nil
local last_image_src = nil  -- set when an image is showing, so free() can evict it

local function W() return lvgl.HOR_RES() end
local function H() return lvgl.VER_RES() end

-- Is a background currently drawn? lib/theme.ensure_background() uses this to
-- decide whether a redraw is needed after returning home.
function M.present()
    return layer ~= nil
end

-- Tear down the current surface and reclaim its PSRAM. Called on app launch and
-- before every fresh render.
function M.free()
    if layer then
        pcall(function() layer:delete() end)  -- deletes children too; a child
        layer = nil                            -- Canvas frees its draw buf on the
    end                                        -- LV_EVENT_DELETE handler
    if last_image_src then
        -- Deleting the Image widget does NOT evict the decoded bitmap from
        -- LVGL's image cache, so the PSRAM stays held until something else
        -- pushes it out. Drop it explicitly. (_lvgl_image_cache_drop runs
        -- lv_image_cache_drop, which is only safe on the LVGL/UI thread — we
        -- are always called from there.)
        pcall(_lvgl_image_cache_drop, last_image_src)
        last_image_src = nil
    end
    collectgarbage("collect")
end

-- Fresh full-screen base at the very back. Internal; every render starts here.
local function new_base(opaque_bg, bg_color)
    M.free()
    local base = lvgl.Object({
        w = W(), h = H(), x = 0, y = 0,
        border_width = 0, pad_all = 0, radius = 0,
        bg_opa = opaque_bg and 255 or 0,
        bg_color = bg_color or "#000000",
    })
    base:clear_flag(lvgl.FLAG.CLICKABLE)
    base:clear_flag(lvgl.FLAG.SCROLLABLE)
    layer = base
    return base
end

local function send_to_back()
    if layer then pcall(_obj_move_background, layer) end
end

-- Solid color fill.
function M.fill(color)
    new_base(true, color)
    send_to_back()
end

-- Image wallpaper from a path, e.g. "S:/themes/midnight/wall.bin" (RGB565 native
-- little-endian per the lodepng/RGB565 note) or a .png.
function M.image(src)
    local base = new_base(false)
    local ok = pcall(function()
        local img = base:Image({ src = src, x = 0, y = 0, w = W(), h = H() })
        img:clear_flag(lvgl.FLAG.CLICKABLE)
        img:clear_flag(lvgl.FLAG.SCROLLABLE)
    end)
    last_image_src = ok and src or nil
    send_to_back()
end

-- Procedural background. Hands draw_fn(canvas, w, h) a fresh, opaque canvas.
-- The canvas defaults to RGB565 (half the PSRAM of ARGB8888) which is all a
-- bottom wallpaper needs. draw_fn does a single pass; do NOT start a timer.
function M.procedural(draw_fn)
    local base = new_base(false)
    local ok, cv = pcall(function()
        return base:Canvas({ w = W(), h = H(), x = 0, y = 0 })
    end)
    if ok and cv then
        cv:clear_flag(lvgl.FLAG.CLICKABLE)
        cv:clear_flag(lvgl.FLAG.SCROLLABLE)
        cv:fill_bg("#000000", 255)
        pcall(draw_fn, cv, W(), H())
    else
        print("[background] canvas alloc failed; wallpaper skipped")
    end
    send_to_back()
end

return M
