-- Settings App for MeshPunk
-- Node name, storage toggle, radio info

local lvgl = require("lvgl")
local clock_fmt_mod = require("lib/clock_fmt")
local utils = require("lib/utils")
local apps = require("lib/apps")
local nav = require("lib/nav")
local theme = require("lib/theme")

local ok2, storage = pcall(_storage_get_info)
if not ok2 or not storage then
    storage = { type = "?", sd_available = false, use_sd = false }
end

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

-- Scrollable content area
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
content:Label { text = "Firmware Settings", w = lvgl.PCT(70), h = 26 }
local back_btn = content:Button { w = 50, h = 22 }
back_btn:Label { text = "Home", align = lvgl.ALIGN.CENTER }
back_btn:onClicked(function()
    apps.go_home()
end)

-- Status line
local status_label = content:Label {
    text = "",
    w = lvgl.PCT(100),
    h = 16,
}

-- ── Section: Storage ──
content:Label { text = "-- Storage --", w = lvgl.PCT(100), h = 16 }

-- Current storage info
local storage_info_label = content:Label {
    text = "Active: " .. (storage.type or "?") ..
           (storage.sd_available and " (SD available)" or " (no SD card)"),
    w = lvgl.PCT(100),
    h = 16,
}

-- SD card toggle button (acts as checkbox)
local sd_enabled = storage.use_sd

local function get_toggle_text()
    if not storage.sd_available then
        return "[ ] Use SD (no card)"
    elseif sd_enabled then
        return "[x] Use SD card"
    else
        return "[ ] Use SD card"
    end
end

local sd_toggle_btn = content:Button { w = lvgl.PCT(65), h = 30 }
local sd_toggle_label = sd_toggle_btn:Label { text = get_toggle_text(), align = lvgl.ALIGN.CENTER }

local apply_btn = content:Button { w = lvgl.PCT(30), h = 30 }
apply_btn:Label { text = "Apply", align = lvgl.ALIGN.CENTER }

sd_toggle_btn:onClicked(function()
    if not storage.sd_available then
        status_label.text = "No SD card inserted"
        return
    end
    sd_enabled = not sd_enabled
    sd_toggle_label.text = get_toggle_text()
end)

apply_btn:onClicked(function()
    if sd_enabled and not storage.sd_available then
        status_label.text = "No SD card inserted"
        return
    end

    local ok3, err = pcall(_storage_set_use_sd, sd_enabled)
    if ok3 then
        -- Re-read storage info
        local ok4, new_storage = pcall(_storage_get_info)
        if ok4 and new_storage then
            storage = new_storage
            storage_info_label.text = "Active: " .. (storage.type or "?") ..
                (storage.sd_available and " (SD available)" or " (no SD card)")
        end
        if sd_enabled then
            status_label.text = "Switched to SD card"
        else
            status_label.text = "Switched to LittleFS"
        end
    else
        status_label.text = "Error: " .. tostring(err)
    end
end)

-- ── Section: Message History ──
content:Label { text = "-- Message History --", w = lvgl.PCT(100), h = 16 }
content:Label { text = "Days kept (0 = forever):", w = lvgl.PCT(100), h = 16 }

local retain_days = (function()
    local ok_r, d = pcall(_msg_retain_get)
    return (ok_r and d) or 30
end)()

local retain_input = content:Textarea {
    one_line = true, text = tostring(retain_days),
    accepted_chars = "0123456789", w = lvgl.PCT(40), h = 30,
}
retain_input:clear_flag(lvgl.FLAG.SCROLLABLE)

local retain_save_btn = content:Button { w = lvgl.PCT(30), h = 30 }
retain_save_btn:Label { text = "Save", align = lvgl.ALIGN.CENTER }
retain_save_btn:onClicked(function()
    local n = tonumber(retain_input.text)
    if not n or n < 0 then
        status_label.text = "Days: enter 0 or more"
        return
    end
    n = math.floor(n)
    local ok_s = pcall(_msg_retain_set, n)
    if ok_s then
        retain_input.text = tostring(n)
        status_label.text = (n == 0) and "History: kept forever"
                                      or ("History: " .. n .. " days")
    else
        status_label.text = "Failed to save retention"
    end
end)

-- ── Section: Time Zone ──
content:Label { text = "-- Time Zone --", w = lvgl.PCT(100), h = 16 }

local function format_offset(mins)
    local sign = (mins < 0) and "-" or "+"
    local a = math.abs(mins)
    return string.format("%s%02d:%02d", sign, math.floor(a / 60), a % 60)
end

local function describe_tz()
    local ok_g, setting = pcall(_rtc_tz_get)
    local ok_o, off = pcall(_rtc_tz_offset_minutes)
    setting = ok_g and setting or "auto"
    off = ok_o and off or 0
    if setting == "auto" then
        return "Current: auto (" .. format_offset(off) .. ")"
    else
        return "Current: " .. format_offset(off) .. " (" .. setting .. " min)"
    end
end

local tz_info_label = content:Label { text = describe_tz(), w = lvgl.PCT(100), h = 16 }

local tz_input = content:Textarea {
    password_mode = false,
    one_line = true,
    text = (function()
        local ok_g, s = pcall(_rtc_tz_get)
        return ok_g and s or "auto"
    end)(),
    w = lvgl.PCT(65), h = 30,
}
tz_input:clear_flag(lvgl.FLAG.SCROLLABLE)

local tz_save_btn = content:Button { w = lvgl.PCT(30), h = 30 }
tz_save_btn:Label { text = "Save", align = lvgl.ALIGN.CENTER }

local function apply_tz(value)
    local arg
    if value == "auto" then
        arg = "auto"
    else
        local n = tonumber(value)
        if not n then
            status_label.text = "TZ: enter 'auto' or minutes (e.g. -300)"
            return
        end
        arg = math.floor(n)
    end
    local ok_s, applied = pcall(_rtc_tz_set, arg)
    if ok_s and applied then
        tz_info_label.text = describe_tz()
        status_label.text = "TZ saved: " .. tostring(arg)
    else
        status_label.text = "TZ: invalid value (range: -840..840)"
    end
end

tz_save_btn:onClicked(function() apply_tz(tz_input.text) end)
tz_input:onevent(lvgl.EVENT.KEY, function(obj, code)
    local indev = lvgl.indev.get_act()
    if indev:get_key() == lvgl.KEY.ENTER then apply_tz(tz_input.text) end
end)

-- Quick-set shortcuts (direct children of content)
local presets = {
    { label = "Auto", value = "auto" },
    { label = "UTC",  value = "0" },
    { label = "PT",   value = "-480" },
    { label = "MT",   value = "-420" },
    { label = "CT",   value = "-360" },
    { label = "ET",   value = "-300" },
    { label = "CET",  value = "60" },
    { label = "IN",   value = "330" },
    { label = "JP",   value = "540" },
}
for _, p in ipairs(presets) do
    local b = content:Button { w = 52, h = 28 }
    b:Label { text = p.label, align = lvgl.ALIGN.CENTER }
    b:onClicked(function()
        tz_input.text = p.value
        apply_tz(p.value)
    end)
end

-- ── Section: Clock Format ──
content:Label { text = "-- Clock Format --", w = lvgl.PCT(100), h = 16 }

local clock_fmt = clock_fmt_mod.get()

local btn_12 = content:Button { w = lvgl.PCT(48), h = 30 }
local lbl_12 = btn_12:Label { align = lvgl.ALIGN.CENTER }
local btn_24 = content:Button { w = lvgl.PCT(48), h = 30 }
local lbl_24 = btn_24:Label { align = lvgl.ALIGN.CENTER }

local function refresh_fmt_labels()
    lbl_12.text = (clock_fmt == "12") and "[x] 12-hour" or "[ ] 12-hour"
    lbl_24.text = (clock_fmt == "24") and "[x] 24-hour" or "[ ] 24-hour"
end
refresh_fmt_labels()

btn_12:onClicked(function()
    clock_fmt = "12"
    status_label.text = clock_fmt_mod.set("12") and "Clock: 12-hour" or "Clock: save failed"
    refresh_fmt_labels()
end)
btn_24:onClicked(function()
    clock_fmt = "24"
    status_label.text = clock_fmt_mod.set("24") and "Clock: 24-hour" or "Clock: save failed"
    refresh_fmt_labels()
end)

-- ── DST toggle ──
local dst_on = pcall(_dst_get) and _dst_get()
local dst_btn = content:Button { w = lvgl.PCT(60), h = 30 }
local dst_label = dst_btn:Label {
    text = dst_on and "[x] Daylight Saving" or "[ ] Daylight Saving",
    align = lvgl.ALIGN.CENTER,
}
dst_btn:onClicked(function()
    dst_on = not dst_on
    pcall(_dst_set, dst_on)
    dst_label.text = dst_on and "[x] Daylight Saving" or "[ ] Daylight Saving"
    status_label.text = dst_on and "DST: +1 hour" or "DST: off"
    tz_info_label.text = describe_tz()
end)

-- ── Section: Manual Time ──
content:Label { text = "-- Manual Time --", w = lvgl.PCT(100), h = 16 }

content:Label {
    text = "Set time (lost on reboot):",
    w = lvgl.PCT(100), h = 16,
}

local function get_current_local()
    local ok_t, epoch = pcall(_rtc_time)
    local ok_o, off = pcall(_rtc_tz_offset_minutes)
    epoch = (ok_t and epoch or 0) + ((ok_o and off or 0) * 60)
    return os.date("!*t", epoch)
end

local now = get_current_local()

content:Label { text = "Y:", w = 16, h = 26 }
local mt_y = content:Textarea {
    one_line = true, text = tostring(now.year),
    accepted_chars = "0123456789", w = 48, h = 26,
}
mt_y:clear_flag(lvgl.FLAG.SCROLLABLE)

content:Label { text = "M:", w = 18, h = 26 }
local mt_m = content:Textarea {
    one_line = true, text = string.format("%02d", now.month),
    accepted_chars = "0123456789", w = 30, h = 26,
}
mt_m:clear_flag(lvgl.FLAG.SCROLLABLE)

content:Label { text = "D:", w = 16, h = 26 }
local mt_d = content:Textarea {
    one_line = true, text = string.format("%02d", now.day),
    accepted_chars = "0123456789", w = 30, h = 26,
}
mt_d:clear_flag(lvgl.FLAG.SCROLLABLE)

content:Label { text = "H:", w = 16, h = 26 }
local mt_h = content:Textarea {
    one_line = true, text = string.format("%02d", now.hour),
    accepted_chars = "0123456789", w = 30, h = 26,
}
mt_h:clear_flag(lvgl.FLAG.SCROLLABLE)

content:Label { text = "Mi:", w = 20, h = 26 }
local mt_mi = content:Textarea {
    one_line = true, text = string.format("%02d", now.min),
    accepted_chars = "0123456789", w = 30, h = 26,
}
mt_mi:clear_flag(lvgl.FLAG.SCROLLABLE)

local mt_override_active = pcall(_rtc_manual_override_get) and _rtc_manual_override_get()
local mt_override_label = content:Label {
    text = mt_override_active
           and "Override: ON (GPS disabled)"
           or  "Override: OFF (GPS active)",
    w = lvgl.PCT(100), h = 16,
}

local mt_set_btn = content:Button { w = lvgl.PCT(45), h = 30 }
mt_set_btn:Label { text = "Set Time", align = lvgl.ALIGN.CENTER }

local mt_clear_btn = content:Button { w = lvgl.PCT(45), h = 30 }
mt_clear_btn:Label { text = "Use GPS", align = lvgl.ALIGN.CENTER }

mt_set_btn:onClicked(function()
    local y = tonumber(mt_y.text)
    local m = tonumber(mt_m.text)
    local d = tonumber(mt_d.text)
    local h = tonumber(mt_h.text)
    local mi = tonumber(mt_mi.text)
    if not y or not m or not d or not h or not mi then
        status_label.text = "Fill all time fields"
        return
    end
    if y < 2024 or y > 2099 then
        status_label.text = "Year: 2024-2099"
        return
    end
    if m < 1 or m > 12 or d < 1 or d > 31 or h < 0 or h > 23 or mi < 0 or mi > 59 then
        status_label.text = "Invalid date/time value"
        return
    end
    local local_epoch = os.time({ year = y, month = m, day = d, hour = h, min = mi, sec = 0 })
    local ok_o, off = pcall(_rtc_tz_offset_minutes)
    local tz_off = (ok_o and off or 0) * 60
    local utc_epoch = local_epoch - tz_off

    local ok, result = pcall(_rtc_set_time, utc_epoch)
    if ok and result then
        status_label.text = "Time set (lost on reboot)"
        mt_override_label.text = "Override: ON (GPS disabled)"
    else
        status_label.text = "Failed to set time"
    end
end)

mt_clear_btn:onClicked(function()
    local ok, result = pcall(_rtc_manual_override_clear)
    if ok then
        status_label.text = "GPS time re-enabled"
        mt_override_label.text = "Override: OFF (GPS active)"
    else
        status_label.text = "Error clearing override"
    end
end)

-- ── Section: GPS Time ──
content:Label { text = "-- GPS Time --", w = lvgl.PCT(100), h = 16 }

local gps_info_label = content:Label {
    text = "Auto-refreshes every 5 min",
    w = lvgl.PCT(100), h = 16,
}

local gps_btn = content:Button { w = lvgl.PCT(60), h = 30 }
gps_btn:Label { text = "Get GPS Time", align = lvgl.ALIGN.CENTER }

gps_btn:onClicked(function()
    local ok, started = pcall(_gps_sync_start)
    if ok and started then
        status_label.text = "GPS sync started"
        gps_info_label.text = "Syncing - see GPS Status below"
    elseif ok then
        status_label.text = "GPS sync already in progress"
    else
        status_label.text = "GPS sync error"
    end
end)

-- ── Section: GPS Status ──
-- Live view of the sync cycle (states from _gps_state; a location hunt can run
-- up to 2 min, so this refreshes on a managed 1s timer instead of a popup).
content:Label { text = "-- GPS Status --", w = lvgl.PCT(100), h = 16 }

local gps_state_label = content:Label { text = "...", w = lvgl.PCT(100), h = 16 }
local gps_loc_label   = content:Label { text = "...", w = lvgl.PCT(100), h = 16 }

local last_gps_state = nil

local function refresh_gps_status()
    local ok, state, elapsed, hunt_s, budget_s, time_fix, loc_fix = pcall(_gps_state)
    if not ok then
        gps_state_label.text = "GPS: status unavailable"
        gps_loc_label.text = ""
        return
    end
    local ok2, _, _, has_loc, lat, lng, sats = pcall(_gps_info)
    if not ok2 then has_loc = false; sats = 0 end

    local line
    if state == 0 then
        local outcome
        if loc_fix then
            outcome = "time + location"
        elseif time_fix then
            outcome = "time only"
        else
            outcome = "no fix"
        end
        line = "Asleep (5 min cycle) - last sync: " .. outcome
    elseif state == 1 then
        line = "Syncing: finding baud rate... " .. elapsed .. "s"
    elseif state == 2 then
        line = "Syncing: waiting for time... " .. elapsed .. "s"
    elseif state == 3 then
        line = "Hunting location fix... " .. hunt_s .. "s/" .. budget_s .. "s"
        if sats and sats > 0 then
            line = line .. " (" .. sats .. " sats)"
        end
    else
        line = "Location acquired, finishing..."
    end
    gps_state_label.text = line

    if has_loc then
        local loc_line = string.format("Last loc: %.4f, %.4f", lat, lng)
        if sats and sats > 0 then
            loc_line = loc_line .. " - " .. sats .. " sats"
        end
        gps_loc_label.text = loc_line
    else
        gps_loc_label.text = "No location yet"
    end

    -- Cycle just finished: refresh the labels the old loading popup used to set.
    if last_gps_state and last_gps_state ~= 0 and state == 0 then
        tz_info_label.text = describe_tz()
        if loc_fix then
            gps_info_label.text = "Last sync: got time + location"
        elseif time_fix then
            gps_info_label.text = "Last sync: got time, no location"
        else
            gps_info_label.text = "Last sync: no fix (timeout)"
        end
    end
    last_gps_state = state
end

refresh_gps_status()
apps.add_timer { period = 1000, cb = refresh_gps_status }

return root
