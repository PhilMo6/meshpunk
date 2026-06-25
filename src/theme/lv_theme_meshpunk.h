/**
 * @file lv_theme_meshpunk.h
 *
 */

#ifndef LV_THEME_MESHPUNK_H
#define LV_THEME_MESHPUNK_H

#ifdef __cplusplus
extern "C" {
#endif

/*********************
 *      INCLUDES
 *********************/
#include "lvgl.h"

/*********************
 *      DEFINES
 *********************/

/**********************
 *      TYPEDEFS
 **********************/

/**********************
 * GLOBAL PROTOTYPES
 **********************/

/**
 * Initialize the Meshpunk theme
 * @param disp pointer to display
 * @param color_primary the primary color of the theme
 * @param color_secondary the secondary color for the theme
 * @param dark
 * @param font pointer to a font to use.
 * @return a pointer to reference this theme later
 */
lv_theme_t * lv_theme_meshpunk_init(lv_display_t * disp, lv_color_t color_primary, lv_color_t color_secondary, bool dark,
                                     const lv_font_t * font);

/**
 * Push a flat color palette to the live theme and re-cascade it to every widget
 * (lv_obj_report_style_change). Colors are 0xRRGGBB. Safe to call repeatedly —
 * it is a no-op when the palette is unchanged. Used by the Lua theme manager so
 * a theme can be switched at runtime without a reboot.
 * @param scr      screen background
 * @param card     card / panel background
 * @param text     default text color
 * @param grey     borders / muted chrome
 * @param accent   button background / highlight (the old hotpink slot)
 * @param btn_text button label color
 * @param dark     true for a dark base, false for light
 */
void lv_theme_meshpunk_set_palette(uint32_t scr, uint32_t card, uint32_t text,
                                   uint32_t grey, uint32_t accent, uint32_t btn_text,
                                   bool dark);

/**
 * Get meshpunk theme
 * @return a pointer to meshpunk theme, or NULL if this is not initialized
 */
lv_theme_t * lv_theme_meshpunk_get(void);

/**
 * Check if meshpunk theme is initialized
 * @return true if meshpunk theme is initialized, false otherwise
 */
bool lv_theme_meshpunk_is_inited(void);

/**
 * Deinitialize the meshpunk theme
 */
void lv_theme_meshpunk_deinit(void);

/**********************
 *      MACROS
 **********************/

#endif

#ifdef __cplusplus
} /*extern "C"*/
#endif

