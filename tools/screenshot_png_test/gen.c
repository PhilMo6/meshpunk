// gen.c — host harness for the firmware's streaming PNG writer.
//
// Compiles src/screenshot.cpp with SCREENSHOT_HOST_TEST (which drops the
// Arduino half and leaves only screenshot_png_write_565), renders a handful of
// 320x240 big-endian RGB565 test images, and writes each as a .png plus a .raw
// of the exact source pixels. verify.py then inflates the PNGs with zlib and
// compares them against the .raw files.
//
//   gcc -O2 -DSCREENSHOT_HOST_TEST -x c++ ../../src/screenshot.cpp gen.c -o gen
//   ./gen && python verify.py

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

bool screenshot_png_write_565(FILE* f, const uint16_t* src, int w, int h);

#define W 320
#define H 240

static uint16_t img[W * H];

// Store native RGB565 byte-swapped, matching what both firmware feeders hand
// over (LV_COLOR_16_SWAP on the LVGL side, pre-swapped palettes in modules).
static void put(int x, int y, int r8, int g8, int b8) {
    uint16_t v = (uint16_t)(((r8 & 0xF8) << 8) | ((g8 & 0xFC) << 3) | (b8 >> 3));
    img[y * W + x] = (uint16_t)((v >> 8) | (v << 8));
}

static uint32_t rng = 12345;
static int rnd(void) { rng = rng * 1103515245u + 12345u; return (int)((rng >> 16) & 0xFF); }

static void fill_flat(void) {
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) put(x, y, 0x18, 0x18, 0x18);
}

static void fill_gradient(void) {
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) put(x, y, x * 255 / (W - 1), y * 255 / (H - 1), 128);
}

static void fill_noise(void) {
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) put(x, y, rnd(), rnd(), rnd());
}

// Flat panels, a bar and some 1px text-ish detail: the shape a UI screenshot
// actually has, and the case the Sub filter plus RLE is meant to win on.
static void fill_uiish(void) {
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) put(x, y, 0x10, 0x12, 0x18);
    for (int y = 0; y < 20; y++)
        for (int x = 0; x < W; x++) put(x, y, 0x30, 0x30, 0x38);
    for (int row = 0; row < 5; row++) {
        int y0 = 30 + row * 40;
        for (int y = y0; y < y0 + 32; y++)
            for (int x = 8; x < W - 8; x++) put(x, y, 0x22, 0x24, 0x2A);
        for (int i = 0; i < 40; i++) {
            int x = 16 + i * 6, y = y0 + 12;
            for (int k = 0; k < 7; k++)
                if ((i + k) & 1) put(x, y + k, 0xE0, 0xE0, 0xE0);
        }
    }
}

// Full-scale white and black must survive the 5/6-bit round trip exactly.
static void fill_extremes(void) {
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            int band = y / 30;
            switch (band) {
                case 0: put(x, y, 255, 255, 255); break;
                case 1: put(x, y, 0, 0, 0);       break;
                case 2: put(x, y, 255, 0, 0);     break;
                case 3: put(x, y, 0, 255, 0);     break;
                case 4: put(x, y, 0, 0, 255);     break;
                case 5: put(x, y, 255, 255, 0);   break;
                case 6: put(x, y, 0, 255, 255);   break;
                default: put(x, y, 255, 0, 255);  break;
            }
        }
}

static int emit(const char* name) {
    char path[128];
    snprintf(path, sizeof(path), "%s.raw", name);
    FILE* r = fopen(path, "wb");
    if (!r) { printf("cannot write %s\n", path); return 1; }
    fwrite(img, 2, W * H, r);
    fclose(r);

    snprintf(path, sizeof(path), "%s.png", name);
    FILE* f = fopen(path, "wb");
    if (!f) { printf("cannot write %s\n", path); return 1; }
    bool ok = screenshot_png_write_565(f, img, W, H);
    long size = ftell(f);
    fclose(f);
    if (!ok) { printf("writer reported failure for %s\n", path); return 1; }
    printf("%-12s %7ld bytes  (%.1f%% of raw RGB888)\n",
           name, size, 100.0 * size / (W * H * 3.0));
    return 0;
}

int main(void) {
    int rc = 0;
    fill_flat();      rc |= emit("flat");
    fill_gradient();  rc |= emit("gradient");
    fill_noise();     rc |= emit("noise");
    fill_uiish();     rc |= emit("uiish");
    fill_extremes();  rc |= emit("extremes");
    return rc;
}
