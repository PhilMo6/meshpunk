--[[
  App manager for MeshPunks.

  Singleton via the require() cache, so anyone can ask what's running or launch
  an app from anywhere. Three jobs:

  1. Registry  -- discover every app ONCE (apps.refresh, at boot) into a cache;
                  apps.list/apps.get read the cache, no per-navigation FS scans.
  2. Launching -- apps.launch(name) loads+runs an app from anywhere (watchdog-safe
                  multi-step loader).
  3. Lifecycle -- the manager owns teardown. An app registers its root (set_root)
                  and timers (add_timer/track_timer); on exit the manager deletes
                  the timers BEFORE the root (a timer firing after its root is freed
                  is a use-after-free), with _nav_clear first and the delete deferred
                  one tick (deleting a focused gridnav body mid-event crashes hw).

  Discovery rule: a folder with main.lua is a launchable app; a folder without one
  is a category (the launcher pages into it). A category needs no main.lua.

  App contract (see any migrated app, e.g. Tools/notes):
    after building root + _nav_setup:   apps.set_root(root)
    when creating a timer:              local t = apps.add_timer{ period=.., cb=.. }
    on exit (after app-specific cleanup like nulling message callbacks / sounds):
                                        apps.go_home()        -- do NOT delete root yourself
]]

local lvgl = require("lvgl")
local utils = require("lib/utils")
local topbar = require("lib/topbar")
local background = require("lib/background")

local M = {}

-- Base directories where apps live (internal LittleFS + SD card). A downloader
-- installs into <base>/<name>; the same paths feed discovery, so this is the
-- single source of truth.
local INTERNAL_APPS = "/lua/apps"
local SD_APPS = "/meshpunk/apps"

-- ── State ──────────────────────────────────────────────────────────────────
M.current = nil       -- name of the running app, nil = launcher
M._screen = nil       -- root object of the current screen owner (app or launcher page)
M._timers = {}        -- timers registered since the last set_root
M._busy = false       -- re-entrancy guard for launch / go_home
M._sound_mark = nil   -- _sound_mark() watermark: sound ids owned by the current app

function M.set_current(name)
    M.current = name
    print("[apps] current: " .. name)
end

function M.clear_current()
    if M.current then print("[apps] cleared: " .. M.current) end
    M.current = nil
end

-- ── Registry ────────────────────────────────────────────────────────────────
-- Scan one directory level. subfolder=nil -> top level; "Games" -> that category.
-- A directory with main.lua is an app; one without is a category.
local function scan_level(subfolder)
    local internal_base = INTERNAL_APPS
    local sd_base = SD_APPS
    if subfolder then
        internal_base = internal_base .. "/" .. subfolder
        sd_base = sd_base .. "/" .. subfolder
    end

    local items, seen = {}, {}

    local ok1, dirs = pcall(_list_dir, internal_base)
    if ok1 and type(dirs) == "table" then
        for _, name in ipairs(dirs) do
            local entry = internal_base .. "/" .. name .. "/main.lua"
            local has_main = utils.file_exists(entry)
            table.insert(items, {
                name = name,
                entrypoint = has_main and entry or nil,
                source = "internal",
                dir = "L:" .. internal_base .. "/" .. name,
                is_category = not has_main,
            })
            seen[name] = true
        end
    end

    local ok2, sd_dirs = pcall(_list_dir_sd, sd_base)
    if ok2 and type(sd_dirs) == "table" then
        for _, name in ipairs(sd_dirs) do
            local entry = sd_base .. "/" .. name .. "/main.lua"
            local ok3, present = pcall(_file_exists_sd, entry)
            local has_main = ok3 and present or false
            if has_main then
                -- SD app: shown side by side with any internal namesake. Only add
                -- the " (SD)" suffix when a LittleFS entry of the same name exists,
                -- so a unique SD app keeps a clean label.
                table.insert(items, {
                    name = seen[name] and (name .. " (SD)") or name,
                    raw_name = name,
                    entrypoint = entry,
                    source = "sd",
                    dir = "S:" .. sd_base .. "/" .. name,
                    is_category = false,
                })
            elseif not seen[name] then
                -- SD category: a same-named internal category already merges the
                -- SD contents (scan_level scans both bases), so only surface an
                -- SD category when there's no internal one to merge into.
                table.insert(items, {
                    name = name,
                    raw_name = name,
                    entrypoint = nil,
                    source = "sd",
                    dir = "S:" .. sd_base .. "/" .. name,
                    is_category = true,
                })
            end
        end
    end

    table.sort(items, function(a, b) return a.name < b.name end)
    return items
end

-- The folder path component used to scan a category's contents.
local function folder_key(item) return item.raw_name or item.name end

-- (Re)build the registry cache. Call at boot and whenever apps are added/removed.
function M.refresh()
    M._top = scan_level(nil)
    M._cats = {}
    M._index = {}
    local app_count = 0
    for _, item in ipairs(M._top) do
        if item.is_category then
            local contents = scan_level(folder_key(item))
            M._cats[folder_key(item)] = contents
            for _, app in ipairs(contents) do
                if not app.is_category then
                    M._index[app.name] = app
                    app_count = app_count + 1
                end
            end
        else
            M._index[item.name] = item
            app_count = app_count + 1
        end
    end
    print("[apps] registry: " .. #M._top .. " top-level, " .. app_count .. " apps total")
    return M._top
end

-- Cached list for a level. nil = top, "Games" (or a category's folder_key) = contents.
function M.list(subfolder)
    if not M._top then M.refresh() end          -- lazy safety net
    if subfolder then return M._cats[subfolder] or {} end
    return M._top
end

-- Cached lookup of any launchable app by name (top-level or inside a category).
function M.get(name)
    if not M._top then M.refresh() end
    return M._index[name]
end

-- ── Registry queries (app management / app downloader) ──────────────────────

-- Base install directories: { internal = "/lua/apps", sd = "/meshpunk/apps" }.
-- A target dir is paths()[source] .. "/" .. name (with a category folder in
-- between for categorized apps). Shared with discovery so they never disagree.
function M.paths()
    return { internal = INTERNAL_APPS, sd = SD_APPS }
end

-- Flat list of EVERY installed app (top-level AND inside categories), sorted by
-- name. apps.list() only returns one level (categories appear there as entries);
-- this is the "everything installed" view a downloader/manager wants.
function M.all()
    if not M._top then M.refresh() end
    local out = {}
    for _, rec in pairs(M._index) do out[#out + 1] = rec end
    table.sort(out, function(a, b) return a.name < b.name end)
    return out
end

-- Is an app with this name already known to the registry? Cache-based, and
-- tolerant of the " (SD)" display suffix so exists("Foo") also matches an
-- SD-installed Foo. Call refresh() first if you just changed the filesystem.
function M.exists(name)
    if not M._top then M.refresh() end
    return (M._index[name] or M._index[name .. " (SD)"]) ~= nil
end

-- Live filesystem check: does the directory `location` already contain a
-- main.lua (i.e. is it an app)? Accepts an internal or SD path, with or without
-- an L:/S: prefix, at any depth. Reads the filesystem directly (not the cache),
-- so a downloader can test a target dir before writing and avoid clobbering an
-- existing app.
function M.is_app(location)
    if type(location) ~= "string" or location == "" then return false end
    local on_sd = location:match("^[Ss]:") ~= nil
                  or location:find(SD_APPS, 1, true) ~= nil
    local path = location:gsub("^[LlSs]:", ""):gsub("/+$", "")
    local entry = path .. "/main.lua"
    if on_sd then
        local ok, present = pcall(_file_exists_sd, entry)
        return (ok and present) and true or false
    end
    return utils.file_exists(entry)
end

-- ── App resource registration (called BY apps) ──────────────────────────────
-- Register the app's root and mark it the current screen owner. Resets the
-- timer list, so each app starts with a clean set.
function M.set_root(obj)
    M._screen = obj
    M._timers = {}
end

-- Create a screen-owning root, already registered, and hand it back. This is
-- the recommended way for an app to make its root: it pairs creation with
-- registration so the manager always knows the real current screen (and a
-- future home/back key can tear down any app). `opts` is the same property
-- table you'd pass to lvgl.Object(); omit it and configure with :set() after.
-- Apps that swap views must keep ONE root from new_root() and swap its children
-- (do NOT delete the root to change views) — see the Doom/PICO-8 launchers.
function M.new_root(opts)
    local root = opts ~= nil and lvgl.Object(opts) or lvgl.Object()
    M.set_root(root)
    return root
end

-- Register an already-created timer for teardown. Returns it for convenience.
function M.track_timer(t)
    if t then table.insert(M._timers, t) end
    return t
end

-- Create + register an lvgl.Timer in one call. Returns the timer.
function M.add_timer(opts)
    return M.track_timer(lvgl.Timer(opts))
end

-- ── Teardown + navigation ───────────────────────────────────────────────────
-- Tear down a (possibly large) view WITHOUT a watchdog-tripping synchronous
-- delete. A view whose object count scales with data (e.g. a few hundred contact
-- rows = ~3 LVGL objects each) takes long enough to delete in one call to starve
-- Core 0 and trip the 5 s task watchdog — construction is already batched across
-- ticks, so destruction must be too.
--
-- delete_leaves deletes up to `budget` LEAF objects (no children) per call,
-- descending depth-first so a deeply nested big list (body → list → N rows) is
-- still bounded per tick — chunking only the direct children wouldn't help when
-- one child holds all the rows. A leaf delete is O(1) and never recurses, so the
-- per-tick cost is bounded regardless of tree shape. Returns the leftover budget
-- and whether `obj` is now empty.
local BG_DELETE_CHUNK = 60   -- leaf objects deleted per tick
local function delete_leaves(obj, budget)
    while budget > 0 do
        local cnt = obj:get_child_cnt()
        if cnt <= 0 then break end
        local child = obj:get_child(cnt - 1)        -- tail child: O(1) removal
        local child_empty
        budget, child_empty = delete_leaves(child, budget)
        if not child_empty then return budget, false end  -- budget spent in subtree
        local before = obj:get_child_cnt()
        child:delete()
        -- A non-Lua internal child (rare) won't actually detach via :delete();
        -- bail to let the caller finish the small remainder synchronously rather
        -- than spin forever on it.
        if obj:get_child_cnt() >= before then error("undeletable child") end
        budget = budget - 1
    end
    return budget, (obj:get_child_cnt() <= 0)
end

function M.delete_view(obj)
    if not obj then return end
    -- Clear any in-flight touch/scroll gesture's references to this subtree. A
    -- synchronous :delete() resets the indev per object; this async drain does
    -- not, so without this a release/scroll still pointing into the view we're
    -- tearing down dereferences a freed ->parent once the drain frees it
    -- (LoadProhibited @ 0x4). This is what made tapping a row in a scroll list
    -- crash while tapping a plain button did not.
    pcall(_indev_reset)
    -- Hide immediately so the replacement view can build over it while the old
    -- one drains away across the next several ticks.
    if not pcall(function() obj:add_flag(lvgl.FLAG.HIDDEN) end) then
        pcall(function() obj:delete() end)   -- already invalid; best-effort
        return
    end
    M.add_timer({ period = 1, cb = function(t)
        local ok, remaining, emptied = pcall(delete_leaves, obj, BG_DELETE_CHUNK)
        -- Finalize on: error/stall (remaining == full budget → no progress made),
        -- or the tree is fully drained. The final :delete() is on an empty (or
        -- tiny remainder) container, so it's cheap.
        if (not ok) or emptied or remaining == BG_DELETE_CHUNK then
            pcall(function() obj:delete() end)
            t:delete()
        end
    end })
end

-- Delete a captured set of timers then the root, pcall-guarded.
local function destroy(scr, timers)
    for _, t in ipairs(timers or {}) do pcall(function() t:delete() end) end
    if scr then pcall(function() scr:delete() end) end
end

-- Return to the launcher. Tears down the current app: _nav_clear now (detach
-- input, safe from a handler), then delete timers+root on the next tick (off the
-- event chain), then rebuild the launcher.
function M.go_home()
    if M._busy then return end
    M._busy = true
    M.clear_current()
    _nav_clear()
    local scr, timers = M._screen, M._timers
    M._screen, M._timers = nil, {}
    lvgl.Timer({ period = 1, cb = function(t)
        t:delete()
        destroy(scr, timers)
        -- Sweep the closed app's C-side sound objects. The Lua handles are
        -- plain int wrappers with no __gc — anything the app didn't delete()
        -- on this exit path would leak its PCM buffers permanently.
        if M._sound_mark and _sound_sweep then
            _sound_sweep(M._sound_mark)
            M._sound_mark = nil
        end
        -- Drop the captured refs and force a full GC before building the
        -- launcher: luavgl frees a deleted timer's memory only at __gc (delete()
        -- just pauses it), and the closed app's closures/wrappers are now
        -- garbage. Reclaiming them here keeps the largest-free PSRAM block
        -- healthy app-to-app on this tight device; any hitch is masked by the
        -- screen rebuild.
        scr, timers = nil, nil
        collectgarbage("collect")
        M._busy = false
        require("launcher").create()
    end })
end

-- Launch an app from anywhere. name_or_record may be an app name (looked up in
-- the registry) or a record from apps.list(). Loads the target with the
-- multi-step popup (watchdog-safe); the target registers its own root/timers via
-- set_root/add_timer, then we delete the PREVIOUS screen+timers. On error nothing
-- is torn down and the current screen stays.
function M.launch(name_or_record)
    if M._busy then return false end
    local rec = type(name_or_record) == "table" and name_or_record or M.get(name_or_record)
    if not rec or rec.is_category or not rec.entrypoint then
        print("[apps] launch: cannot launch " .. tostring(rec and rec.name or name_or_record))
        return false
    end
    M._busy = true

    -- Capture what we're replacing; the target's set_root will repoint M._screen.
    local prev_screen, prev_timers = M._screen, M._timers
    M._screen, M._timers = nil, {}

    -- Sound watermark: every sound id created from here on (the target's chunk
    -- runs in the steps below) belongs to the app being launched. On success
    -- the PREVIOUS app's range [old mark, this mark) is swept — bounded, so
    -- the new app's own sounds survive an app-to-app launch.
    local sound_mark = _sound_mark and _sound_mark() or nil

    local function finish_ok()
        M.set_current(rec.name)
        destroy(prev_screen, prev_timers)   -- target already _nav_setup'd; no _nav_clear here
        if M._sound_mark and sound_mark and _sound_sweep then
            _sound_sweep(M._sound_mark, sound_mark)
        end
        M._sound_mark = sound_mark
        M._busy = false
    end
    local function finish_err(msg)
        print("[apps] launch error: " .. tostring(msg))
        -- Sweep the failed app's partial sounds (nothing newer exists);
        -- the restored app's mark stays in force.
        if sound_mark and _sound_sweep then _sound_sweep(sound_mark) end
        M._screen, M._timers = prev_screen, prev_timers   -- restore; keep current screen
        -- We stay on the launcher, so put back the chrome we tore down for the
        -- launch attempt: resume the topbar and redraw the freed wallpaper.
        topbar.raise()
        pcall(function() require("lib/theme").ensure_background() end)
        M._busy = false
    end

    -- Free the home-screen wallpaper BEFORE the app's chunk runs: a full-screen
    -- canvas/image is ~150-300KB of PSRAM the heavy apps (PICO-8/Doom/Map) need.
    -- ensure_background() redraws it when we return home (or on a failed launch).
    background.free()
    topbar.hide()   -- fully hidden (not just paused) so it can't bleed through a
                    -- transparent app body; raise() reveals it again on demand
    local step, compiled_chunk, deferred_init = 0, nil, nil
    utils.loadingPopUpAdd(nil, rec.name, function()
        step = step + 1

        if step == 1 then
            print("[apps] launching: " .. rec.name)
            if rec.source == "sd" and type(_dofile_sd) == "function" then
                local ok, err = pcall(_dofile_sd, rec.entrypoint, rec.dir)
                if not ok then finish_err(err); return true end
                finish_ok(); return true
            else
                local chunk, lerr = loadfile(rec.entrypoint)
                if not chunk then finish_err(lerr); return true end
                compiled_chunk = chunk
                return false
            end
        end

        if step == 2 and compiled_chunk then
            local ok, result = pcall(compiled_chunk, rec.dir)
            compiled_chunk = nil
            if not ok then finish_err(result); return true end
            if type(result) == "function" then deferred_init = result; return false end
            finish_ok(); return true
        end

        if deferred_init then
            local ok, done = pcall(deferred_init)
            if not ok then finish_err(done); return true end
            if done then deferred_init = nil; finish_ok(); return true end
            return false
        end

        finish_ok(); return true
    end)
    return true
end

return M
