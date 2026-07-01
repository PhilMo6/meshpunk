-- Mesh message module
-- Bridges C++ MeshCore <-> Lua UI
-- Enhanced for full MeshCore integration: multi-channel, DMs, rooms, contacts
--
-- Message persistence lives on the C++ PunkMesh side now, so any app can
-- access the same history. This module is a thin in-memory cache that:
--   * receives live dispatch from C++ (__dispatch / __dispatch_dm)
--   * pulls the persisted history once at app start via _mesh_get_* APIs
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
    -- The two grouped tables above are the SINGLE in-RAM source. The old flat
    -- __history / __dm_history duplicates were removed: __history only existed
    -- for the topbar's O(N) countUnread scan (now O(1) via the counters below)
    -- and the Public fallback (now reads __channel_history[0]); __dm_history was
    -- never read at all. Each list is capped (trim_list) so a busy mesh can't
    -- grow them unbounded — they share the PSRAM heap with LVGL's draw allocator
    -- and were starving it. loadPersisted seeds them from disk once at startup.
    -- Incremental unread counters so badges never rescan history. Bumped only by
    -- live dispatch (own echoes don't count); reset when the thread's chat opens.
    __channel_unread = {}, -- [idx]  = count
    __dm_unread = {},      -- [name] = count
}

-- Avoid double-loading if an app calls loadPersisted() more than once
M._loaded = false

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

-- Pull persisted history from C++ into the in-memory tables. Safe to call
-- multiple times; only the first call hits the C API.
function M:loadPersisted()
    if M._loaded then return end
    M._loaded = true

    -- Channel histories (slots 0..7; C++ ignores unknown slots gracefully)
    if _mesh_get_channel_messages then
        for i = 0, 7 do
            local ok, list = pcall(_mesh_get_channel_messages, i)
            if ok and type(list) == "table" and #list > 0 then
                M.__channel_history[i] = list
                trim_list(M.__channel_history[i])  -- days-retained files can exceed
            end                                    -- HIST_CAP; keep RAM bounded
        end
    end

    -- DM threads
    if _mesh_get_dm_threads and _mesh_get_dm_messages then
        local ok, names = pcall(_mesh_get_dm_threads)
        if ok and type(names) == "table" then
            for _, name in ipairs(names) do
                local ok2, list = pcall(_mesh_get_dm_messages, name)
                if ok2 and type(list) == "table" and #list > 0 then
                    M.__dm_threads[name] = list
                    trim_list(M.__dm_threads[name])  -- bound RAM (see above)
                end
            end
        end
    end
end

-- Drop the in-RAM message history loaded by loadPersisted (the per-channel and
-- per-DM lists — the big consumer: ~1.7MB on a busy mesh). Only the Messenger
-- needs it; it's freed when the Messenger closes so it isn't resident while the
-- heavy apps run. Safe because:
--   * C++ has every message persisted on disk → loadPersisted() rebuilds it
--     verbatim on the next Messenger open (and _loaded is reset so it re-runs).
--   * The unread COUNTERS are left intact (the topbar badge is counter-based),
--     so closing the Messenger never wrongly clears unread state.
--   * Live __dispatch after this just rebuilds the small per-session buckets
--     until the next loadPersisted replaces them with the full disk history.
function M:freePersisted()
    M.__channel_history = {}
    M.__dm_threads = {}
    M.__ack_index = {}          -- held refs into the freed history; drop them too
    M._loaded = false
    collectgarbage("collect")
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
    if not M.__channel_history[0] then M.__channel_history[0] = {} end
    table.insert(M.__channel_history[0], msg)
    trim_list(M.__channel_history[0])
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
    if not M.__dm_threads[name_prefix] then
        M.__dm_threads[name_prefix] = {}
    end
    table.insert(M.__dm_threads[name_prefix], msg)
    trim_list(M.__dm_threads[name_prefix])
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
    if not M.__channel_history[ch_idx] then
        M.__channel_history[ch_idx] = {}
    end
    table.insert(M.__channel_history[ch_idx], msg)
    trim_list(M.__channel_history[ch_idx])
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

    -- File into the per-channel bucket. Unknown-channel (-1) messages surface
    -- under Public (idx 0), both for display and the unread badge.
    local bucket = channel_idx >= 0 and channel_idx or 0
    if not M.__channel_history[bucket] then
        M.__channel_history[bucket] = {}
    end
    table.insert(M.__channel_history[bucket], msg)
    trim_list(M.__channel_history[bucket])
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

    -- Group into thread by sender name
    local key = msg.from
    if not M.__dm_threads[key] then
        M.__dm_threads[key] = {}
    end
    table.insert(M.__dm_threads[key], msg)
    trim_list(M.__dm_threads[key])
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

-- Get all DM thread names (contacts with active conversations)
function M:getDMThreadNames()
    local names = {}
    for name, thread in pairs(self.__dm_threads) do
        if #thread > 0 then
            table.insert(names, {
                name = name,
                count = #thread,
                unread = self.__dm_unread[name] or 0,
                last_msg = thread[#thread]
            })
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
