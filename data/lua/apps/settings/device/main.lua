local lvgl  = require("lvgl")
local utils = require("lib/utils")

local root = lvgl.Object()
root:set { w = lvgl.HOR_RES(), h = lvgl.VER_RES(), pad_all = 0, border_width = 0 }
root:clear_flag(lvgl.FLAG.SCROLLABLE)

local content = root:Object {
    flex = { flex_direction = "row", flex_wrap = "wrap" },
    w = lvgl.HOR_RES(), h = lvgl.VER_RES(),
    border_width = 0, pad_all = 6,
}
_nav_setup(content, GRIDNAV_ROLLOVER + GRIDNAV_SCROLL_FIRST)

-- Title
content:Label { text = "Device Settings", w = lvgl.PCT(70), h = 26 }
local back_btn = content:Button { w = 50, h = 22 }
back_btn:Label { text = "Home", align = lvgl.ALIGN.CENTER }

local status = content:Label { text = "", w = lvgl.PCT(100), h = 16 }

-- ── Display Brightness ───────────────────────────────────────────────────────
content:Label { text = "-- Screen --", w = lvgl.PCT(100), h = 16 }

local disp_val = _disp_get_brightness()
local disp_label = content:Label { text = "", w = lvgl.PCT(100), h = 16 }

local DISP_SEGS = 16
local disp_bar = content:Object {
    flex = { flex_direction = "row", flex_wrap = "nowrap" },
    w = lvgl.PCT(100), h = 18, border_width = 0, pad_all = 0,
}
disp_bar:clear_flag(lvgl.FLAG.SCROLLABLE)
disp_bar:clear_flag(lvgl.FLAG.CLICKABLE)
local dsegs = {}
for i = 1, DISP_SEGS do
    dsegs[i] = disp_bar:Object { w = 16, h = 14, border_width = 1, pad_all = 0, bg_color = "#24ba24"}
    dsegs[i]:clear_flag(lvgl.FLAG.SCROLLABLE)
    dsegs[i]:clear_flag(lvgl.FLAG.CLICKABLE)
end

local function refresh_disp()
    disp_val = _disp_get_brightness()
    disp_label.text = "Screen: " .. disp_val .. "/16"
    for i = 1, DISP_SEGS do
        dsegs[i]:set { bg_opa = (i <= disp_val) and 255 or 40 }
    end
end
refresh_disp()

local disp_dn = content:Button { w = lvgl.PCT(45), h = 30 }
disp_dn:Label { text = "Screen -", align = lvgl.ALIGN.CENTER }
local disp_up = content:Button { w = lvgl.PCT(45), h = 30 }
disp_up:Label { text = "Screen +", align = lvgl.ALIGN.CENTER }

disp_dn:onClicked(function()
    local v = math.max(1, disp_val - 1)
    _disp_set_brightness(v)
    refresh_disp()
    status.text = "Screen: " .. _disp_get_brightness() .. "/16"
end)
disp_up:onClicked(function()
    local v = math.min(16, disp_val + 1)
    _disp_set_brightness(v)
    refresh_disp()
    status.text = "Screen: " .. _disp_get_brightness() .. "/16"
end)

-- ── Screen Timeout ──────────────────────────────────────────────────────────
content:Label { text = "Timeout (sec, 0=never):", w = lvgl.PCT(100), h = 16 }

local scr_to_input = content:Textarea {
    password_mode = false, one_line = true,
    text = tostring(_screen_timeout_get()),
    w = lvgl.PCT(50), h = 30,
}
scr_to_input:clear_flag(lvgl.FLAG.SCROLLABLE)

local scr_to_btn = content:Button { w = lvgl.PCT(40), h = 30 }
scr_to_btn:Label { text = "Set", align = lvgl.ALIGN.CENTER }
scr_to_btn:onClicked(function()
    local v = tonumber(scr_to_input.text)
    if not v or v < 0 then
        status.text = "Enter a number >= 0"
        return
    end
    _screen_timeout_set(math.floor(v))
    status.text = "Screen timeout: " .. math.floor(v) .. "s"
end)

-- ── Keyboard Backlight ───────────────────────────────────────────────────────
content:Label { text = "-- Keyboard --", w = lvgl.PCT(100), h = 16 }

local kbd_val = _kbd_get_brightness()
local kbd_label = content:Label { text = "", w = lvgl.PCT(100), h = 16 }

local KBD_STEP = 32
local KBD_SEGS = 8
local kbd_bar = content:Object {
    flex = { flex_direction = "row", flex_wrap = "nowrap" },
    w = lvgl.PCT(100), h = 18, border_width = 0, pad_all = 0,
}
kbd_bar:clear_flag(lvgl.FLAG.SCROLLABLE)
kbd_bar:clear_flag(lvgl.FLAG.CLICKABLE)
local ksegs = {}
for i = 1, KBD_SEGS do
    ksegs[i] = kbd_bar:Object { w = 30, h = 14, border_width = 1, pad_all = 0 , bg_color = "#24ba24" }
    ksegs[i]:clear_flag(lvgl.FLAG.SCROLLABLE)
    ksegs[i]:clear_flag(lvgl.FLAG.CLICKABLE)
end

local function refresh_kbd()
    kbd_val = _kbd_get_brightness()
    kbd_label.text = "Keyboard: " .. kbd_val .. "/255"
    local filled = math.floor(kbd_val / (255 / KBD_SEGS) + 0.5)
    for i = 1, KBD_SEGS do
        ksegs[i]:set { bg_opa = (i <= filled) and 255 or 40 }
    end
end
refresh_kbd()

local kbd_dn = content:Button { w = lvgl.PCT(45), h = 30 }
kbd_dn:Label { text = "Light -", align = lvgl.ALIGN.CENTER }
local kbd_up = content:Button { w = lvgl.PCT(45), h = 30 }
kbd_up:Label { text = "Light +", align = lvgl.ALIGN.CENTER }

kbd_dn:onClicked(function()
    local v = math.max(0, kbd_val - KBD_STEP)
    _kbd_set_brightness(v)
    refresh_kbd()
    status.text = "Keyboard: " .. _kbd_get_brightness() .. "/255"
end)
kbd_up:onClicked(function()
    local v = math.min(255, kbd_val + KBD_STEP)
    _kbd_set_brightness(v)
    refresh_kbd()
    status.text = "Keyboard: " .. _kbd_get_brightness() .. "/255"
end)

-- ── Keyboard Timeout ────────────────────────────────────────────────────────
content:Label { text = "Timeout (sec, 0=never):", w = lvgl.PCT(100), h = 16 }

local kbd_to_input = content:Textarea {
    password_mode = false, one_line = true,
    text = tostring(_kbd_timeout_get()),
    w = lvgl.PCT(50), h = 30,
}
kbd_to_input:clear_flag(lvgl.FLAG.SCROLLABLE)

local kbd_to_btn = content:Button { w = lvgl.PCT(40), h = 30 }
kbd_to_btn:Label { text = "Set", align = lvgl.ALIGN.CENTER }
kbd_to_btn:onClicked(function()
    local v = tonumber(kbd_to_input.text)
    if not v or v < 0 then
        status.text = "Enter a number >= 0"
        return
    end
    _kbd_timeout_set(math.floor(v))
    status.text = "Kbd timeout: " .. math.floor(v) .. "s"
end)

-- ── Back ─────────────────────────────────────────────────────────────────────
back_btn:onClicked(function()
    utils.loadingPopUpAdd(nil, "Home", function()
        root:delete()
        local launcher = require("launcher")
        launcher.create()
        return true
    end)
end)

return root
