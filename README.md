# MeshPunk - LVGL with Lua for T-Deck

This branch of the Meshpunk project focuses on extending functionality.
Some features have been added or documented with the assistance of AI 

## Features

- Combines the power of LVGL with the simplicity of Lua scripting
- Runs on the LilyGo T-Deck
- Uses PlatformIO for easy building
- Dual buffer display
- Sound support
- Touch and trackball controls
- SD card support
- GPS automaticly gets time
- Loads Lua scripts from the filesystem automaticly as apps
- Integrates MeshCore networking
- Emoji support if provided emoji folder is placed on the t-decks SD card
- Games! Comes with Flappy Bird, Snake, and Scorched Earth (all games are in progress of devopment)

## Project Structure

- `/src` - Main C++ code
  - `main.cpp` - Main application code
- `/data` - Data files that get uploaded to the device filesystem
  - `/lua` - Lua scripts
    - `/apps` - Lua apps
    
## Requirements

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

Place the emoji folder onto a SD card to use emoji support.

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
