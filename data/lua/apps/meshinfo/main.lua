-- Mesh Info / Settings Screen
-- Shows node identity, radio config, and discovered contacts

local lvgl = require("lvgl")

-- Root - two-step pattern from calculator
local root = lvgl.Object()
root:set {
    w = lvgl.HOR_RES(),
    h = lvgl.VER_RES(),
    pad_all = 0,
    border_width = 0,
}
root:clear_flag(lvgl.FLAG.SCROLLABLE)

-- Safely get node info
local ok, info = pcall(_mesh_get_node_info)
if not ok or not info then
    info = { name = "???", freq = 0, tx_power = 0, pubkey = "", lat = 0, lon = 0 }
end

-- Main content area (scrollable column)
local content = root:Object {
    flex = {
        flex_direction = "column",
        flex_wrap = "nowrap",
    },
    w = lvgl.HOR_RES(),
    h = lvgl.VER_RES() - 44,
    y = 0,
    border_width = 0,
    pad_all = 6,
}

-- Title + back
local title_row = content:Object {
    w = lvgl.PCT(100),
    h = 26,
    border_width = 0,
    pad_all = 0,
}
title_row:clear_flag(lvgl.FLAG.SCROLLABLE)

title_row:Label {
    text = "Mesh Settings",
    align = lvgl.ALIGN.LEFT_MID,
}

local back_btn = title_row:Button { w = 50, h = 22, align = lvgl.ALIGN.RIGHT_MID }
back_btn:Label { text = "Back", align = lvgl.ALIGN.CENTER }
back_btn:onClicked(function()
    root:delete()
    local launcher = require("launcher")
    launcher.create()
end)

-- Name editor
content:Label { text = "Name:", w = lvgl.PCT(100), h = 16 }

local name_row = content:Object {
    flex = {
        flex_direction = "row",
        flex_wrap = "nowrap",
    },
    w = lvgl.PCT(100),
    h = 34,
    border_width = 0,
    pad_all = 0,
}
name_row:clear_flag(lvgl.FLAG.SCROLLABLE)

local name_input = name_row:Textarea {
    password_mode = false,
    one_line = true,
    text = info.name or "NONAME",
    w = lvgl.PCT(65),
    h = 30,
}

local save_btn = name_row:Button { w = lvgl.PCT(30), h = 30 }
save_btn:Label { text = "Save", align = lvgl.ALIGN.CENTER }

local status_label = content:Label {
    text = "",
    w = lvgl.PCT(100),
    h = 16,
}

save_btn:onClicked(function()
    local new_name = name_input.text
    if new_name and #new_name > 0 then
        _mesh_set_config("name", new_name)
        status_label.text = "Saved: " .. new_name
    end
end)

name_input:onevent(lvgl.EVENT.KEY, function(obj, code)
    local indev = lvgl.indev.get_act()
    local key = indev:get_key()
    if key == lvgl.KEY.ENTER then
        local new_name = name_input.text
        if new_name and #new_name > 0 then
            _mesh_set_config("name", new_name)
            status_label.text = "Saved: " .. new_name
        end
    end
end)

-- Node details
content:Label { text = "Freq: " .. string.format("%.3f MHz", info.freq or 0), w = lvgl.PCT(100), h = 16 }
content:Label { text = "TX: " .. tostring(info.tx_power or "?") .. " dBm", w = lvgl.PCT(100), h = 16 }
content:Label { text = "Key: " .. string.sub(info.pubkey or "", 1, 16) .. "...", w = lvgl.PCT(100), h = 16 }

-- Contacts section
content:Label { text = "-- Contacts --", w = lvgl.PCT(100), h = 16 }

local contact_list = content:Object {
    flex = {
        flex_direction = "column",
        flex_wrap = "nowrap",
    },
    w = lvgl.PCT(100),
    h = 60,
    border_width = 0,
    pad_all = 0,
}

local function refresh_contacts()
    contact_list:clean()

    local ok2, contacts = pcall(_mesh_get_contacts)
    if not ok2 or not contacts then
        contacts = {}
    end

    if #contacts == 0 then
        contact_list:Label {
            text = "No contacts yet",
            w = lvgl.PCT(100),
            h = 16,
        }
    else
        for _, c in ipairs(contacts) do
            contact_list:Label {
                text = c.name .. " (" .. (c.type_name or "?") .. ") h=" .. (c.path_len or 0),
                w = lvgl.PCT(100),
                h = 16,
            }
        end
    end
end

refresh_contacts()

-- Bottom button row (absolute positioned)
local btn_row = root:Object {
    flex = {
        flex_direction = "row",
    },
    w = lvgl.HOR_RES(),
    h = 40,
    y = lvgl.VER_RES() - 42,
    border_width = 0,
    pad_left = 6,
    pad_right = 6,
}
btn_row:clear_flag(lvgl.FLAG.SCROLLABLE)

local adv_btn = btn_row:Button { w = lvgl.PCT(30), h = 34 }
adv_btn:Label { text = "Advert", align = lvgl.ALIGN.CENTER }
adv_btn:onClicked(function()
    _mesh_send_advert()
    status_label.text = "Sent!"
end)

local ref_btn = btn_row:Button { w = lvgl.PCT(30), h = 34 }
ref_btn:Label { text = "Refresh", align = lvgl.ALIGN.CENTER }
ref_btn:onClicked(function()
    refresh_contacts()
    status_label.text = ""
end)

return root
