-- Tools > USB — USB-OTG host manager.
-- Puts the T-Deck's USB port in host mode to drive a USB device. Today: a
-- USB-C audio dongle, with all device audio (tones, notifications, music,
-- game audio) routed to it. The dongle needs external VBUS: power source ->
-- powered adapter/hub -> dongle -> T-Deck.
-- Host mode takes the USB pins from Serial-JTAG, so USB serial (logs + PC
-- file transfer) is DEAD while it runs; Stop restores it (a boot-time
-- self-heal covers resets mid-session).
local lvgl  = require("lvgl")
local apps  = require("lib/apps")
local nav   = require("lib/nav")
local theme = require("lib/theme")

local root = apps.new_root()
root:set { w = lvgl.HOR_RES(), h = lvgl.VER_RES(), pad_all = 0, border_width = 0, bg_opa = 0 }
root:clear_flag(lvgl.FLAG.SCROLLABLE)

theme.show_background()

local W = lvgl.HOR_RES()
local H = lvgl.VER_RES()

local content = root:Object {
    flex = { flex_direction = "row", flex_wrap = "wrap" },
    w = W, h = H,
    border_width = 0, pad_all = 6, pad_row = 4, bg_opa = 0,
}
nav.replace(content, { flags = nav.ROLLOVER + nav.SCROLL_FIRST })

content:Label { text = "USB Host", w = lvgl.PCT(70), h = 26 }
local back_btn = content:Button { w = 50, h = 22 }
back_btn:Label { text = "Home", align = lvgl.ALIGN.CENTER }
back_btn:onClicked(function() apps.go_home() end)

-- Firmware without the USB manager bridge: hint instead of erroring.
if not (_usb_start and _usb_poll) then
    content:Label {
        text = "This firmware build has no USB\nmanager support - rebuild needed.",
        w = lvgl.PCT(100), h = 40,
    }
    return root
end

local status  = content:Label { text = "Idle. Start switches USB to host mode\n(USB serial off until Stop).", w = lvgl.PCT(100), h = 32 }
local dev_lbl  = content:Label { text = "No device.", w = lvgl.PCT(100), h = 16 }

local start_btn = content:Button { w = lvgl.PCT(48), h = 28 }
start_btn:Label { text = "Start", align = lvgl.ALIGN.CENTER }
local stop_btn = content:Button { w = lvgl.PCT(48), h = 28 }
stop_btn:Label { text = "Stop", align = lvgl.ALIGN.CENTER }

-- Persisted toggles. Label reflects the pref; tap flips it.
local route_btn = content:Button { w = lvgl.PCT(100), h = 28 }
local route_lbl = route_btn:Label { text = "", align = lvgl.ALIGN.CENTER }
local spk_btn = content:Button { w = lvgl.PCT(100), h = 28 }
local spk_lbl = spk_btn:Label { text = "", align = lvgl.ALIGN.CENTER }

local function refresh_toggles()
    route_lbl:set { text = "Route audio to USB: " .. (_usb_audio_get() and "ON" or "off") }
    spk_lbl:set   { text = "Speaker stays on: " .. (_usb_speaker_get() and "ON" or "off") }
end
refresh_toggles()

route_btn:onClicked(function()
    _usb_audio_set(not _usb_audio_get())
    refresh_toggles()
end)
spk_btn:onClicked(function()
    _usb_speaker_set(not _usb_speaker_get())
    refresh_toggles()
end)

-- Debug: 440Hz sine straight into the dongle (bypasses the mixer). Gentle
-- amplitude, but don't press it with earbuds already in your ears.
local tone_btn = content:Button { w = lvgl.PCT(100), h = 26 }
tone_btn:Label { text = "Debug tone (mind your ears)", align = lvgl.ALIGN.CENTER }

-- Log panel: newest lines PREPENDED so results are visible without scrolling.
local log_panel = content:Object {
    w = lvgl.PCT(100), h = 74,
    bg_color = "#111111", bg_opa = 255, radius = 4,
    border_width = 1, border_color = "#444444", pad_all = 4,
}
local log_lbl = log_panel:Label { text = "(no output yet)", w = lvgl.PCT(100), h = lvgl.SIZE_CONTENT }

local heap_lbl = content:Label { text = "", w = lvgl.PCT(100), h = 16 }
-- Sound-pump debug line (wedge hunt): live even with the host off, and the
-- only view into the pump when host mode has USB serial disabled.
local snd_lbl = content:Label { text = "", w = lvgl.PCT(100), h = 16 }

local MAX_LINES = 40
local lines = {}
local function push_line(s)
    table.insert(lines, 1, s)
    if #lines > MAX_LINES then table.remove(lines) end
    log_lbl:set { text = table.concat(lines, "\n") }
end

start_btn:onClicked(function()
    if _usb_running() then status.text = "Already running."; return end
    local ok = _usb_start()
    status.text = ok and "Host starting (watch the log)...\nPlug in: power > adapter > dongle > T-Deck"
                     or "Start FAILED - see log."
end)

stop_btn:onClicked(function()
    if not _usb_running() then status.text = "Not running."; return end
    _usb_stop()
    status.text = "Stopping... USB serial should return\n(replug PC cable if needed)."
end)

tone_btn:onClicked(function()
    if not _usb_running() then status.text = "Start host mode first."; return end
    local on = _usb_tone()
    status.text = on and "Debug tone ON - listen for 440Hz." or "Debug tone off."
end)

-- Drain the C-side log ring; cheap when idle (one nil poll).
apps.add_timer { period = 200, cb = function()
    local n = 0
    while n < 20 do
        local line = _usb_poll()
        if not line then break end
        push_line(line)
        n = n + 1
    end
end }

-- Device + heap status. Internal SRAM is the scarce pool the host stack uses.
apps.add_timer { period = 1000, cb = function()
    local d = _usb_device()
    if d then
        local s = string.format("%s [%s]", (d.product ~= "" and d.product) or "device", d.kind)
        if d.kind == "audio" and d.rate > 0 then
            s = s .. string.format("  %dHz/%dbit%s", d.rate, d.bits,
                                   d.streaming and "  >>USB" or "")
        end
        if d.kbd then s = s .. "  [kbd]" end
        if d.msc then
            if d.msc_mb >= 1024 then
                s = s .. string.format("  [U: %.1fGB]", d.msc_mb / 1024)
            else
                s = s .. string.format("  [U: %dMB]", math.floor(d.msc_mb))
            end
        elseif d.msc_mb and d.msc_mb > 0 then
            s = s .. "  [storage: not mounted]"
        end
        dev_lbl.text = s
    elseif _usb_running() then
        dev_lbl.text = "Host on. Waiting for device..."
    else
        dev_lbl.text = "No device."
    end
    if _heap_info then
        local _, _, int_free, int_big = _heap_info()
        heap_lbl.text = string.format("int RAM %dK (big %dK)  host: %s",
            math.floor(int_free / 1024), math.floor(int_big / 1024),
            _usb_running() and "ON" or "off")
    end
    if _sound_debug then
        local d = _sound_debug()
        snd_lbl.text = string.format("snd %s d:%d m:%d st:%d pl:%d ib:%d p:%d/%d o:%d",
            d.running and "RUN" or "idle", d.dec, d.mix, d.staged, d.played,
            d.inbuff, d.pos, d.dur, d.objs)
    end
end }

return root
