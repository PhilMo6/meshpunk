-- Settings > Emoji — customize the alt-key emoji layer.
-- Each hardware key (normal-layer char) maps to one emoji, typed with alt+key
-- in any text field. Tapping a key here opens a paged picker over the whole
-- emoji blob (singles AND sequence glyphs — sequences are stored as their PUA
-- codepoint; the firmware decomposes them to real Unicode on send).
local lvgl  = require("lvgl")
local apps  = require("lib/apps")
local nav   = require("lib/nav")
local theme = require("lib/theme")

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

content:Label { text = "Emoji Keys", w = lvgl.PCT(70), h = 26 }
local back_btn = content:Button { w = 50, h = 22 }
back_btn:Label { text = "Home", align = lvgl.ALIGN.CENTER }
back_btn:onClicked(function() apps.go_home() end)

local status = content:Label { text = "", w = lvgl.PCT(100), h = 16 }

-- Firmware without the emoji-layer bindings: show a hint instead of erroring.
if not (_kb_emoji_get and _kb_emoji_set and _emoji_blob_count and _emoji_blob_list) then
    content:Label {
        text = "This firmware build has no emoji\nlayer support - rebuild needed.",
        w = lvgl.PCT(100), h = 40,
    }
    return root
end

local BLOB_TOTAL = _emoji_blob_count()

-- Manual UTF-8 encode (the sandbox may not expose the utf8 lib). Handles the
-- 3-byte PUA range and 4-byte emoji planes.
local function ucp(cp)
    if cp < 0x80 then return string.char(cp) end
    if cp < 0x800 then
        return string.char(0xC0 + math.floor(cp / 0x40), 0x80 + cp % 0x40)
    end
    if cp < 0x10000 then
        return string.char(0xE0 + math.floor(cp / 0x1000),
                           0x80 + math.floor(cp / 0x40) % 0x40,
                           0x80 + cp % 0x40)
    end
    return string.char(0xF0 + math.floor(cp / 0x40000),
                       0x80 + math.floor(cp / 0x1000) % 0x40,
                       0x80 + math.floor(cp / 0x40) % 0x40,
                       0x80 + cp % 0x40)
end

local function key_face(k)
    local cp = _kb_emoji_get(k)
    if cp and cp > 0 then
        pcall(_emoji_preload, cp)
        return k .. " " .. ucp(cp)
    end
    return k .. " -"
end

content:Label {
    text = "Alt+key types the emoji. Tap a key to change it.",
    w = lvgl.PCT(100), h = 16,
}

-- ── Key grid (physical layout) ──────────────────────────────────────────────
-- Key buttons are DIRECT children of the nav container (gridnav invariant —
-- nesting them in row wrappers breaks trackball focus). A zero-height
-- full-width spacer after each row forces the flex wrap to break there.
local KEY_ROWS = { "qwertyuiop", "asdfghjkl", "zxcvbnm$" }
local key_lbls = {}          -- key char -> face label (for live refresh)
local open_picker          -- forward decl

for _, rowstr in ipairs(KEY_ROWS) do
    for i = 1, #rowstr do
        local k = rowstr:sub(i, i)
        local btn = content:Button { w = 29, h = 34 }
        local lbl = btn:Label { text = key_face(k), align = lvgl.ALIGN.CENTER }
        key_lbls[k] = lbl
        btn:onClicked(function() open_picker(k) end)
    end
    local brk = content:Object { w = lvgl.PCT(100), h = 0, border_width = 0, pad_all = 0, bg_opa = 0 }
    brk:clear_flag(lvgl.FLAG.SCROLLABLE)
    brk:clear_flag(lvgl.FLAG.CLICKABLE)
end

-- ── Reset ───────────────────────────────────────────────────────────────────
local reset_btn = content:Button { w = lvgl.PCT(100), h = 28 }
reset_btn:Label { text = "Reset all keys to defaults", align = lvgl.ALIGN.CENTER }
reset_btn:onClicked(function()
    if _kb_emoji_reset then _kb_emoji_reset() end
    for k, lbl in pairs(key_lbls) do
        lbl:set { text = key_face(k) }
    end
    status.text = "Emoji keys reset to defaults"
end)

-- ── Extended set (SD) ───────────────────────────────────────────────────────
-- The shipped L:/emojis.bin holds the standard single-emoji set. The full set
-- (skin tones + ZWJ sequences, ~3.7MB) doesn't fit LittleFS, so it lives at
-- S:/meshpunk/emojis.bin (preferred by the firmware when present) and is
-- fetched over WiFi from the repo.
local fileman = require("lib/fileman")

local EXT_URL  = "https://raw.githubusercontent.com/PhilMo6/meshpunk/launcher/tools/emojis_ext.bin"
local EXT_PATH = "S:/meshpunk/emojis.bin"

content:Label { text = "-- Extended set --", w = lvgl.PCT(100), h = 16 }
local ext_lbl = content:Label { text = "", w = lvgl.PCT(100), h = 16 }

local function refresh_ext()
    local kind = fileman.exists(EXT_PATH) and "extended (SD)" or "standard"
    ext_lbl.text = "Active: " .. kind .. ", " .. BLOB_TOTAL .. " emoji"
end
refresh_ext()

local function refresh_faces()
    for k, lbl in pairs(key_lbls) do
        lbl:set { text = key_face(k) }
    end
end

-- Parse the 16-byte EMJB header; returns the expected total file size, or
-- nil + reason. Never reads the body (fileman.read would pull 3.7MB into Lua).
local function blob_expected_size(path)
    local f = io.open(path, "r")
    if not f then return nil, "open failed" end
    local hdr = f:read(16)
    f:close()
    if not hdr or #hdr < 16 then return nil, "short header" end
    if hdr:sub(1, 4) ~= "EMJB" then return nil, "bad magic" end
    local function u16(o) return hdr:byte(o) + hdr:byte(o + 1) * 0x100 end
    local function u32(o)
        return hdr:byte(o) + hdr:byte(o + 1) * 0x100
             + hdr:byte(o + 2) * 0x10000 + hdr:byte(o + 3) * 0x1000000
    end
    local px, cnt, seqs = u16(7), u32(9), u32(13)
    if px == 0 or cnt == 0 then return nil, "empty blob" end
    return 16 + cnt * 4 + cnt * px * px * 4 + seqs * 48
end

local downloading = false

-- Blocking: fetch to a .part, validate the EMJB header + exact size, then swap
-- it in and reload the font. Returns a result message (newlines allowed) for
-- the popup — never touches status/downloading/UI itself.
local function do_download()
    local part = EXT_PATH .. ".part"

    local r = _wifi_download_file(EXT_URL, part)
    pcall(_wifi_download_end)   -- drop the socket, free the TLS buffers
    if not r or not r.success then
        pcall(fileman.remove, part)
        return "Download failed:\n" .. tostring(r and r.error or "no result")
    end

    local expect, herr = blob_expected_size(part)
    if not expect then
        pcall(fileman.remove, part)
        return "Bad blob: " .. tostring(herr)
    end
    local st = fileman.stat(part)
    if not st or st.size ~= expect then
        pcall(fileman.remove, part)
        return "Bad blob: size mismatch"
    end

    -- Release the open blob before replacing the file it may point at, then
    -- re-init (picks up the new SD blob).
    _emoji_font_reload(true)
    if fileman.exists(EXT_PATH) then fileman.remove(EXT_PATH) end
    local ok, err = fileman.rename(part, EXT_PATH)
    BLOB_TOTAL = _emoji_font_reload()
    refresh_faces()
    refresh_ext()
    if not ok then
        pcall(fileman.remove, part)
        return "Rename failed:\n" .. tostring(err)
    end
    return "Extended set active:\n" .. BLOB_TOTAL .. " emoji"
end

local dl_btn = content:Button { w = lvgl.PCT(100), h = 28 }
dl_btn:Label { text = "Download extended set (3.7MB, WiFi)", align = lvgl.ALIGN.CENTER }
dl_btn:onClicked(function()
    if downloading then return end
    if not _wifi_download_file then
        status.text = "Firmware has no download support"
        return
    end
    if not fileman.exists("S:/meshpunk") then
        status.text = "SD card not available"
        return
    end
    downloading = true

    -- Modal progress popup. The fetch is a synchronous blocking call that
    -- freezes the whole UI, so the popup must PAINT before it starts: create
    -- it now, defer the download one timer tick, then swap the popup to a
    -- result dialog with a Close button. Same overlay/nav pattern as the picker.
    local overlay = root:Object {
        w = W, h = H, x = 0, y = 0,
        bg_color = "#000000", bg_opa = 160, border_width = 0, pad_all = 0,
    }
    overlay:clear_flag(lvgl.FLAG.SCROLLABLE)
    overlay:add_flag(lvgl.FLAG.CLICKABLE)   -- modal (also swallows taps mid-freeze)
    local box = overlay:Object {
        w = W - 40, h = lvgl.SIZE_CONTENT, align = lvgl.ALIGN.CENTER,
        bg_color = "#333333", radius = 6,
        border_width = 1, border_color = "#555555", pad_all = 10, pad_row = 6,
        flex = { flex_direction = "column", flex_wrap = "nowrap" },
    }
    local msg = box:Label {
        text = "Downloading extended set...\nThe screen freezes until it\nfinishes (~3.7MB over WiFi).",
        w = lvgl.PCT(100),
    }

    apps.add_timer { period = 60, cb = function(t)
        t:delete()
        local result = do_download()
        downloading = false
        status.text = (result:gsub("\n", " "))
        msg:set { text = result }
        local close_b = box:Button { w = lvgl.PCT(100), h = 28 }
        close_b:Label { text = "Close", align = lvgl.ALIGN.CENTER }
        close_b:onClicked(function()
            nav.pop()
            overlay:delete()
        end)
        -- Push now that there's a focusable child, so trackball focus lands on
        -- Close (the page below stays suspended until nav.pop above).
        nav.push(box)
    end }
end)

local rm_btn = content:Button { w = lvgl.PCT(100), h = 28 }
rm_btn:Label { text = "Remove extended set", align = lvgl.ALIGN.CENTER }
rm_btn:onClicked(function()
    if downloading then return end
    if not fileman.exists(EXT_PATH) then
        status.text = "No extended set on SD"
        return
    end
    _emoji_font_reload(true)              -- close the open SD blob first
    local ok, err = fileman.remove(EXT_PATH)
    BLOB_TOTAL = _emoji_font_reload()     -- falls back to the standard L: set
    refresh_faces()
    refresh_ext()
    status.text = ok and ("Standard set active: " .. BLOB_TOTAL .. " emoji")
                     or ("Remove failed: " .. tostring(err))
end)

-- ── Picker popup ────────────────────────────────────────────────────────────
-- Modal overlay (CLICKABLE blocks the page below); box is the nav scope and
-- every focusable is its DIRECT child (gridnav invariant). nav.pop() runs
-- BEFORE overlay:delete() — see the Messenger popups for the same pattern.
local PAGE = 24

open_picker = function(key)
    if BLOB_TOTAL == 0 then
        status.text = "Emoji blob unavailable"
        return
    end

    local overlay = root:Object {
        w = W, h = H, x = 0, y = 0,
        bg_color = "#000000", bg_opa = 128, border_width = 0, pad_all = 0,
    }
    overlay:clear_flag(lvgl.FLAG.SCROLLABLE)
    overlay:add_flag(lvgl.FLAG.CLICKABLE)

    local box = overlay:Object {
        w = W - 20, h = lvgl.SIZE_CONTENT, align = lvgl.ALIGN.CENTER,
        bg_color = "#333333", radius = 6,
        border_width = 1, border_color = "#555555", pad_all = 6, pad_row = 2,
        flex = { flex_direction = "row", flex_wrap = "wrap" },
    }
    nav.push(box)

    local function close()
        nav.pop()
        overlay:delete()
    end

    box:Label { text = "Emoji for [" .. key .. "]", w = lvgl.PCT(70), h = 18 }
    local page_lbl = box:Label { text = "", w = lvgl.PCT(28), h = 18 }

    -- Fixed pool of PAGE cells, created once; paging only rewrites labels and
    -- the cps[] slots (handlers registered once — never re-bound per page).
    local start = 1
    local cps = {}
    local cells = {}
    for i = 1, PAGE do
        local b = box:Button { w = 42, h = 30 }
        local l = b:Label { text = "", align = lvgl.ALIGN.CENTER }
        cells[i] = { btn = b, lbl = l }
        b:onClicked(function()
            local cp = cps[i]
            if not cp then return end
            _kb_emoji_set(key, cp)
            local kl = key_lbls[key]
            if kl then kl:set { text = key_face(key) } end
            status.text = "[" .. key .. "] = " .. ucp(cp)
            close()
        end)
    end

    local function show_page()
        local list = _emoji_blob_list(start, PAGE)
        for i = 1, PAGE do
            local cp = list[i]
            cps[i] = cp
            if cp then
                pcall(_emoji_preload, cp)
                cells[i].lbl:set { text = ucp(cp) }
                cells[i].btn:clear_flag(lvgl.FLAG.HIDDEN)
            else
                cells[i].btn:add_flag(lvgl.FLAG.HIDDEN)
            end
        end
        local last = math.min(start + PAGE - 1, BLOB_TOTAL)
        page_lbl:set { text = start .. "-" .. last .. "/" .. BLOB_TOTAL }
    end

    local prev_b = box:Button { w = lvgl.PCT(23), h = 26 }
    prev_b:Label { text = "< Prev", align = lvgl.ALIGN.CENTER }
    prev_b:onClicked(function()
        start = math.max(1, start - PAGE)
        show_page()
    end)

    local next_b = box:Button { w = lvgl.PCT(23), h = 26 }
    next_b:Label { text = "Next >", align = lvgl.ALIGN.CENTER }
    next_b:onClicked(function()
        if start + PAGE <= BLOB_TOTAL then start = start + PAGE end
        show_page()
    end)

    local clear_b = box:Button { w = lvgl.PCT(23), h = 26 }
    clear_b:Label { text = "None", align = lvgl.ALIGN.CENTER }
    clear_b:onClicked(function()
        _kb_emoji_set(key, 0)
        local kl = key_lbls[key]
        if kl then kl:set { text = key_face(key) } end
        status.text = "[" .. key .. "] cleared"
        close()
    end)

    local cancel_b = box:Button { w = lvgl.PCT(23), h = 26 }
    cancel_b:Label { text = "Cancel", align = lvgl.ALIGN.CENTER }
    cancel_b:onClicked(close)

    show_page()
end

return root
