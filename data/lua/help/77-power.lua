local caps = ...

local wake_key = caps.trackball and "click the trackball" or "press the USER button"

local body = [[
The device has three power actions and a low-power Standby mode that keeps the mesh running while the screen is off.

WHERE TO FIND THEM
Two places, same actions:
- Tap the battery percentage in the top bar. A power menu drops down, the same way tapping the rest of the bar drops down notifications.
- Settings > Power, which also holds the standby options below.

THE THREE ACTIONS
Standby - the screen and sounds go off, the radio keeps listening. Takes effect immediately.
Power off - a real shutdown. Tap it twice to confirm.
Restart - reboots the device. Tap it twice to confirm.

STANDBY
Standby is the one to use day to day. The screen sleeps, sound and GPS shut down, but the radio stays in full receive: messages keep arriving, get decrypted, saved and acknowledged the whole time it is dark. Nothing is missed and nothing is deferred - when you wake it, your conversations are already up to date.

It wakes two ways:
- A notification arrives. The alert chime plays and the screen comes back, exactly as if you had been looking at it. Traffic that is not for you - other people's messages, adverts, routing - is handled silently and the device stays dark.
- You ]] .. wake_key .. [[.

Waking returns you to whatever was on screen before.

POWER OFF
Power off is a deep sleep: everything shuts down and battery drain drops to almost nothing, enough to leave it for weeks. Nothing is received while it is off. To turn it back on, ]] .. wake_key .. [[.

Messages, contacts and settings are all saved before it powers down.

AUTO STANDBY
In Settings > Power you can have the device enter Standby by itself after a period of no input - 5, 10, 15, 30 or 60 minutes. It is off by default. This is the setting that gets you the most battery life out of a day: the device spends its idle time dark and listening instead of lit.

The screen and keyboard backlight timeouts in Settings > Device still work as before, and happen first.
]]

if caps.kbd_backlight then
  body = body .. [[

STANDBY HEARTBEAT
While in standby, the keyboard backlight glows briefly every so often so you can tell a dark device is alive and listening rather than switched off. Turn it off, or change the interval (15, 30 or 60 seconds), in Settings > Power.
]]
end

body = body .. [[

WHAT IF IT DOES NOT WAKE
If the device seems unresponsive, plug in USB power and try again. A battery too low to start up can leave it unable to boot while still having enough charge to sit in standby.
]]

return { body = body }
