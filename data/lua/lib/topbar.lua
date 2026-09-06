local lvgl = require("lvgl")
local clock_fmt = require("lib/clock_fmt")
local messages = require("lib/mesh/messages")
local nav = require("lib/nav")
local utils = require("lib/utils")

local M = {}

-- bg_opa for the status bar, following the `topbar_transparant` device setting:
-- transparent (0) lets the themed wallpaper show through; opaque (255) gives the
-- bar its themed card background. pcall-guarded so it is safe before the binding
-- exists.
local function topbar_bg_opa()
    local ok, transp = pcall(_topbar_transparant_get)
    return (ok and transp) and 0 or 255
end

local function format_epoch(ts, fmt)
    if not ts or ts < 1 then return "--:--:--" end
    local SECS_PER_DAY = 86400
    local days = math.floor(ts / SECS_PER_DAY)
    local rem = ts - days * SECS_PER_DAY
    local hour = math.floor(rem / 3600)
    local min = math.floor((rem % 3600) / 60)
    local sec = rem % 60

    days = days + 719468
    local era = math.floor(days / 146097)
    local doe = days - era * 146097
    local yoe = math.floor((doe - math.floor(doe / 1460) + math.floor(doe / 36524) - math.floor(doe / 146096)) / 365)
    local y = yoe + era * 400
    local doy = doe - (365 * yoe + math.floor(yoe / 4) - math.floor(yoe / 100))
    local mp = math.floor((5 * doy + 2) / 153)
    local d = doy - math.floor((153 * mp + 2) / 5) + 1
    local m = mp + (mp < 10 and 3 or -9)
    if m <= 2 then y = y + 1 end

    if fmt == "12" then
        local ampm = (hour < 12) and "AM" or "PM"
        local h12 = hour % 12
        if h12 == 0 then h12 = 12 end
        return string.format("%02d:%02d:%02d %s", h12, min, sec, ampm)
    else
        return string.format("%02d:%02d:%02d", hour, min, sec)
    end
end

local ok_sat, has_sat = pcall(_emoji_preload, 0x1F6F0)
local use_sat_emoji = ok_sat and has_sat
local ok_mail, has_mail = pcall(_emoji_preload, 0x2709)
local use_mail_emoji = ok_mail and has_mail
local ok_bell, has_bell = pcall(_emoji_preload, 0x1F514)
local use_bell_emoji = ok_bell and has_bell

M.mail_suffix = use_mail_emoji and " \xE2\x9C\x89" or " unread"
local sat_prefix = use_sat_emoji and "\xF0\x9F\x9B\xB0" or "sat"
local bell_suffix = use_bell_emoji and " \xF0\x9F\x94\x94" or " !"

local function render_sat_indicator()
    local ok, syncing, got_fix, has_loc, lat, lng, sats, hdop = pcall(_gps_info)
    if not ok then return sat_prefix .. " ?" end
    if syncing then
        return sat_prefix .. " ..."
    elseif got_fix and sats > 0 then
        return sat_prefix .. " " .. sats
    elseif got_fix then
        return sat_prefix .. " ok"
    else
        return sat_prefix .. " X"
    end
end

local function render_battery_pct()
    local ok, mv = pcall(_get_battery_mv)
    if not ok or not mv or mv <= 0 then return "?%" end
    local pct = math.floor((mv - 3000) / 1200 * 100 + 0.5)
    if pct < 0 then pct = 0 elseif pct > 100 then pct = 100 end
    return pct .. "%"
end

local function render_time()
    local ok, ts = pcall(_rtc_time)
    local epoch = ok and ts or 0
    local ok2, off = pcall(_rtc_tz_offset_minutes)
    local off_min = (ok2 and off) or 0
    return format_epoch(epoch + off_min * 60, clock_fmt.get())
end

local bar
local paused = false
local updateTimer
local sat_tick_max = 150
local sat_tick = sat_tick_max - 15 --we want gps to update the first time after the gps has a fix
local unread = 0
local unread_label
local unseen = 0
local notif_label
local hidden = false        -- FLAG.HIDDEN mirror (no has_flag binding to read it back)
local peeked = false        -- bar raised over a running app by the mic shortcut
local panel_overlay = nil   -- non-nil while the notification drop-down is open

-- DM / @mention alerts (melody + keyboard blink) are C-side now (notify.cpp,
-- triggered from the mesh RX handlers) so they fire even while Lua is torn
-- down for an ELF run. The topbar owns the unread badge, the notification
-- bell (fed by the C-side notification store, _notify_log_*), and the
-- drop-down that lists the stored notification lines.

function M.updateUnread()
    unread = messages:countUnread()  -- O(threads) sum of the unread counters
    if unread_label and not paused then unread_label:set{ text = unread .. M.mail_suffix } end
end

function M.updateNotif()
    local ok, n = pcall(_notify_log_unseen)
    unseen = (ok and n) or 0
    if notif_label and not paused then
        notif_label:set{ text = (unseen > 0) and (unseen .. bell_suffix) or "" }
    end
end

-- ── Notification drop-down ──────────────────────────────────────────────────
-- View-only list of the C-side notification store, pulled down from the bar
-- (tap the bar, or the mic-key shortcut). Follows the Messenger overlay shape:
-- full-screen dim + top-anchored panel, nav.push on open / nav.pop before
-- delete on close.

local function close_panel()
    if not panel_overlay then return end
    nav.pop()
    panel_overlay:delete()
    panel_overlay = nil
    if peeked then M.hide() end   -- peeked from an app: give it the screen back
end

local function open_panel()
    if panel_overlay then return end
    pcall(_notify_log_seen)   -- opening the list marks everything seen

    local ok, list = pcall(_notify_log_get)
    if not ok or type(list) ~= "table" then list = {} end

    -- Parentless -> sibling of the bar under the luavgl root; foreground so it
    -- covers whatever is up (launcher, or an app while peeked).
    local overlay = lvgl.Object {
        w = 320, h = 240, x = 0, y = 0,
        bg_color = "#000000", bg_opa = 128, border_width = 0, pad_all = 0,
    }
    overlay:clear_flag(lvgl.FLAG.SCROLLABLE)
    overlay:add_flag(lvgl.FLAG.CLICKABLE)   -- modal: swallow taps on the dim area
    pcall(_obj_move_foreground, overlay)
    panel_overlay = overlay

    -- Every focusable is a DIRECT child of the pushed container (gridnav only
    -- reaches direct children — nav_controller_pitfalls): title Label (skipped
    -- by gridnav), scrollable rows, full-width Clear button.
    -- SIZE_CONTENT height: a fixed height clipped the Clear button once the
    -- theme's flex row gaps + border were added; let the column size itself,
    -- capped to the screen. Past the cap the panel scrolls (SCROLLABLE kept),
    -- so the Clear button stays reachable however tall the content gets.
    local panel = overlay:Object {
        w = 320, h = lvgl.SIZE_CONTENT, x = 0, y = 0,
        max_height = 240,
        bg_color = "#333333", border_width = 1, border_color = "#555555",
        pad_all = 4,
        flex = { flex_direction = "column", flex_wrap = "nowrap" },
    }
    nav.push(panel)

    panel:Label { text = "Notifications", w = lvgl.PCT(100), h = 20 }

    local rows = panel:Object {
        w = lvgl.PCT(100), h = 138, bg_opa = 0, border_width = 0, pad_all = 0,
        flex = { flex_direction = "column", flex_wrap = "nowrap" },
    }
    if #list == 0 then
        rows:Label { text = "No notifications", w = lvgl.PCT(100) }
    else
        for _, rec in ipairs(list) do
            rows:Label {
                text = utils.relTime(rec.ts) .. "  " .. utils.emojiText(rec.text or ""),
                w = lvgl.PCT(100),
            }
        end
    end

    local clear_btn = panel:Button { w = lvgl.PCT(100), h = 26 }
    clear_btn:Label { text = "Clear", align = lvgl.ALIGN.CENTER }
    clear_btn:onevent(lvgl.EVENT.RELEASED, function()
        pcall(_notify_log_clear)
        M.updateNotif()
        close_panel()
    end)

    nav.tap(overlay, close_panel)   -- tap the dim area (panel doesn't bubble)
    M.updateNotif()
end

function M.toggleNotifPanel()
    if panel_overlay then close_panel() else open_panel() end
end

-- Close the drop-down if open (no-op otherwise). The panel is parentless, so
-- an app teardown (apps.home_shortcut) must close it explicitly or it would
-- linger over the rebuilt launcher.
function M.closeNotifPanel()
    if panel_overlay then close_panel() end
end

-- ── Power drop-down ─────────────────────────────────────────────────────────
-- Same overlay shape as the notification drop-down, opened by tapping the
-- battery area of the bar. Rows: Standby (immediate), Power off and Restart
-- (two-tap arm/confirm). The C bindings defer the real action to the top of
-- loop(), so handlers here just close up, paint, and call the binding.

local power_overlay = nil   -- non-nil while the power drop-down is open

local function close_power_panel()
    if not power_overlay then return end
    nav.pop()
    power_overlay:delete()
    power_overlay = nil
    if peeked then M.hide() end   -- peeked from an app: give it the screen back
end

-- Small self-expiring notice card (for refused actions).
local function power_notice(text)
    local card = lvgl.Object {
        w = 280, h = lvgl.SIZE_CONTENT, x = 20, y = 104,
        bg_color = "#333333", border_width = 1, border_color = "#555555",
        pad_all = 8,
    }
    card:clear_flag(lvgl.FLAG.SCROLLABLE)
    card:Label { text = text, w = lvgl.PCT(100) }
    pcall(_obj_move_foreground, card)
    lvgl.Timer { period = 1800, cb = function(t)
        t:delete()
        card:delete()
    end }
end

-- Full-screen farewell, painted for ~half a second before the C side takes
-- over (the bindings defer to the next loop() tick, so this stays visible).
local function power_farewell(text, fn)
    local f = lvgl.Object {
        w = 320, h = 240, x = 0, y = 0,
        bg_color = "#000000", bg_opa = 255, border_width = 0, pad_all = 0,
    }
    f:clear_flag(lvgl.FLAG.SCROLLABLE)
    f:add_flag(lvgl.FLAG.CLICKABLE)   -- swallow taps on the way down
    f:Label { text = text, align = lvgl.ALIGN.CENTER }
    pcall(_obj_move_foreground, f)
    lvgl.Timer { period = 500, cb = function(t)
        t:delete()
        local ok, accepted = pcall(fn)
        if not ok or accepted == false then
            f:delete()
            power_notice("Unavailable: USB or link session active")
        end
    end }
end

local function wake_hint()
    local ok, caps = pcall(_input_caps)
    if ok and type(caps) == "table" and caps.trackball then
        return "Click trackball to wake"
    end
    return "Press USER to wake"
end

local function open_power_panel()
    if power_overlay then return end

    local overlay = lvgl.Object {
        w = 320, h = 240, x = 0, y = 0,
        bg_color = "#000000", bg_opa = 128, border_width = 0, pad_all = 0,
    }
    overlay:clear_flag(lvgl.FLAG.SCROLLABLE)
    overlay:add_flag(lvgl.FLAG.CLICKABLE)   -- modal: swallow taps on the dim area
    pcall(_obj_move_foreground, overlay)
    power_overlay = overlay

    -- Same gridnav rule as the notification panel: every focusable is a
    -- DIRECT child of the pushed container.
    local panel = overlay:Object {
        w = 320, h = lvgl.SIZE_CONTENT, x = 0, y = 0,
        max_height = 240,
        bg_color = "#333333", border_width = 1, border_color = "#555555",
        pad_all = 4,
        flex = { flex_direction = "column", flex_wrap = "nowrap" },
    }
    nav.push(panel)

    panel:Label { text = "Power", w = lvgl.PCT(100), h = 20 }

    local standby_btn = panel:Button { w = lvgl.PCT(100), h = 26 }
    standby_btn:Label { text = "Standby - wake on alerts", align = lvgl.ALIGN.CENTER }
    local off_btn = panel:Button { w = lvgl.PCT(100), h = 26 }
    local off_label = off_btn:Label { text = "Power off", align = lvgl.ALIGN.CENTER }
    local restart_btn = panel:Button { w = lvgl.PCT(100), h = 26 }
    local restart_label = restart_btn:Label { text = "Restart", align = lvgl.ALIGN.CENTER }

    local armed = nil   -- "off" | "restart": tapped once, awaiting the confirm tap

    standby_btn:onevent(lvgl.EVENT.RELEASED, function()
        close_power_panel()
        local ok, accepted = pcall(_system_standby)
        if not ok or accepted == false then
            power_notice("Unavailable: USB or link session active")
        end
    end)

    off_btn:onevent(lvgl.EVENT.RELEASED, function()
        if armed ~= "off" then
            armed = "off"
            off_label:set{ text = "Tap again to power off" }
            restart_label:set{ text = "Restart" }
            return
        end
        close_power_panel()
        power_farewell("Powering off...\n" .. wake_hint(), _system_poweroff)
    end)

    restart_btn:onevent(lvgl.EVENT.RELEASED, function()
        if armed ~= "restart" then
            armed = "restart"
            restart_label:set{ text = "Tap again to restart" }
            off_label:set{ text = "Power off" }
            return
        end
        close_power_panel()
        power_farewell("Restarting...", _system_reboot)
    end)

    nav.tap(overlay, close_power_panel)   -- tap the dim area (panel doesn't bubble)
end

function M.togglePowerPanel()
    if power_overlay then close_power_panel() else open_power_panel() end
end

-- Close the power drop-down if open (no-op otherwise). Parentless like the
-- notification panel, so app teardown must close it explicitly too.
function M.closePowerPanel()
    if power_overlay then close_power_panel() end
end

-- Mic-key shortcut (dispatched from loop() via dispatch_topbar_shortcut).
-- Hidden bar (an app owns the screen) -> peek it over the app; peeked -> put
-- it away; visible on the launcher -> toggle the drop-down directly.
function M.on_shortcut()
    if panel_overlay then
        close_panel()             -- also unpeeks when the panel came from a peek
    elseif hidden then
        M.raise()
        peeked = true             -- raise() cleared it; mark AFTER
    elseif peeked then
        M.hide()
    else
        open_panel()
    end
end

function M.create()
    -- The topbar only needs the live unread COUNTERS (countUnread reads the
    -- C-side _store_unread_total, bumped at mesh-task RX — so the count keeps
    -- accruing even while Lua is torn down for an ELF run) — it never reads
    -- message history. Histories don't sit in Lua at all anymore: the Messenger
    -- runs its inbox on C-side summaries (messages:loadSummaries) and loads a
    -- single conversation only while its chat view is open (openThread), so the
    -- Lua arena stays small and the heavy apps (Doom/Map/PICO-8) keep their big
    -- contiguous PSRAM block. C++ persists every message before dispatch, so
    -- none of this loses data (and the unread badge is counter-based anyway).

    bar = lvgl.Object({
        flex = { flex_direction = "row", flex_wrap = "nowrap", justify_content = "space-between" },
        w = 320, h = 20, x = 0, y = 0,
        -- bg_opa follows the topbar_transparant device setting: transparent lets
        -- the themed wallpaper show behind the status text; opaque gives the plain
        -- Object its themed card background. apply_transparency() updates it live.
        border_width = 0, pad_all = 4, pad_top = 2, pad_bottom = 0, bg_opa = topbar_bg_opa(),
    })
    bar:clear_flag(lvgl.FLAG.SCROLLABLE)

    unread_label = bar:Label{ text = "", h = 20 }
    M.updateUnread()
    notif_label = bar:Label{ text = "", h = 20 }
    M.updateNotif()
    local sat_label = bar:Label{ text = render_sat_indicator(), h = 20 }

    -- The whole bar is the tap target for the notification drop-down (the
    -- 20px labels are too small to hit reliably; phone-like pull-down).
    bar:add_flag(lvgl.FLAG.CLICKABLE)
    nav.tap(bar, function() M.toggleNotifPanel() end)
    
    --the time label changes legnth by a couple pixels as time changes so give it a width so it does not move the flex grid
    local time_label = bar:Label{ text = render_time(), h = 20 , w = 100 } 

    -- Fixed width: the text is 2-4 characters wide depending on charge, and
    -- it is a tap target (below), so a text-sized label would give a moving,
    -- sometimes tiny hit area on the touchscreen.
    local battery_label =  bar:Label{ text = render_battery_pct(), h = 20, w = 46,
                                      align = lvgl.ALIGN.RIGHT_MID }

    -- The battery area is its own tap target: it opens the power drop-down
    -- instead of the notification panel (a clickable child swallows the tap,
    -- so the rest of the bar keeps the notification behavior above).
    battery_label:add_flag(lvgl.FLAG.CLICKABLE)
    nav.tap(battery_label, function() M.togglePowerPanel() end)
    
    -- Recompute from the counters (O(threads)) rather than a running +1, so own
    -- echoes don't inflate it and opening a thread (which zeroes its counter) is
    -- reflected on the next update. DMs update the badge too now.
    messages:onMessageFirst(function(msg)
        M.updateUnread()
        M.updateNotif()
    end)

    messages:onDirectMessageFirst(function(msg)
        M.updateUnread()
        M.updateNotif()
    end)

    updateTimer = lvgl.Timer{
        period = 1000,
        cb = function(t)
            if paused then return end
            local ok = pcall(function()
                time_label:set{ text = render_time() }
                -- Bell badge every tick (one C int read): also catches room
                -- msgs and future non-mesh posts with no event plumbing.
                M.updateNotif()
                -- Unread badge too (also one C int read since the counters
                -- moved into the shared store): protocol modules bump the
                -- counter with NO RxEvent plumbing, so the event callbacks
                -- above never fire for them — the timer is their only path.
                M.updateUnread()
                sat_tick = sat_tick + 1
                if sat_tick >= sat_tick_max then
                    sat_tick = 0
                    sat_label:set{ text = render_sat_indicator() }
                    battery_label:set{ text = render_battery_pct() }
                end
            end)
            if not ok then t:delete() end
        end,
    }
end

function M.pause()
    paused = true
    if updateTimer then updateTimer:pause() end
    sat_tick = sat_tick_max --we want to gps info to update on unpause
end

-- Fully hide the bar with FLAG.HIDDEN so it never renders, regardless of what's
-- above it. App bodies are now transparent (for theming), so a z-order drop no
-- longer hides the bar — it would show through. The object stays alive, so
-- M.raise() can reveal it on demand (e.g. to peek the time/notifications while
-- an app is running).
function M.hide()
    M.pause()
    if bar then pcall(function() bar:add_flag(lvgl.FLAG.HIDDEN) end) end
    hidden = true
    peeked = false   -- lifecycle hides (apps.launch) must never strand a peek
end

function M.raise()
    paused = false
    if updateTimer then updateTimer:resume() end
    if bar then
        pcall(function() bar:clear_flag(lvgl.FLAG.HIDDEN) end)
        pcall(_obj_move_foreground, bar)
    end
    hidden = false
    peeked = false   -- launcher raises reset peek state; on_shortcut re-marks
    M.updateUnread()
    M.updateNotif()
end

-- Back-compat alias: dropping the bar below other widgets no longer hides it
-- (transparent app bodies), so route the old "lower" through the HIDDEN flag.
function M.lower()
    M.hide()
end

-- Re-read the topbar_transparant setting and apply it to the live bar. The bar
-- is created once at boot and persists, so the Device Settings toggle calls this
-- to take effect without a reboot (visible next time the bar is shown).
function M.apply_transparency()
    if bar then pcall(function() bar:set({ bg_opa = topbar_bg_opa() }) end) end
end

return M
