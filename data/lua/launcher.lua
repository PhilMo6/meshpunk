--[[
  Grid based launcher for MeshPunks
  Discovers apps by scanning /lua/apps/ and SD:/meshpunk/apps/
]]

local utils = require("lib/utils")
local lvgl = require("lvgl")
local messages = require("lib/mesh/messages")
local clock_fmt = require("lib/clock_fmt")

-- Format a unix epoch as "YYYY-MM-DD HH:MM:SS[ AM/PM] <suffix>". fmt is "12" or "24".
local function format_epoch(ts, suffix, fmt)
    if not ts or ts < 1 then return "----/--/-- --:--:-- " .. (suffix or "") end
    local SECS_PER_DAY = 86400
    local days = math.floor(ts / SECS_PER_DAY)
    local rem = ts - days * SECS_PER_DAY
    local hour = math.floor(rem / 3600)
    local min = math.floor((rem % 3600) / 60)
    local sec = rem % 60

    -- Civil-from-days algorithm (Howard Hinnant), epoch 1970-01-01
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
        return string.format("%04d-%02d-%02d %02d:%02d:%02d %s %s",
                             y, m, d, h12, min, sec, ampm, suffix or "")
    else
        return string.format("%04d-%02d-%02d %02d:%02d:%02d %s",
                             y, m, d, hour, min, sec, suffix or "")
    end
end

-- Format a signed minute offset as "+HH:MM" / "-HH:MM"
local function format_offset(mins)
    local sign = (mins < 0) and "-" or "+"
    local a = math.abs(mins)
    return string.format("%s%02d:%02d", sign, math.floor(a / 60), a % 60)
end

-- Helper: check if a file exists on LittleFS
local function file_exists(path)
    local f = io.open(path, "r")
    if f then
        f:close()
        return true
    end
    return false
end

local indicator
-- Build the app list from directory scanning
local function discover_apps()
    local apps = {}
    local seen = {}

    -- Scan internal apps (LittleFS: /lua/apps/)
    local ok1, internal_dirs = pcall(_list_dir, "/lua/apps")
    if ok1 and type(internal_dirs) == "table" then
         for _, name in ipairs(internal_dirs) do
            local entry = "/lua/apps/" .. name .. "/main.lua"
            if file_exists(entry) then
                table.insert(apps, {
                    name = name,
                    entrypoint = entry,
                    source = "internal",
                    dir = "L:/lua/apps/" .. name
                })
                seen[name] = true
                print("[Launcher] Found internal: " .. name)
            end
        end
    end

    -- Scan SD card apps (/meshpunk/apps/)
    local ok2, sd_dirs = pcall(_list_dir_sd, "/meshpunk/apps")
    if ok2 and type(sd_dirs) == "table" then
        for _, name in ipairs(sd_dirs) do
            local entry = "/meshpunk/apps/" .. name .. "/main.lua"
            local ok3, exists = pcall(_file_exists_sd, entry)
            if ok3 and exists then
                if not seen[name] then
                    table.insert(apps, {
                        name = name .. " (SD)",
                        entrypoint = entry,
                        source = "sd",
                        dir = "S:/meshpunk/apps/" .. name
                    })
                    print("[Launcher] Found SD: " .. name)
                else
                    print("[Launcher] SD app " .. name .. " skipped (internal exists)")
                end
            end
        end
    end

    -- Sort alphabetically
    table.sort(apps, function(a, b) return a.name < b.name end)

    print("[Launcher] Total apps: " .. #apps)
    return apps
end


local function newScreen()
    local root = lvgl.Object({
        flex = {
            flex_direction = "row",
            flex_wrap = "wrap",
            justify_content = "center",
            align_items = "center",
            align_content = "center",
        },
        w = 320,
        h = 240,
        align = lvgl.ALIGN.CENTER,
    })
    return root
end

local function create_launcher(parent)
    local unread = 0

    -- Get past messages
    for _, msg in ipairs(messages:all()) do
        unread = unread + 1
    end

    local root = newScreen()

    -- Enable trackball navigation on the launcher grid
    _gridnav_add(root, GRIDNAV_ROLLOVER)
    local group = lvgl.group.get_default()
    group:add_obj(root)

    -- Unread indicator
    indicator = root:Label{text = unread .. ' unread', align = lvgl.ALIGN.CENTER, w = 100, h = 40}

    -- Live clock (seeded from GPS at boot, then free-running in VolatileRTCClock).
    -- Shows UTC + local (using the effective TZ offset: auto-from-GPS or manual setting).
    local function render_clock()
        local ok, ts = pcall(_rtc_time)
        local epoch = ok and ts or 0
        local ok2, off = pcall(_rtc_tz_offset_minutes)
        local off_min = (ok2 and off) or 0
        local tz_label
        if _rtc_tz_get then
            local ok3, s = pcall(_rtc_tz_get)
            if ok3 and s == "auto" then
                tz_label = "auto " .. format_offset(off_min)
            else
                tz_label = format_offset(off_min)
            end
        else
            tz_label = format_offset(off_min)
        end
        local fmt = clock_fmt.get()
        local utc_text = format_epoch(epoch, "UTC", fmt)
        local local_text = format_epoch(epoch + off_min * 60, tz_label, fmt)
        return utc_text .. "\n" .. local_text
    end

    local clock_label = root:Label{
        text = render_clock(),
        align = lvgl.ALIGN.CENTER,
        w = 260, h = 40,
    }
    local clock_timer
    clock_timer = lvgl.Timer{
        period = 1000,
        cb = function(t)
            local set_ok = pcall(function()
                clock_label:set{ text = render_clock() }
            end)
            if not set_ok then t:delete() end -- label deleted (app launched)
        end,
    }

    -- React to new messages
    messages:onMessage(function(msg)
        print("recv!")
        unread = unread + 1
        print(unread .. " unread")
        indicator.text = unread .. ' unread'
        print("set indicator again!")
    end)

    -- Discover and create app buttons
    local apps = discover_apps()

    for _, app in ipairs(apps) do
        print("Creating app button for", app.name, app.entrypoint)

        local btn = root:Button{w = 140, h = 40}
        btn:Label{text = app.name, align = lvgl.ALIGN.CENTER}

        btn:onClicked(function()
            utils.loadingPopUpAdd(nil, app.name, function()
                print("Launching app:", app.entrypoint, "dir:", app.dir)

                local success, err
                if app.source == "sd" and type(_dofile_sd) == "function" then
                    success, err = pcall(_dofile_sd, app.entrypoint, app.dir)
                else
                    success, err = pcall(function()
                        local chunk, load_err = loadfile(app.entrypoint)
                        if not chunk then return error(load_err) end
                        chunk(app.dir)
                    end)
                end
                if not success then
                    print("Error launching app:", err)
                else
                    root:delete()
                end
                return true
            end)
        end)
    end

    if #apps == 0 then
        root:Label{text = "No apps found!", align = lvgl.ALIGN.CENTER, w = 200, h = 40}
    end

    return root
end

return {
    create = create_launcher,
}
