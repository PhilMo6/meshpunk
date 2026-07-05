// Entry point for the Faux86-remake PC-XT ELF module on T-Deck.
// Pattern per doom/pico8/gnuboy: trap exit(), own the loop, poll input,
// yield every slice. Rendering/audio flow out of vm.simulate() through the
// TDeck host interfaces (tdeck_host.cpp).
//
// argv: [-fda img] [-fdb img] [-hda img] [-hdb img] [-boot a|c|auto]
//       [-bios path] [-vbios path] [-charrom path] [-bootrom path]
//       [-mhz N] [-audio 0|1] [-mouse 0|1|speed]
// (-keymap / -trkball are consumed by the host but remain in argv; the parser
//  skips unknown flag+value pairs.)

#include "faux86-src/VM.h"
#include "tdeck_host.h"

#include <setjmp.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <new>

using namespace Faux86;

// ---------------------------------------------------------------------------
// Host imports
// ---------------------------------------------------------------------------
extern "C" {
    void     host_clear_screen(void);
    uint32_t host_get_ticks_ms(void);
    uint32_t host_get_ticks_us(void);
    void     host_sleep_ms(uint32_t ms);
    int      host_get_key(int* pressed, unsigned char* key);
    int      host_should_exit(void);
    void     host_log(const char* msg);
    uint32_t host_psram_largest_free(void);
    // New export (firmware >= trackball-raw): drains raw trackball ISR counts.
    // First call switches the input task's trackball handling off.
    void     host_trackball_read(int* dx, int* dy, int* click);
}

// ---------------------------------------------------------------------------
// exit() trap (abort() lives in cxxstubs.cpp and routes here)
// ---------------------------------------------------------------------------
static jmp_buf s_exit_jmp;
static int s_exit_code = 0;

extern "C" void exit(int code)
{
    s_exit_code = code;
    longjmp(s_exit_jmp, 1);
}

// ---------------------------------------------------------------------------
// C++ static constructors. The ELF loader does not run .init_array; walk it
// manually. This build currently has NO .init_array section (all globals are
// constant-initialized; our own objects use placement new), so the weak
// encapsulation symbols stay undefined and the loader resolves them to NULL
// (weak-UND support added to elf_loader alongside host_trackball_read).
// NOTE: these weak refs also happen to steer this module's link around a BFD
// assertion bug in the esp-2021r2 binutils (elf32-xtensa.c:3288/3299 —
// every flag combination asserts without them). Do not remove casually.
// ---------------------------------------------------------------------------
typedef void (*init_fn_t)(void);
extern init_fn_t __init_array_start[] __attribute__((weak));
extern init_fn_t __init_array_end[]   __attribute__((weak));

static void run_static_ctors(void)
{
    if (!__init_array_start || !__init_array_end) return;
    int n = 0;
    for (init_fn_t* f = __init_array_start; f < __init_array_end; f++, n++)
        (*f)();
    printf("[pcxt] ran %d static constructors\n", n);
}

// ---------------------------------------------------------------------------
// Keyboard: host key events -> XT set-1 scancodes into InputManager.
// The launcher's -keymap remaps physical keys onto ASCII or the module
// extension codes below; unmapped printable ASCII passes through.
// ---------------------------------------------------------------------------
// Module extension codes (bindable as keymap outputs):
//   0x91..0x94  arrow Up/Down/Left/Right
//   0x96 Ctrl   0x97 Alt   0x98 Del   0x99 Tab   0x1B Esc
//   0xB0..0xB9  F1..F10
//   0x95 right mouse button, 0x85 (TrkClk) left mouse button
static uint8_t ascii_to_scan(unsigned char c)
{
    static const uint8_t letters[26] = {
        0x1E, 0x30, 0x2E, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17, 0x24, // a-j
        0x25, 0x26, 0x32, 0x31, 0x18, 0x19, 0x10, 0x13, 0x1F, 0x14, // k-t
        0x16, 0x2F, 0x11, 0x2D, 0x15, 0x2C                          // u-z
    };
    if (c >= 'a' && c <= 'z') return letters[c - 'a'];
    if (c >= 'A' && c <= 'Z') return letters[c - 'A'];
    if (c >= '1' && c <= '9') return (uint8_t)(0x02 + (c - '1'));

    switch (c) {
        case '0': return 0x0B;
        // shifted symbols -> base key (guest shift state comes from 0x80)
        case '!': return 0x02; case '@': return 0x03; case '#': return 0x04;
        case '$': return 0x05; case '%': return 0x06; case '^': return 0x07;
        case '&': return 0x08; case '*': return 0x09; case '(': return 0x0A;
        case ')': return 0x0B;
        case '-': case '_': return 0x0C;
        case '=': case '+': return 0x0D;
        case '[': case '{': return 0x1A;
        case ']': case '}': return 0x1B;
        case ';': case ':': return 0x27;
        case '\'': case '"': return 0x28;
        case '`': case '~': return 0x29;
        case '\\': case '|': return 0x2B;
        case ',': case '<': return 0x33;
        case '.': case '>': return 0x34;
        case '/': case '?': return 0x35;
        // control keys
        case 0x0D: return 0x1C; // Enter
        case 0x08: return 0x0E; // Backspace (short press; exit = 1.5s hold)
        case ' ':  return 0x39;
        case 0x09: case 0x99: return 0x0F; // Tab
        case 0x1B: return 0x01;            // Esc
        case 0x80: return 0x2A;            // T-Deck shift -> LShift
        // module extension codes
        case 0x81: case 0x91: return 0x48; // Up (trackball key mode / bound)
        case 0x82: case 0x92: return 0x50; // Down
        case 0x83: case 0x93: return 0x4B; // Left
        case 0x84: case 0x94: return 0x4D; // Right
        case 0x96: return 0x1D;            // Ctrl
        case 0x97: return 0x38;            // Alt
        case 0x98: return 0x53;            // Del
        default:
            if (c >= 0xB0 && c <= 0xB9) return (uint8_t)(0x3B + (c - 0xB0)); // F1-F10
            return 0;
    }
}

// On a PC these glyphs have no key of their own — they ARE Shift + a base key
// (! = Shift+1, : = Shift+;, ? = Shift+/, uppercase = Shift+letter …). The
// T-Deck's SYM layer hands us the finished glyph, so we must hold LShift around
// the base scancode or DOS decodes the unshifted char (! -> 1, : -> ;).
static bool char_needs_shift(unsigned char c)
{
    if (c >= 'A' && c <= 'Z') return true;
    switch (c) {
        case '!': case '@': case '#': case '$': case '%':
        case '^': case '&': case '*': case '(': case ')':
        case '_': case '+': case '{': case '}': case '|':
        case ':': case '"': case '<': case '>': case '?':
        case '~':
            return true;
    }
    return false;
}

#define XT_LSHIFT 0x2A

static void poll_keyboard(VM* vm)
{
    int pressed;
    unsigned char key;
    while (host_get_key(&pressed, &key)) {
        // Mouse buttons ride the key queue
        if (key == 0x85) {
            if (pressed) vm->mouse.handleButtonDown(SerialMouse::ButtonType::Left);
            else         vm->mouse.handleButtonUp(SerialMouse::ButtonType::Left);
            continue;
        }
        if (key == 0x95) {
            if (pressed) vm->mouse.handleButtonDown(SerialMouse::ButtonType::Right);
            else         vm->mouse.handleButtonUp(SerialMouse::ButtonType::Right);
            continue;
        }
        uint8_t scan = ascii_to_scan(key);
        if (!scan) continue;
        bool sh = char_needs_shift(key);
        if (pressed) {
            if (sh) vm->input.handleKeyDown(XT_LSHIFT);
            vm->input.handleKeyDown(scan);
        } else {
            vm->input.handleKeyUp(scan);
            if (sh) vm->input.handleKeyUp(XT_LSHIFT);
        }
    }
}

// ---------------------------------------------------------------------------
// Mouse: raw trackball deltas -> serial MS mouse
// ---------------------------------------------------------------------------
static int s_mouse_enabled = 0; // launcher opts in via -mouse N
static int s_mouse_speed = 3;   // counts multiplier

static void poll_mouse(VM* vm)
{
    if (!s_mouse_enabled) return;
    int dx = 0, dy = 0, click = 0;
    host_trackball_read(&dx, &dy, &click);
    if (click) {
        vm->mouse.handleButtonDown(SerialMouse::ButtonType::Left);
        vm->mouse.handleButtonUp(SerialMouse::ButtonType::Left);
    }
    if (dx || dy) {
        int mx = dx * s_mouse_speed;
        int my = dy * s_mouse_speed;
        while (mx || my) {
            int cx = mx; if (cx > 127) cx = 127; if (cx < -127) cx = -127;
            int cy = my; if (cy > 127) cy = 127; if (cy < -127) cy = -127;
            vm->mouse.handleMove((int8_t)cx, (int8_t)cy);
            mx -= cx; my -= cy;
        }
    }
}

// Periodic CPU progress line + real-time timing kick — live in tdeck_host.cpp;
// this TU's link is the fragile one (BFD elf32-xtensa assertion re-trips on
// code growth here).
extern "C" void pcxt_cpu_status(Faux86::VM* vm, uint32_t now_us, uint32_t loops);
extern "C" void pcxt_timing_kick(Faux86::VM* vm);
extern "C" void pcxt_phase(uint32_t sim, uint32_t kick, uint32_t poll, uint32_t pump);

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------
int main(int argc, char** argv)
{
    host_log("pcxt: module starting");

    if (setjmp(s_exit_jmp) != 0) {
        host_log("pcxt: exit()/abort() caught, returning to launcher");
        return s_exit_code;
    }

    run_static_ctors();

    // --- Parse arguments ---
    const char* fda = nullptr;
    const char* fdb = nullptr;
    const char* hda = nullptr;
    const char* hdb = nullptr;
    const char* bios_path = nullptr;
    const char* vbios_path = nullptr;
    const char* charrom_path = nullptr;
    const char* bootrom_path = nullptr;
    const char* boot = "auto";
    int mhz = 12;
    int audio_on = 1;

    for (int i = 1; i < argc; i++) {
        if      (!strcmp(argv[i], "-fda")     && i + 1 < argc) fda = argv[++i];
        else if (!strcmp(argv[i], "-fdb")     && i + 1 < argc) fdb = argv[++i];
        else if (!strcmp(argv[i], "-hda")     && i + 1 < argc) hda = argv[++i];
        else if (!strcmp(argv[i], "-hdb")     && i + 1 < argc) hdb = argv[++i];
        else if (!strcmp(argv[i], "-bios")    && i + 1 < argc) bios_path = argv[++i];
        else if (!strcmp(argv[i], "-vbios")   && i + 1 < argc) vbios_path = argv[++i];
        else if (!strcmp(argv[i], "-charrom") && i + 1 < argc) charrom_path = argv[++i];
        else if (!strcmp(argv[i], "-bootrom") && i + 1 < argc) bootrom_path = argv[++i];
        else if (!strcmp(argv[i], "-boot")    && i + 1 < argc) boot = argv[++i];
        else if (!strcmp(argv[i], "-mhz")     && i + 1 < argc) mhz = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-audio")   && i + 1 < argc) audio_on = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-mouse")   && i + 1 < argc) {
            s_mouse_speed = atoi(argv[++i]);
            s_mouse_enabled = (s_mouse_speed > 0);
            if (s_mouse_speed <= 0) s_mouse_speed = 1;
        }
        else if (argv[i][0] == '-') { if (i + 1 < argc) i++; } // -keymap/-trkball etc.
    }

    // Default ROM paths: same directory as the module ELF (argv[0], which
    // arrives in firmware "L:/..."/"S:/..." form — convert to VFS). Keeps the
    // launch under the host's ELF_MAX_ARGS=16 argv limit.
    static char dir_buf[256];
    static char bios_buf[256], vbios_buf[256], char_buf[256], bootrom_buf[256];
    if (argc > 0 && (!bios_path || !vbios_path || !charrom_path)) {
        const char* self = argv[0];
        const char* prefix = "";
        if (self[0] == 'L' && self[1] == ':')      { prefix = "/littlefs"; self += 2; }
        else if (self[0] == 'S' && self[1] == ':') { prefix = "/sd";       self += 2; }
        snprintf(dir_buf, sizeof(dir_buf), "%s%s", prefix, self);
        char* slash = strrchr(dir_buf, '/');
        if (slash) *slash = 0;
        if (!bios_path) {
            snprintf(bios_buf, sizeof(bios_buf), "%s/pcxtbios.bin", dir_buf);
            bios_path = bios_buf;
        }
        if (!vbios_path) {
            snprintf(vbios_buf, sizeof(vbios_buf), "%s/videorom.bin", dir_buf);
            vbios_path = vbios_buf;
        }
        if (!charrom_path) {
            snprintf(char_buf, sizeof(char_buf), "%s/asciivga.dat", dir_buf);
            charrom_path = char_buf;
        }
        if (!bootrom_path) {
            snprintf(bootrom_buf, sizeof(bootrom_buf), "%s/rombasic.bin", dir_buf);
            bootrom_path = bootrom_buf;
        }
    }
    if (!bios_path || !vbios_path || !charrom_path) {
        host_log("pcxt: missing -bios/-vbios/-charrom paths");
        return 1;
    }
    if (!fda && !hda) {
        host_log("pcxt: no bootable disk image (-fda or -hda)");
        return 1;
    }
    printf("[pcxt] fda=%s hda=%s boot=%s mhz=%d audio=%d mouse=%d psram=%u\n",
           fda ? fda : "-", hda ? hda : "-", boot, mhz, audio_on,
           s_mouse_enabled ? s_mouse_speed : 0,
           (unsigned)host_psram_largest_free());

    // --- Host interface + config (placement-new: no global ctors) ---
    alignas(8) static char host_storage[sizeof(TDeckHostInterface)];
    TDeckHostInterface* host_if = new (host_storage) TDeckHostInterface();

    alignas(8) static char cfg_storage[sizeof(Config)];
    Config* cfg = new (cfg_storage) Config(host_if);

    cfg->singleThreaded = true;
    cfg->enableAudio = (audio_on != 0);
    cfg->enableConsole = false;
    cfg->enableMenu = false;
    cfg->enableDebugger = false;
    cfg->verbose = false;
    cfg->audio.sampleRate = 22050;
    cfg->audio.latency = 140;
    cfg->cpuSpeed = (uint32_t)mhz;
    cfg->cpuTiming = TIMING_INTERVAL;
    // Draw cadence. The software VGA render runs synchronously inside the CPU
    // loop, so the draw interval MUST stay above the render cost or draws pile
    // up and starve the CPU (observed: 242k instr/s collapsing to 4k once
    // drawing began). frameDelay is a uint8_t; drawticks = hostFreq/(1000/fd),
    // so the longest interval it can express is fd=255 => 1000/255=3 =>
    // ~333ms (~3fps), the safest choice here. TODO: renderer optimization
    // (dirty regions / Core-1 offload) to lift the fps ceiling.
    cfg->frameDelay = 255;
    cfg->framebuffer.width = 720;  // large enough for Hercules 720x348
    cfg->framebuffer.height = 480; // and VGA 640x480

    cfg->loadBiosRom(bios_path);
    cfg->loadVideoRom(vbios_path);
    cfg->loadCharRom(charrom_path);
    if (bootrom_path) cfg->loadBootRom(bootrom_path);

    if (fda) cfg->loadFD0(fda);
    if (fdb) cfg->loadFD1(fdb);
    if (hda) cfg->loadHD0(hda);
    if (hdb) cfg->loadHD1(hdb);

    if      (!strcmp(boot, "a")) cfg->bootDrive = DRIVE_A;
    else if (!strcmp(boot, "c")) cfg->bootDrive = DRIVE_C;
    else                         cfg->bootDrive = 254; // auto: C, then A

    // --- Bring up the VM ---
    VM* vm = new VM(*cfg);
    host_if->init(vm);
    if (!vm->init()) {
        host_log("pcxt: VM init failed (see log above)");
        delete vm;
        return 1;
    }

    host_clear_screen();
    host_log("pcxt: entering main loop");

    pcxt_cpu_status(vm, host_get_ticks_us(), 0); // arm the baseline

    // --- Main loop: simulate flat-out, yield every ~8ms for WDT/Core1 ---
    uint32_t last_yield_us = host_get_ticks_us();
    uint32_t loops = 0;

    while (!host_should_exit() && vm->running) {
        uint32_t p0 = host_get_ticks_us();
        vm->simulate();
        uint32_t p1 = host_get_ticks_us();
        pcxt_timing_kick(vm); // keep PIT/draw alive while the CPU is halted
        uint32_t p2 = host_get_ticks_us();
        poll_keyboard(vm);
        poll_mouse(vm);
        uint32_t p3 = host_get_ticks_us();
        host_if->audio().pump();
        uint32_t p4 = host_get_ticks_us();
        pcxt_phase(p1 - p0, p2 - p1, p3 - p2, p4 - p3);
        loops++;

        uint32_t now = host_get_ticks_us();
        if ((uint32_t)(now - last_yield_us) > 8000) {
            host_sleep_ms(1);
            last_yield_us = host_get_ticks_us();
            pcxt_cpu_status(vm, now, loops);
        }
    }

    // --- Teardown: flush disk images, drop the VM ---
    printf("[pcxt] exiting after %u loops\n", (unsigned)loops);
    delete vm;              // DriveManager/Config own no files...
    delete cfg->diskDriveA; // ...FileDisks flush+close in their destructor
    delete cfg->diskDriveB;
    delete cfg->diskDriveC;
    delete cfg->diskDriveD;
    delete cfg->biosFile;
    delete cfg->videoRomFile;
    delete cfg->asciiFile;
    delete cfg->romBasicFile;

    host_clear_screen();
    host_log("pcxt: module done");
    return 0;
}
