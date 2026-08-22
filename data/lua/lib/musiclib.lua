--[[
  musiclib — music library discovery, indexing, caching and playlists for the
  MP3 player. Built on lib/fileman (file ops) and lib/id3 (tags). SD-only.

  Discovery is a bounded TASK (drive it from an LVGL timer, like fileman tasks)
  so scanning a big card never trips the watchdog:

      local task = musiclib.scan_task()
      -- each step() is ONE bounded unit (one dir listed, or one file tagged):
      local done, err = task.step()
      task.phase       -- "walk" | "tags" | "done"
      task.current     -- name of the item being processed
      task.found       -- audio files discovered so far
      task.tags_done   -- files tagged so far
      task.index       -- set once phase == "done" (also cached to disk)
      task.cancelled   -- set true to abort

  Index shape (see build_index):
      index.songs      -- flat array, sorted by artist/album/track/title
      index.artists    -- array { name=, albums = { {name=, songs={song,...}} } }
      index.count      -- #songs

  A song entry:
      { path=, name=, ext=, title=, artist=, album=, track= }

  Cache + playlists live under S:/Music (see constants below).
]]

local fileman = require("lib/fileman")
local id3     = require("lib/id3")

local M = {}

M.MUSIC_DIR    = "S:/Music"    -- library root: scanning + browsing live here
M.CACHE        = "S:/Music/.mp3lib"
M.PLAYLIST_DIR = "S:/Music/Playlists"

-- Extensions the ESP32-audioI2S decoder can play.
M.EXTS = { mp3 = true, wav = true, flac = true, aac = true, m4a = true }

-- Scan cap: every song costs Lua-arena memory (entry table + index refs), so an
-- enormous card must not OOM the scan. Past the cap discovery stops cleanly and
-- the task sets `capped = true` (the index just omits the overflow).
M.MAX_SONGS = 3000

local UNKNOWN_ARTIST = "Unknown Artist"
local UNKNOWN_ALBUM  = "Unknown Album"
local UNKNOWN_GENRE  = "Unknown Genre"

-- ── Small helpers ─────────────────────────────────────────────────────────────

local function ext_of(name)
    local e = name:match("%.([^.]+)$")
    return e and e:lower() or nil
end

local function stem_of(name)
    return (name:gsub("%.[^.]+$", ""))
end

local function is_audio(name)
    local e = ext_of(name)
    return e ~= nil and M.EXTS[e] == true
end

-- Ensure S:/Music and S:/Music/Playlists exist (mkdir creates parents).
function M.ensure_dirs()
    fileman.mkdir(M.MUSIC_DIR)
    fileman.mkdir(M.PLAYLIST_DIR)
end

-- Best display title: real tag title, else the filename without extension.
local function song_title(s)
    return s.title or stem_of(s.name)
end
M.song_title = song_title

-- ── Index build ───────────────────────────────────────────────────────────────

local function track_num(s)
    return tonumber(s.track and s.track:match("%d+")) or 9999
end

-- Group a flat song list into a sorted artist -> album -> songs tree.
function M.build_index(songs)
    table.sort(songs, function(a, b)
        local aa = (a.artist or UNKNOWN_ARTIST):lower()
        local ba = (b.artist or UNKNOWN_ARTIST):lower()
        if aa ~= ba then return aa < ba end
        local al = (a.album or UNKNOWN_ALBUM):lower()
        local bl = (b.album or UNKNOWN_ALBUM):lower()
        if al ~= bl then return al < bl end
        local at, bt = track_num(a), track_num(b)
        if at ~= bt then return at < bt end
        return song_title(a):lower() < song_title(b):lower()
    end)

    local artists, amap = {}, {}
    for _, s in ipairs(songs) do
        local an = s.artist or UNKNOWN_ARTIST
        local ar = amap[an]
        if not ar then
            ar = { name = an, albums = {}, _almap = {} }
            amap[an] = ar
            artists[#artists + 1] = ar
        end
        local ln = s.album or UNKNOWN_ALBUM
        local al = ar._almap[ln]
        if not al then
            al = { name = ln, songs = {} }
            ar._almap[ln] = al
            ar.albums[#ar.albums + 1] = al
        end
        al.songs[#al.songs + 1] = s
    end

    -- Genre buckets (songs stay in the already-sorted order).
    local genres, gmap = {}, {}
    for _, s in ipairs(songs) do
        local gn = s.genre or UNKNOWN_GENRE
        local g = gmap[gn]
        if not g then
            g = { name = gn, songs = {} }
            gmap[gn] = g
            genres[#genres + 1] = g
        end
        g.songs[#g.songs + 1] = s
    end
    table.sort(genres, function(a, b) return a.name:lower() < b.name:lower() end)

    return { songs = songs, artists = artists, genres = genres, count = #songs }
end

-- ── Scan task ─────────────────────────────────────────────────────────────────

-- Read one song's metadata. mp3 -> id3 tags; other formats -> filename only.
local function tag_song(path)
    local name = fileman.basename(path)
    local s = { path = path, name = name, ext = ext_of(name) }
    if s.ext == "mp3" then
        local meta = id3.read(path)
        s.title, s.artist, s.album, s.track, s.genre =
            meta.title, meta.artist, meta.album, meta.track, meta.genre
    end
    return s
end
M.tag_song = tag_song

function M.scan_task(root)
    M.ensure_dirs()                       -- so a fresh card still has S:/Music
    root = fileman.normalize(root or M.MUSIC_DIR)
    local t = {
        phase = "walk", current = "", found = 0, tags_done = 0,
        cancelled = false,
        _dirq = { root }, _files = {}, songs = {}, index = nil,
    }

    function t.step()
        if t.cancelled then return true, "cancelled" end

        if t.phase == "walk" then
            local dir = table.remove(t._dirq, 1)
            if dir == nil then
                t.phase = (#t._files > 0) and "tags" or "done"
                return false
            end
            t.current = fileman.basename(dir)
            local entries = fileman.list(dir, { sizes = false })
            if entries then
                for _, e in ipairs(entries) do
                    local child = fileman.join(dir, e.name)
                    if e.type == "dir" then
                        t._dirq[#t._dirq + 1] = child
                    elseif is_audio(e.name) and t.found < M.MAX_SONGS then
                        t._files[#t._files + 1] = child
                        t.found = t.found + 1
                    end
                end
            end
            if t.found >= M.MAX_SONGS then
                t._dirq = {}          -- stop walking; tag what we have
                t.capped = true
            end
            return false
        end

        if t.phase == "tags" then
            local path = t._files[t.tags_done + 1]
            if not path then t.phase = "done"; return false end
            t.current = fileman.basename(path)
            t.songs[#t.songs + 1] = tag_song(path)
            t.tags_done = t.tags_done + 1
            return false
        end

        -- done: build the index and persist it
        t.index = M.build_index(t.songs)
        M.save_cache(t.index)
        return true, nil
    end

    return t
end

-- ── Cache (TSV, escaped) ──────────────────────────────────────────────────────
-- One header line then one song per line: path<TAB>title<TAB>artist<TAB>album
-- <TAB>track<TAB>genre. Tabs/newlines/backslashes in fields are escaped so any
-- path or tag round-trips. Avoids executing a serialized Lua file.

local function esc(s)
    return (tostring(s or ""):gsub("\\", "\\\\"):gsub("\t", "\\t"):gsub("\n", "\\n"))
end

local function unesc(s)
    return (s:gsub("\\(.)", function(c)
        if c == "t" then return "\t" elseif c == "n" then return "\n" else return c end
    end))
end

function M.save_cache(index)
    M.ensure_dirs()
    local lines = { "MP3LIBv2\t" .. #index.songs }
    for _, s in ipairs(index.songs) do
        lines[#lines + 1] = table.concat({
            esc(s.path), esc(s.title), esc(s.artist), esc(s.album), esc(s.track), esc(s.genre),
        }, "\t")
    end
    return fileman.write(M.CACHE, table.concat(lines, "\n"))
end

-- Load a cached index, or nil if absent/invalid. Verifies each song still
-- exists is NOT done here (too slow); the player skips dead paths on play.
function M.load_cache()
    if not fileman.exists(M.CACHE) then return nil end
    local data = fileman.read(M.CACHE)
    if not data then return nil end
    local songs = {}
    local first = true
    for line in (data .. "\n"):gmatch("(.-)\n") do
        if first then
            if not line:match("^MP3LIBv2\t") then return nil end   -- old cache -> rescan
            first = false
        elseif line ~= "" then
            local f = {}
            for field in (line .. "\t"):gmatch("(.-)\t") do f[#f + 1] = unesc(field) end
            local path = f[1]
            if path and path ~= "" then
                local name = fileman.basename(path)
                songs[#songs + 1] = {
                    path = path, name = name, ext = ext_of(name),
                    title  = f[2] ~= "" and f[2] or nil,
                    artist = f[3] ~= "" and f[3] or nil,
                    album  = f[4] ~= "" and f[4] or nil,
                    track  = f[5] ~= "" and f[5] or nil,
                    genre  = (f[6] and f[6] ~= "") and f[6] or nil,
                }
            end
        end
    end
    if #songs == 0 then return nil end
    return M.build_index(songs)
end

function M.clear_cache()
    if fileman.exists(M.CACHE) then fileman.remove(M.CACHE) end
end

-- ── Playlists (.m3u under S:/Music/Playlists) ─────────────────────────────────

function M.list_playlists()
    M.ensure_dirs()
    local out = {}
    local entries = fileman.list(M.PLAYLIST_DIR, { sizes = false })
    if entries then
        for _, e in ipairs(entries) do
            if e.type == "file" and (ext_of(e.name) == "m3u") then
                out[#out + 1] = { name = stem_of(e.name), path = fileman.join(M.PLAYLIST_DIR, e.name) }
            end
        end
    end
    return out
end

function M.playlist_path(name)
    return fileman.join(M.PLAYLIST_DIR, (id3.safe_name(name) or "playlist") .. ".m3u")
end

-- Returns an array of song PATHS (comment/# lines skipped).
function M.load_playlist(path)
    local out = {}
    local data = fileman.read(path)
    if not data then return out end
    for raw in (data .. "\n"):gmatch("(.-)\n") do
        local line = raw:gsub("^%s+", ""):gsub("[\r%s]+$", "")
        if line ~= "" and line:sub(1, 1) ~= "#" then out[#out + 1] = line end
    end
    return out
end

local function serialize_m3u(paths)
    local lines = { "#EXTM3U" }
    for _, p in ipairs(paths) do lines[#lines + 1] = p end
    return table.concat(lines, "\n")
end

function M.save_playlist(name, paths)
    M.ensure_dirs()
    return fileman.write(M.playlist_path(name), serialize_m3u(paths))
end

-- Rewrite ONE playlist so any entry whose (normalized) path is a key in `moves`
-- is replaced by moves[path]. Writes in place (preserving the exact filename)
-- only if something changed. Returns true when the file was rewritten. This is
-- one bounded unit of work — callers processing many playlists should drive it
-- one-per-tick from a timer so a big list can't starve the watchdog.
function M.remap_one(pl, moves)
    local paths = M.load_playlist(pl.path)
    local changed = false
    for i, p in ipairs(paths) do
        local np = moves[fileman.normalize(p)]
        if np and np ~= p then paths[i] = np; changed = true end
    end
    if changed then fileman.write(pl.path, serialize_m3u(paths)) end
    return changed
end

-- Convenience for non-UI callers: remap every playlist at once (NOT watchdog-
-- bounded — the MP3 app drives remap_one per tick instead). Returns the count
-- of playlists changed.
function M.remap_playlists(moves)
    local updated = 0
    for _, pl in ipairs(M.list_playlists()) do
        if M.remap_one(pl, moves) then updated = updated + 1 end
    end
    return updated
end

-- Append one path to a playlist (creating it if needed), avoiding duplicates.
function M.playlist_add(name, songpath)
    return M.playlist_add_many(name, { songpath })
end

-- Append many paths at once (load + save the file only once), skipping any
-- already present. Returns ok, added_count.
function M.playlist_add_many(name, songpaths)
    local path = M.playlist_path(name)
    local paths = fileman.exists(path) and M.load_playlist(path) or {}
    local seen = {}
    for _, p in ipairs(paths) do seen[p] = true end
    local added = 0
    for _, p in ipairs(songpaths) do
        if not seen[p] then
            paths[#paths + 1] = p
            seen[p] = true
            added = added + 1
        end
    end
    local ok, err = M.save_playlist(name, paths)
    return ok, ok and added or err
end

return M
