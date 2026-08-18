local caps = ...

local body

if caps.keyboard then
    body = [[
Emoji work anywhere you can type:

- Alt + letter types the emoji assigned to that key. Customize every key in Settings > Emoji.
- Alt + Mic opens the emoji search popup for everything you have not put on a key.
]]
else
    body = [[
Emoji work anywhere you can type. Open the on-screen keyboard by tapping a text field, then switch it to its emoji layer and tap the one you want.

The letter keys carry your own assigned emoji, and you can change what sits on each key in Settings > Emoji. The browser layer covers everything you have not put on a key.
]]
end

body = body .. [[

The standard emoji set ships with the firmware. An extended set (skin tones and many more sequences) can be downloaded over WiFi from Settings > Emoji - it lives on the SD card.]]

return {
    title   = "Emoji",
    section = "Guide",
    order   = 60,
    body    = body,
}
