#ifndef EMOJI_FONT_H
#define EMOJI_FONT_H

#ifdef __cplusplus
extern "C" {
#endif

#include <lvgl.h>

/**
 * Create an imgfont that renders emoji from the meshpunk emoji blob
 * (S:/meshpunk/emojis.bin preferred when an SD card is mounted, else
 * L:/emojis.bin — see tools/build_emoji.sh for the format). Codepoints not
 * in the blob fall through to the base font.
 *
 * @param height    pixel height to advertise to LVGL (match base font size)
 * @param fallback  base font used for ASCII/Latin glyphs
 * @return          pointer to the new font, or NULL on failure
 */
lv_font_t * emoji_font_create(uint16_t height, const lv_font_t * fallback);

/** Destroy a font created by emoji_font_create. */
void emoji_font_destroy(lv_font_t * font);

/** Try to load an emoji glyph from the blob into the cache.
 *  Returns true if the glyph loaded successfully, false if missing/failed. */
bool emoji_preload(uint32_t codepoint);

/** Free every cached emoji glyph (the per-glyph PSRAM pixel buffer + descriptor)
 *  and reset the cache; glyphs re-load on demand on the next render. This cache
 *  is global C state that is never otherwise freed, so a session that renders
 *  many emoji (e.g. the Map's contact names) leaves a persistent mid-heap cluster
 *  that caps the largest contiguous PSRAM block. Called before launching a heavy
 *  ELF module so it gets a clean block. The open blob file + codepoint index +
 *  sequence table are kept (cheap, needed for re-load). */
void emoji_font_cache_clear(void);

/* ---- Multi-codepoint sequences (blob v2) ----------------------------------
 * The blob maps each shipped sequence (ZWJ families, skin-tone variants,
 * keycaps, flags) to a Private Use Area codepoint (0xE000+i) whose glyph is a
 * normal blob entry. Rule: WIRE + DISK carry real Unicode; the Lua/UI side
 * carries the composed PUA form. Convert at the boundaries only:
 *   - compose   when pushing received/stored text up to Lua (render as one glyph)
 *   - decompose in the send bindings (peers receive standard emoji)
 */

/** Replace known emoji sequences in a UTF-8 string with their PUA codepoints.
 *  FE0F-insensitive, greedy longest-match. Returns a newly allocated string
 *  (free() it), or NULL if nothing matched / on failure — caller keeps using
 *  the original string in that case. */
char * emoji_compose(const char * in);

/** Expand PUA codepoints back to their real emoji sequences. Returns a newly
 *  allocated string (free() it), or NULL if the input contains no PUA / on
 *  failure — caller keeps using the original string in that case. */
char * emoji_decompose(const char * in);

/** Number of glyphs in the blob (0 if unavailable). For the settings picker. */
uint32_t emoji_blob_count(void);

/** Close and fully re-open the blob (SD preferred), clearing the glyph cache.
 *  Call after downloading/removing the SD extended set so it takes effect
 *  without a reboot. close_only=true releases the file handle and state
 *  WITHOUT re-opening (so the on-disk blob can be removed/renamed safely) —
 *  call again with false afterwards. Returns the new glyph count (0 when
 *  close_only or no blob found). */
uint32_t emoji_font_reload(bool close_only);

/** Codepoint of blob glyph i (sorted ascending), 0 if out of range. */
uint32_t emoji_blob_cp_at(uint32_t i);

#ifdef __cplusplus
}
#endif

#endif
