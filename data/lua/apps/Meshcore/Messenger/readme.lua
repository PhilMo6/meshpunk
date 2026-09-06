local caps = ...

local body = [[
Full MeshCore support: direct messages, channels, and contacts.

Room servers and repeaters: log in, sync messages, and run admin commands right from the Messenger app.

Long-press the message input to open the clipboard menu - that is where you paste copied contact cards and text.

Tip: before your first messages, set the radio to your local defaults (Meshcore > Radio) and review RX boost, Contact Overwrite, and Message Repeat.]]

if caps.keyboard then
    body = body .. [[

Enter sends the message. q backs out of message selection and of any scroll area the focus has moved into.]]
else
    body = body .. [[

Tap the message field to open the on-screen keyboard, type, and send from there.]]
end

return { body = body }
