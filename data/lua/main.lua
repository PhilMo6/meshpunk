--[[
  Main entry point for MeshPunks
]]

print("Meshpunk!")
print("Loading launcher...")

local topbar = require("lib/topbar")
topbar.create()

local launcher = require("launcher")
launcher.create()
