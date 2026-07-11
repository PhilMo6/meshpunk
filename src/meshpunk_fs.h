#pragma once

#include <Arduino.h>
#include <FS.h>

// Unified file open/close that handles:
//   - S:/L:/U: drive prefix parsing (S: = SD, L: = LittleFS, U: = USB drive)
//   - SPI bus locking for SD access (SD shares the bus with LoRa + TFT)
//   - mount guards (sd_mounted for SD, usb_fs_mounted for USB)
//   - flash-guard bracketing for LittleFS writes (internal flash only —
//     SD and USB writes never stall the cache)
//
// Callers must use meshpunk_close() to release the SPI lock when done.
// For bulk reads into a buffer, use meshpunk_read_all().

// Which backing store a path resolves to. Two orthogonal facts derive from
// it: only MP_SD needs the SPI bus lock; only MP_FLASH writes need the USB
// flash guard.
enum MpDrive : uint8_t { MP_FLASH = 0, MP_SD, MP_USB };

struct MeshpunkFile {
    fs::File file;
    bool     is_sd;     // MP_SD: SPI lock held while open
    bool     is_flash;  // MP_FLASH: writes need UsbFlashGuardIf
    bool     valid;     // true if file opened successfully
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

// Strip an S:/L:/U: drive prefix from `path`, setting *drive accordingly
// (no prefix -> MP_SD when default_sd, else MP_FLASH). Returns a pointer
// INTO `path` at the first character after the prefix. Shared by
// fs_bridge.cpp so every binding resolves drives identically.
const char* meshpunk_parse_drive(const char* path, MpDrive* drive, bool default_sd);
