-- ══════════════════════════════════════════════════════════════════
-- New MeshCore Messenger - Full-featured multi-view messenger
-- Supports: channels, DMs, room servers, contact management
-- ══════════════════════════════════════════════════════════════════

local lvgl = require("lvgl")
local messages = require("lib/mesh/messages")
local utils = require("lib/utils")

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
    _nav_setup(body, flags or GRIDNAV_ROLLOVER)
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
local contact_rows = {}  -- name -> LVGL button object

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
    _nav_clear()
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

-- Long-press popup showing message metadata
local function show_msg_info(msg, on_reply, on_dismiss)
    local overlay = root:Object {
        w = W, h = H, x = 0, y = 0,
        bg_color = "#000000", bg_opa = 128,
        border_width = 0, pad_all = 0,
    }
    overlay:clear_flag(lvgl.FLAG.SCROLLABLE)

    local box = overlay:Object {
        w = W - 20, h = lvgl.SIZE_CONTENT,
        align = lvgl.ALIGN.CENTER,
        bg_color = "#333333", radius = 6,
        border_width = 1, border_color = "#555555",
        pad_all = 8,
        flex = { flex_direction = "column", flex_wrap = "nowrap" },
    }
    box:clear_flag(lvgl.FLAG.SCROLLABLE)
    _nav_setup(box, GRIDNAV_ROLLOVER)

    local function info_label(text)
        box:Label { text = text, w = lvgl.PCT(100) }
    end

    info_label("-- Message Info --")
    info_label("From: " .. (msg.from or "?"))
    info_label("Time: " .. (msg.timestamp and utils.formatTime(msg.timestamp) or "?"))
    info_label("Hops: " .. (msg.hops or "?"))
    info_label("SNR: " .. (msg.snr and string.format("%.1f dB", msg.snr) or "N/A"))
    info_label("RSSI: " .. (msg.rssi and string.format("%.0f dBm", msg.rssi) or "N/A"))
    info_label("Route: " .. (msg.direct and "Direct" or "Flood"))

    local btn_row = box:Object {
        flex = { flex_direction = "row", flex_wrap = "nowrap" },
        w = lvgl.PCT(100), h = 30, border_width = 0, pad_all = 2,
    }
    btn_row:clear_flag(lvgl.FLAG.SCROLLABLE)

    if on_reply then
        local reply_btn = btn_row:Button { w = lvgl.PCT(48), h = 26 }
        reply_btn:Label { text = "Reply", align = lvgl.ALIGN.CENTER }
        reply_btn:onevent(lvgl.EVENT.RELEASED,function()
            overlay:delete()
            if on_dismiss then on_dismiss() end
            on_reply(msg)
        end)
    end

    local close_btn = btn_row:Button { w = on_reply and lvgl.PCT(48) or lvgl.PCT(100), h = 26 }
    close_btn:Label { text = "Close", align = lvgl.ALIGN.CENTER }
    close_btn:onevent(lvgl.EVENT.RELEASED,function()
        overlay:delete()
        if on_dismiss then on_dismiss() end
    end)
end

-- ── Periodic peer count update ──────────────────────────────────
local contactTimer = lvgl.Timer {
    period = 5000,
    cb = function(t)
        if current_mode == "inbox" or current_mode =="contacts" then
            header_right.text = "Contact#:" .. _mesh_get_num_contacts()
        end
    end
}


-- ── INBOX VIEW ──────────────────────────────────────────────────
show_inbox = function()
    clear_view()
    current_mode = "inbox"
    set_header("Messenger", "Contact#:" .. _mesh_get_num_contacts())

    local body = root:Object {
        flex = { flex_direction = "row", flex_wrap = "wrap" },
        w = W, h = H - HEADER_H, y = HEADER_H,
        border_width = 0, pad_all = 4,
    }
    _nav_setup(body, GRIDNAV_ROLLOVER)
    current_view = body

    -- Nav buttons (narrow, wrap in top row)
    local back_btn = body:Button { w = 50, h = 24 }
    back_btn:Label { text = "Home", align = lvgl.ALIGN.CENTER }
    back_btn:onevent(lvgl.EVENT.RELEASED,function()
        utils.loadingPopUpAdd(nil, "Home", function()
            contactTimer:delete()
            messages:onMessage(nil)
            messages:onDirectMessage(nil)
            messages:onContactUpdate(nil)
            root:delete()
            local launcher = require("launcher")
            launcher.create()
            return true
        end)
    end)

    local ch_btn = body:Button { w = 70, h = 24 }
    ch_btn:Label { text = "Channels", align = lvgl.ALIGN.CENTER }
    ch_btn:onevent(lvgl.EVENT.RELEASED,function() show_channels() end)

    local ct_btn = body:Button { w = 70, h = 24 }
    ct_btn:Label { text = "Contacts", align = lvgl.ALIGN.CENTER }
    ct_btn:onevent(lvgl.EVENT.RELEASED,function() show_contacts() end)

    local adv_btn = body:Button { w = 55, h = 24 }
    adv_btn:Label { text = "Advert", align = lvgl.ALIGN.CENTER }
    adv_btn:onevent(lvgl.EVENT.RELEASED,function()
        local overlay = root:Object {
            w = W, h = H, x = 0, y = 0,
            bg_opa = 200, border_width = 0, pad_all = 0,
        }
        overlay:clear_flag(lvgl.FLAG.SCROLLABLE)

        local box = overlay:Object {
            w = 200, h = 150, align = lvgl.ALIGN.CENTER,
            border_width = 1, pad_all = 10,
            flex = { flex_direction = "column", flex_wrap = "nowrap" },
        }
        box:clear_flag(lvgl.FLAG.SCROLLABLE)
        _gridnav_add(box, GRIDNAV_ROLLOVER)
        local popup_group = lvgl.group.get_default()
        popup_group:add_obj(box)

        box:Label { text = "Send Advert", w = lvgl.PCT(100), h = 22 }

        local flood_btn = box:Button { w = lvgl.PCT(100), h = 28 }
        flood_btn:Label { text = "Flood", align = lvgl.ALIGN.TOP }
        flood_btn:onevent(lvgl.EVENT.RELEASED,function()
            pcall(_mesh_send_advert, "flood")
            overlay:delete()
        end)

        local zero_btn = box:Button { w = lvgl.PCT(100), h = 28 }
        zero_btn:Label { text = "Zero Hop", align = lvgl.ALIGN.TOP }
        zero_btn:onevent(lvgl.EVENT.RELEASED,function()
            pcall(_mesh_send_advert, "zerohop")
            overlay:delete()
        end)

        local cancel_btn = box:Button { w = lvgl.PCT(100), h = 28 }
        cancel_btn:Label { text = "Cancel", align = lvgl.ALIGN.TOP }
        cancel_btn:onevent(lvgl.EVENT.RELEASED,function()
            overlay:delete()
        end)
    end)

    --keep track of inbox rows for live updating later when messages are recived.
    local inboxRows = {}

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
                text = ch.name .. (preview ~= "" and (" - " .. preview) or ""),
                align = lvgl.ALIGN.LEFT_MID,
            }
            local ch_copy = { type = "channel", idx = ch.idx, name = ch.name, row = row }
            row:onevent(lvgl.EVENT.RELEASED,function() show_chat(ch_copy) end)

            inboxRows[ch.name] = ch_copy

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
        row:onevent(lvgl.EVENT.RELEASED,function()
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
        row:onevent(lvgl.EVENT.RELEASED,function() show_chat({ type = "dm", name = t_name }) end)
        inboxRows["@" .. thread.name] = { type = "dm", name = thread.name, row = row }
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



    -- Live message listener for inbox
    messages:onMessage(function(msg)
        if current_mode ~= "inbox" then return end
        local idx = msg.channel_idx or 0
        for _, entry in pairs(inboxRows) do
            if entry.type == "channel" and entry.idx == idx then
                entry.row:clean()
                entry.row:Label {
                    text = entry.name .. " - " .. truncate((msg.from or "") .. ": " .. msg.text, 30),
                    align = lvgl.ALIGN.LEFT_MID,
                }
                break
            end
        end
    end)

    messages:onDirectMessage(function(msg)
        if current_mode ~= "inbox" then return end
        local thread_name = msg.to or msg.from
        local key = "@" .. thread_name
        if inboxRows[key] then
            local entry = inboxRows[key]
            entry.row:clean()
            local dm_history = messages:getDMThread(thread_name)
            entry.row:Label {
                text = "@" .. thread_name .. " (" .. #dm_history .. ") " .. truncate(msg.text or "", 28),
                align = lvgl.ALIGN.LEFT_MID,
            }
        else
            local row = body:Button { w = lvgl.PCT(100), h = 26 }
            row:Label {
                text = "@" .. thread_name .. " (1) " .. truncate(msg.text or "", 28),
                align = lvgl.ALIGN.LEFT_MID,
            }
            row:onevent(lvgl.EVENT.RELEASED,function() show_chat({ type = "dm", name = thread_name }) end)
            inboxRows[key] = { type = "dm", name = thread_name, row = row }
        end
    end)

end

-- ── CHAT VIEW ───────────────────────────────────────────────────
show_chat = function(target)
    clear_view()
    current_mode = "chat"
    chat_target = target

    local title = target.name
    set_header(title, "")

    -- Single gridnav body: buttons + msg list + input as direct children
    -- SCROLL_FIRST so UP/DOWN scrolls the focused msg_list before moving to next widget
    local body = gridnav_body(root, HEADER_H, H - HEADER_H, GRIDNAV_ROLLOVER + GRIDNAV_SCROLL_FIRST)
    current_view = body

    -- Top buttons (narrow, wrap in first row)
    local back_btn = body:Button { w = 45, h = 20 }
    back_btn:Label { text = "Back", align = lvgl.ALIGN.CENTER }
    back_btn:onevent(lvgl.EVENT.RELEASED,function() show_inbox() end)

    if target.type == "dm" then
        local info_btn = body:Button { w = 45, h = 20 }
        info_btn:Label { text = "Info", align = lvgl.ALIGN.CENTER }
        info_btn:onevent(lvgl.EVENT.RELEASED,function() show_contact_detail(target.name) end)
    elseif target.type == "room" then
        local login_btn = body:Button { w = 50, h = 20 }
        login_btn:Label { text = "Login", align = lvgl.ALIGN.CENTER }
        login_btn:onevent(lvgl.EVENT.RELEASED,function()
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

    local in_msg_select = false
    local textArea
    local context_menu_open = false

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

    --[[
    msg_list:onevent(lvgl.EVENT.SCROLL, function()
        if in_msg_select and not _nav_is_active() then
            in_msg_select = false
            _nav_setup(body, GRIDNAV_ROLLOVER + GRIDNAV_SCROLL_FIRST)
        end
    end)
]]

    local function render_msg(msg)
        msg.seen = true
        local prefix = msg.from or "?"
        --[[local suffix = ""
        if msg.hops and msg.hops > 0 then
            suffix = " [" .. msg.hops .. "h]"
        end
        if msg.snr and msg.snr ~= 0 then
            suffix = suffix .. string.format(" %.0fdB", msg.snr)
        end]]
        local lbl = msg_list:Label {
            border_width = 1,
            pad_bottom = 6,
            text = prefix .. ": " .. msg.text,-- .. suffix,
            w = lvgl.PCT(100),
        }
        lbl:add_flag(lvgl.FLAG.CLICKABLE)
        lbl:add_flag(lvgl.FLAG.CLICK_FOCUSABLE)
        lbl:set_style({border_color = "#FFFFFF"}, lvgl.STATE.FOCUS_KEY)

        local function open_msg_menu()
            if context_menu_open then return end
            context_menu_open = true
            show_msg_info(msg, function(m)
                textArea.text = "@[" .. (m.from or "?") .. "] "
            end, function()
                context_menu_open = false
                if in_msg_select then
                    _nav_setup(msg_list, GRIDNAV_ROLLOVER, true)
                    _nav_set_focused(lbl)
                else
                    _nav_setup(body, GRIDNAV_ROLLOVER + GRIDNAV_SCROLL_FIRST)
                end
            end)
        end

        lbl:onevent(lvgl.EVENT.RELEASED, function()
            if in_msg_select then open_msg_menu() end
        end)
        lbl:onevent(lvgl.EVENT.LONG_PRESSED, open_msg_menu)
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
    textArea = body:Textarea {
        password_mode = false, one_line = true,
        w = lvgl.PCT(75), h = 34,
    }

    local function do_send()
        local text = textArea.text
        if not text or #text == 0 then return end
        if target.type == "channel" then
            if target.idx == 0 then messages:broadcast(text)
            else messages:sendToChannel(target.idx, text) end
        elseif target.type == "dm" then
            messages:sendDirect(target.name, text)
        elseif target.type == "room" then
            messages:sendDirect(target.name, text)
        end
        textArea.text = ""
    end

    textArea:onevent(lvgl.EVENT.KEY, function(obj, code)
        local indev = lvgl.indev.get_act()
        local key = indev:get_key()
        if key == lvgl.KEY.ENTER then do_send() end
    end)

    local send_btn = body:Button { w = lvgl.SIZE_CONTENT, h = 34 }
    send_btn:Label { text = "Send", align = lvgl.ALIGN.CENTER }
    send_btn:onevent(lvgl.EVENT.RELEASED,do_send)
end


-- ── CONTACTS VIEW ───────────────────────────────────
show_contacts = function()
    clear_view()
    current_mode = "contacts"
    set_header("Contacts", "Contact#:" .. _mesh_get_num_contacts())

    local body = root:Object {
        flex = { flex_direction = "row", flex_wrap = "wrap" },
        w = W, h = H - HEADER_H, y = HEADER_H,
        border_width = 0, pad_all = 4,
    }
    _nav_setup(body, GRIDNAV_ROLLOVER)
    current_view = body

    -- Top buttons (narrow, first row)
    local back_btn = body:Button { w = 45, h = 22 }
    back_btn:Label { text = "Back", align = lvgl.ALIGN.CENTER }
    back_btn:onevent(lvgl.EVENT.RELEASED,function() show_inbox() end)

    local clear_btn = body:Button { w = 50, h = 22 }
    clear_btn:Label { text = "Clear", align = lvgl.ALIGN.CENTER }
    clear_btn:onevent(lvgl.EVENT.RELEASED,function()
        local overlay = root:Object {
            w = W, h = H, x = 0, y = 0,
            bg_opa = 200, border_width = 0, pad_all = 0,
        }
        overlay:clear_flag(lvgl.FLAG.SCROLLABLE)

        local box = overlay:Object {
            w = 220, h = 100, align = lvgl.ALIGN.CENTER,
            border_width = 1, pad_all = 10,
            flex = { flex_direction = "column", flex_wrap = "nowrap" },
        }
        box:clear_flag(lvgl.FLAG.SCROLLABLE)
        _gridnav_add(box, GRIDNAV_ROLLOVER)
        local popup_group = lvgl.group.get_default()
        popup_group:add_obj(box)

        box:Label { text = "Clear all contacts?", w = lvgl.PCT(100), h = 24 }

        local btn_row = box:Object {
            flex = { flex_direction = "row", flex_wrap = "nowrap" },
            w = lvgl.PCT(100), h = 40, border_width = 0, pad_all = 4,
        }
        btn_row:clear_flag(lvgl.FLAG.SCROLLABLE)

        local yes_btn = btn_row:Button { w = lvgl.PCT(48), h = 32 }
        yes_btn:Label { text = "Yes", align = lvgl.ALIGN.CENTER }
        yes_btn:onevent(lvgl.EVENT.RELEASED,function()
            pcall(_mesh_clear_contacts)
            overlay:delete()
            show_contacts()
        end)

        local no_btn = btn_row:Button { w = lvgl.PCT(48), h = 32 }
        no_btn:Label { text = "No", align = lvgl.ALIGN.CENTER }
        no_btn:onevent(lvgl.EVENT.RELEASED,function()
            overlay:delete()
        end)
    end)

    -- Contact rows (sorted by last heard)
    -- Load in batches to avoid watchdog timeout with many contacts
    local contacts = nil
    local batch_idx = 0
    local BATCH_SIZE = 5
    contact_rows = {}

    utils.loadingPopUpAdd(root, "contacts", function()
        if not contacts then
            local ok, list = pcall(_mesh_get_contacts)
            if not ok or not list then list = {} end
            table.sort(list, function(a, b)
                return (a.lastmod or 0) > (b.lastmod or 0)
            end)
            contacts = list
            if #contacts == 0 then
                body:Label { text = "No contacts. Send an Advert!", w = lvgl.PCT(100), h = 20 }
                return true
            end
            return false
        end

        local start_i = batch_idx * BATCH_SIZE + 1
        local end_i = math.min(start_i + BATCH_SIZE - 1, #contacts)

        for i = start_i, end_i do
            local c = contacts[i]
            local type_icon = ""
            if c.type == 2 then type_icon = "[Rep] " end
            if c.type == 3 then type_icon = "[Room] " end
            if c.type == 4 then type_icon = "[S] " end

            local row = body:Button { w = lvgl.PCT(85), h = 24 }
            row:Label {
                text = type_icon .. c.name,
                align = lvgl.ALIGN.LEFT_MID,
            }

            local c_name = c.name
            local c_type = c.type
            row:onevent(lvgl.EVENT.RELEASED,function()
                if c_type == 1 then
                    show_chat({ type = "dm", name = c_name })
                elseif c_type == 3 then
                    show_chat({ type = "room", name = c_name })
                else
                    show_contact_detail(c_name)
                end
            end)
            contact_rows[c.name] = row
        end

        batch_idx = batch_idx + 1
        return end_i >= #contacts
    end)
    --we only need to assign the onContactUpdate function once this view page is open.
    messages:onContactUpdate(function(name, ctype)
        if current_mode ~= "contacts" then return end
        if not current_view then return end

        local type_icon = ""
        if ctype == 2 then type_icon = "[Rep] " end
        if ctype == 3 then type_icon = "[Room] " end
        if ctype == 4 then type_icon = "[S] " end
 
        if contact_rows[name] then
            contact_rows[name]:move_to_index(2)
        else
            local row = current_view:Button { w = lvgl.PCT(85), h = 24 }
            row:Label {
                text = type_icon .. name,
                align = lvgl.ALIGN.LEFT_MID,
            }
            local c_name = name
            local c_type = ctype
            row:onevent(lvgl.EVENT.RELEASED,function()
                if c_type == 1 then
                    show_chat({ type = "dm", name = c_name })
                elseif c_type == 3 then
                    show_chat({ type = "room", name = c_name })
                else
                    show_contact_detail(c_name)
                end
            end)
            row:move_to_index(2)
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

    -- Top row
    local back_btn = body:Button { w = 45, h = 22 }
    back_btn:Label { text = "Back", align = lvgl.ALIGN.CENTER }
    back_btn:onevent(lvgl.EVENT.RELEASED,function() show_inbox() end)

    -- Add channel input + button (narrow, share row)
    local ch_input = body:Textarea {
        password_mode = false, one_line = true,
        text = "#",
        w = lvgl.PCT(50), h = 28,
    }

    local add_btn = body:Button { w = 50, h = 28 }
    add_btn:Label { text = "Add", align = lvgl.ALIGN.CENTER }
    add_btn:onevent(lvgl.EVENT.RELEASED,function()
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
        local row = body:Object {
            flex = { flex_direction = "row", flex_wrap = "nowrap" },
            w = lvgl.PCT(100), h = 24, border_width = 0, pad_all = 0,
        }
        row:clear_flag(lvgl.FLAG.SCROLLABLE)

        local chat_btn = row:Button { w = lvgl.PCT(65), h = 24 }
        chat_btn:Label {
            text = ch.name .. (ch.has_key and " *" or ""),
            align = lvgl.ALIGN.LEFT_MID,
        }
        local ch_copy = { type = "channel", idx = ch.idx, name = ch.name }
        chat_btn:onevent(lvgl.EVENT.RELEASED,function() show_chat(ch_copy) end)

        if ch.idx > 0 then
            local del_btn = row:Button { w = 50, h = 24 }
            del_btn:Label { text = "Del", align = lvgl.ALIGN.CENTER }
            local ch_idx = ch.idx
            del_btn:onevent(lvgl.EVENT.RELEASED,function()
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
    back_btn:onevent(lvgl.EVENT.RELEASED,function()
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
    local fav_btn = body:Button { w = 80, h = 26 }
    local fav_label = fav_btn:Label { text = get_fav_text(), align = lvgl.ALIGN.CENTER }
    fav_btn:onevent(lvgl.EVENT.RELEASED,function()
        is_fav = not is_fav
        fav_label.text = get_fav_text()
        pcall(_mesh_set_contact_favorite, contact_name, is_fav)
    end)

    -- Action buttons (narrow, wrap in rows)
    if contact.type == 1 then
        local dm_btn = body:Button { w = 55, h = 26 }
        dm_btn:Label { text = "DM", align = lvgl.ALIGN.CENTER }
        dm_btn:onevent(lvgl.EVENT.RELEASED,function() show_chat({ type = "dm", name = contact_name }) end)
    end

    local share_btn = body:Button { w = 55, h = 26 }
    share_btn:Label { text = "Share", align = lvgl.ALIGN.CENTER }
    share_btn:onevent(lvgl.EVENT.RELEASED,function()
        local ok2 = pcall(_mesh_share_contact, contact_name)
        set_header(contact_name, ok2 and "Shared!" or "Failed")
    end)

    local rp_btn = body:Button { w = 60, h = 26 }
    rp_btn:Label { text = "RstPath", align = lvgl.ALIGN.CENTER }
    rp_btn:onevent(lvgl.EVENT.RELEASED,function()
        pcall(_mesh_reset_path, contact_name)
        set_header(contact_name, "Path reset")
    end)

    local exp_btn = body:Button { w = 55, h = 26 }
    exp_btn:Label { text = "Export", align = lvgl.ALIGN.CENTER }
    exp_btn:onevent(lvgl.EVENT.RELEASED,function()
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
    rm_btn:onevent(lvgl.EVENT.RELEASED,function()
        pcall(_mesh_remove_contact, contact_name)
        show_contacts()
    end)
end

-- ── Initial view ────────────────────────────────────────────────
show_inbox()


