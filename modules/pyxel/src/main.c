// Pyxel game player: ELF module entry point.
//
// argv: <elf> <game> [-pylib DIR] [-heapkb N] [firmware args...]
//   game   a .pyxapp/.zip, or a startup .py script
//   -pylib directory holding pyxel.py and the stdlib shims
//          (default: "pylib" next to the ELF)
//   -heapkb MicroPython heap size (default 1024)
//   -sound 0 turns sound synthesis off (sequencing still keeps time)
//   -kbtoggle N (firmware arg) makes Alt+Enter switch the key bindings off
//          for typing; input() then shows that on its prompt bar
//   -clock E,M the device clock at launch: E UTC epoch seconds, M the UTC
//          offset in minutes (pylib time and datetime)

#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "py/compile.h"
#include "py/cstack.h"
#include "py/gc.h"
#include "py/lexer.h"
#include "py/mphal.h"
#include "py/parse.h"
#include "py/runtime.h"
#include "shared/runtime/gchelper.h"

#include "px.h"

void px_api_shutdown(void);

bool px_sound_enabled = true;
bool px_kbtoggle = false;
int64_t px_clock_epoch = 0;
int px_clock_tzmin = 0;

// Headroom kept below the recursion limit: the firmware may hand this task a
// 32KB stack when a 64KB one does not fit.
#define PX_CSTACK_LIMIT (26 * 1024)
#define PX_DEFAULT_HEAP_KB 1024

// ---------------------------------------------------------------------------
// exit()/abort() traps (same pattern as the other emulator modules)
// ---------------------------------------------------------------------------
static jmp_buf s_exit_jmp;
static int s_exit_code;

void exit(int code) {
    s_exit_code = code;
    longjmp(s_exit_jmp, 1);
}

void abort(void) {
    host_log("pyxel: abort() called");
    s_exit_code = 1;
    longjmp(s_exit_jmp, 1);
}

// ---------------------------------------------------------------------------
// MicroPython port functions
// ---------------------------------------------------------------------------
void mp_hal_stdout_tx_strn_cooked(const char* str, size_t len) {
    px_console_write(str, len);
}

// ---------------------------------------------------------------------------
// Console record, as a terminal would show it: '\n' ends a line, a line wraps
// at PX_CONSOLE_COLS, a tab advances to the next multiple of 4, a UTF-8
// character shows as '?', CSI escape sequences are dropped and one ending in
// 'J' (erase display) clears the record. Other control bytes are dropped.
// ---------------------------------------------------------------------------
static char s_con[PX_CONSOLE_LINES][PX_CONSOLE_COLS + 1];
static int s_con_head;                  // slot the next complete line goes to
static int s_con_count;                 // complete lines kept
static char s_con_part[PX_CONSOLE_COLS + 1];
static int s_con_part_len;
static int s_con_esc;                   // 1 after ESC, 2 inside a CSI sequence
static int s_con_cont;                  // UTF-8 continuation bytes to skip

static void con_end_line(void) {
    memcpy(s_con[s_con_head], s_con_part, (size_t)s_con_part_len);
    s_con[s_con_head][s_con_part_len] = 0;
    s_con_head = (s_con_head + 1) % PX_CONSOLE_LINES;
    if (s_con_count < PX_CONSOLE_LINES) s_con_count++;
    s_con_part_len = 0;
}

static void con_put(char c) {
    if (s_con_part_len == PX_CONSOLE_COLS) con_end_line();
    s_con_part[s_con_part_len++] = c;
}

void px_console_write(const char* s, size_t n) {
    printf("%.*s", (int)n, s);
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (s_con_esc == 1) {
            s_con_esc = c == '[' ? 2 : 0;
            continue;
        }
        if (s_con_esc == 2) {
            if (c >= 0x40 && c <= 0x7E) {
                s_con_esc = 0;
                if (c == 'J') s_con_count = s_con_part_len = 0;
            }
            continue;
        }
        if (s_con_cont && (c & 0xC0) == 0x80) {
            s_con_cont--;
            continue;
        }
        s_con_cont = 0;
        if (c == 0x1B) s_con_esc = 1;
        else if (c == '\n') con_end_line();
        else if (c == '\t') { do con_put(' '); while (s_con_part_len % 4); }
        else if (c >= 0x20 && c < 0x7F) con_put((char)c);
        else if (c >= 0xC0) {
            con_put('?');
            s_con_cont = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : 1;
        }
    }
}

const char* px_console_line(int back) {
    if (back < 0 || back >= s_con_count) return NULL;
    return s_con[(s_con_head - 1 - back + 2 * PX_CONSOLE_LINES) % PX_CONSOLE_LINES];
}

const char* px_console_partial(size_t* len) {
    *len = (size_t)s_con_part_len;
    return s_con_part;
}

void nlr_jump_fail(void* val) {
    printf("[pyxel] FATAL: uncaught NLR %p\n", val);
    abort();
}

uint32_t px_gc_count, px_gc_us;

// The VM's audio hook counter (MICROPY_VM_HOOK_POLL, mpconfigport.h).
unsigned int px_vm_hook_divisor = MICROPY_VM_HOOK_COUNT;

// On the windowed-ABI Xtensa, live caller frames can sit in the register
// file. Nesting calls deeper than the 64-register window forces every one of
// them out to its stack save area, where the stack scan below finds them.
static int spill_windows(int depth) {
    volatile int pad[4];
    pad[0] = depth;
    return depth > 0 ? spill_windows(depth - 1) + pad[0] : pad[0];
}
static int (*volatile s_spill)(int) = spill_windows;

void gc_collect(void) {
    uint32_t t0 = host_get_ticks_us();
    gc_collect_start();
    s_spill(16);
    gc_helper_collect_regs_and_stack();
    gc_collect_end();
    px_gc_count++;
    px_gc_us += host_get_ticks_us() - t0;
}

// ---------------------------------------------------------------------------
// Error screen: the traceback, printed to serial and drawn on the panel
// until a key is pressed.
// ---------------------------------------------------------------------------
typedef struct {
    char* buf;
    size_t len, cap;
} textbuf;

static void textbuf_strn(void* env, const char* str, size_t len) {
    textbuf* t = (textbuf*)env;
    for (size_t i = 0; i < len && t->len + 1 < t->cap; i++) t->buf[t->len++] = str[i];
    t->buf[t->len] = 0;
}

static void show_error(const char* title, const char* text) {
    printf("[pyxel] %s\n%s\n", title, text);
    px_image* img = px_image_new(PX_SCREEN_MAX_W, PX_SCREEN_MAX_H);
    if (!img) return;
    static const uint32_t colors[16] = {
        0x000000, 0x2b335f, 0x7e2072, 0x19959c, 0x8b4852, 0x395c98, 0xa9c1ff, 0xeeeeee,
        0xd4186c, 0xd38441, 0xe9c35b, 0x70c6a9, 0x7696de, 0xa3a3a3, 0xff9798, 0xedc7b0,
    };
    memset(&px, 0, sizeof(px));
    memcpy(px.colors, colors, sizeof(colors));
    px.num_colors = 16;
    px.screen = img;
    px_clear_u8(&img->cv, 1);
    px_image_text(img, 4, 4, title, strlen(title), 8);
    // Wrap at the panel width: 80 columns of the 4px font.
    const int cols = PX_SCREEN_MAX_W / PX_FONT_WIDTH - 2;
    int y = 14;
    const char* p = text;
    while (*p && y < PX_SCREEN_MAX_H - PX_FONT_HEIGHT) {
        const char* nl = strchr(p, '\n');
        int len = nl ? (int)(nl - p) : (int)strlen(p);
        int take = len < cols ? len : cols;
        px_image_text(img, 4, (float)y, p, (size_t)take, 7);
        y += PX_FONT_HEIGHT + 1;
        p += take;
        if (take == len && nl) p++;
    }
    const char* hint = "Press any key";
    px_image_text(img, 4, PX_SCREEN_MAX_H - PX_FONT_HEIGHT - 2, hint, strlen(hint), 13);
    char err[64];
    if (px_display_begin(PX_SCREEN_MAX_W, PX_SCREEN_MAX_H, err, sizeof(err))) {
        int pressed;
        unsigned char key;
        while (host_get_key(&pressed, &key)) {}
        // Pushed every ERROR_FRAME_MS: the firmware stamps its touch pad
        // indicators and touch keyboard into pushed frames, and closing the
        // keyboard leaves its area black until the next frame.
        const uint32_t ERROR_FRAME_MS = 33;
        uint32_t next_frame = host_get_ticks_ms();
        for (;;) {
            uint32_t now = host_get_ticks_ms();
            if ((int32_t)(now - next_frame) >= 0) {
                px_display_render();
                next_frame = now + ERROR_FRAME_MS;
            }
            if (host_should_exit()) break;
            if (host_get_key(&pressed, &key) && pressed) break;
            host_sleep_ms(20);
        }
        px_display_end();
    }
    px_image_free(img);
    memset(&px, 0, sizeof(px));
}

// ---------------------------------------------------------------------------
// One run of the game: fresh MicroPython state, startup script as __main__.
// ---------------------------------------------------------------------------
static void set_sys_path(const char* startup_dir, const char* pylib) {
    mp_obj_t items[2] = {
        mp_obj_new_str(startup_dir, strlen(startup_dir)),
        mp_obj_new_str(pylib, strlen(pylib)),
    };
    mp_obj_t lst = mp_obj_new_list(2, items);
    mp_obj_t sl = mp_obj_new_slice(mp_const_none, mp_const_none, mp_const_none);
    mp_obj_subscr(mp_sys_path, sl, lst);
}

// Returns true when the game asked for reset().
static bool run_once(void* heap, size_t heap_size, const char* startup, const char* pylib) {
    volatile int stack_top = 0;
    mp_cstack_init_with_top((void*)&stack_top, PX_CSTACK_LIMIT);
    gc_init(heap, (uint8_t*)heap + heap_size);
    mp_init();

    char dir[256];
    px_path_dirname(startup, dir, sizeof(dir));
    px_vfs_set_cwd(dir);
    set_sys_path(dir, pylib);
    px_api_register();

    bool reset = false;
    nlr_buf_t nlr;
    if (nlr_push(&nlr) == 0) {
        mp_obj_dict_store(MP_OBJ_FROM_PTR(mp_globals_get()),
                          MP_OBJ_NEW_QSTR(qstr_from_str("__file__")),
                          mp_obj_new_str(startup, strlen(startup)));
        mp_lexer_t* lex = mp_lexer_new_from_file(qstr_from_str(startup));
        qstr source_name = lex->source_name;
        mp_parse_tree_t pt = mp_parse(lex, MP_PARSE_FILE_INPUT);
        mp_obj_t fun = mp_compile(&pt, source_name, false);
        mp_call_function_0(fun);
        nlr_pop();
    } else {
        mp_obj_t exc = MP_OBJ_FROM_PTR(nlr.ret_val);
        if (mp_obj_is_subclass_fast(MP_OBJ_FROM_PTR(mp_obj_get_type(exc)),
                                    MP_OBJ_FROM_PTR(&mp_type_SystemExit))) {
            reset = px.reset_requested;
        } else {
            static char text[4096];
            textbuf tb = { text, 0, sizeof(text) };
            text[0] = 0;
            mp_print_t pr = { &tb, textbuf_strn };
            mp_obj_print_exception(&pr, exc);
            px_api_shutdown();
            show_error("The game stopped with an error:", text);
        }
    }
    px_api_shutdown();
    gc_sweep_all();
    mp_deinit();
    return reset;
}

// ---------------------------------------------------------------------------
// Entry
// ---------------------------------------------------------------------------
int main(int argc, char** argv) {
    host_log("pyxel: module starting");
    static char startup[256];
    static char pylib[256];
    static void* heap;
    heap = NULL;

    if (setjmp(s_exit_jmp) != 0) {
        host_log("pyxel: exit()/abort() caught, returning to launcher");
        px_display_end();
        px_vfs_unmount();
        free(heap);
        host_clear_screen();
        return s_exit_code;
    }

    const char* game = NULL;
    int heap_kb = PX_DEFAULT_HEAP_KB;
    pylib[0] = 0;
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-') {
            if (i + 1 < argc) {
                if (strcmp(argv[i], "-pylib") == 0) snprintf(pylib, sizeof(pylib), "%s", argv[i + 1]);
                else if (strcmp(argv[i], "-heapkb") == 0) heap_kb = atoi(argv[i + 1]);
                else if (strcmp(argv[i], "-sound") == 0) px_sound_enabled = atoi(argv[i + 1]) != 0;
                else if (strcmp(argv[i], "-kbtoggle") == 0) px_kbtoggle = true;
                else if (strcmp(argv[i], "-clock") == 0) {
                    char* end;
                    px_clock_epoch = (int64_t)strtoul(argv[i + 1], &end, 10);
                    if (*end == ',') px_clock_tzmin = atoi(end + 1);
                }
            }
            i++;                                  // every flag takes one value
        } else if (!game) {
            game = argv[i];
        }
    }
    if (!pylib[0]) {
        char elf_dir[256];
        px_path_dirname(argv[0], elf_dir, sizeof(elf_dir));
        snprintf(pylib, sizeof(pylib), "%s/pylib", elf_dir);
    }
    if (!game) {
        show_error("No game given", "The launcher passes a .pyxapp or .py path.");
        return 1;
    }
    printf("[pyxel] game=%s pylib=%s heap=%dKB sound=%d\n", game, pylib, heap_kb, px_sound_enabled);

    char err[160];
    if (!px_vfs_mount(game, startup, sizeof(startup), err, sizeof(err))) {
        show_error("Cannot open the game:", err);
        return 1;
    }
    printf("[pyxel] startup=%s\n", startup);

    size_t heap_size = (size_t)heap_kb * 1024;
    heap = malloc(heap_size);
    if (!heap) {
        px_vfs_unmount();
        show_error("Out of memory", "Could not allocate the Python heap.");
        return 1;
    }

    while (run_once(heap, heap_size, startup, pylib)) {
        printf("[pyxel] reset\n");
    }

    free(heap);
    heap = NULL;
    px_vfs_unmount();
    host_clear_screen();
    host_log("pyxel: module done");
    return 0;
}
