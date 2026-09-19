-- Monthly — re-skins the ENTIRE palette for the current calendar month, computed
-- at apply time from t.month(). Each month has both a clearly-colored background
-- AND its own highlight/accent; the screen, cards and borders are all tinted to
-- the month too, so the month's color shows throughout the UI — not as a muddy
-- brown wash. (The old version desaturated warm hues into brown; the fix is high
-- saturation + a separately-chosen vivid accent per month.)
return {
    name = "Monthly",
    apply = function(t)
        local m = t.month()
        if m < 1 or m > 12 then m = 1 end   -- clock not set yet -> January

        -- Per month: `bg` hue tints the whole UI; `hi` is the vivid button/accent
        -- (also the selection-wash source), `em` the strong emphasized text and
        -- `at` the secondary emphasized text. `em` is hue-separated from `hi` so
        -- emphasized text stays readable ON the selection wash.
        local MONTHS = {
            { bg = 215, hi = "#8fd0ff", em = "#ffbf5e", at = "#6fe0c0" },  -- Jan  icy blue / frost
            { bg = 325, hi = "#ff6fae", em = "#ffd75e", at = "#c089ff" },  -- Feb  plum / rose
            { bg = 130, hi = "#66e06a", em = "#ff85c2", at = "#ffd836" },  -- Mar  spring green
            { bg = 172, hi = "#3fe0cf", em = "#ffb454", at = "#8fd0ff" },  -- Apr  teal / aqua
            { bg = 275, hi = "#c089ff", em = "#9fe06a", at = "#ff6fae" },  -- May  violet / lilac
            { bg = 50,  hi = "#ffd836", em = "#4fb4ff", at = "#ff9a3a" },  -- Jun  gold / sun
            { bg = 32,  hi = "#ff9a3a", em = "#4fc8ff", at = "#ffd836" },  -- Jul  warm orange
            { bg = 8,   hi = "#ff6a52", em = "#4fe0c0", at = "#ffd075" },  -- Aug  coral
            { bg = 40,  hi = "#f0b53a", em = "#c98aff", at = "#ff8e5a" },  -- Sep  amber / harvest
            { bg = 22,  hi = "#ff7e2a", em = "#8ae05a", at = "#b06fff" },  -- Oct  pumpkin
            { bg = 14,  hi = "#e0714a", em = "#8fc8f0", at = "#f0b53a" },  -- Nov  russet
            { bg = 348, hi = "#ff4f6b", em = "#ffd75e", at = "#66e06a" },  -- Dec  crimson / festive
        }
        local mo = MONTHS[m]
        local H = mo.bg

        -- Whole palette tinted to the month hue (high saturation so warm months
        -- read as gold/amber/red, never brown). Text is a near-white with a faint
        -- tint; button label stays dark for contrast on the bright accent.
        t.set_palette {
            scr      = t.hsv(H, 0.72, 0.12),
            card     = t.hsv(H, 0.55, 0.20),
            text     = t.hsv(H, 0.10, 0.95),
            grey     = t.hsv(H, 0.45, 0.34),
            accent   = mo.hi,
            btn_text = t.hsv(H, 0.85, 0.08),
            highlight   = mo.em,
            accent_text = mo.at,
            dark     = true,
        }

        -- Background: a saturated vertical gradient of the month's hue — vivid
        -- near the top, deepening toward the bottom — so the color clearly shows.
        t.background.procedural(function(c, w, h)
            local BANDS = 32
            for i = 0, BANDS - 1 do
                local y1 = math.floor(i * h / BANDS)
                local y2 = math.floor((i + 1) * h / BANDS) - 1
                local f = i / (BANDS - 1)          -- 0 top → 1 bottom
                local v = 0.32 - 0.23 * f          -- 0.32 (top) → 0.09 (bottom)
                c:draw_rect({ x1 = 0, y1 = y1, x2 = w - 1, y2 = y2,
                              bg_color = t.hsv(H, 0.80, v), bg_opa = 255 })
            end
        end)
    end,
}
