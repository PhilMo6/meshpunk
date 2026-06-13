// Minimal C++ runtime stubs for the ELF module.
// Routes operator new/delete through the host's psram_malloc/free.
// Provides the bare minimum ABI glue that libstdc++ needs.

#include <cstdlib>
#include <cstddef>

extern "C" void* malloc(size_t size);
extern "C" void  free(void* ptr);
extern "C" void  exit(int code);   // defined in main_tdeck.cpp (longjmp trap)
extern "C" int   printf(const char*, ...);

// abort() — route through exit() so the longjmp trap catches it
extern "C" void abort(void) {
    exit(-1);
}

// operator new / delete → PSRAM via host-remapped malloc/free
void* operator new(size_t size) { return malloc(size); }
void* operator new[](size_t size) { return malloc(size); }
void  operator delete(void* p) noexcept { free(p); }
void  operator delete[](void* p) noexcept { free(p); }
void  operator delete(void* p, size_t) noexcept { free(p); }
void  operator delete[](void* p, size_t) noexcept { free(p); }

// C++ ABI stubs
extern "C" {
    void __cxa_pure_virtual(void) { abort(); }
    int  __cxa_guard_acquire(int* guard) { if (*guard) return 0; return 1; }
    void __cxa_guard_release(int* guard) { *guard = 1; }
    void __cxa_guard_abort(int* guard) { (void)guard; }
    int  __cxa_atexit(void (*func)(void*), void* arg, void* dso) {
        (void)func; (void)arg; (void)dso;
        return 0; // no-op — module cleanup is handled by the host
    }
    void __cxa_finalize(void* dso) { (void)dso; }
    void* __dso_handle = 0;

    // Assertion handler — routes through abort()
    void __assert_func(const char* file, int line, const char* func, const char* expr) {
        printf("ASSERT FAILED: %s:%d %s: %s\n", file, line, func ? func : "", expr ? expr : "");
        abort();
    }
}

// C++ exception stubs — break the libstdc++ dependency chain.
// std::string/vector reference these; without stubs they pull in
// locale/iostream/exception formatting from libstdc++.
namespace std {
    void __throw_bad_alloc() { abort(); }
    void __throw_length_error(const char*) { abort(); }
    void __throw_out_of_range(const char*) { abort(); }
    void __throw_out_of_range_fmt(const char*, ...) { abort(); }
    void __throw_logic_error(const char*) { abort(); }
    void __throw_bad_cast() { abort(); }
    void __throw_runtime_error(const char*) { abort(); }
    void __throw_range_error(const char*) { abort(); }
    void __throw_bad_function_call() { abort(); }
    void __throw_overflow_error(const char*) { abort(); }
}
