--[[
  id3 — pure-Lua ID3 tag reader for the MP3 player.

  Reads track metadata WITHOUT loading the whole file: the ID3v2 tag at the
  start is walked frame-by-frame (each frame's data is seek()'d past, so a huge
  embedded-art APIC frame is skipped, never read), and if that yields nothing an
  ID3v1 tag in the last 128 bytes is tried. Depends on the esp32_file :seek
  method (added alongside this — see src/main.cpp).

      id3.read(path)  -> { title=, artist=, album=, track= }   (fields may be nil)
      id3.safe_name(s) -> filesystem-safe string (for the organizer)

  Only mp3 files carry ID3; callers fall back to the filename for other formats.
]]

local M = {}

-- ── Byte helpers ─────────────────────────────────────────────────────────────

-- 4 bytes, plain big-endian 32-bit.
local function be32(s, i)
    local a, b, c, d = s:byte(i, i + 3)
    return a * 0x1000000 + b * 0x10000 + c * 0x100 + d
end

-- 4 bytes, syncsafe (7 bits each, top bit always 0) — the ID3v2 size format.
local function syncsafe32(s, i)
    local a, b, c, d = s:byte(i, i + 3)
    return (a % 0x80) * 0x200000 + (b % 0x80) * 0x4000
         + (c % 0x80) * 0x80 + (d % 0x80)
end

-- 3 bytes, plain big-endian 24-bit (ID3v2.2 frame size).
local function be24(s, i)
    local a, b, c = s:byte(i, i + 2)
    return a * 0x10000 + b * 0x100 + c
end

-- Trim whitespace + NULs from a decoded tag string. (Lua 5.3+ dropped the %z
-- pattern class, so strip the literal null byte instead.)
local function clean(s)
    if not s then return nil end
    s = s:gsub("\0", ""):gsub("^%s+", ""):gsub("%s+$", "")
    if s == "" then return nil end
    return s
end

-- Collapse a UTF-16 (LE or BE) byte string to a printable Latin/ASCII-ish
-- string: ASCII code points pass through, anything else becomes '?'. Good
-- enough for on-screen display and building filenames.
local function utf16_flatten(data, big)
    local out, n = {}, #data
    local i = 1
    while i + 1 <= n do
        local lo, hi
        if big then hi, lo = data:byte(i), data:byte(i + 1)
        else        lo, hi = data:byte(i), data:byte(i + 1) end
        if hi == 0 and lo == 0 then break end          -- NUL terminator
        if hi == 0 and lo >= 32 and lo < 127 then out[#out + 1] = string.char(lo)
        else out[#out + 1] = "?" end
        i = i + 2
    end
    return table.concat(out)
end

-- Decode a text-frame body: first byte is the encoding, rest is the text.
local function decode_text(data)
    if not data or #data < 1 then return nil end
    local enc = data:byte(1)
    local body = data:sub(2)
    if enc == 1 then
        -- UTF-16 with BOM
        local b1, b2 = body:byte(1), body:byte(2)
        if b1 == 0xFF and b2 == 0xFE then return clean(utf16_flatten(body:sub(3), false))
        elseif b1 == 0xFE and b2 == 0xFF then return clean(utf16_flatten(body:sub(3), true))
        else return clean(utf16_flatten(body, false)) end
    elseif enc == 2 then
        return clean(utf16_flatten(body, true))       -- UTF-16BE, no BOM
    else
        return clean(body)                            -- 0=Latin-1, 3=UTF-8
    end
end

-- ── Genre resolution ─────────────────────────────────────────────────────────
-- The standard ID3v1 genre list (Winamp-extended). Stored 1-based, so numeric
-- genre N maps to GENRES[N + 1]. Slot for the historical index 133 (a slur in
-- the original spec) is deliberately neutralized to "Other".
local GENRES = {
    "Blues", "Classic Rock", "Country", "Dance", "Disco", "Funk", "Grunge",
    "Hip-Hop", "Jazz", "Metal", "New Age", "Oldies", "Other", "Pop", "R&B",
    "Rap", "Reggae", "Rock", "Techno", "Industrial", "Alternative", "Ska",
    "Death Metal", "Pranks", "Soundtrack", "Euro-Techno", "Ambient", "Trip-Hop",
    "Vocal", "Jazz+Funk", "Fusion", "Trance", "Classical", "Instrumental",
    "Acid", "House", "Game", "Sound Clip", "Gospel", "Noise", "Alternative Rock",
    "Bass", "Soul", "Punk", "Space", "Meditative", "Instrumental Pop",
    "Instrumental Rock", "Ethnic", "Gothic", "Darkwave", "Techno-Industrial",
    "Electronic", "Pop-Folk", "Eurodance", "Dream", "Southern Rock", "Comedy",
    "Cult", "Gangsta", "Top 40", "Christian Rap", "Pop/Funk", "Jungle",
    "Native American", "Cabaret", "New Wave", "Psychadelic", "Rave", "Showtunes",
    "Trailer", "Lo-Fi", "Tribal", "Acid Punk", "Acid Jazz", "Polka", "Retro",
    "Musical", "Rock & Roll", "Hard Rock", "Folk", "Folk-Rock", "National Folk",
    "Swing", "Fast Fusion", "Bebop", "Latin", "Revival", "Celtic", "Bluegrass",
    "Avantgarde", "Gothic Rock", "Progressive Rock", "Psychedelic Rock",
    "Symphonic Rock", "Slow Rock", "Big Band", "Chorus", "Easy Listening",
    "Acoustic", "Humour", "Speech", "Chanson", "Opera", "Chamber Music",
    "Sonata", "Symphony", "Booty Bass", "Primus", "Porn Groove", "Satire",
    "Slow Jam", "Club", "Tango", "Samba", "Folklore", "Ballad", "Power Ballad",
    "Rhythmic Soul", "Freestyle", "Duet", "Punk Rock", "Drum Solo", "A Cappella",
    "Euro-House", "Dance Hall", "Goa", "Drum & Bass", "Club-House", "Hardcore",
    "Terror", "Indie", "BritPop", "Other", "Polsk Punk", "Beat",
    "Christian Gangsta Rap", "Heavy Metal", "Black Metal", "Crossover",
    "Contemporary Christian", "Christian Rock", "Merengue", "Salsa",
    "Thrash Metal", "Anime", "Jpop", "Synthpop",
}

-- A TCON value may be a name ("Rock"), a numeric ref ("17" / "(17)"), or
-- "(17)Name". Resolve to a display name.
local function resolve_genre(s)
    if not s then return nil end
    local num, rest = s:match("^%((%d+)%)(.*)$")
    if num then
        rest = rest:gsub("^%s+", ""):gsub("%s+$", "")
        if rest ~= "" then return rest end
        return GENRES[tonumber(num) + 1] or nil
    end
    if s:match("^%d+$") then return GENRES[tonumber(s) + 1] or s end
    return s
end

-- ── ID3v2 (start of file) ────────────────────────────────────────────────────

-- Frame IDs we care about, mapped to result keys. Both the 4-char (v2.3/2.4)
-- and 3-char (v2.2) forms.
local WANT4 = { TIT2 = "title", TPE1 = "artist", TALB = "album", TRCK = "track", TCON = "genre" }
local WANT3 = { TT2  = "title", TP1  = "artist", TAL  = "album", TRK  = "track", TCO  = "genre" }

local function read_id3v2(f, out)
    local hdr = f:read(10)
    if not hdr or #hdr < 10 or hdr:sub(1, 3) ~= "ID3" then return false end

    local ver   = hdr:byte(4)          -- major version (2, 3 or 4)
    local flags = hdr:byte(6)
    local tagsz = syncsafe32(hdr, 7)   -- bytes of frames after this 10-byte header
    if tagsz <= 0 or tagsz > 4 * 1024 * 1024 then return false end

    local frames_end = 10 + tagsz      -- absolute file offset where frames stop

    -- Skip an extended header if present (rare). Bit 0x40 in flags.
    if flags % 0x80 >= 0x40 then
        local esz = f:read(4)
        if not esz or #esz < 4 then return false end
        local n = (ver >= 4) and (syncsafe32(esz, 1) - 4) or be32(esz, 1)
        if n > 0 then f:seek("cur", n) end
    end

    local got = 0
    while true do
        local pos = f:seek("cur", 0)
        if not pos or pos >= frames_end - 6 then break end

        local id, size, hdrlen
        if ver == 2 then
            local fh = f:read(6)
            if not fh or #fh < 6 then break end
            id = fh:sub(1, 3)
            size = be24(fh, 4)
            hdrlen = 6
        else
            local fh = f:read(10)
            if not fh or #fh < 10 then break end
            id = fh:sub(1, 4)
            size = (ver >= 4) and syncsafe32(fh, 5) or be32(fh, 5)
            hdrlen = 10
        end

        if id:byte(1) == 0 or size <= 0 then break end       -- padding / done
        if size > tagsz then break end                       -- corrupt: stop

        local key = (ver == 2) and WANT3[id] or WANT4[id]
        if key and not out[key] then
            -- Cap the read: a text frame is small; anything huge (shouldn't be a
            -- text frame) we still seek past below.
            local want = size
            if want > 1024 then want = 1024 end
            local data = f:read(want)
            if size > want then f:seek("cur", size - want) end
            local txt = decode_text(data)
            if key == "track" and txt then txt = txt:match("^(%d+)") or txt
            elseif key == "genre" and txt then txt = resolve_genre(txt) end
            if txt then
                out[key] = txt
                got = got + 1
                if got >= 5 then break end
            end
        else
            f:seek("cur", size)                              -- skip frame body
        end
    end
    return out.title or out.artist or out.album or out.track
end

-- ── ID3v1 (last 128 bytes) ───────────────────────────────────────────────────

local function read_id3v1(f, out)
    if not f:seek("end", -128) then return false end
    local t = f:read(128)
    if not t or #t < 128 or t:sub(1, 3) ~= "TAG" then return false end
    out.title  = out.title  or clean(t:sub(4, 33))
    out.artist = out.artist or clean(t:sub(34, 63))
    out.album  = out.album  or clean(t:sub(64, 93))
    -- ID3v1.1: if byte 126 is 0 and 127 is non-zero, 127 is the track number.
    if not out.track and t:byte(126) == 0 and t:byte(127) ~= 0 then
        out.track = tostring(t:byte(127))
    end
    -- Byte 128 (last) is the genre index; 255 = none.
    if not out.genre then
        local gb = t:byte(128)
        if gb and gb < #GENRES then out.genre = GENRES[gb + 1] end
    end
    return out.title or out.artist or out.album or out.genre
end

-- ── Xing/Info header (exact VBR duration) ────────────────────────────────────
-- The first MPEG frame of LAME/most encoders is a metadata frame carrying
-- "Xing" (VBR) or "Info" (CBR) with the file's exact FRAME COUNT. That gives
-- duration = frames * samples_per_frame / samplerate — exact, unlike the C
-- decoder's estimate (averaged from the first ~180 frames, then frozen; it
-- visibly "corrects" to a wrong value on VBR files). Returns whole seconds,
-- or nil when there is no Xing/Info header — the player then falls back to
-- latching the decoder's first estimate.

-- Samplerates by MPEG version bits (3=MPEG1, 2=MPEG2, 0=MPEG2.5).
local SR_TAB = {
    [3] = { 44100, 48000, 32000 },
    [2] = { 22050, 24000, 16000 },
    [0] = { 11025, 12000,  8000 },
}

local function read_xing(f)
    -- Skip the ID3v2 tag if present (same header math as read_id3v2).
    local start = 0
    local hdr = f:read(10)
    if hdr and #hdr == 10 and hdr:sub(1, 3) == "ID3" then
        start = 10 + syncsafe32(hdr, 7)
        if (hdr:byte(6) % 0x20) >= 0x10 then start = start + 10 end  -- footer flag
    end
    if not f:seek("set", start) then return nil end
    local win = f:read(4096)
    if not win or #win < 16 then return nil end

    -- Find a plausible Layer-III frame sync in the window (encoders pad, and
    -- junk can false-sync — keep scanning until the header fields validate).
    local i = 1
    while true do
        i = win:find("\255", i, true)
        if not i or i + 3 > #win then return nil end
        local b2, b3, b4 = win:byte(i + 1), win:byte(i + 2), win:byte(i + 3)
        if b2 >= 0xE0 then
            local mver  = math.floor(b2 / 8) % 4      -- version bits
            local layer = math.floor(b2 / 2) % 4      -- 1 = Layer III
            local sr_i  = math.floor(b3 / 4) % 4
            local srs   = SR_TAB[mver]
            if layer == 1 and srs and sr_i <= 2 then
                local sr   = srs[sr_i + 1]
                local mono = (math.floor(b4 / 64) % 4) == 3
                -- Xing/Info sits after the Layer-III side info: MPEG1 uses
                -- 17/32 bytes (mono/stereo), MPEG2/2.5 uses 9/17.
                local side = (mver == 3) and (mono and 17 or 32) or (mono and 9 or 17)
                local o = i + 4 + side                -- 1-based tag offset
                if o + 11 > #win then return nil end
                local tag = win:sub(o, o + 3)
                if tag ~= "Xing" and tag ~= "Info" then return nil end
                local flags = be32(win, o + 4)
                if not flags or flags % 2 ~= 1 then return nil end  -- no FRAMES field
                local frames = be32(win, o + 8)
                if not frames or frames == 0 then return nil end
                local spf = (mver == 3) and 1152 or 576   -- samples/frame, Layer III
                return math.floor(frames * spf / sr + 0.5)
            end
        end
        i = i + 1
    end
end

-- ── Public ───────────────────────────────────────────────────────────────────

function M.read(path)
    local out = {}
    local f = io.open(path, "r")
    if not f then return out end
    -- pcall so a malformed tag can never crash a library scan.
    pcall(read_id3v2, f, out)
    if not (out.title and out.artist) then pcall(read_id3v1, f, out) end
    f:close()
    return out
end

-- Exact duration in whole seconds from the Xing/Info header, or nil (no
-- header / not an MP3 / malformed) — see read_xing above.
function M.duration(path)
    local f = io.open(path, "r")
    if not f then return nil end
    local ok, dur = pcall(read_xing, f)
    f:close()
    if ok then return dur end
    return nil
end

-- Filesystem-safe name: drop path/illegal chars, collapse whitespace, trim,
-- and bound the length. Empty input -> nil so callers can fall back.
function M.safe_name(s)
    s = tostring(s or "")
    s = s:gsub("\0", "")
    s = s:gsub('[/\\:%*%?"<>|\r\n\t]', " ")
    s = s:gsub("%s+", " "):gsub("^%s+", ""):gsub("%s+$", "")
    s = s:gsub("%.+$", "")            -- no trailing dots (FAT dislikes them)
    if #s > 64 then s = s:sub(1, 64):gsub("%s+$", "") end
    if s == "" then return nil end
    return s
end

return M
