-- Cached clock-format preference ("12" or "24").
-- File is read ONCE at first require; subsequent reads hit the in-memory value.
-- set() updates memory + persists to /clock_fmt.cfg.

local M = {}

local cached -- nil until first load

local function load_from_disk()
    local f = io.open("/clock_fmt.cfg", "r")
    if not f then return "24" end
    local s = f:read("*l") or ""
    f:close()
    s = s:gsub("%s", "")
    return (s == "12") and "12" or "24"
end

function M.get()
    if cached == nil then cached = load_from_disk() end
    return cached
end

function M.set(v)
    v = (v == "12") and "12" or "24"
    cached = v
    local f = io.open("/clock_fmt.cfg", "w")
    if not f then return false end
    f:write(v)
    f:close()
    return true
end

return M
