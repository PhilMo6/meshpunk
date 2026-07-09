--[[
  Music — a full-featured MP3 player for MeshPunk.

  Discovery + organization is built on lib/musiclib (which wraps lib/fileman and
  lib/id3); playback uses lib/sound (the ESP32-audioI2S file path). SD-only.

  Features:
    * Library discovery: bounded SD scan reading ID3 tags, cached to disk
    * Browse by Artist / Album / All Songs / Folders
    * Now Playing: title/artist/album, progress bar with tap-to-seek + ±15s,
      Prev / Play-Pause / Next, Shuffle, Repeat (off/one/all), volume
    * Play queue with auto-advance (background-safe across the app's views)
    * Playlists (.m3u under S:/Music/Playlists)
    * Organize: auto-sort files into Artist/Album/NN - Title by their tags,
      plus manual rename/move/delete/new-folder (reusing fileman)

  UI mirrors the Files app: one stable root, swap_view per screen, one flat
  flex-wrap nav scope per view, modals via nav.push/pop.
]]

local lvgl     = require("lvgl")
local apps     = require("lib/apps")
local nav      = require("lib/nav")
local theme    = require("lib/theme")
local utils    = require("lib/utils")
local fileman  = require("lib/fileman")
local sound    = require("lib/sound")
local id3      = require("lib/id3")
local musiclib = require("lib/musiclib")

local W = lvgl.HOR_RES()
local H = lvgl.VER_RES()

local MAX_SHOW = 150

-- ── App state ────────────────────────────────────────────────────────────────
local library    = nil          -- musiclib index, or nil until scanned/loaded

-- Playback state lives in ONE table so the background contract can carry it
-- across app exits (apps.register_background{ state = pb }): when the app is
-- relaunched over live background playback, it rebinds to the same table and
-- the views pick up mid-track. Everything UI stays in ordinary locals.
local bg_rec = apps.background_of("music")
local pb = (bg_rec and bg_rec.state) or {
    queue = {},            -- array of song entries
    order = {},            -- play order (indices into queue); shuffle-aware
    opos  = 0,             -- position within order
    cur_obj  = nil,        -- current sound object (owns the open file)
    cur_song = nil,
    playing  = false,
    paused   = false,
    shuffle  = false,
    repeat_mode = "off",   -- "off" | "one" | "all"
    dur_exact = nil,       -- Xing/Info-derived seconds (see track_duration)
    dur_latch = nil,       -- first decoder estimate, latched per track
}

-- Playlist multi-select picker. nil = normal (tap plays); otherwise a session:
--   { playlist = name, sel = { [path]=song }, list = { path,... }, count, bar_lbl }
-- When active the library views become a selector (tap toggles), with an
-- "Add N songs" / "Cancel" bar rendered by list_view.
local pick = nil

local root = apps.new_root()
root:set { w = W, h = H, pad_all = 0, border_width = 0, bg_opa = 0 }
root:clear_flag(lvgl.FLAG.SCROLLABLE)
theme.show_background()

local vw = nil                   -- current full-screen view
local np = nil                   -- Now Playing UI refs (nil unless that view is up)

local show_menu, show_nowplaying, show_library, show_queue, show_playlists,
      show_organize, show_folders, show_artist, show_album,
      show_genres, show_genre, show_search, show_artists   -- forward declarations
local song_row, update_pick_bar, commit_pick, cancel_pick, begin_pick  -- picker

local function toast(msg)
    pcall(utils.createNotification, root, tostring(msg), 2200)
end

local function swap_view(builder)
    np = nil                     -- leaving any view clears Now-Playing refs
    local old = vw
    vw = root:Object {
        w = W, h = H, x = 0, y = 0,
        bg_opa = 0, border_width = 0, pad_all = 0, radius = 0,
    }
    vw:clear_flag(lvgl.FLAG.SCROLLABLE)
    builder(vw)
    if old then apps.delete_view(old) end
end

-- ── Modal helpers (same pattern as the Files app) ─────────────────────────────
local function modal(box_opts, build)
    local overlay = root:Object {
        w = W, h = H, x = 0, y = 0,
        bg_color = "#000000", bg_opa = 140, border_width = 0, pad_all = 0, radius = 0,
    }
    overlay:clear_flag(lvgl.FLAG.SCROLLABLE)
    overlay:add_flag(lvgl.FLAG.CLICKABLE)
    local box = overlay:Object {
        w = box_opts.w or (W - 70),
        h = box_opts.h or lvgl.SIZE_CONTENT,
        align = lvgl.ALIGN.CENTER,
        radius = 6, border_width = 1, pad_all = 8,
        flex = { flex_direction = "column", flex_wrap = "nowrap" },
    }
    nav.push(box)
    local closed = false
    local function close()
        if closed then return end
        closed = true
        nav.pop()
        overlay:delete()
    end
    build(box, close)
    return close
end

local function prompt(title, initial, cb)
    modal({}, function(box, close)
        box:Label { text = title, w = lvgl.PCT(100), h = 18 }
        local ta = box:Textarea { one_line = true, text = initial or "", w = lvgl.PCT(100), h = 32 }
        ta:clear_flag(lvgl.FLAG.SCROLLABLE)
        local err_lbl = box:Label { text = "", w = lvgl.PCT(100), h = 16 }
        local function submit()
            local err, after = cb(ta.text or "")
            if err then err_lbl.text = tostring(err)
            else close(); if after then after() end end
        end
        local ok_btn = box:Button { w = lvgl.PCT(100), h = 26 }
        ok_btn:Label { text = "OK", align = lvgl.ALIGN.CENTER }
        ok_btn:onevent(lvgl.EVENT.RELEASED, submit)
        local cancel_btn = box:Button { w = lvgl.PCT(100), h = 26 }
        cancel_btn:Label { text = "Cancel", align = lvgl.ALIGN.CENTER }
        cancel_btn:onevent(lvgl.EVENT.RELEASED, close)
        ta:onevent(lvgl.EVENT.KEY, function()
            if lvgl.indev.get_act():get_key() == lvgl.KEY.ENTER then submit() end
        end)
    end)
end

local function confirm(title, warn, on_yes)
    modal({}, function(box, close)
        box:Label { text = title, w = lvgl.PCT(100) }
        if warn then box:Label { text = warn, text_color = "#ff5555", w = lvgl.PCT(100) } end
        local yes = box:Button { w = lvgl.PCT(100), h = 26 }
        yes:Label { text = "Yes", align = lvgl.ALIGN.CENTER }
        yes:onevent(lvgl.EVENT.RELEASED, function() close(); on_yes() end)
        local no = box:Button { w = lvgl.PCT(100), h = 26 }
        no:Label { text = "No", align = lvgl.ALIGN.CENTER }
        no:onevent(lvgl.EVENT.RELEASED, close)
    end)
end

-- ── Formatting ───────────────────────────────────────────────────────────────
local function mmss(sec)
    sec = tonumber(sec) or 0
    if sec < 0 then sec = 0 end
    return string.format("%d:%02d", math.floor(sec / 60), sec % 60)
end

local function elide(s, n)
    s = tostring(s or "")
    if #s > n then return s:sub(1, n - 1) .. "~" end
    return s
end

local function disp_title(s)  return musiclib.song_title(s) end

-- ── Playback engine ──────────────────────────────────────────────────────────

-- Track duration handling: the C decoder's getDuration() re-estimates VBR
-- durations from the first ~180 frames and then freezes — the display visibly
-- "corrects" to a slightly WRONG value. Preferred source is the exact frame
-- count from the file's Xing/Info header (id3.duration, pb.dur_exact); files
-- without one fall back to the decoder's FIRST non-zero estimate, latched.
local function track_duration()
    if pb.dur_exact then return pb.dur_exact end
    if pb.dur_latch then return pb.dur_latch end
    if pb.playing then
        local d = sound.getDuration() or 0
        if d > 0 then pb.dur_latch = d; return d end
    end
    return 0
end

local function stop_current()
    if pb.cur_obj then
        pb.cur_obj:stop()
        pb.cur_obj:delete()       -- removes the C SoundObject and closes the file
        pb.cur_obj = nil
    end
    pb.cur_song = nil
    pb.playing = false
    pb.paused  = false
    pb.dur_exact = nil
    pb.dur_latch = nil
end

-- Build the play order for the current queue. `start` (1-based queue index) goes
-- first; the rest follow in order, or shuffled when shuffle is on.
local function build_order(start)
    pb.order = {}
    local order = pb.order
    local n = #pb.queue
    if n == 0 then pb.opos = 0; return end
    if pb.shuffle then
        local rest = {}
        for i = 1, n do if i ~= start then rest[#rest + 1] = i end end
        for i = #rest, 2, -1 do
            local j = math.random(i)
            rest[i], rest[j] = rest[j], rest[i]
        end
        order[1] = start
        for _, v in ipairs(rest) do order[#order + 1] = v end
    else
        for i = 1, n do order[i] = i end
    end
    -- pb.opos points at `start` within the order
    for i = 1, #order do if order[i] == start then pb.opos = i; break end end
end

local refresh_np   -- forward decl (Now-Playing UI updater)

-- Try to play EXACTLY order position `p`. Silent on any failure (missing file,
-- open/decode error) — returns false so the caller can skip. Never toasts, so
-- scanning past a run of dead tracks doesn't spam.
-- NOTE: play_at/play_scan/on_track_end are ENGINE functions — reachable from
-- the background tick with no UI alive, so they must never touch np/views.
-- The display timer detects track changes and refreshes on its own.
local function play_at(p)
    if p < 1 or p > #pb.order then return false end
    pb.opos = p
    local song = pb.queue[pb.order[pb.opos]]
    if not song then return false end
    stop_current()
    if not fileman.exists(song.path) then return false end
    -- Lazy-tag entries that skipped the ID3 read at list time (folder view):
    -- one song's tags per track start instead of a whole folder's up front.
    if song.ext == "mp3" and not song.title and not song._tagged then
        song._tagged = true
        local ok, meta = pcall(id3.read, song.path)
        if ok and meta then
            song.title  = meta.title
            song.artist = song.artist or meta.artist
            song.album  = song.album or meta.album
            song.track  = song.track or meta.track
        end
    end
    local f = io.open(song.path, "r")
    if not f then return false end
    local obj = sound.loadFile(f)     -- consumes f (owns the file handle now)
    if not obj then return false end
    pb.cur_obj  = obj
    pb.cur_song = song
    -- Exact duration from the Xing/Info header, probed once per entry
    -- (false = probed, none found → track_duration() latches the decoder's
    -- first estimate instead).
    if song._dur == nil and song.ext == "mp3" then
        local ok, d = pcall(id3.duration, song.path)
        song._dur = (ok and d) or false
    end
    pb.dur_exact = song._dur or nil
    pb.dur_latch = nil
    obj:play()
    pb.playing = true
    pb.paused  = false
    return true
end

-- Play from order position `start`, advancing by `dir` (+1/-1) and skipping any
-- missing/unplayable tracks. Bounded to one pass over the queue so a playlist
-- full of dead paths can't spin. Returns true if something started.
local function play_scan(start, dir)
    local n = #pb.order
    if n == 0 then return false end
    local p = start
    for _ = 1, n do
        if p < 1 or p > n then break end
        if play_at(p) then return true end
        p = p + dir
    end
    stop_current()
    toast("No playable tracks")   -- pcall-guarded; harmless with no UI
    return false
end

-- Replace the queue with `list` and start at index `start` (default 1).
local function set_queue(list, start)
    pb.queue = list or {}
    if #pb.queue == 0 then stop_current(); return end
    start = start or 1
    if start < 1 then start = 1 end
    if start > #pb.queue then start = #pb.queue end
    build_order(start)
    play_scan(pb.opos, 1)
end

local function toggle_pause()
    if not pb.cur_obj then
        -- nothing loaded: (re)start the queue if we have one
        if #pb.queue > 0 then set_queue(pb.queue, pb.order[pb.opos] or 1) end
        return
    end
    pb.cur_obj:pause()            -- toggles the decoder + file_paused
    pb.paused = not pb.paused
    if np then refresh_np() end
end

local function next_track()
    if #pb.order == 0 then return end
    if pb.opos < #pb.order then
        play_scan(pb.opos + 1, 1)
    elseif pb.repeat_mode == "all" then
        if pb.shuffle then build_order(pb.order[math.random(#pb.order)]) end
        play_scan(1, 1)
    else
        toast("End of queue")
    end
end

local function prev_track()
    if #pb.order == 0 then return end
    -- restart the track if we're more than 3s in, else go to the previous one
    if pb.playing and (sound.getPosition() or 0) > 3 then play_scan(pb.opos, 1)
    elseif pb.opos > 1 then play_scan(pb.opos - 1, -1)
    else play_scan(pb.opos, 1) end
end

-- Called by the timer/background tick when the current file finishes on its
-- own. ENGINE function: no UI here (see play_at's note).
local function on_track_end()
    if pb.repeat_mode == "one" and play_at(pb.opos) then return end
    if pb.opos < #pb.order and play_scan(pb.opos + 1, 1) then return end
    if pb.repeat_mode == "all" then
        if pb.shuffle then build_order(pb.order[math.random(#pb.order)]) end
        if play_scan(1, 1) then return end
    end
    stop_current()
end

-- ── Foreground timer: auto-advance + Now-Playing display ─────────────────────
-- While the app is open this drives the engine; while backgrounded the
-- manager's tick (background contract below) does the same auto-advance.
local last_shown_song = false    -- sentinel ≠ nil so the first compare refreshes
apps.add_timer { period = 400, cb = function()
    if pb.playing and not pb.paused and pb.cur_obj and sound.fileEnded() then
        on_track_end()
    end
    -- The engine never touches the UI, so track changes (auto-advance, stop
    -- at queue end) are detected here and re-rendered.
    if np and pb.cur_song ~= last_shown_song then
        refresh_np()
    end
    if np and np.bar then
        local dur = pb.playing and track_duration() or 0
        local pos = pb.playing and (sound.getPosition() or 0) or 0
        np.time.text = mmss(pos) .. " / " .. mmss(dur)
        local c = np.bar:get_coords()
        local tw = c.x2 - c.x1
        local frac = (dur > 0) and (pos / dur) or 0
        if frac > 1 then frac = 1 end
        if tw > 0 then np.fill:set { w = math.floor(tw * frac) } end
    end
end }

-- ── Background contract ──────────────────────────────────────────────────────
-- Registered on EVERY launch: swaps fresh closures in and stops the manager's
-- background tick while we're foreground (the timer above takes over). The
-- "Run in background" menu button exits via apps.go_background("music");
-- Quit and the launcher's X row end playback via apps.close_background.
apps.register_background{
    key      = "music",
    app_name = "Music",
    state    = pb,
    period   = 400,
    -- UI-free heartbeat while backgrounded: same auto-advance as the timer.
    tick     = function()
        if pb.playing and not pb.paused and pb.cur_obj and sound.fileEnded() then
            on_track_end()
        end
    end,
    -- LIVE ids the app manager spares from exit sweeps.
    sounds   = function()
        if pb.cur_obj and pb.cur_obj.id then return { pb.cur_obj.id } end
        return {}
    end,
    -- Deliberate close; must work with no UI alive (launcher X row).
    on_close = function()
        if pb.cur_obj then
            pcall(function() pb.cur_obj:stop() end)
            pcall(function() pb.cur_obj:delete() end)
            pb.cur_obj = nil
        end
        pb.cur_song = nil
        pb.playing  = false
        pb.paused   = false
    end,
    status   = function()
        if pb.playing and pb.cur_song then
            local t = tostring(musiclib.song_title(pb.cur_song))
            return "> " .. t:sub(1, 11)
        end
        return "Music (idle)"
    end,
}

-- ── Now Playing ──────────────────────────────────────────────────────────────

show_nowplaying = function()
    swap_view(function(v)
        local col = v:Object {
            w = W, h = H, x = 0, y = 0, bg_opa = 0, border_width = 0, pad_all = 8,
            flex = { flex_direction = "column", flex_wrap = "nowrap" },
        }
        nav.replace(col, { flags = nav.ROLLOVER })

        local title_lbl  = col:Label { text = "", w = lvgl.PCT(100), h = 22 }
        local artist_lbl = col:Label { text = "", w = lvgl.PCT(100), h = 18 }
        local album_lbl  = col:Label { text = "", w = lvgl.PCT(100), h = 18 }

        -- Seek bar: a track container with a fill child; tap to seek.
        local bar = col:Object {
            w = lvgl.PCT(100), h = 16, border_width = 1, pad_all = 0, radius = 3,
            bg_opa = 40,
        }
        bar:clear_flag(lvgl.FLAG.SCROLLABLE)
        bar:add_flag(lvgl.FLAG.CLICKABLE)
        local fill = bar:Object {
            w = 0, h = lvgl.PCT(100), x = 0, y = 0, align = lvgl.ALIGN.LEFT_MID,
            border_width = 0, pad_all = 0, radius = 3, bg_color = "#24ba24", bg_opa = 255,
        }
        fill:clear_flag(lvgl.FLAG.SCROLLABLE)
        fill:clear_flag(lvgl.FLAG.CLICKABLE)
        bar:onevent(lvgl.EVENT.CLICKED, function()
            local dur = track_duration()
            if dur <= 0 then return end
            local px = lvgl.indev.get_act():get_point()
            local c = bar:get_coords()
            local span = (c.x2 - c.x1)
            if span <= 0 then return end
            local frac = (px - c.x1) / span
            if frac < 0 then frac = 0 elseif frac > 1 then frac = 1 end
            sound.seek(math.floor(dur * frac))
        end)

        local time_lbl = col:Label { text = "0:00 / 0:00", w = lvgl.PCT(100), h = 16 }

        -- Transport row
        local row = col:Object {
            w = lvgl.PCT(100), h = 34, border_width = 0, pad_all = 0,
            flex = { flex_direction = "row", flex_wrap = "nowrap" },
        }
        row:clear_flag(lvgl.FLAG.SCROLLABLE)
        local function tbtn(txt, fn)
            local b = row:Button { w = lvgl.PCT(19), h = 32 }
            b:Label { text = txt, align = lvgl.ALIGN.CENTER }
            b:onClicked(fn)
            return b
        end
        tbtn("|<", prev_track)
        tbtn("-15", function() sound.seek(math.max(0, (sound.getPosition() or 0) - 15)) end)
        local play_btn = row:Button { w = lvgl.PCT(19), h = 32 }
        local play_lbl = play_btn:Label { text = ">", align = lvgl.ALIGN.CENTER }
        play_btn:onClicked(toggle_pause)
        tbtn("+15", function()
            local dur = track_duration()
            sound.seek(math.min(dur, (sound.getPosition() or 0) + 15))
        end)
        tbtn(">|", next_track)

        -- Mode + volume row
        local row2 = col:Object {
            w = lvgl.PCT(100), h = 30, border_width = 0, pad_all = 0,
            flex = { flex_direction = "row", flex_wrap = "nowrap" },
        }
        row2:clear_flag(lvgl.FLAG.SCROLLABLE)
        local shuf_btn = row2:Button { w = lvgl.PCT(24), h = 28 }
        local shuf_lbl = shuf_btn:Label { align = lvgl.ALIGN.CENTER }
        local rep_btn = row2:Button { w = lvgl.PCT(24), h = 28 }
        local rep_lbl = rep_btn:Label { align = lvgl.ALIGN.CENTER }
        local vdn = row2:Button { w = lvgl.PCT(16), h = 28 }
        vdn:Label { text = "V-", align = lvgl.ALIGN.CENTER }
        local vlbl = row2:Label { text = "", w = lvgl.PCT(18), h = 28, align = lvgl.ALIGN.CENTER }
        local vup = row2:Button { w = lvgl.PCT(16), h = 28 }
        vup:Label { text = "V+", align = lvgl.ALIGN.CENTER }

        local function upd_modes()
            shuf_lbl.text = pb.shuffle and "Shuf*" or "Shuf"
            rep_lbl.text  = "Rep:" .. (pb.repeat_mode == "one" and "1" or pb.repeat_mode == "all" and "A" or "-")
            vlbl.text = "Vol " .. sound.getVolume()
            play_lbl.text = (pb.playing and not pb.paused) and "||" or ">"
        end
        shuf_btn:onClicked(function()
            pb.shuffle = not pb.shuffle
            if #pb.queue > 0 and pb.order[pb.opos] then build_order(pb.order[pb.opos]) end
            upd_modes()
        end)
        rep_btn:onClicked(function()
            pb.repeat_mode = (pb.repeat_mode == "off") and "all"
                or (pb.repeat_mode == "all") and "one" or "off"
            upd_modes()
        end)
        vdn:onClicked(function() sound.setVolume(math.max(0, sound.getVolume() - 1)); upd_modes() end)
        vup:onClicked(function() sound.setVolume(math.min(21, sound.getVolume() + 1)); upd_modes() end)

        -- Bottom nav
        local nrow = col:Object {
            w = lvgl.PCT(100), h = 30, border_width = 0, pad_all = 0,
            flex = { flex_direction = "row", flex_wrap = "nowrap" },
        }
        nrow:clear_flag(lvgl.FLAG.SCROLLABLE)
        local qb = nrow:Button { w = lvgl.PCT(48), h = 28 }
        qb:Label { text = "Queue", align = lvgl.ALIGN.CENTER }
        qb:onClicked(function() show_queue() end)   -- no arg: onClicked passes (code,value)
        local mb = nrow:Button { w = lvgl.PCT(48), h = 28 }
        mb:Label { text = "Menu", align = lvgl.ALIGN.CENTER }
        mb:onClicked(show_menu)

        np = {
            title = title_lbl, artist = artist_lbl, album = album_lbl,
            bar = bar, fill = fill, time = time_lbl, upd_modes = upd_modes,
        }
        refresh_np()
    end)
end

refresh_np = function()
    if not np then return end
    last_shown_song = pb.cur_song    -- keep the timer's change detector in sync
    if pb.cur_song then
        np.title.text  = disp_title(pb.cur_song)
        np.artist.text = pb.cur_song.artist or "Unknown Artist"
        np.album.text  = pb.cur_song.album or ""
    else
        np.title.text  = "(nothing playing)"
        np.artist.text = ""
        np.album.text  = ""
    end
    if np.upd_modes then np.upd_modes() end
end

-- ── Library scan ─────────────────────────────────────────────────────────────

local function run_scan(on_done)
    local task = musiclib.scan_task()
    local overlay = root:Object {
        w = W, h = H, x = 0, y = 0, bg_color = "#000000", bg_opa = 140,
        border_width = 0, pad_all = 0, radius = 0,
    }
    overlay:clear_flag(lvgl.FLAG.SCROLLABLE)
    overlay:add_flag(lvgl.FLAG.CLICKABLE)
    local box = overlay:Object {
        w = 240, h = lvgl.SIZE_CONTENT, align = lvgl.ALIGN.CENTER,
        radius = 6, border_width = 1, pad_all = 8,
        flex = { flex_direction = "column", flex_wrap = "nowrap" },
    }
    nav.push(box)
    box:Label { text = "Scanning SD...", w = lvgl.PCT(100), h = 18 }
    local phase_lbl = box:Label { text = "", w = lvgl.PCT(100), h = 16 }
    local cur_lbl   = box:Label { text = "", w = lvgl.PCT(100), h = 16 }
    local cancel_btn = box:Button { w = lvgl.PCT(100), h = 26 }
    cancel_btn:Label { text = "Cancel", align = lvgl.ALIGN.CENTER }
    cancel_btn:onevent(lvgl.EVENT.RELEASED, function() task.cancelled = true end)

    apps.add_timer { period = 15, cb = function(t)
        local done, err
        for _ = 1, 4 do done, err = task.step(); if done then break end end
        if not done then
            phase_lbl.text = (task.phase == "walk")
                and ("Finding files: " .. task.found)
                or  ("Reading tags: " .. task.tags_done .. "/" .. task.found)
            cur_lbl.text = elide(task.current, 30)
            return
        end
        t:delete()
        nav.pop()
        overlay:delete()
        if not err and task.index then library = task.index end
        if task.capped then
            toast("Stopped at " .. musiclib.MAX_SONGS .. " songs (library cap)")
        end
        on_done(err, task.index)
    end }
end

-- ── Library browsing ─────────────────────────────────────────────────────────

-- Standard list-view scaffold: title + Back button, then rows via `add_rows`.
local function list_view(title, back_fn, add_rows)
    swap_view(function(v)
        local content = v:Object {
            w = W, h = H, x = 0, y = 0, bg_opa = 0, border_width = 0, pad_all = 4,
            flex = { flex_direction = "row", flex_wrap = "wrap" },
        }
        nav.replace(content, { flags = nav.ROLLOVER + nav.SCROLL_FIRST })
        content:Label { text = elide(title, 40), w = lvgl.PCT(70), h = 18 }
        local bb = content:Button { w = lvgl.PCT(28), h = 22 }
        bb:Label { text = "Back", align = lvgl.ALIGN.CENTER }
        bb:onClicked(back_fn)
        -- Picker action bar: shown while selecting songs for a playlist.
        if pick then
            local addb = content:Button { w = lvgl.PCT(62), h = 24 }
            pick.bar_lbl = addb:Label { align = lvgl.ALIGN.CENTER }
            addb:onClicked(commit_pick)
            local cxl = content:Button { w = lvgl.PCT(34), h = 24 }
            cxl:Label { text = "Cancel", align = lvgl.ALIGN.CENTER }
            cxl:onClicked(cancel_pick)
            update_pick_bar()
        end
        add_rows(content)
    end)
end

local function row_button(parent, text, sub, fn)
    local b = parent:Button { w = lvgl.PCT(100), h = 26 }
    b:Label { text = elide(text, 34), align = lvgl.ALIGN.LEFT_MID }
    if sub then b:Label { text = elide(sub, 12), align = lvgl.ALIGN.RIGHT_MID } end
    b:onClicked(fn)
    return b
end

local PER_PAGE = 100   -- rows built per page for numbered-page lists

-- Numbered-page list body: a "< Prev | Page N/M | Next >" bar (only when there's
-- more than one page), then render(item, absIndex) for each item on page `page`.
-- `go(newPage)` rebuilds the view at another page. The bar goes at the TOP so
-- paging never needs a scroll-to-bottom; tapping "Page N/M" prompts for a page
-- to jump to. Only PER_PAGE rows are ever constructed at once (watchdog-safe).
local function paged(content, items, page, render, go)
    local total = #items
    local pages = math.max(1, math.ceil(total / PER_PAGE))
    page = math.max(1, math.min(page or 1, pages))
    if pages > 1 then
        local bar = content:Object {
            w = lvgl.PCT(100), h = 28, border_width = 0, pad_all = 0,
            flex = { flex_direction = "row", flex_wrap = "nowrap" },
        }
        bar:clear_flag(lvgl.FLAG.SCROLLABLE)
        local prev = bar:Button { w = lvgl.PCT(30), h = 26 }
        prev:Label { text = "< Prev", align = lvgl.ALIGN.CENTER }
        if page > 1 then prev:onClicked(function() go(page - 1) end) end
        local mid = bar:Button { w = lvgl.PCT(40), h = 26 }
        mid:Label { text = "Page " .. page .. "/" .. pages, align = lvgl.ALIGN.CENTER }
        mid:onClicked(function()
            prompt("Go to page (1-" .. pages .. "):", tostring(page), function(v)
                local n = tonumber(v)
                if not n then return "Enter a number" end
                n = math.floor(n)
                if n < 1 or n > pages then return "1-" .. pages end
                return nil, function() go(n) end
            end)
        end)
        local nxt = bar:Button { w = lvgl.PCT(30), h = 26 }
        nxt:Label { text = "Next >", align = lvgl.ALIGN.CENTER }
        if page < pages then nxt:onClicked(function() go(page + 1) end) end
    end
    local first = (page - 1) * PER_PAGE + 1
    local last = math.min(first + PER_PAGE - 1, total)
    for i = first, last do render(items[i], i) end
end

-- Play a list of songs starting at `idx`, then jump to Now Playing.
local function play_list(list, idx)
    set_queue(list, idx)
    show_nowplaying()
end

-- A song row that plays on tap (normal), or toggles selection (picker mode).
-- `list`/`idx` are the play context; `label` overrides the displayed title.
song_row = function(content, song, list, idx, label)
    label = label or disp_title(song)
    local b = content:Button { w = lvgl.PCT(100), h = 26 }
    local lbl = b:Label { align = lvgl.ALIGN.LEFT_MID }
    if song.artist then
        b:Label { text = elide(song.artist, 10), align = lvgl.ALIGN.RIGHT_MID }
    end
    local function paint()
        local prefix = pick and (pick.sel[song.path] and "[x] " or "[ ] ") or ""
        lbl.text = prefix .. elide(label, pick and 26 or 32)
    end
    paint()
    b:onClicked(function()
        if pick then
            if pick.sel[song.path] then
                pick.sel[song.path] = nil
                pick.count = pick.count - 1
                for i = #pick.list, 1, -1 do
                    if pick.list[i] == song.path then table.remove(pick.list, i); break end
                end
            else
                pick.sel[song.path] = song
                pick.list[#pick.list + 1] = song.path
                pick.count = pick.count + 1
            end
            paint()
            update_pick_bar()
        else
            play_list(list, idx)
        end
    end)
    return b
end

update_pick_bar = function()
    if pick and pick.bar_lbl then
        pick.bar_lbl.text = "Add " .. pick.count .. (pick.count == 1 and " song" or " songs")
    end
end

-- Commit the current selection to the target playlist (one file write).
commit_pick = function()
    if not pick then return end
    if pick.count == 0 then toast("No songs selected"); return end
    local name = pick.playlist
    local ok, added = musiclib.playlist_add_many(name, pick.list)
    pick = nil
    if ok then
        toast("Added " .. added .. (added == 1 and " song" or " songs") .. ' to "' .. name .. '"')
    else
        toast("Save failed")
    end
    show_playlists()
end

cancel_pick = function()
    pick = nil
    show_playlists()
end

-- Enter picker mode for `playlist` and open the library selector.
begin_pick = function(playlist)
    pick = { playlist = playlist, sel = {}, list = {}, count = 0, bar_lbl = nil }
    show_library()
end

show_album = function(artist, album, from_page)
    list_view(album.name, function() show_artist(artist, from_page) end, function(content)
        if not pick then
            local pa = content:Button { w = lvgl.PCT(100), h = 26 }
            pa:Label { text = "Play album", align = lvgl.ALIGN.CENTER }
            pa:onClicked(function() play_list(album.songs, 1) end)
        end
        local shown = 0
        for i, s in ipairs(album.songs) do
            if shown >= MAX_SHOW then break end
            shown = shown + 1
            local tn = tonumber(s.track and s.track:match("%d+"))
            local label = (tn and string.format("%d. ", tn) or "") .. disp_title(s)
            song_row(content, s, album.songs, i, label)
        end
        if #album.songs > MAX_SHOW then
            content:Label { text = "(+" .. (#album.songs - MAX_SHOW) .. " more)", w = lvgl.PCT(100), h = 18 }
        end
    end)
end

-- from_page: the Artists page to return to on Back (preserves scroll position).
show_artist = function(artist, from_page)
    list_view(artist.name, function() show_artists(from_page or 1) end, function(content)
        local shown = 0
        for _, al in ipairs(artist.albums) do
            if shown >= MAX_SHOW then break end
            shown = shown + 1
            row_button(content, al.name, #al.songs .. "", function() show_album(artist, al, from_page) end)
        end
        if #artist.albums > MAX_SHOW then
            content:Label { text = "(+" .. (#artist.albums - MAX_SHOW) .. " more)", w = lvgl.PCT(100), h = 18 }
        end
    end)
end

show_artists = function(page)
    list_view("Artists (" .. #library.artists .. ")", function() show_library() end, function(content)
        paged(content, library.artists, page, function(ar)
            row_button(content, ar.name, #ar.albums .. "", function() show_artist(ar, page) end)
        end, function(p) show_artists(p) end)
    end)
end

local function show_all_songs(page)
    list_view("All Songs (" .. library.count .. ")", function() show_library() end, function(content)
        paged(content, library.songs, page, function(s, i)
            song_row(content, s, library.songs, i)
        end, function(p) show_all_songs(p) end)
    end)
end

-- mode: "artists" (default) or the top-level library menu
show_library = function(mode)
    if not library then
        toast("Scan the library first")
        show_menu()
        return
    end
    if mode == "artists" then show_artists(1); return end   -- legacy entry
    -- top-level library chooser. In picker mode "Back" cancels the selection.
    list_view("Library", pick and cancel_pick or show_menu, function(content)
        row_button(content, "Search", nil, function() show_search("") end)
        row_button(content, "Artists", #library.artists .. "", function() show_artists(1) end)
        row_button(content, "Genres", #library.genres .. "", show_genres)
        row_button(content, "All Songs", library.count .. "", function() show_all_songs() end)
        row_button(content, "Folders", nil, function() show_folders(musiclib.MUSIC_DIR) end)
    end)
end

-- List every genre; tapping one opens its song list.
show_genres = function()
    if not library then show_menu(); return end
    list_view("Genres (" .. #library.genres .. ")", function() show_library() end, function(content)
        local shown = 0
        for _, g in ipairs(library.genres) do
            if shown >= MAX_SHOW then break end
            shown = shown + 1
            row_button(content, g.name, #g.songs .. "", function() show_genre(g) end)
        end
        if #library.genres > MAX_SHOW then
            content:Label { text = "(+" .. (#library.genres - MAX_SHOW) .. " more)", w = lvgl.PCT(100), h = 18 }
        end
    end)
end

-- All songs in one genre (already artist/album/track sorted).
show_genre = function(g)
    list_view(g.name .. " (" .. #g.songs .. ")", show_genres, function(content)
        if not pick then
            local play_all = content:Button { w = lvgl.PCT(100), h = 26 }
            play_all:Label { text = "Play all", align = lvgl.ALIGN.CENTER }
            play_all:onClicked(function() play_list(g.songs, 1) end)
        end
        local shown = 0
        for i, s in ipairs(g.songs) do
            if shown >= MAX_SHOW then break end
            shown = shown + 1
            song_row(content, s, g.songs, i)
        end
        if #g.songs > MAX_SHOW then
            content:Label { text = "(+" .. (#g.songs - MAX_SHOW) .. " more)", w = lvgl.PCT(100), h = 18 }
        end
    end)
end

-- Raw folder browser (audio files + subfolders), tap a track to play the folder.
show_folders = function(path, page)
    path = fileman.normalize(path)
    local entries = fileman.list(path, { sizes = false }) or {}
    -- Combined display list: subfolders (dirs-first from fileman) then audio
    -- files. Tags are NOT read here — a big folder would mean seconds of blocking
    -- SD reads before the view opens (rows show filenames anyway); play_at
    -- lazy-tags the one song it starts. `songs` is the parallel play context.
    local items, songs = {}, {}
    for _, e in ipairs(entries) do
        if e.type == "dir" then
            items[#items + 1] = { dir = true, name = e.name, path = fileman.join(path, e.name) }
        elseif musiclib.EXTS[(fileman.ext(e.name) or "")] then
            songs[#songs + 1] = { path = fileman.join(path, e.name), name = e.name, ext = fileman.ext(e.name) }
            items[#items + 1] = { song = songs[#songs], si = #songs, name = e.name }
        end
    end
    local back = function()
        local p = fileman.parent(path)
        if p and #path > #musiclib.MUSIC_DIR then show_folders(p) else show_library() end
    end
    list_view(fileman.basename(path), back, function(content)
        paged(content, items, page, function(it)
            if it.dir then
                row_button(content, it.name .. "/", nil, function() show_folders(it.path) end)
            else
                song_row(content, it.song, songs, it.si, it.name)
            end
        end, function(p) show_folders(path, p) end)
    end)
end

-- Search titles/artists/albums; results play (or select, in picker mode).
-- Submitting the box re-enters with the new query (keeps the picker bar).
show_search = function(query, page)
    query = query or ""
    if not library then show_menu(); return end
    list_view("Search", function() show_library() end, function(content)
        local ta = content:Textarea { one_line = true, text = query, w = lvgl.PCT(100), h = 32 }
        ta:clear_flag(lvgl.FLAG.SCROLLABLE)
        local go = content:Button { w = lvgl.PCT(100), h = 24 }
        go:Label { text = "Search", align = lvgl.ALIGN.CENTER }
        local function run() show_search(ta.text or "", 1) end   -- new query -> page 1
        go:onClicked(run)
        ta:onevent(lvgl.EVENT.KEY, function()
            if lvgl.indev.get_act():get_key() == lvgl.KEY.ENTER then run() end
        end)

        local q = query:lower():gsub("^%s+", ""):gsub("%s+$", "")
        if q == "" then
            content:Label { text = "Type a title, artist or album.", w = lvgl.PCT(100), h = 20 }
            return
        end
        local results = {}
        for _, s in ipairs(library.songs) do
            local hay = (disp_title(s) .. " " .. (s.artist or "") .. " " .. (s.album or "")):lower()
            if hay:find(q, 1, true) then results[#results + 1] = s end
        end
        if #results == 0 then
            content:Label { text = '(no matches for "' .. elide(query, 16) .. '")', w = lvgl.PCT(100), h = 20 }
            return
        end
        paged(content, results, page, function(s, i)
            song_row(content, s, results, i)
        end, function(p) show_search(query, p) end)
    end)
end

-- ── Queue ────────────────────────────────────────────────────────────────────

show_queue = function(page)
    list_view("Queue (" .. #pb.queue .. ")", show_nowplaying, function(content)
        if #pb.queue == 0 then
            content:Label { text = "(empty)", w = lvgl.PCT(100), h = 22 }
            return
        end
        local cur_qidx = pb.order[pb.opos]
        -- Default to the page holding the current track.
        if not page and cur_qidx then page = math.ceil(cur_qidx / PER_PAGE) end
        paged(content, pb.queue, page, function(s, i)
            local mark = (i == cur_qidx) and "> " or ""
            row_button(content, mark .. disp_title(s), s.artist and elide(s.artist, 8) or nil,
                function()
                    -- jump to this track: find its order position
                    for p = 1, #pb.order do
                        if pb.order[p] == i then play_scan(p, 1); show_nowplaying(); return end
                    end
                end)
        end, function(p) show_queue(p) end)
    end)
end

-- ── Playlists ────────────────────────────────────────────────────────────────

-- Resolve a playlist's stored paths to song entries (tagging as needed).
local function songs_from_paths(paths)
    -- index songs by path for quick lookup when the library is present
    local bypath = {}
    if library then for _, s in ipairs(library.songs) do bypath[s.path] = s end end
    local out = {}
    -- NO per-entry SD I/O here: a big playlist would mean thousands of exists()
    -- + ID3 reads in one call (watchdog + UI freeze). Songs not in the library
    -- become cheap filename stubs; play_at checks existence and lazy-skips any
    -- that are missing at play time, and Now Playing shows the filename.
    for _, p in ipairs(paths) do
        out[#out + 1] = bypath[p] or { path = p, name = fileman.basename(p), ext = fileman.ext(p) }
    end
    return out
end

show_playlists = function()
    local lists = musiclib.list_playlists()
    list_view("Playlists", show_menu, function(content)
        -- Make a playlist by selecting songs from the library.
        local newpick = content:Button { w = lvgl.PCT(100), h = 26 }
        newpick:Label { text = "+ New (pick songs)", align = lvgl.ALIGN.CENTER }
        newpick:onClicked(function()
            if not library then toast("Scan the library first"); return end
            prompt("Playlist name:", "", function(value)
                if value == "" then return "Enter a name" end
                return nil, function() begin_pick(value) end
            end)
        end)
        -- Snapshot the current play queue as a playlist.
        local newb = content:Button { w = lvgl.PCT(100), h = 26 }
        newb:Label { text = "+ New (from queue)", align = lvgl.ALIGN.CENTER }
        newb:onClicked(function()
            if #pb.queue == 0 then toast("Queue is empty"); return end
            prompt("Playlist name:", "", function(value)
                if value == "" then return "Enter a name" end
                local paths = {}
                for _, s in ipairs(pb.queue) do paths[#paths + 1] = s.path end
                local ok, err = musiclib.save_playlist(value, paths)
                if not ok then return err or "Save failed" end
                return nil, show_playlists
            end)
        end)
        if #lists == 0 then
            content:Label { text = "(no playlists)", w = lvgl.PCT(100), h = 22 }
        end
        for _, pl in ipairs(lists) do
            local plist = pl
            row_button(content, pl.name, nil, function()
                modal({ w = 200 }, function(box, close)
                    box:Label { text = elide(plist.name, 22), w = lvgl.PCT(100), h = 18 }
                    local play = box:Button { w = lvgl.PCT(100), h = 26 }
                    play:Label { text = "Play", align = lvgl.ALIGN.CENTER }
                    play:onevent(lvgl.EVENT.RELEASED, function()
                        close()
                        local songs = songs_from_paths(musiclib.load_playlist(plist.path))
                        if #songs == 0 then toast("Playlist empty / missing files"); return end
                        play_list(songs, 1)
                    end)
                    local add = box:Button { w = lvgl.PCT(100), h = 26 }
                    add:Label { text = "Add songs...", align = lvgl.ALIGN.CENTER }
                    add:onevent(lvgl.EVENT.RELEASED, function()
                        close()
                        if not library then toast("Scan the library first"); return end
                        begin_pick(plist.name)
                    end)
                    if pb.cur_song then
                        local addcur = box:Button { w = lvgl.PCT(100), h = 26 }
                        addcur:Label { text = "Add current song", align = lvgl.ALIGN.CENTER }
                        addcur:onevent(lvgl.EVENT.RELEASED, function()
                            close()
                            musiclib.playlist_add(plist.name, pb.cur_song.path)
                            toast("Added")
                        end)
                    end
                    local del = box:Button { w = lvgl.PCT(100), h = 26 }
                    del:Label { text = "Delete", align = lvgl.ALIGN.CENTER }
                    del:onevent(lvgl.EVENT.RELEASED, function()
                        close()
                        confirm('Delete "' .. plist.name .. '"?', nil, function()
                            fileman.remove(plist.path)
                            show_playlists()
                        end)
                    end)
                    local c = box:Button { w = lvgl.PCT(100), h = 26 }
                    c:Label { text = "Cancel", align = lvgl.ALIGN.CENTER }
                    c:onevent(lvgl.EVENT.RELEASED, close)
                end)
            end)
        end
    end)
end

-- ── Organize (auto-sort by tags) ─────────────────────────────────────────────

local function stem(name) return (name:gsub("%.[^.]+$", "")) end

local function organize_target(song)
    local ext    = song.ext or "mp3"
    local title  = id3.safe_name(song.title) or id3.safe_name(stem(song.name)) or "Track"
    local artist = id3.safe_name(song.artist) or "Unknown Artist"
    local album  = id3.safe_name(song.album)  or "Unknown Album"
    local tn     = tonumber(song.track and song.track:match("%d+"))
    local fname  = (tn and string.format("%02d - ", tn) or "") .. title .. "." .. ext
    return fileman.join(fileman.join(fileman.join(musiclib.MUSIC_DIR, artist), album), fname)
end

local function run_organize(plan, on_done)
    local overlay = root:Object {
        w = W, h = H, x = 0, y = 0, bg_color = "#000000", bg_opa = 140,
        border_width = 0, pad_all = 0, radius = 0,
    }
    overlay:clear_flag(lvgl.FLAG.SCROLLABLE)
    overlay:add_flag(lvgl.FLAG.CLICKABLE)
    local box = overlay:Object {
        w = 240, h = lvgl.SIZE_CONTENT, align = lvgl.ALIGN.CENTER,
        radius = 6, border_width = 1, pad_all = 8,
        flex = { flex_direction = "column", flex_wrap = "nowrap" },
    }
    nav.push(box)
    local box_title = box:Label { text = "Organizing...", w = lvgl.PCT(100), h = 18 }
    local cur_lbl = box:Label { text = "", w = lvgl.PCT(100), h = 16 }
    local cnt_lbl = box:Label { text = "", w = lvgl.PCT(100), h = 16 }
    local cancel = box:Button { w = lvgl.PCT(100), h = 26 }
    cancel:Label { text = "Cancel", align = lvgl.ALIGN.CENTER }
    local cancelled = false
    cancel:onevent(lvgl.EVENT.RELEASED, function() cancelled = true end)

    local i, moved, failed = 0, 0, 0
    local moves = {}          -- { [normalized old path] = actual new path }
    local phase = "move"      -- "move" -> "remap" -> "done"
    local pls, pj, pl_updated = nil, 0, 0   -- playlist remap state

    -- Each tick advances exactly one phase's worth of bounded work, then yields
    -- (returns) so the task watchdog is fed between ticks: up to 2 file moves, OR
    -- one playlist rewritten, OR the finalize step.
    apps.add_timer { period = 15, cb = function(t)
        if phase == "move" then
            if cancelled then
                phase = "remap"          -- still fix playlists for what already moved
            else
                for _ = 1, 2 do
                    i = i + 1
                    local p = plan[i]
                    if not p then phase = "remap"; break end
                    cur_lbl.text = elide(p.name, 30)
                    local parent = fileman.parent(p.dst)
                    fileman.mkdir(parent)
                    local dst = p.dst
                    if fileman.exists(dst) then
                        dst = fileman.unique_path(parent, fileman.basename(p.dst))
                    end
                    if dst and fileman.move(p.src, dst) then
                        moved = moved + 1
                        moves[fileman.normalize(p.src)] = dst
                    else
                        failed = failed + 1
                    end
                    cnt_lbl.text = moved .. " moved" .. (failed > 0 and (", " .. failed .. " failed") or "")
                end
            end
            return   -- yield after each move batch (or the transition to remap)
        end

        if phase == "remap" then
            -- Lazy init: only bother listing playlists if anything actually moved.
            if pls == nil then
                pls = (moved > 0) and musiclib.list_playlists() or {}
                pj = 0
                box_title.text = "Updating playlists..."
            end
            pj = pj + 1
            local pl = pls[pj]
            if pl then
                cur_lbl.text = elide(pl.name, 30)
                cnt_lbl.text = pj .. " / " .. #pls
                local ok, changed = pcall(musiclib.remap_one, pl, moves)   -- one file/tick
                if ok and changed then pl_updated = pl_updated + 1 end
                return   -- yield after each playlist
            end
            phase = "done"
            return
        end

        -- done
        t:delete()
        nav.pop()
        overlay:delete()
        on_done(moved, failed, pl_updated)
    end }
end

show_organize = function()
    list_view("Organize", show_menu, function(content)
        content:Label {
            text = "Auto-sort tagged files into\nS:/Music/Artist/Album/",
            w = lvgl.PCT(100), h = 34,
        }
        local auto = content:Button { w = lvgl.PCT(100), h = 30 }
        auto:Label { text = "Auto-organize by tags", align = lvgl.ALIGN.CENTER }
        auto:onClicked(function()
            if not library then toast("Scan the library first"); return end
            -- build the move plan
            local plan = {}
            for _, s in ipairs(library.songs) do
                local dst = organize_target(s)
                if fileman.normalize(s.path) ~= fileman.normalize(dst) then
                    plan[#plan + 1] = { src = s.path, dst = dst, name = s.name }
                end
            end
            if #plan == 0 then toast("Everything is already organized"); return end
            confirm("Move " .. #plan .. " file(s) into\nS:/Music/Artist/Album/ ?", nil, function()
                stop_current()   -- moving files we might be streaming: stop first
                run_organize(plan, function(moved, failed, pl_updated)
                    musiclib.clear_cache()
                    library = nil
                    pb.queue, pb.order, pb.opos = {}, {}, 0
                    toast(moved .. " moved"
                        .. (failed > 0 and (", " .. failed .. " failed") or "")
                        .. ((pl_updated or 0) > 0 and (", " .. pl_updated .. " playlist(s) updated") or ""))
                    -- rescan so the library reflects the new layout
                    run_scan(function() show_menu() end)
                end)
            end)
        end)

        content:Label { text = "-- Manual --", w = lvgl.PCT(100), h = 18 }
        local files = content:Button { w = lvgl.PCT(100), h = 28 }
        files:Label { text = "Browse & edit files (Files-style)", align = lvgl.ALIGN.CENTER }
        files:onClicked(function() show_folders(musiclib.MUSIC_DIR) end)
    end)
end

-- ── Main menu ────────────────────────────────────────────────────────────────

local function quit_app()
    -- Deliberate close: runs the contract's on_close (stop + free playback)
    -- and removes the background record, then a normal exit.
    apps.close_background("music")
    apps.go_home()
end

show_menu = function()
    swap_view(function(v)
        local col = v:Object {
            w = W, h = H, x = 0, y = 0, bg_opa = 0, border_width = 0, pad_all = 8,
            flex = { flex_direction = "column", flex_wrap = "nowrap" },
        }
        nav.replace(col, { flags = nav.ROLLOVER + nav.SCROLL_FIRST })

        col:Label { text = "Music", w = lvgl.PCT(100), h = 24 }

        local status = library
            and (library.count .. " songs in library")
            or  "No library yet"
        col:Label { text = status, w = lvgl.PCT(100), h = 18 }

        if pb.cur_song then
            col:Label {
                text = ((pb.playing and not pb.paused) and "> " or "|| ") .. elide(disp_title(pb.cur_song), 30),
                w = lvgl.PCT(100), h = 18,
            }
        end

        local function menu_btn(txt, fn)
            local b = col:Button { w = lvgl.PCT(100), h = 34 }
            b:Label { text = txt, align = lvgl.ALIGN.CENTER }
            b:onClicked(fn)
            return b
        end

        menu_btn("Now Playing", show_nowplaying)
        menu_btn(library and "Library" or "Scan library", function()
            if library then show_library() else run_scan(function() show_menu() end) end
        end)
        if library then menu_btn("Rescan", function() run_scan(function() show_menu() end) end) end
        menu_btn("Playlists", show_playlists)
        menu_btn("Organize", show_organize)
        menu_btn("Run in background", function() apps.go_background("music") end)
        menu_btn("Quit", quit_app)
    end)
end

-- ── Start ────────────────────────────────────────────────────────────────────

-- Seed shuffle from the device clock so it isn't identical every boot.
pcall(function() math.randomseed(math.floor((utils.now() or 0)) % 2147483647) end)

-- Try the cached index (instant); scanning is user-triggered from the menu.
local ok_lib, idx = pcall(musiclib.load_cache)
if ok_lib and idx then library = idx end

show_menu()

return root
