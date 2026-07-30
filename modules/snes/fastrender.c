// Fast-path renderer for BG modes 0 and 1 (the dominant modes). Replaces
// S9xUpdateScreen's span rendering when snes_fast_ok() says the current
// PPU state is in scope; other modes render through the stock path per
// span, so mid-frame mode switches mix correctly.
//
// Model: spans render in BANDS of up to 8 scroll-constant lines. Within a
// band, layers composite back-to-front in the exact global priority order
// the stock renderer encodes in its depth values (sprite z = D+4*(prio+1))
// into band buffers on the module task's stack — internal SRAM, so all
// intermediate pixel traffic stays off the PSRAM bus. There is no
// Z-buffer (priority is draw order) and no decoded-tile cache: each tile
// visited decodes its needed rows once per band, straight from its 16/32
// contiguous VRAM bytes, through a 256-entry planar LUT into one uint32
// of eight 4-bit pixels per row. Banding is what amortizes the tilemap
// and tile fetches across lines, matching the stock renderer's Lines
// batching.
//
// Color math resolves at store time against the already-composited sub
// band: per pixel the sub state is 0 (no math), 1 (math vs fixed colour,
// never halved) or 2 (math vs sub pixel, halved when $2131 bit 6 is set).
//
// Out of scope (stock path renders these spans): modes 2-7, mosaic,
// pseudo-hires, offset-per-tile, direct colour. Known approximations:
// the 34-tile sprite line limit applies per sprite rather than per tile
// chunk, and the main-screen colour-window black-out pass (Clip[0][5])
// is not replicated.

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "snes9x.h"
#include "memmap.h"
#include "ppu.h"
#include "gfx.h"

// The ELF loader copies .iram.text into internal SRAM. The 16KB instruction
// cache is shared by both cores and fronts PSRAM only, so code running from
// SRAM takes no cache misses and evicts nothing the CPU interpreter is using
// on the other core. Moving the section changes every PC-relative distance
// to anything outside it, so build.ps1 compiles this file with
// -mtext-section-literals to keep each literal pool inside the section, and
// audits the l32r targets after linking.
#define FR_HOT __attribute__((section(".iram.text")))

// Per-pixel helpers must inline: a section attribute suppresses inlining
// (the placement is explicit, so GCC will not copy the body into a caller),
// which leaves an out-of-line callx8 plus register-window traffic in the
// innermost loop. Inlined into an FR_HOT caller, the code lands in
// .iram.text regardless, so these carry no section attribute of their own.
#define FR_INLINE static INLINE __attribute__((always_inline))

// Register-bit tests and constants defined privately in gfx.c — kept
// textually identical here.
#define ON_MAIN(N) \
(GFX.r212c & (1 << (N)))

#define SUB_OR_ADD(N) \
(GFX.r2131 & (1 << (N)))

#define ON_SUB(N) \
((GFX.r2130 & 0x30) != 0x30 && \
 (GFX.r2130 & 2) && \
 (GFX.r212d & (1 << N)))

#define ANYTHING_ON_SUB \
((GFX.r2130 & 0x30) != 0x30 && \
 (GFX.r2130 & 2) && \
 (GFX.r212d & 0x1f))

#define ADD_OR_SUB_ON_ANYTHING \
(GFX.r2131 & 0x3f)

#define BLACK BUILD_PIXEL(0,0,0)

// Band height. The context lives on the caller's stack (internal SRAM,
// which the render worker's task stack must fit into), so this trades
// menu-scene amortization for room to also hold the decode and palette
// tables there: HDMA per-line scrolling collapses gameplay bands to one
// line regardless.
// Band height. The context lives on the caller's stack (internal SRAM,
// and the render worker's task stack caps at 16KB), so this is the main
// stack consumer. Taller bands amortize the tilemap and tile fetches
// across more lines and mean fewer strip pushes, each of which costs a
// bus lock plus three SPI command transactions.
#define FR_MAX_LINES 7

// ---------------------------------------------------------------------------
// Planar-to-chunky LUT: fr_lut[byte] packs bit (7-x) into nibble x, so
// plane bytes OR together (shifted by plane index) into one uint32 holding
// eight 4-bit pixel values, leftmost pixel in nibble 0.
// ---------------------------------------------------------------------------
static uint32_t fr_lut[256];     // screen px x = source bit (7-x) -> nibble x
static int fr_lut_ready = 0;

static FR_HOT void fr_build_lut(void)
{
    int b, x;
    for (b = 0; b < 256; b++) {
        uint32_t v = 0;
        for (x = 0; x < 8; x++)
            if (b & (0x80 >> x))
                v |= 1u << (x * 4);
        fr_lut[b] = v;
    }
    fr_lut_ready = 1;
}

// Horizontal flip = reverse the eight nibbles of a decoded row. Done once
// per tile row rather than per pixel, which is cheaper than carrying a
// second 1KB lookup table through the band context (that stack pays for a
// taller band instead).
FR_INLINE uint32_t fr_nibrev(uint32_t v)
{
    v = ((v & 0x0f0f0f0fu) << 4) | ((v >> 4) & 0x0f0f0f0fu);
    v = ((v & 0x00ff00ffu) << 8) | ((v >> 8) & 0x00ff00ffu);
    return (v << 16) | (v >> 16);
}

// ---------------------------------------------------------------------------
// Band context (lives on the caller's stack = internal SRAM)
// ---------------------------------------------------------------------------
typedef struct {
    uint16_t main_px[FR_MAX_LINES][256];
    uint16_t sub_px[FR_MAX_LINES][256];
    uint8_t  subset[FR_MAX_LINES][256]; // 0 none / 1 fixed / 2 sub pixel
    uint8_t  cov[FR_MAX_LINES][256];    // main-screen pixel drawn by a layer
    uint8_t  lclass[FR_MAX_LINES];      // 2 = subset all 2, 1 = all 1, 0 = mixed
    uint8_t  obj_idx[FR_MAX_LINES][32]; // per line: OBJLines entries kept
    uint8_t  obj_count[FR_MAX_LINES];
    uint8_t  p1seen[2][4];              // [is_sub][bg]: p1 tiles in this band
    // Hot tables, copied here per span: the band context lives on the
    // task stack, which is internal SRAM, so these lookups stop competing
    // for the data cache Core 0's emulation is thrashing.
    uint16_t pal[256];
    uint16_t mathpal[256];
    uint32_t lut[256];
    int      y0;                        // first screen line of the band
    int      lines;                     // 1..FR_MAX_LINES
    int      math_active;
    int      msub;                      // $2131 bit 7
    int      mhalf;                     // $2131 bit 6
    uint16_t fixedc;
    const SLineData *ld;                // journal row for y0
} FrBand;

// Key for the band's mathed palette (COLOR_OP(pal[i], fixed colour)): the
// packed journaled fixed colour it was built for. The table itself lives
// in the band context so the lookups stay in internal SRAM.
static uint32_t fr_mathpal_key = 0xffffffffu;

// Rows the fast path wrote already byte-swapped for the TFT; blit_frame
// skips its swap pass for them and clears the flags after each blit.
// Stock-rendered rows stay 0 and get swapped there, so mixed frames
// (fast + stock spans) remain correct.
uint8_t snes_row_swapped[SNES_HEIGHT_EXTENDED];

// Tile/tilemap source for this span: fr_vram when rendering on Core 0
// (the S9xUpdateScreen hook sets it), or the worker's VRAM snapshot —
// which shrinks the live-VRAM tear window from a whole render (~2 emulated
// frames) to the snapshot copy itself.
const uint8_t *fr_vram;

// Output mode: 1 = push each finished band straight to the panel from the
// band buffer (internal SRAM, no frame buffer at all); 0 = write into
// GFX.Screen for the Core-0 path, which blits a whole frame.
int fr_strip_out;
extern void host_blit_rect(const uint16_t *rgb565, int x, int y, int w, int h);

// Palette and forced-blank state for this span: live IPPU/PPU values when
// rendering on Core 0, or the worker's frame-boundary snapshots.
const uint16_t *fr_pal;
int fr_fblank;

// Sprite state for this span: live GFX/PPU tables when rendering on
// Core 0, or the worker's frame-boundary snapshots.
const SOBJLines *fr_objlines;
const uint8_t *fr_objw;
const uint8_t *fr_objvis;
const SOBJ *fr_objs;
uint32_t fr_objnamebase;
uint32_t fr_objnamesel;

// BG registers, mode bits and clip windows for this span — live values
// when rendering on Core 0, frame-boundary snapshots on the worker.
// Palette writes recorded during active display (ppu.h). NULL on the Core-0
// path, where CGRAM writes flush the span instead.
const SPalWrite *fr_pal_j;
uint32_t fr_pal_jn;

uint16_t fr_bg_scbase[4];
uint16_t fr_bg_namebase[4];
uint16_t fr_bg_scsize[4];
uint8_t fr_bg_tile16[4];
int fr_bgmode;
int fr_bg3prio;
const ClipData *fr_clip;

FR_INLINE uint16_t fr_math_idx(const FrBand *B, int r, uint32_t idx, int x)
{
    uint8_t s = B->subset[r][x];
    uint16_t c, o;
    if (s == 0)
        return B->pal[idx];
    if (s == 1)
        return B->mathpal[idx];
    c = B->pal[idx];
    o = B->sub_px[r][x];
    if (B->msub)
        return B->mhalf ? COLOR_SUB1_2(c, o) : COLOR_SUB(c, o);
    return B->mhalf ? COLOR_ADD1_2(c, o) : COLOR_ADD(c, o);
}

// Uniform-line variant: the whole line is subset==2, so no per-pixel state
// checks — just the operation. The two flag branches predict perfectly.
FR_INLINE uint16_t fr_math2(const FrBand *B, uint16_t c, uint16_t o)
{
    if (B->msub)
        return B->mhalf ? COLOR_SUB1_2(c, o) : COLOR_SUB(c, o);
    return B->mhalf ? COLOR_ADD1_2(c, o) : COLOR_ADD(c, o);
}

// ---------------------------------------------------------------------------
// BG layer: one priority pass over the whole band for one clip range.
// Splits the band at this BG's own tile-row boundaries; each tile decodes
// all its needed rows from one contiguous fetch.
// ---------------------------------------------------------------------------
static FR_HOT void fr_bg_band(FrBand *B, int bg, int prio, int left, int right,
                       int is_sub, int do_math, int bits, uint32_t pal_base)
{
    uint32_t voff = B->ld->BG[bg].VOffset;
    uint32_t hoff = B->ld->BG[bg].HOffset;
    int tile16 = fr_bg_tile16[bg] != 0;
    uint32_t name_base = (uint32_t)fr_bg_namebase[bg] << 1;
    int chr_shift = (bits == 4) ? 5 : 4;
    const uint16_t *sc0, *sc1, *sc2, *sc3;
    int s;

    sc0 = (const uint16_t *)&fr_vram[(uint32_t)fr_bg_scbase[bg] << 1];
    sc1 = (fr_bg_scsize[bg] & 1) ? sc0 + 1024 : sc0;
    if (sc1 >= (const uint16_t *)(fr_vram + 0x10000))
        sc1 -= 0x8000;
    sc2 = (fr_bg_scsize[bg] & 2) ? sc1 + 1024 : sc0;
    if (sc2 >= (const uint16_t *)(fr_vram + 0x10000))
        sc2 -= 0x8000;
    sc3 = (fr_bg_scsize[bg] & 1) ? sc2 + 1024 : sc2;
    if (sc3 >= (const uint16_t *)(fr_vram + 0x10000))
        sc3 -= 0x8000;

    s = 0;
    while (s < B->lines) {
        uint32_t vpx = (voff + (uint32_t)(B->y0 + s)) & 0x3ff;
        uint32_t finev = vpx & 7;
        int seg = 8 - (int)finev;
        uint32_t srow = vpx >> (tile16 ? 4 : 3);
        uint32_t suby = tile16 ? ((vpx >> 3) & 1) : 0;
        const uint16_t *b1, *b2;
        int x;

        if (seg > B->lines - s)
            seg = B->lines - s;

        if (srow & 0x20) {
            b1 = sc2;
            b2 = sc3;
        } else {
            b1 = sc0;
            b2 = sc1;
        }
        b1 += (srow & 0x1f) << 5;
        b2 += (srow & 0x1f) << 5;

        uint32_t hpx = (hoff + (uint32_t)left) & 0x3ff;
        x = left;
        while (x < right) {
            uint32_t finh = hpx & 7;
            int n = 8 - (int)finh;
            uint32_t col8 = hpx >> 3;
            uint32_t tcol = tile16 ? (col8 >> 1) : col8;
            uint32_t subx = tile16 ? (col8 & 1) : 0;
            const uint16_t *rowp = (tcol & 0x20) ? b2 : b1;
            uint16_t t = rowp[tcol & 0x1f];
            int hf, vf, r, i;
            uint32_t chr, tbase, cbase;
            const uint8_t *tp;
            uint32_t nib[FR_MAX_LINES];
            uint32_t any;

            if (n > right - x)
                n = right - x;

            if (t & 0x2000)
                B->p1seen[is_sub][bg] = 1;
            if ((int)((t >> 13) & 1) != prio) {
                x += n;
                hpx = (hpx + (uint32_t)n) & 0x3ff;
                continue;
            }

            hf = (t & H_FLIP) != 0;
            vf = (t & V_FLIP) != 0;
            chr = t & 0x3ff;
            if (tile16)
                chr = (chr + ((subx ^ (uint32_t)hf) ? 1 : 0)
                           + (((suby ^ (uint32_t)vf) ? 1 : 0) << 4)) & 0x3ff;

            tbase = (name_base + (chr << chr_shift)) & 0xffff;
            tp = &fr_vram[tbase];

            // Decode this tile's rows for the whole segment in one visit.
            // The flip-matched LUT bakes horizontal flip into nibble order,
            // so pixel consumption below is always a sequential >> 4.
            {
                const uint32_t *lut = B->lut;
                any = 0;
                for (r = 0; r < seg; r++) {
                    uint32_t frow = finev + (uint32_t)r;
                    uint32_t sr = vf ? (7 - frow) : frow;
                    const uint8_t *rp = tp + sr * 2;
                    uint32_t v = lut[rp[0]] | (lut[rp[1]] << 1);
                    if (bits == 4)
                        v |= (lut[rp[16]] << 2) | (lut[rp[17]] << 3);
                    if (hf)
                        v = fr_nibrev(v);
                    nib[r] = v;
                    any |= v;
                }
            }
            if (!any) {
                x += n;
                hpx = (hpx + (uint32_t)n) & 0x3ff;
                continue;
            }

            cbase = pal_base + ((((uint32_t)t >> 10) & 7) << (bits == 4 ? 4 : 2));
            for (r = 0; r < seg; r++) {
                uint32_t nr = nib[r] >> (finh * 4);
                uint16_t *mrow;
                if (!nr)
                    continue;
                if (is_sub) {
                    uint16_t *srowpx = B->sub_px[s + r];
                    uint8_t *setrow = B->subset[s + r];
                    uint32_t m = nr | (nr >> 1);
                    m |= (m >> 2);
                    m &= 0x11111111u;
                    if (n == 8 && m == 0x11111111u) {
                        srowpx[x + 0] = B->pal[cbase + (nr & 0xf)]; setrow[x + 0] = 2; nr >>= 4;
                        srowpx[x + 1] = B->pal[cbase + (nr & 0xf)]; setrow[x + 1] = 2; nr >>= 4;
                        srowpx[x + 2] = B->pal[cbase + (nr & 0xf)]; setrow[x + 2] = 2; nr >>= 4;
                        srowpx[x + 3] = B->pal[cbase + (nr & 0xf)]; setrow[x + 3] = 2; nr >>= 4;
                        srowpx[x + 4] = B->pal[cbase + (nr & 0xf)]; setrow[x + 4] = 2; nr >>= 4;
                        srowpx[x + 5] = B->pal[cbase + (nr & 0xf)]; setrow[x + 5] = 2; nr >>= 4;
                        srowpx[x + 6] = B->pal[cbase + (nr & 0xf)]; setrow[x + 6] = 2; nr >>= 4;
                        srowpx[x + 7] = B->pal[cbase + (nr & 0xf)]; setrow[x + 7] = 2;
                    } else {
                        for (i = 0; i < n; i++, nr >>= 4) {
                            uint32_t v = nr & 0xf;
                            if (v) {
                                srowpx[x + i] = B->pal[cbase + v];
                                setrow[x + i] = 2;
                            }
                        }
                    }
                    continue;
                }
                mrow = B->main_px[s + r];
                if (do_math) {
                    uint8_t *covrow = B->cov[s + r];
                    uint8_t lc = B->lclass[s + r];
                    if (lc == 2) {
                        const uint16_t *subrow = B->sub_px[s + r];
                        uint32_t m = nr | (nr >> 1);
                        m |= (m >> 2);
                        m &= 0x11111111u;
                        if (n == 8 && m == 0x11111111u) {
                            mrow[x + 0] = fr_math2(B, B->pal[cbase + (nr & 0xf)], subrow[x + 0]); covrow[x + 0] = 1; nr >>= 4;
                            mrow[x + 1] = fr_math2(B, B->pal[cbase + (nr & 0xf)], subrow[x + 1]); covrow[x + 1] = 1; nr >>= 4;
                            mrow[x + 2] = fr_math2(B, B->pal[cbase + (nr & 0xf)], subrow[x + 2]); covrow[x + 2] = 1; nr >>= 4;
                            mrow[x + 3] = fr_math2(B, B->pal[cbase + (nr & 0xf)], subrow[x + 3]); covrow[x + 3] = 1; nr >>= 4;
                            mrow[x + 4] = fr_math2(B, B->pal[cbase + (nr & 0xf)], subrow[x + 4]); covrow[x + 4] = 1; nr >>= 4;
                            mrow[x + 5] = fr_math2(B, B->pal[cbase + (nr & 0xf)], subrow[x + 5]); covrow[x + 5] = 1; nr >>= 4;
                            mrow[x + 6] = fr_math2(B, B->pal[cbase + (nr & 0xf)], subrow[x + 6]); covrow[x + 6] = 1; nr >>= 4;
                            mrow[x + 7] = fr_math2(B, B->pal[cbase + (nr & 0xf)], subrow[x + 7]); covrow[x + 7] = 1;
                        } else {
                            for (i = 0; i < n; i++, nr >>= 4) {
                                uint32_t v = nr & 0xf;
                                if (v) {
                                    mrow[x + i] = fr_math2(B,
                                        B->pal[cbase + v],
                                        subrow[x + i]);
                                    covrow[x + i] = 1;
                                }
                            }
                        }
                    } else if (lc == 1) {
                        for (i = 0; i < n; i++, nr >>= 4) {
                            uint32_t v = nr & 0xf;
                            if (v) {
                                mrow[x + i] = B->mathpal[cbase + v];
                                covrow[x + i] = 1;
                            }
                        }
                    } else {
                        for (i = 0; i < n; i++, nr >>= 4) {
                            uint32_t v = nr & 0xf;
                            if (v) {
                                mrow[x + i] =
                                    fr_math_idx(B, s + r, cbase + v, x + i);
                                covrow[x + i] = 1;
                            }
                        }
                    }
                    continue;
                }
                // Opacity mask: nibble LSBs collect each pixel's non-zero
                // flag; a full mask means eight unconditional stores.
                {
                    uint8_t *covrow = B->cov[s + r];
                    uint32_t m = nr | (nr >> 1);
                    m |= (m >> 2);
                    m &= 0x11111111u;
                    if (n == 8 && m == 0x11111111u) {
                        mrow[x + 0] = B->pal[cbase + (nr & 0xf)]; covrow[x + 0] = 1; nr >>= 4;
                        mrow[x + 1] = B->pal[cbase + (nr & 0xf)]; covrow[x + 1] = 1; nr >>= 4;
                        mrow[x + 2] = B->pal[cbase + (nr & 0xf)]; covrow[x + 2] = 1; nr >>= 4;
                        mrow[x + 3] = B->pal[cbase + (nr & 0xf)]; covrow[x + 3] = 1; nr >>= 4;
                        mrow[x + 4] = B->pal[cbase + (nr & 0xf)]; covrow[x + 4] = 1; nr >>= 4;
                        mrow[x + 5] = B->pal[cbase + (nr & 0xf)]; covrow[x + 5] = 1; nr >>= 4;
                        mrow[x + 6] = B->pal[cbase + (nr & 0xf)]; covrow[x + 6] = 1; nr >>= 4;
                        mrow[x + 7] = B->pal[cbase + (nr & 0xf)]; covrow[x + 7] = 1;
                    } else {
                        for (i = 0; i < n; i++, nr >>= 4) {
                            uint32_t v = nr & 0xf;
                            if (v) {
                                mrow[x + i] = B->pal[cbase + v];
                                covrow[x + i] = 1;
                            }
                        }
                    }
                }
            }
            x += n;
            hpx = (hpx + (uint32_t)n) & 0x3ff;
        }
        s += seg;
    }
}

static FR_HOT void fr_bg(FrBand *B, int bg, int prio, int is_sub, int bits,
                  uint32_t pal_base)
{
    const ClipData *clip = &fr_clip[is_sub];
    int do_math = (!is_sub) && B->math_active && (SUB_OR_ADD(bg) != 0);
    uint32_t bands = clip->Count[bg];
    uint32_t b;

    // The priority-0 pass records whether this band's tilemap run holds
    // any priority-1 tiles; if it doesn't, the p1 walk has nothing to do.
    if (prio == 1 && !B->p1seen[is_sub][bg])
        return;

    if (!bands) {
        fr_bg_band(B, bg, prio, 0, 256, is_sub, do_math, bits, pal_base);
        return;
    }
    for (b = 0; b < bands; b++) {
        int left = (int)clip->Left[b][bg];
        int right = (int)clip->Right[b][bg];
        if (right > 256)
            right = 256;
        if (right <= left)
            continue;
        fr_bg_band(B, bg, prio, left, right, is_sub, do_math, bits, pal_base);
    }
}

// ---------------------------------------------------------------------------
// Sprites: per line, one budget pass (34-tile limit at sprite granularity),
// then per-priority painter passes drawing survivors in reverse list order
// so the earliest OAM entry lands on top.
// ---------------------------------------------------------------------------
static FR_HOT void fr_obj_setup(FrBand *B)
{
    int r;
    for (r = 0; r < B->lines; r++) {
        const SOBJLines *ol = &fr_objlines[B->y0 + r];
        int32_t tiles = ol->Tiles;
        int i, cnt = 0;
        for (i = 0; i < 32; i++) {
            int sp = ol->OBJ[i].Sprite;
            if (sp < 0)
                break;
            tiles += fr_objvis[sp];
            if (tiles <= 0)
                continue;
            B->obj_idx[r][cnt++] = (uint8_t)i;
        }
        B->obj_count[r] = (uint8_t)cnt;
    }
}

static FR_HOT void fr_obj(FrBand *B, int prio, int is_sub, int math_layer_on)
{
    const ClipData *clip = &fr_clip[is_sub];
    uint32_t bands = clip->Count[4];
    int r;

    for (r = 0; r < B->lines; r++) {
        const SOBJLines *ol = &fr_objlines[B->y0 + r];
        uint16_t *mrow = B->main_px[r];
        uint16_t *srowpx = B->sub_px[r];
        uint8_t *setrow = B->subset[r];
        uint8_t *covrow = B->cov[r];
        uint8_t lc = B->lclass[r];
        int k;

        for (k = (int)B->obj_count[r] - 1; k >= 0; k--) {
            int I = B->obj_idx[r][k];
            int S = ol->OBJ[I].Sprite;
            const SOBJ *o = &fr_objs[S];
            uint32_t line, chr_row, chr_page, tilex, cbase, frow;
            int do_math, width, chunks, inc, cx;
            int32_t X0;

            if ((int)o->Priority != prio)
                continue;

            X0 = o->HPos;
            if (X0 == -256)
                continue;
            width = fr_objw[S];
            if (X0 + width <= 0 || X0 >= 256)
                continue;

            do_math = math_layer_on && o->Palette >= 4;
            cbase = 128 + ((uint32_t)o->Palette << 4);

            line = ol->OBJ[I].Line;          // VFlip pre-applied by S9xSetupOBJ
            chr_row = ((line << 1) + (o->Name & 0xf0)) & 0xf0;
            chr_page = o->Name & 0x100;
            frow = line & 7;
            chunks = width >> 3;
            if (o->HFlip) {
                tilex = ((o->Name & 0x0f) + (uint32_t)chunks - 1) & 0x0f;
                inc = -1;
            } else {
                tilex = o->Name & 0x0f;
                inc = 1;
            }

            for (cx = 0; cx < chunks; cx++,
                 tilex = (tilex + (uint32_t)inc) & 0x0f) {
                int32_t X8 = X0 + cx * 8;
                uint32_t chr, taddr, nib;
                const uint8_t *tp;
                int i;

                if (X8 <= -8 || X8 >= 256)
                    continue;

                chr = chr_page | chr_row | tilex;
                taddr = fr_objnamebase + ((chr & 0x3ff) << 5);
                if ((chr & 0x1ff) >= 256)
                    taddr += fr_objnamesel;
                taddr = (taddr + frow * 2) & 0xffff;
                tp = &fr_vram[taddr];
                {
                    const uint32_t *lut = B->lut;
                    nib = lut[tp[0]] | (lut[tp[1]] << 1)
                        | (lut[tp[16]] << 2) | (lut[tp[17]] << 3);
                    if (o->HFlip)
                        nib = fr_nibrev(nib);
                }
                if (!nib)
                    continue;

                for (i = 0; i < 8; i++, nib >>= 4) {
                    int X = X8 + i;
                    uint32_t v = nib & 0xf;
                    if (!v || X < 0 || X >= 256)
                        continue;
                    if (bands) {
                        uint32_t b;
                        int vis = 0;
                        for (b = 0; b < bands; b++)
                            if ((uint32_t)X >= clip->Left[b][4] &&
                                (uint32_t)X < clip->Right[b][4]) {
                                vis = 1;
                                break;
                            }
                        if (!vis)
                            continue;
                    }
                    if (is_sub) {
                        srowpx[X] = B->pal[cbase + v];
                        setrow[X] = 2;
                    } else if (do_math) {
                        if (lc == 2)
                            mrow[X] = fr_math2(B,
                                B->pal[cbase + v], srowpx[X]);
                        else if (lc == 1)
                            mrow[X] = B->mathpal[cbase + v];
                        else
                            mrow[X] = fr_math_idx(B, r, cbase + v, X);
                        covrow[X] = 1;
                    } else {
                        mrow[X] = B->pal[cbase + v];
                        covrow[X] = 1;
                    }
                }
            }
        }
    }
}

// ---------------------------------------------------------------------------
// One band, one screen: composite in the stock renderer's global priority
// order. Mode 0 (back to front): BG3.0 BG2.0 OBJ0 BG3.1 BG2.1 OBJ1 BG1.0
// BG0.0 OBJ2 BG1.1 BG0.1 OBJ3. Mode 1: BG2.0 OBJ0 BG2.1* OBJ1 BG1.0 BG0.0
// OBJ2 BG1.1 BG0.1 OBJ3 (* BG2.1 goes in front of everything when
// BG3Priority is set).
// ---------------------------------------------------------------------------
static FR_HOT void fr_screen(FrBand *B, int is_sub)
{
    int mode = fr_bgmode;
    int obj_math = B->math_active && (SUB_OR_ADD(4) != 0) && !is_sub;
    int en[5];
    int i;

    for (i = 0; i < 5; i++)
        en[i] = is_sub ? (ON_SUB(i) != 0) : (ON_MAIN(i) != 0);

    if (mode == 0) {
        if (en[3]) fr_bg(B, 3, 0, is_sub, 2, 3u << 5);
        if (en[2]) fr_bg(B, 2, 0, is_sub, 2, 2u << 5);
        if (en[4]) fr_obj(B, 0, is_sub, obj_math);
        if (en[3]) fr_bg(B, 3, 1, is_sub, 2, 3u << 5);
        if (en[2]) fr_bg(B, 2, 1, is_sub, 2, 2u << 5);
        if (en[4]) fr_obj(B, 1, is_sub, obj_math);
        if (en[1]) fr_bg(B, 1, 0, is_sub, 2, 1u << 5);
        if (en[0]) fr_bg(B, 0, 0, is_sub, 2, 0);
        if (en[4]) fr_obj(B, 2, is_sub, obj_math);
        if (en[1]) fr_bg(B, 1, 1, is_sub, 2, 1u << 5);
        if (en[0]) fr_bg(B, 0, 1, is_sub, 2, 0);
        if (en[4]) fr_obj(B, 3, is_sub, obj_math);
    } else {
        int bg3p = fr_bg3prio != 0;
        if (en[2]) fr_bg(B, 2, 0, is_sub, 2, 0);
        if (en[4]) fr_obj(B, 0, is_sub, obj_math);
        if (en[2] && !bg3p) fr_bg(B, 2, 1, is_sub, 2, 0);
        if (en[4]) fr_obj(B, 1, is_sub, obj_math);
        if (en[1]) fr_bg(B, 1, 0, is_sub, 4, 0);
        if (en[0]) fr_bg(B, 0, 0, is_sub, 4, 0);
        if (en[4]) fr_obj(B, 2, is_sub, obj_math);
        if (en[1]) fr_bg(B, 1, 1, is_sub, 4, 0);
        if (en[0]) fr_bg(B, 0, 1, is_sub, 4, 0);
        if (en[4]) fr_obj(B, 3, is_sub, obj_math);
        if (en[2] && bg3p) fr_bg(B, 2, 1, is_sub, 2, 0);
    }
}

// ---------------------------------------------------------------------------
// Span entry points (called from S9xUpdateScreen)
// ---------------------------------------------------------------------------
FR_HOT int snes_fast_ok(void)
{
    if (PPU.BGMode > 1)
        return 0;
    if (GFX.Pseudo)
        return 0;
    if (IPPU.RenderedScreenWidth != 256)
        return 0;
    if (PPU.Mosaic > 1 &&
        (PPU.BGMosaic[0] || PPU.BGMosaic[1] ||
         PPU.BGMosaic[2] || PPU.BGMosaic[3]))
        return 0;
    return 1;
}

FR_HOT void snes_fast_span(uint32_t starty, uint32_t endy, const SLineData *linedata,
                    const uint32_t *fixedc_j)
{
    FrBand B;
    uint32_t y;
    uint16_t backdrop;
    int bd_math;
    int fblank = fr_fblank;
    const SPalWrite *pj;
    uint32_t pjn, ji = 0;
    // Strip mode centres the frame itself (the whole-frame blit does this
    // in the host); each finished band goes straight out.
    int y_off = (240 - (int)(endy + 1)) / 2;

    if (y_off < 0)
        y_off = 0;

    if (!fr_lut_ready)
        fr_build_lut();

    // Pull the per-pixel lookup tables into the band context (task stack =
    // internal SRAM). They are read tens of thousands of times per frame
    // and would otherwise contend for the data cache with Core 0.
    memcpy(B.lut, fr_lut, sizeof(B.lut));
    memcpy(B.pal, fr_pal, sizeof(B.pal));

    // Mid-frame palette writes: B.pal starts at the frame's opening state
    // and entries are folded in as the render descends, so each band draws
    // with the palette its lines actually saw. Entries are in line order and
    // apply from their own line onward; the band loop below will not span
    // one. Empty (and the whole mechanism inert) on the Core-0 path.
    pj = fr_pal_j;
    pjn = pj ? fr_pal_jn : 0;
    while (ji < pjn && pj[ji].line <= starty) {
        B.pal[pj[ji].idx] = pj[ji].col;
        ji++;
    }

    B.math_active = !fblank
        && ADD_OR_SUB_ON_ANYTHING
        && (GFX.r2130 & 0x30) != 0x30
        && !((GFX.r2130 & 0x30) == 0x10 && fr_clip[1].Count[5] == 0);
    B.msub = (GFX.r2131 & 0x80) != 0;
    B.mhalf = (GFX.r2131 & 0x40) != 0;
    backdrop = B.pal[0];
    bd_math = B.math_active && (SUB_OR_ADD(5) != 0);

    // The mathed palette depends on ScreenColors (CGRAM changes still
    // flush, giving a fresh span) and the per-line journaled fixed colour;
    // the key forces at least one rebuild per span.
    fr_mathpal_key = 0xffffffffu;

    y = starty;
    while (y <= endy) {
        int n = 1;
        int r, x;

        // Fold in the palette writes that take effect at this line, then
        // refresh what derives from the palette.
        if (ji < pjn && (uint32_t)pj[ji].line <= y) {
            do {
                B.pal[pj[ji].idx] = pj[ji].col;
                ji++;
            } while (ji < pjn && (uint32_t)pj[ji].line <= y);
            backdrop = B.pal[0];
            fr_mathpal_key = 0xffffffffu;
        }

        // Band: consecutive lines whose journal rows (scrolls and fixed
        // colour) are identical, stopping short of the next palette write.
        while (y + (uint32_t)n <= endy && n < FR_MAX_LINES &&
               (ji >= pjn || (uint32_t)pj[ji].line > y + (uint32_t)n) &&
               fixedc_j[y + n] == fixedc_j[y] &&
               memcmp(&linedata[y], &linedata[y + n],
                      sizeof(SLineData)) == 0)
            n++;

        {
            uint32_t fj = fixedc_j[y];
            B.fixedc = (uint16_t)BUILD_PIXEL(IPPU.XB[fj & 0x1f],
                                             IPPU.XB[(fj >> 8) & 0x1f],
                                             IPPU.XB[(fj >> 16) & 0x1f]);
            if (B.math_active && fr_mathpal_key != fj) {
                int i;
                fr_mathpal_key = fj;
                if (B.msub)
                    for (i = 0; i < 256; i++)
                        B.mathpal[i] = COLOR_SUB(B.pal[i], B.fixedc);
                else
                    for (i = 0; i < 256; i++)
                        B.mathpal[i] = COLOR_ADD(B.pal[i], B.fixedc);
            }
        }

        if (fblank) {
            for (r = 0; r < n; r++) {
                uint16_t *out = fr_strip_out ? B.main_px[r] :
                    (uint16_t *)(GFX.Screen + (y + (uint32_t)r) * GFX.Pitch2);
                for (x = 0; x < 256; x++)
                    out[x] = (uint16_t)BLACK; // 0 — identical byte-swapped
                if (!fr_strip_out)
                    snes_row_swapped[y + (uint32_t)r] = 1;
            }
            if (fr_strip_out)
                host_blit_rect(&B.main_px[0][0], 32, y_off + (int)y, 256, n);
            y += (uint32_t)n;
            continue;
        }

        B.y0 = (int)y;
        B.lines = n;
        B.ld = &linedata[y];
        memset(B.p1seen, 0, sizeof(B.p1seen));
        memset(B.cov, 0, (size_t)n * 256);
        fr_obj_setup(&B);

        // Sub-screen state: 1 = fixed colour everywhere, or only inside
        // the colour-window bands when one is active; sub pixels mark 2.
        if (B.math_active) {
            uint32_t cw = fr_clip[1].Count[5];
            if (cw) {
                uint32_t b;
                memset(B.subset[0], 0, 256);
                for (b = 0; b < cw; b++) {
                    uint32_t lft = fr_clip[1].Left[b][5];
                    uint32_t rgt = fr_clip[1].Right[b][5];
                    if (rgt > 256)
                        rgt = 256;
                    if (rgt > lft)
                        memset(B.subset[0] + lft, 1, rgt - lft);
                }
            } else {
                memset(B.subset[0], 1, 256);
            }
            for (r = 1; r < n; r++)
                memcpy(B.subset[r], B.subset[0], 256);
            if (ANYTHING_ON_SUB)
                fr_screen(&B, 1);
            // Classify each line's sub state so the store loops can skip
            // per-pixel checks when it is uniform (2 = sub pixel under
            // every x, 1 = fixed colour everywhere, 0 = mixed).
            for (r = 0; r < n; r++) {
                const uint32_t *w = (const uint32_t *)B.subset[r];
                uint32_t a32 = 0xffffffffu, o32 = 0;
                uint8_t a8, o8;
                for (x = 0; x < 64; x++) {
                    a32 &= w[x];
                    o32 |= w[x];
                }
                a8 = (uint8_t)(a32 & (a32 >> 8) & (a32 >> 16) & (a32 >> 24));
                o8 = (uint8_t)(o32 | (o32 >> 8) | (o32 >> 16) | (o32 >> 24));
                B.lclass[r] = (a8 == 2) ? 2 : (a8 == 1 && o8 == 1) ? 1 : 0;
            }
        } else {
            for (r = 0; r < n; r++) {
                memset(B.subset[r], 0, 256);
                B.lclass[r] = 0;
            }
        }

        fr_screen(&B, 0);

        // Fused backdrop + byte-swap writeout: one traversal per line.
        // Fully-covered lines (the gameplay majority) take a straight
        // swap-copy; gaps get the classified backdrop as they stream out.
        // Rows leave here TFT-ready, so blit_frame skips its swap pass.
        for (r = 0; r < n; r++) {
            uint16_t *out = fr_strip_out ? B.main_px[r] :
                (uint16_t *)(GFX.Screen + (y + (uint32_t)r) * GFX.Pitch2);
            const uint16_t *mrow = B.main_px[r];
            const uint8_t *covrow = B.cov[r];
            const uint32_t *cw = (const uint32_t *)covrow;
            uint32_t a32 = 0xffffffffu;
            uint16_t c;
            for (x = 0; x < 64; x++)
                a32 &= cw[x];
            a32 = a32 & (a32 >> 8) & (a32 >> 16) & (a32 >> 24);

            if ((a32 & 0xff) == 1) {
                // Two pixels per op: masked byte-swap of a uint32 pair
                // (both buffers are 4-aligned).
                const uint32_t *mw = (const uint32_t *)mrow;
                uint32_t *ow = (uint32_t *)out;
                for (x = 0; x < 128; x++) {
                    uint32_t v = mw[x];
                    ow[x] = ((v & 0xff00ff00u) >> 8) | ((v & 0x00ff00ffu) << 8);
                }
            } else if (!bd_math) {
                for (x = 0; x < 256; x++) {
                    c = covrow[x] ? mrow[x] : backdrop;
                    out[x] = (uint16_t)((c >> 8) | (c << 8));
                }
            } else if (B.lclass[r] == 2) {
                const uint16_t *subrow = B.sub_px[r];
                for (x = 0; x < 256; x++) {
                    c = covrow[x] ? mrow[x]
                                  : fr_math2(&B, backdrop, subrow[x]);
                    out[x] = (uint16_t)((c >> 8) | (c << 8));
                }
            } else if (B.lclass[r] == 1) {
                uint16_t bd1 = B.mathpal[0];
                for (x = 0; x < 256; x++) {
                    c = covrow[x] ? mrow[x] : bd1;
                    out[x] = (uint16_t)((c >> 8) | (c << 8));
                }
            } else {
                for (x = 0; x < 256; x++) {
                    c = covrow[x] ? mrow[x] : fr_math_idx(&B, r, 0, x);
                    out[x] = (uint16_t)((c >> 8) | (c << 8));
                }
            }
            if (!fr_strip_out)
                snes_row_swapped[y + (uint32_t)r] = 1;
        }
        if (fr_strip_out)
            host_blit_rect(&B.main_px[0][0], 32, y_off + (int)y, 256, n);

        y += (uint32_t)n;
    }
}
