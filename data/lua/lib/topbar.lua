local lvgl = require("lvgl")
local clock_fmt = require("lib/clock_fmt")
local messages = require("lib/mesh/messages")

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

M.mail_suffix = use_mail_emoji and " \xE2\x9C\x89" or " unread"
local sat_prefix = use_sat_emoji and "\xF0\x9F\x9B\xB0" or "sat"

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

-- DM / @mention alerts (melody + keyboard blink) are C-side now (notify.cpp,
-- triggered from the mesh RX handlers) so they fire even while Lua is torn
-- down for an ELF run. The topbar only owns the unread badge.

function M.updateUnread()
    unread = messages:countUnread()  -- O(threads) sum of the unread counters
    if unread_label and not paused then unread_label:set{ text = unread .. M.mail_suffix } end
end

function M.create()
    -- The topbar only needs the live unread COUNTERS (countUnread reads the
    -- C-side _mesh_unread_total, bumped at mesh-task RX — so the count keeps
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
    local sat_label = bar:Label{ text = render_sat_indicator(), h = 20 }
    
    --the time label changes legnth by a couple pixels as time changes so give it a width so it does not move the flex grid
    local time_label = bar:Label{ text = render_time(), h = 20 , w = 100 } 

    local battery_label =  bar:Label{ text = render_battery_pct(), h = 20 }
    
    -- Recompute from the counters (O(threads)) rather than a running +1, so own
    -- echoes don't inflate it and opening a thread (which zeroes its counter) is
    -- reflected on the next update. DMs update the badge too now.
    messages:onMessageFirst(function(msg)
        M.updateUnread()
    end)

    messages:onDirectMessageFirst(function(msg)
        M.updateUnread()
    end)

    updateTimer = lvgl.Timer{
        period = 1000,
        cb = function(t)
            if paused then return end
            local ok = pcall(function()
                time_label:set{ text = render_time() }
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
end

function M.raise()
    paused = false
    if updateTimer then updateTimer:resume() end
    if bar then
        pcall(function() bar:clear_flag(lvgl.FLAG.HIDDEN) end)
        pcall(_obj_move_foreground, bar)
    end
    M.updateUnread()
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
