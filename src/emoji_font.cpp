#include "emoji_font.h"

#include <cstdio>
#include <cstring>
#include <cstdint>

#include <esp_heap_caps.h>

#include "../lib/lvgl/src/others/imgfont/lv_imgfont.h"
#include "../lib/lvgl/src/misc/lv_fs.h"
#include "../lib/lvgl/lvgl.h"   // lv_timer / lv_image_cache_drop / lv_obj_invalidate
#include "meshpunk_sync.h"      // sd_spi_take() (inline SPI_LOCK) for SD-resident blobs

extern void sd_spi_release();   // defined in main.cpp (legacy TFT-recovery wrapper)
extern bool sd_mounted;         // main.cpp

namespace {

constexpr const char * EMOJI_BLOB_PATH = "L:/emojis.bin";
// SD-resident blob, preferred when a card is mounted: the full emoji set
// (all ZWJ/skin-tone sequences) is too big for the 6MB LittleFS partition,
// so the complete blob can live on the card while the LittleFS blob stays
// as the no-SD fallback set.
constexpr const char * EMOJI_BLOB_PATH_SD = "S:/meshpunk/emojis.bin";

// Blob file format v2 (see tools/build_emoji.sh):
//   Header (16 bytes):
//     [0..3]   'EMJB' magic
//     [4..5]   version  u16 LE (1 or 2)
//     [6..7]   pixel_size u16 LE
//     [8..11]  glyph count u32 LE
//     [12..15] sequence count u32 LE (v2; reserved zeros in v1)
//   Index (count * 4 bytes):
//     sorted codepoints, u32 LE each. Multi-codepoint sequences appear
//     under their assigned PUA codepoint (0xE000 + i).
//   Data (count * pixel_size^2 * 4 bytes):
//     raw BGRA pixels, same order as index
//   Sequence table (v2 only; seq_count * 48 bytes, after the pixel data),
//   sorted by (cps[0] asc, len desc) so compose can binary-search the lead
//   codepoint and greedy-match longest-first:
//     u32 pua, u32 len, u32 cps[10] (zero-padded)
constexpr uint8_t EMJB_MAGIC[4] = {'E', 'M', 'J', 'B'};
constexpr uint32_t EMOJI_PUA_BASE = 0xE000u;

// ---- Blob state (initialized on first emoji access) -----------------------

struct EmojiBlob {
    lv_fs_file_t file;
    bool         open       = false;
    bool         failed     = false;   // true if init was attempted and failed
    bool         is_sd      = false;   // SD blob → bracket file ops with sd_spi_take/release
    uint16_t     pixel_size = 0;
    uint32_t     count      = 0;
    uint32_t   * codepoints = nullptr; // sorted array, count entries
    uint32_t     data_off   = 0;       // byte offset where pixel data starts
    uint32_t     entry_size = 0;       // bytes per emoji = pixel_size^2 * 4
    uint32_t     seq_count  = 0;       // v2: sequence-table entries (0 = none)
};

EmojiBlob s_blob;

// v2 sequence table — maps multi-codepoint sequences (ZWJ, skin tone, keycap,
// flag) to their PUA glyph. Layout mirrors the on-disk entries exactly.
struct SeqEnt {
    uint32_t pua;
    uint32_t len;      // 2..10 real codepoints
    uint32_t cps[10];
};
static_assert(sizeof(SeqEnt) == 48, "seq entry layout must match build_emoji.sh");

SeqEnt   * s_seq        = nullptr;   // PSRAM, s_blob.seq_count entries
uint16_t * s_pua_to_seq = nullptr;   // [pua - EMOJI_PUA_BASE] -> s_seq index

// RAII bracket for SD-resident blob file ops (SPI bus shared with TFT+radio).
struct SdLock {
    bool active;
    explicit SdLock(bool on) : active(on) { if (active) sd_spi_take(); }
    ~SdLock() { if (active) sd_spi_release(); }
};

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

// ---- Cache-full recycling --------------------------------------------------
// The cache has no per-slot eviction: a returned dsc pointer lands in DEFERRED
// draw tasks (imgfont stores it in the glyph dsc; the image draw executes later
// in the refresh cycle) and may sit in LVGL's decoder cache, so freeing a
// published glyph synchronously risks use-after-free. Instead, when the cache
// fills we schedule a FULL clear from an lv_timer: timers run strictly outside
// the display-refresh timer (LV_USE_OS 0 — all draw tasks complete inside it),
// so at that point nothing in the pipeline references any glyph. Visible emoji
// reload lazily after the invalidate; the triggering glyph is blank one frame.

bool s_clear_scheduled = false;

void cache_clear_timer_cb(lv_timer_t * t)
{
    LV_UNUSED(t);   // repeat_count 1: LVGL deletes the timer after this call
    emoji_font_cache_clear();
    lv_image_cache_drop(NULL);   // decoder entries may point at freed dscs
    lv_obj_invalidate(lv_screen_active());
    s_clear_scheduled = false;
}

void schedule_cache_clear()
{
    if (s_clear_scheduled) return;
    lv_timer_t * t = lv_timer_create(cache_clear_timer_cb, 20, nullptr);
    if (!t) return;   // OOM: stay full, blank-render until a later attempt lands
    lv_timer_set_repeat_count(t, 1);
    s_clear_scheduled = true;
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

// Load the v2 sequence table (sits after the pixel data) into PSRAM and
// build the pua->entry map used by decompose. Caller holds the SD lock when
// the blob is SD-resident. Returns false on any failure — the caller then
// zeroes seq_count and the blob still works as a glyph-only (v1) set.
bool seq_table_load()
{
    uint32_t seq_off   = s_blob.data_off + s_blob.count * s_blob.entry_size;
    uint32_t seq_bytes = s_blob.seq_count * static_cast<uint32_t>(sizeof(SeqEnt));

    s_seq = static_cast<SeqEnt *>(
        heap_caps_malloc(seq_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    s_pua_to_seq = static_cast<uint16_t *>(
        heap_caps_malloc(s_blob.seq_count * 2u, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!s_seq || !s_pua_to_seq) goto fail;

    {
        uint32_t br = 0;
        if (lv_fs_seek(&s_blob.file, seq_off, LV_FS_SEEK_SET) != LV_FS_RES_OK) goto fail;
        if (lv_fs_read(&s_blob.file, s_seq, seq_bytes, &br) != LV_FS_RES_OK ||
            br != seq_bytes) goto fail;
    }

    // Entries are sorted by lead codepoint, not PUA — build the reverse map.
    for (uint32_t i = 0; i < s_blob.seq_count; i++) {
        uint32_t slot = s_seq[i].pua - EMOJI_PUA_BASE;
        if (slot >= s_blob.seq_count || s_seq[i].len < 2 ||
            s_seq[i].len > 10) goto fail;   // corrupt table
        s_pua_to_seq[slot] = static_cast<uint16_t>(i);
    }
    return true;

fail:
    if (s_seq)        { heap_caps_free(s_seq);        s_seq = nullptr; }
    if (s_pua_to_seq) { heap_caps_free(s_pua_to_seq); s_pua_to_seq = nullptr; }
    return false;
}

// Open one candidate blob file and read header + codepoint index + (v2)
// sequence table into RAM. The file handle stays open for glyph seeks.
bool blob_try_open(const char * path, bool is_sd)
{
    SdLock lock(is_sd);

    s_blob.seq_count = 0;
    if (lv_fs_open(&s_blob.file, path, LV_FS_MODE_RD) != LV_FS_RES_OK)
        return false;

    // Read 16-byte header
    uint8_t hdr[16];
    uint32_t br = 0;
    if (lv_fs_read(&s_blob.file, hdr, 16, &br) != LV_FS_RES_OK || br != 16 ||
        std::memcmp(hdr, EMJB_MAGIC, 4) != 0) {
        lv_fs_close(&s_blob.file);
        return false;
    }

    uint16_t version  = rd_u16_le(&hdr[4]);
    s_blob.pixel_size = rd_u16_le(&hdr[6]);
    s_blob.count      = rd_u32_le(&hdr[8]);
    s_blob.entry_size = static_cast<uint32_t>(s_blob.pixel_size) * s_blob.pixel_size * 4u;
    s_blob.seq_count  = (version >= 2) ? rd_u32_le(&hdr[12]) : 0;

    if (s_blob.count == 0 || s_blob.entry_size == 0) {
        lv_fs_close(&s_blob.file);
        return false;
    }

    // Read codepoint index into PSRAM
    uint32_t idx_bytes = s_blob.count * 4u;
    s_blob.codepoints = static_cast<uint32_t *>(
        heap_caps_malloc(idx_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!s_blob.codepoints) {
        lv_fs_close(&s_blob.file);
        return false;
    }

    if (lv_fs_read(&s_blob.file, s_blob.codepoints, idx_bytes, &br) != LV_FS_RES_OK ||
        br != idx_bytes) {
        heap_caps_free(s_blob.codepoints);
        s_blob.codepoints = nullptr;
        lv_fs_close(&s_blob.file);
        return false;
    }

    s_blob.data_off = 16u + idx_bytes;

    // Sequence-table failure only disables compose/decompose; glyphs still work.
    if (s_blob.seq_count > 0 && !seq_table_load())
        s_blob.seq_count = 0;

    s_blob.is_sd = is_sd;
    s_blob.open  = true;
    return true;
}

// Called once on first emoji access. Prefers the (bigger) SD-resident blob
// when a card is mounted, falls back to the LittleFS set. Like the original
// single-path init this is attempted once — no retry after failure.
bool blob_init_once()
{
    if (s_blob.open)   return true;
    if (s_blob.failed) return false;

    if (sd_mounted && blob_try_open(EMOJI_BLOB_PATH_SD, true)) return true;
    if (blob_try_open(EMOJI_BLOB_PATH, false)) return true;

    s_blob.failed = true;
    return false;
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

    // Allocate before taking the SPI lock — never hold the bus over a malloc.
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
    bool read_ok;
    {
        SdLock lock(s_blob.is_sd);
        read_ok = lv_fs_seek(&s_blob.file, offset, LV_FS_SEEK_SET) == LV_FS_RES_OK &&
                  lv_fs_read(&s_blob.file, pixels, s_blob.entry_size, &br) == LV_FS_RES_OK &&
                  br == s_blob.entry_size;
    }
    if (!read_ok) {
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

    // Cache full: don't load a glyph we can't keep — pre-fix, the fresh glyph
    // was returned uncached and orphaned PER DRAW once 256 distinct emoji had
    // been seen (the audit's one real leak). Blank this frame, recycle the
    // cache at the next safe point, render normally from the next frame on.
    if (s_cache_count >= EMOJI_CACHE_CAP) {
        schedule_cache_clear();
        return &s_blank_dsc;
    }

    auto * fresh = load_emoji_from_blob(unicode);
    // In the index but failed to load (OOM/decode) — cache the blank so we
    // never retry this codepoint every frame.
    if (!fresh) {
        cache_insert(unicode, &s_blank_dsc);
        return &s_blank_dsc;
    }

    if (!cache_insert(unicode, fresh)) {
        // Unreachable with the cap guard above, but never orphan a glyph:
        // `fresh` was never handed to LVGL, so freeing it here is safe.
        heap_caps_free(const_cast<void *>(static_cast<const void *>(fresh->data)));
        heap_caps_free(fresh);
        schedule_cache_clear();
        return &s_blank_dsc;
    }
    return fresh;
}

// ---- UTF-8 helpers (for compose/decompose) ---------------------------------

// Decode one codepoint at p (NUL-terminated buffer). Returns bytes consumed
// and sets *cp; returns 0 on malformed input (caller copies one raw byte to
// stay lossless). The continuation-byte check makes a NUL terminator fail
// cleanly, so decoding never runs past the end of the string.
uint32_t utf8_decode(const uint8_t * p, uint32_t * cp)
{
    uint8_t b = p[0];
    if (b < 0x80) { *cp = b; return 1; }
    uint32_t len, min;
    if      ((b & 0xE0) == 0xC0) { *cp = b & 0x1Fu; len = 2; min = 0x80u; }
    else if ((b & 0xF0) == 0xE0) { *cp = b & 0x0Fu; len = 3; min = 0x800u; }
    else if ((b & 0xF8) == 0xF0) { *cp = b & 0x07u; len = 4; min = 0x10000u; }
    else return 0;
    for (uint32_t i = 1; i < len; i++) {
        if ((p[i] & 0xC0) != 0x80) return 0;
        *cp = (*cp << 6) | (p[i] & 0x3Fu);
    }
    if (*cp < min) return 0;   // overlong encoding
    return len;
}

// Encode cp as UTF-8 at out (no NUL). Returns bytes written (1..4).
uint32_t utf8_encode(char * out, uint32_t cp)
{
    if (cp < 0x80u)    { out[0] = static_cast<char>(cp); return 1; }
    if (cp < 0x800u)   {
        out[0] = static_cast<char>(0xC0 | (cp >> 6));
        out[1] = static_cast<char>(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000u) {
        out[0] = static_cast<char>(0xE0 | (cp >> 12));
        out[1] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out[2] = static_cast<char>(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = static_cast<char>(0xF0 | (cp >> 18));
    out[1] = static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
    out[2] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out[3] = static_cast<char>(0x80 | (cp & 0x3F));
    return 4;
}

// ---- Sequence matching ------------------------------------------------------

// First s_seq index whose lead codepoint is >= cp (table sorted by cps[0] asc).
int32_t seq_lower_bound(uint32_t cp)
{
    int32_t lo = 0;
    int32_t hi = static_cast<int32_t>(s_blob.seq_count);
    while (lo < hi) {
        int32_t mid = lo + (hi - lo) / 2;
        if (s_seq[mid].cps[0] < cp) lo = mid + 1;
        else                        hi = mid;
    }
    return lo;
}

// Try to match sequence e against the input at p. FE0F-insensitive on BOTH
// sides: the Noto filenames this blob is built from omit U+FE0F, but wire
// text usually includes it (fully-qualified RGI form), so the matcher skips
// it wherever it appears between the real codepoints. Returns bytes consumed
// on success, 0 on no-match.
size_t seq_match(const SeqEnt * e, const uint8_t * p)
{
    const uint8_t * q = p;
    for (uint32_t j = 0; j < e->len; j++) {
        uint32_t ecp = e->cps[j];
        if (ecp == 0xFE0Fu) continue;              // stored-side skip
        uint32_t cp;
        uint32_t n = utf8_decode(q, &cp);
        if (!n) return 0;
        while (cp == 0xFE0Fu) {                    // input-side skip
            q += n;
            n = utf8_decode(q, &cp);
            if (!n) return 0;
        }
        if (cp != ecp) return 0;
        q += n;
    }
    return static_cast<size_t>(q - p);
}

} // anonymous namespace

extern "C" bool emoji_preload(uint32_t codepoint)
{
    cache_init_once();
    if (auto *cached = cache_lookup(codepoint))
        return cached != &s_blank_dsc;
    if (s_cache_count >= EMOJI_CACHE_CAP) {   // full: same recycle as the render path
        schedule_cache_clear();
        return false;
    }
    auto *fresh = load_emoji_from_blob(codepoint);
    if (!fresh) {
        cache_insert(codepoint, &s_blank_dsc);
        return false;
    }
    if (!cache_insert(codepoint, fresh)) {
        heap_caps_free(const_cast<void *>(static_cast<const void *>(fresh->data)));
        heap_caps_free(fresh);
        schedule_cache_clear();
        return false;
    }
    return true;
}

extern "C" char * emoji_compose(const char * in)
{
    if (!in || !in[0]) return nullptr;
    if (!blob_init_once() || s_blob.seq_count == 0 || !s_seq) return nullptr;

    size_t in_len = strlen(in);
    // Output never grows: a PUA emit is 3 bytes and only ever replaces a span
    // of >= 2 codepoints (>= 5 bytes); everything else copies through 1:1.
    char * out = static_cast<char *>(
        heap_caps_malloc(in_len + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!out) return nullptr;

    const uint8_t * p = reinterpret_cast<const uint8_t *>(in);
    char * w = out;
    bool changed = false;

    while (*p) {
        uint32_t cp;
        uint32_t n = utf8_decode(p, &cp);
        if (!n) { *w++ = static_cast<char>(*p++); continue; }   // raw byte, lossless

        const SeqEnt * hit = nullptr;
        size_t consumed = 0;
        for (int32_t i = seq_lower_bound(cp);
             i < static_cast<int32_t>(s_blob.seq_count) && s_seq[i].cps[0] == cp; i++) {
            consumed = seq_match(&s_seq[i], p);
            if (consumed) { hit = &s_seq[i]; break; }   // len-desc order → longest first
        }

        if (hit) {
            w += utf8_encode(w, hit->pua);
            p += consumed;
            changed = true;
        } else {
            memcpy(w, p, n);
            w += n;
            p += n;
        }
    }
    *w = '\0';

    if (!changed) { heap_caps_free(out); return nullptr; }
    return out;
}

extern "C" char * emoji_decompose(const char * in)
{
    if (!in || !in[0]) return nullptr;
    if (!blob_init_once() || s_blob.seq_count == 0 || !s_seq || !s_pua_to_seq) return nullptr;

    // Pass 1: exact output size (PUA expansion can grow the string a lot).
    const uint8_t * p = reinterpret_cast<const uint8_t *>(in);
    size_t out_len = 0;
    bool changed = false;
    char tmp[4];
    while (*p) {
        uint32_t cp;
        uint32_t n = utf8_decode(p, &cp);
        if (!n) { out_len++; p++; continue; }
        if (cp >= EMOJI_PUA_BASE && cp < EMOJI_PUA_BASE + s_blob.seq_count) {
            const SeqEnt * e = &s_seq[s_pua_to_seq[cp - EMOJI_PUA_BASE]];
            for (uint32_t j = 0; j < e->len; j++) out_len += utf8_encode(tmp, e->cps[j]);
            changed = true;
        } else {
            out_len += n;
        }
        p += n;
    }
    if (!changed) return nullptr;

    char * out = static_cast<char *>(
        heap_caps_malloc(out_len + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!out) return nullptr;

    // Pass 2: emit.
    p = reinterpret_cast<const uint8_t *>(in);
    char * w = out;
    while (*p) {
        uint32_t cp;
        uint32_t n = utf8_decode(p, &cp);
        if (!n) { *w++ = static_cast<char>(*p++); continue; }
        if (cp >= EMOJI_PUA_BASE && cp < EMOJI_PUA_BASE + s_blob.seq_count) {
            const SeqEnt * e = &s_seq[s_pua_to_seq[cp - EMOJI_PUA_BASE]];
            for (uint32_t j = 0; j < e->len; j++) w += utf8_encode(w, e->cps[j]);
        } else {
            memcpy(w, p, n);
            w += n;
        }
        p += n;
    }
    *w = '\0';
    return out;
}

extern "C" uint32_t emoji_blob_count(void)
{
    return blob_init_once() ? s_blob.count : 0;
}

extern "C" uint32_t emoji_font_reload(bool close_only)
{
    // Swap blobs at runtime (e.g. right after the extended set finishes
    // downloading to SD). Safe from Lua/timer context — the same context the
    // cache-recycle timer uses: glyph free + decoder drop + invalidate, then
    // full blob state reset so the next access re-runs blob_init_once and
    // picks up the preferred (SD-first) file.
    //
    // close_only: release the file handle + state WITHOUT re-opening, so the
    // caller can remove/rename the on-disk blob first (FatFS misbehaves when
    // an open file is unlinked); call again with false to re-init.
    emoji_font_cache_clear();
    lv_image_cache_drop(NULL);

    if (s_blob.open) {
        SdLock lock(s_blob.is_sd);
        lv_fs_close(&s_blob.file);
    }
    if (s_blob.codepoints) { heap_caps_free(s_blob.codepoints); s_blob.codepoints = nullptr; }
    if (s_seq)             { heap_caps_free(s_seq);             s_seq = nullptr; }
    if (s_pua_to_seq)      { heap_caps_free(s_pua_to_seq);      s_pua_to_seq = nullptr; }
    s_blob.open      = false;
    s_blob.failed    = false;
    s_blob.count     = 0;
    s_blob.seq_count = 0;
    s_blob.is_sd     = false;

    if (close_only) return 0;

    lv_obj_invalidate(lv_screen_active());
    return emoji_blob_count();   // re-inits immediately; 0 = no blob found
}

extern "C" uint32_t emoji_blob_cp_at(uint32_t i)
{
    if (!blob_init_once() || i >= s_blob.count) return 0;
    return s_blob.codepoints[i];
}

extern "C" void emoji_font_cache_clear()
{
    if (!s_cache_inited) return;
    for (auto & e : s_cache) {
        // Free heap-backed glyphs only; s_blank_dsc and its pixel are static.
        if (e.dsc && e.dsc != &s_blank_dsc) {
            heap_caps_free(const_cast<void *>(static_cast<const void *>(e.dsc->data)));
            heap_caps_free(e.dsc);
        }
        e.codepoint = EMOJI_CACHE_EMPTY;
        e.dsc = nullptr;
    }
    s_cache_count = 0;
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
