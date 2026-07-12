-- Doom — a hellish theme: blood-red chrome over a static "hellfire" background.
-- The glow is a vertical fire ramp (near-black at the top so text stays readable,
-- heating to orange/yellow at the bottom), with a row of random flame tongues
-- rising from the bottom edge (re-rolled each time the wallpaper draws).
local lvgl = require("lvgl")

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
            local function circle(cx, cy, r, color, opa)
                c:draw_rect({ x1 = cx - r, y1 = cy - r, x2 = cx + r, y2 = cy + r,
                              radius = r, bg_color = color, bg_opa = opa or 255 })
            end

            c:fill_bg("#140a06", 255)

            -- Hellfire glow: dark until ~40% down, then ramps red -> orange ->
            -- near-white-hot at the bottom (and desaturates as it heats).
            local BANDS = 44
            for i = 0, BANDS - 1 do
                local y1 = math.floor(i * h / BANDS)
                local y2 = math.floor((i + 1) * h / BANDS) - 1
                local f  = i / (BANDS - 1)
                local g  = math.max(0, (f - 0.40) / 0.60)
                local v  = math.min(1, 0.04 + 0.96 * g ^ 1.4)
                c:draw_rect({ x1 = 0, y1 = y1, x2 = w - 1, y2 = y2,
                              bg_color = t.hsv(42 * g, 1.0 - 0.45 * g, v), bg_opa = 255 })
            end

            -- Demon skull: baked onto its OWN ARGB canvas as a solid silhouette,
            -- then composited ONCE at a single low opacity — so overlapping
            -- features can't stack alpha unevenly. Over the gradient, under the
            -- flames. Dim ember eyes are added afterwards (brighter than the faded
            -- skull) as the focal menace.
            do
                local yoff = h * 0.10   -- shift the whole skull down toward mid-screen
                local ok, sk = pcall(function()
                    return lvgl.Canvas { w = w, h = h, cf = lvgl.COLOR_FORMAT.ARGB8888 }
                end)
                if ok and sk then
                    sk:fill_bg("#000000", 0)   -- fully transparent backing
                    local cx = w * 0.5
                    local bone, dark = "#8a7360", "#140a06"
                    local function sc(x, y, r, color)
                        sk:draw_rect({ x1 = x - r, y1 = y - r + yoff, x2 = x + r, y2 = y + r + yoff,
                                       radius = math.floor(r), bg_color = color, bg_opa = 255 })
                    end
                    local function rr(x1, y1, x2, y2, rad, color)
                        sk:draw_rect({ x1 = x1, y1 = y1 + yoff, x2 = x2, y2 = y2 + yoff,
                                       radius = math.floor(rad), bg_color = color, bg_opa = 255 })
                    end
                    local function tri(x1, y1, x2, y2, x3, y3, color)
                        sk:draw_triangle({ p1 = { x = x1, y = y1 + yoff }, p2 = { x = x2, y = y2 + yoff },
                                           p3 = { x = x3, y = y3 + yoff }, bg_color = color, bg_opa = 255 })
                    end
                    local function quad(x1, y1, x2, y2, x3, y3, x4, y4, color)  -- filled quad
                        tri(x1, y1, x2, y2, x3, y3, color); tri(x1, y1, x3, y3, x4, y4, color)
                    end

                    -- Construction (front skull): a cranium SPHERE with a jaw
                    -- TRAPEZOID attached to its underside — one connected, taller-
                    -- than-wide silhouette. Cheekbones = the sphere's lower sides;
                    -- the face tapers to the chin.
                    sc(cx, h * 0.30, w * 0.15, bone)                                     -- cranium sphere
                    quad(cx - w * 0.12, h * 0.42, cx + w * 0.12, h * 0.42,
                         cx + w * 0.095, h * 0.58, cx - w * 0.095, h * 0.58, bone)       -- cheeks -> jaw sides
                    rr(cx - w * 0.095, h * 0.565, cx + w * 0.095, h * 0.65, w * 0.05, bone)  -- jaw / chin (softer corners)

                    -- Horns from the top-sides of the cranium (packed circles that
                    -- curl, capped with an angled triangle point).
                    local function horn(bx, by, dir0, sweep, len, baseR)
                        local x, y, ang = bx, by, dir0
                        local turnPerLen, traveled = sweep / len, 0
                        local lx, ly, lr = x, y, baseR
                        while traveled < len do
                            local r = baseR * (1 - (traveled / len) * 0.6)
                            sc(x, y, r, bone)
                            lx, ly, lr = x, y, r
                            local step = math.max(1.4, r * 0.7)
                            x = x + math.cos(ang) * step
                            y = y + math.sin(ang) * step
                            ang = ang + turnPerLen * step
                            traveled = traveled + step
                        end
                        local px, py = math.cos(ang + math.pi / 2), math.sin(ang + math.pi / 2)
                        local tip = baseR * 2.4
                        tri(lx + px * lr, ly + py * lr, lx - px * lr, ly - py * lr,
                            lx + math.cos(ang) * tip, ly + math.sin(ang) * tip, bone)
                    end
                    horn(cx + w * 0.10, h * 0.20, math.rad(-80),  math.rad(118),  h * 0.42, w * 0.045)
                    horn(cx - w * 0.10, h * 0.20, math.rad(-100), math.rad(-118), h * 0.42, w * 0.045)

                    -- Big tilted "aviator" eye sockets — outer-top high, inner-bottom
                    -- low: the defining skull feature and a demon glare.
                    quad(cx + w * 0.028, h * 0.375, cx + w * 0.125, h * 0.36,
                         cx + w * 0.10, h * 0.43, cx + w * 0.038, h * 0.445, dark)       -- R socket
                    quad(cx - w * 0.028, h * 0.375, cx - w * 0.125, h * 0.36,
                         cx - w * 0.10, h * 0.43, cx - w * 0.038, h * 0.445, dark)       -- L socket

                    -- Under-cheekbone shadows (small, subtle).
                    tri(cx - w * 0.11, h * 0.45, cx - w * 0.075, h * 0.46, cx - w * 0.095, h * 0.49, dark)
                    tri(cx + w * 0.11, h * 0.45, cx + w * 0.075, h * 0.46, cx + w * 0.095, h * 0.49, dark)

                    -- Nasal cavity: inverted triangle on the midline.
                    tri(cx - w * 0.028, h * 0.45, cx + w * 0.028, h * 0.45, cx, h * 0.505, dark)

                    -- Mouth: dark gap with sharp fangs attached to the bone above and
                    -- below, interlocking.
                    local mouthTop, mouthBot = h * 0.545, h * 0.615    -- dropped below the nose
                    rr(cx - w * 0.078, mouthTop, cx + w * 0.078, mouthBot, w * 0.015, dark)
                    local mL, mR = cx - w * 0.072, cx + w * 0.072
                    local nT = 6
                    local step = (mR - mL) / nT
                    local mid = (nT - 1) / 2
                    for i = 0, nT - 1 do
                        local d = math.abs(i - mid) / mid             -- 0 centre .. 1 back
                        local sizef = 1 - d * 0.55                     -- teeth shrink toward the back (depth)
                        local hw = step * (0.42 + 0.14 * sizef)
                        local front = math.abs(i - mid) < 1            -- the two centre fangs
                        -- upper tooth (front two are the big fangs, jittered for unevenness)
                        local cxu = mL + (i + 0.5) * step + (math.random() - 0.5) * 2
                        local uh  = ((front and 0.075 or 0.028 + 0.038 * sizef) + 0.012 * math.random()) * h
                        -- root the base UP into the bone above the gap so the tooth stays fused to the jaw
                        tri(cxu - hw, mouthTop - h * 0.013, cxu + hw, mouthTop - h * 0.013,
                            cxu + (math.random() - 0.5) * hw * 0.4, mouthTop + uh, bone)
                        -- lower tooth (offset half a step, smaller, also shrinking back)
                        if i < nT - 1 then
                            local sl  = 1 - (math.abs(i + 0.5 - mid) / mid) * 0.55
                            local cxl = mL + (i + 1) * step + (math.random() - 0.5) * 2
                            local lh  = (0.024 + 0.032 * sl + 0.01 * math.random()) * h
                            -- root the base DOWN into the chin bone below the gap
                            tri(cxl - hw * 0.85, mouthBot + h * 0.013, cxl + hw * 0.85, mouthBot + h * 0.013,
                                cxl + (math.random() - 0.5) * hw * 0.4, mouthBot - lh, bone)
                        end
                    end

                    local img = sk:get_image()
                    if img then
                        c:draw_image({ src = img, x1 = 0, y1 = 0, x2 = w - 1, y2 = h - 1, opa = 90 })
                    end
                    sk:delete()
                end
                -- Glowing ember eyes set into the sockets.
                circle(w * 0.5 - w * 0.072, h * 0.405 + h * 0.10, 4, "#ff5a1a", 150)
                circle(w * 0.5 + w * 0.072, h * 0.405 + h * 0.10, 4, "#ff5a1a", 150)
            end

            -- Flames: dozens of translucent tapered "licks". Because they're
            -- semi-transparent and overlap, they pile into a soft, glowing, edge-
            -- free fire — dense and bright at the base (many overlap) and wispy at
            -- the tips (few) — rather than reading as hard triangles.
            for _ = 1, 46 do
                local fx   = math.random(-6, w + 6)
                local fh   = (0.10 + 0.40 * math.random() ^ 1.5) * h   -- mostly short, a few tall
                local bw   = 3 + math.random(0, 7)
                local lean = (math.random() - 0.5) * bw * 2.2
                c:draw_triangle({ p1 = { x = fx - bw, y = h }, p2 = { x = fx + bw, y = h },
                                  p3 = { x = fx + lean, y = h - fh },
                                  bg_color = t.hsv(6 + 44 * math.random(), 0.92, 0.7 + 0.3 * math.random()),
                                  bg_opa = 64 })
            end
            -- Bright hot cores near the base for intensity.
            for _ = 1, 12 do
                local fx = math.random(0, w)
                local fh = (0.05 + 0.10 * math.random()) * h
                local bw = 2 + math.random(0, 3)
                c:draw_triangle({ p1 = { x = fx - bw, y = h }, p2 = { x = fx + bw, y = h },
                                  p3 = { x = fx + (math.random() - 0.5) * bw, y = h - fh },
                                  bg_color = t.hsv(44 + 10 * math.random(), 0.45, 1.0), bg_opa = 210 })
            end
            local flameTop = math.floor(h * 0.48)   -- nominal fire top for smoke/sparks

            -- Smoke drifting up ABOVE the flames (dark, translucent, more diffuse
            -- the higher it goes).
            local smokeBase = flameTop - 4
            for _ = 1, 10 do
                local sy = math.random(math.floor(h * 0.08), math.floor(smokeBase))
                local f  = 1 - (sy - h * 0.08) / (smokeBase - h * 0.08)
                circle(math.random(0, w), sy, math.random(10, 16) + math.floor(f * 12),
                       "#3a342c", math.random(12, 30))
            end

            -- Sparks / embers ONLY above the flames, denser just above the tips.
            for _ = 1, 22 do
                local px = math.random(0, w)
                local py = flameTop - (math.random() ^ 1.5) * (flameTop - h * 0.12)
                local col = t.hsv(22 + 30 * math.random(), 0.9, 1.0)
                if math.random() < 0.35 then circle(px, py, 2, col, 55) end  -- glow
                circle(px, py, 1, col, 255)                                  -- ember
            end
        end)
    end,
}
