-- Tools > USB Drive — share the SD card with a PC over USB.
-- The deck enumerates as a USB thumb drive backed by the SD card's raw
-- sectors (MSC device mode, usb_msc_dev.cpp). While sharing, the PC owns the
-- card exclusively: firmware apps see it as absent, the mesh radio pauses
-- (nothing may write the volume the PC owns), and USB serial is off (the OTG
-- side owns the pins). Stop — or closing this app by any path — disconnects,
-- remounts the card and resumes the mesh; the C-side watchdog force-stops if
-- this app dies without cleanup. After a session USB HOST mode (Tools > USB Host)
-- needs a reboot: the device stack has no uninstall.
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

content:Label { text = "USB Drive", w = lvgl.PCT(70), h = 26 }
local back_btn = content:Button { w = 50, h = 22 }
back_btn:Label { text = "Home", align = lvgl.ALIGN.CENTER }
back_btn:onClicked(function() apps.go_home() end)

-- Firmware without the drive-mode bridge: hint instead of erroring.
if not (_usbdrive_start and _usbdrive_status) then
    content:Label {
        text = "This firmware build has no USB\ndrive support - update needed.",
        w = lvgl.PCT(100), h = 40,
    }
    return root
end

local function fmt_bytes(n)
    n = tonumber(n) or 0
    if n >= 1024 * 1024 * 1024 then return string.format("%.1f GB", n / (1024 * 1024 * 1024)) end
    if n >= 1024 * 1024 then return string.format("%.1f MB", n / (1024 * 1024)) end
    return string.format("%.0f KB", n / 1024)
end

local status = content:Label {
    text = "Shares the SD card with a PC as a\nUSB thumb drive (~1 MB/s).\nApps lose the card and the mesh\npauses while sharing.",
    w = lvgl.PCT(100), h = 64,
}
local card_lbl = content:Label { text = "", w = lvgl.PCT(100), h = 16 }
local xfer_lbl = content:Label { text = "", w = lvgl.PCT(100), h = 16 }

local function refresh_card()
    local total, used = _fs_df("S")
    if total then
        card_lbl:set { text = "Card: " .. fmt_bytes(total) .. ", " .. fmt_bytes(used) .. " used" }
    else
        card_lbl:set { text = "No SD card." }
    end
end
refresh_card()

local start_btn = content:Button { w = lvgl.PCT(48), h = 28 }
start_btn:Label { text = "Start sharing", align = lvgl.ALIGN.CENTER }
local stop_btn = content:Button { w = lvgl.PCT(48), h = 28 }
stop_btn:Label { text = "Stop", align = lvgl.ALIGN.CENTER }

content:Label {
    text = "Tip: copy internal files to SD in\nTools > Files to share them.",
    w = lvgl.PCT(100), h = 32,
}

-- One status refresher doubles as the C watchdog ping. Runs always; cheap
-- when idle.
local function refresh()
    local st = _usbdrive_status()
    if st.active then
        _usbdrive_ping()
        local conn = st.connected and "connected to PC"
                  or (st.ejected and "ejected by PC - safe to stop" or "waiting for PC...")
        status:set { text = "Sharing SD card: " .. conn ..
            "\nMesh paused. USB serial off.\nEject on the PC before Stop." }
        xfer_lbl:set { text = string.format("%s transferred (%d rd / %d wr)",
            fmt_bytes(st.bytes), st.reads, st.writes) }
    end
    return st
end

apps.add_timer { period = 500, cb = refresh }

start_btn:onClicked(function()
    local st = _usbdrive_status()
    if st.active then return end
    -- Music (or any background app) may hold SD files open — close them all
    -- before the card is handed to the PC.
    apps.close_all_backgrounds()
    if _usbdrive_start() then
        refresh()
    else
        local why = _usbdrive_status().fail
        status:set { text = "Can't start: " .. (why ~= "" and why or "unknown") ..
            (why == "USB host mode is running" and "\nStop it in Tools > USB Host first." or "") }
    end
end)

local function stop_sharing()
    if not _usbdrive_status().active then return false end
    _usbdrive_stop()
    apps.refresh()   -- re-scan S: app listings after the remount
    return true
end

stop_btn:onClicked(function()
    if stop_sharing() then
        local st = _usbdrive_status()
        status:set { text = "Stopped. SD card remounted." ..
            "\nUSB host mode needs a reboot." }
        xfer_lbl:set { text = string.format("Session: %s transferred", fmt_bytes(st.bytes)) }
        refresh_card()
    end
end)

-- Any close path (Home button, home chord): stop the session first. UI may
-- already be tearing down here — C call + registry refresh only, no labels.
apps.set_on_close(stop_sharing)

return root
