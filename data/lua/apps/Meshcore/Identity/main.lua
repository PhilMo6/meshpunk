local lvgl  = require("lvgl")
local utils = require("lib/utils")
local apps  = require("lib/apps")
local nav   = require("lib/nav")
local theme = require("lib/theme")
local reboot_prompt = require("lib/reboot_prompt")
local clipboard = require("lib/clipboard")

-- MeshCore-only app: under another LoRa protocol, show the notice and bail.
if apps.proto_gate("meshcore") then return end

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
content:Label { text = "Identity", w = lvgl.PCT(70), h = 26 }
local back_btn = content:Button { w = 50, h = 22 }
back_btn:Label { text = "Home", align = lvgl.ALIGN.CENTER }

local status = content:Label { text = "", w = lvgl.PCT(100), h = 16 }

local ok, info = pcall(_mesh_get_node_info)
if not ok or not info then
    info = { pubkey = "???" }
end

-- Node name (identity, not radio): applies on save, announced with the
-- next advert (or the Advert button in the Messenger's My Node card).
content:Label { text = "-- Node Name --", w = lvgl.PCT(100), h = 16 }
local name_input = content:Textarea {
    password_mode = false, one_line = true,
    text = info.name or "NONAME",
    w = lvgl.PCT(100), h = 30,
}
name_input:clear_flag(lvgl.FLAG.SCROLLABLE)
local save_name = content:Button { w = lvgl.PCT(60), h = 30 }
save_name:Label { text = "Save name", align = lvgl.ALIGN.CENTER }
save_name:onClicked(function()
    local nm = name_input.text or ""
    if nm == "" then status.text = "Name required" return end
    if nm == (info.name or "") then status.text = "No name change" return end
    local okc = pcall(_mesh_set_config, "name", nm)
    if okc then
        info.name = nm
        status.text = "Name saved - out with the next advert"
    else
        status.text = "Name save failed"
    end
end)

-- Public key display (the 64-hex key wraps: size to content, never clip).
content:Label { text = "-- Public Key --", w = lvgl.PCT(100), h = 16 }
content:Label { text = info.pubkey or "???", w = lvgl.PCT(100), h = lvgl.SIZE_CONTENT }

-- Export section. The key textarea is created HERE (hidden) so it reveals in
-- place next to the button — appending it on tap landed it at the layout's
-- end, below the fold.
content:Label { text = "-- Private Key --", w = lvgl.PCT(100), h = 16 }

local btn_export = content:Button { w = lvgl.PCT(48), h = 30 }
local export_lbl = btn_export:Label { text = "Show Key", align = lvgl.ALIGN.CENTER }
-- Set: appears when the shown text differs from the current key (typed or
-- pasted) — imports it as THIS node's private key, public key re-derived.
local btn_set = content:Button { w = lvgl.PCT(48), h = 30 }
btn_set:Label { text = "Set", align = lvgl.ALIGN.CENTER }
btn_set:add_flag(lvgl.FLAG.HIDDEN)
-- Sized to content so a long key is never cut off; grows while editing.
local key_ta = content:Textarea {
    password_mode = false, one_line = false,
    text = "", w = lvgl.PCT(100), h = lvgl.SIZE_CONTENT,
}
key_ta:add_flag(lvgl.FLAG.HIDDEN)

local key_shown = false
local shown_key = ""

local function refresh_set_btn()
    local t = key_ta.text or ""
    if key_shown and t ~= "" and t ~= shown_key then
        btn_set:clear_flag(lvgl.FLAG.HIDDEN)
    else
        btn_set:add_flag(lvgl.FLAG.HIDDEN)
    end
end

btn_export:onClicked(function()
    key_shown = not key_shown
    if key_shown then
        local okk, key = pcall(_mesh_export_private_key)
        shown_key = (okk and key) or "unavailable"
        key_ta.text = shown_key
        key_ta:clear_flag(lvgl.FLAG.HIDDEN)
        export_lbl.text = "Hide Key"
    else
        key_ta.text = ""
        shown_key = ""
        key_ta:add_flag(lvgl.FLAG.HIDDEN)
        export_lbl.text = "Show Key"
    end
    refresh_set_btn()
end)

key_ta:onevent(lvgl.EVENT.VALUE_CHANGED, refresh_set_btn)

btn_set:onClicked(function()
    local t = key_ta.text or ""
    local okc, res, reason = pcall(_mesh_import_private_key, t)
    if okc and res then
        status.text = "Private key set - public key re-derived"
        reboot_prompt.show("New identity active after reboot")
    else
        status.text = "Rejected: " .. tostring((okc and reason) or res or "?")
    end
end)

-- Hold the key area for copy/paste (the Messenger input-row clipboard menu).
key_ta:onevent(lvgl.EVENT.LONG_PRESSED, function()
    local overlay = root:Object {
        w = lvgl.HOR_RES(), h = lvgl.VER_RES(), x = 0, y = 0,
        bg_color = "#000000", bg_opa = 128, border_width = 0, pad_all = 0,
    }
    overlay:clear_flag(lvgl.FLAG.SCROLLABLE)
    overlay:add_flag(lvgl.FLAG.CLICKABLE)
    local function close()
        nav.pop()
        overlay:delete()
    end
    local pbox = overlay:Object {
        w = lvgl.HOR_RES() - 40, h = lvgl.SIZE_CONTENT, align = lvgl.ALIGN.CENTER,
        bg_color = "#333333", radius = 6,
        border_width = 1, border_color = "#555555", pad_all = 8,
        flex = { flex_direction = "column", flex_wrap = "nowrap" },
    }
    nav.push(pbox)
    pbox:Label { text = "Clipboard", w = lvgl.PCT(100) }

    local copy_b = pbox:Button { w = lvgl.PCT(100), h = 28 }
    copy_b:Label { text = "Copy key", align = lvgl.ALIGN.CENTER }
    copy_b:onevent(lvgl.EVENT.RELEASED, function()
        clipboard.copy(key_ta.text or "")
        close()
    end)

    local paste_b = pbox:Button { w = lvgl.PCT(100), h = 28 }
    paste_b:Label { text = "Paste", align = lvgl.ALIGN.CENTER }
    paste_b:onevent(lvgl.EVENT.RELEASED, function()
        if clipboard.has() then
            key_ta.text = clipboard.paste()
            refresh_set_btn()
        end
        close()
    end)

    local cancel_b = pbox:Button { w = lvgl.PCT(100), h = 26 }
    cancel_b:Label { text = "Cancel", align = lvgl.ALIGN.CENTER }
    cancel_b:onevent(lvgl.EVENT.RELEASED, close)
end)

-- Generate section
content:Label { text = "-- Generate New --", w = lvgl.PCT(100), h = 16 }

local btn_gen = content:Button { w = lvgl.PCT(60), h = 30 }
btn_gen:Label { text = "New Identity", align = lvgl.ALIGN.CENTER }

btn_gen:onClicked(function()
    local overlay = root:Object {
        w = lvgl.HOR_RES(), h = lvgl.VER_RES(),
        x = 0, y = 0, bg_opa = 200, border_width = 0, pad_all = 0,
    }
    overlay:clear_flag(lvgl.FLAG.SCROLLABLE)
    overlay:add_flag(lvgl.FLAG.CLICKABLE)  -- modal
    local box = overlay:Object {
        w = 280, h = 130, align = lvgl.ALIGN.CENTER,
        border_width = 1, pad_all = 10,
        flex = { flex_direction = "row", flex_wrap = "wrap" },
    }
    box:clear_flag(lvgl.FLAG.SCROLLABLE)
    nav.push(box)

    box:Label { text = "WARNING", w = lvgl.PCT(100), h = 20 }
    box:Label { text = "Generate new identity?", w = lvgl.PCT(100), h = 18 }
    box:Label { text = "Old key is LOST forever!", w = lvgl.PCT(100), h = 18 }

    local yes = box:Button { w = lvgl.PCT(48), h = 32 }
    yes:Label { text = "Confirm", align = lvgl.ALIGN.CENTER }
    local no = box:Button { w = lvgl.PCT(48), h = 32 }
    no:Label { text = "Cancel", align = lvgl.ALIGN.CENTER }
    no:onClicked(function()
        nav.pop()
        overlay:delete()
    end)
    yes:onClicked(function()
        nav.pop()
        overlay:delete()
        local ok2, err = pcall(_mesh_generate_identity)
        if not ok2 then
            status.text = "Error: " .. tostring(err)
        else
            status.text = "New identity generated"
            reboot_prompt.show("New identity active after reboot")
        end
    end)
end)

-- Back
back_btn:onClicked(function()
    apps.go_home()
end)

return root
