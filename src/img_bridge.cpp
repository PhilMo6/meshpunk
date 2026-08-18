// In-memory image buffers: Lua -> C++.
//
// Loads a PNG, a baseline JPEG or an LVGL .bin into ONE app-owned PSRAM RGB565
// buffer and hands Lua a light-userdata lv_image_dsc_t* that works anywhere an
// image source is accepted (img:set_src(dsc), canvas:draw_image{ src = dsc }).
// LVGL draws it through the "use directly" path (lv_bin_decoder), so nothing
// lands in the image cache — which is the whole point: LV_CACHE_DEF_SIZE
// (512KB) silently FAILS the decode of anything larger (lv_cache.c:
// RESERVE_COND_TOO_LARGE -> lv_lodepng.c returns LV_RESULT_INVALID), so a plain
// Image widget pointed at a 640x480 PNG renders nothing at all. Same trick as
// the map's tile pool (_tile_show in main.cpp), generalized to any size, any
// format and any drive.
//
//   _img_info(path)           -> w, h, kind("png"|"jpg"|"bin") | nil, err
//   _img_open(path [, opts])  -> dsc, w, h, div | nil, err
//        opts.div         1|2|4|8 decimation; nil/0 = auto (smallest that fits)
//        opts.max_bytes   output cap that auto-div respects (default 2MB)
//        opts.max_pixels  source-size refusal threshold (default 1.2M for
//                         PNG/.bin, effectively unlimited for JPEG)
//        opts.bg          0xRRGGBB composited under PNG alpha (default black)
//   _img_scale(dsc, w, h)     -> dsc, w, h | nil, err   (box-average resample)
//   _img_close([dsc])         -- one buffer, or every buffer when omitted
//
// Any number of images may be open at once. A handle stays valid until it is
// closed; the caller MUST stop every widget drawing from it first (set another
// src, or hide/delete the widget) — exactly the rule the tile pool documents.
//
// MEMORY POLICY: everything here is PSRAM and transient. Each open is ONE
// heap_caps_malloc(MALLOC_CAP_SPIRAM) holding descriptor + pixels, released
// whole by _img_close. Internal SRAM holds a single 4-byte list head and
// nothing else — no static descriptor table, no pools, no lazily-kept scratch,
// because .bss is internal DRAM and that pool belongs to BLE/WiFi/DMA.
//
// The three decoders have very different appetites:
//
//   PNG   lodepng is all-or-nothing: ~w*h*4 (ARGB, contiguous) + ~w*h*3 of
//         filtered scanlines must be live at once, so the practical ceiling is
//         free PSRAM / 7 — roughly 800x680 with 4MB free. Decimation does NOT
//         help; the decode is the wall.
//   JPEG  TJpgDec streams MCU by MCU and descales as it goes (JD_USE_SCALE,
//         see docs/LVGL_LOCAL_PATCHES.md #6), so only the compressed file and
//         the final buffer are ever live. A 12MP photo opens at 1/8.
//   .bin  read straight through (tightly packed RGB565 only).
//
// Decoding is synchronous and blocks the LVGL thread (~1s for a megapixel PNG,
// seconds for a large JPEG off SD) — neither decoder has an incremental entry
// point. Each open logs dimensions, scale, elapsed ms and PSRAM state so a slow
// or failing image explains itself on the serial log without a debug build.

#include "img_bridge.h"
#include "meshpunk_fs.h"
#include "meshpunk_sync.h"   // SLog

#include <Arduino.h>
#include <lvgl.h>
#include <esp_heap_caps.h>
#include <string.h>

// Driven directly (jd_prepare/jd_decomp), not through LVGL's lv_tjpgd decoder:
// that one re-decodes from the file on every redraw, which panning cannot use.
#include "../lib/lvgl/src/libs/tjpgd/tjpgd.h"

extern "C" {
#include <lua.h>
#include <lauxlib.h>
}

// LVGL's *patched* lodepng: this returns an lv_draw_buf_t* (ARGB8888, pixels
// in ->data as R,G,B,A), NOT a raw pixel buffer, and must be released with
// lv_draw_buf_destroy — lv_free would leak the pixel block. Same declaration
// main.cpp uses for the map's png->bin converter.
extern "C" unsigned lodepng_decode32(unsigned char** out, unsigned* w, unsigned* h,
                                     const unsigned char* in, size_t insize);

// ── Image blocks ────────────────────────────────────────────────────────────
// ONE PSRAM allocation per open: the descriptor Lua receives, a registry link,
// then the pixels. Internal SRAM holds exactly one pointer (s_blocks) — there
// is deliberately no static descriptor table, because a `static` array lands in
// .bss, i.e. internal DRAM, which is the scarce pool (BLE/WiFi/DMA) and must
// not be spent on an idle feature.
//
// The descriptor is FIRST in the block, so the light userdata Lua holds is both
// the lv_image_dsc_t* LVGL wants and the address we free.

#define IMG_BLOCK_HDR 64   // pixels start here: keeps them on the allocator's alignment

struct ImgBlock {
    lv_image_dsc_t dsc;    // MUST stay first
    ImgBlock*      next;   // registry link, for the close-everything sweep
};
static_assert(sizeof(ImgBlock) <= IMG_BLOCK_HDR, "ImgBlock header outgrew IMG_BLOCK_HDR");

// The bridge's ONLY permanent state: 4 bytes of .bss.
static ImgBlock* s_blocks = nullptr;

static uint16_t* img_pixels(ImgBlock* b) {
    return (uint16_t*)((uint8_t*)b + IMG_BLOCK_HDR);
}

// Allocate + register a block sized for w*h RGB565 pixels. Null on OOM.
static ImgBlock* img_alloc(uint32_t w, uint32_t h) {
    uint32_t data_size = w * h * 2;
    uint8_t* mem = (uint8_t*)heap_caps_malloc(IMG_BLOCK_HDR + data_size,
                                              MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!mem) return nullptr;

    ImgBlock* b = (ImgBlock*)mem;
    memset(b, 0, sizeof(*b));
    b->dsc.header.magic  = LV_IMAGE_HEADER_MAGIC;
    b->dsc.header.cf     = LV_COLOR_FORMAT_RGB565;
    b->dsc.header.w      = w;
    b->dsc.header.h      = h;
    b->dsc.header.stride = w * 2;
    b->dsc.data          = mem + IMG_BLOCK_HDR;
    b->dsc.data_size     = data_size;

    b->next  = s_blocks;
    s_blocks = b;
    return b;
}

// Unlink and free one block. Drops the decoder/header cache entry keyed on this
// address FIRST: the allocator recycles addresses, and a stale entry would then
// serve the previous image (or dangle at freed pixels).
static bool img_free(const void* p) {
    for (ImgBlock** link = &s_blocks; *link; link = &(*link)->next) {
        if ((const void*)*link == p) {
            ImgBlock* b = *link;
            *link = b->next;
            lv_image_cache_drop(&b->dsc);
            heap_caps_free(b);
            return true;
        }
    }
    return false;
}

static void img_free_all() {
    while (s_blocks) img_free(s_blocks);
}

static int push_block(lua_State* L, ImgBlock* b) {
    lua_pushlightuserdata(L, &b->dsc);
    lua_pushinteger(L, b->dsc.header.w);
    lua_pushinteger(L, b->dsc.header.h);
    return 3;
}

static int push_err(lua_State* L, const char* msg) {
    lua_pushnil(L);
    lua_pushstring(L, msg);
    return 2;
}

// ── Source probing ──────────────────────────────────────────────────────────

enum ImgKind : uint8_t { IMG_NONE = 0, IMG_PNG, IMG_JPG, IMG_BIN };

static uint32_t be32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

// Header-only sniff: PNG (IHDR w/h at bytes 16..23), JPEG (SOI + any marker —
// covers JFIF and EXIF alike; its dimensions live behind a marker walk, so
// they come from jd_prepare later) or a tightly packed RGB565 LVGL .bin.
// Cheap — opens, reads 24 bytes, closes.
static ImgKind img_probe(const char* path, uint32_t* w, uint32_t* h) {
    static const uint8_t PNG_MAGIC[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };

    *w = 0;
    *h = 0;

    uint8_t head[24];
    memset(head, 0, sizeof(head));
    MeshpunkFile mf = meshpunk_open(path, "r", false);
    if (!mf.valid) return IMG_NONE;
    int rd = mf.file.read(head, sizeof(head));
    meshpunk_close(mf);
    if (rd < (int)sizeof(lv_image_header_t)) return IMG_NONE;

    if (rd >= (int)sizeof(head) && memcmp(head, PNG_MAGIC, sizeof(PNG_MAGIC)) == 0) {
        *w = be32(head + 16);
        *h = be32(head + 20);
        return (*w && *h && *w <= 8192 && *h <= 8192) ? IMG_PNG : IMG_NONE;
    }

    if (head[0] == 0xFF && head[1] == 0xD8 && head[2] == 0xFF) return IMG_JPG;

    lv_image_header_t hdr;
    memcpy(&hdr, head, sizeof(hdr));
    if (hdr.magic == LV_IMAGE_HEADER_MAGIC && hdr.cf == LV_COLOR_FORMAT_RGB565
        && hdr.w && hdr.h && hdr.stride == (uint32_t)hdr.w * 2) {
        *w = hdr.w;
        *h = hdr.h;
        return IMG_BIN;
    }
    return IMG_NONE;
}

// ── Pixel conversion ────────────────────────────────────────────────────────

// ARGB8888 (byte order R,G,B,A) -> native little-endian RGB565, box-averaging
// each div x div block and compositing alpha over `bg`. Native LE is correct
// even with LV_COLOR_16_SWAP=1: v9 swaps the whole framebuffer once at flush,
// so image data itself stays little-endian.
static void argb_to_565(const uint8_t* src, uint32_t sstride,
                        uint16_t* dst, uint32_t dw, uint32_t dh,
                        uint32_t div, uint32_t bg) {
    const uint32_t bg_r = (bg >> 16) & 0xFF;
    const uint32_t bg_g = (bg >> 8) & 0xFF;
    const uint32_t bg_b = bg & 0xFF;
    const uint32_t n = div * div;

    for (uint32_t y = 0; y < dh; y++) {
        for (uint32_t x = 0; x < dw; x++) {
            uint32_t r = 0, g = 0, b = 0;
            for (uint32_t sy = 0; sy < div; sy++) {
                const uint8_t* p = src + (size_t)(y * div + sy) * sstride + (size_t)(x * div) * 4;
                for (uint32_t sx = 0; sx < div; sx++, p += 4) {
                    uint32_t a = p[3];
                    r += (p[0] * a + bg_r * (255 - a)) / 255;
                    g += (p[1] * a + bg_g * (255 - a)) / 255;
                    b += (p[2] * a + bg_b * (255 - a)) / 255;
                }
            }
            r /= n; g /= n; b /= n;
            dst[(size_t)y * dw + x] = (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
        }
    }
}

// RGB565 -> RGB565 at an arbitrary target size, averaging each source box.
// Upscaling degenerates to nearest-neighbour (the box collapses to one pixel).
static void resample565(const uint16_t* src, uint32_t sw, uint32_t sh,
                        uint16_t* dst, uint32_t dw, uint32_t dh) {
    for (uint32_t y = 0; y < dh; y++) {
        uint32_t y0 = y * sh / dh;
        uint32_t y1 = (y + 1) * sh / dh;
        if (y1 <= y0) y1 = y0 + 1;
        if (y1 > sh) y1 = sh;
        for (uint32_t x = 0; x < dw; x++) {
            uint32_t x0 = x * sw / dw;
            uint32_t x1 = (x + 1) * sw / dw;
            if (x1 <= x0) x1 = x0 + 1;
            if (x1 > sw) x1 = sw;
            uint32_t r = 0, g = 0, b = 0, n = 0;
            for (uint32_t sy = y0; sy < y1; sy++) {
                const uint16_t* p = src + (size_t)sy * sw + x0;
                for (uint32_t sx = x0; sx < x1; sx++, p++) {
                    uint16_t v = *p;
                    r += (v >> 11) & 0x1F;
                    g += (v >> 5) & 0x3F;
                    b += v & 0x1F;
                    n++;
                }
            }
            dst[(size_t)y * dw + x] = (uint16_t)(((r / n) << 11) | ((g / n) << 5) | (b / n));
        }
    }
}

// ── JPEG (TJpgDec, driven directly) ─────────────────────────────────────────
// The whole compressed file is read into PSRAM first and fed to the decoder
// from memory. Streaming from the file handle instead would hold the SD SPI
// lock for the entire multi-second decode, starving the LoRa radio that shares
// the bus; a JPEG is small next to the image it expands to, so buffering it is
// the cheaper trade.

#define JPEG_POOL 8192   // TJpgDec work area: input buffer + huffman/quant tables + MCU

struct JpegCtx {
    const uint8_t* data;      // input: whole file
    uint32_t       size;
    uint32_t       pos;
    uint16_t*      out;       // output: our RGB565 buffer (null while probing)
    uint32_t       ow, oh;
};

struct JpegJob {
    void*   file;
    JDEC*   jd;
    void*   pool;
    JpegCtx ctx;
};

static size_t jpeg_in(JDEC* jd, uint8_t* buff, size_t ndata) {
    JpegCtx* c = (JpegCtx*)jd->device;
    size_t left = c->size - c->pos;              // pos never passes size
    size_t n = (ndata <= left) ? ndata : left;
    if (buff) memcpy(buff, c->data + c->pos, n);   // buff == NULL means "skip"
    c->pos += n;
    return n;
}

// One decoded MCU block in ALREADY-DESCALED coordinates (jd_mcu_output shifts
// the rect by jd->scale before calling). Clipped against the buffer: per-MCU
// truncation can make the decoder's last column/row land short of the nominal
// w>>scale.
//
// BYTE ORDER: the block is B,G,R — LVGL's RGB888 memory layout, not stock
// TJpgDec's R,G,B (see the labelled writes in tjpgd.c jd_mcu_output, and
// lv_tjpgd.c reporting LV_COLOR_FORMAT_RGB888). Reading it as R,G,B swaps red
// and blue, which reads on screen as a blue cast over everything.
static int jpeg_out(JDEC* jd, void* bitmap, JRECT* rect) {
    JpegCtx* c = (JpegCtx*)jd->device;
    const uint8_t* src = (const uint8_t*)bitmap;
    const uint32_t rw = (uint32_t)(rect->right - rect->left + 1);

    for (uint32_t y = rect->top; y <= rect->bottom; y++) {
        if (y >= c->oh) break;
        const uint8_t* row = src + (size_t)(y - rect->top) * rw * 3;
        uint16_t* dst = c->out + (size_t)y * c->ow;
        for (uint32_t x = rect->left; x <= rect->right; x++) {
            if (x >= c->ow) break;
            const uint8_t* p = row + (size_t)(x - rect->left) * 3;   // B, G, R
            dst[x] = (uint16_t)(((p[2] & 0xF8) << 8) | ((p[1] & 0xFC) << 3) | (p[0] >> 3));
        }
    }
    return 1;
}

static const char* jpeg_err(JRESULT rc) {
    switch (rc) {
        case JDR_INTR: return "JPEG decode interrupted";
        case JDR_INP:  return "JPEG is truncated or unreadable";
        case JDR_MEM1: return "JPEG needs a bigger work pool";
        case JDR_MEM2: return "JPEG input buffer too small";
        case JDR_PAR:  return "bad JPEG decoder parameter";
        case JDR_FMT1: return "corrupt JPEG data";
        case JDR_FMT2: return "unsupported JPEG variant";
        case JDR_FMT3: return "progressive JPEG is not supported";
        default:       return "JPEG decode failed";
    }
}

static void jpeg_end(JpegJob* j) {
    if (j->file) heap_caps_free(j->file);
    if (j->jd)   heap_caps_free(j->jd);
    if (j->pool) heap_caps_free(j->pool);
    j->file = nullptr;
    j->jd   = nullptr;
    j->pool = nullptr;
}

// Read the file and parse its headers. On success jd->width/height are known
// and the job is ready for jd_decomp; the caller always ends with jpeg_end().
static const char* jpeg_begin(const char* path, JpegJob* j) {
    memset(j, 0, sizeof(*j));

    uint32_t fsize = 0;
    j->file = meshpunk_read_all(path, &fsize, false);
    if (!j->file) return "read failed";

    j->jd   = (JDEC*)heap_caps_malloc(sizeof(JDEC), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    j->pool = heap_caps_malloc(JPEG_POOL, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!j->jd || !j->pool) { jpeg_end(j); return "out of memory"; }

    j->ctx.data = (const uint8_t*)j->file;
    j->ctx.size = fsize;
    j->ctx.pos  = 0;

    JRESULT rc = jd_prepare(j->jd, jpeg_in, j->pool, JPEG_POOL, &j->ctx);
    if (rc != JDR_OK) {
        const char* msg = jpeg_err(rc);
        jpeg_end(j);
        return msg;
    }
    return nullptr;
}

// ── Bindings ────────────────────────────────────────────────────────────────

static uint32_t opt_u32(lua_State* L, int idx, const char* key, uint32_t def) {
    if (!lua_istable(L, idx)) return def;
    lua_getfield(L, idx, key);
    uint32_t v = lua_isnil(L, -1) ? def : (uint32_t)lua_tonumber(L, -1);
    lua_pop(L, 1);
    return v;
}

static uint32_t psram_free() { return (uint32_t)heap_caps_get_free_size(MALLOC_CAP_SPIRAM); }
static uint32_t psram_largest() { return (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM); }

static const char* UNSUPPORTED = "not a PNG, JPEG or RGB565 .bin";

// The JPEG half of _img_open. Split out because its order of operations is
// inverted: the file must be read and parsed before the dimensions — and so the
// output size and memory demand — are known at all.
static int open_jpeg(lua_State* L, const char* path,
                     uint32_t req_div, uint32_t max_bytes, uint32_t max_px, uint32_t bg) {
    uint32_t t0 = millis();
    JpegJob job;
    const char* err = jpeg_begin(path, &job);
    if (err) return push_err(L, err);

    uint32_t w = job.jd->width, h = job.jd->height;
    if ((uint64_t)w * h > max_px) {
        jpeg_end(&job);
        char msg[64];
        snprintf(msg, sizeof(msg), "%ux%u is over the %u pixel limit",
                 (unsigned)w, (unsigned)h, (unsigned)max_px);
        return push_err(L, msg);
    }

    // TJpgDec descales by 2^shift while decoding (shift 0..3 = 1/1 .. 1/8), so
    // an oversized photo never exists at full resolution in RAM.
    uint32_t shift = 0;
    if (req_div) {
        shift = (req_div >= 8) ? 3 : (req_div >= 4) ? 2 : (req_div >= 2) ? 1 : 0;
    } else {
        while (shift < 3 && (uint64_t)(w >> shift) * (h >> shift) * 2 > max_bytes) shift++;
        if ((uint64_t)(w >> shift) * (h >> shift) * 2 > max_bytes) {
            jpeg_end(&job);
            return push_err(L, "image exceeds max_bytes even at 1/8 scale");
        }
    }
    uint32_t ow = w >> shift, oh = h >> shift;
    if (!ow) ow = 1;
    if (!oh) oh = 1;
    uint32_t out_bytes = ow * oh * 2;

    ImgBlock* block = img_alloc(ow, oh);
    if (!block) {
        jpeg_end(&job);
        char msg[64];
        snprintf(msg, sizeof(msg), "no room for %ux%u (%uKB)",
                 (unsigned)ow, (unsigned)oh, (unsigned)(out_bytes >> 10));
        return push_err(L, msg);
    }
    uint16_t* out = img_pixels(block);

    // Pre-fill with the matte: per-MCU truncation can leave the last column or
    // row of a non-multiple-of-16 image unwritten.
    uint16_t bg565 = (uint16_t)((((bg >> 16) & 0xF8) << 8) | (((bg >> 8) & 0xFC) << 3)
                                | ((bg & 0xFF) >> 3));
    for (uint32_t i = 0; i < ow * oh; i++) out[i] = bg565;

    job.ctx.out = out;
    job.ctx.ow  = ow;
    job.ctx.oh  = oh;

    JRESULT rc = jd_decomp(job.jd, jpeg_out, (uint8_t)shift);
    jpeg_end(&job);
    if (rc != JDR_OK) {
        img_free(&block->dsc);
        return push_err(L, jpeg_err(rc));
    }

    SLog.printf("[img] %s %ux%u -> %ux%u scale=1/%u %uKB in %ums; psram free=%uKB largest=%uKB\n",
                path, (unsigned)w, (unsigned)h, (unsigned)ow, (unsigned)oh,
                (unsigned)(1u << shift), (unsigned)(out_bytes >> 10), (unsigned)(millis() - t0),
                (unsigned)(psram_free() >> 10), (unsigned)(psram_largest() >> 10));

    push_block(L, block);
    lua_pushinteger(L, 1u << shift);
    return 4;
}

// _img_info(path) -> w, h, "png"|"jpg"|"bin" | nil, err
static int lua_img_info(lua_State* L) {
    const char* path = luaL_checkstring(L, 1);
    uint32_t w = 0, h = 0;
    ImgKind kind = img_probe(path, &w, &h);
    if (kind == IMG_NONE) return push_err(L, UNSUPPORTED);

    if (kind == IMG_JPG) {
        // JPEG dimensions sit behind a marker walk, so this costs a full read
        // plus a header parse — cheap enough for an info call, but do not put
        // it in a per-row listing loop.
        JpegJob job;
        const char* err = jpeg_begin(path, &job);
        if (err) return push_err(L, err);
        w = job.jd->width;
        h = job.jd->height;
        jpeg_end(&job);
    }

    lua_pushinteger(L, w);
    lua_pushinteger(L, h);
    lua_pushstring(L, kind == IMG_PNG ? "png" : (kind == IMG_JPG ? "jpg" : "bin"));
    return 3;
}

// _img_open(path [, opts]) -> dsc, w, h, div | nil, err
static int lua_img_open(lua_State* L) {
    const char* path = luaL_checkstring(L, 1);
    uint32_t req_div   = opt_u32(L, 2, "div", 0);
    uint32_t max_bytes = opt_u32(L, 2, "max_bytes", 2u * 1024 * 1024);
    uint32_t max_px    = opt_u32(L, 2, "max_pixels", 0);   // 0 = per-format default
    uint32_t bg        = opt_u32(L, 2, "bg", 0);

    uint32_t w = 0, h = 0;
    ImgKind kind = img_probe(path, &w, &h);
    if (kind == IMG_NONE) return push_err(L, UNSUPPORTED);
    if (kind == IMG_JPG) {
        // A JPEG's source size costs nothing — TJpgDec streams it and descales
        // on the way out, so only the OUTPUT (max_bytes) needs bounding. The
        // cap here is pure sanity, not a memory limit.
        return open_jpeg(L, path, req_div, max_bytes,
                         max_px ? max_px : 64000000u, bg);
    }
    // PNG/.bin decode whole-image, so the source size IS the memory demand.
    if (!max_px) max_px = 1200000;
    if ((uint64_t)w * h > max_px) {
        char msg[64];
        snprintf(msg, sizeof(msg), "%ux%u is over the %u pixel limit",
                 (unsigned)w, (unsigned)h, (unsigned)max_px);
        return push_err(L, msg);
    }

    // Decimation: honour an explicit divisor, else pick the smallest power of
    // two whose output fits max_bytes.
    uint32_t div = 0;
    if (req_div) {
        div = (req_div >= 8) ? 8 : (req_div >= 4) ? 4 : (req_div >= 2) ? 2 : 1;
    } else {
        for (uint32_t d = 1; d <= 8; d <<= 1) {
            if ((uint64_t)(w / d) * (h / d) * 2 <= max_bytes) { div = d; break; }
        }
        if (!div) return push_err(L, "image exceeds max_bytes even at 1/8 scale");
    }
    uint32_t ow = w / div, oh = h / div;
    if (!ow) ow = 1;
    if (!oh) oh = 1;
    uint32_t out_bytes = ow * oh * 2;

    // Budget. A PNG decode transiently needs the ARGB8888 image (w*h*4,
    // contiguous) plus lodepng's filtered scanlines (~w*h*3) plus the file
    // itself; a .bin only needs the file. Reclaim the image cache once before
    // giving up — this runs on the LVGL thread, so dropping it is safe.
    // The .bin path reads the whole file (w*h*2) before packing the output, so
    // that read — not the smaller output — is its contiguous demand.
    uint64_t contig = (kind == IMG_PNG) ? (uint64_t)w * h * 4 + 65536 : (uint64_t)w * h * 2;
    uint64_t total  = (kind == IMG_PNG) ? (uint64_t)w * h * 7 + out_bytes + (512u << 10)
                                        : (uint64_t)w * h * 2 + out_bytes + (128u << 10);
    if (psram_largest() < contig || psram_free() < total) {
        lv_image_cache_drop(NULL);
    }
    if (psram_largest() < contig || psram_free() < total) {
        char msg[96];
        snprintf(msg, sizeof(msg), "needs %uKB free / %uKB contiguous, have %uKB / %uKB",
                 (unsigned)(total >> 10), (unsigned)(contig >> 10),
                 (unsigned)(psram_free() >> 10), (unsigned)(psram_largest() >> 10));
        return push_err(L, msg);
    }

    ImgBlock* block = img_alloc(ow, oh);
    if (!block) return push_err(L, "out of memory");
    uint16_t* out = img_pixels(block);

    uint32_t t0 = millis();
    uint32_t fsize = 0;
    void* data = meshpunk_read_all(path, &fsize, false);
    if (!data) {
        img_free(&block->dsc);
        return push_err(L, "read failed");
    }

    if (kind == IMG_PNG) {
        lv_draw_buf_t* decoded = nullptr;
        unsigned dw = 0, dh = 0;
        unsigned err = lodepng_decode32((unsigned char**)&decoded, &dw, &dh,
                                        (const unsigned char*)data, fsize);
        heap_caps_free(data);
        if (err || !decoded || !decoded->data || dw != w || dh != h) {
            SLog.printf("[img] decode FAIL %s err=%u psram_free=%uKB largest=%uKB\n",
                        path, err, (unsigned)(psram_free() >> 10), (unsigned)(psram_largest() >> 10));
            if (decoded) lv_draw_buf_destroy(decoded);
            img_free(&block->dsc);
            char msg[48];
            snprintf(msg, sizeof(msg), "PNG decode failed (err %u)", err);
            return push_err(L, msg);
        }
        argb_to_565((const uint8_t*)decoded->data, decoded->header.stride,
                    out, ow, oh, div, bg);
        lv_draw_buf_destroy(decoded);
    } else {
        // .bin: 12-byte header then tightly packed native-LE RGB565.
        if (fsize < sizeof(lv_image_header_t) + (uint64_t)w * h * 2) {
            heap_caps_free(data);
            img_free(&block->dsc);
            return push_err(L, "truncated .bin");
        }
        const uint16_t* px = (const uint16_t*)((const uint8_t*)data + sizeof(lv_image_header_t));
        if (div == 1) memcpy(out, px, out_bytes);
        else          resample565(px, w, h, out, ow, oh);
        heap_caps_free(data);
    }

    SLog.printf("[img] %s %ux%u -> %ux%u div=%u %uKB in %ums; psram free=%uKB largest=%uKB\n",
                path, (unsigned)w, (unsigned)h, (unsigned)ow, (unsigned)oh, (unsigned)div,
                (unsigned)(out_bytes >> 10), (unsigned)(millis() - t0),
                (unsigned)(psram_free() >> 10), (unsigned)(psram_largest() >> 10));

    push_block(L, block);
    lua_pushinteger(L, div);
    return 4;
}

// _img_scale(dsc, w, h) -> dsc, w, h | nil, err
// Box-average any RGB565 descriptor (one of ours, or another in-memory RGB565
// image) into a new buffer. This is how a viewer builds its fit-to-screen copy
// without decoding the file twice.
static int lua_img_scale(lua_State* L) {
    const lv_image_dsc_t* src = (const lv_image_dsc_t*)lua_touserdata(L, 1);
    int32_t dw = (int32_t)luaL_checkinteger(L, 2);
    int32_t dh = (int32_t)luaL_checkinteger(L, 3);

    if (!src || src->header.magic != LV_IMAGE_HEADER_MAGIC
        || src->header.cf != LV_COLOR_FORMAT_RGB565 || !src->data) {
        return push_err(L, "source is not an RGB565 image");
    }
    if (src->header.stride != (uint32_t)src->header.w * 2) return push_err(L, "padded stride");
    if (dw < 1 || dh < 1 || dw > 8192 || dh > 8192) return push_err(L, "bad target size");

    ImgBlock* block = img_alloc((uint32_t)dw, (uint32_t)dh);
    if (!block) return push_err(L, "out of memory");

    resample565((const uint16_t*)src->data, src->header.w, src->header.h,
                img_pixels(block), (uint32_t)dw, (uint32_t)dh);
    return push_block(L, block);
}

// _img_close([dsc]) — one handle, or every open handle when called bare.
static int lua_img_close(lua_State* L) {
    if (lua_isnoneornil(L, 1)) img_free_all();
    else                       img_free(lua_touserdata(L, 1));
    return 0;
}

void img_bridge_register(lua_State* L) {
    lua_register(L, "_img_info",  lua_img_info);
    lua_register(L, "_img_open",  lua_img_open);
    lua_register(L, "_img_scale", lua_img_scale);
    lua_register(L, "_img_close", lua_img_close);
}
