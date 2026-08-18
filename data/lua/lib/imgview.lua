--[[
  imgview — reusable image display for MeshPunk (fit-to-screen / 1:1 pan).

  ANY app that needs to show an image file should use this instead of pointing
  an Image widget at a path. LVGL's own file path caps out at ~131k pixels: a
  decoded image that doesn't fit LV_CACHE_DEF_SIZE fails SILENTLY (blank
  widget). This wraps the _img_* bridge (src/img_bridge.cpp), which decodes
  into an app-owned PSRAM RGB565 buffer instead — no cache, no ceiling beyond
  free PSRAM.

  Formats: PNG, baseline JPEG (TJpgDec — progressive JPEGs are rejected with a
  message) and LVGL RGB565 .bin.

      local imgview = require("lib/imgview")

      local v = imgview.new(root, { w = W, h = H, bg = "#000000" })
      local ok, err = v:load("S:/photos/mesh.png")
      v:enable_drag()          -- touch panning in 1:1 mode
      v:toggle()               -- fit <-> 1:1
      v:pan(0, -24)            -- keys / trackball, clamped to the edges
      v:destroy()              -- frees the buffers AND deletes the widgets

  Two buffers per image: the loaded one (full resolution, decimated only if it
  would blow max_bytes) and a fit-to-viewport copy built by resampling the
  first — so switching modes never re-reads or re-decodes the file.

  The owner MUST call close() or destroy() when done; the buffers are PSRAM and
  nothing else frees them.
]]

local lvgl = require("lvgl")

local M = {}
local V = {}
V.__index = V

-- "#rrggbb" -> 0xRRGGBB for the bridge's alpha compositing.
local function rgb_int(color)
    if type(color) == "number" then return color end
    local hex = tostring(color or "#000000"):gsub("#", "")
    return tonumber(hex, 16) or 0
end

-- Largest w/h that fits (vw, vh) keeping aspect. Never enlarges.
local function fit_dims(iw, ih, vw, vh)
    if iw <= vw and ih <= vh then return iw, ih end
    local scale = math.min(vw / iw, vh / ih)
    return math.max(1, math.floor(iw * scale)), math.max(1, math.floor(ih * scale))
end

-- imgview.new(parent, opts) -> view
--   opts.w/h/x/y   viewport geometry (defaults: full screen at 0,0)
--   opts.bg        matte behind the image + under PNG alpha ("#000000")
--   opts.max_bytes cap for the loaded buffer; over it the load decimates
--                  by 1/2, 1/4, 1/8 (default 3MB)
--   opts.max_pixels source-size refusal threshold (default: bridge's 1.2M)
function M.new(parent, opts)
    opts = opts or {}
    local self = setmetatable({}, V)

    self.w = opts.w or lvgl.HOR_RES()
    self.h = opts.h or lvgl.VER_RES()
    self.bg = opts.bg or "#000000"
    self.bg_rgb = rgb_int(self.bg)
    self.max_bytes = opts.max_bytes or (3 * 1024 * 1024)
    self.max_pixels = opts.max_pixels
    self.mode = "fit"
    self.px, self.py = 0, 0

    self.cont = parent:Object {
        w = self.w, h = self.h, x = opts.x or 0, y = opts.y or 0,
        bg_color = self.bg, bg_opa = 255,
        border_width = 0, pad_all = 0, radius = 0,
    }
    self.cont:clear_flag(lvgl.FLAG.SCROLLABLE)

    -- Exactly viewport-sized: LVGL clips the (larger) image to the widget and
    -- set_offset slides it underneath — that is the 1:1 pan.
    self.img = self.cont:Image { w = self.w, h = self.h, x = 0, y = 0 }
    self.img:clear_flag(lvgl.FLAG.SCROLLABLE)
    self.img:clear_flag(lvgl.FLAG.CLICKABLE)
    self.img:add_flag(lvgl.FLAG.HIDDEN)

    return self
end

-- Free both buffers. Hides the widget first: nothing may draw from a buffer
-- being freed.
function V:close()
    if self.img then self.img:add_flag(lvgl.FLAG.HIDDEN) end
    if self.fit and self.fit ~= self.full then pcall(_img_close, self.fit) end
    if self.full then pcall(_img_close, self.full) end
    self.full, self.fit = nil, nil
    self.path = nil
end

function V:destroy()
    self:close()
    if self.cont then
        self.cont:delete()
        self.cont = nil
        self.img = nil
    end
end

-- load(path) -> true | nil, err. Replaces whatever was loaded before.
function V:load(path)
    if type(_img_open) ~= "function" then return nil, "firmware has no image bridge" end
    self:close()

    local opts = { max_bytes = self.max_bytes, bg = self.bg_rgb }
    if self.max_pixels then opts.max_pixels = self.max_pixels end

    local dsc, w, h, div = _img_open(path, opts)
    if not dsc then return nil, w or "load failed" end   -- w carries the error

    self.full, self.iw, self.ih, self.div = dsc, w, h, div
    self.path = path

    local fw, fh = fit_dims(w, h, self.w, self.h)
    if fw < w or fh < h then
        local f, rw, rh = _img_scale(dsc, fw, fh)
        if f then self.fit, self.fw, self.fh = f, rw, rh end
    end
    if not self.fit then
        -- No fit copy (PSRAM refused it): showing the oversized image under a
        -- "fit" label would just crop it, so open in 1:1 where panning works.
        self.fit, self.fw, self.fh = dsc, w, h
        if w > self.w or h > self.h then self.mode = "full" end
    end

    self.px, self.py = nil, nil        -- 1:1 starts centred
    self:set_mode(self.mode)
    self.img:clear_flag(lvgl.FLAG.HIDDEN)
    return true
end

-- Offset that puts image pixel (px, py) at the viewport's top-left. The image
-- is centred in the widget, so the neutral offset is half the overflow.
local function apply_offset(self)
    self.img:set_offset {
        x = math.floor((self.iw - self.w) / 2) - self.px,
        y = math.floor((self.ih - self.h) / 2) - self.py,
    }
end

function V:set_mode(mode)
    self.mode = (mode == "full") and "full" or "fit"
    if not self.full then return end
    if self.mode == "full" then
        self.img:set_src(self.full)
        self:pan(0, 0)
    else
        self.img:set_src(self.fit)
        self.img:set_offset { x = 0, y = 0 }
    end
end

function V:toggle()
    self:set_mode(self.mode == "fit" and "full" or "fit")
end

-- Pan by (dx, dy) screen pixels, clamped to the image edges. First call after
-- a load centres the view. No-op outside 1:1 mode.
function V:pan(dx, dy)
    if self.mode ~= "full" or not self.full then return end
    local mx = math.max(0, self.iw - self.w)
    local my = math.max(0, self.ih - self.h)
    if not self.px then self.px, self.py = math.floor(mx / 2), math.floor(my / 2) end
    self.px = math.max(0, math.min(mx, self.px + (dx or 0)))
    self.py = math.max(0, math.min(my, self.py + (dy or 0)))
    apply_offset(self)
end

function V:center()
    if not self.full then return end
    self.px, self.py = nil, nil
    self:pan(0, 0)
end

-- Touch panning on the viewport itself (1:1 mode only). on_tap, if given, is
-- called for a press that never moved — the idiom for toggling a toolbar.
function V:enable_drag(on_tap)
    local cont = self.cont
    cont:add_flag(lvgl.FLAG.CLICKABLE)
    cont:clear_flag(lvgl.FLAG.SCROLLABLE)

    local moved = false
    cont:onevent(lvgl.EVENT.PRESSED, function() moved = false end)
    cont:onevent(lvgl.EVENT.PRESSING, function()
        local vx, vy = lvgl.indev.get_act():get_vect()
        if vx == 0 and vy == 0 then return end
        if math.abs(vx) > 2 or math.abs(vy) > 2 then moved = true end
        self:pan(-vx, -vy)
    end)
    cont:onevent(lvgl.EVENT.RELEASED, function()
        if not moved and on_tap then on_tap() end
    end)
end

-- Everything an app needs for a status line.
function V:info()
    return {
        path = self.path, mode = self.mode,
        w = self.iw, h = self.ih, div = self.div,
        fit_w = self.fw, fit_h = self.fh,
        loaded = self.full ~= nil,
    }
end

return M
