// Entry point for the Doom ELF module on T-Deck.
// Wraps doomgeneric_Create/Tick and traps exit() so it doesn't kill the firmware.

#include "doomgeneric-src/doomgeneric/doomgeneric.h"
#include <setjmp.h>

// Host functions
extern int      host_should_exit(void);
extern void     host_log(const char* msg);
extern uint32_t host_get_ticks_ms(void);
extern void     host_sleep_ms(uint32_t ms);

// Jump buffer for trapping exit() calls from Doom
static jmp_buf exit_jmp;
static int exit_code_val = 0;

// Override exit() — Doom calls this on quit or fatal error.
// We longjmp back to main() instead of killing the firmware.
void exit(int code) {
    exit_code_val = code;
    longjmp(exit_jmp, 1);
}

// Vendored code references abort() (dbopl.c defensive default: branches,
// never reached in practice); the host exports no abort, so trap it too.
void abort(void) {
    host_log("doom: abort() called");
    exit(1);
}

int main(int argc, char** argv) {
    host_log("doom: module starting");

    // Set up exit trap
    if (setjmp(exit_jmp) != 0) {
        // Returned here from exit() call
        host_log("doom: exit() caught, returning to launcher");
        return exit_code_val;
    }

    // Initialize Doom
    doomgeneric_Create(argc, argv);

    // Main loop — yield between frames so Core 1 (mesh/radio) gets
    // SPI bus time and doesn't starve.
    while (!host_should_exit()) {
        doomgeneric_Tick();
        host_sleep_ms(1); // yield to Core 1
    }

    host_log("doom: user requested exit");
    return 0;
}
