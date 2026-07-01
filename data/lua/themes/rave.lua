-- Rave — a dark club scene: a DJ behind the decks (with speaker stacks + an LED
-- glow), a laser fan from the truss, drifting smoke haze, and a foreground crowd
-- of silhouetted ravers with raised arms waving neon objects (glowsticks, glow
-- orbs, balloons). Dark theme with a neon-magenta accent. The crowd, lasers and
-- smoke are re-rolled each time the wallpaper draws.
return {
    name = "Rave",
    apply = function(t)
        t.set_palette {
            scr      = "#08060e",   -- club black (faint purple)
            card     = "#161020",
            text     = "#efe6ff",
            grey     = "#2a2440",
            accent   = "#e8e119",   -- (buttons / highlight)
            btn_text = "#06141c",   -- near-black, readable on the bright cyan
            dark     = true,
        }

        local seed = math.floor(t.now() or 0)
        local addr = tostring({}):match("0x(%x+)")
        if addr then seed = seed + (tonumber(addr, 16) or 0) end
        math.randomseed(seed)
        math.random(); math.random()

        local NEON = { "#ff2db8", "#39ff14", "#00e5ff", "#ffe14d", "#ff7a1f", "#b14dff" }

        t.background.procedural(function(c, w, h)
            local function circle(cx, cy, r, color, opa)
                c:draw_rect({ x1 = cx - r, y1 = cy - r, x2 = cx + r, y2 = cy + r,
                              radius = r, bg_color = color, bg_opa = opa or 255 })
            end

            -- Dark club gradient (faint purple), darkest at the floor.
            local GB = 16
            for i = 0, GB - 1 do
                local y1 = math.floor(i * h / GB)
                local y2 = math.floor((i + 1) * h / GB) - 1
                local f  = i / (GB - 1)
                c:draw_rect({ x1 = 0, y1 = y1, x2 = w - 1, y2 = y2,
                              bg_color = t.hsv(272, 0.55, 0.11 - 0.08 * f), bg_opa = 255 })
            end

            local djx, djy = math.floor(w / 2), math.floor(h * 0.52)

            -- Disco ball + sparkles up top.
            circle(djx, 16, 9, "#9fb0c8")
            for k = 0, 5 do
                local a = k * math.pi / 3
                circle(djx + math.cos(a) * 5, 16 + math.sin(a) * 5, 2, "#e8f0ff")
            end
            for _ = 1, 8 do
                circle(math.random(0, w), math.random(8, math.floor(h * 0.35)), 1,
                       "#ffffff", math.random(120, 255))
            end

            -- Broad spotlight cones from the truss — they light the backdrop so the
            -- DJ/crowd silhouettes stand out.
            for k = 0, 3 do
                local bx = djx + (k - 1.5) * 46 + math.random(-10, 10)
                c:draw_triangle({ p1 = { x = djx, y = -2 },
                                  p2 = { x = bx - 30, y = math.floor(h * 0.66) },
                                  p3 = { x = bx + 30, y = math.floor(h * 0.66) },
                                  bg_color = NEON[(k % #NEON) + 1], bg_opa = 34 })
            end

            -- LED video wall behind the DJ: soft glow, black bezel, neon panel grid.
            local wx1, wy1, wx2, wy2 = 2, djy - 76, w - 2, djy + 30   -- full width, taller
            c:draw_rect({ x1 = wx1 - 12, y1 = wy1 - 8, x2 = wx2 + 12, y2 = wy2 + 18,
                          radius = 10, bg_color = "#ff2db8", bg_opa = 24 })          -- glow bleed
            c:draw_rect({ x1 = wx1 - 3, y1 = wy1 - 3, x2 = wx2 + 3, y2 = wy2 + 3,
                          radius = 4, bg_color = "#000000", bg_opa = 255 })          -- bezel
            local cols, rows = 14, 5
            local cw, ch = (wx2 - wx1) / cols, (wy2 - wy1) / rows
            for ci = 0, cols - 1 do
                for ri = 0, rows - 1 do
                    local px = wx1 + ci * cw + 1
                    local py = wy1 + ri * ch + 1
                    c:draw_rect({ x1 = px, y1 = py, x2 = px + cw - 2, y2 = py + ch - 2,
                                  bg_color = NEON[math.random(1, #NEON)], bg_opa = 170 })
                end
            end

            -- Speaker stacks either side.
            for _, sx in ipairs({ djx - 92, djx + 92 }) do
                c:draw_rect({ x1 = sx - 13, y1 = djy - 40, x2 = sx + 13, y2 = djy + 30,
                              radius = 2, bg_color = "#100c18", bg_opa = 255 })
                circle(sx, djy - 22, 7, "#241c30")
                circle(sx, djy + 8, 9, "#241c30")
            end

            -- DJ booth + silhouette.
            c:draw_rect({ x1 = djx - 34, y1 = djy - 2, x2 = djx + 34, y2 = djy + 24,
                          radius = 2, bg_color = "#0c0a14", bg_opa = 255 })
            c:draw_rect({ x1 = djx - 34, y1 = djy + 22, x2 = djx + 34, y2 = djy + 24,
                          bg_color = "#ff2db8", bg_opa = 255 })          -- neon front edge
            circle(djx - 18, djy + 6, 6, "#1c1828")                      -- turntables
            circle(djx + 18, djy + 6, 6, "#1c1828")
            c:draw_rect({ x1 = djx - 12, y1 = djy - 8, x2 = djx + 12, y2 = djy + 2,
                          radius = 4, bg_color = "#06040a", bg_opa = 255 })  -- shoulders
            circle(djx, djy - 14, 8, "#06040a")                          -- head
            
            c:draw_arc({ center = { x = djx, y = djy - 14 }, radius = 10,
                             start_angle = 200, end_angle = 340, color = "#39ff14",
                             width = 2, opa = 255 })                      -- headphones
            
            c:draw_line({ p1 = { x = djx + 8, y = djy - 6 }, p2 = { x = djx + 20, y = djy - 28 },
                          color = "#06040a", width = 4, opa = 255 })      -- arm up

            -- Ceiling truss bar.
            c:draw_rect({ x1 = 0, y1 = 0, x2 = w - 1, y2 = 5, bg_color = "#0c0a14", bg_opa = 255 })
            -- Lasers fire UP from the stage floor (behind the crowd), fanning out
            -- and away over the dancefloor.
            local lsx, lsy = djx, math.floor(h * 0.72)
            for i = 0, 17 do
                local ang = math.rad(235 + (i / 17) * 70 + math.random(-3, 3))  -- upward fan
                local len = h * 1.2
                c:draw_line({ p1 = { x = lsx, y = lsy },
                              p2 = { x = lsx + math.cos(ang) * len, y = lsy + math.sin(ang) * len },
                              color = NEON[(i % #NEON) + 1], width = 2, opa = 120 })
            end

            -- Smoke / haze puffs (soft, translucent).
            for _ = 1, 10 do
                circle(math.random(0, w), math.random(math.floor(h * 0.45), h),
                       math.random(16, 34), "#cfc8e0", math.random(10, 26))
            end

            -- Foreground crowd: a dark body mass + silhouetted ravers with raised
            -- arms waving neon objects.
            -- ── Crowd: depth rows, lit by stage color so individuals show ──
            local DK = "#06040a"                                  -- shadowed body mass
            local HUES = { 250, 280, 200, 320, 170, 30, 300 }     -- varied club-light hues
            -- Hand offsets (hr units, from each shoulder) for a range of dance
            -- poses, so the crowd isn't all doing the same arm-raise.
            local POSES = {
                { { -2.0, -3.2 }, {  2.0, -3.2 } },  -- both arms up (V)
                { { -3.2, -1.8 }, {  3.2, -1.8 } },  -- both up & wide
                { { -1.4, -3.4 }, {  3.2,  0.3 } },  -- one up, one out
                { { -1.2,  2.0 }, {  1.8, -3.4 } },  -- one up, one low
                { { -3.4, -0.3 }, {  3.4, -0.3 } },  -- arms out to the sides
                { { -0.7, -1.9 }, {  0.7, -1.9 } },  -- hands up together / clap
                { { -1.6,  1.2 }, {  1.0, -3.1 } },  -- fist pump
                { { -2.6, -1.0 }, {  1.4, -2.6 } },  -- asymmetric sway
                { { -1.5, -2.9 }, {  2.4, -1.4 } },  -- loose hands up
                { { -2.0,  1.6 }, {  2.0,  1.6 } },  -- chill / arms low
                { {  0.6, -3.3 }, {  2.6, -2.2 } },  -- both up, leaning one side
            }
            local function hand_obj(hx, hy, hr, aw)
                local col = NEON[math.random(1, #NEON)]
                local kind = math.random(1, 3)
                if kind == 1 then          -- glowstick at a random tilt
                    local a = math.random() * math.pi
                    local dx, dy = math.cos(a) * hr, math.sin(a) * hr
                    c:draw_line({ p1 = { x = hx - dx, y = hy - dy }, p2 = { x = hx + dx, y = hy + dy },
                                  color = col, width = aw, opa = 255 })
                elseif kind == 2 then      -- glow orb (with halo)
                    circle(hx, hy, math.floor(hr * 1.1), col, 70)
                    circle(hx, hy, math.max(2, math.floor(hr * 0.6)), col, 255)
                else                       -- sparkler burst
                    for s = 1, 5 do
                        local a = math.random() * 2 * math.pi
                        local r = hr * (0.9 + 0.7 * math.random())
                        c:draw_line({ p1 = { x = hx, y = hy },
                                      p2 = { x = hx + math.cos(a) * r, y = hy + math.sin(a) * r },
                                      color = col, width = 1, opa = 255 })
                    end
                    circle(hx, hy, math.max(1, math.floor(hr * 0.4)), "#ffffff", 255)
                end
            end
            local function raver(cx, headY, hr, bodyCol)
                local lean = (math.random() - 0.5) * hr               -- head bob / lean
                circle(cx + lean, headY, hr, bodyCol)
                c:draw_rect({ x1 = cx - hr - 2, y1 = headY + hr - 1, x2 = cx + hr + 2, y2 = headY + hr * 4,
                              radius = hr, bg_color = bodyCol, bg_opa = 255 })
                local lsx, lsy = cx - hr * 0.7, headY + hr * 1.1      -- shoulders
                local rsx, rsy = cx + hr * 0.7, headY + hr * 1.1
                local p = POSES[math.random(1, #POSES)]
                local function jit() return (math.random() - 0.5) * hr * 0.7 end
                local lhx, lhy = lsx + p[1][1] * hr + jit(), lsy + p[1][2] * hr + jit()
                local rhx, rhy = rsx + p[2][1] * hr + jit(), rsy + p[2][2] * hr + jit()
                local aw = math.max(2, math.floor(hr * 0.55))
                c:draw_line({ p1 = { x = lsx, y = lsy }, p2 = { x = lhx, y = lhy }, color = bodyCol, width = aw, opa = 255 })
                c:draw_line({ p1 = { x = rsx, y = rsy }, p2 = { x = rhx, y = rhy }, color = bodyCol, width = aw, opa = 255 })
                -- A fun neon object in the higher hand (sometimes both).
                local hi = (lhy <= rhy) and { lhx, lhy } or { rhx, rhy }
                local lo = (lhy <= rhy) and { rhx, rhy } or { lhx, lhy }
                hand_obj(hi[1], hi[2], hr, aw)
                if math.random() < 0.3 then hand_obj(lo[1], lo[2], hr, aw) end
            end
            -- val = how brightly this row is lit (front rows brighter -> depth).
            local function crowd_row(headY, hr, count, jitterY, val)
                for i = 0, count - 1 do
                    local cx = 12 + i * ((w - 24) / (count - 1)) + math.random(-10, 10)
                    local bodyCol = t.hsv(HUES[math.random(1, #HUES)], 0.42, val)
                    raver(cx, headY - math.random(0, jitterY), hr, bodyCol)
                end
            end

            -- Backlight glow behind the crowd so the silhouettes pop.
            c:draw_rect({ x1 = 0, y1 = math.floor(h * 0.60), x2 = w - 1, y2 = math.floor(h * 0.69),
                          bg_color = "#ff2db8", bg_opa = 34 })
            c:draw_rect({ x1 = 0, y1 = math.floor(h * 0.64), x2 = w - 1, y2 = math.floor(h * 0.71),
                          bg_color = "#00e5ff", bg_opa = 36 })

            crowd_row(math.floor(h * 0.66), 4, 11, 6, 0.34)   -- distant row (dimmer)
            c:draw_rect({ x1 = 0, y1 = math.floor(h * 0.70), x2 = w - 1, y2 = h - 1,
                          bg_color = DK, bg_opa = 255 })        -- packed body mass (shadow)
            crowd_row(math.floor(h * 0.75), 5, 9, 8, 0.48)    -- mid row
            crowd_row(math.floor(h * 0.83), 7, 6, 10, 0.60)   -- front row (brightest, closest)
        end)
    end,
}
