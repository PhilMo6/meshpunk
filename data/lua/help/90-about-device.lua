-- A plain summary of what this device has. The rest of the guide is already
-- written for it, so this states facts and does not repeat advice.
local caps, dev = ...

local function yes(v) return v and "yes" or "no" end

local audio = "none"
if dev.audio == "i2s" then audio = "speaker"
elseif dev.audio == "buzzer" then audio = "buzzer (melodies and tones only)" end

local body = string.format([[
Hardware this firmware found:

Board - %s
Screen - %d x %d
Touchscreen - %s
Keyboard - %s
Trackball - %s
Sound - %s
]], dev.name ~= "" and dev.name or "unknown",
    dev.screen_w, dev.screen_h,
    yes(caps.touch), yes(caps.keyboard), yes(caps.trackball), audio)

return {
    title   = "About this device",
    section = "Guide",
    order   = 900,
    body    = body,
}
