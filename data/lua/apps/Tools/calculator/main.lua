local utils = require("lib/utils")
local apps = require("lib/apps")

local W = lvgl.HOR_RES()
local H = lvgl.VER_RES()

local CLR_BG        = "#1a1a1a"
local CLR_PANEL      = "#111111"
local CLR_PANEL_BRD  = "#333333"
local CLR_DIGIT      = "#2d2d2d"
local CLR_OP         = "#FF9500"
local CLR_CLEAR      = "#FF3B30"
local CLR_EQUALS     = "#34C759"
local CLR_TEXT       = "#FFFFFF"
local CLR_DIM        = "#888888"
local CLR_HEADER_TXT = "#AAAAAA"

local HEADER_H  = 20
local ACCENT_H  = 3
local DISPLAY_H = 44
local BTN_H     = 33
local PAD       = 6

local root = apps.new_root()
root:set {
    w = W, h = H,
    bg_color = CLR_BG, bg_opa = 255,
    pad_all = 0, border_width = 0,
}
root:clear_flag(lvgl.FLAG.SCROLLABLE)

local view = root:Object {
    flex = { flex_direction = "column", flex_wrap = "nowrap" },
    bg_color = CLR_BG, bg_opa = 255,
    border_width = 0,
    w = W, h = H,
    pad_left = PAD, pad_right = PAD, pad_top = 2, pad_bottom = 2,
    pad_row = 2, pad_column = 0,
}
view:clear_flag(lvgl.FLAG.SCROLLABLE)

-- Header row
local header = view:Object {
    flex = { flex_direction = "row", flex_wrap = "nowrap" },
    w = lvgl.PCT(100), h = HEADER_H,
    bg_opa = 0, border_width = 0, pad_all = 0,
}
header:clear_flag(lvgl.FLAG.SCROLLABLE)

local back_btn = header:Button { w = 45, h = HEADER_H, radius = 4, bg_color = CLR_DIGIT, bg_opa = 255 }
back_btn:Label { text = "<", align = lvgl.ALIGN.CENTER, text_color = CLR_TEXT }
back_btn:onClicked(function()
    apps.go_home()
end)

header:Label {
    text = "Calculator",
    text_font = lvgl.BUILTIN_FONT.MONTSERRAT_14,
    text_color = CLR_HEADER_TXT,
    align = lvgl.ALIGN.CENTER,
    w = lvgl.PCT(70), h = HEADER_H,
}

-- Canvas accent line
local accent_canvas = view:Canvas {
    w = W - PAD * 2, h = ACCENT_H,
    bg_opa = 0,
}
accent_canvas:fill_bg("#000000", 0)
local aw = W - PAD * 2
accent_canvas:draw_rect({
    x1 = 0, y1 = 0, x2 = aw - 1, y2 = ACCENT_H - 1,
    bg_color = CLR_OP, bg_opa = 60, radius = 0,
})
accent_canvas:draw_rect({
    x1 = math.floor(aw * 0.2), y1 = 0,
    x2 = math.floor(aw * 0.8), y2 = ACCENT_H - 1,
    bg_color = CLR_OP, bg_opa = 100, radius = 0,
})
accent_canvas:draw_rect({
    x1 = math.floor(aw * 0.35), y1 = 1,
    x2 = math.floor(aw * 0.65), y2 = ACCENT_H - 2,
    bg_color = CLR_OP, bg_opa = 180, radius = 0,
})

-- Calculator state
local calc_state = {
    current = 0,
    operator = nil,
    operand = nil,
    fresh = true,
}

-- Display panel
local display_panel = view:Object {
    w = lvgl.PCT(100), h = DISPLAY_H,
    bg_color = CLR_PANEL, bg_opa = 255,
    radius = 8,
    border_width = 1, border_color = CLR_PANEL_BRD,
    pad_left = 10, pad_right = 10, pad_top = 4, pad_bottom = 4,
}
display_panel:clear_flag(lvgl.FLAG.SCROLLABLE)

local op_indicator = display_panel:Label {
    text = "",
    text_font = lvgl.BUILTIN_FONT.MONTSERRAT_14,
    text_color = CLR_DIM,
    align = lvgl.ALIGN.TOP_LEFT,
}

local display = display_panel:Label {
    text = "0",
    text_font = lvgl.BUILTIN_FONT.MONTSERRAT_28,
    text_color = CLR_TEXT,
    align = lvgl.ALIGN.RIGHT_MID,
}

-- Button grid
local grid_h = H - HEADER_H - ACCENT_H - DISPLAY_H - 18
local button_grid = view:Object {
    flex = { flex_direction = "row", flex_wrap = "wrap" },
    bg_opa = 0, border_width = 0,
    w = lvgl.PCT(100), h = grid_h,
    pad_all = 2, pad_row = 3, pad_column = 0,
}
button_grid:clear_flag(lvgl.FLAG.SCROLLABLE)

-- Display update
local function format_value(v)
    if type(v) == "string" then return v end
    if v == math.floor(v) then return tostring(math.floor(v)) end
    return string.format("%.6g", v)
end

local function update_display()
    display:set { text = format_value(calc_state.current) }
    if calc_state.operator then
        op_indicator:set { text = format_value(calc_state.operand) .. " " .. calc_state.operator }
    else
        op_indicator:set { text = "" }
    end
end

-- Calculator logic
local function perform_op()
    local a = calc_state.operand
    local b = calc_state.current
    local op = calc_state.operator
    if not a or not b or not op then return end

    if op == "+" then a = a + b
    elseif op == "-" then a = a - b
    elseif op == "x" then a = a * b
    elseif op == "/" then
        if b == 0 then
            calc_state.current = "Err"
            update_display()
            return
        else
            a = a / b
        end
    end

    calc_state.current = a
    calc_state.operator = nil
    calc_state.operand = 0
    update_display()
end

local function on_button_click(val)
    if val:match("%d") then
        local d = tonumber(val)
        if calc_state.fresh or type(calc_state.current) == "string" then
            calc_state.current = d
            calc_state.fresh = false
        else
            calc_state.current = calc_state.current * 10 + d
        end
    elseif val == "=" then
        perform_op()
        calc_state.fresh = true
    elseif val == "+" or val == "-" or val == "x" or val == "/" then
        if calc_state.operator and not calc_state.fresh then
            perform_op()
        end
        calc_state.operand = calc_state.current
        calc_state.operator = val
        calc_state.fresh = true
    elseif val == "C" then
        calc_state.current = 0
        calc_state.operator = nil
        calc_state.operand = 0
        calc_state.fresh = true
    end
    update_display()
end

-- Button definitions
local buttons = {
    "7", "8", "9", "/",
    "4", "5", "6", "x",
    "1", "2", "3", "-",
    "0", "C", "=", "+",
}

local function btn_colors(value)
    if value:match("%d") then
        return CLR_DIGIT, CLR_TEXT
    elseif value == "C" then
        return CLR_CLEAR, CLR_TEXT
    elseif value == "=" then
        return CLR_EQUALS, CLR_TEXT
    else
        return CLR_OP, CLR_TEXT
    end
end

local function createBtn(parent, value)
    local bg, fg = btn_colors(value)
    local btn = parent:Button {
        w = lvgl.PCT(23), h = BTN_H,
        radius = 6,
        bg_color = bg, bg_opa = 255,
    }
    btn:Label {
        text = value,
        text_font = lvgl.BUILTIN_FONT.MONTSERRAT_18,
        text_color = fg,
        align = lvgl.ALIGN.CENTER,
    }
    btn:onClicked(function()
        on_button_click(value)
    end)
    return btn
end

-- Back button as first grid cell
local back_grid_btn = button_grid:Button {
    w = lvgl.PCT(23), h = BTN_H,
    radius = 6,
    bg_color = CLR_DIGIT, bg_opa = 255,
}
back_grid_btn:Label {
    text = "<",
    text_font = lvgl.BUILTIN_FONT.MONTSERRAT_18,
    text_color = CLR_TEXT,
    align = lvgl.ALIGN.CENTER,
}
back_grid_btn:onClicked(function()
    apps.go_home()
end)

for _, label in ipairs(buttons) do
    createBtn(button_grid, label)
end

_nav_setup(button_grid, GRIDNAV_ROLLOVER)
