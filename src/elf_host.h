#pragma once

#include <Arduino.h>

struct lua_State;

// Register the _launch_elf() Lua binding.
void elf_host_register_lua(lua_State* L);

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
