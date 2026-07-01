
return {
    name = "Sacred Geometry",
    apply = function(t)
        t.set_palette {
            scr      = "#001012",
            card     = "#032328",
            text     = "#ffd36e",
            grey     = "#0f486d",
            accent   = "#8fe9ff",
            btn_text = "#000b12",
            dark     = true,
        }
        

        t.background.procedural(function(c, w, h)

        local function circle(x,y,r,col,opa)
            c:draw_rect({
                x1=x-r,y1=y-r,
                x2=x+r,y2=y+r,
                radius=r,
                bg_color=col,
                bg_opa=opa or 255
            })
        end

        local function triangle(cx,cy,r,rot,col,opa)
            local pts = {}
            for i=0,2 do
                local a = rot + i * (math.pi * 2 / 3)
                pts[#pts+1] = {
                    x = cx + math.cos(a) * r,
                    y = cy + math.sin(a) * r
                }
            end

            for i=1,3 do
                c:draw_line({
                    p1 = pts[i],
                    p2 = pts[(i % 3) + 1],
                    color = col,
                    width = 2,
                    opa = opa or 180
                })
            end
        end

        local function polygon(cx,cy,r,sides,rot,col,opa)
            for i=0,sides-1 do
                local a1 = rot + i * (math.pi * 2 / sides)
                local a2 = rot + (i+1) * (math.pi * 2 / sides)

                local p1 = {
                    x = cx + math.cos(a1) * r,
                    y = cy + math.sin(a1) * r
                }

                local p2 = {
                    x = cx + math.cos(a2) * r,
                    y = cy + math.sin(a2) * r
                }

                c:draw_line({
                    p1=p1,
                    p2=p2,
                    color=col,
                    width=2,
                    opa=opa or 140
                })
            end
        end

        -------------------------------------------------------
        -- Background gradient
        -------------------------------------------------------

        local bands = 26
        for i=0,bands-1 do
            local f = i/(bands-1)
            c:draw_rect({
                x1=0,
                y1=math.floor(i*h/bands),
                x2=w,
                y2=math.floor((i+1)*h/bands),
                bg_color=t.hsv(240 + 20*f, 0.5, 0.12 + 0.04*(1-f)),
                bg_opa=255
            })
        end

        -------------------------------------------------------
        -- Star field
        -------------------------------------------------------

        for i=1,160 do
            local x = math.random(0,w)
            local y = math.random(0,h)
            circle(x,y,1,"#ffffff",math.random(60,170))
        end

        -------------------------------------------------------
        -- Center anchor
        -------------------------------------------------------

        local cx, cy = w/2, h/2

        -- soft bloom
        for r=140,20,-14 do
            circle(cx,cy,r,"#6fd6ff",5)
        end

        -------------------------------------------------------
        -- Outer sacred rings (non-symbolic geometry)
        -------------------------------------------------------

        for r=60,220,26 do
            circle(cx,cy,r,"#8fdcff",18)
        end

        -------------------------------------------------------
        -- Radial spokes
        -------------------------------------------------------

        for i=0,47 do
            local a = i * math.pi / 24
            c:draw_line({
                p1 = {
                    x = cx + math.cos(a)*18,
                    y = cy + math.sin(a)*18
                },
                p2 = {
                    x = cx + math.cos(a)*260,
                    y = cy + math.sin(a)*260
                },
                color = "#7fbfff",
                width = 1,
                opa = 45
            })
        end

        -------------------------------------------------------
        -- Central TRIANGLE CLUSTER (replaces any symbolic form)
        -- 5 overlapping triangles at different rotations + scales
        -------------------------------------------------------

        local tri_r = 92

        local tri_set = {
            {0.00, 1.00, 210},
            {math.rad(72), 0.92, 190},
            {math.rad(144), 1.05, 180},
            {math.rad(216), 0.88, 170},
            {math.rad(288), 1.08, 160},
        }

        local colors = {
            "#ffffff",
            "#8fe9ff",
            "#b38bff",
            "#ff7fd1",
            "#ffd36e"
        }

        for i,v in ipairs(tri_set) do
            triangle(
                cx,
                cy,
                tri_r * v[2],
                v[1],
                colors[math.random(1,#colors)],
                v[3]
            )
        end

        -------------------------------------------------------
        -- Inner rotating octagon ring
        -------------------------------------------------------

        for i=0,2 do
            polygon(
                cx,
                cy,
                120 + i*14,
                8,
                math.rad(i*12),
                "#c0f0ff",
                90
            )
        end

        -------------------------------------------------------
        -- Orbiting nodes
        -------------------------------------------------------

        for ring=1,3 do
            local rr = 140 + ring*60

            for i=0,11 do
                local a = i * math.pi/6 + ring*0.25
                local x = cx + math.cos(a)*rr
                local y = cy + math.sin(a)*rr

                circle(x,y,3,"#ffffff",160)
                circle(x,y,8,"#6fd6ff",18)
            end
        end

        -------------------------------------------------------
        -- Floating geometric noise
        -------------------------------------------------------

        for i=1,110 do
            local a = math.random()*math.pi*2
            local d = math.random()*260

            local x = cx + math.cos(a)*d
            local y = cy + math.sin(a)*d

            circle(x,y,1,"#ffffff",math.random(70,160))
        end

        -------------------------------------------------------
        -- Subtle vignette
        -------------------------------------------------------

        local edge = 20

        c:draw_rect({x1=0,y1=0,x2=w,y2=edge,bg_color="#000000",bg_opa=80})
        c:draw_rect({x1=0,y1=h-edge,x2=w,y2=h,bg_color="#000000",bg_opa=80})
        c:draw_rect({x1=0,y1=0,x2=edge,y2=h,bg_color="#000000",bg_opa=80})
        c:draw_rect({x1=w-edge,y1=0,x2=w,y2=h,bg_color="#000000",bg_opa=80})

        end)




    end
}
