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
    __onMessage = nil,
    __onDirectMessage = nil,
    __onAnyMessage = nil,  -- fires for both channel and DM
    __onContactUpdate = nil,
    __history = {},
    __dm_history = {},
    __dm_threads = {},     -- grouped by contact name: {[name] = {msg, msg, ...}}
    __channel_history = {} -- grouped by channel idx: {[idx] = {msg, msg, ...}}
}

-- Avoid double-loading if an app calls loadPersisted() more than once
M._loaded = false

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
                if i == 0 then
                    for _, m in ipairs(list) do
                        table.insert(M.__history, m)
                    end
                end
            end
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
                    for _, m in ipairs(list) do
                        table.insert(M.__dm_history, m)
                    end
                end
            end
        end
    end
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

-- Send a public channel message via MeshCore
function M:broadcast(text)
    print("Broadcasting message: " .. text)
    local ok, err = _mesh_send_public(text)
    if not ok then
        print("Failed to send public message: " .. tostring(err))
        return false
    end

    -- Local echo: MeshCore doesn't loop back our own group messages, and
    -- C++ appendChannelMessage already persisted it — we just mirror it
    -- into the in-memory cache so the chat UI sees it immediately.
    local info = _mesh_get_node_info()
    local msg = {
        from = info and info.name or "me",
        text = text,
        timestamp = os.time(),
        direct = false,
        hops = 0,
        is_dm = false,
        channel_idx = 0,
        snr = 0,
        rssi = 0
    }
    table.insert(M.__history, msg)
    if not M.__channel_history[0] then M.__channel_history[0] = {} end
    table.insert(M.__channel_history[0], msg)
    if M.__onMessage then M.__onMessage(msg) end
    if M.__onAnyMessage then M.__onAnyMessage(msg) end

    return true
end

-- Send a direct message to a contact by name prefix
function M:sendDirect(name_prefix, text)
    local ok, route = _mesh_send_direct(name_prefix, text)
    if not ok then
        print("Failed to send DM: " .. tostring(route))
        return false
    end

    -- Local echo — C++ side already persisted it.
    local info = _mesh_get_node_info()
    local msg = {
        from = info and info.name or "me",
        text = text,
        timestamp = os.time(),
        direct = true,
        hops = 0,
        is_dm = true,
        to = name_prefix,
        snr = 0,
        rssi = 0
    }
    table.insert(M.__dm_history, msg)
    if not M.__dm_threads[name_prefix] then
        M.__dm_threads[name_prefix] = {}
    end
    table.insert(M.__dm_threads[name_prefix], msg)
    if M.__onDirectMessage then M.__onDirectMessage(msg) end
    if M.__onAnyMessage then M.__onAnyMessage(msg) end

    print("DM sent via " .. route)
    return true
end

-- Send a message to a specific channel by index
function M:sendToChannel(ch_idx, text)
    local ok, err = _mesh_send_channel(ch_idx, text)
    if not ok then
        print("Failed to send to channel: " .. tostring(err))
        return false
    end

    -- Local echo. Only the public channel (idx 0) uses M.__history — other
    -- channels live exclusively in M.__channel_history[ch_idx] so they
    -- don't bleed into the Public view's fallback history.
    local info = _mesh_get_node_info()
    local msg = {
        from = info and info.name or "me",
        text = text,
        timestamp = os.time(),
        direct = false,
        hops = 0,
        is_dm = false,
        channel_idx = ch_idx,
        snr = 0,
        rssi = 0
    }
    if ch_idx == 0 then
        table.insert(M.__history, msg)
    end
    if not M.__channel_history[ch_idx] then
        M.__channel_history[ch_idx] = {}
    end
    table.insert(M.__channel_history[ch_idx], msg)
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
-- Signature: __dispatch(from, text, timestamp, direct, hops, snr, rssi, channel_idx)
-- channel_idx: 0 = Public, 1..N = user-added channel slot, -1 = unknown/no match
-- C++ has already persisted the message before calling us.
function M.__dispatch(from, text, timestamp, direct, hops, snr, rssi, channel_idx)
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
        channel_idx = channel_idx
    }

    -- File into the right per-channel bucket. Public (idx 0) also feeds the
    -- legacy flat M.__history, which is what Public's chat view falls back to.
    if channel_idx == 0 or channel_idx == -1 then
        table.insert(M.__history, msg)
    end
    if channel_idx >= 0 then
        if not M.__channel_history[channel_idx] then
            M.__channel_history[channel_idx] = {}
        end
        table.insert(M.__channel_history[channel_idx], msg)
    end

    if M.__onMessage then M.__onMessage(msg) end
    if M.__onAnyMessage then M.__onAnyMessage(msg) end
end

-- Called from C++ for direct (private) messages
-- Signature: __dispatch_dm(from, text, timestamp, direct, hops, snr, rssi)
-- C++ has already persisted the message before calling us.
function M.__dispatch_dm(from, text, timestamp, direct, hops, snr, rssi)
    local msg = {
        from = from or "unknown",
        text = text,
        timestamp = timestamp,
        direct = direct,
        hops = hops,
        is_dm = true,
        snr = snr or 0,
        rssi = rssi or 0
    }

    table.insert(M.__dm_history, msg)

    -- Group into thread by sender name
    local key = msg.from
    if not M.__dm_threads[key] then
        M.__dm_threads[key] = {}
    end
    table.insert(M.__dm_threads[key], msg)

    if M.__onDirectMessage then M.__onDirectMessage(msg) end
    if M.__onAnyMessage then M.__onAnyMessage(msg) end
end

function M.__dispatch_contact(name, ctype)
    if M.__onContactUpdate then M.__onContactUpdate(name, ctype) end
end

-- Get all channel message history
function M:all()
    return self.__history
end

-- Get all DM history
function M:allDMs()
    return self.__dm_history
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
