--[[
  UI Theme picker + theme downloads.

  Picker: lists the themes lib/theme discovered (internal /lua/themes + SD
  /meshpunk/themes) and applies the chosen one live — chrome recolors
  instantly, the matching background draws behind this page as a preview.
  The choice persists across reboots.

  Get themes: downloads folder themes from the meshpunk-apps repo's "themes"
  catalog section via lib/downloader (same engine, staging discipline and
  .version bookkeeping as the App Library). Installed themes appear in the
  picker immediately — lib/theme rescans on every list(), no cache to bust.
  Themes install to /meshpunk/themes/<id> (SD) when a card is mounted, else
  /lua/themes/<id> (internal).
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

-- Where themes live (mirrors lib/theme's discovery locations).
local THEME_BASES = { sd = "S:/meshpunk/themes", internal = "L:/lua/themes" }
local WIFI_WAIT_MS = 15000

local root = apps.new_root()
root:set { w = W, h = H, pad_all = 0, border_width = 0, bg_opa = 0 }
root:clear_flag(lvgl.FLAG.SCROLLABLE)

-- Show the current theme's wallpaper behind the picker (transparent containers
-- below), so picking a theme previews its background too, not just the chrome.
-- This app is lightweight, so the background's PSRAM is fine here.
theme.show_background()

local vw = nil
local catalog = nil      -- fetched on first Get-themes open; Refresh refetches
local offline = false
local load_err = nil
local view_gen = 0       -- bumped on every view build; stale async cbs no-op
local build_picker, build_downloads   -- forward declarations

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
local function modal(build)
    local overlay = root:Object {
        w = W, h = H, x = 0, y = 0,
        bg_color = "#000000", bg_opa = 140, border_width = 0, pad_all = 0,
        radius = 0,
    }
    overlay:clear_flag(lvgl.FLAG.SCROLLABLE)
    overlay:add_flag(lvgl.FLAG.CLICKABLE)

    local box = overlay:Object {
        w = W - 70, h = lvgl.SIZE_CONTENT, align = lvgl.ALIGN.CENTER,
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

local function confirm(title, on_yes)
    modal(function(box, close)
        box:Label { text = title, w = lvgl.PCT(100) }
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

-- ── Installed store themes ───────────────────────────────────────────────────
-- Folder name = catalog id. Only dirs with a .version file are store-managed;
-- built-in and hand-copied themes have none and are left alone.
local function scan_installed()
    local installed = {}
    for _, base in pairs(THEME_BASES) do
        local entries = fileman.list(base, { sizes = false })
        if entries then
            for _, en in ipairs(entries) do
                if en.type == "dir" then
                    local dir = base .. "/" .. en.name
                    local v = dl.read_version(dir)
                    if v then
                        installed[en.name] = { version = v.version, dir = dir }
                    end
                end
            end
        end
    end
    return installed
end

-- ── Theme downloads view ─────────────────────────────────────────────────────

local function theme_menu(entry, inst)
    -- entry = catalog entry (nil for an orphaned install), inst = installed rec.
    local id = entry and entry.id or inst.id
    local name = entry and entry.name or inst.id
    modal(function(box, close)
        box:Label { text = name, w = lvgl.PCT(100), h = 18 }
        if entry then
            box:Label {
                text = "v" .. entry.version
                    .. (entry.author and ("  by " .. entry.author) or ""),
                w = lvgl.PCT(100), h = 16,
            }
            if entry.description then
                box:Label { text = entry.description, w = lvgl.PCT(100) }
            end
        else
            box:Label { text = "v" .. inst.version .. "  (no longer in catalog)",
                        w = lvgl.PCT(100), h = 16 }
        end

        local function item(text, fn)
            local b = box:Button { w = lvgl.PCT(100), h = 26 }
            b:Label { text = text, align = lvgl.ALIGN.CENTER }
            b:onevent(lvgl.EVENT.RELEASED, function()
                close()
                fn()
            end)
        end

        local function done(verb)
            return function(err)
                -- Rebuild first, toast last (see App Library install_done):
                -- keeps the toast's timers out of the rebuild freeze and
                -- draws it above the fresh view.
                build_downloads()
                if err == "cancelled" then
                    toast("Cancelled")
                elseif err then
                    toast(err)
                else
                    toast(verb .. " " .. name)
                end
            end
        end

        if entry and not inst then
            item("Install", function()
                -- SD preferred (wallpaper-heavy themes belong there); internal
                -- keeps the feature alive on card-less devices.
                local ok, info = pcall(_storage_get_info)
                local loc = (ok and info and info.sd_available) and "sd" or "internal"
                local dir = THEME_BASES[loc] .. "/" .. entry.id
                if fileman.exists(dir) then
                    toast("A theme with that id already exists")
                    return
                end
                dl.run_install(root, {
                    entry = entry, kind = "themes", loc = loc,
                    final_dir = dir,
                    on_done = done("Installed"),
                })
            end)
        elseif entry and inst and entry.version ~= inst.version then
            item("Update to v" .. entry.version, function()
                local loc = (fileman.split(inst.dir) == "S") and "sd" or "internal"
                dl.run_install(root, {
                    entry = entry, kind = "themes", loc = loc,
                    final_dir = inst.dir, old_dir = inst.dir,
                    on_done = done("Updated"),
                })
            end)
        end
        if inst then
            item("Apply", function()
                theme.apply(id)
                toast("Applied " .. name)
            end)
            -- The default theme is the fallback anchor (theme.apply falls back
            -- to it on any failure) — updatable, never removable.
            if id ~= "default" then
                item("Remove", function()
                    confirm('Remove "' .. name .. '"?', function()
                        -- Removing the active theme would leave lib/theme's cached
                        -- record pointing at a deleted dir — fall back first.
                        if theme.current() == id then theme.apply("default") end
                        dl.run_remove(root, name, inst.dir, {
                            on_done = done("Removed"),
                        })
                    end)
                end)
            end
        end

        local cancel_btn = box:Button { w = lvgl.PCT(100), h = 26 }
        cancel_btn:Label { text = "Close", align = lvgl.ALIGN.CENTER }
        cancel_btn:onevent(lvgl.EVENT.RELEASED, close)
    end)
end

build_downloads = function()
    view_gen = view_gen + 1
    local gen = view_gen
    swap_view(function(v)
        local content = v:Object {
            flex = { flex_direction = "row", flex_wrap = "wrap" },
            w = W, h = H,
            border_width = 0, pad_all = 6, bg_opa = 0,
        }
        nav.replace(content, { flags = nav.ROLLOVER + nav.SCROLL_FIRST })

        content:Label {
            text = "Theme Library" .. (offline and "  (offline)" or ""),
            w = 178, h = 22,
        }

        local refresh_btn = content:Button { w = 70, h = 22 }
        refresh_btn:Label { text = "Refresh", align = lvgl.ALIGN.CENTER }
        refresh_btn:onClicked(function()
            catalog, load_err = nil, nil
            build_downloads()
        end)

        local back_btn = content:Button { w = 50, h = 22 }
        back_btn:Label { text = "Back", align = lvgl.ALIGN.CENTER }
        back_btn:onClicked(function() build_picker() end)

        if not catalog then
            if load_err then
                content:Label { text = load_err, w = lvgl.PCT(100) }
                return
            end
            content:Label { text = "Loading catalog...", w = lvgl.PCT(100), h = 20 }
            dl.wifi_wait(WIFI_WAIT_MS, function(connected)
                if gen ~= view_gen then return end   -- user navigated away
                if connected then
                    local cat = dl.fetch_catalog()
                    if cat then
                        catalog, offline = cat, false
                        build_downloads()
                        return
                    end
                end
                local cached = dl.load_cached_catalog()
                if cached then
                    catalog, offline = cached, true
                    toast("Offline — showing cached catalog")
                    build_downloads()
                    return
                end
                load_err = "Cannot fetch catalog.\n\n"
                    .. "Check WiFi in Settings > Wireless, then Refresh."
                build_downloads()
            end)
            return
        end

        local installed = scan_installed()
        local themes = catalog.themes or {}
        local in_catalog = {}

        table.sort(themes, function(a, b) return a.name < b.name end)
        for _, e in ipairs(themes) do
            in_catalog[e.id] = true
            local entry = e
            local inst = installed[e.id]
            local state
            if not inst then
                state = "Install"
            elseif e.version ~= inst.version then
                state = "Update"
            else
                state = "Installed"
            end

            local row = content:Button { w = lvgl.PCT(100), h = 52 }
            row:Label {
                text = e.name .. "  v" .. e.version,
                align = lvgl.ALIGN.TOP_LEFT,
            }
            local desc = e.description or ""
            if #desc > 30 then desc = desc:sub(1, 29) .. "~" end
            row:Label { text = desc, align = lvgl.ALIGN.BOTTOM_LEFT }
            row:Label { text = state, align = lvgl.ALIGN.RIGHT_MID }
            nav.tap(row, function() theme_menu(entry, inst) end)
        end

        if #themes == 0 then
            content:Label { text = "No themes in the catalog yet", w = lvgl.PCT(100), h = 20 }
        end

        -- Installed store themes that fell out of the catalog — still removable.
        local orphans = {}
        for id, inst in pairs(installed) do
            if not in_catalog[id] then
                inst.id = id
                orphans[#orphans + 1] = inst
            end
        end
        table.sort(orphans, function(a, b) return a.id < b.id end)
        if #orphans > 0 then
            content:Label { text = "Installed (not in catalog):", w = lvgl.PCT(100), h = 16 }
            for _, inst in ipairs(orphans) do
                local o = inst
                local row = content:Button { w = lvgl.PCT(100), h = 28 }
                row:Label { text = o.id .. "  v" .. o.version, align = lvgl.ALIGN.LEFT_MID }
                nav.tap(row, function() theme_menu(nil, o) end)
            end
        end
    end)
end

-- ── Picker view ──────────────────────────────────────────────────────────────

build_picker = function()
    view_gen = view_gen + 1   -- cancels any pending downloads-view fetch cb
    swap_view(function(v)
        local content = v:Object {
            flex = { flex_direction = "row", flex_wrap = "wrap" },
            w = W, h = H,
            border_width = 0, pad_all = 6, bg_opa = 0,
        }
        nav.replace(content, { flags = nav.ROLLOVER + nav.SCROLL_FIRST })

        local status   -- forward-declared; the status row is created below

        content:Label { text = "UI Theme", w = 86, h = 26 }

        -- Quick access to the global selection-highlight style (also in Settings >
        -- Device): a translucent "highlighted fill" ([ ]) vs an opaque solid block
        -- ([x]). Applies live, so it previews on the focused items right here.
        local sel_solid_on = _theme_focus_solid_get()
        local function sel_text() return (sel_solid_on and "[x]" or "[ ]") .. " Solid" end
        local sel_btn = content:Button { w = 96, h = 22 }
        local sel_lbl = sel_btn:Label { text = sel_text(), align = lvgl.ALIGN.CENTER }
        sel_btn:onClicked(function()
            sel_solid_on = not sel_solid_on
            _theme_focus_solid_set(sel_solid_on)
            sel_lbl:set { text = sel_text() }
            status.text = sel_solid_on and "Selection: solid" or "Selection: highlighted fill"
        end)

        local get_btn = content:Button { w = 46, h = 22 }
        get_btn:Label { text = "Get", align = lvgl.ALIGN.CENTER }
        get_btn:onClicked(function() build_downloads() end)

        local back_btn = content:Button { w = 56, h = 22 }
        back_btn:Label { text = "Home", align = lvgl.ALIGN.CENTER }
        back_btn:onClicked(function() apps.go_home() end)

        status = content:Label { text = "Tap a theme to apply", w = lvgl.PCT(100), h = 16 }

        local current_id = theme.current()
        local rows = {}   -- { { id, name, lbl }, ... } to refresh the selection mark

        local function mark(id) return (id == current_id) and "* " or "   " end

        local list = theme.list()
        if #list == 0 then
            content:Label { text = "No themes found in /lua/themes.", w = lvgl.PCT(100), h = 20 }
        end

        for _, item in ipairs(list) do
            local disp = item.name .. (item.source == "sd" and "  (SD)" or "")
            local btn = content:Button { w = lvgl.PCT(100), h = 34 }
            local lbl = btn:Label { text = mark(item.id) .. disp, align = lvgl.ALIGN.CENTER }
            rows[#rows + 1] = { id = item.id, name = disp, lbl = lbl }

            btn:onClicked(function()
                theme.apply(item.id)              -- live chrome re-theme; persists the id
                current_id = theme.current()
                for _, r in ipairs(rows) do
                    r.lbl:set { text = mark(r.id) .. r.name }
                end
                status.text = "Applied: " .. item.name
            end)
        end
    end)
end

-- ── Start ────────────────────────────────────────────────────────────────────

dl.cleanup_staging()
build_picker()
