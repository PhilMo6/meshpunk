-- Midnight — dark slate chrome with a dark-purple accent.
-- Solid background (no canvas), so it costs no PSRAM.
return {
    name = "Midnight",
    apply = function(t)
        t.set_palette {
            scr      = "#15171A",
            card     = "#282b30",
            text     = "#e6e6e6",
            grey     = "#2f3237",
            accent   = "#5a2d82",
            btn_text = "#ffffff",
            dark     = true,
        }
        t.background.fill("#15171A")
    end,
}
