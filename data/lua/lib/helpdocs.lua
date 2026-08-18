--[==[
  helpdocs.lua -- discovery and sandboxed loading of help pages.

  NOTE the [==[ level on this comment: the example below contains a long
  string, and a plain --[[ comment would end at that string's closing
  brackets rather than at the end of this block.

  A help page is a Lua file returning a table:

      local caps, dev = ...     -- caps: keyboard/trackball/touch/kbd_backlight
                                -- dev:  name (board slug), audio, screen_w/h
      local body = [[ ...written text... ]]
      if not caps.keyboard then body = "...preamble...\n\n" .. body end
      return { title = "Keyboard shortcuts", section = "Guide", order = 50,
               body = body }

  Sources, both drives:
    system pages  L:/lua/help/*.lua   and  S:/meshpunk/help/*.lua
    app pages     <app dir>/readme.lua for every record from apps.all()

  System pages are executed during discovery because their title and order are
  only knowable from the file. App pages are NOT: the title is the app's name,
  already in the registry, so listing them costs one open() each and the file
  runs when the page is opened.

  Pages run in a fresh restricted environment with no filesystem, no firmware
  bindings and no _G, and receive the capability table as a chunk argument
  rather than fetching it. That keeps a page free of any firmware-binding
  dependency. It is not a defence against a hostile app -- that app's own
  main.lua already runs unrestricted -- it keeps a help page from reaching
  internals that would break it across firmware versions.
]==]

local fileman = require("lib/fileman")
local apps    = require("lib/apps")

local M = {}

-- L: first: a system page present on both drives resolves to the internal one.
local SYS_DIRS = { "L:/lua/help", "S:/meshpunk/help" }

-- ── Sandbox ─────────────────────────────────────────────────────────────────

-- Fresh per page. The library tables are COPIES: a page assigning to
-- string.format changes only its own copy, never the real library. getmetatable
-- is absent, so the copies cannot be bypassed through a string's metatable
-- either. Nothing here can touch the filesystem, the mesh or any _ binding.
local function new_env()
    return {
        string = { format = string.format, rep = string.rep, sub = string.sub,
                   upper  = string.upper,  lower = string.lower, len = string.len,
                   gsub   = string.gsub,   find  = string.find,  match = string.match },
        table  = { concat = table.concat, insert = table.insert },
        math   = { floor = math.floor, min = math.min, max = math.max },
        ipairs = ipairs, pairs = pairs, next = next, select = select,
        tostring = tostring, tonumber = tonumber, type = type,
    }
end

-- Fresh table per page so one page cannot mutate what the next one sees.
-- Absent binding (older firmware) yields all-false rather than an error.
local function caps_snapshot()
    local ok, c = pcall(_input_caps)
    if not ok or type(c) ~= "table" then c = {} end
    return {
        keyboard      = c.keyboard      or false,
        trackball     = c.trackball     or false,
        touch         = c.touch         or false,
        kbd_backlight = c.kbd_backlight or false,
    }
end

-- Board identity and audio kind, so a page can name this device's release
-- build or say where sound comes out. Fresh table per page, same as caps.
local function dev_snapshot()
    local ok, d = pcall(_device_caps)
    if not ok or type(d) ~= "table" then d = {} end
    return {
        name     = d.name  or "",        -- board slug, e.g. "heltec_v4"
        audio    = d.audio or "none",    -- "i2s" | "buzzer" | "none"
        screen_w = d.screen_w or 0,
        screen_h = d.screen_h or 0,
    }
end

-- io.open is drive-aware ("L:/..", "S:/.."). Firmware io handles have no
-- :lines(), so read whole-file.
local function read_file(path)
    local f = io.open(path, "r")
    if not f then return nil end
    local src = f:read("*a")
    f:close()
    return src
end

local function exists(path)
    local f = io.open(path, "r")
    if not f then return false end
    f:close()
    return true
end

-- Runs the page. Returns a page table, or nil plus a reason. A page returning
-- nil has opted out of this device and yields nil with no reason.
local function run_page(path)
    local src = read_file(path)
    if not src then return nil, "could not read " .. path end

    -- "t" refuses precompiled bytecode: a malformed binary chunk can take the
    -- VM down outright, and a help page has no reason to ship one.
    local chunk, cerr = load(src, "@" .. path, "t", new_env())
    if not chunk then return nil, tostring(cerr) end

    local ok, res = pcall(chunk, caps_snapshot(), dev_snapshot())
    if not ok then return nil, tostring(res) end
    if res == nil then return nil, nil end            -- opted out of this device
    if type(res) ~= "table" or type(res.body) ~= "string" then
        return nil, "page did not return { body = <string> }"
    end
    return res
end

-- ── Discovery ───────────────────────────────────────────────────────────────

local function scan_system(out)
    local seen = {}
    for _, dir in ipairs(SYS_DIRS) do
        local list = fileman.list(dir, { sizes = false })
        if list then
            for _, e in ipairs(list) do
                local key = e.name:lower()
                if e.type == "file" and key:match("%.lua$") and not seen[key] then
                    seen[key] = true
                    local path = dir .. "/" .. e.name
                    local page, err = run_page(path)
                    if page then
                        out[#out + 1] = {
                            title   = page.title or e.name:gsub("%.lua$", ""),
                            section = page.section or "Guide",
                            order   = page.order or 500,
                            body    = page.body,
                            path    = path,
                        }
                    elseif err then
                        out[#out + 1] = {
                            title   = e.name:gsub("%.lua$", ""),
                            section = "Guide",
                            order   = 999,   -- broken pages sink to the bottom
                            body    = "This help page could not be loaded.\n\n" .. err,
                            path    = path,
                        }
                    end
                end
            end
        end
    end
end

-- Costs one open() per app and runs nothing: the title is the app's own name.
-- A page's `title`/`section`/`order` are therefore ignored for app pages, and
-- an app page should not return nil -- opting out is a system-page facility,
-- because by the time an app page runs it is already listed.
local function scan_apps(out)
    local ok, list = pcall(apps.all)
    if not ok or type(list) ~= "table" then return end
    local seen = {}
    for _, rec in ipairs(list) do
        local key = (rec.raw_name or rec.name or ""):lower()
        if not rec.is_category and rec.dir and key ~= "" and not seen[key] then
            local path = rec.dir .. "/readme.lua"
            if exists(path) then
                seen[key] = true
                out[#out + 1] = {
                    title   = rec.name,
                    section = "Apps",
                    order   = 0,          -- Apps sort by title
                    path    = path,
                }
            end
        end
    end
end

-- Every discovered page, system first then app. Entries carry title/section
-- and either a resolved `body` (system) or just a `path` (app).
function M.discover()
    local out = {}
    scan_system(out)
    scan_apps(out)
    return out
end

-- Body text for an entry, resolving and caching an app page on first open.
function M.body(entry)
    if entry.body then return entry.body end
    local page, err = run_page(entry.path)
    if page then
        entry.body = page.body
    elseif err then
        entry.body = "This help page could not be loaded.\n\n" .. err
    else
        entry.body = "This page is not applicable on this device."
    end
    return entry.body
end

-- Guide before Apps; within Guide by order then title, within Apps by title.
function M.sort(entries)
    table.sort(entries, function(a, b)
        if a.section ~= b.section then return a.section == "Guide" end
        if a.section == "Guide" and a.order ~= b.order then return a.order < b.order end
        return (a.title or "") < (b.title or "")
    end)
    return entries
end

return M
