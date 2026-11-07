local _, dev = ...

-- Release artifacts are meshpunk-<board>-<version>-<kind>.bin, the T-Deck and
-- Wio L2 Launcher images included, and dev.name is the same board slug the
-- filenames carry.
local build = dev.name ~= "" and dev.name or "your device"

local body = [[
Releases are on the MeshPunk GitHub releases page. Builds are per device: the files for this device have ]] .. build .. [[ in their name, and another device's build will not run correctly on this one.

- First install: download the -merged.bin and flash it at meshcore.io/flasher (bottom of the page, Custom Firmware). Note: it replaces the filesystem with MeshPunk's - the flasher warns about this.

- Updates: Settings > Firmware checks GitHub over WiFi and installs the new release from the device, or installs a -firmware.bin copied to the SD card; no computer needed. The firmware AND the bundled files are refreshed on the next boot; your settings and messages are kept. Flashing the -firmware.bin by USB does the same.

- Channel: the Firmware page checks the stable releases by default. Switching it to dev checks a separate development repo instead, whose -devN builds are unreleased and expected to be rough. Nothing else changes: the same device tag, verification and updater install them. Switching back to stable offers the newest stable release even though it is older than the dev build that is running - that is the way back, and it is labelled Switch rather than Update.

- One-time step for a device flashed before the updater existed: the Firmware page says the layout predates on-device updates. Flash the -merged.bin once by USB (this replaces the filesystem); after that, updates work from the page.

- The updater has its own screen: it shows the job, verifies the file again, draws a progress bar while writing, and restarts when done. Do not power off while it writes; if that happens anyway, it simply runs the same job again on the next start.

- Only releases from the one that introduced the updater onward install this way; each image carries a device tag and the page refuses files built for another device or from before the tag existed. Older releases still flash by USB.

- Recovery: if the firmware ever fails to start, copy a release -firmware.bin for this device into /meshpunk/ota/ on the SD card and restart. The updater installs it and deletes the file.]]

local launcher_req = nil
if dev.name == "tdeck" then
    launcher_req = "2.7.2+"
elseif dev.name == "wio_l2" then
    launcher_req = "a release with Wio Tracker L2 support"
end

if launcher_req then
    body = body .. [[

- Launcher users: with bmorcelli's multi-firmware Launcher (]] .. launcher_req .. [[), install the -launcher.bin through the Launcher (FAT32 SD, WebUI, or direct URL/OTA). First boot sets up the filesystem (about a minute). Don't install the -merged.bin through the Launcher - that one is for the web flasher. Updates come through the Launcher too; the Firmware page detects a Launcher install and offers nothing there.]]
end

return {
    title   = "Firmware updates",
    section = "Guide",
    order   = 80,
    body    = body,
}
