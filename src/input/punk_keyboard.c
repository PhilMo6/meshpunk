/**
 * @file punk_keyboard.c
 *
 * See punk_keyboard.h. Built the way lv_keyboard is: a buttonmatrix subclass
 * whose constructor installs one LV_EVENT_VALUE_CHANGED handler; the class
 * itself declares no event_cb, so the buttonmatrix base keeps handling drawing,
 * input and the popover extended draw size.
 *
 * The maps live here rather than going through lv_keyboard_set_map() because
 * that setter writes into a file-static table shared by every keyboard in the
 * process, and the stock event handler hard-codes "abc"/"ABC"/"1#" to the
 * built-in modes, which would fight the symbol page cycle.
 */

#include "punk_keyboard.h"

#include <string.h>

#include "../../lib/lvgl/src/core/lv_obj_class_private.h"
#include "../../lib/lvgl/src/widgets/buttonmatrix/lv_buttonmatrix_private.h"

#include <luavgl.h>

/*********************
 *      DEFINES
 *********************/

/* Control key (mode switch, size toggle, OK): grey, fires on release, no
 * repeat. Same combination as the stock LV_KEYBOARD_CTRL_BUTTON_FLAGS. */
#define CTL(w) (LV_BUTTONMATRIX_CTRL_NO_REPEAT | LV_BUTTONMATRIX_CTRL_CLICK_TRIG | \
                LV_BUTTONMATRIX_CTRL_CHECKED | (w))

/* Grey key that still repeats when held: backspace and the cursor arrows. */
#define GREY(w) (LV_BUTTONMATRIX_CTRL_CHECKED | (w))

/* Big-layout typing key: popover while pressed, and commits on RELEASE so a
 * finger that lands between two keys can slide onto the right one before
 * lifting (the lesson the module-side host_osk learned on hardware). */
#define BIG(w) (LV_BUTTONMATRIX_CTRL_POPOVER | LV_BUTTONMATRIX_CTRL_CLICK_TRIG | \
                LV_BUTTONMATRIX_CTRL_NO_REPEAT | (w))

/**********************
 *      TYPEDEFS
 **********************/

typedef struct {
    lv_buttonmatrix_t btnm;
    lv_obj_t *ta;        /* assigned text area, or NULL */
    uint8_t mode;        /* punk_keyboard_mode_t */
    uint8_t sym_page;    /* 0/1 — big symbol layout only */
} punk_keyboard_t;

/**********************
 *  STATIC PROTOTYPES
 **********************/

static void punk_kb_constructor(const lv_obj_class_t *class_p, lv_obj_t *obj);
static void punk_kb_event_cb(lv_event_t *e);
static void punk_kb_update_map(lv_obj_t *obj);

/**********************
 *  STATIC VARIABLES
 **********************/

const lv_obj_class_t punk_keyboard_class = {
    .constructor_cb = punk_kb_constructor,
    .width_def      = LV_PCT(100),
    .height_def     = LV_PCT(50),
    .instance_size  = sizeof(punk_keyboard_t),
    .editable       = 1,
    .base_class     = &lv_buttonmatrix_class,
    .name           = "punk_keyboard",
};

/* Big-key mode is process-global on purpose: the OSK modal is destroyed on
 * every close, so a per-instance flag would forget the choice each time. */
static bool s_big = false;

/* ── Normal layouts ────────────────────────────────────────────────────────
 * Byte-for-byte the stock LVGL maps, with two deliberate differences:
 * popovers are absent (the stock keyboard strips them at runtime, so this
 * renders identically), and the uppercase map's bottom-left key is
 * LV_SYMBOL_KEYBOARD rather than LV_SYMBOL_CLOSE so the size toggle carries
 * the same glyph in every mode. */

static const char *const map_lower[] = {
    "1#", "q", "w", "e", "r", "t", "y", "u", "i", "o", "p", LV_SYMBOL_BACKSPACE, "\n",
    "ABC", "a", "s", "d", "f", "g", "h", "j", "k", "l", LV_SYMBOL_NEW_LINE, "\n",
    "_", "-", "z", "x", "c", "v", "b", "n", "m", ".", ",", ":", "\n",
    LV_SYMBOL_KEYBOARD, LV_SYMBOL_LEFT, " ", LV_SYMBOL_RIGHT, LV_SYMBOL_OK, ""
};

static const lv_buttonmatrix_ctrl_t ctrl_lower[] = {
    CTL(5), 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, GREY(7),
    CTL(6), 3, 3, 3, 3, 3, 3, 3, 3, 3, GREY(7),
    GREY(1), GREY(1), 1, 1, 1, 1, 1, 1, 1, GREY(1), GREY(1), GREY(1),
    CTL(2), GREY(2), 6, GREY(2), CTL(2)
};

static const char *const map_upper[] = {
    "1#", "Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P", LV_SYMBOL_BACKSPACE, "\n",
    "abc", "A", "S", "D", "F", "G", "H", "J", "K", "L", LV_SYMBOL_NEW_LINE, "\n",
    "_", "-", "Z", "X", "C", "V", "B", "N", "M", ".", ",", ":", "\n",
    LV_SYMBOL_KEYBOARD, LV_SYMBOL_LEFT, " ", LV_SYMBOL_RIGHT, LV_SYMBOL_OK, ""
};

static const lv_buttonmatrix_ctrl_t ctrl_upper[] = {
    CTL(5), 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, GREY(7),
    CTL(6), 3, 3, 3, 3, 3, 3, 3, 3, 3, GREY(7),
    GREY(1), GREY(1), 1, 1, 1, 1, 1, 1, 1, GREY(1), GREY(1), GREY(1),
    CTL(2), GREY(2), 6, GREY(2), CTL(2)
};

static const char *const map_sym[] = {
    "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", LV_SYMBOL_BACKSPACE, "\n",
    "abc", "+", "&", "/", "*", "=", "%", "!", "?", "#", "<", ">", "\n",
    "\\", "@", "$", "(", ")", "{", "}", "[", "]", ";", "\"", "'", "\n",
    LV_SYMBOL_KEYBOARD, LV_SYMBOL_LEFT, " ", LV_SYMBOL_RIGHT, LV_SYMBOL_OK, ""
};

/* Rows are 11/12/12/5. The stock ctrl map for this layout is 11/13/11 — the
 * total still matches, so LVGL never notices the rows are shifted, and every
 * entry it shifts across is identical anyway. Aligned properly here. */
static const lv_buttonmatrix_ctrl_t ctrl_sym[] = {
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, GREY(2),
    CTL(2), 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    CTL(2), GREY(2), 6, GREY(2), CTL(2)
};

/* ── Big layouts ───────────────────────────────────────────────────────────
 * No mode keys: the letters have the rows to themselves and backspace moves
 * down to the short third row. Mode is picked in the normal layout. The one
 * mode key that survives is 1# on the symbol pages, where it cycles A/B.
 * Row 4 is the stock row 4 in every layout. */

static const char *const map_big_lower[] = {
    "q", "w", "e", "r", "t", "y", "u", "i", "o", "p", "\n",
    "a", "s", "d", "f", "g", "h", "j", "k", "l", "\n",
    "z", "x", "c", "v", "b", "n", "m", LV_SYMBOL_BACKSPACE, "\n",
    LV_SYMBOL_KEYBOARD, LV_SYMBOL_LEFT, " ", LV_SYMBOL_RIGHT, LV_SYMBOL_OK, ""
};

static const lv_buttonmatrix_ctrl_t ctrl_big_letters[] = {
    BIG(1), BIG(1), BIG(1), BIG(1), BIG(1), BIG(1), BIG(1), BIG(1), BIG(1), BIG(1),
    BIG(1), BIG(1), BIG(1), BIG(1), BIG(1), BIG(1), BIG(1), BIG(1), BIG(1),
    BIG(1), BIG(1), BIG(1), BIG(1), BIG(1), BIG(1), BIG(1), GREY(1),
    CTL(2), GREY(2), 6, GREY(2), CTL(2)
};

static const char *const map_big_upper[] = {
    "Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P", "\n",
    "A", "S", "D", "F", "G", "H", "J", "K", "L", "\n",
    "Z", "X", "C", "V", "B", "N", "M", LV_SYMBOL_BACKSPACE, "\n",
    LV_SYMBOL_KEYBOARD, LV_SYMBOL_LEFT, " ", LV_SYMBOL_RIGHT, LV_SYMBOL_OK, ""
};

/* Page A — numbers and everyday punctuation. The five keys evicted from the
 * big letter rows (_ - . , :) live here: the stock symbol map has none of
 * them, so without this you could not type a period in big mode. */
static const char *const map_big_sym_a[] = {
    "1#", "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "\n",
    ".", ",", "?", "!", "'", "\"", "-", "_", ":", "\n",
    "@", "#", "$", "%", "&", "*", "+", LV_SYMBOL_BACKSPACE, "\n",
    LV_SYMBOL_KEYBOARD, LV_SYMBOL_LEFT, " ", LV_SYMBOL_RIGHT, LV_SYMBOL_OK, ""
};

/* Page B — math, brackets and the rest. The digits repeat so numbers are never
 * a page away; ^ ~ ` | fill the spare slots and the stock keyboard has none. */
static const char *const map_big_sym_b[] = {
    "1#", "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "\n",
    "=", "/", "\\", "<", ">", "(", ")", "[", "]", "\n",
    "{", "}", ";", "^", "~", "`", "|", LV_SYMBOL_BACKSPACE, "\n",
    LV_SYMBOL_KEYBOARD, LV_SYMBOL_LEFT, " ", LV_SYMBOL_RIGHT, LV_SYMBOL_OK, ""
};

/* Shared by both pages — 1# is narrowed to 2 units so the digits keep the row. */
static const lv_buttonmatrix_ctrl_t ctrl_big_sym[] = {
    CTL(2), BIG(3), BIG(3), BIG(3), BIG(3), BIG(3), BIG(3), BIG(3), BIG(3), BIG(3), BIG(3),
    BIG(1), BIG(1), BIG(1), BIG(1), BIG(1), BIG(1), BIG(1), BIG(1), BIG(1),
    BIG(1), BIG(1), BIG(1), BIG(1), BIG(1), BIG(1), BIG(1), GREY(1),
    CTL(2), GREY(2), 6, GREY(2), CTL(2)
};

/**********************
 *   GLOBAL FUNCTIONS
 **********************/

lv_obj_t *punk_keyboard_create(lv_obj_t *parent)
{
    lv_obj_t *obj = lv_obj_class_create_obj(&punk_keyboard_class, parent);
    lv_obj_class_init_obj(obj);
    return obj;
}

void punk_keyboard_set_textarea(lv_obj_t *obj, lv_obj_t *ta)
{
    punk_keyboard_t *kb = (punk_keyboard_t *)obj;

    /* Cursor management, same as the stock keyboard. */
    if (kb->ta) lv_obj_remove_state(obj, LV_STATE_FOCUSED);
    kb->ta = ta;
    if (kb->ta) lv_obj_add_state(obj, LV_STATE_FOCUSED);
}

lv_obj_t *punk_keyboard_get_textarea(const lv_obj_t *obj)
{
    return ((const punk_keyboard_t *)obj)->ta;
}

void punk_keyboard_set_mode(lv_obj_t *obj, punk_keyboard_mode_t mode)
{
    punk_keyboard_t *kb = (punk_keyboard_t *)obj;
    if (kb->mode == (uint8_t)mode) return;
    kb->mode = (uint8_t)mode;
    punk_kb_update_map(obj);
}

punk_keyboard_mode_t punk_keyboard_get_mode(const lv_obj_t *obj)
{
    return (punk_keyboard_mode_t)((const punk_keyboard_t *)obj)->mode;
}

void punk_keyboard_set_big(lv_obj_t *obj, bool en)
{
    if (s_big == en) return;
    s_big = en;
    punk_kb_update_map(obj);
}

bool punk_keyboard_get_big(void) { return s_big; }

/**********************
 *   STATIC FUNCTIONS
 **********************/

static void punk_kb_constructor(const lv_obj_class_t *class_p, lv_obj_t *obj)
{
    LV_UNUSED(class_p);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICK_FOCUSABLE);

    punk_keyboard_t *kb = (punk_keyboard_t *)obj;
    kb->ta       = NULL;
    kb->mode     = PUNK_KB_MODE_LOWER;
    kb->sym_page = 0;

    lv_obj_align(obj, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_event_cb(obj, punk_kb_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_set_style_base_dir(obj, LV_BASE_DIR_LTR, 0);

    punk_kb_update_map(obj);
}

static void punk_kb_update_map(lv_obj_t *obj)
{
    punk_keyboard_t *kb = (punk_keyboard_t *)obj;
    const char *const *map;
    const lv_buttonmatrix_ctrl_t *ctrl;

    if (s_big) {
        switch (kb->mode) {
            case PUNK_KB_MODE_UPPER:
                map = map_big_upper; ctrl = ctrl_big_letters; break;
            case PUNK_KB_MODE_SYM:
                map  = kb->sym_page ? map_big_sym_b : map_big_sym_a;
                ctrl = ctrl_big_sym; break;
            default:
                map = map_big_lower; ctrl = ctrl_big_letters; break;
        }
    } else {
        switch (kb->mode) {
            case PUNK_KB_MODE_UPPER: map = map_upper; ctrl = ctrl_upper; break;
            case PUNK_KB_MODE_SYM:   map = map_sym;   ctrl = ctrl_sym;   break;
            default:                 map = map_lower; ctrl = ctrl_lower; break;
        }
    }

    /* Order matters: set_map recounts the buttons, and set_ctrl_map copies
     * exactly that many entries. */
    lv_buttonmatrix_set_map(obj, (const char **)map);
    lv_buttonmatrix_set_ctrl_map(obj, ctrl);
}

static void punk_kb_event_cb(lv_event_t *e)
{
    lv_obj_t *obj = lv_event_get_current_target(e);
    punk_keyboard_t *kb = (punk_keyboard_t *)obj;

    uint32_t id = lv_buttonmatrix_get_selected_button(obj);
    if (id == LV_BUTTONMATRIX_BUTTON_NONE) return;

    const char *txt = lv_buttonmatrix_get_button_text(obj, id);
    if (txt == NULL) return;

    /* Mode keys. They exist on the normal layouts only, except 1#, which the
     * big symbol pages keep as their page cycle. Every branch here rewrites
     * the map, which invalidates txt — so each one returns immediately. */
    if (strcmp(txt, "abc") == 0) {
        kb->mode = PUNK_KB_MODE_LOWER;
        punk_kb_update_map(obj);
        return;
    }
    if (strcmp(txt, "ABC") == 0) {
        kb->mode = PUNK_KB_MODE_UPPER;
        punk_kb_update_map(obj);
        return;
    }
    if (strcmp(txt, "1#") == 0) {
        if (s_big && kb->mode == PUNK_KB_MODE_SYM) kb->sym_page ^= 1;
        else                                       kb->mode = PUNK_KB_MODE_SYM;
        punk_kb_update_map(obj);
        return;
    }

    /* The size toggle. Mode and page are preserved across it. */
    if (strcmp(txt, LV_SYMBOL_KEYBOARD) == 0) {
        s_big = !s_big;
        punk_kb_update_map(obj);
        return;
    }

    if (strcmp(txt, LV_SYMBOL_OK) == 0) {
        /* The handler on the far side of this closes the OSK, deleting this
         * widget — the result check is what makes that safe. Nothing may
         * touch kb after a non-OK result. */
        lv_result_t res = lv_obj_send_event(obj, LV_EVENT_READY, NULL);
        if (res != LV_RESULT_OK) return;

        if (kb->ta) {
            res = lv_obj_send_event(kb->ta, LV_EVENT_READY, NULL);
            if (res != LV_RESULT_OK) return;
        }
        return;
    }

    if (kb->ta == NULL) return;

    if (strcmp(txt, LV_SYMBOL_NEW_LINE) == 0) {
        lv_textarea_add_char(kb->ta, '\n');
        if (lv_textarea_get_one_line(kb->ta)) {
            lv_obj_send_event(kb->ta, LV_EVENT_READY, NULL);
        }
    } else if (strcmp(txt, LV_SYMBOL_LEFT) == 0) {
        lv_textarea_cursor_left(kb->ta);
    } else if (strcmp(txt, LV_SYMBOL_RIGHT) == 0) {
        lv_textarea_cursor_right(kb->ta);
    } else if (strcmp(txt, LV_SYMBOL_BACKSPACE) == 0) {
        lv_textarea_delete_char(kb->ta);
    } else {
        lv_textarea_add_text(kb->ta, txt);
    }
}

/**********************
 *   LUA BINDING
 **********************/

static int l_punk_kb_create(lua_State *L)
{
    return luavgl_obj_create_helper(L, punk_keyboard_create);
}

static int l_punk_kb_set_textarea(lua_State *L)
{
    lv_obj_t *obj = luavgl_to_obj(L, 1);
    lv_obj_t *ta = lua_isnoneornil(L, 2) ? NULL : luavgl_to_obj(L, 2);
    /* The key handler calls lv_textarea_* on whatever it is given, and the
     * stock LV_ASSERT_OBJ compiles out in release. Raise a Lua error rather
     * than let a wrong argument reach the textarea API. */
    if (ta != NULL && !lv_obj_check_type(ta, &lv_textarea_class)) {
        return luaL_argerror(L, 2, "textarea expected");
    }
    punk_keyboard_set_textarea(obj, ta);
    lua_settop(L, 1);
    return 1;
}

static int l_punk_kb_set_mode(lua_State *L)
{
    lv_obj_t *obj = luavgl_to_obj(L, 1);
    punk_keyboard_set_mode(obj, (punk_keyboard_mode_t)luaL_checkinteger(L, 2));
    lua_settop(L, 1);
    return 1;
}

static int l_punk_kb_set_big(lua_State *L)
{
    lv_obj_t *obj = luavgl_to_obj(L, 1);
    punk_keyboard_set_big(obj, lua_toboolean(L, 2));
    lua_settop(L, 1);
    return 1;
}

static int l_punk_kb_get_big(lua_State *L)
{
    lua_pushboolean(L, punk_keyboard_get_big() ? 1 : 0);
    return 1;
}

static const luavgl_table_t punk_kb_property_table = {
    .len = 0,
    .array = NULL,
};

static const rotable_Reg punk_kb_methods[] = {
    {"__property",   LUA_TLIGHTUSERDATA, {.ptr = &punk_kb_property_table}},
    {"set_textarea", LUA_TFUNCTION,      {l_punk_kb_set_textarea}        },
    {"set_mode",     LUA_TFUNCTION,      {l_punk_kb_set_mode}            },
    {"set_big",      LUA_TFUNCTION,      {l_punk_kb_set_big}             },
    {"get_big",      LUA_TFUNCTION,      {l_punk_kb_get_big}             },

    {0,              0,                  {0}                             },
};

void punk_keyboard_lua_register(lua_State *L)
{
    /* The base class metatable (lv_btnm) is registered by luavgl's own
     * keyboard binding, so this needs LV_USE_KEYBOARD — which lv_conf.h has
     * on — and must run after luaopen_lvgl. */
    luavgl_obj_newmetatable(L, &punk_keyboard_class, "punk_kb", punk_kb_methods);
    lua_pop(L, 1);

    /* luavgl's widget-creation table is a plain Lua table whose __index is
     * itself, and every object metatable chains to it — so one field here
     * serves both parent:PunkKeyboard{} and lvgl.PunkKeyboard{}. */
    luaL_getmetatable(L, "widgets");
    lua_pushcfunction(L, l_punk_kb_create);
    lua_setfield(L, -2, "PunkKeyboard");
    lua_pop(L, 1);
}
