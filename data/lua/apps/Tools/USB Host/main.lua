-- Tools > USB Host — USB-OTG host manager.
-- Puts the T-Deck's USB port in host mode to drive a USB device. Today: a
-- USB-C audio dongle, with all device audio (tones, notifications, music,
-- game audio) routed to it. The dongle needs external VBUS: power source ->
-- powered adapter/hub -> dongle -> T-Deck.
-- Host mode takes the USB pins from Serial-JTAG, so USB serial (logs + PC
-- file transfer) is DEAD while it runs; Stop restores it (a boot-time
-- self-heal covers resets mid-session).
local lvgl  = require("lvgl")
local apps  = require("lib/apps")
local nav   = require("lib/nav")
local theme = require("lib/theme")

local root = apps.new_root()
root:set { w = lvgl.HOR_RES(), h = lvgl.VER_RES(), pad_all = 0, border_width = 0, bg_opa = 0 }
root:clear_flag(lvgl.FLAG.SCROLLABLE)

theme.show_background()

local W = lvgl.HOR_RES()
local H = lvgl.VER_RES()

local content = root:Object {
    flex = { flex_direction = "row", flex_wrap = "wrap" },
    w = W, h = H,
    border_width = 0, pad_all = 6, pad_row = 4, bg_opa = 0,
}
nav.replace(content, { flags = nav.ROLLOVER + nav.SCROLL_FIRST })

content:Label { text = "USB Host", w = lvgl.PCT(70), h = 26 }
local back_btn = content:Button { w = 50, h = 22 }
back_btn:Label { text = "Home", align = lvgl.ALIGN.CENTER }
back_btn:onClicked(function() apps.go_home() end)

-- Firmware without the USB manager bridge: hint instead of erroring.
if not (_usb_start and _usb_poll) then
    content:Label {
        text = "This firmware build has no USB\nmanager support - rebuild needed.",
        w = lvgl.PCT(100), h = 40,
    }
    return root
end

local status  = content:Label { text = "Idle. Start switches USB to host mode\n(USB serial off until Stop).", w = lvgl.PCT(100), h = 32 }
local dev_lbl  = content:Label { text = "No device.", w = lvgl.PCT(100), h = 16 }

local start_btn = content:Button { w = lvgl.PCT(48), h = 28 }
start_btn:Label { text = "Start", align = lvgl.ALIGN.CENTER }
local stop_btn = content:Button { w = lvgl.PCT(48), h = 28 }
stop_btn:Label { text = "Stop", align = lvgl.ALIGN.CENTER }

-- Persisted toggles. Label reflects the pref; tap flips it.
local route_btn = content:Button { w = lvgl.PCT(100), h = 28 }
local route_lbl = route_btn:Label { text = "", align = lvgl.ALIGN.CENTER }
local spk_btn = content:Button { w = lvgl.PCT(100), h = 28 }
local spk_lbl = spk_btn:Label { text = "", align = lvgl.ALIGN.CENTER }

local function refresh_toggles()
    route_lbl:set { text = "Route audio to USB: " .. (_usb_audio_get() and "ON" or "off") }
    spk_lbl:set   { text = "Speaker stays on: " .. (_usb_speaker_get() and "ON" or "off") }
end
refresh_toggles()

route_btn:onClicked(function()
    _usb_audio_set(not _usb_audio_get())
    refresh_toggles()
end)
spk_btn:onClicked(function()
    _usb_speaker_set(not _usb_speaker_get())
    refresh_toggles()
end)

-- Debug: 440Hz sine straight into the dongle (bypasses the mixer). Gentle
-- amplitude, but don't press it with earbuds already in your ears.
local tone_btn = content:Button { w = lvgl.PCT(100), h = 26 }
tone_btn:Label { text = "Debug tone (mind your ears)", align = lvgl.ALIGN.CENTER }

-- Dynamic driver manager (own view; defined near the bottom).
local show_drivers
local drv_btn = content:Button { w = lvgl.PCT(100), h = 26 }
drv_btn:Label { text = "USB drivers...", align = lvgl.ALIGN.CENTER }
drv_btn:onClicked(function() if show_drivers then show_drivers() end end)

-- Log panel: newest lines PREPENDED so results are visible without scrolling.
local log_panel = content:Object {
    w = lvgl.PCT(100), h = 74,
    bg_color = "#111111", bg_opa = 255, radius = 4,
    border_width = 1, border_color = "#444444", pad_all = 4,
}
local log_lbl = log_panel:Label { text = "(no output yet)", w = lvgl.PCT(100), h = lvgl.SIZE_CONTENT }

local heap_lbl = content:Label { text = "", w = lvgl.PCT(100), h = 16 }
-- Sound-pump debug line (wedge hunt): live even with the host off, and the
-- only view into the pump when host mode has USB serial disabled.
local snd_lbl = content:Label { text = "", w = lvgl.PCT(100), h = 16 }

local MAX_LINES = 40
local lines = {}
local function push_line(s)
    table.insert(lines, 1, s)
    if #lines > MAX_LINES then table.remove(lines) end
    log_lbl:set { text = table.concat(lines, "\n") }
end

start_btn:onClicked(function()
    if _usb_running() then status.text = "Already running."; return end
    local ok = _usb_start()
    status.text = ok and "Host starting (watch the log)...\nPlug in: power > adapter > dongle > T-Deck"
                     or "Start FAILED - see log."
end)

stop_btn:onClicked(function()
    if not _usb_running() then status.text = "Not running."; return end
    _usb_stop()
    status.text = "Stopping... USB serial should return\n(replug PC cable if needed)."
end)

tone_btn:onClicked(function()
    if not _usb_running() then status.text = "Start host mode first."; return end
    local on = _usb_tone()
    status.text = on and "Debug tone ON - listen for 440Hz." or "Debug tone off."
end)

-- Drain the C-side log ring; cheap when idle (one nil poll).
apps.add_timer { period = 200, cb = function()
    local n = 0
    while n < 20 do
        local line = _usb_poll()
        if not line then break end
        push_line(line)
        n = n + 1
    end
end }

-- Device + heap status. Internal SRAM is the scarce pool the host stack uses.
apps.add_timer { period = 1000, cb = function()
    local d = _usb_device()
    if d then
        local s = string.format("%s [%s]", (d.product ~= "" and d.product) or "device", d.kind)
        if d.kind == "audio" and d.rate > 0 then
            s = s .. string.format("  %dHz/%dbit%s", d.rate, d.bits,
                                   d.streaming and "  >>USB" or "")
        end
        if d.kbd then s = s .. "  [kbd]" end
        if d.msc then
            if d.msc_mb >= 1024 then
                s = s .. string.format("  [U: %.1fGB]", d.msc_mb / 1024)
            else
                s = s .. string.format("  [U: %dMB]", math.floor(d.msc_mb))
            end
        elseif d.msc_mb and d.msc_mb > 0 then
            s = s .. "  [storage: not mounted]"
        end
        dev_lbl.text = s
    elseif _usb_running() then
        dev_lbl.text = "Host on. Waiting for device..."
    else
        dev_lbl.text = "No device."
    end
    if _heap_info then
        local _, _, int_free, int_big = _heap_info()
        heap_lbl.text = string.format("int RAM %dK (big %dK)  host: %s",
            math.floor(int_free / 1024), math.floor(int_big / 1024),
            _usb_running() and "ON" or "off")
    end
    if _sound_debug then
        local d = _sound_debug()
        snd_lbl.text = string.format("snd %s d:%d m:%d st:%d pl:%d ib:%d p:%d/%d o:%d",
            d.running and "RUN" or "idle", d.dec, d.mix, d.staged, d.played,
            d.inbuff, d.pos, d.dur, d.objs)
    end
end }

-- ── Dynamic driver manager ───────────────────────────────────────────────────
-- Drivers live one-dir-each under L:/usb_drivers/ or S:/meshpunk/usb_drivers/
-- (<name>.drv.elf + `match` manifest + store .version [+ .disabled]). They
-- load when a matching device plugs in — install/enable/disable apply on the
-- NEXT PLUG, never a restart. All store calls are guarded so a store-updated
-- copy of this app degrades gracefully on older firmware (no _usb_drivers).
local fileman = require("lib/fileman")
local dl_ok, dl = pcall(require, "lib/downloader")

local DRV_BASES = {
    { drive = "L", dir = "L:/usb_drivers" },
    { drive = "S", dir = "S:/meshpunk/usb_drivers" },
}

local drv_view

local function drivers_close()
    -- Pop-before-delete: re-point nav at the main page, THEN drop the view.
    content:clear_flag(lvgl.FLAG.HIDDEN)
    nav.replace(content, { flags = nav.ROLLOVER + nav.SCROLL_FIRST })
    if drv_view then drv_view:delete(); drv_view = nil end
end

local function drivers_scan()
    local out = {}
    for _, b in ipairs(DRV_BASES) do
        local ents = fileman.list(b.dir, { sizes = false })
        if ents then
            for _, e in ipairs(ents) do
                if e.type == "dir" then
                    local dir = b.dir .. "/" .. e.name
                    local ver = (dl_ok and dl.read_version) and dl.read_version(dir) or nil
                    out[#out + 1] = {
                        id = e.name, dir = dir, drive = b.drive,
                        version  = (ver and ver.version) or "?",
                        disabled = fileman.exists(dir .. "/.disabled"),
                    }
                end
            end
        end
    end
    return out
end

local function drivers_live()
    local map, pool = {}, nil
    if _usb_drivers then
        local ok, list, p = pcall(_usb_drivers)
        if ok and type(list) == "table" then
            for _, d in ipairs(list) do map[d.name] = d end
            pool = p
        end
    end
    return map, pool
end

-- Flat driver dirs only (.drv.elf/match/.version/.disabled) — a tiny loop
-- beats dragging in the task_remove progress machinery.
local function drivers_remove(dir)
    local ents = fileman.list(dir, { sizes = false }) or {}
    for _, e in ipairs(ents) do fileman.remove(dir .. "/" .. e.name) end
    fileman.remove(dir)
end

show_drivers = function()
    content:add_flag(lvgl.FLAG.HIDDEN)
    local old_view = drv_view
    drv_view = root:Object {
        flex = { flex_direction = "row", flex_wrap = "wrap" },
        w = W, h = H,
        border_width = 0, pad_all = 6, pad_row = 4, bg_opa = 0,
    }
    -- Re-point nav BEFORE deleting the old view (pop-before-delete rule).
    nav.replace(drv_view, { flags = nav.ROLLOVER + nav.SCROLL_FIRST })
    if old_view then old_view:delete() end

    drv_view:Label { text = "USB Drivers", w = lvgl.PCT(70), h = 26 }
    local back = drv_view:Button { w = 50, h = 22 }
    back:Label { text = "Back", align = lvgl.ALIGN.CENTER }
    back:onClicked(drivers_close)

    local note = drv_view:Label { text = "", w = lvgl.PCT(100), h = 16 }

    local live, pool = drivers_live()
    if not _usb_drivers then
        note.text = "Firmware too old for dynamic drivers."
    elseif pool then
        note.text = string.format("Changes apply on next plug.  Pool free: %dK",
                                  math.floor(pool / 1024))
    end

    -- Installed drivers.
    local inst = drivers_scan()
    if #inst == 0 then
        drv_view:Label { text = "No drivers installed.", w = lvgl.PCT(100), h = 16 }
    end
    for _, it in ipairs(inst) do
        local st = live[it.id]
        local state = it.disabled and "disabled"
                   or (st and st.running and "RUNNING")
                   or (st and st.active and "loaded")
                   or "idle"
        drv_view:Label {
            text = string.format("%s v%s (%s:)  %s", it.id, it.version, it.drive, state),
            w = lvgl.PCT(52), h = 22,
        }
        local en = drv_view:Button { w = lvgl.PCT(24), h = 22 }
        en:Label { text = it.disabled and "Enable" or "Disable", align = lvgl.ALIGN.CENTER }
        en:onClicked(function()
            if it.disabled then fileman.remove(it.dir .. "/.disabled")
            else fileman.write(it.dir .. "/.disabled", "1") end
            show_drivers()   -- rebuild with fresh state
        end)
        local rm = drv_view:Button { w = lvgl.PCT(20), h = 22 }
        rm:Label { text = "Del", align = lvgl.ALIGN.CENTER }
        rm:onClicked(function()
            drivers_remove(it.dir)
            show_drivers()
        end)
    end

    -- Catalog installs (needs the store lib + WiFi; cached catalog works
    -- offline). Kept deliberately small: fetch, then per-entry install
    -- buttons for internal / SD.
    if dl_ok then
        local fetch = drv_view:Button { w = lvgl.PCT(100), h = 24 }
        fetch:Label { text = "Get drivers from App Library...", align = lvgl.ALIGN.CENTER }
        fetch:onClicked(function()
            note.text = "Fetching catalog..."
            local cat, err = dl.fetch_catalog()
            if not cat then cat = dl.load_cached_catalog() end
            if not cat then note.text = "Catalog: " .. tostring(err or "unavailable"); return end
            local list = cat.drivers or {}
            if #list == 0 then note.text = "No drivers in the catalog yet."; return end
            note.text = string.format("%d driver(s) in catalog.", #list)

            local installed = {}
            for _, it in ipairs(drivers_scan()) do installed[it.id] = it end
            for _, e in ipairs(list) do
                local have = installed[e.id]
                local gated = dl.fw_required and dl.fw_required(e)
                local tag = gated and "Needs FW"
                         or (have and dl.version_newer and dl.version_newer(e.version, have.version) and "Update")
                         or (have and "Installed")
                         or "New"
                drv_view:Label {
                    text = string.format("%s v%s  [%s]", e.name or e.id, e.version, tag),
                    w = lvgl.PCT(52), h = 22,
                }
                local function install_to(loc, base)
                    dl.run_install(root, {
                        entry = e, kind = "drivers", loc = loc,
                        final_dir = base .. "/" .. e.id,
                        old_dir = have and have.dir or nil,
                        on_done = function(ierr)
                            if ierr then note.text = tostring(ierr)
                            else show_drivers() end
                        end,
                    })
                end
                if not gated then
                    local bi = drv_view:Button { w = lvgl.PCT(24), h = 22 }
                    bi:Label { text = "-> L:", align = lvgl.ALIGN.CENTER }
                    bi:onClicked(function() install_to("internal", "L:/usb_drivers") end)
                    local info = _storage_get_info and _storage_get_info()
                    if info and info.sd_available then
                        local bs = drv_view:Button { w = lvgl.PCT(20), h = 22 }
                        bs:Label { text = "-> S:", align = lvgl.ALIGN.CENTER }
                        bs:onClicked(function() install_to("sd", "S:/meshpunk/usb_drivers") end)
                    end
                else
                    drv_view:Label { text = "(update firmware)", w = lvgl.PCT(44), h = 22 }
                end
            end
        end)
    end
end

return root
