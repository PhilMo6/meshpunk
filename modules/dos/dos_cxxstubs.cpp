// Minimal C++ runtime glue for the DOS module.
//
// Only one translation unit is C++ (dos_folderdisk.cpp, reused from the PC-XT
// module), so this is the bare minimum: operator new/delete routed through the
// host's PSRAM-backed malloc, plus the ABI symbols GCC emits.
//
// abort() and __assert_func() are NOT here — main_tdeck.c defines them so they
// hit the setjmp/longjmp exit trap.

#include <cstddef>

extern "C" void *malloc(size_t size);
extern "C" void  free(void *ptr);
extern "C" void  abort(void);

void *operator new(size_t size)        { return malloc(size); }
void *operator new[](size_t size)      { return malloc(size); }
void  operator delete(void *p) noexcept   { free(p); }
void  operator delete[](void *p) noexcept { free(p); }
void  operator delete(void *p, size_t) noexcept   { free(p); }
void  operator delete[](void *p, size_t) noexcept { free(p); }

extern "C" {
    void __cxa_pure_virtual(void) { abort(); }
    int  __cxa_guard_acquire(int *guard) { if (*guard) return 0; return 1; }
    void __cxa_guard_release(int *guard) { *guard = 1; }
    void __cxa_guard_abort(int *guard) { (void)guard; }
    int  __cxa_atexit(void (*func)(void *), void *arg, void *dso) {
        (void)func; (void)arg; (void)dso;
        return 0;   // no-op: module cleanup is the host's job
    }
    void __cxa_finalize(void *dso) { (void)dso; }
    void *__dso_handle = 0;
}
