-- Doom — a hellish theme: blood-red chrome over a static "hellfire" background.
-- The glow is a vertical fire ramp (near-black at the top so text stays readable,
-- heating to orange/yellow at the bottom), with a row of random flame tongues
-- rising from the bottom edge (re-rolled each time the wallpaper draws).
return {
    name = "Doom",
    apply = function(t)
        t.set_palette {
            scr      = "#140a06",   -- charred near-black
            card     = "#2a1410",   -- dark rust panel
            text     = "#e6d2b8",   -- bone / parchment
            grey     = "#5a2a1a",   -- rusted border
            accent   = "#8a1414",   -- dark blood red (buttons / highlight)
            btn_text = "#ffe9c0",
            dark     = true,
        }

        -- Seed from the clock + a heap address so the flames differ per draw.
        local seed = math.floor(t.now() or 0)
        local addr = tostring({}):match("0x(%x+)")
        if addr then seed = seed + (tonumber(addr, 16) or 0) end
        math.randomseed(seed)
        math.random(); math.random()

        t.background.procedural(function(c, w, h)
            c:fill_bg("#140a06", 255)

            -- Hellfire glow: dark until ~40% down, then ramps red -> orange ->
            -- near-white-hot at the bottom (and desaturates as it heats).
            local BANDS = 48
            for i = 0, BANDS - 1 do
                local y1 = math.floor(i * h / BANDS)
                local y2 = math.floor((i + 1) * h / BANDS) - 1
                local f  = i / (BANDS - 1)                 -- 0 top → 1 bottom
                local g  = math.max(0, (f - 0.40) / 0.60)  -- heat, 0 until 40% down
                local v  = math.min(1, 0.04 + 0.96 * g ^ 1.4)
                local hue = 42 * g                          -- red → orange/yellow
                local sat = 1.0 - 0.45 * g
                c:draw_rect({ x1 = 0, y1 = y1, x2 = w - 1, y2 = y2,
                              bg_color = t.hsv(hue, sat, v), bg_opa = 255 })
            end

            -- Flame tongues rising from the bottom edge — random height/lean/hue.
            local NF = 16
            local fw = w / NF
            for i = 0, NF - 1 do
                local fx   = i * fw + fw * 0.5
                local fh   = (0.16 + 0.34 * math.random()) * h
                local half = fw * (0.55 + 0.35 * math.random())
                local lean = (math.random() - 0.5) * fw * 0.6
                local col  = t.hsv(18 + 32 * math.random(), 0.9, 0.78 + 0.22 * math.random())
                c:draw_triangle({
                    p1 = { x = fx - half, y = h },
                    p2 = { x = fx + half, y = h },
                    p3 = { x = fx + lean, y = h - fh },
                    bg_color = col, bg_opa = 255,
                })
            end
        end)
    end,
}
