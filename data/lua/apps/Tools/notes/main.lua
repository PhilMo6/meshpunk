--[[
  Offline Notes App for T-Deck
  Loads/saves notes.txt from its own app directory (works from internal
  flash or SD — the launcher passes the install dir as the first argument).
]]

local lvgl = require("lvgl")
local apps = require("lib/apps")

local app_dir = ...

local NOTES_PATH = app_dir .. "/notes.txt"

local root = apps.new_root {
    w = lvgl.HOR_RES(),
    h = lvgl.VER_RES(),
    flex = {
        flex_direction = "row",
        flex_wrap = "wrap",
    },
    bg_color = "#222",
    pad_all = 6,
}
_nav_setup(root, GRIDNAV_ROLLOVER)

local ta = root:Textarea {
    text = "",
    one_line = false,
    text_color = "#EEE",
    bg_color = "#444",
    border_width = 1,
    border_color = "#777",
    pad_all = 6,
    w = lvgl.PCT(100),
    h = lvgl.PCT(70),
}

-- Try to load from file
local function load_file()
    local f = io.open(NOTES_PATH, "r")
    if f then
        local txt = f:read("*a")
        ta:set { text = txt or "" }
        f:close()
    else
        print("No existing note found")
    end
end

-- Save current text to file
local function save_file()
    local f = io.open(NOTES_PATH, "w")
    if f then
        f:write(ta:get("text"))
        f:close()
        print("Note saved")
    else
        print("Failed to write file")
    end
end

local save = root:Button { w = lvgl.PCT(45), h = 40 }
save:Label { text = "Save", align = lvgl.ALIGN.CENTER }
save:onClicked(save_file)

-- save file then quit
local function quit_app()
    save_file()
    apps.go_home()
end

local quit = root:Button { w = lvgl.PCT(45), h = 40 }
quit:Label { text = "Quit", align = lvgl.ALIGN.CENTER }
quit:onClicked(quit_app)

load_file()

return root
