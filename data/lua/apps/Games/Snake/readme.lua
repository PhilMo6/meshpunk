local caps = ...

local body = [[
Classic snake. Eat, grow, do not hit yourself. Your best score is kept between runs.
]]

if caps.keyboard then
    body = body .. [[
Steer with W A S D or the trackball.]]
else
    body = body .. [[
Steer with the on-screen pad.]]
end

return { body = body }
