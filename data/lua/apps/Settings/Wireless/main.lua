local lvgl  = require("lvgl")
local utils = require("lib/utils")
local apps  = require("lib/apps")
local nav   = require("lib/nav")
local theme = require("lib/theme")

local wifi_avail = type(_wifi_get_enabled) == "function"
local ble_avail  = type(_ble_get_enabled) == "function"

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
content:Label { text = "Wireless", w = lvgl.PCT(70), h = 26 }
local back_btn = content:Button { w = 50, h = 22 }
back_btn:Label { text = "Home", align = lvgl.ALIGN.CENTER }

local status = content:Label { text = "", w = lvgl.PCT(100), h = 16 }
local scan_timer = nil

-- ═══════════════════════════════════════════════════════════════════
-- WiFi Section
-- ═══════════════════════════════════════════════════════════════════

if wifi_avail then

content:Label { text = "-- WiFi --", w = lvgl.PCT(100), h = 16 }

local wifi_on = _wifi_get_enabled()
local btn_wifi = content:Button { w = lvgl.PCT(60), h = 30 }
local lbl_wifi = btn_wifi:Label { align = lvgl.ALIGN.CENTER }

local function refresh_wifi_toggle()
    wifi_on = _wifi_get_enabled()
    lbl_wifi.text = wifi_on and "ON" or "OFF"
end
refresh_wifi_toggle()

btn_wifi:onClicked(function()
    _wifi_set_enabled(not wifi_on)
    refresh_wifi_toggle()
    status.text = "WiFi: " .. (wifi_on and "ON" or "OFF")
end)

-- WiFi status display
local wifi_status_lbl = content:Label { text = "", w = lvgl.PCT(100), h = 16 }

local function refresh_wifi_status()
    if not _wifi_get_enabled() then
        wifi_status_lbl.text = "Disabled"
        return
    end
    local st, ip, ssid = _wifi_status()
    if st == "connected" then
        wifi_status_lbl.text = ssid .. " (" .. ip .. ")"
    elseif st == "connecting" then
        wifi_status_lbl.text = "Connecting..."
    elseif st == "idle" or st == "disconnected" or st == "off" then
        wifi_status_lbl.text = "Not connected"
    else
        wifi_status_lbl.text = "Status: " .. st
    end
end
refresh_wifi_status()

-- Saved networks (multi-slot): tap the list to enter select, click a network
-- to reveal its Connect/Forget actions.
content:Label { text = "Saved networks:", w = lvgl.PCT(100), h = 16 }

local saved_container = content:Object {
    flex = { flex_direction = "column", flex_wrap = "nowrap" },
    w = lvgl.PCT(100), h = lvgl.SIZE_CONTENT,
    border_width = 0, pad_all = 0,
}
saved_container:clear_flag(lvgl.FLAG.SCROLLABLE)
local exit_saved_select = nav.list(saved_container)

local saved_sel = ""
local connect_btn = content:Button { w = 70, h = 24 }
connect_btn:Label { text = "Connect", align = lvgl.ALIGN.CENTER }
local forget_btn = content:Button { w = 60, h = 24 }
forget_btn:Label { text = "Forget", align = lvgl.ALIGN.CENTER }

local function hide_saved_actions()
    saved_sel = ""
    connect_btn:add_flag(lvgl.FLAG.HIDDEN)
    forget_btn:add_flag(lvgl.FLAG.HIDDEN)
end

local function show_saved_actions(ssid)
    saved_sel = ssid
    connect_btn:clear_flag(lvgl.FLAG.HIDDEN)
    forget_btn:clear_flag(lvgl.FLAG.HIDDEN)
    status.text = "Selected: " .. ssid
end

-- Rebuilds the list; only called while the list's nav scope is inactive
-- (row clicks exit the scope before anything mutates the container).
local function refresh_saved()
    hide_saved_actions()
    saved_container:clean()
    local creds = _wifi_get_saved_creds()
    if #creds == 0 then
        saved_container:Label { text = "No saved networks", w = lvgl.PCT(100), h = 16 }
        return
    end
    for i = 1, #creds do
        local net = creds[i]
        local row = saved_container:Button { w = lvgl.PCT(95), h = 26 }
        row:Label { text = net.ssid, align = lvgl.ALIGN.LEFT_MID }
        row:onClicked(function()
            exit_saved_select()
            show_saved_actions(net.ssid)
        end)
    end
end
refresh_saved()

connect_btn:onClicked(function()
    if #saved_sel == 0 then return end
    _wifi_connect_saved(saved_sel)
    status.text = "Joining " .. saved_sel .. "..."
    hide_saved_actions()
end)

forget_btn:onClicked(function()
    if #saved_sel == 0 then return end
    _wifi_forget_cred(saved_sel)
    status.text = "Forgot " .. saved_sel
    refresh_saved()
    refresh_wifi_status()
end)

-- Password input + Join (direct children, hidden initially)
local pass_input = content:Textarea {
    password_mode = true, one_line = true,
    text = "", placeholder_text = "Password",
    w = lvgl.PCT(60), h = 30,
}
pass_input:clear_flag(lvgl.FLAG.SCROLLABLE)
pass_input:add_flag(lvgl.FLAG.HIDDEN)

local join_btn = content:Button { w = 50, h = 28 }
join_btn:Label { text = "Join", align = lvgl.ALIGN.CENTER }
join_btn:add_flag(lvgl.FLAG.HIDDEN)

local selected_ssid = ""
local selected_secure = false

local function show_pass_row()
    pass_input:clear_flag(lvgl.FLAG.HIDDEN)
    join_btn:clear_flag(lvgl.FLAG.HIDDEN)
end

local function hide_pass_row()
    pass_input:add_flag(lvgl.FLAG.HIDDEN)
    join_btn:add_flag(lvgl.FLAG.HIDDEN)
end

join_btn:onClicked(function()
    local pass = pass_input.text or ""
    if #selected_ssid == 0 then
        status.text = "No network selected"
        return
    end
    _wifi_save_creds(selected_ssid, pass)
    _wifi_connect(selected_ssid, pass)
    hide_pass_row()
    status.text = "Joining " .. selected_ssid .. "..."
    refresh_saved()
end)

-- Scan button
local scan_btn = content:Button { w = lvgl.PCT(60), h = 30 }
scan_btn:Label { text = "Scan Networks", align = lvgl.ALIGN.CENTER }

-- Scan results container (click-to-enter pattern)
local scan_container = content:Object {
    flex = { flex_direction = "column", flex_wrap = "nowrap" },
    w = lvgl.PCT(100), h = lvgl.SIZE_CONTENT,
    border_width = 0, pad_all = 0,
}
scan_container:clear_flag(lvgl.FLAG.SCROLLABLE)
-- Tap the results to enter select (trackball steps networks); 'q' or choosing a
-- network exits back to the controls. nav.list owns the scope push/pop.
local exit_scan_select = nav.list(scan_container)

local function show_scan_results(results)
    scan_container:clean()
    if not results or #results == 0 then
        scan_container:Label { text = "No networks found", w = lvgl.PCT(100), h = 16 }
        return
    end
    for i = 1, math.min(#results, 8) do
        local net = results[i]
        local label_text = net.ssid
        if net.secure then label_text = label_text .. " *" end
        label_text = label_text .. " (" .. net.rssi .. "dB)"
        local nbtn = scan_container:Button { w = lvgl.PCT(95), h = 26 }
        nbtn:Label { text = label_text, align = lvgl.ALIGN.LEFT_MID }
        nbtn:onClicked(function()
            selected_ssid = net.ssid
            selected_secure = net.secure
            exit_scan_select()
            if net.secure then
                show_pass_row()
                pass_input.text = ""
                status.text = "Enter password for " .. net.ssid
            else
                hide_pass_row()
                _wifi_save_creds(net.ssid, "")
                _wifi_connect(net.ssid, "")
                status.text = "Joining " .. net.ssid .. "..."
                refresh_saved()
            end
        end)
    end
end

scan_btn:onClicked(function()
    if not _wifi_get_enabled() then
        status.text = "Enable WiFi first"
        return
    end
    scan_container:clean()
    scan_container:Label { text = "Scanning...", w = lvgl.PCT(100), h = 16 }
    _wifi_scan_start()
    if scan_timer then scan_timer:delete(); scan_timer = nil end
    scan_timer = lvgl.Timer { period = 500, cb = function()
        local results = _wifi_scan_results()
        if results then
            if scan_timer then scan_timer:delete(); scan_timer = nil end
            show_scan_results(results)
        end
    end }
end)

-- Periodic WiFi status refresh
apps.add_timer { period = 2000, cb = function()
    refresh_wifi_status()
end }

end -- wifi_avail

-- ═══════════════════════════════════════════════════════════════════
-- BLE Section
-- ═══════════════════════════════════════════════════════════════════

if ble_avail then

content:Label { text = "-- BLE Companion --", w = lvgl.PCT(100), h = 16 }

local ble_on = _ble_get_enabled()
local btn_ble = content:Button { w = lvgl.PCT(60), h = 30 }
local lbl_ble = btn_ble:Label { align = lvgl.ALIGN.CENTER }

local function refresh_ble()
    ble_on = _ble_get_enabled()
    lbl_ble.text = ble_on and "ON" or "OFF"
end
refresh_ble()

btn_ble:onClicked(function()
    _ble_set_enabled(not ble_on)
    refresh_ble()
    status.text = "BLE: " .. (ble_on and "ON" or "OFF")
end)

-- Bond clear toggle
content:Label { text = "Requires PIN re-entry on reconnect", w = lvgl.PCT(100), h = 16 }
local bc_on = _ble_get_bond_clear()
local btn_bc = content:Button { w = lvgl.PCT(60), h = 30 }
local lbl_bc = btn_bc:Label { align = lvgl.ALIGN.CENTER }
lbl_bc.text = bc_on and "Bond Clear: ON" or "Bond Clear: OFF"

btn_bc:onClicked(function()
    bc_on = not bc_on
    _ble_set_bond_clear(bc_on)
    lbl_bc.text = bc_on and "Bond Clear: ON" or "Bond Clear: OFF"
end)

-- Connection status
local lbl_conn = content:Label { text = "", w = lvgl.PCT(100), h = 16 }

local function refresh_conn()
    if not _ble_get_enabled() then
        lbl_conn.text = "Disabled"
    elseif _ble_is_connected() then
        lbl_conn.text = "Connected"
    else
        lbl_conn.text = "Waiting for app..."
    end
end
refresh_conn()

apps.add_timer { period = 2000, cb = function() refresh_conn() end }

end -- ble_avail

-- ═══════════════════════════════════════════════════════════════════
-- Back button
-- ═══════════════════════════════════════════════════════════════════

back_btn:onClicked(function()
    -- scan_timer is dynamic (recreated per scan, self-deleting) and not
    -- manager-tracked; kill it before teardown. The wifi/ble refresh timers
    -- were registered via apps.add_timer, so the manager deletes those.
    if scan_timer then pcall(function() scan_timer:delete() end) end
    apps.go_home()
end)

return root
