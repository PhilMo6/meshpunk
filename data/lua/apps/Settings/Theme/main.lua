--[[
  UI Theme picker.

  Lists the themes lib/theme discovered (internal /lua/themes + SD
  /meshpunk/themes) and applies the chosen one live — chrome recolors
  instantly, the matching background draws behind this page as a preview.
  The choice persists across reboots.

  Themes install, update and remove in the App Library (its pinned Themes
  category); the Get button opens it. Installed themes appear here
  immediately — lib/theme rescans on every list(), no cache to bust.
]]

local lvgl    = require("lvgl")
local apps    = require("lib/apps")
local nav     = require("lib/nav")
local theme   = require("lib/theme")
local utils   = require("lib/utils")

local W = lvgl.HOR_RES()
local H = lvgl.VER_RES()

local root = apps.new_root()
root:set { w = W, h = H, pad_all = 0, border_width = 0, bg_opa = 0 }
root:clear_flag(lvgl.FLAG.SCROLLABLE)

-- Show the current theme's wallpaper behind the picker (transparent containers
-- below), so picking a theme previews its background too, not just the chrome.
-- This app is lightweight, so the background's PSRAM is fine here.
theme.show_background()

local vw = nil

local function toast(msg)
    pcall(utils.createNotification, root, tostring(msg), 2500)
end

local function swap_view(builder)
    local old = vw
    vw = root:Object {
        w = W, h = H, x = 0, y = 0,
        bg_opa = 0, border_width = 0, pad_all = 0, radius = 0,
    }
    vw:clear_flag(lvgl.FLAG.SCROLLABLE)
    builder(vw)
    if old then apps.delete_view(old) end
end

-- ── Picker view ──────────────────────────────────────────────────────────────

local function build_picker()
    swap_view(function(v)
        local content = v:Object {
            flex = { flex_direction = "row", flex_wrap = "wrap" },
            w = W, h = H,
            border_width = 0, pad_all = 6, bg_opa = 0,
        }
        nav.replace(content, { flags = nav.ROLLOVER + nav.SCROLL_FIRST })

        local status   -- forward-declared; the status row is created below

        content:Label { text = "UI Theme", w = 86, h = 26 }

        -- Quick access to the global selection-highlight style (also in Settings >
        -- Device): a translucent "highlighted fill" ([ ]) vs an opaque solid block
        -- ([x]). Applies live, so it previews on the focused items right here.
        local sel_solid_on = _theme_focus_solid_get()
        local function sel_text() return (sel_solid_on and "[x]" or "[ ]") .. " Solid" end
        local sel_btn = content:Button { w = 96, h = 22 }
        local sel_lbl = sel_btn:Label { text = sel_text(), align = lvgl.ALIGN.CENTER }
        sel_btn:onClicked(function()
            sel_solid_on = not sel_solid_on
            _theme_focus_solid_set(sel_solid_on)
            sel_lbl:set { text = sel_text() }
            status.text = sel_solid_on and "Selection: solid" or "Selection: highlighted fill"
        end)

        -- More themes: the App Library's Themes category.
        local get_btn = content:Button { w = 46, h = 22 }
        get_btn:Label { text = "Get", align = lvgl.ALIGN.CENTER }
        get_btn:onClicked(function()
            if not apps.launch("App Library") then
                toast("App Library not found")
            end
        end)

        local back_btn = content:Button { w = 56, h = 22 }
        back_btn:Label { text = "Home", align = lvgl.ALIGN.CENTER }
        back_btn:onClicked(function() apps.go_home() end)

        status = content:Label { text = "Tap a theme to apply", w = lvgl.PCT(100), h = 16 }

        local current_id = theme.current()
        local rows = {}   -- { { id, name, lbl }, ... } to refresh the selection mark

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
    end)
end

-- ── Start ────────────────────────────────────────────────────────────────────

build_picker()

return root
