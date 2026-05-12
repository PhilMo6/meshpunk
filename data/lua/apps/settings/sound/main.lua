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
    segs[i] = bar_row:Object { w = 13, h = 14, border_width = 1, pad_all = 0,bg_color = "#24ba24"}
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

-- ── Waveforms ─────────────────────────────────────────────────────────────────
content:Label { text = "-- Waveforms --", w = lvgl.PCT(100), h = 16 }

local wave_row = content:Object {
    flex = { flex_direction = "row", flex_wrap = "wrap" },
    w = lvgl.PCT(100),
    h = lvgl.SIZE_CONTENT,
    border_width = 0,
    pad_all = 0,
}
wave_row:clear_flag(lvgl.FLAG.SCROLLABLE)

local demo_tones = {}

local wave_types = { "sine", "square", "saw", "triangle", "noise" }
local wave_labels = { "Sine", "Square", "Saw", "Tri", "Noise" }
for i, wf in ipairs(wave_types) do
    local btn = wave_row:Button { w = lvgl.PCT(30), h = 30 }
    btn:Label { text = wave_labels[i], align = lvgl.ALIGN.CENTER }
    btn:onClicked(function()
        local opts = nil
        if wf ~= "sine" then opts = { waveform = wf } end
        local t = sound.generateTone(440, 300, opts)
        if t then
            t:play()
            demo_tones[#demo_tones + 1] = t
            status.text = wave_labels[i] .. " wave"
        end
    end)
end

-- ── Effects ───────────────────────────────────────────────────────────────────
content:Label { text = "-- Effects --", w = lvgl.PCT(100), h = 16 }

local fx_row = content:Object {
    flex = { flex_direction = "row", flex_wrap = "wrap" },
    w = lvgl.PCT(100),
    h = lvgl.SIZE_CONTENT,
    border_width = 0,
    pad_all = 0,
}
fx_row:clear_flag(lvgl.FLAG.SCROLLABLE)

local btn_adsr = fx_row:Button { w = lvgl.PCT(45), h = 30 }
btn_adsr:Label { text = "ADSR", align = lvgl.ALIGN.CENTER }
btn_adsr:onClicked(function()
    local t = sound.generateTone(440, 800, {
        attack = 100, decay = 100, sustain = 0.5, release = 200
    })
    if t then
        t:play()
        demo_tones[#demo_tones + 1] = t
        status.text = "ADSR envelope"
    end
end)

local btn_sweep = fx_row:Button { w = lvgl.PCT(45), h = 30 }
btn_sweep:Label { text = "Sweep", align = lvgl.ALIGN.CENTER }
btn_sweep:onClicked(function()
    local t = sound.generateTone(200, 500, {
        end_freq = 2000, sweep = "exp"
    })
    if t then
        t:play()
        demo_tones[#demo_tones + 1] = t
        status.text = "Exp sweep"
    end
end)

local btn_bell = fx_row:Button { w = lvgl.PCT(45), h = 30 }
btn_bell:Label { text = "Bell", align = lvgl.ALIGN.CENTER }
btn_bell:onClicked(function()
    local t = sound.generateTone(440, 1000, {
        fm_ratio = 1.4, fm_index = 5,
        attack = 5, decay = 200, sustain = 0.2, release = 300
    })
    if t then
        t:play()
        demo_tones[#demo_tones + 1] = t
        status.text = "FM bell"
    end
end)

local btn_epiano = fx_row:Button { w = lvgl.PCT(45), h = 30 }
btn_epiano:Label { text = "EPiano", align = lvgl.ALIGN.CENTER }
btn_epiano:onClicked(function()
    local t = sound.generateTone(440, 800, {
        fm_ratio = 1.0, fm_index = 1.5,
        attack = 10, decay = 150, sustain = 0.4, release = 200
    })
    if t then
        t:play()
        demo_tones[#demo_tones + 1] = t
        status.text = "FM e-piano"
    end
end)

-- ── Chord / Melody ────────────────────────────────────────────────────────────
content:Label { text = "-- Chord / Melody --", w = lvgl.PCT(100), h = 16 }

local cm_row = content:Object {
    flex = { flex_direction = "row", flex_wrap = "wrap" },
    w = lvgl.PCT(100),
    h = lvgl.SIZE_CONTENT,
    border_width = 0,
    pad_all = 0,
}
cm_row:clear_flag(lvgl.FLAG.SCROLLABLE)

local btn_chord = cm_row:Button { w = lvgl.PCT(45), h = 30 }
btn_chord:Label { text = "Chord", align = lvgl.ALIGN.CENTER }
btn_chord:onClicked(function()
    local t = sound.generateChord({262, 330, 392}, 600, {
        attack = 20, decay = 100, sustain = 0.5, release = 150
    })
    if t then
        t:play()
        demo_tones[#demo_tones + 1] = t
        status.text = "C major chord"
    end
end)

local btn_melody = cm_row:Button { w = lvgl.PCT(45), h = 30 }
btn_melody:Label { text = "Melody", align = lvgl.ALIGN.CENTER }
btn_melody:onClicked(function()
    local t = sound.generateMelody({
        {freq=523, ms=150}, {freq=0, ms=30},
        {freq=659, ms=150}, {freq=0, ms=30},
        {freq=784, ms=150}, {freq=0, ms=30},
        {freq=1047, ms=300},
    }, { attack = 5, decay = 30, sustain = 0.6, release = 30 })
    if t then
        t:play()
        demo_tones[#demo_tones + 1] = t
        status.text = "C-E-G-C melody"
    end
end)

-- Cleanup on back
back_btn:onClicked(function()
    utils.loadingPopUpAdd(nil, "Home", function()
        if test_tone then test_tone:delete(); test_tone = nil end
        if test_file then test_file:delete(); test_file = nil end
        for _, t in ipairs(demo_tones) do t:delete() end
        demo_tones = {}
        root:delete()
        local launcher = require("launcher")
        launcher.create()
        return true
    end)
end)

return root
