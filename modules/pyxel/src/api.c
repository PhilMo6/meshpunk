// The `_pyxel` native module and the MicroPython port hooks (imports, open()).
// pylib/pyxel.py wraps this module into the public `pyxel` API.
//
// Names are interned at runtime (qstr_from_str) rather than listed in genhdr,
// so this file needs no MicroPython code generation step.

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "py/builtin.h"
#include "py/gc.h"
#include "py/lexer.h"
#include "py/mperrno.h"
#include "py/objlist.h"
#include "py/objmodule.h"
#include "py/objstr.h"
#include "py/runtime.h"

#include "px.h"

px_state_t px;

// ---------------------------------------------------------------------------
// Runtime qstrs
// ---------------------------------------------------------------------------
#define PX_QSTRS(X) \
    X(_h) X(_st) X(_buf) X(_imgsrc_obj) X(_pyxel) X(__file__) X(mode) \
    X(colors) X(images) X(tilemaps) X(sounds) X(musics) \
    X(notes) X(tones) X(volumes) X(effects) X(speed) X(seqs) \
    X(frame_count) X(mouse_x) X(mouse_y) X(mouse_wheel) X(width) X(height) \
    X(input_keys) X(input_text) \
    X(title) X(fps) X(quit_key) X(display_scale) X(capture_scale) X(capture_sec) X(headless) \
    X(exclude_images) X(exclude_tilemaps) X(exclude_sounds) X(exclude_musics) X(include_colors) \
    X(hold) X(repeat) X(colkey) X(tilekey) X(rotate) X(scale) X(font) X(fov) \
    X(sample_bits) X(wavetable) X(gain) X(_mml) X(_mml_old) X(_pcm) X(_font) X(_WFile)

#define QENUM(n) Q_##n,
#define QNAME(n) #n,
enum { PX_QSTRS(QENUM) Q_COUNT };
static const char* const QNAMES[] = { PX_QSTRS(QNAME) };
static qstr Q[Q_COUNT];
#define QOBJ(n) MP_OBJ_NEW_QSTR(Q[Q_##n])

// The public pyxel module's globals; pyxel.py hands them over with _bind().
// It stays alive through sys.modules for the whole run.
static mp_obj_dict_t* s_pyxel_dict;

// Timing for the periodic serial report.
extern uint32_t px_gc_count, px_gc_us;
static uint32_t s_rep_start_ms, s_rep_frames, s_rep_updates;
static uint32_t s_rep_update_us, s_rep_draw_us, s_rep_render_us;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static mp_obj_t argkw(size_t n, const mp_obj_t* a, mp_map_t* kw, size_t pos, int q, mp_obj_t def) {
    if (pos < n) return a[pos];
    if (kw) {
        mp_map_elem_t* e = mp_map_lookup(kw, MP_OBJ_NEW_QSTR(Q[q]), MP_MAP_LOOKUP);
        if (e) return e->value;
    }
    return def;
}

static inline float F(mp_obj_t o) { return (float)mp_obj_get_float(o); }
static inline int I(mp_obj_t o) { return (int)mp_obj_get_int(o); }
static inline int colkey_of(mp_obj_t o) { return o == mp_const_none ? -1 : (I(o) & 0xFF); }
static inline float optf(mp_obj_t o, float def) { return o == mp_const_none ? def : F(o); }

static void need_init(void) {
    if (!px.initialized) mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("pyxel is not initialized"));
}

static void raise_msg(const char* msg) {
    mp_raise_msg_varg(&mp_type_RuntimeError, MP_ERROR_TEXT("%s"), msg);
}

// OSError for an errno.h value from vfs.c, as CPython's subclass where one
// exists, with args (errno, "<description>: '<path>'").
static MP_NORETURN void raise_file_error(int e, const char* path) {
    const mp_obj_type_t* type = &mp_type_OSError;
    const char* what;
    switch (e) {
    case ENOENT: type = &mp_type_FileNotFoundError; what = "No such file or directory"; break;
    case EEXIST: type = &mp_type_FileExistsError; what = "File exists"; break;
    case EISDIR: type = &mp_type_IsADirectoryError; what = "Is a directory"; break;
    case EROFS:  what = "Read-only file system"; break;
    case EMFILE: what = "Too many open files"; break;
    case ENOMEM: what = "Out of memory"; break;
    case EBADF:  what = "Bad file descriptor"; break;
    default:     what = "Input/output error"; break;
    }
    char msg[320];
    snprintf(msg, sizeof(msg), "%s: '%s'", what, path);
    mp_obj_t args[2] = { MP_OBJ_NEW_SMALL_INT(e), mp_obj_new_str(msg, strlen(msg)) };
    nlr_raise(mp_obj_exception_make_new(type, 2, 0, args));
}

#define QOBJ_IDX(q) MP_OBJ_NEW_QSTR(Q[q])

static void dict_set(int q, mp_obj_t v) {
    if (s_pyxel_dict) mp_obj_dict_store(MP_OBJ_FROM_PTR(s_pyxel_dict), QOBJ_IDX(q), v);
}

static mp_obj_t dict_get(int q) {
    if (!s_pyxel_dict) return MP_OBJ_NULL;
    mp_map_elem_t* e = mp_map_lookup(&s_pyxel_dict->map, QOBJ_IDX(q), MP_MAP_LOOKUP);
    return e ? e->value : MP_OBJ_NULL;
}

static mp_obj_t tile_tuple(uint16_t t) {
    mp_obj_t items[2] = { MP_OBJ_NEW_SMALL_INT(PX_TILE_X(t)), MP_OBJ_NEW_SMALL_INT(PX_TILE_Y(t)) };
    return mp_obj_new_tuple(2, items);
}

static uint16_t tile_of(mp_obj_t o) {
    size_t n;
    mp_obj_t* items;
    mp_obj_get_array(o, &n, &items);
    if (n != 2) mp_raise_ValueError(MP_ERROR_TEXT("tile must be (image_tx, image_ty)"));
    int tx = I(items[0]), ty = I(items[1]);
    if (tx < 0 || ty < 0 || tx > 255 || ty > 255)
        mp_raise_ValueError(MP_ERROR_TEXT("tile coordinates must be 0-255"));
    return PX_TILE(tx, ty);
}

// ---------------------------------------------------------------------------
// Banks and object resolution
// Handles: images 0..2 are the banks, 3 the screen, 4 the cursor.
// User-created images/tilemaps keep their struct (_st) and pixels (_buf) in
// bytearrays owned by the Python object, so the GC frees them with it.
// ---------------------------------------------------------------------------
#define H_SCREEN 3
#define H_CURSOR 4

px_image* px_bank_image(int i) {
    return (i >= 0 && i < PX_NUM_IMAGES) ? px.images[i] : NULL;
}

px_tilemap* px_bank_tilemap(int i) {
    if (i < 0 || i >= PX_NUM_TILEMAPS) return NULL;
    px_tilemap* tm = px.tilemaps[i];
    if (!tm->cv.data) {
        uint16_t* d = (uint16_t*)calloc((size_t)PX_TILEMAP_SIZE * PX_TILEMAP_SIZE, sizeof(uint16_t));
        if (!d) mp_raise_msg(&mp_type_MemoryError, MP_ERROR_TEXT("tilemap"));
        px_tilemap_init(tm, PX_TILEMAP_SIZE, PX_TILEMAP_SIZE, d, tm->imgsrc);
    }
    return tm;
}

static void* buf_ptr(mp_obj_t o, size_t min_len) {
    mp_buffer_info_t bi;
    mp_get_buffer_raise(o, &bi, MP_BUFFER_RW);
    if (bi.len < min_len) mp_raise_ValueError(MP_ERROR_TEXT("corrupt image object"));
    return bi.buf;
}

// A number names an entry of pyxel.images / pyxel.tilemaps, which a game may
// have replaced with its own object (as upstream, where the lists hold the
// objects the number resolves to).
static mp_obj_t list_entry(int q, mp_obj_t index) {
    mp_obj_t lst = dict_get(q);
    return lst == MP_OBJ_NULL ? MP_OBJ_NULL : mp_obj_subscr(lst, index, MP_OBJ_SENTINEL);
}

// An instance of a class derived from int (an IntEnum member) counts as its
// int value.
static bool bank_number(mp_obj_t o, mp_obj_t* n) {
    *n = mp_obj_cast_to_native_base(o, MP_OBJ_FROM_PTR(&mp_type_int));
    return *n != MP_OBJ_NULL && mp_obj_is_small_int(*n);
}

static px_image* resolve_image(mp_obj_t o) {
    mp_obj_t n;
    if (bank_number(o, &n)) {
        mp_obj_t e = list_entry(Q_images, n);
        if (e != MP_OBJ_NULL) {
            o = e;
        } else {
            px_image* img = px_bank_image(MP_OBJ_SMALL_INT_VALUE(n));
            if (!img) mp_raise_ValueError(MP_ERROR_TEXT("image bank out of range"));
            return img;
        }
    }
    mp_obj_t h = mp_load_attr(o, Q[Q__h]);
    if (h != mp_const_none) {
        int i = I(h);
        if (i == H_SCREEN) return px.screen;
        if (i == H_CURSOR) return px.cursor;
        px_image* img = px_bank_image(i);
        if (!img) mp_raise_ValueError(MP_ERROR_TEXT("image bank out of range"));
        return img;
    }
    px_image* img = (px_image*)buf_ptr(mp_load_attr(o, Q[Q__st]), sizeof(px_image));
    img->cv.data = buf_ptr(mp_load_attr(o, Q[Q__buf]), (size_t)img->cv.w * (size_t)img->cv.h);
    return img;
}

static px_tilemap* resolve_tilemap(mp_obj_t o, mp_obj_t* obj_out) {
    mp_obj_t n;
    if (bank_number(o, &n)) {
        mp_obj_t e = list_entry(Q_tilemaps, n);
        if (e != MP_OBJ_NULL) {
            o = e;
        } else {
            px_tilemap* tm = px_bank_tilemap(MP_OBJ_SMALL_INT_VALUE(n));
            if (!tm) mp_raise_ValueError(MP_ERROR_TEXT("tilemap out of range"));
            if (obj_out) *obj_out = MP_OBJ_NULL;
            return tm;
        }
    }
    if (obj_out) *obj_out = o;
    mp_obj_t h = mp_load_attr(o, Q[Q__h]);
    if (h != mp_const_none) {
        px_tilemap* tm = px_bank_tilemap(I(h));
        if (!tm) mp_raise_ValueError(MP_ERROR_TEXT("tilemap out of range"));
        return tm;
    }
    px_tilemap* tm = (px_tilemap*)buf_ptr(mp_load_attr(o, Q[Q__st]), sizeof(px_tilemap));
    tm->cv.data = buf_ptr(mp_load_attr(o, Q[Q__buf]),
                          (size_t)tm->cv.w * (size_t)tm->cv.h * sizeof(uint16_t));
    return tm;
}

// The image a tilemap draws from: its bank index, or the Image object the
// game assigned to imgsrc (kept on the Python object as _imgsrc_obj).
static px_image* tilemap_source(px_tilemap* tm, mp_obj_t tm_obj) {
    if (tm->imgsrc >= 0) return resolve_image(MP_OBJ_NEW_SMALL_INT(tm->imgsrc));
    if (tm_obj == MP_OBJ_NULL) mp_raise_ValueError(MP_ERROR_TEXT("tilemap has no image source"));
    return resolve_image(mp_load_attr(tm_obj, Q[Q__imgsrc_obj]));
}

static mp_obj_t alloc_pair(size_t st_size, size_t buf_size, void** st, void** buf) {
    byte* s = m_new0(byte, st_size);
    byte* b = m_new0(byte, buf_size ? buf_size : 1);
    *st = s;
    *buf = b;
    mp_obj_t items[2] = { mp_obj_new_bytearray_by_ref(st_size, s),
                          mp_obj_new_bytearray_by_ref(buf_size ? buf_size : 1, b) };
    return mp_obj_new_tuple(2, items);
}

// ---------------------------------------------------------------------------
// Colors: pyxel.colors (a list subclass in pyxel.py) is the source of truth.
// ---------------------------------------------------------------------------
static mp_obj_t colors_list(void) {
    mp_obj_t o = dict_get(Q_colors);
    if (o == MP_OBJ_NULL) return MP_OBJ_NULL;
    return mp_obj_cast_to_native_base(o, MP_OBJ_FROM_PTR(&mp_type_list));
}

static void colors_from_python(void) {
    mp_obj_t lst = colors_list();
    if (lst == MP_OBJ_NULL) return;
    size_t n;
    mp_obj_t* items;
    mp_obj_list_get(lst, &n, &items);
    if (n > PX_MAX_COLORS) n = PX_MAX_COLORS;
    for (size_t i = 0; i < n; i++) px.colors[i] = (uint32_t)mp_obj_get_int(items[i]) & 0xFFFFFF;
    px.num_colors = (int)n;
}

static void colors_to_python(void) {
    mp_obj_t lst = colors_list();
    if (lst == MP_OBJ_NULL) return;
    mp_obj_list_t* l = MP_OBJ_TO_PTR(lst);
    mp_obj_t sl = mp_obj_new_slice(mp_const_none, mp_const_none, mp_const_none);
    mp_obj_t items = mp_obj_new_list(0, NULL);
    for (int i = 0; i < px.num_colors; i++)
        mp_obj_list_append(items, mp_obj_new_int((mp_int_t)px.colors[i]));
    mp_obj_subscr(MP_OBJ_FROM_PTR(l), sl, items);
    px.colors_dirty = false;
}

// ---------------------------------------------------------------------------
// State lifecycle
// ---------------------------------------------------------------------------
static const char* const CURSOR_DATA[8] = {
    "11111100", "17776100", "17761000", "17676100",
    "16167610", "11016761", "00001610", "00000100",
};
static const uint32_t DEFAULT_COLORS[PX_NUM_COLORS] = {
    0x000000, 0x2b335f, 0x7e2072, 0x19959c, 0x8b4852, 0x395c98, 0xa9c1ff, 0xeeeeee,
    0xd4186c, 0xd38441, 0xe9c35b, 0x70c6a9, 0x7696de, 0xa3a3a3, 0xff9798, 0xedc7b0,
};

static void state_free(void) {
    px_display_end();
    px_image_free(px.screen);
    px_image_free(px.cursor);
    for (int i = 0; i < PX_NUM_IMAGES; i++) px_image_free(px.images[i]);
    for (int i = 0; i < PX_NUM_TILEMAPS; i++) {
        if (px.tilemaps[i]) free(px.tilemaps[i]->cv.data);
        free(px.tilemaps[i]);
    }
    memset(&px, 0, sizeof(px));
}

static void state_alloc(void) {
    state_free();
    for (int i = 0; i < PX_NUM_IMAGES; i++) {
        px.images[i] = px_image_new(PX_IMAGE_SIZE, PX_IMAGE_SIZE);
        if (!px.images[i]) mp_raise_msg(&mp_type_MemoryError, MP_ERROR_TEXT("image banks"));
    }
    for (int i = 0; i < PX_NUM_TILEMAPS; i++) {
        px.tilemaps[i] = (px_tilemap*)calloc(1, sizeof(px_tilemap));
        if (!px.tilemaps[i]) mp_raise_msg(&mp_type_MemoryError, MP_ERROR_TEXT("tilemaps"));
        px_tilemap_init(px.tilemaps[i], PX_TILEMAP_SIZE, PX_TILEMAP_SIZE, NULL, 0);
    }
    px.cursor = px_image_new(8, 8);
    if (!px.cursor) mp_raise_msg(&mp_type_MemoryError, MP_ERROR_TEXT("cursor"));
    uint8_t* c = (uint8_t*)px.cursor->cv.data;
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++) c[y * 8 + x] = (uint8_t)(CURSOR_DATA[y][x] - '0');
    memcpy(px.colors, DEFAULT_COLORS, sizeof(DEFAULT_COLORS));
    px.num_colors = PX_NUM_COLORS;
    px.quit_key = PX_KEY_ESCAPE;
    px.fps = 30;
}

void px_api_shutdown(void) {
    px_wfile_close_all();
    px_audio_shutdown();
    px_font_shutdown();
    state_free();
    s_pyxel_dict = NULL;
}

// Waits (frame pacing, time.sleep) sleep at most this long at a time, with
// px_audio_service() between sleeps: the audio ring runs 75 ms ahead of real
// time, so a single sleep for a whole frame (100 ms at fps=10) lets it run dry.
#define SLEEP_SLICE_MS 10

// ---------------------------------------------------------------------------
// For pylib's time, datetime and os modules
// ---------------------------------------------------------------------------
static uint64_t s_ticks_total;         // microseconds since the first call
static uint32_t s_ticks_last;
static bool s_ticks_started;

// The firmware's 32-bit microsecond tick wraps every 71.6 minutes; this is
// called every frame (begin_update) so no wrap is missed.
static uint64_t ticks_us64(void) {
    uint32_t now = host_get_ticks_us();
    if (!s_ticks_started) {
        s_ticks_started = true;
        s_ticks_last = now;
    }
    s_ticks_total += (uint32_t)(now - s_ticks_last);
    s_ticks_last = now;
    return s_ticks_total;
}

// clock() -> (epoch, utc_offset_minutes): the device clock when the game
// was launched (-clock), epoch 0 when the launcher did not pass one.
static mp_obj_t f_clock(void) {
    mp_obj_t items[2] = { mp_obj_new_int_from_ll(px_clock_epoch), MP_OBJ_NEW_SMALL_INT(px_clock_tzmin) };
    return mp_obj_new_tuple(2, items);
}
static MP_DEFINE_CONST_FUN_OBJ_0(f_clock_obj, f_clock);

// ticks_us() -> microseconds since the game started, as an int.
static mp_obj_t f_ticks_us(void) {
    return mp_obj_new_int_from_ull(ticks_us64());
}
static MP_DEFINE_CONST_FUN_OBJ_0(f_ticks_us_obj, f_ticks_us);

// sleep(seconds): audio keeps playing; the quit keys still end the game.
static mp_obj_t f_sleep(mp_obj_t sec_in) {
    mp_float_t sec = mp_obj_get_float(sec_in);
    if (sec < 0) mp_raise_ValueError(MP_ERROR_TEXT("sleep length must be non-negative"));
    uint64_t end = ticks_us64() + (uint64_t)(sec * (mp_float_t)1000000);
    for (;;) {
        uint64_t now = ticks_us64();
        if (now >= end) break;
        if (host_should_exit()) mp_raise_type(&mp_type_SystemExit);
        uint64_t ms = (end - now) / 1000;
        host_sleep_ms(ms < SLEEP_SLICE_MS ? (uint32_t)ms : SLEEP_SLICE_MS);
        px_audio_service();
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(f_sleep_obj, f_sleep);

static mp_obj_t f_getcwd(void) {
    const char* cwd = px_vfs_cwd();
    return mp_obj_new_str(cwd, strlen(cwd));
}
static MP_DEFINE_CONST_FUN_OBJ_0(f_getcwd_obj, f_getcwd);

static mp_obj_t f_chdir(mp_obj_t path) {
    px_vfs_set_cwd(mp_obj_str_get_str(path));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(f_chdir_obj, f_chdir);

// path_stat(path) -> 0 missing, 1 file, 2 directory (px_vfs_stat).
static mp_obj_t f_path_stat(mp_obj_t path) {
    return MP_OBJ_NEW_SMALL_INT(px_vfs_stat(mp_obj_str_get_str(path)));
}
static MP_DEFINE_CONST_FUN_OBJ_1(f_path_stat_obj, f_path_stat);

static void listdir_add(void* ctx, const char* name, size_t len) {
    mp_obj_t lst = MP_OBJ_FROM_PTR(ctx);
    mp_obj_t s = mp_obj_new_str(name, len);
    size_t n;
    mp_obj_t* items;
    mp_obj_list_get(lst, &n, &items);
    for (size_t i = 0; i < n; i++)
        if (mp_obj_equal(items[i], s)) return;
    mp_obj_list_append(lst, s);
}

// listdir(path) -> names; inside the mounted .pyxapp only.
static mp_obj_t f_listdir(mp_obj_t path_in) {
    const char* path = mp_obj_str_get_str(path_in);
    mp_obj_t lst = mp_obj_new_list(0, NULL);
    int e = px_vfs_listdir(path, listdir_add, MP_OBJ_TO_PTR(lst));
    if (e < 0) {
        mp_raise_msg_varg(&mp_type_OSError, MP_ERROR_TEXT("listing '%s' is not supported: only folders inside a .pyxapp can be listed"), path);
    }
    if (e) raise_file_error(e, path);
    return lst;
}
static MP_DEFINE_CONST_FUN_OBJ_1(f_listdir_obj, f_listdir);

// Font(filename) handle / Font.text_width(s)
static mp_obj_t f_font_load(mp_obj_t fn) {
    char err[160];
    int h = px_font_load(mp_obj_str_get_str(fn), err, sizeof(err));
    if (h < 0) raise_msg(err);
    return MP_OBJ_NEW_SMALL_INT(h);
}
static MP_DEFINE_CONST_FUN_OBJ_1(f_font_load_obj, f_font_load);

static mp_obj_t f_font_text_width(mp_obj_t h, mp_obj_t s) {
    size_t len;
    const char* str = mp_obj_str_get_data(s, &len);
    return MP_OBJ_NEW_SMALL_INT(px_font_text_width((int)mp_obj_get_int(h), str, len));
}
static MP_DEFINE_CONST_FUN_OBJ_2(f_font_text_width_obj, f_font_text_width);

// ---------------------------------------------------------------------------
// Frame loop (system.rs)
// ---------------------------------------------------------------------------
static void publish_frame_state(void) {
    dict_set(Q_frame_count, mp_obj_new_int_from_uint(px.frame_count));
    dict_set(Q_mouse_x, MP_OBJ_NEW_SMALL_INT(px.mouse_x));
    dict_set(Q_mouse_y, MP_OBJ_NEW_SMALL_INT(px.mouse_y));
    dict_set(Q_mouse_wheel, MP_OBJ_NEW_SMALL_INT(px.mouse_wheel));
    char text[32];
    uint32_t keys[16];
    int nkeys = 0;
    px_input_frame_text(text, sizeof(text), keys, &nkeys, 16);
    static bool s_had_input = true;
    if (nkeys || text[0] || s_had_input) {
        mp_obj_t lst = mp_obj_new_list(0, NULL);
        for (int i = 0; i < nkeys; i++) mp_obj_list_append(lst, mp_obj_new_int_from_uint(keys[i]));
        dict_set(Q_input_keys, lst);
        dict_set(Q_input_text, mp_obj_new_str(text, strlen(text)));
        s_had_input = nkeys || text[0];
    }
}

// begin_update_frame: poll input, then the quit key and the firmware's exit.
static void begin_update(void) {
    ticks_us64();
    px_input_poll();
    publish_frame_state();
    if (px_btnp(px.quit_key, 0, 0) || host_should_exit())
        mp_raise_type(&mp_type_SystemExit);
}

static void finish_draw(void) {
    uint32_t t0 = host_get_ticks_us();
    if (px.colors_dirty) colors_to_python();
    colors_from_python();
    px_draw_cursor();
    px_display_render();
    s_rep_render_us += host_get_ticks_us() - t0;
    s_rep_frames++;
}

static void report_tick(void) {
    uint32_t now = host_get_ticks_ms();
    uint32_t span = now - s_rep_start_ms;
    if (span < 5000) return;
    gc_info_t gi;
    gc_info(&gi);
    uint32_t f = s_rep_frames ? s_rep_frames : 1;
    uint32_t u = s_rep_updates ? s_rep_updates : 1;
    printf("[pyxel] fps=%u.%u/%d upd=%u.%ums(x%u) draw=%u.%ums render=%u.%ums gc=%ux %ums heap=%uK/%uK "
           "snd=%u/s %ums/s under=%u\n",
           (unsigned)(s_rep_frames * 1000 / span), (unsigned)(s_rep_frames * 10000 / span % 10), px.fps,
           (unsigned)(s_rep_update_us / u / 1000), (unsigned)(s_rep_update_us / u / 100 % 10),
           (unsigned)s_rep_updates,
           (unsigned)(s_rep_draw_us / f / 1000), (unsigned)(s_rep_draw_us / f / 100 % 10),
           (unsigned)(s_rep_render_us / f / 1000), (unsigned)(s_rep_render_us / f / 100 % 10),
           (unsigned)px_gc_count, (unsigned)(px_gc_us / 1000),
           (unsigned)(gi.used / 1024), (unsigned)((gi.used + gi.free) / 1024),
           (unsigned)((uint64_t)px_audio_samples * 1000 / span),
           (unsigned)((uint64_t)px_audio_us / span), (unsigned)px_audio_underruns);
    s_rep_start_ms = now;
    s_rep_frames = s_rep_updates = 0;
    s_rep_update_us = s_rep_draw_us = s_rep_render_us = 0;
    px_gc_count = px_gc_us = 0;
    px_audio_samples = px_audio_underruns = px_audio_us = 0;
}

static void run_update(mp_obj_t update) {
    begin_update();
    uint32_t t0 = host_get_ticks_us();
    if (update != mp_const_none) mp_call_function_0(update);
    s_rep_update_us += host_get_ticks_us() - t0;
    s_rep_updates++;
}

static void run_draw(mp_obj_t draw) {
    uint32_t t0 = host_get_ticks_us();
    if (draw != mp_const_none) mp_call_function_0(draw);
    s_rep_draw_us += host_get_ticks_us() - t0;
    finish_draw();
}

#define MAX_FRAME_DELAY_MS 100

static mp_obj_t f_run(mp_obj_t update, mp_obj_t draw) {
    need_init();
    uint32_t frame_us = 1000000u / (uint32_t)px.fps;
    uint32_t last = host_get_ticks_us();
    uint32_t next = last;
    s_rep_start_ms = host_get_ticks_ms();
    for (;;) {
        uint32_t now = host_get_ticks_us();
        int32_t wait = (int32_t)(next - now);
        if (wait > 0) {
            px_audio_service();
            if (wait >= 2000) {
                uint32_t ms = (uint32_t)wait / 1000 - 1;
                host_sleep_ms(ms < SLEEP_SLICE_MS ? ms : SLEEP_SLICE_MS);
            }
            continue;
        }
        uint32_t delta_us = now - last;
        last = now;
        next += frame_us;
        if ((int32_t)(now - next) > (int32_t)(frame_us * 4)) next = now + frame_us;

        int update_count = delta_us > MAX_FRAME_DELAY_MS * 1000u ? 1 : (int)(delta_us / frame_us);
        for (int i = 1; i < update_count; i++) {
            run_update(update);
            px.frame_count++;
        }
        run_update(update);
        run_draw(draw);
        // Fed every frame, not only while waiting for the next one: a game
        // running behind its frame rate never waits.
        px_audio_service();
        px.frame_count++;
        report_tick();
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(f_run_obj, f_run);

static uint32_t s_flip_next;

static mp_obj_t f_flip(void) {
    need_init();
    finish_draw();
    px.frame_count++;
    uint32_t frame_us = 1000000u / (uint32_t)px.fps;
    px_audio_service();
    uint32_t now = host_get_ticks_us();
    if ((int32_t)(s_flip_next - now) > 0) {
        int32_t wait;
        while ((wait = (int32_t)(s_flip_next - host_get_ticks_us())) >= 1000) {
            uint32_t ms = (uint32_t)wait / 1000;
            host_sleep_ms(ms < SLEEP_SLICE_MS ? ms : SLEEP_SLICE_MS);
            px_audio_service();
        }
        s_flip_next += frame_us;
    } else {
        s_flip_next = now + frame_us;
    }
    report_tick();
    begin_update();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(f_flip_obj, f_flip);

static mp_obj_t f_show(void) {
    need_init();
    for (;;) f_flip();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(f_show_obj, f_show);

static mp_obj_t f_quit(void) {
    mp_raise_type(&mp_type_SystemExit);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(f_quit_obj, f_quit);

static mp_obj_t f_reset(void) {
    px.reset_requested = true;
    mp_raise_type(&mp_type_SystemExit);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(f_reset_obj, f_reset);

// input([prompt]), the builtin. CPython reads a line from stdin; here the line
// is typed on input()'s own 320x240 screen in the default palette. It shows
// the newest console lines (what the game printed, the prompt included), the
// line being typed and a hint, drawn from the top: a touch keyboard covers the
// panel below row INPUT_TOP_H, so without -kbtoggle (a board with no keyboard)
// only that band is used. Keys arrive as typed with the launcher's key
// bindings off (Alt+Enter, -kbtoggle) or from the touch keyboard. The game's
// screen, palette and display come back afterwards.
#define INPUT_LINE_MAX 127
#define INPUT_TOP_H    60
// The firmware stamps its touch pad indicators and touch keyboard into each
// pushed frame, and closing the keyboard leaves its area black until the next
// frame, so the input screen is pushed every INPUT_FRAME_MS, not only when
// the text changes.
#define INPUT_FRAME_MS 33

static void input_draw(px_image* img, const char* line, size_t len) {
    const int lh = PX_FONT_HEIGHT + 1;
    int rows = ((px_kbtoggle ? img->cv.h : INPUT_TOP_H) - 2) / lh;
    int hist = rows - 2;
    px_clear_u8(&img->cv, 0);
    int shown = 0;
    while (shown < hist && px_console_line(shown)) shown++;
    int y = 2;
    for (int i = shown - 1; i >= 0; i--, y += lh) {
        const char* s = px_console_line(i);
        px_image_text(img, 2.0f, (float)y, s, strlen(s), 7);
    }
    // The line still being printed + the typed text + a cursor: its last
    // PX_CONSOLE_COLS characters.
    char text[PX_CONSOLE_COLS];
    size_t plen;
    const char* part = px_console_partial(&plen);
    size_t total = plen + len + 1;
    size_t skip = total > PX_CONSOLE_COLS ? total - PX_CONSOLE_COLS : 0;
    size_t n = 0;
    for (size_t i = skip; i < total; i++)
        text[n++] = i < plen ? part[i] : i < plen + len ? line[i - plen] : '_';
    px_image_text(img, 2.0f, (float)y, text, n, 11);
    const char* hint = px_kbtoggle ? "Alt+Enter: typing  Enter: done" : "Enter: done";
    px_image_text(img, 2.0f, (float)(y + lh), hint, strlen(hint), 13);
}

static mp_obj_t f_input(size_t n, const mp_obj_t* a) {
    if (n) {
        mp_obj_t p = mp_obj_is_str(a[0]) ? a[0] : mp_call_function_1(MP_OBJ_FROM_PTR(&mp_type_str), a[0]);
        size_t plen;
        const char* prompt = mp_obj_str_get_data(p, &plen);
        px_console_write(prompt, plen);
    }
    px_image* screen = px_image_new(PX_SCREEN_MAX_W, PX_SCREEN_MAX_H);
    if (!screen) mp_raise_msg(&mp_type_MemoryError, MP_ERROR_TEXT("input screen"));
    px_image* game_screen = px.screen;
    uint32_t game_colors[PX_MAX_COLORS];
    int game_num_colors = px.num_colors;
    memcpy(game_colors, px.colors, sizeof(game_colors));
    px.screen = screen;
    memcpy(px.colors, DEFAULT_COLORS, sizeof(DEFAULT_COLORS));
    px.num_colors = PX_NUM_COLORS;
    char err[80] = "";
    bool shown = px_display_begin(PX_SCREEN_MAX_W, PX_SCREEN_MAX_H, err, sizeof(err));
    char line[INPUT_LINE_MAX + 1];
    size_t len = 0;
    bool enter_down = false, done = !shown, redraw = true, quit = false;
    uint32_t next_frame = host_get_ticks_ms();
    while (!done) {
        if (host_should_exit()) { quit = true; break; }
        int pressed;
        unsigned char key;
        while (!done && host_get_key(&pressed, &key)) {
            if (key == 0x0D) {                     // finish on Enter's release
                if (pressed) enter_down = true;
                else if (enter_down) done = true;
            } else if (!pressed) {
            } else if (key == 0x08) {
                if (len) { len--; redraw = true; }
            } else if (key >= 0x20 && key < 0x7F && len < INPUT_LINE_MAX) {
                line[len++] = (char)key;
                redraw = true;
            }
        }
        uint32_t now = host_get_ticks_ms();
        if (redraw) {
            input_draw(screen, line, len);
            redraw = false;
            next_frame = now;
        }
        if ((int32_t)(now - next_frame) >= 0) {
            px_display_render();
            next_frame = now + INPUT_FRAME_MS;
        }
        px_audio_service();
        host_sleep_ms(10);
    }
    px.screen = game_screen;
    memcpy(px.colors, game_colors, sizeof(game_colors));
    px.num_colors = game_num_colors;
    px_image_free(screen);
    char err2[80] = "";
    bool back = true;
    if (px.initialized) back = px_display_begin(px.width, px.height, err2, sizeof(err2));
    else px_display_end();
    if (!shown) raise_msg(err);
    if (!back) raise_msg(err2);
    px_input_reset();
    if (quit) mp_raise_type(&mp_type_SystemExit);
    px_console_write(line, len);
    px_console_write("\n", 1);
    return mp_obj_new_str(line, len);
}
MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(px_builtin_input_obj, 0, 1, f_input);

// init(width, height, title, fps, quit_key, display_scale, capture_scale,
//      capture_sec, headless). Like upstream, relative paths from here on
// resolve against the directory of the file that called init().
static mp_obj_t f_init(size_t n, const mp_obj_t* a, mp_map_t* kw) {
    int w = I(a[0]);
    int h = I(a[1]);
    mp_obj_t fps = argkw(n, a, kw, 3, Q_fps, mp_const_none);
    mp_obj_t qk = argkw(n, a, kw, 4, Q_quit_key, mp_const_none);

    mp_map_elem_t* fe = mp_map_lookup(&mp_globals_get()->map, QOBJ(__file__), MP_MAP_LOOKUP);
    if (fe && mp_obj_is_str(fe->value)) {
        char dir[256];
        px_path_dirname(mp_obj_str_get_str(fe->value), dir, sizeof(dir));
        px_vfs_set_cwd(dir);
    }

    if (!px.images[0]) state_alloc();
    px_image_free(px.screen);
    px.screen = px_image_new(w > 0 ? w : 1, h > 0 ? h : 1);
    if (!px.screen) mp_raise_msg(&mp_type_MemoryError, MP_ERROR_TEXT("screen"));
    px.width = w;
    px.height = h;
    px.fps = fps == mp_const_none ? 30 : I(fps);
    if (px.fps <= 0) mp_raise_ValueError(MP_ERROR_TEXT("fps must be greater than 0"));
    px.quit_key = qk == mp_const_none ? PX_KEY_ESCAPE : (uint32_t)mp_obj_get_int(qk);
    char err[96];
    if (!px_display_begin(w, h, err, sizeof(err))) raise_msg(err);
    px_input_reset();
    px.frame_count = 0;
    px.initialized = true;
    dict_set(Q_width, MP_OBJ_NEW_SMALL_INT(w));
    dict_set(Q_height, MP_OBJ_NEW_SMALL_INT(h));
    publish_frame_state();
    printf("[pyxel] init %dx%d fps=%d cwd=%s\n", w, h, px.fps, px_vfs_cwd());
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(f_init_obj, 2, f_init);

static mp_obj_t f_resize(mp_obj_t wo, mp_obj_t ho) {
    need_init();
    int w = I(wo), h = I(ho);
    char err[96];
    if (!px_display_begin(w, h, err, sizeof(err))) raise_msg(err);
    px_image_free(px.screen);
    px.screen = px_image_new(w, h);
    if (!px.screen) mp_raise_msg(&mp_type_MemoryError, MP_ERROR_TEXT("screen"));
    px.width = w;
    px.height = h;
    dict_set(Q_width, MP_OBJ_NEW_SMALL_INT(w));
    dict_set(Q_height, MP_OBJ_NEW_SMALL_INT(h));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(f_resize_obj, f_resize);

// title/icon/fullscreen/screen_mode/perf_monitor/integer_scale configure the
// desktop window; the panel has no window, so they have no effect here.
static mp_obj_t f_window_noop(size_t n, const mp_obj_t* a, mp_map_t* kw) {
    (void)n; (void)a; (void)kw;
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(f_window_noop_obj, 0, f_window_noop);

static mp_obj_t f_bind(mp_obj_t globals) {
    s_pyxel_dict = MP_OBJ_TO_PTR(globals);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(f_bind_obj, f_bind);

// ---------------------------------------------------------------------------
// Resources
// ---------------------------------------------------------------------------
static mp_obj_t intarr_list(const px_intarr* a) {
    mp_obj_t lst = mp_obj_new_list(0, NULL);
    for (int i = 0; i < a->n; i++) mp_obj_list_append(lst, mp_obj_new_int(a->v[i]));
    return lst;
}

static void sink_sound(void* ctx, int index, const px_intarr* notes, const px_intarr* tones,
                       const px_intarr* volumes, const px_intarr* effects, int speed) {
    (void)ctx;
    mp_obj_t lst = dict_get(Q_sounds);
    if (lst == MP_OBJ_NULL) return;
    size_t n;
    mp_obj_t* items;
    mp_obj_list_get(lst, &n, &items);
    if ((size_t)index >= n) return;
    mp_obj_t s = items[index];
    mp_store_attr(s, Q[Q_notes], intarr_list(notes));
    mp_store_attr(s, Q[Q_tones], intarr_list(tones));
    mp_store_attr(s, Q[Q_volumes], intarr_list(volumes));
    mp_store_attr(s, Q[Q_effects], intarr_list(effects));
    mp_store_attr(s, Q[Q_speed], MP_OBJ_NEW_SMALL_INT(speed));
}

static void sink_music(void* ctx, int index, const px_intarr* seqs) {
    (void)ctx;
    mp_obj_t lst = dict_get(Q_musics);
    if (lst == MP_OBJ_NULL) return;
    size_t n;
    mp_obj_t* items;
    mp_obj_list_get(lst, &n, &items);
    if ((size_t)index >= n) return;
    mp_obj_t rows = mp_obj_new_list(0, NULL);
    for (int r = 0; r < seqs->rows; r++) {
        mp_obj_t row = mp_obj_new_list(0, NULL);
        for (int k = 0; k < seqs->row_len[r]; k++)
            mp_obj_list_append(row, mp_obj_new_int(seqs->v[seqs->row_start[r] + k]));
        mp_obj_list_append(rows, row);
    }
    mp_store_attr(items[index], Q[Q_seqs], rows);
}

static mp_obj_t f_load(size_t n, const mp_obj_t* a, mp_map_t* kw) {
    need_init();
    const char* path = mp_obj_str_get_str(a[0]);
    bool ex_img = mp_obj_is_true(argkw(n, a, kw, 1, Q_exclude_images, mp_const_false));
    bool ex_tm = mp_obj_is_true(argkw(n, a, kw, 2, Q_exclude_tilemaps, mp_const_false));
    bool ex_snd = mp_obj_is_true(argkw(n, a, kw, 3, Q_exclude_sounds, mp_const_false));
    bool ex_mus = mp_obj_is_true(argkw(n, a, kw, 4, Q_exclude_musics, mp_const_false));
    px_res_sink sink = { sink_sound, sink_music, NULL };
    char err[160];
    if (!px_res_load(path, ex_img, ex_tm, ex_snd, ex_mus, &sink, err, sizeof(err))) raise_msg(err);
    uint32_t pal[PX_MAX_COLORS];
    int np = px_res_load_pal(path, pal, PX_MAX_COLORS);
    if (np > 0) {
        memcpy(px.colors, pal, sizeof(uint32_t) * (size_t)np);
        px.num_colors = np;
        colors_to_python();
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(f_load_obj, 1, f_load);

static mp_obj_t f_load_pal(mp_obj_t fn) {
    char path[256];
    snprintf(path, sizeof(path), "%s", mp_obj_str_get_str(fn));
    uint32_t pal[PX_MAX_COLORS];
    int np = px_res_load_pal(path, pal, PX_MAX_COLORS);
    if (np <= 0) raise_msg("Failed to load palette file");
    memcpy(px.colors, pal, sizeof(uint32_t) * (size_t)np);
    px.num_colors = np;
    colors_to_python();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(f_load_pal_obj, f_load_pal);

// A pyxel.tilemaps entry for saving. An unused bank has no tile data yet;
// px_res_build writes that as all zero tiles.
static px_tilemap* save_tilemap(mp_obj_t o) {
    mp_obj_t h = mp_load_attr(o, Q[Q__h]);
    if (h == mp_const_none) return resolve_tilemap(o, NULL);
    int i = I(h);
    if (i < 0 || i >= PX_NUM_TILEMAPS) mp_raise_ValueError(MP_ERROR_TEXT("tilemap out of range"));
    return px.tilemaps[i];
}

// save(filename, images, tilemaps, sounds_toml, musics_toml): pyxel.save
// passes an empty list for an excluded image/tilemap bank and "" for
// excluded sounds/musics.
static mp_obj_t f_save(size_t n, const mp_obj_t* a) {
    (void)n;
    need_init();
    const char* path = mp_obj_str_get_str(a[0]);
    size_t ni, nt;
    mp_obj_t* io;
    mp_obj_t* to;
    mp_obj_get_array(a[1], &ni, &io);
    mp_obj_get_array(a[2], &nt, &to);
    px_image** imgs = m_new(px_image*, ni + 1);
    px_tilemap** tms = m_new(px_tilemap*, nt + 1);
    for (size_t i = 0; i < ni; i++) {
        imgs[i] = resolve_image(io[i]);
        if (imgs[i]->cv.w <= 0 || imgs[i]->cv.h <= 0)
            mp_raise_ValueError(MP_ERROR_TEXT("cannot save an image with no pixels"));
    }
    for (size_t i = 0; i < nt; i++) {
        tms[i] = save_tilemap(to[i]);
        if (tms[i]->cv.w <= 0 || tms[i]->cv.h <= 0)
            mp_raise_ValueError(MP_ERROR_TEXT("cannot save a tilemap with no tiles"));
    }
    size_t zn = 0;
    unsigned char* z = px_res_build(imgs, (int)ni, tms, (int)nt, mp_obj_str_get_str(a[3]),
                                    mp_obj_str_get_str(a[4]), &zn);
    m_del(px_image*, imgs, ni + 1);
    m_del(px_tilemap*, tms, nt + 1);
    if (!z) mp_raise_msg(&mp_type_MemoryError, MP_ERROR_TEXT("resource file"));
    int e = px_vfs_write(path, z, zn);
    free(z);
    if (e) raise_file_error(e, path);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(f_save_obj, 5, 5, f_save);

// Files open()ed for writing (pyxel._WFile); h is the px_wfile handle.
static mp_obj_t f_file_write(mp_obj_t h, mp_obj_t data) {
    mp_buffer_info_t bi;
    mp_get_buffer_raise(data, &bi, MP_BUFFER_READ);
    int e = px_wfile_write(I(h), bi.buf, bi.len);
    if (e == ENOMEM) mp_raise_msg(&mp_type_MemoryError, MP_ERROR_TEXT("file contents"));
    if (e) raise_file_error(e, "");
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(f_file_write_obj, f_file_write);

static mp_obj_t f_file_flush(mp_obj_t h, mp_obj_t name) {
    int e = px_wfile_flush(I(h));
    if (e) raise_file_error(e, mp_obj_str_get_str(name));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(f_file_flush_obj, f_file_flush);

static mp_obj_t f_file_close(mp_obj_t h, mp_obj_t name) {
    int e = px_wfile_close(I(h));
    if (e) raise_file_error(e, mp_obj_str_get_str(name));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(f_file_close_obj, f_file_close);

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------
static bool is_mouse_key(uint32_t k) {
    return k >= PX_MOUSE_START && k < PX_GAMEPAD_START;
}

static mp_obj_t f_btn(mp_obj_t k) {
    uint32_t key = (uint32_t)mp_obj_get_int(k);
    if (is_mouse_key(key)) px_input_enable_mouse();
    return mp_obj_new_bool(px_btn(key));
}
static MP_DEFINE_CONST_FUN_OBJ_1(f_btn_obj, f_btn);

static mp_obj_t f_btnp(size_t n, const mp_obj_t* a, mp_map_t* kw) {
    uint32_t key = (uint32_t)mp_obj_get_int(a[0]);
    if (is_mouse_key(key)) px_input_enable_mouse();
    mp_obj_t hold = argkw(n, a, kw, 1, Q_hold, mp_const_none);
    mp_obj_t rep = argkw(n, a, kw, 2, Q_repeat, mp_const_none);
    return mp_obj_new_bool(px_btnp(key, hold == mp_const_none ? 0 : I(hold),
                                   rep == mp_const_none ? 0 : I(rep)));
}
static MP_DEFINE_CONST_FUN_OBJ_KW(f_btnp_obj, 1, f_btnp);

static mp_obj_t f_btnr(mp_obj_t k) {
    uint32_t key = (uint32_t)mp_obj_get_int(k);
    if (is_mouse_key(key)) px_input_enable_mouse();
    return mp_obj_new_bool(px_btnr(key));
}
static MP_DEFINE_CONST_FUN_OBJ_1(f_btnr_obj, f_btnr);

static mp_obj_t f_btnv(mp_obj_t k) {
    uint32_t key = (uint32_t)mp_obj_get_int(k);
    if (is_mouse_key(key)) px_input_enable_mouse();
    return mp_obj_new_int(px_btnv(key));
}
static MP_DEFINE_CONST_FUN_OBJ_1(f_btnv_obj, f_btnv);

static mp_obj_t f_mouse(mp_obj_t v) {
    px.mouse_visible = mp_obj_is_true(v);
    if (px.mouse_visible) px_input_enable_mouse();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(f_mouse_obj, f_mouse);

// ---------------------------------------------------------------------------
// Drawing: one implementation per primitive, bound twice — on the screen
// (pyxel.rect) and on an explicit image (Image.rect -> _pyxel.i_rect).
// ---------------------------------------------------------------------------
static inline uint8_t PC(px_image* d, mp_obj_t c) { return d->pal[I(c) & 0xFF]; }

static mp_obj_t g_cls(px_image* d, const mp_obj_t* a) { px_clear_u8(&d->cv, PC(d, a[0])); return mp_const_none; }
static mp_obj_t g_pget(px_image* d, const mp_obj_t* a) { return MP_OBJ_NEW_SMALL_INT(px_value_u8(&d->cv, F(a[0]), F(a[1]))); }
static mp_obj_t g_pset(px_image* d, const mp_obj_t* a) { px_set_value_u8(&d->cv, F(a[0]), F(a[1]), PC(d, a[2])); return mp_const_none; }
static mp_obj_t g_line(px_image* d, const mp_obj_t* a) { px_line_u8(&d->cv, F(a[0]), F(a[1]), F(a[2]), F(a[3]), PC(d, a[4])); return mp_const_none; }
static mp_obj_t g_rect(px_image* d, const mp_obj_t* a) { px_rect_u8(&d->cv, F(a[0]), F(a[1]), F(a[2]), F(a[3]), PC(d, a[4])); return mp_const_none; }
static mp_obj_t g_rectb(px_image* d, const mp_obj_t* a) { px_rectb_u8(&d->cv, F(a[0]), F(a[1]), F(a[2]), F(a[3]), PC(d, a[4])); return mp_const_none; }
static mp_obj_t g_circ(px_image* d, const mp_obj_t* a) { px_circ_u8(&d->cv, F(a[0]), F(a[1]), F(a[2]), PC(d, a[3])); return mp_const_none; }
static mp_obj_t g_circb(px_image* d, const mp_obj_t* a) { px_circb_u8(&d->cv, F(a[0]), F(a[1]), F(a[2]), PC(d, a[3])); return mp_const_none; }
static mp_obj_t g_elli(px_image* d, const mp_obj_t* a) { px_elli_u8(&d->cv, F(a[0]), F(a[1]), F(a[2]), F(a[3]), PC(d, a[4])); return mp_const_none; }
static mp_obj_t g_ellib(px_image* d, const mp_obj_t* a) { px_ellib_u8(&d->cv, F(a[0]), F(a[1]), F(a[2]), F(a[3]), PC(d, a[4])); return mp_const_none; }
static mp_obj_t g_tri(px_image* d, const mp_obj_t* a) { px_tri_u8(&d->cv, F(a[0]), F(a[1]), F(a[2]), F(a[3]), F(a[4]), F(a[5]), PC(d, a[6])); return mp_const_none; }
static mp_obj_t g_trib(px_image* d, const mp_obj_t* a) { px_trib_u8(&d->cv, F(a[0]), F(a[1]), F(a[2]), F(a[3]), F(a[4]), F(a[5]), PC(d, a[6])); return mp_const_none; }
static mp_obj_t g_fill(px_image* d, const mp_obj_t* a) { px_fill_u8(&d->cv, F(a[0]), F(a[1]), PC(d, a[2])); return mp_const_none; }
static mp_obj_t g_dither(px_image* d, const mp_obj_t* a) { d->cv.alpha = F(a[0]); return mp_const_none; }

#define BIND_DRAW(name, nargs) \
    static mp_obj_t s_##name(size_t n, const mp_obj_t* a) { (void)n; need_init(); return g_##name(px.screen, a); } \
    static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(s_##name##_obj, nargs, nargs, s_##name); \
    static mp_obj_t i_##name(size_t n, const mp_obj_t* a) { (void)n; return g_##name(resolve_image(a[0]), a + 1); } \
    static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(i_##name##_obj, nargs + 1, nargs + 1, i_##name);

BIND_DRAW(cls, 1)
BIND_DRAW(pget, 2)
BIND_DRAW(pset, 3)
BIND_DRAW(line, 5)
BIND_DRAW(rect, 5)
BIND_DRAW(rectb, 5)
BIND_DRAW(circ, 4)
BIND_DRAW(circb, 4)
BIND_DRAW(elli, 5)
BIND_DRAW(ellib, 5)
BIND_DRAW(tri, 7)
BIND_DRAW(trib, 7)
BIND_DRAW(fill, 3)
BIND_DRAW(dither, 1)

// clip(), clip(x, y, w, h) / camera(), camera(x, y) / pal(), pal(c1, c2)
static mp_obj_t g_clip(px_image* d, size_t n, const mp_obj_t* a) {
    if (n == 0) px_canvas_reset_clip(&d->cv);
    else if (n == 4) px_canvas_set_clip(&d->cv, F(a[0]), F(a[1]), F(a[2]), F(a[3]));
    else mp_raise_TypeError(MP_ERROR_TEXT("clip() takes 0 or 4 arguments"));
    return mp_const_none;
}
static mp_obj_t g_camera(px_image* d, size_t n, const mp_obj_t* a) {
    if (n == 0) { d->cv.cam_x = d->cv.cam_y = 0; }
    else if (n == 2) { d->cv.cam_x = px_f2i(F(a[0])); d->cv.cam_y = px_f2i(F(a[1])); }
    else mp_raise_TypeError(MP_ERROR_TEXT("camera() takes 0 or 2 arguments"));
    return mp_const_none;
}
static mp_obj_t g_pal(px_image* d, size_t n, const mp_obj_t* a) {
    if (n == 0) px_image_reset_pal(d);
    else if (n == 2) { d->pal[I(a[0]) & 0xFF] = (uint8_t)I(a[1]); d->pal_identity = false; }
    else mp_raise_TypeError(MP_ERROR_TEXT("pal() takes 0 or 2 arguments"));
    return mp_const_none;
}

#define BIND_VAR(name, maxargs) \
    static mp_obj_t s_##name(size_t n, const mp_obj_t* a) { need_init(); return g_##name(px.screen, n, a); } \
    static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(s_##name##_obj, 0, maxargs, s_##name); \
    static mp_obj_t i_##name(size_t n, const mp_obj_t* a) { return g_##name(resolve_image(a[0]), n - 1, a + 1); } \
    static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(i_##name##_obj, 1, maxargs + 1, i_##name);

BIND_VAR(clip, 4)
BIND_VAR(camera, 2)
BIND_VAR(pal, 2)

// blt(x, y, img, u, v, w, h, colkey=None, rotate=None, scale=None)
static mp_obj_t g_blt(px_image* d, size_t n, const mp_obj_t* a, mp_map_t* kw) {
    if (n < 7) mp_raise_TypeError(MP_ERROR_TEXT("blt() needs x, y, img, u, v, w, h"));
    px_image* src = resolve_image(a[2]);
    px_image_blt(d, F(a[0]), F(a[1]), src, F(a[3]), F(a[4]), F(a[5]), F(a[6]),
                 colkey_of(argkw(n, a, kw, 7, Q_colkey, mp_const_none)),
                 optf(argkw(n, a, kw, 8, Q_rotate, mp_const_none), 0.0f),
                 optf(argkw(n, a, kw, 9, Q_scale, mp_const_none), 1.0f));
    return mp_const_none;
}

// bltm(x, y, tm, u, v, w, h, colkey=None, rotate=None, scale=None)
static mp_obj_t g_bltm(px_image* d, size_t n, const mp_obj_t* a, mp_map_t* kw) {
    if (n < 7) mp_raise_TypeError(MP_ERROR_TEXT("bltm() needs x, y, tm, u, v, w, h"));
    mp_obj_t tm_obj = MP_OBJ_NULL;
    px_tilemap* tm = resolve_tilemap(a[2], &tm_obj);
    px_image* src = tilemap_source(tm, tm_obj);
    px_image_bltm(d, F(a[0]), F(a[1]), tm, src, F(a[3]), F(a[4]), F(a[5]), F(a[6]),
                  colkey_of(argkw(n, a, kw, 7, Q_colkey, mp_const_none)),
                  optf(argkw(n, a, kw, 8, Q_rotate, mp_const_none), 0.0f),
                  optf(argkw(n, a, kw, 9, Q_scale, mp_const_none), 1.0f));
    return mp_const_none;
}

// text(x, y, s, col, font=None)
static mp_obj_t g_text(px_image* d, size_t n, const mp_obj_t* a, mp_map_t* kw) {
    if (n < 4) mp_raise_TypeError(MP_ERROR_TEXT("text() needs x, y, s, col"));
    mp_obj_t font = argkw(n, a, kw, 4, Q_font, mp_const_none);
    size_t len;
    const char* s = mp_obj_str_get_data(a[2], &len);
    if (font != mp_const_none) {
        int h = I(mp_load_attr(font, Q[Q__font]));
        if (!px_font_valid(h)) mp_raise_ValueError(MP_ERROR_TEXT("invalid font"));
        px_font_draw(h, d, F(a[0]), F(a[1]), s, len, I(a[3]));
    } else {
        px_image_text(d, F(a[0]), F(a[1]), s, len, I(a[3]));
    }
    return mp_const_none;
}

#define BIND_KW(name, minargs) \
    static mp_obj_t s_##name(size_t n, const mp_obj_t* a, mp_map_t* kw) { need_init(); return g_##name(px.screen, n, a, kw); } \
    static MP_DEFINE_CONST_FUN_OBJ_KW(s_##name##_obj, minargs, s_##name); \
    static mp_obj_t i_##name(size_t n, const mp_obj_t* a, mp_map_t* kw) { return g_##name(resolve_image(a[0]), n - 1, a + 1, kw); } \
    static MP_DEFINE_CONST_FUN_OBJ_KW(i_##name##_obj, minargs + 1, i_##name);

BIND_KW(blt, 7)
BIND_KW(bltm, 7)
BIND_KW(text, 4)

static void vec3(mp_obj_t o, float out[3]) {
    size_t n;
    mp_obj_t* items;
    mp_obj_get_array(o, &n, &items);
    if (n != 3) mp_raise_ValueError(MP_ERROR_TEXT("expected (x, y, z)"));
    for (int i = 0; i < 3; i++) out[i] = F(items[i]);
}

// blt3d(x, y, w, h, img, pos, rot, fov=60, colkey=None)
static mp_obj_t g_blt3d(px_image* d, size_t n, const mp_obj_t* a, mp_map_t* kw) {
    if (n < 7) mp_raise_TypeError(MP_ERROR_TEXT("blt3d() needs x, y, w, h, img, pos, rot"));
    float pos[3], rot[3];
    vec3(a[5], pos);
    vec3(a[6], rot);
    px_image* src = resolve_image(a[4]);
    px_image_blt3d(d, F(a[0]), F(a[1]), F(a[2]), F(a[3]), src, pos, rot,
                   optf(argkw(n, a, kw, 7, Q_fov, mp_const_none), 60.0f),
                   colkey_of(argkw(n, a, kw, 8, Q_colkey, mp_const_none)));
    return mp_const_none;
}

// bltm3d(x, y, w, h, tm, pos, rot, fov=60, colkey=None)
static mp_obj_t g_bltm3d(px_image* d, size_t n, const mp_obj_t* a, mp_map_t* kw) {
    if (n < 7) mp_raise_TypeError(MP_ERROR_TEXT("bltm3d() needs x, y, w, h, tm, pos, rot"));
    float pos[3], rot[3];
    vec3(a[5], pos);
    vec3(a[6], rot);
    mp_obj_t tm_obj = MP_OBJ_NULL;
    px_tilemap* tm = resolve_tilemap(a[4], &tm_obj);
    px_image* src = tilemap_source(tm, tm_obj);
    px_image_bltm3d(d, F(a[0]), F(a[1]), F(a[2]), F(a[3]), tm, src, pos, rot,
                    optf(argkw(n, a, kw, 7, Q_fov, mp_const_none), 60.0f),
                    colkey_of(argkw(n, a, kw, 8, Q_colkey, mp_const_none)));
    return mp_const_none;
}

BIND_KW(blt3d, 7)
BIND_KW(bltm3d, 7)

// ---------------------------------------------------------------------------
// Image objects
// ---------------------------------------------------------------------------
static mp_obj_t f_image_alloc(mp_obj_t wo, mp_obj_t ho) {
    int w = I(wo), h = I(ho);
    if (w <= 0 || h <= 0) mp_raise_ValueError(MP_ERROR_TEXT("image size must be positive"));
    void *st, *buf;
    mp_obj_t pair = alloc_pair(sizeof(px_image), (size_t)w * (size_t)h, &st, &buf);
    px_image_init((px_image*)st, w, h, (uint8_t*)buf);
    return pair;
}
static MP_DEFINE_CONST_FUN_OBJ_2(f_image_alloc_obj, f_image_alloc);

static mp_obj_t f_i_size(mp_obj_t img) {
    px_image* d = resolve_image(img);
    mp_obj_t items[2] = { MP_OBJ_NEW_SMALL_INT(d->cv.w), MP_OBJ_NEW_SMALL_INT(d->cv.h) };
    return mp_obj_new_tuple(2, items);
}
static MP_DEFINE_CONST_FUN_OBJ_1(f_i_size_obj, f_i_size);

static int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    c |= 0x20;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

// Rows of a set() call with ASCII whitespace removed (utils::simplify_string).
static size_t simplified(mp_obj_t row, char* out, size_t cap) {
    size_t len;
    const char* s = mp_obj_str_get_data(row, &len);
    size_t n = 0;
    for (size_t i = 0; i < len && n < cap; i++)
        if (s[i] != ' ' && s[i] != '\t' && s[i] != '\n' && s[i] != '\r') out[n++] = s[i];
    return n;
}

// Image.set(x, y, data): data is rows of hex digits, drawn like a blit.
static mp_obj_t f_i_set(size_t n, const mp_obj_t* a) {
    (void)n;
    px_image* d = resolve_image(a[0]);
    size_t rows;
    mp_obj_t* items;
    mp_obj_get_array(a[3], &rows, &items);
    if (rows == 0) mp_raise_ValueError(MP_ERROR_TEXT("Invalid image data: no rows"));
    char line[1024];
    size_t w = simplified(items[0], line, sizeof(line));
    if (w == 0) mp_raise_ValueError(MP_ERROR_TEXT("Invalid image data at row 0: no pixels"));
    px_image* tmp = px_image_new((int)w, (int)rows);
    if (!tmp) mp_raise_msg(&mp_type_MemoryError, MP_ERROR_TEXT("image data"));
    uint8_t* p = (uint8_t*)tmp->cv.data;
    for (size_t y = 0; y < rows; y++) {
        size_t rw = simplified(items[y], line, sizeof(line));
        for (size_t x = 0; x < rw && x < w; x++) {
            int v = hex_digit(line[x]);
            if (rw != w || v < 0) {
                px_image_free(tmp);
                mp_raise_ValueError(MP_ERROR_TEXT("Invalid image data: rows must be equal-length hex strings"));
            }
            p[y * w + x] = (uint8_t)v;
        }
    }
    px_image_blt(d, (float)I(a[1]), (float)I(a[2]), tmp, 0.0f, 0.0f, (float)w, (float)rows, -1, 0.0f, 1.0f);
    px_image_free(tmp);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(f_i_set_obj, 4, 4, f_i_set);

// Image.load(x, y, filename, include_colors=False): PNG only.
static mp_obj_t f_i_load(size_t n, const mp_obj_t* a) {
    px_image* d = resolve_image(a[0]);
    bool include = n > 4 && mp_obj_is_true(a[4]);
    char err[160];
    colors_from_python();
    px_image* img = px_image_decode_png(mp_obj_str_get_str(a[3]), include, err, sizeof(err));
    if (!img) raise_msg(err);
    px_image_blt(d, (float)I(a[1]), (float)I(a[2]), img, 0.0f, 0.0f,
                 (float)img->cv.w, (float)img->cv.h, -1, 0.0f, 1.0f);
    px_image_free(img);
    if (px.colors_dirty) colors_to_python();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(f_i_load_obj, 4, 5, f_i_load);

// Image.from_image(filename, include_colors) -> (st, buf) for a new Image.
static mp_obj_t f_image_from_file(mp_obj_t fn, mp_obj_t inc) {
    char err[160];
    colors_from_python();
    px_image* img = px_image_decode_png(mp_obj_str_get_str(fn), mp_obj_is_true(inc), err, sizeof(err));
    if (!img) raise_msg(err);
    void *st, *buf;
    mp_obj_t pair = alloc_pair(sizeof(px_image), (size_t)img->cv.w * (size_t)img->cv.h, &st, &buf);
    px_image_init((px_image*)st, img->cv.w, img->cv.h, (uint8_t*)buf);
    memcpy(buf, img->cv.data, (size_t)img->cv.w * (size_t)img->cv.h);
    px_image_free(img);
    if (px.colors_dirty) colors_to_python();
    return pair;
}
static MP_DEFINE_CONST_FUN_OBJ_2(f_image_from_file_obj, f_image_from_file);

// ---------------------------------------------------------------------------
// Tilemap objects
// ---------------------------------------------------------------------------
static mp_obj_t f_tilemap_alloc(mp_obj_t wo, mp_obj_t ho, mp_obj_t src) {
    int w = I(wo), h = I(ho);
    if (w <= 0 || h <= 0) mp_raise_ValueError(MP_ERROR_TEXT("tilemap size must be positive"));
    void *st, *buf;
    mp_obj_t pair = alloc_pair(sizeof(px_tilemap), (size_t)w * (size_t)h * sizeof(uint16_t), &st, &buf);
    px_tilemap_init((px_tilemap*)st, w, h, (uint16_t*)buf, I(src));
    return pair;
}
static MP_DEFINE_CONST_FUN_OBJ_3(f_tilemap_alloc_obj, f_tilemap_alloc);

// Tilemap.from_tmx(filename, layer) -> (st, buf) for a new Tilemap on bank 0.
static mp_obj_t f_tilemap_from_tmx(mp_obj_t fn, mp_obj_t layer) {
    char err[160];
    int w = 0, h = 0;
    uint16_t* tiles = px_tmx_load(mp_obj_str_get_str(fn), I(layer), &w, &h, err, sizeof(err));
    if (!tiles) raise_msg(err);
    void *st, *buf;
    nlr_buf_t nlr;
    mp_obj_t pair = MP_OBJ_NULL;
    if (nlr_push(&nlr) == 0) {
        pair = alloc_pair(sizeof(px_tilemap), (size_t)w * (size_t)h * sizeof(uint16_t), &st, &buf);
        nlr_pop();
    } else {
        free(tiles);
        nlr_jump(nlr.ret_val);
    }
    memcpy(buf, tiles, (size_t)w * (size_t)h * sizeof(uint16_t));
    free(tiles);
    px_tilemap_init((px_tilemap*)st, w, h, (uint16_t*)buf, 0);
    return pair;
}
static MP_DEFINE_CONST_FUN_OBJ_2(f_tilemap_from_tmx_obj, f_tilemap_from_tmx);

// Tilemap.load(x, y, filename, layer): the TMX layer drawn into this tilemap at (x, y).
static mp_obj_t t_load(size_t n, const mp_obj_t* a) {
    (void)n;
    px_tilemap* tm = resolve_tilemap(a[0], NULL);
    char err[160];
    int w = 0, h = 0;
    uint16_t* tiles = px_tmx_load(mp_obj_str_get_str(a[3]), I(a[4]), &w, &h, err, sizeof(err));
    if (!tiles) raise_msg(err);
    px_canvas src;
    px_canvas_init(&src, w, h, tiles);
    px_blit_u16(&tm->cv, (float)I(a[1]), (float)I(a[2]), &src, 0.0f, 0.0f, (float)w, (float)h, -1, NULL);
    free(tiles);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(t_load_obj, 5, 5, t_load);

// Tilemap.collide(x, y, w, h, dx, dy, walls) -> (dx, dy)
static mp_obj_t t_collide(size_t n, const mp_obj_t* a) {
    (void)n;
    px_tilemap* tm = resolve_tilemap(a[0], NULL);
    size_t nw;
    mp_obj_t* items;
    mp_obj_get_array(a[7], &nw, &items);
    uint16_t walls[64];
    if (nw > 64) mp_raise_ValueError(MP_ERROR_TEXT("at most 64 wall tiles"));
    for (size_t i = 0; i < nw; i++) walls[i] = tile_of(items[i]);
    float dx = F(a[5]), dy = F(a[6]);
    px_tilemap_collide(tm, F(a[1]), F(a[2]), F(a[3]), F(a[4]), &dx, &dy, walls, (int)nw);
    mp_obj_t out[2] = { mp_obj_new_float(dx), mp_obj_new_float(dy) };
    return mp_obj_new_tuple(2, out);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(t_collide_obj, 8, 8, t_collide);

static mp_obj_t f_t_size(mp_obj_t o) {
    px_tilemap* tm = resolve_tilemap(o, NULL);
    mp_obj_t items[2] = { MP_OBJ_NEW_SMALL_INT(tm->cv.w), MP_OBJ_NEW_SMALL_INT(tm->cv.h) };
    return mp_obj_new_tuple(2, items);
}
static MP_DEFINE_CONST_FUN_OBJ_1(f_t_size_obj, f_t_size);

// imgsrc as seen from C: a bank index, or -1 meaning "use _imgsrc_obj".
static mp_obj_t f_t_imgsrc(mp_obj_t o, mp_obj_t v) {
    px_tilemap* tm = resolve_tilemap(o, NULL);
    if (v == mp_const_none) return MP_OBJ_NEW_SMALL_INT(tm->imgsrc);
    tm->imgsrc = I(v);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(f_t_imgsrc_obj, f_t_imgsrc);

#define TM(a0) resolve_tilemap(a0, NULL)
static mp_obj_t t_cls(size_t n, const mp_obj_t* a) { (void)n; px_clear_u16(&TM(a[0])->cv, tile_of(a[1])); return mp_const_none; }
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(t_cls_obj, 2, 2, t_cls);
static mp_obj_t t_pget(size_t n, const mp_obj_t* a) { (void)n; return tile_tuple(px_value_u16(&TM(a[0])->cv, F(a[1]), F(a[2]))); }
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(t_pget_obj, 3, 3, t_pget);
static mp_obj_t t_pset(size_t n, const mp_obj_t* a) { (void)n; px_set_value_u16(&TM(a[0])->cv, F(a[1]), F(a[2]), tile_of(a[3])); return mp_const_none; }
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(t_pset_obj, 4, 4, t_pset);
static mp_obj_t t_line(size_t n, const mp_obj_t* a) { (void)n; px_line_u16(&TM(a[0])->cv, F(a[1]), F(a[2]), F(a[3]), F(a[4]), tile_of(a[5])); return mp_const_none; }
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(t_line_obj, 6, 6, t_line);
static mp_obj_t t_rect(size_t n, const mp_obj_t* a) { (void)n; px_rect_u16(&TM(a[0])->cv, F(a[1]), F(a[2]), F(a[3]), F(a[4]), tile_of(a[5])); return mp_const_none; }
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(t_rect_obj, 6, 6, t_rect);
static mp_obj_t t_rectb(size_t n, const mp_obj_t* a) { (void)n; px_rectb_u16(&TM(a[0])->cv, F(a[1]), F(a[2]), F(a[3]), F(a[4]), tile_of(a[5])); return mp_const_none; }
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(t_rectb_obj, 6, 6, t_rectb);
static mp_obj_t t_circ(size_t n, const mp_obj_t* a) { (void)n; px_circ_u16(&TM(a[0])->cv, F(a[1]), F(a[2]), F(a[3]), tile_of(a[4])); return mp_const_none; }
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(t_circ_obj, 5, 5, t_circ);
static mp_obj_t t_circb(size_t n, const mp_obj_t* a) { (void)n; px_circb_u16(&TM(a[0])->cv, F(a[1]), F(a[2]), F(a[3]), tile_of(a[4])); return mp_const_none; }
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(t_circb_obj, 5, 5, t_circb);
static mp_obj_t t_elli(size_t n, const mp_obj_t* a) { (void)n; px_elli_u16(&TM(a[0])->cv, F(a[1]), F(a[2]), F(a[3]), F(a[4]), tile_of(a[5])); return mp_const_none; }
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(t_elli_obj, 6, 6, t_elli);
static mp_obj_t t_ellib(size_t n, const mp_obj_t* a) { (void)n; px_ellib_u16(&TM(a[0])->cv, F(a[1]), F(a[2]), F(a[3]), F(a[4]), tile_of(a[5])); return mp_const_none; }
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(t_ellib_obj, 6, 6, t_ellib);
static mp_obj_t t_tri(size_t n, const mp_obj_t* a) { (void)n; px_tri_u16(&TM(a[0])->cv, F(a[1]), F(a[2]), F(a[3]), F(a[4]), F(a[5]), F(a[6]), tile_of(a[7])); return mp_const_none; }
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(t_tri_obj, 8, 8, t_tri);
static mp_obj_t t_trib(size_t n, const mp_obj_t* a) { (void)n; px_trib_u16(&TM(a[0])->cv, F(a[1]), F(a[2]), F(a[3]), F(a[4]), F(a[5]), F(a[6]), tile_of(a[7])); return mp_const_none; }
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(t_trib_obj, 8, 8, t_trib);
static mp_obj_t t_fill(size_t n, const mp_obj_t* a) { (void)n; px_fill_u16(&TM(a[0])->cv, F(a[1]), F(a[2]), tile_of(a[3])); return mp_const_none; }
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(t_fill_obj, 4, 4, t_fill);

static mp_obj_t t_clip(size_t n, const mp_obj_t* a) {
    px_tilemap* tm = TM(a[0]);
    if (n == 1) px_canvas_reset_clip(&tm->cv);
    else if (n == 5) px_canvas_set_clip(&tm->cv, F(a[1]), F(a[2]), F(a[3]), F(a[4]));
    else mp_raise_TypeError(MP_ERROR_TEXT("clip() takes 0 or 4 arguments"));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(t_clip_obj, 1, 5, t_clip);

static mp_obj_t t_camera(size_t n, const mp_obj_t* a) {
    px_tilemap* tm = TM(a[0]);
    if (n == 1) { tm->cv.cam_x = tm->cv.cam_y = 0; }
    else if (n == 3) { tm->cv.cam_x = px_f2i(F(a[1])); tm->cv.cam_y = px_f2i(F(a[2])); }
    else mp_raise_TypeError(MP_ERROR_TEXT("camera() takes 0 or 2 arguments"));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(t_camera_obj, 1, 3, t_camera);

// Tilemap.blt(x, y, tm, u, v, w, h, tilekey=None, rotate=None, scale=None)
static mp_obj_t t_blt(size_t n, const mp_obj_t* a, mp_map_t* kw) {
    if (n < 8) mp_raise_TypeError(MP_ERROR_TEXT("blt() needs x, y, tm, u, v, w, h"));
    px_tilemap* dst = TM(a[0]);
    px_tilemap* src = TM(a[3]);
    const mp_obj_t* r = a + 1;
    size_t rn = n - 1;
    mp_obj_t key = argkw(rn, r, kw, 7, Q_tilekey, mp_const_none);
    int tk = key == mp_const_none ? -1 : tile_of(key);
    float rot = optf(argkw(rn, r, kw, 8, Q_rotate, mp_const_none), 0.0f);
    float sc = optf(argkw(rn, r, kw, 9, Q_scale, mp_const_none), 1.0f);
    // A tilemap copy onto itself reads its own tiles: copy the source first.
    px_canvas copy = src->cv;
    uint16_t* dup = NULL;
    if (src == dst) {
        size_t bytes = (size_t)src->cv.w * (size_t)src->cv.h * sizeof(uint16_t);
        dup = (uint16_t*)malloc(bytes);
        if (!dup) mp_raise_msg(&mp_type_MemoryError, MP_ERROR_TEXT("tilemap copy"));
        memcpy(dup, src->cv.data, bytes);
        copy.data = dup;
    }
    if (rot != 0.0f || sc != 1.0f)
        px_blit_xform_u16(&dst->cv, F(r[0]), F(r[1]), &copy, F(r[3]), F(r[4]), F(r[5]), F(r[6]), tk, NULL, rot, sc);
    else
        px_blit_u16(&dst->cv, F(r[0]), F(r[1]), &copy, F(r[3]), F(r[4]), F(r[5]), F(r[6]), tk, NULL);
    free(dup);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(t_blt_obj, 8, t_blt);

// Tilemap.set(x, y, data): rows of 4-hex-digit tiles (tx high byte, ty low).
static mp_obj_t t_set(size_t n, const mp_obj_t* a) {
    (void)n;
    px_tilemap* tm = TM(a[0]);
    size_t rows;
    mp_obj_t* items;
    mp_obj_get_array(a[3], &rows, &items);
    if (rows == 0) mp_raise_ValueError(MP_ERROR_TEXT("Invalid tilemap data: no rows"));
    char line[2048];
    size_t digits = simplified(items[0], line, sizeof(line));
    if (digits == 0 || digits % 4) mp_raise_ValueError(MP_ERROR_TEXT("Invalid tilemap data: digit count must be a multiple of 4"));
    size_t w = digits / 4;
    uint16_t* tmp = (uint16_t*)calloc(w * rows, sizeof(uint16_t));
    if (!tmp) mp_raise_msg(&mp_type_MemoryError, MP_ERROR_TEXT("tilemap data"));
    for (size_t y = 0; y < rows; y++) {
        size_t d = simplified(items[y], line, sizeof(line));
        if (d != digits) { free(tmp); mp_raise_ValueError(MP_ERROR_TEXT("Invalid tilemap data: rows differ in length")); }
        for (size_t x = 0; x < w; x++) {
            uint32_t t = 0;
            for (int k = 0; k < 4; k++) {
                int v = hex_digit(line[x * 4 + k]);
                if (v < 0) { free(tmp); mp_raise_ValueError(MP_ERROR_TEXT("Invalid tilemap data: not a hex digit")); }
                t = (t << 4) | (uint32_t)v;
            }
            tmp[y * w + x] = PX_TILE((t >> 8) & 0xFF, t & 0xFF);
        }
    }
    px_canvas src;
    px_canvas_init(&src, (int)w, (int)rows, tmp);
    px_blit_u16(&tm->cv, (float)I(a[1]), (float)I(a[2]), &src, 0.0f, 0.0f, (float)w, (float)rows, -1, NULL);
    free(tmp);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(t_set_obj, 4, 4, t_set);

// ---------------------------------------------------------------------------
// Math (math.rs). The generators are not upstream's Xoshiro/Perlin crates,
// so seeded sequences differ from desktop Pyxel.
// ---------------------------------------------------------------------------
static uint32_t s_rng = 2463534242u;
static uint32_t rng_next(void) {
    uint32_t x = s_rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    s_rng = x;
    return x;
}

static mp_obj_t f_ceil(mp_obj_t x) { return mp_obj_new_int((mp_int_t)ceilf(F(x))); }
static MP_DEFINE_CONST_FUN_OBJ_1(f_ceil_obj, f_ceil);
static mp_obj_t f_floor(mp_obj_t x) { return mp_obj_new_int((mp_int_t)floorf(F(x))); }
static MP_DEFINE_CONST_FUN_OBJ_1(f_floor_obj, f_floor);
static mp_obj_t f_sqrt(mp_obj_t x) { return mp_obj_new_float(sqrtf(F(x))); }
static MP_DEFINE_CONST_FUN_OBJ_1(f_sqrt_obj, f_sqrt);
#define DEG2RAD 0.017453292519943295f
static mp_obj_t f_sin(mp_obj_t d) { return mp_obj_new_float(sinf(F(d) * DEG2RAD)); }
static MP_DEFINE_CONST_FUN_OBJ_1(f_sin_obj, f_sin);
static mp_obj_t f_cos(mp_obj_t d) { return mp_obj_new_float(cosf(F(d) * DEG2RAD)); }
static MP_DEFINE_CONST_FUN_OBJ_1(f_cos_obj, f_cos);
static mp_obj_t f_atan2(mp_obj_t y, mp_obj_t x) { return mp_obj_new_float(atan2f(F(y), F(x)) / DEG2RAD); }
static MP_DEFINE_CONST_FUN_OBJ_2(f_atan2_obj, f_atan2);

// An int, a bool, or an instance of a class derived from int (IntEnum).
static bool int_like(mp_obj_t o) {
    return mp_obj_is_int(o) || mp_obj_is_bool(o)
           || mp_obj_cast_to_native_base(o, MP_OBJ_FROM_PTR(&mp_type_int)) != MP_OBJ_NULL;
}

static mp_obj_t f_sgn(mp_obj_t x) {
    if (int_like(x)) {
        mp_int_t v = mp_obj_get_int(x);
        return MP_OBJ_NEW_SMALL_INT(v > 0 ? 1 : v < 0 ? -1 : 0);
    }
    float v = F(x);
    return mp_obj_new_float(v > 0.0f ? 1.0f : v < 0.0f ? -1.0f : 0.0f);
}
static MP_DEFINE_CONST_FUN_OBJ_1(f_sgn_obj, f_sgn);

static mp_obj_t f_clamp(mp_obj_t x, mp_obj_t lo, mp_obj_t hi) {
    if (int_like(x) && int_like(lo) && int_like(hi)) {
        mp_int_t v = mp_obj_get_int(x), l = mp_obj_get_int(lo), h = mp_obj_get_int(hi);
        if (l > h) { mp_int_t t = l; l = h; h = t; }
        return mp_obj_new_int(v < l ? l : v > h ? h : v);
    }
    float v = F(x), l = F(lo), h = F(hi);
    if (l > h) { float t = l; l = h; h = t; }
    return mp_obj_new_float(v < l ? l : v > h ? h : v);
}
static MP_DEFINE_CONST_FUN_OBJ_3(f_clamp_obj, f_clamp);

static mp_obj_t f_rseed(mp_obj_t s) {
    s_rng = (uint32_t)mp_obj_get_int(s) * 2654435761u + 0x9E3779B9u;
    if (!s_rng) s_rng = 2463534242u;
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(f_rseed_obj, f_rseed);

static mp_obj_t f_rndi(mp_obj_t ao, mp_obj_t bo) {
    int32_t a = (int32_t)mp_obj_get_int(ao), b = (int32_t)mp_obj_get_int(bo);
    if (a > b) { int32_t t = a; a = b; b = t; }
    uint32_t span = (uint32_t)(b - a) + 1u;
    if (span == 0) return mp_obj_new_int((int32_t)rng_next());
    return mp_obj_new_int(a + (int32_t)(rng_next() % span));
}
static MP_DEFINE_CONST_FUN_OBJ_2(f_rndi_obj, f_rndi);

static mp_obj_t f_rndf(mp_obj_t ao, mp_obj_t bo) {
    float a = F(ao), b = F(bo);
    if (a > b) { float t = a; a = b; b = t; }
    float r = (float)(rng_next() >> 8) / 16777215.0f;
    return mp_obj_new_float(a + (b - a) * r);
}
static MP_DEFINE_CONST_FUN_OBJ_2(f_rndf_obj, f_rndf);

// Improved Perlin noise (Ken Perlin, 2002) over a seed-shuffled permutation.
static uint8_t s_perm[512];
static bool s_perm_ready;

static void perlin_seed(uint32_t seed) {
    uint32_t saved = s_rng;
    s_rng = seed * 2654435761u + 0x9E3779B9u;
    if (!s_rng) s_rng = 1;
    for (int i = 0; i < 256; i++) s_perm[i] = (uint8_t)i;
    for (int i = 255; i > 0; i--) {
        int j = (int)(rng_next() % (uint32_t)(i + 1));
        uint8_t t = s_perm[i]; s_perm[i] = s_perm[j]; s_perm[j] = t;
    }
    for (int i = 0; i < 256; i++) s_perm[256 + i] = s_perm[i];
    s_rng = saved;
    s_perm_ready = true;
}

static float fade(float t) { return t * t * t * (t * (t * 6 - 15) + 10); }
static float lerpf(float t, float a, float b) { return a + t * (b - a); }
static float grad(int h, float x, float y, float z) {
    h &= 15;
    float u = h < 8 ? x : y;
    float v = h < 4 ? y : (h == 12 || h == 14) ? x : z;
    return ((h & 1) ? -u : u) + ((h & 2) ? -v : v);
}

static float perlin(float x, float y, float z) {
    if (!s_perm_ready) perlin_seed(host_get_ticks_us());
    int X = (int)floorf(x) & 255, Y = (int)floorf(y) & 255, Z = (int)floorf(z) & 255;
    x -= floorf(x); y -= floorf(y); z -= floorf(z);
    float u = fade(x), v = fade(y), w = fade(z);
    const uint8_t* p = s_perm;
    int A = p[X] + Y, AA = p[A] + Z, AB = p[A + 1] + Z;
    int B = p[X + 1] + Y, BA = p[B] + Z, BB = p[B + 1] + Z;
    return lerpf(w, lerpf(v, lerpf(u, grad(p[AA], x, y, z), grad(p[BA], x - 1, y, z)),
                             lerpf(u, grad(p[AB], x, y - 1, z), grad(p[BB], x - 1, y - 1, z))),
                    lerpf(v, lerpf(u, grad(p[AA + 1], x, y, z - 1), grad(p[BA + 1], x - 1, y, z - 1)),
                             lerpf(u, grad(p[AB + 1], x, y - 1, z - 1), grad(p[BB + 1], x - 1, y - 1, z - 1))));
}

static mp_obj_t f_nseed(mp_obj_t s) { perlin_seed((uint32_t)mp_obj_get_int(s)); return mp_const_none; }
static MP_DEFINE_CONST_FUN_OBJ_1(f_nseed_obj, f_nseed);

static mp_obj_t f_noise(size_t n, const mp_obj_t* a) {
    return mp_obj_new_float(perlin(F(a[0]), n > 1 ? F(a[1]) : 0.0f, n > 2 ? F(a[2]) : 0.0f));
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(f_noise_obj, 1, 3, f_noise);

// ---------------------------------------------------------------------------
// Audio: Sound/Tone objects (pyxel.py) are compiled into C arrays at play time.
// ---------------------------------------------------------------------------
static mp_obj_t attr(mp_obj_t o, int q) {
    return mp_load_attr(o, Q[q]);
}

static void compile_tones(void) {
    mp_obj_t lst = dict_get(Q_tones);
    if (lst == MP_OBJ_NULL) return;
    size_t n;
    mp_obj_t* items;
    mp_obj_get_array(lst, &n, &items);
    static uint32_t table[PX_TONE_MAX_SAMPLES];
    for (size_t i = 0; i < n && i < PX_NUM_TONES; i++) {
        size_t wn;
        mp_obj_t* w;
        mp_obj_get_array(attr(items[i], Q_wavetable), &wn, &w);
        if (wn > PX_TONE_MAX_SAMPLES) wn = PX_TONE_MAX_SAMPLES;
        for (size_t k = 0; k < wn; k++) table[k] = (uint32_t)mp_obj_get_int(w[k]);
        px_audio_set_tone((int)i, I(attr(items[i], Q_mode)), table, (int)wn,
                          I(attr(items[i], Q_sample_bits)), F(attr(items[i], Q_gain)));
    }
}

static void tone_modes(int* modes) {
    for (int i = 0; i < PX_NUM_TONES; i++) modes[i] = px_audio_tone_mode(i);
}

// A Python list of ints as a malloc'd C array (freed by the caller).
static int* int_array(mp_obj_t lst, int* n_out) {
    size_t n;
    mp_obj_t* items;
    mp_obj_get_array(lst, &n, &items);
    int* v = (int*)malloc(sizeof(int) * (n ? n : 1));
    if (!v) mp_raise_msg(&mp_type_MemoryError, MP_ERROR_TEXT("sound"));
    for (size_t i = 0; i < n; i++) v[i] = (int)mp_obj_get_int(items[i]);
    *n_out = (int)n;
    return v;
}

static void compile_mml(const char* code, bool old, px_snd* out) {
    int modes[PX_NUM_TONES];
    tone_modes(modes);
    char err[160];
    err[0] = 0;
    if (!px_snd_from_mml(out, code, old, modes, PX_NUM_TONES, err, sizeof(err))) raise_msg(err);
}

// A Sound object (pyxel.py) as a command list: its PCM, its MML, or its notes.
static void compile_sound(mp_obj_t s, px_snd* out) {
    mp_obj_t pcm = attr(s, Q__pcm);
    if (pcm != mp_const_none) {
        px_snd_from_pcm(out, I(pcm));
        return;
    }
    mp_obj_t mml = attr(s, Q__mml);
    if (mml != mp_const_none) {
        compile_mml(mp_obj_str_get_str(mml), mp_obj_is_true(attr(s, Q__mml_old)), out);
        return;
    }
    int speed = I(attr(s, Q_speed));
    if (speed <= 0) mp_raise_ValueError(MP_ERROR_TEXT("speed must be greater than 0"));
    int nn = 0, nt = 0, nv = 0, nf = 0;
    int* notes = int_array(attr(s, Q_notes), &nn);
    int* tones = NULL;
    int* vols = NULL;
    int* fxs = NULL;
    nlr_buf_t nlr;
    if (nlr_push(&nlr) == 0) {
        tones = int_array(attr(s, Q_tones), &nt);
        vols = int_array(attr(s, Q_volumes), &nv);
        fxs = int_array(attr(s, Q_effects), &nf);
        nlr_pop();
    } else {
        free(notes);
        free(tones);
        free(vols);
        nlr_jump(nlr.ret_val);
    }
    int modes[PX_NUM_TONES];
    tone_modes(modes);
    bool ok = px_snd_from_legacy(out, notes, nn, tones, nt, vols, nv, fxs, nf, speed, modes, PX_NUM_TONES);
    free(notes);
    free(tones);
    free(vols);
    free(fxs);
    if (!ok) {
        px_snd_free(out);
        mp_raise_msg(&mp_type_MemoryError, MP_ERROR_TEXT("sound"));
    }
}

static px_snd* compile_sounds(size_t ns, const mp_obj_t* sounds) {
    compile_tones();
    px_snd* snds = (px_snd*)calloc(ns ? ns : 1, sizeof(px_snd));
    if (!snds) mp_raise_msg(&mp_type_MemoryError, MP_ERROR_TEXT("sound"));
    for (size_t i = 0; i < ns; i++) px_snd_init(&snds[i]);
    nlr_buf_t nlr;
    if (nlr_push(&nlr) == 0) {
        for (size_t i = 0; i < ns; i++) compile_sound(sounds[i], &snds[i]);
        nlr_pop();
    } else {
        for (size_t i = 0; i < ns; i++) px_snd_free(&snds[i]);
        free(snds);
        nlr_jump(nlr.ret_val);
    }
    return snds;
}

// Sound.set_notes/set_tones/set_volumes/set_effects strings (sound.rs): read
// after simplify_string, which drops ASCII whitespace and lowercases ASCII.
typedef struct {
    const char* p;
    const char* end;
    const char* at;     // start of the character last read
} sound_str_t;

// The next character, lowercased, or -1 at the end. A non-ASCII character is
// returned as its UTF-8 lead byte, with s->at..s->p spanning all its bytes.
static int sound_str_next(sound_str_t* s) {
    while (s->p < s->end) {
        unsigned char c = (unsigned char)*s->p;
        if (c == ' ' || c == '\t' || c == '\n' || c == '\f' || c == '\r') {
            s->p++;
            continue;
        }
        s->at = s->p++;
        if (c >= 0x80) {
            while (s->p < s->end && ((unsigned char)*s->p & 0xC0) == 0x80) s->p++;
            return c;
        }
        return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c;
    }
    s->at = s->end;
    return -1;
}

// "Invalid sound <what> '<c>'"; the end of the string shows as ''.
static NORETURN void sound_str_invalid(const char* what, const sound_str_t* s, int c) {
    if (c >= 0 && c < 0x80)
        mp_raise_msg_varg(&mp_type_ValueError, MP_ERROR_TEXT("Invalid sound %s '%c'"), what, c);
    mp_raise_msg_varg(&mp_type_ValueError, MP_ERROR_TEXT("Invalid sound %s '%.*s'"),
                      what, (int)(s->p - s->at), s->at);
}

static sound_str_t sound_str(mp_obj_t str) {
    size_t len;
    const char* data = mp_obj_str_get_data(str, &len);
    sound_str_t s = { data, data + len, data };
    return s;
}

// Notes: a-g with an optional # or -, then octave 0-4; r is a rest (-1).
static mp_obj_t f_parse_notes(mp_obj_t str) {
    static const int8_t BASE[7] = { 9, 11, 0, 2, 4, 5, 7 };    // a b c d e f g
    sound_str_t s = sound_str(str);
    mp_obj_t out = mp_obj_new_list(0, NULL);
    for (int c; (c = sound_str_next(&s)) >= 0;) {
        int note;
        if (c >= 'a' && c <= 'g') {
            note = BASE[c - 'a'];
            c = sound_str_next(&s);
            if (c == '#') {
                note++;
                c = sound_str_next(&s);
            } else if (c == '-') {
                note--;
                c = sound_str_next(&s);
            }
            if (c < '0' || c > '4') sound_str_invalid("note", &s, c);
            note += (c - '0') * 12;
        } else if (c == 'r') {
            note = -1;
        } else {
            sound_str_invalid("note", &s, c);
        }
        mp_obj_list_append(out, MP_OBJ_NEW_SMALL_INT(note));
    }
    return out;
}
static MP_DEFINE_CONST_FUN_OBJ_1(f_parse_notes_obj, f_parse_notes);

// Tones: t s p n (TONE_TRIANGLE..TONE_NOISE) or a digit (a tones index).
static mp_obj_t f_parse_tones(mp_obj_t str) {
    sound_str_t s = sound_str(str);
    mp_obj_t out = mp_obj_new_list(0, NULL);
    for (int c; (c = sound_str_next(&s)) >= 0;) {
        int tone;
        switch (c) {
            case 't': tone = 0; break;
            case 's': tone = 1; break;
            case 'p': tone = 2; break;
            case 'n': tone = 3; break;
            default:
                if (c < '0' || c > '9') sound_str_invalid("tone", &s, c);
                tone = c - '0';
                break;
        }
        mp_obj_list_append(out, MP_OBJ_NEW_SMALL_INT(tone));
    }
    return out;
}
static MP_DEFINE_CONST_FUN_OBJ_1(f_parse_tones_obj, f_parse_tones);

// Volumes: 0-7.
static mp_obj_t f_parse_volumes(mp_obj_t str) {
    sound_str_t s = sound_str(str);
    mp_obj_t out = mp_obj_new_list(0, NULL);
    for (int c; (c = sound_str_next(&s)) >= 0;) {
        if (c < '0' || c > '7') sound_str_invalid("volume", &s, c);
        mp_obj_list_append(out, MP_OBJ_NEW_SMALL_INT(c - '0'));
    }
    return out;
}
static MP_DEFINE_CONST_FUN_OBJ_1(f_parse_volumes_obj, f_parse_volumes);

// Effects: n s v f h q (EFFECT_NONE..EFFECT_QUARTER_FADEOUT).
static mp_obj_t f_parse_effects(mp_obj_t str) {
    static const char CODES[] = "nsvfhq";
    sound_str_t s = sound_str(str);
    mp_obj_t out = mp_obj_new_list(0, NULL);
    for (int c; (c = sound_str_next(&s)) >= 0;) {
        const char* hit = c > 0 && c < 0x80 ? strchr(CODES, c) : NULL;
        if (!hit) sound_str_invalid("effect", &s, c);
        mp_obj_list_append(out, MP_OBJ_NEW_SMALL_INT(hit - CODES));
    }
    return out;
}
static MP_DEFINE_CONST_FUN_OBJ_1(f_parse_effects_obj, f_parse_effects);

// Sound.mml(code) validates at call time, as upstream parses it then.
static mp_obj_t f_mml_check(mp_obj_t code, mp_obj_t old) {
    px_snd s;
    px_snd_init(&s);
    compile_mml(mp_obj_str_get_str(code), mp_obj_is_true(old), &s);
    px_snd_free(&s);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(f_mml_check_obj, f_mml_check);

static mp_obj_t f_pcm_load(mp_obj_t fn) {
    char err[160];
    int h = px_pcm_load(mp_obj_str_get_str(fn), err, sizeof(err));
    if (h < 0) raise_msg(err);
    return MP_OBJ_NEW_SMALL_INT(h);
}
static MP_DEFINE_CONST_FUN_OBJ_1(f_pcm_load_obj, f_pcm_load);

static mp_obj_t f_pcm_release(mp_obj_t h) {
    px_pcm_unref(I(h));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(f_pcm_release_obj, f_pcm_release);

// Sound.total_sec() for MML and PCM sounds (None when it repeats forever).
static mp_obj_t f_sound_total_sec(mp_obj_t s) {
    px_snd* snd = compile_sounds(1, &s);
    uint64_t clocks = 0;
    bool finite = px_snd_total_clocks(snd, &clocks);
    px_snd_free(snd);
    free(snd);
    return finite ? mp_obj_new_float((mp_float_t)((double)clocks / PX_AUDIO_CLOCK_RATE)) : mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(f_sound_total_sec_obj, f_sound_total_sec);

static mp_obj_t f_channel_set(mp_obj_t ch, mp_obj_t gain, mp_obj_t detune) {
    px_audio_set_channel(I(ch), F(gain), I(detune));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_3(f_channel_set_obj, f_channel_set);

// play(ch, [Sound...], sec, loop, resume): pyxel.py resolves every snd form.
static mp_obj_t f_play(size_t n, const mp_obj_t* a) {
    (void)n;
    int ch = I(a[0]);
    if (ch < 0 || ch >= PX_NUM_CHANNELS) mp_raise_ValueError(MP_ERROR_TEXT("ch out of range"));
    float sec = a[2] == mp_const_none ? 0.0f : F(a[2]);
    if (!(sec >= 0.0f)) mp_raise_ValueError(MP_ERROR_TEXT("sec must be greater than or equal to 0"));
    size_t ns;
    mp_obj_t* sounds;
    mp_obj_get_array(a[1], &ns, &sounds);
    if (ns == 0) return mp_const_none;
    px_snd* snds = compile_sounds(ns, sounds);
    px_audio_play(ch, snds, (int)ns, sec, mp_obj_is_true(a[3]), mp_obj_is_true(a[4]));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(f_play_obj, 5, 5, f_play);

static mp_obj_t f_stop(mp_obj_t ch) {
    px_audio_stop(ch == mp_const_none ? -1 : I(ch));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(f_stop_obj, f_stop);

static mp_obj_t f_play_pos(mp_obj_t ch) {
    int idx;
    float sec;
    if (!px_audio_pos(I(ch), &idx, &sec)) return mp_const_none;
    mp_obj_t items[2] = { MP_OBJ_NEW_SMALL_INT(idx), mp_obj_new_float(sec) };
    return mp_obj_new_tuple(2, items);
}
static MP_DEFINE_CONST_FUN_OBJ_1(f_play_pos_obj, f_play_pos);

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------
typedef struct {
    const char* name;
    const void* fn;
} px_fn;

static const px_fn FUNCS[] = {
    { "_bind", &f_bind_obj },
    { "init", &f_init_obj }, { "run", &f_run_obj }, { "flip", &f_flip_obj },
    { "show", &f_show_obj }, { "quit", &f_quit_obj }, { "reset", &f_reset_obj },
    { "resize", &f_resize_obj },
    { "title", &f_window_noop_obj }, { "icon", &f_window_noop_obj },
    { "fullscreen", &f_window_noop_obj }, { "screen_mode", &f_window_noop_obj },
    { "perf_monitor", &f_window_noop_obj }, { "integer_scale", &f_window_noop_obj },
    { "screenshot", &f_window_noop_obj }, { "screencast", &f_window_noop_obj },
    { "reset_screencast", &f_window_noop_obj },
    { "load", &f_load_obj }, { "load_pal", &f_load_pal_obj },
    { "save", &f_save_obj },
    { "file_write", &f_file_write_obj }, { "file_flush", &f_file_flush_obj },
    { "file_close", &f_file_close_obj },
    { "btn", &f_btn_obj }, { "btnp", &f_btnp_obj }, { "btnr", &f_btnr_obj },
    { "btnv", &f_btnv_obj }, { "mouse", &f_mouse_obj },
    { "cls", &s_cls_obj }, { "pget", &s_pget_obj }, { "pset", &s_pset_obj },
    { "line", &s_line_obj }, { "rect", &s_rect_obj }, { "rectb", &s_rectb_obj },
    { "circ", &s_circ_obj }, { "circb", &s_circb_obj }, { "elli", &s_elli_obj },
    { "ellib", &s_ellib_obj }, { "tri", &s_tri_obj }, { "trib", &s_trib_obj },
    { "fill", &s_fill_obj }, { "dither", &s_dither_obj }, { "clip", &s_clip_obj },
    { "camera", &s_camera_obj }, { "pal", &s_pal_obj }, { "blt", &s_blt_obj },
    { "bltm", &s_bltm_obj }, { "text", &s_text_obj },
    { "blt3d", &s_blt3d_obj }, { "bltm3d", &s_bltm3d_obj },
    { "i_blt3d", &i_blt3d_obj }, { "i_bltm3d", &i_bltm3d_obj },
    { "i_cls", &i_cls_obj }, { "i_pget", &i_pget_obj }, { "i_pset", &i_pset_obj },
    { "i_line", &i_line_obj }, { "i_rect", &i_rect_obj }, { "i_rectb", &i_rectb_obj },
    { "i_circ", &i_circ_obj }, { "i_circb", &i_circb_obj }, { "i_elli", &i_elli_obj },
    { "i_ellib", &i_ellib_obj }, { "i_tri", &i_tri_obj }, { "i_trib", &i_trib_obj },
    { "i_fill", &i_fill_obj }, { "i_dither", &i_dither_obj }, { "i_clip", &i_clip_obj },
    { "i_camera", &i_camera_obj }, { "i_pal", &i_pal_obj }, { "i_blt", &i_blt_obj },
    { "i_bltm", &i_bltm_obj }, { "i_text", &i_text_obj },
    { "i_set", &f_i_set_obj }, { "i_load", &f_i_load_obj }, { "i_size", &f_i_size_obj },
    { "image_alloc", &f_image_alloc_obj }, { "image_from_file", &f_image_from_file_obj },
    { "tilemap_alloc", &f_tilemap_alloc_obj }, { "t_size", &f_t_size_obj },
    { "t_imgsrc", &f_t_imgsrc_obj },
    { "t_cls", &t_cls_obj }, { "t_pget", &t_pget_obj }, { "t_pset", &t_pset_obj },
    { "t_line", &t_line_obj }, { "t_rect", &t_rect_obj }, { "t_rectb", &t_rectb_obj },
    { "t_circ", &t_circ_obj }, { "t_circb", &t_circb_obj }, { "t_elli", &t_elli_obj },
    { "t_ellib", &t_ellib_obj }, { "t_tri", &t_tri_obj }, { "t_trib", &t_trib_obj },
    { "t_fill", &t_fill_obj }, { "t_clip", &t_clip_obj }, { "t_camera", &t_camera_obj },
    { "t_blt", &t_blt_obj }, { "t_set", &t_set_obj },
    { "tilemap_from_tmx", &f_tilemap_from_tmx_obj }, { "t_load", &t_load_obj },
    { "t_collide", &t_collide_obj },
    { "ceil", &f_ceil_obj }, { "floor", &f_floor_obj }, { "sqrt", &f_sqrt_obj },
    { "sin", &f_sin_obj }, { "cos", &f_cos_obj }, { "atan2", &f_atan2_obj },
    { "sgn", &f_sgn_obj }, { "clamp", &f_clamp_obj }, { "rseed", &f_rseed_obj },
    { "rndi", &f_rndi_obj }, { "rndf", &f_rndf_obj }, { "nseed", &f_nseed_obj },
    { "noise", &f_noise_obj },
    { "play", &f_play_obj }, { "stop", &f_stop_obj }, { "play_pos", &f_play_pos_obj },
    { "parse_notes", &f_parse_notes_obj }, { "parse_tones", &f_parse_tones_obj },
    { "parse_volumes", &f_parse_volumes_obj }, { "parse_effects", &f_parse_effects_obj },
    { "mml_check", &f_mml_check_obj }, { "pcm_load", &f_pcm_load_obj },
    { "pcm_release", &f_pcm_release_obj }, { "sound_total_sec", &f_sound_total_sec_obj },
    { "channel_set", &f_channel_set_obj },
    { "font_load", &f_font_load_obj }, { "font_text_width", &f_font_text_width_obj },
    { "clock", &f_clock_obj }, { "ticks_us", &f_ticks_us_obj }, { "sleep", &f_sleep_obj },
    { "getcwd", &f_getcwd_obj }, { "chdir", &f_chdir_obj }, { "path_stat", &f_path_stat_obj },
    { "listdir", &f_listdir_obj },
};

void px_api_register(void) {
    for (int i = 0; i < Q_COUNT; i++) Q[i] = qstr_from_str(QNAMES[i]);
    s_pyxel_dict = NULL;
    memset(&px, 0, sizeof(px));
    s_rng = host_get_ticks_us() | 1u;
    s_perm_ready = false;
    px_audio_init(px_sound_enabled);
    mp_obj_t mod = mp_obj_new_module(Q[Q__pyxel]);
    mp_obj_t globals = MP_OBJ_FROM_PTR(mp_obj_module_get_globals(mod));
    for (size_t i = 0; i < sizeof(FUNCS) / sizeof(FUNCS[0]); i++)
        mp_obj_dict_store(globals, MP_OBJ_NEW_QSTR(qstr_from_str(FUNCS[i].name)),
                          MP_OBJ_FROM_PTR(FUNCS[i].fn));
}

// ---------------------------------------------------------------------------
// Port hooks: imports and open()
// ---------------------------------------------------------------------------
mp_import_stat_t mp_import_stat(const char* path) {
    switch (px_vfs_stat(path)) {
    case 1: return MP_IMPORT_STAT_FILE;
    case 2: return MP_IMPORT_STAT_DIR;
    default: return MP_IMPORT_STAT_NO_EXIST;
    }
}

mp_lexer_t* mp_lexer_new_from_file(qstr filename) {
    size_t n = 0;
    char* data = px_vfs_read(qstr_str(filename), &n);
    if (!data) mp_raise_OSError(MP_ENOENT);
    char* src = m_new(char, n ? n : 1);
    memcpy(src, data, n);
    free(data);
    return mp_lexer_new_from_str_len(filename, src, n, n ? n : 1);
}

// open(): a read returns the whole file as a StringIO ("r") or BytesIO ("rb");
// modes "w", "a" and "x" return a pyxel._WFile.
extern const mp_obj_type_t mp_type_stringio;
extern const mp_obj_type_t mp_type_bytesio;

mp_obj_t mp_builtin_open(size_t n_args, const mp_obj_t* args, mp_map_t* kwargs) {
    const char* path = mp_obj_str_get_str(args[0]);
    mp_obj_t m = argkw(n_args, args, kwargs, 1, Q_mode, mp_const_none);
    const char* mode = m == mp_const_none ? "r" : mp_obj_str_get_str(m);
    if (strchr(mode, '+')) mp_raise_ValueError(MP_ERROR_TEXT("open() modes with '+' are not supported"));
    char kind = strchr(mode, 'w') ? 'w' : strchr(mode, 'a') ? 'a' : strchr(mode, 'x') ? 'x' : 'r';
    if (kind != 'r') {
        mp_obj_t cls = dict_get(Q__WFile);
        if (cls == MP_OBJ_NULL)
            mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("open() for writing needs pyxel imported"));
        int e = 0;
        int h = px_wfile_open(path, kind, &e);
        if (h < 0) raise_file_error(e, path);
        mp_obj_t a[3] = { MP_OBJ_NEW_SMALL_INT(h), args[0], m };
        return mp_call_function_n_kw(cls, 3, 0, a);
    }
    size_t n = 0;
    char* data = px_vfs_read(path, &n);
    if (!data) raise_file_error(px_vfs_stat(path) == 2 ? EISDIR : ENOENT, path);
    bool binary = strchr(mode, 'b') != NULL;
    mp_obj_t contents = binary ? mp_obj_new_bytes((const byte*)data, n) : mp_obj_new_str(data, n);
    free(data);
    return mp_call_function_1(MP_OBJ_FROM_PTR(binary ? &mp_type_bytesio : &mp_type_stringio), contents);
}
MP_DEFINE_CONST_FUN_OBJ_KW(mp_builtin_open_obj, 1, mp_builtin_open);
