local lvgl  = require("lvgl")
local utils = require("lib/utils")
local apps  = require("lib/apps")
local nav   = require("lib/nav")
local theme = require("lib/theme")

local root = apps.new_root()
root:set { w = lvgl.HOR_RES(), h = lvgl.VER_RES(), pad_all = 0, border_width = 0, bg_opa = 0 }
root:clear_flag(lvgl.FLAG.SCROLLABLE)

-- Themed wallpaper behind this (lightweight) screen; containers below are transparent.
theme.show_background()

local content = root:Object {
    flex = { flex_direction = "row", flex_wrap = "wrap" },
    w = lvgl.HOR_RES(), h = lvgl.VER_RES(),
    border_width = 0, pad_all = 6, bg_opa = 0,
}
nav.replace(content)

-- Title
content:Label { text = "Notifications", w = lvgl.PCT(70), h = 26 }
local back_btn = content:Button { w = 50, h = 22 }
back_btn:Label { text = "Home", align = lvgl.ALIGN.CENTER }

local status = content:Label { text = "", w = lvgl.PCT(100), h = 16 }

-- ── Keyboard Blink ──────────────────────────────────────────────────────────
content:Label { text = "-- Keyboard Blink --", w = lvgl.PCT(100), h = 16 }

local kbd_enabled = _notify_kbd_get()
local btn_kbd = content:Button { w = lvgl.PCT(60), h = 30 }
local lbl_kbd = btn_kbd:Label { align = lvgl.ALIGN.CENTER }

local function refresh_kbd()
    kbd_enabled = _notify_kbd_get()
    lbl_kbd.text = kbd_enabled and "ON" or "OFF"
end
refresh_kbd()

btn_kbd:onClicked(function()
    _notify_kbd_set(not kbd_enabled)
    refresh_kbd()
    status.text = "Keyboard blink: " .. (kbd_enabled and "ON" or "OFF")
end)

-- ── Sound ───────────────────────────────────────────────────────────────────
content:Label { text = "-- Sound --", w = lvgl.PCT(100), h = 16 }

local snd_enabled = _notify_sound_get()
local btn_snd = content:Button { w = lvgl.PCT(60), h = 30 }
local lbl_snd = btn_snd:Label { align = lvgl.ALIGN.CENTER }

local function refresh_snd()
    snd_enabled = _notify_sound_get()
    lbl_snd.text = snd_enabled and "ON" or "OFF"
end
refresh_snd()

btn_snd:onClicked(function()
    _notify_sound_set(not snd_enabled)
    refresh_snd()
    status.text = "Sound: " .. (snd_enabled and "ON" or "OFF")
end)

-- ── Per-channel mode ────────────────────────────────────────────────────────
-- 0 = Off, 1 = Mentions only (default), 2 = All messages. Stored C-side by
-- channel NAME (slots shift when channels are added/removed). DMs always alert.
content:Label { text = "-- Channels --", w = lvgl.PCT(100), h = 16 }

local MODE_NAMES = { [0] = "Off", [1] = "Mentions", [2] = "All" }

local channels = _mesh_get_channels()
if #channels == 0 then
    content:Label { text = "(no channels)", w = lvgl.PCT(100), h = 16 }
end
for _, ch in ipairs(channels) do
    content:Label { text = ch.name, w = lvgl.PCT(45), h = 30 }
    local btn = content:Button { w = lvgl.PCT(50), h = 30 }
    local lbl = btn:Label { align = lvgl.ALIGN.CENTER }
    local mode = _notify_channel_get(ch.name)
    lbl.text = MODE_NAMES[mode] or "Mentions"
    btn:onClicked(function()
        mode = (mode + 1) % 3
        _notify_channel_set(ch.name, mode)
        lbl.text = MODE_NAMES[mode]
        status.text = ch.name .. ": " .. MODE_NAMES[mode]
    end)
end

-- ── Back ────────────────────────────────────────────────────────────────────
back_btn:onClicked(function()
    apps.go_home()
end)

return root
