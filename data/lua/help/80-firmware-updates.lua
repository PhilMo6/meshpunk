local _, dev = ...

-- Release artifacts are meshpunk-<board>-<version>-{merged,firmware,
-- littlefs}.bin, and dev.name is the same board slug the filenames carry.
-- The Launcher image is T-Deck only and keeps its own unslugged name
-- (its LauncherHub catalog entry downloads it by explicit URL).
local build = dev.name ~= "" and dev.name or "your device"

local body = [[
Releases are on the MeshPunk GitHub releases page. Builds are per device: the files for this device have ]] .. build .. [[ in their name, and another device's build will not run correctly on this one.

- First install: download the -merged.bin and flash it at meshcore.io/flasher (bottom of the page, Custom Firmware). Note: it replaces the filesystem with MeshPunk's - the flasher warns about this.

- Updates: download the -firmware.bin. It updates the firmware AND refreshes the bundled files automatically on the next boot; your settings and messages are kept.]]

if dev.name == "tdeck" then
    body = body .. [[

- Launcher users: with bmorcelli's multi-firmware Launcher (2.7.2+), install the -launcher.bin through the Launcher (FAT32 SD, WebUI, or direct URL/OTA). First boot sets up the filesystem (about a minute). Don't install the -merged.bin through the Launcher - that one is for the web flasher.]]
end

return {
    title   = "Firmware updates",
    section = "Guide",
    order   = 80,
    body    = body,
}
