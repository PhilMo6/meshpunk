-- Pride — the 6-stripe pride flag drawn as bold diagonal bands across the whole
-- screen, at a RANDOM diagonal orientation chosen each time the wallpaper is
-- drawn (so it changes whenever you return home). Chrome stays neutral-dark with
-- white text and a hot-pink highlight so the flag is the star.
--
-- The bands are perpendicular to a randomly-chosen direction (one of the four
-- diagonals ± jitter, so it always reads diagonal but never the same twice).
-- Each band is filled with two triangles (a rotated quad) — reliable for an
-- arbitrary angle, unlike an axis-aligned rect.
return {
    name = "Pride",
    apply = function(t)
        t.set_palette {
            scr      = "#0e0e12",
            card     = "#1b1b22",
            text     = "#ffffff",
            grey     = "#3a3a44",
            accent   = "#ff2d95",   -- hot-pink highlight / buttons
            btn_text = "#ffffff",
            dark     = true,
        }

        -- Seed RNG from the clock plus a heap address, so the orientation varies
        -- per draw even when the RTC isn't set / two draws share a second.
        local seed = math.floor(t.now() or 0)
        local addr = tostring({}):match("0x(%x+)")
        if addr then seed = seed + (tonumber(addr, 16) or 0) end
        math.randomseed(seed)
        math.random(); math.random()

        -- Random diagonal: one of the 4 diagonals, jittered, never axis-aligned.
        local base = ({ 45, 135, 225, 315 })[math.random(1, 4)]
        local deg  = base + math.random(-40, 40)
        local rad  = math.rad(deg)
        local dx, dy = math.cos(rad), math.sin(rad)   -- band-gradient direction

        local FLAG = { "#E40303", "#FF8C00", "#FFED00", "#008026", "#004DFF", "#750787" }

        t.background.procedural(function(c, w, h)
            c:fill_bg("#000000", 255)

            local cx, cy = w / 2, h / 2
            local px, py = -dy, dx                 -- along-stripe (perpendicular)
            local L = w + h                        -- overshoot so stripes span fully
            -- Half-extent of the screen projected onto the gradient direction:
            -- every pixel's projection lies within [-ext, ext], so N bands across
            -- that range cover the whole screen.
            local ext = (w / 2) * math.abs(dx) + (h / 2) * math.abs(dy)
            local N  = #FLAG
            local bw = (2 * ext) / N

            local function corner(proj, side)
                return {
                    x = cx + proj * dx + side * L * px,
                    y = cy + proj * dy + side * L * py,
                }
            end

            for k = 0, N - 1 do
                local lo = -ext + k * bw - 1        -- ±1 overlap kills seams
                local hi = -ext + (k + 1) * bw + 1
                local a = corner(lo,  1)
                local b = corner(lo, -1)
                local d = corner(hi, -1)
                local e = corner(hi,  1)
                local col = FLAG[k + 1]
                c:draw_triangle({ p1 = a, p2 = b, p3 = d, bg_color = col, bg_opa = 255 })
                c:draw_triangle({ p1 = a, p2 = d, p3 = e, bg_color = col, bg_opa = 255 })
            end
        end)
    end,
}
