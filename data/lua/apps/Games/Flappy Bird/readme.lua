local caps = ...

local body = [[
Flap through the gaps. One tap, one job, endlessly frustrating.

Every pipe you pass is a point. The gaps tighten and the world speeds up as your score climbs, night falls every ten pipes, and dying on a decent run earns a medal: bronze at 10, silver at 20, gold at 30, platinum at 40.

There is a mute button, because the flap sound gets old fast - especially on a device with a buzzer.
]]

if caps.keyboard then
    body = body .. [[
Tap the screen, click the trackball or press almost any key to flap. The trackball also moves between the menu buttons.]]
else
    body = body .. [[
Tap the screen to flap.]]
end

return { body = body }
