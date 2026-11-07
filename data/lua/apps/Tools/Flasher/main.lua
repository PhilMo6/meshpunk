-- Tools > Flasher — flash and back up ESP32 devices over a USB cable.
--
-- This device runs USB host mode; the other device (the target) is flashed
-- from a .bin on this device's SD card, or read out into one. Any ESP32
-- firmware: Meshpunk, MeshCore, Meshtastic, a custom build.
--
-- All the work is done by the `espserial` USB driver, which talks to the
-- target's ROM bootloader and reads and writes the files itself. This app
-- only sends it commands (_usb_drv_write) and shows its status
-- (_usb_drv_read):
--   command blob  byte 1 op, byte 2 nonce; CHECK adds kind, offset, path;
--                 READ adds offset, length, path
--   status blob   byte 1 state (+0x80 = flasher holds the port), 2 op,
--                 3 stage, 4 percent, 5 nonce it answers, 9-12 instance
--                 stamp, 13.. text
-- While a command blob exists the driver keeps the port off the peer link,
-- so every way out of the app clears it.
local lvgl     = require("lvgl")
local apps     = require("lib/apps")
local nav      = require("lib/nav")
local theme      = require("lib/theme")
local fileman    = require("lib/fileman")
local filepick   = require("lib/filepick")
local downloader = require("lib/downloader")   -- wifi_wait

local DRV = "espserial"

local OP_HOLD, OP_BOOT, OP_CHECK, OP_WRITE, OP_RESET, OP_CANCEL, OP_READ = 1, 2, 3, 4, 5, 6, 7
local ST_BUSY, ST_OK, ST_FAIL = 1, 2, 3
-- FULL = whole-flash image at 0 after a chip erase; UPDATE = app image into
-- an app partition; RAW = any file at an offset the user typed.
local KIND_FULL, KIND_UPDATE, KIND_RAW = 1, 2, 3

local NOT_IN_BOOTLOADER = "target is not in the bootloader"

local POLL_MS = 150
local function ticks(ms) return math.floor(ms / POLL_MS) end

-- The ROM reads flash 64 bytes a request, about 2 ms each.
local READ_BYTES_PER_SEC = 32 * 1024

local BOARD_NAMES = {
    tdeck     = "T-Deck",
    heltec_v4 = "Heltec V4",
    wio_l2    = "Wio Tracker L2",
}
local function board_name(slug) return BOARD_NAMES[slug] or slug end

-- S:/firmware holds firmware for ANY device; S:/meshpunk is Meshpunk's own.
local IMAGE_DIR   = "S:/firmware"
local IMAGE_DIRS  = { IMAGE_DIR, "S:/" }
local BACKUP_DIR  = IMAGE_DIR .. "/backups"

-- Meshpunk release channels (the same two repos Settings > Firmware polls).
local CHANNELS = {
    { id = "stable", label = "Stable", repo = "PhilMo6/meshpunk" },
    { id = "dev",    label = "Dev",    repo = "PhilMo6/meshpunk-dev" },
}
local TABLE_FILE  = BACKUP_DIR .. "/.table.bin"
local DRIVER_DIRS = { "L:/usb_drivers", "S:/meshpunk/usb_drivers" }

local W = lvgl.HOR_RES()
local H = lvgl.VER_RES()

local root = apps.new_root()
root:set { w = W, h = H, pad_all = 0, border_width = 0, bg_opa = 0 }
root:clear_flag(lvgl.FLAG.SCROLLABLE)

theme.show_background()

-- ── Driver channel ───────────────────────────────────────────────────────────

local nonce = 0
local started_host = false

-- Sends one command; returns the nonce its status will carry, or nil.
local function send(op, extra)
    nonce = nonce % 250 + 1
    local ok = _usb_drv_write(DRV, string.char(op, nonce) .. (extra or ""))
    if ok then return nonce end
    return nil
end

local function release()
    if _usb_drv_write then pcall(_usb_drv_write, DRV, "") end
end

local function status()
    local b = _usb_drv_read(DRV)
    if not b or #b < 12 then return nil end
    local s0 = b:byte(1)
    return {
        state = s0 & 0x7F,
        hold  = (s0 & 0x80) ~= 0,
        op    = b:byte(2),
        stage = b:byte(3),
        pct   = b:byte(4),
        nonce = b:byte(5),
        stamp = b:sub(9, 12),
        text  = b:sub(13):match("^[^\0]*") or "",
    }
end

local function le32(n)
    return string.char(n & 0xFF, (n >> 8) & 0xFF, (n >> 16) & 0xFF, (n >> 24) & 0xFF)
end

-- CHECK payload: kind, offset (RAW: where; UPDATE: 0 = automatic, or the slot
-- chosen from the "slots" answer), path.
local function check_payload(it)
    return string.char(it.kind) .. le32(it.offset or 0) .. it.path .. "\0"
end

-- READ payload: offset, length, destination path (SD card only).
local function read_payload(part)
    return le32(part.offset) .. le32(part.size) .. part.path .. "\0"
end

apps.set_on_close(function()
    release()
    if started_host then pcall(_usb_stop) end
end)

-- ── Views ────────────────────────────────────────────────────────────────────
-- One root, one view at a time. `ui` holds the widgets the poll timer
-- touches; it is replaced with every view, so the timer never reaches a
-- deleted widget.

local view
local ui = {}
local step = nil          -- what the poll timer is waiting for; nil = nothing
local count = 0           -- poll ticks spent in the current step
local lost = 0            -- consecutive polls with no driver status
local job = {}            -- the image / backup being worked on + command nonces

local function new_view(title, with_home)
    local old = view
    ui = {}
    view = root:Object {
        flex = { flex_direction = "row", flex_wrap = "wrap" },
        w = W, h = H,
        border_width = 0, pad_all = 6, pad_row = 4, bg_opa = 0,
    }
    nav.replace(view, { flags = nav.ROLLOVER + nav.SCROLL_FIRST })
    if old then apps.delete_view(old) end

    view:Label { text = title, w = lvgl.PCT(70), h = 26 }
    if with_home then
        local home = view:Button { w = 50, h = 22 }
        home:Label { text = "Home", align = lvgl.ALIGN.CENTER }
        home:onClicked(function() apps.go_home() end)
    end
    return view
end

local function text(v, s)
    return v:Label { text = s, w = lvgl.PCT(100), h = lvgl.SIZE_CONTENT }
end

local function button(v, label, fn, width)
    local b = v:Button { w = width or lvgl.PCT(100), h = 30 }
    local l = b:Label { text = label, align = lvgl.ALIGN.CENTER }
    b:onClicked(fn)
    return b, l
end

local function set_step(s)
    step = s
    count = 0
    lost = 0
end

-- Dimmed overlay + centred box in its own nav scope (Tools/Files' pattern).
-- `build(box, close)` adds the content; close() pops the scope and deletes.
local function modal(build)
    local overlay = root:Object {
        w = W, h = H, x = 0, y = 0,
        bg_color = "#000000", bg_opa = 140, border_width = 0, pad_all = 0, radius = 0,
    }
    overlay:clear_flag(lvgl.FLAG.SCROLLABLE)
    overlay:add_flag(lvgl.FLAG.CLICKABLE)
    local box = overlay:Object {
        w = W - 40, h = lvgl.SIZE_CONTENT, align = lvgl.ALIGN.CENTER,
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

local function minutes_text(bytes)
    local secs = bytes / READ_BYTES_PER_SEC
    if secs < 50 then return "under a minute" end
    return string.format("about %d min", math.floor(secs / 60 + 0.5))
end

local show_pick, show_selected, show_offset, show_connect, show_confirm, show_slots,
      show_progress, show_done, show_fail, show_backup, show_releases, show_release_list

-- ── Images and partition tables ──────────────────────────────────────────────

-- An image described from its file name: any .bin is accepted. The kind here
-- is only what the list shows before a target is connected: -merged.bin
-- (Meshpunk, MeshCore), .factory.bin (Meshtastic) and whole-device backups
-- are full images, anything else an update. The driver decides for real from
-- the content - a file that holds a bootloader and a partition table is a
-- full image whatever it is called. Meshpunk release names also give board +
-- version; the driver checks those against the file's board tag.
local function image_from(path, size)
    local name = fileman.basename(path)
    if not name:lower():match("%.bin$") then return nil end
    local full = name:match("%-merged%.bin$") or name:match("%.factory%.bin$")
              or name:match("^whole%-device%-")
    local it = {
        path = path,
        name = name,
        kind = full and KIND_FULL or KIND_UPDATE,
        size = size or 0,
        offset = 0,
    }
    local slug, ver = name:match("^meshpunk%-([^%-]+)%-(.+)%-%a+%.bin$")
    if slug then
        it.slug, it.ver = slug, ver
        it.label = board_name(slug) .. "  " .. ver
    else
        it.label = name
    end
    return it
end

local function kind_text(it)
    if it.kind == KIND_FULL then return "Full image" end
    if it.kind == KIND_RAW then return string.format("At 0x%X", it.offset) end
    return "Update"
end

-- Entries of an ESP partition table (32-byte entries, magic AA 50): the
-- bytes handed in start at the table's first entry.
local function parse_table(d)
    if not d or #d < 32 or d:byte(1) ~= 0xAA or d:byte(2) ~= 0x50 then return nil end
    local out = {}
    for i = 0, (#d // 32) - 1 do
        local p = i * 32
        if d:byte(p + 1) ~= 0xAA or d:byte(p + 2) ~= 0x50 then break end
        local e = {
            type = d:byte(p + 3),
            sub  = d:byte(p + 4),
            off  = d:byte(p + 5) | (d:byte(p + 6) << 8) | (d:byte(p + 7) << 16) | (d:byte(p + 8) << 24),
            size = d:byte(p + 9) | (d:byte(p + 10) << 8) | (d:byte(p + 11) << 16) | (d:byte(p + 12) << 24),
        }
        e.label = d:sub(p + 13, p + 28):match("^[^\0]*") or ""
        if e.label == "" then e.label = string.format("part%d", i) end
        out[#out + 1] = e
    end
    return out
end

-- The partition table inside a full image (at 0x8000), or nil.
local function file_table(path)
    local f = io.open(path, "r")
    if not f then return nil end
    f:seek("set", 0x8000)
    local d = f:read(0xC00)
    f:close()
    return parse_table(d)
end

local function table_find(tbl, typ, sub)
    for _, e in ipairs(tbl or {}) do
        if e.type == typ and e.sub == sub then return e end
    end
    return nil
end

-- The other files of a Meshtastic install, when they sit beside the
-- firmware-<board>-<ver>.factory.bin that was picked (bin/device-install.sh
-- in the Meshtastic firmware repo): the OTA helper mt-<mcu>-ota.bin into the
-- ota_1 partition and littlefs-<board>-<ver>.bin into the filesystem
-- partition. The offsets are read from the image's own partition table.
local MCU_NAME = {
    ["ESP32-S3"] = "esp32s3", ["ESP32-C3"] = "esp32c3", ["ESP32-C6"] = "esp32c6",
    ["ESP32-S2"] = "esp32s2", ["ESP32"] = "esp32",
}

local function companions(it, chip)
    local out = {}
    local rest = it.name:match("^firmware%-(.+)%.factory%.bin$")
    if not rest then return out end
    local tbl = file_table(it.path)
    local dir = fileman.parent(it.path)
    if not tbl or not dir then return out end
    local function add(name, e, what)
        if not e then return end
        local p = fileman.join(dir, name)
        local st = fileman.stat(p)
        if st and st.type ~= "dir" then
            out[#out + 1] = { path = p, name = name, label = name, what = what,
                              kind = KIND_RAW, offset = e.off, size = st.size or 0 }
        end
    end
    local mcu = MCU_NAME[chip]
    if mcu then add("mt-" .. mcu .. "-ota.bin", table_find(tbl, 0, 0x11), "update helper") end
    add("littlefs-" .. rest .. ".bin", table_find(tbl, 1, 0x82), "filesystem")
    return out
end

-- A partition backup is named <label>-<offset hex>-<size hex>.bin, so the
-- file alone says where it goes. nil for any other name.
local function backup_from_name(path, size)
    local name = fileman.basename(path)
    local label, off, len = name:match("^(.+)%-(%x+)%-(%x+)%.bin$")
    if not label then return nil end
    return { path = path, name = name, label = name, what = "backup of " .. label,
             kind = KIND_RAW, offset = tonumber(off, 16), part_size = tonumber(len, 16),
             size = size or 0 }
end

-- mkdir with parents ("S:/a/b/c" makes a, a/b, a/b/c as needed).
local function mkdirs(path)
    if fileman.is_dir(path) then return true end
    local parent = fileman.parent(path)
    if parent and not fileman.is_dir(parent) and not mkdirs(parent) then return false end
    return fileman.mkdir(path)
end

local function scan_images()
    local out = {}
    for _, dir in ipairs(IMAGE_DIRS) do
        local ents = fileman.list(dir)
        if ents then
            for _, e in ipairs(ents) do
                if e.type ~= "dir" then
                    local it = image_from(fileman.join(dir, e.name), e.size)
                    if it then out[#out + 1] = it end
                end
            end
        end
    end
    return out
end

-- ── Preflight ────────────────────────────────────────────────────────────────

local function driver_installed(id)
    for _, base in ipairs(DRIVER_DIRS) do
        local dir = base .. "/" .. id
        if fileman.exists(dir .. "/match") and not fileman.exists(dir .. "/.disabled") then
            return true
        end
    end
    return false
end

-- What stops the app from working at all, or nil.
local function blocker()
    if not (_usb_start and _usb_drv_read and _usb_drv_write) then
        return "This firmware is too old for the Flasher. Update this device first."
    end
    if not driver_installed(DRV) then
        return "The espserial USB driver is not installed. Get it in Tools > USB Host > USB drivers."
    end
    if driver_installed("tdeck") then
        return "The old tdeck USB driver is still installed and claims the same port. " ..
               "Delete it in Tools > USB Host > USB drivers, then come back."
    end
    return nil
end

-- ── Pick an image ────────────────────────────────────────────────────────────

show_pick = function()
    set_step(nil)
    release()
    job = {}
    local v = new_view("Flasher", true)

    local why = blocker()
    if why then
        text(v, why)
        return
    end

    text(v, "Flashes ESP32 firmware onto another device over USB, from a .bin on this device's " ..
            "SD card - or backs a device up to one.")

    button(v, "Browse for an image...", function()
        filepick.open(root, {
            mode  = "file",
            title = "Choose an image",
            start = "S:/",
            exts  = { "bin" },
            on_pick = function(path, e)
                local it = image_from(path, e.size)
                if it then
                    it.browsed = true
                    show_selected(it)
                end
            end,
        })
    end)

    -- Advanced: any file at a typed flash offset (a bootloader, a partition
    -- table, a filesystem image from a custom build).
    button(v, "Write a file at an offset...", function()
        filepick.open(root, {
            mode  = "file",
            title = "Choose a file to write",
            start = "S:/",
            exts  = { "bin" },
            on_pick = function(path, e)
                local it = image_from(path, e.size)
                if not it then return end
                it.kind = KIND_RAW
                show_offset(it)
            end,
        })
    end)

    button(v, "Back up a device...", function()
        show_connect(nil)
    end)

    button(v, "Download a Meshpunk release...", function()
        show_releases()
    end)

    local images = scan_images()
    if #images == 0 then
        text(v, "No .bin files in the firmware folder or the top folder of the SD card. " ..
                "Copy a firmware file there, or browse to one.")
        return
    end

    for _, it in ipairs(images) do
        local line = string.format("%s\n%s  %s", it.label, kind_text(it), fileman.size_str(it.size))
        local b = v:Button { w = lvgl.PCT(100), h = 44 }
        b:Label { text = line, align = lvgl.ALIGN.LEFT_MID }
        b:onClicked(function() show_selected(it) end)
    end
end

-- ── Meshpunk releases over WiFi ──────────────────────────────────────────────
-- GitHub's releases/latest answer for the channel's repo, read with
-- _wifi_fetch (12-25 KB of compact JSON) and picked apart with patterns:
-- inside "assets":[...] every asset carries "name" before "size" before
-- "browser_download_url". Only the -merged.bin / -firmware.bin files are
-- offered.
local function parse_release(json)
    local tag = json:match('"tag_name":"([^"]+)"')
    local assets = json:match('"assets":%[(.-)%]')
    if not tag or not assets then return nil end
    local out = {}
    for name, size, url in assets:gmatch('"name":"([^"]+)".-"size":(%d+).-"browser_download_url":"([^"]+)"') do
        local slug, _, tail = name:match("^meshpunk%-([^%-]+)%-(.+)%-(%a+)%.bin$")
        if slug and (tail == "merged" or tail == "firmware") then
            out[#out + 1] = { name = name, size = tonumber(size) or 0, url = url,
                              slug = slug, kind = tail }
        end
    end
    table.sort(out, function(a, b)
        if a.slug ~= b.slug then return a.slug < b.slug end
        return a.kind < b.kind
    end)
    return tag, out
end

show_releases = function()
    set_step(nil)
    release()
    local v = new_view("Meshpunk releases", false)
    text(v, "Fetches the latest release of a channel from GitHub and saves the files you pick " ..
            "to the firmware folder on the SD card.")
    local st = text(v, "")

    local sd_ok = false
    for _, d in ipairs(fileman.drives()) do
        if d.id == "S" and d.mounted then sd_ok = true end
    end
    if not sd_ok then
        st:set { text = "No SD card. The files are saved to the card." }
        button(v, "Back", function() show_pick() end)
        return
    end

    local busy = false
    local function fetch(ch)
        if busy then return end
        busy = true
        st:set { text = "Connecting to WiFi..." }
        downloader.wifi_wait(15000, function(connected)
            if not connected then
                busy = false
                st:set { text = "No WiFi connection. Set one up in Settings > Wifi and try again." }
                return
            end
            st:set { text = "Fetching the " .. ch.label .. " release list..." }
            -- One tick later so the label is drawn before the blocking fetch.
            apps.add_timer { period = 100, cb = function(t)
                t:delete()
                local res = _wifi_fetch("https://api.github.com/repos/" .. ch.repo .. "/releases/latest")
                busy = false
                if not (res and res.success) then
                    st:set { text = "Could not reach GitHub: " .. tostring(res and res.error or "?") }
                    return
                end
                if res.status == 404 then
                    st:set { text = "No release published on the " .. ch.label .. " channel yet." }
                    return
                end
                if res.status ~= 200 then
                    st:set { text = "GitHub answered HTTP " .. tostring(res.status) }
                    return
                end
                local tag, files = parse_release(res.body or "")
                if not tag then
                    st:set { text = "Could not read the release list." }
                    return
                end
                show_release_list(ch, tag, files)
            end }
        end)
    end

    for _, ch in ipairs(CHANNELS) do
        button(v, string.format("%s  (%s)", ch.label, ch.repo), function() fetch(ch) end)
    end
    button(v, "Back", function() show_pick() end)
end

show_release_list = function(ch, tag, files)
    local v = new_view(ch.label .. " " .. tag, false)
    text(v, "Tap a file to download it to " .. IMAGE_DIR .. ". A merged image is around 16 MB; " ..
            "the screen does not update while a file downloads.")
    local st = text(v, "")
    local rows = {}

    local function row_text(f)
        local have = fileman.exists(fileman.join(IMAGE_DIR, f.name))
        return string.format("%s  %s  %s%s", board_name(f.slug),
                             f.kind == "merged" and "full image" or "update",
                             fileman.size_str(f.size), have and "  (on card)" or "")
    end

    -- Confirmation popup with the exact file name; once confirmed it stays
    -- up through the (blocking) download and shows the result.
    local busy = false
    local function download(f)
        if busy then return end
        local total, used = fileman.df("S")
        if total and used and total - used < f.size + 65536 then
            st:set { text = string.format("Not enough room on the card: %s free, %s needed.",
                                          fileman.size_str(total - used), fileman.size_str(f.size)) }
            return
        end
        if not mkdirs(IMAGE_DIR) then
            st:set { text = "Could not create " .. IMAGE_DIR .. " on the card." }
            return
        end
        local dst = fileman.join(IMAGE_DIR, f.name)
        busy = true
        local existing = fileman.stat(dst)
        modal(function(box, close)
            local msg = box:Label {
                text = string.format("Download this file?\n\n%s\n\n%s, to %s", f.name,
                                     fileman.size_str(f.size), IMAGE_DIR),
                w = lvgl.PCT(100), h = lvgl.SIZE_CONTENT,
            }
            if existing then
                box:Label {
                    text = string.format("A file with this name is already on the card (%s). " ..
                                         "It will be OVERWRITTEN.", fileman.size_str(existing.size)),
                    text_color = "#ff5555", w = lvgl.PCT(100), h = lvgl.SIZE_CONTENT,
                }
            end
            local yes = box:Button { w = lvgl.PCT(100), h = 28 }
            yes:Label { text = existing and "Yes, overwrite it" or "Yes, download", align = lvgl.ALIGN.CENTER }
            local no = box:Button { w = lvgl.PCT(100), h = 28 }
            local no_lbl = no:Label { text = "No", align = lvgl.ALIGN.CENTER }
            local started = false
            no:onevent(lvgl.EVENT.RELEASED, function()
                if started then return end
                busy = false
                close()
            end)
            yes:onevent(lvgl.EVENT.RELEASED, function()
                if started then return end
                started = true
                msg:set { text = string.format("Downloading\n%s\n(%s)\n\nPlease wait - the screen " ..
                                               "does not update until it finishes.", f.name,
                                               fileman.size_str(f.size)) }
                yes:add_flag(lvgl.FLAG.HIDDEN)
                no:add_flag(lvgl.FLAG.HIDDEN)
                -- One tick later so the text above is drawn first.
                apps.add_timer { period = 100, cb = function(t)
                    t:delete()
                    local res = _wifi_download_file(f.url, dst)
                    busy = false
                    if res and res.success then
                        msg:set { text = "Saved\n" .. dst }
                    else
                        fileman.remove(dst)
                        msg:set { text = "Download failed:\n" .. tostring(res and res.error or "?") }
                    end
                    for i, ff in ipairs(files) do rows[i]:set { text = row_text(ff) } end
                    no_lbl:set { text = "Close" }
                    no:clear_flag(lvgl.FLAG.HIDDEN)
                    started = false
                end }
            end)
        end)
    end

    if #files == 0 then text(v, "This release has no flashable files.") end
    for i, f in ipairs(files) do
        local b = v:Button { w = lvgl.PCT(100), h = 30 }
        rows[i] = b:Label { text = row_text(f), align = lvgl.ALIGN.LEFT_MID }
        b:onClicked(function() download(f) end)
    end
    button(v, "Back", function() show_releases() end, lvgl.PCT(48))
    button(v, "Done", function() show_pick() end, lvgl.PCT(48))
end

-- ── Selected file ────────────────────────────────────────────────────────────
-- The full name, readable, before anything happens to the target: a list row
-- or a picker tap is easy to get wrong with long names on a small screen.

show_selected = function(it)
    set_step(nil)
    release()
    local v = new_view("Selected file", false)
    text(v, it.path)
    text(v, string.format("%s, %s", kind_text(it), fileman.size_str(it.size)))
    if it.slug then text(v, "Meshpunk release for the " .. board_name(it.slug) .. ", " .. it.ver) end
    if it.kind == KIND_FULL then
        text(v, "A full image: the target is erased completely before it is written.")
    else
        text(v, "An update: written into the target's app partition, its data kept. " ..
                "(The file itself decides; a full image under another name is treated as one.)")
    end
    button(v, "Use this file", function() show_connect(it) end, lvgl.PCT(48))
    -- Back to where this file was found: the picker, reopened in the same
    -- folder, or the list.
    button(v, "Choose another", function()
        if not it.browsed then show_pick(); return end
        filepick.open(root, {
            mode  = "file",
            title = "Choose an image",
            start = fileman.parent(it.path) or "S:/",
            exts  = { "bin" },
            on_pick = function(path, e)
                local nxt = image_from(path, e.size)
                if nxt then
                    nxt.browsed = true
                    show_selected(nxt)
                end
            end,
            on_cancel = function() show_pick() end,
        })
    end, lvgl.PCT(48))
end

-- ── Offset entry (raw writes) ────────────────────────────────────────────────

show_offset = function(it)
    local v = new_view("Write at an offset", false)
    text(v, it.path)
    text(v, fileman.size_str(it.size))
    text(v, "Flash offset in hex (a multiple of 0x1000). Common ones: 0 bootloader, " ..
            "8000 partition table, E000 boot_app0, 10000 app.")
    local ta = v:Textarea { one_line = true, text = "10000", w = lvgl.PCT(100), h = 32 }
    ta:clear_flag(lvgl.FLAG.SCROLLABLE)
    local err = text(v, "")
    local function go()
        local s = (ta.text or ""):gsub("^0[xX]", "")
        local off = tonumber(s, 16)
        if not off or off < 0 then err:set { text = "Enter a hex number" }; return end
        if off % 0x1000 ~= 0 then err:set { text = "Must be a multiple of 0x1000" }; return end
        it.offset = off
        show_connect(it)
    end
    button(v, "Continue", go, lvgl.PCT(48))
    button(v, "Cancel", function() show_pick() end, lvgl.PCT(48))
    ta:onevent(lvgl.EVENT.KEY, function()
        if lvgl.indev.get_act():get_key() == lvgl.KEY.ENTER then go() end
    end)
end

-- ── Connect ──────────────────────────────────────────────────────────────────
-- `it` = the image to flash, or nil for a backup-only session.

show_connect = function(it)
    job = { it = it, want_table = (it == nil) }
    local v = new_view("Connect the target", false)

    local ok_caps, caps = pcall(_device_caps)
    local powers = ok_caps and type(caps) == "table" and caps.usb_power

    if it then
        text(v, string.format("%s  (%s, %s)", it.label, kind_text(it):lower(), fileman.size_str(it.size)))
    else
        text(v, "Back up a device: its partitions are read into files on this SD card.")
    end
    text(v, "Connect the target to this device with a USB-C cable.\n" ..
            (powers and "This device powers the target."
                     or  "The target needs its own battery or power."))
    ui.status = text(v, "Starting USB host mode...")
    ui.hint = text(v, "")
    button(v, "Back", function() show_pick() end)

    if not _usb_running() then
        if not _usb_start() then
            ui.status:set { text = "USB host mode could not start. Tools > USB Host shows why." }
            return
        end
        started_host = true
    end
    if not send(OP_HOLD) then
        ui.status:set { text = "The USB driver channel is not available." }
        return
    end
    ui.status:set { text = "Waiting for the target..." }
    ui.hint:set { text = "If it is not found, put the target into download mode and wait." }
    set_step("wait_dev")
end

-- ── Confirm ──────────────────────────────────────────────────────────────────

show_confirm = function(info)
    set_step(nil)
    local it = job.it
    job.info = info
    local v = new_view("Ready to flash", false)

    text(v, it.path)
    text(v, string.format("%s  (%s)", it.label, fileman.size_str(it.size)))
    if info.slug then
        text(v, "The target cannot tell us which board it is. Flash this only if the target is a " ..
                board_name(info.slug) .. ".")
    else
        text(v, "The target cannot tell us which board it is, and this image carries no board " ..
                "name. Make sure it was built for the connected device.")
    end
    if info.kind == KIND_FULL then
        text(v, "FULL IMAGE: the whole flash is erased first, so everything on the target is " ..
                "lost - identity, settings, contacts and messages.")
    elseif info.kind == KIND_RAW then
        text(v, string.format("Writes the file at 0x%X. Nothing else is checked.", info.offset))
    else
        text(v, string.format("UPDATE: replaces the firmware at 0x%X and keeps the target's data.",
                              info.offset))
    end
    text(v, "Keep the cable connected until it finishes. A stopped flash leaves the target " ..
            "unable to start until it is flashed again.")

    -- The parts written after the image on the same connection: Meshtastic
    -- companions found beside a .factory.bin, plus any backups added here.
    if not job.extra then
        job.extra = (info.kind == KIND_FULL) and companions(it, info.chip) or {}
    end
    local extra = job.extra

    local function start(parts)
        job.queue  = parts
        job.part_i = 1
        job.part_n = 1 + #parts
        job.part   = it
        job.write_nonce = send(OP_WRITE)
        if not job.write_nonce then
            show_fail("The USB driver channel is not available.")
            return
        end
        show_progress("Flashing")
        set_step("write")
    end

    if #extra > 0 then
        local lines = { "Also written, after the image:" }
        for _, p in ipairs(extra) do
            lines[#lines + 1] = string.format("%s (%s, at 0x%X)", p.name, p.what, p.offset)
        end
        text(v, table.concat(lines, "\n"))
    end
    ui.note = text(v, "")

    if info.kind == KIND_FULL then
        button(v, "Back up the target first...", function()
            job.after = "confirm"
            job.want_table = true
            set_step("settle")
            show_progress("Backing up")
            ui.status:set { text = "Reading the target's partition table..." }
        end)
        -- A partition backup goes back where it came from: the image's table
        -- must have a partition there that is large enough.
        button(v, "Add a backup file to restore...", function()
            filepick.open(root, {
                mode  = "file",
                title = "Choose a backup file",
                start = BACKUP_DIR,
                exts  = { "bin" },
                on_pick = function(path, e)
                    local b = backup_from_name(path, e.size)
                    if not b then
                        ui.note:set { text = "Not a partition backup (name must be <label>-<offset>-<size>.bin)." }
                        return
                    end
                    local tbl = file_table(it.path) or {}
                    local hit
                    for _, p in ipairs(tbl) do
                        if p.off == b.offset and p.size >= b.size then hit = p end
                    end
                    if not hit then
                        ui.note:set { text = string.format(
                            "The image has no partition at 0x%X big enough for %s.", b.offset, b.name) }
                        return
                    end
                    extra[#extra + 1] = b
                    show_confirm(info)
                end,
            })
        end)
    end

    if #extra > 0 then
        button(v, string.format("Flash all %d files", 1 + #extra), function() start(extra) end)
        button(v, "Flash the image only", function() start({}) end)
        button(v, "Cancel", function() show_pick() end)
    else
        button(v, "Flash", function() start({}) end, lvgl.PCT(48))
        button(v, "Cancel", function() show_pick() end, lvgl.PCT(48))
    end
end

-- The target's partition table has no app slot at 0x10000: the driver listed
-- the slots it does have ("slots 10000 210000 ..."), one button each.
show_slots = function(txt)
    set_step(nil)
    local v = new_view("Choose where to write", false)
    text(v, "This target's partition table has no app slot at the usual 0x10000. " ..
            "Its app partitions are:")
    for hex in txt:gmatch("%x+") do
        if hex ~= "" then
            local off = tonumber(hex, 16)
            button(v, string.format("0x%X", off), function()
                job.it.offset = off
                show_connect(job.it)
            end)
        end
    end
    button(v, "Cancel", function() show_pick() end)
end

-- ── Backup: choose partitions ────────────────────────────────────────────────
-- `entries` = the target's partition table, `flash` = its flash size.

show_backup = function(entries, flash)
    set_step(nil)
    local v = new_view("Back up", false)
    text(v, string.format("Target: %s, %s flash. Tick what to back up. Reading is slow " ..
                          "(64 bytes at a time): %s per MB.", job.chip or "?",
                          fileman.size_str(flash), minutes_text(1024 * 1024)))

    -- Data partitions start ticked (settings, identity, filesystems); app
    -- partitions can be ticked too.
    local picked = {}
    local total = 0
    local labels = {}
    local function row_text(i, e)
        return string.format("%s %s  0x%X  %s  (%s)", picked[i] and "[x]" or "[ ]",
                             e.label, e.off, fileman.size_str(e.size), e.type == 0 and "app" or "data")
    end
    for i, e in ipairs(entries) do
        picked[i] = (e.type == 1)
        if picked[i] then total = total + e.size end
    end
    local go_lbl
    local function refresh()
        total = 0
        for i, e in ipairs(entries) do
            if picked[i] then total = total + e.size end
            labels[i]:set { text = row_text(i, e) }
        end
        go_lbl:set { text = total > 0
            and string.format("Back up ticked (%s, %s)", fileman.size_str(total), minutes_text(total))
            or  "Nothing ticked" }
    end
    for i, e in ipairs(entries) do
        local b = v:Button { w = lvgl.PCT(100), h = 26 }
        labels[i] = b:Label { text = row_text(i, e), align = lvgl.ALIGN.LEFT_MID }
        b:onClicked(function()
            picked[i] = not picked[i]
            refresh()
        end)
    end

    -- One backup folder per run: backups/<chip>, <chip>_2, ...
    local function new_backup_dir()
        if not mkdirs(BACKUP_DIR) then return nil end
        local stem = ((job.chip or "device"):lower():gsub("[^%w]", ""))
        local dir = fileman.unique_path(BACKUP_DIR, stem)
        if not dir or not fileman.mkdir(dir) then return nil end
        return dir
    end

    local function run(parts)
        local dir = new_backup_dir()
        if not dir then show_fail("Could not create a folder under " .. BACKUP_DIR .. "."); return end
        for _, p in ipairs(parts) do p.path = fileman.join(dir, p.name) end
        job.backup_dir = dir
        job.queue  = parts
        job.part_n = #parts
        job.part_i = 0
        show_progress("Backing up")
        text(view, "Reading is slow: the bar moves every few seconds. Do not unplug the cable.")
        set_step("read_next")
    end

    local go_btn
    go_btn, go_lbl = button(v, "", function()
        local parts = {}
        for i, e in ipairs(entries) do
            if picked[i] then
                parts[#parts + 1] = { name = string.format("%s-%X-%X.bin", e.label, e.off, e.size),
                                      offset = e.off, size = e.size }
            end
        end
        if #parts > 0 then run(parts) end
    end)
    refresh()

    button(v, string.format("Whole device (%s, %s)", fileman.size_str(flash), minutes_text(flash)), function()
        run({ { name = string.format("whole-device-%X.bin", flash), offset = 0, size = flash } })
    end)
    text(v, "A whole-device file is an exact copy, bootloader included: it flashes back as a " ..
            "full image, and onto a second board it would clone this one's identity.")
    button(v, job.after == "confirm" and "Back to the flash" or "Cancel", function()
        if job.after == "confirm" then
            job.want_table = false
            set_step("settle")
            show_progress("Checking")
        else
            show_pick()
        end
    end)
end

-- ── Progress ─────────────────────────────────────────────────────────────────

show_progress = function(title)
    local v = new_view(title, false)
    ui.status = text(v, "Starting...")
    ui.bar_bg = v:Object {
        w = lvgl.PCT(100), h = 14,
        bg_color = "#222222", bg_opa = 255, radius = 4,
        border_width = 1, border_color = "#666666", pad_all = 0,
    }
    ui.bar_bg:clear_flag(lvgl.FLAG.SCROLLABLE)
    ui.bar = ui.bar_bg:Object {
        w = 1, h = lvgl.PCT(100),
        bg_color = "#44AA44", bg_opa = 255, radius = 3, border_width = 0,
    }
    text(v, "Do not unplug the cable or leave this app.")

    local armed = false
    local cancel_btn, cancel_lbl
    cancel_btn, cancel_lbl = button(v, "Stop", function()
        if not armed then
            armed = true
            cancel_lbl:set { text = "Tap again to stop" }
            return
        end
        send(OP_CANCEL)
    end)
end

-- ── Done / failed ────────────────────────────────────────────────────────────

show_done = function(manual)
    set_step(nil)
    release()
    local v = new_view("Done", true)
    if job.backup_dir and not job.it then
        text(v, string.format("%d file(s) saved and verified in %s.", job.part_n or 0, job.backup_dir))
    elseif (job.part_n or 1) > 1 then
        text(v, string.format("%s and %d more files were written and verified.",
                              job.it.label, job.part_n - 1))
    else
        text(v, job.it.label .. " was written and verified.")
    end
    text(v, manual and "The target was put into download mode by hand, so it may not restart " ..
                       "by itself: press its reset button if it stays dark."
                   or  "The target is restarting. You can unplug it.")
    button(v, "Back", function() show_pick() end)
end

show_fail = function(reason)
    set_step(nil)
    release()
    local v = new_view("Stopped", true)
    text(v, reason)
    button(v, "Back", function() show_pick() end)
end

-- ── Poll timer: the flash state machine ─────────────────────────────────────

local STAGE_TEXT = { "Connecting", "Checking the image", "Reading the target", "Writing",
                     "Restarting", "Reading" }

local function answered(st, n, op)
    return st and n and st.nonce == n and st.op == op and st.state ~= ST_BUSY
end

-- The driver acknowledges every command it takes at once (a busy status
-- with the command's nonce). Ten seconds without one = a driver that does
-- not know the command, i.e. an old espserial.drv.elf.
local STALE_DRIVER = "The USB driver did not take the command. Update the espserial driver " ..
                     "(espserial.drv.elf and match) to the one that matches this firmware."
local function unanswered(st, n)
    return count > ticks(10000) and not (st and st.nonce == n)
end

-- "File 2 of 3 (littlefs-x.bin): " while a set is being worked on, else "".
local function part_prefix()
    if (job.part_n or 1) <= 1 then return "" end
    return string.format("File %d of %d (%s): ", job.part_i, job.part_n, job.part.name)
end

local function set_status(s)
    if ui.status then ui.status:set { text = s } end
end

local function set_bar(pct)
    if ui.bar then ui.bar:set { w = lvgl.PCT(math.max(1, pct)) } end
end

-- A failure that ends the session: restart the target, then report.
local function fail_then_reset(reason)
    job.reason = reason
    job.reset_nonce = send(OP_RESET)
    set_status("Restarting the target...")
    set_step("reset")
end

-- After BOOT/settle: either read the target's partition table (backup) or
-- check the image (flash).
local function after_settle(st)
    job.stamp = st.stamp
    if job.want_table then
        mkdirs(BACKUP_DIR)
        job.read_nonce = send(OP_READ, read_payload { offset = 0x8000, size = 0xC00, path = TABLE_FILE })
        set_status("Reading the target's partition table...")
        set_step("table")
    else
        job.check_nonce = send(OP_CHECK, check_payload(job.it))
        set_status("Checking...")
        set_step("check")
    end
end

local function poll()
    if not step then return end
    count = count + 1
    local st = status()
    if st then lost = 0 else lost = lost + 1 end

    if step == "wait_dev" then
        -- No driver status = no serial port on the bus yet.
        if st and st.hold then
            job.stamp = st.stamp
            job.boot_nonce = send(OP_BOOT)
            set_status("Resetting the target into its bootloader...")
            set_step("boot")
        end

    elseif step == "boot" then
        -- The target may drop off the bus and come back: a new stamp is the
        -- driver instance that loaded for the re-attached target.
        if st and st.hold and (answered(st, job.boot_nonce, OP_BOOT) or st.stamp ~= job.stamp) then
            set_step("settle")
        elseif count > ticks(12000) then
            show_fail("The target did not come back after the reset.")
        end

    elseif step == "settle" then
        if st and count >= ticks(750) then
            after_settle(st)
        elseif count > ticks(12000) then
            show_fail("The target did not come back after the reset.")
        end

    elseif step == "check" or step == "table" then
        local n, op = job.check_nonce, OP_CHECK
        if step == "table" then n, op = job.read_nonce, OP_READ end
        if lost > ticks(3000) or (st and st.stamp ~= job.stamp) then
            -- Unplugged or re-attached (e.g. put into download mode by hand):
            -- sent again once a driver instance holds the port.
            set_status("Waiting for the target...")
            set_step("retry")
        elseif unanswered(st, n) then
            show_fail(STALE_DRIVER)
        elseif answered(st, n, op) then
            if st.state == ST_OK and step == "table" then
                -- "read <off> <len> <chip> <flash hex>"
                local chip, flash = st.text:match("^read %x+ %d+ (%S+) (%x+)$")
                job.chip = chip
                local entries = parse_table(fileman.read(TABLE_FILE))
                fileman.remove(TABLE_FILE)
                if not entries or #entries == 0 then
                    fail_then_reset("The target has no partition table to back up.")
                else
                    show_backup(entries, tonumber(flash or "0", 16) or 0)
                end
            elseif st.state == ST_OK then
                -- "<kind> <offset hex> <size> <chip> [<board slug>]"
                local kind, off, chip, slug = st.text:match("^(%a+) (%x+) %d+ (%S+) ?(%S*)$")
                show_confirm {
                    kind   = (kind == "full") and KIND_FULL or (kind == "raw") and KIND_RAW or KIND_UPDATE,
                    offset = tonumber(off or "0", 16) or 0,
                    chip   = chip,
                    slug   = (slug and slug ~= "" and slug) or job.it.slug,
                }
            elseif st.text:match("^slots ") then
                show_slots(st.text)
            elseif st.text == NOT_IN_BOOTLOADER then
                set_status("The target did not enter its bootloader.")
                if ui.hint then
                    ui.hint:set { text = "Put the target into download mode by hand (hold its BOOT " ..
                                         "button while pressing reset, or while plugging it in). " ..
                                         "Trying again every few seconds..." }
                end
                set_step("retry")
            else
                show_fail(st.text)
            end
        elseif st and st.nonce == n and st.state == ST_BUSY then
            set_status(string.format("%s... %d%%", STAGE_TEXT[st.stage] or "Working", st.pct))
        end

    elseif step == "retry" then
        if st and st.hold and count >= ticks(3000) then
            after_settle(st)
        end

    elseif step == "read_next" then
        -- Next backup part, or finished.
        if #job.queue == 0 then
            if job.after == "confirm" then
                -- Back to the flash: the check again (the read reused the
                -- driver's file handle), then the confirm screen.
                job.after = nil
                job.want_table = false
                job.part_n = nil
                set_status("Checking the image again...")
                set_step("settle")
            else
                job.reset_nonce = send(OP_RESET)
                set_status("Restarting the target...")
                set_step("reset")
            end
        else
            job.part   = table.remove(job.queue, 1)
            job.part_i = job.part_i + 1
            job.read_nonce = send(OP_READ, read_payload(job.part))
            set_status(part_prefix() .. "reading...")
            set_bar(0)
            set_step("read")
        end

    elseif step == "read" then
        if lost > ticks(3000) or (st and st.stamp ~= job.stamp) then
            show_fail("The target disconnected during the backup.")
        elseif unanswered(st, job.read_nonce) then
            show_fail(STALE_DRIVER)
        elseif answered(st, job.read_nonce, OP_READ) then
            if st.state == ST_OK then
                set_step("read_next")
            else
                fail_then_reset(job.part.name .. ": " .. st.text)
            end
        elseif st and st.nonce == job.read_nonce then
            set_status(string.format("%sreading... %d%%", part_prefix(), st.pct))
            set_bar(st.pct)
        end

    elseif step == "write" then
        if lost > ticks(3000) or (st and st.stamp ~= job.stamp) then
            show_fail("The target disconnected before the flash finished. It will not start " ..
                      "until it is flashed again.")
        elseif unanswered(st, job.write_nonce) then
            show_fail(STALE_DRIVER)
        elseif answered(st, job.write_nonce, OP_WRITE) then
            if st.state == ST_OK and job.queue and #job.queue > 0 then
                -- The next file of the set, on the same connection.
                job.part   = table.remove(job.queue, 1)
                job.part_i = job.part_i + 1
                job.check_nonce = send(OP_CHECK, check_payload(job.part))
                set_status(part_prefix() .. "checking...")
                set_step("part_check")
            elseif st.state == ST_OK then
                job.reason = nil
                job.reset_nonce = send(OP_RESET)
                set_status("Restarting the target...")
                set_step("reset")
            else
                fail_then_reset(st.text)
            end
        elseif st and st.nonce == job.write_nonce then
            local pre = part_prefix()
            set_status(string.format("%s%s and verifying... %d%%", pre,
                                     pre == "" and "Writing" or "writing", st.pct))
            set_bar(st.pct)
        end

    elseif step == "part_check" then
        if lost > ticks(3000) or (st and st.stamp ~= job.stamp) then
            show_fail("The target disconnected before the flash finished. It will not start " ..
                      "until it is flashed again.")
        elseif unanswered(st, job.check_nonce) then
            show_fail(STALE_DRIVER)
        elseif answered(st, job.check_nonce, OP_CHECK) then
            if st.state == ST_OK then
                job.write_nonce = send(OP_WRITE)
                set_step("write")
            else
                fail_then_reset(job.part.name .. ": " .. st.text)
            end
        end

    elseif step == "reset" then
        -- The restart can take the target off the bus before the answer is
        -- read, so a lost or new driver instance also ends the wait.
        if answered(st, job.reset_nonce, OP_RESET) or lost > ticks(1500)
           or (st and st.stamp ~= job.stamp) or count > ticks(5000) then
            if job.reason then
                show_fail(job.reason == "cancelled"
                    and (job.it and "Stopped. The target will not start until it is flashed again."
                                or  "Stopped.")
                    or  ((job.it and "The flash failed: " or "The backup failed: ") .. job.reason ..
                         (job.it and ". The target will not start until it is flashed again." or ".")))
            else
                show_done(st ~= nil and st.nonce == job.reset_nonce and st.text == "manual")
            end
        end
    end
end

apps.add_timer { period = POLL_MS, cb = poll }

show_pick()

return root
