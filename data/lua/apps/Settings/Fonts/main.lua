-- Settings > Fonts — the user's default fonts for the two font roles.
-- "UI" is the interface font everything inherits; "Text" is the reading font
-- used by objects that opted in (Messenger chat bubbles). Per role the
-- resolution is: active theme's font > this default > bundled Noto Sans.
-- Candidates are .ttf files (glyf outlines only — no CFF .otf) from L:/fonts
-- and S:/meshpunk/fonts; drop files on the SD card to add choices.
local lvgl    = require("lvgl")
local apps    = require("lib/apps")
local nav     = require("lib/nav")
local theme   = require("lib/theme")
local fileman = require("lib/fileman")

local root = apps.new_root()
root:set { w = lvgl.HOR_RES(), h = lvgl.VER_RES(), pad_all = 0, border_width = 0, bg_opa = 0 }
root:clear_flag(lvgl.FLAG.SCROLLABLE)

theme.show_background()

local W = lvgl.HOR_RES()
local H = lvgl.VER_RES()

local content = root:Object {
    flex = { flex_direction = "row", flex_wrap = "wrap" },
    w = W, h = H,
    border_width = 0, pad_all = 6, bg_opa = 0,
}
nav.replace(content, { flags = nav.ROLLOVER + nav.SCROLL_FIRST })

content:Label { text = "Fonts", w = lvgl.PCT(70), h = 26 }
local back_btn = content:Button { w = 50, h = 22 }
back_btn:Label { text = "Home", align = lvgl.ALIGN.CENTER }
back_btn:onClicked(function() apps.go_home() end)

local status = content:Label { text = "", w = lvgl.PCT(100), h = 16 }

if not (_font_default_get and _font_default_set) then
    content:Label {
        text = "This firmware build has no runtime\nfont support - rebuild needed.",
        w = lvgl.PCT(100), h = 40,
    }
    return root
end

content:Label {
    text = "A theme's own font overrides these while active.",
    w = lvgl.PCT(100), h = 16,
}

local FONT_DIRS = { "L:/fonts", "S:/meshpunk/fonts" }

local function role_current(role)
    local ok, p = pcall(_font_default_get, role)
    if ok and type(p) == "string" and p ~= "" then
        return fileman.basename(p)
    end
    return "Default (Noto Sans)"
end

-- Collect candidate .ttf files across the font dirs -> { {label, path}, ... }
-- The bundled Noto Sans is skipped: the picker's "Default (Noto Sans)" entry
-- already IS that font (and resolves through the shared bundled slot).
local function collect_fonts()
    local out = {}
    for _, dir in ipairs(FONT_DIRS) do
        local entries = fileman.list(dir, {
            sizes = false,
            filter = function(e)
                return e.type == "file"
                    and e.name:lower():match("%.ttf$") ~= nil
                    and e.name ~= "NotoSans-Regular.ttf"
            end,
        })
        if entries then
            for _, e in ipairs(entries) do
                out[#out + 1] = { label = e.name, path = fileman.join(dir, e.name) }
            end
        end
    end
    return out
end

local open_picker   -- forward decl

-- One section per role: current-font row (tap to change) + a live preview
-- label rendered in that role's font (the chain heads are stable pointers,
-- so previews re-render automatically whenever the fonts change).
local function make_role_section(role, title, preview_text)
    content:Label { text = "-- " .. title .. " --", w = lvgl.PCT(100), h = 16 }
    local btn = content:Button { w = lvgl.PCT(100), h = 30 }
    local lbl = btn:Label { text = role_current(role), align = lvgl.ALIGN.LEFT_MID }
    btn:onClicked(function() open_picker(role, lbl) end)

    local prev = content:Label { text = preview_text, w = lvgl.PCT(100), h = 22 }
    -- Size 16 selects the role's chain head (Font defaults to 12 otherwise).
    local okf, f = pcall(lvgl.Font, role, 16)
    if okf and f then prev:set { text_font = f } end
end

make_role_section("ui",   "UI font",   "Interface chrome 123 \xC3\xA9\xD0\xB6 \xF0\x9F\x98\x80")
make_role_section("text", "Text font", "Chat message text 123 \xC3\xA9\xD0\xB6 \xF0\x9F\x98\x80")

-- ── Picker popup ─────────────────────────────────────────────────────────────
-- Messenger-popup pattern: CLICKABLE overlay, focusables DIRECT children of
-- the nav scope, nav.pop() BEFORE overlay:delete().
open_picker = function(role, row_lbl)
    local fonts = collect_fonts()

    local overlay = root:Object {
        w = W, h = H, x = 0, y = 0,
        bg_color = "#000000", bg_opa = 128, border_width = 0, pad_all = 0,
    }
    overlay:clear_flag(lvgl.FLAG.SCROLLABLE)
    overlay:add_flag(lvgl.FLAG.CLICKABLE)

    local box = overlay:Object {
        w = W - 40, h = lvgl.SIZE_CONTENT, align = lvgl.ALIGN.CENTER,
        bg_color = "#333333", radius = 6,
        border_width = 1, border_color = "#555555", pad_all = 8, pad_row = 3,
        flex = { flex_direction = "column", flex_wrap = "nowrap" },
    }
    nav.push(box)

    local function close()
        nav.pop()
        overlay:delete()
    end

    box:Label { text = "Pick " .. role .. " font", w = lvgl.PCT(100), h = 18 }

    local function choose(path, shown)
        local okc, r = pcall(_font_default_set, role, path)
        if okc and r then
            row_lbl:set { text = role_current(role) }
            status.text = role .. " font: " .. shown
        else
            status.text = "Font failed to load: " .. shown
        end
        close()
    end

    local def_b = box:Button { w = lvgl.PCT(100), h = 28 }
    def_b:Label { text = "Default (Noto Sans)", align = lvgl.ALIGN.LEFT_MID }
    def_b:onClicked(function() choose("", "Default (Noto Sans)") end)

    for _, f in ipairs(fonts) do
        local b = box:Button { w = lvgl.PCT(100), h = 28 }
        b:Label { text = f.label, align = lvgl.ALIGN.LEFT_MID }
        local path, label = f.path, f.label
        b:onClicked(function() choose(path, label) end)
    end

    if #fonts == 0 then
        box:Label {
            text = "No .ttf files found.\nDrop fonts in S:/meshpunk/fonts",
            w = lvgl.PCT(100), h = 34,
        }
    end

    local cancel_b = box:Button { w = lvgl.PCT(100), h = 26 }
    cancel_b:Label { text = "Cancel", align = lvgl.ALIGN.CENTER }
    cancel_b:onClicked(close)
end

return root
