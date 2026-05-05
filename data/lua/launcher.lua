--[[
  Grid based launcher for MeshPunks
  Discovers apps by scanning /lua/apps/ and SD:/meshpunk/apps/
]]

local utils = require("lib/utils")
local lvgl = require("lvgl")
local topbar = require("lib/topbar")

local function file_exists(path)
    local f = io.open(path, "r")
    if f then
        f:close()
        return true
    end
    return false
end

local function discover_apps()
    local apps = {}
    local seen = {}

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

    table.sort(apps, function(a, b) return a.name < b.name end)
    print("[Launcher] Total apps: " .. #apps)
    return apps
end


local function create_launcher(parent)
    local body = lvgl.Object({
        flex = {
            flex_direction = "row",
            flex_wrap = "wrap",
            justify_content = "center",
            align_items = "center",
            align_content = "center",
        },
        w = lvgl.HOR_RES(), h = lvgl.VER_RES(), x = 0, y = 20,
        border_width = 0, pad_all = 4,
    })
    _nav_setup(body, GRIDNAV_ROLLOVER)
    topbar.raise()

    local apps = discover_apps()

    for _, app in ipairs(apps) do
        print("Creating app button for", app.name, app.entrypoint)

        local btn = body:Button{w = 140, h = 40}
        btn:Label{text = app.name, align = lvgl.ALIGN.CENTER}

        btn:onClicked(function()
            topbar.pause()
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
                    body:delete()
                end
                return true
            end)
        end)
    end

    if #apps == 0 then
        body:Label{text = "No apps found!", align = lvgl.ALIGN.CENTER, w = 200, h = 40}
    end

    return body
end

return {
    create = create_launcher,
}
