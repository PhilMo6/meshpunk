--[[
  lib/reboot_prompt.lua — shared "reboot required" modal.

  For settings whose effect lands at the next boot (identity change, storage
  swap: the protocol's data home is fixed at select_and_load). One call:

      require("lib/reboot_prompt").show("New identity active after reboot")

  [Reboot now] paints a farewell and calls _system_reboot() — which can
  REFUSE (false) while a USB drive or peer-link session owns the device;
  the prompt reopens with the refusal named. [Later] just closes.

  Modal conventions follow lib/emoji_popup: parentless overlay moved to the
  foreground, nav.push(box) for the buttons, nav.pop() BEFORE overlay:delete().
]]

local lvgl = require("lvgl")
local nav  = require("lib/nav")

local M = {}

local function open(message, refusal)
    local overlay = lvgl.Object {
        w = lvgl.HOR_RES(), h = lvgl.VER_RES(),
        x = 0, y = 0, bg_opa = 200, border_width = 0, pad_all = 0,
    }
    overlay:clear_flag(lvgl.FLAG.SCROLLABLE)
    overlay:add_flag(lvgl.FLAG.CLICKABLE)   -- modal: swallow taps behind it
    pcall(_obj_move_foreground, overlay)

    local box = overlay:Object {
        w = 280, h = 140, align = lvgl.ALIGN.CENTER,
        border_width = 1, pad_all = 10,
        flex = { flex_direction = "row", flex_wrap = "wrap" },
    }
    box:clear_flag(lvgl.FLAG.SCROLLABLE)
    nav.push(box)

    box:Label { text = "Reboot required", w = lvgl.PCT(100), h = 20 }
    box:Label { text = message or "", w = lvgl.PCT(100), h = 34 }
    if refusal then
        box:Label { text = "Reboot refused: USB or link session active",
                    w = lvgl.PCT(100), h = 16 }
    end

    local now   = box:Button { w = lvgl.PCT(48), h = 32 }
    now:Label { text = "Reboot now", align = lvgl.ALIGN.CENTER }
    local later = box:Button { w = lvgl.PCT(48), h = 32 }
    later:Label { text = "Later", align = lvgl.ALIGN.CENTER }

    local function close()
        nav.pop()
        overlay:delete()
    end
    later:onClicked(close)

    now:onClicked(function()
        close()
        -- Farewell overlay, painted before the C side reboots on the next
        -- loop() tick (the topbar power-menu pattern).
        local f = lvgl.Object {
            w = lvgl.HOR_RES(), h = lvgl.VER_RES(), x = 0, y = 0,
            bg_color = "#000000", bg_opa = 255, border_width = 0, pad_all = 0,
        }
        f:clear_flag(lvgl.FLAG.SCROLLABLE)
        f:add_flag(lvgl.FLAG.CLICKABLE)
        f:Label { text = "Restarting...", align = lvgl.ALIGN.CENTER }
        pcall(_obj_move_foreground, f)
        lvgl.Timer { period = 500, cb = function(t)
            t:delete()
            local ok, accepted = pcall(_system_reboot)
            if not ok or accepted == false then
                f:delete()
                open(message, true)
            end
        end }
    end)
end

function M.show(message)
    open(message, false)
end

return M
