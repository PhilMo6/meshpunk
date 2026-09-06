-- Settings/Ble — the BLE protocol slot and every BLE setting.
-- BLE is its own protocol slot, independent of the LoRa protocol (different
-- radios, so they run side by side): the picker below selects which BLE
-- protocol runs, switching LIVE with no reboot. A BLE protocol that proxies
-- a LoRa protocol (the MeshCore app link) is refused while that LoRa
-- protocol is not running, with the reason shown — never silently degraded.
-- The companion section below only applies while the MeshCore app link is
-- the selected protocol.
local lvgl  = require("lvgl")
local apps  = require("lib/apps")
local nav   = require("lib/nav")
local theme = require("lib/theme")

local root = apps.new_root()
root:set { w = lvgl.HOR_RES(), h = lvgl.VER_RES(), pad_all = 0, border_width = 0, bg_opa = 0 }
root:clear_flag(lvgl.FLAG.SCROLLABLE)

theme.show_background()

local content = root:Object {
    flex = { flex_direction = "row", flex_wrap = "wrap" },
    w = lvgl.HOR_RES(), h = lvgl.VER_RES(),
    border_width = 0, pad_all = 6, bg_opa = 0,
}
nav.replace(content, { flags = nav.ROLLOVER + nav.SCROLL_FIRST })

content:Label { text = "BLE", w = lvgl.PCT(70), h = 26 }
local back_btn = content:Button { w = 50, h = 22 }
back_btn:Label { text = "Home", align = lvgl.ALIGN.CENTER }

local status = content:Label { text = "", w = lvgl.PCT(100), h = 16 }

-- ── Protocol slot ───────────────────────────────────────────────────────────
content:Label { text = "-- BLE protocol --", w = lvgl.PCT(100), h = 16 }

local rows = {}   -- id -> { lbl = <label>, disp = <display text> }

local function refresh_rows()
    local req, active, running = _ble_proto_get()
    for id, row in pairs(rows) do
        local tag = ""
        if id == req then
            tag = (id == active and running) and "  [running]" or "  [selected]"
        end
        row.lbl.text = row.disp .. tag
    end
end

local function add_row(id, disp)
    local btn = content:Button { w = lvgl.PCT(100), h = 30 }
    local lbl = btn:Label { align = lvgl.ALIGN.LEFT_MID }
    rows[id] = { lbl = lbl, disp = disp }
    btn:onClicked(function()
        local ok, reason = _ble_proto_set(id)
        if ok then
            status.text = (id == "none") and "BLE off" or ("BLE: " .. id)
        else
            status.text = id .. " refused: " .. (reason or "?")
        end
        refresh_rows()
    end)
end

for _, p in ipairs(_ble_proto_list() or {}) do
    add_row(p.id, p.name .. (p.requires and ("  (needs " .. p.requires .. ")") or ""))
end
add_row("none", "none (BLE off)")
refresh_rows()

-- ── MeshCore app link (companion) ───────────────────────────────────────────
-- The companion bindings are compiled out on builds without it; the protocol
-- slot above always exists.
local companion_avail = type(_ble_is_connected) == "function"

if companion_avail then

content:Label { text = "-- MeshCore app link --", w = lvgl.PCT(100), h = 16 }

local lbl_conn = content:Label { text = "", w = lvgl.PCT(100), h = 16 }

local function refresh_conn()
    local req, active, running = _ble_proto_get()
    if req == "none" then
        lbl_conn.text = "BLE off"
    elseif req ~= "meshcore_companion" then
        lbl_conn.text = "Another BLE protocol is selected"
    elseif not running then
        lbl_conn.text = "Not running (needs the MeshCore protocol)"
    elseif _ble_is_connected() then
        lbl_conn.text = "Connected"
    else
        lbl_conn.text = "Waiting for app..."
    end
end
refresh_conn()

-- Bond clear: forces PIN re-entry on every reconnect.
content:Label { text = "Requires PIN re-entry on reconnect", w = lvgl.PCT(100), h = 16 }
local bc_on = _ble_get_bond_clear()
local btn_bc = content:Button { w = lvgl.PCT(60), h = 30 }
local lbl_bc = btn_bc:Label { align = lvgl.ALIGN.CENTER }
lbl_bc.text = bc_on and "Bond Clear: ON" or "Bond Clear: OFF"

btn_bc:onClicked(function()
    bc_on = not bc_on
    _ble_set_bond_clear(bc_on)
    lbl_bc.text = bc_on and "Bond Clear: ON" or "Bond Clear: OFF"
end)

-- Backlog sync limit: newest messages sent per chat on a fresh connection
if type(_ble_get_sync_limit) == "function" then
    content:Label { text = "Sync limit per chat (0 = all):", w = lvgl.PCT(100), h = 16 }

    local sync_limit = (function()
        local ok_g, n = pcall(_ble_get_sync_limit)
        return (ok_g and n) or 0
    end)()

    local limit_input = content:Textarea {
        one_line = true, text = tostring(sync_limit),
        accepted_chars = "0123456789", w = lvgl.PCT(40), h = 30,
    }
    limit_input:clear_flag(lvgl.FLAG.SCROLLABLE)

    local limit_save_btn = content:Button { w = lvgl.PCT(30), h = 30 }
    limit_save_btn:Label { text = "Save", align = lvgl.ALIGN.CENTER }
    limit_save_btn:onClicked(function()
        local n = tonumber(limit_input.text)
        if not n or n < 0 then
            status.text = "Limit: enter 0 or more"
            return
        end
        n = math.floor(n)
        local ok_s, applied = pcall(_ble_set_sync_limit, n)
        if ok_s then
            applied = applied or n
            limit_input.text = tostring(applied)
            status.text = (applied == 0) and "Sync: all messages"
                                          or ("Sync: newest " .. applied .. " per chat")
        else
            status.text = "Failed to save sync limit"
        end
    end)
end

apps.add_timer { period = 2000, cb = function()
    refresh_conn()
    refresh_rows()
end }

else
    apps.add_timer { period = 2000, cb = refresh_rows }
end -- companion_avail

content:Label {
    text = "BLE and LoRa are separate radios and run at the same time. The MeshCore app link carries the MeshCore mesh to the phone app, so it needs the MeshCore protocol running (Settings > Lora).",
    w = lvgl.PCT(100),
}

back_btn:onClicked(function()
    apps.go_home()
end)

return root
