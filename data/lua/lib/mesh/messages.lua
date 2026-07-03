-- Mesh message module
-- Bridges C++ MeshCore <-> Lua UI
-- Enhanced for full MeshCore integration: multi-channel, DMs, rooms, contacts
--
-- Message persistence lives on the C++ PunkMesh side, so any app can access
-- the same history. This module keeps Lua's RESIDENT footprint tiny — the Lua
-- heap is a fixed PSRAM arena, and overflowing it spills objects into the
-- shared heap, re-fragmenting the big region the arena exists to protect:
--   * loadSummaries() pulls one {count, last} entry per conversation for the
--     inbox (a C-side file scan — no histories materialize in Lua)
--   * openThread()/closeThread() load ONE conversation's full history while
--     its chat view is open, and drop it again on the way out
--   * live dispatch from C++ (__dispatch / __dispatch_dm) appends only to an
--     open bucket, keeps the summaries fresh, and bumps the unread counters
--   * fires onMessage / onDirectMessage / onAnyMessage callbacks

local M = {
    __onMessageFirst = nil,
    __onMessage = nil,
    __onDirectMessage = nil,
    __onAnyMessage = nil,  -- fires for both channel and DM
    __onContactUpdate = nil,
    __onDirectMessageFirst = nil,
    __onMessageMention = nil,
    __onMessageMentionFirst = nil,
    __onAck = nil,         -- fires when a sent DM is delivered or fails
    __ack_index = {},      -- [expected_ack_crc] = sent msg (latest send only)
    __dm_threads = {},     -- grouped by contact name: {[name] = {msg, msg, ...}}
    __channel_history = {}, -- grouped by channel idx: {[idx] = {msg, msg, ...}}
    -- The two grouped tables above hold ONLY the open conversation's history
    -- (openThread loads it from disk for the chat view; closeThread drops it).
    -- They are windows, not stores: C++ persists every message before dispatch
    -- and the inbox runs on __summaries, so a busy mesh never accumulates
    -- megabytes of message tables in the Lua arena.
    __summaries = nil,     -- {channels={[idx]=e}, dms={[name]=e}} where e =
                           -- {kind, idx|name, count, last}; nil until
                           -- loadSummaries() (i.e. while the Messenger is closed)
    __open_thread = nil,   -- {kind="channel"|"dm", key=idx|name}: the loaded bucket
    -- Incremental unread counters so badges never rescan history. Bumped only by
    -- live dispatch (own echoes don't count); reset when the thread's chat opens.
    __channel_unread = {}, -- [idx]  = count
    __dm_unread = {},      -- [name] = count
}

-- Device RTC epoch (UTC). os.time() is NOT synced to the RTC on this firmware, so
-- never use it for message timestamps — the C side persists our sent messages with
-- the RTC clock, and this keeps the in-RAM echo consistent with that.
local function now_ts()
    local ok, t = pcall(_rtc_time)
    if ok and t and t > 0 then return t end
    return os.time()
end

-- Cap each in-RAM list so a busy mesh can't grow it without bound (it shares the
-- PSRAM heap with LVGL's draw allocator, which hard-crashes on alloc failure).
-- Mirrors the on-disk _max_messages cap; trims in a SLACK batch so the O(n) shift
-- amortizes to O(1) per message. Bump these if you raise the firmware cap.
local HIST_CAP = 400
local HIST_SLACK = 100
local function trim_list(list)
    local n = #list
    if n <= HIST_CAP + HIST_SLACK then return end
    local drop = n - HIST_CAP
    for i = 1, HIST_CAP do list[i] = list[i + drop] end
    for i = HIST_CAP + 1, n do list[i] = nil end
end

-- Configure the on-disk cap. Delegates to C++ (PunkMesh owns the files).
function M:setMaxMessages(n)
    n = tonumber(n)
    if not n or n <= 0 then return end
    if _mesh_set_max_messages then
        _mesh_set_max_messages(math.floor(n))
    end
end

-- ── Conversation summaries (the inbox model) ────────────────────────────────
-- One tiny entry per stored conversation ({count, last message}) built C-side
-- by _mesh_get_msg_summaries — a whole busy mesh is a few KB, where the old
-- loadPersisted materialized EVERY history (~1.7MB) and overflowed the Lua
-- arena into the shared PSRAM heap. Kept fresh by live dispatch below.
function M:loadSummaries()
    local sums = { channels = {}, dms = {} }
    local ok, list = pcall(_mesh_get_msg_summaries)
    if ok and type(list) == "table" then
        for _, e in ipairs(list) do
            if e.kind == "channel" then sums.channels[e.idx] = e
            elseif e.kind == "dm" then sums.dms[e.name] = e end
        end
    end
    M.__summaries = sums
end

-- Channel summary entry ({count, last}) or nil — inbox preview data.
function M:getChannelSummary(ch_idx)
    local s = M.__summaries
    return s and s.channels[ch_idx] or nil
end

-- Load ONE conversation's full history from disk into its bucket (the chat
-- view is its only consumer; trim_list bounds the RAM). Any previously open
-- thread is dropped first, so at most one history is ever resident. Room
-- chats keep their historical no-bucket behavior (live view only).
function M:openThread(target)
    M:closeThread()
    if target.type == "channel" then
        local ok, list = pcall(_mesh_get_channel_messages, target.idx)
        if ok and type(list) == "table" then
            trim_list(list)   -- days-retained files can exceed HIST_CAP
            M.__channel_history[target.idx] = list
        end
        M.__open_thread = { kind = "channel", key = target.idx }
    elseif target.type == "dm" then
        local ok, list = pcall(_mesh_get_dm_messages, target.name)
        if ok and type(list) == "table" then
            trim_list(list)
            M.__dm_threads[target.name] = list
        end
        M.__open_thread = { kind = "dm", key = target.name }
    end
end

function M:closeThread()
    local t = M.__open_thread
    if not t then return end
    if t.kind == "channel" then M.__channel_history[t.key] = nil
    else M.__dm_threads[t.key] = nil end
    M.__open_thread = nil
end

-- Drop everything a Messenger session loaded: the open bucket, the summaries
-- and the ack ref. Called on Messenger exit so nothing big stays resident in
-- the Lua arena; it all rebuilds from disk on the next open. The unread
-- COUNTERS are left intact (the topbar badge is counter-based), so closing
-- the Messenger never wrongly clears unread state.
function M:freePersisted()
    M.__channel_history = {}
    M.__dm_threads = {}
    M.__ack_index = {}
    M.__summaries = nil
    M.__open_thread = nil
    collectgarbage("collect")
end

-- Keep the loaded summaries in step with live traffic / local echoes so the
-- inbox stays current without reloading from disk. No-ops while summaries
-- aren't loaded (Messenger closed) — they rebuild from disk on the next open.
local function summary_touch_channel(idx, msg)
    local s = M.__summaries
    if not s then return end
    local e = s.channels[idx]
    if not e then
        e = { kind = "channel", idx = idx, count = 0 }
        s.channels[idx] = e
    end
    e.count = (e.count or 0) + 1
    e.last = msg
end

local function summary_touch_dm(name, msg)
    local s = M.__summaries
    if not s then return end
    local e = s.dms[name]
    if not e then
        e = { kind = "dm", name = name, count = 0 }
        s.dms[name] = e
    end
    e.count = (e.count or 0) + 1
    e.last = msg
end

-- Register priority callback (fires before onMessage, used by topbar)
function M:onMessageFirst(cb)
    M.__onMessageFirst = cb
end

-- Register callback for incoming channel messages
function M:onMessage(cb)
    M.__onMessage = cb
end

-- Register callback for incoming direct messages
function M:onDirectMessage(cb)
    M.__onDirectMessage = cb
end

-- Register callback for any message (channel or DM)
function M:onAnyMessage(cb)
    M.__onAnyMessage = cb
end

function M:onContactUpdate(cb)
    M.__onContactUpdate = cb
end

-- Register a callback for DM delivery results (delivered / failed). The
-- callback receives the sent msg table (with .status and .rtt updated).
function M:onAck(cb)
    M.__onAck = cb
end

function M:onDirectMessageFirst(cb)
    M.__onDirectMessageFirst = cb
end

function M:onMessageMention(cb)
    M.__onMessageMention = cb
end

function M:onMessageMentionFirst(cb)
    M.__onMessageMentionFirst = cb
end

-- Send a public channel message via MeshCore
function M:broadcast(text)
    print("Broadcasting message: " .. text)
    local ok, hash_hex = _mesh_send_public(text)
    if not ok then
        print("Failed to send public message: " .. tostring(hash_hex))
        return false
    end

    -- Local echo: MeshCore doesn't loop back our own group messages, and
    -- C++ appendChannelMessage already persisted it — we just mirror it
    -- into the in-memory cache so the chat UI sees it immediately.
    local info = _mesh_get_node_info()
    local msg = {
        from = info and info.name or "me",
        text = text,
        timestamp = now_ts(),
        direct = false,
        hops = 0,
        is_dm = false,
        channel_idx = 0,
        snr = 0,
        rssi = 0,
        hash = hash_hex
    }
    -- Echo into the bucket only when Public's chat is open (the bucket is the
    -- chat view's window); the summary keeps the inbox preview current.
    local list = M.__channel_history[0]
    if list then
        table.insert(list, msg)
        trim_list(list)
    end
    summary_touch_channel(0, msg)
    if M.__onMessageFirst then M.__onMessageFirst(msg) end
    if M.__onMessage then M.__onMessage(msg) end
    if M.__onAnyMessage then M.__onAnyMessage(msg) end

    return true
end

-- Send a direct message to a contact by name prefix
function M:sendDirect(name_prefix, text)
    local ok, route, ack, hash_hex = _mesh_send_direct(name_prefix, text)
    if not ok then
        print("Failed to send DM: " .. tostring(route))
        return false
    end

    -- Local echo — C++ side already persisted it. `ack` is the expected-ack
    -- CRC the firmware waits for; the delivery result arrives later via
    -- __dispatch_ack. hash_hex is only set for flood-routed DMs.
    local info = _mesh_get_node_info()
    local msg = {
        from = info and info.name or "me",
        text = text,
        timestamp = now_ts(),
        direct = true,
        hops = 0,
        is_dm = true,
        to = name_prefix,
        snr = 0,
        rssi = 0,
        hash = hash_hex,
        ack = ack,
        status = "sent",
    }
    -- Echo into the bucket only when this DM's chat is open; the summary
    -- keeps the inbox preview current. (Room sends land here too — rooms have
    -- no bucket, matching their no-history chat view.)
    local list = M.__dm_threads[name_prefix]
    if list then
        table.insert(list, msg)
        trim_list(list)
    end
    summary_touch_dm(name_prefix, msg)
    -- Firmware tracks one outstanding ack, so only the latest send can be
    -- confirmed; index just this one (bounded, no stale build-up).
    M.__ack_index = {}
    if ack and ack ~= 0 then M.__ack_index[ack] = msg end
    if M.__onDirectMessage then M.__onDirectMessage(msg) end
    if M.__onAnyMessage then M.__onAnyMessage(msg) end

    print("DM sent via " .. route)
    return true
end

-- Send a message to a specific channel by index
function M:sendToChannel(ch_idx, text)
    local ok, hash_hex = _mesh_send_channel(ch_idx, text)
    if not ok then
        print("Failed to send to channel: " .. tostring(hash_hex))
        return false
    end

    -- Local echo — C++ side already persisted it.
    local info = _mesh_get_node_info()
    local msg = {
        from = info and info.name or "me",
        text = text,
        timestamp = now_ts(),
        direct = false,
        hops = 0,
        is_dm = false,
        channel_idx = ch_idx,
        snr = 0,
        rssi = 0,
        hash = hash_hex
    }
    local list = M.__channel_history[ch_idx]
    if list then
        table.insert(list, msg)
        trim_list(list)
    end
    summary_touch_channel(ch_idx, msg)
    if M.__onMessageFirst then M.__onMessageFirst(msg) end
    if M.__onMessage then M.__onMessage(msg) end
    if M.__onAnyMessage then M.__onAnyMessage(msg) end

    return true
end

-- ── Channel management ──────────────────────────────────────────

function M:getChannels()
    return _mesh_get_channels()
end

function M:setChannel(idx, name, psk)
    return _mesh_set_channel(idx, name, psk or "")
end

function M:deleteChannel(idx)
    return _mesh_set_channel(idx, "", "")
end

-- ── Contact management ──────────────────────────────────────────

function M:getContacts()
    return _mesh_get_contacts()
end

function M:removeContact(name_prefix)
    return _mesh_remove_contact(name_prefix)
end

function M:resetPath(name_prefix)
    return _mesh_reset_path(name_prefix)
end

function M:exportContact(name_prefix)
    return _mesh_export_contact(name_prefix)
end

function M:importContact(card_hex)
    return _mesh_import_contact(card_hex)
end

function M:shareContact(name_prefix)
    return _mesh_share_contact(name_prefix)
end

-- ── Room server ─────────────────────────────────────────────────

function M:loginRoom(name_prefix, password)
    return _mesh_login_room(name_prefix, password)
end

function M:sendRequest(name_prefix, req_type)
    return _mesh_send_request(name_prefix, req_type)
end

-- ── Radio info ──────────────────────────────────────────────────

function M:getRxInfo()
    return _mesh_get_rx_info()
end

function M:getNodeInfo()
    return _mesh_get_node_info()
end

-- Called from C++ for channel messages
-- Signature: __dispatch(from, text, timestamp, direct, hops, snr, rssi, channel_idx, is_mention, path)
-- channel_idx: 0 = Public, 1..N = user-added channel slot, -1 = unknown/no match
-- path: array of hex relay hashes (may be nil or empty)
-- C++ has already persisted the message before calling us.
function M.__dispatch(from, text, timestamp, direct, hops, snr, rssi, channel_idx, is_mention, path, hash)
    if channel_idx == nil then channel_idx = -1 end
    local msg = {
        from = from or "unknown",
        text = text,
        timestamp = timestamp,
        direct = direct,
        hops = hops,
        is_dm = false,
        snr = snr or 0,
        rssi = rssi or 0,
        channel_idx = channel_idx,
        path = path or {},
        hash = hash
    }

    -- Unknown-channel (-1) messages surface under Public (idx 0), both for
    -- display and the unread badge. Append to the bucket only when that
    -- conversation's chat is open (openThread loaded it) — the bucket is a
    -- window, not a store; disk + the summaries carry everything else.
    local bucket = channel_idx >= 0 and channel_idx or 0
    local list = M.__channel_history[bucket]
    if list then
        table.insert(list, msg)
        trim_list(list)
    end
    summary_touch_channel(bucket, msg)
    M.__channel_unread[bucket] = (M.__channel_unread[bucket] or 0) + 1

    if M.__onMessageFirst then M.__onMessageFirst(msg) end
    if M.__onMessage then M.__onMessage(msg) end
    if is_mention then
        msg.is_mention = true
        if M.__onMessageMentionFirst then M.__onMessageMentionFirst(msg) end
        if M.__onMessageMention then M.__onMessageMention(msg) end
    end
    if M.__onAnyMessage then M.__onAnyMessage(msg) end
end

-- Called from C++ for direct (private) messages
-- Signature: __dispatch_dm(from, text, timestamp, direct, hops, snr, rssi, path)
-- path: array of hex relay hashes (may be nil or empty)
-- C++ has already persisted the message before calling us.
function M.__dispatch_dm(from, text, timestamp, direct, hops, snr, rssi, path, hash)
    local msg = {
        from = from or "unknown",
        text = text,
        timestamp = timestamp,
        direct = direct,
        hops = hops,
        is_dm = true,
        snr = snr or 0,
        rssi = rssi or 0,
        path = path or {},
        hash = hash
    }

    -- Thread key is the sender name. Append to the bucket only when this DM's
    -- chat is open; the summary keeps the inbox row current.
    local key = msg.from
    local list = M.__dm_threads[key]
    if list then
        table.insert(list, msg)
        trim_list(list)
    end
    summary_touch_dm(key, msg)
    M.__dm_unread[key] = (M.__dm_unread[key] or 0) + 1

    if M.__onDirectMessageFirst then M.__onDirectMessageFirst(msg) end
    if M.__onDirectMessage then M.__onDirectMessage(msg) end
    if M.__onAnyMessage then M.__onAnyMessage(msg) end
end

function M.__dispatch_contact(name, ctype)
    if M.__onContactUpdate then M.__onContactUpdate(name, ctype) end
end

-- Called from C++ when a sent DM is confirmed (rtt >= 0, round-trip ms) or
-- times out with no ack (rtt < 0). Correlated to the sent message by the
-- expected-ack CRC returned from _mesh_send_direct.
function M.__dispatch_ack(ack, rtt)
    local msg = M.__ack_index[ack]
    if not msg then return end
    M.__ack_index[ack] = nil
    if rtt and rtt >= 0 then
        msg.status = "delivered"
        msg.rtt = rtt
    else
        msg.status = "failed"
    end
    if M.__onAck then M.__onAck(msg) end
end

-- Total unread across all channels + DMs (O(threads), not O(messages)). Used by
-- the topbar's global mail badge. Own echoes never bump these counters.
function M:countUnread()
    local count = 0
    for _, c in pairs(M.__channel_unread) do count = count + c end
    for _, c in pairs(M.__dm_unread) do count = count + c end
    return count
end

-- Per-thread unread counts. "Unread" means "arrived live while you weren't
-- looking at that conversation" — counters are bumped by dispatch and reset
-- when the chat opens, so these reads are O(1) regardless of history size.
function M:unreadInChannel(ch_idx)
    return self.__channel_unread[ch_idx] or 0
end

function M:unreadInDM(name)
    return self.__dm_unread[name] or 0
end

-- O(1) badge reset — used by the open chat's live listener so a thread you're
-- actively viewing never accrues unread.
function M:clearUnreadChannel(ch_idx)
    self.__channel_unread[ch_idx] = 0
end

function M:clearUnreadDM(name)
    self.__dm_unread[name] = 0
end

-- Called once when a chat view opens. Clears the thread's unread counter, which
-- (summed) is the topbar's global badge — so opening a thread tidies it.
function M:markChannelSeen(ch_idx)
    self.__channel_unread[ch_idx] = 0
end

function M:markDMSeen(name)
    self.__dm_unread[name] = 0
end

-- Public-channel history (idx 0, which also holds unknown-channel messages).
-- Kept as an alias so existing Public-view fallbacks keep working.
function M:all()
    return self.__channel_history[0] or {}
end

-- Get DM thread for a specific contact
function M:getDMThread(contact_name)
    return self.__dm_threads[contact_name] or {}
end

-- All DM threads for the inbox ({name, count, unread, last_msg} per thread),
-- most recent first. Built from the summaries — no histories needed.
function M:getDMThreadNames()
    local names = {}
    local s = M.__summaries
    if s then
        for name, e in pairs(s.dms) do
            if (e.count or 0) > 0 and e.last then
                table.insert(names, {
                    name = name,
                    count = e.count,
                    unread = self.__dm_unread[name] or 0,
                    last_msg = e.last
                })
            end
        end
    end
    -- Sort by most recent message
    table.sort(names, function(a, b)
        return (a.last_msg.timestamp or 0) > (b.last_msg.timestamp or 0)
    end)
    return names
end

-- Get channel-specific history
function M:getChannelHistory(ch_idx)
    return self.__channel_history[ch_idx] or {}
end

return M
