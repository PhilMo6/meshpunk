-- Packets — live raw radio packet monitor.
-- Shows every frame the SX1262 hands up (including ones MeshCore rejects) and
-- our own transmissions, and can record them to a CSV stamped with the radio
-- settings in force. Frames come from the C capture ring: _mesh_pkt_capture()
-- arms it, _mesh_pkt_poll() drains it.
--
-- MEMORY SHAPE (the reason this app is built the way it is): a captured packet
-- is stored as ONE packed string, never as a Lua table and never as an LVGL
-- object. A table with ~10 fields costs ~290 B of hash nodes, plus its strings;
-- the packed string is ~280 B. Only PAGE rows exist as widgets at any moment —
-- an unbounded row list is what caused trouble in the messages and contacts
-- lists. Tables are rebuilt transiently, for rendering or inspection only.
local lvgl      = require("lvgl")
local apps      = require("lib/apps")
local nav       = require("lib/nav")
local theme     = require("lib/theme")
local utils     = require("lib/utils")
local fileman   = require("lib/fileman")
local clipboard = require("lib/clipboard")

local root = apps.new_root()
root:set { w = lvgl.HOR_RES(), h = lvgl.VER_RES(), pad_all = 0, border_width = 0, bg_opa = 0 }
root:clear_flag(lvgl.FLAG.SCROLLABLE)

theme.show_background()

local W = lvgl.HOR_RES()
local H = lvgl.VER_RES()

-- ── Wire format ─────────────────────────────────────────────────────────────
-- Mirrors Dispatcher::tryParsePacket (lib/MeshCore/src/Dispatcher.cpp):
--   [0]      header: route = bits 0-1, payload type = bits 2-5, ver = bits 6-7
--   [1..4]   transport codes, ONLY on the two transport route types
--   [next]   path_len byte: hash size = (b >> 6) + 1, hash count = b & 63
--   [...]    count * size path bytes
--   [rest]   payload

local ROUTE_NAME = { [0] = "TF", [1] = "F", [2] = "D", [3] = "TD" }
local ROUTE_LONG = {
    [0] = "TRANSPORT_FLOOD", [1] = "FLOOD",
    [2] = "DIRECT",          [3] = "TRANSPORT_DIRECT",
}

-- Short tag first (list rows), long name second (detail view).
local TYPES = {
    [0]  = { "REQ",  "REQ" },       [1]  = { "RESP", "RESPONSE" },
    [2]  = { "TXT",  "TXT_MSG" },   [3]  = { "ACK",  "ACK" },
    [4]  = { "ADV",  "ADVERT" },    [5]  = { "GTXT", "GRP_TXT" },
    [6]  = { "GDAT", "GRP_DATA" },  [7]  = { "AREQ", "ANON_REQ" },
    [8]  = { "PATH", "PATH" },      [9]  = { "TRCE", "TRACE" },
    [10] = { "MULT", "MULTIPART" }, [11] = { "CTRL", "CONTROL" },
    [15] = { "RAW",  "RAW_CUSTOM" },
}
local function type_tag(t)  return TYPES[t] and TYPES[t][1] or string.format("T%X", t) end
local function type_name(t) return TYPES[t] and TYPES[t][2] or string.format("unknown(%d)", t) end

-- Byte n (0-based) of a hex string, or nil past the end.
local function byte_at(hex, n)
    local i = n * 2 + 1
    if i + 1 > #hex then return nil end
    return tonumber(hex:sub(i, i + 1), 16)
end

-- Payload type without building the whole decode — used by the filter, which
-- runs on every captured frame.
local function peek_type(hex)
    local h = byte_at(hex, 0)
    if not h then return nil end
    return (h >> 2) & 0x0F
end

-- Split a captured frame into its header fields. Returns nil when the frame is
-- too short to hold what its own header claims — which is a real result here,
-- not an error: those are exactly the frames the monitor exists to show.
local function decode(hex)
    local h = byte_at(hex, 0)
    if not h then return nil end
    local d = {
        route = h & 0x03,
        ptype = (h >> 2) & 0x0F,
        ver   = (h >> 6) & 0x03,
    }
    local off = 1
    if d.route == 0 or d.route == 3 then
        -- Two uint16 codes occupy bytes 1..4, i.e. hex chars 3..10.
        if not byte_at(hex, 4) then return nil end
        d.transport = hex:sub(3, 10)
        off = 5
    end
    local pl = byte_at(hex, off)
    if not pl then return nil end
    d.hash_size  = (pl >> 6) + 1
    d.path_count = pl & 63
    off = off + 1
    local path_bytes = d.path_count * d.hash_size
    if (off + path_bytes) * 2 > #hex then return nil end
    d.path    = hex:sub(off * 2 + 1, (off + path_bytes) * 2)
    d.payload = hex:sub((off + path_bytes) * 2 + 1)
    return d
end

-- ── Clock ───────────────────────────────────────────────────────────────────
-- Device RTC is the truth; os.date("!...") formats a given epoch as UTC, so
-- adding the tz offset gives local wall clock without a second date library.
local tz_off = 0
do
    local ok, off = pcall(_rtc_tz_offset_minutes)
    if ok and off then tz_off = off * 60 end
end

-- "!" forces UTC formatting of the epoch we hand it, so the tz offset added
-- here is the only one applied.
local function stamp(ts, fmt)
    if not ts or ts < 1 then return "--:--:--" end
    local ok, s = pcall(os.date, "!" .. fmt, ts + tz_off)
    if ok and type(s) == "string" then return s end
    return tostring(ts)
end

-- ── Packed store ────────────────────────────────────────────────────────────
-- " "-separated, raw hex last (it is the only variable-length field, and hex
-- contains no spaces). SNR 9999 means "no radio stats" (a TX frame); score -1
-- means the hook had none; hash "-" means the frame never parsed.
local DIR_NAME = { [0] = "rx", [1] = "tx", [2] = "txfail" }
local NO_SNR   = 9999

local function pack(p)
    return table.concat({
        p.seq, p.ts, p.ms,
        p.dir == "tx" and 1 or (p.dir == "txfail" and 2 or 0),
        p.parsed and 1 or 0,
        -- snr*4 is exact (4 is a power of two); score*1000 is not, so round
        -- rather than floor or 0.875 can come back as 874.
        p.snr and math.floor(p.snr * 4) or NO_SNR,
        p.rssi or 0,
        p.score and math.floor(p.score * 1000 + 0.5) or -1,
        p.len,
        p.hash or "-",
        p.raw,
    }, " ")
end

local UNPACK_PAT =
    "^(%d+) (%d+) (%d+) (%d) (%d) (%-?%d+) (%-?%d+) (%-?%d+) (%d+) (%S+) (%S*)$"

local function unpack_pkt(s)
    local seq, ts, ms, dir, parsed, snrq4, rssi, score, len, hash, raw =
        s:match(UNPACK_PAT)
    if not seq then return nil end
    local p = {
        seq    = tonumber(seq),
        ts     = tonumber(ts),
        ms     = tonumber(ms),
        dir    = DIR_NAME[tonumber(dir)],
        parsed = parsed == "1",
        len    = tonumber(len),
        raw    = raw,
    }
    if hash ~= "-" then p.hash = hash end
    local sq = tonumber(snrq4)
    if sq ~= NO_SNR then
        p.snr  = sq / 4
        p.rssi = tonumber(rssi)
    end
    local sc = tonumber(score)
    if sc >= 0 then p.score = sc / 1000 end
    return p
end

-- ── State ───────────────────────────────────────────────────────────────────
local STORE_CAP = 400    -- packed packets held off-screen (~120 KB at ~280 B)
local PAGE      = 40     -- rows rendered at once; the hard LVGL object bound
local POLL_MS   = 120
local POLL_MAX  = 32

local pkts     = {}      -- packed strings, newest first, capped at STORE_CAP
local rows     = {}      -- rendered row objects, newest first, at most PAGE
local page     = 0       -- 0 = newest page and the only live one
local dropped  = 0       -- C-ring overwrites reported by the poller
local paused   = false   -- stop feeding the store (paging back sets this)
local full     = false   -- store hit STORE_CAP in "pause" mode

local filt = {
    rx      = true,      -- received and parsed
    rx_bad  = true,      -- received, tryParsePacket rejected it
    tx      = true,
    txfail  = true,
    types   = {},        -- payload type -> true; empty table = all types
    on_full = "overwrite",   -- or "pause"
}

local rec = { on = false, file = nil, path = nil, bytes = 0, cap = 0, rows = 0 }

-- One flat flex-wrap gridnav scope holds the toolbar AND the packet rows, so
-- the trackball can focus a row (the Files app convention — a nested list
-- container would put the rows outside the nav scope and make them touch-only).
-- header_n = how many toolbar children precede the rows, so a new row can be
-- moved to the top OF THE ROWS rather than above the toolbar.
local content_view, status_label, header_n
local show_main, show_detail, show_filters

local function toast(msg)
    pcall(utils.createNotification, root, tostring(msg), 2500)
end

local function page_count()
    if #pkts == 0 then return 1 end
    return math.floor((#pkts - 1) / PAGE) + 1
end

-- ── Filtering ───────────────────────────────────────────────────────────────
-- Applied at CAPTURE time, so a filter change does not retroactively reveal
-- packets already discarded — that is the point, it keeps them out of the store.
local function type_filter_active()
    return next(filt.types) ~= nil
end

local function passes(p)
    if p.dir == "tx" then
        if not filt.tx then return false end
    elseif p.dir == "txfail" then
        if not filt.txfail then return false end
    elseif p.parsed then
        if not filt.rx then return false end
    else
        if not filt.rx_bad then return false end
    end
    if type_filter_active() then
        local t = peek_type(p.raw)
        if not (t and filt.types[t]) then return false end
    end
    return true
end

-- ── Recording ───────────────────────────────────────────────────────────────
local CSV_HEAD =
    "seq,ts,ms,dir,parsed,snr,rssi,score,len,route,type,ver,path_len,path,hash,raw\n"

local function num(v, fmt)
    if v == nil then return "" end
    return string.format(fmt, v)
end

local function csv_row(p, d)
    return table.concat({
        p.seq, p.ts, p.ms, p.dir, p.parsed and 1 or 0,
        num(p.snr, "%.2f"), num(p.rssi, "%d"), num(p.score, "%.3f"),
        p.len,
        d and ROUTE_LONG[d.route] or "",
        d and type_name(d.ptype) or "",
        d and d.ver or "",
        d and d.path_count or "",
        d and d.path or "",
        p.hash or "",
        p.raw,
    }, ",") .. "\n"
end

-- SD is preferred: internal flash is small and shared with everything else.
local function log_dir()
    for _, dr in ipairs(fileman.drives()) do
        if dr.id == "S" and dr.mounted then return "S:/meshpunk/pktlog", 32 * 1024 * 1024 end
    end
    return "L:/meshpunk/pktlog", 4 * 1024 * 1024
end

local function rec_stop(reason)
    if rec.file then pcall(function() rec.file:close() end) end
    rec.file = nil
    rec.on = false
    if reason then toast(reason) end
end

local function rec_start()
    local dir, cap = log_dir()
    fileman.mkdir(fileman.parent(dir))
    fileman.mkdir(dir)

    local now = utils.now()
    local path = dir .. "/pkt_" .. stamp(now, "%Y%m%d_%H%M%S") .. ".csv"
    local f, err = io.open(path, "a")
    if not f then
        toast("Log open failed: " .. tostring(err))
        return
    end

    local info = {}
    pcall(function() info = _mesh_get_node_info() or {} end)
    local boost = false
    pcall(function() boost = _mesh_get_rx_boost() and true or false end)
    local phm = 0
    pcall(function() phm = _mesh_get_path_hash_mode() or 0 end)

    local head = table.concat({
        "# meshpunk packet log\n",
        string.format("# started=%s epoch=%d\n", stamp(now, "%Y-%m-%dT%H:%M:%S"), now),
        string.format("# node=%s pubkey=%s\n", info.name or "?", info.pubkey or ""),
        string.format("# freq=%.3f bw=%g sf=%d cr=%d tx_power=%d rx_boost=%d path_hash_mode=%d\n",
            info.freq or 0, info.bandwidth or 0, info.spreading_factor or 0,
            info.coding_rate or 0, info.tx_power or 0, boost and 1 or 0, phm),
        string.format("# fw=%s fw_api=%s\n", tostring(_FW_VERSION), tostring(_FW_API)),
        CSV_HEAD,
    })
    f:write(head)
    f:flush()

    rec.file, rec.path, rec.cap = f, path, cap
    rec.bytes, rec.rows, rec.on = #head, 0, true
end

-- ── Live rows ───────────────────────────────────────────────────────────────
local function row_text(p, d)
    local tag
    if p.dir == "tx" then
        tag = "TX "
    elseif p.dir == "txfail" then
        tag = "TX!"
    elseif p.parsed then
        tag = "RX "
    else
        tag = "RX?"
    end

    local body
    if d then
        body = string.format("%-2s %-4s %3dB p%d",
            ROUTE_NAME[d.route], type_tag(d.ptype), p.len, d.path_count)
    else
        body = string.format("   unparsed %3dB", p.len)
    end

    local radio = ""
    if p.snr then radio = string.format("  %.1f/%d", p.snr, p.rssi or 0) end

    return stamp(p.ts, "%H:%M:%S") .. " " .. tag .. " " .. body .. radio
end

local function row_color(p)
    if p.dir == "txfail" then return "#ff5555" end
    if p.dir == "tx" then return "#55b0ff" end
    if not p.parsed then return "#ffaa33" end
    return nil
end

local function update_status()
    if not status_label then return end
    local bits = { string.format("p%d/%d", page + 1, page_count()),
                   #pkts .. "/" .. STORE_CAP }
    if dropped > 0 then bits[#bits + 1] = dropped .. " lost" end
    if full then bits[#bits + 1] = "FULL"
    elseif paused then bits[#bits + 1] = "HOLD" end
    if rec.on then
        bits[#bits + 1] = string.format("REC %d (%dK)", rec.rows, math.floor(rec.bytes / 1024))
    end
    status_label.text = table.concat(bits, " | ")
end

-- Takes the PACKED string, not a table: the click closure then retains only a
-- reference to the string already living in `pkts`, so a rendered page costs no
-- extra Lua tables. The unpacked form here is transient, for the row text only.
-- `at_top` = a live arrival (goes above every existing row); otherwise the row
-- is appended, which is how a page renders oldest-last.
local function add_row(packed, at_top)
    if not content_view then return end
    local p = unpack_pkt(packed)
    if not p then return end
    local d = decode(p.raw)
    local b = content_view:Button { w = lvgl.PCT(100), h = 22, pad_all = 2 }
    local c = row_color(p)
    if c then
        b:Label { text = row_text(p, d), align = lvgl.ALIGN.LEFT_MID, text_color = c }
    else
        b:Label { text = row_text(p, d), align = lvgl.ALIGN.LEFT_MID }
    end
    b:onClicked(function() show_detail(packed) end)
    if at_top then
        b:move_to_index(header_n)
        table.insert(rows, 1, b)
        -- Hard bound on live widgets: past a full page the oldest row goes.
        -- This runs on the poll timer, never inside a gridnav event handler.
        while #rows > PAGE do
            local old = table.remove(rows)
            pcall(function() old:delete() end)
        end
    else
        rows[#rows + 1] = b
    end
end

-- ── Poll ────────────────────────────────────────────────────────────────────
local function poll()
    local ok, got, lost = pcall(_mesh_pkt_poll, POLL_MAX)
    if not ok or type(got) ~= "table" then return end
    if lost and lost > 0 then dropped = dropped + lost end
    if #got == 0 then
        if lost and lost > 0 then update_status() end
        return
    end

    local wrote = false
    for _, p in ipairs(got) do
        if passes(p) then
            -- Recording is independent of the in-RAM store: a full or held
            -- store still logs to disk.
            if rec.on and rec.file then
                local line = csv_row(p, decode(p.raw))
                local okw, n = pcall(function() return rec.file:write(line) end)
                if okw and n == #line then
                    rec.bytes = rec.bytes + #line
                    rec.rows = rec.rows + 1
                    wrote = true
                    if rec.bytes >= rec.cap then
                        rec_stop("Log size limit reached - recording stopped")
                    end
                else
                    rec_stop("Log write failed - recording stopped")
                end
            end

            if not paused and not full then
                if #pkts >= STORE_CAP then
                    if filt.on_full == "pause" then
                        full = true
                    else
                        table.remove(pkts)        -- ring: oldest falls off
                    end
                end
                if not full then
                    local packed = pack(p)
                    table.insert(pkts, 1, packed)
                    if page == 0 then add_row(packed, true) end
                end
            end
        end
    end

    if wrote and rec.file then pcall(function() rec.file:flush() end) end
    update_status()
end

-- ── Views ───────────────────────────────────────────────────────────────────
local vw

local function swap_view(builder)
    local old = vw
    -- The poll timer keeps running across views; drop the handles it writes
    -- through so it can never touch a widget the swap is about to delete.
    content_view, status_label = nil, nil
    rows = {}
    vw = root:Object {
        w = W, h = H, x = 0, y = 0,
        bg_opa = 0, border_width = 0, pad_all = 0, radius = 0,
    }
    vw:clear_flag(lvgl.FLAG.SCROLLABLE)
    builder(vw)
    if old then apps.delete_view(old) end
end

local function new_content(v)
    -- pad_column overrides the theme's PAD_SMALL (~8 px) gap. With 5 toolbar
    -- buttons the gaps alone ate 32 px of the 312 px content box, overflowing
    -- the row and wrapping buttons into the paging row below.
    local content = v:Object {
        w = W, h = H, x = 0, y = 0,
        bg_opa = 0, border_width = 0, pad_all = 4, pad_column = 2,
        flex = { flex_direction = "row", flex_wrap = "wrap" },
    }
    nav.replace(content, { flags = nav.ROLLOVER + nav.SCROLL_FIRST })
    return content
end

local function tool(content, txt, width, fn)
    local b = content:Button { w = width, h = 24 }
    local l = b:Label { text = txt, align = lvgl.ALIGN.CENTER }
    b:onClicked(function() fn(l) end)
    return l
end

-- Paging rebuilds the view rather than deleting rows in place: swap_view routes
-- the outgoing page through apps.delete_view's deferred, watchdog-safe teardown,
-- and this runs inside a gridnav event handler where a bulk delete is unsafe.
local function go_page(n)
    local last = page_count() - 1
    if n < 0 then n = 0 elseif n > last then n = last end
    if n == page then return end
    page = n
    -- Browsing history holds the store still, so the page under you cannot
    -- shift as new frames arrive. Run returns to the live page.
    if page > 0 then paused = true end
    show_main()
end

show_main = function()
    swap_view(function(v)
        local content = new_content(v)
        content_view = content
        header_n = 0

        -- Every toolbar element adds exactly one child of `content`; count them
        -- as they go so add_row's insert index can never drift out of sync.
        local function header(obj) header_n = header_n + 1 return obj end

        -- Row 1 sums to 92%, row 2 to 90%: the slack is what keeps each row
        -- from overflowing into the next once gaps are added.
        local rec_lbl
        rec_lbl = header(tool(content, rec.on and "Stop" or "Rec", lvgl.PCT(18), function()
            if rec.on then
                rec_stop()
                toast("Saved " .. tostring(rec.path))
            else
                rec_start()
                if rec.on then toast("Recording to " .. tostring(rec.path)) end
            end
            rec_lbl.text = rec.on and "Stop" or "Rec"
            update_status()
        end))

        -- Run always returns to the live page; that is the single way out of
        -- both a manual hold and the automatic one that paging back applies.
        header(tool(content, paused and "Run" or "Hold", lvgl.PCT(18), function()
            paused = not paused
            if not paused then page = 0 end
            show_main()
        end))

        header(tool(content, "Filter", lvgl.PCT(20), function() show_filters() end))

        header(tool(content, "Clear", lvgl.PCT(18), function()
            pkts = {}
            dropped, page = 0, 0
            full, paused = false, false
            show_main()
        end))

        header(tool(content, "Home", lvgl.PCT(18), function() apps.go_home() end))

        header(tool(content, "<", lvgl.PCT(12), function() go_page(page - 1) end))

        local srow = content:Object { w = lvgl.PCT(66), h = lvgl.SIZE_CONTENT, pad_all = 4 }
        srow:clear_flag(lvgl.FLAG.SCROLLABLE)
        srow:clear_flag(lvgl.FLAG.CLICKABLE)
        -- No explicit width: the label sizes to its text and so never wraps.
        -- With a wrapping label the row's SIZE_CONTENT height would change as
        -- the status string grows, shunting the whole packet list every tick.
        status_label = srow:Label { text = "" }
        header(srow)

        header(tool(content, ">", lvgl.PCT(12), function() go_page(page + 1) end))

        -- Render this page only: at most PAGE widgets exist at any time.
        local first = page * PAGE + 1
        for i = first, math.min(first + PAGE - 1, #pkts) do
            add_row(pkts[i], false)
        end
        update_status()
    end)
end

-- 16 bytes per line, offset-prefixed, with the ASCII gutter a hex dump wants.
local function hex_dump(hex)
    local out = {}
    local bytes = math.floor(#hex / 2)
    for off = 0, bytes - 1, 16 do
        local n = math.min(16, bytes - off)
        local cols, ascii = {}, {}
        for i = 0, n - 1 do
            local b = byte_at(hex, off + i)
            cols[#cols + 1] = string.format("%02X", b)
            ascii[#ascii + 1] = (b >= 32 and b < 127) and string.char(b) or "."
        end
        out[#out + 1] = string.format("%04X  %-47s  %s",
            off, table.concat(cols, " "), table.concat(ascii))
    end
    return table.concat(out, "\n")
end

show_detail = function(packed)
    local p = unpack_pkt(packed)
    if not p then return end
    local d = decode(p.raw)
    swap_view(function(v)
        local content = new_content(v)

        tool(content, "Back", lvgl.PCT(31), function() show_main() end)
        tool(content, "Copy hex", lvgl.PCT(35), function()
            local ok = pcall(clipboard.copy, p.raw)
            toast(ok and "Frame hex copied" or "Copy failed")
        end)
        tool(content, "Home", lvgl.PCT(28), function() apps.go_home() end)

        local lines = {
            string.format("seq %d   %s", p.seq, stamp(p.ts, "%Y-%m-%d %H:%M:%S")),
            string.format("dir %s   parsed %s   len %d B",
                p.dir:upper(), p.parsed and "yes" or "NO", p.len),
        }
        if p.snr then
            lines[#lines + 1] = string.format("SNR %.2f dB   RSSI %d dBm", p.snr, p.rssi or 0)
        end
        if p.score then
            lines[#lines + 1] = string.format("score %.3f", p.score)
        end
        if p.hash then
            lines[#lines + 1] = "hash " .. p.hash
        end
        if d then
            lines[#lines + 1] = string.format("route %s (%d)", ROUTE_LONG[d.route], d.route)
            lines[#lines + 1] = string.format("type  %s (%d)", type_name(d.ptype), d.ptype)
            lines[#lines + 1] = string.format("ver   %d", d.ver)
            if d.transport then
                lines[#lines + 1] = "transport " .. d.transport
            end
            lines[#lines + 1] = string.format("path  %d hash%s x %d byte%s",
                d.path_count, d.path_count == 1 and "" or "es",
                d.hash_size, d.hash_size == 1 and "" or "s")
            if d.path_count > 0 then
                local hops = {}
                for i = 0, d.path_count - 1 do
                    hops[#hops + 1] = d.path:sub(i * d.hash_size * 2 + 1,
                                                 (i + 1) * d.hash_size * 2)
                end
                lines[#lines + 1] = "      " .. table.concat(hops, " > ")
            end
            lines[#lines + 1] = string.format("payload %d B", math.floor(#d.payload / 2))
        else
            lines[#lines + 1] = "header did not parse"
        end
        lines[#lines + 1] = ""
        lines[#lines + 1] = hex_dump(p.raw)

        local body = content:Object { w = lvgl.PCT(100), h = H - 42, pad_all = 6 }
        body:Label { text = table.concat(lines, "\n"), w = lvgl.PCT(100) }
    end)
end

show_filters = function()
    swap_view(function(v)
        local content = new_content(v)

        tool(content, "Back", lvgl.PCT(48), function() show_main() end)
        tool(content, "All types", lvgl.PCT(48), function()
            filt.types = {}
            show_filters()
        end)

        local function box(on) return on and "[x] " or "[ ] " end

        local function heading(text)
            -- SIZE_CONTENT, not a fixed height: a taller theme font would clip
            -- a fixed row (hw-hit — these headings came back vertically cut off).
            local r = content:Object { w = lvgl.PCT(100), h = lvgl.SIZE_CONTENT, pad_all = 4 }
            r:clear_flag(lvgl.FLAG.SCROLLABLE)
            r:clear_flag(lvgl.FLAG.CLICKABLE)
            r:Label { text = text, w = lvgl.PCT(100) }
        end

        local function level(key, text, width)
            local l
            l = tool(content, box(filt[key]) .. text, width, function()
                filt[key] = not filt[key]
                l.text = box(filt[key]) .. text
            end)
        end

        heading("Capture level")
        level("rx",     "RX",        lvgl.PCT(48))
        level("rx_bad", "RX bad",    lvgl.PCT(48))
        level("tx",     "TX",        lvgl.PCT(48))
        level("txfail", "TX failed", lvgl.PCT(48))

        heading(string.format("Store  %d/%d packets", #pkts, STORE_CAP))
        local fl
        local function full_text()
            return filt.on_full == "pause" and "When full: pause"
                                            or "When full: overwrite"
        end
        fl = tool(content, full_text(), lvgl.PCT(60), function()
            filt.on_full = (filt.on_full == "pause") and "overwrite" or "pause"
            if filt.on_full == "overwrite" then full = false end
            fl.text = full_text()
        end)
        tool(content, "Clear", lvgl.PCT(36), function()
            pkts = {}
            dropped, page = 0, 0
            full, paused = false, false
            show_filters()
        end)

        heading(type_filter_active() and "Payload types (filtered)"
                                      or "Payload types (all)")

        local order = {}
        for t in pairs(TYPES) do order[#order + 1] = t end
        table.sort(order)
        for _, t in ipairs(order) do
            local label = type_name(t)
            local l
            l = tool(content, box(filt.types[t]) .. label, lvgl.PCT(48), function()
                filt.types[t] = (not filt.types[t]) or nil
                l.text = box(filt.types[t]) .. label
            end)
        end
    end)
end

-- ── Lifecycle ───────────────────────────────────────────────────────────────
apps.set_on_close(function()
    rec_stop()
    pcall(_mesh_pkt_capture, false)
end)

local armed = false
pcall(function() armed = _mesh_pkt_capture(true) and true or false end)

show_main()
if not armed then
    status_label.text = "capture unavailable (ring alloc failed)"
else
    apps.add_timer { period = POLL_MS, cb = poll }
end

return root
