local lvgl = require("lvgl")

local W = lvgl.HOR_RES()

-- transparent: pass true to make the body see-through (so a themed wallpaper
-- shows behind it); nil/false leaves the default opaque card background.
local function gridnav_body(parent, y, h, flags, transparent)
    local body = parent:Object {
        flex = { flex_direction = "row", flex_wrap = "wrap" },
        w = W, h = h, y = y,
        border_width = 0, pad_all = 4,
    }
    if transparent then body:set { bg_opa = 0 } end
    body:clear_flag(lvgl.FLAG.SCROLLABLE)
    _nav_setup(body, flags or GRIDNAV_ROLLOVER)
    return body
end

return gridnav_body
