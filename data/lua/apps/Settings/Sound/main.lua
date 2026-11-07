local lvgl  = require("lvgl")
local sound = require("lib/sound")
local utils = require("lib/utils")
local apps  = require("lib/apps")
local nav   = require("lib/nav")
local theme = require("lib/theme")

-- Root
local root = apps.new_root()
root:set {
    w = lvgl.HOR_RES(),
    h = lvgl.VER_RES(),
    pad_all = 0,
    border_width = 0,
    bg_opa = 0,
}
root:clear_flag(lvgl.FLAG.SCROLLABLE)

-- Themed wallpaper behind this (lightweight) screen; containers below are transparent.
theme.show_background()

-- Scrollable content
local content = root:Object {
    flex = { flex_direction = "row", flex_wrap = "wrap" },
    w = lvgl.HOR_RES(),
    h = lvgl.VER_RES(),
    y = 0,
    border_width = 0,
    pad_all = 6,
    bg_opa = 0,
}

nav.replace(content, { flags = nav.ROLLOVER + nav.SCROLL_FIRST })

-- Title
content:Label { text = "Sound Settings", w = lvgl.PCT(70), h = 26 }
local back_btn = content:Button { w = 50, h = 22 }
back_btn:Label { text = "Home", align = lvgl.ALIGN.CENTER }

-- Status line
local status = content:Label { text = "", w = lvgl.PCT(100), h = 16 }

-- ── Volume ────────────────────────────────────────────────────────────────────
content:Label { text = "-- Volume --", w = lvgl.PCT(100), h = 16 }

local cur_vol = sound.getVolume()
local is_muted = sound.isMuted()

local vol_label = content:Label { text = "", w = lvgl.PCT(100), h = 16 }

-- 21-segment visual bar
local bar_row = content:Object {
    flex = { flex_direction = "row", flex_wrap = "nowrap" },
    w = lvgl.PCT(100),
    h = 18,
    border_width = 0,
    pad_hor = 0,
    pad_ver = 2,
    pad_column = 1,
}
bar_row:clear_flag(lvgl.FLAG.SCROLLABLE)
bar_row:clear_flag(lvgl.FLAG.CLICKABLE)
local segs = {}
for i = 1, 21 do
    segs[i] = bar_row:Object { w = math.min(13, math.floor((lvgl.HOR_RES() - 12) / 21) - 1), h = 14, border_width = 1, pad_all = 0,bg_color = "#24ba24"}
    segs[i]:clear_flag(lvgl.FLAG.SCROLLABLE)
    segs[i]:clear_flag(lvgl.FLAG.CLICKABLE)
end

local function refresh_ui()
    cur_vol  = sound.getVolume()
    is_muted = sound.isMuted()
    vol_label.text = "Volume: " .. cur_vol .. "/21" .. (is_muted and " (MUTED)" or "")
    for i = 1, 21 do
        segs[i]:set { bg_opa = (is_muted or i > cur_vol) and 40 or 255 }
    end
end
refresh_ui()

-- Volume buttons as direct children
local btn_dn = content:Button { w = lvgl.PCT(28), h = 30 }
btn_dn:Label { text = "Vol -", align = lvgl.ALIGN.CENTER }

local btn_up = content:Button { w = lvgl.PCT(28), h = 30 }
btn_up:Label { text = "Vol +", align = lvgl.ALIGN.CENTER }

local btn_mute = content:Button { w = lvgl.PCT(38), h = 30 }
local lbl_mute = btn_mute:Label { align = lvgl.ALIGN.CENTER }

local function upd_mute_btn()
    lbl_mute.text = is_muted and "Unmute" or "Mute"
end
upd_mute_btn()

btn_dn:onClicked(function()
    if cur_vol > 0 then sound.setVolume(cur_vol - 1) end
    refresh_ui(); upd_mute_btn()
    status.text = "Volume: " .. sound.getVolume()
end)
btn_up:onClicked(function()
    if cur_vol < 21 then sound.setVolume(cur_vol + 1) end
    refresh_ui(); upd_mute_btn()
    status.text = "Volume: " .. sound.getVolume()
end)
btn_mute:onClicked(function()
    sound.toggleMute()
    refresh_ui(); upd_mute_btn()
    status.text = is_muted and "Muted" or "Unmuted"
end)

-- ── Test ──────────────────────────────────────────────────────────────────────
-- The same C-E-G-C arpeggio notify.cpp pre-renders for message alerts
-- (notes and envelope copied from NOTIFY_MELODY there), so this plays what
-- an alert sounds like at the volume set above. Generated once, on first tap.
content:Label { text = "-- Test --", w = lvgl.PCT(100), h = 16 }

local test_sound = nil
local btn_test = content:Button { w = lvgl.PCT(100), h = 30 }
btn_test:Label { text = "Play notification sound", align = lvgl.ALIGN.CENTER }
btn_test:onClicked(function()
    if not test_sound then
        test_sound = sound.generateMelody({
            {freq=523, ms=150}, {freq=0, ms=30},
            {freq=659, ms=150}, {freq=0, ms=30},
            {freq=784, ms=150}, {freq=0, ms=30},
            {freq=1047, ms=300},
        }, { attack = 5, decay = 30, sustain = 0.6, release = 30 })
    end
    if not test_sound then
        status.text = "Sound generation failed"
        return
    end
    test_sound:play()
    status.text = is_muted and "Muted - unmute to hear it" or "Playing notification sound"
end)

-- Cleanup on back
back_btn:onClicked(function()
    -- app-specific audio cleanup first, then let the manager tear down the root
    if test_sound then test_sound:delete(); test_sound = nil end
    apps.go_home()
end)

return root
