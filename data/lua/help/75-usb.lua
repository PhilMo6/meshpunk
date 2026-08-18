local caps = ...

local body = [[
The device can act as a USB host, so you can plug accessories into it: an audio adapter, a keyboard, a mouse, a gamepad, or a thumb drive.

POWER COMES FIRST
The device does not supply power over USB, so the accessory has to get power from somewhere else. If nothing happens when you plug something in, this is almost always the reason.

WHAT TO BUY
Two kinds of adapter cover everything. Any generic one of either kind works - there is nothing special about a particular brand.

For a keyboard, mouse, gamepad or thumb drive:
A USB-C OTG splitter Y-cable with PD. It has three ends - a USB-C plug into the device, a USB-A socket for your accessory, and a second USB-C socket for a charger or power bank. Search terms are "USB-C OTG splitter" and "PD"; the common ones handle 100W charging and USB 2.0 accessories at up to 480Mbps, and need no drivers.

Plug the charger in as well as the accessory. That is the part people miss - a plain OTG adapter with no power input does nothing here.

For headphones or speakers:
A USB-C to 3.5mm adapter that also has a USB-C charging port - sold as a 2-in-1 audio and charge adapter, usually with PD up to 60W. These have a DAC chip inside and appear to the device as a USB audio device. They do not carry data, which is fine: audio and power are all they need to do. This kind does not need the OTG Y-cable, because it brings its own charging port.

An accessory with its own power supply or battery works without any adapter.

USB hubs are not supported. The firmware recognises a hub and refuses it, so it is one accessory at a time.

TURNING IT ON
Tools > USB Host, then Start. Press Stop when you are finished: host mode runs background tasks the whole time it is on, so leaving it off saves memory and battery.

WHAT WORKS
- Audio adapter: routes all device audio out over USB - music, app sounds and game audio alike.
- Gamepad: map it to controls for any game with the Games > Gamepad app.
- Mouse: moves the highlight, click selects.
- Keyboard: types, and its keys work anywhere a built-in keyboard would.
- Thumb drive: browsable as the U: drive in Tools > Files.

Drivers for the gamepad, mouse and link cable download automatically from the App Library the first time they are needed.
]]

if not caps.keyboard then
    body = body .. [[

ON THIS DEVICE
A USB keyboard unlocks the keyboard shortcuts listed in the Guide tab and types anywhere the on-screen keyboard would. A gamepad is the comfortable way to play the emulators, and an audio adapter is how you get music and game sound out.
]]
end

body = body .. [[

NOT THE SAME AS USB DRIVE
Tools > USB Drive is the opposite arrangement: the device plugs into a PC and appears as a removable drive, and the PC supplies the power. Only one of the two can own the USB port, so after a USB Drive session the device needs a reboot before host mode will work again.]]

return {
    title   = "USB accessories",
    section = "Guide",
    order   = 75,
    body    = body,
}
