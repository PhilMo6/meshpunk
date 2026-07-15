#pragma once

#include <Arduino.h>

struct lua_State;

// Register the _launch_elf() Lua binding.
void elf_host_register_lua(lua_State* L);

// Deferred ELF launch (Core 0 / loop). _launch_elf() only stashes the request;
// the main loop drives the teardown→run→recreate cycle around these:
//   if (elf_host_pending_take()) { luaTearDown(); elf_host_run_pending(); luaBringUp(); }
// elf_host_pending_take(): true (and marks running) if a launch was requested.
// elf_host_run_pending():  loads+runs the stashed module to completion. MUST be
//   called only after Lua is torn down — it never touches lua_State.
bool elf_host_pending_take(void);
int  elf_host_run_pending(void);

// Firmware-internal input injection (NOT module exports). Second producer for
// the key-event ring the Core-1 input task fills — used by the USB HID
// keyboard driver (usb_manager.cpp, usb_task on Core 1).
// elf_input_active(): true while a module owns input (input task alive).
// elf_input_inject(): queue one press/release edge; applies the module's
//   -keymap translation exactly like the matrix poll does. No-op when no
//   module is running.
bool elf_input_active(void);
void elf_input_inject(unsigned char key, int pressed);

// Dynamic USB driver modules (usb_core.cpp's attach-time loader; see the
// section comment in elf_host.cpp). out_ops receives the module's exported
// `usbdrv_ops` (a const UsbDriverDesc*). Segments live in the boot-reserved
// USB driver pool. usb_task context only.
void* elf_usb_driver_load(const char* path, const void** out_ops);
void  elf_usb_driver_unload(void* mod);

// ---------------------------------------------------------------------------
// Host functions exported to loaded ELF modules.
// These are resolved by name via the elf_loader symbol table.
// All SPI bus locking is handled internally.
// ---------------------------------------------------------------------------

#ifdef __cplusplus
extern "C" {
#endif

// Display: push a 320×200 RGB565 framebuffer to the TFT.
// Centered vertically in the 320×240 panel (20px letterbox top+bottom).
void host_blit_frame(const uint16_t* rgb565, int w, int h);

// Display: clear the entire screen to black.
void host_clear_screen(void);

// Timing
uint32_t host_get_ticks_ms(void);
void     host_sleep_ms(uint32_t ms);

// Input: read a key event from the queue.
// Returns 1 if an event was available, 0 if queue empty.
// *pressed = 1 for key-down, 0 for key-up.
// *key = Doom key code (ASCII or special constant).
int host_get_key(int* pressed, unsigned char* key);

// Input: raw trackball deltas for modules emulating a pointing device.
// First call opts in — the input task then stops consuming the trackball
// (no more 0x81-0x85 pseudo-keys) for the rest of the module run.
void host_trackball_read(int* dx, int* dy, int* click);

// Audio: push PCM samples (mono, 16-bit signed) to the I2S output.
void host_audio_push(const int16_t* samples, int count, int sample_rate);

// Lifecycle: returns true when the user wants to exit (ESC held).
int host_should_exit(void);

// File I/O: read an entire file from SD into a PSRAM buffer.
// Caller must free() the returned pointer.
// *out_size receives the file size. Returns NULL on failure.
void* host_read_file(const char* path, uint32_t* out_size);

// File I/O: write data to a file on SD. Returns 0 on success.
int host_write_file(const char* path, const void* data, uint32_t size);

// Debug
void host_log(const char* msg);

#ifdef __cplusplus
}
#endif
