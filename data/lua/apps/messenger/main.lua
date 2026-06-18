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

-- Persistence lives on the C++ PunkMesh side (respects _storage: LittleFS
-- root or /meshpunk on SD), so any app can access the same message history.
-- The per-file cap is owned by the firmware (_max_messages, default 400) — we
-- deliberately don't shrink it here, so the full stored history is available.
-- The chat view only renders the most recent slice and pages older messages
-- in on demand, so a large history stays cheap to display.
messages:loadPersisted()

local W = lvgl.HOR_RES()
local H = lvgl.VER_RES()
local HEADER_H = 24

-- Focus group for trackball navigation
local group = lvgl.group.get_default()

-- Root container
local root = apps.new_root()
root:set { w = W, h = H, pad_all = 0, border_width = 0 }
root:clear_flag(lvgl.FLAG.SCROLLABLE)

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
    border_width = 0, pad_left = 4, pad_right = 4,
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
    _nav_clear()
    if current_view then current_view:delete(); current_view = nil end
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

-- Make a scrollable list's rows "scroll-aware": while the list is being dragged
-- (touch scroll) row clicks are suppressed, then re-enabled once it settles, so
-- a tap opens a row but a drag scrolls past it. A plain single click works (also
-- nice for the trackball — no double-press). Returns a binder that attaches the
-- click handler to each row.
local function scroll_aware_list(list)
    local scrolling = false
    local settle_timer = nil
    list:onevent(lvgl.EVENT.SCROLL_BEGIN, function()
        scrolling = true
        if settle_timer then settle_timer:delete(); settle_timer = nil end
    end)
    list:onevent(lvgl.EVENT.SCROLL_END, function()
        -- Hold the suppression briefly past the scroll so the release that
        -- finishes the drag isn't taken as a tap.
        if settle_timer then settle_timer:delete() end
        settle_timer = lvgl.Timer { period = 150, cb = function(t)
            t:delete(); settle_timer = nil; scrolling = false
        end }
    end)
    return function(obj, activate)
        obj:onevent(lvgl.EVENT.RELEASED, function()
            if scrolling then return end
            activate()
        end)
    end
end

-- Delivery word shown after the time on our own DM bubbles.
local function dm_status_text(status)
    if status == "delivered" then return "delivered"
    elseif status == "failed" then return "failed"
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

-- Forward declarations
local show_inbox, show_chat, show_contacts, show_channels
local show_contact_detail, show_my_node, show_import_contact
local show_contact_settings, show_clear_confirm

-- ── Long-press popup showing message metadata ───────────────────
local function show_msg_info(msg, on_reply, on_dismiss)
    local overlay = root:Object {
        w = W, h = H, x = 0, y = 0,
        bg_color = "#000000", bg_opa = 128,
        border_width = 0, pad_all = 0,
    }
    overlay:clear_flag(lvgl.FLAG.SCROLLABLE)

    local box = overlay:Object {
        w = W - 20, h = H - 20,
        align = lvgl.ALIGN.CENTER,
        bg_color = "#333333", radius = 6,
        border_width = 1, border_color = "#555555",
        pad_all = 8,
        flex = { flex_direction = "column", flex_wrap = "nowrap" },
    }
    _nav_setup(box, GRIDNAV_ROLLOVER)

    local function info_label(text)
        box:Label { text = text, w = lvgl.PCT(100) }
    end

    info_label("-- Message Info --")
    info_label("From: " .. (msg.from or "?"))
    local ci_btn = box:Button { w = 100, h = 22 }
    ci_btn:Label { text = "Contact Info", align = lvgl.ALIGN.CENTER }
    ci_btn:onevent(lvgl.EVENT.RELEASED, function()
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

        local box2 = overlay2:Object {
            w = W - 10, h = H - 20,
            align = lvgl.ALIGN.CENTER,
            bg_color = "#333333", radius = 6,
            border_width = 1, border_color = "#555555",
            pad_all = 6,
            flex = { flex_direction = "column", flex_wrap = "nowrap" },
        }
        _nav_setup(box2, GRIDNAV_ROLLOVER)

        box2:Label { text = "-- Message Paths --", w = lvgl.PCT(100) }

        local paths = nil
        if msg.hash then
            local peer = msg.peer or msg.to or msg.from
            local ch = msg.is_dm and -1 or (msg.channel_idx or 0)
            local ok2, result = pcall(_mesh_get_message_paths, msg.hash, ch, peer)
            if ok2 and result and #result > 0 then paths = result end
        end

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
            overlay2:delete()
            _nav_setup(box, GRIDNAV_ROLLOVER)
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
            overlay:delete()
            if on_dismiss then on_dismiss() end
            on_reply(msg)
        end)
    end

    local close_btn = box:Button { w = on_reply and lvgl.PCT(48) or lvgl.PCT(100), h = 26 }
    close_btn:Label { text = "Close", align = lvgl.ALIGN.CENTER }
    close_btn:onevent(lvgl.EVENT.RELEASED, function()
        overlay:delete()
        if on_dismiss then on_dismiss() end
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
local function build_conversations()
    local convos = {}
    local ok_ch, channels = pcall(_mesh_get_channels)
    if ok_ch and channels then
        for _, ch in ipairs(channels) do
            local hist = messages:getChannelHistory(ch.idx)
            if #hist == 0 and ch.idx == 0 then hist = messages:all() end
            local last = hist[#hist]
            convos[#convos + 1] = {
                kind = "channel", idx = ch.idx, name = ch.name,
                last = last, ts = last and last.timestamp or 0,
                unread = messages:unreadInChannel(ch.idx), count = #hist,
            }
        end
    end
    if (not ok_ch or not channels or #channels == 0) and #messages:all() > 0 then
        local last = messages:all()[#messages:all()]
        convos[#convos + 1] = {
            kind = "channel", idx = 0, name = "Public",
            last = last, ts = last and last.timestamp or 0,
            unread = messages:unreadInChannel(0), count = #messages:all(),
        }
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
                              GRIDNAV_ROLLOVER + GRIDNAV_SCROLL_FIRST)
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
        apps.go_home()
    end)
    ctrl("Channels", 72, function() show_channels() end)
    ctrl("Contacts", 70, function() show_contacts() end)
    ctrl("Node", 48, function() show_my_node() end)

    -- Scrollable conversation list (rows live here, not in the gridnav body).
    local list = body:Object {
        w = lvgl.PCT(100), h = H - HEADER_H - 36,
        border_width = 0, pad_all = 0,
        flex = { flex_direction = "column", flex_wrap = "nowrap" },
    }
    list:add_flag(lvgl.FLAG.CLICK_FOCUSABLE)

    local in_select = false
    list:onevent(lvgl.EVENT.RELEASED, function()
        if in_select then return end
        in_select = true
        _nav_setup(list, GRIDNAV_ROLLOVER, true)
    end)
    list:onevent(lvgl.EVENT.KEY, function()
        local key = lvgl.indev.get_act():get_key()
        if key == 113 then -- 'q' returns focus to the controls
            in_select = false
            _nav_setup(body, GRIDNAV_ROLLOVER + GRIDNAV_SCROLL_FIRST)
        end
    end)

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
            preview = truncate((c.last.from or "") .. ": " .. (c.last.text or ""), 24)
        end
        local left = row:Label { align = lvgl.ALIGN.LEFT_MID }
        left.text = prefix .. c.name .. (preview ~= "" and ("  " .. preview) or "")
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
            show_chat { type = "dm", name = c.name }
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
end

-- ── CHAT VIEW ───────────────────────────────────────────────────
show_chat = function(target)
    clear_view()
    current_mode = "chat"
    chat_target = target

    -- Clear unread for this thread now that it's open.
    if target.type == "channel" then
        messages:markChannelSeen(target.idx)
    elseif target.type == "dm" then
        messages:markDMSeen(target.name)
    end

    local me = self_name()
    -- Channel names already include their '#'; only DMs get an '@' marker.
    local title = (target.type == "dm") and ("@" .. target.name) or target.name
    set_header(title, "")

    local body = gridnav_body(root, HEADER_H, H - HEADER_H, GRIDNAV_ROLLOVER + GRIDNAV_SCROLL_FIRST)
    current_view = body

    -- Top buttons (narrow, wrap in first row)
    local back_btn = body:Button { w = 45, h = 20 }
    back_btn:Label { text = "Home", align = lvgl.ALIGN.CENTER }
    back_btn:onevent(lvgl.EVENT.RELEASED, function() show_inbox() end)

    if target.type == "dm" then
        local info_btn = body:Button { w = 45, h = 20 }
        info_btn:Label { text = "Info", align = lvgl.ALIGN.CENTER }
        info_btn:onevent(lvgl.EVENT.RELEASED, function() show_contact_detail(target.name) end)
    elseif target.type == "room" then
        local login_btn = body:Button { w = 50, h = 20 }
        login_btn:Label { text = "Login", align = lvgl.ALIGN.CENTER }
        login_btn:onevent(lvgl.EVENT.RELEASED, function()
            local ok = pcall(_mesh_login_room, target.name, "")
            set_header(title, ok and "Logging in.." or "Login fail")
        end)
    end

    -- Message scroll area (full width).
    local MSG_H = H - HEADER_H - 20 - 34 - 24
    msg_list = body:Object {
        w = lvgl.PCT(100), h = MSG_H,
        border_width = 0, pad_all = 2,
        flex = { flex_direction = "column", flex_wrap = "nowrap" },
    }
    msg_list:add_flag(lvgl.FLAG.CLICK_FOCUSABLE)

    local in_msg_select = false
    local textArea
    local context_menu_open = false
    local ack_labels = {}  -- own-DM msg -> its header label, for live ack updates

    msg_list:onevent(lvgl.EVENT.RELEASED, function()
        if context_menu_open then return end
        if in_msg_select then return end
        in_msg_select = true
        _nav_setup(msg_list, GRIDNAV_ROLLOVER, true)
    end)

    msg_list:onevent(lvgl.EVENT.KEY, function()
        local indev = lvgl.indev.get_act()
        local key = indev:get_key()
        if key == 113 then -- 'q' exits message selection
            in_msg_select = false
            _nav_setup(body, GRIDNAV_ROLLOVER + GRIDNAV_SCROLL_FIRST)
        end
    end)

    -- Build one chat bubble (a focusable direct child of msg_list).
    local function render_msg(msg)
        msg.seen = true
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

        -- Header line: sender + time (+ hop count for received). Signal detail
        -- (SNR/RSSI) lives in the long-press info popup, not the bubble.
        local hdr = is_me and "You" or (msg.from or "?")
        local meta = utils.clockHM(msg.timestamp)
        if not is_me and msg.hops and msg.hops > 0 then
            meta = meta .. "  " .. msg.hops .. "h"
        end
        -- Live sends carry msg.status (sent/delivered/failed); persisted/received
        -- messages don't, so they show no delivery word.
        local track_status = is_me and target.type == "dm" and msg.status ~= nil
        local head_text = hdr .. "  " .. meta
        if track_status then head_text = head_text .. "  " .. dm_status_text(msg.status) end
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
                context_menu_open = false
                if in_msg_select then
                    _nav_setup(msg_list, GRIDNAV_ROLLOVER, true)
                    _nav_set_focused(bubble)
                else
                    _nav_setup(body, GRIDNAV_ROLLOVER + GRIDNAV_SCROLL_FIRST)
                end
            end)
        end

        bubble:onevent(lvgl.EVENT.RELEASED, function()
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
                timer = lvgl.Timer {
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

    -- Load existing messages
    local history = {}
    if target.type == "channel" then
        history = messages:getChannelHistory(target.idx)
        if #history == 0 and target.idx == 0 then history = messages:all() end
    elseif target.type == "dm" then
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

    -- Auto-load older messages when scrolled to top.
    local load_start = start_idx
    msg_list:onevent(lvgl.EVENT.SCROLL_END, function()
        if load_start <= 1 then return end
        if msg_list:get_scroll_top() > 5 then return end
        local new_start = math.max(1, load_start - 20)
        for i = load_start - 1, new_start, -1 do
            local lbl = render_msg(history[i])
            lbl:move_to_index(0)
        end
        load_start = new_start
    end)

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
                .. "  " .. dm_status_text(m.status)
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
        end
        textArea.text = ""
    end

    textArea:onevent(lvgl.EVENT.KEY, function()
        local indev = lvgl.indev.get_act()
        local key = indev:get_key()
        if key == lvgl.KEY.ENTER then do_send() end
    end)

    -- Hold the input to open the clipboard menu (paste a copied card, etc.).
    textArea:onevent(lvgl.EVENT.LONG_PRESSED, function()
        local overlay = root:Object {
            w = W, h = H, x = 0, y = 0,
            bg_color = "#000000", bg_opa = 128, border_width = 0, pad_all = 0,
        }
        overlay:clear_flag(lvgl.FLAG.SCROLLABLE)
        local function close()
            overlay:delete()
            _nav_setup(body, GRIDNAV_ROLLOVER + GRIDNAV_SCROLL_FIRST)
        end
        local pbox = overlay:Object {
            w = W - 40, h = lvgl.SIZE_CONTENT, align = lvgl.ALIGN.CENTER,
            bg_color = "#333333", radius = 6,
            border_width = 1, border_color = "#555555", pad_all = 8,
            flex = { flex_direction = "column", flex_wrap = "nowrap" },
        }
        _nav_setup(pbox, GRIDNAV_ROLLOVER)
        pbox:Label { text = "Clipboard", w = lvgl.PCT(100), text_color = COL_META }

        local paste_b = pbox:Button { w = lvgl.PCT(100), h = 28 }
        paste_b:Label { text = "Paste", align = lvgl.ALIGN.CENTER }
        paste_b:onevent(lvgl.EVENT.RELEASED, function()
            if clipboard.has() then
                textArea.text = (textArea.text or "") .. clipboard.paste()
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
    local saved_view = current_view
    if saved_view then saved_view:clear_flag(lvgl.FLAG.CLICKABLE) end

    local overlay = root:Object {
        w = W, h = H, x = 0, y = 0,
        bg_color = "#000000", bg_opa = 128, border_width = 0, pad_all = 0,
    }
    overlay:clear_flag(lvgl.FLAG.SCROLLABLE)

    local function close_popup()
        overlay:delete()
        if saved_view then
            saved_view:add_flag(lvgl.FLAG.CLICKABLE)
            _nav_setup(saved_view, GRIDNAV_ROLLOVER)
        end
    end

    local box = overlay:Object {
        w = W - 20, h = H - 20, align = lvgl.ALIGN.CENTER,
        bg_color = "#333333", radius = 6,
        border_width = 1, border_color = "#555555", pad_all = 8,
        flex = { flex_direction = "column", flex_wrap = "nowrap" },
    }
    _nav_setup(box, GRIDNAV_ROLLOVER)

    -- Full-screen QR of our own contact card, scannable by the MeshCore app.
    local function show_qr()
        -- MeshCore app contact-import URI (NOT the raw advert biz-card the
        -- firmware's importCard uses): meshcore://contact/add?name=..&public_key=..&type=..
        local okc, ni = pcall(_mesh_get_node_info)
        if not okc or not ni or not ni.pubkey or ni.pubkey == "" then return false end
        local card = contact_uri(ni.name, ni.pubkey, 1)
        box:clear_flag(lvgl.FLAG.CLICKABLE)
        local qov = root:Object {
            w = W, h = H, x = 0, y = 0,
            bg_color = "#000000", bg_opa = 245, border_width = 0, pad_all = 0,
        }
        qov:clear_flag(lvgl.FLAG.SCROLLABLE)
        local function qclose()
            qov:delete()
            box:add_flag(lvgl.FLAG.CLICKABLE)
            _nav_setup(box, GRIDNAV_ROLLOVER)
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
        _nav_setup(qov, GRIDNAV_ROLLOVER)
        return true
    end

    local function info(text) box:Label { text = text, w = lvgl.PCT(100) } end

    info("-- My Node --")
    local ok, ni = pcall(_mesh_get_node_info)
    if ok and ni then
        my_name = ni.name
        info("Name: " .. (ni.name or "?"))
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
    local saved_view = current_view
    if saved_view then saved_view:clear_flag(lvgl.FLAG.CLICKABLE) end

    local overlay = root:Object {
        w = W, h = H, x = 0, y = 0,
        bg_color = "#000000", bg_opa = 128, border_width = 0, pad_all = 0,
    }
    overlay:clear_flag(lvgl.FLAG.SCROLLABLE)

    local function close_popup(refresh)
        overlay:delete()
        if saved_view then
            saved_view:add_flag(lvgl.FLAG.CLICKABLE)
            _nav_setup(saved_view, GRIDNAV_ROLLOVER)
        end
        if refresh and on_done then on_done() end
    end

    local box = overlay:Object {
        w = W - 20, h = H - 40, align = lvgl.ALIGN.CENTER,
        bg_color = "#333333", radius = 6,
        border_width = 1, border_color = "#555555", pad_all = 8,
        flex = { flex_direction = "column", flex_wrap = "nowrap" },
    }
    _nav_setup(box, GRIDNAV_ROLLOVER)

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
    local box = overlay:Object {
        w = 220, h = 100, align = lvgl.ALIGN.CENTER,
        border_width = 1, pad_all = 10,
        flex = { flex_direction = "column", flex_wrap = "nowrap" },
    }
    box:clear_flag(lvgl.FLAG.SCROLLABLE)
    _gridnav_add(box, GRIDNAV_ROLLOVER)
    lvgl.group.get_default():add_obj(box)
    box:Label { text = "Clear all contacts?", w = lvgl.PCT(100), h = 24 }
    local yes_btn = box:Button { w = lvgl.PCT(48), h = 32 }
    yes_btn:Label { text = "Yes", align = lvgl.ALIGN.CENTER }
    yes_btn:onevent(lvgl.EVENT.RELEASED, function()
        pcall(_mesh_clear_contacts); overlay:delete(); show_contacts()
    end)
    local no_btn = box:Button { w = lvgl.PCT(48), h = 32 }
    no_btn:Label { text = "No", align = lvgl.ALIGN.CENTER }
    no_btn:onevent(lvgl.EVENT.RELEASED, function() overlay:delete() end)
end

-- ── CONTACT SETTINGS POPUP ──────────────────────────────────────
-- Type-visibility toggles (the "exclude" filter) plus the Add / Clear actions
-- relocated off the main contacts row.
show_contact_settings = function()
    local saved_view = current_view
    if saved_view then saved_view:clear_flag(lvgl.FLAG.CLICKABLE) end

    local overlay = root:Object {
        w = W, h = H, x = 0, y = 0,
        bg_color = "#000000", bg_opa = 128, border_width = 0, pad_all = 0,
    }
    overlay:clear_flag(lvgl.FLAG.SCROLLABLE)

    -- rebuild=true re-enters show_contacts so toggle changes take effect.
    local function close_popup(rebuild)
        overlay:delete()
        if rebuild then
            show_contacts()
        elseif saved_view then
            saved_view:add_flag(lvgl.FLAG.CLICKABLE)
            _nav_setup(saved_view, GRIDNAV_ROLLOVER)
        end
    end

    local box = overlay:Object {
        w = W - 20, h = H - 20, align = lvgl.ALIGN.CENTER,
        bg_color = "#333333", radius = 6,
        border_width = 1, border_color = "#555555", pad_all = 8,
        flex = { flex_direction = "column", flex_wrap = "nowrap" },
    }
    _nav_setup(box, GRIDNAV_ROLLOVER)

    box:Label { text = "-- Contact Settings --", w = lvgl.PCT(100) }

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

    -- Do not add: stops the firmware from auto-adding these advert types as
    -- contacts in the first place (persisted via _prefs.no_add_mask). Checked =
    -- excluded. Gracefully no-ops if the firmware lacks the bridge yet.
    local no_users, no_reps, no_rooms, no_sensors = false, false, false, false
    local ok_na, x1, x2, x3, x4 = pcall(_mesh_get_no_add)
    if ok_na then no_users, no_reps, no_rooms, no_sensors = x1, x2, x3, x4 end
    local function persist_no_add()
        pcall(_mesh_set_no_add, no_users, no_reps, no_rooms, no_sensors)
    end

    box:Label { text = "Do not add:", w = lvgl.PCT(100), text_color = COL_META }
    toggle_btn("Users", function() return no_users end,
                        function(v) no_users = v; persist_no_add() end)
    toggle_btn("Repeaters", function() return no_reps end,
                        function(v) no_reps = v; persist_no_add() end)
    toggle_btn("Rooms", function() return no_rooms end,
                        function(v) no_rooms = v; persist_no_add() end)
    toggle_btn("Sensors", function() return no_sensors end,
                        function(v) no_sensors = v; persist_no_add() end)

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

-- ── CONTACTS VIEW ───────────────────────────────────────────────
show_contacts = function()
    clear_view()
    current_mode = "contacts"
    set_header("Contacts", "Contacts: " .. _mesh_get_num_contacts())

    -- Controls live in the gridnav body; rows live in a CLICK_FOCUSABLE scroll
    -- list with the same tap-to-arm / long-press scheme as the inbox and chat,
    -- so touch drags scroll the list instead of opening a contact.
    local body = gridnav_body(root, HEADER_H, H - HEADER_H,
                              GRIDNAV_ROLLOVER + GRIDNAV_SCROLL_FIRST)
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
        border_width = 0, pad_all = 0,
        flex = { flex_direction = "column", flex_wrap = "nowrap" },
    }
    list:add_flag(lvgl.FLAG.CLICK_FOCUSABLE)

    local in_select = false
    list:onevent(lvgl.EVENT.RELEASED, function()
        if in_select then return end
        in_select = true
        _nav_setup(list, GRIDNAV_ROLLOVER, true)
    end)
    list:onevent(lvgl.EVENT.KEY, function()
        local key = lvgl.indev.get_act():get_key()
        if key == 113 then -- 'q' returns focus to the controls
            in_select = false
            _nav_setup(body, GRIDNAV_ROLLOVER + GRIDNAV_SCROLL_FIRST)
        end
    end)

    local bind_click = scroll_aware_list(list)

    -- Tap/click to open; suppressed while the list is being scrolled.
    local function bind_row(row, c_name, c_type)
        bind_click(row, function()
            if c_type == 1 then show_chat { type = "dm", name = c_name }
            elseif c_type == 3 then show_chat { type = "room", name = c_name }
            else show_contact_detail(c_name) end
        end)
    end


    -- Contact rows (batched to avoid watchdog timeout with many contacts).
    local contacts = nil
    local batch_idx = 0
    local BATCH_SIZE = 6
    contact_rows = {}

    utils.loadingPopUpAdd(root, "contacts", function()
        if not contacts then
            local ok, raw = pcall(_mesh_get_contacts)
            if not ok or not raw then raw = {} end

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

            -- Sort purely by the selected criterion. Favourites aren't pinned —
            -- the Favorites mode filters to them; otherwise they sort like any
            -- other contact (the '*' prefix still marks them).
            table.sort(filtered, function(a, b)
                if contacts_sort == "name" then
                    return a.name:lower() < b.name:lower()
                elseif contacts_sort == "type" then
                    if (a.type or 0) ~= (b.type or 0) then return (a.type or 0) < (b.type or 0) end
                    return a.name:lower() < b.name:lower()
                end
                return (a.lastmod or 0) > (b.lastmod or 0)
            end)

            contacts = filtered
            if #contacts == 0 then
                local empty_msg
                if #raw == 0 then empty_msg = "No contacts. Send an Advert!"
                elseif fav_only then empty_msg = "No favorite contacts."
                else empty_msg = "No matching contacts." end
                list:Label { text = empty_msg, w = lvgl.PCT(100), h = 20 }
                return true
            end
            return false
        end

        local start_i = batch_idx * BATCH_SIZE + 1
        local end_i = math.min(start_i + BATCH_SIZE - 1, #contacts)
        for i = start_i, end_i do
            local c = contacts[i]
            local seen = (c.last_seen and c.last_seen > 0) and utils.relTime(c.last_seen) or ""
            local row = list:Button { w = lvgl.PCT(100), h = 24 }
            local left = row:Label { align = lvgl.ALIGN.LEFT_MID }
            left.text = (c.favorite and "* " or "") .. type_icon(c.type) .. c.name
            local right = row:Label { align = lvgl.ALIGN.RIGHT_MID, text = seen, text_color = COL_META }
            bind_row(row, c.name, c.type)
            contact_rows[c.name] = row
        end

        batch_idx = batch_idx + 1
        return end_i >= #contacts
    end)

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
        border_width = 0, pad_all = 4,
    }
    _nav_setup(body, GRIDNAV_ROLLOVER)
    current_view = body

    local back_btn = body:Button { w = 45, h = 22 }
    back_btn:Label { text = "Home", align = lvgl.ALIGN.CENTER }
    back_btn:onevent(lvgl.EVENT.RELEASED, function() show_inbox() end)

    -- Add channel: name (+ optional PSK). For #hashtag names the key is derived.
    local ch_input = body:Textarea {
        password_mode = false, one_line = true, text = "#",
        w = lvgl.PCT(46), h = 28,
    }
    ch_input:clear_flag(lvgl.FLAG.SCROLLABLE)

    local psk_input = body:Textarea {
        password_mode = false, one_line = true, placeholder_text = "PSK (opt)",
        w = lvgl.PCT(30), h = 28,
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

    for _, ch in ipairs(channels) do
        local unread = messages:unreadInChannel(ch.idx)
        local chat_btn = body:Button { w = ch.idx > 0 and lvgl.PCT(65) or lvgl.PCT(100), h = 24 }
        local lbl = chat_btn:Label { align = lvgl.ALIGN.LEFT_MID }
        lbl.text = ch.name .. (unread > 0 and ("  (" .. unread .. ")") or "")
        if unread > 0 then lbl:set { text_color = COL_ACCENT } end
        local ch_copy = { type = "channel", idx = ch.idx, name = ch.name }
        chat_btn:onevent(lvgl.EVENT.RELEASED, function() show_chat(ch_copy) end)

        if ch.idx > 0 then
            local del_btn = body:Button { w = 50, h = 24 }
            del_btn:Label { text = "Del", align = lvgl.ALIGN.CENTER }
            local ch_idx = ch.idx
            del_btn:onevent(lvgl.EVENT.RELEASED, function()
                _mesh_set_channel(ch_idx, "", ""); show_channels()
            end)
        end
    end
end

-- ── CONTACT DETAIL POPUP ────────────────────────────────────────
show_contact_detail = function(contact_name)
    local saved_view = current_view
    if saved_view then saved_view:clear_flag(lvgl.FLAG.CLICKABLE) end

    local overlay = root:Object {
        w = W, h = H, x = 0, y = 0,
        bg_color = "#000000", bg_opa = 128, border_width = 0, pad_all = 0,
    }
    overlay:clear_flag(lvgl.FLAG.SCROLLABLE)

    local function close_popup()
        overlay:delete()
        if saved_view then
            saved_view:add_flag(lvgl.FLAG.CLICKABLE)
            _nav_setup(saved_view, GRIDNAV_ROLLOVER)
        end
    end

    local box = overlay:Object {
        w = W - 20, h = H - 20, align = lvgl.ALIGN.CENTER,
        bg_color = "#333333", radius = 6,
        border_width = 1, border_color = "#555555", pad_all = 8,
        flex = { flex_direction = "column", flex_wrap = "nowrap" },
    }
    _nav_setup(box, GRIDNAV_ROLLOVER)

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
    local nm = box:Label { text = "Name: " .. contact.name, w = lvgl.PCT(100) }
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
    paths_btn:onevent(lvgl.EVENT.RELEASED, function()
        local ok2, paths = pcall(_mesh_get_contact_paths, contact.pubkey)
        local overlay2 = root:Object {
            w = W, h = H, x = 0, y = 0,
            bg_color = "#000000", bg_opa = 128, border_width = 0, pad_all = 0,
        }
        overlay2:clear_flag(lvgl.FLAG.SCROLLABLE)
        local box2 = overlay2:Object {
            w = W - 10, h = H - 20, align = lvgl.ALIGN.CENTER,
            bg_color = "#333333", radius = 6,
            border_width = 1, border_color = "#555555", pad_all = 6,
            flex = { flex_direction = "column", flex_wrap = "nowrap" },
        }
        _nav_setup(box2, GRIDNAV_ROLLOVER)
        box2:Label { text = "-- Paths: " .. contact.name .. " --", w = lvgl.PCT(100) }
        if not ok2 or not paths or #paths == 0 then
            box2:Label { text = "No path data", w = lvgl.PCT(100) }
        else
            for i, rec in ipairs(paths) do
                local chain = "Direct"
                if not rec.direct and rec.path and #rec.path > 0 then
                    chain = table.concat(rec.path, ">")
                elseif not rec.direct then chain = "Flood" end
                box2:Label {
                    text = string.format("#%d %s h:%d snr:%.0f rssi:%.0f",
                        i, rec.source or "?", rec.hops or 0, rec.snr or 0, rec.rssi or 0),
                    w = lvgl.PCT(100),
                }
                box2:Label { text = "  " .. chain, w = lvgl.PCT(100) }
                box2:Label {
                    text = string.format("  ok:%d fail:%d %dms",
                        rec.success or 0, rec.failure or 0, rec.trip_time_ms or 0),
                    w = lvgl.PCT(100),
                }
            end
        end
        local close_btn2 = box2:Button { w = lvgl.PCT(100), h = 26 }
        close_btn2:Label { text = "Close", align = lvgl.ALIGN.CENTER }
        close_btn2:onevent(lvgl.EVENT.RELEASED, function()
            overlay2:delete()
            _nav_setup(box, GRIDNAV_ROLLOVER)
        end)
    end)

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

    -- Login (room server)
    if contact.type == 3 then
        local login_btn = box:Button { w = lvgl.PCT(48), h = 26 }
        login_btn:Label { text = "Login", align = lvgl.ALIGN.CENTER }
        login_btn:onevent(lvgl.EVENT.RELEASED, function()
            local lok = pcall(_mesh_login_room, contact_name, "")
            set_header(contact_name, lok and "Logging in.." or "Login fail")
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
