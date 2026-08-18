local caps = ...

local body = [[
Runs the device as a USB host so you can plug accessories into it.

POWER COMES FIRST
The device does not supply power over USB. If nothing happens when you plug something in, that is almost always the reason.

For a keyboard, mouse, gamepad or thumb drive, use a USB-C OTG splitter Y-cable with PD: a USB-C plug into the device, a USB-A socket for the accessory, and a second USB-C socket for a charger or power bank. Plug the charger in as well as the accessory - a plain OTG adapter with no power input does nothing here.

For headphones, use a USB-C to 3.5mm adapter that also has a USB-C charging port. Those bring their own power, so they need no OTG cable.

Any generic adapter of either kind works. The USB accessories page in the guide covers what to look for.

USB hubs are not supported: the firmware recognises a hub and refuses it, so it is one accessory at a time.

USING IT
Press Start to begin, and Stop when you are finished. Host mode runs background tasks the whole time it is on, so leaving it off saves memory and battery.

WHAT WORKS
- Audio adapter: routes all device audio out over USB - music, app sounds and game audio alike.
- Gamepad: map it to controls for any game with the Games > Gamepad app.
- Mouse: moves the highlight, click selects.
- Keyboard: types, and its keys work anywhere a built-in keyboard would.
- Thumb drive: browsable as the U: drive in Tools > Files.

Drivers for the gamepad, mouse and link cable download automatically from the App Library the first time they are needed.]]

if not caps.keyboard then
    body = body .. [[

On this device a USB keyboard also unlocks the keyboard shortcuts listed in the Guide tab, a gamepad is the comfortable way to play the emulators, and an audio adapter is how you get music and game sound out.]]
end

body = body .. [[

Tools > USB Drive is the opposite arrangement - the device plugs into a PC and the PC supplies the power. Only one of the two can own the USB port, so after a USB Drive session the device needs a reboot before host mode will work again.]]

return { body = body }
