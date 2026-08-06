// Lua engine layer for the jet3d module: binds Jet's retained-mode scene graph
// to a private Lua interpreter.
//
// Binding shape: Lua never enters the per-triangle or per-pixel path. Game code
// mutates scene state (object transforms, camera, materials) and the C++ side
// runs the whole render pipeline once per frame. There is one Lua->C boundary
// crossing per frame plus one per scene mutation, never one per primitive.
//
// Ownership: Renderer::Scene does not own the objects added to it (its
// destructor frees only the rasteriser and PostFX). Every Object, Material and
// Texture created through this API is recorded in the registries below and
// freed by jet_lua_close(). Lua userdata holds non-owning pointers, so no
// __gc metamethod can double-free one.

#include "jet_lua.h"
#include "jet_overlay.h"
#include "jet_audio.h"
#include "jet_mesh.h"
#include "jet_terrain.h"

#include "Jet.hpp"
#include "Primitives.hpp"
#include "Texture.hpp"
#include "Sprite2D.hpp"
#include "Picking.hpp"

extern "C" {
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
}

#include <vector>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <math.h>

extern "C" {
    void host_log(const char* msg);
    uint32_t host_get_ticks_ms(void);
}

// Diagnostic in main_tdeck.cpp: captures one finished scanline out of the band
// buffer before it is blitted and prints it as channel runs.
void jet_request_row_dump(int y);

// ---------------------------------------------------------------------------
// Lua console output -> host_log (see lua_out.h)
// ---------------------------------------------------------------------------
// print() arrives in pieces, so accumulate and emit whole lines.
static char   s_outBuf[192];
static size_t s_outLen = 0;

static void outFlush(void) {
    if (s_outLen == 0) return;
    s_outBuf[s_outLen] = '\0';
    host_log(s_outBuf);
    s_outLen = 0;
}

extern "C" void jet_lua_write(const char* s, size_t len) {
    if (!s) return;
    for (size_t i = 0; i < len; ++i) {
        char c = s[i];
        if (c == '\n') { outFlush(); continue; }
        if (c == '\r') continue;
        if (s_outLen + 1 >= sizeof(s_outBuf)) outFlush();
        s_outBuf[s_outLen++] = c;
    }
}

extern "C" void jet_lua_write_err(const char* fmt, const char* arg) {
    char tmp[192];
    snprintf(tmp, sizeof(tmp), fmt ? fmt : "%s", arg ? arg : "");
    jet_lua_write(tmp, strlen(tmp));
    outFlush();
}

using namespace Renderer;

// ---------------------------------------------------------------------------
// Engine state
// ---------------------------------------------------------------------------

static lua_State* L        = nullptr;
static Scene*     g_scene  = nullptr;
static int        g_screenW = 0, g_screenH = 0;
static bool       g_quit   = false;

static Camera*           g_camera   = nullptr;
static DirectionalLight* g_dirLight = nullptr;
static AmbientLight*     g_ambLight = nullptr;

// Freed as a group by jet_lua_close().
static std::vector<Object*>   g_objects;
static std::vector<Material*> g_materials;
static std::vector<Texture*>  g_textures;
static std::vector<uint16_t*> g_texturePixels;
static std::vector<Sprite2D*> g_sprites;

// Palette animations serviced once per frame. dt is accumulated and consumed in
// whole steps here rather than through Texture::advancePalette, whose per-call
// rounding turns any per-frame dt into >= 1 step and so runs every animation at
// the frame rate instead of the requested one.
struct TexAnim { Texture* tex; float fps; float acc; };
static std::vector<TexAnim> g_texAnims;

// Pick slots, armed from Lua and applied to the scene each frame.
// (Compiled out at MAX_PICK_QUERIES 0; the jet.pick bindings become no-ops.)
#if MAX_PICK_QUERIES > 0
static PickQuery g_pickQ[MAX_PICK_QUERIES];
static bool      g_pickDirty = false;
#endif

// Input state. Indexed by host key code; host_get_key() reports ASCII for
// printable keys and the module extension codes for the rest.
static uint8_t g_keyDown[256];
// Edge latches, set by the key feed and cleared after each frame's callbacks.
// Latching rather than comparing against last frame's level matters because a
// key can be pressed AND released inside one frame at 20fps — a prev-state
// compare would miss that tap entirely.
static uint8_t g_keyHit[256];
static uint8_t g_keyRel[256];
static int     g_trkDx = 0, g_trkDy = 0, g_trkClick = 0;
static float   g_fps = 0.0f, g_uptime = 0.0f, g_lastDt = 0.0f;

#define MT_OBJECT   "jet.object"
#define MT_MATERIAL "jet.material"
#define MT_TEXTURE  "jet.texture"
#define MT_BUILDER  "jet.builder"
#define MT_CULLGROUP "jet.cullgroup"

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static void logf_(const char* fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    host_log(buf);
}

// lroundf is not among the host exports; round explicitly.
static inline int32_t toFixed(lua_Number v) {
    return (int32_t)(v >= 0 ? (v + 0.5f) : (v - 0.5f));
}

static inline int32_t argInt(lua_State* Ls, int idx) {
    return toFixed(luaL_checknumber(Ls, idx));
}

static inline int32_t optInt(lua_State* Ls, int idx, int32_t def) {
    if (lua_isnoneornil(Ls, idx)) return def;
    return toFixed(luaL_checknumber(Ls, idx));
}

// World-space quantities (positions, primitive dimensions, clip planes) are
// scaled by JET32_WORLD_SCALE on the way into Jet and unscaled on the way out,
// so Lua authors in whatever units it likes while the renderer's integer
// transform chain gets the extra bits.
//
// This matters beyond edge wobble. Primitives derive vertex NORMALS from the
// integer vertex positions — createSphere computes `normal = position * 1024 /
// radius` — so a small mesh quantises its normals coarsely. Measured on the
// unscaled demo sphere (radius 70): normal lengths ranged 999..1024 against an
// expected 1024, giving a worst-case per-vertex diffuse error of 9/255, about
// one 5-bit colour step. At 8x that drops to 2/255. FLAT shading hides this
// (one normal per face); GOURAUD interpolates between per-vertex values and
// shows it.
//
// Angles, scale multipliers and segment counts are NOT world-space and are
// passed through untouched.
//
// Range note: createSphere evaluates `radius * sin * cos` in int32 before
// dividing down, which overflows past a scaled radius of ~2047 — with the
// scale below that caps an authored sphere radius at roughly 255 units.
static inline int32_t argWorld(lua_State* Ls, int idx) {
    return toFixed(luaL_checknumber(Ls, idx) * JET32_WORLD_SCALE);
}

static inline lua_Number toWorldOut(int32_t v) {
    return (lua_Number)v / (lua_Number)JET32_WORLD_SCALE;
}

// Optional named field from an options table at `idx`.
static lua_Number optField(lua_State* Ls, int idx, const char* k, lua_Number def) {
    if (!lua_istable(Ls, idx)) return def;
    lua_getfield(Ls, idx, k);
    const lua_Number v = lua_isnil(Ls, -1) ? def : (lua_Number)luaL_checknumber(Ls, -1);
    lua_pop(Ls, 1);
    return v;
}

static int optFieldBool(lua_State* Ls, int idx, const char* k, int def) {
    if (!lua_istable(Ls, idx)) return def;
    lua_getfield(Ls, idx, k);
    const int v = lua_isnil(Ls, -1) ? def : (lua_toboolean(Ls, -1) != 0);
    lua_pop(Ls, 1);
    return v;
}

static Object* checkObject(lua_State* Ls, int idx) {
    Object** ud = (Object**)luaL_checkudata(Ls, idx, MT_OBJECT);
    // obj:destroy() nulls the handle, so a stale reference raises a Lua error
    // instead of dereferencing freed memory.
    if (!*ud) luaL_error(Ls, "object has been destroyed");
    return *ud;
}

static Material* checkMaterial(lua_State* Ls, int idx) {
    Material** ud = (Material**)luaL_checkudata(Ls, idx, MT_MATERIAL);
    return *ud;
}

// Material argument that also accepts a plain RGB565 number, creating an
// owned FLAT material for it. Keeps primitive calls terse in game code.
//
// Those implicit materials are cached by colour and shared. No handle to one is
// ever returned to Lua — the primitive constructors hand back the Object, not
// the Material — so nothing can mutate one and surprise another user of the
// same colour. Without the cache a game creating objects in a loop allocated a
// fresh Material per call and held every one of them until jet_lua_close.
struct ImplicitMaterial { uint16_t color; Material* mat; };
static std::vector<ImplicitMaterial> g_implicitMats;

static Material* argMaterial(lua_State* Ls, int idx) {
    if (lua_isnumber(Ls, idx)) {
        const uint16_t c = (uint16_t)lua_tointeger(Ls, idx);
        for (size_t i = 0; i < g_implicitMats.size(); ++i) {
            if (g_implicitMats[i].color == c) return g_implicitMats[i].mat;
        }
        Material* m = new Material(c);
        g_materials.push_back(m);          // freed with everything else at close
        g_implicitMats.push_back({ c, m });
        return m;
    }
    return checkMaterial(Ls, idx);
}

static void pushObject(lua_State* Ls, Object* o) {
    Object** ud = (Object**)lua_newuserdatauv(Ls, sizeof(Object*), 0);
    *ud = o;
    luaL_getmetatable(Ls, MT_OBJECT);
    lua_setmetatable(Ls, -2);
}

static void pushMaterial(lua_State* Ls, Material* m) {
    Material** ud = (Material**)lua_newuserdatauv(Ls, sizeof(Material*), 0);
    *ud = m;
    luaL_getmetatable(Ls, MT_MATERIAL);
    lua_setmetatable(Ls, -2);
}

static void pushTexture(lua_State* Ls, Texture* t) {
    Texture** ud = (Texture**)lua_newuserdatauv(Ls, sizeof(Texture*), 0);
    *ud = t;
    luaL_getmetatable(Ls, MT_TEXTURE);
    lua_setmetatable(Ls, -2);
}

// Register a freshly created primitive and hand it to the scene.
static int finishObject(lua_State* Ls, Object* o) {
    if (!o) return luaL_error(Ls, "primitive allocation failed");
    g_objects.push_back(o);
    g_scene->addObject(o);
    pushObject(Ls, o);
    return 1;
}

// ---------------------------------------------------------------------------
// jet.rgb / jet.log / jet.quit
// ---------------------------------------------------------------------------

static int l_rgb(lua_State* Ls) {
    int r = (int)luaL_checkinteger(Ls, 1);
    int g = (int)luaL_checkinteger(Ls, 2);
    int b = (int)luaL_checkinteger(Ls, 3);
    if (r < 0) r = 0; if (r > 255) r = 255;
    if (g < 0) g = 0; if (g > 255) g = 255;
    if (b < 0) b = 0; if (b > 255) b = 255;
    uint16_t c = (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
    lua_pushinteger(Ls, c);
    return 1;
}

static int l_log(lua_State* Ls) {
    host_log(luaL_checkstring(Ls, 1));
    return 0;
}

// jet.mem() -> liveBytes, liveBlocks, psramLargestFree, unsizedFrees
//
// liveBytes/liveBlocks come from the module's own operator new/delete
// (cxxstubs.cpp), so they measure exactly what THIS module has outstanding —
// which is the only way to answer "did destroying the world give the bytes
// back?". The heap's own largest-free-block barely moves when thousands of
// small scattered blocks are released, so it cannot answer that.
//
// unsizedFrees is the validity check on liveBytes: 0 means the byte figure is
// exact (see cxxstubs.cpp). Anything else and liveBlocks is the reliable one.
extern "C" void jet_mem_stats(uint32_t*, uint32_t*, uint32_t*);
extern "C" uint32_t host_psram_largest_free(void);

static int l_mem(lua_State* Ls) {
    uint32_t bytes = 0, blocks = 0, unsized = 0;
    jet_mem_stats(&bytes, &blocks, &unsized);
    lua_pushinteger(Ls, (lua_Integer)bytes);
    lua_pushinteger(Ls, (lua_Integer)blocks);
    lua_pushinteger(Ls, (lua_Integer)host_psram_largest_free());
    lua_pushinteger(Ls, (lua_Integer)unsized);
    return 4;
}

// Mirror of Scene.cpp's static sceneLambertDiffuse, against the current light
// with the identity object rotation. Kept in step with that function by hand;
// it exists so the probe can report the brightness values the GOURAUD path is
// fed without exposing Scene's private per-frame scratch.
static uint16_t mirrorLambert(const Vector3& N) {
    if (!g_dirLight) return 0;
    const Vector3& L = g_dirLight->worldLightDir;
    long long lit = (long long)N.x * L.x + (long long)N.y * L.y + (long long)N.z * L.z;
    if (lit <= 0) return 0;
    uint32_t lambert = (uint32_t)(lit >> 12);
    if (lambert > 255) lambert = 255;
    lambert = (lambert * lambert + 128) >> 8;
    uint16_t intensity = g_dirLight->intensity > 255 ? 255 : g_dirLight->intensity;
    lambert = (lambert * intensity) >> 8;
    uint32_t diffuseTerm = (lambert * 255u) >> 8;
    if (diffuseTerm > 255) diffuseTerm = 255;
    return (uint16_t)diffuseTerm;
}

// ---------------------------------------------------------------------------
// 2D overlay: jet.text / jet.rect / jet.textwidth
// ---------------------------------------------------------------------------
// Coordinates are SCREEN PIXELS with (0,0) top-left, so they are NOT world-
// scaled. Calls are recorded into a display list and replayed per band by the
// render loop, which is why they belong in jet.draw() rather than jet.update():
// the list is cleared once per frame before the callbacks run.

static int l_text(lua_State* Ls) {
    const int x = (int)luaL_checkinteger(Ls, 1);
    const int y = (int)luaL_checkinteger(Ls, 2);
    const char* s = luaL_checkstring(Ls, 3);
    const uint16_t color = lua_isnoneornil(Ls, 4)
                         ? (uint16_t)0xFFFF : (uint16_t)luaL_checkinteger(Ls, 4);
    const int scale = (int)luaL_optinteger(Ls, 5, 1);
    jet_ovl_text(x, y, color, scale, s);
    return 0;
}

static int l_rect(lua_State* Ls) {
    jet_ovl_rect((int)luaL_checkinteger(Ls, 1), (int)luaL_checkinteger(Ls, 2),
                 (int)luaL_checkinteger(Ls, 3), (int)luaL_checkinteger(Ls, 4),
                 (uint16_t)luaL_checkinteger(Ls, 5));
    return 0;
}

static int l_textwidth(lua_State* Ls) {
    const char* s = luaL_checkstring(Ls, 1);
    lua_pushinteger(Ls, jet_ovl_text_width((int)luaL_optinteger(Ls, 2, 1), s));
    return 1;
}

// jet.dumprow(y) — print the finished pixels of screen row y to the log.
//
// Reads the band buffer after the rasteriser has filled it and before it is
// blitted, so it reports exactly what reaches the panel. Used to tell a genuine
// shading fault from RGB565 quantisation: a clean gradient steps monotonically
// (and green, having 6 bits, steps twice as often as red and blue, which is
// what makes a grey ramp appear to shift hue as it bands).
static int l_dumprow(lua_State* Ls) {
    jet_request_row_dump((int)luaL_checkinteger(Ls, 1));
    return 0;
}

// jet.probe(object [, samples])
//
// Reports the numbers the diffuse lighting depends on, to the serial log.
//
// Scene.cpp's sceneLambertDiffuse computes dot(N, L) >> 12, which is only a
// 0..255 Lambert term when BOTH vectors are exactly FIXED_POINT_SCALE (1024)
// long. FLAT shading reads one vertex normal per face, GOURAUD reads all three
// and interpolates between them, so a per-vertex magnitude error is invisible
// under FLAT and shows up as wrong shading under GOURAUD. This prints the
// measured lengths so that assumption can be checked instead of trusted.
static int l_probe(lua_State* Ls) {
    Object* o = checkObject(Ls, 1);
    int samples = (int)luaL_optinteger(Ls, 2, 8);

    // Light vector length.
    if (g_dirLight) {
        const Vector3& L = g_dirLight->worldLightDir;
        double lLen = sqrt((double)L.x * L.x + (double)L.y * L.y + (double)L.z * L.z);
        logf_("[probe] light world dir (%d,%d,%d) len=%d (expected %d)",
              (int)L.x, (int)L.y, (int)L.z, (int)(lLen + 0.5), FIXED_POINT_SCALE);
    }

    const size_t n = o->vertices.size();
    logf_("[probe] object: %u vertices, %u triangles",
          (unsigned)n, (unsigned)o->triangles.size());
    if (n == 0) return 0;

    // Walk the whole mesh for the min/max, print a spread of individual
    // samples so a systematic error is distinguishable from an occasional one.
    int minLen = 1 << 30, maxLen = 0;
    long long sumLen = 0;
    for (size_t i = 0; i < n; ++i) {
        const Vector3& v = o->vertices[i].normal;
        int len = (int)(sqrt((double)v.x * v.x + (double)v.y * v.y + (double)v.z * v.z) + 0.5);
        if (len < minLen) minLen = len;
        if (len > maxLen) maxLen = len;
        sumLen += len;
    }
    logf_("[probe] normal len: min=%d max=%d avg=%d (expected %d)",
          minLen, maxLen, (int)(sumLen / (long long)n), FIXED_POINT_SCALE);

    if (samples > (int)n) samples = (int)n;
    for (int s = 0; s < samples; ++s) {
        size_t i = (size_t)((long long)s * (long long)(n - 1) / (samples > 1 ? samples - 1 : 1));
        const Vector3& v = o->vertices[i].normal;
        const Vector3& p = o->vertices[i].position;
        int len = (int)(sqrt((double)v.x * v.x + (double)v.y * v.y + (double)v.z * v.z) + 0.5);
        logf_("[probe]  v%u pos(%d,%d,%d) n(%d,%d,%d) len=%d b=%d",
              (unsigned)i, (int)p.x, (int)p.y, (int)p.z,
              (int)v.x, (int)v.y, (int)v.z, len, (int)mirrorLambert(v));
    }

    // Per-vertex brightness across the whole mesh. GOURAUD interpolates
    // between these three values per triangle, so if they vary smoothly the
    // inputs are sound and any banding comes from the interpolation; if they
    // jump around, the fault is upstream in the precompute.
    int minB = 256, maxB = -1;
    long long sumB = 0;
    for (size_t i = 0; i < n; ++i) {
        int b = (int)mirrorLambert(o->vertices[i].normal);
        if (b < minB) minB = b;
        if (b > maxB) maxB = b;
        sumB += b;
    }
    logf_("[probe] brightness: min=%d max=%d avg=%d", minB, maxB, (int)(sumB / (long long)n));
    return 0;
}

static int l_quit(lua_State* Ls) {
    (void)Ls;
    g_quit = true;
    return 0;
}

// ---------------------------------------------------------------------------
// jet.material{...}
// ---------------------------------------------------------------------------

static int l_material(lua_State* Ls) {
    uint16_t color = 0xFFFF;
    uint8_t alpha = 255, diffuse = 255, specular = 0;
    bool emissive = false;
    int shading = (int)ShadingMode::FLAT;
    Texture* tex = nullptr;

    if (lua_istable(Ls, 1)) {
        lua_getfield(Ls, 1, "color");
        if (!lua_isnil(Ls, -1)) color = (uint16_t)luaL_checkinteger(Ls, -1);
        lua_pop(Ls, 1);

        lua_getfield(Ls, 1, "alpha");
        if (!lua_isnil(Ls, -1)) alpha = (uint8_t)luaL_checkinteger(Ls, -1);
        lua_pop(Ls, 1);

        lua_getfield(Ls, 1, "diffuse");
        if (!lua_isnil(Ls, -1)) diffuse = (uint8_t)luaL_checkinteger(Ls, -1);
        lua_pop(Ls, 1);

        lua_getfield(Ls, 1, "specular");
        if (!lua_isnil(Ls, -1)) specular = (uint8_t)luaL_checkinteger(Ls, -1);
        lua_pop(Ls, 1);

        lua_getfield(Ls, 1, "emissive");
        if (!lua_isnil(Ls, -1)) emissive = lua_toboolean(Ls, -1) != 0;
        lua_pop(Ls, 1);

        lua_getfield(Ls, 1, "shading");
        if (!lua_isnil(Ls, -1)) shading = (int)luaL_checkinteger(Ls, -1);
        lua_pop(Ls, 1);

        lua_getfield(Ls, 1, "texture");
        if (!lua_isnil(Ls, -1)) {
            Texture** ud = (Texture**)luaL_checkudata(Ls, -1, MT_TEXTURE);
            tex = *ud;
        }
        lua_pop(Ls, 1);
    } else if (lua_isnumber(Ls, 1)) {
        color = (uint16_t)lua_tointeger(Ls, 1);
        if (lua_isnumber(Ls, 2)) alpha = (uint8_t)lua_tointeger(Ls, 2);
    }

    Material* m = new Material(color, tex, nullptr, emissive, alpha, diffuse, specular);
    m->shadingMode = (ShadingMode)shading;
    g_materials.push_back(m);
    pushMaterial(Ls, m);
    return 1;
}

static int l_material_set(lua_State* Ls) {
    Material* m = checkMaterial(Ls, 1);
    luaL_checktype(Ls, 2, LUA_TTABLE);

    lua_getfield(Ls, 2, "color");
    if (!lua_isnil(Ls, -1)) m->color = (uint16_t)luaL_checkinteger(Ls, -1);
    lua_pop(Ls, 1);

    lua_getfield(Ls, 2, "alpha");
    if (!lua_isnil(Ls, -1)) m->alpha = (uint8_t)luaL_checkinteger(Ls, -1);
    lua_pop(Ls, 1);

    lua_getfield(Ls, 2, "shading");
    if (!lua_isnil(Ls, -1)) m->shadingMode = (ShadingMode)(int)luaL_checkinteger(Ls, -1);
    lua_pop(Ls, 1);

    lua_getfield(Ls, 2, "emissive");
    if (!lua_isnil(Ls, -1)) m->emissive = lua_toboolean(Ls, -1) != 0;
    lua_pop(Ls, 1);

    lua_getfield(Ls, 2, "specular");
    if (!lua_isnil(Ls, -1)) m->specular = (uint8_t)luaL_checkinteger(Ls, -1);
    lua_pop(Ls, 1);

    lua_settop(Ls, 1);
    return 1;
}

// ---------------------------------------------------------------------------
// jet.texture(path, w, h [, opts])
//
// Loads a raw little-endian RGB565 blob, matching the firmware's existing
// .bin texture convention. Size must be exactly w*h*2 bytes.
// ---------------------------------------------------------------------------

static int l_texture(lua_State* Ls) {
    const char* path = luaL_checkstring(Ls, 1);
    int w = (int)luaL_checkinteger(Ls, 2);
    int h = (int)luaL_checkinteger(Ls, 3);
    if (w <= 0 || h <= 0) return luaL_error(Ls, "texture: bad dimensions");

    bool hasAlpha = false;
    uint16_t alphaColor = 0;
    int addressMode = (int)WRAP;
    if (lua_istable(Ls, 4)) {
        lua_getfield(Ls, 4, "alphacolor");
        if (!lua_isnil(Ls, -1)) {
            hasAlpha = true;
            alphaColor = (uint16_t)luaL_checkinteger(Ls, -1);
        }
        lua_pop(Ls, 1);
        lua_getfield(Ls, 4, "address");
        if (!lua_isnil(Ls, -1)) addressMode = (int)luaL_checkinteger(Ls, -1);
        lua_pop(Ls, 1);
    }

    // opts.palette = N switches to indexed mode: the file is w*h bytes of
    // palette indices followed by N RGB565 entries. Indices are widened to the
    // uint16 array Texture expects; the palette gets its own buffer so
    // tex:shift()/tex:animate() can rotate it.
    int palN = 0;
    if (lua_istable(Ls, 4)) {
        lua_getfield(Ls, 4, "palette");
        if (!lua_isnil(Ls, -1)) palN = (int)luaL_checkinteger(Ls, -1);
        lua_pop(Ls, 1);
        if (palN < 0 || palN > 256) return luaL_error(Ls, "texture: bad palette size");
    }

    FILE* f = fopen(path, "rb");
    if (!f) return luaL_error(Ls, "texture: cannot open %s", path);

    uint16_t* pix = nullptr;
    uint16_t* pal = nullptr;

    if (palN > 0) {
        const size_t npix = (size_t)w * (size_t)h;
        const size_t want = npix + (size_t)palN * 2u;
        uint8_t* raw = (uint8_t*)malloc(want);
        if (!raw) { fclose(f); return luaL_error(Ls, "texture: out of memory"); }
        size_t got = fread(raw, 1, want, f);
        fclose(f);
        if (got != want) {
            free(raw);
            return luaL_error(Ls, "texture: %s is %u bytes, expected %u",
                              path, (unsigned)got, (unsigned)want);
        }
        pix = (uint16_t*)malloc(npix * 2u);
        pal = (uint16_t*)malloc((size_t)palN * 2u);
        if (!pix || !pal) {
            free(raw); if (pix) free(pix); if (pal) free(pal);
            return luaL_error(Ls, "texture: out of memory");
        }
        for (size_t i = 0; i < npix; ++i) pix[i] = raw[i];
        memcpy(pal, raw + npix, (size_t)palN * 2u);
        free(raw);
    } else {
        const size_t want = (size_t)w * (size_t)h * 2u;
        pix = (uint16_t*)malloc(want);
        if (!pix) { fclose(f); return luaL_error(Ls, "texture: out of memory"); }
        size_t got = fread(pix, 1, want, f);
        fclose(f);
        if (got != want) {
            free(pix);
            return luaL_error(Ls, "texture: %s is %u bytes, expected %u",
                              path, (unsigned)got, (unsigned)want);
        }
    }

    Texture* t = new Texture(w, h, pix, hasAlpha, alphaColor, false,
                             (TextureAddressMode)addressMode, pal);
    if (pal) t->paletteSize = palN;
    g_textures.push_back(t);
    g_texturePixels.push_back(pix);
    if (pal) g_texturePixels.push_back(pal);
    pushTexture(Ls, t);
    return 1;
}

// ---------------------------------------------------------------------------
// Procedural land (see engine/jet_terrain.h)
//
// Heights and positions cross this boundary in AUTHORED units, the same units
// the builder takes, so a game can plant a prop with jet.landheight() and the
// terrain under it will agree.
// ---------------------------------------------------------------------------

// jet.land{ seed=, relief=, featurelen=, octaves=, gain=, lacunarity=,
//           erosion=, warp=, plains=, snowline=, rockslope=, sandlevel=,
//           grass=, rock=, snow=, sand=, soil=, ambient=, light={x,y,z} }
static int l_land(lua_State* Ls) {
    JetLandCfg c;                       // starts from the documented defaults
    if (lua_istable(Ls, 1)) {
        c.seed       = (uint32_t)optField(Ls, 1, "seed",       (float)c.seed);
        c.relief     =           optField(Ls, 1, "relief",     c.relief);
        c.featureLen =           optField(Ls, 1, "featurelen", c.featureLen);
        c.octaves    = (int)     optField(Ls, 1, "octaves",    (float)c.octaves);
        c.gain       =           optField(Ls, 1, "gain",       c.gain);
        c.lacunarity =           optField(Ls, 1, "lacunarity", c.lacunarity);
        c.erosion    =           optField(Ls, 1, "erosion",    c.erosion);
        c.warp       =           optField(Ls, 1, "warp",       c.warp);
        c.plainsBias =           optField(Ls, 1, "plains",     c.plainsBias);
        c.continentLen =         optField(Ls, 1, "contlen",    c.continentLen);
        c.continentAmp =         optField(Ls, 1, "contamp",    c.continentAmp);
        c.terraceSteps = (int)   optField(Ls, 1, "terraces",   (float)c.terraceSteps);
        c.terraceSharp =         optField(Ls, 1, "terracesharp", c.terraceSharp);
        c.terraceMix   =         optField(Ls, 1, "terracemix",  c.terraceMix);
        c.erosionOctaves = (int) optField(Ls, 1, "erosionoct", (float)c.erosionOctaves);
        c.shadeGain    =         optField(Ls, 1, "shadegain",  c.shadeGain);
        c.snowLine   =           optField(Ls, 1, "snowline",   c.snowLine);
        c.rockSlope  =           optField(Ls, 1, "rockslope",  c.rockSlope);
        c.sandLevel  =           optField(Ls, 1, "sandlevel",  c.sandLevel);
        c.ambient    =           optField(Ls, 1, "ambient",    c.ambient);
        c.cGrass = (uint16_t)optField(Ls, 1, "grass", 0.0f);
        c.cRock  = (uint16_t)optField(Ls, 1, "rock",  0.0f);
        c.cSnow  = (uint16_t)optField(Ls, 1, "snow",  0.0f);
        c.cSand  = (uint16_t)optField(Ls, 1, "sand",  0.0f);
        c.cSoil  = (uint16_t)optField(Ls, 1, "soil",  0.0f);
        lua_getfield(Ls, 1, "light");
        if (lua_istable(Ls, -1)) {
            for (int k = 1; k <= 3; ++k) {
                lua_rawgeti(Ls, -1, k);
                const float v = (float)lua_tonumber(Ls, -1);
                if (k == 1) c.lightX = v; else if (k == 2) c.lightY = v;
                else c.lightZ = v;
                lua_pop(Ls, 1);
            }
        }
        lua_pop(Ls, 1);
    }
    jet_land_config(c);
    return 0;
}

// jet.landheight(x, z) -> authored height
static int l_landheight(lua_State* Ls) {
    lua_pushnumber(Ls, (lua_Number)jet_land_height((float)luaL_checknumber(Ls, 1),
                                                   (float)luaL_checknumber(Ls, 2)));
    return 1;
}

// jet.landtex(cx, cz, size, texels) -> texture
//
// The baked colour map for one tile: rule-based material colour shaded by the
// FINE surface normal. A coarse tile's mesh cannot carry that relief; the
// texture can, for texels*texels*2 bytes.
static int l_landtex(lua_State* Ls) {
    const float cx   = (float)luaL_checknumber(Ls, 1);
    const float cz   = (float)luaL_checknumber(Ls, 2);
    const float size = (float)luaL_checknumber(Ls, 3);
    const int   n    = (int)luaL_optinteger(Ls, 4, 32);
    if (n < 2 || n > 256) return luaL_error(Ls, "landtex: texels must be 2..256");

    uint16_t* pix = (uint16_t*)malloc((size_t)n * (size_t)n * 2u);
    if (!pix) return luaL_error(Ls, "landtex: out of memory");
    jet_land_bake(cx, cz, size, n, pix);

    Texture* t = new Texture(n, n, pix, false, 0, false, CLAMP, nullptr);
    t->buildMips();
    g_textures.push_back(t);
    g_texturePixels.push_back(pix);
    pushTexture(Ls, t);
    return 1;
}

// jet.tiletex(kind, texels, base, accent [, variant]) -> texture
//
// A seamless tiling detail texture, generated ONCE. This is the replacement for
// per-tile colour-map baking: a small fixed set repeated at a fixed world scale
// costs nothing per tile and puts the detail at the size the eye expects.
// `variant` folds into the noise seed: same kind/palette, different pattern.
static int l_tiletex(lua_State* Ls) {
    const int kind = (int)luaL_checkinteger(Ls, 1);
    const int n    = (int)luaL_optinteger(Ls, 2, 32);
    if (n < 2 || n > 256) return luaL_error(Ls, "tiletex: texels must be 2..256");
    const uint16_t base   = (uint16_t)luaL_optinteger(Ls, 3, 0x8410);
    const uint16_t accent = (uint16_t)luaL_optinteger(Ls, 4, 0xC618);
    const uint32_t variant = (uint32_t)luaL_optinteger(Ls, 5, 0);

    uint16_t* pix = (uint16_t*)malloc((size_t)n * (size_t)n * 2u);
    if (!pix) return luaL_error(Ls, "tiletex: out of memory");
    jet_land_tiletex(kind, n, base, accent, pix, variant);

    // WRAP, not CLAMP: these are meant to repeat.
    Texture* t = new Texture(n, n, pix, false, 0, false, WRAP, nullptr);
    // Ground tiles are the one thing minified hard enough to alias — the
    // camera flies tens of thousands of units above a surface whose texture
    // repeats every few thousand. Without a chain a pixel covering 2-9 texels
    // picks one at random and the surface crawls.
    t->buildMips();
    g_textures.push_back(t);
    g_texturePixels.push_back(pix);
    pushTexture(Ls, t);
    return 1;
}

// jet.cullgroup{ obj, obj, ... } -> group
//
// One coarse frustum gate over many objects — the reference engine's
// aggregation walk (a culled parent skips its whole subtree). See
// Scene::CullGroup. The group's bound is computed HERE, from the members'
// positions at call time — create groups after o:position(), and rebuild
// them when the world rebuilds. grp:destroy() disbands (members return to
// individual culling); destroying a member object removes it from its group
// engine-side, so a stale group is harmless, just useless.
static int l_cullgroup(lua_State* Ls) {
    luaL_checktype(Ls, 1, LUA_TTABLE);
    if (!g_scene) return luaL_error(Ls, "cullgroup: no scene");
    std::vector<Object*> members;
    const int n = (int)lua_rawlen(Ls, 1);
    members.reserve((size_t)n);
    for (int i = 1; i <= n; ++i) {
        lua_rawgeti(Ls, 1, i);
        members.push_back(checkObject(Ls, -1));
        lua_pop(Ls, 1);
    }
    Scene::CullGroup* g = g_scene->addCullGroup(members);
    if (!g) return luaL_error(Ls, "cullgroup: no members");
    Scene::CullGroup** ud =
        (Scene::CullGroup**)lua_newuserdatauv(Ls, sizeof(g), 0);
    *ud = g;
    luaL_getmetatable(Ls, MT_CULLGROUP);
    lua_setmetatable(Ls, -2);
    return 1;
}

static int l_cullgroup_destroy(lua_State* Ls) {
    Scene::CullGroup** ud =
        (Scene::CullGroup**)luaL_checkudata(Ls, 1, MT_CULLGROUP);
    if (*ud && g_scene) g_scene->removeCullGroup(*ud);
    *ud = nullptr;   // double-destroy safe
    return 0;
}

// jet.horizon{ ... } -> texture, peakElevationRadians
//
// Bake the far world into ONE panorama texture for a cylinder around the
// camera. Fields (all optional): x, y, z (viewpoint), near, far (radial band),
// elevlo, elevhi (radians the rows span), w, h, steps, skytop, skybot, haze,
// hazenear, hazefar, hazemax.
//
// The returned peak elevation is the highest angle any terrain reached; if it
// exceeds `elevhi` the ring is clipping its own mountains and elevhi should go
// up. Texture address mode is WRAP so U runs 0..FIXED_POINT_SCALE once around
// the ring, which is exactly what the cylinder primitive's barrel UVs give.
static int l_horizon(lua_State* Ls) {
    JetHorizonCfg c;
    int w = 256, h = 64;
    if (lua_istable(Ls, 1)) {
        c.camX     = optField(Ls, 1, "x", c.camX);
        c.camY     = optField(Ls, 1, "y", c.camY);
        c.camZ     = optField(Ls, 1, "z", c.camZ);
        c.rNear    = optField(Ls, 1, "near", c.rNear);
        c.rFar     = optField(Ls, 1, "far",  c.rFar);
        c.elevLo   = optField(Ls, 1, "elevlo", c.elevLo);
        c.elevHi   = optField(Ls, 1, "elevhi", c.elevHi);
        c.steps    = (int)optField(Ls, 1, "steps", (float)c.steps);
        c.skyTop   = (uint16_t)optField(Ls, 1, "skytop", 0.0f);
        c.skyBot   = (uint16_t)optField(Ls, 1, "skybot", 0.0f);
        c.haze     = (uint16_t)optField(Ls, 1, "haze",   0.0f);
        c.hazeNear = optField(Ls, 1, "hazenear", c.hazeNear);
        c.hazeFar  = optField(Ls, 1, "hazefar",  c.hazeFar);
        c.hazeMax  = optField(Ls, 1, "hazemax",  c.hazeMax);
        c.rangeAmp  = optField(Ls, 1, "rangeamp",  c.rangeAmp);
        c.rangeLen  = optField(Ls, 1, "rangelen",  c.rangeLen);
        c.rangeFrom = optField(Ls, 1, "rangefrom", c.rangeFrom);
        c.rangeFull = optField(Ls, 1, "rangefull", c.rangeFull);
        w = (int)optField(Ls, 1, "w", (float)w);
        h = (int)optField(Ls, 1, "h", (float)h);
        // col0/cols: partial-column window for the amortised sweep (see
        // JetHorizonCfg::colStart). Only meaningful with `into`.
        c.colStart = (int)optField(Ls, 1, "col0", 0.0f);
        c.colCount = (int)optField(Ls, 1, "cols", 0.0f);
    }
    if (w < 8 || w > 1024) return luaL_error(Ls, "horizon: w must be 8..1024");
    if (h < 4 || h > 256)  return luaL_error(Ls, "horizon: h must be 4..256");
    if (c.rFar <= c.rNear) return luaL_error(Ls, "horizon: far must exceed near");

    // into = an existing horizon texture: rebake IN PLACE (w/h must match).
    // The drift rebake that follows a moving camera runs every few hundred
    // thousand units of flight; a fresh 64 KB texture per bake would bleed
    // PSRAM dry in minutes. Only the mip chain is reallocated.
    Texture* into = nullptr;
    if (lua_istable(Ls, 1)) {
        lua_getfield(Ls, 1, "into");
        if (!lua_isnil(Ls, -1)) {
            Texture** ud = (Texture**)luaL_checkudata(Ls, -1, MT_TEXTURE);
            into = *ud;
        }
        lua_pop(Ls, 1);
    }
    if (into) {
        if (into->width != w || into->height != h)
            return luaL_error(Ls, "horizon: into is %dx%d but the bake is %dx%d",
                              into->width, into->height, w, h);
        const float peakIn = jet_land_horizon(c, w, h, into->data);
        // Mips are rebuilt only when the write reaches the last column — for
        // a column-sweep that is once per revolution, not once per slice.
        // buildMips mallocs fresh level buffers; drop the old chain first.
        if (c.colCount <= 0 || c.colStart + c.colCount >= w) {
            for (int i = 1; i < into->mipCount; ++i) {
                free(into->mipData[i]);
                into->mipData[i] = nullptr;
            }
            into->mipCount = 1;
            into->buildMips();
        }
        lua_getfield(Ls, 1, "into");     // same userdata back to the caller
        lua_pushnumber(Ls, peakIn);
        return 2;
    }

    uint16_t* pix = (uint16_t*)malloc((size_t)w * (size_t)h * 2u);
    if (!pix) return luaL_error(Ls, "horizon: out of memory");
    const float peak = jet_land_horizon(c, w, h, pix);

    Texture* t = new Texture(w, h, pix, false, 0, false, WRAP, nullptr);
    // The panorama is the largest texture in a scene by a wide margin, so it is
    // the one most worth having a chain for — both to sample correctly when the
    // ring is seen edge-on and to keep the working set small. buildMips needs
    // power-of-two dimensions, so `h` must be 32 or 64, not 48.
    t->buildMips();
    g_textures.push_back(t);
    g_texturePixels.push_back(pix);
    pushTexture(Ls, t);
    lua_pushnumber(Ls, peak);
    return 2;
}

// jet.landscatter(cx, cz, size, i, n) -> x, z, y, rand   (nil when rejected)
static int l_landscatter(lua_State* Ls) {
    float x, z, y; uint32_t rnd;
    const bool ok = jet_land_scatter((float)luaL_checknumber(Ls, 1),
                                     (float)luaL_checknumber(Ls, 2),
                                     (float)luaL_checknumber(Ls, 3),
                                     (int)luaL_checkinteger(Ls, 4),
                                     (int)luaL_checkinteger(Ls, 5),
                                     &x, &z, &y, &rnd);
    if (!ok) { lua_pushnil(Ls); return 1; }
    lua_pushnumber(Ls, x); lua_pushnumber(Ls, z); lua_pushnumber(Ls, y);
    lua_pushinteger(Ls, (lua_Integer)(rnd & 0x7FFFFFFFu));
    return 4;
}

// ---------------------------------------------------------------------------
// Texture methods: palette animation
// ---------------------------------------------------------------------------

static Texture* checkTexture(lua_State* Ls, int idx) {
    Texture** ud = (Texture**)luaL_checkudata(Ls, idx, MT_TEXTURE);
    return *ud;
}

// tex:animate(fps) — rotate the palette fps steps per second; 0 stops.
static int t_animate(lua_State* Ls) {
    Texture* t = checkTexture(Ls, 1);
    const float fps = (float)luaL_checknumber(Ls, 2);
    if (t->paletteSize <= 0) return luaL_error(Ls, "animate: texture has no palette");
    for (size_t i = 0; i < g_texAnims.size(); ++i) {
        if (g_texAnims[i].tex == t) {
            if (fps <= 0) g_texAnims.erase(g_texAnims.begin() + i);
            else { g_texAnims[i].fps = fps; }
            lua_settop(Ls, 1);
            return 1;
        }
    }
    if (fps > 0) g_texAnims.push_back({ t, fps, 0.0f });
    lua_settop(Ls, 1);
    return 1;
}

// tex:rebake(cx, cz, size) — re-bake a land colour map into this texture's
// EXISTING pixel buffer.
//
// A camera-following world reuses tile slots constantly, and allocating a
// texture per tile would churn PSRAM until it fragmented. Baking in place makes
// the whole texture set a fixed cost paid once at start-up, which is what a
// streaming ring needs.
static int t_rebake(lua_State* Ls) {
    Texture* t = checkTexture(Ls, 1);
    if (t->palette) return luaL_error(Ls, "rebake: not for paletted textures");
    if (t->width != t->height)
        return luaL_error(Ls, "rebake: texture must be square");
    jet_land_bake((float)luaL_checknumber(Ls, 2),
                  (float)luaL_checknumber(Ls, 3),
                  (float)luaL_checknumber(Ls, 4), t->width, t->data);
    lua_settop(Ls, 1);
    return 1;
}

// tex:shift(n) — set the palette offset directly (manual animation).
static int t_shift(lua_State* Ls) {
    Texture* t = checkTexture(Ls, 1);
    if (t->paletteSize <= 0) return luaL_error(Ls, "shift: texture has no palette");
    int n = (int)luaL_checkinteger(Ls, 2) % t->paletteSize;
    if (n < 0) n += t->paletteSize;
    t->paletteOffset = n;
    lua_settop(Ls, 1);
    return 1;
}

// ---------------------------------------------------------------------------
// Primitives
// ---------------------------------------------------------------------------

static int l_cube(lua_State* Ls) {
    int32_t w = argWorld(Ls, 1), h = argWorld(Ls, 2), d = argWorld(Ls, 3);
    return finishObject(Ls, Primitives::createCube(w, h, d, argMaterial(Ls, 4)));
}

static int l_sphere(lua_State* Ls) {
    int32_t r = argWorld(Ls, 1);
    int32_t seg = optInt(Ls, 2, 12);
    return finishObject(Ls, Primitives::createSphere(r, seg, argMaterial(Ls, 3)));
}

static int l_plane(lua_State* Ls) {
    int32_t w = argWorld(Ls, 1), h = argWorld(Ls, 2);
    return finishObject(Ls, Primitives::createPlane(w, h, argMaterial(Ls, 3)));
}

static int l_pyramid(lua_State* Ls) {
    int32_t b = argWorld(Ls, 1), h = argWorld(Ls, 2);
    return finishObject(Ls, Primitives::createPyramid(b, h, argMaterial(Ls, 3)));
}

static int l_cylinder(lua_State* Ls) {
    int32_t r = argWorld(Ls, 1), h = argWorld(Ls, 2);
    int32_t seg = optInt(Ls, 3, 12);
    bool caps = lua_isnoneornil(Ls, 4) ? true : (lua_toboolean(Ls, 4) != 0);
    return finishObject(Ls, Primitives::createCylinder(r, h, seg, caps, argMaterial(Ls, 5)));
}

static int l_capsule(lua_State* Ls) {
    int32_t r = argWorld(Ls, 1), h = argWorld(Ls, 2);
    int32_t seg = optInt(Ls, 3, 12);
    return finishObject(Ls, Primitives::createCapsule(r, h, seg, argMaterial(Ls, 4)));
}

static int l_quad(lua_State* Ls) {
    int32_t w = argWorld(Ls, 1), h = argWorld(Ls, 2);
    return finishObject(Ls, Primitives::createQuad(w, h, argMaterial(Ls, 3)));
}

static int l_billboard(lua_State* Ls) {
    int32_t w = argWorld(Ls, 1), h = argWorld(Ls, 2);
    return finishObject(Ls, Primitives::createBillboard(w, h, argMaterial(Ls, 3)));
}

static int l_grid(lua_State* Ls) {
    int32_t w = argWorld(Ls, 1), h = argWorld(Ls, 2);
    int32_t rows = argInt(Ls, 3), cols = argInt(Ls, 4);
    Material* m1 = argMaterial(Ls, 5);
    Material* m2 = lua_isnoneornil(Ls, 6) ? m1 : argMaterial(Ls, 6);
    return finishObject(Ls, Primitives::createGrid(w, h, rows, cols, m1, m2, false));
}

// ---------------------------------------------------------------------------
// Object methods
// ---------------------------------------------------------------------------

static int o_position(lua_State* Ls) {
    Object* o = checkObject(Ls, 1);
    if (lua_isnoneornil(Ls, 2)) {
        lua_pushnumber(Ls, toWorldOut(o->position.x));
        lua_pushnumber(Ls, toWorldOut(o->position.y));
        lua_pushnumber(Ls, toWorldOut(o->position.z));
        return 3;
    }
    o->setPosition(argWorld(Ls, 2), argWorld(Ls, 3), argWorld(Ls, 4));
    lua_settop(Ls, 1);
    return 1;
}

static int o_translate(lua_State* Ls) {
    Object* o = checkObject(Ls, 1);
    o->translate(argWorld(Ls, 2), argWorld(Ls, 3), argWorld(Ls, 4));
    lua_settop(Ls, 1);
    return 1;
}

static int o_rotation(lua_State* Ls) {
    Object* o = checkObject(Ls, 1);
    if (lua_isnoneornil(Ls, 2)) {
        lua_pushinteger(Ls, o->rotation.x);
        lua_pushinteger(Ls, o->rotation.y);
        lua_pushinteger(Ls, o->rotation.z);
        return 3;
    }
    o->setRotation(argInt(Ls, 2), argInt(Ls, 3), argInt(Ls, 4));
    lua_settop(Ls, 1);
    return 1;
}

static int o_rotate(lua_State* Ls) {
    Object* o = checkObject(Ls, 1);
    o->rotate(argInt(Ls, 2), argInt(Ls, 3), argInt(Ls, 4));
    lua_settop(Ls, 1);
    return 1;
}

// Scale is fixed-point: FIXED_POINT_SCALE (1024) is 1.0. transformScale is set
// so the factor is applied in the world transform (non-destructive, so it can
// animate) rather than baked into the mesh by bakeScale, which is permanent and
// uniform-only. Per-axis factors are supported; normals get the inverse scale
// in the transform so non-uniform scaling still lights correctly.
static int o_scale(lua_State* Ls) {
    Object* o = checkObject(Ls, 1);
    if (lua_isnoneornil(Ls, 2)) {
        lua_pushnumber(Ls, (lua_Number)o->scale.x / FIXED_POINT_SCALE);
        lua_pushnumber(Ls, (lua_Number)o->scale.y / FIXED_POINT_SCALE);
        lua_pushnumber(Ls, (lua_Number)o->scale.z / FIXED_POINT_SCALE);
        return 3;
    }
    lua_Number sx = luaL_checknumber(Ls, 2);
    lua_Number sy = lua_isnoneornil(Ls, 3) ? sx : luaL_checknumber(Ls, 3);
    lua_Number sz = lua_isnoneornil(Ls, 4) ? sx : luaL_checknumber(Ls, 4);
    o->scale.x = toFixed(sx * FIXED_POINT_SCALE);
    o->scale.y = toFixed(sy * FIXED_POINT_SCALE);
    o->scale.z = toFixed(sz * FIXED_POINT_SCALE);
    o->transformScale = true;
    lua_settop(Ls, 1);
    return 1;
}

static int o_enabled(lua_State* Ls) {
    Object* o = checkObject(Ls, 1);
    if (lua_isnoneornil(Ls, 2)) { lua_pushboolean(Ls, o->enabled); return 1; }
    o->enabled = lua_toboolean(Ls, 2) != 0;
    // getStatistics caches its totals and skips disabled objects, and the Scene
    // has no way to notice this flag changing underneath it.
    if (g_scene) g_scene->invalidateStatistics();
    lua_settop(Ls, 1);
    return 1;
}

// obj:fade(near, far)   — opaque inside `near`, dissolves to nothing at `far`,
//                         and SKIPPED beyond it.
// obj:appear(near, far) — invisible inside `near`, dissolves in by `far`.
//
// Both take AUTHORED units. Either pair alone is a draw distance; PAIRED across
// two objects they are an aggregating LOD: give the fine object fade(a,b) and
// the coarse object that replaces it appear(a,b), and the overlap is a
// screen-door cross-dissolve — which is how the swap hides without fog.
//
// This is a real performance mechanism, not just a visual one. Scene.cpp does
// the range test AFTER the cheap frustum cull but BEFORE renderObject, so an
// out-of-band object costs the ~3.4us cull and nothing else — no transform, no
// per-triangle work. That is what lets a world hold far more tiles than it
// could ever draw: only the active distance band pays the full price.
static int o_fade(lua_State* Ls) {
    Object* o = checkObject(Ls, 1);
    o->fadeNear = argWorld(Ls, 2);
    o->fadeFar  = argWorld(Ls, 3);
    lua_settop(Ls, 1);
    return 1;
}

static int o_appear(lua_State* Ls) {
    Object* o = checkObject(Ls, 1);
    o->appearNear = argWorld(Ls, 2);
    o->appearFar  = argWorld(Ls, 3);
    lua_settop(Ls, 1);
    return 1;
}

// o:landpaint(spacing) -> painted-triangle count
//
// Stamp every UNTEXTURED triangle's baked colour from the FINE terrain
// palette at its centroid (jet_land_shade_at) — geometric detail folded into
// per-triangle colour, at span-fill price. This is the reference engine's
// "bake appearance so the mesh can be coarse" translated into the currency
// this renderer can afford: its baked images are our baked triangle colours.
// The shade already carries the land cfg's light + ambient, so it REPLACES
// bakeFlatLighting's stamp (call order does not matter — later wins) and
// must not be lit again. Textured triangles are skipped: their texels carry
// the colour. `spacing` is authored units, roughly the area one triangle
// covers. Call AFTER o:position() — the centroid is offset by the object's
// position at call time; object rotation is not applied (terrain nodes are
// unrotated).
static int o_landpaint(lua_State* Ls) {
    Object* o = checkObject(Ls, 1);
    const float spacing = (float)luaL_checknumber(Ls, 2);
    const float inv = 1.0f / (float)JET32_WORLD_SCALE;
    int painted = 0;
    for (auto& tri : o->triangles) {
        if (tri.material && tri.material->diffuseMap) continue;
        const Vector3& a = o->vertices[tri.v1].position;
        const Vector3& b = o->vertices[tri.v2].position;
        const Vector3& c = o->vertices[tri.v3].position;
        const float wx = (((float)a.x + (float)b.x + (float)c.x) * (1.0f / 3.0f)
                          + (float)o->position.x) * inv;
        const float wz = (((float)a.z + (float)b.z + (float)c.z) * (1.0f / 3.0f)
                          + (float)o->position.z) * inv;
        tri.bakedColor = jet_land_shade_at(wx, wz, spacing);
        tri.colorBaked = true;
        ++painted;
    }
    lua_pushinteger(Ls, painted);
    return 1;
}

static int o_billboard(lua_State* Ls) {
    Object* o = checkObject(Ls, 1);
    o->isBillboard = lua_isnoneornil(Ls, 2) ? true : (lua_toboolean(Ls, 2) != 0);
    lua_settop(Ls, 1);
    return 1;
}

// obj:sortdepth(jet.SORT_FARTHEST) — which depth of each triangle drives the
// painter's sort. FARTHEST makes a surface sort by its back edge, so anything
// standing on it always draws in front: the fix for ground planes, free at
// runtime. Do NOT use it on a surface that should hide things behind it.
static int o_sortdepth(lua_State* Ls) {
    Object* o = checkObject(Ls, 1);
    o->sortDepth = (SortDepth)(int)luaL_checkinteger(Ls, 2);
    lua_settop(Ls, 1);
    return 1;
}

static int o_culling(lua_State* Ls) {
    Object* o = checkObject(Ls, 1);
    o->cullingMode = (CullingMode)(int)luaL_checkinteger(Ls, 2);
    lua_settop(Ls, 1);
    return 1;
}

static int o_material(lua_State* Ls) {
    Object* o = checkObject(Ls, 1);
    Material* m = argMaterial(Ls, 2);
    for (size_t i = 0; i < o->triangles.size(); ++i) o->triangles[i].material = m;
    lua_settop(Ls, 1);
    return 1;
}

static int o_lookat(lua_State* Ls) {
    Object* o = checkObject(Ls, 1);
    Vector3 t = { argWorld(Ls, 2), argWorld(Ls, 3), argWorld(Ls, 4) };
    o->lookAt(t);
    lua_settop(Ls, 1);
    return 1;
}

static int o_shading(lua_State* Ls) {
    Object* o = checkObject(Ls, 1);
    ShadingMode sm = (ShadingMode)(int)luaL_checkinteger(Ls, 2);
    for (size_t i = 0; i < o->triangles.size(); ++i) {
        if (o->triangles[i].material) o->triangles[i].material->shadingMode = sm;
    }
    lua_settop(Ls, 1);
    return 1;
}

// Drops the object from the scene, frees it, and nulls the Lua handle so a
// later method call raises rather than touching freed memory. Scene::addObject
// had no counterpart upstream, so hiding via enabled() was the only option and
// the object kept paying a cull test forever.
// Objects the engine created ALONGSIDE a handle it returned: the spatial cells
// of a celled bake (Lua only ever gets cell 0) and the LOD levels of a chain.
// Destroying the handle has to take them all, or a game that builds and
// rebuilds a world leaks every cell but the first.
//
// Order matters and is creation order: the blob-adopting member is LAST, and
// the others borrow from that blob, so it must be freed last.
struct ObjGroup { Object* head; std::vector<Object*> all; };
static std::vector<ObjGroup> g_objGroups;

static void registerGroup(Object* head, const std::vector<Object*>& all) {
    if (all.size() < 2) return;            // nothing extra to track
    ObjGroup g; g.head = head; g.all = all;
    g_objGroups.push_back(g);
}

static void destroyOne(Object* o) {
    if (!o) return;
    if (g_scene) g_scene->removeObject(o);
    for (size_t i = 0; i < g_objects.size(); ++i) {
        if (g_objects[i] == o) { g_objects.erase(g_objects.begin() + i); break; }
    }
    delete o;
}

static int o_destroy(lua_State* Ls) {
    Object** ud = (Object**)luaL_checkudata(Ls, 1, MT_OBJECT);
    Object* o = *ud;
    if (!o) return 0;                      // already destroyed: no-op

    for (size_t g = 0; g < g_objGroups.size(); ++g) {
        if (g_objGroups[g].head != o) continue;
        const std::vector<Object*> all = g_objGroups[g].all;
        g_objGroups.erase(g_objGroups.begin() + g);
        for (size_t i = 0; i < all.size(); ++i) destroyOne(all[i]);
        *ud = nullptr;
        return 0;
    }

    destroyOne(o);
    *ud = nullptr;
    return 0;
}

// Triangles this object put into the render queue last frame. 0 while enabled
// means everything was culled.
// obj:lodlevel() -> level the scene picked last frame (0 = full detail,
// N = lodMeshes[N-1], -1 = culled for running out of LODs). Reports what the
// renderer actually chose rather than what the distance implies.
static int o_lodlevel(lua_State* Ls) {
    Object* o = checkObject(Ls, 1);
    lua_pushinteger(Ls, o->lastLodLevel);
    return 1;
}

static int o_emitted(lua_State* Ls) {
    lua_pushinteger(Ls, checkObject(Ls, 1)->lastEmitted);
    return 1;
}

// Stable identity for comparing against jet.picked() results. Pointer value as
// an integer: PSRAM addresses (~0x3D9xxxxx) fit the 32-bit lua_Integer.
static int o_id(lua_State* Ls) {
    lua_pushinteger(Ls, (lua_Integer)(uintptr_t)checkObject(Ls, 1));
    return 1;
}

// Painter's-sort depth bias: positive wins against coplanar geometry (decals,
// ground markings). Participates in the sort via the MESHPUNK emitTri patch.
static int o_bias(lua_State* Ls) {
    Object* o = checkObject(Ls, 1);
    if (lua_isnoneornil(Ls, 2)) { lua_pushinteger(Ls, o->zBias); return 1; }
    lua_Integer b = luaL_checkinteger(Ls, 2);
    o->zBias = (int8_t)(b < -127 ? -127 : b > 127 ? 127 : b);
    lua_settop(Ls, 1);
    return 1;
}

static const luaL_Reg object_methods[] = {
    { "id",        o_id        },
    { "bias",      o_bias      },
    { "emitted",   o_emitted   },
    { "lodlevel",  o_lodlevel  },
    { "destroy",   o_destroy   },
    { "position",  o_position  },
    { "translate", o_translate },
    { "rotation",  o_rotation  },
    { "rotate",    o_rotate    },
    { "scale",     o_scale     },
    { "enabled",   o_enabled   },
    { "billboard", o_billboard },
    { "culling",   o_culling   },
    { "sortdepth", o_sortdepth },
    { "fade",      o_fade      },
    { "appear",    o_appear    },
    { "landpaint", o_landpaint },
    { "material",  o_material  },
    { "shading",   o_shading   },
    { "lookat",    o_lookat    },
    { nullptr, nullptr }
};

// ---------------------------------------------------------------------------
// jet.camera
// ---------------------------------------------------------------------------

static int c_position(lua_State* Ls) {
    if (lua_isnoneornil(Ls, 1)) {
        lua_pushnumber(Ls, toWorldOut(g_camera->position.x));
        lua_pushnumber(Ls, toWorldOut(g_camera->position.y));
        lua_pushnumber(Ls, toWorldOut(g_camera->position.z));
        return 3;
    }
    g_camera->setPosition(argWorld(Ls, 1), argWorld(Ls, 2), argWorld(Ls, 3));
    return 0;
}

static int c_translate(lua_State* Ls) {
    g_camera->translate(argWorld(Ls, 1), argWorld(Ls, 2), argWorld(Ls, 3));
    return 0;
}

static int c_translatelocal(lua_State* Ls) {
    g_camera->translateLocal(argWorld(Ls, 1), argWorld(Ls, 2), argWorld(Ls, 3));
    return 0;
}

// Degrees in, degrees out, at full float precision.
//
// Camera::rotation holds RADIANS under FLOAT_CAMERA_ANGLES, and
// Camera::setRotation only takes int32_t degrees — so routing through it both
// quantised absolute aiming to whole degrees and, because this getter returned
// the raw field, made rotation(90) read back as 1.5708. The conversion is done
// here against the field directly instead.
static const lua_Number JET_DEG2RAD = (lua_Number)(2.0 * 3.14159265358979323846 / 360.0);

static int c_rotation(lua_State* Ls) {
    if (lua_isnoneornil(Ls, 1)) {
        lua_pushnumber(Ls, (lua_Number)g_camera->rotation.x / JET_DEG2RAD);
        lua_pushnumber(Ls, (lua_Number)g_camera->rotation.y / JET_DEG2RAD);
        lua_pushnumber(Ls, (lua_Number)g_camera->rotation.z / JET_DEG2RAD);
        return 3;
    }
    g_camera->rotation.assign(
        (float)(luaL_checknumber(Ls, 1) * JET_DEG2RAD),
        (float)(luaL_checknumber(Ls, 2) * JET_DEG2RAD),
        (float)(luaL_checknumber(Ls, 3) * JET_DEG2RAD));
    return 0;
}

static int c_rotate(lua_State* Ls) {
    g_camera->rotate((float)luaL_checknumber(Ls, 1),
                     (float)luaL_checknumber(Ls, 2),
                     (float)luaL_checknumber(Ls, 3));
    return 0;
}

static int c_fov(lua_State* Ls) {
    if (lua_isnoneornil(Ls, 1)) { lua_pushnumber(Ls, g_camera->fov); return 1; }
    g_camera->setFOV((float)luaL_checknumber(Ls, 1), g_screenW);
    return 0;
}

static int c_clip(lua_State* Ls) {
    if (lua_isnoneornil(Ls, 1)) {
        lua_pushnumber(Ls, toWorldOut(g_camera->nearPlane));
        lua_pushnumber(Ls, toWorldOut(g_camera->farPlane));
        return 2;
    }
    g_camera->nearPlane = argWorld(Ls, 1);
    g_camera->farPlane  = argWorld(Ls, 2);
    return 0;
}

static int c_lookat(lua_State* Ls) {
    Vector3 t = { argWorld(Ls, 1), argWorld(Ls, 2), argWorld(Ls, 3) };
    g_camera->lookAt(t);
    return 0;
}

// Single-axis aim: pitch-only / yaw-only versions of lookat, for cameras that
// track a target on one axis while the game drives the other (chase cams,
// turrets).
static int c_lookatx(lua_State* Ls) {
    Vector3 t = { argWorld(Ls, 1), argWorld(Ls, 2), argWorld(Ls, 3) };
    g_camera->lookAtX(t);
    return 0;
}

static int c_lookaty(lua_State* Ls) {
    Vector3 t = { argWorld(Ls, 1), argWorld(Ls, 2), argWorld(Ls, 3) };
    g_camera->lookAtY(t);
    return 0;
}

// Rotation about the camera's OWN axes rather than the world's — the natural
// control scheme for free-look.
static int c_rotatelocal(lua_State* Ls) {
    g_camera->rotateLocal((float)luaL_checknumber(Ls, 1),
                          (float)luaL_checknumber(Ls, 2),
                          (float)luaL_checknumber(Ls, 3));
    return 0;
}

static const luaL_Reg camera_funcs[] = {
    { "position",       c_position       },
    { "translate",      c_translate      },
    { "translatelocal", c_translatelocal },
    { "rotation",       c_rotation       },
    { "rotate",         c_rotate         },
    { "rotatelocal",    c_rotatelocal    },
    { "fov",            c_fov            },
    { "clip",           c_clip           },
    { "lookat",         c_lookat         },
    { "lookatx",        c_lookatx        },
    { "lookaty",        c_lookaty        },
    { nullptr, nullptr }
};

// ---------------------------------------------------------------------------
// jet.scene
// ---------------------------------------------------------------------------

static int s_backcolor(lua_State* Ls) {
    g_scene->setBackcolor((uint16_t)luaL_checkinteger(Ls, 1));
    return 0;
}

static int s_clear(lua_State* Ls) {
    g_scene->setClearBuffer(lua_toboolean(Ls, 1) != 0);
    return 0;
}

static inline uint8_t argChannel(lua_State* Ls, int idx) {
    lua_Integer v = luaL_checkinteger(Ls, idx);
    if (v < 0) v = 0; if (v > 255) v = 255;
    return (uint8_t)v;
}

static int s_ambient(lua_State* Ls) {
    g_ambLight->color = { argChannel(Ls, 1), argChannel(Ls, 2), argChannel(Ls, 3) };
    return 0;
}

// azimuth/elevation in degrees, colour as three 0..255 channels.
static int s_light(lua_State* Ls) {
    lua_Number az = luaL_checknumber(Ls, 1);
    lua_Number el = luaL_checknumber(Ls, 2);
    g_dirLight->updateDirection({ toFixed(az), toFixed(el), 0 });
    if (!lua_isnoneornil(Ls, 3)) {
        g_dirLight->color = { argChannel(Ls, 3), argChannel(Ls, 4), argChannel(Ls, 5) };
    }
    return 0;
}

// jet.scene.sky(topColor, bottomColor) — per-row vertical gradient used by the
// clear instead of the flat backcolor. jet.scene.sky() reverts to backcolor.
//
// NOTE deliberately absent: Scene::addPointLight. Verified before binding:
// pointLights is pushed to and never read anywhere in Scene.cpp/Renderer.cpp,
// so exposing it would create a silent no-op API — the same trap Object::scale
// was before the transform patch.
// backgroundGradientColors is indexed by SCREEN ROW, so a fixed table is
// screen-space and rides along with the camera — the horizon stays glued to the
// middle of the display however you pitch. It cannot be world-anchored as-is.
//
// Fix: build the gradient over THREE screens' worth of rows and hand the clear
// a pointer into it, offset by camera pitch. Looking up shows rows nearer the
// zenith (horizon slides down the screen); looking down does the reverse. Yaw
// cannot affect a purely horizontal gradient, so pitch is the entire cue, and
// the per-frame cost is one pointer add.
//
// Deliberately absent: Scene::addPointLight. Verified before binding —
// pointLights is pushed to and never read in Scene.cpp/Renderer.cpp, so
// exposing it would be a silent no-op API, the trap Object::scale was.
static uint16_t* g_skyGradient = nullptr;   // 3 * screenH rows, zenith -> nadir
static bool      g_skyActive   = false;

static int s_sky(lua_State* Ls) {
    Scene* sc = g_scene;
    if (lua_isnoneornil(Ls, 1)) {
        sc->backgroundGradientColors = nullptr;
        g_skyActive = false;
        return 0;
    }
    const uint16_t top = (uint16_t)luaL_checkinteger(Ls, 1);
    const uint16_t bot = (uint16_t)luaL_checkinteger(Ls, 2);

    const int rows = g_screenH * 3;
    if (!g_skyGradient)
        g_skyGradient = (uint16_t*)malloc(sizeof(uint16_t) * (size_t)rows);
    if (!g_skyGradient) return luaL_error(Ls, "sky: out of memory");

    const int tr = (top >> 11) & 0x1F, tg = (top >> 5) & 0x3F, tb = top & 0x1F;
    const int br = (bot >> 11) & 0x1F, bg = (bot >> 5) & 0x3F, bb = bot & 0x1F;
    const int denom = rows > 1 ? rows - 1 : 1;
    for (int y = 0; y < rows; ++y) {
        const int r = tr + ((br - tr) * y) / denom;
        const int g = tg + ((bg - tg) * y) / denom;
        const int b = tb + ((bb - tb) * y) / denom;
        g_skyGradient[y] = (uint16_t)((r << 11) | (g << 5) | b);
    }
    g_skyActive = true;
    return 0;   // window position is set per frame by jet_sky_update()
}

// Repoint the clear's gradient window from the camera's pitch. Called once per
// frame before rendering.
static void jet_sky_update(void) {
    if (!g_skyActive || !g_skyGradient || !g_scene || !g_camera) return;

    // Camera::rotation.x is radians under FLOAT_CAMERA_ANGLES.
    const float pitchDeg = (float)(g_camera->rotation.x / JET_DEG2RAD);

    // Vertical FOV from the horizontal one the camera was built with, then
    // rows per degree. Recomputed each frame so changing fov stays correct.
    // atan2f(y,1) rather than atanf: atanf is not among the host exports, and
    // the build-time UND audit is what caught it.
    const float hFovRad = (float)g_camera->fov * (float)JET_DEG2RAD;
    const float vFovRad = 2.0f * atan2f(tanf(hFovRad * 0.5f) *
                                        (float)g_screenH / (float)g_screenW, 1.0f);
    const float vFovDeg = vFovRad / (float)JET_DEG2RAD;
    const float rowsPerDeg = (vFovDeg > 1.0f) ? (float)g_screenH / vFovDeg : 1.0f;

    // Pitch 0 centres the horizon: the middle screen of the three.
    int off = g_screenH - (int)(pitchDeg * rowsPerDeg + 0.5f);
    if (off < 0) off = 0;
    if (off > g_screenH * 2) off = g_screenH * 2;

    g_scene->backgroundGradientColors = g_skyGradient + off;

    // Point the atmospheric haze at the SAME window. Distant geometry then
    // blends toward the exact sky colour standing behind it, and the two track
    // together automatically when the camera pitches.
    if (Rasterizer* r = g_scene->getRenderer()) {
        if (r->atmosEnabled) {
            r->atmosGradient     = g_scene->backgroundGradientColors;
            r->atmosGradientRows = g_screenH;
        }
    }
}

// jet.scene.stats() -> objects, triangles, vertices,   (scene totals)
//                      drawnObjects, queuedTris, rasterTris   (last frame)
//
// The last-frame trio is what culling effectiveness is measured with: at world
// scale the question is not how big the scene is but how much of it survives
// the frustum test each frame. Upstream already tracked these; they were just
// never reachable from a game.
static int s_stats(lua_State* Ls) {
    int objects = 0, triangles = 0, vertices = 0;
    g_scene->getStatistics(objects, triangles, vertices);
    lua_pushinteger(Ls, objects);
    lua_pushinteger(Ls, triangles);
    lua_pushinteger(Ls, vertices);
    lua_pushinteger(Ls, g_scene->lastFrameDrawnObjects);
    lua_pushinteger(Ls, g_scene->lastFrameDrawnTriangles);
    lua_pushinteger(Ls, g_scene->lastFrameRasterizedTriangles);
    return 6;
}

// jet.scene.fog(near, far) in world units; jet.scene.fog() disables.
//
// DEPTH_ALPHA_BLEND fades distant geometry toward the backcolor, but its range
// came from the compile-time depthFogNear/depthFogFar macros, which cannot track
// a camera far plane set from Lua. With the default macros sitting beyond the
// far plane the fade could never trigger, so the feature was dead code.
// Launcher override (-fog 0): fog stays off no matter what the game asks for,
// so distance behaviour is observable without fog hiding it.
static bool g_fogEnabled = true;
void jet_lua_set_fog_enabled(int on) { g_fogEnabled = (on != 0); }

static int s_fog(lua_State* Ls) {
    if (!g_fogEnabled) { g_scene->clearFog(); return 0; }
    if (lua_isnoneornil(Ls, 1)) { g_scene->clearFog(); return 0; }
    g_scene->setFog(argWorld(Ls, 1), argWorld(Ls, 2));
    return 0;
}

// jet.scene.haze(near, far [, maxfade] [, color]) — atmospheric perspective.
// jet.scene.haze() disables.
//
// This is NOT jet.scene.fog(). Fog drops the triangle's ALPHA, so distant
// geometry dissolves toward whatever is behind it and loses the span-fill fast
// path on the way. Haze blends the triangle's COLOUR toward the sky standing
// behind it and leaves it fully opaque, so it is both cheaper and closer to
// real aerial perspective. When jet.scene.sky() is active the haze colour is
// sampled from that same gradient per triangle, giving the vertical variation
// a single fog colour cannot.
static int s_haze(lua_State* Ls) {
    Rasterizer* r = g_scene->getRenderer();
    if (!r) return 0;
    if (lua_isnoneornil(Ls, 1)) {
        r->atmosEnabled = false;
        r->atmosGradient = nullptr;
        r->atmosGradientRows = 0;
        return 0;
    }
    r->atmosNear = argWorld(Ls, 1);
    r->atmosFar  = argWorld(Ls, 2);
    r->atmosMax  = (uint8_t)luaL_optinteger(Ls, 3, 255);
    r->atmosColor = (uint16_t)luaL_optinteger(Ls, 4, 0xC618);
    r->atmosEnabled = (r->atmosFar > r->atmosNear);
    // jet_sky_update() repoints the gradient each frame; seed it now so a
    // single-frame render before the first update still has something sane.
    r->atmosGradient     = g_scene->backgroundGradientColors;
    r->atmosGradientRows = g_screenH;
    return 0;
}

// jet.scene.texlod(near, far) — drop diffuse textures with distance.
// jet.scene.texlod() disables.
//
// Past `far` the texture is dropped entirely, the triangle renders as flat
// material colour, and it is PROMOTED BACK ONTO THE SPAN-FILL FAST PATH.
// Between near and far the texel cross-fades toward that colour.
//
// This costs nothing visually when the texture is already below the screen's
// sampling rate — measured on hardware from two captures at an identical
// camera: textures on vs off were indistinguishable in the image and 55,410 vs
// 11,250 us of raster. A texture finer than a pixel is 4.9x the fill cost for
// an average colour the flat path produces for free.
static int s_texlod(lua_State* Ls) {
    Rasterizer* r = g_scene->getRenderer();
    if (!r) return 0;
    if (lua_isnoneornil(Ls, 1)) { r->textureLodEnabled = false; return 0; }
    r->textureLodNear = argWorld(Ls, 1);
    r->textureLodFar  = argWorld(Ls, 2);
    r->textureLodEnabled = (r->textureLodFar > r->textureLodNear);
    return 0;
}

// jet.scene.lod(unitsPerLevel [, bias]) — world-units-per-LOD-step for the
// upstream per-object chain; 0 (or no args) disables. An object at distance d
// draws lodMeshes[d / units - 1], clamped per its own lodPersist/lodBias.
static int s_lod(lua_State* Ls) {
    const lua_Number units = luaL_optnumber(Ls, 1, 0);
    g_scene->lodScale = (int32_t)(units * JET32_WORLD_SCALE);
    g_scene->lodBias  = (int8_t)luaL_optinteger(Ls, 2, 0);
    return 0;
}

static const luaL_Reg scene_funcs[] = {
    { "lod",       s_lod       },
    { "fog",       s_fog       },
    { "haze",      s_haze      },
    { "texlod",    s_texlod    },
    { "sky",       s_sky       },
    { "backcolor", s_backcolor },
    { "clear",     s_clear     },
    { "ambient",   s_ambient   },
    { "light",     s_light     },
    { "stats",     s_stats     },
    { nullptr, nullptr }
};

// ---------------------------------------------------------------------------
// jet.input
// ---------------------------------------------------------------------------

// Accepts a single-character string ("w"), a name ("up", "enter") or a raw
// numeric host key code.
static int keyCodeFromArg(lua_State* Ls, int idx) {
    if (lua_isnumber(Ls, idx)) return (int)lua_tointeger(Ls, idx) & 0xFF;
    size_t len = 0;
    const char* s = luaL_checklstring(Ls, idx, &len);
    if (len == 1) return (unsigned char)s[0];
    struct { const char* name; int code; } names[] = {
        { "up",    0x81 }, { "down",  0x82 }, { "left",  0x83 }, { "right", 0x84 },
        { "click", 0x85 }, { "shift", 0x80 }, { "enter", 0x0D }, { "space", 0x20 },
        { "back",  0x08 }, { "esc",   0x1B }, { "tab",   0x09 },
    };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        if (strcmp(s, names[i].name) == 0) return names[i].code;
    }
    return -1;
}

static int i_down(lua_State* Ls) {
    int code = keyCodeFromArg(Ls, 1);
    lua_pushboolean(Ls, code >= 0 && g_keyDown[code]);
    return 1;
}

// True on the frame the key went down, including a press+release that both
// landed inside one frame.
static int i_pressed(lua_State* Ls) {
    int code = keyCodeFromArg(Ls, 1);
    lua_pushboolean(Ls, code >= 0 && g_keyHit[code]);
    return 1;
}

static int i_released(lua_State* Ls) {
    int code = keyCodeFromArg(Ls, 1);
    lua_pushboolean(Ls, code >= 0 && g_keyRel[code]);
    return 1;
}

// Any key newly pressed this frame — for "press any key to continue".
static int i_anypressed(lua_State* Ls) {
    for (int i = 0; i < 256; ++i) {
        if (g_keyHit[i]) { lua_pushinteger(Ls, i); return 1; }
    }
    lua_pushnil(Ls);
    return 1;
}

// Returns accumulated dx, dy and click count since the previous call, then
// resets the accumulators.
static int i_trackball(lua_State* Ls) {
    lua_pushinteger(Ls, g_trkDx);
    lua_pushinteger(Ls, g_trkDy);
    lua_pushinteger(Ls, g_trkClick);
    g_trkDx = g_trkDy = g_trkClick = 0;
    return 3;
}

static const luaL_Reg input_funcs[] = {
    { "down",       i_down       },
    { "pressed",    i_pressed    },
    { "released",   i_released   },
    { "anypressed", i_anypressed },
    { "trackball",  i_trackball  },
    { nullptr, nullptr }
};

// ---------------------------------------------------------------------------
// jet.sprite — Jet's Sprite2D: screen-space textured/solid quads
// ---------------------------------------------------------------------------
// Sits UNDER the jet.text/jet.rect overlay (sprites draw in the band pass
// before the overlay replay) and OVER the 3D scene. Use it for HUD art,
// portraits, backgrounds; use the overlay for text and plain fills. Sprites
// persist frame to frame like scene objects — the overlay is re-recorded every
// frame. Coordinates are SCREEN pixels.

#define MT_SPRITE "jet.sprite"


static Sprite2D* checkSprite(lua_State* Ls, int idx) {
    Sprite2D** ud = (Sprite2D**)luaL_checkudata(Ls, idx, MT_SPRITE);
    if (!*ud) luaL_error(Ls, "sprite has been destroyed");
    return *ud;
}

// jet.sprite{ x=,y=, texture=|w=,h=,color=, alpha=, z=, add=, scale= }
static int l_sprite(lua_State* Ls) {
    luaL_checktype(Ls, 1, LUA_TTABLE);

    Sprite2D* sp = new Sprite2D();
    sp->x = (int)optField(Ls, 1, "x", 0);
    sp->y = (int)optField(Ls, 1, "y", 0);

    // A sprite needs a Material; build a private one the same way the
    // primitives do. Textured sprites take dimensions from the texture.
    Material* m;
    lua_getfield(Ls, 1, "texture");
    if (!lua_isnil(Ls, -1)) {
        Texture** ud = (Texture**)luaL_checkudata(Ls, -1, MT_TEXTURE);
        m = new Material(0xFFFF, *ud);
    } else {
        m = new Material((uint16_t)(int)optField(Ls, 1, "color", 0xFFFF));
        sp->width  = (int)optField(Ls, 1, "w", 8);
        sp->height = (int)optField(Ls, 1, "h", 8);
    }
    lua_pop(Ls, 1);
    m->alpha = (uint8_t)(int)optField(Ls, 1, "alpha", 255);
    g_materials.push_back(m);
    sp->material = m;

    sp->zOrder = (int)optField(Ls, 1, "z", 0);
    sp->scale  = (int)optField(Ls, 1, "scale", 1);
    if (optFieldBool(Ls, 1, "add", 0)) sp->blendMode = BlendMode::BLEND_ADD;

    g_sprites.push_back(sp);
    g_scene->addSprite(sp);

    Sprite2D** ud = (Sprite2D**)lua_newuserdatauv(Ls, sizeof(Sprite2D*), 0);
    *ud = sp;
    luaL_getmetatable(Ls, MT_SPRITE);
    lua_setmetatable(Ls, -2);
    return 1;
}

static int sp_position(lua_State* Ls) {
    Sprite2D* sp = checkSprite(Ls, 1);
    if (lua_isnoneornil(Ls, 2)) {
        lua_pushinteger(Ls, sp->x);
        lua_pushinteger(Ls, sp->y);
        return 2;
    }
    sp->x = (int)luaL_checkinteger(Ls, 2);
    sp->y = (int)luaL_checkinteger(Ls, 3);
    lua_settop(Ls, 1);
    return 1;
}

static int sp_alpha(lua_State* Ls) {
    Sprite2D* sp = checkSprite(Ls, 1);
    if (lua_isnoneornil(Ls, 2)) { lua_pushinteger(Ls, sp->alpha); return 1; }
    int a = (int)luaL_checkinteger(Ls, 2);
    if (a < 0) a = 0; if (a > 255) a = 255;
    sp->alpha = (uint8_t)a;
    lua_settop(Ls, 1);
    return 1;
}

static int sp_enabled(lua_State* Ls) {
    Sprite2D* sp = checkSprite(Ls, 1);
    if (lua_isnoneornil(Ls, 2)) { lua_pushboolean(Ls, sp->enabled); return 1; }
    sp->enabled = lua_toboolean(Ls, 2) != 0;
    lua_settop(Ls, 1);
    return 1;
}

static int sp_destroy(lua_State* Ls) {
    Sprite2D** ud = (Sprite2D**)luaL_checkudata(Ls, 1, MT_SPRITE);
    Sprite2D* sp = *ud;
    if (!sp) return 0;
    if (g_scene) {
        std::vector<Sprite2D*>& list = g_scene->getSprites();
        for (size_t i = 0; i < list.size(); ++i)
            if (list[i] == sp) { list.erase(list.begin() + i); break; }
    }
    for (size_t i = 0; i < g_sprites.size(); ++i)
        if (g_sprites[i] == sp) { g_sprites.erase(g_sprites.begin() + i); break; }
    delete sp;   // its Material stays in g_materials, freed at close
    *ud = nullptr;
    return 0;
}

static const luaL_Reg sprite_methods[] = {
    { "position", sp_position },
    { "alpha",    sp_alpha    },
    { "enabled",  sp_enabled  },
    { "destroy",  sp_destroy  },
    { nullptr, nullptr }
};

// ---------------------------------------------------------------------------
// jet.sound
// ---------------------------------------------------------------------------
// Clips are raw signed-16-bit mono PCM at jet.sound.RATE (22050), matching the
// headerless convention the textures use. play/tone return a voice id that
// carries a generation counter, so holding one past the sound's end and calling
// stop() on it cannot silence a different sound that reused the slot.

static int snd_load(lua_State* Ls) {
    const int id = jet_audio_load(luaL_checkstring(Ls, 1));
    if (id < 0) { lua_pushnil(Ls); return 1; }
    lua_pushinteger(Ls, id);
    return 1;
}

static int snd_play(lua_State* Ls) {
    const int clip = (int)luaL_checkinteger(Ls, 1);
    const int v = jet_audio_play(clip,
                                 (float)optField(Ls, 2, "volume", 1.0),
                                 optFieldBool(Ls, 2, "loop", 0),
                                 (float)optField(Ls, 2, "pitch", 1.0));
    if (v < 0) { lua_pushnil(Ls); return 1; }
    lua_pushinteger(Ls, v);
    return 1;
}

static int snd_tone(lua_State* Ls) {
    const int v = jet_audio_tone((float)luaL_checknumber(Ls, 1),
                                 (int)luaL_checkinteger(Ls, 2),
                                 (float)optField(Ls, 3, "volume", 1.0),
                                 (int)optField(Ls, 3, "wave", JET_WAVE_SQUARE));
    lua_pushinteger(Ls, v);
    return 1;
}

static int snd_stop(lua_State* Ls) {
    jet_audio_stop((int)luaL_checkinteger(Ls, 1));
    return 0;
}

static int snd_stopall(lua_State* Ls) { (void)Ls; jet_audio_stop_all(); return 0; }

static int snd_volume(lua_State* Ls) {
    jet_audio_master((float)luaL_checknumber(Ls, 1));
    return 0;
}

static int snd_active(lua_State* Ls) {
    lua_pushinteger(Ls, jet_audio_active());
    return 1;
}

static const luaL_Reg sound_funcs[] = {
    { "load",    snd_load    },
    { "play",    snd_play    },
    { "tone",    snd_tone    },
    { "stop",    snd_stop    },
    { "stopall", snd_stopall },
    { "volume",  snd_volume  },
    { "active",  snd_active  },
    { nullptr, nullptr }
};

// ---------------------------------------------------------------------------
// jet.builder / jet.mesh — composed meshes, baked and loaded
// ---------------------------------------------------------------------------
// A builder accumulates transformed primitives; bake() welds and produces one
// scene Object, save() writes a .jmsh, jet.mesh() loads one. Part methods
// mirror the jet.* primitive constructors with a trailing transform table
// {x,y,z, rx,ry,rz, s or sx,sy,sz} and return the builder for chaining.
//
// The builder userdata OWNS its JetMeshBuilder (unlike object/material
// handles, which are non-owning) — __gc frees it, so an abandoned builder
// costs nothing after collection.

static JetMeshBuilder* checkBuilder(lua_State* Ls, int idx) {
    JetMeshBuilder** ud = (JetMeshBuilder**)luaL_checkudata(Ls, idx, MT_BUILDER);
    if (!*ud) luaL_error(Ls, "builder has been collected");
    return *ud;
}

static JetMeshBuilder::Transform parseXform(lua_State* Ls, int idx) {
    JetMeshBuilder::Transform t;
    if (!lua_istable(Ls, idx)) return t;
    // Translation is world-space and gets the same scale argWorld applies.
    t.tx = (float)(optField(Ls, idx, "x", 0) * JET32_WORLD_SCALE);
    t.ty = (float)(optField(Ls, idx, "y", 0) * JET32_WORLD_SCALE);
    t.tz = (float)(optField(Ls, idx, "z", 0) * JET32_WORLD_SCALE);
    t.rx = (float)optField(Ls, idx, "rx", 0);
    t.ry = (float)optField(Ls, idx, "ry", 0);
    t.rz = (float)optField(Ls, idx, "rz", 0);
    const lua_Number s = optField(Ls, idx, "s", 1);
    t.sx = (float)optField(Ls, idx, "sx", s);
    t.sy = (float)optField(Ls, idx, "sy", s);
    t.sz = (float)optField(Ls, idx, "sz", s);
    return t;
}

// A world built from many tiles bakes hundreds of meshes, and one log line per
// bake floods the serial output that the run is being judged from. Print the
// first few, then go quiet with a count — the interesting information (the
// pipeline is working, what it dropped) is in the first few either way.
static int  g_bakeLogs = 0;
static bool bakeLogWanted() {
    if (g_bakeLogs < 6) { ++g_bakeLogs; return true; }
    if (g_bakeLogs == 6) {
        ++g_bakeLogs;
        logf_("[mesh] (further per-bake logs suppressed this run)");
    }
    return false;
}

static uint32_t bld_hashName(const char* s) {
    uint32_t h = 2166136261u;
    while (*s) h = (h ^ (uint8_t)*s++) * 16777619u;
    return h;
}

// Shared tail for every part method: record the part in the recipe and return
// the builder itself so parts chain. Nothing is evaluated until bake/save —
// the recipe is the source of truth (see jet_mesh.h).
//
// Rigging options ride on the same trailing table as the transform:
//   part="name"        assign this part to a bone (created on first mention)
//   parent="name"      the new bone's parent — must already exist, so parents
//                      always precede children and composition can run in
//                      array order
//   pivot={x,y,z}      the bone's rotation origin (authored units); defaults
//                      to the part's own translation on first mention
// Bone 0 is the implicit root that unnamed parts belong to. A recipe with any
// named part saves as a RIGGED file (PART section, bone-local geometry).
static int bld_finish(lua_State* Ls, JetMeshBuilder::Part& p, int xfIdx) {
    JetMeshBuilder* b = checkBuilder(Ls, 1);
    p.xf = parseXform(Ls, xfIdx);

    if (b->bones.empty()) b->bones.push_back(JetMeshBuilder::Bone());
    p.bone = 0;

    if (lua_istable(Ls, xfIdx)) {
        lua_getfield(Ls, xfIdx, "part");
        if (lua_isstring(Ls, -1)) {
            const uint32_t h = bld_hashName(lua_tostring(Ls, -1));
            int bi = -1;
            for (size_t i = 1; i < b->bones.size(); ++i)
                if (b->bones[i].nameHash == h) { bi = (int)i; break; }

            if (bi < 0) {
                if (b->bones.size() >= 16)
                    return luaL_error(Ls, "too many bones (max 16)");
                JetMeshBuilder::Bone bone;
                bone.nameHash = h;
                bone.parent   = 0;   // default: child of the implicit root
                {
                    const char* nm = lua_tostring(Ls, -1);
                    size_t n = strlen(nm);
                    if (n >= sizeof(bone.name)) n = sizeof(bone.name) - 1;
                    memcpy(bone.name, nm, n);
                }

                lua_getfield(Ls, xfIdx, "parent");
                if (lua_isstring(Ls, -1)) {
                    const uint32_t ph = bld_hashName(lua_tostring(Ls, -1));
                    int pi = -1;
                    for (size_t i = 1; i < b->bones.size(); ++i)
                        if (b->bones[i].nameHash == ph) { pi = (int)i; break; }
                    if (pi < 0)
                        return luaL_error(Ls,
                            "parent bone '%s' is not defined yet — declare "
                            "parents before children", lua_tostring(Ls, -1));
                    bone.parent = (int16_t)pi;
                }
                lua_pop(Ls, 1);

                // Pivot: {x,y,z} array in authored units, or the part's own
                // placement when omitted.
                lua_getfield(Ls, xfIdx, "pivot");
                if (lua_istable(Ls, -1)) {
                    for (int k = 1; k <= 3; ++k) {
                        lua_rawgeti(Ls, -1, k);
                        const int32_t v = (int32_t)(lua_tonumber(Ls, -1)
                                                    * JET32_WORLD_SCALE);
                        if (k == 1) bone.px = v;
                        else if (k == 2) bone.py = v;
                        else bone.pz = v;
                        lua_pop(Ls, 1);
                    }
                } else {
                    bone.px = (int32_t)p.xf.tx;
                    bone.py = (int32_t)p.xf.ty;
                    bone.pz = (int32_t)p.xf.tz;
                }
                lua_pop(Ls, 1);

                b->bones.push_back(bone);
                bi = (int)b->bones.size() - 1;
            }
            p.bone = (uint16_t)bi;
        }
        lua_pop(Ls, 1);
    }

    b->addPart(p);
    lua_settop(Ls, 1);
    return 1;
}

static int bld_cube(lua_State* Ls) {
    JetMeshBuilder::Part p; p.type = JetMeshBuilder::PT_CUBE;
    p.a = argWorld(Ls, 2); p.b = argWorld(Ls, 3); p.c = argWorld(Ls, 4);
    p.mat = argMaterial(Ls, 5);
    return bld_finish(Ls, p, 6);
}
static int bld_sphere(lua_State* Ls) {
    JetMeshBuilder::Part p; p.type = JetMeshBuilder::PT_SPHERE;
    p.a = argWorld(Ls, 2); p.b = optInt(Ls, 3, 12);
    p.mat = argMaterial(Ls, 4);
    return bld_finish(Ls, p, 5);
}
static int bld_plane(lua_State* Ls) {
    JetMeshBuilder::Part p; p.type = JetMeshBuilder::PT_PLANE;
    p.a = argWorld(Ls, 2); p.b = argWorld(Ls, 3);
    p.mat = argMaterial(Ls, 4);
    return bld_finish(Ls, p, 5);
}
static int bld_pyramid(lua_State* Ls) {
    JetMeshBuilder::Part p; p.type = JetMeshBuilder::PT_PYRAMID;
    p.a = argWorld(Ls, 2); p.b = argWorld(Ls, 3);
    p.mat = argMaterial(Ls, 4);
    return bld_finish(Ls, p, 5);
}
static int bld_cylinder(lua_State* Ls) {
    JetMeshBuilder::Part p; p.type = JetMeshBuilder::PT_CYLINDER;
    p.a = argWorld(Ls, 2); p.b = argWorld(Ls, 3); p.c = optInt(Ls, 4, 12);
    p.caps = lua_isnoneornil(Ls, 5) ? true : (lua_toboolean(Ls, 5) != 0);
    p.mat = argMaterial(Ls, 6);
    // MESHPUNK: opts.rings — vertical subdivisions of the barrel, independent
    // of `segments`. Omitted (0) keeps the primitive's original behaviour of
    // using segments for both, which costs segments^2*2 triangles. A backdrop
    // ring wants rings = 1: 24 angular segments then cost 48 triangles instead
    // of 1,152.
    p.d = (int32_t)optField(Ls, 7, "rings", 0.0f);
    return bld_finish(Ls, p, 7);
}
static int bld_capsule(lua_State* Ls) {
    JetMeshBuilder::Part p; p.type = JetMeshBuilder::PT_CAPSULE;
    p.a = argWorld(Ls, 2); p.b = argWorld(Ls, 3); p.c = optInt(Ls, 4, 12);
    p.mat = argMaterial(Ls, 5);
    return bld_finish(Ls, p, 6);
}
static int bld_quad(lua_State* Ls) {
    JetMeshBuilder::Part p; p.type = JetMeshBuilder::PT_QUAD;
    p.a = argWorld(Ls, 2); p.b = argWorld(Ls, 3);
    p.mat = argMaterial(Ls, 4);
    return bld_finish(Ls, p, 5);
}
// b:ribbon(points, material [, opts]) — quad strip through arbitrary 3D
// stations (see PT_RIBBON). `points` is a flat array in AUTHORED units, six
// numbers per station: left x,y,z then right x,y,z. opts: closed = true
// wraps the last station back to the first. Transform fields read as usual.
static int bld_ribbon(lua_State* Ls) {
    JetMeshBuilder::Part p; p.type = JetMeshBuilder::PT_RIBBON;
    luaL_checktype(Ls, 2, LUA_TTABLE);
    const int nn = (int)lua_rawlen(Ls, 2);
    if (nn < 12 || nn % 6 != 0)
        return luaL_error(Ls, "ribbon: points must be 6 numbers per station,"
                              " at least 2 stations (got %d values)", nn);
    p.ribbonPts.reserve((size_t)nn);
    for (int i = 1; i <= nn; ++i) {
        lua_rawgeti(Ls, 2, i);
        p.ribbonPts.push_back(toFixed(luaL_checknumber(Ls, -1)
                                      * JET32_WORLD_SCALE));
        lua_pop(Ls, 1);
    }
    p.mat = argMaterial(Ls, 3);
    if (lua_istable(Ls, 4)) {
        lua_getfield(Ls, 4, "closed");
        p.ribbonClosed = lua_toboolean(Ls, -1) != 0;
        lua_pop(Ls, 1);
    }
    return bld_finish(Ls, p, 4);
}
static int bld_grid(lua_State* Ls) {
    JetMeshBuilder::Part p; p.type = JetMeshBuilder::PT_GRID;
    p.a = argWorld(Ls, 2); p.b = argWorld(Ls, 3);
    p.c = argInt(Ls, 4); p.d = argInt(Ls, 5);
    p.mat = argMaterial(Ls, 6);
    p.mat2 = lua_isnoneornil(Ls, 7) ? p.mat : argMaterial(Ls, 7);
    return bld_finish(Ls, p, 8);
}

// b:heightfield(width, depth, cols, rows, material, fn) — a terrain patch.
//
// fn(col, row, x, z) is called once per vertex and returns that vertex's height
// in AUTHORED units; x/z are the vertex's authored offsets from the patch
// centre, so the same function can be sampled by neighbouring patches at their
// own world offsets and the seams will agree.
//
// The samples are STORED IN THE RECIPE, not re-fetched at bake time. evaluate()
// must stay reproducible — releaseEvaluated discards the geometry and a later
// save or rebake reruns the pipeline with no Lua reachable — and storing points
// rather than meshes is the memory win this exists for: 2 bytes per sample
// against ~5 KB for a baked patch.
static int bld_heightfield(lua_State* Ls) {
    JetMeshBuilder::Part p; p.type = JetMeshBuilder::PT_HEIGHTFIELD;
    p.a = argWorld(Ls, 2); p.b = argWorld(Ls, 3);
    p.c = argInt(Ls, 4);   p.d = argInt(Ls, 5);
    p.mat = argMaterial(Ls, 6);
    luaL_checktype(Ls, 7, LUA_TFUNCTION);
    if (p.c < 2 || p.d < 2)
        return luaL_error(Ls, "heightfield: needs at least 2x2 samples");
    if ((int64_t)p.c * p.d > 65536)
        return luaL_error(Ls, "heightfield: %d x %d samples is too many",
                          (int)p.c, (int)p.d);

    p.heights.resize((size_t)p.c * p.d);
    const int32_t hw = p.a / 2, hd = p.b / 2;
    for (int r = 0; r < p.d; ++r) {
        for (int c = 0; c < p.c; ++c) {
            const int32_t x = (int32_t)(((int64_t)c * p.a) / (p.c - 1)) - hw;
            const int32_t z = (int32_t)(((int64_t)r * p.b) / (p.d - 1)) - hd;
            lua_pushvalue(Ls, 7);
            lua_pushinteger(Ls, c);
            lua_pushinteger(Ls, r);
            lua_pushnumber(Ls, (lua_Number)x / JET32_WORLD_SCALE);
            lua_pushnumber(Ls, (lua_Number)z / JET32_WORLD_SCALE);
            lua_call(Ls, 4, 1);
            const lua_Number h = luaL_optnumber(Ls, -1, 0) * JET32_WORLD_SCALE;
            lua_pop(Ls, 1);
            const int32_t hi = (int32_t)(h >= 0 ? h + 0.5 : h - 0.5);
            p.heights[(size_t)r * p.c + c] =
                (int16_t)(hi >  32767 ?  32767 : (hi < -32768 ? -32768 : hi));
        }
    }
    return bld_finish(Ls, p, 8);
}

// b:trifield(...) — identical to b:heightfield but samples on a TRIANGULAR
// lattice: odd rows offset half a cell, rows pulled together by sqrt(3)/2, so
// every interior vertex has six equidistant neighbours and the surface carries
// no diagonal grain. See Part::triRows.
static int bld_trifield(lua_State* Ls) {
    const int r = bld_heightfield(Ls);
    // bld_heightfield already pushed the part; flag the one it just added.
    JetMeshBuilder* b = checkBuilder(Ls, 1);
    if (!b->parts.empty()) b->parts.back().triRows = true;
    return r;
}

// b:landfield(size, samples, material, cx, cz [, opts]) — a terrain tile
// sampled from the procedural land (jet.land) with NO Lua callback.
//
// The patch is built centred on the ORIGIN and sampled around (cx, cz), so the
// caller positions the object and vertex coordinates stay small no matter how
// far out the tile sits. Every level samples the same point function at the
// same absolute positions, so tiles that share an edge share its samples
// exactly and there is nothing to reconcile.
//
// opts: skirt = authored curtain depth (see Part::skirt — this is the LOD
//       crack fix), uv = emit patch-spanning UVs for a baked colour map,
//       tri = triangular lattice. Transform fields are read as usual.
// b:landfield(width, depth, cols, rows, material, cx, cz [, opts])
//
// RECTANGULAR, not square: a level built as an annulus is four long thin strips
// around a centre patch, and a square-only primitive cannot express one.
static int bld_landfield(lua_State* Ls) {
    JetMeshBuilder::Part p; p.type = JetMeshBuilder::PT_HEIGHTFIELD;
    const lua_Number wid = luaL_checknumber(Ls, 2);
    const lua_Number dep = luaL_checknumber(Ls, 3);
    const int cols = (int)luaL_checkinteger(Ls, 4);
    const int rows = (int)luaL_checkinteger(Ls, 5);
    p.a = toFixed(wid * JET32_WORLD_SCALE);
    p.b = toFixed(dep * JET32_WORLD_SCALE);
    p.c = cols; p.d = rows;
    p.mat = argMaterial(Ls, 6);
    const float cx = (float)luaL_checknumber(Ls, 7);
    const float cz = (float)luaL_checknumber(Ls, 8);
    float yoff = 0.0f;
    if (cols < 2 || rows < 2)
        return luaL_error(Ls, "landfield: needs at least 2x2 samples");
    if ((int64_t)cols * rows > 65536)
        return luaL_error(Ls, "landfield: %d x %d samples is too many", cols, rows);

    if (lua_istable(Ls, 9)) {
        lua_getfield(Ls, 9, "skirt");
        if (!lua_isnil(Ls, -1))
            p.skirt = toFixed(luaL_checknumber(Ls, -1) * JET32_WORLD_SCALE);
        lua_pop(Ls, 1);
        lua_getfield(Ls, 9, "uv");
        p.uvSpan = lua_toboolean(Ls, -1) != 0;
        lua_pop(Ls, 1);
        lua_getfield(Ls, 9, "tri");
        p.triRows = lua_toboolean(Ls, -1) != 0;
        lua_pop(Ls, 1);
        // yoffset: baked into the STORED HEIGHTS, not applied as a transform.
        //
        // obj:position() on a celled bake moves only the head — the bake owns
        // every cell and hands Lua the first — so a per-level drop applied that
        // way reached 1 cell of 64 and the other 63 stayed put. Folding it into
        // the samples applies it to every cell by construction.
        yoff = (float)optField(Ls, 9, "yoffset", 0.0);
        // uvrepeat = authored units per texture repeat. Detail size then
        // belongs to the material, not to the tile it lands on.
        lua_getfield(Ls, 9, "uvrepeat");
        if (!lua_isnil(Ls, -1)) {
            p.uvRepeat = toFixed(luaL_checknumber(Ls, -1) * JET32_WORLD_SCALE);
            p.uvSpan = true;
        }
        lua_pop(Ls, 1);
        // uvmix = per-cell rotated/mirrored tile orientation (see Part::uvMix).
        // Only meaningful with uvrepeat.
        lua_getfield(Ls, 9, "uvmix");
        p.uvMix = lua_toboolean(Ls, -1) != 0;
        lua_pop(Ls, 1);
        // smooth = smooth per-vertex ground normals (see Part::smoothNormals);
        // what makes GOURAUD interpolate instead of degenerating to flat.
        lua_getfield(Ls, 9, "smooth");
        p.smoothNormals = lua_toboolean(Ls, -1) != 0;
        lua_pop(Ls, 1);
        // variants = { mat, mat, ... }: per-cell ground material from this
        // list instead of the main material (see Part::groundVariants). The
        // budget that matters is texture cache working set, not switch cost.
        lua_getfield(Ls, 9, "variants");
        if (lua_istable(Ls, -1)) {
            const int vn = (int)lua_rawlen(Ls, -1);
            if (vn > 8) return luaL_error(Ls, "landfield: at most 8 variants");
            for (int vi = 1; vi <= vn; ++vi) {
                lua_rawgeti(Ls, -1, vi);
                Material** ud = (Material**)luaL_checkudata(Ls, -1, MT_MATERIAL);
                p.groundVariants.push_back(*ud);
                lua_pop(Ls, 1);
            }
        }
        lua_pop(Ls, 1);
        // wall = material for faces steeper than wallslope (rise over run).
        lua_getfield(Ls, 9, "wall");
        if (!lua_isnil(Ls, -1)) {
            Material** ud = (Material**)luaL_checkudata(Ls, -1, MT_MATERIAL);
            p.mat2 = *ud;
        }
        lua_pop(Ls, 1);
        if (p.mat2) {
            const double s = optField(Ls, 9, "wallslope", 0.55);
            // A face of slope s has normal.y = 1/sqrt(1+s^2).
            p.wallCos = (int32_t)((double)FIXED_POINT_SCALE
                                  / sqrt(1.0 + s * s));
        }
    }

    // fitstep = the FINEST level's sample spacing, authored units. Makes this
    // patch agree-by-construction with that level: vertices sample the same
    // filtered surface (shared lattice points match exactly) and the fit pass
    // clamps every chord under the fine surface, replacing the global-SINK
    // burial that erased the far relief. fitmargin = safety clearance added
    // only where a clamp fired (float/quantisation slack).
    double fitstep = 0.0, fitmargin = 500.0;
    if (lua_istable(Ls, 9)) {
        fitstep   = optField(Ls, 9, "fitstep", 0.0);
        fitmargin = optField(Ls, 9, "fitmargin", 500.0);
    }

    // Sampled straight into the recipe in one C call. This is the difference
    // between a tile costing microseconds and costing a Lua call per vertex —
    // and it is what makes rebuilding a ring on a cell crossing affordable.
    static std::vector<float> hbuf;
    hbuf.resize((size_t)cols * rows);
    float hlo = 0.0f, hhi = 0.0f;
    jet_land_patch(cx, cz, (float)wid, (float)dep, cols, rows, p.triRows,
                   hbuf.data(), &hlo, &hhi, (float)fitstep);
    if (fitstep > 0.0) {
        jet_land_fit(cx, cz, (float)wid, (float)dep, cols, rows,
                     hbuf.data(), (float)fitstep, (float)fitmargin);
        // The clamp only lowers heights; rescan so the shift pick below sees
        // the true extent.
        hlo = 1e30f; hhi = -1e30f;
        for (float v : hbuf) { if (v < hlo) hlo = v; if (v > hhi) hhi = v; }
    }

    // Pick the smallest shift that fits this patch's OWN extent, so near tiles
    // keep 1/8-unit precision and only a huge coarse tile trades any away.
    {
        float mag = (hhi - yoff) > -(hlo - yoff) ? (hhi - yoff) : -(hlo - yoff);
        mag *= (float)JET32_WORLD_SCALE;
        p.heightShift = 0;
        while (mag > 32767.0f && p.heightShift < 12) {
            mag *= 0.5f;
            ++p.heightShift;
        }
    }

    p.heights.resize((size_t)cols * rows);
    for (size_t i = 0; i < hbuf.size(); ++i) {
        const float hw = (hbuf[i] - yoff) * (float)JET32_WORLD_SCALE
                       / (float)(1 << p.heightShift);
        const int32_t hi = (int32_t)(hw >= 0 ? hw + 0.5f : hw - 0.5f);
        p.heights[i] = (int16_t)(hi > 32767 ? 32767 : (hi < -32768 ? -32768 : hi));
    }
    return bld_finish(Ls, p, 9);
}

static int bld_stats(lua_State* Ls) {
    JetMeshBuilder* b = checkBuilder(Ls, 1);
    // After a bake the geometry has been released, so report the counts the
    // pipeline recorded. Re-evaluating instead would answer with PRE-cull
    // numbers and quietly change what this function has always meant.
    if (b->dirty && b->lastStats.counted) {
        lua_pushinteger(Ls, (lua_Integer)b->lastStats.verts);
        lua_pushinteger(Ls, (lua_Integer)b->lastStats.tris);
        lua_pushinteger(Ls, (lua_Integer)b->lastStats.mats);
        return 3;
    }
    if (b->dirty && !b->evaluate())
        return luaL_error(Ls, "stats: recipe evaluation failed");
    lua_pushinteger(Ls, (lua_Integer)b->verts.size());
    lua_pushinteger(Ls, (lua_Integer)b->tris.size());
    lua_pushinteger(Ls, (lua_Integer)b->matParams.size());
    return 3;
}

// The full bake pipeline: evaluate -> interior cull -> weld. Interior culling
// is ON by default — buried geometry is never wanted, and butted parts are
// protected by the inward epsilon — with {cull=false} as the escape hatch.
// {tolerance=N} is the cull refinement floor in authored units (default 4):
// the skirt of extra triangles along an intersection is about that wide, and
// any seam gap the floor rule leaves is at most half of it. Smaller tolerance
// = cleaner seams but more triangles; it is the quality/size dial.
static bool bld_pipeline(lua_State* Ls, JetMeshBuilder* b, int optIdx) {
    // Record whatever this call supplies into the RECIPE, then run from it.
    // Fields the caller omits keep their previous value, so `b:bake{lod=...}`
    // followed by a plain `b:save(path)` reproduces the same mesh instead of
    // silently writing a file with no LOD sections. See JetMeshBuilder::pipe.
    if (lua_istable(Ls, optIdx)) {
        lua_getfield(Ls, optIdx, "cull");
        if (!lua_isnil(Ls, -1)) b->pipe.cull = lua_toboolean(Ls, -1) != 0;
        lua_pop(Ls, 1);

        lua_getfield(Ls, optIdx, "tolerance");
        if (lua_isnumber(Ls, -1))
            b->pipe.toleranceAuthored = (float)lua_tonumber(Ls, -1);
        lua_pop(Ls, 1);

        // {maxedge=N} caps triangle size (authored units). FAST_Z gives each
        // triangle ONE sort depth, so a large triangle at a glancing angle is
        // mis-sorted against small objects near it — a floor tile painting over
        // a character standing on it. Bounding the size bounds that error.
        lua_getfield(Ls, optIdx, "maxedge");
        if (lua_isnumber(Ls, -1))
            b->pipe.maxEdgeAuthored = (float)lua_tonumber(Ls, -1);
        lua_pop(Ls, 1);

        // {cells=N} splits a large static mesh into N-unit spatial cells, each
        // its own Object with a tight AABB — so the frustum cull rejects whole
        // regions instead of transforming every vertex of the mesh. Static
        // world geometry only: a celled mesh is placed at BAKE time and is not
        // transformable as one unit afterwards.
        lua_getfield(Ls, optIdx, "cells");
        if (lua_isnumber(Ls, -1))
            b->pipe.cellSizeAuthored = (float)lua_tonumber(Ls, -1);
        lua_pop(Ls, 1);

        // cells2d = true: split in X/Z only. Mandatory for terrain — see
        // Pipeline::cellSplit2D for why a vertical split breaks every
        // distance-based band downstream.
        b->pipe.cellSplit2D = optFieldBool(Ls, optIdx, "cells2d", 0) != 0;

        // {lod = {{segs=0.5, minpart=20}, {segs=0.25, minpart=60}}} — each
        // entry re-evaluates the recipe with segment counts scaled by `segs`
        // and parts thinner than `minpart` (authored units) dropped.
        lua_getfield(Ls, optIdx, "lod");
        if (lua_istable(Ls, -1)) {
            b->pipe.lodSpecs.clear();
            const int n = (int)lua_rawlen(Ls, -1);
            for (int i = 1; i <= n && i <= 4; ++i) {
                lua_rawgeti(Ls, -1, i);
                if (lua_istable(Ls, -1)) {
                    JetMeshBuilder::LodSpec spec;
                    lua_getfield(Ls, -1, "segs");
                    if (lua_isnumber(Ls, -1))
                        spec.segScale = (float)lua_tonumber(Ls, -1);
                    lua_pop(Ls, 1);
                    lua_getfield(Ls, -1, "minpart");
                    if (lua_isnumber(Ls, -1))
                        spec.minPart = (int32_t)(lua_tonumber(Ls, -1)
                                                 * JET32_WORLD_SCALE);
                    lua_pop(Ls, 1);
                    b->pipe.lodSpecs.push_back(spec);
                }
                lua_pop(Ls, 1);
            }
        }
        lua_pop(Ls, 1);
    }

    return b->run((float)JET32_WORLD_SCALE);
}

static int bld_bake(lua_State* Ls) {
    JetMeshBuilder* b = checkBuilder(Ls, 1);
    if (!bld_pipeline(Ls, b, 2))
        return luaL_error(Ls, "bake: recipe evaluation failed");

    // {sortdepth=} is a bake option rather than only an Object method because a
    // celled bake owns every cell and hands Lua a handle on just the first —
    // the policy has to reach all of them.
    SortDepth bakeSort = SortDepth::AVERAGE;
    if (lua_istable(Ls, 2)) {
        lua_getfield(Ls, 2, "sortdepth");
        if (lua_isnumber(Ls, -1))
            bakeSort = (SortDepth)(int)lua_tointeger(Ls, -1);
        lua_pop(Ls, 1);
    }

    // {fade={near,far}} / {appear={near,far}} are BAKE options for the same
    // reason sortdepth is: a celled bake owns every cell and hands Lua a handle
    // on just the first, so obj:fade() could only ever reach one of them. A
    // distance band has to apply to the whole surface or the level tears.
    int32_t fadeN = 0, fadeF = 0, appN = 0, appF = 0;
    auto pairOpt = [&](const char* key, int32_t& a, int32_t& bb) {
        if (!lua_istable(Ls, 2)) return;
        lua_getfield(Ls, 2, key);
        if (lua_istable(Ls, -1)) {
            lua_rawgeti(Ls, -1, 1);
            a = (int32_t)(luaL_optnumber(Ls, -1, 0) * JET32_WORLD_SCALE);
            lua_pop(Ls, 1);
            lua_rawgeti(Ls, -1, 2);
            bb = (int32_t)(luaL_optnumber(Ls, -1, 0) * JET32_WORLD_SCALE);
            lua_pop(Ls, 1);
        }
        lua_pop(Ls, 1);
    };
    pairOpt("fade", fadeN, fadeF);
    pairOpt("appear", appN, appF);
    auto applyBands = [&](Object* o) {
        o->fadeNear = fadeN; o->fadeFar = fadeF;
        o->appearNear = appN; o->appearFar = appF;
    };

    // Celled bakes produce one Object per cell; the first is returned and the
    // rest are scene-resident siblings (static world geometry).
    if (!b->cells.empty()) {
        Object* head = nullptr;
        std::vector<Object*> made;
        for (size_t c = 0; c < b->cells.size(); ++c) {
            Object* co = b->build((int)c);
            if (!co) break;
            co->sortDepth = bakeSort;
            applyBands(co);
            if (optFieldBool(Ls, 2, "light", 0))
                co->bakeFlatLighting(g_dirLight, g_ambLight);
            g_objects.push_back(co);
            g_scene->addObject(co);
            made.push_back(co);
            if (!head) head = co;
        }
        if (!head) return luaL_error(Ls, "bake: cell build failed");
        registerGroup(head, made);
        if (bakeLogWanted())
        logf_("[mesh] bake: %u verts, %u tris, %u cells, %u materials",
              (unsigned)b->verts.size(), (unsigned)b->tris.size(),
              (unsigned)b->cells.size(), (unsigned)b->matParams.size());
        b->releaseEvaluated();
        pushObject(Ls, head);
        return 1;
    }

    Object* o = b->build();
    if (!o) return luaL_error(Ls, "bake: builder is empty or exceeds 65535 vertices");
    o->sortDepth = bakeSort;
    applyBands(o);

    if (optFieldBool(Ls, 2, "light", 0)) {
        o->bakeFlatLighting(g_dirLight, g_ambLight);
    }

    // Wire baked LOD levels: mesh-only Objects owned by the registry (freed
    // at close), never added to the scene — the head draws with their data
    // when Scene::lodScale picks a level.
    std::vector<Object*> made;
    for (size_t i = 0; i < b->lods.size(); ++i) {
        Object* lo = b->buildLod(i);
        if (!lo) break;
        g_objects.push_back(lo);
        o->lodMeshes.push_back(lo);
        made.push_back(lo);
    }

    g_objects.push_back(o);
    g_scene->addObject(o);
    made.push_back(o);          // head last: it owns any shared allocation
    registerGroup(o, made);
    if (bakeLogWanted())
    logf_("[mesh] bake: %u verts (%d welded), %u tris (%d buried dropped,"
          " %d split-added), %u materials, %u lods",
          (unsigned)b->verts.size(), b->lastStats.vertsWelded,
          (unsigned)b->tris.size(), b->lastStats.trisDropped,
          b->lastStats.trisAdded, (unsigned)b->matParams.size(),
          (unsigned)b->lods.size());
    // The evaluated geometry is dead now: build() copied everything into owned
    // Objects and save() reruns the pipeline. Holding it is what exhausted
    // PSRAM on a 144-tile world — a builder userdata is one pointer, so Lua's
    // GC never feels the megabytes behind it. See JetMeshBuilder::pipe.
    b->releaseEvaluated();
    pushObject(Ls, o);
    return 1;
}

static int bld_save(lua_State* Ls) {
    JetMeshBuilder* b = checkBuilder(Ls, 1);
    const char* path = luaL_checkstring(Ls, 2);
    if (!bld_pipeline(Ls, b, 3))
        return luaL_error(Ls, "save: recipe evaluation failed");
    const bool ok = b->save(path, (uint8_t)JET32_WORLD_SCALE);
    if (bakeLogWanted())
    logf_("[mesh] save %s: %s (%u verts, %u tris, %d buried dropped)", path,
          ok ? "ok" : "FAILED",
          (unsigned)b->verts.size(), (unsigned)b->tris.size(),
          b->lastStats.trisDropped);
    b->releaseEvaluated();
    lua_pushboolean(Ls, ok);
    return 1;
}

// b:clip(name, rate, seconds, fn) — bake an animation clip by SAMPLING a Lua
// animator: fn(t, pose) is called once per key with `pose` pre-filled with an
// identity entry per named bone ({x,y,z, rx,ry,rz}, authored units / degrees);
// whatever the function leaves in the table becomes that key. Animations are
// authored as code and shipped as data, the same philosophy as the meshes.
static int bld_clip(lua_State* Ls) {
    JetMeshBuilder* b = checkBuilder(Ls, 1);
    const char* name  = luaL_checkstring(Ls, 2);
    const int   rate  = (int)luaL_checkinteger(Ls, 3);
    const float secs  = (float)luaL_checknumber(Ls, 4);
    luaL_checktype(Ls, 5, LUA_TFUNCTION);
    if (!b->rigged())
        return luaL_error(Ls, "clip: recipe has no named parts to animate");
    if (rate < 1 || rate > 60 || secs <= 0)
        return luaL_error(Ls, "clip: bad rate/length");

    int keyCount = (int)(rate * secs) + 1;
    if (keyCount < 2) keyCount = 2;
    if (keyCount > 512) return luaL_error(Ls, "clip: too many keys");

    const size_t nBones = b->bones.size();
    JetMeshBuilder::Clip clip;
    clip.nameHash = bld_hashName(name);
    clip.rate     = (uint16_t)rate;
    clip.keyCount = (uint16_t)keyCount;
    clip.keys.reserve((size_t)keyCount * nBones);

    const float DEG = 0.017453292519943295f;
    for (int k = 0; k < keyCount; ++k) {
        // Fresh pose table with an identity entry per named bone.
        lua_pushvalue(Ls, 5);
        lua_pushnumber(Ls, (lua_Number)k / rate);
        lua_createtable(Ls, 0, (int)nBones - 1);
        for (size_t i = 1; i < nBones; ++i) {
            lua_createtable(Ls, 0, 6);
            static const char* f6[6] = { "x","y","z","rx","ry","rz" };
            for (int fi = 0; fi < 6; ++fi) {
                lua_pushnumber(Ls, 0);
                lua_setfield(Ls, -2, f6[fi]);
            }
            lua_setfield(Ls, -2, b->bones[i].name);
        }
        lua_pushvalue(Ls, -1);          // keep the pose table for readback
        lua_insert(Ls, -4);             // [pose, fn, t, pose]
        if (lua_pcall(Ls, 2, 0, 0) != LUA_OK) {
            const char* err = lua_tostring(Ls, -1);
            return luaL_error(Ls, "clip '%s' key %d: %s", name, k,
                              err ? err : "error");
        }
        // Read the pose back; bone 0 (implicit root) is always identity.
        for (size_t i = 0; i < nBones; ++i) {
            JetMeshBuilder::ClipKey key = {};
            key.qw = 16384;   // identity quaternion at Q1.14
            if (i > 0) {
                lua_getfield(Ls, -1, b->bones[i].name);
                if (lua_istable(Ls, -1)) {
                    float v[6];
                    static const char* f6[6] = { "x","y","z","rx","ry","rz" };
                    for (int fi = 0; fi < 6; ++fi) {
                        lua_getfield(Ls, -1, f6[fi]);
                        v[fi] = (float)lua_tonumber(Ls, -1);
                        lua_pop(Ls, 1);
                    }
                    // Positions: authored -> world-scaled int16.
                    for (int a = 0; a < 3; ++a) {
                        float w = v[a] * (float)JET32_WORLD_SCALE;
                        if (w > 32767) w = 32767;
                        if (w < -32768) w = -32768;
                        (&key.px)[a] = (int16_t)(w >= 0 ? w + 0.5f : w - 0.5f);
                    }
                    // Euler degrees -> quaternion via the SHARED formula,
                    // host-tested against jm_eulerMatrix so the sampler and
                    // the runtime can never disagree on rotation order.
                    const JmQuat q = jm_eulerToQuat(v[3], v[4], v[5]);
                    key.qx = (int16_t)(q.x * 16384.0f);
                    key.qy = (int16_t)(q.y * 16384.0f);
                    key.qz = (int16_t)(q.z * 16384.0f);
                    key.qw = (int16_t)(q.w * 16384.0f);
                }
                lua_pop(Ls, 1);
            }
            clip.keys.push_back(key);
        }
        lua_pop(Ls, 1);   // pose table
    }

    b->clips.push_back(clip);
    lua_settop(Ls, 1);
    return 1;
}

static int bld_gc(lua_State* Ls) {
    JetMeshBuilder** ud = (JetMeshBuilder**)luaL_checkudata(Ls, 1, MT_BUILDER);
    delete *ud;
    *ud = nullptr;
    return 0;
}

static const luaL_Reg builder_methods[] = {
    { "cube",     bld_cube     },
    { "sphere",   bld_sphere   },
    { "plane",    bld_plane    },
    { "pyramid",  bld_pyramid  },
    { "cylinder", bld_cylinder },
    { "capsule",  bld_capsule  },
    { "quad",     bld_quad     },
    { "ribbon",   bld_ribbon   },
    { "grid",     bld_grid     },
    { "heightfield", bld_heightfield },
    { "trifield",    bld_trifield    },
    { "landfield",   bld_landfield   },
    { "stats",    bld_stats    },
    { "bake",     bld_bake     },
    { "save",     bld_save     },
    { "clip",     bld_clip     },
    { nullptr, nullptr }
};

// jet.instance(source) -> a new scene object that SHARES source's mesh.
//
// The instance BORROWS the source's vertex and triangle arrays (JetSpan::borrow
// is non-owning), so it costs one Object struct — ~216 bytes — instead of a
// full copy of the geometry. Bake a handful of tiles once and stamp them across
// the world: 256 ground tiles drop from ~390 KB of duplicated meshes to ~61 KB.
//
// This is only a MEMORY win. Each instance is still its own scene object, so it
// still pays a frustum-cull test and still transforms its own vertices every
// frame — the prepare cost model is unchanged.
//
// LIFETIME RULE: the source must outlive its instances. Destroying the source
// first leaves the instances pointing at freed arrays. Destroying them without
// rendering in between is harmless (a borrowed span frees nothing and the scene
// removal never reads the mesh), which is what makes a bulk teardown loop safe
// regardless of order — but do not destroy a source and then draw a frame.
static int l_instance(lua_State* Ls) {
    Object* src = checkObject(Ls, 1);
    if (src->vertices.empty() || src->triangles.empty())
        return luaL_error(Ls, "instance: source object has no mesh");

    Object* o = new Object();
    o->vertices.borrow(src->vertices.data(), src->vertices.size());
    o->triangles.borrow(src->triangles.data(), src->triangles.size());
    // Carry the draw policy across: an instance of a floor is still a floor.
    o->sortDepth   = src->sortDepth;
    o->cullingMode = src->cullingMode;
    o->zBias       = src->zBias;

    // Inherit the LOD chain. lodMeshes holds mesh-only Objects that are never
    // scene-added and are owned by the source's group, so sharing the POINTERS
    // costs nothing and is freed exactly once. Without this an instanced world
    // pays full detail at every distance — and per-visible-object vertex
    // transform is the dominant prepare term at world scale, so this is the
    // difference between a distant tile costing 16 vertices and costing 4.
    // Covered by the same lifetime rule: the source outlives its instances.
    for (size_t i = 0; i < src->lodMeshes.size(); ++i)
        o->lodMeshes.push_back(src->lodMeshes[i]);
    o->lodPersist = src->lodPersist;
    o->lodBias    = src->lodBias;

    o->calculateBoundingBox();

    g_objects.push_back(o);
    g_scene->addObject(o);
    pushObject(Ls, o);
    return 1;
}

static int l_builder(lua_State* Ls) {
    JetMeshBuilder** ud = (JetMeshBuilder**)lua_newuserdatauv(Ls, sizeof(void*), 0);
    *ud = new JetMeshBuilder();
    luaL_getmetatable(Ls, MT_BUILDER);
    lua_setmetatable(Ls, -2);
    return 1;
}

// jet.mesh(path [, {light=true}]) -> object or nil
//
// Zero-copy: the file is read as one blob, the material table is decoded into
// engine Materials, and the Object borrows its vertex/triangle arrays straight
// from the blob (which it owns and frees when destroyed).
static int l_mesh(lua_State* Ls) {
    const char* path = luaL_checkstring(Ls, 1);

    JetMeshFile mf;
    if (!jet_mesh_read(path, (uint8_t)JET32_WORLD_SCALE, &mf)) {
        logf_("[mesh] load %s: FAILED (missing, corrupt, or a pre-v2 bake —"
              " re-save it)", path);
        lua_pushnil(Ls);
        return 1;
    }

    // Materials come back as parameters; engine Materials are created fresh
    // and owned by the usual registry.
    Material** mv = new Material*[mf.matCount];
    for (uint32_t i = 0; i < mf.matCount; ++i) {
        const JetMatParams& p = mf.matData[i];
        Material* m = new Material(p.color, nullptr, nullptr,
                                   p.emissive != 0, p.alpha, p.diffuse, p.specular);
        m->shadingMode = (ShadingMode)p.shading;
        g_materials.push_back(m);
        mv[i] = m;
    }

    // Celled file: one Object per spatial cell, all scene-resident. The first
    // is returned; a celled mesh is static world geometry placed at bake time.
    if (mf.cellCount > 1) {
        Object** cellObjs = new Object*[mf.cellCount];
        const int n = jet_mesh_instantiate_cells(mf, mv, cellObjs,
                                                 (int)mf.cellCount);
        delete[] mv;
        if (n == 0) {
            delete[] cellObjs;
            jet_mesh_file_free(&mf);
            logf_("[mesh] load %s: cell instantiate failed", path);
            lua_pushnil(Ls);
            return 1;
        }
        std::vector<Object*> made;
        for (int i = 0; i < n; ++i) {
            if (optFieldBool(Ls, 2, "light", 0))
                cellObjs[i]->bakeFlatLighting(g_dirLight, g_ambLight);
            g_objects.push_back(cellObjs[i]);   // adopter (last) deleted last
            g_scene->addObject(cellObjs[i]);
            made.push_back(cellObjs[i]);
        }
        Object* head = cellObjs[0];
        delete[] cellObjs;
        registerGroup(head, made);
        if (bakeLogWanted())
        logf_("[mesh] load %s: %u verts, %u tris, %d cells (in place)", path,
              (unsigned)mf.vertCount, (unsigned)mf.triCount, n);
        pushObject(Ls, head);
        return 1;
    }

    Object* lodObjs[4] = {};
    Object* o = jet_mesh_instantiate(mf, mv, lodObjs, 4);
    delete[] mv;
    if (!o) {
        jet_mesh_file_free(&mf);
        logf_("[mesh] load %s: instantiate failed", path);
        lua_pushnil(Ls);
        return 1;
    }
    if (optFieldBool(Ls, 2, "light", 0)) {
        o->bakeFlatLighting(g_dirLight, g_ambLight);
    }
    // LOD Objects BEFORE the head: close deletes g_objects forward, and the
    // head adopts the blob every LOD level borrows from.
    std::vector<Object*> made;
    for (int i = 0; i < 4 && lodObjs[i]; ++i) {
        g_objects.push_back(lodObjs[i]);
        made.push_back(lodObjs[i]);
    }
    g_objects.push_back(o);
    g_scene->addObject(o);
    made.push_back(o);          // head last: it adopts the shared blob
    registerGroup(o, made);
    if (bakeLogWanted())
    logf_("[mesh] load %s: %u verts, %u tris, %u materials, %u lods (in place)",
          path, (unsigned)mf.vertCount, (unsigned)mf.triCount,
          (unsigned)mf.matCount, (unsigned)mf.lodCount);
    pushObject(Ls, o);
    return 1;
}


// ---------------------------------------------------------------------------
// jet.rig — rigged mesh instances (segmented rigid-part animation)
// ---------------------------------------------------------------------------
// One scene Object per bone, each borrowing its geometry from the rig file's
// single blob (the LAST bone adopts and frees it — so teardown must destroy
// bones in order with the adopter last; pushing them into g_objects in file
// order satisfies jet_lua_close, which deletes forward).
//
// Per frame, each bone's local pose comes from the playing clip (nlerp between
// keys) with any procedural rig:pose() values ADDED on top, then parent
// composition runs in array order (parents always precede children) and each
// bone Object receives its world rotation via the matrix override and its
// world position. Bone geometry is bone-local, so the Object transform IS the
// bone transform.

#define MT_RIG "jet.rig"

struct JetRig {
    int      boneCount = 0;
    Object*  bone[16]  = {};
    int16_t  parent[16];
    float    pivX[16], pivY[16], pivZ[16];      // world-scaled floats
    uint32_t nameHash[16];

    // Clip data lives inside the blob, which lives as long as the bones do.
    const JmshClipRec* clips = nullptr;
    uint32_t clipCount = 0;
    const JetMeshBuilder::ClipKey* keys = nullptr;

    int   playing = -1;
    float time = 0, speed = 1;
    bool  loop = false;

    // Procedural pose per bone: Euler degrees + authored-unit offsets,
    // applied on top of (or instead of) the clip sample.
    float poseR[16][3] = {};
    float poseP[16][3] = {};

    float rootPos[3] = { 0, 0, 0 };
    float rootRot[3] = { 0, 0, 0 };

    // One-shot: dump each bone's WORLD-space Y extent on the next tick. Armed
    // by rig:position() so the numbers reflect the placed rig, which is what
    // decides whether geometry sits above a floor or intersects it. Answers
    // "is this a placement bug or a painter's sort artefact" with numbers
    // instead of by eye.
    bool dumpNext = true;
};

static std::vector<JetRig*> g_rigs;

// Quat/matrix math shared with the clip sampler and the host tests:
// jm_eulerToQuat / jm_quatToMatrix / jm_eulerMatrix / jm_mat3Mul (jet_mesh.h).

static void jet_rig_tick(JetRig* rig, float dt)
{
    // Sample the playing clip.
    JmQuat clipQ[16];
    float clipP[16][3];
    for (int i = 0; i < rig->boneCount; ++i) {
        clipQ[i] = { 0, 0, 0, 1 };
        clipP[i][0] = clipP[i][1] = clipP[i][2] = 0;
    }
    if (rig->playing >= 0 && rig->playing < (int)rig->clipCount) {
        const JmshClipRec& c = rig->clips[rig->playing];
        const float dur = (float)(c.keyCount - 1) / c.rate;
        rig->time += dt * rig->speed;
        if (rig->time >= dur) {
            if (rig->loop) rig->time = fmodf(rig->time, dur);
            else { rig->time = dur; }
        }
        float ft = rig->time * c.rate;
        int k0 = (int)ft;
        if (k0 > c.keyCount - 2) k0 = c.keyCount - 2;
        const float u = ft - k0;
        const JetMeshBuilder::ClipKey* a =
            rig->keys + c.keyOff + (size_t)k0 * rig->boneCount;
        const JetMeshBuilder::ClipKey* b = a + rig->boneCount;
        const float Q = 1.0f / 16384.0f;
        for (int i = 0; i < rig->boneCount; ++i) {
            // nlerp with hemisphere fix: negate the second key when the dot
            // is negative or the blend swings the long way round.
            float ax = a[i].qx*Q, ay = a[i].qy*Q, az = a[i].qz*Q, aw = a[i].qw*Q;
            float bx = b[i].qx*Q, by = b[i].qy*Q, bz = b[i].qz*Q, bw = b[i].qw*Q;
            if (ax*bx + ay*by + az*bz + aw*bw < 0) {
                bx = -bx; by = -by; bz = -bz; bw = -bw;
            }
            JmQuat q = { ax + (bx-ax)*u, ay + (by-ay)*u,
                        az + (bz-az)*u, aw + (bw-aw)*u };
            const float len = sqrtf(q.x*q.x + q.y*q.y + q.z*q.z + q.w*q.w);
            if (len > 0.0001f) {
                const float inv = 1.0f / len;
                q.x *= inv; q.y *= inv; q.z *= inv; q.w *= inv;
            } else q = { 0, 0, 0, 1 };
            clipQ[i] = q;
            for (int ax2 = 0; ax2 < 3; ++ax2)
                clipP[i][ax2] = (&a[i].px)[ax2] + ((&b[i].px)[ax2] - (&a[i].px)[ax2]) * u;
        }
    }

    // Compose parent chains in array order (parents precede children).
    float R[16][9];
    float P[16][3];
    float rootM[9];
    jm_eulerMatrix(rig->rootRot[0], rig->rootRot[1], rig->rootRot[2], rootM);

    for (int i = 0; i < rig->boneCount; ++i) {
        // Local rotation: clip sample composed with the procedural pose.
        float local[9], cm[9];
        jm_quatToMatrix(clipQ[i], cm);
        if (rig->poseR[i][0] != 0 || rig->poseR[i][1] != 0 || rig->poseR[i][2] != 0) {
            float pm[9];
            jm_eulerMatrix(rig->poseR[i][0], rig->poseR[i][1], rig->poseR[i][2], pm);
            jm_mat3Mul(cm, pm, local);
        } else {
            for (int k = 0; k < 9; ++k) local[k] = cm[k];
        }
        const float offX = clipP[i][0] + rig->poseP[i][0] * (float)JET32_WORLD_SCALE;
        const float offY = clipP[i][1] + rig->poseP[i][1] * (float)JET32_WORLD_SCALE;
        const float offZ = clipP[i][2] + rig->poseP[i][2] * (float)JET32_WORLD_SCALE;

        const int par = rig->parent[i];
        if (par < 0) {
            jm_mat3Mul(rootM, local, R[i]);
            const float dx = rig->pivX[i] + offX;
            const float dy = rig->pivY[i] + offY;
            const float dz = rig->pivZ[i] + offZ;
            P[i][0] = rig->rootPos[0] + rootM[0]*dx + rootM[1]*dy + rootM[2]*dz;
            P[i][1] = rig->rootPos[1] + rootM[3]*dx + rootM[4]*dy + rootM[5]*dz;
            P[i][2] = rig->rootPos[2] + rootM[6]*dx + rootM[7]*dy + rootM[8]*dz;
        } else {
            jm_mat3Mul(R[par], local, R[i]);
            const float dx = rig->pivX[i] - rig->pivX[par] + offX;
            const float dy = rig->pivY[i] - rig->pivY[par] + offY;
            const float dz = rig->pivZ[i] - rig->pivZ[par] + offZ;
            P[i][0] = P[par][0] + R[par][0]*dx + R[par][1]*dy + R[par][2]*dz;
            P[i][1] = P[par][1] + R[par][3]*dx + R[par][4]*dy + R[par][5]*dz;
            P[i][2] = P[par][2] + R[par][6]*dx + R[par][7]*dy + R[par][8]*dz;
        }

        Object* o = rig->bone[i];
        for (int k = 0; k < 9; ++k) o->matrix[k] = R[i][k];
        o->useMatrix = true;
        o->position.assign((int32_t)P[i][0], (int32_t)P[i][1], (int32_t)P[i][2]);
    }

    if (rig->dumpNext) {
        rig->dumpNext = false;
        for (int i = 0; i < rig->boneCount; ++i) {
            const Object* o = rig->bone[i];
            if (o->vertices.empty()) {
                logf_("[rig] bone %d: empty (implicit root)", i);
                continue;
            }
            // Rotate the local AABB corners by this bone's world matrix so the
            // reported extent is the real drawn one, not an axis-aligned guess.
            float lo = 1e30f, hi = -1e30f;
            for (int c = 0; c < 8; ++c) {
                const float lx = (c & 1) ? o->boundingBoxMax.x : o->boundingBoxMin.x;
                const float ly = (c & 2) ? o->boundingBoxMax.y : o->boundingBoxMin.y;
                const float lz = (c & 4) ? o->boundingBoxMax.z : o->boundingBoxMin.z;
                const float wy = P[i][1] + R[i][3]*lx + R[i][4]*ly + R[i][5]*lz;
                if (wy < lo) lo = wy;
                if (wy > hi) hi = wy;
            }
            logf_("[rig] bone %d: world y %.1f..%.1f authored (%d tris)", i,
                  lo / JET32_WORLD_SCALE, hi / JET32_WORLD_SCALE,
                  (int)o->triangles.size());
        }
    }
}

void jet_rigs_update(float dt)
{
    for (size_t i = 0; i < g_rigs.size(); ++i) jet_rig_tick(g_rigs[i], dt);
}

static JetRig* checkRig(lua_State* Ls, int idx) {
    JetRig** ud = (JetRig**)luaL_checkudata(Ls, idx, MT_RIG);
    if (!*ud) luaL_error(Ls, "rig has been destroyed");
    return *ud;
}

static int rigBoneIndex(JetRig* r, lua_State* Ls, int idx) {
    const uint32_t h = bld_hashName(luaL_checkstring(Ls, idx));
    for (int i = 0; i < r->boneCount; ++i)
        if (r->nameHash[i] == h) return i;
    return -1;
}

// rig:play("name" [, {loop=, speed=}])
static int rig_play(lua_State* Ls) {
    JetRig* r = checkRig(Ls, 1);
    const uint32_t h = bld_hashName(luaL_checkstring(Ls, 2));
    int ci = -1;
    for (uint32_t i = 0; i < r->clipCount; ++i)
        if (r->clips[i].nameHash == h) { ci = (int)i; break; }
    if (ci < 0) return luaL_error(Ls, "rig: no clip '%s'", lua_tostring(Ls, 2));
    r->playing = ci;
    r->time    = 0;
    r->loop    = optFieldBool(Ls, 3, "loop", 1) != 0;
    r->speed   = 1.0f;
    if (lua_istable(Ls, 3)) {
        lua_getfield(Ls, 3, "speed");
        if (lua_isnumber(Ls, -1)) r->speed = (float)lua_tonumber(Ls, -1);
        lua_pop(Ls, 1);
    }
    lua_settop(Ls, 1);
    return 1;
}

static int rig_stop(lua_State* Ls) {
    JetRig* r = checkRig(Ls, 1);
    r->playing = -1;
    lua_settop(Ls, 1);
    return 1;
}

// rig:pose("bone", rx, ry, rz [, x, y, z]) — procedural local pose, composed
// on top of whatever a playing clip provides. Angles degrees, offsets
// authored units.
static int rig_pose(lua_State* Ls) {
    JetRig* r = checkRig(Ls, 1);
    const int b = rigBoneIndex(r, Ls, 2);
    if (b < 0) return luaL_error(Ls, "rig: no bone '%s'", lua_tostring(Ls, 2));
    r->poseR[b][0] = (float)luaL_optnumber(Ls, 3, 0);
    r->poseR[b][1] = (float)luaL_optnumber(Ls, 4, 0);
    r->poseR[b][2] = (float)luaL_optnumber(Ls, 5, 0);
    r->poseP[b][0] = (float)luaL_optnumber(Ls, 6, 0);
    r->poseP[b][1] = (float)luaL_optnumber(Ls, 7, 0);
    r->poseP[b][2] = (float)luaL_optnumber(Ls, 8, 0);
    lua_settop(Ls, 1);
    return 1;
}

static int rig_position(lua_State* Ls) {
    JetRig* r = checkRig(Ls, 1);
    if (lua_isnoneornil(Ls, 2)) {
        lua_pushnumber(Ls, r->rootPos[0] / JET32_WORLD_SCALE);
        lua_pushnumber(Ls, r->rootPos[1] / JET32_WORLD_SCALE);
        lua_pushnumber(Ls, r->rootPos[2] / JET32_WORLD_SCALE);
        return 3;
    }
    r->rootPos[0] = (float)luaL_checknumber(Ls, 2) * JET32_WORLD_SCALE;
    r->rootPos[1] = (float)luaL_checknumber(Ls, 3) * JET32_WORLD_SCALE;
    r->rootPos[2] = (float)luaL_checknumber(Ls, 4) * JET32_WORLD_SCALE;
    r->dumpNext = true;   // report the placed extents once (see JetRig)
    lua_settop(Ls, 1);
    return 1;
}

static int rig_rotation(lua_State* Ls) {
    JetRig* r = checkRig(Ls, 1);
    if (lua_isnoneornil(Ls, 2)) {
        lua_pushnumber(Ls, r->rootRot[0]);
        lua_pushnumber(Ls, r->rootRot[1]);
        lua_pushnumber(Ls, r->rootRot[2]);
        return 3;
    }
    r->rootRot[0] = (float)luaL_checknumber(Ls, 2);
    r->rootRot[1] = (float)luaL_checknumber(Ls, 3);
    r->rootRot[2] = (float)luaL_checknumber(Ls, 4);
    lua_settop(Ls, 1);
    return 1;
}

// Destroy the rig: bones leave the scene and die in order with the
// blob-adopting last bone destroyed LAST.
static int rig_destroy(lua_State* Ls) {
    JetRig** ud = (JetRig**)luaL_checkudata(Ls, 1, MT_RIG);
    JetRig* r = *ud;
    if (!r) return 0;
    for (int i = 0; i < r->boneCount; ++i) {
        Object* o = r->bone[i];
        g_scene->removeObject(o);
        for (size_t k = 0; k < g_objects.size(); ++k)
            if (g_objects[k] == o) { g_objects.erase(g_objects.begin() + k); break; }
        delete o;
    }
    for (size_t i = 0; i < g_rigs.size(); ++i)
        if (g_rigs[i] == r) { g_rigs.erase(g_rigs.begin() + i); break; }
    delete r;
    *ud = nullptr;
    return 0;
}

static const luaL_Reg rig_methods[] = {
    { "play",     rig_play     },
    { "stop",     rig_stop     },
    { "pose",     rig_pose     },
    { "position", rig_position },
    { "rotation", rig_rotation },
    { "destroy",  rig_destroy  },
    { nullptr, nullptr }
};

// jet.rig(path) -> rig or nil. The file must be a rigged bake (named parts).
static int l_rig(lua_State* Ls) {
    const char* path = luaL_checkstring(Ls, 1);

    JetMeshFile mf;
    if (!jet_mesh_read(path, (uint8_t)JET32_WORLD_SCALE, &mf) ||
        mf.partCount < 2 || mf.partCount > 16) {
        jet_mesh_file_free(&mf);
        logf_("[rig] load %s: FAILED (missing, corrupt, or not a rigged bake)",
              path);
        lua_pushnil(Ls);
        return 1;
    }

    Material** mv = new Material*[mf.matCount];
    for (uint32_t i = 0; i < mf.matCount; ++i) {
        const JetMatParams& p = mf.matData[i];
        Material* m = new Material(p.color, nullptr, nullptr,
                                   p.emissive != 0, p.alpha, p.diffuse, p.specular);
        m->shadingMode = (ShadingMode)p.shading;
        g_materials.push_back(m);
        mv[i] = m;
    }

    JetRig* r = new JetRig();
    r->boneCount = (int)mf.partCount;
    for (uint32_t i = 0; i < mf.partCount; ++i) {
        const JmshPartRec& pr = mf.partData[i];
        r->parent[i]   = pr.parent;
        r->pivX[i]     = (float)pr.px;
        r->pivY[i]     = (float)pr.py;
        r->pivZ[i]     = (float)pr.pz;
        r->nameHash[i] = pr.nameHash;
    }
    r->clips     = mf.clipData;
    r->clipCount = mf.clipCount;
    r->keys      = mf.clipKeys;

    const int n = jet_mesh_instantiate_rig(mf, mv, r->bone, 16);
    delete[] mv;
    if (n == 0) {
        jet_mesh_file_free(&mf);
        delete r;
        logf_("[rig] load %s: instantiate failed", path);
        lua_pushnil(Ls);
        return 1;
    }
    for (int i = 0; i < n; ++i) {
        g_objects.push_back(r->bone[i]);   // adopter last: close deletes forward
        g_scene->addObject(r->bone[i]);
    }
    g_rigs.push_back(r);
    jet_rig_tick(r, 0);   // rest pose before the first frame renders

    logf_("[rig] load %s: %d bones, %u clips, %u verts (in place)", path,
          n, (unsigned)r->clipCount, (unsigned)mf.vertCount);

    JetRig** ud = (JetRig**)lua_newuserdatauv(Ls, sizeof(void*), 0);
    *ud = r;
    luaL_getmetatable(Ls, MT_RIG);
    lua_setmetatable(Ls, -2);
    return 1;
}

// ---------------------------------------------------------------------------
// jet.pick — screen-space picking
// ---------------------------------------------------------------------------
// jet.pick(x, y [, slot=1]) arms a sticky query; results come from the LAST
// completed render, read with jet.picked([slot]) -> id, triIndex or nil.
// Compare ids against obj:id(). jet.pickoff([slot]) disarms.

static int l_pick(lua_State* Ls) {
#if MAX_PICK_QUERIES == 0
    (void)Ls; return 0;
#else
    const int slot = (int)luaL_optinteger(Ls, 3, 1);
    if (slot < 1 || slot > MAX_PICK_QUERIES)
        return luaL_error(Ls, "pick: slot 1..%d", MAX_PICK_QUERIES);
    g_pickQ[slot - 1].x = (int16_t)luaL_checkinteger(Ls, 1);
    g_pickQ[slot - 1].y = (int16_t)luaL_checkinteger(Ls, 2);
    g_pickDirty = true;
    return 0;
#endif
}

static int l_pickoff(lua_State* Ls) {
#if MAX_PICK_QUERIES == 0
    (void)Ls; return 0;
#else
    if (lua_isnoneornil(Ls, 1)) {
        for (int i = 0; i < MAX_PICK_QUERIES; ++i) { g_pickQ[i].x = -1; g_pickQ[i].y = -1; }
    } else {
        const int slot = (int)luaL_checkinteger(Ls, 1);
        if (slot >= 1 && slot <= MAX_PICK_QUERIES) { g_pickQ[slot-1].x = -1; g_pickQ[slot-1].y = -1; }
    }
    g_pickDirty = true;
    return 0;
#endif
}

static int l_picked(lua_State* Ls) {
#if MAX_PICK_QUERIES == 0
    lua_pushnil(Ls); return 1;
#else
    const int slot = (int)luaL_optinteger(Ls, 1, 1);
    if (slot < 1 || slot > MAX_PICK_QUERIES) { lua_pushnil(Ls); return 1; }
    const PickResult* r = g_scene ? g_scene->getPickResults() : nullptr;
    if (!r || !r[slot - 1].hit || !r[slot - 1].object) { lua_pushnil(Ls); return 1; }
    lua_pushinteger(Ls, (lua_Integer)(uintptr_t)r[slot - 1].object);
    lua_pushinteger(Ls, r[slot - 1].triangleIndex);
    return 2;
#endif
}

// ---------------------------------------------------------------------------
// jet.timer
// ---------------------------------------------------------------------------

static int t_fps(lua_State* Ls)  { lua_pushnumber(Ls, g_fps);    return 1; }
static int t_time(lua_State* Ls) { lua_pushnumber(Ls, g_uptime); return 1; }
static int t_dt(lua_State* Ls)   { lua_pushnumber(Ls, g_lastDt); return 1; }
// Milliseconds since DEVICE boot — unlike time(), which starts at zero when
// the module loads. This is the entropy source for random seeds: a seed
// taken from time() at startup is near-constant ("[kart] seed 0" — the same
// track every launch, hw 2026-08-03).
static int t_ticks(lua_State* Ls) {
    lua_pushinteger(Ls, (lua_Integer)host_get_ticks_ms());
    return 1;
}

static const luaL_Reg timer_funcs[] = {
    { "fps",  t_fps  },
    { "time", t_time },
    { "dt",   t_dt   },
    { "ticks", t_ticks },
    { nullptr, nullptr }
};

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

static void setIntField(lua_State* Ls, const char* k, lua_Integer v) {
    lua_pushinteger(Ls, v);
    lua_setfield(Ls, -2, k);
}

static void newSubTable(lua_State* Ls, const char* name, const luaL_Reg* fns) {
    lua_newtable(Ls);
    luaL_setfuncs(Ls, fns, 0);
    lua_setfield(Ls, -2, name);
}

static void registerAPI(lua_State* Ls) {
    // Object metatable: __index points at the method table.
    luaL_newmetatable(Ls, MT_OBJECT);
    lua_newtable(Ls);
    luaL_setfuncs(Ls, object_methods, 0);
    lua_setfield(Ls, -2, "__index");
    lua_pop(Ls, 1);

    luaL_newmetatable(Ls, MT_MATERIAL);
    lua_newtable(Ls);
    lua_pushcfunction(Ls, l_material_set);
    lua_setfield(Ls, -2, "set");
    lua_setfield(Ls, -2, "__index");
    lua_pop(Ls, 1);

    luaL_newmetatable(Ls, MT_TEXTURE);
    lua_newtable(Ls);
    lua_pushcfunction(Ls, t_animate); lua_setfield(Ls, -2, "animate");
    lua_pushcfunction(Ls, t_shift);   lua_setfield(Ls, -2, "shift");
    lua_pushcfunction(Ls, t_rebake);  lua_setfield(Ls, -2, "rebake");
    lua_setfield(Ls, -2, "__index");
    lua_pop(Ls, 1);

    luaL_newmetatable(Ls, MT_CULLGROUP);
    lua_newtable(Ls);
    lua_pushcfunction(Ls, l_cullgroup_destroy); lua_setfield(Ls, -2, "destroy");
    lua_setfield(Ls, -2, "__index");
    lua_pop(Ls, 1);

    luaL_newmetatable(Ls, MT_SPRITE);
    lua_newtable(Ls);
    luaL_setfuncs(Ls, sprite_methods, 0);
    lua_setfield(Ls, -2, "__index");
    lua_pop(Ls, 1);

    luaL_newmetatable(Ls, MT_RIG);
    lua_newtable(Ls);
    luaL_setfuncs(Ls, rig_methods, 0);
    lua_setfield(Ls, -2, "__index");
    lua_pop(Ls, 1);

    // Builder: __index for the part/bake/save methods, __gc because this
    // userdata OWNS its C++ object.
    luaL_newmetatable(Ls, MT_BUILDER);
    lua_newtable(Ls);
    luaL_setfuncs(Ls, builder_methods, 0);
    lua_setfield(Ls, -2, "__index");
    lua_pushcfunction(Ls, bld_gc);
    lua_setfield(Ls, -2, "__gc");
    lua_pop(Ls, 1);

    lua_newtable(Ls);   // jet

    lua_pushcfunction(Ls, l_rgb);       lua_setfield(Ls, -2, "rgb");
    lua_pushcfunction(Ls, l_log);       lua_setfield(Ls, -2, "log");
    lua_pushcfunction(Ls, l_mem);       lua_setfield(Ls, -2, "mem");
    lua_pushcfunction(Ls, l_instance);  lua_setfield(Ls, -2, "instance");
    lua_pushcfunction(Ls, l_probe);     lua_setfield(Ls, -2, "probe");
    lua_pushcfunction(Ls, l_dumprow);   lua_setfield(Ls, -2, "dumprow");
    lua_pushcfunction(Ls, l_text);      lua_setfield(Ls, -2, "text");
    lua_pushcfunction(Ls, l_rect);      lua_setfield(Ls, -2, "rect");
    lua_pushcfunction(Ls, l_textwidth); lua_setfield(Ls, -2, "textwidth");

    setIntField(Ls, "FONT_W", JET_OVL_GLYPH_W);
    setIntField(Ls, "FONT_H", JET_OVL_GLYPH_H);
    lua_pushcfunction(Ls, l_quit);      lua_setfield(Ls, -2, "quit");
    lua_pushcfunction(Ls, l_material);  lua_setfield(Ls, -2, "material");
    lua_pushcfunction(Ls, l_texture);   lua_setfield(Ls, -2, "texture");
    lua_pushcfunction(Ls, l_land);        lua_setfield(Ls, -2, "land");
    lua_pushcfunction(Ls, l_landheight);  lua_setfield(Ls, -2, "landheight");
    lua_pushcfunction(Ls, l_landtex);     lua_setfield(Ls, -2, "landtex");
    lua_pushcfunction(Ls, l_tiletex);     lua_setfield(Ls, -2, "tiletex");
    lua_pushcfunction(Ls, l_cullgroup);   lua_setfield(Ls, -2, "cullgroup");
    setIntField(Ls, "TILE_GROUND", JT_GROUND);
    setIntField(Ls, "TILE_STRATA", JT_STRATA);
    setIntField(Ls, "TILE_SAND",   JT_SAND);
    setIntField(Ls, "TILE_GRASS",  JT_GRASS);
    setIntField(Ls, "TILE_ROCK",   JT_ROCK);
    lua_pushcfunction(Ls, l_horizon);     lua_setfield(Ls, -2, "horizon");
    lua_pushcfunction(Ls, l_landscatter); lua_setfield(Ls, -2, "landscatter");
    lua_pushcfunction(Ls, l_builder);   lua_setfield(Ls, -2, "builder");
    lua_pushcfunction(Ls, l_mesh);      lua_setfield(Ls, -2, "mesh");
    lua_pushcfunction(Ls, l_rig);       lua_setfield(Ls, -2, "rig");
    lua_pushcfunction(Ls, l_sprite);    lua_setfield(Ls, -2, "sprite");
    lua_pushcfunction(Ls, l_pick);      lua_setfield(Ls, -2, "pick");
    lua_pushcfunction(Ls, l_pickoff);   lua_setfield(Ls, -2, "pickoff");
    lua_pushcfunction(Ls, l_picked);    lua_setfield(Ls, -2, "picked");

    lua_pushcfunction(Ls, l_cube);      lua_setfield(Ls, -2, "cube");
    lua_pushcfunction(Ls, l_sphere);    lua_setfield(Ls, -2, "sphere");
    lua_pushcfunction(Ls, l_plane);     lua_setfield(Ls, -2, "plane");
    lua_pushcfunction(Ls, l_pyramid);   lua_setfield(Ls, -2, "pyramid");
    lua_pushcfunction(Ls, l_cylinder);  lua_setfield(Ls, -2, "cylinder");
    lua_pushcfunction(Ls, l_capsule);   lua_setfield(Ls, -2, "capsule");
    lua_pushcfunction(Ls, l_quad);      lua_setfield(Ls, -2, "quad");
    lua_pushcfunction(Ls, l_billboard); lua_setfield(Ls, -2, "billboard");
    lua_pushcfunction(Ls, l_grid);      lua_setfield(Ls, -2, "grid");

    newSubTable(Ls, "camera", camera_funcs);
    newSubTable(Ls, "scene",  scene_funcs);
    newSubTable(Ls, "input",  input_funcs);
    newSubTable(Ls, "timer",  timer_funcs);
    newSubTable(Ls, "sound",  sound_funcs);

    // Sample rate clips must be authored at, exposed so a game can assert it.
    lua_getfield(Ls, -1, "sound");
    setIntField(Ls, "RATE", JET_AUDIO_RATE);
    setIntField(Ls, "VOICES", JET_AUDIO_VOICES);
    lua_pop(Ls, 1);

    setIntField(Ls, "SQUARE",   JET_WAVE_SQUARE);
    setIntField(Ls, "NOISE",    JET_WAVE_NOISE);
    setIntField(Ls, "TRIANGLE", JET_WAVE_TRIANGLE);

    // Shading modes
    setIntField(Ls, "FLAT",      (lua_Integer)ShadingMode::FLAT);
    setIntField(Ls, "GOURAUD",   (lua_Integer)ShadingMode::GOURAUD);
    setIntField(Ls, "PHONG",     (lua_Integer)ShadingMode::PHONG);
    setIntField(Ls, "WIREFRAME", (lua_Integer)ShadingMode::WIREFRAME);
    setIntField(Ls, "UNLIT",     (lua_Integer)ShadingMode::UNLIT);
    setIntField(Ls, "ADDITIVE",  (lua_Integer)ShadingMode::ADDITIVE);
    setIntField(Ls, "WATER",     (lua_Integer)ShadingMode::WATER_REFLECT);

    // Painter's-sort depth policy (obj:sortdepth)
    setIntField(Ls, "SORT_AVERAGE",  (lua_Integer)SortDepth::AVERAGE);
    setIntField(Ls, "SORT_FARTHEST", (lua_Integer)SortDepth::FARTHEST);

    // Culling modes
    setIntField(Ls, "CULL_BACK",  (lua_Integer)CullingMode::CULL_BACKFACES);
    setIntField(Ls, "CULL_FRONT", (lua_Integer)CullingMode::CULL_FRONTFACES);
    setIntField(Ls, "CULL_NONE",  (lua_Integer)CullingMode::NO_CULLING);

    // Texture addressing
    setIntField(Ls, "WRAP",  (lua_Integer)WRAP);
    setIntField(Ls, "CLAMP", (lua_Integer)CLAMP);
    setIntField(Ls, "ZERO",  (lua_Integer)ZERO);

    setIntField(Ls, "WIDTH",  g_screenW);
    setIntField(Ls, "HEIGHT", g_screenH);
    // Jet applies no world scaling itself; the fog distances in JetConfig.hpp
    // are expressed in these units, so a game that wants the documented fog
    // ranges multiplies its coordinates by this.
    setIntField(Ls, "WORLD_SCALE", JET32_WORLD_SCALE);
    setIntField(Ls, "FIXED_ONE", FIXED_POINT_SCALE);

    lua_setglobal(Ls, "jet");
}

// ---------------------------------------------------------------------------
// Error reporting
// ---------------------------------------------------------------------------

static int msgHandler(lua_State* Ls) {
    const char* msg = lua_tostring(Ls, 1);
    if (!msg) msg = "(non-string error)";
    luaL_traceback(Ls, Ls, msg, 1);
    return 1;
}

// Calls a function already on the stack with `nargs` arguments below it.
static bool protectedCall(lua_State* Ls, int nargs) {
    int base = lua_gettop(Ls) - nargs;   // function index
    lua_pushcfunction(Ls, msgHandler);
    lua_insert(Ls, base);
    int rc = lua_pcall(Ls, nargs, 0, base);
    lua_remove(Ls, base);
    if (rc != LUA_OK) {
        const char* err = lua_tostring(Ls, -1);
        logf_("[jet3d] lua error: %s", err ? err : "?");
        lua_pop(Ls, 1);
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Allocator
// ---------------------------------------------------------------------------

// Lua's default allocator is realloc/free, which the host remaps to PSRAM.
// Routing it explicitly keeps a running total for diagnostics and means a
// future arena swap only touches this function.
static size_t g_luaBytes = 0;

static void* luaAlloc(void* ud, void* ptr, size_t osize, size_t nsize) {
    (void)ud;
    if (nsize == 0) {
        if (ptr) { g_luaBytes -= osize; free(ptr); }
        return nullptr;
    }
    void* np = realloc(ptr, nsize);
    if (np) g_luaBytes += nsize - (ptr ? osize : 0);
    return np;
}

// ---------------------------------------------------------------------------
// Public interface
// ---------------------------------------------------------------------------

bool jet_lua_open(Scene* scene, int screenW, int screenH) {
    g_scene   = scene;
    g_screenW = screenW;
    g_screenH = screenH;
    g_quit    = false;
    memset(g_keyDown, 0, sizeof(g_keyDown));

    g_camera = new Camera();
    // World-space, so it carries the same scale argWorld() applies.
    g_camera->setPosition(0, 0, -400 * JET32_WORLD_SCALE);
    g_camera->setRotation(0, 0, 0);
    g_camera->setFOV(70.0f, screenW);
    // Camera's defaults (128 / 1024) are in unscaled units and would clip the
    // whole scene away once authored coordinates are scaled up.
    g_camera->nearPlane = 100 * JET32_WORLD_SCALE;
    g_camera->farPlane  = 6000 * JET32_WORLD_SCALE;
    scene->setCamera(g_camera);

    g_dirLight = new DirectionalLight({-90, 0, 0}, {255, 255, 255});
    g_ambLight = new AmbientLight({60, 60, 60});
    scene->setDirectionalLight(g_dirLight);
    scene->setAmbientLight(g_ambLight);

    // Seed 0: the module has no entropy source at start-up and nothing here
    // depends on hash randomisation.
    L = lua_newstate(luaAlloc, nullptr, 0);
    if (!L) { host_log("[jet3d] lua_newstate failed"); return false; }

    // Everything except the debug library. luaL_traceback works without it.
    luaL_openselectedlibs(L,
        LUA_GLIBK | LUA_LOADLIBK | LUA_COLIBK | LUA_IOLIBK |
        LUA_MATHLIBK | LUA_OSLIBK | LUA_STRLIBK | LUA_TABLIBK | LUA_UTF8LIBK,
        0);

    // Generational mode collects the short-lived garbage a frame loop produces
    // without the pause of a full incremental cycle.
    lua_gc(L, LUA_GCGEN);

    registerAPI(L);
    return true;
}

bool jet_lua_run_file(const char* path) {
    if (!L) return false;

    // package.path so the game can require() files beside its main.lua, and
    // jet.dir so it can build paths for its own assets and save files
    // (io.open(jet.dir .. "/save.dat") — the io library is open and the host's
    // fopen/fread/fwrite are SPI-locked).
    char dir[256];
    snprintf(dir, sizeof(dir), "%s", path);
    char* slash = strrchr(dir, '/');
    if (slash) {
        *slash = '\0';
        char pat[512];
        snprintf(pat, sizeof(pat), "%s/?.lua;%s/?/init.lua", dir, dir);
        lua_getglobal(L, "package");
        if (lua_istable(L, -1)) {
            lua_pushstring(L, pat);
            lua_setfield(L, -2, "path");
        }
        lua_pop(L, 1);

        lua_getglobal(L, "jet");
        if (lua_istable(L, -1)) {
            lua_pushstring(L, dir);
            lua_setfield(L, -2, "dir");
        }
        lua_pop(L, 1);
    }

    if (luaL_loadfile(L, path) != LUA_OK) {
        const char* err = lua_tostring(L, -1);
        logf_("[jet3d] load failed: %s", err ? err : "?");
        lua_pop(L, 1);
        return false;
    }
    return protectedCall(L, 0);
}

// Pushes jet.<name> if it is a function. Returns false if absent.
static bool pushCallback(const char* name) {
    lua_getglobal(L, "jet");
    if (!lua_istable(L, -1)) { lua_pop(L, 1); return false; }
    lua_getfield(L, -1, name);
    lua_remove(L, -2);
    if (!lua_isfunction(L, -1)) { lua_pop(L, 1); return false; }
    return true;
}

bool jet_lua_call_load(void) {
    if (!L) return false;
    if (!pushCallback("load")) return true;   // optional
    return protectedCall(L, 0);
}

bool jet_lua_call_frame(float dt) {
    if (!L) return false;
    g_lastDt = dt;

    // The overlay list is rebuilt every frame: the render loop replays it once
    // per band, so it must describe this frame only.
    jet_ovl_begin();

    // Palette animations: consume dt in whole steps so the requested rate is
    // honoured regardless of frame rate.
    for (size_t i = 0; i < g_texAnims.size(); ++i) {
        TexAnim& a = g_texAnims[i];
        a.acc += dt * a.fps;
        const int steps = (int)a.acc;
        if (steps > 0 && a.tex->paletteSize > 0) {
            a.acc -= (float)steps;
            a.tex->paletteOffset = (a.tex->paletteOffset + steps) % a.tex->paletteSize;
        }
    }

    // Sticky pick queries for the render that follows this frame's callbacks.
#if MAX_PICK_QUERIES > 0
    if (g_scene) g_scene->setPickQueries(g_pickQ, MAX_PICK_QUERIES);
#endif

    if (pushCallback("update")) {
        lua_pushnumber(L, dt);
        if (!protectedCall(L, 1)) return false;
    }
    if (pushCallback("draw")) {
        if (!protectedCall(L, 0)) return false;
    }

    // Rigs pose AFTER the callbacks (update() is where the game plays clips
    // and sets procedural poses) and before the render consumes the bone
    // Objects' matrices — same reasoning as the sky update below.
    jet_rigs_update(dt);

    // Sky window tracks camera pitch. After the callbacks, since update() is
    // where the game moves the camera, and before the render reads it.
    jet_sky_update();

    // Edge latches last exactly one frame, cleared after the callbacks have had
    // their chance to read them.
    memset(g_keyHit, 0, sizeof(g_keyHit));
    memset(g_keyRel, 0, sizeof(g_keyRel));
    return true;
}

void jet_lua_close(void) {
    if (L) { lua_close(L); L = nullptr; }

    // Rig structs first; their bone Objects are owned by g_objects, whose
    // forward deletion order keeps each rig's blob-adopting bone last.
    for (size_t i = 0; i < g_rigs.size(); ++i) delete g_rigs[i];
    g_rigs.clear();
    g_objGroups.clear();   // non-owning: the Objects belong to g_objects

    for (size_t i = 0; i < g_objects.size(); ++i)   delete g_objects[i];
    for (size_t i = 0; i < g_materials.size(); ++i) delete g_materials[i];
    for (size_t i = 0; i < g_textures.size(); ++i)  delete g_textures[i];
    for (size_t i = 0; i < g_texturePixels.size(); ++i) free(g_texturePixels[i]);
    for (size_t i = 0; i < g_sprites.size(); ++i)  delete g_sprites[i];
    g_objects.clear();
    g_materials.clear();
    g_implicitMats.clear();   // non-owning: the Materials came from g_materials
    g_sprites.clear();
    g_textures.clear();
    g_texturePixels.clear();

    g_texAnims.clear();       // non-owning: the textures were freed above
#if MAX_PICK_QUERIES > 0
    for (int i = 0; i < MAX_PICK_QUERIES; ++i) { g_pickQ[i].x = -1; g_pickQ[i].y = -1; }
#endif

    delete g_camera;   g_camera   = nullptr;
    delete g_dirLight; g_dirLight = nullptr;
    delete g_ambLight; g_ambLight = nullptr;
    if (g_skyGradient) { free(g_skyGradient); g_skyGradient = nullptr; }
    g_scene = nullptr;
}

void jet_lua_set_key(unsigned char key, int pressed) {
    if (pressed) g_keyHit[key] = 1; else g_keyRel[key] = 1;
    g_keyDown[key] = pressed ? 1 : 0;
}

void jet_lua_add_trackball(int dx, int dy, int click) {
    g_trkDx += dx;
    g_trkDy += dy;
    g_trkClick += click;
}

void jet_lua_set_timing(float fps, float uptime) {
    g_fps = fps;
    g_uptime = uptime;
}

bool jet_lua_wants_quit(void) {
    return g_quit;
}
