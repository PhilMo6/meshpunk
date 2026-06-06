# MeshPunk - LVGL with Lua for T-Deck

## Features

- Combines the power of LVGL with the simplicity of Lua scripting
- Runs on the LilyGo T-Deck
- Uses PlatformIO for easy building
- Dual buffer display
- Sound support
- Touch and trackball nav controls
- SD card support
- BLE support for phone apps
- Wifi support though no use yet
- GPS automaticly gets time
- Loads Lua scripts from the filesystem automaticly as apps
- Integrates MeshCore networking
- Emoji support
- Games! Comes with Flappy Bird, Snake, and Scorched Earth (all games are in progress of devopment)
- Elf file loader
- Doom! you must provide your own .wad files. PWADs require a valid IWAD. Place doom wads onto SD card.

## Installation 

1. Download the release you want to install from the release page.
2. Go to https://meshcore.io/flasher scroll to bottom and click on Custom Firmware
3. Select the firmware release you downloaded. It will be a merged firmware and will erase your filesystem to replace it with the Meshpunk one!
4. Flash the firmware and wait.
5. It is highly suggested to use a SD card to persist your mesh and firmware settings.
6. Go to the radio settings and set them to your local default. 

Optional. Download and place doom wad files onto the sd card in ether /doom or /meshpunk/apps/Games/doom. You can get doom wads from https://freedoom.github.io/download.html. You can also use the original wad files. PWADS require a valid IWAD to run. remember that loading large wads can take a while.


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

## VSCode hints

You must close the serial monitor before uploadfs or it wont work.

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

## Branch
This branch of the Meshpunk project focuses on extending functionality.
Some features have been added or documented with the assistance of AI 
