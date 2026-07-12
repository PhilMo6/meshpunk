-- Settings App for MeshPunk
-- Node name, storage toggle, radio info

local lvgl = require("lvgl")
local clock_fmt_mod = require("lib/clock_fmt")
local utils = require("lib/utils")
local apps = require("lib/apps")
local nav = require("lib/nav")
local theme = require("lib/theme")

-- Root
local root = apps.new_root()
root:set {
    w = lvgl.HOR_RES(),
    h = lvgl.VER_RES(),
    pad_all = 0,
    border_width = 0,
    bg_opa = 0,
}
root:clear_flag(lvgl.FLAG.SCROLLABLE)

-- Themed wallpaper behind this (lightweight) screen; containers below are transparent.
theme.show_background()

-- Safely get info
local ok, info = pcall(_mesh_get_node_info)
if not ok or not info then
    info = { name = "???", freq = 0, tx_power = 0, pubkey = "", lat = 0, lon = 0 }
end


-- Scrollable content area with trackball navigation
local content = root:Object {
    flex = { flex_direction = "row", flex_wrap = "wrap" },
    w = lvgl.HOR_RES(),
    h = lvgl.VER_RES(),
    y = 0,
    border_width = 0,
    pad_all = 6,
    bg_opa = 0,
}

nav.replace(content, { flags = nav.ROLLOVER + nav.SCROLL_FIRST })

-- Title
content:Label { text = "Radio Settings", w = lvgl.PCT(70), h = 26 }
local back_btn = content:Button { w = 50, h = 22 }
back_btn:Label { text = "Home", align = lvgl.ALIGN.CENTER }
back_btn:onClicked(function()
    apps.go_home()
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
    overlay:add_flag(lvgl.FLAG.CLICKABLE)  -- modal

    local box = overlay:Object {
        w = 220, h = 120,
        align = lvgl.ALIGN.CENTER,
        border_width = 1,
        pad_all = 10,
        flex = { flex_direction = "row", flex_wrap = "wrap" },
    }
    box:clear_flag(lvgl.FLAG.SCROLLABLE)
    nav.push(box)

    box:Label { text = "Settings saved.", w = lvgl.PCT(100), h = 20 }
    box:Label { text = "Restart to apply?", w = lvgl.PCT(100), h = 20 }

    local restart_btn = box:Button { w = lvgl.PCT(48), h = 32 }
    restart_btn:Label { text = "Restart", align = lvgl.ALIGN.CENTER }
    restart_btn:onClicked(function()
        pcall(_system_reboot)
    end)

    local wait_btn = box:Button { w = lvgl.PCT(48), h = 32 }
    wait_btn:Label { text = "Wait", align = lvgl.ALIGN.CENTER }
    wait_btn:onClicked(function()
        nav.pop()
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
    { name = "Australia (Mid)",     freq = 915.075, bw = 125,   sf = 9,  cr = 5, tx = 20 },
    { name = "Australia (Wide)",    freq = 915.800, bw = 250,   sf = 11, cr = 5, tx = 20 },
    { name = "Australia (Narrow)",  freq = 916.575, bw = 62.5,  sf = 7,  cr = 8, tx = 20 },
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

local preset_dd = content:Dropdown {
    options = table.concat(preset_names, "\n"),
    w = lvgl.PCT(65),
    h = 30,
    dir = lvgl.DIR.BOTTOM,
}

local preset_apply_btn = content:Button { w = lvgl.PCT(30), h = 30 }
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

local function get_boost_text()
    return boost_enabled and "[x] RX Boost" or "[ ] RX Boost"
end

local boost_toggle_btn = content:Button { w = lvgl.PCT(65), h = 30 }
local boost_label = boost_toggle_btn:Label { text = get_boost_text(), align = lvgl.ALIGN.CENTER }

local boost_apply_btn = content:Button { w = lvgl.PCT(30), h = 30 }
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

local function get_overwrite_text()
    return overwrite_enabled and "[x] Contact Overwrite" or "[ ] Contact Overwrite"
end

local overwrite_toggle_btn = content:Button { w = lvgl.PCT(65), h = 30 }
local overwrite_label = overwrite_toggle_btn:Label { text = get_overwrite_text(), align = lvgl.ALIGN.CENTER }

local overwrite_apply_btn = content:Button { w = lvgl.PCT(30), h = 30 }
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

-- ── Advert Location ──
content:Label { text = "-- Advert Location --", w = lvgl.PCT(100), h = 16 }

local ok_al, al = pcall(_mesh_get_advert_loc)
local advert_loc_on = ok_al and (al == true)  -- default: off (privacy)

local function get_advert_loc_text()
    return advert_loc_on and "[x] Share location in adverts" or "[ ] Share location in adverts"
end

local advert_loc_btn = content:Button { w = lvgl.PCT(100), h = 30 }
local advert_loc_label = advert_loc_btn:Label { text = get_advert_loc_text(), align = lvgl.ALIGN.CENTER }
advert_loc_btn:onClicked(function()
    advert_loc_on = not advert_loc_on
    advert_loc_label.text = get_advert_loc_text()
    local ok_set = pcall(_mesh_set_advert_loc, advert_loc_on)
    status_label.text = ok_set
        and ("Advert location: " .. (advert_loc_on and "shared" or "hidden"))
        or "Error saving advert location"
end)

-- ── Message Repeat ──
content:Label { text = "-- Message Repeat --", w = lvgl.PCT(100), h = 16 }

local ok_rep, rep_cfg = pcall(_mesh_get_msg_repeat)
if not ok_rep or not rep_cfg then rep_cfg = { enabled = false, max_repeats = 3, interval = 30 } end

local repeat_enabled = rep_cfg.enabled or false

local function get_repeat_text()
    return repeat_enabled and "[x] Msg Repeat" or "[ ] Msg Repeat"
end

local repeat_toggle_btn = content:Button { w = lvgl.PCT(65), h = 30 }
local repeat_label = repeat_toggle_btn:Label { text = get_repeat_text(), align = lvgl.ALIGN.CENTER }

local repeat_apply_btn = content:Button { w = lvgl.PCT(30), h = 30 }
repeat_apply_btn:Label { text = "Apply", align = lvgl.ALIGN.CENTER }

repeat_toggle_btn:onClicked(function()
    repeat_enabled = not repeat_enabled
    repeat_label.text = get_repeat_text()
end)

content:Label { text = "Max Repeats:", w = lvgl.PCT(45), h = 20 }
local rep_max_input = content:Textarea {
    password_mode = false,
    one_line = true,
    w = lvgl.PCT(50), h = 30,
    text = tostring(rep_cfg.max_repeats or 3),
    max_length = 2,
}

content:Label { text = "Interval (s):", w = lvgl.PCT(45), h = 20 }
local rep_int_input = content:Textarea {
    password_mode = false,
    one_line = true,
    w = lvgl.PCT(50), h = 30,
    text = tostring(rep_cfg.interval or 30),
    max_length = 2,
}

repeat_apply_btn:onClicked(function()
    local max_r = tonumber(rep_max_input.text) or 3
    local intv  = tonumber(rep_int_input.text) or 30
    local ok_set, err = pcall(_mesh_set_msg_repeat, repeat_enabled, max_r, intv)
    if ok_set then
        status_label.text = "Msg Repeat: " .. (repeat_enabled and "ON" or "OFF")
    else
        status_label.text = "Error: " .. tostring(err)
    end
end)

-- ── Multi-byte IDs (path hash size) ──
content:Label { text = "-- Multi-byte IDs --", w = lvgl.PCT(100), h = 16 }
content:Label {
    text = "Bytes per hop in routed paths. Higher = fewer ID collisions in dense meshes, but bigger packets and fewer hops.",
    w = lvgl.PCT(100), h = 44,
}

local path_hash_mode = (function()
    local ok_p, m = pcall(_mesh_get_path_hash_mode)
    return (ok_p and m) or 0
end)()

local phm_dd = content:Dropdown {
    options = "1 byte (default)\n2 bytes\n3 bytes",
    w = lvgl.PCT(65), h = 30, dir = lvgl.DIR.TOP,
}
phm_dd:set({ selected = path_hash_mode })
phm_dd:onevent(lvgl.EVENT.VALUE_CHANGED, function()
    local mode = phm_dd:get("selected")
    local ok_s = pcall(_mesh_set_path_hash_mode, mode)
    if ok_s then
        status_label.text = "Path hash: " .. (mode + 1) .. ((mode == 0) and " byte" or " bytes")
    else
        status_label.text = "Failed to set path hash"
    end
end)

-- ── Repeater Mode (client repeat) ──
content:Label { text = "-- Repeater Mode --", w = lvgl.PCT(100), h = 16 }
content:Label {
    text = "Re-transmit other nodes' packets like a repeater.",
    w = lvgl.PCT(100), h = 30,
}

local client_repeat_on = (function()
    local ok_cr, cr = pcall(_mesh_get_client_repeat)
    return ok_cr and (cr == true)
end)()

local function get_client_repeat_text()
    return client_repeat_on and "[x] Repeat others' packets" or "[ ] Repeat others' packets"
end

local client_repeat_btn = content:Button { w = lvgl.PCT(100), h = 30 }
local client_repeat_label = client_repeat_btn:Label { text = get_client_repeat_text(), align = lvgl.ALIGN.CENTER }
client_repeat_btn:onClicked(function()
    local want = not client_repeat_on
    local ok_call, ok_set, err = pcall(_mesh_set_client_repeat, want)
    if ok_call and ok_set then
        client_repeat_on = want
        status_label.text = "Repeater mode: " .. (want and "ON" or "OFF")
    else
        status_label.text = "Repeat: " .. tostring(err or "failed to save")
    end
    client_repeat_label.text = get_client_repeat_text()
end)

return root
