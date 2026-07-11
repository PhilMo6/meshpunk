-- ══════════════════════════════════════════════════════════════════
-- MeshCore Messenger — full-featured multi-view messenger
-- Views: inbox (merged channels + DMs), chat (bubbles), contacts
--        (search/sort/import), channels, contact detail, new DM, my node.
-- ══════════════════════════════════════════════════════════════════

local lvgl = require("lvgl")
local messages = require("lib/mesh/messages")
local utils = require("lib/utils")
local gridnav_body = require("lib/gridnav_body")
local apps = require("lib/apps")
local nav = require("lib/nav")
local theme = require("lib/theme")

-- Persistence lives on the C++ PunkMesh side (respects _storage: LittleFS
-- root or /meshpunk on SD), so any app can access the same message history.
-- The per-file cap is owned by the firmware (_max_messages, default 400) — we
-- deliberately don't shrink it here, so the full stored history is available.
-- Only SUMMARIES load here (one {count, last} entry per conversation, built by
-- a C-side scan): the inbox never needs more, and materializing every history
-- (~1.7MB on a busy mesh) overflowed the Lua arena into the shared PSRAM heap,
-- fragmenting the big region the Map/ELF apps need. A conversation's full
-- history loads when its chat opens (openThread) and is dropped on the way
-- out (clear_view -> closeThread), so at most one is ever resident.
messages:loadSummaries()

local W = lvgl.HOR_RES()
local H = lvgl.VER_RES()
local HEADER_H = 24

-- Focus group for trackball navigation
local group = lvgl.group.get_default()

-- Root container
local root = apps.new_root()
root:set { w = W, h = H, pad_all = 0, border_width = 0, bg_opa = 0 }
root:clear_flag(lvgl.FLAG.SCROLLABLE)

-- Themed wallpaper behind the messenger (header, view bodies and scroll lists are
-- transparent; chat bubbles and list rows stay opaque). Lightweight app, so the
-- background's PSRAM is fine here.
theme.show_background()

-- ── Theme ───────────────────────────────────────────────────────
local COL_ME_BG       = "#0b3d2e"   -- own message bubble
local COL_ME_TX       = "#d7f5e6"
local COL_THEM_BG     = "#262626"   -- received message bubble
local COL_THEM_TX     = "#f0f0f0"
local COL_META        = "#9aa0a6"   -- muted metadata
local COL_ACCENT      = "#7fb3ff"   -- unread / links
local COL_FOCUS       = "#ffffff"
local NAME_COLORS = {
    "#7fb3ff", "#ffb37f", "#a0e57f", "#e57fb3",
    "#7fe5e5", "#e5e57f", "#c79fff", "#ff9f9f",
}

-- Current view state
local current_view = nil   -- the lvgl object for the body area
local current_input = nil  -- the lvgl object for the input bar (if any)
local current_mode = "inbox"
local chat_target = nil
local contact_rows = {}    -- name -> LVGL button object

-- Contacts view filter/sort, persisted across re-entries this session.
local contacts_filter = ""
local contacts_sort = "recent"  -- recent | name | type | favorites
-- Contact-type visibility (the "exclude" filter), toggled in the settings popup.
local contacts_show_users = true       -- type 1 (companion)
local contacts_show_repeaters = true   -- type 2
local contacts_show_rooms = true       -- type 3
local contacts_show_sensors = true     -- type 4

-- Cached self name (to flag own messages). Refreshed lazily.
local my_name = nil
local function self_name()
    if my_name then return my_name end
    local ok, info = pcall(_mesh_get_node_info)
    if ok and info and info.name then my_name = info.name end
    return my_name or "me"
end

-- ── Header (always visible) ─────────────────────────────────────
local header = root:Object {
    w = W, h = HEADER_H, y = 0,
    border_width = 0, pad_left = 4, pad_right = 4, bg_opa = 0,
}
header:clear_flag(lvgl.FLAG.SCROLLABLE)

local header_title = header:Label { text = "Messenger", align = lvgl.ALIGN.LEFT_MID }
local header_right = header:Label { text = "", align = lvgl.ALIGN.RIGHT_MID, text_color = COL_META }

local function set_header(title, right_text)
    header_title.text = title or "Messenger"
    header_right.text = right_text or ""
end

-- ── Helpers ─────────────────────────────────────────────────────
local function clear_view()
    nav.reset()   -- drop every nav scope (any open popup's too) before the swap
    -- Drop the open chat's history bucket (the chat view is the only view that
    -- loads one, and every way out of it comes through here).
    messages:closeThread()
    -- current_view can hold hundreds of rows (contacts/inbox); a synchronous
    -- delete of that many objects starves Core 0 and trips the watchdog, so tear
    -- it down in the background. The input bar is always small — delete it inline.
    if current_view then apps.delete_view(current_view); current_view = nil end
    if current_input then current_input:delete(); current_input = nil end
end

local function truncate(str, max)
    if not str then return "" end
    if #str <= max then return str end
    return string.sub(str, 1, max - 2) .. ".."
end

local function name_color(name)
    if not name or name == "" then return "#cccccc" end
    local h = 0
    for i = 1, #name do h = (h * 31 + string.byte(name, i)) % 2147483647 end
    return NAME_COLORS[(h % #NAME_COLORS) + 1]
end

local function type_icon(t)
    if t == 2 then return "[R] " end   -- repeater
    if t == 3 then return "[#] " end   -- room server
    if t == 4 then return "[S] " end   -- sensor
    return ""                           -- companion
end

-- Honour the contact-type visibility toggles. Types outside the three filters
-- (e.g. sensors) are always shown so they can't silently vanish.
local function contact_type_visible(t)
    if t == 1 then return contacts_show_users end
    if t == 2 then return contacts_show_repeaters end
    if t == 3 then return contacts_show_rooms end
    if t == 4 then return contacts_show_sensors end
    return true
end

-- Scroll-aware row taps live in lib/nav (shared with the launcher and any other
-- app): nav.scroll_aware(list, on_settle) returns a binder(row, fn) that opens a
-- row on a tap but lets a drag scroll past it; on_settle runs from the list's
-- single SCROLL_END (used here for windowed paging).
local scroll_aware_list = nav.scroll_aware

-- Delivery word shown after the time on our own DM bubbles. `msg` is only
-- needed for the retry-ladder progress ("retry 2/5").
local function dm_status_text(status, msg)
    if status == "delivered" then return "delivered"
    elseif status == "failed" then return "failed"
    elseif status == "retrying" then
        if msg and msg.retry_n then
            return string.format("retry %d/%d", msg.retry_n, msg.retry_total or 0)
        end
        return "retrying"
    else return "sent" end
end

-- Shared inter-app clipboard: Copy on a message stores text, Paste in the Add
-- Contact field reads it back (also works across apps).
local clipboard = require("lib/clipboard")

-- application/x-www-form-urlencoded (space -> '+', others -> %XX) for query URIs.
local function url_encode(s)
    return (tostring(s or ""):gsub("[^%w%-_%.~]", function(c)
        if c == " " then return "+" end
        return string.format("%%%02X", string.byte(c))
    end))
end

-- The MeshCore app's contact share/QR/clipboard URI (NOT the firmware biz-card).
-- The firmware's importCard parses this same form back into a contact.
local function contact_uri(name, pubkey, ctype)
    return "meshcore://contact/add?name=" .. url_encode(name)
        .. "&public_key=" .. (pubkey or "")
        .. "&type=" .. tostring(ctype or 1)
end

-- ── Saved login passwords (rooms / repeaters) ────────────────────
-- One "pubkeyprefix=password" line per server. Keyed by the first 8 hex
-- chars of the pubkey so a rename doesn't lose the credential. Plain text
-- on flash — these are mesh guest/admin passwords, not secrets worth more.
-- Saved only after a login the server accepted; login itself stays manual.
--
-- Lives on the mesh-data filesystem: SD (under /meshpunk, next to
-- contacts/messages) when the card is the primary storage, else LittleFS.
-- Resolved per call — the user can switch storage at runtime in Settings.
local function logins_path()
    local ok, info = pcall(_storage_get_info)
    if ok and info and info.type == "SD" then
        return "S:/meshpunk/messenger_logins"   -- mirrors the firmware's SD prefix
    end
    return "L:/messenger_logins"
end

local function load_logins()
    local t = {}
    local f = io.open(logins_path(), "r")
    if not f then
        -- Continuity across a storage switch: the C-side migration moves only
        -- the mesh's own files, so fall back to reading the other drive.
        local other = logins_path():sub(1, 1) == "S"
            and "L:/messenger_logins" or "S:/meshpunk/messenger_logins"
        f = io.open(other, "r")
    end
    if not f then return t end
    -- NOTE: this firmware's io handles have no :lines() — whole-file read
    -- + gmatch, same as every other app's prefs loader.
    local txt = f:read("*a") or ""
    f:close()
    for k, v in string.gmatch(txt, "(%x+)=([^\r\n]*)") do
        t[k] = v
    end
    return t
end

local function save_logins(t)
    local f = io.open(logins_path(), "w")
    if not f then return end
    for k, v in pairs(t) do
        if v and v ~= "" then f:write(k .. "=" .. v .. "\n") end
    end
    f:close()
end

local function contact_login_key(name)
    local ok, raw = pcall(_mesh_get_contacts)
    if ok and type(raw) == "table" then
        for _, c in ipairs(raw) do
            if c.name == name and c.pubkey and #c.pubkey >= 8 then
                return string.sub(c.pubkey, 1, 8)
            end
        end
    end
    return nil
end

local function get_saved_password(name)
    local key = contact_login_key(name)
    if not key then return nil end
    return load_logins()[key]
end

local function set_saved_password(name, pw)
    local key = contact_login_key(name)
    if not key then return end
    local t = load_logins()
    t[key] = pw
    save_logins(t)
end

-- ── Login flow (rooms / repeaters) ───────────────────────────────
-- One login may be in flight per server: name → {password, on_status, timer}.
-- The single onLoginResult handler below routes the result back to whoever
-- asked (chat header or contact detail) and saves the accepted password.
local login_pending = {}

messages:onLoginResult(function(name, ok, perms, keepalive)
    local p = login_pending[name]
    login_pending[name] = nil
    if p and p.timer then pcall(function() p.timer:delete() end) end
    local status
    if ok then
        if p and p.password then set_saved_password(name, p.password) end
        status = (perms and perms > 0) and "Logged in (admin)" or "Logged in"
    else
        status = "Login failed"
    end
    if p and p.on_status then p.on_status(status, ok) end
end)

-- Password prompt → sendLogin. Result (or timeout) lands via on_status(text,
-- ok): ok == true/false for a server verdict, nil while still in flight.
local function show_login_popup(contact_name, on_status)
    local overlay = root:Object {
        w = W, h = H, x = 0, y = 0,
        bg_color = "#000000", bg_opa = 128, border_width = 0, pad_all = 0,
    }
    overlay:clear_flag(lvgl.FLAG.SCROLLABLE)
    overlay:add_flag(lvgl.FLAG.CLICKABLE)  -- modal
    local box = overlay:Object {
        w = W - 40, h = lvgl.SIZE_CONTENT, align = lvgl.ALIGN.CENTER,
        bg_color = "#333333", radius = 6,
        border_width = 1, border_color = "#555555", pad_all = 8,
        flex = { flex_direction = "column", flex_wrap = "nowrap" },
    }
    nav.push(box)

    box:Label { text = "Login: " .. utils.emojiText(contact_name), w = lvgl.PCT(100) }
    local pw_ta = box:Textarea {
        one_line = true,
        max_length = 15,   -- wire cap: sendLogin truncates at 15 chars
        w = lvgl.PCT(100), h = 30,
        text = get_saved_password(contact_name) or "",
    }
    box:Label { text = "Blank = guest login", text_color = COL_META, w = lvgl.PCT(100) }

    local row = box:Object {
        w = lvgl.PCT(100), h = lvgl.SIZE_CONTENT,
        bg_opa = 0, border_width = 0, pad_all = 0,
        flex = { flex_direction = "row", flex_wrap = "nowrap" },
    }

    local function close()
        nav.pop()
        overlay:delete()
    end

    -- Send the login and arm a timeout for the firmware's route estimate
    -- (+ slack). A DIRECT login that gets no response likely rode a dead
    -- learned path: drop the path and retry ONCE flooded (the response then
    -- re-teaches a fresh route). Only the flood attempt's silence is final.
    local function send_login(pw, is_flood_retry)
        local lok, route, est = messages:login(contact_name, pw)
        if not lok then
            if on_status then on_status("Login send failed", false) end
            return
        end
        local p = { password = pw, on_status = on_status }
        login_pending[contact_name] = p
        local timer
        timer = apps.add_timer {
            period = (tonumber(est) or 8000) + 2000,
            cb = function()
                if timer then timer:delete(); timer = nil end
                if login_pending[contact_name] ~= p then return end
                login_pending[contact_name] = nil
                if route == "direct" and not is_flood_retry then
                    pcall(_mesh_reset_path, contact_name)
                    send_login(pw, true)
                else
                    if on_status then on_status("No response", false) end
                end
            end,
        }
        p.timer = timer
        if on_status then
            on_status(is_flood_retry and "Retrying via flood.." or "Logging in..", nil)
        end
    end

    local function do_login()
        local pw = pw_ta.text or ""
        close()
        send_login(pw, false)
    end

    local login_btn = row:Button { w = lvgl.PCT(48), h = 26 }
    login_btn:Label { text = "Login", align = lvgl.ALIGN.CENTER }
    login_btn:onevent(lvgl.EVENT.RELEASED, do_login)

    local cancel_btn = row:Button { w = lvgl.PCT(48), h = 26 }
    cancel_btn:Label { text = "Cancel", align = lvgl.ALIGN.CENTER }
    cancel_btn:onevent(lvgl.EVENT.RELEASED, close)

    pw_ta:onevent(lvgl.EVENT.KEY, function()
        local indev = lvgl.indev.get_act()
        if indev:get_key() == lvgl.KEY.ENTER then do_login() end
    end)
end

-- Decoded GET_STATUS responses pop a modal (they arrive seconds after the
-- user taps "Req Status", wherever they are by then).
local function show_status_popup(name, text)
    local overlay = root:Object {
        w = W, h = H, x = 0, y = 0,
        bg_color = "#000000", bg_opa = 128, border_width = 0, pad_all = 0,
    }
    overlay:clear_flag(lvgl.FLAG.SCROLLABLE)
    overlay:add_flag(lvgl.FLAG.CLICKABLE)  -- modal
    local box = overlay:Object {
        w = W - 30, h = lvgl.SIZE_CONTENT, align = lvgl.ALIGN.CENTER,
        bg_color = "#333333", radius = 6,
        border_width = 1, border_color = "#555555", pad_all = 8,
        flex = { flex_direction = "column", flex_wrap = "nowrap" },
    }
    nav.push(box)
    box:Label { text = "-- Status: " .. utils.emojiText(name) .. " --", w = lvgl.PCT(100) }
    box:Label { text = text or "", w = lvgl.PCT(100) }
    local close_btn = box:Button { w = lvgl.PCT(100), h = 26 }
    close_btn:Label { text = "Close", align = lvgl.ALIGN.CENTER }
    close_btn:onevent(lvgl.EVENT.RELEASED, function()
        nav.pop()
        overlay:delete()
    end)
end

messages:onStatusText(function(name, text)
    show_status_popup(name, text)
end)

-- ── Server admin console popup (rooms + repeaters) ───────────────
-- Canned admin tasks + free-form CLI. Commands only take effect once logged
-- in with the server's ADMIN password (non-admin CLI is ignored). Replies
-- render inline while the popup is open (they also persist into the
-- server's thread like any CLI traffic).
local function show_server_admin(server_name)
    local overlay = root:Object {
        w = W, h = H, x = 0, y = 0,
        bg_color = "#000000", bg_opa = 128, border_width = 0, pad_all = 0,
    }
    overlay:clear_flag(lvgl.FLAG.SCROLLABLE)
    overlay:add_flag(lvgl.FLAG.CLICKABLE)  -- modal
    local box = overlay:Object {
        w = W - 20, h = lvgl.SIZE_CONTENT, align = lvgl.ALIGN.CENTER,
        bg_color = "#333333", radius = 6,
        border_width = 1, border_color = "#555555", pad_all = 8,
        flex = { flex_direction = "column", flex_wrap = "nowrap" },
    }
    nav.push(box)

    box:Label { text = "-- Admin: " .. utils.emojiText(server_name) .. " --", w = lvgl.PCT(100) }

    local reply_lbl = box:Label {
        text = "Needs admin login. Replies show here.",
        text_color = COL_META, w = lvgl.PCT(100),
    }

    -- Live CLI replies render inline. onCliResponse is a single slot and the
    -- repeater console (behind this popup) registers its own — chain to it
    -- so the console keeps rendering, and restore it on close.
    local prev_cli = messages.__onCliResponse
    messages:onCliResponse(function(msg)
        if prev_cli then pcall(prev_cli, msg) end
        if msg.from == server_name then
            pcall(function()
                reply_lbl.text = msg.text or ""
                reply_lbl:set { text_color = COL_ACCENT }
            end)
        end
    end)

    local function send_cli(cmd)
        if not cmd or #cmd == 0 then return end
        local okc = messages:sendCommand(server_name, cmd)
        pcall(function()
            reply_lbl.text = okc and ("> " .. cmd) or "Send failed"
            reply_lbl:set { text_color = COL_META }
        end)
    end

    -- Canned tasks. "Sync clock" sets the room's clock from THIS device's
    -- timestamp (CommonCLI "clock sync") — run it after every room power
    -- cycle, or post sync breaks (see the room clock/cursor saga).
    local row1 = box:Object {
        w = lvgl.PCT(100), h = lvgl.SIZE_CONTENT,
        bg_opa = 0, border_width = 0, pad_all = 0,
        flex = { flex_direction = "row", flex_wrap = "nowrap" },
    }
    local sync_btn = row1:Button { w = lvgl.PCT(48), h = 26 }
    sync_btn:Label { text = "Sync clock", align = lvgl.ALIGN.CENTER }
    sync_btn:onevent(lvgl.EVENT.RELEASED, function() send_cli("clock sync") end)

    local clock_btn = row1:Button { w = lvgl.PCT(48), h = 26 }
    clock_btn:Label { text = "Show clock", align = lvgl.ALIGN.CENTER }
    clock_btn:onevent(lvgl.EVENT.RELEASED, function() send_cli("clock") end)

    local row2 = box:Object {
        w = lvgl.PCT(100), h = lvgl.SIZE_CONTENT,
        bg_opa = 0, border_width = 0, pad_all = 0,
        flex = { flex_direction = "row", flex_wrap = "nowrap" },
    }
    local adv_btn = row2:Button { w = lvgl.PCT(48), h = 26 }
    adv_btn:Label { text = "Advert", align = lvgl.ALIGN.CENTER }
    adv_btn:onevent(lvgl.EVENT.RELEASED, function() send_cli("advert") end)

    local st_btn = row2:Button { w = lvgl.PCT(48), h = 26 }
    st_btn:Label { text = "Status", align = lvgl.ALIGN.CENTER }
    st_btn:onevent(lvgl.EVENT.RELEASED, function()
        -- Binary GET_STATUS (works for any perms); decoded result pops the
        -- status modal (rooms also get their Posted/Pushed counters).
        pcall(_mesh_send_request, server_name, 1)
        pcall(function() reply_lbl.text = "Status requested.." end)
    end)

    -- Free-form CLI ("get ...", "set ...", "password ...", "time <epoch>").
    local cmd_ta = box:Textarea {
        one_line = true, max_length = 140,
        w = lvgl.PCT(100), h = 30,
    }
    local function send_freeform()
        local cmd = cmd_ta.text
        if cmd and #cmd > 0 then
            send_cli(cmd)
            cmd_ta.text = ""
        end
    end
    cmd_ta:onevent(lvgl.EVENT.KEY, function()
        local indev = lvgl.indev.get_act()
        if indev:get_key() == lvgl.KEY.ENTER then send_freeform() end
    end)

    local row3 = box:Object {
        w = lvgl.PCT(100), h = lvgl.SIZE_CONTENT,
        bg_opa = 0, border_width = 0, pad_all = 0,
        flex = { flex_direction = "row", flex_wrap = "nowrap" },
    }
    local send_btn = row3:Button { w = lvgl.PCT(48), h = 26 }
    send_btn:Label { text = "Send", align = lvgl.ALIGN.CENTER }
    send_btn:onevent(lvgl.EVENT.RELEASED, send_freeform)

    local close_btn = row3:Button { w = lvgl.PCT(48), h = 26 }
    close_btn:Label { text = "Close", align = lvgl.ALIGN.CENTER }
    close_btn:onevent(lvgl.EVENT.RELEASED, function()
        messages:onCliResponse(prev_cli)   -- restore the console's listener
        nav.pop()
        overlay:delete()
    end)
end

-- Resolve a DM-thread name to the right chat target: rooms and repeaters
-- get their own chat types (login button; the repeater chat is a CLI
-- console). Falls back to a plain DM when the contact is unknown.
local function thread_target(name)
    local ok, raw = pcall(_mesh_get_contacts)
    if ok and type(raw) == "table" then
        for _, c in ipairs(raw) do
            if c.name == name then
                if c.type == 2 then return { type = "repeater", name = name } end
                if c.type == 3 then return { type = "room", name = name } end
                break
            end
        end
    end
    return { type = "dm", name = name }
end

-- Forward declarations
local show_inbox, show_chat, show_contacts, show_channels
local show_contact_detail, show_my_node, show_import_contact
local show_contact_settings, show_clear_confirm, show_flood_scope

-- ── Long-press popup showing message metadata ───────────────────
local function show_msg_info(msg, on_reply, on_dismiss)
    local overlay = root:Object {
        w = W, h = H, x = 0, y = 0,
        bg_color = "#000000", bg_opa = 128,
        border_width = 0, pad_all = 0,
    }
    overlay:clear_flag(lvgl.FLAG.SCROLLABLE)
    overlay:add_flag(lvgl.FLAG.CLICKABLE)  -- modal: swallow taps on the dim area

    local box = overlay:Object {
        w = W - 20, h = H - 20,
        align = lvgl.ALIGN.CENTER,
        bg_color = "#333333", radius = 6,
        border_width = 1, border_color = "#555555",
        pad_all = 8,
        flex = { flex_direction = "column", flex_wrap = "nowrap" },
    }
    nav.push(box)

    local function info_label(text)
        box:Label { text = text, w = lvgl.PCT(100) }
    end

    info_label("-- Message Info --")
    info_label("From: " .. utils.emojiText(msg.from or "?"))
    local ci_btn = box:Button { w = 100, h = 22 }
    ci_btn:Label { text = "Contact Info", align = lvgl.ALIGN.CENTER }
    ci_btn:onevent(lvgl.EVENT.RELEASED, function()
        nav.pop()
        overlay:delete()
        if on_dismiss then on_dismiss() end
        show_contact_detail(msg.from)
    end)
    info_label("Time: " .. (msg.timestamp and utils.clockDateTime(msg.timestamp) or "?"))
    info_label("Hops: " .. (msg.hops or "?"))
    info_label("SNR: " .. (msg.snr and string.format("%.1f dB", msg.snr) or "N/A"))
    info_label("RSSI: " .. (msg.rssi and string.format("%.0f dBm", msg.rssi) or "N/A"))
    info_label("Route: " .. (msg.direct and "Direct" or "Flood"))

    -- Message path button for all known routes
    local paths_btn = box:Button { w = lvgl.PCT(38), h = 22 }
    paths_btn:Label { text = "Paths", align = lvgl.ALIGN.CENTER }
    paths_btn:onevent(lvgl.EVENT.RELEASED, function()
        local overlay2 = root:Object {
            w = W, h = H, x = 0, y = 0,
            bg_color = "#000000", bg_opa = 128,
            border_width = 0, pad_all = 0,
        }
        overlay2:clear_flag(lvgl.FLAG.SCROLLABLE)
        overlay2:add_flag(lvgl.FLAG.CLICKABLE)  -- modal

        local box2 = overlay2:Object {
            w = W - 10, h = H - 20,
            align = lvgl.ALIGN.CENTER,
            bg_color = "#333333", radius = 6,
            border_width = 1, border_color = "#555555",
            pad_all = 6,
            flex = { flex_direction = "column", flex_wrap = "nowrap" },
        }
        nav.push(box2)

        box2:Label { text = "-- Message Paths --", w = lvgl.PCT(100) }

        -- Render the path list + Close button into box2. Called after the loading
        -- popup has painted, since the disk read blocks LVGL until it returns.
        local function render(paths)
            if not paths or #paths == 0 then
                if msg.path and #msg.path > 0 then
                    box2:Label { text = "1 path (first arrival only)", w = lvgl.PCT(100) }
                    local rc = msg.direct and "Direct" or table.concat(msg.path, " > ")
                    box2:Label {
                        text = string.format("#1 h:%d snr:%.0f rssi:%.0f %s",
                            msg.hops or 0, msg.snr or 0, msg.rssi or 0,
                            msg.direct and "DIRECT" or "FLOOD"),
                        w = lvgl.PCT(100),
                    }
                    box2:Label { text = "  " .. rc, w = lvgl.PCT(100) }
                else
                    box2:Label { text = "No paths observed", w = lvgl.PCT(100) }
                end
            else
                box2:Label { text = #paths .. " path(s) seen", w = lvgl.PCT(100) }
                for i, rec in ipairs(paths) do
                    local rc = "Direct"
                    if not rec.direct and rec.path and #rec.path > 0 then
                        rc = table.concat(rec.path, " > ")
                    elseif not rec.direct then
                        rc = "Flood (no path)"
                    end
                    box2:Label {
                        text = string.format("#%d h:%d snr:%.0f rssi:%.0f %s",
                            i, rec.hops or 0, rec.snr or 0, rec.rssi or 0,
                            rec.direct and "DIRECT" or "FLOOD"),
                        w = lvgl.PCT(100),
                    }
                    box2:Label { text = "  " .. rc, w = lvgl.PCT(100) }
                end
            end

            local close_btn2 = box2:Button { w = lvgl.PCT(100), h = 26 }
            close_btn2:Label { text = "Close", align = lvgl.ALIGN.CENTER }
            close_btn2:onevent(lvgl.EVENT.RELEASED, function()
                nav.pop()
                overlay2:delete()
            end)
        end

        -- The path lookup scans the conversation log on disk, which can lag on a
        -- large channel. Show the loading popup and defer the read one tick so the
        -- modal paints first (the C read blocks rendering until it returns).
        local step = 0
        utils.loadingPopUpAdd(overlay2, "paths", function()
            step = step + 1
            if step == 1 then return false end   -- let the popup paint
            local paths = nil
            if msg.hash then
                local peer = msg.peer or msg.to or msg.from
                local ch = msg.is_dm and -1 or (msg.channel_idx or 0)
                local ok2, result = pcall(_mesh_get_message_paths, msg.hash, ch, peer)
                if ok2 and result and #result > 0 then paths = result end
            end
            pcall(render, paths)
            return true
        end)
    end)

    -- Copy the message text to the in-app clipboard (e.g. a pasted contact card).
    local copy_btn = box:Button { w = 90, h = 26 }
    local copy_lbl = copy_btn:Label { text = "Copy Text", align = lvgl.ALIGN.CENTER }
    copy_btn:onevent(lvgl.EVENT.RELEASED, function()
        clipboard.copy(msg.text or "")
        copy_lbl.text = "Copied!"
    end)

    if on_reply then
        local reply_btn = box:Button { w = lvgl.PCT(48), h = 26 }
        reply_btn:Label { text = "Reply", align = lvgl.ALIGN.CENTER }
        reply_btn:onevent(lvgl.EVENT.RELEASED, function()
            nav.pop()
            overlay:delete()
            if on_dismiss then on_dismiss() end
            on_reply(msg)
        end)
    end

    local close_btn = box:Button { w = on_reply and lvgl.PCT(48) or lvgl.PCT(100), h = 26 }
    close_btn:Label { text = "Close", align = lvgl.ALIGN.CENTER }
    close_btn:onevent(lvgl.EVENT.RELEASED, function()
        nav.pop()
        overlay:delete()
        if on_dismiss then on_dismiss() end
    end)
end

-- ── Navigation help popup (trackball/keyboard 'q' to back out) ───
local function show_nav_help()
    local overlay = root:Object {
        w = W, h = H, x = 0, y = 0,
        bg_color = "#000000", bg_opa = 128,
        border_width = 0, pad_all = 0,
    }
    overlay:clear_flag(lvgl.FLAG.SCROLLABLE)
    overlay:add_flag(lvgl.FLAG.CLICKABLE)  -- modal: swallow taps on the dim area

    local box = overlay:Object {
        w = W - 30, h = lvgl.SIZE_CONTENT,
        align = lvgl.ALIGN.CENTER,
        bg_color = "#333333", radius = 6,
        border_width = 1, border_color = "#555555",
        pad_all = 10,
        flex = { flex_direction = "column", flex_wrap = "nowrap" },
    }
    nav.push(box)

    box:Label { text = "-- Navigation Help --", w = lvgl.PCT(100) }
    box:Label {
        text = "When navigating with the trackball or keyboard, press Q to " ..
               "back out of a selected list or message and return focus to " ..
               "the buttons.",
        w = lvgl.PCT(100),
    }

    local close_btn = box:Button { w = lvgl.PCT(100), h = 26 }
    close_btn:Label { text = "Got it", align = lvgl.ALIGN.CENTER }
    close_btn:onevent(lvgl.EVENT.RELEASED, function()
        nav.pop()
        overlay:delete()
    end)
end

-- ── Periodic header refresh ─────────────────────────────────────
apps.add_timer {
    period = 5000,
    cb = function(t)
        if current_mode == "inbox" or current_mode == "contacts" then
            header_right.text = "Contacts: " .. _mesh_get_num_contacts()
        end
    end
}

-- ── Conversation model (shared by inbox) ────────────────────────
-- Rows come from the conversation SUMMARIES (count + last message each), not
-- from loaded histories — the inbox never materializes a thread.
local function build_conversations()
    local convos = {}
    local have_ch0 = false
    local ok_ch, channels = pcall(_mesh_get_channels)
    if ok_ch and channels then
        for _, ch in ipairs(channels) do
            if ch.idx == 0 then have_ch0 = true end
            local sum = messages:getChannelSummary(ch.idx)
            local last = sum and sum.last or nil
            convos[#convos + 1] = {
                kind = "channel", idx = ch.idx, name = ch.name,
                last = last, ts = last and last.timestamp or 0,
                unread = messages:unreadInChannel(ch.idx),
                count = sum and sum.count or 0,
            }
        end
    end
    -- Unknown-channel traffic surfaces under Public (idx 0). When no channel
    -- occupies slot 0 (e.g. Public deleted), a live summary entry for it still
    -- gets a row so those messages aren't invisible.
    if not have_ch0 then
        local sum = messages:getChannelSummary(0)
        if sum and sum.last then
            convos[#convos + 1] = {
                kind = "channel", idx = 0, name = "Public",
                last = sum.last, ts = sum.last.timestamp or 0,
                unread = messages:unreadInChannel(0), count = sum.count or 0,
            }
        end
    end
    for _, t in ipairs(messages:getDMThreadNames()) do
        convos[#convos + 1] = {
            kind = "dm", name = t.name, last = t.last_msg,
            ts = t.last_msg and t.last_msg.timestamp or 0,
            unread = t.unread or 0, count = t.count,
        }
    end
    table.sort(convos, function(a, b) return (a.ts or 0) > (b.ts or 0) end)
    return convos
end

-- ── INBOX VIEW ──────────────────────────────────────────────────
show_inbox = function()
    clear_view()
    current_mode = "inbox"
    messages:onAck(nil)  -- ack updates only matter inside a chat view
    set_header("Messenger", "Contacts: " .. _mesh_get_num_contacts())

    -- Controls live in the gridnav body; conversation rows live in a separate
    -- CLICK_FOCUSABLE scroll list. On the touchscreen every press registers as
    -- a click, so (like the chat) the first tap on the list just "arms" it so
    -- you can drag-scroll, and a row only opens on a second tap/click — or a
    -- long-press, which opens immediately.
    local body = gridnav_body(root, HEADER_H, H - HEADER_H,
                              GRIDNAV_ROLLOVER + GRIDNAV_SCROLL_FIRST, true)
    current_view = body

    -- Control buttons (narrow, wrap into the top row).
    local function ctrl(label, w, cb)
        local b = body:Button { w = w, h = 24 }
        b:Label { text = label, align = lvgl.ALIGN.CENTER }
        b:onevent(lvgl.EVENT.RELEASED, cb)
        return b
    end

    ctrl("Exit", 50, function()
        messages:onMessage(nil)
        messages:onDirectMessage(nil)
        messages:onContactUpdate(nil)
        messages:onAck(nil)
        -- Drop everything this session loaded: the summaries and any open
        -- chat's history bucket. Rebuilt from disk on the next Messenger open.
        -- The unread badge is counter-based, so it's unaffected.
        messages:freePersisted()
        -- Also drop the cached 500-contact Lua table (~388KB, pinned in the
        -- registry by _mesh_get_contacts) — the Messenger is its other consumer.
        -- go_home's collectgarbage reclaims it; rebuilt on demand next open.
        pcall(_mesh_drop_contacts_cache)
        apps.go_home()
    end)
    ctrl("Channels", 72, function() show_channels() end)
    ctrl("Contacts", 70, function() show_contacts() end)
    ctrl("Node", 48, function() show_my_node() end)
    ctrl("?", 26, function() show_nav_help() end)

    -- Scrollable conversation list (rows live here, not in the gridnav body).
    local list = body:Object {
        w = lvgl.PCT(100), h = H - HEADER_H - 36,
        border_width = 0, pad_all = 0, bg_opa = 0,
        flex = { flex_direction = "column", flex_wrap = "nowrap" },
    }
    -- Tap/click the list to enter row-select (trackball steps the rows); 'q'
    -- returns focus to the controls. nav.list owns the scope push/pop.
    nav.list(list)

    local bind_click = scroll_aware_list(list)

    -- Conversation rows (full-width), keyed for live updates.
    local convoRows = {}

    local function row_key(c)
        return c.kind == "channel" and ("ch" .. c.idx) or ("@" .. c.name)
    end

    local function fill_row(row, c)
        row:clean()
        -- Channel names already carry their own '#'; only DMs get an '@' marker.
        local prefix = c.kind == "channel" and "" or "@"
        local preview = ""
        if c.last then
            -- last.text arrives composed from C; sender names do not (they
            -- round-trip as identity) — compose them for display only.
            preview = truncate(utils.emojiText(c.last.from or "") .. ": " .. (c.last.text or ""), 24)
        end
        local left = row:Label { align = lvgl.ALIGN.LEFT_MID }
        left.text = prefix .. utils.emojiText(c.name) .. (preview ~= "" and ("  " .. preview) or "")
        if c.unread and c.unread > 0 then left:set { text_color = COL_FOCUS } end
        local right = row:Label { align = lvgl.ALIGN.RIGHT_MID, text_color = COL_META }
        local rt = (c.ts and c.ts > 0) and utils.relTime(c.ts) or ""
        if c.unread and c.unread > 0 then
            right:set { text = "(" .. c.unread .. ") " .. rt, text_color = COL_ACCENT }
        else
            right:set { text = rt }
        end
    end

    local function open_convo(c)
        if c.kind == "channel" then
            show_chat { type = "channel", idx = c.idx, name = c.name }
        else
            -- Room / repeater threads live in the DM store; resolve the
            -- contact type so their chats get the login/CLI features.
            show_chat(thread_target(c.name))
        end
    end

    -- Register the tap handler ONCE per row; it reads the live conversation from
    -- the keyed entry so updates never need to re-bind (which would stack
    -- handlers and fire show_chat multiple times).
    local function new_row(key, c)
        local row = list:Button { w = lvgl.PCT(100), h = 28 }
        fill_row(row, c)
        convoRows[key] = { row = row, c = c }
        -- Tap/click to open; suppressed while the list is being scrolled.
        bind_click(row, function()
            local e = convoRows[key]
            if e then open_convo(e.c) end
        end)
        return row
    end

    local convos = build_conversations()
    for _, c in ipairs(convos) do new_row(row_key(c), c) end

    if #convos == 0 then
        list:Label {
            text = "No conversations yet.\nOpen Contacts or Channels to start.",
            w = lvgl.PCT(100), h = 40,
        }
    end

    -- Live updates: refresh a row's preview/unread and float it to the top.
    local function touch_row(key, c)
        local entry = convoRows[key]
        if entry then
            entry.c = c
            fill_row(entry.row, c)
            entry.row:move_to_index(0)
        else
            new_row(key, c):move_to_index(0)
        end
    end

    messages:onMessage(function(msg)
        if current_mode ~= "inbox" then return end
        if not current_view then return end
        local idx = msg.channel_idx or 0
        local cname = "Public"
        local ok_ch, channels = pcall(_mesh_get_channels)
        if ok_ch and channels then
            for _, ch in ipairs(channels) do
                if ch.idx == idx then cname = ch.name break end
            end
        end
        touch_row("ch" .. idx, {
            kind = "channel", idx = idx, name = cname,
            last = msg, ts = msg.timestamp, unread = messages:unreadInChannel(idx),
        })
    end)

    messages:onDirectMessage(function(msg)
        if current_mode ~= "inbox" then return end
        if not current_view then return end
        local thread_name = msg.to or msg.from
        touch_row("@" .. thread_name, {
            kind = "dm", name = thread_name, last = msg, ts = msg.timestamp,
            unread = messages:unreadInDM(thread_name),
        })
    end)

    messages:onRoomMessage(function(msg)
        if current_mode ~= "inbox" then return end
        if not current_view then return end
        -- Room posts thread under the ROOM's name (msg.from is the author).
        touch_row("@" .. msg.room, {
            kind = "dm", name = msg.room, last = msg, ts = msg.timestamp,
            unread = messages:unreadInDM(msg.room),
        })
    end)
end

-- ── CHAT VIEW ───────────────────────────────────────────────────
show_chat = function(target)
    clear_view()
    current_mode = "chat"
    chat_target = target

    -- Load THIS conversation's history from disk — the only bucket resident.
    -- clear_view -> closeThread drops it again on the way out of the chat.
    messages:openThread(target)

    -- Clear unread for this thread now that it's open. Rooms and repeaters
    -- share the DM store/counters, keyed by the server contact's name.
    if target.type == "channel" then
        messages:markChannelSeen(target.idx)
    else
        messages:markDMSeen(target.name)
    end

    local me = self_name()
    -- Channel names already include their '#'; only DMs get an '@' marker.
    local title = (target.type == "dm") and ("@" .. utils.emojiText(target.name))
                                          or utils.emojiText(target.name)
    set_header(title, "")

    local body = gridnav_body(root, HEADER_H, H - HEADER_H, GRIDNAV_ROLLOVER + GRIDNAV_SCROLL_FIRST, true)
    current_view = body

    -- Top buttons (narrow, wrap in first row)
    local back_btn = body:Button { w = 45, h = 20 }
    back_btn:Label { text = "Home", align = lvgl.ALIGN.CENTER }
    back_btn:onevent(lvgl.EVENT.RELEASED, function() show_inbox() end)

    -- Region / flood-scope settings. Channel chats also get that channel's
    -- per-channel override section; DMs/rooms/repeaters see only the global.
    local scope_btn = body:Button { w = 45, h = 20 }
    scope_btn:Label { text = "Rgn", align = lvgl.ALIGN.CENTER }

    -- Active-region indicator: which region this chat's floods go out under
    -- (per-channel override > phone-set runtime scope > global; blank = none).
    -- Shows "[phone]" for the runtime BLE scope — it's a raw key with no name.
    local rgn_lbl = body:Label { text = "", text_color = COL_META, h = 20 }
    local function refresh_rgn()
        local rname = ""
        if target.type == "channel" then
            local okc, cs = pcall(_mesh_get_channel_scope, target.name)
            if okc and cs and cs ~= "" then rname = cs end
        end
        if rname == "" then
            local okb, ble_on = pcall(_mesh_ble_scope_active)
            if okb and ble_on then rname = "phone" end
        end
        if rname == "" then
            local okg, gs = pcall(_mesh_get_flood_scope)
            if okg and gs and gs ~= "" then rname = gs end
        end
        rgn_lbl.text = (rname ~= "") and ("[" .. utils.emojiText(rname) .. "]") or ""
    end
    refresh_rgn()
    scope_btn:onevent(lvgl.EVENT.RELEASED, function()
        show_flood_scope(target.type == "channel" and target.name or nil, refresh_rgn)
    end)

    -- Info (contact detail — also the way to favorite a room/repeater so its
    -- contact record survives removal/eviction).
    if target.type ~= "channel" then
        local info_btn = body:Button { w = 45, h = 20 }
        info_btn:Label { text = "Info", align = lvgl.ALIGN.CENTER }
        info_btn:onevent(lvgl.EVENT.RELEASED, function() show_contact_detail(target.name) end)
    end

    -- Login/Logout (room server or repeater). The button reads Logout while a
    -- keep-alive session is live; legacy servers (no keep-alive) just re-login.
    if target.type == "room" or target.type == "repeater" then
        local login_btn = body:Button { w = 50, h = 20 }
        local login_lbl = login_btn:Label {
            text = messages:isConnected(target.name) and "Logout" or "Login",
            align = lvgl.ALIGN.CENTER,
        }
        login_btn:onevent(lvgl.EVENT.RELEASED, function()
            if messages:isConnected(target.name) then
                messages:logout(target.name)
                login_lbl.text = "Login"
                set_header(title, "Logged out")
            else
                show_login_popup(target.name, function(status, lok)
                    -- Only touch the header while this chat is still the view.
                    if current_mode == "chat" and chat_target and chat_target.name == target.name then
                        set_header(title, status)
                        if lok and messages:isConnected(target.name) then
                            login_lbl.text = "Logout"
                        end
                    end
                end)
            end
        end)

        -- Keep-alive expiry for THIS server: header note + button flips back.
        -- (Path is intentionally kept — re-login covers the dead-path case
        -- with its flood fallback.)
        messages:onConnectionLost(function(name)
            if name ~= target.name then return end
            if current_mode ~= "chat" or not chat_target or chat_target.name ~= target.name then return end
            pcall(function()
                set_header(title, "Connection lost")
                login_lbl.text = "Login"
            end)
        end)

        -- Admin console (clock sync, advert, status, free-form CLI) for
        -- rooms AND repeaters. Server-side it only obeys admins; the button
        -- is always offered. The repeater chat stays a plain console — this
        -- is the canned-tasks shortcut on top of it.
        local adm_btn = body:Button { w = 45, h = 20 }
        adm_btn:Label { text = "Adm", align = lvgl.ALIGN.CENTER }
        adm_btn:onevent(lvgl.EVENT.RELEASED, function()
            show_server_admin(target.name)
        end)
    end

    -- Message scroll area (full width).
    local MSG_H = H - HEADER_H - 20 - 34 - 24
    local msg_list = body:Object {
        w = lvgl.PCT(100), h = MSG_H,
        border_width = 0, pad_all = 2, bg_opa = 0,
        flex = { flex_direction = "column", flex_wrap = "nowrap" },
    }
    msg_list:add_flag(lvgl.FLAG.CLICK_FOCUSABLE)

    local in_msg_select = false
    local textArea
    local context_menu_open = false
    local ack_labels = {}  -- own-DM msg -> its header label, for live ack updates

    -- Scroll-aware taps (same fix as the inbox/contacts lists): a touch DRAG
    -- scrolls the chat instead of arming selection or opening a bubble menu.
    -- The windowed older-message paging is routed through on_settle, since
    -- luavgl allows only one SCROLL_END handler per object.
    local page_load_older  -- assigned below, once history/render_msg exist
    local bind_msg = scroll_aware_list(msg_list, function()
        if page_load_older then page_load_older() end
    end)

    bind_msg(msg_list, function()
        if context_menu_open then return end
        if in_msg_select then return end
        in_msg_select = true
        nav.push(msg_list, { preserve = true })
    end)

    msg_list:onevent(lvgl.EVENT.KEY, function()
        local indev = lvgl.indev.get_act()
        local key = indev:get_key()
        if key == 113 then -- 'q' exits message selection
            in_msg_select = false
            nav.pop()
        end
    end)

    -- Build one chat bubble (a focusable direct child of msg_list).
    local function render_msg(msg)
        local is_me = (msg.from == me)

        local bubble = msg_list:Object {
            w = lvgl.PCT(92), h = lvgl.SIZE_CONTENT,
            bg_color = is_me and COL_ME_BG or COL_THEM_BG,
            bg_opa = 255, radius = 6,
            border_width = 1,
            border_color = is_me and COL_ME_BG or COL_THEM_BG,
            pad_all = 4, pad_bottom = 5,
            flex = { flex_direction = "column", flex_wrap = "nowrap" },
        }
        bubble:clear_flag(lvgl.FLAG.SCROLLABLE)
        bubble:add_flag(lvgl.FLAG.CLICKABLE)
        bubble:add_flag(lvgl.FLAG.CLICK_FOCUSABLE)
        bubble:set_style({ border_color = COL_FOCUS }, lvgl.STATE.FOCUS_KEY)

        -- Header line: sender + time. Hop count and signal detail (SNR/RSSI)
        -- live in the long-press info popup, not the bubble.
        local hdr = is_me and "You" or utils.emojiText(msg.from or "?")
        local meta = utils.clockHM(msg.timestamp)
        -- Live sends carry msg.status (sent/delivered/failed); persisted/received
        -- messages don't, so they show no delivery word. Room posts are acked
        -- by the room server, so they track delivery like DMs.
        local track_status = is_me and (target.type == "dm" or target.type == "room")
                                   and msg.status ~= nil
        local head_text = hdr .. "  " .. meta
        if track_status then head_text = head_text .. "  " .. dm_status_text(msg.status, msg) end
        local head_lbl = bubble:Label {
            text = head_text,
            w = lvgl.PCT(100),
            text_color = is_me and COL_META or name_color(msg.from),
        }
        if track_status then ack_labels[msg] = head_lbl end

        local body_lbl = bubble:Label {
            text = msg.text or "",
            w = lvgl.PCT(100),
            text_color = is_me and COL_ME_TX or COL_THEM_TX,
        }

        local function open_msg_menu()
            if context_menu_open then return end
            context_menu_open = true
            show_msg_info(msg, function(m)
                textArea.text = "@[" .. (m.from or "?") .. "] "
            end, function()
                -- show_msg_info already nav.pop()'d back to this chat's scope
                -- (msg_list if we were row-selecting, else the body); just clear
                -- the menu guard and re-focus the bubble we acted on.
                context_menu_open = false
                if in_msg_select then nav.set_focused(bubble) end
            end)
        end

        bind_msg(bubble, function()
            if in_msg_select then open_msg_menu() end
        end)
        bubble:onevent(lvgl.EVENT.LONG_PRESSED, open_msg_menu)

        -- Repeat status for zero-hop sends we're echoing into the mesh.
        if msg.hash and msg.hops == 0 then
            local ok_rs, rs = pcall(_mesh_get_repeat_status, msg.hash)
            if ok_rs and rs == 1 then
                local rep_lbl = msg_list:Label {
                    text = "repeating...", text_color = COL_META,
                    w = lvgl.PCT(100), h = 14, pad_bottom = 2,
                }
                local poll_hash = msg.hash
                local timer
                -- Manager-tracked so exiting the app (go_home) tears it down
                -- immediately — an untracked one would otherwise tick once more
                -- after teardown (harmless, self-heals via the ok_poll guard
                -- below, but a wasted poll + error each session).
                timer = apps.add_timer {
                    period = 3000,
                    cb = function()
                        local ok_poll = pcall(function()
                            local ok2, st = pcall(_mesh_get_repeat_status, poll_hash)
                            if not ok2 or st == 0 then
                                if timer then timer:delete(); timer = nil end
                                rep_lbl:delete()
                                return
                            end
                            if st == 2 then
                                rep_lbl.text = "repeated"
                                if timer then timer:delete(); timer = nil end
                            elseif st == 3 then
                                rep_lbl.text = "no echo"
                                if timer then timer:delete(); timer = nil end
                            end
                        end)
                        if not ok_poll and timer then timer:delete(); timer = nil end
                    end,
                }
            end
        end

        return bubble
    end

    -- Existing messages: the bucket openThread just loaded. Rooms and
    -- repeaters persist in the DM store keyed by the server's name.
    local history = {}
    if target.type == "channel" then
        history = messages:getChannelHistory(target.idx)
    else
        history = messages:getDMThread(target.name)
    end

    -- Render only the most recent 20 to keep the UI responsive.
    local last_lbl
    local start_idx = math.max(1, #history - 19)
    for i = start_idx, #history do last_lbl = render_msg(history[i]) end
    if last_lbl then last_lbl:scroll_to_view(false) end

    if #history == 0 then
        msg_list:Label {
            text = "No messages yet — say hello.",
            text_color = COL_META, w = lvgl.PCT(100),
        }
    end

    -- Auto-load older messages when scrolled to top. Routed through
    -- scroll_aware_list's single SCROLL_END (page_load_older) so it doesn't
    -- clobber the scroll-suppression handler.
    local load_start = start_idx
    page_load_older = function()
        if load_start <= 1 then return end
        if msg_list:get_scroll_top() > 5 then return end
        local new_start = math.max(1, load_start - 20)
        for i = load_start - 1, new_start, -1 do
            local lbl = render_msg(history[i])
            lbl:move_to_index(0)
        end
        load_start = new_start
    end

    -- Live message listener for the open thread. The thread is on screen, so
    -- anything arriving for it is already seen — clear its badge counter.
    if target.type == "channel" then
        messages:onMessage(function(msg)
            local idx = msg.channel_idx or 0
            if idx == target.idx then
                local lbl = render_msg(msg)
                if lbl then lbl:scroll_to_view(false) end
                messages:clearUnreadChannel(target.idx)
            end
        end)
    elseif target.type == "dm" then
        messages:onDirectMessage(function(msg)
            if msg.from == target.name or msg.to == target.name then
                local lbl = render_msg(msg)
                if lbl then lbl:scroll_to_view(false) end
                messages:clearUnreadDM(target.name)
            end
        end)
    elseif target.type == "room" then
        -- Two live sources: our own posts echo via the DM path (msg.to =
        -- room), other members' posts arrive as room messages.
        messages:onDirectMessage(function(msg)
            if msg.to == target.name then
                local lbl = render_msg(msg)
                if lbl then lbl:scroll_to_view(false) end
            end
        end)
        messages:onRoomMessage(function(msg)
            if msg.room == target.name then
                local lbl = render_msg(msg)
                if lbl then lbl:scroll_to_view(false) end
                messages:clearUnreadDM(target.name)
            end
        end)
    elseif target.type == "repeater" then
        messages:onCliResponse(function(msg)
            if msg.from == target.name then
                local lbl = render_msg(msg)
                if lbl then lbl:scroll_to_view(false) end
            end
        end)
    end

    -- Live delivery status for our own DMs: update the bubble header when the
    -- ack (or timeout) for the sent message comes back. Registered for every
    -- chat (replacing any prior handler); it's a no-op unless the acked message
    -- has a tracked bubble in this view.
    messages:onAck(function(m)
        if current_mode ~= "chat" then return end
        local lbl = ack_labels[m]
        if not lbl then return end
        pcall(function()
            lbl.text = "You  " .. utils.clockHM(m.timestamp)
                .. "  " .. dm_status_text(m.status, m)
            lbl:set { text_color = (m.status == "failed") and "#ff8080" or COL_META }
        end)
    end)

    -- Input row. Wire payload caps at 160 chars. Channel messages are sent as
    -- "<name>: <text>", so our node name + ": " count against it; DMs/rooms carry
    -- no name prefix (sender is known by key) and get the full 160.
    local MAX_TEXT_LEN = 160
    local max_len = MAX_TEXT_LEN
    if target.type == "channel" then
        max_len = MAX_TEXT_LEN - #me - 2
        if max_len < 1 then max_len = 1 end
    end
    textArea = body:Textarea {
        password_mode = false, one_line = true,
        max_length = max_len,
        w = lvgl.PCT(75), h = 34,
    }

    local function do_send()
        local text = textArea.text
        if not text or #text == 0 then return end
        if target.type == "channel" then
            if target.idx == 0 then messages:broadcast(text)
            else messages:sendToChannel(target.idx, text) end
        elseif target.type == "dm" or target.type == "room" then
            messages:sendDirect(target.name, text)
        elseif target.type == "repeater" then
            -- The repeater chat is a CLI console: sends go out as commands
            -- (needs a logged-in session or the repeater ignores them).
            local okc, m = messages:sendCommand(target.name, text)
            if okc and m then
                local lbl = render_msg(m)
                if lbl then lbl:scroll_to_view(false) end
            end
        end
        textArea.text = ""
    end

    -- Composed sequence emojis (PUA form, 3 bytes each in the textarea) expand
    -- to their real Unicode on the wire (up to ~25 bytes each), so max_length
    -- alone can't guarantee the 160-byte wire cap. Enforce the true budget
    -- live: drop trailing codepoints while the decomposed form is over.
    local function enforce_wire_budget()
        local t = textArea.text
        if not t or #t == 0 then return end
        local ok, wire = pcall(_emoji_decompose, t)
        if not ok or not wire then return end   -- pre-rebuild firmware: no-op
        while #wire > max_len and #t > 0 do
            local i = #t
            while i > 1 and t:byte(i) >= 0x80 and t:byte(i) < 0xC0 do i = i - 1 end
            t = t:sub(1, i - 1)
            ok, wire = pcall(_emoji_decompose, t)
            if not ok or not wire then return end
        end
        if t ~= textArea.text then textArea.text = t end
    end

    textArea:onevent(lvgl.EVENT.KEY, function()
        local indev = lvgl.indev.get_act()
        local key = indev:get_key()
        if key == lvgl.KEY.ENTER then do_send() return end
        enforce_wire_budget()
    end)

    -- Hold the input to open the clipboard menu (paste a copied card, etc.).
    textArea:onevent(lvgl.EVENT.LONG_PRESSED, function()
        local overlay = root:Object {
            w = W, h = H, x = 0, y = 0,
            bg_color = "#000000", bg_opa = 128, border_width = 0, pad_all = 0,
        }
        overlay:clear_flag(lvgl.FLAG.SCROLLABLE)
        overlay:add_flag(lvgl.FLAG.CLICKABLE)  -- modal
        local function close()
            nav.pop()
            overlay:delete()
        end
        local pbox = overlay:Object {
            w = W - 40, h = lvgl.SIZE_CONTENT, align = lvgl.ALIGN.CENTER,
            bg_color = "#333333", radius = 6,
            border_width = 1, border_color = "#555555", pad_all = 8,
            flex = { flex_direction = "column", flex_wrap = "nowrap" },
        }
        nav.push(pbox)
        pbox:Label { text = "Clipboard", w = lvgl.PCT(100), text_color = COL_META }

        local paste_b = pbox:Button { w = lvgl.PCT(100), h = 28 }
        paste_b:Label { text = "Paste", align = lvgl.ALIGN.CENTER }
        paste_b:onevent(lvgl.EVENT.RELEASED, function()
            if clipboard.has() then
                textArea.text = (textArea.text or "") .. clipboard.paste()
                enforce_wire_budget()
            end
            close()
        end)

        local copy_b = pbox:Button { w = lvgl.PCT(100), h = 28 }
        copy_b:Label { text = "Copy", align = lvgl.ALIGN.CENTER }
        copy_b:onevent(lvgl.EVENT.RELEASED, function()
            clipboard.copy(textArea.text or "")
            close()
        end)

        local cancel_b = pbox:Button { w = lvgl.PCT(100), h = 26 }
        cancel_b:Label { text = "Cancel", align = lvgl.ALIGN.CENTER }
        cancel_b:onevent(lvgl.EVENT.RELEASED, close)
    end)

    local send_btn = body:Button { w = lvgl.SIZE_CONTENT, h = 34 }
    send_btn:Label { text = "Send", align = lvgl.ALIGN.CENTER }
    send_btn:onevent(lvgl.EVENT.RELEASED, do_send)
end

-- ── MY NODE CARD ────────────────────────────────────────────────
show_my_node = function()
    -- Modal overlay: nav.push (below) suspends the view beneath; CLICKABLE makes
    -- the dimmed area swallow taps instead of passing them to that view.
    local overlay = root:Object {
        w = W, h = H, x = 0, y = 0,
        bg_color = "#000000", bg_opa = 128, border_width = 0, pad_all = 0,
    }
    overlay:clear_flag(lvgl.FLAG.SCROLLABLE)
    overlay:add_flag(lvgl.FLAG.CLICKABLE)

    local function close_popup()
        nav.pop()
        overlay:delete()
    end

    local box = overlay:Object {
        w = W - 20, h = H - 20, align = lvgl.ALIGN.CENTER,
        bg_color = "#333333", radius = 6,
        border_width = 1, border_color = "#555555", pad_all = 8,
        flex = { flex_direction = "column", flex_wrap = "nowrap" },
    }
    nav.push(box)

    -- Full-screen QR of our own contact card, scannable by the MeshCore app.
    local function show_qr()
        -- MeshCore app contact-import URI (NOT the raw advert biz-card the
        -- firmware's importCard uses): meshcore://contact/add?name=..&public_key=..&type=..
        local okc, ni = pcall(_mesh_get_node_info)
        if not okc or not ni or not ni.pubkey or ni.pubkey == "" then return false end
        local card = contact_uri(ni.name, ni.pubkey, 1)
        local qov = root:Object {
            w = W, h = H, x = 0, y = 0,
            bg_color = "#000000", bg_opa = 245, border_width = 0, pad_all = 0,
        }
        qov:clear_flag(lvgl.FLAG.SCROLLABLE)
        qov:add_flag(lvgl.FLAG.CLICKABLE)  -- modal
        local function qclose()
            nav.pop()
            qov:delete()
        end
        qov:Label { text = "Scan in the MeshCore app", text_color = "#FFFFFF",
                    align = lvgl.ALIGN.TOP_MID, y = 4 }
        -- White holder gives the QR its quiet-zone margin.
        local holder = qov:Object {
            w = 180, h = 180, align = lvgl.ALIGN.CENTER,
            bg_color = "#FFFFFF", border_width = 0, pad_all = 8,
        }
        holder:clear_flag(lvgl.FLAG.SCROLLABLE)
        local pok, qok = pcall(_qr_create, holder, card, 164)
        if not (pok and qok) then
            holder:Label { text = "QR unavailable", align = lvgl.ALIGN.CENTER,
                           text_color = "#000000" }
        end
        -- Small square X in the top corner (clear of the QR).
        local qclose_btn = qov:Button { w = 28, h = 28, align = lvgl.ALIGN.TOP_RIGHT, x = -4, y = 4 }
        qclose_btn:Label { text = "X", align = lvgl.ALIGN.CENTER }
        qclose_btn:onevent(lvgl.EVENT.RELEASED, qclose)
        nav.push(qov)
        return true
    end

    local function info(text) box:Label { text = text, w = lvgl.PCT(100) } end

    info("-- My Node --")
    local ok, ni = pcall(_mesh_get_node_info)
    if ok and ni then
        my_name = ni.name
        info("Name: " .. utils.emojiText(ni.name or "?"))
        info("Key: " .. string.sub(ni.pubkey or "", 1, 16) .. "..")
        info(string.format("Radio: %.3f MHz", ni.freq or 0))
        info(string.format("SF%d  BW%.0f  CR%s",
            ni.spreading_factor or 0, ni.bandwidth or 0, tostring(ni.coding_rate or "?")))
        info("TX power: " .. (ni.tx_power or "?") .. " dBm")
        if ni.lat and ni.lon and (ni.lat ~= 0 or ni.lon ~= 0) then
            info(string.format("Loc: %.4f, %.4f", ni.lat, ni.lon))
        end
    else
        info("Node info unavailable")
    end

    local ok_rx, rx = pcall(_mesh_get_rx_info)
    if ok_rx and rx then
        info(string.format("Last RX: SNR %.1f / RSSI %.0f", rx.snr or 0, rx.rssi or 0))
    end
    local ok_b, mv = pcall(_get_battery_mv)
    if ok_b and mv and mv > 0 then info("Battery: " .. mv .. " mV") end

    local adv_btn = box:Button { w = lvgl.PCT(48), h = 28 }
    adv_btn:Label { text = "Advert", align = lvgl.ALIGN.CENTER }
    adv_btn:onevent(lvgl.EVENT.RELEASED, function()
        pcall(_mesh_send_advert, "flood")
        adv_btn:clean(); adv_btn:Label { text = "Sent!", align = lvgl.ALIGN.CENTER }
    end)

    local zero_btn = box:Button { w = lvgl.PCT(48), h = 28 }
    zero_btn:Label { text = "Advert 0hop", align = lvgl.ALIGN.CENTER }
    zero_btn:onevent(lvgl.EVENT.RELEASED, function()
        pcall(_mesh_send_advert, "zerohop")
        zero_btn:clean(); zero_btn:Label { text = "Sent!", align = lvgl.ALIGN.CENTER }
    end)

    -- Copy our own contact (app-format URI) to the clipboard.
    local copy_btn = box:Button { w = lvgl.PCT(48), h = 28 }
    local copy_lbl = copy_btn:Label { text = "Copy Contact", align = lvgl.ALIGN.CENTER }
    copy_btn:onevent(lvgl.EVENT.RELEASED, function()
        local ok, ni = pcall(_mesh_get_node_info)
        if ok and ni and ni.pubkey and ni.pubkey ~= "" then
            clipboard.copy(contact_uri(ni.name, ni.pubkey, 1))
            copy_lbl.text = "Copied!"
        else
            copy_lbl.text = "Copy failed"
        end
    end)

    -- Show a scannable QR of our contact for the MeshCore app's QR scanner.
    local qr_btn = box:Button { w = lvgl.PCT(48), h = 28 }
    local qr_lbl = qr_btn:Label { text = "Show QR", align = lvgl.ALIGN.CENTER }
    qr_btn:onevent(lvgl.EVENT.RELEASED, function()
        if not show_qr() then qr_lbl.text = "No card" end
    end)

    local close_btn = box:Button { w = lvgl.PCT(100), h = 26 }
    close_btn:Label { text = "Close", align = lvgl.ALIGN.CENTER }
    close_btn:onevent(lvgl.EVENT.RELEASED, close_popup)
end

-- ── IMPORT CONTACT (paste biz card) ─────────────────────────────
show_import_contact = function(on_done)
    -- Modal overlay: nav.push (below) suspends the view beneath; CLICKABLE makes
    -- the dimmed area swallow taps instead of passing them to that view.
    local overlay = root:Object {
        w = W, h = H, x = 0, y = 0,
        bg_color = "#000000", bg_opa = 128, border_width = 0, pad_all = 0,
    }
    overlay:clear_flag(lvgl.FLAG.SCROLLABLE)
    overlay:add_flag(lvgl.FLAG.CLICKABLE)

    local function close_popup(refresh)
        nav.pop()
        overlay:delete()
        if refresh and on_done then on_done() end
    end

    local box = overlay:Object {
        w = W - 20, h = H - 40, align = lvgl.ALIGN.CENTER,
        bg_color = "#333333", radius = 6,
        border_width = 1, border_color = "#555555", pad_all = 8,
        flex = { flex_direction = "column", flex_wrap = "nowrap" },
    }
    nav.push(box)

    box:Label { text = "-- Add Contact --", w = lvgl.PCT(100) }
    box:Label { text = "Paste a meshcore:// card:", w = lvgl.PCT(100), text_color = COL_META }

    local field = box:Textarea {
        one_line = false, placeholder_text = "meshcore://...",
        w = lvgl.PCT(100), h = 60,
    }

    -- Paste from the in-app clipboard (e.g. a card you copied from a message).
    local paste_btn = box:Button { w = lvgl.PCT(100), h = 26 }
    paste_btn:Label { text = "Paste", align = lvgl.ALIGN.CENTER }
    paste_btn:onevent(lvgl.EVENT.RELEASED, function()
        if clipboard.has() then field.text = clipboard.paste() end
    end)

    local status = box:Label { text = "", w = lvgl.PCT(100), text_color = COL_ACCENT }

    local import_btn = box:Button { w = lvgl.PCT(100), h = 28 }
    import_btn:Label { text = "Import", align = lvgl.ALIGN.CENTER }
    import_btn:onevent(lvgl.EVENT.RELEASED, function()
        local card = field.text
        if not card or #card < 12 then
            status.text = "Card too short"
            return
        end
        local ok = pcall(_mesh_import_contact, card)
        if ok then
            close_popup(true)
        else
            status.text = "Import failed"
        end
    end)

    local cancel = box:Button { w = lvgl.PCT(100), h = 26 }
    cancel:Label { text = "Cancel", align = lvgl.ALIGN.CENTER }
    cancel:onevent(lvgl.EVENT.RELEASED, function() close_popup(false) end)
end

-- ── CLEAR-ALL CONFIRM ───────────────────────────────────────────
show_clear_confirm = function()
    local overlay = root:Object {
        w = W, h = H, x = 0, y = 0, bg_opa = 200, border_width = 0, pad_all = 0,
    }
    overlay:clear_flag(lvgl.FLAG.SCROLLABLE)
    overlay:add_flag(lvgl.FLAG.CLICKABLE)  -- modal
    local box = overlay:Object {
        w = 220, h = 100, align = lvgl.ALIGN.CENTER,
        border_width = 1, pad_all = 10,
        flex = { flex_direction = "column", flex_wrap = "nowrap" },
    }
    box:clear_flag(lvgl.FLAG.SCROLLABLE)
    nav.push(box)
    box:Label { text = "Clear all contacts?", w = lvgl.PCT(100), h = 24 }
    local yes_btn = box:Button { w = lvgl.PCT(48), h = 32 }
    yes_btn:Label { text = "Yes", align = lvgl.ALIGN.CENTER }
    yes_btn:onevent(lvgl.EVENT.RELEASED, function()
        pcall(_mesh_clear_contacts); nav.pop(); overlay:delete(); show_contacts()
    end)
    local no_btn = box:Button { w = lvgl.PCT(48), h = 32 }
    no_btn:Label { text = "No", align = lvgl.ALIGN.CENTER }
    no_btn:onevent(lvgl.EVENT.RELEASED, function() nav.pop(); overlay:delete() end)
end

-- ── CONTACT SETTINGS POPUP ──────────────────────────────────────
-- Type-visibility toggles (the "exclude" filter) plus the Add / Clear actions
-- relocated off the main contacts row.
show_contact_settings = function()
    -- Modal overlay: CLICKABLE so it swallows taps on the dimmed area instead of
    -- letting them fall through to the contacts list behind it (nav.push already
    -- removes that list from the focus group for the trackball/keyboard).
    local overlay = root:Object {
        w = W, h = H, x = 0, y = 0,
        bg_color = "#000000", bg_opa = 128, border_width = 0, pad_all = 0,
    }
    overlay:clear_flag(lvgl.FLAG.SCROLLABLE)
    overlay:add_flag(lvgl.FLAG.CLICKABLE)

    -- rebuild=true re-enters show_contacts so toggle changes take effect. Either
    -- way nav.pop() resumes the contacts view beneath — no saved_view dance.
    local function close_popup(rebuild)
        nav.pop()
        overlay:delete()
        if rebuild then
            lvgl.Timer { period = 1, cb = function(t)
                t:delete()
                if current_mode == "contacts" then show_contacts() end
            end }
        end
    end

    local box = overlay:Object {
        w = W - 20, h = H - 20, align = lvgl.ALIGN.CENTER,
        bg_color = "#333333", radius = 6,
        border_width = 1, border_color = "#555555", pad_all = 8,
        flex = { flex_direction = "column", flex_wrap = "nowrap" },
    }
    nav.push(box)

    local hdr = box:Object {
        w = lvgl.PCT(100), h = 20, pad_all = 0, border_width = 0,
        bg_opa = 0, flex = { flex_direction = "row", flex_wrap = "nowrap" },
    }
    hdr:clear_flag(lvgl.FLAG.SCROLLABLE)
    hdr:Label { text = "-- Contact Settings --", flex_grow = 1 }
    local x_btn = hdr:Button { w = 22, h = 20 }
    x_btn:Label { text = "X", align = lvgl.ALIGN.CENTER }
    x_btn:onevent(lvgl.EVENT.RELEASED, function() close_popup(true) end)

    local function toggle_btn(label, get, set)
        local b = box:Button { w = lvgl.PCT(100), h = 26 }
        local lbl = b:Label { align = lvgl.ALIGN.LEFT_MID }
        local function refresh() lbl.text = (get() and "[x] " or "[ ] ") .. label end
        refresh()
        b:onevent(lvgl.EVENT.RELEASED, function() set(not get()); refresh() end)
    end

    -- Filters: which contact types are shown in the list (display only).
    box:Label { text = "Filters (show):", w = lvgl.PCT(100), text_color = COL_META }
    toggle_btn("Users", function() return contacts_show_users end,
                        function(v) contacts_show_users = v end)
    toggle_btn("Repeaters", function() return contacts_show_repeaters end,
                        function(v) contacts_show_repeaters = v end)
    toggle_btn("Rooms", function() return contacts_show_rooms end,
                        function(v) contacts_show_rooms = v end)
    toggle_btn("Sensors", function() return contacts_show_sensors end,
                        function(v) contacts_show_sensors = v end)

    -- Auto add: mirrors the BLE companion app. "Auto add all" (default) adds
    -- every discovered advert type; turn it off to auto-add only the selected
    -- types below. The type checkboxes stay visible in all-mode but are ignored
    -- until "Auto add all" is off. Persisted via _prefs.manual_add_contacts +
    -- autoadd_config. Gracefully no-ops if the firmware lacks the bridge yet.
    local aa_selected, aa_chat, aa_reps, aa_rooms, aa_sensors =
        false, false, false, false, false
    local ok_aa, m, c1, c2, c3, c4 = pcall(_mesh_get_autoadd)
    if ok_aa then aa_selected, aa_chat, aa_reps, aa_rooms, aa_sensors = m, c1, c2, c3, c4 end
    local function persist_autoadd()
        pcall(_mesh_set_autoadd, aa_selected, aa_chat, aa_reps, aa_rooms, aa_sensors)
    end

    box:Label { text = "Auto add:", w = lvgl.PCT(100), text_color = COL_META }
    -- Checked "Auto add all" == NOT selected-mode.
    toggle_btn("Auto add all", function() return not aa_selected end,
                        function(v) aa_selected = not v; persist_autoadd() end)
    toggle_btn("Users", function() return aa_chat end,
                        function(v) aa_chat = v; persist_autoadd() end)
    toggle_btn("Repeaters", function() return aa_reps end,
                        function(v) aa_reps = v; persist_autoadd() end)
    toggle_btn("Rooms", function() return aa_rooms end,
                        function(v) aa_rooms = v; persist_autoadd() end)
    toggle_btn("Sensors", function() return aa_sensors end,
                        function(v) aa_sensors = v; persist_autoadd() end)

    -- Auto-add hop limit: how far away an advert may be and still auto-add.
    box:Label { text = "Auto-add max hops:", w = lvgl.PCT(100), text_color = COL_META }
    local mh_vals = { 0, 1, 2, 3, 4 }  -- dropdown index -> autoadd_max_hops value
    local cur_mh = 0
    local ok_mh, mh = pcall(_mesh_get_autoadd_max_hops)
    if ok_mh and mh then cur_mh = mh end
    local mh_sel = 0
    for i = 1, #mh_vals do
        if mh_vals[i] == cur_mh then mh_sel = i - 1; break end
    end
    local mh_dd = box:Dropdown {
        options = "Any\nDirect only\nUp to 1 hop\nUp to 2 hops\nUp to 3 hops",
        w = lvgl.PCT(100), h = 30, dir = lvgl.DIR.BOTTOM,
    }
    mh_dd:set({ selected = mh_sel })
    mh_dd:onevent(lvgl.EVENT.VALUE_CHANGED, function()
        pcall(_mesh_set_autoadd_max_hops, mh_vals[mh_dd:get("selected") + 1] or 0)
    end)

    -- When the contact list is full: overwrite the oldest non-favourite, or
    -- discard the new one (and archiving of contacts leaving the active list).
    local ok_ni, ni = pcall(_mesh_get_node_info)
    local overwrite_on = (ok_ni and ni and ni.contact_overwrite) or false
    local archive_on = (ok_ni and ni and ni.archive_contacts)
    if archive_on == nil then archive_on = true end  -- firmware default = on

    box:Label { text = "When full:", w = lvgl.PCT(100), text_color = COL_META }
    toggle_btn("Overwrite oldest", function() return overwrite_on end,
        function(v) overwrite_on = v
            pcall(_mesh_set_config, "contact_overwrite", v and "1" or "0") end)
    toggle_btn("Archive contacts", function() return archive_on end,
        function(v) archive_on = v
            pcall(_mesh_set_config, "archive_contacts", v and "1" or "0") end)

    -- Compact the on-disk archive: C-side streaming rewrite down to one record
    -- per contact (newest wins, live contacts dropped). Nothing loads into Lua
    -- but the two record counts shown on the button.
    local arch_n = 0
    local ok_an, an = pcall(_mesh_archive_count)
    if ok_an and type(an) == "number" then arch_n = an end
    local compact_btn = box:Button { w = lvgl.PCT(100), h = 28 }
    local compact_lbl = compact_btn:Label {
        text = "Compact archive (" .. arch_n .. ")", align = lvgl.ALIGN.CENTER }
    local compacting = false
    compact_btn:onevent(lvgl.EVENT.RELEASED, function()
        if compacting then return end
        compacting = true
        compact_lbl.text = "Compacting..."
        -- One tick later so the label paints before the blocking C call.
        apps.add_timer { period = 50, cb = function(t)
            t:delete()
            local ok_c, before, after = pcall(_mesh_archive_compact)
            if ok_c and before then
                compact_lbl.text = "Compacted: " .. before .. " -> " .. after
            else
                compact_lbl.text = "Compact failed (" .. tostring(after or before) .. ")"
            end
            compacting = false
        end }
    end)

    local add_btn = box:Button { w = lvgl.PCT(100), h = 28 }
    add_btn:Label { text = "Add Contact", align = lvgl.ALIGN.CENTER }
    add_btn:onevent(lvgl.EVENT.RELEASED, function()
        close_popup(false)
        show_import_contact(function() show_contacts() end)
    end)

    local clear_btn = box:Button { w = lvgl.PCT(100), h = 28 }
    clear_btn:Label { text = "Clear All Contacts", align = lvgl.ALIGN.CENTER }
    clear_btn:onevent(lvgl.EVENT.RELEASED, function()
        close_popup(false)
        show_clear_confirm()
    end)

    local close_btn = box:Button { w = lvgl.PCT(100), h = 28 }
    close_btn:Label { text = "Apply & Close", align = lvgl.ALIGN.CENTER }
    close_btn:onevent(lvgl.EVENT.RELEASED, function() close_popup(true) end)
end

-- Region / flood-scope settings popup. Floods carry a transport code derived
-- from the region name; region-enforcing repeaters only relay matching codes.
-- The GLOBAL region is the default for all sends. When opened from a channel
-- chat (chan_name given) it also offers that channel's override: blank =
-- inherit global, a name = use that region for this channel's sends only.
-- on_change (optional) is called after any save/clear so the opener can
-- refresh its active-region indicator.
show_flood_scope = function(chan_name, on_change)
    local overlay = root:Object {
        w = W, h = H, x = 0, y = 0,
        bg_color = "#000000", bg_opa = 128, border_width = 0, pad_all = 0,
    }
    overlay:clear_flag(lvgl.FLAG.SCROLLABLE)
    overlay:add_flag(lvgl.FLAG.CLICKABLE)

    local function close_popup()
        nav.pop()
        overlay:delete()
    end

    -- Fixed height (not SIZE_CONTENT) so the flex column scrolls when its
    -- content is taller than the box; the box keeps its default SCROLLABLE flag.
    local box = overlay:Object {
        w = W - 20, h = H - 20, align = lvgl.ALIGN.CENTER,
        bg_color = "#333333", radius = 6,
        border_width = 1, border_color = "#555555", pad_all = 8,
        flex = { flex_direction = "column", flex_wrap = "nowrap" },
    }
    nav.push(box)

    -- Header row: title + small square X close (same pattern as the contact
    -- settings popup) so the popup can be dismissed without scrolling to the
    -- bottom Close button.
    local hdr = box:Object {
        w = lvgl.PCT(100), h = 20, pad_all = 0, border_width = 0,
        bg_opa = 0, flex = { flex_direction = "row", flex_wrap = "nowrap" },
    }
    hdr:clear_flag(lvgl.FLAG.SCROLLABLE)
    hdr:Label { text = "-- Region / Flood Scope --", flex_grow = 1 }
    local x_btn = hdr:Button { w = 22, h = 20 }
    x_btn:Label { text = "X", align = lvgl.ALIGN.CENTER }
    x_btn:onevent(lvgl.EVENT.RELEASED, close_popup)

    -- ── Global region (the default for every send) ──
    box:Label { text = "GLOBAL REGION", w = lvgl.PCT(100) }
    box:Label {
        text = "Default for ALL sends (DMs + channels) unless a channel overrides it. Blank = none (world-wide). Must match other nodes exactly.",
        w = lvgl.PCT(100), text_color = COL_META,
    }

    local cur = ""
    local ok_fs, name = pcall(_mesh_get_flood_scope)
    if ok_fs and name then cur = name end

    -- The phone app can set a session-only scope key (no name) that outranks
    -- the global default until cleared or reboot; flag it on the status line.
    local ble_note = ""
    local ok_ba, ble_on = pcall(_mesh_ble_scope_active)
    if ok_ba and ble_on then ble_note = " (phone override active)" end

    local input = box:Textarea {
        one_line = true, text = cur, placeholder_text = "region name",
        w = lvgl.PCT(100), h = 32,
    }
    input:clear_flag(lvgl.FLAG.SCROLLABLE)

    local status = box:Label {
        text = ((cur ~= "" and ("Current: " .. cur)) or "Current: none (world-wide)") .. ble_note,
        w = lvgl.PCT(100), text_color = COL_META,
    }

    local save_btn = box:Button { w = lvgl.PCT(100), h = 30 }
    save_btn:Label { text = "Save Global", align = lvgl.ALIGN.CENTER }
    save_btn:onevent(lvgl.EVENT.RELEASED, function()
        local n = input.text or ""
        if pcall(_mesh_set_flood_scope, n) then
            status.text = ((n ~= "") and ("Set: " .. n) or "Cleared (world-wide)") .. ble_note
            if on_change then pcall(on_change) end
        else
            status.text = "Failed to save"
        end
    end)

    local clear_btn = box:Button { w = lvgl.PCT(100), h = 30 }
    clear_btn:Label { text = "Clear Global", align = lvgl.ALIGN.CENTER }
    clear_btn:onevent(lvgl.EVENT.RELEASED, function()
        input.text = ""
        pcall(_mesh_set_flood_scope, "")
        status.text = "Cleared (world-wide)" .. ble_note
        if on_change then pcall(on_change) end
    end)

    -- ── This channel's override (only when opened from a channel chat) ──
    if chan_name then
        box:Label {
            text = "CHANNEL REGION - " .. utils.emojiText(chan_name),
            w = lvgl.PCT(100),
        }
        box:Label {
            text = "Overrides the global region for this channel's sends only. Blank = use global.",
            w = lvgl.PCT(100), text_color = COL_META,
        }

        local ccur = ""
        local ok_cs, cname = pcall(_mesh_get_channel_scope, chan_name)
        if ok_cs and cname then ccur = cname end

        local cinput = box:Textarea {
            one_line = true, text = ccur, placeholder_text = "region name",
            w = lvgl.PCT(100), h = 32,
        }
        cinput:clear_flag(lvgl.FLAG.SCROLLABLE)

        local cstatus = box:Label {
            text = (ccur ~= "" and ("Override: " .. ccur)) or "Using global",
            w = lvgl.PCT(100), text_color = COL_META,
        }

        local csave_btn = box:Button { w = lvgl.PCT(100), h = 30 }
        csave_btn:Label { text = "Save Channel", align = lvgl.ALIGN.CENTER }
        csave_btn:onevent(lvgl.EVENT.RELEASED, function()
            local n = cinput.text or ""
            if pcall(_mesh_set_channel_scope, chan_name, n) then
                cstatus.text = (n ~= "") and ("Override: " .. n) or "Using global"
                if on_change then pcall(on_change) end
            else
                cstatus.text = "Failed to save"
            end
        end)

        local cclear_btn = box:Button { w = lvgl.PCT(100), h = 30 }
        cclear_btn:Label { text = "Use Global", align = lvgl.ALIGN.CENTER }
        cclear_btn:onevent(lvgl.EVENT.RELEASED, function()
            cinput.text = ""
            pcall(_mesh_set_channel_scope, chan_name, "")
            cstatus.text = "Using global"
            if on_change then pcall(on_change) end
        end)
    end

    local close_btn = box:Button { w = lvgl.PCT(100), h = 30 }
    close_btn:Label { text = "Close", align = lvgl.ALIGN.CENTER }
    close_btn:onevent(lvgl.EVENT.RELEASED, close_popup)
end

-- ── CONTACTS VIEW ───────────────────────────────────────────────
show_contacts = function()
    clear_view()
    current_mode = "contacts"
    set_header("Contacts", "Contacts: " .. _mesh_get_num_contacts())

    -- Controls live in the gridnav body; rows live in a CLICK_FOCUSABLE scroll
    -- list with the same tap-to-arm / long-press scheme as the inbox and chat,
    -- so touch drags scroll the list instead of opening a contact.
    local body = gridnav_body(root, HEADER_H, H - HEADER_H,
                              GRIDNAV_ROLLOVER + GRIDNAV_SCROLL_FIRST, true)
    current_view = body

    -- Row 1 controls: Back, Sort (dropdown), Add, Clear
    local back_btn = body:Button { w = 45, h = 22 }
    back_btn:Label { text = "Home", align = lvgl.ALIGN.CENTER }
    back_btn:onevent(lvgl.EVENT.RELEASED, function() show_inbox() end)

    -- Sort dropdown (Recent / Name / Type). Applies on selection; the rebuild
    -- is deferred one tick so the dropdown isn't deleted from inside its own
    -- VALUE_CHANGED handler.
    local sort_idx_to_key = { [0] = "recent", [1] = "name", [2] = "type", [3] = "favorites" }
    local sort_key_to_idx = { recent = 0, name = 1, type = 2, favorites = 3 }
    -- Give the dropdown enough height for its full line height (the default
    -- padding was clipping the text); keep the vertical padding trimmed so the
    -- label stays centered.
    local sort_dd = body:Dropdown {
        options = "Recent\nName\nType\nFavorites",
        w = 104, h = 28, dir = lvgl.DIR.BOTTOM,
        pad_top = 2, pad_bottom = 2,
    }
    sort_dd:set { selected = sort_key_to_idx[contacts_sort] or 0 }
    sort_dd:onevent(lvgl.EVENT.VALUE_CHANGED, function()
        local new_sort = sort_idx_to_key[sort_dd:get("selected")] or "recent"
        if new_sort == contacts_sort then return end
        contacts_sort = new_sort
        lvgl.Timer { period = 1, cb = function(t)
            t:delete()
            if current_mode == "contacts" then show_contacts() end
        end }
    end)

    -- Settings (gear): contact-type filters + Add / Clear, kept off the main row.
    local ok_gear, has_gear = pcall(_emoji_preload, 0x2699)
    local gear = (ok_gear and has_gear) and "\xE2\x9A\x99" or "Set"
    local set_btn = body:Button { w = 40, h = 22 }
    set_btn:Label { text = gear, align = lvgl.ALIGN.CENTER }
    set_btn:onevent(lvgl.EVENT.RELEASED, function() show_contact_settings() end)

    -- Row 2: search field + apply. Trim the textarea's vertical padding (same
    -- as the sort dropdown) so the text isn't clipped/offset in a short field.
    local search = body:Textarea {
        one_line = true, placeholder_text = "search",
        text = contacts_filter,
        w = lvgl.PCT(70), h = 28,
        pad_top = 2, pad_bottom = 2,
    }
    search:clear_flag(lvgl.FLAG.SCROLLABLE)
    local function apply_filter()
        contacts_filter = search.text or ""
        show_contacts()
    end
    search:onevent(lvgl.EVENT.KEY, function()
        local key = lvgl.indev.get_act():get_key()
        if key == lvgl.KEY.ENTER then apply_filter() end
    end)
    local find_btn = body:Button { w = 50, h = 28 }
    find_btn:Label { text = (contacts_filter ~= "" and "Reset" or "Find"), align = lvgl.ALIGN.CENTER }
    find_btn:onevent(lvgl.EVENT.RELEASED, function()
        if contacts_filter ~= "" then contacts_filter = "" else contacts_filter = search.text or "" end
        show_contacts()
    end)

    -- Scrollable contact list (rows live here, not in the gridnav body).
    local list = body:Object {
        w = lvgl.PCT(100), h = H - HEADER_H - 66,
        border_width = 0, pad_all = 0, bg_opa = 0,
        flex = { flex_direction = "column", flex_wrap = "nowrap" },
    }
    -- Tap/click the list to enter row-select (trackball steps the rows); 'q'
    -- returns focus to the controls. nav.list owns the scope push/pop.
    nav.list(list)

    -- The windowed pager hooks the list's single SCROLL_END (assigned below once
    -- render_page exists). Routing it through scroll_aware_list avoids a second
    -- SCROLL_END handler on `list` (luavgl is one-callback-per-code).
    local page_on_settle
    local bind_click = scroll_aware_list(list, function()
        if page_on_settle then page_on_settle() end
    end)

    -- Tap/click to open; suppressed while the list is being scrolled.
    local function bind_row(row, c_name, c_type)
        bind_click(row, function()
            if c_type == 1 then show_chat { type = "dm", name = c_name }
            elseif c_type == 2 then show_chat { type = "repeater", name = c_name }
            elseif c_type == 3 then show_chat { type = "room", name = c_name }
            else show_contact_detail(c_name) end
        end)
    end


    -- Contact rows are WINDOWED: render a page at a time and extend the window
    -- when the user scrolls near the bottom. This keeps the view cheap to build
    -- and — just as importantly — cheap to tear down. Rendering every row meant a
    -- few hundred contacts produced ~3 LVGL objects each, and deleting them all
    -- synchronously on the next rebuild (sort/filter/settings-close) starved
    -- Core 0 and tripped the watchdog. (Teardown is also backgrounded now; see
    -- apps.delete_view. Windowing keeps the common case small in the first place.)
    local PAGE = 30
    contact_rows = {}

    local ok_c, raw = pcall(_mesh_get_contacts)
    if not ok_c or not raw then raw = {} end

    -- Filter by search text, and to favourites only in Favorites mode.
    local filtered = {}
    local lf = contacts_filter:lower()
    local fav_only = (contacts_sort == "favorites")
    for _, c in ipairs(raw) do
        local name_ok = (lf == "" or c.name:lower():find(lf, 1, true))
        if name_ok and (not fav_only or c.favorite)
           and contact_type_visible(c.type) then
            filtered[#filtered + 1] = c
        end
    end

    -- Sort purely by the selected criterion. Favourites aren't pinned — the
    -- Favorites mode filters to them; otherwise they sort like any other contact
    -- (the '*' prefix still marks them).
    table.sort(filtered, function(a, b)
        if contacts_sort == "name" then
            return a.name:lower() < b.name:lower()
        elseif contacts_sort == "type" then
            if (a.type or 0) ~= (b.type or 0) then return (a.type or 0) < (b.type or 0) end
            return a.name:lower() < b.name:lower()
        end
        return (a.lastmod or 0) > (b.lastmod or 0)
    end)

    if #filtered == 0 then
        local empty_msg
        if #raw == 0 then empty_msg = "No contacts. Send an Advert!"
        elseif fav_only then empty_msg = "No favorite contacts."
        else empty_msg = "No matching contacts." end
        list:Label { text = empty_msg, w = lvgl.PCT(100), h = 20 }
    else
        local rendered = 0
        local function render_page()
            local stop = math.min(rendered + PAGE, #filtered)
            for i = rendered + 1, stop do
                local c = filtered[i]
                local seen = (c.last_seen and c.last_seen > 0) and utils.relTime(c.last_seen) or ""
                local row = list:Button { w = lvgl.PCT(100), h = 24 }
                local left = row:Label { align = lvgl.ALIGN.LEFT_MID }
                left.text = (c.favorite and "* " or "") .. type_icon(c.type) .. utils.emojiText(c.name)
                row:Label { align = lvgl.ALIGN.RIGHT_MID, text = seen, text_color = COL_META }
                bind_row(row, c.name, c.type)
                contact_rows[c.name] = row
            end
            rendered = stop
        end
        render_page()
        -- Extend the window when scrolled within ~2 rows of the bottom. The
        -- list's SCROLL_END (owned by scroll_aware_list) calls this on each
        -- settle, so a flick pages in as it decelerates; adding rows grows the
        -- scrollable area so the next settle near the bottom pages again.
        page_on_settle = function()
            if rendered >= #filtered then return end
            if list:get_scroll_bottom() <= 48 then render_page() end
        end
    end

    -- Live contact updates float a contact to the top of the list.
    messages:onContactUpdate(function(name, ctype)
        if current_mode ~= "contacts" then return end
        if not current_view then return end
        if not contact_type_visible(ctype) then return end
        if contacts_filter ~= "" and not name:lower():find(contacts_filter:lower(), 1, true) then
            return
        end
        if contact_rows[name] then
            contact_rows[name]:move_to_index(0)
        elseif contacts_sort ~= "favorites" then
            -- In Favorites mode we don't add contacts that aren't already
            -- shown (the update carries no favourite flag to test).
            local row = list:Button { w = lvgl.PCT(100), h = 24 }
            row:Label { text = type_icon(ctype) .. name, align = lvgl.ALIGN.LEFT_MID }
            bind_row(row, name, ctype)
            row:move_to_index(0)
            contact_rows[name] = row
        end
    end)
end

-- ── CHANNELS VIEW ───────────────────────────────────────────────
show_channels = function()
    clear_view()
    current_mode = "channels"
    set_header("Channels", "")

    local body = root:Object {
        flex = { flex_direction = "row", flex_wrap = "wrap" },
        w = W, h = H - HEADER_H, y = HEADER_H,
        border_width = 0, pad_all = 4, bg_opa = 0,
    }
    nav.replace(body)
    current_view = body

    local back_btn = body:Button { w = 45, h = 22 }
    back_btn:Label { text = "Home", align = lvgl.ALIGN.CENTER }
    back_btn:onevent(lvgl.EVENT.RELEASED, function() show_inbox() end)

    -- Add channel: name (+ optional PSK). For #hashtag names the key is derived.
    local ch_input = body:Textarea {
        password_mode = false, one_line = true, text = "#",
        w = lvgl.PCT(36), h = 28,
    }
    ch_input:clear_flag(lvgl.FLAG.SCROLLABLE)

    local psk_input = body:Textarea {
        password_mode = false, one_line = true, placeholder_text = "PSK",
        w = lvgl.PCT(22), h = 28,
    }
    psk_input:clear_flag(lvgl.FLAG.SCROLLABLE)

    local add_btn = body:Button { w = 45, h = 28 }
    add_btn:Label { text = "Add", align = lvgl.ALIGN.CENTER }
    add_btn:onevent(lvgl.EVENT.RELEASED, function()
        local name = ch_input.text
        local psk = psk_input.text or ""
        if name and #name > 1 then
            local ok_ch, channels = pcall(_mesh_get_channels)
            local used = {}
            if ok_ch and channels then
                for _, ch in ipairs(channels) do used[ch.idx] = true end
            end
            for i = 1, 7 do  -- slot 0 is Public; MAX_GROUP_CHANNELS is 8
                if not used[i] then
                    local ok = pcall(_mesh_set_channel, i, name, psk)
                    if ok then show_channels() else set_header("Channels", "Bad PSK") end
                    return
                end
            end
            set_header("Channels", "All slots full!")
        end
    end)

    ch_input:onevent(lvgl.EVENT.KEY, function()
        local key = lvgl.indev.get_act():get_key()
        if key == lvgl.KEY.ENTER then add_btn:send_event(lvgl.EVENT.CLICKED, nil) end
    end)

    -- Channel rows: chat button + key indicator + optional delete
    local ok, channels = pcall(_mesh_get_channels)
    if not ok or not channels then channels = {} end

    local public_present = false
    for _, ch in ipairs(channels) do
        local unread = messages:unreadInChannel(ch.idx)
        local chat_btn = body:Button { w = lvgl.PCT(72), h = 24 }
        local lbl = chat_btn:Label { align = lvgl.ALIGN.LEFT_MID }
        lbl.text = utils.emojiText(ch.name) .. (unread > 0 and ("  (" .. unread .. ")") or "")
        if unread > 0 then lbl:set { text_color = COL_ACCENT } end
        local ch_copy = { type = "channel", idx = ch.idx, name = ch.name }
        chat_btn:onevent(lvgl.EVENT.RELEASED, function() show_chat(ch_copy) end)

        -- Every channel can be deleted, incl. Public. Public is matched by NAME
        -- (its slot is incidental) and routes through _mesh_delete_public so the
        -- deletion persists across reboot.
        local del_btn = body:Button { w = 50, h = 24 }
        del_btn:Label { text = "Del", align = lvgl.ALIGN.CENTER }
        local ch_idx = ch.idx
        local is_public = (ch.name == "Public")
        if is_public then public_present = true end
        del_btn:onevent(lvgl.EVENT.RELEASED, function()
            if is_public then _mesh_delete_public() else _mesh_set_channel(ch_idx, "", "") end
            show_channels()
        end)
    end

    -- Public absent (deleted) → show an Add row to bring it back. Driven by actual
    -- presence in the channel list, not a flag, so it can never disagree with reality.
    if not public_present then
        body:Label {
            text = "Public (off)", align = lvgl.ALIGN.LEFT_MID,
            w = lvgl.PCT(72), h = 24, text_color = "#888888",
        }
        local add_btn = body:Button { w = 50, h = 24 }
        add_btn:Label { text = "Add", align = lvgl.ALIGN.CENTER }
        add_btn:onevent(lvgl.EVENT.RELEASED, function()
            _mesh_restore_public(); show_channels()
        end)
    end
end

-- ── CONTACT DETAIL POPUP ────────────────────────────────────────
show_contact_detail = function(contact_name)
    -- Modal overlay: nav.push (below) suspends the view beneath; CLICKABLE makes
    -- the dimmed area swallow taps instead of passing them to that view.
    local overlay = root:Object {
        w = W, h = H, x = 0, y = 0,
        bg_color = "#000000", bg_opa = 128, border_width = 0, pad_all = 0,
    }
    overlay:clear_flag(lvgl.FLAG.SCROLLABLE)
    overlay:add_flag(lvgl.FLAG.CLICKABLE)

    local function close_popup()
        nav.pop()
        overlay:delete()
    end

    local box = overlay:Object {
        w = W - 20, h = H - 20, align = lvgl.ALIGN.CENTER,
        bg_color = "#333333", radius = 6,
        border_width = 1, border_color = "#555555", pad_all = 8,
        flex = { flex_direction = "column", flex_wrap = "nowrap" },
    }
    nav.push(box)

    local function info_label(text) box:Label { text = text, w = lvgl.PCT(100) } end

    local ok, contacts = pcall(_mesh_get_contacts)
    local contact = nil
    if ok and contacts then
        for _, c in ipairs(contacts) do
            if c.name == contact_name then contact = c break end
        end
    end

    if not contact then
        info_label("-- Contact Info --")
        info_label("Contact not found")
        local close_btn = box:Button { w = lvgl.PCT(100), h = 26 }
        close_btn:Label { text = "Close", align = lvgl.ALIGN.CENTER }
        close_btn:onevent(lvgl.EVENT.RELEASED, function() close_popup() end)
        return
    end

    info_label("-- Contact Info --")
    -- Display name is composed; name_color stays keyed on the RAW name so the
    -- color matches the chat bubbles (which also hash the raw name).
    local nm = box:Label { text = "Name: " .. utils.emojiText(contact.name), w = lvgl.PCT(100) }
    nm:set { text_color = name_color(contact.name) }
    info_label("Type: " .. (contact.type_name or "?"))
    info_label("Key: " .. string.sub(contact.pubkey or "", 1, 16) .. "..")
    info_label("Path: " .. (contact.path_len >= 0 and (contact.path_len .. " hops") or "flood"))
    if contact.last_seen and contact.last_seen > 0 then
        info_label("Last seen: " .. utils.relTime(contact.last_seen)
            .. " (" .. utils.clockDateTime(contact.last_seen) .. ")")
    end
    if contact.lat and contact.lon and (contact.lat ~= 0 or contact.lon ~= 0) then
        info_label(string.format("Loc: %.4f, %.4f", contact.lat, contact.lon))
    end

    local paths_btn = box:Button { w = lvgl.PCT(38), h = 22 }
    paths_btn:Label { text = "Paths", align = lvgl.ALIGN.CENTER }
    -- Path history + picker: every record can be made the CURRENT route
    -- ("Use"), and "Flood" drops the learned path. After an action the popup
    -- is recreated fresh (pop-before-delete; no in-place clean of an active
    -- nav scope) so the current-marker reflects the new state.
    local show_paths_popup
    show_paths_popup = function(note)
        local ok2, paths = pcall(_mesh_get_contact_paths, contact.pubkey)
        if not ok2 or type(paths) ~= "table" then paths = {} end
        -- Proven routes first (delivery successes), then most recently seen.
        table.sort(paths, function(a, b)
            if (a.success or 0) ~= (b.success or 0) then
                return (a.success or 0) > (b.success or 0)
            end
            return (a.timestamp or 0) > (b.timestamp or 0)
        end)

        local overlay2 = root:Object {
            w = W, h = H, x = 0, y = 0,
            bg_color = "#000000", bg_opa = 128, border_width = 0, pad_all = 0,
        }
        overlay2:clear_flag(lvgl.FLAG.SCROLLABLE)
        overlay2:add_flag(lvgl.FLAG.CLICKABLE)  -- modal
        local box2 = overlay2:Object {
            w = W - 10, h = H - 20, align = lvgl.ALIGN.CENTER,
            bg_color = "#333333", radius = 6,
            border_width = 1, border_color = "#555555", pad_all = 6,
            flex = { flex_direction = "column", flex_wrap = "nowrap" },
        }
        nav.push(box2)
        local function close2()
            nav.pop()
            overlay2:delete()
        end

        box2:Label { text = "-- Paths: " .. contact.name .. " --", w = lvgl.PCT(100) }
        if note then
            box2:Label { text = note, text_color = COL_ACCENT, w = lvgl.PCT(100) }
        end

        local any_current = false
        if #paths == 0 then
            box2:Label { text = "No path data", w = lvgl.PCT(100) }
        else
            for i, rec in ipairs(paths) do
                local chain = "Direct (0 hop)"
                if rec.path and #rec.path > 0 then chain = table.concat(rec.path, ">") end
                if rec.current then any_current = true end
                local head = box2:Label {
                    text = string.format("%s %s h:%d snr:%.0f rssi:%.0f",
                        rec.current and ">" or ("#" .. i),
                        rec.source or "?", rec.hops or 0, rec.snr or 0, rec.rssi or 0),
                    w = lvgl.PCT(100),
                }
                if rec.current then head:set { text_color = COL_ACCENT } end
                box2:Label { text = "  " .. chain, w = lvgl.PCT(100) }
                box2:Label {
                    text = string.format("  ok:%d fail:%d %dms",
                        rec.success or 0, rec.failure or 0, rec.trip_time_ms or 0),
                    w = lvgl.PCT(100),
                }
                if not rec.current and rec.path_len and rec.path_hex then
                    local use_btn = box2:Button { w = 70, h = 22 }
                    use_btn:Label { text = "Use", align = lvgl.ALIGN.CENTER }
                    use_btn:onevent(lvgl.EVENT.RELEASED, function()
                        local pok, sok = pcall(_mesh_set_contact_path, contact.pubkey,
                                               rec.path_len, rec.path_hex)
                        close2()
                        show_paths_popup((pok and sok) and "Path set" or "Set failed")
                    end)
                end
            end
        end
        if not any_current then
            box2:Label {
                text = "Current: flood (no set path)",
                text_color = COL_META, w = lvgl.PCT(100),
            }
        end

        local flood_btn = box2:Button { w = lvgl.PCT(48), h = 26 }
        flood_btn:Label { text = "Flood", align = lvgl.ALIGN.CENTER }
        flood_btn:onevent(lvgl.EVENT.RELEASED, function()
            pcall(_mesh_reset_path, contact_name)
            close2()
            show_paths_popup("Path reset - sends flood")
        end)

        local close_btn2 = box2:Button { w = lvgl.PCT(48), h = 26 }
        close_btn2:Label { text = "Close", align = lvgl.ALIGN.CENTER }
        close_btn2:onevent(lvgl.EVENT.RELEASED, close2)
    end
    paths_btn:onevent(lvgl.EVENT.RELEASED, function() show_paths_popup() end)

    -- Favourite toggle
    local is_fav = contact.favorite or false
    local ok_star, has_star = pcall(_emoji_preload, 0x2B50)
    local ok_circle, has_circle = pcall(_emoji_preload, 0x26AB)
    local use_emoji = ok_star and has_star and ok_circle and has_circle
    local function get_fav_text()
        if use_emoji then
            return is_fav and "\xe2\xad\x90 Favorite" or "\xe2\x9a\xab Favorite"
        end
        return is_fav and "[x] Favorite" or "[ ] Favorite"
    end
    local fav_btn = box:Button { w = 90, h = 26 }
    local fav_label = fav_btn:Label { text = get_fav_text(), align = lvgl.ALIGN.CENTER }
    fav_btn:onevent(lvgl.EVENT.RELEASED, function()
        is_fav = not is_fav
        fav_label.text = get_fav_text()
        pcall(_mesh_set_contact_favorite, contact_name, is_fav)
    end)

    -- DM (companion)
    if contact.type == 1 then
        local dm_btn = box:Button { w = lvgl.PCT(48), h = 26 }
        dm_btn:Label { text = "DM", align = lvgl.ALIGN.CENTER }
        dm_btn:onevent(lvgl.EVENT.RELEASED, function()
            close_popup(); show_chat { type = "dm", name = contact_name }
        end)
    end

    -- Chat / console + login (room server or repeater)
    if contact.type == 2 or contact.type == 3 then
        local chat_btn = box:Button { w = lvgl.PCT(48), h = 26 }
        chat_btn:Label { text = contact.type == 2 and "Console" or "Chat", align = lvgl.ALIGN.CENTER }
        chat_btn:onevent(lvgl.EVENT.RELEASED, function()
            close_popup()
            show_chat { type = contact.type == 2 and "repeater" or "room", name = contact_name }
        end)

        local login_btn = box:Button { w = lvgl.PCT(48), h = 26 }
        login_btn:Label { text = "Login", align = lvgl.ALIGN.CENTER }
        login_btn:onevent(lvgl.EVENT.RELEASED, function()
            show_login_popup(contact_name, function(status)
                set_header(contact_name, status)
            end)
        end)
    end

    -- Request status (repeater / room / sensor)
    if contact.type == 2 or contact.type == 3 or contact.type == 4 then
        local req_btn = box:Button { w = lvgl.PCT(48), h = 26 }
        req_btn:Label { text = "Req Status", align = lvgl.ALIGN.CENTER }
        req_btn:onevent(lvgl.EVENT.RELEASED, function()
            local rok = pcall(_mesh_send_request, contact_name, 1)
            set_header(contact_name, rok and "Requested" or "Req fail")
        end)
    end

    local share_btn = box:Button { w = lvgl.PCT(48), h = 26 }
    share_btn:Label { text = "Share", align = lvgl.ALIGN.CENTER }
    share_btn:onevent(lvgl.EVENT.RELEASED, function()
        pcall(_mesh_share_contact, contact_name)
        set_header(contact_name, "Shared!")
    end)

    local rp_btn = box:Button { w = lvgl.PCT(48), h = 26 }
    rp_btn:Label { text = "RstPath", align = lvgl.ALIGN.CENTER }
    rp_btn:onevent(lvgl.EVENT.RELEASED, function()
        pcall(_mesh_reset_path, contact_name)
        set_header(contact_name, "Path reset")
    end)

    -- Copy this contact as an app-format URI to the clipboard (to share / paste).
    local exp_btn = box:Button { w = lvgl.PCT(48), h = 26 }
    exp_btn:Label { text = "Copy", align = lvgl.ALIGN.CENTER }
    exp_btn:onevent(lvgl.EVENT.RELEASED, function()
        if contact.pubkey and contact.pubkey ~= "" then
            clipboard.copy(contact_uri(contact.name, contact.pubkey, contact.type or 1))
            set_header(contact_name, "Copied!")
        else
            set_header(contact_name, "No key")
        end
    end)

    local rm_btn = box:Button { w = lvgl.PCT(48), h = 26 }
    rm_btn:Label { text = "Remove", align = lvgl.ALIGN.CENTER }
    rm_btn:onevent(lvgl.EVENT.RELEASED, function()
        close_popup()
        pcall(_mesh_remove_contact, contact_name)
        if current_mode == "contacts" then show_contacts() end
    end)

    local close_btn = box:Button { w = lvgl.PCT(48), h = 26 }
    close_btn:Label { text = "Close", align = lvgl.ALIGN.CENTER }
    close_btn:onevent(lvgl.EVENT.RELEASED, function() close_popup() end)
end

-- ── Initial view ────────────────────────────────────────────────
show_inbox()
