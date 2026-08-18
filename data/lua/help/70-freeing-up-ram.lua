return {
    title   = "Freeing up RAM",
    section = "Guide",
    order   = 70,
    body    = [[
Games and emulators are the hungriest things the device runs. Each one needs a single large block of free memory, so a launch can fail even when the total free memory looks like plenty - the memory is there, just broken into pieces by whatever ran before it.

If an app or game fails to launch with a low-RAM message:

1. Restart the device and launch it again, before opening anything else. A fresh boot gives the largest unbroken block of memory, and this fixes most launch failures on its own.
2. Launch the game first, then do everything else afterwards. Apps that load a lot - Map tiles, long Messenger chats, Music - leave memory broken up behind them.

If it still won't launch, switch off what you are not using. Each of these frees a modest amount, so use them together:

- WiFi (Settings > Wireless): leave it off unless you are downloading map tiles, apps, or the extended emoji set.
- BLE companion (Settings > Wireless): off unless a phone app is connected to the device.
- USB host mode (Tools > USB Host): press Stop when you are done with a USB device. Its own memory use is small, but it also runs background tasks while it is on.

A restart is by far the biggest win here - try that first, every time.]],
}
