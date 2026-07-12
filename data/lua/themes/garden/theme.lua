-- Garden — a sunny garden scene drawn procedurally: gradient sky, sun + rays,
-- clouds, a white picket fence, grass tufts, butterflies, and a row of flowers
-- (stem, leaves, petaled bloom). A light theme so dark text reads over it; the
-- busy planting sits low while the sky up top stays clean for UI text. The
-- flowers/grass are re-rolled each time the wallpaper draws.
return {
    name = "Garden",
    apply = function(t)
        t.set_palette {
            scr      = "#bfe3f2",   -- soft sky
            card     = "#ffffff",
            text     = "#2a3a1c",   -- dark leaf green
            grey     = "#9fc49a",
            accent   = "#8fd35e",   -- light leaf green (buttons / highlight)
            btn_text = "#2a3a1c",   -- dark leaf green (matches body text)
            dark     = false,
        }

        -- Seed from clock + a heap address so the garden differs per draw.
        local seed = math.floor(t.now() or 0)
        local addr = tostring({}):match("0x(%x+)")
        if addr then seed = seed + (tonumber(addr, 16) or 0) end
        math.randomseed(seed)
        math.random(); math.random()

        t.background.procedural(function(c, w, h)
            local function circle(cx, cy, r, color, opa)
                c:draw_rect({ x1 = cx - r, y1 = cy - r, x2 = cx + r, y2 = cy + r,
                              radius = r, bg_color = color, bg_opa = opa or 255 })
            end

            local groundY = math.floor(h * 0.62)

            -- Sky: blue up top fading paler toward the horizon.
            local SKY = 22
            for i = 0, SKY - 1 do
                local y1 = math.floor(i * groundY / SKY)
                local y2 = math.floor((i + 1) * groundY / SKY) - 1
                local f  = i / (SKY - 1)
                c:draw_rect({ x1 = 0, y1 = y1, x2 = w - 1, y2 = y2,
                              bg_color = t.hsv(205, 0.45 - 0.32 * f, 0.97), bg_opa = 255 })
            end

            -- Sun + rays (top-right), with a soft glow.
            local sx, sy = w - 46, 40
            circle(sx, sy, 30, "#fff2a0", 90)
            for k = 0, 7 do
                local a = k * math.pi / 4
                c:draw_line({ p1 = { x = sx + math.cos(a) * 26, y = sy + math.sin(a) * 26 },
                              p2 = { x = sx + math.cos(a) * 36, y = sy + math.sin(a) * 36 },
                              color = "#ffd83a", width = 3, opa = 220 })
            end
            circle(sx, sy, 20, "#ffd83a", 255)

            -- Clouds.
            local function cloud(x, y, s)
                circle(x, y, s, "#ffffff"); circle(x + s, y + 2, s * 0.8, "#ffffff")
                circle(x - s, y + 2, s * 0.8, "#ffffff"); circle(x, y + s * 0.5, s * 1.1, "#ffffff")
            end
            cloud(72, 42, 13); cloud(176, 26, 10)

            -- Butterflies fluttering in the sky.
            local function butterfly(x, y, col)
                circle(x - 3, y, 3, col); circle(x + 3, y, 3, col)
                circle(x - 3, y + 4, 2, col); circle(x + 3, y + 4, 2, col)
                c:draw_line({ p1 = { x = x, y = y - 3 }, p2 = { x = x, y = y + 5 },
                              color = "#333333", width = 1, opa = 255 })
            end
            butterfly(118, 74, "#ff8c3a"); butterfly(228, 96, "#b06bff")

            -- Ground: green, darkening toward the bottom.
            local GB = 10
            for i = 0, GB - 1 do
                local y1 = groundY + math.floor(i * (h - groundY) / GB)
                local y2 = groundY + math.floor((i + 1) * (h - groundY) / GB) - 1
                local f  = i / (GB - 1)
                c:draw_rect({ x1 = 0, y1 = y1, x2 = w - 1, y2 = y2,
                              bg_color = t.hsv(110, 0.55, 0.55 - 0.18 * f), bg_opa = 255 })
            end

            -- White picket fence along the horizon (behind the flowers).
            local fy, PN = groundY - 2, 7
            c:draw_rect({ x1 = 10, y1 = fy - 14, x2 = w - 10, y2 = fy - 10,
                          bg_color = "#e8e8df", bg_opa = 255 })   -- rail
            for i = 0, PN - 1 do
                local px = 16 + i * ((w - 32) / (PN - 1))
                c:draw_rect({ x1 = px - 4, y1 = fy - 22, x2 = px + 4, y2 = fy + 6,
                              radius = 1, bg_color = "#f4f4ee", bg_opa = 255 })
                c:draw_triangle({ p1 = { x = px - 4, y = fy - 22 }, p2 = { x = px + 4, y = fy - 22 },
                                  p3 = { x = px, y = fy - 30 }, bg_color = "#f4f4ee", bg_opa = 255 })
            end

            -- Grass tufts.
            local GRASS = { "#3f9a3a", "#4fb04a", "#2f8a2f", "#5fbf52" }
            for _ = 1, 40 do
                local gx = math.random(0, w - 1)
                local gh = math.random(6, 16)
                c:draw_line({ p1 = { x = gx, y = groundY + 4 },
                              p2 = { x = gx + math.random(-3, 3), y = groundY + 4 - gh },
                              color = GRASS[math.random(1, #GRASS)], width = 2, opa = 255 })
            end

            -- Flowers: stem, two leaves, a petaled bloom with a yellow center.
            local PETAL = { "#e23b6b", "#ffd23a", "#ff7fae", "#b06bff", "#ff8c3a", "#ffffff", "#e2493b" }
            local FN = 6
            for i = 0, FN - 1 do
                local fx = 26 + i * ((w - 52) / (FN - 1)) + math.random(-8, 8)
                local bloomY = groundY - math.random(34, 70)
                local stemTop = bloomY + 6
                c:draw_line({ p1 = { x = fx, y = groundY + 2 }, p2 = { x = fx, y = stemTop },
                              color = "#2f8a2f", width = 3, opa = 255 })
                local ly = (groundY + stemTop) / 2
                c:draw_triangle({ p1 = { x = fx, y = ly }, p2 = { x = fx + 14, y = ly - 3 },
                                  p3 = { x = fx + 2, y = ly - 11 }, bg_color = "#3aa83a", bg_opa = 255 })
                c:draw_triangle({ p1 = { x = fx, y = ly + 8 }, p2 = { x = fx - 14, y = ly + 5 },
                                  p3 = { x = fx - 2, y = ly - 3 }, bg_color = "#3aa83a", bg_opa = 255 })
                local pc = PETAL[math.random(1, #PETAL)]
                local pr = math.random(7, 10)
                for k = 0, 5 do
                    local a = k * math.pi / 3 + math.random() * 0.15
                    circle(fx + math.cos(a) * pr, bloomY + math.sin(a) * pr, math.floor(pr * 0.7), pc)
                end
                circle(fx, bloomY, math.floor(pr * 0.6), "#ffcf33")
            end
        end)
    end,
}
