// Shared declarations for the Pyxel module.
// Graphics semantics are ported from pyxel-core (MIT, Takashi Kitao):
// canvas.rs, image.rs, tilemap.rs, graphics.rs, input.rs, system.rs.
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// ---------------------------------------------------------------------------
// Firmware exports (src/elf_host.cpp host_exports[])
// ---------------------------------------------------------------------------
extern void     host_blit_frame_async(const uint16_t* rgb565, int w, int h);
extern void     host_blit_wait(void);
extern void     host_clear_screen(void);
extern uint32_t host_get_ticks_ms(void);
extern uint32_t host_get_ticks_us(void);
extern void     host_sleep_ms(uint32_t ms);
extern int      host_get_key(int* pressed, unsigned char* key);
extern void     host_trackball_read(int* dx, int* dy, int* click);
extern int      host_trackball_button(void);
extern int      host_should_exit(void);
extern void     host_log(const char* msg);
// Whole-file write under the SD bus lock; creates missing parent directories.
// 0 on success, -1 on failure.
extern int      host_write_file(const char* path, const void* data, uint32_t size);

// ---------------------------------------------------------------------------
// Constants (settings.rs)
// ---------------------------------------------------------------------------
#define PX_NUM_COLORS    16
#define PX_MAX_COLORS    256
#define PX_NUM_IMAGES    3
#define PX_IMAGE_SIZE    256
#define PX_NUM_TILEMAPS  8
#define PX_TILEMAP_SIZE  256
#define PX_TILE_SIZE     8
#define PX_TILE_SHIFT    3
#define PX_TILE_MASK     7
#define PX_FONT_WIDTH    4
#define PX_FONT_HEIGHT   6
#define PX_SCREEN_MAX_W  320
#define PX_SCREEN_MAX_H  240

// ---------------------------------------------------------------------------
// Canvas (canvas.rs). Images hold uint8_t color indices; tilemaps hold
// uint16_t tiles packed as tx | (ty << 8).
// ---------------------------------------------------------------------------
typedef struct {
    int left, top, right, bottom;
    int width, height;
} px_rect;

typedef struct {
    int w, h;
    px_rect self_rect;
    px_rect clip;
    int cam_x, cam_y;
    float alpha;
    void* data;
} px_canvas;

typedef struct {
    px_canvas cv;
    uint8_t pal[PX_MAX_COLORS];
    bool pal_identity;
} px_image;

typedef struct {
    px_canvas cv;
    int imgsrc;           // image bank index
} px_tilemap;

#define PX_TILE(tx, ty)   ((uint16_t)(((tx) & 0xFF) | (((ty) & 0xFF) << 8)))
#define PX_TILE_X(t)      ((t) & 0xFF)
#define PX_TILE_Y(t)      (((t) >> 8) & 0xFF)

// Rust `f32::round() as i32` / `as u32` (round half away from zero, saturate).
int px_f2i(float x);
int px_f2u(float x);

px_rect px_rect_new(int left, int top, int width, int height);
px_rect px_rect_intersect(px_rect a, px_rect b);
static inline bool px_rect_empty(px_rect r) { return r.width <= 0 || r.height <= 0; }
static inline bool px_rect_contains(px_rect r, int x, int y) {
    return x >= r.left && x <= r.right && y >= r.top && y <= r.bottom;
}

void px_canvas_init(px_canvas* cv, int w, int h, void* data);
void px_canvas_set_clip(px_canvas* cv, float x, float y, float w, float h);
void px_canvas_reset_clip(px_canvas* cv);

// Template instances: _u8 for images, _u16 for tilemaps.
#define PX_CANVAS_DECL(SUF, T) \
    void px_clear_##SUF(px_canvas* cv, T v); \
    T    px_value_##SUF(px_canvas* cv, float x, float y); \
    void px_set_value_##SUF(px_canvas* cv, float x, float y, T v); \
    void px_line_##SUF(px_canvas* cv, float x1, float y1, float x2, float y2, T v); \
    void px_rect_##SUF(px_canvas* cv, float x, float y, float w, float h, T v); \
    void px_rectb_##SUF(px_canvas* cv, float x, float y, float w, float h, T v); \
    void px_circ_##SUF(px_canvas* cv, float x, float y, float r, T v); \
    void px_circb_##SUF(px_canvas* cv, float x, float y, float r, T v); \
    void px_elli_##SUF(px_canvas* cv, float x, float y, float w, float h, T v); \
    void px_ellib_##SUF(px_canvas* cv, float x, float y, float w, float h, T v); \
    void px_tri_##SUF(px_canvas* cv, float x1, float y1, float x2, float y2, float x3, float y3, T v); \
    void px_trib_##SUF(px_canvas* cv, float x1, float y1, float x2, float y2, float x3, float y3, T v); \
    void px_fill_##SUF(px_canvas* cv, float x, float y, T v); \
    void px_blit_##SUF(px_canvas* dst, float x, float y, const px_canvas* src, float sx, float sy, \
                       float w, float h, int colkey, const T* pal); \
    void px_blit_xform_##SUF(px_canvas* dst, float x, float y, const px_canvas* src, float sx, float sy, \
                             float w, float h, int colkey, const T* pal, float rotate, float scale); \
    void px_write_clipped_##SUF(px_canvas* cv, int x, int y, T v);

PX_CANVAS_DECL(u8, uint8_t)
PX_CANVAS_DECL(u16, uint16_t)

// ---------------------------------------------------------------------------
// Images (image.rs) and tilemaps (tilemap.rs)
// ---------------------------------------------------------------------------
px_image* px_image_new(int w, int h);            // malloc'd struct + pixels
void      px_image_free(px_image* img);
void      px_image_init(px_image* img, int w, int h, uint8_t* data);
void      px_image_reset_pal(px_image* img);
static inline const uint8_t* px_image_pal(px_image* img) {
    return img->pal_identity ? NULL : img->pal;
}
void px_image_blt(px_image* dst, float x, float y, px_image* src, float u, float v,
                  float w, float h, int colkey, float rotate, float scale);
void px_image_bltm(px_image* dst, float x, float y, px_tilemap* tm, px_image* tm_img,
                   float u, float v, float w, float h, int colkey, float rotate, float scale);
void px_image_text(px_image* dst, float x, float y, const char* s, size_t len, int col);
// Perspective blits: pos = camera (x, y, z), rot = degrees (x, y, z), fov degrees.
void px_image_blt3d(px_image* dst, float x, float y, float w, float h, px_image* src,
                    const float pos[3], const float rot[3], float fov, int colkey);
void px_image_bltm3d(px_image* dst, float x, float y, float w, float h, px_tilemap* tm,
                     px_image* tm_img, const float pos[3], const float rot[3], float fov, int colkey);

void px_tilemap_init(px_tilemap* tm, int w, int h, uint16_t* data, int imgsrc);

// ---------------------------------------------------------------------------
// Runtime state (pyxel.rs singletons)
// ---------------------------------------------------------------------------
typedef struct {
    bool initialized;
    int width, height, fps;
    uint32_t quit_key;
    uint32_t frame_count;
    px_image* screen;
    px_image* images[PX_NUM_IMAGES];
    px_tilemap* tilemaps[PX_NUM_TILEMAPS];
    px_image* cursor;
    uint32_t colors[PX_MAX_COLORS];
    int num_colors;
    bool colors_dirty;        // C changed colors[]: the API copies them to pyxel.colors
    bool mouse_visible;
    bool mouse_mode;          // trackball drives the mouse (sticky once set)
    int mouse_x, mouse_y, mouse_wheel;
    bool quit_requested;
    bool reset_requested;
} px_state_t;

extern px_state_t px;

px_image*   px_bank_image(int i);
px_tilemap* px_bank_tilemap(int i);   // allocates tile data on first use

// ---------------------------------------------------------------------------
// Input (input.rs)
// ---------------------------------------------------------------------------
// Key codes: identical to upstream below 0x80. Upstream's SDL scancode keys
// (0x4000_00nn) are renumbered 0x10000 + nn, and its virtual/mouse/gamepad
// keys (0x5000_0000 + n) are renumbered 0x20000 + n, so every key constant is
// a MicroPython small int.
#define PX_KEY_SCAN(n)        (0x10000u + (n))
#define PX_KEY_VIRT(n)        (0x20000u + (n))
#define PX_KEY_RETURN         0x0Du
#define PX_KEY_ESCAPE         0x1Bu
#define PX_KEY_RIGHT          PX_KEY_SCAN(0x4F)
#define PX_KEY_LEFT           PX_KEY_SCAN(0x50)
#define PX_KEY_DOWN           PX_KEY_SCAN(0x51)
#define PX_KEY_UP             PX_KEY_SCAN(0x52)
#define PX_KEY_LCTRL          PX_KEY_SCAN(0xE0)
#define PX_KEY_LSHIFT         PX_KEY_SCAN(0xE1)
#define PX_KEY_LALT           PX_KEY_SCAN(0xE2)
#define PX_KEY_NONE           PX_KEY_VIRT(0)
#define PX_KEY_SHIFT          PX_KEY_VIRT(1)
#define PX_KEY_CTRL           PX_KEY_VIRT(2)
#define PX_KEY_ALT            PX_KEY_VIRT(3)
#define PX_MOUSE_START        PX_KEY_VIRT(0x100)
#define PX_MOUSE_POS_X        (PX_MOUSE_START + 0)
#define PX_MOUSE_POS_Y        (PX_MOUSE_START + 1)
#define PX_MOUSE_WHEEL_X      (PX_MOUSE_START + 2)
#define PX_MOUSE_WHEEL_Y      (PX_MOUSE_START + 3)
#define PX_MOUSE_BUTTON_LEFT  (PX_MOUSE_START + 4)
#define PX_MOUSE_BUTTON_RIGHT (PX_MOUSE_START + 6)
#define PX_GAMEPAD_START      PX_KEY_VIRT(0x200)
#define PX_GAMEPAD_STRIDE     0x100u
#define PX_GAMEPAD_AXIS_COUNT 6u
#define PX_GAMEPAD1_BUTTON(n) (PX_GAMEPAD_START + PX_GAMEPAD_AXIS_COUNT + (n))  // n = 0 (A) .. 14 (DPAD_RIGHT)

// Host key bytes the launcher's keymap can output for gamepad 1 buttons:
// 0xC0 + n  ->  GAMEPAD1_BUTTON_A + n  (n = 0..14).
#define PX_HOST_GAMEPAD_BASE  0xC0
#define PX_HOST_GAMEPAD_COUNT 15

void px_input_reset(void);
void px_input_poll(void);          // drain host events into key states for this frame
bool px_btn(uint32_t key);
bool px_btnp(uint32_t key, int hold, int repeat);
bool px_btnr(uint32_t key);
int  px_btnv(uint32_t key);
void px_input_enable_mouse(void);
void px_input_frame_text(char* out, size_t cap, uint32_t* keys, int* nkeys, int max_keys);

// ---------------------------------------------------------------------------
// Display (system.rs render path)
// ---------------------------------------------------------------------------
bool px_display_begin(int w, int h, char* err, size_t errlen);
void px_display_render(void);
void px_display_end(void);
void px_draw_cursor(void);

// ---------------------------------------------------------------------------
// Files: .pyxapp mounts and path resolution (vfs.c)
// ---------------------------------------------------------------------------
// Returns malloc'd contents (NUL-terminated past *size) or NULL.
char* px_vfs_read(const char* path, size_t* size);
// 0 = missing, 1 = file, 2 = directory.
int   px_vfs_stat(const char* path);
// Mount a .pyxapp/.zip (or a plain .py path); fills the startup script path.
bool  px_vfs_mount(const char* game_path, char* startup, size_t cap, char* err, size_t errlen);
void  px_vfs_unmount(void);
void  px_vfs_set_cwd(const char* dir);
const char* px_vfs_cwd(void);
// Resolve `path` against the cwd into `out` (normalized, absolute).
void  px_vfs_resolve(const char* path, char* out, size_t cap);
// Names in a directory of the mounted .pyxapp, one callback per archive entry
// below it (the same name repeats for each file of a subdirectory). Returns 0,
// ENOTDIR, ENOENT, or -1 for a path outside the archive (the firmware exports
// no directory listing).
int   px_vfs_listdir(const char* path, void (*cb)(void* ctx, const char* name, size_t len), void* ctx);
void  px_path_dirname(const char* path, char* out, size_t cap);
// Write a whole file. A path inside the mounted .pyxapp replaces or adds that
// archive entry for the rest of the run (upstream runs a .pyxapp from a
// temporary extracted copy); a path under /sd/ goes to host_write_file.
// Returns 0 or an errno value.
int   px_vfs_write(const char* path, const void* data, size_t n);
// Files open for writing (open() modes "w", "a", "x"): the contents are held
// in memory and written with px_vfs_write by flush and close. Functions
// return 0 or an errno value; px_wfile_open returns a handle, or -1 with *err set.
int   px_wfile_open(const char* path, char mode, int* err);
int   px_wfile_write(int h, const void* data, size_t n);
int   px_wfile_flush(int h);
int   px_wfile_close(int h);
// Close every open write file, writing its contents (failures are logged).
void  px_wfile_close_all(void);
// Raw DEFLATE -> malloc'd buffer (lodepng).
unsigned char* px_inflate(const unsigned char* in, size_t in_size, size_t expect, size_t* out_size);

// ZIP central-directory walk. The callback returns false (with err set) to stop.
typedef struct {
    const char* name;           // not NUL-terminated
    int name_len;
    int method;                 // 0 stored, 8 deflate
    uint32_t csize, usize;
    const unsigned char* data;  // compressed bytes inside the archive
} px_zip_file;
typedef bool (*px_zip_cb)(void* ctx, const px_zip_file* f, char* err, size_t errlen);
bool px_zip_walk(const unsigned char* z, size_t zn, px_zip_cb cb, void* ctx, char* err, size_t errlen);
// Stored/inflated contents, malloc'd with a NUL after *out_size.
unsigned char* px_zip_file_contents(const px_zip_file* f, size_t* out_size, char* err, size_t errlen);

// ---------------------------------------------------------------------------
// Resources (res.c)
// ---------------------------------------------------------------------------
// Callback gets each sound / music as parsed int arrays; the API layer turns
// them into Python values.
typedef struct {
    int32_t* v;        // flat values
    int n;             // number of values
    int* row_start;    // 2-level arrays: start index of each row
    int* row_len;
    int rows;          // 0 for 1-level arrays
} px_intarr;

typedef struct {
    void (*sound)(void* ctx, int index, const px_intarr* notes, const px_intarr* tones,
                  const px_intarr* volumes, const px_intarr* effects, int speed);
    void (*music)(void* ctx, int index, const px_intarr* seqs);
    void* ctx;
} px_res_sink;

bool px_res_load(const char* path, bool ex_img, bool ex_tm, bool ex_snd, bool ex_mus,
                 const px_res_sink* sink, char* err, size_t errlen);
// Parse a .pyxpal next to `pyxres_path`, if present. Returns the color count (0 = none).
int  px_res_load_pal(const char* pyxres_path, uint32_t* colors, int max);
// A .pyxres archive (resource.rs save_resource): the TOML resource_data.rs
// writes, as the one stored entry of a ZIP. A bank with count 0 is written
// as an empty array. sounds_toml / musics_toml hold the finished [[sounds]] /
// [[musics]] tables ("" = empty array). Returns a malloc'd archive, or NULL
// when out of memory.
unsigned char* px_res_build(px_image* const* images, int nimages, px_tilemap* const* tilemaps,
                            int ntilemaps, const char* sounds_toml, const char* musics_toml,
                            size_t* out_size);
// Decode a PNG to a new image (nearest palette color per pixel, or the file's
// own colors as the new palette when include_colors).
px_image* px_image_decode_png(const char* path, bool include_colors, char* err, size_t errlen);
// One layer of a Tiled TMX map (tmx_parser.rs): CSV-encoded layers, 8x8
// tiles, tile coordinates from the first tileset's column count. Returns a
// malloc'd w*h tile array.
uint16_t* px_tmx_load(const char* path, int layer, int* w, int* h, char* err, size_t errlen);

// Tilemap.collide (tilemap.rs): the (dx, dy) that stops the w x h pixel
// rectangle at (x, y) at the first wall tile.
void px_tilemap_collide(const px_tilemap* tm, float x, float y, float w, float h,
                        float* dx, float* dy, const uint16_t* walls, int nwalls);

// ---------------------------------------------------------------------------
// BDF fonts (font.c)
// ---------------------------------------------------------------------------
int  px_font_load(const char* path, char* err, size_t errlen);   // handle, -1 on error
bool px_font_valid(int handle);
void px_font_draw(int handle, px_image* dst, float x, float y, const char* s, size_t len, int col);
int  px_font_text_width(int handle, const char* s, size_t len);
void px_font_shutdown(void);

// ---------------------------------------------------------------------------
// Audio (audio.c)
// ---------------------------------------------------------------------------
#define PX_NUM_CHANNELS     4
#define PX_NUM_TONES        4
#define PX_TONE_MAX_SAMPLES 256
#define PX_AUDIO_CLOCK_RATE 1789773u      // NTSC NES APU clock (settings.rs)
#define PX_AUDIO_RATE       22050u
#define PX_AUTO             0xFFFFFFFFu   // glide parameter taken from the note

// Sound commands (mml_command.rs). Every sound compiles to a list of these:
// classic note/tone/volume/effect sounds exactly as sound.rs emit_commands,
// MML through mml_parser.rs / old_mml_parser.rs.
enum {
    PX_CMD_TEMPO,         // u = clocks per tick
    PX_CMD_QUANTIZE,      // f = gate ratio
    PX_CMD_TONE,          // u = tone
    PX_CMD_VOLUME,        // f = level
    PX_CMD_TRANSPOSE,     // f = semitones
    PX_CMD_DETUNE,        // f = semitones
    PX_CMD_ENVELOPE,      // u = slot (0 = off)
    PX_CMD_ENVELOPE_SET,  // u = slot, f = initial level, seg/nseg = segments
    PX_CMD_VIBRATO,       // u = slot
    PX_CMD_VIBRATO_SET,   // u = slot, ticks = delay, period, f = depth (semitones)
    PX_CMD_GLIDE,         // u = slot
    PX_CMD_GLIDE_SET,     // u = slot, f = offset (NAN = auto), ticks = duration (PX_AUTO)
    PX_CMD_NOTE,          // u = MIDI note, ticks = duration
    PX_CMD_REST,          // ticks = duration
    PX_CMD_REPEAT_START,
    PX_CMD_REPEAT_END,    // u = play count (0 = forever)
};

typedef struct {
    uint32_t ticks;
    float level;
} px_env_seg;

typedef struct {
    uint8_t type;
    uint32_t u;
    uint32_t ticks;
    uint32_t period;
    uint32_t seg, nseg;   // envelope segments in the sound's segment pool
    float f;
} px_cmd;

typedef struct {
    px_cmd* cmds;
    int ncmd, capcmd;
    px_env_seg* segs;
    int nseg, capseg;
    int pcm;              // PCM handle, -1 = none
    bool empty;           // nothing to play (no notes, no MML commands, no PCM)
} px_snd;

// sound.c
void px_snd_init(px_snd* s);
void px_snd_free(px_snd* s);
// tone_modes: mode of each of the n_tones tones (0 wavetable, 1/2 noise).
bool px_snd_from_legacy(px_snd* s, const int* notes, int nn, const int* tones, int nt,
                        const int* vols, int nv, const int* fxs, int nf, int speed,
                        const int* tone_modes, int n_tones);
// force_old: the old syntax (Sound.old_mml); otherwise code containing 'x',
// 'X' or '~' is old syntax, as Sound.mml decides upstream.
bool px_snd_from_mml(px_snd* s, const char* code, bool force_old, const int* tone_modes,
                     int n_tones, char* err, size_t errlen);
void px_snd_from_pcm(px_snd* s, int handle);
// Total clocks, false when the sound repeats forever.
bool px_snd_total_clocks(const px_snd* s, uint64_t* clocks);
uint32_t px_bpm_to_clocks_per_tick(uint32_t bpm);

// PCM: WAV decoded to mono 16-bit at PX_AUDIO_RATE (pcm_decoder.rs).
int  px_pcm_load(const char* path, char* err, size_t errlen);   // handle with one reference
void px_pcm_ref(int handle);
void px_pcm_unref(int handle);
const int16_t* px_pcm_samples(int handle, uint32_t* n);
void px_pcm_shutdown(void);

// audio.c
void px_audio_init(bool enabled);
void px_audio_shutdown(void);
void px_audio_set_tone(int i, int mode, const uint32_t* table, int len, int sample_bits, float gain);
int  px_audio_tone_mode(int i);
void px_audio_set_channel(int ch, float gain, int detune_cents);
// Takes ownership of `snds` (malloc'd array of n compiled sounds).
void px_audio_play(int ch, px_snd* snds, int n, float sec, bool loop, bool resume);
void px_audio_stop(int ch);          // -1 = all channels
bool px_audio_pos(int ch, int* sound_index, float* sec);
void px_audio_service(void);
void px_audio_poll(void);
extern uint32_t px_audio_samples, px_audio_underruns, px_audio_us;

// ---------------------------------------------------------------------------
// Python binding (api.c)
// ---------------------------------------------------------------------------
void px_api_register(void);
extern bool px_sound_enabled;       // -sound 0 on the command line turns synthesis off
extern bool px_kbtoggle;            // -kbtoggle given: Alt+Enter switches the key bindings off for typing
extern int64_t px_clock_epoch;      // -clock: device clock (UTC seconds) at launch, 0 if not given
extern int px_clock_tzmin;          // -clock: UTC offset in minutes

// Console (main.c): what the game prints goes to the serial log, and its last
// PX_CONSOLE_LINES lines, wrapped at PX_CONSOLE_COLS, are kept for input().
#define PX_CONSOLE_LINES 32
#define PX_CONSOLE_COLS  79
void px_console_write(const char* s, size_t n);
const char* px_console_line(int back);          // back 0 = newest complete line; NULL past the oldest
const char* px_console_partial(size_t* len);    // the line still being printed
