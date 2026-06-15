--[[
  Main entry point for MeshPunks
]]

print("Meshpunk!")
print("Loading launcher...")

local topbar = require("lib/topbar")
topbar.create()

-- Discover all apps once, into the manager's registry. Navigation then reads
-- the cache instead of re-scanning the filesystem every time.
require("lib/apps").refresh()

local launcher = require("launcher")
launcher.create()
