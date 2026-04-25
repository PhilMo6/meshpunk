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

local function discover_games()
    local games = {}
    local seen = {}

    local ok1, internal_dirs = pcall(_list_dir, "/lua/apps/Games")
    if ok1 and type(internal_dirs) == "table" then
        for _, name in ipairs(internal_dirs) do
            local entry = "/lua/apps/Games/" .. name .. "/main.lua"
            if file_exists(entry) then
                table.insert(games, {
                    name = name,
                    entrypoint = entry,
                    source = "internal",
                    dir = "L:/lua/apps/Games/" .. name
                })
                seen[name] = true
                print("[Games] Found internal: " .. name)
            end
        end
    end

    local ok2, sd_dirs = pcall(_list_dir_sd, "/meshpunk/apps/Games")
    if ok2 and type(sd_dirs) == "table" then
        for _, name in ipairs(sd_dirs) do
            local entry = "/meshpunk/apps/Games/" .. name .. "/main.lua"
            local ok3, exists = pcall(_file_exists_sd, entry)
            if ok3 and exists then
                if not seen[name] then
                    table.insert(games, {
                        name = name .. " (SD)",
                        entrypoint = entry,
                        source = "sd",
                        dir = "S:/meshpunk/apps/Games/" .. name
                    })
                    print("[Games] Found SD: " .. name)
                else
                    print("[Games] SD game " .. name .. " skipped (internal exists)")
                end
            end
        end
    end

    table.sort(games, function(a, b) return a.name < b.name end)

    print("[Games] Total games: " .. #games)
    return games
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

root:Label{text = "Games", align = lvgl.ALIGN.CENTER, w = 260, h = 40}

local games = discover_games()

for _, game in ipairs(games) do
    print("Creating game button for", game.name, game.entrypoint)

    local btn = root:Button{w = 140, h = 40}
    btn:Label{text = game.name, align = lvgl.ALIGN.CENTER}

    btn:onClicked(function()
        print("Launching game:", game.entrypoint, "dir:", game.dir)

        local success, err
        if game.source == "sd" and type(_dofile_sd) == "function" then
            success, err = pcall(_dofile_sd, game.entrypoint, game.dir)
        else
            success, err = pcall(function()
                local chunk, load_err = loadfile(game.entrypoint)
                if not chunk then return error(load_err) end
                root:delete()
                chunk(game.dir)
            end)
        end
        if not success then
            print("Error launching game:", err)
        end
    end)
end

if #games == 0 then
    root:Label{text = "No games found!", align = lvgl.ALIGN.CENTER, w = 200, h = 40}
end

local back_btn = root:Button{w = 140, h = 40}
back_btn:Label{text = "Back", align = lvgl.ALIGN.CENTER}
back_btn:onClicked(function()
    nav.goHome(root)
end)
