// MicroPython configuration for the Pyxel module.
// genhdr/ was generated from this feature set: enabling or disabling a module,
// builtin or feature here requires regenerating genhdr (see ../VENDOR.md).

#include <stdint.h>
#include <alloca.h>

typedef long mp_off_t;
#define MP_SSIZE_MAX (0x7fffffff)

#define MICROPY_MPHALPORT_H                     "mphalport.h"

#define MICROPY_CONFIG_ROM_LEVEL                (MICROPY_CONFIG_ROM_LEVEL_EXTRA_FEATURES)
#define MICROPY_ENABLE_COMPILER                 (1)
#define MICROPY_ENABLE_GC                       (1)
#define MICROPY_PY_GC                           (1)
#define MICROPY_FLOAT_IMPL                      (MICROPY_FLOAT_IMPL_FLOAT)
// Floats are stored in the object word (30 bits: float32 minus its 2 lowest
// mantissa bits), so float arithmetic allocates nothing on the GC heap.
#define MICROPY_OBJ_REPR                        (MICROPY_OBJ_REPR_C)
#define MICROPY_LONGINT_IMPL                    (MICROPY_LONGINT_IMPL_MPZ)
#define MICROPY_ENABLE_EXTERNAL_IMPORT          (1)
#define MICROPY_PY_SYS                          (1)
#define MICROPY_PY_SYS_PATH                     (1)
#define MICROPY_PY_IO                           (1)
#define MICROPY_PY_JSON                         (1)
#define MICROPY_PY_TIME                         (0)
#define MICROPY_PY_OS                           (0)
#define MICROPY_ENABLE_FINALISER                (0)
#define MICROPY_PY_BUILTINS_HELP                (0)
#define MICROPY_HELPER_REPL                     (0)
#define MICROPY_PY_THREAD                       (0)
#define MICROPY_VFS                             (0)
#define MICROPY_PY_MICROPYTHON_MEM_INFO         (1)
#define MICROPY_PY_SYS_STDFILES                 (0)
#define MICROPY_USE_INTERNAL_ERRNO              (1)
#define MICROPY_PY_ERRNO                        (0)
#define MICROPY_PY_BUILTINS_INPUT               (0)
#define MICROPY_PY_SYS_PLATFORM                 "meshpunk"
#define MICROPY_KBD_EXCEPTION                   (0)
#define MICROPY_PY_UCTYPES                      (0)

// Builtins: the OSError subclasses (py/objexcept.c MESHPUNK patch), exit()
// and quit() (sys.exit, as CPython's site builtins), and input() (src/api.c).
extern const struct _mp_obj_fun_builtin_var_t px_builtin_input_obj;
#define MICROPY_PORT_BUILTINS \
    { MP_ROM_QSTR(MP_QSTR_FileExistsError), MP_ROM_PTR(&mp_type_FileExistsError) }, \
    { MP_ROM_QSTR(MP_QSTR_FileNotFoundError), MP_ROM_PTR(&mp_type_FileNotFoundError) }, \
    { MP_ROM_QSTR(MP_QSTR_IsADirectoryError), MP_ROM_PTR(&mp_type_IsADirectoryError) }, \
    { MP_ROM_QSTR(MP_QSTR_exit), MP_ROM_PTR(&mp_sys_exit_obj) }, \
    { MP_ROM_QSTR(MP_QSTR_quit), MP_ROM_PTR(&mp_sys_exit_obj) }, \
    { MP_ROM_QSTR(MP_QSTR_input), MP_ROM_PTR(&px_builtin_input_obj) },

// The ESP32-S3 uses the windowed register ABI: nlrxtensa.c is call0-only, so
// NLR and the GC register capture both go through setjmp.
#define MICROPY_NLR_SETJMP                      (1)
#define MICROPY_GCREGS_SETJMP                   (1)

// Source files are read by src/vfs.c (mp_lexer_new_from_file), not POSIX read().
#define MICROPY_READER_POSIX                    (0)

// Bytecode dispatch through a label table (in .rodata) instead of a switch.
#define MICROPY_OPT_COMPUTED_GOTO               (1)

// Shared cache of recent map lookup positions (py/map.c, py/mpstate.h:
// 16-bit entries, MESHPUNK).
#define MICROPY_OPT_MAP_LOOKUP_CACHE_SIZE       (512)

// The interpreter's hottest functions run from internal SRAM: the ELF loader
// copies the .iram.text section there. Functions MicroPython has a
// MICROPY_WRAP_* hook for are placed below; the others carry PX_IRAM in py/
// (MESHPUNK). Files holding them are compiled with -mtext-section-literals
// -fno-jump-tables (build.ps1).
#if defined(__xtensa__)
#define PX_IRAM __attribute__((section(".iram.text")))
#else
#define PX_IRAM
#endif
#define MICROPY_WRAP_MP_EXECUTE_BYTECODE(f)     PX_IRAM f
#define MICROPY_WRAP_MP_MAP_LOOKUP(f)           PX_IRAM f
#define MICROPY_WRAP_MP_LOAD_GLOBAL(f)          PX_IRAM f
#define MICROPY_WRAP_MP_LOAD_NAME(f)            PX_IRAM f
#define MICROPY_WRAP_MP_BINARY_OP(f)            PX_IRAM f
#define MICROPY_WRAP_MP_OBJ_GET_TYPE(f)         PX_IRAM f

// Every 64 branches/returns the VM calls px_audio_poll() (src/audio.c), which
// keeps the audio ring fed while Python code runs. The count runs across all
// bytecode in one global (px_vm_hook_divisor, src/main.c): a counter local to
// mp_execute_bytecode restarts at every function call.
void px_audio_poll(void);
extern unsigned int px_vm_hook_divisor;
#define MICROPY_VM_HOOK_COUNT                   (64)
#define MICROPY_VM_HOOK_INIT
#define MICROPY_VM_HOOK_POLL                    if (--px_vm_hook_divisor == 0) { \
        px_vm_hook_divisor = MICROPY_VM_HOOK_COUNT; \
        px_audio_poll(); \
}
#define MICROPY_VM_HOOK_LOOP                    MICROPY_VM_HOOK_POLL
#define MICROPY_VM_HOOK_RETURN                  MICROPY_VM_HOOK_POLL
