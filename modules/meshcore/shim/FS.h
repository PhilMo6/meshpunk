#pragma once

// Arduino FS over VFS stdio for the punkmesh port. Every stdio call here
// resolves to the host's proto_* exports at load — SPI-locked, PSRAM-bounced —
// so the Arduino-era locking ceremony (sd_spi_take, SPI_LOCK) collapses to
// no-ops in the shim meshpunk_sync.h. Paths are full VFS paths
// ("/littlefs/..." or "/sd/..."): the package's storage prefix carries the
// mount root (set at wiring, step 5f).
//
// The LittleFS/SD objects exist only as IDENTITIES for punkmesh's
// `_storage != &LittleFS` is-sd checks; both open through the same stdio.
// Shim FS/File objects must NEVER cross the module boundary (the firmware's
// fs::FS is a different class entirely) — boundary calls use the mcs_* C
// bridges.

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include "WString.h"
#include "Stream.h"

namespace fs {

// IS-A Stream, like Arduino's File: mesh::Identity reads/writes identity
// files through the Stream virtuals (readBytes/write).
class File : public Stream {
    FILE* _f;
    long  _size;

public:
    File() : _f(nullptr), _size(0) {}
    explicit File(FILE* f) : _f(f), _size(0) {
        if (_f) {
            long pos = ftell(_f);
            fseek(_f, 0, SEEK_END);
            _size = ftell(_f);
            fseek(_f, pos, SEEK_SET);
        }
    }

    operator bool() const { return _f != nullptr; }

    size_t read(uint8_t* buf, size_t len)        { return _f ? fread(buf, 1, len, _f) : 0; }
    int read() {
        if (!_f) return -1;
        unsigned char c;
        return fread(&c, 1, 1, _f) == 1 ? (int)c : -1;
    }
    size_t readBytes(uint8_t* buf, size_t len) override { return read(buf, len); }
    size_t write(const uint8_t* buf, size_t len) override {
        return _f ? fwrite(buf, 1, len, _f) : 0;
    }
    size_t write(uint8_t c) override             { return _f ? fwrite(&c, 1, 1, _f) : 0; }
    size_t print(char c) override                { return write((uint8_t)c); }

    size_t print(const char* s)  { return _f && s ? fwrite(s, 1, strlen(s), _f) : 0; }
    size_t print(const String& s) { return print(s.c_str()); }
    size_t printf(const char* fmt, ...) {
        if (!_f) return 0;
        char b[256];
        __builtin_va_list ap;
        __builtin_va_start(ap, fmt);
        int n = vsnprintf(b, sizeof(b), fmt, ap);
        __builtin_va_end(ap);
        if (n <= 0) return 0;
        if (n > (int)sizeof(b) - 1) n = (int)sizeof(b) - 1;
        return fwrite(b, 1, (size_t)n, _f);
    }

    bool seek(uint32_t pos) { return _f ? fseek(_f, (long)pos, SEEK_SET) == 0 : false; }
    size_t position() const { return _f ? (size_t)ftell(_f) : 0; }
    size_t size() const     { return (size_t)_size; }
    int available() const   { return _f ? (int)(_size - ftell(_f)) : 0; }

    void flush() { if (_f) fflush(_f); }

    void close() {
        if (_f) {
            fclose(_f);
            _f = nullptr;
        }
    }
};

class FS {
public:
    // Distinct objects only for identity checks; behavior is identical.
    File open(const char* path, const char* mode = "r", bool create = false) {
        (void)create;                    // stdio "w" creates
        FILE* f = fopen(path, mode);
        return File(f);
    }
    File open(const String& path, const char* mode = "r", bool create = false) {
        return open(path.c_str(), mode, create);
    }
    bool exists(const char* path) {
        FILE* f = fopen(path, "r");
        if (!f) return false;
        fclose(f);
        return true;
    }
    bool exists(const String& path) { return exists(path.c_str()); }
    bool remove(const char* path)   { return ::remove(path) == 0; }
    bool remove(const String& path) { return remove(path.c_str()); }
    bool rename(const char* a, const char* b) { return ::rename(a, b) == 0; }
    bool rename(const String& a, const String& b) { return rename(a.c_str(), b.c_str()); }
    bool mkdir(const char* path)   { return ::mkdir(path, 0777) == 0; }
    bool mkdir(const String& path) { return mkdir(path.c_str()); }
};

}  // namespace fs

using fs::File;
using fs::FS;

extern fs::FS LittleFS;
extern fs::FS SD;

#define FILE_READ   "r"
#define FILE_WRITE  "w"
#define FILE_APPEND "a"
