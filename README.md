# MeshPunk - LVGL with Lua for LoRa handhelds

## Supported devices

| Device | PlatformIO env | Inputs |
| --- | --- | --- |
| LilyGo T-Deck | `meshpunk` | Keyboard, trackball, touchscreen |
| Heltec V4 + Expansion Kit V2 | `meshpunk_heltec` | Touchscreen only |

Device-specific code lives behind a per-subsystem backend (`src/input`,
`src/display`, `src/audio`, `src/power`, `src/gps`), so the rest of the
firmware is board-neutral and gates on **capabilities** rather than on which
board it is. Releases are built per device — flash the build that matches
your hardware.

## Features

- Combines the power of LVGL with the simplicity of Lua scripting
- Uses PlatformIO for easy building
- Sound support (speaker, or buzzer melodies on boards without an audio amp)
- Touch, trackball and keyboard nav controls — whichever the board has
- On-screen keyboard and on-screen game pads for keyboardless boards, with a
  per-game layout editor
- SD card support
- BLE support for phone apps
- WiFi support
- GPS automatically gets time
- Loads Lua scripts from the filesystem automatically as apps
- Full MeshCore support
- Room server and repeater support: log in, sync messages, and run admin commands right from the Messenger app
- Swappable radio protocols — the LoRa protocol is an installable package. MeshCore comes preinstalled; MTLite, a Meshtastic-compatible protocol (channels with PSKs, direct messages, channel URL/QR sharing), installs from the App Library. See [Radio protocols](#radio-protocols).
- Full emoji support! Type emoji with the alt key layer (customize per-key in Settings > Emoji), plus a downloadable extended emoji set that lives on SD
- Map app with offline tile caching, message path animations, message path replay, and meshprint sender triangulation!
- Lua games! Comes with Flappy Bird, Snake, and Scorched Earth (all games are in progress of development)
- Elf file loader
- Doom! Now with music and sound effects! you must provide your own .wad files. PWADs require a valid IWAD. Place doom wads onto SD card.
- Pico8 emulator, same as doom you must provide your own .p8 or .png pico8 carts. (thanks to https://github.com/mintylinux)
- GameBoy emulator! you must provide your own .gb/.gbc roms. Link two T-Decks with a USB cable to play 2-player games — one deck runs USB host mode, the other plugs in as the device.
- NES emulator! you must provide your own .nes roms.
- SNES emulator! you must provide your own .smc/.sfc/.fig roms. Special-chip carts work too — SuperFX (Yoshi's Island) and Cx4 (Mega Man X2) — though the heaviest SuperFX games are slow.
- Neo Geo Pocket / Color emulator! you must provide your own .ngp/.ngc roms.
- Sega 8-bit emulator! Game Gear, Master System and SG-1000 in one app — you must provide your own .gg/.sms/.sg roms. The file extension picks the system, and the launcher tells you which one a rom will run as before you start it.
- DOS emulator! A full 386 PC with VGA, Adlib, Sound Blaster and a PS/2 mouse, running real DOS from .img disk images — or point it at a folder of games on your SD card and it becomes a writable C: drive. No disks yet? The app's **Download DOS** button fetches ready-made FreeDOS boot disks straight to the device over WiFi. The trackball works as a mouse (with a DOS mouse driver loaded) or as arrow keys.
- MP3 music player with a tag-based library, playlists, and auto-organizing by artist/album
- Background apps — music keeps playing while you use the rest of the device
- USB host support (Tools > USB Host): plug devices in — a USB-C audio adapter (routes all device audio), a gamepad (map it to controls for any game via the Games > Gamepad app), a mouse (moves focus, click selects), a keyboard, or a thumb drive (browsable as the `U:` drive). Gamepad, mouse and link-cable drivers download automatically from the App Library. **The device supplies no USB power — see [USB accessories](#usb-accessories) for what adapter you need.**
- Themes! make Meshpunk look the way you want. 15 themes are included!
- File manager (Tools > Files) for both internal flash and SD
- App Library — browse and install apps and themes straight from GitHub over WiFi, and update the ones you already have, no reflash needed


## Installation 

1. Download the release file you want to install from the release page. **Releases are built per device — the filenames carry the board name** (`meshpunk-tdeck-<version>-…` / `meshpunk-heltec_v4-<version>-…`); a build for the wrong board will not run correctly.
- For a first-time install download the -merged.bin file
- For updates download the -firmware.bin: it updates the firmware AND refreshes MeshPunk's bundled files automatically on the next boot (your settings and messages are kept)
2. Go to https://meshcore.io/flasher scroll to bottom and click on Custom Firmware
3. Select the firmware release you downloaded. If it is the merged firmware it will erase your filesystem to replace it with the Meshpunk one! The flasher will give you a warning about this.
4. Flash the firmware and wait.
5. It is highly suggested to use a SD card to persist your mesh and firmware settings.
6. Go to the radio settings and set them to your local default.
7. Set your extra settings, RX boost, Contact Overwrite, and Message Repeat
8. Get meshing!
9. Install apps!

Optional if installed. Download and place doom wad files onto the sd card in either /doom or /lua/apps/Games/Doom. You can get doom wads from https://freedoom.github.io/download.html. You can also use the original wad files. PWADS require a valid IWAD to run. remember that loading large wads can take a while.
Pico8 carts go onto the sd card in either /p8carts or /lua/apps/Games/PICO-8 folder.
Gameboy roms go onto the sd card in either /gb or /lua/apps/Games/GameBoy folder.
NES roms (.nes) go onto the sd card in either /nes or /lua/apps/Games/Nes folder.
SNES roms (.smc, .sfc, .fig) go onto the sd card in either /snes or /lua/apps/Games/Snes folder.
Neo Geo Pocket roms (.ngp, .ngc) go onto the sd card in either /ngpc or /lua/apps/Games/NeoGeoPocket folder.
Sega 8-bit roms (.gg, .sms, .sg) go onto the sd card in either /sega8 or /lua/apps/Games/Sega8 folder.
DOS disk images and game folders go onto the sd card in /dos.

If you have no disks the **Dos** app shows a **Download DOS** button — pick a disk, pick internal or SD storage, and it downloads over WiFi. The same list lives in Settings once you have one. Four disks are offered, all FreeDOS 1.4 with an XMS driver, CTMOUSE and EDIT already set up:

| Disk | Use |
| --- | --- |
| `freedos-a.img` | Boot floppy for the A: drive — start here |
| `freedos-c.img` | Bootable hard disk for C:, leaving A: free for game disks |
| `freedos-a-nb.img` / `freedos-c-nb.img` | Same two without `SET BLASTER`, for games that misbehave when they find a sound card |

Put a boot disk in A: (or C:), then either add game .img disks in A: or point C: at a folder of games — a folder becomes a real writable C: drive, so DOS installers work and save games persist. The **Boot** button shows every setting the emulator will start with, including whether the trackball is a mouse or arrow keys.

If a game misbehaves: **Audio rate** (the `?` next to it explains) trades pitch for emulation speed and cures crackle — 0.5 is the usual answer; **SB digital** can fake a card fault so a game turns its own digitised audio off; **Timer cap** keeps games alive that pace sound with the system timer. Keyboard: SYM+key for numbers/symbols, ALT+number for F1-F10, ALT+Backspace held to quit, Shift+Backspace for Esc, and ALT+Enter toggles your key bindings (WASD are arrows by default) so you can still type at the DOS prompt.

MP3s go onto the sd card in /Music. The Music app can auto-sort tagged files into /Music/Artist/Album for you, and playlists live in /Music/Playlists.

## Using with the Launcher (optional, T-Deck only)

MeshPunk can also be installed through [bmorcelli's Launcher](https://github.com/bmorcelli/Launcher) — a multi-firmware boot menu that lets you keep several firmwares on one device and choose which to boot. If you run the Launcher, install the **`-launcher.bin`** release, not the other files.

1. Download `meshpunk-<version>-launcher.bin` from the releases page.
2. Install it through the Launcher: from a FAT32 SD card, through the WebUI, or as a direct download URL / OTA.
3. On the first boot MeshPunk sets up its filesystem and unpacks its bundled files (about a minute). After that it boots normally.

Notes:

- The MeshPunk app carries its own files and populates its filesystem by itself, so no extra steps are needed in the Launcher. Launcher 2.7.x may ask whether to copy SPIFFS during the install — either answer works.
- Use `-launcher.bin` only. `-firmware.bin` (the bare app) installs but does not boot: it declares no partition layout, so the Launcher creates no data partition and MeshPunk has nowhere to unpack its files. `-merged.bin` is a full-flash image for the web flasher, not for the Launcher.
- Works with Launcher 2.7.2 and newer.
- This path is only for devices running the Launcher. For a normal install, use the flasher steps above.

## Radio protocols

The LoRa protocol is not baked into the firmware — it runs as an installable protocol package:

- **MeshCore** comes preinstalled and is the default; everything works out of the box.
- **MTLite**, a Meshtastic-compatible protocol, installs from the App Library's LoRa Protocols category over WiFi — installing it offers its Messenger, Radio, Notifications and Identity apps, which land in an MTLite launcher category. It speaks to standard Meshtastic networks: channels with PSKs, encrypted direct messages, node discovery, positions on the Map. Share or join a whole network the official way with a channel URL — the device generates the QR code for a phone to scan, or imports a pasted link.
- Pick the boot protocol in **Settings > Lora**. The switch happens at the next reboot, and each protocol keeps its own messages and settings — nothing mixes.
- "None" is a real choice: the device boots with the LoRa radio parked and everything else works.
- If the selected protocol is not installed, the device boots with the radio off and says so with a notification — nothing is substituted silently.

Bluetooth is its own slot (**Settings > Ble**): the MeshCore phone-app link is a BLE protocol that requires the MeshCore LoRa protocol running. There is no Meshtastic BLE protocol yet, so phone apps only pair while MeshCore is active.

Protocols update through the App Library like apps do; a protocol update takes effect at the next reboot. Nothing about protocols installs in the background: picking a protocol app whose protocol is missing asks first, and installing a protocol asks before downloading its apps.

## Map App

The Map app displays OpenStreetMap tiles with mesh contact positions overlaid. Tiles are downloaded over WiFi, converted to RGB565 `.bin` format, and cached on SD card for offline use.

- Map app touch controls

Drag to pan; the on-screen buttons cover zoom and the map menu. Long-press a contact marker for its details.

- Map app Keyboard Shortcuts (boards with a keyboard, or a USB one)

| Key | Action |
|-----|--------|
| `h` | Center on home (own GPS position) |
| `q` | Quit (close popup first if open) |
| `o` / `+` | Zoom in |
| `i` / `-` | Zoom out |
| `Space` | Stop scrolling |
| `Enter` | Select contact at center / stop scrolling |
| `c` | Cycle archived-contact page (when archived contacts are shown) |
| Trackball | Pan the map |

- Pre-cache Downloads

In the map settings you can download map tiles to bulk-download tiles for offline use. Choose an area size and zoom range, then download. Tiles are written atomically to SD so interrupted downloads won't leave corrupt files.

- Contact Selection

Long-press on a contact marker (touchscreen) or center the trackball on one and press Enter to view contact details including name, type, distance, hop count, and last seen time.

- Meshprint

With enough mesh data you can run a meshprint on the sender of a message to try to capture the first and second hop repeaters which will then be used to triangulate the senders general location.
The more data you have the better your results will be!

## App Library

The App Library (top-level app) installs apps and themes onto your device over WiFi, and updates ones already installed — no firmware reflash required. It reads its catalog from the companion repo:

**[github.com/PhilMo6/meshpunk-apps](https://github.com/PhilMo6/meshpunk-apps)**

Apps are grouped by category; when an installed app is behind the catalog an **Updates** list appears at the top. Themes have their own downloader under Settings > Theme > Get. Every app and theme that ships with the firmware is tracked here too, so even preinstalled ones can be updated OTA. Contributions (your own apps and themes) are welcome via pull request — see that repo's README.

System apps (App Library, Files, Map, Messenger, and the Settings pages) are non-removable, but can still be updated.

## Project Structure

- `/src` - Main C++ code
  - `main.cpp` - Main application code
- `/data` - Data files that get uploaded to the device filesystem
  - `/lua` - Lua scripts
    - `/apps` - Lua apps
    
## Requirements for Development

- PlatformIO
- A supported device (T-Deck or Heltec V4 + Expansion Kit)
- Git (for submodules)

## Building and Development

0. `softwareupdate --install-rosetta` (macOS on Apple Silicon only — the xtensa toolchain needs Rosetta)
1. Clone this repository
2. Initialize the submodules:
   ```
   git submodule update --init --recursive
   ```
3. Open in PlatformIO
4. Edit Lua scripts in the `/data/lua` directory
5. Build and upload to your device. The default env is the T-Deck; pass `-e`
   for any other board:
   ```
   pio run --target upload                    # T-Deck
   pio run -e meshpunk_heltec --target upload # Heltec V4
   ```
   This will upload only the firmware, not the filesystem data.
6. To upload the filesystem data (when changing lua scripts)
   ```
   pio run --target uploadfs
   pio run -e meshpunk_heltec --target uploadfs
   ```
7. To build release artifacts (written to `releases/`): the release envs embed
   the `data/` tree into the app so the published binaries are self-contained
   ```
   pio run -e meshpunk_release        # T-Deck  -> meshpunk-tdeck-<ver>-*.bin + launcher
   pio run -e meshpunk_heltec_release # Heltec  -> meshpunk-heltec_v4-<ver>-*.bin
   ```
   Each produces the board's `-merged`, `-firmware` and `-littlefs` binaries;
   the Launcher image is T-Deck only and keeps its original
   `meshpunk-<ver>-launcher.bin` name (its LauncherHub catalog entry depends
   on it). If a build reports missing littlefs, run
   `pio run [-e <env>] -t buildfs` first; if the firmware was already up to
   date, force the artifact step with `pio run -e <release env> -t mergebin`.

## VSCode hints

You must close the serial monitor before uploadfs or it wont work.

## Navigation

Which of these you have depends on the board:

- **Touchscreen** — tap to interact with elements directly. Present on every supported board, and the primary input on a board without a keyboard or trackball
- **Trackball** (T-Deck) — roll to move focus between elements, click to select
- **WASD keys** (boards with a keyboard) — `W`/`A`/`S`/`D` mirror trackball directions (up/left/down/right). When a text input is focused, WASD type normally instead
- **USB keyboard / mouse / gamepad** — attached through Tools → USB Host, these drive focus exactly as a trackball does, which is how a touch-only board gets key-driven navigation

Trackball and WASD share a configurable sensitivity setting (Device Settings → Trackball) that controls the minimum time between accepted direction inputs (0–500ms). That setting only applies to boards that have them.

A touch-only board has no directional input, so there is no focus to move — everything is reached by tapping it directly.

### On-screen keyboard and game pads

Boards without a keyboard get both, built into the firmware:

- **On-screen keyboard** — opens when a text field is focused; it is the text entry path in apps and in native games alike
- **On-screen game pad** — native games (Doom, GameBoy, DOS, the emulators) draw a button layout over the game, rendered outline-and-label only so the game stays visible behind it

One control cycles the on-screen input through pad → pad hidden → keyboard → off: the **IO button** on the Heltec, or **Shift+Alt** on a board with a keyboard. The **Touch input** row in Settings → Device does the same cycle — the way in for legacy T-Deck keyboards, which report no modifiers and so cannot chord. A board with a keyboard boots with all of it off; a keyboardless board boots with the pad on.

**Quitting a native game** — any of: hold `Alt`+`Backspace` ~1.5s (needs a keyboard), hold the on-screen **QUIT** button ~1s, or a key bound to quit in the launcher's Controls screen.

Each ELF game launcher has a **Touch** button opening a layout editor: drag to move a button, size steppers to resize, nudge arrows for fine positioning. Layouts are saved per game (`L:/touch_layouts/<app>.cfg`) and Reset restores the preset.

### Keyboard shortcuts

These require a built-in keyboard (a USB keyboard covers most of them too); on a touch-only board see the on-screen keyboard above.

- **Mic key** — global notifications shortcut: over a running app it peeks the top bar; on the launcher (or while peeked) it toggles the notification drop-down. `Sym`+`Mic` still types `0`.
- **Alt + letter (while typing)** — emoji layer: each letter key types its assigned emoji into the focused text field. Assign emojis per key in Settings → Emoji; an optional tap-to-latch mode for `Alt` (Settings → Device → Keyboard) keeps the layer on between taps.
- **Alt + Mic (while typing)** — emoji search: opens a popup over the whole emoji set (page through it, or jump by hex codepoint — e.g. `1F600` for smileys). Tapping an emoji inserts it into the text field you were typing in; the popup stays open for multiple inserts until Close (or `Alt`+`Mic` again). Use it for emojis you haven't assigned to a key.
- **Sym (tap-to-latch)** — with the optional latch mode (Settings → Device → Keyboard), a clean tap of `Sym` latches the symbol layer until the next tap; holding `Sym` while typing stays momentary. WASD navigation pauses while latched (the keys resolve to symbols) — tap `Sym` again to resume.
- **Alt + Backspace (hold ~1.5s)** — quit to home: closes the current app and returns to the launcher home page. The same chord quits a running native game (Doom, GameBoy, PICO-8, DOS) back to the launcher — each game launcher's `?` button shows it alongside the game's controls.
- **`q`** — backs out of selection modes: message selection in a chat, row-select lists, and the Map app.
- **Enter (in a chat)** — sends the message. Long-press the message input to open the clipboard menu (paste copied contact cards and text).

### Keyboard firmware compatibility (T-Deck only)

Full keyboard function requires LilyGo's **250620 or newer** keyboard firmware on the T-Deck's keyboard MCU (the separate ESP32-C3 that scans the keys). Units manufactured before mid-2025 shipped older keyboard firmware without raw-matrix support — on those, Meshpunk detects the mismatch after a few keypresses and automatically switches to a **legacy compatibility mode** (a notification confirms it; manual override in Settings → Device → "Legacy keyboard").

Legacy mode limitations (the old keyboard firmware reports one character per press, with no key-release or modifier information):

- Typing, WASD navigation, Enter/Backspace work — tap-based only, no key holds or repeats
- Sym/Alt tap-latches, the emoji layer, and all keyboard chords (including **Alt+Backspace quit-to-home**) are unavailable — use each app's on-screen controls, and quit native games with the **on-screen QUIT button** or a key bound to quit in the launcher (a USB keyboard's Alt+Backspace chord also works)
- The Shift+Alt on-screen-input trigger is a chord too — use the **Touch input** row in Settings → Device instead: set it to **Pad** before launching a game and the on-screen controls (including QUIT) are there
- On the oldest (2023) keyboard firmware the backlight ignores Meshpunk's brightness setting — toggle it with `Alt`+`B` (handled inside the keyboard itself)

For full function, the keyboard MCU can be reflashed with [LilyGo's keyboard firmware](https://github.com/Xinyuan-LilyGO/T-Deck/tree/master/firmware) (`T-Keyboard_Keyboard_ESP32C3_250620.bin`) via an external USB-TTL adapter on the 6-pin header next to the RST button — then turn legacy mode off in Settings → Device.

### USB accessories

The device can act as a USB host (Tools → USB Host → **Start**), so you can attach an audio adapter, keyboard, mouse, gamepad or thumb drive. Press **Stop** when finished — host mode runs background tasks the whole time it is on.

**The device supplies no power over USB.** If nothing happens when you plug something in, that is almost always why. Two kinds of adapter cover everything, and any generic one of either kind works:

| You want to attach | Use | Notes |
| --- | --- | --- |
| Keyboard, mouse, gamepad, thumb drive | **USB-C OTG splitter Y-cable with PD** — USB-C plug into the device, USB-A socket for the accessory, second USB-C socket for a charger or power bank | Search for "USB-C OTG splitter" + "PD". Common ones do 100W charging and USB 2.0 at up to 480 Mbps, no drivers. **Plug the charger in as well as the accessory** — a plain OTG adapter with no power input does nothing |
| Headphones or speakers | **USB-C to 3.5mm adapter with its own USB-C charging port** — sold as a 2-in-1 audio-and-charge adapter, typically PD 60W | Has a DAC and enumerates as a USB audio device. Carries no data, which does not matter for audio. **Does not need the OTG Y-cable** — it brings its own charging port |

An accessory with its own power supply or battery works without any adapter.

**USB hubs are not supported.** The firmware recognises a hub and refuses it (`usb_core.cpp` — single device only until IDF5), so it is one accessory at a time.

Drivers for the gamepad, mouse and link cable download automatically from the App Library the first time they are needed.

### USB drive mode

Tools → USB Drive shares the SD card with a PC: plug the device into the PC, press **Start sharing**, and it appears as a removable USB drive (~1 MB/s — the chip's USB is full-speed). While sharing, the PC owns the card exclusively: apps lose the SD drive and the mesh radio pauses. Eject the drive on the PC, then press **Stop** (or just leave the app) — the card remounts and the mesh resumes. Internal files can be shared by copying them to SD in Tools → Files first. After a drive session, USB **host** mode (Tools → USB Host) needs a reboot.

### Freeing up RAM

Games and emulators need a single large block of free memory, so a launch can fail even when total free memory looks like plenty — it is fragmented by whatever ran before. When a launch fails with a low-RAM notification:

1. **Restart and launch it again before opening anything else.** A fresh boot gives the largest unbroken block, and this fixes most launch failures on its own.
2. **Launch the game first, do everything else after.** Map tiles, long Messenger chats and Music leave memory broken up behind them.
3. If it still won't launch, switch off what you aren't using — each frees a modest amount: **WiFi** and the **BLE companion** (Settings → Wireless), and **USB host mode** (Tools → USB Host → Stop; its own memory use is small, but it runs background tasks while on).

## Usage

The example loads the `launcher.lua` script from the filesystem and displays a simple launcher UI. You can edit the Lua scripts in your IDE with proper syntax highlighting and then upload just the filesystem to quickly iterate on your UI design.

### Developing Lua Scripts

1. Edit the Lua scripts in `/data/lua`
2. Upload the filesystem with `pio run --target uploadfs`
3. The device will automatically load the updated scripts

### Adding Additional Scripts

You can create additional Lua scripts in the `/data/lua` directory. Scripts can be loaded from other scripts using `require`:

```lua
local utils = require('utils')
```

## PRs

Pull requests are welcome! Please keep in mind the following rules:

- should be minimal (only touch required files, minimal changes)
- respect the current code style and indentation
- be the most obvious code that will run performantly
- keep your code idiomatic unless theres a good reason not to

The goal is to make this project easy for new developers to pick up and contribute to.

## License

MIT

## Credits

- Original MeshPunk firmware, which this project is forked from and builds upon:
  - Ben Nolan — https://github.com/bnolan
  - Cameron L — https://github.com/mueslimak3r
- LuaVGL by XuNeo: https://github.com/XuNeo/luavgl
- LVGL: https://lvgl.io/
- LilyGo for the T-Deck hardware
- Heltec for the WiFi LoRa 32 V4 hardware and Expansion Kit
- Emojis from https://github.com/googlefonts/noto-emoji
- Emoji converted to .bin with ImageMagick
- doomgeneric https://github.com/ozkl/doomgeneric
   Doom music via Chocolate Doom's OPL/MIDI stack and the DOSBox dbopl emulator
- Pico8 emulation done with fake08 https://github.com/jtothebell/fake-08
   conversion of fake08 to meshpunk elf done by https://github.com/mintylinux
- GameBoy emulation via the gnuboy core from retro-go https://github.com/ducalex/retro-go
- NES emulation via Nofrendo (LGPL v2) (c) 1998-2000 Matthew Conte
   vendored as arduino-nofrendo; mapper and port work by Neil Stevens, Firebug,
   Benjamin C. W. Sittler and The Mighty Mike Master
- SNES emulation via the Snes9x core from retro-go, based on libretro snes9x2010
   NOTE: Snes9x is NOT free software — it may be used and distributed for
   NON-COMMERCIAL, personal use only; commercial use needs the copyright holders'
   permission. Also includes ndssfc (GPL v2) and ZSNES code (GPL v2)
- Neo Geo Pocket / Color emulation via RACE (GPL v2) by Judge_, Flavor, Thor and neopop_uk
   CZ80 by S. Dallongeville; Blip_Buffer (LGPL v2.1) by Shay Green;
   libretro-common (MIT); sound from NEOPOP, based on sn76496.c from MAME
- Sega Game Gear / Master System / SG-1000 emulation via the SMS Plus core from retro-go
   SMS Plus (GPL v2) (c) 1998-2007 Charles MacDonald, accuracy work by Eke-Eke (SMS Plus GX)
   SN76489 PSG by Maxim; YM2413 FM via emu2413 by Mitsutaka Okazaki
- DOS (386) emulation via tiny386 by Chunhui He https://github.com/hchunhui/tiny386 (BSD-3-Clause)
   Peripherals ported from QEMU/TinyEMU (MIT); VGA and IDE by Fabrice Bellard
   Adlib OPL2 via fmopl (LGPL); firmware is SeaBIOS + SeaVGABIOS (LGPL v3)
   The downloadable boot disks are FreeDOS https://www.freedos.org (GPL),
   with HIMEMX, CuteMouse (CTMOUSE) and FreeDOS Edit

## Branch
This branch of the Meshpunk project focuses on extending functionality.
Some features have been added or documented with the assistance of AI 
