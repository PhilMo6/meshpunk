-- Read Me — on-device user guide. Content mirrors the project README's
-- device-usage sections (keep the two in sync when either changes).
-- Contents page -> one scrollable page per topic, with < / > page flipping.
local lvgl  = require("lvgl")
local apps  = require("lib/apps")
local nav   = require("lib/nav")
local theme = require("lib/theme")

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

{ t = "Welcome", b = [[
MeshPunk turns the LilyGo T-Deck into a LoRa mesh communicator with full MeshCore support - plus offline maps, music, games and emulators, themes, a file manager, and an App Library for installing more over WiFi.

Highlights:
- Sound, SD card, BLE (phone apps), WiFi
- GPS sets the clock automatically
- Full emoji support, with a downloadable extended set
- Background apps - music keeps playing while you do other things
- USB host support (experimental)

This guide covers day-to-day use. Flip pages with the < and > buttons above.]] },

{ t = "First-time setup", b = [[
After flashing:

1. Use an SD card - it persists your mesh and firmware settings (highly recommended).
2. Open Settings > Radio and set the radio to your local defaults.
3. Set your extra settings: RX boost, Contact Overwrite, and Message Repeat.
4. Get meshing!
5. Install apps from the App Library.

Game and music files live on the SD card - see the Games and Music pages for where each kind goes.]] },

{ t = "Navigation", b = [[
Three ways to move around the UI:

- Trackball: roll to move focus between elements, click to select.
- WASD keys: W/A/S/D mirror the trackball directions (up/left/down/right). When a text input is focused they type normally instead.
- Touchscreen: tap to interact directly.

Trackball and WASD share a configurable sensitivity setting (Settings > Device > Trackball): the minimum time between accepted direction inputs, 0-500 ms.]] },

{ t = "Keyboard shortcuts", b = [[
- Mic key: global notifications shortcut. Over a running app it peeks the top bar; on the launcher (or while peeked) it toggles the notification drop-down. Sym+Mic still types 0.

- Alt + letter (while typing): emoji layer - each letter key types its assigned emoji into the text field. Assign emojis per key in Settings > Emoji. An optional tap-to-latch mode for Alt (Settings > Device > Keyboard) keeps the layer on between taps.

- Alt + Mic (while typing): emoji search - opens a popup over the whole emoji set. Page through it, or jump by hex codepoint (e.g. 1F600 for smileys). Tapping an emoji inserts it into the text field you were typing in; the popup stays open for multiple inserts until Close (or Alt+Mic again).

- Sym (tap-to-latch): with the optional latch mode (Settings > Device > Keyboard), a clean tap of Sym latches the symbol layer until the next tap; holding Sym while typing stays momentary. WASD navigation pauses while latched - tap Sym again to resume.

- Alt + Backspace (hold ~1.5s): quit to home - closes the current app and returns to the launcher home page. The same chord quits a running native game (Doom, GameBoy, PICO-8, PC-XT).

- q: backs out of selection modes - message selection in a chat, row-select lists, and the Map app.

- Enter (in a chat): sends the message. Long-press the message input for the clipboard menu.]] },

{ t = "Emoji", b = [[
Emoji work anywhere you can type:

- Alt + letter types the emoji assigned to that key. Customize every key in Settings > Emoji.
- Alt + Mic opens the emoji search popup for everything you have not put on a key.

The standard emoji set ships with the firmware. An extended set (skin tones and many more sequences) can be downloaded over WiFi from Settings > Emoji - it lives on the SD card.]] },

{ t = "Messenger and mesh", b = [[
Full MeshCore support: direct messages, channels, and contacts.

Room servers and repeaters: log in, sync messages, and run admin commands right from the Messenger app.

In a chat: Enter sends the message. Long-press the message input to open the clipboard menu (paste copied contact cards and text). When selecting messages in the list, q backs out of selection.

Tip: before your first messages, set the radio to your local defaults (Settings > Radio) and review RX boost, Contact Overwrite, and Message Repeat.]] },

{ t = "Map", b = [[
The Map app shows OpenStreetMap tiles with mesh contact positions overlaid. Tiles download over WiFi and are cached on the SD card for offline use.

Keyboard shortcuts:
h - center on home (own GPS position)
q - quit (closes a popup first if open)
o or + - zoom in
i or - - zoom out
Space - stop scrolling
Enter - select contact at center / stop scrolling
c - cycle archived-contact pages
Trackball - pan the map

Pre-cache downloads: in the map settings you can bulk-download tiles for offline use - choose an area size and zoom range, then download. Tiles are written atomically, so interrupted downloads won't leave corrupt files.

Contact selection: long-press a contact marker (touch), or center it and press Enter, to view details - name, type, distance, hop count, last seen.

Meshprint: with enough mesh data you can run a meshprint on a message sender to capture the first and second hop repeaters and triangulate the sender's general location. The more data you have, the better the results.]] },

{ t = "Games and emulators", b = [[
Lua games included: Flappy Bird, Snake, and Scorched Earth.

Emulators install from the App Library; you provide the game files on the SD card:

- Doom: .wad files in /doom (or the Doom app folder). PWADs need a valid IWAD; Freedoom wads work too. Large wads take a while to load. Music and sound effects included.

- PICO-8: .p8 or .png carts in /p8carts (or the PICO-8 app folder).

- GameBoy: .gb/.gbc roms in /gb (or the GameBoy app folder).

- PC-XT (DOS): disk images in /dos. The app needs a bootable DOS floppy image (.img) to start; a game folder in /dos can then be mounted directly as the C: drive. A FreeDOS copy is in the MeshPunk GitHub - freedos40boot.img is modified for 40-column text.

Quitting a native game: hold Alt + Backspace for about 1.5 seconds to return to the launcher. Each game launcher's ? button shows this plus the game's controls.]] },

{ t = "Music and audio", b = [[
The Music app plays MP3s from /Music on the SD card.

- Tag-based library with playlists (playlists live in /Music/Playlists).
- Auto-organize sorts tagged files into /Music/Artist/Album for you.
- Background playback: music keeps playing while you use the rest of the device.

USB audio (experimental): route all device audio to a USB-C audio dongle via Tools > USB.]] },

{ t = "App Library and themes", b = [[
The App Library installs apps and themes onto the device over WiFi, and updates ones already installed - no firmware reflash required. It reads its catalog from github.com/PhilMo6/meshpunk-apps.

- Apps are grouped by category; when an installed app is behind the catalog, an Updates list appears at the top.
- Everything that ships with the firmware is tracked too, so even preinstalled apps and themes update OTA.
- System apps (App Library, Files, Map, Messenger, and the Settings pages) are non-removable, but can still be updated.

Themes: 15 are included - switch in Settings > Theme, and download more via Settings > Theme > Get.

Contributions (your own apps and themes) are welcome via pull request - see the catalog repo's README.]] },

{ t = "Files and storage", b = [[
Tools > Files is the file manager, covering both internal flash and the SD card.

An SD card is highly recommended: it persists your mesh and firmware settings, and holds game files, music, cached map tiles, and the extended emoji set.]] },

{ t = "Firmware updates", b = [[
Releases are on the MeshPunk GitHub releases page.

- First install: download the -merged.bin and flash it at meshcore.io/flasher (bottom of the page, Custom Firmware). Note: it replaces the filesystem with MeshPunk's - the flasher warns about this.

- Updates: download the -firmware.bin. It updates the firmware AND refreshes the bundled files automatically on the next boot; your settings and messages are kept.

- Launcher users: with bmorcelli's multi-firmware Launcher (2.7.2+), install the -launcher.bin through the Launcher (FAT32 SD, WebUI, or direct URL/OTA). First boot sets up the filesystem (about a minute). Don't install the -merged.bin through the Launcher - that one is for the web flasher.]] },

}

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

show_page = function(i)
    local pg = PAGES[i]
    swap_view(function(v)
        local content = new_content(v)
        tool(content, "Back", lvgl.PCT(31), show_contents)
        tool(content, "< Prev", lvgl.PCT(31), function()
            show_page(i > 1 and i - 1 or #PAGES)
        end)
        tool(content, "Next >", lvgl.PCT(31), function()
            show_page(i < #PAGES and i + 1 or 1)
        end)
        -- Title on its own themed card (readable over wallpaper). Clear just
        -- CLICKABLE so gridnav skips it (nav pitfall: default Objects are
        -- focusable).
        local trow = content:Object { w = lvgl.PCT(100), h = 26, pad_all = 4 }
        trow:clear_flag(lvgl.FLAG.SCROLLABLE)
        trow:clear_flag(lvgl.FLAG.CLICKABLE)
        trow:Label { text = i .. "/" .. #PAGES .. "  " .. pg.t, w = lvgl.PCT(100) }
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
        body:Label { text = pg.b, w = lvgl.PCT(100) }
    end)
end

show_contents = function()
    swap_view(function(v)
        local content = new_content(v)
        local trow = content:Object { w = lvgl.PCT(70), h = 24, pad_all = 3 }
        trow:clear_flag(lvgl.FLAG.SCROLLABLE)
        trow:clear_flag(lvgl.FLAG.CLICKABLE)
        trow:Label { text = "MeshPunk Guide", w = lvgl.PCT(100) }
        tool(content, "Home", 60, function() apps.go_home() end)
        for i, pg in ipairs(PAGES) do
            local b = content:Button { w = lvgl.PCT(100), h = 26 }
            b:Label { text = i .. ". " .. pg.t, align = lvgl.ALIGN.LEFT_MID }
            b:onClicked(function() show_page(i) end)
        end
    end)
end

show_contents()

return root
