local lvgl = require("lvgl")
local nav = require("lib/nav")
local utils = require("lib/utils")
local topbar = require("lib/topbar")

local function file_exists(path)
    local f = io.open(path, "r")
    if f then f:close() return true end
    return false
end

local function create(app_dir)
    local prefix = app_dir:sub(1, 2)
    local path = app_dir:sub(3)
    local category = path:match("([^/]+)$")

    local internal_dir = "/lua/apps/" .. category
    local sd_dir = "/meshpunk/apps/" .. category

    local function discover()
        local apps, seen = {}, {}

        local ok1, internal_dirs = pcall(_list_dir, internal_dir)
        if ok1 and type(internal_dirs) == "table" then
            for _, name in ipairs(internal_dirs) do
                local entry = internal_dir .. "/" .. name .. "/main.lua"
                if file_exists(entry) then
                    table.insert(apps, {
                        name = name,
                        entrypoint = entry,
                        source = "internal",
                        dir = "L:" .. internal_dir .. "/" .. name
                    })
                    seen[name] = true
                    print("[" .. category .. "] Found internal: " .. name)
                end
            end
        end

        local ok2, sd_dirs = pcall(_list_dir_sd, sd_dir)
        if ok2 and type(sd_dirs) == "table" then
            for _, name in ipairs(sd_dirs) do
                local entry = sd_dir .. "/" .. name .. "/main.lua"
                local ok3, exists = pcall(_file_exists_sd, entry)
                if ok3 and exists and not seen[name] then
                    table.insert(apps, {
                        name = name .. " (SD)",
                        entrypoint = entry,
                        source = "sd",
                        dir = "S:" .. sd_dir .. "/" .. name
                    })
                    print("[" .. category .. "] Found SD: " .. name)
                end
            end
        end

        table.sort(apps, function(a, b) return a.name < b.name end)
        print("[" .. category .. "] Total: " .. #apps)
        return apps
    end

    local body = lvgl.Object({
        flex = {
            flex_direction = "row",
            flex_wrap = "wrap",
            justify_content = "center",
            align_items = "center",
            align_content = "center",
        },
        w = 320, h = 220, x = 0, y = 20,
        border_width = 0, pad_all = 4,
    })
    _nav_setup(body, GRIDNAV_ROLLOVER)
    topbar.raise()

    body:Label{text = category, align = lvgl.ALIGN.CENTER, w = 260, h = 40}

    local apps = discover()

    for _, app in ipairs(apps) do
        local btn = body:Button{w = 140, h = 40}
        btn:Label{text = app.name, align = lvgl.ALIGN.CENTER}

        btn:onClicked(function()
            topbar.pause()
            local step = 0
            local compiled_chunk = nil
            local deferred_init = nil
            utils.loadingPopUpAdd(nil, app.name, function()
                step = step + 1

                -- Step 1: compile / load the app
                if step == 1 then
                    print("Launching:", app.entrypoint, "dir:", app.dir)
                    if app.source == "sd" and type(_dofile_sd) == "function" then
                        local success, err = pcall(_dofile_sd, app.entrypoint, app.dir)
                        if not success then
                            print("Error launching:", err)
                            return true
                        end
                        body:delete()
                        return true
                    else
                        local chunk, load_err = loadfile(app.entrypoint)
                        if not chunk then
                            print("Error launching:", load_err)
                            return true
                        end
                        compiled_chunk = chunk
                        return false
                    end
                end

                -- Step 2: execute the compiled chunk
                if step == 2 and compiled_chunk then
                    local success, result = pcall(compiled_chunk, app.dir)
                    compiled_chunk = nil
                    if not success then
                        print("Error launching:", result)
                        return true
                    end
                    if type(result) == "function" then
                        deferred_init = result
                        return false
                    end
                    body:delete()
                    return true
                end

                -- Step 3+: app-specific deferred init steps
                if deferred_init then
                    local ok, done = pcall(deferred_init)
                    if not ok then
                        print("Error in deferred init:", done)
                        return true
                    end
                    if done then
                        deferred_init = nil
                        body:delete()
                        return true
                    end
                    return false
                end

                body:delete()
                return true
            end)
        end)
    end

    if #apps == 0 then
        body:Label{text = "No " .. category:lower() .. " found!",
                   align = lvgl.ALIGN.CENTER, w = 200, h = 40}
    end

    local back_btn = body:Button{w = 140, h = 40}
    back_btn:Label{text = "Back", align = lvgl.ALIGN.CENTER}
    back_btn:onClicked(function()
        utils.loadingPopUpAdd(nil, "Home", function()
            body:delete()
            local launcher = require("launcher")
            launcher.create()
            return true
        end)
    end)
end

return create
