--[[
  Inter-app clipboard.

  A singleton via the require() cache, so any app can copy/paste plain text
  between sessions without an OS clipboard. Loaded once at boot (main.lua) and
  shared: an app copies with clipboard.copy(text), another pastes with
  clipboard.paste().

  Usage:
    local clipboard = require("lib/clipboard")
    clipboard.copy("hello")        -- store text
    if clipboard.has() then        -- something to paste?
        field.text = clipboard.paste()
    end
]]

local M = { _text = "" }

-- Store text on the clipboard (nil/non-string coerced to a string).
function M.copy(text)
    M._text = tostring(text or "")
end

-- Return the current clipboard text ("" if nothing has been copied).
function M.paste()
    return M._text or ""
end

-- True if there is something to paste.
function M.has()
    return M._text ~= nil and M._text ~= ""
end

-- Empty the clipboard.
function M.clear()
    M._text = ""
end

return M
