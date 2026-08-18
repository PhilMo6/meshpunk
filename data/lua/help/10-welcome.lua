local caps, dev = ...

local body = [[
MeshPunk is a LoRa mesh communicator with full MeshCore support - plus offline maps, music, games and emulators, themes, a file manager, and an App Library for installing more over WiFi.

Highlights:
- SD card, BLE (phone apps), WiFi
- GPS sets the clock automatically
- Full emoji support, with a downloadable extended set
- Background apps - music keeps playing while you do other things
- USB accessories: keyboard, mouse, gamepad, audio adapter, thumb drive (they need external power - see the USB accessories page)
]]

-- What this particular device gives you to work with. Everything else in the
-- guide is written for the device it is being read on, so this sets the scene
-- rather than listing alternatives the reader does not have.
if caps.keyboard and caps.trackball then
    body = body .. [[

You have a keyboard, a trackball and a touchscreen, so every part of this guide applies to you.]]
elseif caps.touch then
    body = body .. [[

This device is driven by its touchscreen. There is no built-in keyboard, so text is typed on the on-screen keyboard and games use on-screen buttons - the Touch controls page covers both.]]
end

if dev.audio == "buzzer" then
    body = body .. [[

Sound comes from a buzzer, which plays notification melodies and app tones. For music and game audio you need a USB audio adapter, and like every USB accessory it needs external power - see the USB accessories page.]]
end

body = body .. [[

This guide covers day-to-day use. Flip pages with the < and > buttons above.]]

return {
    title   = "Welcome",
    section = "Guide",
    order   = 10,
    body    = body,
}
