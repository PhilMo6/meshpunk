local caps, dev = ...

local body = [[
Flashes ESP32 firmware onto another device over a USB cable, with this device as the flasher: Meshpunk, MeshCore, Meshtastic, your own build - any ESP32 firmware image, with no PC involved.

WHAT YOU NEED
- The firmware as a .bin file on this device's SD card. The app lists the files in the SD card's firmware folder and its top folder, and Browse reaches any other folder. For Meshpunk itself, "Download a Meshpunk release" fetches the latest stable or dev release over WiFi and saves the files you pick into that folder, so nothing has to be copied by hand.
- A USB-C cable between the two devices.
]]

if dev.usb_power then
    body = body .. [[
- This device powers the target while it is flashed, so the target needs no battery.
]]
else
    body = body .. [[
- The target needs its own battery or power: this device does not supply power over USB.
]]
end

body = body .. [[

WHICH CHIPS AND BOARDS
The target's chip is read over USB: ESP32-S3, ESP32-C3, ESP32-C6, ESP32-S2 and the classic ESP32. Boards that connect through the chip's own USB port work directly - every Meshpunk board does - and so do boards with a CP2102 USB-serial chip, such as the Heltec V3. Boards with a CH9102 or CH340 chip (the LilyGo T-Beam family, for one) are not supported yet.

A board with a USB-serial chip needs USB POWER even when it has a battery: the serial chip runs from the USB 5 V, not from the battery, so on a host that supplies no power it never shows up. Use a powered adapter, or a host that powers the port.

The target cannot say which BOARD it is, only which chip. An image built for another chip is refused; an image for another board with the same chip is not, so check the board yourself before pressing Flash - as with every flasher.

THE KINDS OF FILE
A FULL IMAGE holds everything from the bootloader up: Meshpunk and MeshCore name these -merged.bin, Meshtastic names them .factory.bin. The whole flash is erased first, then the image is written from the start, so everything on the target is lost: identity, settings, contacts, messages. Use it for a device that has never run this firmware, or to start one over. This is what the MeshCore flasher calls an erase-all install.

Any other firmware file is an UPDATE. It is written into the app partition the target already has (0x10000 on every standard layout; otherwise the app shows the target's partitions to choose from) and the target's data is kept. An update is refused when the target has no partition table yet - use a full image first.

The name is only a first guess. The file itself decides: one that contains a bootloader and a partition table is treated as a full image whatever it is called, and the confirm screen says which it is before anything is written.

MESHTASTIC
A complete Meshtastic install is three files from the release zip: firmware-<board>-<version>.factory.bin, the update helper mt-<chip>-ota.bin, and littlefs-<board>-<version>.bin. Put them in the same folder and pick the .factory.bin: the app finds the other two, reads where they belong from the image's own partition table, and offers to flash all three in one go. The firmware alone can be flashed too. To update a device that already runs Meshtastic, pick the plain firmware-<board>-<version>.bin instead.

"Write a file at an offset" writes any file at an address you type: a bootloader, a partition table, a filesystem image from a custom build. Only the chip and the flash size are checked.

Meshpunk release files also carry the board name inside the image, and that is checked against the file name - keep their published names.

BACKING UP
"Back up a device" reads the target's partition table and lists every partition; tick the ones to save. Data partitions (settings, identity, filesystems - whatever the firmware calls them) start ticked, app partitions can be ticked too, and "Whole device" copies the entire flash into one file. Before a full-image flash the same choice is offered as "Back up the target first", so a repeater can be flashed with new firmware and then given its old settings back.

Reading is slow: the bootloader hands over 64 bytes at a time, roughly a minute per 2 MB, so a whole 8 MB device takes several minutes. The bar keeps moving; every file is checked against the device's own checksum when it is done. Backups go to firmware/backups on the SD card, one folder per run, each file named <partition>-<offset>-<size>.bin so it knows where it belongs.

To restore, flash the firmware as usual and use "Add a backup file to restore" on the confirm screen: the file is written back to its partition after the image, provided the image's layout has a partition there that is large enough. A whole-device file flashes back as a full image - and onto another board it would clone this one's identity, so keep it for the board it came from.

USING IT
Pick the file, connect the target, and wait for the check. Confirm the board, then Flash. Leave the cable in and stay in the app until it says Done; every part is verified after it is written.

IF THE TARGET IS NOT FOUND
The app resets the target into its bootloader by itself. That does not work when the target runs firmware that has taken over its USB port, or is itself in USB host or USB drive mode. Put it into download mode by hand: hold its BOOT button while pressing reset, or while plugging the cable in. On a T-Deck the BOOT button is the trackball press; on Heltec boards it is marked PRG or BOOT.

A target put into download mode by hand may not restart by itself afterwards. Press its reset button.

IF A FLASH STOPS HALFWAY
The target will not start, but it is not damaged. Put it into download mode by hand and flash it again.

The app turns USB host mode on if it is off, and off again when you leave. The link cable to another Meshpunk device is paused while the app is open.]]

return { body = body }
