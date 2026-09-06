#pragma once

// Minimal Arduino environment for compiling lib/MeshCore inside the meshcore
// protocol package. Module builds have no Arduino core: everything here is
// implemented in the package over the MeshHostApi (mcshim.cpp) — the
// submodule stays pristine and gets the environment it expects instead.

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "Stream.h"
#include "WString.h"

// Host uptime (H->millis32). MeshCore's dispatcher paces on its
// MillisecondClock abstraction; the few direct calls route here too.
unsigned long millis();

// Arduino random over the host TRNG (mcshim.cpp).
long random(long max);
long random(long min, long max);

// StdRNG::begin calls this. A no-op is CORRECT here: random() reads the
// host TRNG, and seeding must never switch anything off that path (the
// firmware-side randomSeed lesson).
inline void randomSeed(unsigned long) {}

inline char* ltoa(long v, char* buf, int base) {
    snprintf(buf, 16, base == 16 ? "%lx" : "%ld", v);
    return buf;
}

template <class T> static inline T min(T a, T b) { return a < b ? a : b; }
template <class T> static inline T max(T a, T b) { return a > b ? a : b; }

// Placement new/delete (-nostdlib has no <new>): the companion constructs
// its handler into pool memory.
inline void* operator new(size_t, void* p) noexcept { return p; }
inline void  operator delete(void*, void*) noexcept {}

// Debug/log output: collected line-wise and handed to H->log. MeshCore's
// MESH_DEBUG_PRINTLN and packet-log paths print through this.
class MpSerialStub : public Stream {
public:
    size_t print(char c) override;
    size_t print(const char* s) override;
    size_t print(int v);
    size_t print(uint32_t v);
    size_t print(const String& s) { return print(s.c_str()); }
    size_t println() override;
    size_t println(const char* s) override;
    size_t println(const String& s) { return println(s.c_str()); }
    void printf(const char* fmt, ...);
    // No console attached: the CLI-over-serial path reads nothing here.
    int available() override { return 0; }
    int read() override { return -1; }
};

extern MpSerialStub Serial;
