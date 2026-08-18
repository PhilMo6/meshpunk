-- lib/touchlayout.lua — controller-mode layouts for Lua apps.
--
-- Controller mode turns the touchscreen into a game controller: the C zone
-- layer (_zones_*) intercepts ALL touch and routes zone hits into the same
-- key stream physical keys use, under a mostly-transparent overlay of small
-- indicators. This module owns the zone plumbing and the overlay; the
-- MODE itself is firmware state (_touch_mode), shared with the ELF world.
--
-- One trigger drives that mode everywhere: the board's aux button (the
-- Heltec IO key) or the Shift+Alt chord on boards with a keyboard. loop()'s
-- dispatcher calls M.on_mode_button(), which cycles the shared mode and
-- applies it here. There is deliberately no on-screen trigger.
--
-- Modes (firmware enum): 0 OFF, 1 PAD (indicators shown), 2 PAD hidden,
-- 3 KB (no pad; the on-screen keyboard opens on textarea focus). Apps with
-- no layout skip the PAD states, so the cycle there is OFF <-> KB.
--
-- This module owns the zone plumbing and the overlay, nothing else. An app
-- sets its own layout with M.set(zones) and drops it with M.clear() through
-- apps.set_on_close, the same hook every other app uses for teardown; the
-- manager has no knowledge of layouts.

local lvgl = require("lvgl")

local M = {}

local MODE_OFF, MODE_PAD, MODE_PAD_HIDDEN = 0, 1, 2

-- ── State ───────────────────────────────────────────────────────────────────
local layout  = nil       -- zones the running app set, or nil for no pad
local overlay = nil       -- indicator layer

local function delete_overlay()
    if overlay then
        local ov = overlay
        overlay = nil
        pcall(function() ov:delete() end)
    end
end

local function build_overlay(zones)
    delete_overlay()
    overlay = lvgl.Object {
        w = lvgl.HOR_RES(), h = lvgl.VER_RES(), x = 0, y = 0,
        bg_opa = 0, border_width = 0, pad_all = 0,
    }
    overlay:clear_flag(lvgl.FLAG.SCROLLABLE)
    overlay:clear_flag(lvgl.FLAG.CLICKABLE)   -- purely visual; C owns the touch
    pcall(_obj_move_foreground, overlay)

    for i = 1, #zones do
        local z = zones[i]
        local chip = overlay:Object {
            x = z.x, y = z.y, w = z.w, h = z.h,
            bg_color = "#FFFFFF", bg_opa = 30,
            radius = 8, border_width = 1, border_color = "#AAAAAA",
            border_opa = 90, pad_all = 0,
        }
        chip:clear_flag(lvgl.FLAG.SCROLLABLE)
        chip:clear_flag(lvgl.FLAG.CLICKABLE)
        chip:Label { text = z.label or "", align = lvgl.ALIGN.CENTER }
    end
end

-- Apply a mode to the running app: arm/disarm the zone layer and build or
-- drop the indicator overlay. KB and OFF both mean "no pad here" — the
-- on-screen keyboard is triggered by textarea focus in the firmware, not
-- from this module.
local function apply(mode)
    if layout and (mode == MODE_PAD or mode == MODE_PAD_HIDDEN) then
        _zones_set(layout)
        _zones_enable(true)
        if mode == MODE_PAD then build_overlay(layout) else delete_overlay() end
    else
        _zones_enable(false)
        _zones_clear()
        delete_overlay()
    end
end

-- ── App-facing API ──────────────────────────────────────────────────────────
-- An app owns its own controller layout, sets it when it wants one, and drops
-- it on the way out through the normal apps.set_on_close hook:
--
--     local touchlayout = require("lib/touchlayout")
--     touchlayout.set{
--         { x = 0, y = 170, w = 70, h = 70, out = 0x61, label = "<" },
--         ...
--     }
--     apps.set_on_close(touchlayout.clear)
--
-- `out` is the key code a zone sends, which is why only the app can write
-- this: the zones stand in for keys that app already reads. Call set() again
-- at any time to change the layout - an app that loads a saved one applies it
-- the same way.
--
-- Register the close hook AFTER the app's root exists: apps.set_root clears
-- any callback registered before it.
function M.set(zones)
    layout = zones
    delete_overlay()
    apply(_touch_mode())
end

function M.clear()
    layout = nil
    _zones_enable(false)
    _zones_clear()
    delete_overlay()
end

-- ── Mode trigger (aux button / Shift+Alt chord / MODE zone) ─────────────────
function M.on_mode_button()
    apply(_touch_mode_cycle(layout ~= nil))
end

return M
