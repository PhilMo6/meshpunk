-- Read Me — on-device user guide. A viewer over lib/helpdocs: system pages
-- from lua/help on both drives plus a readme.lua inside any installed app,
-- merged with the inline PAGES below into a Guide tab and an Apps tab.
-- Contents page -> one scrollable page per topic, with < / > page flipping.
local lvgl     = require("lvgl")
local apps     = require("lib/apps")
local nav      = require("lib/nav")
local theme    = require("lib/theme")
local helpdocs = require("lib/helpdocs")

local root = apps.new_root()
root:set { w = lvgl.HOR_RES(), h = lvgl.VER_RES(), pad_all = 0, border_width = 0, bg_opa = 0 }
root:clear_flag(lvgl.FLAG.SCROLLABLE)

theme.show_background()

local W = lvgl.HOR_RES()
local H = lvgl.VER_RES()

-- ── Guide content ───────────────────────────────────────────────────────────
-- Plain ASCII on purpose (every theme font renders it). Long strings keep the
-- text editable without escape noise.
local PAGES = {

{ t = "Games and emulators", b = [[
Lua games included: Flappy Bird, Snake, and Scorched Earth.

Emulators install from the App Library; you provide the game files on the SD card. Each emulator reads its own folder - /gb for GameBoy, /snes for SNES, and so on - or its app folder. Every installed emulator has its own page in the Apps tab of this guide with its file types, folders, settings and default keys, and that page installs along with it.

Quitting a native game, any of these:
- Hold Alt + Backspace for about 1.5 seconds (needs a keyboard).
- Hold the on-screen QUIT button for about a second.
- A key you bound to quit in the launcher's Controls screen.

Each game launcher's ? button shows this plus the game's controls.]] },

{ t = "Audio", b = [[
Where sound comes out, and how to route it.

USB audio: route all device audio to a USB audio adapter via Tools > USB Host. Music, app sounds and game audio all follow the route. The adapter needs external power - see the USB accessories page.

Devices with a buzzer instead of a speaker play notification melodies and app tones on the buzzer, one note at a time. For music and game audio on those, a powered USB audio adapter is the way.

Music playback is the Music app (App Library) - its page in the Apps tab covers the library, playlists and background playback.]] },

{ t = "Files and storage", b = [[
Tools > Files is the file manager, covering both internal flash and the SD card.

An SD card is highly recommended: it persists your mesh and firmware settings, and holds game files, music, cached map tiles, and the extended emoji set.]] },

}

-- ── Page set ────────────────────────────────────────────────────────────────
-- This app's own pages, plus everything lib/helpdocs discovers: system pages
-- under lua/help on either drive, and a readme.lua inside any installed app.
-- Discovery runs nothing for an app page (its title is the app's name) and
-- executes a system page once, in a sandbox, to read its title and order.
-- Orders start at 110 so these sit after the migrated guide pages (10-80) and
-- before About (900). Each is about an app that will carry its own readme.lua,
-- at which point its entry here goes away -- an app must not be described in
-- both places or it appears twice.
local ENTRIES = {}
for i, pg in ipairs(PAGES) do
    ENTRIES[#ENTRIES + 1] = { title = pg.t, section = "Guide", order = 100 + i * 10, body = pg.b }
end
do
    local ok, found = pcall(helpdocs.discover)
    if ok and type(found) == "table" then
        for _, e in ipairs(found) do ENTRIES[#ENTRIES + 1] = e end
    end
end
helpdocs.sort(ENTRIES)

-- Fallback so an install with no pages at all still says something useful
-- rather than presenting an empty list. Runs BEFORE the tab split so the
-- fallback page lands in a tab like any other entry.
if #ENTRIES == 0 then
    ENTRIES[1] = { title = "No help installed", section = "Guide", order = 1, body = [[
No help pages were found on this device.

Guide pages live in lua/help on internal storage or the SD card, and an app can ship its own page as readme.lua inside its folder. Installing an app from the App Library brings its help with it.]] }
end

-- Split into tabs: the guide reads as a narrative, app help is a directory of
-- whatever is installed. Both keep the order helpdocs.sort established.
local TABS   = { "Guide", "Apps" }
local BY_TAB = { Guide = {}, Apps = {} }
for _, e in ipairs(ENTRIES) do
    local t = BY_TAB[e.section] and e.section or "Guide"
    BY_TAB[t][#BY_TAB[t] + 1] = e
end

-- ── Views ───────────────────────────────────────────────────────────────────
-- App Library's swap_view pattern: build the new full-screen view, then
-- delete the old one. Each view's content container is the nav scope
-- (focusables are its DIRECT children); SCROLL_FIRST lets the trackball
-- scroll long pages before moving focus.
local vw
local show_contents, show_page

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

local function new_content(v)
    local content = v:Object {
        w = W, h = H, x = 0, y = 0,
        bg_opa = 0, border_width = 0, pad_all = 4,
        flex = { flex_direction = "row", flex_wrap = "wrap" },
    }
    nav.replace(content, { flags = nav.ROLLOVER + nav.SCROLL_FIRST })
    return content
end

local function tool(content, txt, width, fn)
    local b = content:Button { w = width, h = 24 }
    b:Label { text = txt, align = lvgl.ALIGN.CENTER }
    b:onClicked(fn)
end

-- The tab the reader is in. Paging stays inside it, and Back returns to it.
local cur_tab = TABS[1]

show_page = function(tab, i)
    local list = BY_TAB[tab]
    local pg   = list[i]
    -- Resolved here, not at discovery: an app page's file runs the first time
    -- its page is opened, then the text is cached on the entry.
    local text = helpdocs.body(pg)
    swap_view(function(v)
        local content = new_content(v)
        tool(content, "Back", lvgl.PCT(31), function() show_contents(tab) end)
        tool(content, "< Prev", lvgl.PCT(31), function()
            show_page(tab, i > 1 and i - 1 or #list)
        end)
        tool(content, "Next >", lvgl.PCT(31), function()
            show_page(tab, i < #list and i + 1 or 1)
        end)
        -- Title on its own themed card (readable over wallpaper). Clear just
        -- CLICKABLE so gridnav skips it (nav pitfall: default Objects are
        -- focusable).
        local trow = content:Object { w = lvgl.PCT(100), h = 26, pad_all = 4 }
        trow:clear_flag(lvgl.FLAG.SCROLLABLE)
        trow:clear_flag(lvgl.FLAG.CLICKABLE)
        trow:Label { text = i .. "/" .. #list .. "  " .. pg.title, w = lvgl.PCT(100) }
        -- Body text lives in its own scrollable, focusable wrapper. Gridnav's
        -- SCROLL_FIRST scrolls the FOCUSED CHILD (never the nav container),
        -- so trackball-scrollable text must itself be a focusable scrollable
        -- direct child: roll down from the buttons to focus it, then up/down
        -- scroll the text a quarter-screen per tick; at either end focus
        -- moves back out to the buttons. Keep the default SCROLLABLE +
        -- CLICKABLE flags — they are what make this work. Fixed height =
        -- the viewport left below the button + title rows.
        -- No bg_opa/border overrides: the theme's default CARD style is the
        -- readable background over wallpaper (and tracks every theme).
        local body = content:Object {
            w = lvgl.PCT(100), h = H - 86,
            pad_all = 6,
        }
        body:Label { text = text, w = lvgl.PCT(100) }
    end)
end

show_contents = function(tab)
    cur_tab = tab or cur_tab
    local list = BY_TAB[cur_tab]
    swap_view(function(v)
        local content = new_content(v)
        -- Tab row: Home, then one button per tab. The active tab is marked in
        -- its label rather than by styling, so it reads on every theme.
        tool(content, "Home", lvgl.PCT(31), function() apps.go_home() end)
        for _, name in ipairs(TABS) do
            local n = #BY_TAB[name]
            local mark = (name == cur_tab) and "* " or ""
            tool(content, mark .. name .. " (" .. n .. ")", lvgl.PCT(31), function()
                show_contents(name)
            end)
        end
        if #list == 0 then
            -- An empty Apps tab is the normal state until an app ships help,
            -- so say why rather than showing a blank page.
            local erow = content:Object { w = lvgl.PCT(100), h = 60, pad_all = 6 }
            erow:clear_flag(lvgl.FLAG.SCROLLABLE)
            erow:clear_flag(lvgl.FLAG.CLICKABLE)
            erow:Label { text = "No app help installed yet. An app can ship its own page, and installing it brings the page with it.",
                         w = lvgl.PCT(100) }
            return
        end
        for i, pg in ipairs(list) do
            local b = content:Button { w = lvgl.PCT(100), h = 26 }
            b:Label { text = i .. ". " .. pg.title, align = lvgl.ALIGN.LEFT_MID }
            b:onClicked(function() show_page(cur_tab, i) end)
        end
    end)
end

show_contents(TABS[1])

return root
