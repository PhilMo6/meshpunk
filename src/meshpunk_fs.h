#pragma once

#include <Arduino.h>
#include <FS.h>

// Unified file open/close that handles:
//   - S:/L: drive prefix parsing (S: = SD, L: = LittleFS)
//   - SPI bus locking for SD access
//   - sd_mounted guard for SD
//
// Callers must use meshpunk_close() to release the SPI lock when done.
// For bulk reads into a buffer, use meshpunk_read_all().

struct MeshpunkFile {
    fs::File file;
    bool     is_sd;    // true if opened from SD (SPI lock held)
    bool     valid;    // true if file opened successfully
};

// Parse an S:/L: prefixed path and open the file.
// `default_sd` controls the default when no prefix is present
// (true = default to SD, false = default to LittleFS).
// On success: .valid = true, .file is open, SPI lock held if SD.
// On failure: .valid = false, no SPI lock held.
MeshpunkFile meshpunk_open(const char* path, const char* mode, bool default_sd = false);

// Close the file and release SPI lock if it was an SD file.
void meshpunk_close(MeshpunkFile& mf);

// Read an entire file into a PSRAM-allocated buffer.
// Handles open, read, close, and SPI locking.
// Caller must heap_caps_free() the returned pointer.
// Returns NULL on failure.
void* meshpunk_read_all(const char* path, uint32_t* out_size, bool default_sd = false);
