-- Daylight — a light theme: near-white cards, dark text, blue accent. Exercises
-- the C theme's light-mode path (dark = false).
return {
    name = "Daylight",
    apply = function(t)
        t.set_palette {
            scr      = "#f3f4f6",
            card     = "#ffffff",
            text     = "#1f2937",
            grey     = "#c8ccd2",
            accent   = "#2563eb",
            btn_text = "#ffffff",
            dark     = false,
        }
        t.background.fill("#f3f4f6")
    end,
}
