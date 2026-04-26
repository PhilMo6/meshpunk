-- ══════════════════════════════════════════════════════════════════
-- New MeshCore Messenger - Full-featured multi-view messenger
-- Supports: channels, DMs, room servers, contact management
-- ══════════════════════════════════════════════════════════════════

local lvgl = require("lvgl")
local messages = require("lib/mesh/messages")

-- Persistence lives on the C++ PunkMesh side (respects _storage: LittleFS
-- root or /meshpunk on SD), so any app can access the same message history.
-- Cap applies per channel / per DM thread.
messages:setMaxMessages(100)
messages:loadPersisted()

local W = lvgl.HOR_RES()
local H = lvgl.VER_RES()
local HEADER_H = 24
local INPUT_H = 40
local BODY_H = H - HEADER_H - INPUT_H

-- Focus group for trackball navigation
local group = lvgl.group.get_default()

-- Helper: create a gridnav-enabled body container
-- All interactive widgets must be DIRECT children of this container.
-- Gridnav spatially navigates between clickable children using arrow keys.
local function gridnav_body(parent, y, h, flags)
    local body = parent:Object {
        flex = { flex_direction = "row", flex_wrap = "wrap" },
        w = W, h = h, y = y,
        border_width = 0, pad_all = 4,
    }
    body:clear_flag(lvgl.FLAG.SCROLLABLE)
    _gridnav_add(body, flags or GRIDNAV_ROLLOVER)
    group:add_obj(body)
    return body
end

-- Root container
local root = lvgl.Object()
root:set { w = W, h = H, pad_all = 0, border_width = 0 }
root:clear_flag(lvgl.FLAG.SCROLLABLE)

-- Current view state
local current_view = nil  -- the lvgl object for the body area
local current_input = nil -- the lvgl object for the input bar (if any)
local current_mode = "inbox" -- "inbox", "chat", "contacts", "channels", "contact_detail"
local chat_target = nil   -- {type="channel", idx=0, name="Public"} or {type="dm", name="alice"} or {type="room", name="room1"}

-- ── Header (always visible) ────────────────────────────────────
local header = root:Object {
    w = W, h = HEADER_H, y = 0,
    border_width = 0, pad_left = 4, pad_right = 4,
}
header:clear_flag(lvgl.FLAG.SCROLLABLE)

local header_title = header:Label { text = "Messenger", align = lvgl.ALIGN.LEFT_MID }
local header_right = header:Label { text = "", align = lvgl.ALIGN.RIGHT_MID }

local function set_header(title, right_text)
    header_title.text = title or "Messenger"
    header_right.text = right_text or ""
end

-- ── Helpers ─────────────────────────────────────────────────────
local function clear_view()
    if current_view then
        current_view:delete()
        current_view = nil
    end
    if current_input then
        current_input:delete()
        current_input = nil
    end
end

local function truncate(str, max)
    if not str then return "" end
    if #str <= max then return str end
    return string.sub(str, 1, max - 2) .. ".."
end


-- Forward declarations
local show_inbox, show_chat, show_contacts, show_channels, show_contact_detail

-- ── Periodic peer count update ──────────────────────────────────
local contactTimer = lvgl.Timer {
    period = 5000,
    cb = function(t)
        if current_mode == "inbox" then
            header_right.text = "Contact#:".. _mesh_get_num_contacts()
        end
    end
}


-- ── INBOX VIEW ──────────────────────────────────────────────────
show_inbox = function()
    clear_view()
    current_mode = "inbox"
    set_header("Messenger", _mesh_get_num_contacts() .. "p")

    -- Single gridnav body: all buttons are direct children
    -- Flex row wrap: narrow buttons share a row, wide buttons get own row
    local body = gridnav_body(root, HEADER_H, H - HEADER_H)
    current_view = body

    -- Nav buttons (narrow, wrap in top row)
    local back_btn = body:Button { w = 45, h = 24 }
    back_btn:Label { text = "Home", align = lvgl.ALIGN.CENTER }
    back_btn:onClicked(function()
        contactTimer:delete()
        root:delete()
        local launcher = require("launcher")
        launcher.create()
    end)

    local ch_btn = body:Button { w = 50, h = 24 }
    ch_btn:Label { text = "Chan", align = lvgl.ALIGN.CENTER }
    ch_btn:onClicked(function() show_channels() end)

    local ct_btn = body:Button { w = 50, h = 24 }
    ct_btn:Label { text = "DM+", align = lvgl.ALIGN.CENTER }
    ct_btn:onClicked(function() show_contacts() end)

    local adv_btn = body:Button { w = 55, h = 24 }
    adv_btn:Label { text = "Advert", align = lvgl.ALIGN.CENTER }
    adv_btn:onClicked(function() _mesh_send_advert() end)

    -- Channel rows (full width, each gets own row)
    local ok_ch, channels = pcall(_mesh_get_channels)
    if ok_ch and channels then
        for _, ch in ipairs(channels) do
            local ch_history = messages:getChannelHistory(ch.idx)
            local preview = ""
            if #ch_history > 0 then
                local last = ch_history[#ch_history]
                preview = truncate((last.from or "") .. ": " .. last.text, 30)
            end

            local row = body:Button { w = lvgl.PCT(100), h = 26 }
            row:Label {
                text = "#" .. ch.name .. (preview ~= "" and (" - " .. preview) or ""),
                align = lvgl.ALIGN.LEFT_MID,
            }
            local ch_copy = { type = "channel", idx = ch.idx, name = ch.name }
            row:onClicked(function() show_chat(ch_copy) end)
        end
    end

    -- Public channel fallback
    local pub_msgs = messages:all()
    if #pub_msgs > 0 and (not ok_ch or not channels or #channels == 0) then
        local row = body:Button { w = lvgl.PCT(100), h = 26 }
        local last = pub_msgs[#pub_msgs]
        row:Label {
            text = "#Public - " .. truncate((last.from or "") .. ": " .. last.text, 30),
            align = lvgl.ALIGN.LEFT_MID,
        }
        row:onClicked(function()
            show_chat({ type = "channel", idx = 0, name = "Public" })
        end)
    end

    -- DM threads (full width)
    local dm_threads = messages:getDMThreadNames()
    for _, thread in ipairs(dm_threads) do
        local preview = truncate(thread.last_msg.text or "", 28)
        local row = body:Button { w = lvgl.PCT(100), h = 26 }
        row:Label {
            text = "@" .. thread.name .. " (" .. thread.count .. ") " .. preview,
            align = lvgl.ALIGN.LEFT_MID,
        }
        local t_name = thread.name
        row:onClicked(function() show_chat({ type = "dm", name = t_name }) end)
    end

    -- Empty state
    local child_count = 0
    if ok_ch and channels then child_count = child_count + #channels end
    child_count = child_count + #dm_threads
    if child_count == 0 and #pub_msgs == 0 then
        body:Label {
            text = "No conversations yet.\nTap DM+ or send an Advert.",
            w = lvgl.PCT(100), h = 40,
        }
    end
end

-- ── CHAT VIEW ───────────────────────────────────────────────────
show_chat = function(target)
    clear_view()
    current_mode = "chat"
    chat_target = target

    local title = ""
    if target.type == "channel" then
        title = "#" .. target.name
    elseif target.type == "dm" then
        title = "@" .. target.name
    elseif target.type == "room" then
        title = "[" .. target.name .. "]"
    end
    set_header(title, "")

    -- Single gridnav body: buttons + msg list + input as direct children
    -- SCROLL_FIRST so UP/DOWN scrolls the focused msg_list before moving to next widget
    local body = gridnav_body(root, HEADER_H, H - HEADER_H, GRIDNAV_ROLLOVER + GRIDNAV_SCROLL_FIRST)
    current_view = body

    -- Top buttons (narrow, wrap in first row)
    local back_btn = body:Button { w = 45, h = 20 }
    back_btn:Label { text = "Back", align = lvgl.ALIGN.CENTER }
    back_btn:onClicked(function() show_inbox() end)

    local scroll_to_btn = body:Button { w = 100, h = 20 }
    scroll_to_btn:Label { text = "Scroll to bot", align = lvgl.ALIGN.RIGHT }
    scroll_to_btn:onClicked(function() msg_list:scroll_to({x = 10000, anim = false}) end)

    if target.type == "dm" then
        local info_btn = body:Button { w = 45, h = 20 }
        info_btn:Label { text = "Info", align = lvgl.ALIGN.CENTER }
        info_btn:onClicked(function() show_contact_detail(target.name) end)
    elseif target.type == "room" then
        local login_btn = body:Button { w = 50, h = 20 }
        login_btn:Label { text = "Login", align = lvgl.ALIGN.CENTER }
        login_btn:onClicked(function()
            local ok, route = pcall(_mesh_login_room, target.name, "")
            set_header(title, ok and "Logging in.." or "Login fail")
        end)
    end

    -- Message scroll area (full width). CLICK_FOCUSABLE (not CLICKABLE) so gridnav
    -- can focus it for scrolling without treating Enter as a click action.
    local MSG_H = H - HEADER_H - 20 - 34 - 24 -- room for top buttons + input + padding
    msg_list = body:Object {
        w = lvgl.PCT(100), h = MSG_H,
        border_width = 0, pad_all = 2,
        flex = { flex_direction = "column", flex_wrap = "nowrap" },
    }
    msg_list:add_flag(lvgl.FLAG.CLICK_FOCUSABLE)

    local function render_msg(msg)
        local prefix = msg.from or "?"
        local suffix = ""
        if msg.hops and msg.hops > 0 then
            suffix = " [" .. msg.hops .. "h]"
        end
        if msg.snr and msg.snr ~= 0 then
            suffix = suffix .. string.format(" %.0fdB", msg.snr)
        end
        local lbl = msg_list:Label {
            text = prefix .. ": " .. msg.text .. suffix,
            w = lvgl.PCT(100),
        }
        return lbl
    end

    -- Load existing messages
    local history = {}
    if target.type == "channel" then
        history = messages:getChannelHistory(target.idx)
        if #history == 0 and target.idx == 0 then
            history = messages:all()
        end
    elseif target.type == "dm" then
        history = messages:getDMThread(target.name)
    end
    -- Only render the most recent 20 messages to avoid blocking the UI
    -- on channels with long history (up to 100 messages).
    local last_lbl
    local start_idx = math.max(1, #history - 19)
    for i = start_idx, #history do last_lbl = render_msg(history[i]) end
    if last_lbl then last_lbl:scroll_to_view(false) end

    -- Auto-load older messages when the user scrolls to the top.
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

    -- Live message listener — only render messages for the channel we're viewing.
    -- channel_idx may be nil on locally-broadcast messages (broadcast() doesn't set it),
    -- in which case treat them as Public (idx 0).
    if target.type == "channel" then
        messages:onMessage(function(msg)
            local idx = msg.channel_idx or 0
            if idx == target.idx then
                local lbl = render_msg(msg)
                if lbl then lbl:scroll_to_view(false) end
            end
        end)
    elseif target.type == "dm" then
        messages:onDirectMessage(function(msg)
            if msg.from == target.name or msg.to == target.name then
                local lbl = render_msg(msg)
                if lbl then lbl:scroll_to_view(false) end
            end
        end)
    end

    -- Input row (textarea + send as direct children, wrap in bottom row)
    local ta = body:Textarea {
        password_mode = false, one_line = true,
        w = lvgl.PCT(75), h = 34,
    }

    local function do_send()
        local text = ta.text
        if not text or #text == 0 then return end
        if target.type == "channel" then
            if target.idx == 0 then messages:broadcast(text)
            else messages:sendToChannel(target.idx, text) end
        elseif target.type == "dm" then
            messages:sendDirect(target.name, text)
        elseif target.type == "room" then
            messages:sendDirect(target.name, text)
        end
        ta.text = ""
    end

    ta:onevent(lvgl.EVENT.KEY, function(obj, code)
        local indev = lvgl.indev.get_act()
        local key = indev:get_key()
        if key == lvgl.KEY.ENTER then do_send() end
    end)

    local send_btn = body:Button { w = lvgl.SIZE_CONTENT, h = 34 }
    send_btn:Label { text = "Send", align = lvgl.ALIGN.CENTER }
    send_btn:onClicked(do_send)
end

-- ── CONTACTS VIEW (DM picker) ───────────────────────────────────
show_contacts = function()
    clear_view()
    current_mode = "contacts"
    set_header("Contacts", "")

    local body = gridnav_body(root, HEADER_H, H - HEADER_H)
    current_view = body

    -- Top buttons (narrow, first row)
    local back_btn = body:Button { w = 45, h = 22 }
    back_btn:Label { text = "Back", align = lvgl.ALIGN.CENTER }
    back_btn:onClicked(function() show_inbox() end)

    local import_btn = body:Button { w = 60, h = 22 }
    import_btn:Label { text = "Import", align = lvgl.ALIGN.CENTER }
    import_btn:onClicked(function()
        set_header("Contacts", "Paste card in serial")
    end)

    -- Contact rows (full width)
    local ok, contacts = pcall(_mesh_get_contacts)
    if not ok or not contacts then contacts = {} end

    if #contacts == 0 then
        body:Label { text = "No contacts. Send an Advert!", w = lvgl.PCT(100), h = 20 }
    end

    for _, c in ipairs(contacts) do
        local type_icon = ""
        if c.type == 2 then type_icon = "[R] " end
        if c.type == 3 then type_icon = "[Room] " end
        if c.type == 4 then type_icon = "[S] " end

        local path_str = c.path_len >= 0 and (" h=" .. c.path_len) or " flood"

        local row = body:Button { w = lvgl.PCT(100), h = 24 }
        row:Label {
            text = type_icon .. c.name .. path_str,
            align = lvgl.ALIGN.LEFT_MID,
        }

        local c_name = c.name
        local c_type = c.type
        row:onClicked(function()
            if c_type == 1 then
                show_chat({ type = "dm", name = c_name })
            elseif c_type == 3 then
                show_chat({ type = "room", name = c_name })
            else
                show_contact_detail(c_name)
            end
        end)
    end
end

-- ── CHANNELS VIEW ───────────────────────────────────────────────
show_channels = function()
    clear_view()
    current_mode = "channels"
    set_header("Channels", "")

    local body = gridnav_body(root, HEADER_H, H - HEADER_H)
    current_view = body

    -- Top row
    local back_btn = body:Button { w = 45, h = 22 }
    back_btn:Label { text = "Back", align = lvgl.ALIGN.CENTER }
    back_btn:onClicked(function() show_inbox() end)

    -- Add channel input + button (narrow, share row)
    local ch_input = body:Textarea {
        password_mode = false, one_line = true,
        text = "#",
        w = lvgl.PCT(50), h = 28,
    }

    local add_btn = body:Button { w = 50, h = 28 }
    add_btn:Label { text = "Add", align = lvgl.ALIGN.CENTER }
    add_btn:onClicked(function()
        local name = ch_input.text
        if name and #name > 1 then
            local ok_ch, channels = pcall(_mesh_get_channels)
            local used = {}
            if ok_ch and channels then
                for _, ch in ipairs(channels) do used[ch.idx] = true end
            end
            for i = 1, 7 do  -- MAX_GROUP_CHANNELS is 8, slot 0 is Public
                if not used[i] then
                    _mesh_set_channel(i, name, "")
                    show_channels() 
                    return
                end
            end
            set_header("Channels", "All slots full!")
        end
    end)

    ch_input:onevent(lvgl.EVENT.KEY, function(obj, code)
        local indev = lvgl.indev.get_act()
        local key = indev:get_key()
        if key == lvgl.KEY.ENTER then
            add_btn:send_event(lvgl.EVENT.CLICKED, nil)
        end
    end)

    -- Channel rows (chat button + optional delete button, share a row per channel)
    local ok, channels = pcall(_mesh_get_channels)
    if not ok or not channels then channels = {} end

    for _, ch in ipairs(channels) do
        local chat_btn = body:Button { w = lvgl.PCT(65), h = 24 }
        chat_btn:Label {
            text = ch.name .. (ch.has_key and " *" or ""),
            align = lvgl.ALIGN.LEFT_MID,
        }
        local ch_copy = { type = "channel", idx = ch.idx, name = ch.name }
        chat_btn:onClicked(function() show_chat(ch_copy) end)

        if ch.idx > 0 then
            local del_btn = body:Button { w = 50, h = 24 }
            del_btn:Label { text = "Del", align = lvgl.ALIGN.CENTER }
            local ch_idx = ch.idx
            del_btn:onClicked(function()
                _mesh_set_channel(ch_idx, "", "")
                show_channels()
            end)
        end
    end
end

-- ── CONTACT DETAIL VIEW ─────────────────────────────────────────
show_contact_detail = function(contact_name)
    clear_view()
    current_mode = "contact_detail"
    set_header(contact_name, "")

    local body = gridnav_body(root, HEADER_H, H - HEADER_H)
    current_view = body

    -- Back button
    local back_btn = body:Button { w = 45, h = 22 }
    back_btn:Label { text = "Back", align = lvgl.ALIGN.CENTER }
    back_btn:onClicked(function()
        if chat_target and chat_target.type == "dm" and chat_target.name == contact_name then
            show_chat(chat_target)
        else
            show_contacts()
        end
    end)

    -- Find the contact
    local ok, contacts = pcall(_mesh_get_contacts)
    local contact = nil
    if ok and contacts then
        for _, c in ipairs(contacts) do
            if c.name == contact_name then contact = c; break end
        end
    end

    if not contact then
        body:Label { text = "Contact not found", w = lvgl.PCT(100), h = 20 }
        return
    end

    -- Info labels (not clickable, gridnav skips them)
    body:Label { text = "Name: " .. contact.name, w = lvgl.PCT(100), h = 16 }
    body:Label { text = "Type: " .. (contact.type_name or "?"), w = lvgl.PCT(100), h = 16 }
    body:Label { text = "Path: " .. (contact.path_len >= 0 and (contact.path_len .. " hops") or "flood"), w = lvgl.PCT(100), h = 16 }
    body:Label { text = "Key: " .. string.sub(contact.pubkey or "", 1, 16) .. "..", w = lvgl.PCT(100), h = 16 }

    -- Action buttons (narrow, wrap in rows)
    if contact.type == 1 then
        local dm_btn = body:Button { w = 55, h = 26 }
        dm_btn:Label { text = "DM", align = lvgl.ALIGN.CENTER }
        dm_btn:onClicked(function() show_chat({ type = "dm", name = contact_name }) end)
    end

    local share_btn = body:Button { w = 55, h = 26 }
    share_btn:Label { text = "Share", align = lvgl.ALIGN.CENTER }
    share_btn:onClicked(function()
        local ok2 = pcall(_mesh_share_contact, contact_name)
        set_header(contact_name, ok2 and "Shared!" or "Failed")
    end)

    local rp_btn = body:Button { w = 60, h = 26 }
    rp_btn:Label { text = "RstPath", align = lvgl.ALIGN.CENTER }
    rp_btn:onClicked(function()
        pcall(_mesh_reset_path, contact_name)
        set_header(contact_name, "Path reset")
    end)

    local exp_btn = body:Button { w = 55, h = 26 }
    exp_btn:Label { text = "Export", align = lvgl.ALIGN.CENTER }
    exp_btn:onClicked(function()
        local card = _mesh_export_contact(contact_name)
        if card then
            print("BIZ CARD: " .. card)
            set_header(contact_name, "See serial")
        else
            set_header(contact_name, "No card data")
        end
    end)

    local rm_btn = body:Button { w = 60, h = 26 }
    rm_btn:Label { text = "Remove", align = lvgl.ALIGN.CENTER }
    rm_btn:onClicked(function()
        pcall(_mesh_remove_contact, contact_name)
        show_contacts()
    end)
end

-- ── Initial view ────────────────────────────────────────────────
show_inbox()


