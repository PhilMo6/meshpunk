// Minimal test module for the ELF loader.
// Draws a colored rectangle on screen via host_blit_frame(), then exits.

#include <stdint.h>
#include <stddef.h>

// Host functions — resolved at load time via symbol table
extern void     host_blit_frame(const uint16_t* rgb565, int w, int h);
extern void     host_clear_screen(void);
extern uint32_t host_get_ticks_ms(void);
extern void     host_sleep_ms(uint32_t ms);
extern int      host_get_key(int* pressed, unsigned char* key);
extern int      host_should_exit(void);
extern void     host_log(const char* msg);

// RGB565 color helpers
#define RGB565(r,g,b) ((uint16_t)(((r)&0xF8)<<8 | ((g)&0xFC)<<3 | ((b)>>3)))

static uint16_t framebuf[320 * 200];

static void fill_rect(int x, int y, int w, int h, uint16_t color) {
    for (int row = y; row < y + h && row < 200; row++) {
        for (int col = x; col < x + w && col < 320; col++) {
            framebuf[row * 320 + col] = color;
        }
    }
}

int main(int argc, char** argv) {
    host_log("test_elf: starting");

    // Draw a test pattern: blue background, red/green/white rectangles
    uint16_t blue  = RGB565(0, 0, 128);
    uint16_t red   = RGB565(255, 0, 0);
    uint16_t green = RGB565(0, 255, 0);
    uint16_t white = RGB565(255, 255, 255);

    // Fill background blue
    for (int i = 0; i < 320 * 200; i++) framebuf[i] = blue;

    // Draw colored rectangles
    fill_rect(20, 20, 80, 60, red);
    fill_rect(120, 20, 80, 60, green);
    fill_rect(220, 20, 80, 60, white);

    // Draw "ELF OK" text approximation (big block letters)
    fill_rect(60, 110, 60, 10, white);  // E top
    fill_rect(60, 110, 10, 70, white);  // E left
    fill_rect(60, 140, 40, 10, white);  // E mid
    fill_rect(60, 170, 60, 10, white);  // E bottom

    fill_rect(140, 110, 10, 70, white); // L left
    fill_rect(140, 170, 60, 10, white); // L bottom

    fill_rect(220, 110, 60, 10, white); // F top
    fill_rect(220, 110, 10, 70, white); // F left
    fill_rect(220, 140, 40, 10, white); // F mid

    host_blit_frame(framebuf, 320, 200);
    host_log("test_elf: frame drawn, waiting for exit");

    // Wait for ESC hold or 10 seconds
    uint32_t start = host_get_ticks_ms();
    while (!host_should_exit()) {
        int pressed;
        unsigned char key;
        while (host_get_key(&pressed, &key)) {
            // drain key queue
        }
        host_sleep_ms(50);
        if (host_get_ticks_ms() - start > 10000) break;
    }

    host_log("test_elf: exiting");
    return 0;
}
