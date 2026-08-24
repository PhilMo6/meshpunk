// screenshot.cpp — RGB565 staging buffer + streaming PNG writer (contract:
// screenshot.h).
//
// WHY A HAND-WRITTEN PNG WRITER: lodepng's encoder is already vendored, but it
// allocates a fixed 262,144-byte hash head plus four window-sized arrays, and
// materialises both the filtered image (~231KB at 320x240) and the whole
// compressed output in RAM — 0.8-1.1MB peak. A screenshot has to work while an
// emulator owns most of PSRAM, so the writer here streams instead: one row of
// scratch, one 4KB deflate block, one 4KB IDAT buffer.
//
// The compressed form is deflate with FIXED Huffman codes and an RLE-only
// matcher: every match is distance 1, so there is no hash table and no window
// buffer. Rows are Sub-filtered (PNG filter type 1), which turns flat UI areas
// into runs of zero bytes — exactly what a distance-1 matcher compresses. Each
// block is costed before it is emitted and falls back to a stored block when
// fixed Huffman would come out larger, so incompressible content cannot inflate
// the file.
//
// The writer half compiles standalone on a host with SCREENSHOT_HOST_TEST
// defined; tools/screenshot_png_test verifies its output against zlib.

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifndef SCREENSHOT_HOST_TEST
#include <Arduino.h>
#include <esp_heap_caps.h>
#include <sys/stat.h>
#include <time.h>

#include "screenshot.h"
#include "display/display_dev.h"
#include "meshpunk_sync.h"   // SLog
#include "usb_manager.h"     // usbdrive_active

extern bool sd_mounted;
extern uint32_t firmware_rtc_epoch();          // main.cpp: our clock authority
extern int32_t  firmware_tz_offset_minutes();   // main.cpp: effective tz offset

#define SS_MALLOC(n) heap_caps_malloc((n), MALLOC_CAP_SPIRAM)
#define SS_FREE(p)   heap_caps_free(p)
#else
#define SS_MALLOC(n) malloc(n)
#define SS_FREE(p)   free(p)
#endif

// ── PNG / deflate writer ────────────────────────────────────────────────────

#define PNG_OUTBUF   4096   // IDAT staging; each fill becomes one IDAT chunk
#define PNG_RAWCHUNK 4096   // uncompressed bytes per deflate block

typedef struct {
    FILE*    f;
    uint8_t* out;          // PNG_OUTBUF
    uint8_t* raw;          // PNG_RAWCHUNK
    uint8_t* row;          // 1 + w*3 filtered scanline
    uint32_t crc_tab[256];
    int      outn;
    int      rawn;
    uint32_t bitbuf;
    int      bitcnt;
    uint32_t adler_a, adler_b;
    bool     err;
} PngW;

// Length code table (RFC 1951 3.2.5), symbols 257..285.
static const uint16_t LEN_BASE[29] = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51,
    59, 67, 83, 99, 115, 131, 163, 195, 227, 258
};
static const uint8_t LEN_EXTRA[29] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3,
    3, 4, 4, 4, 4, 5, 5, 5, 5, 0
};

static void crc_init(PngW* w) {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++)
            c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        w->crc_tab[i] = c;
    }
}

static uint32_t crc_run(const PngW* w, uint32_t c, const void* p, size_t n) {
    const uint8_t* d = (const uint8_t*)p;
    for (size_t i = 0; i < n; i++)
        c = w->crc_tab[(c ^ d[i]) & 0xFF] ^ (c >> 8);
    return c;
}

static void put_be32(PngW* w, uint32_t v) {
    uint8_t b[4] = { (uint8_t)(v >> 24), (uint8_t)(v >> 16),
                     (uint8_t)(v >> 8),  (uint8_t)v };
    if (fwrite(b, 1, 4, w->f) != 4) w->err = true;
}

static void write_chunk(PngW* w, const char* type,
                        const uint8_t* data, uint32_t len) {
    put_be32(w, len);
    if (fwrite(type, 1, 4, w->f) != 4) w->err = true;
    if (len && fwrite(data, 1, len, w->f) != len) w->err = true;
    uint32_t c = crc_run(w, 0xFFFFFFFFu, type, 4);
    c = crc_run(w, c, data, len);
    put_be32(w, c ^ 0xFFFFFFFFu);
}

// Every filled output buffer becomes its own IDAT chunk. The zlib stream may
// be split at any byte boundary across IDATs (PNG 12.3.4), so no bit-level
// bookkeeping crosses this.
static void idat_flush(PngW* w) {
    if (w->outn <= 0) return;
    write_chunk(w, "IDAT", w->out, (uint32_t)w->outn);
    w->outn = 0;
}

static void out_byte(PngW* w, uint8_t b) {
    w->out[w->outn++] = b;
    if (w->outn >= PNG_OUTBUF) idat_flush(w);
}

// Deflate bit order: stream bits fill each byte from the LSB up. Huffman codes
// are packed starting at their MOST significant bit; everything else (block
// headers, extra bits) is least-significant-bit first.
static void bw_bit(PngW* w, int b) {
    if (b) w->bitbuf |= (1u << w->bitcnt);
    if (++w->bitcnt == 8) {
        out_byte(w, (uint8_t)w->bitbuf);
        w->bitbuf = 0;
        w->bitcnt = 0;
    }
}

static void bw_bits(PngW* w, uint32_t v, int n) {
    for (int i = 0; i < n; i++) bw_bit(w, (int)((v >> i) & 1u));
}

static void bw_huff(PngW* w, uint32_t code, int n) {
    for (int i = n - 1; i >= 0; i--) bw_bit(w, (int)((code >> i) & 1u));
}

static void bw_align(PngW* w) {
    while (w->bitcnt) bw_bit(w, 0);
}

// Fixed Huffman literal codes: 0-143 are 8 bits from 0x30, 144-255 are 9 bits
// from 0x190.
static int lit_bits(uint8_t b) { return b <= 143 ? 8 : 9; }

static void emit_lit(PngW* w, uint8_t b) {
    if (b <= 143) bw_huff(w, 0x30u + b, 8);
    else          bw_huff(w, 0x190u + (uint32_t)(b - 144), 9);
}

static int len_index(int len) {
    int k = 28;
    while (k > 0 && len < (int)LEN_BASE[k]) k--;
    return k;
}

// Length symbols 257-279 are 7 bits from 0x00, 280-285 are 8 bits from 0xC0.
// Distance code 0 (5 bits, no extra) IS distance 1 — the only one used.
static void emit_match(PngW* w, int len, int k) {
    int sym = 257 + k;
    if (sym <= 279) bw_huff(w, (uint32_t)(sym - 256), 7);
    else            bw_huff(w, (uint32_t)(0xC0 + (sym - 280)), 8);
    if (LEN_EXTRA[k]) bw_bits(w, (uint32_t)(len - (int)LEN_BASE[k]), LEN_EXTRA[k]);
    bw_huff(w, 0, 5);
}

// One pass of the RLE encoder over a block. w == NULL counts the bits it would
// emit instead of emitting them, which is how a block is costed before the
// stored-vs-fixed choice.
static long rle_pass(PngW* w, const uint8_t* d, int n) {
    long bits = 0;
    int i = 0;
    while (i < n) {
        int j = i + 1;
        while (j < n && d[j] == d[i]) j++;
        bits += lit_bits(d[i]);                 // a match needs a byte behind it
        if (w) emit_lit(w, d[i]);
        int rem = j - i - 1;
        while (rem >= 3) {
            // Never leave a 1 or 2 byte tail that would cost whole literals.
            int L = (rem > 258) ? ((rem - 258 < 3) ? rem - 3 : 258) : rem;
            int k = len_index(L);
            bits += ((257 + k) <= 279 ? 7 : 8) + LEN_EXTRA[k] + 5;
            if (w) emit_match(w, L, k);
            rem -= L;
        }
        while (rem-- > 0) {
            bits += lit_bits(d[i]);
            if (w) emit_lit(w, d[i]);
        }
        i = j;
    }
    return bits;
}

static void deflate_block(PngW* w, const uint8_t* d, int n, int final) {
    long fixed_bits  = 3 + rle_pass(NULL, d, n) + 7;   // header + data + EOB
    long stored_bits = 3 + 7 + 32 + 8L * n;            // header + align + LEN/NLEN
    if (fixed_bits <= stored_bits) {
        bw_bits(w, final ? 1u : 0u, 1);
        bw_bits(w, 1, 2);                 // BTYPE 01: fixed Huffman
        rle_pass(w, d, n);
        bw_huff(w, 0, 7);                 // end of block (symbol 256)
    } else {
        bw_bits(w, final ? 1u : 0u, 1);
        bw_bits(w, 0, 2);                 // BTYPE 00: stored
        bw_align(w);
        uint16_t ln = (uint16_t)n, inv = (uint16_t)~ln;
        out_byte(w, (uint8_t)ln);  out_byte(w, (uint8_t)(ln >> 8));
        out_byte(w, (uint8_t)inv); out_byte(w, (uint8_t)(inv >> 8));
        for (int i = 0; i < n; i++) out_byte(w, d[i]);
    }
}

static void raw_push(PngW* w, const uint8_t* d, int n) {
    for (int i = 0; i < n; i++) {
        w->adler_a = (w->adler_a + d[i]) % 65521u;
        w->adler_b = (w->adler_b + w->adler_a) % 65521u;
        w->raw[w->rawn++] = d[i];
        if (w->rawn >= PNG_RAWCHUNK) {
            deflate_block(w, w->raw, w->rawn, 0);
            w->rawn = 0;
        }
    }
}

// Sub filter (PNG type 1) applied to RGB888 unpacked from big-endian RGB565.
// 5- and 6-bit channels are expanded by bit replication, not a bare shift, so
// full-scale values stay full scale.
static void build_row(uint8_t* dst, const uint16_t* src, int w) {
    dst[0] = 1;
    uint8_t pr = 0, pg = 0, pb = 0;
    uint8_t* o = dst + 1;
    for (int x = 0; x < w; x++) {
        uint16_t be = src[x];
        uint16_t v  = (uint16_t)((be >> 8) | (be << 8));
        uint8_t r5 = (uint8_t)((v >> 11) & 0x1F);
        uint8_t g6 = (uint8_t)((v >> 5)  & 0x3F);
        uint8_t b5 = (uint8_t)(v & 0x1F);
        uint8_t r  = (uint8_t)((r5 << 3) | (r5 >> 2));
        uint8_t g  = (uint8_t)((g6 << 2) | (g6 >> 4));
        uint8_t b  = (uint8_t)((b5 << 3) | (b5 >> 2));
        *o++ = (uint8_t)(r - pr);
        *o++ = (uint8_t)(g - pg);
        *o++ = (uint8_t)(b - pb);
        pr = r; pg = g; pb = b;
    }
}

// Write `src` (big-endian RGB565, tightly packed, w*h) to an already-open file
// as an 8-bit truecolour PNG. Returns false on any write failure.
bool screenshot_png_write_565(FILE* f, const uint16_t* src, int w_px, int h_px) {
    if (!f || !src || w_px <= 0 || h_px <= 0) return false;

    PngW* w = (PngW*)SS_MALLOC(sizeof(PngW));
    if (!w) return false;
    memset(w, 0, sizeof(PngW));
    w->f   = f;
    w->out = (uint8_t*)SS_MALLOC(PNG_OUTBUF);
    w->raw = (uint8_t*)SS_MALLOC(PNG_RAWCHUNK);
    w->row = (uint8_t*)SS_MALLOC((size_t)w_px * 3 + 1);
    if (!w->out || !w->raw || !w->row) {
        SS_FREE(w->out); SS_FREE(w->raw); SS_FREE(w->row); SS_FREE(w);
        return false;
    }
    crc_init(w);
    w->adler_a = 1;

    static const uint8_t SIG[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
    if (fwrite(SIG, 1, 8, f) != 8) w->err = true;

    uint8_t ihdr[13] = {
        (uint8_t)(w_px >> 24), (uint8_t)(w_px >> 16), (uint8_t)(w_px >> 8), (uint8_t)w_px,
        (uint8_t)(h_px >> 24), (uint8_t)(h_px >> 16), (uint8_t)(h_px >> 8), (uint8_t)h_px,
        8,    // bit depth
        2,    // colour type: truecolour RGB
        0, 0, 0
    };
    write_chunk(w, "IHDR", ihdr, sizeof(ihdr));

    // zlib header: CM 8 / CINFO 7, no dict, check bits chosen so the pair is a
    // multiple of 31.
    out_byte(w, 0x78);
    out_byte(w, 0x01);

    for (int y = 0; y < h_px; y++) {
        build_row(w->row, src + (size_t)y * w_px, w_px);
        raw_push(w, w->row, w_px * 3 + 1);
        if (w->err) break;
    }

    // Always closes with a final block, even when the last push emptied the
    // buffer exactly (an empty fixed block is legal and costs 10 bits).
    deflate_block(w, w->raw, w->rawn, 1);
    w->rawn = 0;
    bw_align(w);
    out_byte(w, (uint8_t)(w->adler_b >> 8)); out_byte(w, (uint8_t)w->adler_b);
    out_byte(w, (uint8_t)(w->adler_a >> 8)); out_byte(w, (uint8_t)w->adler_a);
    idat_flush(w);
    write_chunk(w, "IEND", NULL, 0);

    bool ok = !w->err;
    SS_FREE(w->out); SS_FREE(w->raw); SS_FREE(w->row); SS_FREE(w);
    return ok;
}

#ifndef SCREENSHOT_HOST_TEST

// ── Staging buffer ──────────────────────────────────────────────────────────

static uint16_t* s_buf = nullptr;
static int       s_w = 0, s_h = 0;
static uint32_t  s_seq = 0;      // same-second / no-clock filename disambiguator

bool screenshot_begin(void) {
    if (s_buf) return false;                 // one capture at a time
    s_w = display_dev_width();
    s_h = display_dev_height();
    size_t bytes = (size_t)s_w * s_h * 2;
    s_buf = (uint16_t*)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
    if (!s_buf) {
        SLog.printf("[shot] no PSRAM for %u byte frame\n", (unsigned)bytes);
        return false;
    }
    memset(s_buf, 0, bytes);                 // 0x0000 is black in either byte order
    return true;
}

bool screenshot_busy(void) { return s_buf != nullptr; }

void screenshot_end(void) {
    if (!s_buf) return;
    heap_caps_free(s_buf);
    s_buf = nullptr;
}

void screenshot_feed_be565(const uint16_t* px, int x, int y, int w, int h) {
    if (!s_buf || !px || w <= 0 || h <= 0) return;
    int sx = x < 0 ? -x : 0;                 // source column to start from
    int sy = y < 0 ? -y : 0;
    int dx = x < 0 ? 0 : x;
    int dy = y < 0 ? 0 : y;
    int cw = w - sx; if (dx + cw > s_w) cw = s_w - dx;
    int ch = h - sy; if (dy + ch > s_h) ch = s_h - dy;
    if (cw <= 0 || ch <= 0) return;
    for (int r = 0; r < ch; r++)
        memcpy(s_buf + (size_t)(dy + r) * s_w + dx,
               px + (size_t)(sy + r) * w + sx,
               (size_t)cw * 2);
}

bool screenshot_write_png(const char* vfs_path) {
    if (!s_buf || !vfs_path) return false;
    uint32_t t0 = millis();
    FILE* f = fopen(vfs_path, "wb");
    if (!f) {
        SLog.printf("[shot] cannot open %s\n", vfs_path);
        return false;
    }
    bool ok = screenshot_png_write_565(f, s_buf, s_w, s_h);
    long size = ftell(f);
    fclose(f);
    if (!ok) {
        remove(vfs_path);
        SLog.printf("[shot] write failed: %s\n", vfs_path);
        return false;
    }
    SLog.printf("[shot] %dx%d -> %s (%ld bytes, %lu ms)\n",
                s_w, s_h, vfs_path, size, (unsigned long)(millis() - t0));
    return true;
}

// ── Naming ──────────────────────────────────────────────────────────────────

bool screenshot_make_path(char* out, size_t n) {
    if (!out || n < 16) return false;

    // Writing to a card the PC owns is not safe. NO PATH REACHES THIS TODAY
    // (hw-checked): starting USB drive mode calls apps.close_all_backgrounds,
    // which takes the Screenshot app's button with it, and a module run stops
    // sharing because the app that pings the session dies with the Lua state.
    // Kept as the invariant guard — the cost of being wrong here is a corrupt
    // filesystem, not a missed screenshot.
    if (usbdrive_active()) {
        snprintf(out, n, "USB drive mode owns the SD card");
        return false;
    }
    const char* dir = sd_mounted ? "/sd/screenshots" : "/littlefs/screenshots";
    mkdir(dir, 0777);   // pre-existing is the normal case; the open below reports real failures

    uint32_t epoch = firmware_rtc_epoch();
    char stem[32];
    if (epoch > 0) {
        time_t local = (time_t)epoch + (time_t)firmware_tz_offset_minutes() * 60;
        struct tm tmv;
        gmtime_r(&local, &tmv);   // already shifted to local: no second conversion
        snprintf(stem, sizeof(stem), "shot_%04d%02d%02d_%02d%02d%02d",
                 tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
                 tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
    } else {
        snprintf(stem, sizeof(stem), "shot_%04u", (unsigned)(++s_seq & 0xFFFF));
    }

    snprintf(out, n, "%s/%s.png", dir, stem);
    for (int i = 2; i <= 99; i++) {
        struct stat st;
        if (stat(out, &st) != 0) return true;
        snprintf(out, n, "%s/%s_%02d.png", dir, stem, i);
    }
    return true;   // 99 shots in one second: the last name wins and is overwritten
}

bool screenshot_finish_to_disk(char* path_out, size_t path_n) {
    char path[96];
    // On either failure `path` carries the REASON, not a filename: callers put
    // this straight in front of the user, and "failed: /sd/screenshots/..."
    // reads like a success. The path itself is in the log line above.
    bool ok = screenshot_make_path(path, sizeof(path));
    if (ok && !screenshot_write_png(path)) {
        snprintf(path, sizeof(path), "%s", sd_mounted ? "SD write failed"
                                                      : "flash write failed");
        ok = false;
    }
    if (path_out && path_n) snprintf(path_out, path_n, "%s", path);
    screenshot_end();
    return ok;
}

#endif // !SCREENSHOT_HOST_TEST
