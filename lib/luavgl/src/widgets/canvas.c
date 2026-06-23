#include "luavgl.h"
#include "private.h"
#include "rotable.h"

static int32_t canvas_get_int_field(lua_State *L, int idx, const char *field,
                                    int32_t def)
{
  lua_getfield(L, idx, field);
  int32_t val = lua_isnil(L, -1) ? def : luavgl_tointeger(L, -1);
  lua_pop(L, 1);
  return val;
}

static lv_color_t canvas_get_color_field(lua_State *L, int idx,
                                         const char *field, lv_color_t def)
{
  lua_getfield(L, idx, field);
  lv_color_t val = lua_isnil(L, -1) ? def : luavgl_tocolor(L, -1);
  lua_pop(L, 1);
  return val;
}

static lv_point_precise_t canvas_get_point_field(lua_State *L, int idx,
                                                  const char *field)
{
  lv_point_precise_t p = {0, 0};
  lua_getfield(L, idx, field);
  if (lua_istable(L, -1)) {
    lua_getfield(L, -1, "x");
    if (!lua_isnil(L, -1))
      p.x = lua_tonumber(L, -1);
    lua_pop(L, 1);
    lua_getfield(L, -1, "y");
    if (!lua_isnil(L, -1))
      p.y = lua_tonumber(L, -1);
    lua_pop(L, 1);
  }
  lua_pop(L, 1);
  return p;
}

static void canvas_delete_cb(lv_event_t *e)
{
  lv_obj_t *canvas = lv_event_get_target(e);
  lv_draw_buf_t *draw_buf = lv_canvas_get_draw_buf(canvas);
  if (draw_buf != NULL) {
    lv_draw_buf_destroy(draw_buf);
  }
}

static int luavgl_canvas_create(lua_State *L)
{
  luavgl_ctx_t *ctx = luavgl_context(L);
  lv_obj_t *parent;

  int type = lua_type(L, 1);
  if (type == LUA_TTABLE) {
    parent = ctx->root;
  } else if (type == LUA_TNONE || type == LUA_TNIL) {
    parent = ctx->root;
    lua_remove(L, 1);
  } else {
    parent = luavgl_to_obj(L, 1);
    lua_remove(L, 1);
  }

  int32_t w = 0, h = 0;
  lv_color_format_t cf = LV_COLOR_FORMAT_RGB565;

  if (lua_istable(L, 1)) {
    w = canvas_get_int_field(L, 1, "w", 0);
    h = canvas_get_int_field(L, 1, "h", 0);
    cf = (lv_color_format_t)canvas_get_int_field(L, 1, "cf",
                                                  LV_COLOR_FORMAT_RGB565);
  }

  if (w <= 0 || h <= 0) {
    return luaL_error(L, "canvas requires w and h > 0");
  }

  lv_obj_t *obj = lv_canvas_create(parent);

  lv_draw_buf_t *draw_buf = lv_draw_buf_create(w, h, cf, 0);
  if (draw_buf == NULL) {
    lv_obj_del(obj);
    return luaL_error(L, "failed to allocate canvas buffer (%dx%d)", w, h);
  }
  lv_canvas_set_draw_buf(obj, draw_buf);

  luavgl_add_lobj(L, obj)->lua_created = true;
  lv_obj_add_event_cb(obj, canvas_delete_cb, LV_EVENT_DELETE, NULL);

  if (lua_istable(L, 1)) {
    luavgl_iterate(L, 1, luavgl_obj_set_property_kv, obj);
  }

  return 1;
}

static int luavgl_canvas_fill_bg(lua_State *L)
{
  lv_obj_t *obj = luavgl_to_obj(L, 1);
  lv_color_t color = luavgl_tocolor(L, 2);
  lv_opa_t opa = (lv_opa_t)luavgl_tointeger(L, 3);
  lv_canvas_fill_bg(obj, color, opa);
  return 0;
}

static int luavgl_canvas_set_px(lua_State *L)
{
  lv_obj_t *obj = luavgl_to_obj(L, 1);
  int32_t x = luavgl_tointeger(L, 2);
  int32_t y = luavgl_tointeger(L, 3);
  lv_color_t color = luavgl_tocolor(L, 4);
  lv_opa_t opa = (lv_opa_t)luavgl_tointeger(L, 5);
  lv_canvas_set_px(obj, x, y, color, opa);
  return 0;
}

static int luavgl_canvas_get_px(lua_State *L)
{
  lv_obj_t *obj = luavgl_to_obj(L, 1);
  int32_t x = luavgl_tointeger(L, 2);
  int32_t y = luavgl_tointeger(L, 3);
  lv_color32_t c = lv_canvas_get_px(obj, x, y);
  lua_createtable(L, 0, 4);
  lua_pushinteger(L, c.red);
  lua_setfield(L, -2, "r");
  lua_pushinteger(L, c.green);
  lua_setfield(L, -2, "g");
  lua_pushinteger(L, c.blue);
  lua_setfield(L, -2, "b");
  lua_pushinteger(L, c.alpha);
  lua_setfield(L, -2, "a");
  return 1;
}

static int luavgl_canvas_set_palette(lua_State *L)
{
  lv_obj_t *obj = luavgl_to_obj(L, 1);
  uint8_t index = (uint8_t)luavgl_tointeger(L, 2);
  uint32_t c = (uint32_t)lua_tointeger(L, 3);
  lv_color32_t color32;
  color32.alpha = (c >> 24) & 0xFF;
  color32.red   = (c >> 16) & 0xFF;
  color32.green = (c >> 8) & 0xFF;
  color32.blue  = c & 0xFF;
  lv_canvas_set_palette(obj, index, color32);
  return 0;
}

static int luavgl_canvas_draw_rect(lua_State *L)
{
  lv_obj_t *obj = luavgl_to_obj(L, 1);
  luaL_checktype(L, 2, LUA_TTABLE);

  lv_draw_rect_dsc_t dsc;
  lv_draw_rect_dsc_init(&dsc);

  dsc.radius         = canvas_get_int_field(L, 2, "radius", 0);
  dsc.bg_opa         = (lv_opa_t)canvas_get_int_field(L, 2, "bg_opa",
                                                       LV_OPA_COVER);
  dsc.bg_color       = canvas_get_color_field(L, 2, "bg_color",
                                               lv_color_black());
  dsc.border_color   = canvas_get_color_field(L, 2, "border_color",
                                               lv_color_black());
  dsc.border_width   = canvas_get_int_field(L, 2, "border_width", 0);
  dsc.border_opa     = (lv_opa_t)canvas_get_int_field(L, 2, "border_opa",
                                                       LV_OPA_COVER);
  dsc.shadow_color   = canvas_get_color_field(L, 2, "shadow_color",
                                               lv_color_black());
  dsc.shadow_width   = canvas_get_int_field(L, 2, "shadow_width", 0);
  dsc.shadow_opa     = (lv_opa_t)canvas_get_int_field(L, 2, "shadow_opa",
                                                       LV_OPA_COVER);
  dsc.shadow_offset_x = canvas_get_int_field(L, 2, "shadow_offset_x", 0);
  dsc.shadow_offset_y = canvas_get_int_field(L, 2, "shadow_offset_y", 0);
  dsc.shadow_spread  = canvas_get_int_field(L, 2, "shadow_spread", 0);
  dsc.outline_color  = canvas_get_color_field(L, 2, "outline_color",
                                               lv_color_black());
  dsc.outline_width  = canvas_get_int_field(L, 2, "outline_width", 0);
  dsc.outline_pad    = canvas_get_int_field(L, 2, "outline_pad", 0);
  dsc.outline_opa    = (lv_opa_t)canvas_get_int_field(L, 2, "outline_opa",
                                                       LV_OPA_COVER);

  lv_area_t coords;
  coords.x1 = canvas_get_int_field(L, 2, "x1", 0);
  coords.y1 = canvas_get_int_field(L, 2, "y1", 0);
  coords.x2 = canvas_get_int_field(L, 2, "x2", 0);
  coords.y2 = canvas_get_int_field(L, 2, "y2", 0);

  lv_layer_t layer;
  lv_canvas_init_layer(obj, &layer);
  lv_draw_rect(&layer, &dsc, &coords);
  lv_canvas_finish_layer(obj, &layer);

  return 0;
}

static int luavgl_canvas_draw_line(lua_State *L)
{
  lv_obj_t *obj = luavgl_to_obj(L, 1);
  luaL_checktype(L, 2, LUA_TTABLE);

  lv_draw_line_dsc_t dsc;
  lv_draw_line_dsc_init(&dsc);

  dsc.p1          = canvas_get_point_field(L, 2, "p1");
  dsc.p2          = canvas_get_point_field(L, 2, "p2");
  dsc.color       = canvas_get_color_field(L, 2, "color", lv_color_white());
  dsc.width       = canvas_get_int_field(L, 2, "width", 1);
  dsc.opa         = (lv_opa_t)canvas_get_int_field(L, 2, "opa", LV_OPA_COVER);
  dsc.dash_width  = canvas_get_int_field(L, 2, "dash_width", 0);
  dsc.dash_gap    = canvas_get_int_field(L, 2, "dash_gap", 0);
  dsc.round_start = canvas_get_int_field(L, 2, "round_start", 0);
  dsc.round_end   = canvas_get_int_field(L, 2, "round_end", 0);

  lv_layer_t layer;
  lv_canvas_init_layer(obj, &layer);
  lv_draw_line(&layer, &dsc);
  lv_canvas_finish_layer(obj, &layer);

  return 0;
}

static int luavgl_canvas_draw_arc(lua_State *L)
{
  lv_obj_t *obj = luavgl_to_obj(L, 1);
  luaL_checktype(L, 2, LUA_TTABLE);

  lv_draw_arc_dsc_t dsc;
  lv_draw_arc_dsc_init(&dsc);

  lua_getfield(L, 2, "center");
  if (lua_istable(L, -1)) {
    lua_getfield(L, -1, "x");
    if (!lua_isnil(L, -1))
      dsc.center.x = lua_tointeger(L, -1);
    lua_pop(L, 1);
    lua_getfield(L, -1, "y");
    if (!lua_isnil(L, -1))
      dsc.center.y = lua_tointeger(L, -1);
    lua_pop(L, 1);
  }
  lua_pop(L, 1);

  dsc.radius = (uint16_t)canvas_get_int_field(L, 2, "radius", 0);

  lua_getfield(L, 2, "start_angle");
  dsc.start_angle = lua_isnil(L, -1) ? 0 : lua_tonumber(L, -1);
  lua_pop(L, 1);

  lua_getfield(L, 2, "end_angle");
  dsc.end_angle = lua_isnil(L, -1) ? 360 : lua_tonumber(L, -1);
  lua_pop(L, 1);

  dsc.color   = canvas_get_color_field(L, 2, "color", lv_color_white());
  dsc.width   = canvas_get_int_field(L, 2, "width", 1);
  dsc.opa     = (lv_opa_t)canvas_get_int_field(L, 2, "opa", LV_OPA_COVER);
  dsc.rounded = canvas_get_int_field(L, 2, "rounded", 0);

  lv_layer_t layer;
  lv_canvas_init_layer(obj, &layer);
  lv_draw_arc(&layer, &dsc);
  lv_canvas_finish_layer(obj, &layer);

  return 0;
}

static int luavgl_canvas_draw_label(lua_State *L)
{
  lv_obj_t *obj = luavgl_to_obj(L, 1);
  luaL_checktype(L, 2, LUA_TTABLE);

  lv_draw_label_dsc_t dsc;
  lv_draw_label_dsc_init(&dsc);

  lua_getfield(L, 2, "text");
  dsc.text = lua_isnil(L, -1) ? "" : lua_tostring(L, -1);

  dsc.color        = canvas_get_color_field(L, 2, "color", lv_color_white());
  dsc.opa          = (lv_opa_t)canvas_get_int_field(L, 2, "opa", LV_OPA_COVER);
  dsc.line_space   = canvas_get_int_field(L, 2, "line_space", 0);
  dsc.letter_space = canvas_get_int_field(L, 2, "letter_space", 0);

  lua_getfield(L, 2, "font");
  if (lua_islightuserdata(L, -1)) {
    dsc.font = (const lv_font_t *)lua_touserdata(L, -1);
  }
  lua_pop(L, 1);

  lv_area_t coords;
  coords.x1 = canvas_get_int_field(L, 2, "x1", 0);
  coords.y1 = canvas_get_int_field(L, 2, "y1", 0);
  coords.x2 = canvas_get_int_field(L, 2, "x2", 0);
  coords.y2 = canvas_get_int_field(L, 2, "y2", 0);

  lv_layer_t layer;
  lv_canvas_init_layer(obj, &layer);
  lv_draw_label(&layer, &dsc, &coords);
  lv_canvas_finish_layer(obj, &layer);

  lua_pop(L, 1); /* pop the text string */

  return 0;
}

static int luavgl_canvas_draw_triangle(lua_State *L)
{
  lv_obj_t *obj = luavgl_to_obj(L, 1);
  luaL_checktype(L, 2, LUA_TTABLE);

  lv_draw_triangle_dsc_t dsc;
  lv_draw_triangle_dsc_init(&dsc);

  dsc.p[0]     = canvas_get_point_field(L, 2, "p1");
  dsc.p[1]     = canvas_get_point_field(L, 2, "p2");
  dsc.p[2]     = canvas_get_point_field(L, 2, "p3");
  dsc.bg_color = canvas_get_color_field(L, 2, "bg_color", lv_color_white());
  dsc.bg_opa   = (lv_opa_t)canvas_get_int_field(L, 2, "bg_opa", LV_OPA_COVER);

  lv_layer_t layer;
  lv_canvas_init_layer(obj, &layer);
  lv_draw_triangle(&layer, &dsc);
  lv_canvas_finish_layer(obj, &layer);

  return 0;
}

static int luavgl_canvas_draw_image(lua_State *L)
{
  lv_obj_t *obj = luavgl_to_obj(L, 1);
  luaL_checktype(L, 2, LUA_TTABLE);

  lv_draw_image_dsc_t dsc;
  lv_draw_image_dsc_init(&dsc);

  lua_getfield(L, 2, "src");
  dsc.src = luavgl_toimgsrc(L, -1);

  dsc.opa      = (lv_opa_t)canvas_get_int_field(L, 2, "opa", LV_OPA_COVER);
  dsc.rotation = canvas_get_int_field(L, 2, "rotation", 0);
  dsc.scale_x  = canvas_get_int_field(L, 2, "scale_x", 256);
  dsc.scale_y  = canvas_get_int_field(L, 2, "scale_y", 256);
  /* antialias: pass 0 for a crisp/blocky (nearest-neighbour) scale, 1 for the
     smooth (bilinear) default. Absent -> keep LVGL's default. */
  dsc.antialias = canvas_get_int_field(L, 2, "antialias", dsc.antialias) ? 1 : 0;

  lua_getfield(L, 2, "pivot");
  if (lua_istable(L, -1)) {
    lua_getfield(L, -1, "x");
    if (!lua_isnil(L, -1))
      dsc.pivot.x = lua_tointeger(L, -1);
    lua_pop(L, 1);
    lua_getfield(L, -1, "y");
    if (!lua_isnil(L, -1))
      dsc.pivot.y = lua_tointeger(L, -1);
    lua_pop(L, 1);
  }
  lua_pop(L, 1);

  lv_area_t coords;
  coords.x1 = canvas_get_int_field(L, 2, "x1", 0);
  coords.y1 = canvas_get_int_field(L, 2, "y1", 0);
  coords.x2 = canvas_get_int_field(L, 2, "x2", 0);
  coords.y2 = canvas_get_int_field(L, 2, "y2", 0);

  lv_layer_t layer;
  lv_canvas_init_layer(obj, &layer);
  lv_draw_image(&layer, &dsc, &coords);
  lv_canvas_finish_layer(obj, &layer);

  lua_pop(L, 1); /* pop the src string */

  return 0;
}

/*
 * Return the canvas' image descriptor as a light userdata pointer.
 *
 * The returned value can be passed straight back as the `src` of another
 * canvas' draw_image{}, which makes it possible to pre-render ("bake") an
 * object into an off-screen canvas once and then blit it -- with rotation,
 * scale and a pivot -- every frame, instead of re-issuing its primitives.
 *
 * The pointer stays valid for the canvas' lifetime and always reflects the
 * canvas' current pixels, so keep the source canvas object alive while in use.
 *
 * local sprite = lvgl.Canvas{ w = 48, h = 48, cf = lvgl.COLOR_FORMAT.ARGB8888 }
 * sprite:fill_bg("#000000", 0)        -- transparent
 * sprite:draw_rect{ ... }             -- bake the texture once
 * local src = sprite:get_image()
 * scene:draw_image{ src = src, rotation = 450, pivot = { x = 24, y = 24 },
 *                   x1 = px, y1 = py, x2 = px + 47, y2 = py + 47 }
 */
static int luavgl_canvas_get_image(lua_State *L)
{
  lv_obj_t *obj = luavgl_to_obj(L, 1);
  lv_image_dsc_t *dsc = lv_canvas_get_image(obj);
  if (dsc == NULL) {
    lua_pushnil(L);
  } else {
    lua_pushlightuserdata(L, dsc);
  }
  return 1;
}

static const rotable_Reg luavgl_canvas_methods[] = {
    {"fill_bg",       LUA_TFUNCTION, {luavgl_canvas_fill_bg}      },
    {"set_px",        LUA_TFUNCTION, {luavgl_canvas_set_px}       },
    {"get_px",        LUA_TFUNCTION, {luavgl_canvas_get_px}       },
    {"set_palette",   LUA_TFUNCTION, {luavgl_canvas_set_palette}  },
    {"draw_rect",     LUA_TFUNCTION, {luavgl_canvas_draw_rect}    },
    {"draw_line",     LUA_TFUNCTION, {luavgl_canvas_draw_line}    },
    {"draw_arc",      LUA_TFUNCTION, {luavgl_canvas_draw_arc}     },
    {"draw_label",    LUA_TFUNCTION, {luavgl_canvas_draw_label}   },
    {"draw_triangle", LUA_TFUNCTION, {luavgl_canvas_draw_triangle}},
    {"draw_image",    LUA_TFUNCTION, {luavgl_canvas_draw_image}   },
    {"get_image",     LUA_TFUNCTION, {luavgl_canvas_get_image}    },
    {0,               0,             {0}                          },
};

static void luavgl_canvas_init(lua_State *L)
{
  luavgl_obj_newmetatable(L, &lv_canvas_class, "lv_canvas",
                          luavgl_canvas_methods);
  lua_pop(L, 1);
}
