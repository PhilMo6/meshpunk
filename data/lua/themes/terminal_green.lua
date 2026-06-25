-- Terminal Green — phosphor CRT: black background, green scanlines + text.
return {
    name = "Terminal Green",
    apply = function(t)
        t.set_palette {
            scr      = "#001200",
            card     = "#032803",
            text     = "#39ff14",
            grey     = "#0a4a0a",
            accent   = "#19a019",
            btn_text = "#001200",
            dark     = true,
        }
        t.background.procedural(function(c, w, h)
            c:fill_bg("#001200", 255)
            for y = 0, h - 1, 3 do
                c:draw_rect({ x1 = 0, y1 = y, x2 = w - 1, y2 = y, bg_color = "#39ff14", bg_opa = 16 })
            end
        end)
    end,
}
