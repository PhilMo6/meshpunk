-- Settings/Wifi — WiFi-specific settings (the WiFi half of the retired
-- Wireless app, unchanged mechanics): on/off, saved multi-network creds with
-- Connect/Forget, scan + join. BLE lives in Settings > BLE now.
local lvgl  = require("lvgl")
local apps  = require("lib/apps")
local nav   = require("lib/nav")
local theme = require("lib/theme")

local wifi_avail = type(_wifi_get_enabled) == "function"

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
content:Label { text = "WiFi", w = lvgl.PCT(70), h = 26 }
local back_btn = content:Button { w = 50, h = 22 }
back_btn:Label { text = "Home", align = lvgl.ALIGN.CENTER }

local status = content:Label { text = "", w = lvgl.PCT(100), h = 16 }
local scan_timer = nil

if wifi_avail then

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
local forget_lbl = forget_btn:Label { text = "Forget", align = lvgl.ALIGN.CENTER }

-- Forget is two-tap: the first tap arms for FORGET_TIMEOUT_MS, the second
-- drops the credentials. Any selection change disarms.
local FORGET_TIMEOUT_MS = 8000
local forget_armed = false
local forget_timer = nil

local function disarm_forget()
    forget_armed = false
    if forget_timer then forget_timer:delete(); forget_timer = nil end
    forget_lbl:set({ text = "Forget" })
end

local function hide_saved_actions()
    saved_sel = ""
    disarm_forget()
    connect_btn:add_flag(lvgl.FLAG.HIDDEN)
    forget_btn:add_flag(lvgl.FLAG.HIDDEN)
end

local function show_saved_actions(ssid)
    disarm_forget()
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
    if not forget_armed then
        forget_armed = true
        forget_lbl:set({ text = "Sure?" })
        status.text = "Tap again to forget " .. saved_sel
        forget_timer = apps.add_timer { period = FORGET_TIMEOUT_MS, cb = function(t)
            t:delete()
            forget_timer = nil
            disarm_forget()
            status.text = "Selected: " .. saved_sel
        end }
        return
    end
    _wifi_forget_cred(saved_sel)
    status.text = "Forgot " .. saved_sel
    refresh_saved()
    refresh_wifi_status()
end)

-- Password prompt for a secure network: dimmed overlay parented to the app
-- root (torn down with the app), centered box pushed as its own nav scope with
-- the textarea focused. Join / Enter saves the creds and connects; Cancel just
-- closes. nav.pop() runs BEFORE overlay:delete() (lib/emoji_popup convention).
local function show_password_popup(ssid)
    local overlay = root:Object {
        w = lvgl.HOR_RES(), h = lvgl.VER_RES(), x = 0, y = 0,
        bg_color = "#000000", bg_opa = 128, border_width = 0, pad_all = 0,
    }
    overlay:clear_flag(lvgl.FLAG.SCROLLABLE)
    overlay:add_flag(lvgl.FLAG.CLICKABLE)   -- modal: swallow taps behind it
    local box = overlay:Object {
        w = lvgl.HOR_RES() - 40, h = lvgl.SIZE_CONTENT, align = lvgl.ALIGN.CENTER,
        radius = 6, border_width = 1, pad_all = 8,
        flex = { flex_direction = "row", flex_wrap = "wrap" },
    }
    box:clear_flag(lvgl.FLAG.SCROLLABLE)

    -- Every focusable is a DIRECT child of box: gridnav does not descend into
    -- sub-containers, so a button row would be touch-only.
    box:Label { text = "Password for " .. ssid, w = lvgl.PCT(100), h = 18 }
    local pass_input = box:Textarea {
        password_mode = true, one_line = true,
        text = "", placeholder_text = "Password",
        w = lvgl.PCT(100), h = 30,
    }
    pass_input:clear_flag(lvgl.FLAG.SCROLLABLE)

    local join_btn = box:Button { w = lvgl.PCT(48), h = 26 }
    join_btn:Label { text = "Join", align = lvgl.ALIGN.CENTER }
    local cancel_btn = box:Button { w = lvgl.PCT(48), h = 26 }
    cancel_btn:Label { text = "Cancel", align = lvgl.ALIGN.CENTER }

    nav.push(box, { focus = pass_input })

    local function close()
        nav.pop()
        overlay:delete()
    end

    local function do_join()
        local pass = pass_input.text or ""
        close()
        _wifi_save_creds(ssid, pass)
        _wifi_connect(ssid, pass)
        status.text = "Joining " .. ssid .. "..."
        refresh_saved()
    end

    join_btn:onevent(lvgl.EVENT.RELEASED, do_join)
    cancel_btn:onevent(lvgl.EVENT.RELEASED, close)
    pass_input:onevent(lvgl.EVENT.KEY, function()
        if lvgl.indev.get_act():get_key() == lvgl.KEY.ENTER then do_join() end
    end)
end

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
    for i = 1, #results do
        local net = results[i]
        local label_text = net.ssid
        if net.secure then label_text = label_text .. " *" end
        label_text = label_text .. " (" .. net.rssi .. "dB)"
        local nbtn = scan_container:Button { w = lvgl.PCT(95), h = 26 }
        nbtn:Label { text = label_text, align = lvgl.ALIGN.LEFT_MID }
        nbtn:onClicked(function()
            exit_scan_select()
            if net.secure then
                show_password_popup(net.ssid)
            else
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

else
    content:Label { text = "WiFi is not available on this build.", w = lvgl.PCT(100) }
end -- wifi_avail

back_btn:onClicked(function()
    -- scan_timer is dynamic (recreated per scan, self-deleting) and not
    -- manager-tracked; kill it before teardown. The refresh timer was
    -- registered via apps.add_timer, so the manager deletes that one.
    if scan_timer then pcall(function() scan_timer:delete() end) end
    apps.go_home()
end)

return root
