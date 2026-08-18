local caps = ...

local body

if caps.trackball then
    body = [[
Roll the trackball to move the highlight between things on screen, and click it to select what is highlighted.

W, A, S and D do the same thing - up, left, down, right - so you can navigate without reaching for the trackball. When you are typing in a text field they type letters instead.

You can also just tap the screen. Tapping acts on what you touch directly, with no highlight involved.

Trackball and WASD share one speed setting: Settings > Device > Trackball sets the shortest time between accepted direction inputs, from 0 to 500 ms. Turn it up if a single roll jumps too far.]]
else
    body = [[
Tap what you want. Tap once to open something, and drag to scroll a long list or page.

That covers everyday use. For entering text and for game controls, see the Touch controls page.

If you would rather use keys, you can add them: a USB keyboard, mouse or gamepad plugged in through Tools > USB Host. A keyboard types and its keys work as shortcuts, a mouse moves a highlight and clicks to select, and a gamepad plays the emulators. USB accessories need external power - see the USB accessories page before you buy one.]]
end

return {
    title   = "Navigation",
    section = "Guide",
    order   = 30,
    body    = body,
}
