-- Settings/Firmware — firmware updates without a computer.
--
-- Over WiFi: looks up the latest MeshPunk release on GitHub, downloads this
-- board's -firmware.bin to the SD card (internal flash without a card),
-- verifies it, and restarts into the updater partition, which writes it.
-- From SD: the same for a release file the user copied onto the card.
-- Everything is driven by the _ota_* bindings (src/ota_update.cpp); the page
-- shows what they report. The download and the verify run as bounded slices
-- from a timer (_ota_step), so the page stays live and can be left.
--
-- All buttons exist from the start and are shown/hidden, never created
-- later: the nav container's children must not change while it is active.
local lvgl       = require("lvgl")
local apps       = require("lib/apps")
local nav        = require("lib/nav")
local theme      = require("lib/theme")
local fileman    = require("lib/fileman")
local downloader = require("lib/downloader")

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
content:Label { text = "Firmware", w = lvgl.PCT(70), h = 26 }
local back_btn = content:Button { w = 50, h = 22 }
back_btn:Label { text = "Home", align = lvgl.ALIGN.CENTER }

local status = content:Label { text = "", w = lvgl.PCT(100), h = 16 }

local info = _ota_info()
local board = info.board or ""
local installed = info.installed or ""

content:Label {
    text = "Installed: " .. ((installed ~= "" and installed) or "dev build"),
    w = lvgl.PCT(100), h = 16,
}
content:Label { text = "Board: " .. board, w = lvgl.PCT(100), h = 16 }

if info.mode ~= "ota" then
    -- Legacy table, Launcher install, or an empty updater slot: the reason
    -- says what to do instead; nothing on this page can proceed.
    content:Label { text = info.reason or "", w = lvgl.PCT(100), h = 48 }
    back_btn:onClicked(function()
        apps.go_home()
    end)
    return root
end

local function show(obj, on)
    if on then
        obj:clear_flag(lvgl.FLAG.HIDDEN)
    else
        obj:add_flag(lvgl.FLAG.HIDDEN)
    end
end

-- ── Update over WiFi ────────────────────────────────────────────────────────
content:Label { text = "-- Update over WiFi --", w = lvgl.PCT(100), h = 16 }

local check_btn = content:Button { w = lvgl.PCT(100), h = 30 }
check_btn:Label { text = "Check for update", align = lvgl.ALIGN.CENTER }
local avail_lbl = content:Label { text = "", w = lvgl.PCT(100), h = 16 }
local dl_btn = content:Button { w = lvgl.PCT(100), h = 30 }
local dl_lbl = dl_btn:Label { text = "Download", align = lvgl.ALIGN.CENTER }
show(dl_btn, false)
local dl_url, dl_tag = nil, nil

-- ── Update from SD card ─────────────────────────────────────────────────────
content:Label { text = "-- Update from SD card --", w = lvgl.PCT(100), h = 16 }

local sd_btn = content:Button { w = lvgl.PCT(100), h = 30 }
sd_btn:Label { text = "Scan SD card for a release file", align = lvgl.ALIGN.CENTER }
local sd_lbl = content:Label { text = "", w = lvgl.PCT(100), h = 16 }

local MAX_FILES = 6
local file_btns, file_lbls, file_paths = {}, {}, {}
for i = 1, MAX_FILES do
    file_btns[i] = content:Button { w = lvgl.PCT(100), h = 30 }
    file_lbls[i] = file_btns[i]:Label { text = "", align = lvgl.ALIGN.LEFT_MID }
    show(file_btns[i], false)
end

-- ── Staged image ────────────────────────────────────────────────────────────
local staged_hdr = content:Label { text = "-- Staged image --", w = lvgl.PCT(100), h = 16 }
local staged_lbl = content:Label { text = "", w = lvgl.PCT(100), h = 32 }
local progress_lbl = content:Label { text = "", w = lvgl.PCT(100), h = 16 }
local verify_btn = content:Button { w = lvgl.PCT(100), h = 30 }
verify_btn:Label { text = "Verify staged image", align = lvgl.ALIGN.CENTER }
local install_btn = content:Button { w = lvgl.PCT(100), h = 30 }
local install_lbl = install_btn:Label { text = "Restart to install", align = lvgl.ALIGN.CENTER }
local cancel_btn = content:Button { w = lvgl.PCT(100), h = 30 }
cancel_btn:Label { text = "Delete staged image", align = lvgl.ALIGN.CENTER }

local staged_path = info.staged or ""
local step_timer = nil
local armed = false

local function show_staged_rows(header, label, verify, install, cancel)
    show(staged_hdr, header)
    show(staged_lbl, label)
    show(verify_btn, verify)
    show(install_btn, install)
    show(cancel_btn, cancel)
end

local function stop_stepping()
    if step_timer then
        step_timer:delete()
        step_timer = nil
    end
end

local function disarm()
    armed = false
    install_lbl:set({ text = "Restart to install" })
end

local function refresh_staged()
    local now = _ota_info()
    staged_path = now.staged or ""
    if staged_path ~= "" then
        staged_lbl.text = "Staged: " .. staged_path
        show_staged_rows(true, true, true, false, true)
    else
        staged_lbl.text = ""
        show_staged_rows(false, false, false, false, false)
    end
    show(progress_lbl, false)
    disarm()
end

local function on_ready(r)
    stop_stepping()
    progress_lbl.text = string.format("Verified: %d KB", (r.total or 0) // 1024)
    staged_lbl.text = "Ready: " .. tostring(r.path)
    show_staged_rows(true, true, false, true, true)
    status.text = "Image verified. Restart to install."
end

local function on_error(r)
    stop_stepping()
    status.text = "Update failed: " .. tostring(r.error)
    refresh_staged()
    show(progress_lbl, true)
    progress_lbl.text = "Failed: " .. tostring(r.error)
end

local function start_stepping()
    stop_stepping()
    disarm()
    show(staged_hdr, true)
    show(staged_lbl, true)
    show(progress_lbl, true)
    show(verify_btn, false)
    show(install_btn, false)
    show(cancel_btn, true)
    step_timer = apps.add_timer { period = 30, cb = function(t)
        local ok, r = pcall(_ota_step)
        if not ok or type(r) ~= "table" then
            step_timer = nil
            t:delete()
            status.text = "Update step failed: " .. tostring(r)
            return
        end
        if r.phase == "download" then
            progress_lbl.text = string.format("Downloading %d / %d KB",
                (r.done or 0) // 1024, (r.total or 0) // 1024)
        elseif r.phase == "verify" then
            local pct = 0
            if (r.total or 0) > 0 then pct = math.floor((r.done or 0) * 100 / r.total) end
            progress_lbl.text = "Verifying " .. pct .. "%"
        elseif r.phase == "ready" then
            step_timer = nil
            t:delete()
            on_ready(r)
        else
            step_timer = nil
            t:delete()
            on_error(r)
        end
    end }
end

-- Starts a session on a file or URL and reports the refusal, if any.
local function begin(source, arg, label)
    local ok, r = pcall(_ota_begin, source, arg)
    if not ok or type(r) ~= "table" then
        status.text = "Update failed: " .. tostring(r)
        return
    end
    if not r.ok then
        status.text = "Update failed: " .. tostring(r.error)
        return
    end
    staged_lbl.text = label .. " " .. tostring(r.path)
    progress_lbl.text = source == "url" and "Downloading..." or "Verifying..."
    status.text = ""
    start_stepping()
end

check_btn:onClicked(function()
    status.text = "Connecting to WiFi..."
    avail_lbl.text = ""
    show(dl_btn, false)
    downloader.wifi_wait(15000, function(connected)
        if not connected then
            status.text = "WiFi not connected"
            return
        end
        status.text = "Checking GitHub releases..."
        local ok, r = pcall(_ota_check)
        if not ok or type(r) ~= "table" then
            status.text = "Check failed: " .. tostring(r)
            return
        end
        if not r.ok then
            status.text = "Check failed: " .. tostring(r.error)
            return
        end
        status.text = ""
        local action = "Download "
        if r.newer then
            avail_lbl.text = r.tag .. " is available"
        elseif installed == "" then
            avail_lbl.text = "Latest release: " .. r.tag .. " (installed version unknown)"
        else
            -- Same release as the bundle marker: offered as a reinstall so a
            -- damaged bundle can be repaired without a computer.
            avail_lbl.text = "Up to date (latest is " .. r.tag .. ")"
            action = "Reinstall "
        end
        dl_url, dl_tag = r.url, r.tag
        dl_lbl:set({ text = action .. r.tag })
        show(dl_btn, true)
    end)
end)

dl_btn:onClicked(function()
    if not dl_url then return end
    begin("url", dl_url, "Downloading " .. tostring(dl_tag) .. " to")
end)

local function sd_scan()
    for i = 1, MAX_FILES do
        show(file_btns[i], false)
        file_paths[i] = nil
    end
    local sd_ok = false
    for _, d in ipairs(fileman.drives()) do
        if d.id == "S" and d.mounted then sd_ok = true end
    end
    if not sd_ok then
        sd_lbl.text = "No SD card"
        return
    end
    local pattern = "^meshpunk%-" .. (board:gsub("%-", "%%-")) .. "%-.*%-firmware%.bin$"
    local found = 0
    for _, dir in ipairs({ "S:/", "S:/meshpunk", "S:/meshpunk/ota" }) do
        local entries = fileman.list(dir, { sizes = false })
        if entries then
            for _, e in ipairs(entries) do
                if e.type ~= "dir" and e.name:match(pattern) and found < MAX_FILES then
                    found = found + 1
                    file_paths[found] = fileman.join(dir, e.name)
                    file_lbls[found]:set({ text = e.name })
                    show(file_btns[found], true)
                end
            end
        end
    end
    if found > 0 then
        sd_lbl.text = "Tap a file to verify it"
    else
        sd_lbl.text = "No meshpunk-" .. board .. "-*-firmware.bin on the card"
    end
end

sd_btn:onClicked(sd_scan)

for i = 1, MAX_FILES do
    file_btns[i]:onClicked(function()
        if file_paths[i] then
            begin("file", file_paths[i], "Verifying")
        end
    end)
end

verify_btn:onClicked(function()
    if staged_path ~= "" then
        begin("file", staged_path, "Verifying")
    end
end)

-- Black farewell overlay, painted before the C side restarts on its own
-- (the Power page pattern). fn returns nothing on success (the device is
-- gone) or a table with error on refusal.
local function farewell(text, fn)
    local f = lvgl.Object {
        w = lvgl.HOR_RES(), h = lvgl.VER_RES(), x = 0, y = 0,
        bg_color = "#000000", bg_opa = 255, border_width = 0, pad_all = 0,
    }
    f:clear_flag(lvgl.FLAG.SCROLLABLE)
    f:add_flag(lvgl.FLAG.CLICKABLE)   -- swallow taps on the way down
    f:Label { text = text, align = lvgl.ALIGN.CENTER }
    pcall(_obj_move_foreground, f)
    lvgl.Timer { period = 500, cb = function(t)
        t:delete()
        local ok, r = pcall(fn)
        f:delete()
        if not ok then
            status.text = "Install failed: " .. tostring(r)
        elseif type(r) == "table" and not r.ok then
            status.text = "Install failed: " .. tostring(r.error)
        end
        disarm()
    end }
end

install_btn:onClicked(function()
    if not armed then
        armed = true
        install_lbl:set({ text = "Tap again to restart and install" })
        status.text = "Tap again to confirm"
        return
    end
    farewell("Restarting into the updater...\nDo not power off.", _ota_install)
end)

cancel_btn:onClicked(function()
    stop_stepping()
    pcall(_ota_cancel)
    status.text = "Staged image deleted"
    refresh_staged()
end)

refresh_staged()

-- Leaving the page stops an in-flight download (its partial file is deleted);
-- a verified staged image stays for the "Staged image" row next time.
apps.set_on_close(function()
    stop_stepping()
    pcall(_ota_abort)
end)

back_btn:onClicked(function()
    apps.go_home()
end)

return root
