--[[
  downloader — shared engine for on-device catalog downloads. The App Library
  app (apps) and Settings/Theme (themes) are its two clients; anything that
  wants to install content from the meshpunk-apps GitHub repo goes through
  here so the catalog format, staging discipline and .version bookkeeping
  can never diverge between them.

  The repo's catalog.toml carries three TOML table-arrays with identical
  entry shape: "apps", "themes" and "drivers" (USB .drv.elf modules) — id
  (repo folder), name, version, author, description, type, category (apps
  only), files (relative paths to download from <base_url>/<kind>/<id>/),
  min_fw (optional integer: the minimum firmware API level — the _FW_API Lua
  global — the entry's files need), drivers (apps only, optional: a list of
  ids from the drivers table-array that the app depends on; the App Library
  auto-installs missing ones right after the app, to the app's location).
  NOTE: never write double-square-bracket TOML names inside this header —
  it is a Lua long comment and two adjacent closing square brackets in the
  text terminate it early (that exact bug has now happened twice).
  Entries with path-hostile ids/names/files ("..", "/", "\") are dropped at
  parse time — a hostile catalog must not be able to write outside its own
  staging dir.

  Install discipline (see the App Library header for the full story):
    * staging dirs live OUTSIDE the apps/themes bases (partial downloads must
      never be discovered), on the SAME drive as the destination so the final
      fileman.rename() is one atomic hop;
    * a .version file (version\nlocation\ncategory) inside the installed dir
      marks it store-managed and drives update detection;
    * callers own any post-install cache refresh (apps.refresh() for apps;
      themes need none — lib/theme rescans on every list/apply).

  UI: run_install/run_remove build their own progress modal over the caller's
  root (nav scope pushed/popped here); everything else is UI-free.
]]

local lvgl    = require("lvgl")
local apps    = require("lib/apps")
local nav     = require("lib/nav")
local fileman = require("lib/fileman")
local toml    = require("lib/toml")

local M = {}

-- Repo the downloads come from. Override by writing S:/meshpunk/appstore.toml:
--   [source]
--   url = "https://raw.githubusercontent.com/you/your-apps-repo/main"
-- (Read once per Lua state — reboot/ELF-run picks up an edited config.)
M.DEFAULT_URL = "https://raw.githubusercontent.com/PhilMo6/meshpunk-apps/main"
local CONFIG_PATH = "S:/meshpunk/appstore.toml"

-- Staging + catalog cache. Cleanup removes DIRS here; the cache is a file.
M.STAGING = { sd = "S:/meshpunk/.appstore", internal = "L:/lua/.appstore" }
local CACHE_PATH = M.STAGING.sd .. "/catalog.toml"

local base_url = nil
function M.base_url()
    if base_url then return base_url end
    base_url = M.DEFAULT_URL
    local data = fileman.read(CONFIG_PATH)
    if data then
        local ok, cfg = pcall(toml.parse, data)
        if ok and type(cfg) == "table" and type(cfg.source) == "table"
            and type(cfg.source.url) == "string" and cfg.source.url ~= "" then
            base_url = cfg.source.url:gsub("/+$", "")
        end
    end
    return base_url
end

-- ── Catalog parsing ──────────────────────────────────────────────────────────

-- Safe as a single path segment: no separators, no ".." traversal.
local function safe_segment(s)
    return type(s) == "string" and s ~= ""
        and not s:find("/", 1, true)
        and not s:find("\\", 1, true)
        and not s:find("..", 1, true)
end

-- File entries may use subdirs ("assets/x.bin") but must stay inside the
-- entry's folder: relative, no "..", no leading slash.
local function safe_relpath(s)
    return type(s) == "string" and s ~= ""
        and s:sub(1, 1) ~= "/"
        and not s:find("\\", 1, true)
        and not s:find("..", 1, true)
end

-- Validate one [[apps]]/[[themes]] array; bad entries are dropped rather than
-- failing the whole catalog. Display fields are coerced to strings so UI code
-- can concatenate them blindly.
local function sanitize_list(list)
    if type(list) ~= "table" then return {} end
    local kept = {}
    for _, e in ipairs(list) do
        local good = type(e) == "table"
            and safe_segment(e.id) and safe_segment(e.name)
            and type(e.files) == "table" and #e.files > 0
            and (e.category == nil or safe_segment(e.category))
        if good then
            for _, f in ipairs(e.files) do
                if not safe_relpath(f) then good = false end
            end
        end
        if good then
            e.version = tostring(e.version or "?")
            e.author = e.author and tostring(e.author) or nil
            e.description = e.description and tostring(e.description) or nil
            e.min_fw = tonumber(e.min_fw)   -- nil = no firmware requirement
            -- Optional USB-driver dependencies: ids from the [[drivers]]
            -- list. Installed automatically after the app (App Library).
            if type(e.drivers) == "table" then
                local ds = {}
                for _, id in ipairs(e.drivers) do
                    if safe_segment(id) then ds[#ds + 1] = id end
                end
                e.drivers = (#ds > 0) and ds or nil
            else
                e.drivers = nil
            end
            kept[#kept + 1] = e
        end
    end
    return kept
end

function M.parse_catalog(body)
    local ok, parsed = pcall(toml.parse, body)
    if not ok or type(parsed) ~= "table"
        or (type(parsed.apps) ~= "table" and type(parsed.themes) ~= "table") then
        return nil, "Bad catalog format"
    end
    parsed.apps = sanitize_list(parsed.apps)
    parsed.themes = sanitize_list(parsed.themes)
    parsed.drivers = sanitize_list(parsed.drivers)   -- [[drivers]]: USB .drv.elf
    return parsed
end

function M.fetch_catalog()
    local res = _wifi_fetch(M.base_url() .. "/catalog.toml")
    if not (res and res.success) then
        return nil, (res and res.error) or "Fetch failed"
    end
    if res.status ~= 200 then
        return nil, "HTTP " .. tostring(res.status)
    end
    local cat, err = M.parse_catalog(res.body or "")
    if not cat then return nil, err end
    -- Cache the raw body for offline browsing (SD only; skip silently without one).
    if fileman.mkdir(M.STAGING.sd) then
        fileman.write(CACHE_PATH, res.body)
    end
    return cat
end

function M.load_cached_catalog()
    local data = fileman.read(CACHE_PATH)
    if not data then return nil end
    return M.parse_catalog(data)
end

-- ── Firmware gating ──────────────────────────────────────────────────────────
-- Catalog entries may carry min_fw (integer): the minimum firmware API level
-- their files need. The firmware registers _FW_API at Lua boot (version.h);
-- firmware too old to register it reads as 0, so every gated entry blocks
-- there — the safe default. NOTE: this lib ships with FIRMWARE (data/lua/lib),
-- not through the store, so the App Library carries its own fallback copy of
-- this check — it must gate correctly even where this lib predates min_fw.

function M.fw_api()
    return tonumber(_FW_API) or 0
end

-- nil when the entry is installable on this firmware, else the API level it
-- requires (for the caller's messaging).
function M.fw_required(entry)
    local need = tonumber(entry and entry.min_fw)
    if need and need > M.fw_api() then return need end
    return nil
end

-- Ordered version compare: true only when the catalog version is strictly
-- newer than the installed one — plain inequality offered DOWNGRADES whenever
-- a device was ahead of the catalog (freshly flashed firmware, repo not
-- pushed yet). Versions split into numeric segments ("1.0.10" -> 1,0,10;
-- missing segments = 0); if either side has no digits at all, fall back to
-- inequality so exotic version strings keep updating. Rollback convention:
-- republish old content under a HIGHER version. Used by Settings/Theme (in
-- sync with this lib — both ship with firmware); the App Library carries its
-- own copy, same reason as the fw gate above.
function M.version_newer(cat_v, inst_v)
    cat_v, inst_v = tostring(cat_v or ""), tostring(inst_v or "")
    local a, b = {}, {}
    for n in cat_v:gmatch("%d+") do a[#a + 1] = tonumber(n) end
    for n in inst_v:gmatch("%d+") do b[#b + 1] = tonumber(n) end
    if #a == 0 or #b == 0 then return cat_v ~= inst_v end
    for i = 1, math.max(#a, #b) do
        local x, y = a[i] or 0, b[i] or 0
        if x ~= y then return x > y end
    end
    return false
end

-- ── WiFi ─────────────────────────────────────────────────────────────────────

-- Wait for WiFi (kicking auto-connect with saved creds), then cb(connected).
-- UI-free: the caller shows its own "Connecting..." view around this.
function M.wifi_wait(wait_ms, cb)
    if _wifi_status() == "connected" then
        cb(true)
        return
    end
    pcall(_wifi_auto_connect)
    local waited = 0
    apps.add_timer { period = 500, cb = function(t)
        if _wifi_status() == "connected" then
            t:delete()
            cb(true)
            return
        end
        waited = waited + 500
        if waited >= wait_ms then
            t:delete()
            cb(false)
        end
    end }
end

-- ── .version bookkeeping ─────────────────────────────────────────────────────
-- Store-managed dirs are exactly those with a .version file (3 lines:
-- version / location / category). Built-in firmware content never has one.

-- .version is positional: line 1 version, 2 location, 3 category (may be
-- empty for a top-level app), 4 "locked" when the app must not be removable
-- (e.g. the App Library itself). Split must PRESERVE empty interior lines so
-- a locked top-level app (empty line 3) still reads its lock on line 4.
function M.read_version(dir)
    local data = fileman.read(dir .. "/.version")
    if not data then return nil end
    data = data:gsub("\r", "")
    local lines, start = {}, 1
    while true do
        local nl = data:find("\n", start, true)
        if nl then
            lines[#lines + 1] = data:sub(start, nl - 1)
            start = nl + 1
        else
            lines[#lines + 1] = data:sub(start)
            break
        end
    end
    return {
        version  = (lines[1] ~= "" and lines[1]) or "?",
        location = (lines[2] ~= "" and lines[2]) or "?",
        category = (lines[3] ~= "" and lines[3]) or nil,
        locked   = lines[4] == "locked",
    }
end

-- ── Staging cleanup ──────────────────────────────────────────────────────────
-- Silently delete a tree in the background (no modal) — leftover staging dirs
-- from interrupted installs and post-cancel cleanup. `cleaning` marks paths a
-- background remover is still walking, so run_install won't start writing into
-- a staging dir that's being deleted out from under it.
local cleaning = {}

local function silent_remove(path)
    path = fileman.normalize(path)
    if cleaning[path] or not fileman.exists(path) then return end
    cleaning[path] = true
    local task = fileman.task_remove(path)
    apps.add_timer { period = 20, cb = function(t)
        for _ = 1, 4 do
            local done = task.step()
            if done then
                cleaning[path] = nil
                t:delete()
                return
            end
        end
    end }
end

function M.cleanup_staging()
    for _, base in pairs(M.STAGING) do
        local entries = fileman.list(base, { sizes = false })
        if entries then
            for _, e in ipairs(entries) do
                if e.type == "dir" then
                    silent_remove(base .. "/" .. e.name)
                end
            end
        end
    end
end

-- ── Install / update runner ──────────────────────────────────────────────────

-- Download + install a catalog entry. opts:
--   entry     catalog entry ({id, name, version, files, category?, ...})
--   kind      "apps" | "themes" — the repo subdir the files download from
--   loc       "sd" | "internal" — which drive (staging + .version location)
--   final_dir full drive-prefixed destination dir
--   old_dir   set for an update: staging completes first, then each staged
--             file is copied OVER the live dir (manifest files only — user
--             data living inside the app dir, e.g. saves/configs, survives)
--   on_done   fn(err) — err nil on success, "cancelled" on user cancel
--             (failed staging is cleaned up silently either way)
--
-- Builds a progress modal over `root`. Runs as a phase machine on a timer:
-- each tick does one bounded unit (one file download, one task_remove step,
-- one file applied). _wifi_download_file is synchronous, so the UI freezes
-- for one file's duration — the label updates between files (two ticks per
-- file: label renders only after the callback returns, so label and download
-- alternate). Cancel works during download; once the update starts applying,
-- it runs to completion (a half-applied app is worse than a short wait).
function M.run_install(root, opts)
    local entry, kind, loc = opts.entry, opts.kind, opts.loc
    local final_dir, old_dir, on_done = opts.final_dir, opts.old_dir, opts.on_done
    -- Firmware gate, last ditch: UIs check first and never offer the action,
    -- but no caller may install an entry this firmware can't run.
    local need = M.fw_required(entry)
    if need then
        on_done("Needs firmware update (app needs API " .. need
            .. ", device has " .. M.fw_api() .. ")")
        return
    end
    -- Kind-prefixed staging name so ids can never collide across kinds.
    local prefix = (kind == "themes" and "th_")
                or (kind == "drivers" and "dr_")
                or "app_"
    local staging = fileman.normalize(M.STAGING[loc] .. "/" .. prefix .. entry.id)
    local files = entry.files

    local W, H = lvgl.HOR_RES(), lvgl.VER_RES()
    local overlay = root:Object {
        w = W, h = H, x = 0, y = 0,
        bg_color = "#000000", bg_opa = 140, border_width = 0, pad_all = 0,
        radius = 0,
    }
    overlay:clear_flag(lvgl.FLAG.SCROLLABLE)
    overlay:add_flag(lvgl.FLAG.CLICKABLE)

    local box = overlay:Object {
        w = 250, h = lvgl.SIZE_CONTENT, align = lvgl.ALIGN.CENTER,
        radius = 6, border_width = 1, pad_all = 8,
        flex = { flex_direction = "column", flex_wrap = "nowrap" },
    }
    nav.push(box)

    box:Label { text = (old_dir and "Updating " or "Installing ") .. entry.name,
                w = lvgl.PCT(100), h = 18 }
    local cur_lbl = box:Label { text = "...", w = lvgl.PCT(100), h = 18 }
    local cnt_lbl = box:Label { text = "", w = lvgl.PCT(100), h = 16 }
    local cancelled = false
    local cancel_btn = box:Button { w = lvgl.PCT(100), h = 26 }
    cancel_btn:Label { text = "Cancel", align = lvgl.ALIGN.CENTER }
    cancel_btn:onevent(lvgl.EVENT.RELEASED, function()
        cancelled = true
    end)

    local function close(err)
        nav.pop()
        overlay:delete()
        pcall(_wifi_download_end)
        if err then silent_remove(staging) end
        on_done(err)
    end

    -- Phases: clear (old staging) -> download -> then either
    --   fresh install: finish (atomic same-drive rename), or
    --   update:        apply (copy staged files over the live dir) -> cleanup
    local phase = "clear"
    local clear_checked = false
    local clear_task = nil
    local cleanup_task = nil
    local file_idx = 0
    local labeled = false   -- label tick / download tick alternation
    local apply_list = nil  -- files + .version, built when an update applies
    local apply_idx = 0

    apps.add_timer { period = 30, cb = function(t)
        -- Cancel is only honored before anything touches the live dir.
        if cancelled and (phase == "clear" or phase == "download") then
            t:delete()
            close("cancelled")
            return
        end

        if phase == "clear" then
            -- A background cleanup (startup sweep / earlier cancel) may still
            -- be walking this exact dir — wait it out, don't race it.
            if cleaning[staging] then return end
            if not clear_checked then
                clear_checked = true
                if fileman.exists(staging) then
                    clear_task = fileman.task_remove(staging)
                end
            end
            if clear_task then
                local done, err = clear_task.step()
                if not done then return end
                if err then
                    t:delete()
                    close("Staging cleanup failed: " .. tostring(err))
                    return
                end
                clear_task = nil
            end
            if not fileman.mkdir(staging) then
                t:delete()
                close("Cannot create staging dir")
                return
            end
            phase = "download"
            return
        end

        if phase == "download" then
            if not labeled then
                file_idx = file_idx + 1
                local fname = files[file_idx]
                if not fname then
                    -- All files down. Record the install, then swap into place.
                    -- Line 4 "locked" is carried from the catalog so an update
                    -- can't silently make a locked app removable.
                    if not fileman.write(staging .. "/.version",
                        tostring(entry.version or "?") .. "\n" .. loc .. "\n"
                        .. tostring(entry.category or "")
                        .. (entry.locked and "\nlocked" or "")) then
                        t:delete()
                        close("Cannot write .version")
                        return
                    end
                    if old_dir then
                        phase = "apply"
                        apply_list = {}
                        for i = 1, #files do apply_list[i] = files[i] end
                        apply_list[#apply_list + 1] = ".version"   -- committed last
                    else
                        phase = "finish"
                    end
                    return
                end
                cur_lbl.text = fname
                cnt_lbl.text = file_idx .. " / " .. #files
                labeled = true
                return
            end
            labeled = false
            local fname = files[file_idx]
            local dst = staging .. "/" .. fname
            local parent = fileman.parent(dst)
            if parent then fileman.mkdir(parent) end
            local res = _wifi_download_file(
                M.base_url() .. "/" .. kind .. "/" .. entry.id .. "/" .. fname, dst)
            if not (res and res.success) then
                t:delete()
                close("Download failed (" .. fname .. "): "
                    .. tostring(res and res.error or "?"))
                return
            end
            return
        end

        if phase == "apply" then
            -- Update: copy one staged file per tick over the live dir. Only
            -- manifest files are touched, so saves/configs the app keeps in
            -- its own dir survive; files dropped between versions linger
            -- (harmless). .version goes last — the version bump is only
            -- recorded once every file made it.
            apply_idx = apply_idx + 1
            local fname = apply_list[apply_idx]
            if not fname then
                phase = "cleanup"
                cleanup_task = fileman.task_remove(staging)
                return
            end
            cur_lbl.text = fname
            cnt_lbl.text = "applying " .. apply_idx .. " / " .. #apply_list
            local ok, err = fileman.copy_file(staging .. "/" .. fname,
                                              final_dir .. "/" .. fname)
            if not ok then
                t:delete()
                close("Update apply failed (" .. fname .. "): " .. tostring(err))
                return
            end
            return
        end

        if phase == "cleanup" then
            local done = cleanup_task.step()
            if not done then return end
            -- Leftover staging is non-fatal; the startup sweep gets it.
            t:delete()
            close(nil)
            return
        end

        -- finish: destination parent, then the atomic same-drive rename.
        t:delete()
        local parent = fileman.parent(final_dir)
        if parent then fileman.mkdir(parent) end
        local ok, err = fileman.rename(staging, final_dir)
        if not ok then
            close("Install failed: " .. tostring(err))
            return
        end
        close(nil)
    end }
end

-- ── Uninstall ────────────────────────────────────────────────────────────────

-- Recursively remove an installed dir behind a progress modal. opts:
--   on_done     fn(err)
--   parent_base when set: after a successful remove, the now-empty parent dir
--               is removed too UNLESS it is parent_base itself. App installs
--               use this to drop an emptied category folder (an empty SD
--               category with no internal namesake shows as a blank launcher
--               page); themes pass nil — their parent IS the themes base.
function M.run_remove(root, name, dir, opts)
    local on_done = opts.on_done
    local task = fileman.task_remove(dir)

    local W, H = lvgl.HOR_RES(), lvgl.VER_RES()
    local overlay = root:Object {
        w = W, h = H, x = 0, y = 0,
        bg_color = "#000000", bg_opa = 140, border_width = 0, pad_all = 0,
        radius = 0,
    }
    overlay:clear_flag(lvgl.FLAG.SCROLLABLE)
    overlay:add_flag(lvgl.FLAG.CLICKABLE)

    local box = overlay:Object {
        w = 240, h = lvgl.SIZE_CONTENT, align = lvgl.ALIGN.CENTER,
        radius = 6, border_width = 1, pad_all = 8,
        flex = { flex_direction = "column", flex_wrap = "nowrap" },
    }
    nav.push(box)

    box:Label { text = "Removing " .. name, w = lvgl.PCT(100), h = 18 }
    local cur_lbl = box:Label { text = "...", w = lvgl.PCT(100), h = 18 }
    local cnt_lbl = box:Label { text = "", w = lvgl.PCT(100), h = 16 }

    apps.add_timer { period = 15, cb = function(t)
        local done, err
        for _ = 1, 3 do
            done, err = task.step()
            if done then break end
        end
        if not done then
            cur_lbl.text = tostring(task.current or "")
            cnt_lbl.text = (task.files_done or 0) .. " / " .. (task.files_total or "?")
            return
        end
        t:delete()
        if not err and opts.parent_base then
            local parent = fileman.parent(dir)
            if parent and fileman.normalize(parent) ~= fileman.normalize(opts.parent_base) then
                local left = fileman.list(parent, { sizes = false })
                if left and #left == 0 then fileman.remove(parent) end
            end
        end
        nav.pop()
        overlay:delete()
        on_done(err)
    end }
end

return M
