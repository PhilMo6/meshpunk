#include "emoji_font.h"

#include <cstdio>
#include <cstring>
#include <cstdint>

#include <esp_heap_caps.h>

#include "../lib/lvgl/src/others/imgfont/lv_imgfont.h"
#include "../lib/lvgl/src/misc/lv_fs.h"

namespace {

// 0x2300 covers Miscellaneous Technical (⏸⏹⏺ ⌚⌛, 0x23xx) and Geometric
// Shapes (▶◀▪▫, 0x25xx) — emoji the blob ships but the old 0x2600 gate
// routed to montserrat (which lacks them → tofu). Codepoints in range but
// missing from the blob render as zero-width blanks, same as before.
// NOTE: emoji_preload() has no gate, so keep this aligned with the blob —
// a preload "true" must mean the render path will actually use it.
constexpr uint32_t EMOJI_MIN_CODEPOINT = 0x2300u;
constexpr const char * EMOJI_BLOB_PATH = "L:/emojis.bin";

// Blob file format (see tools/build_emoji.sh):
//   Header (16 bytes):
//     [0..3]   'EMJB' magic
//     [4..5]   version  u16 LE
//     [6..7]   pixel_size u16 LE
//     [8..11]  entry count u32 LE
//     [12..15] reserved
//   Index (count * 4 bytes):
//     sorted codepoints, u32 LE each
//   Data (count * pixel_size^2 * 4 bytes):
//     raw premultiplied BGRA pixels, same order as index
constexpr uint8_t EMJB_MAGIC[4] = {'E', 'M', 'J', 'B'};

// ---- Blob state (initialized on first emoji access) -----------------------

struct EmojiBlob {
    lv_fs_file_t file;
    bool         open       = false;
    bool         failed     = false;   // true if init was attempted and failed
    uint16_t     pixel_size = 0;
    uint32_t     count      = 0;
    uint32_t   * codepoints = nullptr; // sorted array, count entries
    uint32_t     data_off   = 0;       // byte offset where pixel data starts
    uint32_t     entry_size = 0;       // bytes per emoji = pixel_size^2 * 4
};

EmojiBlob s_blob;

// ---- Cache (same as before — PSRAM hash table of loaded descriptors) ------

constexpr uint32_t EMOJI_CACHE_CAP   = 256u;          // must be power of two
constexpr uint32_t EMOJI_CACHE_EMPTY = 0xFFFFFFFFu;

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

// ---- Helpers --------------------------------------------------------------

inline uint16_t rd_u16_le(const uint8_t * p)
{
    return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

inline uint32_t rd_u32_le(const uint8_t * p)
{
    return static_cast<uint32_t>(p[0])
         | (static_cast<uint32_t>(p[1]) << 8)
         | (static_cast<uint32_t>(p[2]) << 16)
         | (static_cast<uint32_t>(p[3]) << 24);
}

// Near-zero-width sentinel. Returned instead of nullptr for codepoints we
// want to swallow (combining modifiers + missing entries) so LVGL treats
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

// ---- Blob loading ---------------------------------------------------------

// Open the blob file and read the header + codepoint index into RAM.
// Called once on first emoji access; the file handle stays open for seeks.
bool blob_init_once()
{
    if (s_blob.open)   return true;
    if (s_blob.failed) return false;

    if (lv_fs_open(&s_blob.file, EMOJI_BLOB_PATH, LV_FS_MODE_RD) != LV_FS_RES_OK) {
        s_blob.failed = true;
        return false;
    }

    // Read 16-byte header
    uint8_t hdr[16];
    uint32_t br = 0;
    if (lv_fs_read(&s_blob.file, hdr, 16, &br) != LV_FS_RES_OK || br != 16 ||
        std::memcmp(hdr, EMJB_MAGIC, 4) != 0) {
        lv_fs_close(&s_blob.file);
        s_blob.failed = true;
        return false;
    }

    s_blob.pixel_size = rd_u16_le(&hdr[6]);
    s_blob.count      = rd_u32_le(&hdr[8]);
    s_blob.entry_size = static_cast<uint32_t>(s_blob.pixel_size) * s_blob.pixel_size * 4u;

    if (s_blob.count == 0 || s_blob.entry_size == 0) {
        lv_fs_close(&s_blob.file);
        s_blob.failed = true;
        return false;
    }

    // Read codepoint index into PSRAM
    uint32_t idx_bytes = s_blob.count * 4u;
    s_blob.codepoints = static_cast<uint32_t *>(
        heap_caps_malloc(idx_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!s_blob.codepoints) {
        lv_fs_close(&s_blob.file);
        s_blob.failed = true;
        return false;
    }

    if (lv_fs_read(&s_blob.file, s_blob.codepoints, idx_bytes, &br) != LV_FS_RES_OK ||
        br != idx_bytes) {
        heap_caps_free(s_blob.codepoints);
        s_blob.codepoints = nullptr;
        lv_fs_close(&s_blob.file);
        s_blob.failed = true;
        return false;
    }

    s_blob.data_off = 16u + idx_bytes;
    s_blob.open = true;
    return true;
}

// Binary search the sorted codepoint index. Returns index or -1.
int32_t blob_find(uint32_t cp)
{
    int32_t lo = 0;
    int32_t hi = static_cast<int32_t>(s_blob.count) - 1;
    while (lo <= hi) {
        int32_t mid = lo + (hi - lo) / 2;
        uint32_t v = s_blob.codepoints[mid];
        if (v == cp) return mid;
        if (v < cp) lo = mid + 1;
        else        hi = mid - 1;
    }
    return -1;
}

// Load one emoji's pixel data from the blob into PSRAM.
lv_image_dsc_t * load_emoji_from_blob(uint32_t cp)
{
    if (!blob_init_once()) return nullptr;

    int32_t idx = blob_find(cp);
    if (idx < 0) return nullptr;

    uint32_t offset = s_blob.data_off + static_cast<uint32_t>(idx) * s_blob.entry_size;

    if (lv_fs_seek(&s_blob.file, offset, LV_FS_SEEK_SET) != LV_FS_RES_OK)
        return nullptr;

    auto * pixels = static_cast<uint8_t *>(
        heap_caps_malloc(s_blob.entry_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    auto * out = static_cast<lv_image_dsc_t *>(
        heap_caps_malloc(sizeof(lv_image_dsc_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!pixels || !out) {
        if (pixels) heap_caps_free(pixels);
        if (out)    heap_caps_free(out);
        return nullptr;
    }

    uint32_t br = 0;
    if (lv_fs_read(&s_blob.file, pixels, s_blob.entry_size, &br) != LV_FS_RES_OK ||
        br != s_blob.entry_size) {
        heap_caps_free(pixels);
        heap_caps_free(out);
        return nullptr;
    }

    uint16_t stride = s_blob.pixel_size * 4u;

    lv_memzero(out, sizeof(*out));
    out->header.w      = s_blob.pixel_size;
    out->header.h      = s_blob.pixel_size;
    out->header.cf     = LV_COLOR_FORMAT_ARGB8888;
    out->header.stride = stride;
    out->data          = pixels;
    out->data_size     = s_blob.entry_size;
    return out;
}

// ---- imgfont callback -----------------------------------------------------

const void * emoji_path_cb(const lv_font_t * font,
                           uint32_t unicode, uint32_t unicode_next,
                           int32_t * offset_y, void * user_data)
{
    LV_UNUSED(font);
    LV_UNUSED(unicode_next);
    LV_UNUSED(user_data);

    // ASCII fast path — the overwhelming majority of glyphs; never emoji.
    if (unicode < 0x80) return nullptr;

    blank_dsc_init_once();

    // Non-rendering combining codepoints — returning the zero-width blank
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

    // No range gate: blob MEMBERSHIP decides the route. In the blob →
    // emoji glyph (every emoji the blob ships is available); not in the
    // blob → montserrat fallback (Latin-1, FontAwesome, and anything we
    // can't render shows montserrat's placeholder rather than vanishing).
    // The in-RAM binary search is ~11 compares — cheap enough per glyph.
    if (!blob_init_once() || blob_find(unicode) < 0) return nullptr;

    auto * fresh = load_emoji_from_blob(unicode);
    // In the index but failed to load (OOM/decode) — cache the blank so we
    // never retry this codepoint every frame.
    if (!fresh) {
        cache_insert(unicode, &s_blank_dsc);
        return &s_blank_dsc;
    }

    (void)cache_insert(unicode, fresh);
    return fresh;
}

} // anonymous namespace

extern "C" bool emoji_preload(uint32_t codepoint)
{
    cache_init_once();
    if (auto *cached = cache_lookup(codepoint))
        return cached != &s_blank_dsc;
    auto *fresh = load_emoji_from_blob(codepoint);
    if (!fresh) {
        cache_insert(codepoint, &s_blank_dsc);
        return false;
    }
    cache_insert(codepoint, fresh);
    return true;
}

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
