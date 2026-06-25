-- Amber CRT — vintage amber monochrome monitor: black background, amber scanlines.
return {
    name = "Amber CRT",
    apply = function(t)
        t.set_palette {
            scr      = "#140d00",
            card     = "#2a1c00",
            text     = "#ffb000",
            grey     = "#4a3300",
            accent   = "#ff8800",
            btn_text = "#140d00",
            dark     = true,
        }
        t.background.procedural(function(c, w, h)
            c:fill_bg("#140d00", 255)
            for y = 0, h - 1, 3 do
                c:draw_rect({ x1 = 0, y1 = y, x2 = w - 1, y2 = y, bg_color = "#ffb000", bg_opa = 16 })
            end
        end)
    end,
}
