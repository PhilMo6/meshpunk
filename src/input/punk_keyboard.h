/**
 * @file punk_keyboard.h
 *
 * Meshpunk on-screen keyboard widget — the LVGL keyboard for lib/osk.lua.
 *
 * A buttonmatrix subclass (same construction as lv_keyboard) that owns its own
 * key maps so it can offer a BIG layout beside the normal one. The bottom-left
 * keyboard key toggles the two; it no longer sends LV_EVENT_CANCEL.
 *
 * Size and mode are orthogonal: the big layouts carry no mode keys, so
 * uppercase and symbols are picked in the normal layout and the big layout for
 * that mode comes up on the next toggle. Big symbol mode is the one exception —
 * its 1# key cycles the two symbol pages.
 *
 * Size is process-global (a file static in punk_keyboard.c), so the OSK reopens
 * in whichever size was last used and returns to normal on reboot.
 */

#ifndef PUNK_KEYBOARD_H
#define PUNK_KEYBOARD_H

#ifdef __cplusplus
extern "C" {
#endif

#include <lvgl.h>

/** Key map set. Size is a separate axis — see punk_keyboard_set_big(). */
typedef enum {
    PUNK_KB_MODE_LOWER = 0,
    PUNK_KB_MODE_UPPER,
    PUNK_KB_MODE_SYM,
} punk_keyboard_mode_t;

extern const lv_obj_class_t punk_keyboard_class;

lv_obj_t *punk_keyboard_create(lv_obj_t *parent);

void punk_keyboard_set_textarea(lv_obj_t *obj, lv_obj_t *ta);
lv_obj_t *punk_keyboard_get_textarea(const lv_obj_t *obj);

void punk_keyboard_set_mode(lv_obj_t *obj, punk_keyboard_mode_t mode);
punk_keyboard_mode_t punk_keyboard_get_mode(const lv_obj_t *obj);

/** Big-key layout on/off. Global, not per widget. */
void punk_keyboard_set_big(lv_obj_t *obj, bool en);
bool punk_keyboard_get_big(void);

/* Registers the luavgl binding: the "punk_kb" metatable plus a PunkKeyboard
 * entry in luavgl's widget-creation table (parent:PunkKeyboard{} and
 * lvgl.PunkKeyboard{}). Call once, after luaopen_lvgl — the base class
 * metatable (lv_buttonmatrix, registered by luavgl's own keyboard binding)
 * must already exist. The struct is forward-declared at file scope so callers
 * that only want the widget need no Lua headers. */
struct lua_State;
void punk_keyboard_lua_register(struct lua_State *L);

#ifdef __cplusplus
} /*extern "C"*/
#endif

#endif /*PUNK_KEYBOARD_H*/
