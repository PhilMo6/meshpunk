--[[
  Main entry point for MeshPunks
]]

print("Meshpunk!")
print("Loading launcher...")

local topbar = require("lib/topbar")
topbar.create()

-- Inter-app clipboard (singleton via the require cache). Loaded here so any app
-- can require("lib/clipboard") and copy/paste text between apps.
require("lib/clipboard")

-- Discover all apps once, into the manager's registry. Navigation then reads
-- the cache instead of re-scanning the filesystem every time.
require("lib/apps").refresh()

-- Apply the saved UI theme: pushes its palette to the C theme (live) and draws
-- the home-screen background. Falls back to the default theme if unset/unknown.
require("lib/theme").apply(_theme_pref_get())

local launcher = require("launcher")
launcher.create()
