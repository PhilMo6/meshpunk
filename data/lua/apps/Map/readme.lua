-- Title comes from the app's own name in the registry, so this returns body
-- only. caps decides whether the key list is worth printing.
local caps = ...

local body = [[
The Map app shows OpenStreetMap tiles with mesh contact positions overlaid. Tiles download over WiFi and are cached on the SD card for offline use.

Drag to pan. The on-screen buttons cover zoom and the map menu, and a long-press on a contact marker opens its details - name, type, distance, hop count, last seen.

PRE-CACHE DOWNLOADS
In the map settings you can bulk-download tiles for offline use - choose an area size and zoom range, then download. Tiles are written atomically, so an interrupted download won't leave corrupt files.

MESHPRINT
With enough mesh data you can run a meshprint on a message sender to capture the first and second hop repeaters and triangulate the sender's general location. The more data you have, the better the results.]]

local keys = [[

KEYS
h - center on home (own GPS position)
q - quit (closes a popup first if open)
o or + - zoom in
i or - - zoom out
Space - stop scrolling
Enter - select contact at center / stop scrolling
c - cycle archived-contact pages]]

if caps.keyboard then
    body = body .. keys
    if caps.trackball then
        body = body .. [[

Roll the trackball to pan, and press Enter to select the contact at the center.]]
    end
else
    body = body .. [[

With a USB keyboard attached through Tools > USB Host, these keys work too:]] .. keys .. [[

USB accessories need external power - see the USB accessories page.]]
end

return { body = body }
