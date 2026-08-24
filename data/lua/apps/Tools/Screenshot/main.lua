--[[
  Tools/Screenshot — a capture button that floats over every app.

  The button is PARENTLESS, so it is a child of luavgl's full-screen root
  rather than of this app's root: app teardown deletes the root and leaves the
  button alone. Its Lua handle lives in the background record's state table
  because luavgl deletes an object whose handle is collected, and go_home runs
  a full collectgarbage.

  Taking a shot goes through _screenshot(btn), which QUEUES the capture — the
  firmware performs it from loop(), outside lv_timer_handler, and hides the
  button passed in so the trigger stays out of its own picture. The result
  arrives through _screenshot_poll.

  An ELF module launch tears the whole Lua state down, button included. Inside
  a module the triggers are the gamepad's SHOT pad and the bindable Screenshot
  key in its Controls screen.
]]

local lvgl  = require("lvgl")
local apps  = require("lib/apps")
local utils = require("lib/utils")

local W, H = lvgl.HOR_RES(), lvgl.VER_RES()
local BTN_W, BTN_H = 46, 28
local CFG = "L:/screenshot.cfg"
local SLOP_SQ = 16 * 16          -- same tap-vs-drag threshold nav.tap uses
local INDEV_POINTER = 1

-- State that outlives the UI (see the header).
local rec = apps.background_of("screenshot")
local st = (rec and rec.state) or {
    x = W - BTN_W - 4,
    y = H - BTN_H - 34,
    btn = nil,               -- the floating button, while it exists
    timer = nil,             -- keeps it on top of later apps
    last = nil,              -- last saved path
    count = 0,
}

local function clamp(v, lo, hi)
    if v < lo then return lo end
    if v > hi then return hi end
    return v
end

-- ── Position ────────────────────────────────────────────────────────────────

local function load_pos()
    local f = io.open(CFG, "r")
    if not f then return end
    local txt = f:read("*a") or ""
    f:close()
    local x = tonumber(txt:match("x=(%-?%d+)"))
    local y = tonumber(txt:match("y=(%-?%d+)"))
    if x then st.x = clamp(x, 0, W - BTN_W) end
    if y then st.y = clamp(y, 0, H - BTN_H) end
end

local function save_pos()
    local f = io.open(CFG, "w")
    if not f then return end
    f:write(string.format("x=%d\ny=%d\n", st.x, st.y))
    f:close()
end

-- ── Taking a shot ───────────────────────────────────────────────────────────

local function toast(msg)
    pcall(function()
        utils.createNotification(lvgl.disp.get_scr_act(), msg, 2500)
    end)
end

local poll_timer = nil

-- on_done (optional) runs once the result is in, so a caller showing it never
-- has to guess how long the write took. pcall'd: the app may be gone by then.
local function shoot(on_done)
    if poll_timer then return end          -- one in flight
    _screenshot(st.btn)
    poll_timer = lvgl.Timer {
        period = 100,
        cb = function(t)
            local res, err = _screenshot_poll()
            if res == nil then return end  -- still pending
            t:delete()
            poll_timer = nil
            if res == false then
                toast("Screenshot failed\n" .. tostring(err))
            else
                st.last = res
                st.count = st.count + 1
                toast("Saved " .. (res:match("[^/]+$") or res))
            end
            if on_done then pcall(on_done) end
        end,
    }
end

-- ── The floating button ─────────────────────────────────────────────────────

local function overlay_stop()
    if st.timer then
        pcall(function() st.timer:delete() end)
        st.timer = nil
    end
    if st.btn then
        pcall(function() st.btn:delete() end)
        st.btn = nil
    end
end

local function overlay_start()
    if st.btn then return end

    local b = lvgl.Object {
        x = st.x, y = st.y, w = BTN_W, h = BTN_H,
        bg_color = "#000000", bg_opa = 150,
        radius = 6, border_width = 1,
        border_color = "#DDDDDD", border_opa = 200,
        pad_all = 0,
    }
    b:clear_flag(lvgl.FLAG.SCROLLABLE)
    b:add_flag(lvgl.FLAG.CLICKABLE)
    -- A drag has to stay with this object: SCROLL_CHAIN would hand the press to
    -- a scrollable ancestor, PRESS_LOCK keeps it ours once it leaves the box.
    b:clear_flag(lvgl.FLAG.SCROLL_CHAIN)
    b:add_flag(lvgl.FLAG.PRESS_LOCK)
    b:Label { text = "SHOT", text_color = "#FFFFFF", align = lvgl.ALIGN.CENTER }

    -- Tap vs drag on one object, so the two cannot both fire. Nothing moves
    -- until the finger passes the tap threshold; after that the press is a
    -- drag and never takes a shot. These are plain handlers rather than
    -- nav.tap because luavgl allows only ONE callback per event code and the
    -- release has to choose between saving a position and capturing.
    local px, py, dragging
    b:onevent(lvgl.EVENT.PRESSED, function()
        dragging = false
        local indev = lvgl.indev.get_act()
        if indev and indev:get_type() == INDEV_POINTER then
            px, py = indev:get_point()
        else
            px, py = nil, nil
        end
    end)
    b:onevent(lvgl.EVENT.PRESSING, function()
        local indev = lvgl.indev.get_act()
        if not indev or px == nil then return end
        if not dragging then
            local x, y = indev:get_point()
            local dx, dy = x - px, y - py
            if dx * dx + dy * dy <= SLOP_SQ then return end
            dragging = true
        end
        local vx, vy = indev:get_vect()
        if vx == 0 and vy == 0 then return end
        st.x = clamp(st.x + vx, 0, W - BTN_W)
        st.y = clamp(st.y + vy, 0, H - BTN_H)
        b:set { x = st.x, y = st.y }
    end)
    b:onevent(lvgl.EVENT.RELEASED, function()
        if dragging then save_pos() else shoot() end
    end)

    pcall(_obj_move_foreground, b)
    st.btn = b

    -- Every app launched after this creates its root ABOVE the button, so it
    -- has to come back to the front. lv_obj_move_to_index returns immediately
    -- when the index is already right, so this costs nothing while idle. The
    -- timer is deliberately untracked (it must survive this app's teardown)
    -- and is owned by overlay_stop.
    st.timer = lvgl.Timer {
        period = 400,
        cb = function()
            if st.btn then pcall(_obj_move_foreground, st.btn) end
        end,
    }
end

-- ── Background contract ─────────────────────────────────────────────────────
-- Re-registered on every launch. No tick: the overlay owns its own timer, so
-- the button keeps coming to the front however the app was exited (a plain
-- go_home never starts the manager's heartbeat).
apps.register_background {
    key      = "screenshot",
    app_name = "Screenshot",
    state    = st,
    on_close = overlay_stop,
    status   = function()
        if not st.btn then return "overlay off" end
        return st.count > 0 and ("overlay on - " .. st.count .. " taken")
                             or "overlay on"
    end,
}

-- ── UI ──────────────────────────────────────────────────────────────────────

load_pos()

local root = apps.new_root {
    w = W, h = H,
    flex = { flex_direction = "column", flex_wrap = "wrap" },
    pad_all = 8,
    bg_opa = 0, border_width = 0,
}
_nav_setup(root, GRIDNAV_ROLLOVER)

root:Label {
    text = "Screenshot",
    text_color = "#FFFFFF",
    w = lvgl.PCT(100), h = lvgl.SIZE_CONTENT,
}

local info = root:Label {
    text = "",
    text_color = "#AAAAAA",
    w = lvgl.PCT(100), h = lvgl.SIZE_CONTENT,
}

local function refresh()
    local lines = {}
    lines[#lines + 1] = st.btn
        and "Overlay ON - tap SHOT anywhere, drag to move it"
        or  "Overlay off"
    lines[#lines + 1] = st.last and ("Last: " .. st.last) or "Saved to S:/screenshots (L: with no card)"
    lines[#lines + 1] = "Games: enable the SHOT pad, or bind Screenshot in Controls"
    info:set { text = table.concat(lines, "\n") }
end

local function button(label, fn)
    local b = root:Button { w = lvgl.PCT(100), h = 30 }
    local l = b:Label { text = label, align = lvgl.ALIGN.CENTER }
    b:onClicked(fn)
    return b, l
end

-- Declared before the closure that uses it: a local only comes into scope
-- after its whole statement, so assigning it in the same `local` line would
-- leave the callback reading a nil global.
local toggle_btn, toggle_lbl
toggle_btn, toggle_lbl = button("Turn overlay off", function()
    if st.btn then overlay_stop() else overlay_start() end
    toggle_lbl:set { text = st.btn and "Turn overlay off" or "Turn overlay on" }
    refresh()
end)

button("Capture this screen", function() shoot(refresh) end)

button("Run in background", function() apps.go_background("screenshot") end)

button("Quit (removes overlay)", function()
    apps.close_background("screenshot")
    apps.go_home()
end)

-- After the root exists, so the button is raised above it rather than waiting
-- for its own timer to notice. Opening the app IS the request for the button.
overlay_start()
toggle_lbl:set { text = st.btn and "Turn overlay off" or "Turn overlay on" }
refresh()

return root
