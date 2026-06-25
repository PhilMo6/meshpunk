local lvgl  = require("lvgl")
local utils = require("lib/utils")
local apps  = require("lib/apps")
local nav   = require("lib/nav")
local theme = require("lib/theme")

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
content:Label { text = "Identity", w = lvgl.PCT(70), h = 26 }
local back_btn = content:Button { w = 50, h = 22 }
back_btn:Label { text = "Home", align = lvgl.ALIGN.CENTER }

local status = content:Label { text = "", w = lvgl.PCT(100), h = 16 }

-- Public key display
local ok, info = pcall(_mesh_get_node_info)
if not ok or not info then
    info = { pubkey = "???" }
end
content:Label { text = "-- Public Key --", w = lvgl.PCT(100), h = 16 }
content:Label { text = info.pubkey or "???", w = lvgl.PCT(100), h = 16 }

-- Export section
content:Label { text = "-- Private Key --", w = lvgl.PCT(100), h = 16 }

local key_ta = nil
local btn_export = content:Button { w = lvgl.PCT(60), h = 30 }
btn_export:Label { text = "Show Key", align = lvgl.ALIGN.CENTER }

btn_export:onClicked(function()
    if key_ta then
        key_ta:delete()
        key_ta = nil
        return
    end
    local key = _mesh_export_private_key()
    key_ta = content:Textarea {
        password_mode = false, one_line = false,
        text = key,
        w = lvgl.PCT(100), h = 60,
    }
    key_ta:clear_flag(lvgl.FLAG.SCROLLABLE)
end)

-- Generate section
content:Label { text = "-- Generate New --", w = lvgl.PCT(100), h = 16 }

local btn_gen = content:Button { w = lvgl.PCT(60), h = 30 }
btn_gen:Label { text = "New Identity", align = lvgl.ALIGN.CENTER }

btn_gen:onClicked(function()
    local overlay = root:Object {
        w = lvgl.HOR_RES(), h = lvgl.VER_RES(),
        x = 0, y = 0, bg_opa = 200, border_width = 0, pad_all = 0,
    }
    overlay:clear_flag(lvgl.FLAG.SCROLLABLE)
    overlay:add_flag(lvgl.FLAG.CLICKABLE)  -- modal
    local box = overlay:Object {
        w = 280, h = 130, align = lvgl.ALIGN.CENTER,
        border_width = 1, pad_all = 10,
        flex = { flex_direction = "row", flex_wrap = "wrap" },
    }
    box:clear_flag(lvgl.FLAG.SCROLLABLE)
    nav.push(box)

    box:Label { text = "WARNING", w = lvgl.PCT(100), h = 20 }
    box:Label { text = "Generate new identity?", w = lvgl.PCT(100), h = 18 }
    box:Label { text = "Old key is LOST forever!", w = lvgl.PCT(100), h = 18 }

    local yes = box:Button { w = lvgl.PCT(48), h = 32 }
    yes:Label { text = "Confirm", align = lvgl.ALIGN.CENTER }
    local no = box:Button { w = lvgl.PCT(48), h = 32 }
    no:Label { text = "Cancel", align = lvgl.ALIGN.CENTER }
    no:onClicked(function()
        nav.pop()
        overlay:delete()
    end)
    yes:onClicked(function()
        nav.pop()
        overlay:delete()
        status.text = "Generating..."
        local ok2, err = pcall(_mesh_generate_identity)
        if not ok2 then
            status.text = "Error: " .. tostring(err)
        end
    end)
end)

-- Back
back_btn:onClicked(function()
    apps.go_home()
end)

return root
