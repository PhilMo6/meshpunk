#ifndef EMOJI_FONT_H
#define EMOJI_FONT_H

#ifdef __cplusplus
extern "C" {
#endif

#include <lvgl.h>

/**
 * Create an imgfont that maps Unicode codepoints >= 0x2600 to PNG files on SD
 * at S:/emoji/<hex>.png (lowercase, no zero-padding). Codepoints below that
 * threshold return NULL so the base font is used.
 *
 * @param height    pixel height to advertise to LVGL (match base font size)
 * @param fallback  base font used for ASCII/Latin glyphs
 * @return          pointer to the new font, or NULL on failure
 */
lv_font_t * emoji_font_create(uint16_t height, const lv_font_t * fallback);

/** Destroy a font created by emoji_font_create. */
void emoji_font_destroy(lv_font_t * font);

/** Try to load an emoji .bin from SD into the cache.
 *  Returns true if the glyph loaded successfully, false if missing/failed. */
bool emoji_preload(uint32_t codepoint);

#ifdef __cplusplus
}
#endif

#endif
