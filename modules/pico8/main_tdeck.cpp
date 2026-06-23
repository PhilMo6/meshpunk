// Entry point for the fake-08 PICO-8 ELF module on T-Deck.
// Same pattern as Doom's main_tdeck.c: trap exit() via longjmp.

#include "fake08-src/source/vm.h"
#include "fake08-src/source/host.h"
#include "fake08-src/source/Audio.h"
#include "fake08-src/source/PicoRam.h"
#include "fake08-src/source/logger.h"

#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <setjmp.h>

extern "C" {
    extern void     host_log(const char* msg);
    extern uint32_t host_get_ticks_ms(void);
    extern void     host_sleep_ms(uint32_t ms);
    extern int      host_should_exit(void);
    extern void     host_check_heap(const char* tag);

    extern int  printf(const char*, ...);
    extern int  snprintf(char*, size_t, const char*, ...);
    extern void free(void*);
    extern void* malloc(size_t);
}

// Canary fences around PicoRam (checked every frame in TDeckHost
// waitForTargetFps): PicoRam sits in the module heap right next to Lua's
// allocations, and PICO-8 memory addresses come straight from cart code —
// any writer that runs off either end of the 64KB arena lands here first
// and is reported with the frame it happened on, instead of surfacing
// minutes later as inexplicable Lua-state corruption.
extern uint32_t* g_ram_guard_pre;    // defined in TDeckHost.cpp
extern uint32_t* g_ram_guard_post;
extern int       g_ram_guard_words;

#define RAM_GUARD_WORDS   16
#define RAM_GUARD_PATTERN 0xC0DEFA11u

struct GuardedRam {
    uint32_t pre[RAM_GUARD_WORDS];
    PicoRam  ram;
    uint32_t post[RAM_GUARD_WORDS];
};

// Jump buffer for trapping exit() calls
static jmp_buf s_exit_jmp;
static int s_exit_code = 0;

// Override exit() — fake-08 or z8lua may call this on errors.
// longjmp back to main() instead of killing the firmware.
extern "C" void exit(int code) {
    s_exit_code = code;
    longjmp(s_exit_jmp, 1);
}

extern "C" int main(int argc, char** argv) {
    host_log("pico8: module starting");

    // Set up exit trap
    if (setjmp(s_exit_jmp) != 0) {
        host_log("pico8: exit() caught, returning to launcher");
        return s_exit_code;
    }

    // Parse arguments: argv[0] = elf path, argv[1] = cart path (optional)
    const char* cartPath = nullptr;
    if (argc > 1) {
        cartPath = argv[1];
        printf("[pico8] cart: %s\n", cartPath);
    }

    // Create core objects
    Host* host = new Host(320, 240);
    GuardedRam* gram = new GuardedRam();
    for (int i = 0; i < RAM_GUARD_WORDS; i++) {
        gram->pre[i]  = RAM_GUARD_PATTERN;
        gram->post[i] = RAM_GUARD_PATTERN;
    }
    g_ram_guard_pre   = gram->pre;
    g_ram_guard_post  = gram->post;
    g_ram_guard_words = RAM_GUARD_WORDS;
    PicoRam* memory = &gram->ram;
    memory->Reset();
    Audio* audio = new Audio(memory);

    // Skip file-based logging on T-Deck — use printf/host_log instead
    // Logger_Initialize(host->logFilePrefix());

    Vm* vm = new Vm(host, memory, nullptr, nullptr, audio);

    host->setUpPaletteColors();
    host->oneTimeSetup(audio);
    host->setTargetFps(30); // PICO-8 default is 30 fps

    // Load the cart
    if (cartPath) {
        printf("[pico8] loading cart: %s\n", cartPath);
        if (!vm->LoadCart(std::string(cartPath), false)) {
            printf("[pico8] failed to load cart!\n");
            host->oneTimeCleanup();
            delete vm;
            delete host;
            return 1;
        }
    } else {
        // No cart specified — load the built-in BIOS menu
        printf("[pico8] no cart specified, loading BIOS\n");
        vm->LoadBiosCart();
    }

    vm->vm_run();

    // Main loop — GameLoop handles input, update, draw, and frame timing.
    // It calls host->shouldRunMainLoop() each frame, which checks host_should_exit().
    host_log("pico8: entering game loop");
    vm->GameLoop();

    host_log("pico8: game loop exited");

    vm->CloseCart();
    host->oneTimeCleanup();

    delete vm;
    delete host;

    host_log("pico8: module done");
    return 0;
}
