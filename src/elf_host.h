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

// LoRa-protocol module load/unload (segments in the protocol pool; ops
// struct is the module's exported `loraproto_ops`). See
// src/radio/proto_loader.cpp.
void* elf_loraproto_load(const char* path, const void** out_ops);
void  elf_loraproto_unload(void* mod);
// BLE-slot protocol modules: same pool/exports, plus unresolved imports fall
// back to the loaded LoRa protocol elf (coupled protocols link against it at
// load; the miss under any other LoRa protocol IS the dependency check).
void* elf_bleproto_load(const char* path, const void** out_ops);
void  elf_bleproto_unload(void* mod);

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

// T-Deck peer link, gblink service (src/tdeck_link.cpp). status: 0 = no
// channel, 1 = peer session up (cable present), 2 = session + remote GameBoy
// attached (lockstep window applies). send cmds mirror TDL_GB_*: 1 ATTACH,
// 2 DETACH, 3 SYNC1, 4 SYNC2, 5 SYNC3(ack), 6 TSYNC; data_ctrl =
// (control<<8)|data (BGB b3/b2), ts = 2MiHz emulated clock (SYNC1/TSYNC).
// poll: next event as (cmd<<16)|(ctrl<<8)|data with timestamp in *ts_out.
int host_link_status(void);
int host_link_gb_send(int cmd, int data_ctrl, unsigned int ts);
int host_link_gb_poll(unsigned int* ts_out);

// T-Deck peer link, dgram service (tdeck_link.h TDL_SVC_DGRAM): datagrams of
// up to 1500 bytes to the module running on the other deck, fire-and-forget
// like UDP. open marks this module as listening (and pauses the mesh for the
// linked session, like a GameBoy link game); close undoes it — elf_host also
// closes after every module run. send: 1 handed to the wire, 0 dropped (no
// session, or the transport was full). recv: the next datagram's length
// (copied into buf, truncated to max), or -1 when none is waiting. Whether a
// peer is present is host_link_status() & 3 >= 1, as for gblink.
int  host_link_dgram_open(void);
void host_link_dgram_close(void);
int  host_link_dgram_send(const void* data, int len);
int  host_link_dgram_recv(void* buf, int max);

// WiFi sockets (src/net_bridge.cpp). IPv4 addresses are host-order u32
// (192.168.1.5 = 0xC0A80105); ports host-order ints. Every socket is
// non-blocking; a module polls. Up to 4 sockets per run; elf_host closes
// any left open after the module exits. While a module socket is open,
// WiFi modem sleep is off (it adds up to a beacon interval of receive
// latency per packet), and restored at the last close.
// host_net_status: 1 while the station is connected with an address.
// host_net_resolve: dotted quad or DNS name -> address, 0 on failure
// (DNS needs the station connected; blocks for the lookup).
int      host_net_status(void);
unsigned host_net_local_ip(void);
unsigned host_net_resolve(const char* name);
// UDP: open binds port (0 = any) with broadcast enabled -> socket or -1.
// send -> bytes sent, 0 would-block, -1 error. recv -> bytes, 0 none,
// -1 error; *ip/*port receive the sender (either may be NULL).
int  host_udp_open(int port);
int  host_udp_send(int sock, unsigned ip, int port, const void* data, int len);
int  host_udp_recv(int sock, unsigned* ip, int* port, void* buf, int max);
// TCP: connect blocks up to timeout_ms -> socket or -1 (Nagle off). listen
// -> listening socket or -1; accept -> a connected socket (Nagle off) or -1
// when nothing is pending. send -> bytes accepted (may be short), 0
// would-block, -1 closed/error. recv -> bytes, 0 none yet, -1 closed/error.
int  host_tcp_connect(unsigned ip, int port, int timeout_ms);
int  host_tcp_listen(int port);
int  host_tcp_accept(int lsock, unsigned* ip, int* port);
int  host_tcp_send(int sock, const void* data, int len);
int  host_tcp_recv(int sock, void* buf, int max);
// Close any socket from the calls above (UDP or TCP).
void host_net_close(int sock);

#ifdef __cplusplus
}
#endif
