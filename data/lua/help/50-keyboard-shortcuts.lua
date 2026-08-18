-- The shortcut list is accurate on every device: on one without a built-in
-- keyboard these still work through a USB keyboard, so the page is not hidden
-- there. Only the lead paragraph differs, and a device that has a keyboard
-- needs no explanation at all.
local caps = ...

local body = [[
- Mic key: global notifications shortcut. Over a running app it peeks the top bar; on the launcher (or while peeked) it toggles the notification drop-down. Sym+Mic still types 0.

- Alt + letter (while typing): emoji layer - each letter key types its assigned emoji into the text field. Assign emojis per key in Settings > Emoji. An optional tap-to-latch mode for Alt (Settings > Device > Keyboard) keeps the layer on between taps.

- Alt + Mic (while typing): emoji search - opens a popup over the whole emoji set. Page through it, or jump by hex codepoint (e.g. 1F600 for smileys). Tapping an emoji inserts it into the text field you were typing in; the popup stays open for multiple inserts until Close (or Alt+Mic again).

- Sym (tap-to-latch): with the optional latch mode (Settings > Device > Keyboard), a clean tap of Sym latches the symbol layer until the next tap; holding Sym while typing stays momentary. WASD navigation pauses while latched - tap Sym again to resume.

- Alt + Backspace (hold ~1.5s): quit to home - closes the current app and returns to the launcher home page. The same chord quits a running native game (Doom, GameBoy, PICO-8, DOS).

- q: backs out of selection modes - message selection in a chat, row-select lists, and the Map app.

- Enter (in a chat): sends the message. Long-press the message input for the clipboard menu.]]

if not caps.keyboard then
    body = [[
This device has no built-in keyboard, so these apply to a USB keyboard attached through Tools > USB Host - which needs external power, as the USB accessories page explains. For everyday typing and game controls without one, see the Touch controls page.

]] .. body
end

return {
    title   = "Keyboard shortcuts",
    section = "Guide",
    order   = 50,
    body    = body,
}
