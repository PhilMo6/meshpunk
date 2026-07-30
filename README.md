# MeshPunk - LVGL with Lua for T-Deck

## Features

- Combines the power of LVGL with the simplicity of Lua scripting
- Runs on the LilyGo T-Deck
- Uses PlatformIO for easy building
- Sound support
- Touch and trackball nav controls
- SD card support
- BLE support for phone apps
- WiFi support
- GPS automatically gets time
- Loads Lua scripts from the filesystem automatically as apps
- Full MeshCore support
- Room server and repeater support: log in, sync messages, and run admin commands right from the Messenger app
- Full emoji support! Type emoji with the alt key layer (customize per-key in Settings > Emoji), plus a downloadable extended emoji set that lives on SD
- Map app with offline tile caching, message path animations, message path replay, and meshprint sender triangulation!
- Lua games! Comes with Flappy Bird, Snake, and Scorched Earth (all games are in progress of development)
- Elf file loader
- Doom! Now with music and sound effects! you must provide your own .wad files. PWADs require a valid IWAD. Place doom wads onto SD card.
- Pico8 emulator, same as doom you must provide your own .p8 or .png pico8 carts. (thanks to https://github.com/mintylinux)
- GameBoy emulator! you must provide your own .gb/.gbc roms. Link two T-Decks with a USB cable to play 2-player games — one deck runs USB host mode, the other plugs in as the device.
- DOS emulator! A full 386 PC with VGA, Adlib, Sound Blaster and a PS/2 mouse, running real DOS from .img disk images — or point it at a folder of games on your SD card and it becomes a writable C: drive. No disks yet? The app's **Download DOS** button fetches ready-made FreeDOS boot disks straight to the device over WiFi. The trackball works as a mouse (with a DOS mouse driver loaded) or as arrow keys.
- MP3 music player with a tag-based library, playlists, and auto-organizing by artist/album
- Background apps — music keeps playing while you use the rest of the device
- USB host support (Tools > USB Host): plug devices into the T-Deck — a USB-C audio dongle (routes all device audio), a gamepad (map it to controls for any game via the Games > Gamepad app), a mouse (moves focus, click selects), a keyboard, or a thumb drive (browsable as the `U:` drive). Gamepad, mouse and link-cable drivers download automatically from the App Library.
- Themes! make Meshpunk look the way you want. 15 themes are included!
- File manager (Tools > Files) for both internal flash and SD
- App Library — browse and install apps and themes straight from GitHub over WiFi, and update the ones you already have, no reflash needed


## Installation 

1. Download the release file you want to install from the release page.
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

## Using with the Launcher (optional)

MeshPunk can also be installed through [bmorcelli's Launcher](https://github.com/bmorcelli/Launcher) — a multi-firmware boot menu that lets you keep several firmwares on one device and choose which to boot. If you run the Launcher, install the **`-launcher.bin`** release, not the other files.

1. Download `meshpunk-<version>-launcher.bin` from the releases page.
2. Install it through the Launcher: from a FAT32 SD card, through the WebUI, or as a direct download URL / OTA.
3. On the first boot MeshPunk sets up its filesystem and unpacks its bundled files (about a minute). After that it boots normally.

Notes:

- The MeshPunk app carries its own files and populates its filesystem by itself, so no SPIFFS copy options or extra steps are needed in the Launcher.
- `-firmware.bin` (the bare app) also installs through the Launcher — MeshPunk creates its own data partition if none exists. `-launcher.bin` is preferred since it declares the partition layout up front.
- Don't install the `-merged.bin` through the Launcher; that one is a full-flash image for the web flasher.
- Works with Launcher 2.7.2 and newer.
- This path is only for devices running the Launcher. For a normal install, use the flasher steps above.

## Map App

The Map app displays OpenStreetMap tiles with mesh contact positions overlaid. Tiles are downloaded over WiFi, converted to RGB565 `.bin` format, and cached on SD card for offline use.

- Map app Keyboard Shortcuts

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
- T-Deck device
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
5. Build and upload to your T-Deck device:
   ```
   pio run --target upload
   ```
   This will upload only the firmware, not the filesystem data.
6. To upload the filesystem data (when changing lua scripts)
   ```
   pio run --target uploadfs
   ```
7. To build release artifacts (written to `releases/`): the release env embeds
   the `data/` tree into the app so the published binaries are self-contained
   ```
   pio run -e meshpunk_release
   ```
   If it reports missing littlefs, run `pio run -t buildfs` first; if the
   firmware was already up to date, force the artifact step with
   `pio run -e meshpunk_release -t mergebin`.

## VSCode hints

You must close the serial monitor before uploadfs or it wont work.

## Navigation

The device supports three input methods for navigating the UI:

- **Trackball** — roll to move focus between elements, click to select
- **WASD keys** — `W`/`A`/`S`/`D` mirror trackball directions (up/left/down/right). When a text input is focused, WASD type normally instead
- **Touchscreen** — tap to interact with elements directly

Trackball and WASD share a configurable sensitivity setting (Device Settings → Trackball) that controls the minimum time between accepted direction inputs (0–500ms).

### Keyboard shortcuts

- **Mic key** — global notifications shortcut: over a running app it peeks the top bar; on the launcher (or while peeked) it toggles the notification drop-down. `Sym`+`Mic` still types `0`.
- **Alt + letter (while typing)** — emoji layer: each letter key types its assigned emoji into the focused text field. Assign emojis per key in Settings → Emoji; an optional tap-to-latch mode for `Alt` (Settings → Device → Keyboard) keeps the layer on between taps.
- **Alt + Mic (while typing)** — emoji search: opens a popup over the whole emoji set (page through it, or jump by hex codepoint — e.g. `1F600` for smileys). Tapping an emoji inserts it into the text field you were typing in; the popup stays open for multiple inserts until Close (or `Alt`+`Mic` again). Use it for emojis you haven't assigned to a key.
- **Sym (tap-to-latch)** — with the optional latch mode (Settings → Device → Keyboard), a clean tap of `Sym` latches the symbol layer until the next tap; holding `Sym` while typing stays momentary. WASD navigation pauses while latched (the keys resolve to symbols) — tap `Sym` again to resume.
- **Alt + Backspace (hold ~1.5s)** — quit to home: closes the current app and returns to the launcher home page. The same chord quits a running native game (Doom, GameBoy, PICO-8, DOS) back to the launcher — each game launcher's `?` button shows it alongside the game's controls.
- **`q`** — backs out of selection modes: message selection in a chat, row-select lists, and the Map app.
- **Enter (in a chat)** — sends the message. Long-press the message input to open the clipboard menu (paste copied contact cards and text).

### USB drive mode

Tools → USB Drive shares the SD card with a PC: plug the device into the PC, press **Start sharing**, and it appears as a removable USB drive (~1 MB/s — the chip's USB is full-speed). While sharing, the PC owns the card exclusively: apps lose the SD drive and the mesh radio pauses. Eject the drive on the PC, then press **Stop** (or just leave the app) — the card remounts and the mesh resumes. Internal files can be shared by copying them to SD in Tools → Files first. After a drive session, USB **host** mode (Tools → USB Host) needs a reboot.

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
- Emojis from https://github.com/googlefonts/noto-emoji
- Emoji converted to .bin with ImageMagick
- doomgeneric https://github.com/ozkl/doomgeneric
   Doom music via Chocolate Doom's OPL/MIDI stack and the DOSBox dbopl emulator
- Pico8 emulation done with fake08 https://github.com/jtothebell/fake-08
   conversion of fake08 to meshpunk elf done by https://github.com/mintylinux
- GameBoy emulation via the gnuboy core from retro-go https://github.com/ducalex/retro-go
- DOS (386) emulation via tiny386 by Chunhui He https://github.com/hchunhui/tiny386 (BSD-3-Clause)
   Peripherals ported from QEMU/TinyEMU (MIT); VGA and IDE by Fabrice Bellard
   Adlib OPL2 via fmopl (LGPL); firmware is SeaBIOS + SeaVGABIOS (LGPL v3)
   The downloadable boot disks are FreeDOS https://www.freedos.org (GPL),
   with HIMEMX, CuteMouse (CTMOUSE) and FreeDOS Edit

## Branch
This branch of the Meshpunk project focuses on extending functionality.
Some features have been added or documented with the assistance of AI 
