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
title_row:Label { text = "Keyboard Backlight", align = lvgl.ALIGN.LEFT_MID }
local back_btn = title_row:Button { w = 50, h = 22, align = lvgl.ALIGN.RIGHT_MID }
back_btn:Label { text = "Home", align = lvgl.ALIGN.CENTER }

local status = content:Label { text = "", w = lvgl.PCT(100), h = 16 }

content:Label { text = "-- Brightness --", w = lvgl.PCT(100), h = 16 }

local cur_val = _kbd_get_brightness()
local val_label = content:Label { text = "", w = lvgl.PCT(100), h = 16 }

-- 8-segment visual bar (each segment = 32 units)
local STEP = 32
local NUM_SEGS = 8
local bar_row = content:Object {
    flex = { flex_direction = "row", flex_wrap = "nowrap" },
    w = lvgl.PCT(100), h = 18, border_width = 0, pad_all = 0,
}
bar_row:clear_flag(lvgl.FLAG.SCROLLABLE)
local segs = {}
for i = 1, NUM_SEGS do
    segs[i] = bar_row:Object { w = 30, h = 14, border_width = 1, pad_all = 0 }
    segs[i]:clear_flag(lvgl.FLAG.SCROLLABLE)
    segs[i]:clear_flag(lvgl.FLAG.CLICKABLE)
end

local function refresh_ui()
    cur_val = _kbd_get_brightness()
    val_label.text = "Level: " .. cur_val .. "/255"
    local filled = math.floor(cur_val / (255 / NUM_SEGS) + 0.5)
    for i = 1, NUM_SEGS do
        segs[i]:set { bg_opa = (i <= filled) and 255 or 40 }
    end
end
refresh_ui()

local btn_row = content:Object {
    flex = { flex_direction = "row", flex_wrap = "nowrap" },
    w = lvgl.PCT(100), h = 34, border_width = 0, pad_all = 0,
}
btn_row:clear_flag(lvgl.FLAG.SCROLLABLE)

local btn_dn = btn_row:Button { w = lvgl.PCT(45), h = 30 }
btn_dn:Label { text = "Light -", align = lvgl.ALIGN.CENTER }
local btn_up = btn_row:Button { w = lvgl.PCT(45), h = 30 }
btn_up:Label { text = "Light +", align = lvgl.ALIGN.CENTER }

btn_dn:onClicked(function()
    local v = math.max(0, cur_val - STEP)
    _kbd_set_brightness(v)
    refresh_ui()
    status.text = "Brightness: " .. _kbd_get_brightness()
end)
btn_up:onClicked(function()
    local v = math.min(255, cur_val + STEP)
    _kbd_set_brightness(v)
    refresh_ui()
    status.text = "Brightness: " .. _kbd_get_brightness()
end)

back_btn:onClicked(function()
    utils.loadingPopUpAdd(nil, "Home", function()
        root:delete()
        local launcher = require("launcher")
        launcher.create()
        return true
    end)
end)

return root
