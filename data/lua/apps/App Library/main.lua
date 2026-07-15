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

  Catalog rows show a [Lua]/[ELF] type badge; the Info view explains the
  difference (labeling, not gating, per the store's trust model).

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
    catalog   = nil,     -- parsed catalog.toml ({ meta=, apps={...} })
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
local show_browse, show_category, show_updates, refresh_view, start  -- forward decls

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

local function confirm(title, warn, on_yes)
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
        no:onevent(lvgl.EVENT.RELEASED, close)
    end)
end

-- ── Installed scan ───────────────────────────────────────────────────────────
-- Tracked apps are those with a .version file. Every firmware app now ships
-- one, and store installs write one, so this maps the catalog to what's on
-- the device — keyed by the folder name (which equals the catalog `name`).

local function scan_installed()
    local installed = {}
    for _, rec in ipairs(apps.all()) do
        local v = dl.read_version(rec.dir)
        if v then
            local clean = rec.raw_name or rec.name
            installed[clean] = {
                name     = clean,
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
        local inst = store.installed[e.name]
        if inst and version_newer(e.version, inst.version) and not fw_required(e) then
            out[#out + 1] = { entry = e, inst = inst }
        end
    end
    table.sort(out, function(a, b) return a.entry.name < b.entry.name end)
    return out
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
                .. "Everything in the store comes from a curated\n"
                .. "GitHub repo, reviewed before listing.",
            w = lvgl.PCT(100),
        }
        local close_btn = box:Button { w = lvgl.PCT(100), h = 26 }
        close_btn:Label { text = "Close", align = lvgl.ALIGN.CENTER }
        close_btn:onevent(lvgl.EVENT.RELEASED, close)
    end)
end

-- Pick SD/Internal, showing free space; cb(loc) on choice.
local function pick_location(cb)
    local drives = {}
    for _, d in ipairs(fileman.drives()) do drives[d.id] = d end
    modal({}, function(box, close)
        box:Label { text = "Install where?", w = lvgl.PCT(100), h = 18 }

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

        option("SD card (recommended)", "sd", drives.S or { mounted = false })
        option("Internal", "internal", drives.L or { mounted = true })

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

local function install_done(entry, verb)
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
        end
    end
end

local function do_install(entry, inst)
    -- Firmware gate (the menu never offers the action; this catches the rest).
    local need = fw_required(entry)
    if need then
        toast("Needs firmware update (API " .. need .. ")")
        return
    end
    if inst then
        -- Update in place: same location the app already lives in.
        local loc = (fileman.split(inst.dir) == "S") and "sd" or "internal"
        dl.run_install(root, {
            entry = entry, kind = "apps", loc = loc,
            final_dir = inst.dir, old_dir = inst.dir,
            on_done = function(err)
                if err then install_done(entry, "Updated")(err) return end
                install_driver_deps(entry, loc, function()
                    install_done(entry, "Updated")(nil)
                end)
            end,
        })
        return
    end

    pick_location(function(loc)
        local dir = target_dir(entry, loc)
        if apps.is_app(dir) then
            toast("Already installed there")
            return
        end
        if fileman.exists(dir) then
            toast("A folder with that name is in the way")
            return
        end
        dl.run_install(root, {
            entry = entry, kind = "apps", loc = loc,
            final_dir = dir,
            on_done = function(err)
                if err then install_done(entry, "Installed")(err) return end
                install_driver_deps(entry, loc, function()
                    install_done(entry, "Installed")(nil)
                end)
            end,
        })
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
        -- needs firmware this device doesn't have yet.
        local need = entry and fw_required(entry)
        local actionable = entry
            and (not inst or version_newer(entry.version, inst.version))
        if actionable and need then
            box:Label {
                text = "Needs a firmware update first\n(app needs API " .. need
                    .. ", device has " .. FW_API .. ")",
                text_color = "#ff5555", w = lvgl.PCT(100),
            }
        elseif entry and not inst then
            item("Install", function() do_install(entry, nil) end)
        elseif entry and inst and version_newer(entry.version, inst.version) then
            item("Update to v" .. tostring(entry.version),
                 function() do_install(entry, inst) end)
        end
        if inst then
            item("Open", function() open_app(entry and entry.name or inst.name) end)
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
    local inst = store.installed[e.name]
    local state
    if inst and not version_newer(e.version, inst.version) then
        state = "Installed"  -- up to date, or ahead of the catalog
    elseif fw_required(e) then
        state = "Needs FW"   -- installable/updatable, but firmware is too old
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

-- One category's app list (launcher-style sub-page with a Back button).
show_category = function(cat)
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
    if #ups == 0 then   -- last update just applied — nothing left to show
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
    end)
end

-- Category root: an "Updates (N)" button when updates are pending, then one
-- button per category.
show_browse = function()
    cur_cat, cur_updates = nil, false
    local _, order = group_catalog()
    local ups = pending_updates()

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

        if #ups > 0 then
            local b = content:Button { w = lvgl.PCT(100), h = 32 }
            b:Label { text = "Updates  (" .. #ups .. ")", align = lvgl.ALIGN.LEFT_MID }
            nav.tap(b, function() show_updates() end)
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
            .. "\n\nCheck WiFi in Settings > Wireless.")
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
                .. "Connect in Settings > Wireless, then Retry.")
        end
    end)
end

-- ── Start ────────────────────────────────────────────────────────────────────

dl.cleanup_staging()
start()

return root
