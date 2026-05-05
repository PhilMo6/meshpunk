local lvgl = require("lvgl")
local clock_fmt = require("lib/clock_fmt")
local messages = require("lib/mesh/messages")

local M = {}

local function format_epoch(ts, fmt)
    if not ts or ts < 1 then return "----/--/-- --:--:--" end
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
        return string.format("%02d/%02d/%02d %02d:%02d:%02d %s", m, d, y % 100, h12, min, sec, ampm)
    else
        return string.format("%02d/%02d/%02d %02d:%02d:%02d", m, d, y % 100, hour, min, sec)
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
local sat_tick = 0
local sat_tick_max = 300
local sat_tick = sat_tick_max - 15 --we want gps to update the first time after the gps has a fix
local unread = 0
local unread_label

function M.updateUnread()
    unread = messages:countUnread()
    if unread_label then unread_label:set{ text = unread .. M.mail_suffix } end
end

function M.create()
    messages:loadPersisted()

    bar = lvgl.Object({
        flex = { flex_direction = "row", flex_wrap = "nowrap", justify_content = "space-between" },
        w = 320, h = 20, x = 0, y = 0,
        border_width = 0, pad_all = 4, pad_top = 2, pad_bottom = 0,
    })
    bar:clear_flag(lvgl.FLAG.SCROLLABLE)

    unread_label = bar:Label{ text = "", h = 20 }
    M.updateUnread()
    local sat_label = bar:Label{ text = render_sat_indicator(), h = 20 }
    local time_label = bar:Label{ text = render_time(), h = 20 }

    messages:onMessageFirst(function(msg)
        unread = unread + 1
        if not paused then unread_label:set{ text = unread .. M.mail_suffix } end
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

function M.raise()
    paused = false
    if updateTimer then updateTimer:resume() end
    if bar then pcall(_obj_move_foreground, bar) end
    M.updateUnread()
end

function M.lower()
    M.pause()
    if bar then pcall(_obj_move_background, bar) end
end

return M
