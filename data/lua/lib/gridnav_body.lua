local lvgl = require("lvgl")

local W = lvgl.HOR_RES()

local function gridnav_body(parent, y, h, flags)
    local body = parent:Object {
        flex = { flex_direction = "row", flex_wrap = "wrap" },
        w = W, h = h, y = y,
        border_width = 0, pad_all = 4,
    }
    body:clear_flag(lvgl.FLAG.SCROLLABLE)
    _nav_setup(body, flags or GRIDNAV_ROLLOVER)
    return body
end

return gridnav_body
