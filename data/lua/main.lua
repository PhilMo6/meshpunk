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

local launcher = require("launcher")
launcher.create()
