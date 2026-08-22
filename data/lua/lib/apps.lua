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

  Background apps (opt-in; see the Music app for the reference client):
    an app may register a BACKGROUND CONTRACT (apps.register_background) naming
    what survives when its UI closes: a state table (the rendezvous point a
    relaunched instance rebinds to), a UI-FREE tick the manager runs on its own
    timer while backgrounded, a LIVE sound-id provider consulted at sweep time
    so exit sweeps spare those ids, and an on_close that stops everything
    without any UI existing. apps.go_background(key) exits keeping all that
    alive; apps.close_background(key) is the deliberate close (the launcher
    shows one row per backgrounded app with exactly that as its X button).
    Rules: ticks run inside every other app's frame budget — keep them CHEAP;
    re-register on every launch (fresh closures; stops the manager tick while
    the app is foreground); route exits through go_background/close — a plain
    go_home keeps the contract's sounds alive but does not start the tick.
    ELF launches still tear down the whole Lua state, background apps included.
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
M._background = {}    -- key -> background contract record (see register_background)
M._bg_listener = nil  -- launcher callback(rec, status): a record's status changed

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
    M._on_close = nil
end

-- Register a cleanup callback for the CURRENT foreground app. go_home() (and
-- therefore the home chord) invokes it exactly once, before teardown, then
-- clears it; set_root also clears it, so a stale callback never outlives its
-- app. For cleanup that must survive a full Lua teardown (ELF launch), use a
-- background contract or a C-side mechanism instead — this hook only covers
-- Lua-side close paths.
function M.set_on_close(fn)
    M._on_close = fn
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

-- ── Background contracts ─────────────────────────────────────────────────────
-- See the header. A record survives app exits; only close_background (or an
-- ELF launch's full Lua teardown) removes it.

-- Register (or refresh) a background contract. Re-registering on every launch
-- is the intended pattern: it swaps fresh closures in and stops the manager's
-- background tick — the app's own foreground timers take over while open.
--   key      caller-chosen record id ("music"); go/close/background_of use it
--   app_name registry name, so the launcher row can relaunch the app
--   state    the app's survivable state table (rendezvous on relaunch)
--   period   background tick cadence in ms (default 400)
--   tick     UI-FREE heartbeat run while backgrounded (auto-advance etc.)
--   sounds   LIVE provider fn -> array of sound ids to spare from exit sweeps
--   on_close deliberate-close handler; must work with NO UI alive
--   status   fn -> short line for the launcher's background row
function M.register_background(c)
    if not (c and c.key) then return nil end
    local rec = M._background[c.key] or {}
    if rec._timer then pcall(function() rec._timer:delete() end); rec._timer = nil end
    rec.key      = c.key
    rec.app_name = c.app_name or rec.app_name
    rec.state    = c.state or rec.state or {}
    rec.period   = c.period or rec.period or 400
    rec.tick     = c.tick
    rec.sounds   = c.sounds
    rec.on_close = c.on_close
    rec.status   = c.status
    M._background[c.key] = rec
    return rec
end

function M.background_of(key)
    return M._background[key]
end

-- Sorted array of records, for the launcher's background rows. Also snapshots
-- each record's current status so the change relay below has a baseline.
function M.background_list()
    local out = {}
    for _, rec in pairs(M._background) do
        if rec.status then
            local ok, s = pcall(rec.status)
            rec._last_status = ok and s or nil
        end
        out[#out + 1] = rec
    end
    table.sort(out, function(a, b) return a.key < b.key end)
    return out
end

-- The launcher registers ONE listener while its page shows background rows;
-- it's cleared on page swaps and app launches (the labels die with the page).
-- Called as listener(rec, status) whenever a record's status() output changes
-- after its background tick — event-driven row updates, no polling.
function M.set_background_listener(fn)
    M._bg_listener = fn
end

-- Sound ids currently protected by background contracts. Consulted at SWEEP
-- time, not registration: a backgrounded app's tick keeps creating NEW ids
-- (e.g. each auto-advanced track) that fall inside later apps' watermark
-- ranges — only the provider knows the live set.
local function protected_sound_ids()
    local ids = {}
    for _, rec in pairs(M._background) do
        if rec.sounds then
            local ok, list = pcall(rec.sounds)
            if ok and type(list) == "table" then
                for _, id in ipairs(list) do
                    if type(id) == "number" then ids[#ids + 1] = id end
                end
            end
        end
    end
    table.sort(ids)
    return ids
end

-- _sound_sweep(from[, to]) that spares protected ids by sweeping the gaps
-- around them. Drop-in for every sweep site below.
local function sweep_protected(from, to)
    if not (_sound_sweep and from) then return end
    local lo = from
    for _, id in ipairs(protected_sound_ids()) do
        if id >= lo and (to == nil or id < to) then
            if id > lo then _sound_sweep(lo, id) end
            lo = id + 1
        end
    end
    if to == nil then
        _sound_sweep(lo)
    elseif lo < to then
        _sound_sweep(lo, to)
    end
end

-- The deliberate close: stop the tick, run the app's on_close, safety-delete
-- any still-protected sound ids (idempotent — on_close normally already did),
-- and drop the record. Callable with the app closed (the launcher's X row),
-- so on_close must not touch UI.
function M.close_background(key)
    local rec = M._background[key]
    if not rec then return end
    if rec._timer then pcall(function() rec._timer:delete() end); rec._timer = nil end
    local ids = {}
    if rec.sounds then
        local ok, list = pcall(rec.sounds)
        if ok and type(list) == "table" then ids = list end
    end
    if rec.on_close then pcall(rec.on_close) end
    if _sound_delete then
        for _, id in ipairs(ids) do
            if type(id) == "number" then pcall(_sound_delete, id) end
        end
    end
    M._background[key] = nil
end

-- Close every background contract (used by modes that need exclusive device
-- ownership, e.g. USB drive mode stopping Music before the SD card is handed
-- to a PC). Keys are collected first: close_background mutates the registry.
function M.close_all_backgrounds()
    local keys = {}
    for key in pairs(M._background) do keys[#keys + 1] = key end
    for _, key in ipairs(keys) do M.close_background(key) end
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
    if M._on_close then
        local fn = M._on_close
        M._on_close = nil
        pcall(fn)
    end
    M.clear_current()
    _nav_clear()
    local scr, timers = M._screen, M._timers
    M._screen, M._timers = nil, {}
    lvgl.Timer({ period = 1, cb = function(t)
        t:delete()
        destroy(scr, timers)
        -- Sweep the closed app's C-side sound objects. The Lua handles are
        -- plain int wrappers with no __gc — anything the app didn't delete()
        -- on this exit path would leak its PCM buffers permanently. Ids named
        -- by background contracts are spared (sweep_protected).
        if M._sound_mark then
            sweep_protected(M._sound_mark)
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

-- Alt+Backspace home chord (dispatched from loop() via dispatch_home_shortcut):
-- close any parentless popups first — they float ABOVE M._screen, so go_home's
-- teardown would strand them over the rebuilt launcher — then a normal go_home.
-- Safe anywhere: on the launcher it just rebuilds the home page.
function M.home_shortcut()
    local ok, ep = pcall(require, "lib/emoji_popup")
    if ok and ep and ep.close then pcall(ep.close) end
    if topbar.closeNotifPanel then pcall(topbar.closeNotifPanel) end
    if topbar.closePowerPanel then pcall(topbar.closePowerPanel) end
    M.go_home()
end

-- Legacy-keyboard auto-switch toast (dispatched from loop() via
-- dispatch_kb_legacy_autoswitch, right after the C side enabled compatibility
-- mode and posted the bell notification). Parented to the active screen, so
-- an app teardown inside the display window takes the toast down with it —
-- utils.createNotification's anim/timer callbacks are pcall-guarded for
-- exactly that.
function M.kb_legacy_popup()
    utils.createNotification(lvgl.disp.get_scr_act(),
        "Old keyboard firmware detected\nCompatibility mode enabled", 6000)
end

-- Exit to the launcher KEEPING the app's background contract alive: the same
-- teardown as go_home (nav, UI root, foreground timers, sound sweep) except
-- the sweep spares the contract's live sound ids and the manager starts the
-- record's background tick. Falls back to a normal go_home when the key has
-- no registered contract.
function M.go_background(key)
    local rec = key and M._background[key] or nil
    if not rec then return M.go_home() end
    if M._busy then return end
    M._busy = true
    M.clear_current()
    _nav_clear()
    local scr, timers = M._screen, M._timers
    M._screen, M._timers = nil, {}
    lvgl.Timer({ period = 1, cb = function(t)
        t:delete()
        destroy(scr, timers)
        -- Leak protection still applies to everything the contract does NOT
        -- name — only the provider's live ids survive.
        if M._sound_mark then
            sweep_protected(M._sound_mark)
            M._sound_mark = nil
        end
        -- Manager-owned heartbeat; deleted when the app re-registers on
        -- relaunch or the record is closed. After each tick, relay a status
        -- change to the launcher's listener (background-row label updates).
        if rec.tick and not rec._timer then
            rec._timer = lvgl.Timer({
                period = rec.period or 400,
                cb = function()
                    pcall(rec.tick)
                    if rec.status and M._bg_listener then
                        local ok, s = pcall(rec.status)
                        if ok and s ~= rec._last_status then
                            rec._last_status = s
                            pcall(M._bg_listener, rec, s)
                        end
                    end
                end,
            })
        end
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
        M._bg_listener = nil   -- the launcher page (and its row labels) is gone
        destroy(prev_screen, prev_timers)   -- target already _nav_setup'd; no _nav_clear here
        if M._sound_mark and sound_mark then
            sweep_protected(M._sound_mark, sound_mark)
        end
        M._sound_mark = sound_mark
        M._busy = false
    end
    local function finish_err(msg)
        print("[apps] launch error: " .. tostring(msg))
        -- Sweep the failed app's partial sounds (nothing newer exists);
        -- the restored app's mark stays in force.
        if sound_mark then sweep_protected(sound_mark) end
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
                local ok, result = pcall(_dofile_sd, rec.entrypoint, rec.dir)
                if not ok then finish_err(result); return true end
                -- Deferred-init apps (the ELF launchers) return a phased init
                -- function; hand it to the same stepper the internal path uses.
                -- Dropping it left the app as an empty black root (SD black
                -- screen on Doom/PICO-8/GameBoy).
                if type(result) == "function" then deferred_init = result; return false end
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
