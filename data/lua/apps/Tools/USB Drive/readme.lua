return { body = [[
Shares the SD card with a PC. Plug the device into the PC, press Start sharing, and the card appears as a removable USB drive - about 1 MB/s, because the chip's USB is full-speed.

While sharing, the PC owns the card exclusively: apps lose the SD drive and the mesh radio pauses. Eject the drive on the PC, then press Stop (or just leave the app) - the card remounts and the mesh resumes.

Internal files are not shared. To get one onto the PC, copy it to SD in Tools > Files first, then start sharing.

After a drive session, USB host mode (Tools > USB Host) needs a reboot before it will work again - the two cannot both own the USB port.]] }
