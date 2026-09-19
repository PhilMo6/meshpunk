/* MESHPUNK addition (not upstream luavgl): lv_spangroup binding for inline
 * mixed-style flowing text — paragraphs whose runs differ in color, font or
 * decoration (the Web app's link rendering).
 *
 *   local sg = parent:Spangroup { w = 300 }   -- base obj properties only
 *   sg:set_mode("fixed"|"expand"|"break")     -- LV_SPAN_MODE_*
 *   sg:add_span(text[, tbl]) -> index         -- appends a span, 1-based index
 *   sg:set_span(index, tbl)                   -- restyle / retext one span
 *   sg:span_count() -> n
 *
 * tbl keys: text (string), text_color (any luavgl color), text_font
 * (lvgl.Font userdata), text_opa (0-255), underline, strike (booleans; the
 * decoration is rewritten only when at least one of the two keys is present,
 * so a table without them keeps the span's current decoration).
 *
 * Spans have no Lua userdata: they are addressed by index. Indices are
 * stable because the binding exposes no single-span delete — a span lives
 * exactly as long as its spangroup. A span with no style key falls back to
 * the spangroup object's own style (theme font/color) like label text.
 */
#include "luavgl.h"
#include "private.h"
#include "rotable.h"

static int luavgl_spangroup_create(lua_State *L)
{
  return luavgl_obj_create_helper(L, lv_spangroup_create);
}

static int luavgl_spangroup_set_mode(lua_State *L)
{
  lv_obj_t *obj = luavgl_to_obj(L, 1);
  const char *mode = luaL_checkstring(L, 2);

  if (lv_strcmp(mode, "break") == 0) {
    lv_spangroup_set_mode(obj, LV_SPAN_MODE_BREAK);
  } else if (lv_strcmp(mode, "expand") == 0) {
    lv_spangroup_set_mode(obj, LV_SPAN_MODE_EXPAND);
  } else if (lv_strcmp(mode, "fixed") == 0) {
    lv_spangroup_set_mode(obj, LV_SPAN_MODE_FIXED);
  } else {
    return luaL_error(L, "unknown spangroup mode: %s", mode);
  }

  return 0;
}

/* Apply text + style keys from the table at tidx (absolute index) to span. */
static void luavgl_span_apply_table(lua_State *L, int tidx, lv_span_t *span)
{
  lv_style_t *style = lv_span_get_style(span);

  lua_getfield(L, tidx, "text");
  if (!lua_isnil(L, -1)) {
    if (!lua_isstring(L, -1)) {
      luaL_error(L, "span text must be a string");
      return;
    }
    lv_span_set_text(span, lua_tostring(L, -1));
  }
  lua_pop(L, 1);

  lua_getfield(L, tidx, "text_color");
  if (!lua_isnil(L, -1)) {
    lv_style_set_text_color(style, luavgl_tocolor(L, -1));
  }
  lua_pop(L, 1);

  lua_getfield(L, tidx, "text_font");
  if (!lua_isnil(L, -1)) {
    /* Same conversion style.c applies for text_font: the userdata address
     * IS the lv_font_t (see g_style_map, STYLE_TYPE_POINTER). */
    lv_style_set_text_font(style, lua_touserdata(L, -1));
  }
  lua_pop(L, 1);

  lua_getfield(L, tidx, "text_opa");
  if (!lua_isnil(L, -1)) {
    lv_style_set_text_opa(style, luavgl_tointeger(L, -1));
  }
  lua_pop(L, 1);

  lua_getfield(L, tidx, "underline");
  int has_underline = !lua_isnil(L, -1);
  int underline = lua_toboolean(L, -1);
  lua_pop(L, 1);

  lua_getfield(L, tidx, "strike");
  int has_strike = !lua_isnil(L, -1);
  int strike = lua_toboolean(L, -1);
  lua_pop(L, 1);

  if (has_underline || has_strike) {
    lv_text_decor_t decor = LV_TEXT_DECOR_NONE;
    if (underline) decor |= LV_TEXT_DECOR_UNDERLINE;
    if (strike) decor |= LV_TEXT_DECOR_STRIKETHROUGH;
    lv_style_set_text_decor(style, decor);
  }
}

static int luavgl_spangroup_add_span(lua_State *L)
{
  lv_obj_t *obj = luavgl_to_obj(L, 1);
  const char *text = luaL_checkstring(L, 2);

  lv_span_t *span = lv_spangroup_new_span(obj);
  if (span == NULL) {
    return luaL_error(L, "spangroup: new_span failed");
  }
  lv_span_set_text(span, text);

  if (lua_istable(L, 3)) {
    luavgl_span_apply_table(L, 3, span);
    /* Style writes bypass the widget; set_text only refreshes sizes. */
    lv_spangroup_refr_mode(obj);
  }

  lua_pushinteger(L, (lua_Integer)lv_spangroup_get_span_count(obj));
  return 1;
}

static int luavgl_spangroup_set_span(lua_State *L)
{
  lv_obj_t *obj = luavgl_to_obj(L, 1);
  int idx = luavgl_tointeger(L, 2);
  luaL_checktype(L, 3, LUA_TTABLE);

  /* Reject idx < 1 here: lv_spangroup_get_child treats negative ids as
   * from-the-end indexing, which would silently hit the wrong span. */
  if (idx < 1) {
    return luaL_error(L, "spangroup: bad span index %d", idx);
  }
  lv_span_t *span = lv_spangroup_get_child(obj, idx - 1);
  if (span == NULL) {
    return luaL_error(L, "spangroup: no span %d", idx);
  }

  luavgl_span_apply_table(L, 3, span);
  lv_spangroup_refr_mode(obj);
  return 0;
}

static int luavgl_spangroup_span_count(lua_State *L)
{
  lv_obj_t *obj = luavgl_to_obj(L, 1);
  lua_pushinteger(L, (lua_Integer)lv_spangroup_get_span_count(obj));
  return 1;
}

static const rotable_Reg luavgl_spangroup_methods[] = {
    {"set_mode",   LUA_TFUNCTION, {luavgl_spangroup_set_mode}  },
    {"add_span",   LUA_TFUNCTION, {luavgl_spangroup_add_span}  },
    {"set_span",   LUA_TFUNCTION, {luavgl_spangroup_set_span}  },
    {"span_count", LUA_TFUNCTION, {luavgl_spangroup_span_count}},

    {0,            0,             {0}                          },
};

static void luavgl_spangroup_init(lua_State *L)
{
  luavgl_obj_newmetatable(L, &lv_spangroup_class, "lv_spangroup",
                          luavgl_spangroup_methods);
  lua_pop(L, 1);
}
