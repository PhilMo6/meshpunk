
return {
    name = "Fractal",
    apply = function(t)
        
t.background.procedural(function(c, w, h)

    local function circle(x, y, r, col, opa)
        c:draw_rect({
            x1 = x - r,
            y1 = y - r,
            x2 = x + r,
            y2 = y + r,
            radius = r,
            bg_color = col,
            bg_opa = opa or 255
        })
    end

    ---------------------------------------------------------
    -- Background
    ---------------------------------------------------------

    local bands = 26
    for i = 0, bands - 1 do
        local y1 = math.floor(i * h / bands)
        local y2 = math.floor((i + 1) * h / bands)
        local f = i / (bands - 1)

        c:draw_rect({
            x1 = 0,
            y1 = y1,
            x2 = w,
            y2 = y2,
            bg_color = t.hsv(
                235 + 25 * math.sin(f * math.pi),
                0.55,
                0.18 - f * 0.10
            ),
            bg_opa = 255
        })
    end

    ---------------------------------------------------------
    -- Nebula haze
    ---------------------------------------------------------

    for i = 1, 40 do
        local r = math.random(18, 60)

        circle(
            math.random(0, w),
            math.random(0, h),
            r,
            t.hsv(
                250 + math.random(-20, 20),
                0.45,
                0.8
            ),
            math.random(5, 18)
        )
    end

    ---------------------------------------------------------
    -- Stars
    ---------------------------------------------------------

    for i = 1, 180 do

        local x = math.random(0, w)
        local y = math.random(0, h)

        local b = math.random(140,255)

        circle(x,y,1,"#ffffff",b)

        if math.random() < .12 then
            circle(x,y,2,"#88ffff",40)
        end
    end

    ---------------------------------------------------------
    -- Recursive Fractal
    ---------------------------------------------------------

    local palette = {
        "#7df9ff",
        "#74cfff",
        "#a593ff",
        "#d17cff",
        "#ff72ce"
    }

    t.set_palette {
            scr      = "#0f1e28",  
            card     = "#161020",
            text     = "#e6e9ff",
            grey     = "#225255",
            accent   = palette[1],   -- (buttons / highlight)
            btn_text = "#000000",   
            dark     = true,
        }

    local function branch(x,y,len,ang,depth)

        if depth <= 0 or len < 4 then

            circle(
                x,
                y,
                3,
                palette[math.random(#palette)],
                210
            )

            circle(
                x,
                y,
                8,
                palette[math.random(#palette)],
                24
            )

            return
        end

        local nx = x + math.cos(ang) * len
        local ny = y + math.sin(ang) * len

        local col = palette[
            math.min(
                #palette,
                6-depth
            )
        ]

        c:draw_line({
            p1={x=x,y=y},
            p2={x=nx,y=ny},
            color=col,
            width=math.max(1,depth),
            opa=210
        })

        -------------------------------------------------
        -- glow
        -------------------------------------------------

        if depth > 2 then

            c:draw_line({
                p1={x=x,y=y},
                p2={x=nx,y=ny},
                color=col,
                width=depth*2,
                opa=18
            })

        end

        local spread = math.rad(
            18 + math.random()*18
        )

        local scale =
            0.70 +
            math.random()*0.08

        branch(
            nx,
            ny,
            len*scale,
            ang-spread,
            depth-1
        )

        branch(
            nx,
            ny,
            len*scale,
            ang+spread,
            depth-1
        )

        -------------------------------------------------
        -- occasional third branch
        -------------------------------------------------

        if math.random() < .35 then

            branch(
                nx,
                ny,
                len*(0.55+math.random()*0.1),
                ang+math.rad(math.random(-8,8)),
                depth-2
            )

        end
    end

    ---------------------------------------------------------
    -- Main crystal
    ---------------------------------------------------------

    local cx = w/2
    local cy = h/2

    local arms = 6

    for i=0,arms-1 do

        local angle =
            (math.pi*2/arms)*i +
            math.rad(math.random(-8,8))

        branch(
            cx,
            cy,
            math.min(w,h)*0.18,
            angle,
            6
        )

    end

    ---------------------------------------------------------
    -- Secondary fractals
    ---------------------------------------------------------

    for i=1,4 do

        local px =
            math.random(w*0.15,w*0.85)

        local py =
            math.random(h*0.15,h*0.85)

        branch(
            px,
            py,
            math.random(24,42),
            math.rad(math.random(0,359)),
            4
        )

    end

    ---------------------------------------------------------
    -- Core bloom
    ---------------------------------------------------------

    for r=24,8,-6 do

        circle(
            cx,
            cy,
            r,
            "#88cfff",
            4
        )

    end

    circle(cx,cy,7,"#ffffff",210)

    ---------------------------------------------------------
    -- Floating particles
    ---------------------------------------------------------

    for i=1,60 do

        local x=math.random(0,w)
        local y=math.random(0,h)

        local dx=x-cx
        local dy=y-cy

        local dist=math.sqrt(dx*dx+dy*dy)

        if dist<math.min(w,h)*0.33 then

            circle(
                x,
                y,
                1,
                palette[math.random(#palette)],
                math.random(80,180)
            )

        end

    end

    ---------------------------------------------------------
    -- Vignette
    ---------------------------------------------------------

    local edge = 18

    c:draw_rect({
        x1=0,
        y1=0,
        x2=w,
        y2=edge,
        bg_color="#000000",
        bg_opa=90
    })

    c:draw_rect({
        x1=0,
        y1=h-edge,
        x2=w,
        y2=h,
        bg_color="#000000",
        bg_opa=90
    })

    c:draw_rect({
        x1=0,
        y1=0,
        x2=edge,
        y2=h,
        bg_color="#000000",
        bg_opa=90
    })

    c:draw_rect({
        x1=w-edge,
        y1=0,
        x2=w,
        y2=h,
        bg_color="#000000",
        bg_opa=90
    })

end)
        
        
    end,
}
