local app_dir = ...
local lvgl = require("lvgl")
local nav = require("lib/nav")

local function file_exists(path)
    local f = io.open(path, "r")
    if f then
        f:close()
        return true
    end
    return false
end

local function discover_Settings()
    local Settings = {}
    local seen = {}

    local ok1, internal_dirs = pcall(_list_dir, "/lua/apps/Settings")
    if ok1 and type(internal_dirs) == "table" then
        for _, name in ipairs(internal_dirs) do
            local entry = "/lua/apps/Settings/" .. name .. "/main.lua"
            if file_exists(entry) then
                table.insert(Settings, {
                    name = name,
                    entrypoint = entry,
                    source = "internal",
                    dir = "L:/lua/apps/Settings/" .. name
                })
                seen[name] = true
                print("[Settings] Found internal: " .. name)
            end
        end
    end

    local ok2, sd_dirs = pcall(_list_dir_sd, "/meshpunk/apps/Settings")
    if ok2 and type(sd_dirs) == "table" then
        for _, name in ipairs(sd_dirs) do
            local entry = "/meshpunk/apps/Settings/" .. name .. "/main.lua"
            local ok3, exists = pcall(_file_exists_sd, entry)
            if ok3 and exists then
                if not seen[name] then
                    table.insert(Settings, {
                        name = name .. " (SD)",
                        entrypoint = entry,
                        source = "sd",
                        dir = "S:/meshpunk/apps/Settings/" .. name
                    })
                    print("[Settings] Found SD: " .. name)
                else
                    print("[Settings] SD tool " .. name .. " skipped (internal exists)")
                end
            end
        end
    end

    table.sort(Settings, function(a, b) return a.name < b.name end)

    print("[Settings] Total Settings: " .. #Settings)
    return Settings
end

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

_gridnav_add(root, GRIDNAV_ROLLOVER)
local group = lvgl.group.get_default()
group:add_obj(root)

root:Label{text = "Settings", align = lvgl.ALIGN.CENTER, w = 260, h = 40}

local Settings = discover_Settings()

for _, tool in ipairs(Settings) do
    print("Creating tool button for", tool.name, tool.entrypoint)

    local btn = root:Button{w = 140, h = 40}
    btn:Label{text = tool.name, align = lvgl.ALIGN.CENTER}

    btn:onClicked(function()
        print("Launching tool:", tool.entrypoint, "dir:", tool.dir)

        local success, err
        if tool.source == "sd" and type(_dofile_sd) == "function" then
            success, err = pcall(_dofile_sd, tool.entrypoint, tool.dir)
        else
            success, err = pcall(function()
                local chunk, load_err = loadfile(tool.entrypoint)
                if not chunk then return error(load_err) end
                root:delete()
                chunk(tool.dir)
            end)
        end
        if not success then
            print("Error launching tool:", err)
        end
    end)
end

if #Settings == 0 then
    root:Label{text = "No Settings found!", align = lvgl.ALIGN.CENTER, w = 200, h = 40}
end

local back_btn = root:Button{w = 140, h = 40}
back_btn:Label{text = "Back", align = lvgl.ALIGN.CENTER}
back_btn:onClicked(function()
    nav.goHome(root)
end)
