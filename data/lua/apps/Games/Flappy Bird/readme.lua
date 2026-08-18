local caps = ...

local body = [[
Flap through the gaps. One control, one job, endlessly frustrating.

There is a mute button, because the flap sound gets old fast - especially on a device with a buzzer.
]]

if caps.keyboard then
    body = body .. [[
Flap with the trackball click or a key; the on-screen button works too.]]
else
    body = body .. [[
Tap the screen to flap.]]
end

return { body = body }
