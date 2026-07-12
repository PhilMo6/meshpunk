-- Terminal Green — phosphor CRT: black background, green scanlines + text.
return {
    name = "Circut",
    apply = function(t)
t.set_palette {
    scr      = "#020b0c",  -- deep PCB black-teal substrate
    card     = "#061a1c",  -- raised board / chip surface
    text     = "#7fffd4",  -- aquamarine signal text (clean + readable)
    grey     = "#0f3b3f",  -- muted trace lines / inactive components
    accent   = "#158520",  -- primary neon trace glow (circuit highlights)
    btn_text = "#c2c4c5",  -- dark contrast for neon buttons
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

    -------------------------------------------------------
    -- PCB GREEN SOLDER MASK BASE
    -------------------------------------------------------

    local bands = 30
    for i=0,bands-1 do
        local f = i/(bands-1)

        c:draw_rect({
            x1=0,
            y1=math.floor(i*h/bands),
            x2=w,
            y2=math.floor((i+1)*h/bands),
            bg_color=t.hsv(
                120 + math.random(-3,3), -- PCB green variation
                0.55,
                0.18 + 0.05*(1-f)
            ),
            bg_opa=255
        })
    end

    -------------------------------------------------------
    -- GRID (manufacturing / routing grid feel)
    -------------------------------------------------------

    local grid = 24

    for x=0,w,grid do
        c:draw_line({
            p1={x=x,y=0},
            p2={x=x,y=h},
            color="#0a3d0f",
            width=1,
            opa=60
        })
    end

    for y=0,h,grid do
        c:draw_line({
            p1={x=0,y=y},
            p2={x=w,y=y},
            color="#0a3d0f",
            width=1,
            opa=60
        })
    end

    -------------------------------------------------------
    -- VIA HOLES (drilled pads)
    -------------------------------------------------------

    local vias = {}

    for i=1,60 do
        local x = math.random(0,w)
        local y = math.random(0,h)

        vias[i] = {x=x,y=y}

        circle(x,y,2,"#c9d4c7",200)
        circle(x,y,5,"#0b1f0c",60) -- drill shadow
    end

    -------------------------------------------------------
    -- IC CHIPS (rectangular packages)
    -------------------------------------------------------

    local chips = {}

    for i=1,6 do
        local x = math.random(w*0.15,w*0.85)
        local y = math.random(h*0.15,h*0.85)

        local cw = math.random(60,120)
        local ch = math.random(40,80)

        chips[i] = {x=x,y=y,w=cw,h=ch}

        -- chip body
        c:draw_rect({
            x1=x-cw/2,
            y1=y-ch/2,
            x2=x+cw/2,
            y2=y+ch/2,
            radius=4,
            bg_color="#081a0c",
            bg_opa=255
        })

        -- chip label glow
        c:draw_rect({
            x1=x-cw/2,
            y1=y-ch/2,
            x2=x+cw/2,
            y2=y+ch/2,
            radius=4,
            bg_color="#39ff14",
            bg_opa=10
        })

        -- pins
        for p=0,7 do
            local tpos = p/7
            local py = y - ch/2 + tpos*ch

            c:draw_line({
                p1={x=x-cw/2,y=py},
                p2={x=x-cw/2-10,y=py},
                color="#9adf9a",
                width=2,
                opa=160
            })

            c:draw_line({
                p1={x=x+cw/2,y=py},
                p2={x=x+cw/2+10,y=py},
                color="#9adf9a",
                width=2,
                opa=160
            })
        end
    end

    -------------------------------------------------------
    -- RESISTORS (small striped components)
    -------------------------------------------------------

    for i=1,35 do
        local x = math.random(0,w)
        local y = math.random(0,h)

        local len = 14

        c:draw_line({
            p1={x=x-len,y=y},
            p2={x=x+len,y=y},
            color="#d9c27a",
            width=3,
            opa=200
        })

        -- resistor bands
        for b=-2,2 do
            c:draw_line({
                p1={x=x+b*3,y=y-4},
                p2={x=x+b*3,y=y+4},
                color="#5a3d1a",
                width=2,
                opa=180
            })
        end
    end

    -------------------------------------------------------
    -- CAPACITORS (vertical cylinders)
    -------------------------------------------------------

    for i=1,25 do
        local x = math.random(0,w)
        local y = math.random(0,h)

        c:draw_rect({
            x1=x-3,
            y1=y-10,
            x2=x+3,
            y2=y+10,
            radius=2,
            bg_color="#2b4d2b",
            bg_opa=220
        })

        c:draw_line({
            p1={x=x,y=y-10},
            p2={x=x,y=y-16},
            color="#b7d6b7",
            width=2,
            opa=180
        })

        c:draw_line({
            p1={x=x,y=y+10},
            p2={x=x,y=y+16},
            color="#b7d6b7",
            width=2,
            opa=180
        })
    end

    -------------------------------------------------------
    -- ROUTED TRACES (organized orthogonal routing)
    -------------------------------------------------------

    for i=1,#chips do
        local a = chips[i]

        for j=1,3 do
            local target = vias[math.random(#vias)]

            local mx = (a.x + target.x)/2

            -- orthogonal routing (PCB style)
            c:draw_line({
                p1={x=a.x,y=a.y},
                p2={x=mx,y=a.y},
                color="#2de2e6",
                width=2,
                opa=120
            })

            c:draw_line({
                p1={x=mx,y=a.y},
                p2={x=mx,y=target.y},
                color="#2de2e6",
                width=2,
                opa=120
            })

            c:draw_line({
                p1={x=mx,y=target.y},
                p2={x=target.x,y=target.y},
                color="#2de2e6",
                width=2,
                opa=120
            })
        end
    end

    -------------------------------------------------------
    -- SIGNAL PULSES (data flow visualization)
    -------------------------------------------------------

    for i=1,40 do
        local a = vias[math.random(#vias)]
        local b = vias[math.random(#vias)]

        local t = math.random()

        circle(
            a.x + (b.x-a.x)*t,
            a.y + (b.y-a.y)*t,
            2,
            "#ffffff",
            160
        )
    end

    -------------------------------------------------------
    -- EDGE DARKENING (board shadow)
    -------------------------------------------------------

    local e = 18

    c:draw_rect({x1=0,y1=0,x2=w,y2=e,bg_color="#000000",bg_opa=70})
    c:draw_rect({x1=0,y1=h-e,x2=w,y2=h,bg_color="#000000",bg_opa=70})
    c:draw_rect({x1=0,y1=0,x2=e,y2=h,bg_color="#000000",bg_opa=70})
    c:draw_rect({x1=w-e,y1=0,x2=w,y2=h,bg_color="#000000",bg_opa=70})

end)
        
        
        
    end
}
