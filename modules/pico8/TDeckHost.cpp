// T-Deck platform implementation for fake-08.
// Bridges the Host class to the meshpunk firmware's host_* ABI.
// No SDL, no OS — just raw host exports.

#include "fake08-src/source/host.h"
#include "fake08-src/source/hostVmShared.h"
#include "fake08-src/source/nibblehelpers.h"
#include "fake08-src/source/logger.h"
#include "fake08-src/source/filehelpers.h"
#include "fake08-src/source/Audio.h"

#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <string>
#include <vector>
#include <sys/stat.h>

// ---------------------------------------------------------------------------
// Host function imports (resolved at ELF load time by elf_host.cpp)
// ---------------------------------------------------------------------------
extern "C" {
    void     host_blit_frame(const uint16_t* rgb565, int w, int h);
    void     host_blit_frame_async(const uint16_t* rgb565, int w, int h);
    void     host_clear_screen(void);
    uint32_t host_get_ticks_ms(void);
    uint32_t host_get_ticks_us(void);
    void     host_sleep_ms(uint32_t ms);
    int      host_get_key(int* pressed, unsigned char* key);
    int      host_should_exit(void);
    void     host_log(const char* msg);
    void     host_check_heap(const char* tag);
    void*    host_read_file(const char* path, uint32_t* out_size);
    int      host_write_file(const char* path, const void* data, uint32_t size);
    void     host_audio_set_pull(void (*cb)(int16_t* out, int count), int sample_rate);

    extern int printf(const char*, ...);
    extern void free(void*);
    extern void* malloc(size_t);
}

// ── Corruption detectors (canary layout in main_tdeck.cpp) ──────────────────
// Set by main() after allocating the guarded PicoRam; checked every frame
// from waitForTargetFps. A trip names the frame it happened on instead of
// letting an arena overrun surface minutes later as broken Lua state.
uint32_t* g_ram_guard_pre   = nullptr;
uint32_t* g_ram_guard_post  = nullptr;
int       g_ram_guard_words = 0;

static uint32_t s_frame_total = 0;

static void check_ram_guards(void) {
    if (!g_ram_guard_pre) return;
    for (int side = 0; side < 2; side++) {
        uint32_t* g = side ? g_ram_guard_post : g_ram_guard_pre;
        for (int i = 0; i < g_ram_guard_words; i++) {
            if (g[i] != 0xC0DEFA11u) {
                printf("[ramguard] PicoRam overrun! %s[%d]=%08x frame=%u\n",
                       side ? "post" : "pre", i, (unsigned)g[i],
                       (unsigned)s_frame_total);
                g[i] = 0xC0DEFA11u;  // re-arm to catch repeats
                host_check_heap("ramguard-trip");
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Display constants
// ---------------------------------------------------------------------------
#define PICO_W 128
#define PICO_H 128

// We scale 128x128 to 240x240 (nearest-neighbor ~1.875x); the host centers it
// on the 320x240 panel. Double-buffered: host_blit_frame_async() pushes one
// buffer over SPI from a Core-1 task while the next frame renders into the
// other, so the ~12-30 ms SPI push no longer blocks the game loop.
#define FB_SIZE 240

static uint16_t s_framebuf[2][FB_SIZE * FB_SIZE];
static int s_fb_idx = 0;
static uint16_t s_palette565[144];

// Pull-model audio: the firmware's sound task (Core 1) calls audio_pull_cb
// for exactly the mono samples the I2S pipeline needs, so the synth runs
// off the game loop entirely (it was 13-15 ms of every Core-0 frame, and
// wall-clock pacing still underran at ~21.0k of 22050 smp/s). The game loop
// keeps mutating audio state through api_sfx()/api_music() unlocked — same
// arrangement as fake-08's threaded-audio ports; everything those calls
// touch is fixed-size arrays inside PicoRam/audioState_t, so a torn read
// costs one mixer chunk (~6 ms) of slightly-off samples, never a crash.
#define AUDIO_SAMPLE_RATE   22050
static Audio* s_audio_obj = nullptr;
static void audio_pull_cb(int16_t* out, int count);

// Audio experiment mode — single digit read from /sd/p8carts/audiomode.txt
// at launch (missing/invalid file = 0). Consumed by Audio.cpp's fade and
// phase paths so synth A/B tests don't need rebuilds:
//   0 stock     — upstream behavior (130/s declick fade everywhere)
//   1 noisefast — noise->noise transitions fade at 600/s (anti-"echo")
//   2 fastfade  — 600/s everywhere (known: adds static to dense music)
//   3 noisefast + phase-lockstep fmod on harsh transitions
int g_p8_audio_mode = 0;

// Timing
static uint32_t s_last_frame_ms = 0;
static uint32_t s_target_frame_ms = 16; // ~60 fps

// Input state — accumulated from host_get_key polling
static uint8_t s_keys_down = 0;
static uint8_t s_keys_held = 0;

// ---------------------------------------------------------------------------
// RGB888 → RGB565 (byte-swapped for SPI TFT)
// ---------------------------------------------------------------------------
static inline uint16_t rgb565_swap(uint8_t r, uint8_t g, uint8_t b) {
    uint16_t c = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
    return (c >> 8) | (c << 8); // byte-swap for TFT_eSPI
}

// ---------------------------------------------------------------------------
// Host class implementation
// ---------------------------------------------------------------------------

Host::Host(int windowWidth, int windowHeight) {
    _cartDirectory = "/sd/p8carts";
    _logFilePrefix = "/sd/p8carts/";
    _customBiosLua = "";
}

void Host::setPlatformParams(
    int windowWidth, int windowHeight,
    uint32_t sdlWindowFlags, uint32_t sdlRendererFlags,
    uint32_t sdlPixelFormat,
    std::string logFilePrefix, std::string customBiosLua,
    std::string cartDirectory)
{
    _logFilePrefix = logFilePrefix;
    _customBiosLua = customBiosLua;
    _cartDirectory = cartDirectory;
}

void Host::oneTimeSetup(Audio* audio) {
    host_log("pico8: oneTimeSetup");

    // Build the 565 palette from the PICO-8 colors
    for (int i = 0; i < 144; i++) {
        s_palette565[i] = rgb565_swap(
            _paletteColors[i].Red,
            _paletteColors[i].Green,
            _paletteColors[i].Blue);
    }

    // Clear framebuffers
    memset(s_framebuf, 0, sizeof(s_framebuf));
    host_clear_screen();

    s_last_frame_ms = host_get_ticks_ms();

    g_p8_audio_mode = 0;
    {
        uint32_t sz = 0;
        void* data = host_read_file("/sd/p8carts/audiomode.txt", &sz);
        if (data) {
            const char* s = (const char*)data;
            if (sz > 0 && s[0] >= '0' && s[0] <= '3')
                g_p8_audio_mode = s[0] - '0';
            free(data);
        }
    }
    printf("[pico8] audio mode %d\n", g_p8_audio_mode);

    // Hand the synth to the firmware's sound task. Until the cart finishes
    // loading the channels are all off, so it renders silence.
    s_audio_obj = audio;
    host_audio_set_pull(audio_pull_cb, AUDIO_SAMPLE_RATE);
}

void Host::oneTimeCleanup() {
    // Unregister first: returns only once the mixer is outside the callback,
    // after which main() may safely delete the Audio object.
    host_audio_set_pull(nullptr, 0);
    s_audio_obj = nullptr;
    host_clear_screen();
    host_log("pico8: oneTimeCleanup");
}

void Host::setTargetFps(int targetFps) {
    if (targetFps > 0)
        s_target_frame_ms = 1000 / targetFps;
    else
        s_target_frame_ms = 16;
}

bool Host::shouldRunMainLoop() {
    return !host_should_exit();
}

bool Host::shouldQuit() {
    return host_should_exit() != 0;
}

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------

// Map key codes to PICO-8 button bits.
// When the launcher uses keymap mode, physical keys are translated to these
// canonical codes by the host before they arrive here. When in passthrough
// mode, raw key codes arrive directly.
static uint8_t mapKeyToP8(unsigned char key) {
    switch (key) {
        // Trackball pseudo-codes (always these values from the host)
        case 0x83: return P8_KEY_LEFT;
        case 0x84: return P8_KEY_RIGHT;
        case 0x81: return P8_KEY_UP;
        case 0x82: return P8_KEY_DOWN;
        case 0x85: return P8_KEY_O;     // trackball click
        // WASD — canonical direction keys (and keymap output targets)
        case 'a': return P8_KEY_LEFT;
        case 'd': return P8_KEY_RIGHT;
        case 'w': return P8_KEY_UP;
        case 's': return P8_KEY_DOWN;
        // O and X buttons
        case 'z': case 'n': return P8_KEY_O;
        case 'x': case 'm': case ' ':  return P8_KEY_X;
        // Pause
        case 0x0D: return P8_KEY_PAUSE; // Enter
        case 'p':  return P8_KEY_PAUSE;
        default: return 0;
    }
}

InputState_t Host::scanInput() {
    s_keys_down = 0;
    // s_keys_held persists across frames — set on press, cleared on release

    int pressed;
    unsigned char key;
    while (host_get_key(&pressed, &key)) {
        uint8_t p8 = mapKeyToP8(key);
        if (p8) {
            if (pressed) {
                s_keys_down |= p8;
                s_keys_held |= p8;   // set on press
            } else {
                s_keys_held &= ~p8;  // clear on release
            }
        }
    }

    InputState_t state = {};
    state.KDown = s_keys_down;
    state.KHeld = s_keys_held;
    state.mouseX = 0;
    state.mouseY = 0;
    state.mouseBtnState = 0;
    state.KBdown = false;
    state.KBkey = "";
    return state;
}

// ---------------------------------------------------------------------------
// Display — scale 128x128 PICO-8 framebuffer to 240x240 centered on 320x240
// ---------------------------------------------------------------------------

void Host::drawFrame(uint8_t* picoFb, uint8_t* screenPaletteMap, uint8_t drawMode) {
    uint16_t* fb = s_framebuf[s_fb_idx];

    // Per-frame palette: nibble (0-15) -> swapped RGB565 via the screen map
    uint16_t pal[16];
    for (int c = 0; c < 16; c++)
        pal[c] = s_palette565[screenPaletteMap[c] & 0x8f];

    // Output-column -> source-column map for the 128 -> 240 expansion
    static uint8_t sx_map[FB_SIZE];
    static bool sx_init = false;
    if (!sx_init) {
        for (int ox = 0; ox < FB_SIZE; ox++)
            sx_map[ox] = (uint8_t)(ox * PICO_W / FB_SIZE);
        sx_init = true;
    }

    // Nearest-neighbor scale. Each source row is decoded once (2 px/byte,
    // even x = low nibble) and expanded via sx_map; of the 240 output rows
    // 112 are duplicates of the previous row and are memcpy'd instead.
    int prev_sy = -1;
    const uint16_t* prev_row = nullptr;
    for (int oy = 0; oy < FB_SIZE; oy++) {
        uint16_t* row = &fb[oy * FB_SIZE];
        int sy = oy * PICO_H / FB_SIZE;
        if (sy == prev_sy) {
            memcpy(row, prev_row, FB_SIZE * sizeof(uint16_t));
        } else {
            prev_sy = sy;
            uint16_t line[PICO_W];
            const uint8_t* src = &picoFb[sy * (PICO_W / 2)];
            for (int b = 0; b < PICO_W / 2; b++) {
                uint8_t v = src[b];
                line[b * 2]     = pal[v & 0x0f];
                line[b * 2 + 1] = pal[v >> 4];
            }
            for (int ox = 0; ox < FB_SIZE; ox++)
                row[ox] = line[sx_map[ox]];
        }
        prev_row = row;
    }

    // Hand the finished buffer to the Core-1 blit task and flip; only waits
    // if the previous frame's SPI push is still in flight.
    host_blit_frame_async(fb, FB_SIZE, FB_SIZE);
    s_fb_idx ^= 1;
}

// ---------------------------------------------------------------------------
// Timing
// ---------------------------------------------------------------------------

void Host::waitForTargetFps() {
    s_frame_total++;
    check_ram_guards();


    uint32_t now = host_get_ticks_ms();
    uint32_t elapsed = now - s_last_frame_ms;

    if (elapsed < s_target_frame_ms) {
        host_sleep_ms(s_target_frame_ms - elapsed);
    }

    s_last_frame_ms = host_get_ticks_ms();
}

double Host::deltaTMs() {
    uint32_t now = host_get_ticks_ms();
    double dt = (double)(now - s_last_frame_ms);
    return dt;
}

// ---------------------------------------------------------------------------
// Audio
// ---------------------------------------------------------------------------

// Runs on the firmware's sound task (Core 1), under its mixer mutex.
// Renders straight into the mixer's buffer — no intermediate copies, no
// pacing: the I2S pipeline's own consumption is the clock now. Must not
// block or allocate (the host's module-alloc tracker is single-task).
static void audio_pull_cb(int16_t* out, int count) {
    s_audio_obj->FillMonoAudioBuffer(out, 0, (size_t)count);
}

// The GameLoop fill path is dead with pull-model audio registered.
bool Host::shouldFillAudioBuff() {
    return false;
}

void* Host::getAudioBufferPointer() {
    return nullptr;
}

size_t Host::getAudioBufferSize() {
    return 0;
}

void Host::playFilledAudioBuffer() {}

// ---------------------------------------------------------------------------
// Filesystem
// ---------------------------------------------------------------------------

std::vector<std::string> Host::listcarts() {
    // The launcher passes the cart path directly via argv.
    // For the built-in BIOS menu, return an empty list — we skip it.
    return std::vector<std::string>();
}

std::vector<std::string> Host::listdirs() {
    return std::vector<std::string>();
}

std::string Host::getCartDirectory() {
    return _cartDirectory;
}

void Host::setCartDirectory(std::string cartDirectory) {
    _cartDirectory = cartDirectory;
}

void Host::overrideLogFilePrefix(const char* newPrefix) {
    _logFilePrefix = std::string(newPrefix);
}

const char* Host::logFilePrefix() {
    return _logFilePrefix.c_str();
}

std::string Host::customBiosLua() {
    return _customBiosLua;
}

std::string Host::getCartDataFileContents(std::string cartDataKey) {
    std::string path = getCartDataFile(cartDataKey);
    return get_file_contents(path);
}

std::string Host::getCartDataFile(std::string cartDataKey) {
    return _logFilePrefix + "cdata/" + cartDataKey + ".p8d.txt";
}

void Host::saveCartData(std::string cartDataKey, std::string contents) {
    std::string path = getCartDataFile(cartDataKey);
    host_write_file(path.c_str(), contents.c_str(), contents.size());
}

size_t Host::getFileContents(std::string fileName, char* buffer) {
    uint32_t size = 0;
    void* data = host_read_file(fileName.c_str(), &size);
    if (data && size > 0) {
        memcpy(buffer, data, size);
        free(data);
    }
    return size;
}

void Host::writeBufferToFile(std::string path, char* buffer, size_t length) {
    host_write_file(path.c_str(), buffer, length);
}

// ---------------------------------------------------------------------------
// Settings (stub — we don't persist settings on T-Deck for now)
// ---------------------------------------------------------------------------

int Host::getSetting(std::string sname) {
    return 0;
}

void Host::setSetting(std::string sname, int sdata) {
    // no-op
}

// ---------------------------------------------------------------------------
// Stretch (not applicable — we have a fixed scale)
// ---------------------------------------------------------------------------

void Host::changeStretch() {}
void Host::forceStretch(StretchOption newStretch) {}

// ---------------------------------------------------------------------------
// Palette setup (from hostCommonFunctions.cpp)
// ---------------------------------------------------------------------------

void Host::setUpPaletteColors() {
    _paletteColors[0]  = COLOR_00;
    _paletteColors[1]  = COLOR_01;
    _paletteColors[2]  = COLOR_02;
    _paletteColors[3]  = COLOR_03;
    _paletteColors[4]  = COLOR_04;
    _paletteColors[5]  = COLOR_05;
    _paletteColors[6]  = COLOR_06;
    _paletteColors[7]  = COLOR_07;
    _paletteColors[8]  = COLOR_08;
    _paletteColors[9]  = COLOR_09;
    _paletteColors[10] = COLOR_10;
    _paletteColors[11] = COLOR_11;
    _paletteColors[12] = COLOR_12;
    _paletteColors[13] = COLOR_13;
    _paletteColors[14] = COLOR_14;
    _paletteColors[15] = COLOR_15;

    for (int i = 16; i < 128; i++) {
        _paletteColors[i] = {0, 0, 0, 0};
    }

    _paletteColors[128] = COLOR_128;
    _paletteColors[129] = COLOR_129;
    _paletteColors[130] = COLOR_130;
    _paletteColors[131] = COLOR_131;
    _paletteColors[132] = COLOR_132;
    _paletteColors[133] = COLOR_133;
    _paletteColors[134] = COLOR_134;
    _paletteColors[135] = COLOR_135;
    _paletteColors[136] = COLOR_136;
    _paletteColors[137] = COLOR_137;
    _paletteColors[138] = COLOR_138;
    _paletteColors[139] = COLOR_139;
    _paletteColors[140] = COLOR_140;
    _paletteColors[141] = COLOR_141;
    _paletteColors[142] = COLOR_142;
    _paletteColors[143] = COLOR_143;
}

Color* Host::GetPaletteColors() {
    return _paletteColors;
}

void Host::unpackCarts() {
    // no-op on T-Deck
}

void Host::loadSettingsIni() {
    // no-op — we don't persist settings
}

void Host::saveSettingsIni() {
    // no-op
}
