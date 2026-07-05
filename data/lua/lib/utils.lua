-- Utility functions for Lua apps

local W = lvgl.HOR_RES()
local H = lvgl.VER_RES()

local utils = {}

-- Returns true if an internal (LittleFS) file exists.
-- For SD-card paths use the _file_exists_sd C++ binding instead.
function utils.file_exists(path)
    local f = io.open(path, "r")
    if f then f:close() return true end
    return false
end

-- Compose multi-codepoint emoji sequences (ZWJ families, skin tones, flags)
-- into their single-glyph PUA form for DISPLAY. Use this ONLY when building
-- label text from a name — never on identity strings that go back into C
-- bindings, table keys, or comparisons (contact search, unread counters and
-- history files are keyed by the RAW name). Message text is already composed
-- C-side on its way up to Lua; names are not, because they round-trip.
function utils.emojiText(s)
    if not s or s == "" then return s end
    local ok, r = pcall(_emoji_compose, s)
    if ok and r then return r end
    return s
end

-- Format time string
function utils.formatTime(timestamp)
    local time = os.date("*t", timestamp or os.time())
    return string.format("%02d:%02d", time.hour, time.min)
end

-- RTC-aware time helpers
-- Messages/contacts carry RTC epoch seconds (UTC). These mirror topbar's
-- civil-date maths so chat/inbox timestamps honour the device clock + tz
-- offset without depending on the Lua os clock being set.

-- Current epoch in RTC seconds (UTC). Falls back to os.time().
function utils.now()
    local ok, ts = pcall(_rtc_time)
    if ok and ts and ts > 0 then return ts end
    return os.time()
end

local SECS_PER_DAY = 86400
-- Decompose a UTC epoch into local wall-clock components (y, mo, d, hour, min).
local function local_components(ts)
    local ok, off = pcall(_rtc_tz_offset_minutes)
    local local_ts = ts + ((ok and off or 0) * 60)
    if local_ts < 0 then local_ts = 0 end
    local days = math.floor(local_ts / SECS_PER_DAY)
    local rem = local_ts - days * SECS_PER_DAY
    local hour = math.floor(rem / 3600)
    local min = math.floor((rem % 3600) / 60)
    local z = days + 719468
    local era = math.floor(z / 146097)
    local doe = z - era * 146097
    local yoe = math.floor((doe - math.floor(doe / 1460) + math.floor(doe / 36524) - math.floor(doe / 146096)) / 365)
    local y = yoe + era * 400
    local doy = doe - (365 * yoe + math.floor(yoe / 4) - math.floor(yoe / 100))
    local mp = math.floor((5 * doy + 2) / 153)
    local d = doy - math.floor((153 * mp + 2) / 5) + 1
    local mo = mp + (mp < 10 and 3 or -9)
    if mo <= 2 then y = y + 1 end
    return y, mo, d, hour, min
end

-- "HH:MM" honouring the 12/24h clock preference.
function utils.clockHM(ts)
    if not ts or ts < 1 then return "--:--" end
    local _, _, _, hour, min = local_components(ts)
    local ok, fmt = pcall(_clock_fmt_get)
    if ok and fmt == "12" then
        local ampm = (hour < 12) and "AM" or "PM"
        local h12 = hour % 12
        if h12 == 0 then h12 = 12 end
        return string.format("%d:%02d %s", h12, min, ampm)
    end
    return string.format("%02d:%02d", hour, min)
end

-- Absolute "M/D HH:MM" for tooltips / detail views.
function utils.clockDateTime(ts)
    if not ts or ts < 1 then return "unknown" end
    local _, mo, d = local_components(ts)
    return string.format("%d/%d %s", mo, d, utils.clockHM(ts))
end

-- Compact relative age: now / 5m / 3h / 2d / M/D.
function utils.relTime(ts)
    if not ts or ts < 1 then return "" end
    local diff = utils.now() - ts
    if diff < 0 then diff = 0 end
    if diff < 45 then return "now" end
    if diff < 3600 then return math.floor(diff / 60) .. "m" end
    if diff < 86400 then return math.floor(diff / 3600) .. "h" end
    if diff < 7 * SECS_PER_DAY then return math.floor(diff / SECS_PER_DAY) .. "d" end
    local _, mo, d = local_components(ts)
    return string.format("%d/%d", mo, d)
end

-- Create a simple notification
function utils.createNotification(parent, message, duration)
    duration = duration or 3000  -- default 3 seconds
    
    local notification = parent:Object {
        bg_color = "#333333", 
        radius = 8,
        border_width = 0,
        pad_all = 10,
        w = 280,
        h = lvgl.SIZE_CONTENT,
        align = lvgl.ALIGN.BOTTOM_MID,
        y = -20
    }
    
    notification:Label {
        text = message,
        text_color = "#FFFFFF",
        align = lvgl.ALIGN.CENTER,
    }
    
    -- Animate in from bottom
    notification:set { y = 50 }
    notification:Anim {
        run = true,
        start_value = 50,
        end_value = -20,
        duration = 300,
        path = "ease_out",
        exec_cb = function(obj, value)
            obj:set { y = value }
        end
    }
    
    -- Auto-destroy after duration
    notification.timer = lvgl.Timer.create(function()
        notification:delete()
    end, duration, 1)
    
    return notification
end


local loadingDone = false
local loadingPopUpOverlay
local loadingPopUpOverlayParent
local function loadingPopUpRemove()
    if loadingPopUpOverlay then loadingPopUpOverlay:delete() end
    if loadingPopUpOverlayParent then loadingPopUpOverlayParent:delete() end
    loadingPopUpOverlay = nil
    loadingPopUpOverlayParent = nil
end
function utils.loadingPopUpAdd(parent,loadingText,loadingFunction)
    if not loadingPopUpOverlay then
        if not parent then   
            parent = lvgl.Object({
            w = W,
            h = H,
            align = lvgl.ALIGN.CENTER,
            border_width = 0, pad_all = 0
        })
        loadingPopUpOverlayParent = parent
        end
        loadingPopUpOverlay = parent:Object {
            w = W, h = H, x = 0, y = 0,
            bg_opa = 200, border_width = 0, pad_all = 0,
        }
        loadingPopUpOverlay:clear_flag(lvgl.FLAG.SCROLLABLE)
        local box = loadingPopUpOverlay:Object {
            w = 220, h = 100, align = lvgl.ALIGN.CENTER,
            border_width = 1, pad_all = 10,
            flex = { flex_direction = "column", flex_wrap = "nowrap" },
        }
        box:clear_flag(lvgl.FLAG.SCROLLABLE)
        _gridnav_add(box, GRIDNAV_ROLLOVER)
        local popup_group = lvgl.group.get_default()
        popup_group:add_obj(box)
        box:Label { text = "Loading " .. loadingText .. "...", w = lvgl.PCT(100), h = 24 }
        
        loadingDone = false
        local loadingTimer = lvgl.Timer {
            period = 1,
            cb =  function(t)
                if not loadingDone then
                    loadingDone = loadingFunction()
                    if loadingDone then
                        loadingPopUpRemove()
                        t:delete()
                    end
                end
            end
        }
    end

end



return utils