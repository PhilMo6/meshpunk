-- lib/keybind.lua — the shared key-binding system for ELF module launchers.
--
-- One key table, one Controls screen, one key picker and one trackball Input
-- screen for every launcher. Each app used to carry its own copy of all four;
-- they had already drifted (every copy was missing the 'y' key, and none could
-- bind '$').
--
-- A launcher declares WHAT it wants bound; this owns HOW it is bound, stored
-- and drawn:
--
--   local kb = keybind.new{
--       actions = { { id="up", label="Up", out=GB.UP, key1="w", key2="TrkUp" }, ... },
--       root = root, show_screen = show_screen,   -- the app's own view swapper
--       font = FONT, accent = ACCENT,
--       on_back = function() create_main_screen() end,
--       on_save = function() save_config() end,
--       trackball = { momentum=true, impulse=15, friction=82, thresh=4 },
--   }
--   kb:open()              -- Controls screen
--   kb:keymap_string()     -- "AD=77+81,..." for -keymap (nil when nothing bound)
--   kb:trkball_string()    -- "1,15,82,4"    for -trkball
--   kb:save_lines(f)       -- app keeps owning its config FILE; we own our lines
--   kb:load_line(line)     -- true when the line was ours
--
-- The on-disk format is the one the launchers already wrote (`id=K1,K2` and
-- `trk_*=N`), so existing controls.cfg files load unchanged.
--
-- Screens render through the app's own show_screen(builder), so each launcher
-- keeps its accent, nav flags and view teardown. Only Detect opens a scope of
-- its own (modal overlay pattern: CLICKABLE overlay, focusables as DIRECT
-- children of the pushed box, nav.pop() BEFORE overlay:delete()).

local lvgl = require("lvgl")
local nav  = require("lib/nav")

local M = {}

-- Quit is a FIRMWARE code, not a module code: elf_host swallows it on both
-- edges and ends the module through host_should_exit, so it never reaches the
-- guest and cannot collide with a module's own keycodes. It is only live while
-- the binding layer is on, which is why a bound quit key still types normally
-- at a Dos prompt.
M.QUIT = 0xFF

-- Screenshot is the same kind of code: a FIRMWARE out (0xFD) elf_host swallows
-- on both edges, saving a PNG of the module's frame instead of passing it on.
-- Shipped UNBOUND. The keymap is a pure remapper, so any default binding takes
-- that key away from every module — worth it for an exit, not for an occasional
-- capture. It shows as a "---" row in Controls for the user to bind.
M.SHOT = 0xFD

-- Every physical T-Deck key that resolves to a character, plus the trackball
-- pseudo-codes elf_host emits. Order here is the order the picker shows.
local KEY_ORDER = {
    { "a", 0x61 }, { "b", 0x62 }, { "c", 0x63 }, { "d", 0x64 }, { "e", 0x65 },
    { "f", 0x66 }, { "g", 0x67 }, { "h", 0x68 }, { "i", 0x69 }, { "j", 0x6A },
    { "k", 0x6B }, { "l", 0x6C }, { "m", 0x6D }, { "n", 0x6E }, { "o", 0x6F },
    { "p", 0x70 }, { "q", 0x71 }, { "r", 0x72 }, { "s", 0x73 }, { "t", 0x74 },
    { "u", 0x75 }, { "v", 0x76 }, { "w", 0x77 }, { "x", 0x78 }, { "y", 0x79 },
    { "z", 0x7A },
    { "$",      0x24 },
    { "Space",  0x20 },
    { "Enter",  0x0D },
    { "BkSpc",  0x08 },
    { "Shift",  0x80 },
    { "TrkUp",  0x81 },
    { "TrkDn",  0x82 },
    { "TrkLt",  0x83 },
    { "TrkRt",  0x84 },
    { "TrkClk", 0x85 },
}

M.KEYS = {}
M.KEY_NAMES = {}
for _, k in ipairs(KEY_ORDER) do
    M.KEYS[k[1]] = k[2]
    M.KEY_NAMES[k[2]] = k[1]
end

-- Codes the Detect poll can see. _kb_just_pressed only answers for codes below
-- 0x80, so Shift and the trackball are list-only; Enter is excluded because
-- LVGL uses it to activate the focused button (it cancels Detect instead).
local DETECT_CODES = {}
for _, k in ipairs(KEY_ORDER) do
    if k[2] < 0x80 and k[2] ~= 0x0D then DETECT_CODES[#DETECT_CODES + 1] = k[2] end
end

-- 10ms, not the 33ms a game's input timer uses: keyboard_read_cb snapshots
-- kb_key_prev once per indev cycle (LV_DEF_REFR_PERIOD = 16ms), so a
-- just-pressed edge is only visible for ~16ms. Polling slower than that drops
-- keypresses; a capture that ignores the key you pressed reads as broken.
local DETECT_PERIOD_MS = 10
local DETECT_TIMEOUT_MS = 10000

local function key_display(code)
    if not code then return "---" end
    return M.KEY_NAMES[code] or string.format("0x%02X", code)
end
M.key_display = key_display

-- Accept a key by name ("w", "TrkUp") or by raw code.
local function key_code(k)
    if type(k) == "number" then return k end
    if type(k) == "string" then return M.KEYS[k] end
    return nil
end

-- A label + value button pair; the value button cycles and persists. Both are
-- direct children of the scope so the trackball can land on each button.
-- M.rows{ font=, on_save= } returns a setting_row(parent, label, get, on) bound
-- to one app, so call sites stay short.
function M.rows(o)
    o = o or {}
    return function(parent, label, get_text, on_click)
        parent:Label{
            text = label,
            text_font = o.font,
            text_color = "#CCCCCC",
            w = lvgl.PCT(100), h = lvgl.SIZE_CONTENT,
        }
        local valBtn = parent:Button{ w = lvgl.PCT(100), h = 28 }
        local valLbl = valBtn:Label{
            text = get_text(),
            text_font = o.font,
            align = lvgl.ALIGN.CENTER,
        }
        valBtn:onClicked(function()
            on_click()
            valLbl:set{ text = get_text() }
            if o.on_save then o.on_save() end
        end)
        return valBtn
    end
end

-- ── the binding object ──────────────────────────────────────────────────────

local Kb = {}
Kb.__index = Kb

-- opts: actions, root, show_screen, font, accent, title, on_back, on_save,
--       trackball = { momentum, impulse, friction, thresh }, input_note,
--       quit = false to suppress the standard Quit action,
--       shot = false to suppress the standard Screenshot action.
function M.new(opts)
    local self = setmetatable({}, Kb)
    self.root        = opts.root
    self.show_screen = opts.show_screen
    self.font        = opts.font
    self.accent      = opts.accent or "#FFFFFF"
    self.title       = opts.title or "CONTROLS"
    self.note        = opts.note          -- blurb under the Controls heading
    self.on_back     = opts.on_back
    self.on_save     = opts.on_save
    self.input_note  = opts.input_note

    self.actions = {}
    for _, a in ipairs(opts.actions or {}) do
        self.actions[#self.actions + 1] = {
            id = a.id, label = a.label, out = a.out,
            def1 = key_code(a.key1), def2 = key_code(a.key2),
        }
    end
    -- Appended here rather than by each launcher, so every module gains the
    -- same exit and none can drift out of having one. 'y' is deliberately far
    -- from WASD.
    if opts.quit ~= false then
        self.actions[#self.actions + 1] = {
            id = "quit", label = "Quit", out = M.QUIT, def1 = M.KEYS.y,
        }
    end
    -- Same reasoning as quit: appended here so every module gains it and none
    -- can drift out of having one. Unbound until the user picks a key.
    if opts.shot ~= false then
        self.actions[#self.actions + 1] = {
            id = "shot", label = "Screenshot", out = M.SHOT,
        }
    end

    local t = opts.trackball
    if t then
        self.trk_def = {
            momentum = t.momentum ~= false,
            impulse  = t.impulse  or 15,
            friction = t.friction or 82,
            thresh   = t.thresh   or 4,
        }
    end

    self.row = M.rows{ font = self.font, on_save = function() self:save() end }
    self:reset_defaults()
    return self
end

function Kb:reset_defaults()
    self.bind = {}
    for _, a in ipairs(self.actions) do
        self.bind[a.id] = { key1 = a.def1, key2 = a.def2 }
    end
    if self.trk_def then
        self.trk = {
            momentum = self.trk_def.momentum,
            impulse  = self.trk_def.impulse,
            friction = self.trk_def.friction,
            thresh   = self.trk_def.thresh,
        }
    end
end

function Kb:save()
    if self.on_save then self.on_save() end
end

-- Live binding for one action id: { key1, key2 } or nil.
function Kb:get(id) return self.bind[id] end

-- ── launch arguments ────────────────────────────────────────────────────────

-- "OUT=IN[+ALT_IN],..." — the firmware keymap is a pure remapper (unmapped
-- codes pass through unchanged), so only real bindings are emitted. nil when
-- nothing is bound: the caller then omits -keymap entirely.
function Kb:keymap_string()
    local parts = {}
    for _, a in ipairs(self.actions) do
        local b = self.bind[a.id]
        if b and (b.key1 or b.key2) then
            local s = string.format("%02X=", a.out)
            if b.key1 then
                s = s .. string.format("%02X", b.key1)
                if b.key2 then s = s .. string.format("+%02X", b.key2) end
            else
                s = s .. string.format("%02X", b.key2)
            end
            parts[#parts + 1] = s
        end
    end
    if #parts == 0 then return nil end
    return table.concat(parts, ",")
end

-- "enabled,impulse*10,friction*100,threshold*10" for -trkball.
function Kb:trkball_string()
    local t = self.trk
    if not t then return nil end
    return string.format("%d,%d,%d,%d",
        t.momentum and 1 or 0, t.impulse, t.friction, t.thresh)
end

-- Touch controller layouts moved to lib/padlayout (per-launcher presets +
-- user editing) — this lib stays physical-keys only.

-- ── config persistence (the app owns the file, we own these lines) ──────────

function Kb:save_lines(f)
    for _, a in ipairs(self.actions) do
        local b = self.bind[a.id]
        local k1 = b.key1 and string.format("%02X", b.key1) or "--"
        local k2 = b.key2 and string.format("%02X", b.key2) or "--"
        f:write(a.id .. "=" .. k1 .. "," .. k2 .. "\n")
    end
    if self.trk then
        f:write(string.format("trk_momentum=%d\n", self.trk.momentum and 1 or 0))
        f:write(string.format("trk_impulse=%d\n", self.trk.impulse))
        f:write(string.format("trk_friction=%d\n", self.trk.friction))
        f:write(string.format("trk_thresh=%d\n", self.trk.thresh))
    end
end

-- Returns true when the line belonged to us, so the caller's own parser can
-- skip it. An unknown action id falls through as not-ours.
function Kb:load_line(line)
    local id, k1s, k2s = line:match("^([%w_]+)=(%S+),(%S+)$")
    if id and self.bind[id] then
        self.bind[id].key1 = (k1s ~= "--") and tonumber(k1s, 16) or nil
        self.bind[id].key2 = (k2s ~= "--") and tonumber(k2s, 16) or nil
        return true
    end
    if self.trk then
        local m = line:match("^trk_momentum=([01])$")
        if m then self.trk.momentum = (m == "1"); return true end
        local k, n = line:match("^(trk_%a+)=(%d+)$")
        if k == "trk_impulse"  then self.trk.impulse  = tonumber(n); return true end
        if k == "trk_friction" then self.trk.friction = tonumber(n); return true end
        if k == "trk_thresh"   then self.trk.thresh   = tonumber(n); return true end
    end
    return false
end

-- ── screens ─────────────────────────────────────────────────────────────────

function Kb:heading(parent, text)
    return parent:Label{
        text = text,
        text_font = self.font,
        text_color = self.accent,
        w = lvgl.PCT(100), h = lvgl.SIZE_CONTENT,
    }
end

function Kb:button(parent, text, width, height, on_click)
    local b = parent:Button{ w = lvgl.PCT(width), h = height }
    b:Label{ text = text, text_font = self.font, align = lvgl.ALIGN.CENTER }
    b:onClicked(on_click)
    return b
end

-- Controls overview: one label + two key buttons per action.
function Kb:open()
    self.show_screen(function(c)
        self:heading(c, self.title)

        if self.note then
            c:Label{
                text = self.note,
                text_font = self.font,
                text_color = "#CCCCCC",
                w = lvgl.PCT(100), h = lvgl.SIZE_CONTENT,
            }
        end

        for idx, a in ipairs(self.actions) do
            local b = self.bind[a.id]
            c:Label{
                text = a.label,
                text_font = self.font,
                text_color = "#CCCCCC",
                w = lvgl.PCT(100), h = lvgl.SIZE_CONTENT,
            }
            self:button(c, key_display(b.key1), 48, 26,
                        function() self:pick(idx, 1) end)
            self:button(c, key_display(b.key2), 48, 26,
                        function() self:pick(idx, 2) end)
        end

        self:button(c, "Defaults", 48, 28, function()
            self:reset_defaults()
            self:save()
            self:open()
        end)

        if self.trk then
            self:button(c, "Input", 48, 28, function() self:input_screen() end)
        end

        self:button(c, "Back", 48, 28, function()
            if self.on_back then self.on_back() end
        end)
    end)
end

-- Key picker for one action slot: Detect, clear, then the whole key table.
function Kb:pick(action_idx, slot)
    local a = self.actions[action_idx]
    local b = self.bind[a.id]

    local function assign(code)
        -- The firmware keymap holds ONE output per physical key (keymap_table
        -- is indexed by the input code), so a key left in two slots silently
        -- loses one of them. Strip it everywhere else first.
        if code then
            for _, o in ipairs(self.actions) do
                local ob = self.bind[o.id]
                if ob.key1 == code then ob.key1 = nil end
                if ob.key2 == code then ob.key2 = nil end
            end
        end
        if slot == 1 then b.key1 = code else b.key2 = code end
        self:save()
        self:open()
    end

    self.show_screen(function(c)
        self:heading(c, a.label .. " - " .. ((slot == 1) and "Primary" or "Alt"))

        self:button(c, "Detect key", 100, 26, function() self:detect(assign) end)

        self:button(c, "--- (clear)", 100, 24, function() assign(nil) end)

        local current = (slot == 1) and b.key1 or b.key2
        for _, k in ipairs(KEY_ORDER) do
            local label = k[1]
            if k[2] == current then label = "> " .. label .. " <" end
            self:button(c, label, 48, 24, function() assign(k[2]) end)
        end

        self:button(c, "Cancel", 100, 26, function() self:open() end)
    end)
end

-- Modal "press a key" capture. Polls the physical matrix rather than LVGL key
-- events, so what gets bound is the key the user actually pressed and not the
-- navigation key LVGL translated it into. Enter and the trackball are LVGL's
-- activation inputs — they land on Cancel, so both stay list-only.
function Kb:detect(assign)
    local W, H = lvgl.HOR_RES(), lvgl.VER_RES()

    local ovargs = {
        w = W, h = H, x = 0, y = 0,
        bg_color = "#000000", bg_opa = 200, border_width = 0, pad_all = 0,
    }
    local overlay = self.root and self.root:Object(ovargs) or lvgl.Object(ovargs)
    overlay:clear_flag(lvgl.FLAG.SCROLLABLE)
    overlay:add_flag(lvgl.FLAG.CLICKABLE)   -- modal: swallow taps underneath
    if not self.root then pcall(_obj_move_foreground, overlay) end

    local box = overlay:Object{
        w = W - 40, h = lvgl.SIZE_CONTENT, align = lvgl.ALIGN.CENTER,
        bg_color = "#333333", radius = 6,
        border_width = 1, border_color = "#555555", pad_all = 8, pad_row = 6,
        flex = { flex_direction = "row", flex_wrap = "wrap",
                 justify_content = "center" },
    }
    box:clear_flag(lvgl.FLAG.SCROLLABLE)
    nav.push(box)

    local timer
    local function close()
        if timer then timer:delete(); timer = nil end
        nav.pop()
        overlay:delete()
    end

    box:Label{
        text = "Press a key to bind.\nEnter cancels.",
        text_font = self.font,
        text_color = "#CCCCCC",
        w = lvgl.PCT(100), h = lvgl.SIZE_CONTENT,
    }
    self:button(box, "Cancel", 60, 28, function()
        close()
        self:open()
    end)

    -- Bounded so an abandoned capture cannot leave a timer polling forever.
    local elapsed = 0
    -- Shift has no _kb_just_pressed (that binding rejects codes >= 0x80), so
    -- edge-detect the level here. Seeded from the CURRENT level: a shift still
    -- held from whatever opened this screen must not count as a press.
    local shift_prev = _kb_shift()
    timer = lvgl.Timer{
        period = DETECT_PERIOD_MS,
        cb = function()
            local hit = nil
            for _, code in ipairs(DETECT_CODES) do
                if _kb_just_pressed(code) then hit = code; break end
            end
            local shift_now = _kb_shift()
            if not hit and shift_now and not shift_prev then hit = M.KEYS.Shift end
            shift_prev = shift_now
            if hit then
                close()
                assign(hit)
                return
            end
            elapsed = elapsed + DETECT_PERIOD_MS
            if elapsed >= DETECT_TIMEOUT_MS then
                close()
                self:open()
            end
        end,
    }
end

-- Trackball feel. The ranges are the firmware's (elf_host parses
-- impulse/10, friction/100, threshold/10); the starting values are whatever
-- the launcher passed in, so each app keeps its own defaults.
function Kb:input_screen()
    self.show_screen(function(c)
        self:heading(c, "INPUT SETTINGS")

        local t = self.trk
        local row = self.row

        row(c, "Momentum",
            function() return t.momentum and "< ON >" or "< OFF >" end,
            function() t.momentum = not t.momentum end)

        row(c, "Sensitivity",
            function() return string.format("< %.1f >", t.impulse / 10) end,
            function()
                t.impulse = t.impulse + 1
                if t.impulse > 30 then t.impulse = 5 end
            end)

        row(c, "Friction",
            function() return string.format("< %.2f >", t.friction / 100) end,
            function()
                t.friction = t.friction + 2
                if t.friction > 95 then t.friction = 50 end
            end)

        row(c, "Dead Zone",
            function() return string.format("< %.1f >", t.thresh / 10) end,
            function()
                t.thresh = t.thresh + 1
                if t.thresh > 10 then t.thresh = 2 end
            end)

        c:Label{
            text = self.input_note or
                   "Sensitivity: impulse per tick\n"
                .. "Friction: decay rate (lower=faster stop)\n"
                .. "Dead Zone: min velocity to register",
            text_font = self.font,
            text_color = "#666666",
            w = lvgl.PCT(100), h = lvgl.SIZE_CONTENT,
        }

        self:button(c, "Reset", 48, 28, function()
            t.momentum = self.trk_def.momentum
            t.impulse  = self.trk_def.impulse
            t.friction = self.trk_def.friction
            t.thresh   = self.trk_def.thresh
            self:save()
            self:input_screen()
        end)

        self:button(c, "Back", 48, 28, function() self:open() end)
    end)
end

return M
