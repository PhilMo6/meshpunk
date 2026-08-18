#include "luavgl.h"
#include "private.h"
#include "rotable.h"

static int luavgl_keyboard_create(lua_State *L)
{
  return luavgl_obj_create_helper(L, lv_keyboard_create);
}

/* MESHPUNK: upstream binds only create; the on-screen keyboard (lib/osk.lua)
 * needs the textarea pairing and mode switching. */
static int luavgl_keyboard_set_textarea(lua_State *L)
{
  lv_obj_t *obj = luavgl_to_obj(L, 1);
  lv_obj_t *ta = lua_isnoneornil(L, 2) ? NULL : luavgl_to_obj(L, 2);
  lv_keyboard_set_textarea(obj, ta);
  lua_settop(L, 1);
  return 1;
}

static int luavgl_keyboard_set_mode(lua_State *L)
{
  lv_obj_t *obj = luavgl_to_obj(L, 1);
  lv_keyboard_set_mode(obj, (lv_keyboard_mode_t)luaL_checkinteger(L, 2));
  lua_settop(L, 1);
  return 1;
}

static const luavgl_table_t keyboard_property_table = {
    .len = 0,
    .array = NULL,
};

static const rotable_Reg luavgl_keyboard_methods[] = {
    {"__property",   LUA_TLIGHTUSERDATA, {.ptr = &keyboard_property_table}},
    /* MESHPUNK additions */
    {"set_textarea", LUA_TFUNCTION,      {luavgl_keyboard_set_textarea}   },
    {"set_mode",     LUA_TFUNCTION,      {luavgl_keyboard_set_mode}       },

    {0,              0,                  {0}                              },
};

static void luavgl_keyboard_init(lua_State *L)
{
  static const rotable_Reg btm_methods[] = {
      {0, 0, {0}},
  };
  luavgl_obj_newmetatable(L, &lv_buttonmatrix_class, "lv_btnm", btm_methods);
  lua_pop(L, 1);

  luavgl_obj_newmetatable(L, &lv_keyboard_class, "lv_keyboard",
                          luavgl_keyboard_methods);
  lua_pop(L, 1);
}
