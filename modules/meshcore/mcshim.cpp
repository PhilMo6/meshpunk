// Arduino-environment shim implementations (shim/Arduino.h declares these):
// millis + the Serial stub over the host log, plus the C++ runtime stub a
// -nostdlib build with virtuals needs. lib/MeshCore compiles against the
// shim headers untouched.

// RNG.h FIRST: RNGClass has a MEMBER named SEED_SIZE, and MeshCore.h
// (reached through meshpunk_sync.h below) defines a macro of the same name —
// included later, the macro mangles the class declaration.
#include <RNG.h>

#include "shim/Arduino.h"
#include "shim/FS.h"
#include "shim/meshpunk_sync.h"
#include "mc_internal.h"

#include <stdarg.h>

const MeshHostApi* MCH = nullptr;

unsigned long millis() {
    return MCH ? MCH->millis32() : 0;
}

// Arduino random over the host TRNG.
long random(long max) {
    if (max <= 0 || !MCH) return 0;
    uint32_t r;
    MCH->random_bytes((uint8_t*)&r, sizeof(r));
    return (long)(r % (uint32_t)max);
}
long random(long min, long max) {
    if (max <= min) return min;
    return min + random(max - min);
}

// Shim FS identities (FS.h): distinct objects for punkmesh's is-sd checks;
// both open through the same host stdio.
fs::FS LittleFS;
fs::FS SD;

// Module-owned RxEvent queue (shim meshpunk_sync.h extern); created at
// package init (5c-2 wiring), drained by lua_tick.
QueueHandle_t rx_event_queue = nullptr;

// Line-buffered: MeshCore prints fragments (getLogDateTime, printf pieces,
// printHex chars); a line is handed to H->log on '\n' or overflow.
static char   s_line[192];
static size_t s_line_len = 0;

static void line_flush() {
    if (!s_line_len) return;
    s_line[s_line_len] = '\0';
    if (MCH) MCH->log("%s", s_line);
    s_line_len = 0;
}

static void line_put(char c) {
    if (c == '\n') { line_flush(); return; }
    if (c == '\r') return;
    if (s_line_len >= sizeof(s_line) - 1) line_flush();
    s_line[s_line_len++] = c;
}

static void line_put_str(const char* s) {
    while (s && *s) line_put(*s++);
}

size_t MpSerialStub::print(char c)          { line_put(c); return 1; }
size_t MpSerialStub::print(const char* s)   { line_put_str(s); return s ? strlen(s) : 0; }
size_t MpSerialStub::println()              { line_flush(); return 1; }
size_t MpSerialStub::println(const char* s) { size_t n = print(s); line_flush(); return n + 1; }

size_t MpSerialStub::print(int v) {
    char b[16];
    snprintf(b, sizeof(b), "%d", v);
    return print(b);
}

size_t MpSerialStub::print(uint32_t v) {
    char b[16];
    snprintf(b, sizeof(b), "%u", (unsigned)v);
    return print(b);
}

void MpSerialStub::printf(const char* fmt, ...) {
    char b[192];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof(b), fmt, ap);
    va_end(ap);
    line_put_str(b);
}

MpSerialStub Serial;

// -nostdlib C++ with abstract classes: the vtable slot for unimplemented
// pure virtuals must exist. Reaching it is a bug; make it loud.
extern "C" void __cxa_pure_virtual(void) {
    if (MCH) MCH->log("FATAL: pure virtual call");
    for (;;) {}
}

// C++ allocator set over the boot-reserved protocol pool (-nostdlib supplies
// none; virtual destructors emit the sized deletes even when never reached).
void* operator new(size_t n)                 { return MCH->mem_alloc(n); }
void* operator new[](size_t n)               { return MCH->mem_alloc(n); }
void  operator delete(void* p)               { if (p) MCH->mem_free(p); }
void  operator delete[](void* p)             { if (p) MCH->mem_free(p); }
void  operator delete(void* p, size_t)       { if (p) MCH->mem_free(p); }
void  operator delete[](void* p, size_t)     { if (p) MCH->mem_free(p); }

// rweather Crypto's global RNG object (header included at the very top —
// see the SEED_SIZE note). The only reachable caller is
// Ed25519::generatePrivateKey — which MeshCore never uses (keygen goes
// through lib/ed25519's C API with caller-provided entropy) but shared-lib
// linking keeps every Crypto function exported. Entropy here is the host
// TRNG; rweather's own RNG state machinery (EEPROM seeds, noise sources —
// RNG.cpp) is deliberately not linked, so this shim owns the three symbols.
RNGClass::RNGClass()  {}
RNGClass::~RNGClass() {}
void RNGClass::rand(uint8_t* data, size_t len) {
    if (MCH) MCH->random_bytes(data, (int)len);
}
RNGClass RNG;
