local lvgl  = require("lvgl")
local utils = require("lib/utils")

local root = lvgl.Object()
root:set { w = lvgl.HOR_RES(), h = lvgl.VER_RES(), pad_all = 0, border_width = 0 }
root:clear_flag(lvgl.FLAG.SCROLLABLE)

local content = root:Object {
    flex = { flex_direction = "column", flex_wrap = "nowrap" },
    w = lvgl.HOR_RES(), h = lvgl.VER_RES(),
    border_width = 0, pad_all = 6,
}
_nav_setup(content, GRIDNAV_ROLLOVER)

-- Title row
local title_row = content:Object { w = lvgl.PCT(100), h = 26, border_width = 0, pad_all = 0 }
title_row:clear_flag(lvgl.FLAG.SCROLLABLE)
title_row:Label { text = "Notifications", align = lvgl.ALIGN.LEFT_MID }
local back_btn = title_row:Button { w = 50, h = 22, align = lvgl.ALIGN.RIGHT_MID }
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

-- ── Back ────────────────────────────────────────────────────────────────────
back_btn:onClicked(function()
    utils.loadingPopUpAdd(nil, "Home", function()
        root:delete()
        local launcher = require("launcher")
        launcher.create()
        return true
    end)
end)

return root
