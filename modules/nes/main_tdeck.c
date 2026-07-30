// Entry point for the Nofrendo NES ELF module on T-Deck.
// Same pattern as gameboy/doom: trap exit()/abort(), launch nofrendo_main.

#include <setjmp.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include <nofrendo.h>

extern void host_log(const char *msg);
extern void host_clear_screen(void);

// ---------------------------------------------------------------------------
// Exit/abort traps
// ---------------------------------------------------------------------------
static jmp_buf s_exit_jmp;
static int s_exit_code = 0;

void exit(int code) {
    s_exit_code = code;
    longjmp(s_exit_jmp, 1);
}

void abort(void) {
    host_log("nes: abort() called");
    s_exit_code = 1;
    longjmp(s_exit_jmp, 1);
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------
int main(int argc, char **argv)
{
    host_log("nes: module starting");

    if (setjmp(s_exit_jmp) != 0) {
        host_log("nes: exit()/abort() caught, returning to launcher");
        host_clear_screen();
        return s_exit_code;
    }

    // argv[0] = ELF path, argv[1] = ROM path
    const char *rom_path = NULL;
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] != '-') {
            rom_path = argv[i];
            break;
        }
    }

    if (!rom_path) {
        host_log("nes: no ROM path given");
        return 1;
    }
    printf("[nes] rom=%s\n", rom_path);

    // Nofrendo takes the ROM path as argv[0] of its own argc/argv
    char *nf_argv[1] = { (char *)rom_path };
    nofrendo_main(1, nf_argv);

    host_clear_screen();
    host_log("nes: module done");
    return 0;
}
