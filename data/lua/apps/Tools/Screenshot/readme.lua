local caps = ...

local body = [[
Saves a PNG of whatever is on the screen.

Opening the app puts a small SHOT button on top of everything. Tap it to capture; drag it anywhere if it is covering what you want, and it stays where you left it next time. It hides itself for the moment of the capture, so the button is never in its own picture.

Pictures go to the screenshots folder on the SD card, named by the date and time. With no card they go to internal storage instead. Open them again in Tools > Images, or copy them off with Tools > USB Drive.

Sharing the card with a PC closes every background app, this one included, because a shared card cannot have files open on the device. Take your shots first, then start sharing to copy them off.

Use Run in background to keep the button while you go elsewhere - that is the point of it. The launcher lists the app while it is backgrounded; the X on that row takes the button away again. Turn overlay off does the same from inside the app.

Native games (Doom, GameBoy, DOS and the rest) take over the whole device, so the button goes away while one runs. Each game launcher carries its own two ways instead:

Its Touch editor has a SHOT pad, switched off to begin with so it costs no screen space in games you never capture. Select it, press On, then drag it somewhere your thumbs will not hit by accident.

Its Controls screen has a Screenshot action, with no key on it to begin with so it takes no key away from the game. Pick any key and it becomes a capture that the game never sees.]]

if not caps.keyboard then
    body = body .. [[

This device has no built-in keyboard, so the Controls binding applies to a USB keyboard attached through Tools > USB Host. The SHOT pad is the one to use otherwise.]]
end

body = body .. [[

A capture during a game costs the game a brief stutter while the file is written - the SD card and the screen share one bus.]]

return { body = body }
