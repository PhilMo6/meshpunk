-- Settings App for MeshPunk
-- Node name, storage toggle, radio info

local lvgl = require("lvgl")
local clock_fmt_mod = require("lib/clock_fmt")
local utils = require("lib/utils")

-- Root
local root = lvgl.Object()
root:set {
    w = lvgl.HOR_RES(),
    h = lvgl.VER_RES(),
    pad_all = 0,
    border_width = 0,
}
root:clear_flag(lvgl.FLAG.SCROLLABLE)

-- Safely get info
local ok, info = pcall(_mesh_get_node_info)
if not ok or not info then
    info = { name = "???", freq = 0, tx_power = 0, pubkey = "", lat = 0, lon = 0 }
end


-- Scrollable content area with trackball navigation
local content = root:Object {
    flex = {
        flex_direction = "column",
        flex_wrap = "nowrap",
    },
    w = lvgl.HOR_RES(),
    h = lvgl.VER_RES(),
    y = 0,
    border_width = 0,
    pad_all = 6,
}

_nav_setup(content, GRIDNAV_ROLLOVER)

-- Title row
local title_row = content:Object {
    w = lvgl.PCT(100),
    h = 26,
    border_width = 0,
    pad_all = 0,
}
title_row:clear_flag(lvgl.FLAG.SCROLLABLE)

title_row:Label {
    text = "Radio Settings",
    align = lvgl.ALIGN.LEFT_MID,
}

local back_btn = title_row:Button { w = 50, h = 22, align = lvgl.ALIGN.RIGHT_MID }
back_btn:Label { text = "Home", align = lvgl.ALIGN.CENTER }
back_btn:onClicked(function()
    utils.loadingPopUpAdd(nil, "Home", function()
        root:delete()
        local launcher = require("launcher")
        launcher.create()
        return true
    end)
end)

-- Status line
local status_label = content:Label {
    text = "",
    w = lvgl.PCT(100),
    h = 16,
}

-- ── Restart popup ──
local function show_restart_popup()
    local overlay = root:Object {
        w = lvgl.HOR_RES(), h = lvgl.VER_RES(),
        x = 0, y = 0,
        bg_opa = 200,
        border_width = 0,
        pad_all = 0,
    }
    overlay:clear_flag(lvgl.FLAG.SCROLLABLE)

    local box = overlay:Object {
        w = 220, h = 120,
        align = lvgl.ALIGN.CENTER,
        border_width = 1,
        pad_all = 10,
        flex = { flex_direction = "column", flex_wrap = "nowrap" },
    }
    box:clear_flag(lvgl.FLAG.SCROLLABLE)

    box:Label { text = "Settings saved.", w = lvgl.PCT(100), h = 20 }
    box:Label { text = "Restart to apply?", w = lvgl.PCT(100), h = 20 }

    local btn_row = box:Object {
        flex = { flex_direction = "row", flex_wrap = "nowrap" },
        w = lvgl.PCT(100), h = 40,
        border_width = 0, pad_all = 4,
    }
    btn_row:clear_flag(lvgl.FLAG.SCROLLABLE)

    local restart_btn = btn_row:Button { w = lvgl.PCT(48), h = 32 }
    restart_btn:Label { text = "Restart", align = lvgl.ALIGN.CENTER }
    restart_btn:onClicked(function()
        pcall(_system_reboot)
    end)

    local wait_btn = btn_row:Button { w = lvgl.PCT(48), h = 32 }
    wait_btn:Label { text = "Wait", align = lvgl.ALIGN.CENTER }
    wait_btn:onClicked(function()
        overlay:delete()
    end)
end

-- ── Section: Node Name ──
content:Label { text = "-- Node Name --", w = lvgl.PCT(100), h = 16 }

local name_input = content:Textarea {
    password_mode = false,
    one_line = true,
    text = info.name or "NONAME",
    w = lvgl.PCT(100),
    h = 30,
}
name_input:clear_flag(lvgl.FLAG.SCROLLABLE)

-- ── Section: Radio Info ──
content:Label { text = "-- Radio --", w = lvgl.PCT(100), h = 16 }

content:Label { text = "Freq (MHz):", w = lvgl.PCT(100), h = 16 }
local freq_input = content:Textarea {
    password_mode = false, one_line = true,
    text = string.format("%.3f", info.freq or 0),
    w = lvgl.PCT(50), h = 30,
}
freq_input:clear_flag(lvgl.FLAG.SCROLLABLE)

content:Label { text = "TX Power (dBm):", w = lvgl.PCT(100), h = 16 }
local tx_input = content:Textarea {
    password_mode = false, one_line = true,
    text = tostring(info.tx_power or 20),
    w = lvgl.PCT(50), h = 30,
}
tx_input:clear_flag(lvgl.FLAG.SCROLLABLE)

content:Label { text = "Bandwidth (kHz):", w = lvgl.PCT(100), h = 16 }
local bw_input = content:Textarea {
    password_mode = false, one_line = true,
    text = string.format("%g", info.bandwidth or 250),
    w = lvgl.PCT(50), h = 30,
}
bw_input:clear_flag(lvgl.FLAG.SCROLLABLE)

content:Label { text = "Spreading Factor:", w = lvgl.PCT(100), h = 16 }
local sf_input = content:Textarea {
    password_mode = false, one_line = true,
    text = tostring(info.spreading_factor or 10),
    w = lvgl.PCT(50), h = 30,
}
sf_input:clear_flag(lvgl.FLAG.SCROLLABLE)

content:Label { text = "Coding Rate:", w = lvgl.PCT(100), h = 16 }
local cr_input = content:Textarea {
    password_mode = false, one_line = true,
    text = tostring(info.coding_rate or 5),
    w = lvgl.PCT(50), h = 30,
}
cr_input:clear_flag(lvgl.FLAG.SCROLLABLE)

-- ── Save All ──
local function save_all()
    local new_name = name_input.text
    if new_name and #new_name > 0 then
        pcall(_mesh_set_config, "name", new_name)
    end

    local fields = {
        { input = freq_input, key = "freq", label = "Freq" },
        { input = tx_input,   key = "tx",   label = "TX" },
        { input = bw_input,   key = "bw",   label = "BW" },
        { input = sf_input,   key = "sf",   label = "SF" },
        { input = cr_input,   key = "cr",   label = "CR" },
    }

    for _, f in ipairs(fields) do
        local val = f.input.text
        if not tonumber(val) then
            status_label.text = f.label .. ": enter a number"
            return
        end
    end

    for _, f in ipairs(fields) do
        pcall(_mesh_set_config, f.key, f.input.text)
    end

    show_restart_popup()
end

local save_btn = content:Button { w = lvgl.PCT(100), h = 34 }
save_btn:Label { text = "Save All", align = lvgl.ALIGN.CENTER }
save_btn:onClicked(save_all)

-- ── Regional presets ──
local presets = {
    { name = "USA/Canada",          freq = 910.525, bw = 62.5,  sf = 7,  cr = 5, tx = 20 },
    { name = "USA Arizona",         freq = 908.205, bw = 62.5,  sf = 10, cr = 5, tx = 20 },
    { name = "EU/UK (Narrow)",      freq = 869.618, bw = 62.5,  sf = 8,  cr = 5, tx = 14 },
    { name = "EU/UK (Med Range)",   freq = 869.525, bw = 250,   sf = 10, cr = 5, tx = 14 },
    { name = "EU/UK (Long Range)",  freq = 869.525, bw = 250,   sf = 11, cr = 5, tx = 14 },
    { name = "EU 433MHz",           freq = 433.650, bw = 250,   sf = 11, cr = 5, tx = 20 },
    { name = "Switzerland",         freq = 869.618, bw = 62.5,  sf = 8,  cr = 8, tx = 14 },
    { name = "Czech Republic",      freq = 869.432, bw = 62.5,  sf = 7,  cr = 5, tx = 14 },
    { name = "Portugal 433",        freq = 433.375, bw = 62.5,  sf = 9,  cr = 5, tx = 20 },
    { name = "Portugal 869",        freq = 869.618, bw = 62.5,  sf = 7,  cr = 5, tx = 14 },
    { name = "Australia",           freq = 915.800, bw = 250,   sf = 10, cr = 5, tx = 20 },
    { name = "Australia (Narrow)",  freq = 916.575, bw = 62.5,  sf = 7,  cr = 5, tx = 20 },
    { name = "Australia SA/WA/QLD", freq = 923.125, bw = 62.5,  sf = 8,  cr = 5, tx = 20 },
    { name = "New Zealand",         freq = 917.375, bw = 250,   sf = 11, cr = 5, tx = 20 },
    { name = "New Zealand (Narrow)",freq = 917.375, bw = 62.5,  sf = 7,  cr = 5, tx = 20 },
    { name = "Vietnam",             freq = 920.250, bw = 250,   sf = 11, cr = 5, tx = 20 },
    { name = "Off-Grid 433",        freq = 433.000, bw = 250,   sf = 11, cr = 5, tx = 20 },
    { name = "Off-Grid 869",        freq = 869.000, bw = 250,   sf = 11, cr = 5, tx = 14 },
    { name = "Off-Grid 918",        freq = 918.000, bw = 250,   sf = 11, cr = 5, tx = 20 },
}

local function apply_preset(p)
    freq_input.text = string.format("%.3f", p.freq)
    tx_input.text   = tostring(p.tx)
    bw_input.text   = string.format("%g", p.bw)
    sf_input.text   = tostring(p.sf)
    cr_input.text   = tostring(p.cr)
    status_label.text = "Preset: " .. p.name .. " (hit Save All)"
end

content:Label { text = "-- Region Preset --", w = lvgl.PCT(100), h = 16 }

local preset_names = {}
for _, p in ipairs(presets) do
    preset_names[#preset_names + 1] = p.name
end

local preset_row = content:Object {
    flex = { flex_direction = "row", flex_wrap = "nowrap" },
    w = lvgl.PCT(100), h = 34, border_width = 0, pad_all = 0,
}
preset_row:clear_flag(lvgl.FLAG.SCROLLABLE)

local preset_dd = preset_row:Dropdown {
    options = table.concat(preset_names, "\n"),
    w = lvgl.PCT(65),
    h = 30,
    dir = lvgl.DIR.BOTTOM,
}

local preset_apply_btn = preset_row:Button { w = lvgl.PCT(30), h = 30 }
preset_apply_btn:Label { text = "Load", align = lvgl.ALIGN.CENTER }

preset_apply_btn:onClicked(function()
    local idx = preset_dd:get("selected") + 1
    if presets[idx] then
        apply_preset(presets[idx])
    end
end)

-- Public key (read-only)
content:Label { text = "Key: " .. string.sub(info.pubkey or "", 1, 16) .. "...", w = lvgl.PCT(100), h = 16 }

-- RX Boost toggle
local ok_boost, rx_boost = pcall(_mesh_get_rx_boost)
local boost_enabled = (ok_boost and rx_boost) or false

local boost_row = content:Object {
    flex = { flex_direction = "row", flex_wrap = "nowrap" },
    w = lvgl.PCT(100), h = 34, border_width = 0, pad_all = 0,
}
boost_row:clear_flag(lvgl.FLAG.SCROLLABLE)

local function get_boost_text()
    return boost_enabled and "[x] RX Boost" or "[ ] RX Boost"
end

local boost_toggle_btn = boost_row:Button { w = lvgl.PCT(65), h = 30 }
local boost_label = boost_toggle_btn:Label { text = get_boost_text(), align = lvgl.ALIGN.CENTER }

local boost_apply_btn = boost_row:Button { w = lvgl.PCT(30), h = 30 }
boost_apply_btn:Label { text = "Apply", align = lvgl.ALIGN.CENTER }

boost_toggle_btn:onClicked(function()
    boost_enabled = not boost_enabled
    boost_label.text = get_boost_text()
end)

boost_apply_btn:onClicked(function()
    local ok_set, err = pcall(_mesh_set_rx_boost, boost_enabled)
    if ok_set then
        status_label.text = "RX Boost: " .. (boost_enabled and "ON" or "OFF")
    else
        status_label.text = "Error: " .. tostring(err)
    end
end)

-- Contact Overwrite toggle
local overwrite_enabled = info.contact_overwrite or false

local overwrite_row = content:Object {
    flex = { flex_direction = "row", flex_wrap = "nowrap" },
    w = lvgl.PCT(100), h = 34, border_width = 0, pad_all = 0,
}
overwrite_row:clear_flag(lvgl.FLAG.SCROLLABLE)

local function get_overwrite_text()
    return overwrite_enabled and "[x] Contact Overwrite" or "[ ] Contact Overwrite"
end

local overwrite_toggle_btn = overwrite_row:Button { w = lvgl.PCT(65), h = 30 }
local overwrite_label = overwrite_toggle_btn:Label { text = get_overwrite_text(), align = lvgl.ALIGN.CENTER }

local overwrite_apply_btn = overwrite_row:Button { w = lvgl.PCT(30), h = 30 }
overwrite_apply_btn:Label { text = "Apply", align = lvgl.ALIGN.CENTER }

overwrite_toggle_btn:onClicked(function()
    overwrite_enabled = not overwrite_enabled
    overwrite_label.text = get_overwrite_text()
end)

overwrite_apply_btn:onClicked(function()
    local ok_set, err = pcall(_mesh_set_config, "contact_overwrite", overwrite_enabled and "1" or "0")
    if ok_set then
        status_label.text = "Contact Overwrite: " .. (overwrite_enabled and "ON" or "OFF")
    else
        status_label.text = "Error: " .. tostring(err)
    end
end)

return root
