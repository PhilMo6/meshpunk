// Minimal C++ runtime stubs for the jet3d ELF module.
// Routes operator new/delete through the host's psram_malloc/free and provides
// the ABI glue libstdc++ expects. Jet uses std::vector and std::sort, which
// reference the __throw_* entry points below; the module is built with
// -fno-exceptions, so each one aborts instead of unwinding.

#include <cstdlib>
#include <cstddef>
#include <cstdint>
#include <new>

extern "C" void* malloc(size_t size);
extern "C" void  free(void* ptr);
extern "C" void  exit(int code);   // defined in main_tdeck.cpp (longjmp trap)
extern "C" int   snprintf(char*, size_t, const char*, ...);
extern "C" void  host_log(const char* msg);

// abort() — route through exit() so the longjmp trap catches it
extern "C" void abort(void) {
    exit(-1);
}

// strtof: Lua's number parser resolves to it because LUA_32BITS makes
// lua_Number a float. host_exports[] carries strtod but not strtof, so define
// it here in terms of the exported double parser rather than widening the
// firmware's export table.
extern "C" double strtod(const char* nptr, char** endptr);

extern "C" float strtof(const char* nptr, char** endptr) {
    return (float)strtod(nptr, endptr);
}

// ---------------------------------------------------------------------------
// Allocation accounting (jet.mem())
// ---------------------------------------------------------------------------
// The device-side twin of the host memory probe, so the two produce directly
// comparable numbers. It exists to answer one question the heap cannot:
// "did destroying the world actually give the bytes back?" The only host export
// for memory is host_psram_largest_free, and largest-block barely moves when
// thousands of small scattered blocks are freed — useless for that question.
//
// NO per-block header. Bytes are decremented through the SIZED delete
// overloads, which -std=gnu++17 makes the compiler emit for std::vector
// deallocation and for `delete obj` on a known type. Adding a header instead
// would mean returning ptr+N, and any pointer that ever reached free() by a
// path other than operator delete would corrupt the heap — an unacceptable
// risk for a diagnostic.
//
// SELF-VALIDATING: an UNSIZED delete cannot know how much it released, so it
// only decrements the block count and bumps `unsized`. If jet.mem() reports
// unsized == 0 the byte figure is exact; if not, live bytes read high by
// however much those frees released and the block count is the reliable one.
//
// Module-side allocation is single-threaded (the blit task on Core 1 uses the
// firmware's allocator, not this one), so plain counters are safe.
static size_t   g_memBytes   = 0;
static uint32_t g_memBlocks  = 0;
static uint32_t g_memUnsized = 0;

extern "C" void jet_mem_stats(uint32_t* bytes, uint32_t* blocks,
                              uint32_t* unsized) {
    if (bytes)   *bytes   = (uint32_t)g_memBytes;
    if (blocks)  *blocks  = g_memBlocks;
    if (unsized) *unsized = g_memUnsized;
}

static inline void* memTrack(void* p, size_t size) {
    if (p) { g_memBytes += size; ++g_memBlocks; }
    return p;
}
static inline void memUntrackSized(void* p, size_t size) {
    if (p) { g_memBytes -= size; --g_memBlocks; }
}
static inline void memUntrackUnsized(void* p) {
    if (p) { --g_memBlocks; ++g_memUnsized; }
}

// operator new / delete → PSRAM via host-remapped malloc/free
void* operator new(size_t size) { return memTrack(malloc(size), size); }
void* operator new[](size_t size) { return memTrack(malloc(size), size); }
void  operator delete(void* p) noexcept { memUntrackUnsized(p); free(p); }
void  operator delete[](void* p) noexcept { memUntrackUnsized(p); free(p); }
void  operator delete(void* p, size_t n) noexcept { memUntrackSized(p, n); free(p); }
void  operator delete[](void* p, size_t n) noexcept { memUntrackSized(p, n); free(p); }

// Nothrow forms. std::stable_sort (used by Scene::drawSprites to order sprites
// by zOrder) allocates its merge buffer through these and falls back to an
// in-place algorithm when they return null, which is the whole point of the
// nothrow overload — so returning malloc's result directly is correct.
// nothrow_t's default constructor is explicit, so it cannot be brace-initialised.
const std::nothrow_t std::nothrow = std::nothrow_t();
void* operator new(size_t size, const std::nothrow_t&) noexcept {
    return memTrack(malloc(size), size);
}
void* operator new[](size_t size, const std::nothrow_t&) noexcept {
    return memTrack(malloc(size), size);
}
void  operator delete(void* p, const std::nothrow_t&) noexcept {
    memUntrackUnsized(p); free(p);
}
void  operator delete[](void* p, const std::nothrow_t&) noexcept {
    memUntrackUnsized(p); free(p);
}

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

    // Assertion handler — routes through abort().
    // Reports via host_log (SerialMux, under SLOG_LOCK) rather than the exported
    // raw printf, which writes the UART without that lock and so races the
    // firmware tasks that take it.
    void __assert_func(const char* file, int line, const char* func, const char* expr) {
        char buf[200];
        snprintf(buf, sizeof(buf), "ASSERT FAILED: %s:%d %s: %s",
                 file, line, func ? func : "", expr ? expr : "");
        host_log(buf);
        abort();
    }
}

// C++ exception stubs — break the libstdc++ dependency chain.
// std::vector references these; without stubs they pull in locale/iostream
// formatting from libstdc++.
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
