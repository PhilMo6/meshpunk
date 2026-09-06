#pragma once

// Minimal Stream for compiling lib/MeshCore inside the meshcore protocol
// package (module builds have no Arduino core). Arduino-true signatures
// (print returns size_t) so File and the Serial stub override cleanly.

#include <stddef.h>
#include <stdint.h>

class Stream {
public:
    virtual ~Stream() {}
    virtual size_t write(uint8_t) { return 0; }
    virtual size_t write(const uint8_t*, size_t) { return 0; }
    virtual size_t readBytes(uint8_t*, size_t) { return 0; }
    virtual size_t print(char) { return 0; }
    virtual size_t print(const char*) { return 0; }
    virtual size_t println() { return 0; }
    virtual size_t println(const char*) { return 0; }
    virtual int available() { return 0; }
    virtual int read() { return -1; }
};
