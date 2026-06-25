-- Forest — a thick pine forest in depth layers. Distant trees are small, pale,
-- bluish and hazy (aerial perspective); nearer trees get larger, darker and more
-- saturated, with trunks in the foreground. A soft haze band and faint god-rays
-- sell the depth. Dark theme so light text reads over it. Re-rolled each draw,
-- so the forest is arranged differently every time the wallpaper draws.
return {
    name = "Forest",
    apply = function(t)
        t.set_palette {
            scr      = "#0c1610",   -- deep forest near-black green
            card     = "#16241a",
            text     = "#dcecdc",   -- pale mint
            grey      = "#2c4030",
            accent   = "#5aa83a",   -- leaf green (buttons / highlight)
            btn_text = "#ffffff",
            dark     = true,
        }

        -- Seed from clock + a heap address so the forest differs per draw.
        local seed = math.floor(t.now() or 0)
        local addr = tostring({}):match("0x(%x+)")
        if addr then seed = seed + (tonumber(addr, 16) or 0) end
        math.randomseed(seed)
        math.random(); math.random()

        t.background.procedural(function(c, w, h)
            -- A pine: a few overlapping triangle tiers (widest at the base),
            -- optionally over a trunk. `withTrunk` for the foreground layer.
            local function pine(cx, baseY, height, color, withTrunk)
                local topY = baseY - height
                if withTrunk then
                    local tw = math.max(2, math.floor(height * 0.05))
                    c:draw_rect({ x1 = cx - tw, y1 = baseY - math.floor(height * 0.16),
                                  x2 = cx + tw, y2 = baseY, bg_color = "#3a2616", bg_opa = 255 })
                end
                local fH = height * (withTrunk and 0.86 or 1.0)
                for i = 0, 2 do
                    local ty = topY + (i / 3) * fH * 0.62
                    local by = ty + fH / 3 + fH * 0.10
                    local hw = height * (0.14 + 0.12 * i)
                    c:draw_triangle({ p1 = { x = cx, y = ty },
                                      p2 = { x = cx - hw, y = by },
                                      p3 = { x = cx + hw, y = by },
                                      bg_color = color, bg_opa = 255 })
                end
            end

            -- Background: misty up top, deep forest floor at the bottom.
            local GB = 24
            for i = 0, GB - 1 do
                local y1 = math.floor(i * h / GB)
                local y2 = math.floor((i + 1) * h / GB) - 1
                local f  = i / (GB - 1)
                c:draw_rect({ x1 = 0, y1 = y1, x2 = w - 1, y2 = y2,
                              bg_color = t.hsv(158 - 20 * f, 0.28 + 0.24 * f, 0.36 - 0.28 * f),
                              bg_opa = 255 })
            end

            -- Far layer: small, pale, bluish-green, bases high up, densely packed.
            for _ = 1, 20 do
                local cx = math.random(-10, w + 10)
                local baseY = math.floor(h * 0.40) + math.random(-10, 10)
                pine(cx, baseY, math.random(20, 38), t.hsv(158, 0.26, 0.52), false)
            end

            -- Haze band pushes the far layer back (atmospheric depth).
            c:draw_rect({ x1 = 0, y1 = math.floor(h * 0.30), x2 = w - 1, y2 = math.floor(h * 0.50),
                          bg_color = "#c4d8cf", bg_opa = 55 })

            -- Mid layer: medium, mid-green, a bit lower.
            for _ = 1, 13 do
                local cx = math.random(-10, w + 10)
                local baseY = math.floor(h * 0.60) + math.random(-10, 10)
                pine(cx, baseY, math.random(50, 80), t.hsv(142, 0.48, 0.32), false)
            end

            -- Faint god-rays slanting down through the canopy (in front of the
            -- distance, behind the foreground trees).
            for k = 1, 3 do
                local bx = w * (0.18 + 0.30 * k) + math.random(-22, 22)
                c:draw_triangle({ p1 = { x = bx, y = 0 },
                                  p2 = { x = bx - 18, y = math.floor(h * 0.72) },
                                  p3 = { x = bx + 28, y = math.floor(h * 0.72) },
                                  bg_color = "#fff3c8", bg_opa = 20 })
            end

            -- Near layer: large, dark, saturated, bases at the bottom, with trunks.
            for _ = 1, 6 do
                local cx = math.random(-10, w + 10)
                local baseY = h + math.random(0, 8)
                pine(cx, baseY, math.random(95, 150), t.hsv(126, 0.62, 0.16), true)
            end
        end)
    end,
}
