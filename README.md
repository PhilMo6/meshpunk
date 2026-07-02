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
- Emoji support
- Map app with offline tile caching, message path animations, message path replay, and meshprint sender triangulation!
- Lua games! Comes with Flappy Bird, Snake, and Scorched Earth (all games are in progress of development)
- Elf file loader
- Doom! you must provide your own .wad files. PWADs require a valid IWAD. Place doom wads onto SD card.
- Pico8 emulator, same as doom you must provide your own .p8 or .png pico8 carts. (thanks to https://github.com/mintylinux)
- Themes! make Meshpunk look the way you want. 14 themes are included!

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

Optional. Download and place doom wad files onto the sd card in either /doom or /meshpunk/apps/Games/doom. You can get doom wads from https://freedoom.github.io/download.html. You can also use the original wad files. PWADS require a valid IWAD to run. remember that loading large wads can take a while.
Pico8 carts go onto the sd card in either /p8carts or /meshpunk/apps/Games/PICO-8 folder.

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

With enough mesh data you can run a meshprint on the sender of a message to try to capture the first and second hop repeaters which will then be used to triangulate the senders general locaton.
The more data you have the better your results will be!

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

0. `softwareupdate --install-rosetta`
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

- LuaVGL by XuNeo: https://github.com/XuNeo/luavgl
- LVGL: https://lvgl.io/
- LilyGo for the T-Deck hardware
- Emojis from https://github.com/googlefonts/noto-emoji
- Emoji converted to .bin with ImageMagick
- doomgeneric https://github.com/ozkl/doomgeneric
- Pico8 emulation done with fake08 https://github.com/jtothebell/fake-08
   conversion of fake08 to meshpunk elf done by https://github.com/mintylinux

## Branch
This branch of the Meshpunk project focuses on extending functionality.
Some features have been added or documented with the assistance of AI 
