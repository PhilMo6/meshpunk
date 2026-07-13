#ifndef THEME_FONT_H
#define THEME_FONT_H

#include <lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Runtime TTF fonts (LVGL tiny_ttf, data mode — whole file in PSRAM).
 *
 * TWO FONT ROLES:
 *   ui   — all interface chrome; everything inherits it (the emoji imgfont
 *          g_ui_font that every style points at).
 *   text — reading content; objects OPT IN with
 *          `text_font = lvgl.Font("text", 16)` (e.g. Messenger chat bubbles;
 *          16 = the standard UI size, selecting the role's chain head —
 *          luavgl's Font() defaults to size 12 when omitted).
 *
 * Per-role resolution (first hit wins), applied by fallback-pointer swaps on
 * two STABLE emoji-imgfont chain heads — nothing ever rebuilds views:
 *
 *   head(role) -> theme font(role) ?: user default(role) ?: bundled Noto Sans
 *              -> lv_font_montserrat_14   (FontAwesome symbols + last resort)
 *
 * Theme fonts come from a theme's t.set_font{ui=,text=}; user defaults are
 * persisted firmware prefs (font_ui= / font_text=) set from Settings > Fonts.
 * .ttf (glyf outlines) only — CFF .otf is rejected cleanly. All calls are
 * LVGL/Lua-task only. */

typedef enum {
    THEME_FONT_UI = 0,
    THEME_FONT_TEXT = 1,
} theme_font_role_t;

/** Load the bundled default + the user-default prefs (idempotent), create the
 *  text-role chain head, and apply both chains. Called from setupLuaVGL() so
 *  the post-ELF Lua rebuild restores everything after release_all().
 *  ui_pref/text_pref: persisted user-default paths ("" = bundled). */
void theme_font_init(const char * ui_pref, const char * text_pref);

/** Set a role's THEME font from a drive-prefixed path. px <= 0 = UI size (16).
 *  Idempotent per path+size (themes re-apply on every show_background). On
 *  failure the current resolution is kept. Replaced fonts are destroyed
 *  DEFERRED (one-shot lv_timer — draw units may hold glyph-cache refs). */
bool theme_font_set(theme_font_role_t role, const char * path, int px);

/** Drop BOTH theme slots (lib/theme calls this before every theme apply). */
void theme_font_clear(void);

/** Set a role's USER DEFAULT font. NULL/"" reverts to the bundled Noto Sans.
 *  Persistence is the caller's job (the binding saves firmware_prefs). */
bool font_default_set(theme_font_role_t role, const char * path);

/** The role's font at px: the emoji-wrapped chain head (px 16 = the role head
 *  itself; other sizes are cached emoji-wrapped instances that survive font
 *  swaps — their internals are rebuilt in place). NULL only when nothing can
 *  be served. Backs lvgl.Font("ui"/"text", px). */
const lv_font_t * theme_font_sized(theme_font_role_t role, int px);

/** Free EVERYTHING (all slots, sized instances, the text head, pending
 *  deferred frees) and restore both chains to montserrat. Called from
 *  luaTearDown before an ELF module launches. */
void theme_font_release_all(void);

#ifdef __cplusplus
}
#endif

#endif
