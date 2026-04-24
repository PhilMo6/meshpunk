#include "emoji_font.h"

#include <cstdio>
#include <cstring>
#include <cstdint>

#include <esp_heap_caps.h>

#include "../lib/lvgl/src/others/imgfont/lv_imgfont.h"
#include "../lib/lvgl/src/misc/lv_fs.h"

namespace {

constexpr uint32_t EMOJI_MIN_CODEPOINT = 0x2600u;
constexpr const char * EMOJI_PATH_PREFIX = "S:/emoji/";

// Cache: first time an emoji is seen we read the .bin from SD, copy the raw
// pixels into PSRAM, and build a persistent lv_image_dsc_t. Returning that
// struct from path_cb makes LVGL treat the src as LV_IMAGE_SRC_VARIABLE —
// subsequent lookups and draws read directly from PSRAM, never hitting SD.
constexpr uint32_t EMOJI_CACHE_CAP   = 256u;          // must be power of two
constexpr uint32_t EMOJI_CACHE_EMPTY = 0xFFFFFFFFu;

// .bin header layout (see tools/build_emoji.sh):
//   [0..3]   'MEMO'
//   [4..5]   width   u16 LE
//   [6..7]   height  u16 LE
//   [8]      color format (LV_COLOR_FORMAT_ARGB8888 = 0x10)
//   [9..10]  stride  u16 LE
//   [11..15] reserved
constexpr uint32_t MEMO_HEADER_SIZE    = 16u;
constexpr uint8_t  MEMO_MAGIC[4]       = {'M', 'E', 'M', 'O'};

struct EmojiCacheEntry {
    uint32_t codepoint;
    lv_image_dsc_t * dsc;
};

EmojiCacheEntry s_cache[EMOJI_CACHE_CAP];
uint32_t s_cache_count = 0;
bool s_cache_inited = false;

void cache_init_once()
{
    if (s_cache_inited) return;
    for (auto & e : s_cache) {
        e.codepoint = EMOJI_CACHE_EMPTY;
        e.dsc = nullptr;
    }
    s_cache_inited = true;
}

uint32_t hash_cp(uint32_t cp)
{
    cp = ((cp >> 16) ^ cp) * 0x45d9f3bu;
    cp = ((cp >> 16) ^ cp) * 0x45d9f3bu;
    cp = (cp >> 16) ^ cp;
    return cp;
}

lv_image_dsc_t * cache_lookup(uint32_t cp)
{
    uint32_t idx = hash_cp(cp) & (EMOJI_CACHE_CAP - 1u);
    for (uint32_t probe = 0; probe < EMOJI_CACHE_CAP; probe++) {
        uint32_t i = (idx + probe) & (EMOJI_CACHE_CAP - 1u);
        if (s_cache[i].codepoint == EMOJI_CACHE_EMPTY) return nullptr;
        if (s_cache[i].codepoint == cp) return s_cache[i].dsc;
    }
    return nullptr;
}

bool cache_insert(uint32_t cp, lv_image_dsc_t * dsc)
{
    if (s_cache_count >= EMOJI_CACHE_CAP) return false;
    uint32_t idx = hash_cp(cp) & (EMOJI_CACHE_CAP - 1u);
    for (uint32_t probe = 0; probe < EMOJI_CACHE_CAP; probe++) {
        uint32_t i = (idx + probe) & (EMOJI_CACHE_CAP - 1u);
        if (s_cache[i].codepoint == EMOJI_CACHE_EMPTY) {
            s_cache[i].codepoint = cp;
            s_cache[i].dsc = dsc;
            s_cache_count++;
            return true;
        }
    }
    return false;
}

inline uint16_t rd_u16_le(const uint8_t * p)
{
    return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

// Near-zero-width sentinel. Returned instead of nullptr for codepoints we
// want to swallow (combining modifiers + missing .bin files) so LVGL treats
// the glyph as resolved and doesn't walk to the montserrat fallback, which
// would render its LV_USE_FONT_PLACEHOLDER box (tofu).
//
// NOTE: can't use w=0 / data=NULL — lv_image_decoder.c:302 rejects variable
// sources with data==NULL and returns LV_RESULT_INVALID, which makes
// imgfont_get_glyph_dsc return false and we fall through to the montserrat
// placeholder again. So we give it a real 1x1 fully-transparent ARGB8888
// pixel. adv_w/box_w = 1 — one invisible column, still visually a blank,
// still no tofu.
alignas(4) const uint8_t s_blank_pixel[4] = { 0, 0, 0, 0 };  // B,G,R,A
lv_image_dsc_t s_blank_dsc = {};
bool s_blank_dsc_inited = false;

void blank_dsc_init_once()
{
    if (s_blank_dsc_inited) return;
    s_blank_dsc.header.cf     = LV_COLOR_FORMAT_ARGB8888;
    s_blank_dsc.header.w      = 1;
    s_blank_dsc.header.h      = 1;
    s_blank_dsc.header.stride = 4;
    s_blank_dsc.data          = s_blank_pixel;
    s_blank_dsc.data_size     = sizeof(s_blank_pixel);
    s_blank_dsc_inited = true;
}

// Read the .bin straight into PSRAM. The file already stores premultiplied
// BGRA at the right size, so there's no decoder, no resize, no color swap.
lv_image_dsc_t * load_bin_to_psram(uint32_t cp)
{
    char path[48];
    std::snprintf(path, sizeof(path), "%s%x.bin", EMOJI_PATH_PREFIX,
                  static_cast<unsigned>(cp));

    lv_fs_file_t f;
    if (lv_fs_open(&f, path, LV_FS_MODE_RD) != LV_FS_RES_OK) return nullptr;

    uint8_t header[MEMO_HEADER_SIZE];
    uint32_t br = 0;
    if (lv_fs_read(&f, header, MEMO_HEADER_SIZE, &br) != LV_FS_RES_OK ||
        br != MEMO_HEADER_SIZE ||
        std::memcmp(header, MEMO_MAGIC, 4) != 0) {
        lv_fs_close(&f);
        return nullptr;
    }

    uint16_t w      = rd_u16_le(&header[4]);
    uint16_t h      = rd_u16_le(&header[6]);
    uint8_t  cf     = header[8];
    uint16_t stride = rd_u16_le(&header[9]);

    uint32_t data_size = static_cast<uint32_t>(stride) * h;
    if (data_size == 0) {
        lv_fs_close(&f);
        return nullptr;
    }

    auto * pixels = static_cast<uint8_t *>(
        heap_caps_malloc(data_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    auto * out = static_cast<lv_image_dsc_t *>(
        heap_caps_malloc(sizeof(lv_image_dsc_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!pixels || !out) {
        if (pixels) heap_caps_free(pixels);
        if (out)    heap_caps_free(out);
        lv_fs_close(&f);
        return nullptr;
    }

    if (lv_fs_read(&f, pixels, data_size, &br) != LV_FS_RES_OK || br != data_size) {
        heap_caps_free(pixels);
        heap_caps_free(out);
        lv_fs_close(&f);
        return nullptr;
    }
    lv_fs_close(&f);

    lv_memzero(out, sizeof(*out));
    out->header.w      = w;
    out->header.h      = h;
    out->header.cf     = static_cast<lv_color_format_t>(cf);
    out->header.stride = stride;
    out->data          = pixels;
    out->data_size     = data_size;
    return out;
}

const void * emoji_path_cb(const lv_font_t * font,
                           uint32_t unicode, uint32_t unicode_next,
                           int32_t * offset_y, void * user_data)
{
    LV_UNUSED(font);
    LV_UNUSED(unicode_next);
    LV_UNUSED(user_data);

    // Below the emoji threshold — let LVGL fall through to the montserrat
    // fallback (ASCII/Latin/FontAwesome).
    if (unicode < EMOJI_MIN_CODEPOINT) return nullptr;

    blank_dsc_init_once();

    // Non-rendering combining codepoints — no glyph, and hitting SD for each
    // stalls rendering on emoji-heavy labels. Returning the zero-width blank
    // (instead of nullptr) prevents LVGL from walking to montserrat and
    // drawing a placeholder tofu box.
    if (unicode == 0xFE0F) return &s_blank_dsc;                        // VS-16
    if (unicode == 0xFE0E) return &s_blank_dsc;                        // VS-15
    if (unicode == 0x200D) return &s_blank_dsc;                        // ZWJ
    if (unicode >= 0x1F3FB && unicode <= 0x1F3FF) return &s_blank_dsc; // skin tone
    if (unicode >= 0xE0020 && unicode <= 0xE007F) return &s_blank_dsc; // tag chars
    if (unicode >= 0x1F1E6 && unicode <= 0x1F1FF) return &s_blank_dsc; // regional

    if (offset_y) *offset_y = 0;

    cache_init_once();

    if (auto * cached = cache_lookup(unicode)) return cached;

    auto * fresh = load_bin_to_psram(unicode);
    // Missing .bin on SD (or OOM) — swallow silently rather than tofu.
    if (!fresh) return &s_blank_dsc;

    // cache_insert failing means the table is full — we still return the
    // loaded dsc so the glyph renders. Subsequent lookups will reload (the
    // leak is bounded: once full, the working set is stable).
    (void)cache_insert(unicode, fresh);
    return fresh;
}

} // anonymous namespace

extern "C" lv_font_t * emoji_font_create(uint16_t height, const lv_font_t * fallback)
{
    lv_font_t * f = lv_imgfont_create(height, emoji_path_cb, nullptr);
    if (!f) return nullptr;
    f->fallback = fallback;
    return f;
}

extern "C" void emoji_font_destroy(lv_font_t * font)
{
    if (font) lv_imgfont_destroy(font);
}
