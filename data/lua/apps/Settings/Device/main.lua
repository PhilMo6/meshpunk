local lvgl   = require("lvgl")
local utils  = require("lib/utils")
local apps   = require("lib/apps")
local nav    = require("lib/nav")
local theme  = require("lib/theme")
local topbar = require("lib/topbar")

local root = apps.new_root()
root:set { w = lvgl.HOR_RES(), h = lvgl.VER_RES(), pad_all = 0, border_width = 0, bg_opa = 0 }
root:clear_flag(lvgl.FLAG.SCROLLABLE)

-- Themed wallpaper behind this (lightweight) screen; containers below are transparent.
theme.show_background()

local content = root:Object {
    flex = { flex_direction = "row", flex_wrap = "wrap" },
    w = lvgl.HOR_RES(), h = lvgl.VER_RES(),
    border_width = 0, pad_all = 6, bg_opa = 0,
}
nav.replace(content, { flags = nav.ROLLOVER + nav.SCROLL_FIRST })

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

-- Top bar: transparent (themed wallpaper shows behind the status bar) vs opaque
-- (solid themed panel). Applies live; visible on the home screen.
local topbar_transp_on = _topbar_transparant_get()
local function topbar_transp_text()
    return (topbar_transp_on and "[x]" or "[ ]") .. " Transparent top bar"
end
local topbar_btn = content:Button { w = lvgl.PCT(100), h = 30 }
local topbar_lbl = topbar_btn:Label { text = topbar_transp_text(), align = lvgl.ALIGN.LEFT_MID }
topbar_btn:onClicked(function()
    topbar_transp_on = not topbar_transp_on
    _topbar_transparant_set(topbar_transp_on)
    topbar.apply_transparency()
    topbar_lbl:set({ text = topbar_transp_text() })
    status.text = topbar_transp_on and "Top bar: transparent" or "Top bar: opaque"
end)

-- Selection highlight: a translucent "highlighted fill" wash (off) vs an opaque
-- solid block (on). Global — applies to every theme. Applies live.
local sel_solid_on = _theme_focus_solid_get()
local function sel_solid_text()
    return (sel_solid_on and "[x]" or "[ ]") .. " Solid selection highlight"
end
local sel_btn = content:Button { w = lvgl.PCT(100), h = 30 }
local sel_lbl = sel_btn:Label { text = sel_solid_text(), align = lvgl.ALIGN.LEFT_MID }
sel_btn:onClicked(function()
    sel_solid_on = not sel_solid_on
    _theme_focus_solid_set(sel_solid_on)
    sel_lbl:set({ text = sel_solid_text() })
    status.text = sel_solid_on and "Selection: solid" or "Selection: highlighted fill"
end)

-- Selection tint direction: brighten the selected item vs darken it. Global —
-- applies to every theme. Brighten can wash out light accents; darken is the fix.
local sel_darken_on = _theme_focus_darken_get()
local function sel_dir_text()
    return "Selection tint: " .. (sel_darken_on and "Darken" or "Brighten")
end
local seldir_btn = content:Button { w = lvgl.PCT(100), h = 30 }
local seldir_lbl = seldir_btn:Label { text = sel_dir_text(), align = lvgl.ALIGN.LEFT_MID }
seldir_btn:onClicked(function()
    sel_darken_on = not sel_darken_on
    _theme_focus_darken_set(sel_darken_on)
    seldir_lbl:set({ text = sel_dir_text() })
    status.text = sel_darken_on and "Selection: darken" or "Selection: brighten"
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

-- Sym key: hold modifier (default) vs tap-to-toggle the symbol layer.
-- Holding sym still works as a momentary modifier in toggle mode.
local sym_toggle_on = _kb_sym_toggle_get()
local function sym_toggle_text()
    return (sym_toggle_on and "[x]" or "[ ]") .. " Sym key tap toggles"
end
local sym_btn = content:Button { w = lvgl.PCT(100), h = 30 }
local sym_lbl = sym_btn:Label { text = sym_toggle_text(), align = lvgl.ALIGN.LEFT_MID }

sym_btn:onClicked(function()
    sym_toggle_on = not sym_toggle_on
    _kb_sym_toggle_set(sym_toggle_on)
    sym_lbl:set({ text = sym_toggle_text() })
    status.text = sym_toggle_on and "Sym: tap toggles symbol layer"
                                or "Sym: hold to use symbols"
end)

-- Alt key: hold modifier (default) vs tap-to-toggle the emoji layer (mirrors
-- the sym toggle above; per-key emoji live in Settings > Emoji). Guarded so
-- the page still loads on firmware without the emoji-layer bindings.
if _kb_alt_toggle_get then
    local alt_toggle_on = _kb_alt_toggle_get()
    local function alt_toggle_text()
        return (alt_toggle_on and "[x]" or "[ ]") .. " Alt key tap toggles emoji"
    end
    local alt_btn = content:Button { w = lvgl.PCT(100), h = 30 }
    local alt_lbl = alt_btn:Label { text = alt_toggle_text(), align = lvgl.ALIGN.LEFT_MID }

    alt_btn:onClicked(function()
        alt_toggle_on = not alt_toggle_on
        _kb_alt_toggle_set(alt_toggle_on)
        alt_lbl:set({ text = alt_toggle_text() })
        status.text = alt_toggle_on and "Alt: tap toggles emoji layer"
                                    or "Alt: hold to type emoji"
    end)
end

-- ── Trackball Sensitivity ────────────────────────────────────────────────────
content:Label { text = "-- Trackball --", w = lvgl.PCT(100), h = 16 }

local TRK_STEP = 25
local TRK_MAX  = 500
local TRK_SEGS = 20
local trk_val = _trackball_sensitivity_get()
local trk_label = content:Label { text = "", w = lvgl.PCT(100), h = 16 }

local trk_bar = content:Object {
    flex = { flex_direction = "row", flex_wrap = "nowrap" },
    w = lvgl.PCT(100), h = 18, border_width = 0, pad_all = 0,
    pad_column = 1,
}
trk_bar:clear_flag(lvgl.FLAG.SCROLLABLE)
trk_bar:clear_flag(lvgl.FLAG.CLICKABLE)
local tsegs = {}
for i = 1, TRK_SEGS do
    tsegs[i] = trk_bar:Object { w = 12, h = 14, border_width = 1, pad_all = 0, bg_color = "#24ba24" }
    tsegs[i]:clear_flag(lvgl.FLAG.SCROLLABLE)
    tsegs[i]:clear_flag(lvgl.FLAG.CLICKABLE)
end

local function refresh_trk()
    trk_val = _trackball_sensitivity_get()
    trk_label.text = "Sensitivity: " .. trk_val .. "ms"
    local filled = math.min(TRK_SEGS, math.floor(trk_val / TRK_STEP + 0.5))
    for i = 1, TRK_SEGS do
        tsegs[i]:set { bg_opa = (i <= filled) and 255 or 40 }
    end
end
refresh_trk()

local trk_dn = content:Button { w = lvgl.PCT(45), h = 30 }
trk_dn:Label { text = "Faster", align = lvgl.ALIGN.CENTER }
local trk_up = content:Button { w = lvgl.PCT(45), h = 30 }
trk_up:Label { text = "Slower", align = lvgl.ALIGN.CENTER }

trk_dn:onClicked(function()
    local v = math.max(0, trk_val - TRK_STEP)
    _trackball_sensitivity_set(v)
    refresh_trk()
    status.text = "Trackball: " .. _trackball_sensitivity_get() .. "ms"
end)
trk_up:onClicked(function()
    local v = math.min(TRK_MAX, trk_val + TRK_STEP)
    _trackball_sensitivity_set(v)
    refresh_trk()
    status.text = "Trackball: " .. _trackball_sensitivity_get() .. "ms"
end)

-- ── Trackball Roll ───────────────────────────────────────────────────────────
local ROLL_STEP = 1
local ROLL_MAX  = 25
local ROLL_SEGS = 25
local roll_val = _trackball_roll_get()
local roll_label = content:Label { text = "", w = lvgl.PCT(100), h = 16 }

local roll_bar = content:Object {
    flex = { flex_direction = "row", flex_wrap = "nowrap" },
    w = lvgl.PCT(100), h = 18, border_width = 0, pad_all = 0,
    pad_column = 1,
}
roll_bar:clear_flag(lvgl.FLAG.SCROLLABLE)
roll_bar:clear_flag(lvgl.FLAG.CLICKABLE)
local rsegs = {}
for i = 1, ROLL_SEGS do
    rsegs[i] = roll_bar:Object { w = 8, h = 14, border_width = 1, pad_all = 0, bg_color = "#24ba24" }
    rsegs[i]:clear_flag(lvgl.FLAG.SCROLLABLE)
    rsegs[i]:clear_flag(lvgl.FLAG.CLICKABLE)
end

local function refresh_roll()
    roll_val = _trackball_roll_get()
    if roll_val == 0 then
        roll_label.text = "Roll: off"
    else
        roll_label.text = "Roll: " .. roll_val .. "ms"
    end
    local filled = math.min(roll_val, ROLL_MAX)
    for i = 1, ROLL_SEGS do
        rsegs[i]:set { bg_opa = (i <= filled) and 255 or 40 }
    end
end
refresh_roll()

local roll_dn = content:Button { w = lvgl.PCT(45), h = 30 }
roll_dn:Label { text = "Less", align = lvgl.ALIGN.CENTER }
local roll_up = content:Button { w = lvgl.PCT(45), h = 30 }
roll_up:Label { text = "More", align = lvgl.ALIGN.CENTER }

roll_dn:onClicked(function()
    local v = math.max(0, roll_val - ROLL_STEP)
    _trackball_roll_set(v)
    refresh_roll()
    status.text = "Roll: " .. (roll_val == 0 and "off" or (roll_val .. "ms"))
end)
roll_up:onClicked(function()
    local v = math.min(ROLL_MAX, roll_val + ROLL_STEP)
    _trackball_roll_set(v)
    refresh_roll()
    status.text = "Roll: " .. _trackball_roll_get() .. "ms"
end)

-- ── Back ─────────────────────────────────────────────────────────────────────
back_btn:onClicked(function()
    apps.go_home()
end)

return root
