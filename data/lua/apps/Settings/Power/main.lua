local lvgl   = require("lvgl")
local apps   = require("lib/apps")
local nav    = require("lib/nav")
local theme  = require("lib/theme")

local root = apps.new_root()
root:set { w = lvgl.HOR_RES(), h = lvgl.VER_RES(), pad_all = 0, border_width = 0, bg_opa = 0 }
root:clear_flag(lvgl.FLAG.SCROLLABLE)

-- Themed wallpaper behind this (lightweight) screen; containers below are transparent.
theme.show_background()

local content = root:Object {
    flex = { flex_direction = "row", flex_wrap = "wrap" },
    w = lvgl.HOR_RES(), h = lvgl.VER_RES(),
    border_width = 0, pad_all = 6, bg_opa = 0,
}
nav.replace(content, { flags = nav.ROLLOVER + nav.SCROLL_FIRST })

-- Title
content:Label { text = "Power", w = lvgl.PCT(70), h = 26 }
local back_btn = content:Button { w = 50, h = 22 }
back_btn:Label { text = "Home", align = lvgl.ALIGN.CENTER }

local status = content:Label { text = "", w = lvgl.PCT(100), h = 16 }

-- ── Actions ─────────────────────────────────────────────────────────────────
-- Same semantics as the topbar battery drop-down: Standby is immediate,
-- Power off / Restart are two-tap arm/confirm. The C bindings defer the real
-- action to the top of loop(), so the farewell below stays painted.
-- Standby/Power off rows are guarded so the page still loads on firmware
-- without the bindings.
content:Label { text = "-- Actions --", w = lvgl.PCT(100), h = 16 }

local function farewell(text, fn)
    local f = lvgl.Object {
        w = 320, h = 240, x = 0, y = 0,
        bg_color = "#000000", bg_opa = 255, border_width = 0, pad_all = 0,
    }
    f:clear_flag(lvgl.FLAG.SCROLLABLE)
    f:add_flag(lvgl.FLAG.CLICKABLE)   -- swallow taps on the way down
    f:Label { text = text, align = lvgl.ALIGN.CENTER }
    pcall(_obj_move_foreground, f)
    lvgl.Timer { period = 500, cb = function(t)
        t:delete()
        local ok, accepted = pcall(fn)
        if not ok or accepted == false then
            f:delete()
            status.text = "Unavailable: USB or link session active"
        end
    end }
end

local function wake_hint()
    local ok, caps = pcall(_input_caps)
    if ok and type(caps) == "table" and caps.trackball then
        return "Click trackball to wake"
    end
    return "Press USER to wake"
end

local off_lbl, restart_lbl
local armed = nil   -- "off" | "restart": tapped once, awaiting the confirm tap

if _system_standby then
    local standby_btn = content:Button { w = lvgl.PCT(100), h = 30 }
    standby_btn:Label { text = "Standby - wake on alerts", align = lvgl.ALIGN.CENTER }
    standby_btn:onClicked(function()
        local ok, accepted = pcall(_system_standby)
        if not ok or accepted == false then
            status.text = "Unavailable: USB or link session active"
        end
    end)
end

if _system_poweroff then
    local off_btn = content:Button { w = lvgl.PCT(100), h = 30 }
    off_lbl = off_btn:Label { text = "Power off", align = lvgl.ALIGN.CENTER }
    off_btn:onClicked(function()
        if armed ~= "off" then
            armed = "off"
            off_lbl:set({ text = "Tap again to power off" })
            if restart_lbl then restart_lbl:set({ text = "Restart" }) end
            status.text = "Tap Power off again to confirm"
            return
        end
        farewell("Powering off...\n" .. wake_hint(), _system_poweroff)
    end)
end

local restart_btn = content:Button { w = lvgl.PCT(100), h = 30 }
restart_lbl = restart_btn:Label { text = "Restart", align = lvgl.ALIGN.CENTER }
restart_btn:onClicked(function()
    if armed ~= "restart" then
        armed = "restart"
        restart_lbl:set({ text = "Tap again to restart" })
        if off_lbl then off_lbl:set({ text = "Power off" }) end
        status.text = "Tap Restart again to confirm"
        return
    end
    farewell("Restarting...", _system_reboot)
end)

-- ── Auto standby ────────────────────────────────────────────────────────────
-- Enter standby automatically after the configured idle time (same activity
-- clock as the screen timeout). Guarded for firmware without the bindings.
content:Label { text = "-- Auto standby --", w = lvgl.PCT(100), h = 16 }

if _auto_standby_get then
    local as_on = _auto_standby_get()
    local function as_text()
        return (as_on and "[x]" or "[ ]") .. " Auto standby when idle"
    end
    local as_btn = content:Button { w = lvgl.PCT(100), h = 30 }
    local as_lbl = as_btn:Label { text = as_text(), align = lvgl.ALIGN.LEFT_MID }

    as_btn:onClicked(function()
        as_on = not as_on
        _auto_standby_set(as_on)
        as_lbl:set({ text = as_text() })
        status.text = as_on and "Auto standby on" or "Auto standby off"
    end)
end

if _auto_standby_mins_get then
    local as_mins = _auto_standby_mins_get()
    local function as_mins_text()
        return "Auto standby after: " .. as_mins .. " min"
    end
    local asm_btn = content:Button { w = lvgl.PCT(100), h = 30 }
    local asm_lbl = asm_btn:Label { text = as_mins_text(), align = lvgl.ALIGN.LEFT_MID }

    asm_btn:onClicked(function()
        if as_mins == 5 then
            as_mins = 10
        elseif as_mins == 10 then
            as_mins = 15
        elseif as_mins == 15 then
            as_mins = 30
        elseif as_mins == 30 then
            as_mins = 60
        else
            as_mins = 5
        end
        _auto_standby_mins_set(as_mins)
        asm_lbl:set({ text = as_mins_text() })
        status.text = "Auto standby after " .. as_mins .. " minutes"
    end)
end

-- ── Standby heartbeat ───────────────────────────────────────────────────────
-- Periodic keyboard-backlight glow while in standby, showing the dark device
-- is alive. Rides standby's own wake windows — it never wakes the chip.
-- Gated on the keyboard backlight capability: boards without one (Heltec)
-- have no heartbeat channel, so show that instead of dead toggles.
content:Label { text = "-- Standby heartbeat --", w = lvgl.PCT(100), h = 16 }

local hb_caps_ok, hb_caps = pcall(_input_caps)
local has_heartbeat = hb_caps_ok and type(hb_caps) == "table" and hb_caps.kbd_backlight

if not has_heartbeat then
    content:Label {
        text = "Not available: this device has no keyboard backlight",
        w = lvgl.PCT(100), h = 16,
    }
end

if has_heartbeat and _standby_heartbeat_get then
    local hb_on = _standby_heartbeat_get()
    local function hb_text()
        return (hb_on and "[x]" or "[ ]") .. " Standby heartbeat (kbd glow)"
    end
    local hb_btn = content:Button { w = lvgl.PCT(100), h = 30 }
    local hb_lbl = hb_btn:Label { text = hb_text(), align = lvgl.ALIGN.LEFT_MID }

    hb_btn:onClicked(function()
        hb_on = not hb_on
        _standby_heartbeat_set(hb_on)
        hb_lbl:set({ text = hb_text() })
        status.text = hb_on and "Standby: periodic keyboard glow on"
                            or "Standby: keyboard stays dark"
    end)
end

-- Heartbeat pacing: minimum seconds between standby glows (15/30/60 cycle).
if has_heartbeat and _standby_heartbeat_secs_get then
    local hb_secs = _standby_heartbeat_secs_get()
    local function hb_secs_text()
        return "Heartbeat interval: " .. hb_secs .. "s"
    end
    local hbs_btn = content:Button { w = lvgl.PCT(100), h = 30 }
    local hbs_lbl = hbs_btn:Label { text = hb_secs_text(), align = lvgl.ALIGN.LEFT_MID }

    hbs_btn:onClicked(function()
        if hb_secs == 15 then
            hb_secs = 30
        elseif hb_secs == 30 then
            hb_secs = 60
        else
            hb_secs = 15
        end
        _standby_heartbeat_secs_set(hb_secs)
        hbs_lbl:set({ text = hb_secs_text() })
        status.text = "Standby heartbeat every " .. hb_secs .. "s"
    end)
end

-- ── Back ────────────────────────────────────────────────────────────────────
back_btn:onClicked(function()
    apps.go_home()
end)

return root
