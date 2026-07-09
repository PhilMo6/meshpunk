--[[
  Grid-based, multi-page launcher for MeshPunks.

  One page is shown at a time (a single `body`). The root page lists top-level
  apps; clicking a category swaps the body in place to that category's page
  instead of relaunching anything. Opening an app hands off to apps.launch(),
  which owns loading + tearing down this page.

  Page swaps are deferred one timer tick: deleting the focused gridnav body
  synchronously inside its own button event is a use-after-free that has crashed
  on hardware (see the Map app's replay-overlay teardown). We drop gridnav now
  and rebuild on the next tick once the event chain has unwound.

  Discovery + launching + state all live in lib/apps (the app manager). The
  launcher only renders pages from the cached registry.
]]

local lvgl = require("lvgl")
local topbar = require("lib/topbar")
local apps = require("lib/apps")
local nav = require("lib/nav")
local theme = require("lib/theme")

-- Module-level so build_page() can delete the previous page on a swap.
local body = nil
local build_page  -- forward declaration (button handlers reference it)

-- Defer a page swap out of the current event handler. Detach gridnav now (so no
-- input reaches the dying page), then run the rebuild on the next tick.
local function request_swap(rebuild)
    nav.reset()
    lvgl.Timer({
        period = 1,
        cb = function(t)
            t:delete()
            rebuild()
        end,
    })
end

-- Tap-vs-swipe button activation (so a drag to scroll the page doesn't launch
-- an app) lives in lib/nav, shared with the messenger and any other app.
local bind_tap = nav.tap

-- Build a page. category = nil for the root page, or a category name for a
-- sub-page (adds a title + Back button).
function build_page(items, category)
    -- Any previous page's background-row labels are about to die with it.
    apps.set_background_listener(nil)
    if body then
        pcall(function() body:delete() end)
        body = nil
    end

    body = lvgl.Object({
        flex = {
            flex_direction = "row",
            flex_wrap = "wrap",
            justify_content = "center",
            align_items = "center",
            -- Stack wrapped rows from the top (not centered) so an overflowing
            -- page scrolls cleanly downward instead of clipping its first rows.
            align_content = "flex-start",
        },
        -- Fit the visible area below the 20px topbar; the wrapped app grid then
        -- scrolls vertically (Objects keep their default SCROLLABLE flag) so a
        -- category with many apps isn't cut off at the bottom of the screen.
        w = lvgl.HOR_RES(), h = lvgl.VER_RES() - 20, x = 0, y = 20,
        -- Transparent so the themed background (lib/background) shows on the home
        -- screen; the plain Object would otherwise get the opaque card style.
        border_width = 0, pad_all = 4, bg_opa = 0,
    })
    nav.replace(body)
    topbar.raise()
    -- Redraw the wallpaper if an app freed it (no-op when already present, e.g.
    -- category page swaps that never left the launcher).
    theme.ensure_background()
    apps.clear_current()
    apps.set_root(body)   -- the manager tears this page down when an app launches

    if category then
        body:Label{text = category, align = lvgl.ALIGN.CENTER, w = 260, h = 40}
    else
        body:Label{text = "Home", align = lvgl.ALIGN.CENTER, w = 260, h = 40}
        -- Backgrounded apps: one row each — tap the wide button to reopen the
        -- app, X to close it for real (runs the contract's on_close; the app
        -- itself never has to be launched for that).
        local bg_rows = {}
        for _, rec in ipairs(apps.background_list()) do
            local key, app_name = rec.key, rec.app_name
            local label = key
            if rec.status then
                local ok, s = pcall(rec.status)
                if ok and s then label = tostring(s) end
            end
            local open_btn = body:Button{w = 106, h = 40}
            local open_lbl = open_btn:Label{text = label, align = lvgl.ALIGN.CENTER}
            bind_tap(open_btn, function()
                if app_name then apps.launch(app_name) end
            end)
            local close_btn = body:Button{w = 30, h = 40}
            close_btn:Label{text = "X", align = lvgl.ALIGN.CENTER}
            bind_tap(close_btn, function()
                apps.close_background(key)
                request_swap(function() build_page(apps.list(), nil) end)
            end)
            bg_rows[#bg_rows + 1] = { rec = rec, lbl = open_lbl }
        end
        if #bg_rows > 0 then
            -- Event-driven label refresh: the app manager notifies when a
            -- record's status() output changes (checked after its background
            -- tick — i.e. exactly when a track auto-advances). No polling.
            apps.set_background_listener(function(rec, s)
                for _, row in ipairs(bg_rows) do
                    if row.rec == rec then
                        pcall(function() row.lbl.text = tostring(s) end)
                        return
                    end
                end
            end)
        end
    end

    for _, app in ipairs(items) do
        local btn = body:Button{w = 140, h = 40}
        btn:Label{text = app.name, align = lvgl.ALIGN.CENTER}

        if app.is_category then
            local folder = app.raw_name or app.name
            local cat_name = app.name
            bind_tap(btn, function()
                request_swap(function() build_page(apps.list(folder), cat_name) end)
            end)
        else
            bind_tap(btn, function()
                apps.launch(app)
            end)
        end
    end

    if #items == 0 then
        local msg = category
            and ("No " .. category:lower() .. " found!")
            or "No apps found!"
        body:Label{text = msg, align = lvgl.ALIGN.CENTER, w = 200, h = 40}
    end

    if category then
        local back_btn = body:Button{w = 140, h = 40}
        back_btn:Label{text = "Back", align = lvgl.ALIGN.CENTER}
        bind_tap(back_btn, function()
            request_swap(function() build_page(apps.list(), nil) end)
        end)
    end
end

local function create_launcher()
    -- Called at boot and when an app exits. The app manager has already torn
    -- down the previous screen, so `body` here is stale (pcall-guarded above).
    build_page(apps.list(), nil)
end

return {
    create = create_launcher,
}
