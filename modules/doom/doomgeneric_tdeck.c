// doomgeneric platform implementation for T-Deck ESP32-S3.
// Runs as a loaded ELF module — all hardware access goes through host functions.

#include "doomgeneric-src/doomgeneric/doomgeneric.h"
#include "doomgeneric-src/doomgeneric/doomkeys.h"

#include <stdint.h>
#include <string.h>

// Host functions — resolved at load time
extern void     host_blit_frame(const uint16_t* rgb565, int w, int h);
extern void     host_clear_screen(void);
extern uint32_t host_get_ticks_ms(void);
extern void     host_sleep_ms(uint32_t ms);
extern int      host_get_key(int* pressed, unsigned char* key);
extern int      host_should_exit(void);
extern void     host_log(const char* msg);
extern void*    host_read_file(const char* path, uint32_t* out_size);
extern void     host_audio_push(const int16_t* samples, int count, int sample_rate);

// Provided by libc via host exports
extern int  printf(const char*, ...);
extern int  snprintf(char*, unsigned int, const char*, ...);
extern void free(void*);

// Audio pump (i_tdeck_sound.c) — refills the firmware ring mid-frame.
extern void I_TDeck_AudioPump(void);

// RGB565 conversion buffer (320x200 = 64000 pixels × 2 bytes = 128KB)
static uint16_t rgb565_buf[DOOMGENERIC_RESX * DOOMGENERIC_RESY];

void DG_Init(void) {
    host_log("DG_Init: T-Deck platform ready");
}

void DG_DrawFrame(void) {
    // Convert XRGB8888 → RGB565 and blit
    pixel_t* src = DG_ScreenBuffer;
    uint16_t* dst = rgb565_buf;
    int count = DOOMGENERIC_RESX * DOOMGENERIC_RESY;

    for (int i = 0; i < count; i++) {
        uint32_t px = src[i];
        uint8_t r = (px >> 16) & 0xFF;
        uint8_t g = (px >> 8) & 0xFF;
        uint8_t b = px & 0xFF;
        // RGB565: RRRRRGGG GGGBBBBB — byte-swapped for SPI (big-endian)
        uint16_t c = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
        dst[i] = (c >> 8) | (c << 8); // byte-swap for TFT_eSPI
    }

    host_blit_frame(rgb565_buf, DOOMGENERIC_RESX, DOOMGENERIC_RESY);

    // Rendering + blit is the frame's long pole; top the audio ring back
    // up here so it isn't drained by the time the next frame's
    // S_UpdateSounds pump comes around.
    I_TDeck_AudioPump();
}

void DG_SleepMs(uint32_t ms) {
    host_sleep_ms(ms);
}

uint32_t DG_GetTicksMs(void) {
    // Yield periodically so the watchdog doesn't fire during long
    // init phases (WAD loading, texture building) where the main
    // loop's yield hasn't been reached yet.
    static uint32_t last_yield = 0;
    uint32_t now = host_get_ticks_ms();
    if (now - last_yield > 100) {
        host_sleep_ms(1);
        last_yield = now;
    }
    return now;
}

int DG_GetKey(int* pressed, unsigned char* doomKey) {
    return host_get_key(pressed, doomKey);
}

void DG_SetWindowTitle(const char* title) {
    (void)title;
}
