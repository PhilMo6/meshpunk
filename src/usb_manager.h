#pragma once

// USB-OTG host manager. Owns the S3's USB port in host mode: host stack
// lifecycle, device enumeration/classification, and class drivers (UAC 1.0
// audio out today; HID/MSC/CDC planned). Started manually from the Tools/USB
// app — while host mode runs, the port can't do Serial-JTAG (USB serial and
// PC file transfer come back on Stop, or via the boot self-heal after a
// reset; the PHY mux lives in an RTC-domain register that survives warm
// resets).
//
// Audio: when a UAC dongle is connected and the usb_audio pref is on, the
// manager streams continuously (zeros when idle) and sound.cpp mirrors both
// output paths into usb_audio_push() — mixer chunks (tones + ELF module
// audio, 44.1k stereo) and Audio-library file playback (file-native rate and
// channel count). The sink resamples to the dongle rate.

#include <stdint.h>

struct lua_State;

// ── Lifecycle ────────────────────────────────────────────────────────────────
void usb_manager_init(void (*prefs_save_fn)());   // call once at boot (main.cpp)
bool usb_manager_start();
void usb_manager_stop();
bool usb_manager_running();
void usb_manager_register_lua(lua_State* L);      // also runs the PHY boot self-heal

// ── Audio sink (called from sound.cpp taps) ──────────────────────────────────
// usb_audio_active(): dongle routed and ISO streaming.
// usb_audio_push(): resample `pcm` (interleaved, `channels` ch, int16 at
//   src_rate) into the streaming ring and return true if the CALLER should
//   silence its own I2S buffer (routed and the "speaker stays on" pref is
//   off). Returns false and does nothing when not active — speaker plays as
//   normal. Cheap no-op on the hot path when USB audio is idle.
bool usb_audio_active();
bool usb_audio_push(const int16_t* pcm, int frames, int src_rate, int channels);

// ── HID boot keyboard (read by keyboard_read_cb in main.cpp) ─────────────────
// Copies the USB-held key set (ASCII-indexed, shift already applied) into
// out[128]. Returns false (and touches nothing) when no keyboard is attached
// or no key is held — callers can skip their merge loop.
bool usb_kbd_snapshot(bool out[128]);

// ── MSC thumb drive: sector API (consumed by usb_fs.cpp's diskio) ───────────
// Callable from ANY task; transactions are serialized internally and
// completions are pumped by usb_task. All return false once the device is
// gone (ready() flips first). Reads/writes chunk internally; `count` is in
// device sectors of usb_msc_sector_size() bytes. usb_msc_sync() flushes the
// device write cache (best-effort — many sticks stub it).
bool     usb_msc_ready();
uint32_t usb_msc_sector_count();
uint32_t usb_msc_sector_size();
bool     usb_msc_read(uint32_t lba, uint32_t count, uint8_t* buf);
bool     usb_msc_write(uint32_t lba, uint32_t count, const uint8_t* buf);
bool     usb_msc_sync();

// Log a line into the USB log ring (drained by Tools/USB via _usb_poll).
void usb_ulog(const char* fmt, ...);

// ── Flash-write safety ───────────────────────────────────────────────────────
// Writing INTERNAL flash (LittleFS/NVS) disables the CPU cache and freezes both
// cores for ms. That stalls the USB host controller past its 1ms isochronous
// deadline and crashes the (flash-resident, non-IRAM) prebuilt host stack.
// Bracket EVERY internal-flash write with these: begin() drains and pauses the
// ISO stream; end() resumes it. No-op when USB host isn't streaming. Safe from
// any task/core; concurrent callers are serialized. Prefer the RAII guard.
void usb_flash_guard_begin();
void usb_flash_guard_end();

// RAII: `{ UsbFlashGuard _g; ...write flash...; }` — end() always runs.
struct UsbFlashGuard {
    UsbFlashGuard()  { usb_flash_guard_begin(); }
    ~UsbFlashGuard() { usb_flash_guard_end(); }
    UsbFlashGuard(const UsbFlashGuard&) = delete;
    UsbFlashGuard& operator=(const UsbFlashGuard&) = delete;
};

// Conditional RAII for call sites whose target is only sometimes internal
// flash (e.g. an fs::FS* that may be SD or LittleFS). Guard ONLY when the
// write really hits internal flash: SD writes are SPI (no cache stall), and
// guarding them would pause the USB audio stream for nothing.
struct UsbFlashGuardIf {
    bool active;
    explicit UsbFlashGuardIf(bool internal_flash) : active(internal_flash) {
        if (active) usb_flash_guard_begin();
    }
    ~UsbFlashGuardIf() { if (active) usb_flash_guard_end(); }
    UsbFlashGuardIf(const UsbFlashGuardIf&) = delete;
    UsbFlashGuardIf& operator=(const UsbFlashGuardIf&) = delete;
};

// ── Prefs (persisted via firmware_prefs in main.cpp) ─────────────────────────
// Setters do NOT save; the Lua bridge saves after set, the prefs loader
// calls them bare.
bool usb_audio_pref_get();                        // route audio to USB (default on)
void usb_audio_pref_set(bool on);
bool usb_speaker_pref_get();                      // speaker stays on while routed (default off)
void usb_speaker_pref_set(bool on);
