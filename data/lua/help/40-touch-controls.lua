local caps = ...

-- The mode cycle is driven by the board's aux button where there is no
-- keyboard, and by Shift+Alt where there is one.
local trigger = caps.keyboard and "hold Shift and Alt together"
                              or  "press the IO button on the side of the device"

local body

if caps.keyboard then
    body = [[
Your device has a keyboard, so the on-screen keyboard and on-screen game pads start switched off. They are there if you want them - handy for playing a game one-handed, or for typing without opening the keyboard.

To turn them on, ]] .. trigger .. [[. That cycles through the modes: game pad, pad hidden, on-screen keyboard, off.

The Touch input row in Settings > Device does the same thing. Use that if your keyboard runs the legacy firmware - it cannot press two keys at once, so the Shift+Alt trigger does not work there. Set the mode to Pad before launching a game and the on-screen controls will be waiting.
]]
else
    body = [[
This device has no keyboard, so typing and game controls happen on screen. Both are built in - nothing to install.

On-screen keyboard: tap any text field and it opens. Type, then close it to commit. This is how you enter text anywhere, in apps and in native games alike.

On-screen game pad: native games (Doom, GameBoy, DOS and the rest) show a button layout over the game. The buttons are outline-and-label only, so you can still see the game behind them.

To switch between them, ]] .. trigger .. [[. That cycles through the modes: game pad, pad hidden, on-screen keyboard, off. Use it when a game needs typing rather than a pad, or when you want the screen clear.
]]
end

body = body .. [[

Bigger keys: the keyboard key in the bottom left corner of the on-screen keyboard swaps between the normal layout and a big-key one. The big layout drops the keys that do not type so the letters get the room, and it shows the key you are pressing above your finger. Choose capitals or symbols in the normal layout first - the big layout comes up in whichever one you were in. In big symbol mode the 1# key flips between two pages of symbols. The check key is what closes the keyboard and keeps your text.]]

body = body .. [[

Quitting a native game: hold the on-screen QUIT button for about a second.]]

if caps.keyboard then
    body = body .. [[ Holding Alt and Backspace for about 1.5 seconds does the same thing.]]
else
    body = body .. [[ Game launchers also let you bind quit to a key, which is what to use if you have a USB keyboard attached.]]
end

body = body .. [[

Editing the pad layout: each game launcher has a Touch button that opens a layout editor. Drag a button to move it, use the size steppers to resize it, and the nudge arrows for fine positioning. Your layout is saved per game, so every game can have its own. Reset puts the original layout back.]]

body = body .. [[

Screenshots from a game: every layout carries a SHOT pad, switched off to begin with so it costs no screen space in games you never capture. Turn it on in that same editor - select it, press On, then drag it clear of the buttons you actually play with. Each launcher's Controls screen also has a Screenshot action you can bind to a key, unbound to begin with so it takes no key away from the game. Either one writes a PNG to the SD card; the Screenshot app in Tools does the same for everything outside a game.]]

return {
    title   = "Touch controls",
    section = "Guide",
    order   = 40,
    body    = body,
}
