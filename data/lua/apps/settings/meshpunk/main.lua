-- Settings App for MeshPunk
-- Node name, storage toggle, radio info

local lvgl = require("lvgl")
local clock_fmt_mod = require("lib/clock_fmt")

local ok2, storage = pcall(_storage_get_info)
if not ok2 or not storage then
    storage = { type = "?", sd_available = false, use_sd = false }
end

-- Root
local root = lvgl.Object()
root:set {
    w = lvgl.HOR_RES(),
    h = lvgl.VER_RES(),
    pad_all = 0,
    border_width = 0,
}
root:clear_flag(lvgl.FLAG.SCROLLABLE)

-- Scrollable content area
local content = root:Object {
    flex = {
        flex_direction = "column",
        flex_wrap = "nowrap",
    },
    w = lvgl.HOR_RES(),
    h = lvgl.VER_RES(),
    y = 0,
    border_width = 0,
    pad_all = 6,
}

-- Title row
local title_row = content:Object {
    w = lvgl.PCT(100),
    h = 26,
    border_width = 0,
    pad_all = 0,
}
title_row:clear_flag(lvgl.FLAG.SCROLLABLE)

title_row:Label {
    text = "Firmware Settings",
    align = lvgl.ALIGN.LEFT_MID,
}

local back_btn = title_row:Button { w = 50, h = 22, align = lvgl.ALIGN.RIGHT_MID }
back_btn:Label { text = "Back", align = lvgl.ALIGN.CENTER }
back_btn:onClicked(function()
    root:delete()
    local launcher = require("launcher")
    launcher.create()
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

local sd_row = content:Object {
    flex = {
        flex_direction = "row",
        flex_wrap = "nowrap",
    },
    w = lvgl.PCT(100),
    h = 34,
    border_width = 0,
    pad_all = 0,
}
sd_row:clear_flag(lvgl.FLAG.SCROLLABLE)

local function get_toggle_text()
    if not storage.sd_available then
        return "[ ] Use SD (no card)"
    elseif sd_enabled then
        return "[x] Use SD card"
    else
        return "[ ] Use SD card"
    end
end

local sd_toggle_btn = sd_row:Button { w = lvgl.PCT(65), h = 30 }
local sd_toggle_label = sd_toggle_btn:Label { text = get_toggle_text(), align = lvgl.ALIGN.CENTER }

local apply_btn = sd_row:Button { w = lvgl.PCT(30), h = 30 }
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

local tz_row = content:Object {
    flex = { flex_direction = "row", flex_wrap = "nowrap" },
    w = lvgl.PCT(100), h = 34, border_width = 0, pad_all = 0,
}
tz_row:clear_flag(lvgl.FLAG.SCROLLABLE)

local tz_input = tz_row:Textarea {
    password_mode = false,
    one_line = true,
    text = (function()
        local ok_g, s = pcall(_rtc_tz_get)
        return ok_g and s or "auto"
    end)(),
    w = lvgl.PCT(65), h = 30,
}

local tz_save_btn = tz_row:Button { w = lvgl.PCT(30), h = 30 }
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

-- Quick-set shortcuts
local tz_quick = content:Object {
    flex = { flex_direction = "row", flex_wrap = "wrap" },
    w = lvgl.PCT(100), h = 34, border_width = 0, pad_all = 0,
}
tz_quick:clear_flag(lvgl.FLAG.SCROLLABLE)

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
    local b = tz_quick:Button { w = 52, h = 28 }
    b:Label { text = p.label, align = lvgl.ALIGN.CENTER }
    b:onClicked(function()
        tz_input.text = p.value
        apply_tz(p.value)
    end)
end

-- ── Section: Clock Format ──
content:Label { text = "-- Clock Format --", w = lvgl.PCT(100), h = 16 }

local clock_fmt = clock_fmt_mod.get()

local fmt_row = content:Object {
    flex = { flex_direction = "row", flex_wrap = "nowrap" },
    w = lvgl.PCT(100), h = 34, border_width = 0, pad_all = 0,
}
fmt_row:clear_flag(lvgl.FLAG.SCROLLABLE)

local btn_12 = fmt_row:Button { w = lvgl.PCT(48), h = 30 }
local lbl_12 = btn_12:Label { align = lvgl.ALIGN.CENTER }
local btn_24 = fmt_row:Button { w = lvgl.PCT(48), h = 30 }
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

return root
