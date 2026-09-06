--[[
  App Library — browse, install, update and remove apps from the meshpunk-apps
  GitHub repo, straight onto the device over WiFi.

  How it works:
    * lib/downloader is the engine (shared with the theme downloads in
      Settings/Theme): catalog fetch/parse/cache, staging discipline, atomic
      installs, .version bookkeeping. This app is the UI + the apps-specific
      bits: install targets under the apps bases (category = subfolder), the
      launcher-registry scan, and apps.refresh() after installs/removes so
      the launcher sees changes immediately — no reboot.
    * A .version file inside the installed app marks it store-managed
      (built-in apps never get one) and is invisible to discovery, which
      only looks for main.lua.
    * LoRa protocol packages (the catalog's protocols list) are first-class
      here: a pinned "LoRa Protocols" category lists them like apps. NOTHING
      about protocols installs silently — installing a protocol OFFERS its
      apps, and installing an app whose protocol is missing ASKS to download
      the protocol first. Activation stays in Settings > Lora.

  Catalog rows show a [Lua]/[ELF]/[Protocol] type badge; the Info view
  explains the difference (labeling, not gating, per the store's trust model).

  UI structure mirrors Tools/Files: one root, swap_view for pages, modal() over
  nav.push for dialogs, fileman tasks driven from a timer for recursive deletes.
]]

local lvgl    = require("lvgl")
local apps    = require("lib/apps")
local nav     = require("lib/nav")
local theme   = require("lib/theme")
local utils   = require("lib/utils")
local fileman = require("lib/fileman")
local dl      = require("lib/downloader")

local W = lvgl.HOR_RES()
local H = lvgl.VER_RES()

local WIFI_WAIT_MS = 15000    -- auto-connect patience before giving up

-- ── Firmware gating ──────────────────────────────────────────────────────────
-- Catalog entries may carry min_fw (integer): the minimum firmware API level
-- (the _FW_API global, registered at Lua boot; see src/version.h) their files
-- need. Deliberately NOT delegated to lib/downloader's copy of this check:
-- this app updates through the store while that lib only ships with firmware,
-- so the gate must work here even where the on-device downloader predates
-- min_fw. Old firmware never registers _FW_API — it reads as 0 and every
-- gated entry blocks, which is exactly right.
local FW_API = tonumber(_FW_API) or 0

-- nil when installable on this firmware, else the required API level.
local function fw_required(entry)
    local need = tonumber(entry and entry.min_fw)
    if need and need > FW_API then return need end
    return nil
end

-- ── LoRa protocol packages ───────────────────────────────────────────────────
-- Protocols install to internal flash (recommended: it loads even without
-- the SD card) or to the card — the user's choice, like apps. The boot loader
-- searches internal first, then the mounted card; a card-hosted package with
-- no card at boot is "not installed" = the radio-off floor with a notice.
-- Truth for "installed" = the package dir on either drive (what
-- _lora_proto_list enumerates), rechecked per render so an install in the
-- same session updates every row. BLE companions ALWAYS land on internal
-- flash: the BLE loader is SD-independent and they are small.
local PROTO_BASES   = { internal = "L:/meshpunk/lora_protos", sd = "S:/meshpunk/lora_protos" }
local BLEPROTO_BASE = "L:/meshpunk/ble_protos"
local PROTO_CAT     = "LoRa Protocols"   -- the pinned browse category

-- Where a protocol package lives: its dir and loc ("internal"|"sd"), internal
-- first (the loader's order), or nil when not installed.
local function proto_where(id)
    local d = PROTO_BASES.internal .. "/" .. id
    if fileman.exists(d) then return d, "internal" end
    d = PROTO_BASES.sd .. "/" .. id
    if fileman.exists(d) then return d, "sd" end
    return nil
end

local function proto_installed(id)
    return proto_where(id) ~= nil
end

-- nil when the entry's LoRa-protocol dependency is satisfied (or it has
-- none), else the required protocol id.
local function proto_required(entry)
    local need = entry and entry.requires_protocol
    if not need then return nil end
    if proto_installed(need) then return nil end
    return need
end

-- The protocol running this boot (nil when the binding is missing).
local function active_proto()
    local ok, a = pcall(_lora_proto)
    return ok and a or nil
end

-- Ordered version compare: true only when the catalog version is strictly
-- newer than the installed one. Plain inequality offered DOWNGRADES whenever
-- the device was ahead of the catalog (freshly flashed firmware, repo not
-- pushed yet). Versions split into numeric segments ("1.0.10" -> 1,0,10;
-- missing segments = 0); if either side has no digits at all, fall back to
-- inequality so exotic version strings keep updating. Rollback convention:
-- republish old content under a HIGHER version — lowering a catalog version
-- no longer reaches devices. (Self-contained here, like fw_required: the
-- store-updated app can't rely on the firmware-shipped downloader.)
local function version_newer(cat_v, inst_v)
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

-- ── App state ────────────────────────────────────────────────────────────────
local store = {
    catalog   = nil,     -- parsed catalog.toml ({ meta=, apps={...}, protocols={...} })
    installed = {},      -- clean name -> {version, location, category, dir, display}
    offline   = false,   -- true when showing the cached catalog
}

local root = apps.new_root()
root:set { w = W, h = H, pad_all = 0, border_width = 0, bg_opa = 0 }
root:clear_flag(lvgl.FLAG.SCROLLABLE)
theme.show_background()

local vw = nil
local cur_cat = nil        -- category page being viewed (nil = category root)
local cur_updates = false  -- true when the Updates page is showing
local show_browse, show_category, show_protocols, show_updates, refresh_view, start  -- forward decls

local function toast(msg)
    pcall(utils.createNotification, root, tostring(msg), 2500)
end

local function swap_view(builder)
    local old = vw
    vw = root:Object {
        w = W, h = H, x = 0, y = 0,
        bg_opa = 0, border_width = 0, pad_all = 0, radius = 0,
    }
    vw:clear_flag(lvgl.FLAG.SCROLLABLE)
    builder(vw)
    if old then apps.delete_view(old) end
end

-- Dimmed overlay + centered box as its own nav scope (Files-app pattern).
local function modal(box_opts, build)
    local overlay = root:Object {
        w = W, h = H, x = 0, y = 0,
        bg_color = "#000000", bg_opa = 140, border_width = 0, pad_all = 0,
        radius = 0,
    }
    overlay:clear_flag(lvgl.FLAG.SCROLLABLE)
    overlay:add_flag(lvgl.FLAG.CLICKABLE)

    local box = overlay:Object {
        w = box_opts.w or (W - 70),
        h = box_opts.h or lvgl.SIZE_CONTENT,
        align = lvgl.ALIGN.CENTER,
        radius = 6, border_width = 1, pad_all = 8,
        flex = { flex_direction = "column", flex_wrap = "nowrap" },
    }
    nav.push(box)

    local closed = false
    local function close()
        if closed then return end
        closed = true
        nav.pop()
        overlay:delete()
    end
    build(box, close)
    return close
end

local function confirm(title, warn, on_yes, on_no)
    modal({}, function(box, close)
        box:Label { text = title, w = lvgl.PCT(100) }
        if warn then
            box:Label { text = warn, text_color = "#ff5555", w = lvgl.PCT(100) }
        end
        local yes = box:Button { w = lvgl.PCT(100), h = 26 }
        yes:Label { text = "Yes", align = lvgl.ALIGN.CENTER }
        yes:onevent(lvgl.EVENT.RELEASED, function()
            close()
            on_yes()
        end)
        local no = box:Button { w = lvgl.PCT(100), h = 26 }
        no:Label { text = "No", align = lvgl.ALIGN.CENTER }
        no:onevent(lvgl.EVENT.RELEASED, function()
            close()
            if on_no then on_no() end
        end)
    end)
end

-- A plain message with a Close button (for pointers the user must read, as
-- opposed to a toast that vanishes).
local function notice(text)
    modal({}, function(box, close)
        box:Label { text = text, w = lvgl.PCT(100) }
        local ok = box:Button { w = lvgl.PCT(100), h = 26 }
        ok:Label { text = "Close", align = lvgl.ALIGN.CENTER }
        ok:onevent(lvgl.EVENT.RELEASED, close)
    end)
end

-- ── Installed scan ───────────────────────────────────────────────────────────
-- Tracked apps are those with a .version file. Every firmware app now ships
-- one, and store installs write one, so this maps the catalog to what's on
-- the device — keyed by the marker's catalog id when it has one, else by the
-- folder name (older markers; folder = catalog `name` for those). The id key
-- is what lets two categories each hold e.g. a "Settings" folder.

local function inst_key(entry)
    return entry.id or entry.name
end

-- Installed record for a catalog entry. Id key first; an entry carrying
-- `was` (its pre-id folder name) also joins an old un-id'd install under
-- that key — the category-migration path. NO blanket name fallback: two
-- entries may share a display name across categories.
local function find_inst(entry)
    return store.installed[inst_key(entry)]
        or (entry.was and store.installed[entry.was]) or nil
end

local function scan_installed()
    local installed = {}
    for _, rec in ipairs(apps.all()) do
        local v = dl.read_version(rec.dir)
        if v then
            local clean = rec.raw_name or rec.name
            installed[v.id or clean] = {
                name     = clean,
                id       = v.id,
                version  = v.version,
                location = v.location,
                category = v.category,
                locked   = v.locked,
                dir      = rec.dir,
                display  = rec.name,
            }
        end
    end
    store.installed = installed
end

-- Catalog entries whose installed version differs from the catalog version.
-- The Updates list on the root page; empty = nothing to show. Firmware-gated
-- updates are excluded — nothing actionable to offer; their category rows
-- show "Needs FW" instead.
local function pending_updates()
    local out = {}
    for _, e in ipairs(store.catalog.apps) do
        local inst = find_inst(e)
        if inst and version_newer(e.version, inst.version) and not fw_required(e) then
            out[#out + 1] = { entry = e, inst = inst }
        end
    end
    table.sort(out, function(a, b) return a.entry.name < b.entry.name end)
    return out
end

-- ── Catalog lookups ──────────────────────────────────────────────────────────

local function catalog_protocols()
    return (store.catalog and store.catalog.protocols) or {}
end

local function find_proto(id)
    for _, pe in ipairs(catalog_protocols()) do
        if pe.id == id then return pe end
    end
    return nil
end

local function find_app(id)
    for _, ae in ipairs((store.catalog and store.catalog.apps) or {}) do
        if ae.id == id then return ae end
    end
    return nil
end

-- Display name for a protocol id (catalog name, else the id itself).
local function proto_name(id)
    local pe = find_proto(id)
    return (pe and pe.name) or id
end

-- Installed record of a protocol package: its dir, loc and .version marker
-- version ("?" = present but unmarked, a hand-copied elf), or nil.
local function proto_inst(pe)
    local dir, loc = proto_where(pe.id)
    if not dir then return nil end
    local v = dl.read_version(dir)
    return { dir = dir, loc = loc, version = (v and v.version) or "?" }
end

-- ── Install targets ──────────────────────────────────────────────────────────

local function type_badge(entry)
    return (entry.type == "elf") and "[ELF]" or "[Lua]"
end

-- Final install dir for a catalog entry at a location ("sd"|"internal").
local function target_dir(entry, loc)
    local drive = (loc == "sd") and "S" or "L"
    local dir = drive .. ":" .. apps.paths()[loc]
    if type(entry.category) == "string" and entry.category ~= "" then
        dir = dir .. "/" .. entry.category
    end
    return fileman.normalize(dir .. "/" .. entry.name)
end

-- ── Dialogs ──────────────────────────────────────────────────────────────────

local function show_info()
    -- Fixed-height box + inner scrollable body (Files preview-modal pattern) —
    -- the text is taller than the screen, SIZE_CONTENT would overflow it.
    modal({ w = W - 20, h = H - 20 }, function(box, close)
        box:Label { text = "About app types", w = lvgl.PCT(100), h = 18 }
        local body = box:Object {
            w = lvgl.PCT(100), h = H - 90,
            border_width = 1, pad_all = 4, radius = 0,
        }
        body:Label {
            text = "[Lua] apps run inside the firmware's Lua\n"
                .. "interpreter. They can use the Meshpunk API\n"
                .. "(files, WiFi, radio) but can't run native\n"
                .. "code on the hardware.\n\n"
                .. "[ELF] apps are native binaries loaded by the\n"
                .. "ELF loader. They have full hardware access\n"
                .. "and take over the whole device while running\n"
                .. "(Lua and other apps are shut down).\n\n"
                .. "[Protocol] packages are LoRa radio protocols\n"
                .. "(MeshCore, MTLite). They install to internal\n"
                .. "storage, load at boot, and are chosen in\n"
                .. "Settings > Lora. Installing one offers its\n"
                .. "apps; nothing about them installs silently.\n\n"
                .. "Everything in the store comes from a curated\n"
                .. "GitHub repo, reviewed before listing.",
            w = lvgl.PCT(100),
        }
        local close_btn = box:Button { w = lvgl.PCT(100), h = 26 }
        close_btn:Label { text = "Close", align = lvgl.ALIGN.CENTER }
        close_btn:onevent(lvgl.EVENT.RELEASED, close)
    end)
end

-- Pick SD/Internal, showing free space; cb(loc) on choice. Apps recommend
-- the card; opts.recommend = "internal" flips that (protocols: an internal
-- package loads even when the card is missing at boot) and opts.note adds
-- the reason line under the title.
local function pick_location(cb, opts)
    opts = opts or {}
    local drives = {}
    for _, d in ipairs(fileman.drives()) do drives[d.id] = d end
    modal({}, function(box, close)
        box:Label { text = "Install where?", w = lvgl.PCT(100), h = 18 }
        if opts.note then
            box:Label { text = opts.note, w = lvgl.PCT(100) }
        end

        local function option(label, loc, d)
            local sub
            if not d.mounted then
                sub = "not mounted"
            elseif d.total then
                sub = fileman.size_str((d.total or 0) - (d.used or 0)) .. " free"
            else
                sub = "mounted"
            end
            local b = box:Button { w = lvgl.PCT(100), h = 52 }
            b:Label { text = label, align = lvgl.ALIGN.TOP_LEFT }
            b:Label { text = sub, align = lvgl.ALIGN.BOTTOM_LEFT }
            b:onevent(lvgl.EVENT.RELEASED, function()
                if not d.mounted then
                    toast("SD card not mounted")
                    return
                end
                close()
                cb(loc)
            end)
        end

        local rec = opts.recommend or "sd"
        if rec == "internal" then
            option("Internal (recommended)", "internal", drives.L or { mounted = true })
            option("SD card", "sd", drives.S or { mounted = false })
        else
            option("SD card (recommended)", "sd", drives.S or { mounted = false })
            option("Internal", "internal", drives.L or { mounted = true })
        end

        local cancel_btn = box:Button { w = lvgl.PCT(100), h = 26 }
        cancel_btn:Label { text = "Cancel", align = lvgl.ALIGN.CENTER }
        cancel_btn:onevent(lvgl.EVENT.RELEASED, close)
    end)
end

-- Resolve a registry record for a (possibly SD-suffixed) app name and open it.
local function open_app(name)
    local rec = apps.get(name) or apps.get(name .. " (SD)")
    if rec then
        apps.launch(rec)
    else
        toast("App not found in launcher")
    end
end

-- ── USB driver dependencies ─────────────────────────────────────────────────
-- An app entry may carry drivers = { "<id>", ... } (ids from the catalog's
-- [[drivers]] list). They install AFTER the app, to the same location, one
-- run_install modal each. Already-installed ids are skipped (updates stay
-- explicit in Tools/USB); a failed dep toasts but never rolls back the app;
-- removal never touches drivers (they're shared between apps).
-- Drivers are the ONE dependency kind that installs on its own: they are
-- invisible plumbing for the app. LoRa protocols are deliberately NOT — see
-- the protocol flows below.
local DRV_BASES = { internal = "L:/usb_drivers", sd = "S:/meshpunk/usb_drivers" }

local function driver_installed(id)
    return fileman.exists("L:/usb_drivers/" .. id)
        or fileman.exists("S:/meshpunk/usb_drivers/" .. id)
end

local function install_driver_deps(entry, loc, done)
    local wants = entry.drivers
    local cat_drivers = store.catalog and store.catalog.drivers
    if type(wants) ~= "table" or #wants == 0 then done() return end
    if type(cat_drivers) ~= "table" then done() return end

    local queue = {}
    for _, id in ipairs(wants) do
        if not driver_installed(id) then
            local found = nil
            for _, de in ipairs(cat_drivers) do
                if de.id == id then found = de break end
            end
            if found then queue[#queue + 1] = found
            else toast("Driver '" .. id .. "' not in catalog") end
        end
    end

    local i = 0
    local function next_dep()
        i = i + 1
        local de = queue[i]
        if not de then done() return end
        dl.run_install(root, {
            entry = de, kind = "drivers", loc = loc,
            final_dir = DRV_BASES[loc] .. "/" .. de.id,
            on_done = function(err)
                if err then toast("Driver " .. de.id .. ": " .. tostring(err)) end
                next_dep()
            end,
        })
    end
    next_dep()
end

-- Installed record of a driver (its dir on whichever drive holds it, plus the
-- .version marker run_install wrote there — "?" for a hand-copied driver,
-- which version_newer then treats as updatable, the USB Host manager's rule).
local function driver_inst(de)
    for loc, base in pairs(DRV_BASES) do
        local dir = base .. "/" .. de.id
        if fileman.exists(dir) then
            local v = dl.read_version(dir)
            return { dir = dir, loc = loc, version = (v and v.version) or "?" }
        end
    end
    return nil
end

-- Installed drivers whose catalog version is ahead: they ride the Updates
-- page like apps and protocols (the in-place install the USB Host manager's
-- own "Get drivers" list runs). A driver loads when its device is plugged
-- in, so an update lands at the next plug-in — no reboot.
local function pending_driver_updates()
    local out = {}
    for _, de in ipairs((store.catalog and store.catalog.drivers) or {}) do
        local inst = driver_inst(de)
        if inst and not fw_required(de) and version_newer(de.version, inst.version) then
            out[#out + 1] = { entry = de, inst = inst }
        end
    end
    table.sort(out, function(a, b) return (a.entry.name or a.entry.id) < (b.entry.name or b.entry.id) end)
    return out
end

-- ── Protocol package install ─────────────────────────────────────────────────

-- A protocol package may ship BLE-protocol elfs alongside its LoRa elf (the
-- meshcore package does — docs/PROTOCOL_ABI.md: both slots, versioned
-- together). The BLE slot loads from its own tree, so after a protocol
-- install/update every *.bleproto.elf moves to
-- L:/meshpunk/ble_protos/<filename minus suffix>/ and gets its own .version
-- marker there, stamped with the PACKAGE version it came from — that marker
-- is what lets the Updates page notice a BLE protocol that is missing or
-- behind its package (see pending_bleproto_issues).
local function relocate_bleprotos(pe)
    local dir = proto_where(pe.id)
    if not dir then return end
    local entries = fileman.list(dir, { sizes = false }) or {}
    for _, e in ipairs(entries) do
        if e.type ~= "dir" and e.name:sub(-13) == ".bleproto.elf" then
            local pid = e.name:sub(1, -14)
            local pdir = BLEPROTO_BASE .. "/" .. pid
            fileman.mkdir(pdir)
            -- fileman.move: same-drive rename, or copy + delete from a
            -- card-hosted package (the companion always lives on L:).
            local ok, err = fileman.move(dir .. "/" .. e.name, pdir .. "/" .. e.name)
            if not ok then
                toast("BLE proto move failed: " .. tostring(err))
            elseif not fileman.write(pdir .. "/.version",
                    tostring(pe.version or "?") .. "\ninternal\n\nid=" .. pid) then
                toast("BLE proto marker write failed: " .. pid)
            end
        end
    end
end

-- BLE protocols shipped by a protocol package, as {pid, file} pairs (from the
-- catalog entry's file list — the same names relocate_bleprotos moves).
local function package_bleprotos(pe)
    local out = {}
    for _, f in ipairs(pe.files or {}) do
        if f:sub(-13) == ".bleproto.elf" then
            out[#out + 1] = { pid = f:sub(1, -14), file = f }
        end
    end
    return out
end

-- State of one BLE protocol on the device relative to its package: the
-- installed marker version (nil when the elf is missing, "?" when unmarked).
local function bleproto_state(bp)
    local pdir = BLEPROTO_BASE .. "/" .. bp.pid
    if not fileman.exists(pdir .. "/" .. bp.file) then return nil end
    local v = dl.read_version(pdir)
    return (v and v.version) or "?"
end

-- Install (fresh, at `loc`) or update (in place, wherever it already lives)
-- one protocol package, with its own visible progress modal. done(err): nil
-- on success, "cancelled" on cancel.
local function run_proto_install(pe, loc, done)
    local dir, have_loc = proto_where(pe.id)
    if dir then
        loc = have_loc
    else
        dir = PROTO_BASES[loc] .. "/" .. pe.id
    end
    dl.run_install(root, {
        entry = pe, kind = "protocols", loc = loc,
        final_dir = dir, old_dir = (have_loc and dir) or nil,
        on_done = function(err)
            if not err then relocate_bleprotos(pe) end
            done(err)
        end,
    })
end

-- BLE protocols that are BEHIND the installed package that ships them
-- (present, but marker absent or marker version ~= the package's installed
-- version). An ABSENT companion is not an issue — it is a separate install
-- the user may have removed on purpose (the protocol menu offers the
-- package reinstall that restores it). The fix for "behind" is that same
-- reinstall: it re-relocates the elf and rewrites the marker — so these ride
-- the Updates page as rows that run the package install.
local function pending_bleproto_issues()
    local out = {}
    for _, pe in ipairs(catalog_protocols()) do
        local pinst = (not fw_required(pe)) and proto_inst(pe) or nil
        if pinst then
            local want = pinst.version
            for _, bp in ipairs(package_bleprotos(pe)) do
                local have = bleproto_state(bp)
                if have and have ~= want then
                    out[#out + 1] = { entry = pe, pid = bp.pid, have = have, want = want }
                end
            end
        end
    end
    return out
end

-- Installed protocol packages whose catalog version is ahead: they ride the
-- same Updates flow as apps (in-place apply into their install dir; the
-- running protocol is the boot-loaded copy, so the update lands at reboot).
local function pending_proto_updates()
    local out = {}
    for _, pe in ipairs(catalog_protocols()) do
        local pinst = (not fw_required(pe)) and proto_inst(pe) or nil
        if pinst and version_newer(pe.version, pinst.version) then
            out[#out + 1] = { entry = pe, inst = pinst }
        end
    end
    return out
end

local function proto_updated_prompt()
    -- The running protocol is the boot-loaded copy.
    local okrp, rp = pcall(require, "lib/reboot_prompt")
    if okrp and rp then
        rp.show("Updated LoRa protocol loads at reboot")
    else
        toast("Reboot to load the updated protocol")
    end
end

-- ── App install ──────────────────────────────────────────────────────────────

local function install_done(entry, verb, after)
    return function(err)
        -- Rebuild FIRST, toast LAST: the registry rescan blocks the UI for
        -- seconds, and a toast created before it would have its self-delete
        -- timer and entry anim both come due during the freeze — the delete
        -- wins and the anim then fires on a dead object. Last also puts the
        -- toast above the fresh view instead of behind it.
        pcall(apps.refresh)   -- launcher registry picks up the change
        scan_installed()
        refresh_view()
        if err == "cancelled" then
            toast("Cancelled")
        elseif err then
            toast(err)
        else
            toast(verb .. " " .. entry.name)
            if after then after() end
        end
    end
end

-- The install/update itself, location already decided. `inst` = installed
-- record for an update (nil = fresh install at `loc`). finish(err) runs
-- after the app AND its driver deps (a driver failure toasts, never fails
-- the app).
local function install_app_at(entry, inst, loc, finish)
    if inst then
        -- Update in the location the app already lives in. The DIRECTORY is
        -- the catalog's current category+name: when an entry moved category,
        -- the update lands at the new home and run_install removes the old
        -- one (final_dir ~= old_dir = the migration case).
        dl.run_install(root, {
            entry = entry, kind = "apps", loc = loc,
            final_dir = target_dir(entry, loc), old_dir = inst.dir,
            on_done = function(err)
                if err then finish(err) return end
                install_driver_deps(entry, loc, function() finish(nil) end)
            end,
        })
        return
    end
    local dir = target_dir(entry, loc)
    if apps.is_app(dir) then
        finish("Already installed there")
        return
    end
    if fileman.exists(dir) then
        finish("A folder with that name is in the way")
        return
    end
    dl.run_install(root, {
        entry = entry, kind = "apps", loc = loc,
        final_dir = dir,
        on_done = function(err)
            if err then finish(err) return end
            install_driver_deps(entry, loc, function() finish(nil) end)
        end,
    })
end

-- Install or update one app from its detail menu. `after` (optional) runs
-- once the app is in place — the app-first protocol flow uses it for the
-- activation pointer.
local function do_install(entry, inst, after)
    -- Firmware gate (the menu never offers the action; this catches the rest).
    local need = fw_required(entry)
    if need then
        toast("Needs firmware update (API " .. need .. ")")
        return
    end
    if inst then
        local loc = (fileman.split(inst.dir) == "S") and "sd" or "internal"
        install_app_at(entry, inst, loc, install_done(entry, "Updated", after))
        return
    end
    pick_location(function(loc)
        install_app_at(entry, nil, loc, install_done(entry, "Installed", after))
    end)
end

-- Sequential batch (the protocol-first apps offer): every app in `list`
-- installs or updates at `loc`, one progress modal after another; each
-- result toasts, the registry refreshes ONCE at the end (the rescan blocks
-- for seconds — not once per app). done() runs after the last one.
local function install_app_batch(list, loc, done)
    local i = 0
    local function next_app()
        i = i + 1
        local ae = list[i]
        if not ae then
            pcall(apps.refresh)
            scan_installed()
            refresh_view()
            done()
            return
        end
        local inst = find_inst(ae)
        local at = loc
        if inst then at = (fileman.split(inst.dir) == "S") and "sd" or "internal" end
        install_app_at(ae, inst, at, function(err)
            if err == "cancelled" then toast("Cancelled " .. ae.name)
            elseif err then toast(ae.name .. ": " .. tostring(err))
            else toast((inst and "Updated " or "Installed ") .. ae.name) end
            next_app()
        end)
    end
    next_app()
end

-- ── Protocol flows ───────────────────────────────────────────────────────────

local function activation_hint(pe)
    return "Pick " .. pe.name .. " in Settings > Lora and reboot to run it."
end

-- Protocol-first: right after a protocol package installs, offer its primary
-- apps (catalog `apps` ids) — the ones not yet installed or behind the
-- catalog. Yes = one location pick, then the batch, each app visible. Either
-- answer ends with the activation pointer.
local function offer_proto_apps(pe)
    local todo, names = {}, {}
    for _, id in ipairs(pe.apps or {}) do
        local ae = find_app(id)
        if ae and not fw_required(ae) then
            local inst = find_inst(ae)
            if not inst or version_newer(ae.version, inst.version) then
                todo[#todo + 1] = ae
                names[#names + 1] = ae.name
            end
        end
    end
    if #todo == 0 then
        notice(pe.name .. " installed.\n\n" .. activation_hint(pe))
        return
    end
    confirm("Download the " .. pe.name .. " apps?\n" .. table.concat(names, ", "),
        nil,
        function()
            pick_location(function(loc)
                install_app_batch(todo, loc, function()
                    notice(activation_hint(pe))
                end)
            end)
        end,
        function()
            notice(activation_hint(pe))
        end)
end

-- Remove a protocol PACKAGE (its lora_protos/<id>/ dir only). Its apps and
-- the BLE companion it shipped stay installed — they are separate installs,
-- gated on the protocol, and the user removes them on their own. The
-- protocol's data home (settings, peers, messages on the mesh storage) is
-- untouched, so a reinstall picks up where it left off. Removing the
-- running/selected protocol is allowed and warned: the next boot runs the
-- no-radio floor until another protocol is picked — nothing is re-selected
-- silently.
local function do_remove_proto(pe)
    local okp, active, requested = pcall(_lora_proto)
    if not okp then active, requested = nil, nil end
    local warn = nil
    if pe.id == active or pe.id == requested then
        warn = "This is the " .. ((pe.id == active) and "running" or "selected")
            .. " LoRa protocol: the radio will be OFF after the next reboot "
            .. "until you pick another one in Settings > Lora."
    end
    confirm('Remove the "' .. pe.name .. '" protocol?\n'
            .. "Its apps and BLE companion stay installed; its settings and "
            .. "messages stay on storage.",
        warn,
        function()
            local dir, loc = proto_where(pe.id)
            if not dir then toast("Not installed") refresh_view() return end
            dl.run_remove(root, pe.name, dir, {
                parent_base = PROTO_BASES[loc],
                on_done = function(err)
                    refresh_view()
                    if err then toast(err) else toast("Removed " .. pe.name .. " protocol") end
                end,
            })
        end)
end

-- Remove one BLE companion (its ble_protos/<pid>/ dir only) — the same
-- separate-install rule as apps: the package that shipped it stays, and a
-- package reinstall brings the companion back. Removing the selected BLE
-- protocol is allowed and warned: BLE is off after the next reboot until
-- another is picked in Settings > Ble (the loaded copy keeps running until
-- then; nothing is re-selected silently).
local function do_remove_bleproto(bp)
    local okb, req = pcall(_ble_proto_get)
    if not okb then req = nil end
    local warn = nil
    if req == bp.pid then
        warn = "This is the selected BLE protocol: BLE will be OFF after the "
            .. "next reboot until you pick another one in Settings > Ble."
    end
    confirm('Remove the "' .. bp.pid .. '" BLE protocol?\n'
            .. "Its package stays installed; reinstalling the package brings it back.",
        warn,
        function()
            dl.run_remove(root, bp.pid, BLEPROTO_BASE .. "/" .. bp.pid, {
                parent_base = BLEPROTO_BASE,
                on_done = function(err)
                    refresh_view()
                    if err then toast(err) else toast("Removed BLE " .. bp.pid) end
                end,
            })
        end)
end

-- Detail menu for a protocol package: description, versions, and the one
-- action its state allows. Install -> the apps offer; Update -> the reboot
-- prompt (the running copy is the boot-loaded one). Installed packages also
-- get Remove (package dir only — see do_remove_proto), and each installed
-- BLE companion its own Remove (see do_remove_bleproto).
local function proto_menu(pe)
    local inst = proto_inst(pe)
    local active = active_proto()
    modal({}, function(box, close)
        box:Label { text = pe.name, w = lvgl.PCT(100), h = 18 }
        box:Label {
            text = "[Protocol]  v" .. tostring(pe.version)
                .. (pe.author and ("  by " .. pe.author) or ""),
            w = lvgl.PCT(100), h = 16,
        }
        if pe.description then
            box:Label { text = pe.description, w = lvgl.PCT(100) }
        end
        if inst then
            box:Label {
                text = "Installed: v" .. tostring(inst.version) .. " on "
                    .. ((inst.loc == "sd") and "SD" or "Internal")
                    .. ((active == pe.id) and "  (running now)" or ""),
                w = lvgl.PCT(100), h = 16,
            }
        end

        local function item(text, fn)
            local b = box:Button { w = lvgl.PCT(100), h = 26 }
            b:Label { text = text, align = lvgl.ALIGN.CENTER }
            b:onevent(lvgl.EVENT.RELEASED, function()
                close()
                fn()
            end)
        end

        if inst then
            -- Its BLE companions: the version each marker carries, and a
            -- Remove for each one present (a separate install, like apps).
            for _, bp in ipairs(package_bleprotos(pe)) do
                local have = bleproto_state(bp)
                box:Label {
                    text = "BLE " .. bp.pid .. ": "
                        .. (have and ("v" .. have) or "not installed")
                        .. ((have and have ~= tostring(inst.version)) and "  (behind)" or ""),
                    w = lvgl.PCT(100), h = 16,
                }
                if have then
                    local b = bp
                    item("Remove BLE " .. bp.pid, function() do_remove_bleproto(b) end)
                end
            end
        end

        local need = fw_required(pe)
        if need then
            box:Label {
                text = "Needs a firmware update first\n(needs API " .. need
                    .. ", device has " .. FW_API .. ")",
                text_color = "#ff5555", w = lvgl.PCT(100),
            }
        elseif not inst then
            item("Install", function()
                pick_location(function(loc)
                    run_proto_install(pe, loc, function(err)
                        if err then
                            toast(err == "cancelled" and "Cancelled" or err)
                            refresh_view()
                            return
                        end
                        refresh_view()
                        offer_proto_apps(pe)
                    end)
                end, { recommend = "internal",
                       note = "Internal loads even without the SD card; a card-hosted protocol needs the card at boot (radio off without it)." })
            end)
        elseif version_newer(pe.version, inst.version) then
            item("Update to v" .. tostring(pe.version), function()
                run_proto_install(pe, nil, function(err)
                    if err then
                        toast(err == "cancelled" and "Cancelled" or err)
                        refresh_view()
                        return
                    end
                    refresh_view()
                    proto_updated_prompt()
                end)
            end)
        else
            -- Up to date — unless a BLE protocol it ships is absent (removed)
            -- or behind, in which case a package reinstall restores it (same
            -- download).
            local broken = false
            for _, bp in ipairs(package_bleprotos(pe)) do
                if bleproto_state(bp) ~= tostring(inst.version) then broken = true end
            end
            if broken then
                item("Reinstall package (restores BLE protocol)", function()
                    run_proto_install(pe, nil, function(err)
                        if err then
                            toast(err == "cancelled" and "Cancelled" or err)
                            refresh_view()
                            return
                        end
                        refresh_view()
                        proto_updated_prompt()
                    end)
                end)
            else
                box:Label { text = "Up to date. " .. activation_hint(pe), w = lvgl.PCT(100) }
            end
        end
        if inst then
            item("Remove", function() do_remove_proto(pe) end)
        end

        local cancel_btn = box:Button { w = lvgl.PCT(100), h = 26 }
        cancel_btn:Label { text = "Close", align = lvgl.ALIGN.CENTER }
        cancel_btn:onevent(lvgl.EVENT.RELEASED, close)
    end)
end

-- One row on the LoRa Protocols page (same shape as an app row).
local function proto_row(content, pe)
    local inst = proto_inst(pe)
    local state
    if inst and not version_newer(pe.version, inst.version) then
        state = "Installed"
    elseif fw_required(pe) then
        state = "Needs FW"
    elseif inst then
        state = "Update"
    else
        state = "Install"
    end
    if inst and active_proto() == pe.id then state = state .. " (active)" end

    local row = content:Button { w = lvgl.PCT(100), h = 52 }
    row:Label {
        text = pe.name .. "  v" .. tostring(pe.version) .. "  [Protocol]",
        align = lvgl.ALIGN.TOP_LEFT,
    }
    local desc = pe.description or ""
    if #desc > 30 then desc = desc:sub(1, 29) .. "~" end
    row:Label { text = desc, align = lvgl.ALIGN.BOTTOM_LEFT }
    row:Label { text = state, align = lvgl.ALIGN.RIGHT_MID }
    nav.tap(row, function() proto_menu(pe) end)
end

-- One Updates-page row for a protocol package; tapping applies the update
-- directly (its detail menu offers the same thing).
local function proto_update_row(content, su)
    local pe = su.entry
    local row = content:Button { w = lvgl.PCT(100), h = 52 }
    row:Label {
        text = pe.name .. "  v" .. tostring(su.inst.version)
            .. " > v" .. tostring(pe.version) .. "  [Protocol]",
        align = lvgl.ALIGN.TOP_LEFT,
    }
    row:Label { text = "LoRa protocol package", align = lvgl.ALIGN.BOTTOM_LEFT }
    row:Label { text = "Update", align = lvgl.ALIGN.RIGHT_MID }
    nav.tap(row, function()
        run_proto_install(pe, nil, function(err)
            if err then
                if err ~= "cancelled" then toast("Protocol: " .. tostring(err)) end
                return
            end
            proto_updated_prompt()
            refresh_view()
        end)
    end)
end

-- One Updates-page row for a USB driver; tapping updates it in place where
-- it lives (drivers have no detail menu — Tools > USB Host manages them).
local function driver_update_row(content, du)
    local de, inst = du.entry, du.inst
    local row = content:Button { w = lvgl.PCT(100), h = 52 }
    row:Label {
        text = (de.name or de.id) .. "  v" .. tostring(inst.version)
            .. " > v" .. tostring(de.version) .. "  [Driver]",
        align = lvgl.ALIGN.TOP_LEFT,
    }
    row:Label { text = "USB driver - loads at next plug-in", align = lvgl.ALIGN.BOTTOM_LEFT }
    row:Label { text = "Update", align = lvgl.ALIGN.RIGHT_MID }
    nav.tap(row, function()
        dl.run_install(root, {
            entry = de, kind = "drivers", loc = inst.loc,
            final_dir = inst.dir, old_dir = inst.dir,
            on_done = function(err)
                if err then
                    if err ~= "cancelled" then toast("Driver: " .. tostring(err)) end
                    return
                end
                toast("Updated driver " .. (de.name or de.id))
                refresh_view()
            end,
        })
    end)
end

-- One Updates-page row for a BLE protocol that is behind the package
-- shipping it; tapping reinstalls that package (the elf and its marker come
-- back with it).
local function bleproto_issue_row(content, bi)
    local pe = bi.entry
    local row = content:Button { w = lvgl.PCT(100), h = 52 }
    row:Label {
        text = bi.pid .. "  v" .. tostring(bi.have)
            .. " > v" .. tostring(bi.want) .. "  [BLE protocol]",
        align = lvgl.ALIGN.TOP_LEFT,
    }
    row:Label { text = "Reinstalls the " .. pe.name .. " package", align = lvgl.ALIGN.BOTTOM_LEFT }
    row:Label { text = "Repair", align = lvgl.ALIGN.RIGHT_MID }
    nav.tap(row, function()
        run_proto_install(pe, nil, function(err)
            if err then
                if err ~= "cancelled" then toast("Protocol: " .. tostring(err)) end
                return
            end
            proto_updated_prompt()
            refresh_view()
        end)
    end)
end

local function do_remove(name, dir)
    -- Emptied category folders are cleaned up, but never the apps base itself.
    local p = apps.paths()
    local base = (fileman.split(dir) == "S") and ("S:" .. p.sd) or ("L:" .. p.internal)
    confirm('Remove "' .. name .. '"?', nil, function()
        dl.run_remove(root, name, dir, {
            parent_base = base,
            on_done = function(err)
                pcall(apps.refresh)
                scan_installed()
                refresh_view()
                if err then toast(err) else toast("Removed " .. name) end
            end,
        })
    end)
end

-- Detail modal for a catalog entry. `inst` is its installed record (or nil if
-- not installed). The entry-nil branches are a defensive fallback; every live
-- caller now passes a catalog entry.
local function app_menu(entry, inst)
    local name = entry and entry.name or inst.display
    modal({}, function(box, close)
        box:Label { text = name, w = lvgl.PCT(100), h = 18 }
        if entry then
            box:Label {
                text = type_badge(entry) .. "  v" .. tostring(entry.version)
                    .. (entry.author and ("  by " .. entry.author) or ""),
                w = lvgl.PCT(100), h = 16,
            }
            if entry.description then
                box:Label { text = entry.description, w = lvgl.PCT(100) }
            end
        else
            box:Label {
                text = "v" .. inst.version .. "  (no longer in catalog)",
                w = lvgl.PCT(100), h = 16,
            }
        end
        if inst then
            box:Label {
                text = "Installed: v" .. inst.version .. " on "
                    .. ((fileman.split(inst.dir) == "S") and "SD" or "Internal"),
                w = lvgl.PCT(100), h = 16,
            }
        end

        local function item(text, fn)
            local b = box:Button { w = lvgl.PCT(100), h = 26 }
            b:Label { text = text, align = lvgl.ALIGN.CENTER }
            b:onevent(lvgl.EVENT.RELEASED, function()
                close()
                fn()
            end)
        end

        -- Install/Update action — replaced by an explanation when the entry
        -- needs firmware this device doesn't have, or by the protocol ASK
        -- when its LoRa protocol is missing: the message names what is
        -- missing and the one button downloads the protocol FIRST, then the
        -- app — two visible installs, nothing automatic.
        local need = entry and fw_required(entry)
        local need_proto = entry and proto_required(entry)
        local actionable = entry
            and (not inst or version_newer(entry.version, inst.version))
        if actionable and need then
            box:Label {
                text = "Needs a firmware update first\n(app needs API " .. need
                    .. ", device has " .. FW_API .. ")",
                text_color = "#ff5555", w = lvgl.PCT(100),
            }
        elseif actionable and need_proto then
            local pe = find_proto(need_proto)
            box:Label {
                text = "Needs the " .. proto_name(need_proto)
                    .. " LoRa protocol - not installed.",
                text_color = "#ff5555", w = lvgl.PCT(100),
            }
            if not pe then
                box:Label { text = "(not in this catalog)", w = lvgl.PCT(100) }
            elseif fw_required(pe) then
                box:Label {
                    text = "The protocol needs a firmware update\n(needs API "
                        .. fw_required(pe) .. ", device has " .. FW_API .. ")",
                    text_color = "#ff5555", w = lvgl.PCT(100),
                }
            else
                item("Download protocol, then " .. (inst and "update" or "install"), function()
                    -- The protocol asks for ITS location first (internal
                    -- recommended); the app then asks for its own.
                    pick_location(function(loc)
                        run_proto_install(pe, loc, function(err)
                            if err then
                                toast(err == "cancelled" and "Cancelled" or err)
                                refresh_view()
                                return
                            end
                            toast("Installed " .. pe.name .. " protocol")
                            do_install(entry, inst, function()
                                notice(activation_hint(pe))
                            end)
                        end)
                    end, { recommend = "internal",
                           note = "Where should the " .. pe.name .. " protocol go? Internal loads even without the SD card." })
                end)
            end
        elseif entry and not inst then
            item("Install", function() do_install(entry, nil) end)
        elseif entry and inst and version_newer(entry.version, inst.version) then
            item("Update to v" .. tostring(entry.version),
                 function() do_install(entry, inst) end)
        end
        if inst then
            -- Id-bearing apps key the launcher registry by id; older ones by name.
            item("Open", function()
                open_app((inst and inst.id) or (entry and entry.id) or
                         (entry and entry.name) or inst.name)
            end)
            -- Locked apps (line 4 of .version) hide Remove — e.g. the App
            -- Library itself, so it can't be uninstalled from inside itself.
            if not inst.locked then
                item("Remove", function()
                    do_remove(entry and entry.name or inst.name, inst.dir)
                end)
            end
        end

        local cancel_btn = box:Button { w = lvgl.PCT(100), h = 26 }
        cancel_btn:Label { text = "Close", align = lvgl.ALIGN.CENTER }
        cancel_btn:onevent(lvgl.EVENT.RELEASED, close)
    end)
end

-- ── Views ────────────────────────────────────────────────────────────────────

local function show_loading(msg)
    swap_view(function(v)
        local col = v:Object {
            w = W, h = H, x = 0, y = 0,
            bg_opa = 0, border_width = 0, pad_all = 8,
            flex = { flex_direction = "column", flex_wrap = "nowrap" },
        }
        nav.replace(col, { flags = nav.ROLLOVER })
        col:Label { text = "App Library", w = lvgl.PCT(100), h = 22 }
        col:Label { text = msg, w = lvgl.PCT(100), h = 40 }
        local quit_btn = col:Button { w = lvgl.PCT(100), h = 30 }
        quit_btn:Label { text = "Quit", align = lvgl.ALIGN.CENTER }
        quit_btn:onClicked(function() apps.go_home() end)
    end)
end

local function show_error(msg)
    swap_view(function(v)
        local col = v:Object {
            w = W, h = H, x = 0, y = 0,
            bg_opa = 0, border_width = 0, pad_all = 8,
            flex = { flex_direction = "column", flex_wrap = "nowrap" },
        }
        nav.replace(col, { flags = nav.ROLLOVER })
        col:Label { text = "App Library", w = lvgl.PCT(100), h = 22 }
        col:Label { text = msg, w = lvgl.PCT(100) }
        local retry_btn = col:Button { w = lvgl.PCT(100), h = 30 }
        retry_btn:Label { text = "Retry", align = lvgl.ALIGN.CENTER }
        retry_btn:onClicked(function() start() end)
        local quit_btn = col:Button { w = lvgl.PCT(100), h = 30 }
        quit_btn:Label { text = "Quit", align = lvgl.ALIGN.CENTER }
        quit_btn:onClicked(function() apps.go_home() end)
    end)
end

-- Catalog grouped by category (uncategorized apps land under "Other").
-- Returns groups (name -> sorted entry array) and the sorted category order.
local function group_catalog()
    local groups, order = {}, {}
    for _, e in ipairs(store.catalog.apps) do
        local cat = (type(e.category) == "string" and e.category ~= "")
            and e.category or "Other"
        if not groups[cat] then
            groups[cat] = {}
            order[#order + 1] = cat
        end
        table.insert(groups[cat], e)
    end
    table.sort(order)
    for _, cat in ipairs(order) do
        table.sort(groups[cat], function(a, b) return a.name < b.name end)
    end
    return groups, order
end

-- One catalog row (name/version/badge + description + state), tapping opens
-- the detail menu. Shared by the category and updates pages.
local function catalog_row(content, e)
    local entry = e
    local inst = find_inst(e)
    local state
    if inst and not version_newer(e.version, inst.version) then
        state = "Installed"  -- up to date, or ahead of the catalog
    elseif fw_required(e) then
        state = "Needs FW"   -- installable/updatable, but firmware is too old
    elseif proto_required(e) then
        -- Its LoRa protocol is missing; the detail menu offers the download.
        state = "Needs " .. proto_name(proto_required(e))
    elseif inst then
        state = "Update"
    else
        state = "Install"
    end

    -- h=52 fits two label lines above/below the button's own padding
    -- (h=40 squeezed name and description into each other on hw).
    local row = content:Button { w = lvgl.PCT(100), h = 52 }
    row:Label {
        text = e.name .. "  v" .. tostring(e.version) .. "  " .. type_badge(e),
        align = lvgl.ALIGN.TOP_LEFT,
    }
    local desc = e.description or ""
    if #desc > 30 then desc = desc:sub(1, 29) .. "~" end
    row:Label { text = desc, align = lvgl.ALIGN.BOTTOM_LEFT }
    row:Label { text = state, align = lvgl.ALIGN.RIGHT_MID }
    nav.tap(row, function() app_menu(entry, inst) end)
end

-- The pinned LoRa Protocols page: every protocol package, rendered like an
-- app row (name, version, [Protocol] badge, description, state).
show_protocols = function()
    cur_cat, cur_updates = PROTO_CAT, false
    swap_view(function(v)
        local content = v:Object {
            w = W, h = H, x = 0, y = 0,
            bg_opa = 0, border_width = 0, pad_all = 4,
            flex = { flex_direction = "row", flex_wrap = "wrap" },
        }
        nav.replace(content, { flags = nav.ROLLOVER + nav.SCROLL_FIRST })

        content:Label { text = PROTO_CAT, w = lvgl.PCT(100), h = 18 }

        local back_btn = content:Button { w = lvgl.PCT(100), h = 24 }
        back_btn:Label { text = "Back", align = lvgl.ALIGN.CENTER }
        back_btn:onClicked(function() show_browse() end)

        local list = {}
        for _, pe in ipairs(catalog_protocols()) do list[#list + 1] = pe end
        table.sort(list, function(a, b) return a.name < b.name end)
        for _, pe in ipairs(list) do
            proto_row(content, pe)
        end
        if #list == 0 then
            content:Label { text = "No protocols in this catalog", w = lvgl.PCT(100), h = 24 }
        end
    end)
end

-- One category's app list (launcher-style sub-page with a Back button).
show_category = function(cat)
    if cat == PROTO_CAT then
        show_protocols()
        return
    end
    cur_cat, cur_updates = cat, false
    local groups = group_catalog()
    local entries = groups[cat]
    if not entries then   -- category vanished (e.g. after a Refresh)
        show_browse()
        return
    end

    swap_view(function(v)
        local content = v:Object {
            w = W, h = H, x = 0, y = 0,
            bg_opa = 0, border_width = 0, pad_all = 4,
            flex = { flex_direction = "row", flex_wrap = "wrap" },
        }
        nav.replace(content, { flags = nav.ROLLOVER + nav.SCROLL_FIRST })

        content:Label { text = cat, w = lvgl.PCT(100), h = 18 }

        local back_btn = content:Button { w = lvgl.PCT(100), h = 24 }
        back_btn:Label { text = "Back", align = lvgl.ALIGN.CENTER }
        back_btn:onClicked(function() show_browse() end)

        for _, e in ipairs(entries) do
            catalog_row(content, e)
        end
    end)
end

-- Updates page: every catalog app whose installed version is behind. Reached
-- from the root's "Updates (N)" button; that button only exists when N > 0.
show_updates = function()
    cur_cat, cur_updates = nil, true
    local ups = pending_updates()
    local sups = pending_proto_updates()
    local bups = pending_bleproto_issues()
    local dups = pending_driver_updates()
    if #ups == 0 and #sups == 0 and #bups == 0 and #dups == 0 then   -- last update just applied
        show_browse()
        return
    end

    swap_view(function(v)
        local content = v:Object {
            w = W, h = H, x = 0, y = 0,
            bg_opa = 0, border_width = 0, pad_all = 4,
            flex = { flex_direction = "row", flex_wrap = "wrap" },
        }
        nav.replace(content, { flags = nav.ROLLOVER + nav.SCROLL_FIRST })

        content:Label { text = "Updates", w = lvgl.PCT(100), h = 18 }

        local back_btn = content:Button { w = lvgl.PCT(100), h = 24 }
        back_btn:Label { text = "Back", align = lvgl.ALIGN.CENTER }
        back_btn:onClicked(function() show_browse() end)

        for _, u in ipairs(ups) do
            catalog_row(content, u.entry)
        end
        for _, su in ipairs(sups) do
            proto_update_row(content, su)
        end
        for _, bi in ipairs(bups) do
            bleproto_issue_row(content, bi)
        end
        for _, du in ipairs(dups) do
            driver_update_row(content, du)
        end
    end)
end

-- Category root: an "Updates (N)" button when updates are pending, the pinned
-- LoRa Protocols button (whenever the catalog carries protocols), then one
-- button per app category.
show_browse = function()
    cur_cat, cur_updates = nil, false
    local _, order = group_catalog()
    local ups = pending_updates()
    local n_ups = #ups + #pending_proto_updates() + #pending_bleproto_issues()
                  + #pending_driver_updates()

    swap_view(function(v)
        local content = v:Object {
            w = W, h = H, x = 0, y = 0,
            bg_opa = 0, border_width = 0, pad_all = 4,
            flex = { flex_direction = "row", flex_wrap = "wrap" },
        }
        nav.replace(content, { flags = nav.ROLLOVER + nav.SCROLL_FIRST })

        content:Label {
            text = "App Library" .. (store.offline and "  (offline)" or ""),
            w = lvgl.PCT(100), h = 18,
        }

        local function tool(text, width, fn)
            local b = content:Button { w = width, h = 24 }
            b:Label { text = text, align = lvgl.ALIGN.CENTER }
            b:onClicked(fn)
        end
        tool("Refresh", 70, function() start() end)
        tool("Info", 50, show_info)
        tool("Quit", 50, function() apps.go_home() end)

        if n_ups > 0 then
            local b = content:Button { w = lvgl.PCT(100), h = 32 }
            b:Label { text = "Updates  (" .. n_ups .. ")", align = lvgl.ALIGN.LEFT_MID }
            nav.tap(b, function() show_updates() end)
        end

        if #catalog_protocols() > 0 then
            local b = content:Button { w = lvgl.PCT(100), h = 32 }
            b:Label { text = PROTO_CAT, align = lvgl.ALIGN.LEFT_MID }
            nav.tap(b, function() show_protocols() end)
        end

        for _, cat in ipairs(order) do
            local c = cat
            local b = content:Button { w = lvgl.PCT(100), h = 32 }
            b:Label { text = cat, align = lvgl.ALIGN.LEFT_MID }
            nav.tap(b, function() show_category(c) end)
        end

        if #store.catalog.apps == 0 then
            content:Label { text = "Catalog is empty", w = lvgl.PCT(100), h = 24 }
        end
    end)
end

-- Rebuild whatever page the user is on (after install/update/remove).
refresh_view = function()
    if cur_updates then
        show_updates()
    elseif cur_cat then
        show_category(cur_cat)
    else
        show_browse()
    end
end

-- ── Startup flow ─────────────────────────────────────────────────────────────

local function fetch_and_show()
    show_loading("Fetching catalog...")
    -- One tick so the label actually renders before the synchronous fetch.
    apps.add_timer { period = 50, cb = function(t)
        t:delete()
        local cat, err = dl.fetch_catalog()
        if cat then
            store.catalog = cat
            store.offline = false
            scan_installed()
            show_browse()
            return
        end
        local cached = dl.load_cached_catalog()
        if cached then
            store.catalog = cached
            store.offline = true
            scan_installed()
            toast("Offline — showing cached catalog")
            show_browse()
            return
        end
        show_error("Cannot fetch catalog:\n" .. tostring(err)
            .. "\n\nCheck WiFi in Settings > Wifi.")
    end }
end

start = function()
    show_loading("Connecting to WiFi...")
    dl.wifi_wait(WIFI_WAIT_MS, function(connected)
        if connected then
            fetch_and_show()
            return
        end
        local cached = dl.load_cached_catalog()
        if cached then
            store.catalog = cached
            store.offline = true
            scan_installed()
            toast("No WiFi — showing cached catalog")
            show_browse()
        else
            show_error("WiFi not connected.\n\n"
                .. "Connect in Settings > Wifi, then Retry.")
        end
    end)
end

-- ── Start ────────────────────────────────────────────────────────────────────

dl.cleanup_staging()
start()

return root
