local lvgl  = require("lvgl")
local sound = require("lib/sound")
local utils = require("lib/utils")

-- Root
local root = lvgl.Object()
root:set {
    w = lvgl.HOR_RES(),
    h = lvgl.VER_RES(),
    pad_all = 0,
    border_width = 0,
}
root:clear_flag(lvgl.FLAG.SCROLLABLE)

-- Scrollable content
local content = root:Object {
    flex = { flex_direction = "column", flex_wrap = "nowrap" },
    w = lvgl.HOR_RES(),
    h = lvgl.VER_RES(),
    y = 0,
    border_width = 0,
    pad_all = 6,
}

_nav_setup(content, GRIDNAV_ROLLOVER)

-- Title row
local title_row = content:Object {
    w = lvgl.PCT(100),
    h = 26,
    border_width = 0,
    pad_all = 0,
}
title_row:clear_flag(lvgl.FLAG.SCROLLABLE)

title_row:Label {
    text = "Sound Settings",
    align = lvgl.ALIGN.LEFT_MID,
}

local back_btn = title_row:Button { w = 50, h = 22, align = lvgl.ALIGN.RIGHT_MID }
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
    pad_all = 0,
}
bar_row:clear_flag(lvgl.FLAG.SCROLLABLE)
local segs = {}
for i = 1, 21 do
    segs[i] = bar_row:Object { w = 13, h = 14, border_width = 1, pad_all = 0 }
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

-- +/- / Mute button row
local btn_row = content:Object {
    flex = { flex_direction = "row", flex_wrap = "nowrap" },
    w = lvgl.PCT(100),
    h = 34,
    border_width = 0,
    pad_all = 0,
}
btn_row:clear_flag(lvgl.FLAG.SCROLLABLE)

local btn_dn = btn_row:Button { w = lvgl.PCT(28), h = 30 }
btn_dn:Label { text = "Vol -", align = lvgl.ALIGN.CENTER }

local btn_up = btn_row:Button { w = lvgl.PCT(28), h = 30 }
btn_up:Label { text = "Vol +", align = lvgl.ALIGN.CENTER }

local btn_mute = btn_row:Button { w = lvgl.PCT(38), h = 30 }
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
content:Label { text = "-- Test --", w = lvgl.PCT(100), h = 16 }

local test_row = content:Object {
    flex = { flex_direction = "row", flex_wrap = "wrap" },
    w = lvgl.PCT(100),
    h = 34,
    border_width = 0,
    pad_all = 0,
}
test_row:clear_flag(lvgl.FLAG.SCROLLABLE)

local test_tone = nil
local btn_tone = test_row:Button { w = lvgl.PCT(45), h = 30 }
btn_tone:Label { text = "Test Tone", align = lvgl.ALIGN.CENTER }
btn_tone:onClicked(function()
    if not test_tone then
        test_tone = sound.generateTone(880, 300)
    end
    if test_tone then
        test_tone:play()
        status.text = "Playing tone..."
    else
        status.text = "Tone gen failed"
    end
end)

local test_file = nil
local btn_file = test_row:Button { w = lvgl.PCT(45), h = 30 }
btn_file:Label { text = "Test File", align = lvgl.ALIGN.CENTER }
btn_file:onClicked(function()
    if not test_file then
        local f = io.open("L:/sounds/notify.mp3", "r")
        if f then
            test_file = sound.loadFile(f)
        end
    end
    if test_file then
        test_file:play()
        status.text = "Playing file..."
    else
        status.text = "No notify.mp3 found"
    end
end)

-- Cleanup on back
back_btn:onClicked(function()
    utils.loadingPopUpAdd(nil, "Home", function()
        if test_tone then test_tone:delete(); test_tone = nil end
        if test_file then test_file:delete(); test_file = nil end
        root:delete()
        local launcher = require("launcher")
        launcher.create()
        return true
    end)
end)

return root
