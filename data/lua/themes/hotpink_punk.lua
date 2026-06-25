-- Hotpink Punk — cyberpunk: deep purple-black gradient, hotpink scanlines + grid.
return {
    name = "Hotpink Punk",
    apply = function(t)
        t.set_palette {
            scr      = "#0d0b12",
            card     = "#1c1326",
            text     = "#f3e9ff",
            grey     = "#3a2a4a",
            accent   = "#ff2bbf",
            btn_text = "#ffffff",
            dark     = true,
        }
        t.background.procedural(function(c, w, h)
            -- Vertical gradient (coarse bands keep it cheap): brighter at the top.
            local BANDS = 32
            for i = 0, BANDS - 1 do
                local y1 = math.floor(i * h / BANDS)
                local y2 = math.floor((i + 1) * h / BANDS) - 1
                local f = i / (BANDS - 1)
                local col = t.hsv(280, 0.5, 0.05 + 0.09 * (1 - f))
                c:draw_rect({ x1 = 0, y1 = y1, x2 = w - 1, y2 = y2, bg_color = col, bg_opa = 255 })
            end
            -- Hotpink CRT scanlines.
            for y = 0, h - 1, 3 do
                c:draw_rect({ x1 = 0, y1 = y, x2 = w - 1, y2 = y, bg_color = "#ff2bbf", bg_opa = 22 })
            end
            -- Faint vertical grid.
            for x = 0, w - 1, 24 do
                c:draw_rect({ x1 = x, y1 = 0, x2 = x, y2 = h - 1, bg_color = "#ff2bbf", bg_opa = 14 })
            end
        end)
    end,
}
