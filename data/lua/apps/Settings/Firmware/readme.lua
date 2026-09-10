local caps, dev = ...

local body = [[
Firmware updates without a computer. The page shows the installed release (a development build shows "dev build") and whether this device's partition layout supports on-device updates.

- Check for update: connects to WiFi, looks up the latest MeshPunk release on GitHub and offers this device's -firmware.bin, as Download when it is newer and as Reinstall when the device already has it. The download goes to the SD card (S:/meshpunk/ota/) when a card is present, otherwise to internal flash.
- Scan SD card for a release file: lists meshpunk-]] .. (dev.name ~= "" and dev.name or "<board>") .. [[-<version>-firmware.bin files found in the card's root, /meshpunk or /meshpunk/ota. USB drive mode is the easy way to copy one there.
- Tap a file to verify it: the image header and its built-in SHA-256 are checked before anything is written.
- Restart to install: tap twice. The device restarts into the updater, which shows its progress on screen, writes the new firmware and restarts again. The first boot then unpacks the new bundled files. Settings, messages and contacts are kept. Do not power off while the updater is writing; if that happens it simply runs again on the next start.
- Delete staged image removes a downloaded file that has not been installed.

If the page says the layout predates on-device updates, flash the -merged.bin once by USB; after that, updates work from here.

Recovery: if the main firmware ever fails to start, copy a release -firmware.bin for this device into S:/meshpunk/ota/ and restart. The updater installs it and deletes the file.]]

if dev.name == "tdeck" then
    body = body .. [[

Devices installed through bmorcelli's Launcher update through the Launcher instead (install the new -launcher.bin there); this page says so when it detects one.]]
end

return { body = body }
