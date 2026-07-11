--[[
  fileman — drive-aware file management for MeshPunk.

  ANY app that needs file operations should use this instead of talking to the
  raw bindings. Paths are drive-prefixed the same way io.open() takes them:

      "L:/lua/apps"       internal LittleFS (also the default with no prefix)
      "S:/meshpunk"       SD card

  All functions normalize their inputs, so "/lua", "L:/lua" and "L://lua/" are
  the same place. Backed by the C _fs_* bridge (src/fs_bridge.cpp).

  Quick ops (synchronous, bounded work — fine to call directly):
      fileman.drives()               -> { {id="L", label=, root=, mounted=, total=, used=}, ... }
      fileman.list(path [, opts])    -> entries|nil, err   (sorted dirs-first;
                                        opts.filter = function(entry) -> bool;
                                        opts.sizes = false skips per-entry
                                        sizes — much faster on big folders,
                                        entries come back with size = 0)
      fileman.stat(path)             -> { type="file"|"dir", size= } | nil
      fileman.exists(path)           -> bool
      fileman.is_dir(path)           -> bool
      fileman.mkdir(path)            -> bool               (creates parents)
      fileman.remove(path)           -> ok, err            (file or EMPTY dir)
      fileman.rename(src, dst)       -> ok, err            (same drive only)
      fileman.copy_file(src, dst)    -> ok, err            (one file, any drives)
      fileman.move(src, dst)         -> ok, err            (file; rename fast-path)
      fileman.read(path)             -> data|nil, err
      fileman.write(path, data)      -> ok, err            (parent must exist)
      fileman.df(drive)              -> total, used | nil  (drive = "L"|"S")

  Bulk ops (a whole tree) MUST use a task so the UI/watchdog stay alive:

      local task = fileman.task_copy(src, dst)    -- file or directory tree
      local task = fileman.task_move(src, dst)    -- rename fast-path same drive
      local task = fileman.task_remove(path)      -- recursive delete

      -- drive it from a timer; each step() is one bounded unit of work
      -- (one directory scanned, or one file copied/removed):
      local done, err = task.step()
      task.current      -- name of the item being processed (for progress UI)
      task.files_done   -- files completed so far
      task.files_total  -- nil while scanning, then the file count
      task.cancelled    -- set true to abort on the next step

  Path helpers: split, normalize, join, parent, basename, ext, size_str,
  unique_path, is_protected.
]]

local M = {}

-- ── Path helpers ─────────────────────────────────────────────────────────────

-- "S:/foo" -> "S", "/foo".  No prefix -> "L".  U: = USB thumb drive.
function M.split(path)
    path = tostring(path or "")
    local d, rest = path:match("^([LlSsUu]):(.*)$")
    if d then return d:upper(), rest end
    return "L", path
end

-- Canonical form: upper drive prefix, single slashes, no trailing slash
-- (except the bare root "X:/").
function M.normalize(path)
    local d, rest = M.split(path)
    if rest == "" then rest = "/" end
    if rest:sub(1, 1) ~= "/" then rest = "/" .. rest end
    rest = rest:gsub("/+", "/")
    if #rest > 1 then rest = rest:gsub("/+$", "") end
    return d .. ":" .. rest
end

function M.join(dir, name)
    return M.normalize(tostring(dir) .. "/" .. tostring(name))
end

-- Parent directory, or nil at a drive root.
function M.parent(path)
    path = M.normalize(path)
    local d, rest = M.split(path)
    if rest == "/" then return nil end
    local p = rest:match("^(.*)/[^/]+$") or ""
    if p == "" then p = "/" end
    return d .. ":" .. p
end

-- Last path component ("/" for a drive root).
function M.basename(path)
    local _, rest = M.split(M.normalize(path))
    return rest:match("([^/]+)$") or "/"
end

-- Lowercased extension without the dot, or nil.
function M.ext(path)
    local e = M.basename(path):match("%.([^.]+)$")
    return e and e:lower() or nil
end

-- ── Drives ───────────────────────────────────────────────────────────────────

function M.drives()
    local sd_ok, usb_ok = false, false
    local ok, info = pcall(_storage_get_info)
    if ok and type(info) == "table" then
        sd_ok  = info.sd_available  and true or false
        usb_ok = info.usb_available and true or false
    end
    local out = {
        { id = "L", label = "Internal",  root = "L:/", mounted = true },
        { id = "S", label = "SD card",   root = "S:/", mounted = sd_ok },
        { id = "U", label = "USB drive", root = "U:/", mounted = usb_ok },
    }
    for _, d in ipairs(out) do
        if d.mounted then
            local okd, total, used = pcall(_fs_df, d.id)
            if okd and total then
                d.total, d.used = total, used
            end
        end
    end
    return out
end

function M.df(drive)
    local ok, total, used = pcall(_fs_df, drive)
    if ok and total then return total, used end
    return nil
end

-- ── Quick ops ────────────────────────────────────────────────────────────────

-- Entries sorted directories-first, then case-insensitive by name.
-- opts.filter(entry) -> bool keeps only matching entries.
-- opts.sizes = false skips the per-entry size lookup (the slow part of
-- listing a big directory); entries then carry size = 0.
function M.list(path, opts)
    local want_sizes = not (opts and opts.sizes == false)
    local ok, entries, err = pcall(_fs_list, M.normalize(path), want_sizes)
    if not ok then return nil, tostring(entries) end
    if not entries then return nil, err or "list failed" end
    if opts and opts.filter then
        local kept = {}
        for _, e in ipairs(entries) do
            if opts.filter(e) then kept[#kept + 1] = e end
        end
        entries = kept
    end
    table.sort(entries, function(a, b)
        if a.type ~= b.type then return a.type == "dir" end
        return a.name:lower() < b.name:lower()
    end)
    return entries
end

function M.stat(path)
    local ok, st = pcall(_fs_stat, M.normalize(path))
    if ok and type(st) == "table" then return st end
    return nil
end

function M.exists(path)
    local ok, ex = pcall(_fs_exists, M.normalize(path))
    return (ok and ex) and true or false
end

function M.is_dir(path)
    local st = M.stat(path)
    return (st and st.type == "dir") and true or false
end

function M.mkdir(path)
    local ok, res = pcall(_fs_mkdir, M.normalize(path))
    return (ok and res) and true or false
end

-- ok, err triple from the C side, pcall-armored.
local function ok_err(pok, res, err)
    if not pok then return false, tostring(res) end
    if not res then return false, err end
    return true
end

function M.remove(path)
    return ok_err(pcall(_fs_remove, M.normalize(path)))
end

function M.rename(src, dst)
    return ok_err(pcall(_fs_rename, M.normalize(src), M.normalize(dst)))
end

function M.copy_file(src, dst)
    return ok_err(pcall(_fs_copy, M.normalize(src), M.normalize(dst)))
end

-- Move ONE file (or same-drive rename of anything). For a cross-drive
-- directory tree use task_move.
function M.move(src, dst)
    src, dst = M.normalize(src), M.normalize(dst)
    local sdrive = M.split(src)
    local ddrive = M.split(dst)
    if sdrive == ddrive then
        return M.rename(src, dst)
    end
    local st = M.stat(src)
    if not st then return false, "source not found" end
    if st.type == "dir" then return false, "use task_move for directories" end
    local ok, err = M.copy_file(src, dst)
    if not ok then return false, err end
    return M.remove(src)
end

function M.read(path)
    local f, err = io.open(M.normalize(path), "r")
    if not f then return nil, err or "open failed" end
    local data = f:read("*a")
    f:close()
    return data or ""
end

-- The parent directory must exist (mkdir it first if unsure).
function M.write(path, data)
    local f, err = io.open(M.normalize(path), "w")
    if not f then return false, err or "open failed" end
    f:write(data or "")
    f:close()
    return true
end

-- ── Presentation helpers ─────────────────────────────────────────────────────

function M.size_str(n)
    n = tonumber(n) or 0
    if n < 1024 then return string.format("%dB", n) end
    if n < 1024 * 1024 then return string.format("%.1fK", n / 1024) end
    if n < 1024 * 1024 * 1024 then return string.format("%.1fM", n / (1024 * 1024)) end
    return string.format("%.2fG", n / (1024 * 1024 * 1024))
end

-- A path in `dir` for `name` that doesn't collide: "name", then "name_2",
-- "name_3", ... (extension preserved). nil if 99 attempts all exist.
function M.unique_path(dir, name)
    local target = M.join(dir, name)
    if not M.exists(target) then return target end
    local stem, ext = name:match("^(.+)(%.[^.]+)$")
    if not stem then stem, ext = name, "" end
    for i = 2, 99 do
        target = M.join(dir, stem .. "_" .. i .. ext)
        if not M.exists(target) then return target end
    end
    return nil, "no free name"
end

-- Paths the firmware needs to boot / keep its identity. Deleting or renaming
-- these (or a folder containing them) bricks or resets the device — apps
-- should require an extra confirmation, not silently refuse.
local PROTECTED = {
    "L:/lua/main.lua",
    "L:/lua/launcher.lua",
    "L:/lua/lib",
    "L:/lua/themes/default.lua",
    "L:/identity",
    "L:/node_prefs",
    "L:/contacts",
    "L:/channels",
    "L:/firmware_prefs",
    "L:/wifi_creds",
    "L:/channel_notify",
}

-- True if `path` IS a protected item, contains one (deleting it would take
-- the protected item with it), or lives inside a protected folder.
function M.is_protected(path)
    path = M.normalize(path)
    for _, p in ipairs(PROTECTED) do
        if path == p
            or p:sub(1, #path + 1) == path .. "/"
            or path:sub(1, #p + 1) == p .. "/" then
            return true
        end
    end
    return false
end

-- ── Bulk tasks ───────────────────────────────────────────────────────────────
-- A task walks a tree in bounded steps so callers can drive it from an LVGL
-- timer: phase "scan" lists ONE directory per step (building the work lists),
-- then the op phases process ONE item per step. Relative paths in the lists
-- carry a leading "/" so src..rel / dst..rel concatenate directly.

local function task_fail(msg)
    local t = { current = "", files_done = 0, files_total = 0 }
    function t.step() return true, msg end
    return t
end

-- Shared scan step: pops one dir off t.scan_queue and lists it into
-- t.dirs/t.files. Returns true while scanning, false when the queue is empty.
local function scan_step(t)
    local rel = table.remove(t.scan_queue, 1)
    if rel == nil then return false end
    local base = (rel == "") and t.src or (t.src .. rel)
    t.current = M.basename(base)
    -- sizes=false: the tree walk only needs names/types, and skipping the
    -- per-entry size lookup keeps one step bounded even on huge directories.
    local entries = M.list(base, { sizes = false })
    if entries then
        for _, e in ipairs(entries) do
            local child = rel .. "/" .. e.name
            if e.type == "dir" then
                t.dirs[#t.dirs + 1] = child
                t.scan_queue[#t.scan_queue + 1] = child
            else
                t.files[#t.files + 1] = child
            end
        end
    end
    return true
end

-- Copy src (file or tree) to dst. dst is the FULL destination path (the new
-- name), not the containing folder.
function M.task_copy(src, dst)
    src, dst = M.normalize(src), M.normalize(dst)
    local st = M.stat(src)
    if not st then return task_fail("source not found: " .. src) end
    if src == dst then return task_fail("source = destination") end
    if st.type == "dir" and dst:sub(1, #src + 1) == src .. "/" then
        return task_fail("cannot copy a folder into itself")
    end

    local t = {
        src = src, dst = dst,
        phase = "scan", scan_queue = {}, dirs = {}, files = {},
        files_done = 0, files_total = nil, mkdir_done = 0,
        current = M.basename(src), cancelled = false,
    }
    if st.type == "dir" then
        t.scan_queue[1] = ""
        t.is_dir = true
    else
        t.files[1] = ""
        t.files_total = 1
        t.phase = "copy"
    end

    function t.step()
        if t.cancelled then return true, "cancelled" end

        if t.phase == "scan" then
            if not scan_step(t) then
                t.phase = "mkdir"
                t.files_total = #t.files
            end
            return false
        end

        if t.phase == "mkdir" then
            -- destination root first, then subdirs in parent-first order
            if not t.root_made then
                t.root_made = true
                if not M.mkdir(t.dst) then
                    return true, "mkdir failed: " .. t.dst
                end
                return false
            end
            local d = t.dirs[t.mkdir_done + 1]
            if not d then
                t.phase = "copy"
                return false
            end
            t.mkdir_done = t.mkdir_done + 1
            if not M.mkdir(t.dst .. d) then
                return true, "mkdir failed: " .. t.dst .. d
            end
            return false
        end

        -- copy phase: one file per step
        local f = t.files[t.files_done + 1]
        if not f then return true, nil end
        local from = (f == "") and t.src or (t.src .. f)
        local to = (f == "") and t.dst or (t.dst .. f)
        t.current = M.basename(from)
        local ok, err = M.copy_file(from, to)
        if not ok then
            return true, (err or "copy failed") .. ": " .. from
        end
        t.files_done = t.files_done + 1
        return false
    end
    return t
end

-- Recursively delete a file or tree.
function M.task_remove(path)
    path = M.normalize(path)
    local st = M.stat(path)
    if not st then return task_fail("not found: " .. path) end

    local t = {
        src = path,
        phase = "scan", scan_queue = {}, dirs = {}, files = {},
        files_done = 0, files_total = nil, dirs_done = 0,
        current = M.basename(path), cancelled = false,
    }
    if st.type == "dir" then
        t.scan_queue[1] = ""
        t.is_dir = true
    else
        t.files[1] = ""
        t.files_total = 1
        t.phase = "files"
    end

    function t.step()
        if t.cancelled then return true, "cancelled" end

        if t.phase == "scan" then
            if not scan_step(t) then
                t.phase = "files"
                t.files_total = #t.files
            end
            return false
        end

        if t.phase == "files" then
            local f = t.files[t.files_done + 1]
            if not f then
                t.phase = "dirs"
                return false
            end
            local full = (f == "") and t.src or (t.src .. f)
            t.current = M.basename(full)
            local ok, err = M.remove(full)
            if not ok then
                return true, (err or "remove failed") .. ": " .. full
            end
            t.files_done = t.files_done + 1
            return false
        end

        -- dirs phase: children before parents (reverse discovery order),
        -- then the root itself
        local n = #t.dirs - t.dirs_done
        if n > 0 then
            local d = t.dirs[n]
            t.dirs_done = t.dirs_done + 1
            t.current = M.basename(d)
            local ok, err = M.remove(t.src .. d)
            if not ok then
                return true, (err or "rmdir failed") .. ": " .. t.src .. d
            end
            return false
        end
        if t.is_dir and not t.root_removed then
            t.root_removed = true
            local ok, err = M.remove(t.src)
            if not ok then return true, err or "rmdir failed" end
        end
        return true, nil
    end
    return t
end

-- Move src to dst: instant rename when both are on the same drive, otherwise
-- copy the tree then delete the source.
function M.task_move(src, dst)
    src, dst = M.normalize(src), M.normalize(dst)
    if src == dst then return task_fail("source = destination") end
    if dst:sub(1, #src + 1) == src .. "/" then
        return task_fail("cannot move a folder into itself")
    end

    local sdrive = M.split(src)
    local ddrive = M.split(dst)
    if sdrive == ddrive then
        local t = { current = M.basename(src), files_done = 0, files_total = 1,
                    phase = "move", cancelled = false }
        function t.step()
            if t.cancelled then return true, "cancelled" end
            local ok, err = M.rename(src, dst)
            if not ok then return true, err or "rename failed" end
            t.files_done = 1
            return true, nil
        end
        return t
    end

    -- Cross-drive: copy everything, then remove the source.
    local t = { current = M.basename(src), files_done = 0, files_total = nil,
                phase = "scan", cancelled = false }
    local inner = M.task_copy(src, dst)
    local removing = false
    function t.step()
        if t.cancelled then inner.cancelled = true end
        local done, err = inner.step()
        t.current = inner.current
        t.phase = removing and "remove" or inner.phase
        t.files_done = inner.files_done
        t.files_total = inner.files_total
        if done and not err and not removing then
            removing = true
            inner = M.task_remove(src)
            return false
        end
        return done, err
    end
    return t
end

return M
