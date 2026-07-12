-- Default — the original MeshPunk look, expressed as a normal theme (same system
-- as every other theme; nothing special). Values taken straight from the
-- pre-theme-system firmware (commit 7163dd8, dark mode): a dark charcoal
-- background, dark slate panels, near-black buttons with white labels, near-white
-- text. The selection highlight is derived from the button color by the theme.
return {
    name = "Default",
    apply = function(t)
        t.set_palette {
            scr      = "#202329",   -- charcoal background (lifted from the original #15171a)
            card     = "#282b30",   -- dark slate panels
            text     = "#fafafa",   -- near-white text
            grey     = "#2f3237",   -- borders / muted chrome
            accent   = "#101010",   -- near-black buttons (the original primary)
            btn_text = "#ffffff",   -- white button labels
            dark     = true,
        }
        t.background.fill("#202329")
    end,
}
