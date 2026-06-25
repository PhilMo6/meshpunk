-- UI Theme picker. Lists the themes under /lua/themes and applies the chosen one
-- live: selecting a theme recolors the chrome instantly (a preview you can see
-- right here). The matching background is drawn when you return home (it isn't
-- allocated while an app is running). The choice is persisted across reboots.

local lvgl  = require("lvgl")
local apps  = require("lib/apps")
local nav   = require("lib/nav")
local theme = require("lib/theme")

local root = apps.new_root()
root:set { w = lvgl.HOR_RES(), h = lvgl.VER_RES(), pad_all = 0, border_width = 0, bg_opa = 0 }
root:clear_flag(lvgl.FLAG.SCROLLABLE)

-- Show the current theme's wallpaper behind the picker (transparent containers
-- below), so picking a theme previews its background too, not just the chrome.
-- This app is lightweight, so the background's PSRAM is fine here.
theme.show_background()

local content = root:Object {
    flex = { flex_direction = "row", flex_wrap = "wrap" },
    w = lvgl.HOR_RES(), h = lvgl.VER_RES(),
    border_width = 0, pad_all = 6, bg_opa = 0,
}
nav.replace(content, { flags = nav.ROLLOVER + nav.SCROLL_FIRST })

content:Label { text = "UI Theme", w = lvgl.PCT(70), h = 26 }
local back_btn = content:Button { w = 50, h = 22 }
back_btn:Label { text = "Home", align = lvgl.ALIGN.CENTER }
back_btn:onClicked(function() apps.go_home() end)

local status = content:Label { text = "Tap a theme to apply", w = lvgl.PCT(100), h = 16 }

local current_id = theme.current()
local rows = {}   -- { { id, name, lbl }, ... } so we can refresh the selection mark

local function mark(id) return (id == current_id) and "* " or "   " end

local list = theme.list()
if #list == 0 then
    content:Label { text = "No themes found in /lua/themes.", w = lvgl.PCT(100), h = 20 }
end

for _, item in ipairs(list) do
    local disp = item.name .. (item.source == "sd" and "  (SD)" or "")
    local btn = content:Button { w = lvgl.PCT(100), h = 34 }
    local lbl = btn:Label { text = mark(item.id) .. disp, align = lvgl.ALIGN.CENTER }
    rows[#rows + 1] = { id = item.id, name = disp, lbl = lbl }

    btn:onClicked(function()
        theme.apply(item.id)              -- live chrome re-theme; persists the id
        current_id = theme.current()
        for _, r in ipairs(rows) do
            r.lbl:set { text = mark(r.id) .. r.name }
        end
        status.text = "Applied: " .. item.name
    end)
end
