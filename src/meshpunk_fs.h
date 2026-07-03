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

// Create all parent directories of `path` (the final segment is treated as
// a file name and not created). Same prefix routing as meshpunk_open().
// Returns true if the parents exist on return.
bool meshpunk_mkdirs(const char* path, bool default_sd = false);

// Strip an S:/L: drive prefix from `path`, setting *use_sd accordingly
// (no prefix -> *use_sd = default_sd). Returns a pointer INTO `path` at the
// first character after the prefix. Shared by fs_bridge.cpp so every binding
// resolves drives identically.
const char* meshpunk_parse_prefix(const char* path, bool* use_sd, bool default_sd);
