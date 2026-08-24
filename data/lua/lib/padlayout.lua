-- lib/padlayout.lua — per-app touch controller layouts with user editing.
--
-- A launcher declares one or more named PRESETS, each a full zone table:
--   { id="up", out=<code>, label="^", x=52, y=118, w=64, h=56 }, ...
-- The lib owns the user's adjustments (drag-to-move, size steppers, per-zone
-- on/off, preset choice), persisted per app. Out codes are opaque bytes
-- here — module keycodes from ELF launchers, key chars from Lua apps — so
-- the same lib serves both worlds. Deliberately independent of lib/keybind.
--
--   local pad = padlayout.new{ app = "GameBoy", presets = { {name=..., zones={...}}, ... } }
--   pad:zones()   -- resolved zone table for _elf_touch_layout / _zones_set,
--                 -- nil when everything is disabled or no preset exists
--   pad:open{ show_screen=..., font=..., accent=..., on_back=... }  -- editor
--
-- Storage: L:/touch_layouts/<app>.cfg — chosen preset + per-zone GEOMETRY
-- overrides keyed by zone id (`zone=up,52,118,64,56`, `zone=select,off`).
-- Outs and labels always come from the launcher's preset, so a launcher
-- update never invalidates saved geometry; unknown ids are ignored. A zone
-- may also carry `,on` (or a bare `zone=<id>,on`), which is what a preset
-- zone marked default_off needs to say it was deliberately enabled.
--
-- Every preset gains a Screenshot pad automatically — see M.new.
--
-- Editor: live full-screen preview, drag a pad to move it, W/H steppers in
-- a two-row control bar that flips to the far half of the screen from the
-- selected pad. All edits auto-save on Back.

local lvgl = require("lvgl")

local M = {}

local SCREEN_W, SCREEN_H = 320, 240
local MIN_SIDE = 24
local CFG_DIR = "L:/touch_layouts"

-- The Screenshot pad every layout gains (see M.new). 0xFD is a FIRMWARE out
-- (INPUT_ZONE_SHOT in input_zones.h): the zone layer turns a tap on it into a
-- capture request, and no app or module ever receives it as a key.
local SHOT_ID  = "shot"
local SHOT_OUT = 0xFD

local Pad = {}
Pad.__index = Pad

local function cfg_path(app)
    return CFG_DIR .. "/" .. app:gsub("[^%w_%-]", "_") .. ".cfg"
end

local function clamp(v, lo, hi)
    if v < lo then return lo end
    if v > hi then return hi end
    return v
end

-- ── Persistence ─────────────────────────────────────────────────────────────

function Pad:_load()
    local f = io.open(cfg_path(self.app), "r")
    if not f then return end
    -- This firmware's io handles have no :lines() — whole-file read.
    local txt = f:read("*a") or ""
    f:close()
    for line in txt:gmatch("[^\r\n]+") do
        local preset = line:match("^preset=(.+)%s*$")
        if preset then
            self.preset_name = preset
        else
            local id, rest = line:match("^zone=([%w_%-]+),(.+)%s*$")
            if id then
                -- off is TRI-STATE here: nil means the line carried no on/off
                -- marker, which leaves the choice to the preset's default_off.
                -- Zones that ship disabled (Screenshot) need the explicit "on"
                -- form; every other zone still reads exactly as it used to.
                local off = nil
                local geom = rest
                if rest == "off" then
                    self.over[id] = { off = true }
                    geom = nil
                elseif rest == "on" then
                    self.over[id] = { off = false }
                    geom = nil
                elseif rest:sub(-4) == ",off" then
                    off = true
                    geom = rest:sub(1, -5)
                elseif rest:sub(-3) == ",on" then
                    off = false
                    geom = rest:sub(1, -4)
                end
                if geom then
                    local x, y, w, h =
                        geom:match("^(%-?%d+),(%-?%d+),(%d+),(%d+)$")
                    if x then
                        self.over[id] = {
                            x = tonumber(x), y = tonumber(y),
                            w = tonumber(w), h = tonumber(h),
                            off = off,
                        }
                    end
                end
            end
        end
    end
end

function Pad:save()
    _fs_mkdir(CFG_DIR)
    local f = io.open(cfg_path(self.app), "w")
    if not f then return false end
    if self.preset_name then f:write("preset=" .. self.preset_name .. "\n") end
    for id, o in pairs(self.over) do
        -- An explicit false writes ",on": without it a zone whose preset says
        -- default_off would come back disabled after the user enabled it.
        local mark = ""
        if o.off == true then mark = ",off"
        elseif o.off == false then mark = ",on" end
        if o.x then
            f:write(string.format("zone=%s,%d,%d,%d,%d%s\n",
                id, o.x, o.y, o.w, o.h, mark))
        elseif mark ~= "" then
            f:write(string.format("zone=%s,%s\n", id, mark:sub(2)))
        end
    end
    f:close()
    return true
end

-- ── Resolution ──────────────────────────────────────────────────────────────

function Pad:_preset()
    for _, p in ipairs(self.presets) do
        if p.name == self.preset_name then return p end
    end
    return self.presets[1]
end

-- Effective geometry of one preset zone (override applied, clamped on-screen).
function Pad:_geom(z)
    local o = self.over[z.id]
    local off
    if o and o.off ~= nil then off = o.off else off = z.default_off or false end
    local g = {
        x = (o and o.x) or z.x, y = (o and o.y) or z.y,
        w = (o and o.w) or z.w, h = (o and o.h) or z.h,
        off = off,
    }
    g.w = clamp(g.w, MIN_SIDE, SCREEN_W)
    g.h = clamp(g.h, MIN_SIDE, SCREEN_H)
    g.x = clamp(g.x, 0, SCREEN_W - g.w)
    g.y = clamp(g.y, 0, SCREEN_H - g.h)
    return g
end

function Pad:zones()
    local p = self:_preset()
    if not p then return nil end
    local out = {}
    for _, z in ipairs(p.zones) do
        local g = self:_geom(z)
        if not g.off then
            out[#out + 1] = {
                x = g.x, y = g.y, w = g.w, h = g.h,
                out = z.out, label = z.label,
            }
        end
    end
    if #out == 0 then return nil end
    return out
end

function M.new(opts)
    local self = setmetatable({}, Pad)
    self.app = assert(opts.app, "padlayout.new: app name required")
    self.presets = opts.presets or {}
    self.preset_name = self.presets[1] and self.presets[1].name or nil
    self.over = {}
    -- Added here rather than by each launcher, so every app that has a pad can
    -- take a screenshot and none can drift out of having one. It ships DISABLED
    -- (default_off): a pad the user never asked for should not take screen
    -- space in a game, and a stray tap costs a capture. Enable it per app with
    -- this editor's Off button, then drag it anywhere.
    --
    -- Upper right, one row below the top: QUIT sits at 0,0 in every launcher,
    -- and the top row itself is full to x=286 in the widest of them (Dos), so
    -- nothing 54 wide fits up there. The band this occupies is empty in all
    -- ten. 54 is the width zone_overlay gives a four-letter chip, and a chip
    -- is centred on its zone and clipped at the screen edge — a narrower pad
    -- would render as "SHO".
    for _, p in ipairs(self.presets) do
        local has = false
        for _, z in ipairs(p.zones or {}) do
            if z.id == SHOT_ID then has = true end
        end
        if p.zones and not has then
            p.zones[#p.zones + 1] = {
                id = SHOT_ID, out = SHOT_OUT, label = "SHOT",
                x = 266, y = 32, w = 54, h = 30, default_off = true,
            }
        end
    end
    self:_load()
    return self
end

-- ── Editor ──────────────────────────────────────────────────────────────────
-- o: show_screen (required — the launcher's view swapper), font, accent,
-- on_back. Widgets use IGNORE_LAYOUT + absolute coords, so the launcher's
-- flex container just hosts them; its padding is zeroed for screen-aligned
-- positions.

local BAR_H = 88   -- three 26px rows: status/actions, move, size
local NUDGE_STEP = 2
local SIZE_STEP  = 4

function Pad:open(o)
    if o then self.ui = o end
    local ui = self.ui or {}
    local font   = ui.font or lvgl.BUILTIN_FONT.MONTSERRAT_12
    local accent = ui.accent or "#FFAA00"

    ui.show_screen(function(c)
        c:set{ pad_all = 0 }
        c:clear_flag(lvgl.FLAG.SCROLLABLE)
        c:clear_flag(lvgl.FLAG.SCROLL_CHAIN)   -- see the box drag flags below

        local sel = nil          -- selected preset-zone index
        local boxes = {}         -- idx -> { obj=, label_obj=, z=, g= }
        local dirty = false
        local bar, name_lbl, off_btn_lbl, preset_lbl

        local function abs(obj)
            obj:add_flag(lvgl.FLAG.IGNORE_LAYOUT)
            return obj
        end

        -- The bar parks at the bottom and only jumps to the top when the
        -- selected pad would sit under it — moving on overlap (not on a
        -- midline test) gives natural hysteresis, so dragging across the
        -- middle of the screen can't make it flicker back and forth.
        local bar_y = SCREEN_H - BAR_H

        local function bar_hides(g, y)
            return g.y < y + BAR_H and g.y + g.h > y
        end

        local function bar_place()
            if not bar or not sel then return end
            local g = boxes[sel].g
            if not bar_hides(g, bar_y) then return end
            local other = (bar_y == 0) and (SCREEN_H - BAR_H) or 0
            if bar_hides(g, other) then return end   -- covered either way
            bar_y = other
            bar:set{ y = bar_y }
        end

        local function box_paint(i)
            local b = boxes[i]
            b.obj:set{
                x = b.g.x, y = b.g.y, w = b.g.w, h = b.g.h,
                bg_opa = b.g.off and 8 or 45,
                border_color = (i == sel) and accent or "#FFFFFF",
                border_opa = b.g.off and 80 or 200,
                border_width = (i == sel) and 2 or 1,
            }
            b.label_obj:set{
                text = b.z.label or b.z.id,
                text_color = b.g.off and "#777777" or "#FFFFFF",
            }
        end

        local function bar_refresh()
            if not name_lbl then return end
            if sel then
                local b = boxes[sel]
                name_lbl:set{ text = string.format("%s %dx%d",
                    b.z.label or b.z.id, b.g.w, b.g.h) }
                off_btn_lbl:set{ text = b.g.off and "On" or "Off" }
            else
                name_lbl:set{ text = "tap a pad" }
                off_btn_lbl:set{ text = "Off" }
            end
        end

        local function select_box(i)
            local prev = sel
            sel = i
            if prev then box_paint(prev) end
            if sel then box_paint(sel) end
            bar_place()
            bar_refresh()
        end

        -- Record the selected zone's current geometry as an override.
        local function touch_override()
            local b = boxes[sel]
            local o2 = self.over[b.z.id] or {}
            o2.x, o2.y, o2.w, o2.h = b.g.x, b.g.y, b.g.w, b.g.h
            o2.off = b.g.off and true or false   -- explicit: nil means "use the preset default"
            self.over[b.z.id] = o2
            dirty = true
        end

        local function build_boxes()
            for _, b in pairs(boxes) do b.obj:delete() end
            boxes = {}
            sel = nil
            local p = self:_preset()
            if not p then return end
            for i, z in ipairs(p.zones) do
                local g = self:_geom(z)
                local obj = abs(c:Object{
                    x = g.x, y = g.y, w = g.w, h = g.h,
                    bg_color = "#FFFFFF", bg_opa = 45,
                    radius = 6, border_width = 1,
                    border_color = "#FFFFFF", border_opa = 200,
                    pad_all = 0,
                })
                obj:clear_flag(lvgl.FLAG.SCROLLABLE)
                obj:add_flag(lvgl.FLAG.CLICKABLE)
                -- A drag must stay with this box: SCROLL_CHAIN would hand
                -- the press to a scrollable ancestor mid-drag (the view
                -- slides, the box snaps back to where it started), and
                -- PRESS_LOCK keeps the press ours once it leaves the box.
                obj:clear_flag(lvgl.FLAG.SCROLL_CHAIN)
                obj:add_flag(lvgl.FLAG.PRESS_LOCK)
                local lbl = obj:Label{
                    text = z.label or z.id, text_font = font,
                    text_color = "#FFFFFF", align = lvgl.ALIGN.CENTER,
                }
                boxes[i] = { obj = obj, label_obj = lbl, z = z, g = g }
                obj:onevent(lvgl.EVENT.PRESSED, function() select_box(i) end)
                obj:onevent(lvgl.EVENT.PRESSING, function()
                    if sel ~= i then return end
                    local vx, vy = lvgl.indev.get_act():get_vect()
                    if vx == 0 and vy == 0 then return end
                    local b = boxes[i]
                    b.g.x = clamp(b.g.x + vx, 0, SCREEN_W - b.g.w)
                    b.g.y = clamp(b.g.y + vy, 0, SCREEN_H - b.g.h)
                    obj:set{ x = b.g.x, y = b.g.y }
                    touch_override()
                    bar_place()
                end)
                box_paint(i)
            end
            -- Boxes are created after the bar, so re-front it: a pad parked
            -- under the bar must not cover its buttons.
            if bar then pcall(_obj_move_foreground, bar) end
        end

        -- ── Control bar ─────────────────────────────────────────────
        bar = abs(c:Object{
            x = 0, y = 0, w = SCREEN_W, h = BAR_H,
            bg_color = "#101010", bg_opa = 230,
            border_width = 0, radius = 0, pad_all = 2,
        })
        bar:clear_flag(lvgl.FLAG.SCROLLABLE)

        -- rep=true: also fire while held (steppers and the pad spinner).
        -- Never on Back/preset/Off — Back deletes this view on its first
        -- fire, so a repeat would run against deleted widgets.
        local function bar_btn(x, y, w, text, cb, rep)
            local b = bar:Button{ x = x, y = y, w = w, h = 26 }
            b:add_flag(lvgl.FLAG.IGNORE_LAYOUT)
            local l = b:Label{ text = text, text_font = font,
                               align = lvgl.ALIGN.CENTER }
            b:onClicked(cb)
            if rep then b:onevent(lvgl.EVENT.LONG_PRESSED_REPEAT, cb) end
            return b, l
        end

        -- Resize the selected pad by (dw, dh), anchored at its top-left.
        local function grow(dw, dh)
            if not sel then return end
            local b = boxes[sel]
            b.g.w = clamp(b.g.w + dw, MIN_SIDE, SCREEN_W - b.g.x)
            b.g.h = clamp(b.g.h + dh, MIN_SIDE, SCREEN_H - b.g.y)
            box_paint(sel)
            touch_override()
            bar_refresh()
        end

        -- Fine positioning: the panel isn't accurate enough to place a pad
        -- to the pixel by dragging.
        local function nudge(dx, dy)
            if not sel then return end
            local b = boxes[sel]
            b.g.x = clamp(b.g.x + dx, 0, SCREEN_W - b.g.w)
            b.g.y = clamp(b.g.y + dy, 0, SCREEN_H - b.g.h)
            b.obj:set{ x = b.g.x, y = b.g.y }
            touch_override()
            bar_place()
        end

        -- Step the selection through the pads in preset order. The only way
        -- to reach a pad parked behind the bar — and selecting it moves the
        -- bar off it (select_box -> bar_place).
        local function cycle(dir)
            local n = #boxes
            if n == 0 then return end
            local i = (sel or 0) + dir
            if i < 1 then i = n elseif i > n then i = 1 end
            select_box(i)
        end

        -- Each row is laid out centered on the bar: a button flush against
        -- the left edge sits in a screen corner whenever the bar parks at
        -- the top or bottom, and corners are hard to hit accurately.
        -- items = { { w = <width>, make = function(x, y) ... end }, ... }
        local function row_center(y, gap, items)
            local total = 0
            for i, it in ipairs(items) do
                total = total + it.w
                if i > 1 then total = total + gap end
            end
            local x = math.floor((SCREEN_W - total) / 2)
            for _, it in ipairs(items) do
                it.make(x, y)
                x = x + it.w + gap
            end
        end

        -- Row 0: pad spinner (< NAME >) then the actions.
        local row0 = {
            { w = 36, make = function(x, y)
                bar_btn(x, y, 36, "<", function() cycle(-1) end, true)
            end },
            { w = 70, make = function(x, y)
                name_lbl = bar:Label{
                    text = "tap a pad", text_font = font,
                    text_color = "#CCCCCC", x = x, y = y + 6,
                }
                name_lbl:add_flag(lvgl.FLAG.IGNORE_LAYOUT)
            end },
            { w = 36, make = function(x, y)
                bar_btn(x, y, 36, ">", function() cycle(1) end, true)
            end },
            { w = 32, make = function(x, y)
                local _, l = bar_btn(x, y, 32, "Off", function()
                    if not sel then return end
                    local b = boxes[sel]
                    b.g.off = not b.g.off
                    box_paint(sel)
                    touch_override()
                    bar_refresh()
                end)
                off_btn_lbl = l
            end },
            { w = 32, make = function(x, y)
                bar_btn(x, y, 32, "Rst", function()
                    if not sel then return end
                    local b = boxes[sel]
                    self.over[b.z.id] = nil
                    dirty = true
                    b.g = self:_geom(b.z)
                    box_paint(sel)
                    bar_place()
                    bar_refresh()
                end)
            end },
        }
        if #self.presets > 1 then
            row0[#row0 + 1] = { w = 44, make = function(x, y)
                local _, l = bar_btn(x, y, 44, self.preset_name or "?", function()
                    local idx = 1
                    for i, p in ipairs(self.presets) do
                        if p.name == self.preset_name then idx = i end
                    end
                    self.preset_name = self.presets[(idx % #self.presets) + 1].name
                    dirty = true
                    preset_lbl:set{ text = self.preset_name }
                    build_boxes()
                    bar_place()
                    bar_refresh()
                end)
                preset_lbl = l
            end }
        end
        row0[#row0 + 1] = { w = 38, make = function(x, y)
            bar_btn(x, y, 38, "Back", function()
                if dirty then self:save() end
                if ui.on_back then ui.on_back() end
            end)
        end }
        row_center(2, 4, row0)

        -- Rows 1 and 2: move / resize, four thumb-sized steppers each.
        bar:Label{ text = "MOVE", text_font = font, text_color = "#888888",
                   x = 4, y = 36 }:add_flag(lvgl.FLAG.IGNORE_LAYOUT)
        bar_btn(48,  30, 64, "<", function() nudge(-NUDGE_STEP, 0) end, true)
        bar_btn(114, 30, 64, ">", function() nudge(NUDGE_STEP, 0) end, true)
        bar_btn(180, 30, 64, "^", function() nudge(0, -NUDGE_STEP) end, true)
        bar_btn(246, 30, 64, "v", function() nudge(0, NUDGE_STEP) end, true)

        bar:Label{ text = "SIZE", text_font = font, text_color = "#888888",
                   x = 4, y = 64 }:add_flag(lvgl.FLAG.IGNORE_LAYOUT)
        bar_btn(48,  58, 64, "W-", function() grow(-SIZE_STEP, 0) end, true)
        bar_btn(114, 58, 64, "W+", function() grow(SIZE_STEP, 0) end, true)
        bar_btn(180, 58, 64, "H-", function() grow(0, -SIZE_STEP) end, true)
        bar_btn(246, 58, 64, "H+", function() grow(0, SIZE_STEP) end, true)

        build_boxes()
        bar:set{ y = bar_y }
        bar_place()
        bar_refresh()
    end)
end

return M
