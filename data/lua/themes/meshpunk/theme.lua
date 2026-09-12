-- Meshpunk — the project logo as the wallpaper: the M/P mesh glyph in the brand
-- neon ramp (green #3dff8e -> teal #35e6c8 -> cyan #4be1ff) over a dark ground,
-- surrounded by a faint field of far-off nodes.
--
-- wall.png carries meshpunk-icon.svg's logo geometry, re-laid-out for a 320x240
-- landscape frame — the resolution of every supported board. background.image()
-- places the image 1:1 without scaling, so its size must match the screen.
return {
    name = "Meshpunk",
    apply = function(t)
        t.set_palette {
            scr      = "#070b0a",   -- the wallpaper's own ground color
            card     = "#0f1a17",
            text     = "#dcffe9",
            grey     = "#1c2b24",
            accent   = "#3dff8e",
            btn_text = "#04120b",
            dark     = true,
        }
        t.background.image(t.dir .. "/wall.png")
    end,
}
