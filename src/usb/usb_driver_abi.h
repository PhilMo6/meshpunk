#pragma once

// ── Meshpunk USB driver ABI ──────────────────────────────────────────────────
// The contract between the USB host core (src/usb/usb_core.cpp) and a class
// driver — whether compiled into firmware (src/usb/usb_drv_*.cpp) or loaded
// at runtime as a .drv.elf module. Plain C: this header is included verbatim
// by driver-module builds (modules/usbdrv_*), which have no Arduino headers.
//
// VERSIONING: after the first dynamic driver ships, both structs are
// APPEND-ONLY. Any breaking change bumps USB_DRIVER_ABI_VERSION and the
// loader refuses mismatched modules (logged, non-fatal).
//
// RULES (the invariants the vtable exists to enforce):
//  - probe/start/stop/want/tick/park/resume/busy run in usb_task (core 1)
//    ONLY; the core calls them there. Drivers never create tasks (v1).
//  - control() and pipe_xfer() are DUAL-CONTEXT: callable from any task.
//    On usb_task they self-pump the event handlers; elsewhere they block on
//    a per-waiter semaphore that usb_task's pump gives.
//  - pipe_submit() callbacks fire in usb_task. A periodic resubmit loop MUST
//    check flash_guard_pending() and decline to resubmit while it is set
//    (drivers with such loops also provide busy/park/resume so the core can
//    quiesce them around internal-flash writes; drivers with only bulk
//    traffic pass all three as NULL = flash-guard-exempt).
//  - input_key() carries RAW host pseudo-codes, untranslated and pre-keymap:
//    printable ASCII, 0x0D/0x08/0x1B/0x09/0x7F, arrows 0x81-0x84, click
//    0x85, modifiers 0x80 shift / 0x8B ctrl / 0x8C alt / 0x8D gui, F-keys
//    0xB0-0xBB. 0x8C ALT EDGES ARE MANDATORY for keyboard drivers — the
//    module exit chord is Alt+Backspace and elf_host latches both from
//    these codes. stop()/detach must emit release edges for every held key
//    AND modifier (no stuck chords).
//  - No heap in drivers: static buffers + pipe_buf(). The module export
//    table deliberately has no malloc.
//  - Stack budget: <= 2KB per driver call (everything runs on usb_task's
//    8KB stack).

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define USB_DRIVER_ABI_VERSION 1

typedef struct UsbPipe UsbPipe;      // opaque, core-owned

typedef enum {
    USB_XFER_OK = 0,
    USB_XFER_STALL,
    USB_XFER_TIMEOUT,
    USB_XFER_NO_DEVICE,
    USB_XFER_CANCELED,
    USB_XFER_ERROR,
} UsbXferResult;

// Block-device socket: a storage driver registers this; the firmware's
// FatFs layer (usb_fs.cpp) mounts through it. One binding at a time.
typedef struct {
    bool     (*ready)(void);
    uint32_t (*sector_count)(void);
    uint32_t (*sector_size)(void);
    bool     (*read)(uint32_t lba, uint32_t count, uint8_t* buf);
    bool     (*write)(uint32_t lba, uint32_t count, const uint8_t* buf);
    bool     (*sync)(void);
} UsbBlockOps;

// Peer-link socket: the `tdeck` link driver registers this; the firmware's
// T-Deck↔T-Deck bridge (src/tdeck_link.cpp) transmits frames through it.
// send() must deliver one whole frame (<= 64 bytes) and may block briefly
// (ride pipe_xfer — dual-context). One binding at a time.
typedef struct {
    bool (*send)(const uint8_t* d, uint32_t n);
} UsbLinkOps;

// Host services passed to every driver call. All function pointers valid
// for the lifetime of the USB session; descriptor pointers valid from
// probe until the stop(dev_present=false)/detach that follows DEV_GONE.
typedef struct UsbHostApi {
    uint32_t abi;                    // == USB_DRIVER_ABI_VERSION

    // Logging → the Tools/USB ring (serial is dead in host mode).
    void (*log)(const char* fmt, ...);

    // Cached descriptors of the attached device (read-only; the audio
    // driver's MPS patch is a core-internal privilege, not an ABI feature).
    const void* (*device_desc)(void);        // const usb_device_desc_t*
    const void* (*config_desc)(void);        // const usb_config_desc_t*

    // Interface lifecycle.
    bool (*claim_interface)(uint8_t ifnum, uint8_t alt);
    void (*release_interface)(uint8_t ifnum);

    // Control transfer, DUAL-CONTEXT (any task). IN payloads (bmReqType
    // bit7) are copied out into `data` on success.
    bool (*control)(uint8_t bmReqType, uint8_t bReq, uint16_t wValue,
                    uint16_t wIndex, uint8_t* data, uint16_t wLength);

    // Pipes: the core allocates the DMA transfer object and owns
    // submit/wait/teardown. buf_len is the transfer buffer size; IN wire
    // lengths must be MPS-multiples (round up; short completions are fine).
    UsbPipe* (*pipe_open)(uint8_t ep_addr, uint16_t mps, uint32_t buf_len);
    void     (*pipe_close)(UsbPipe* p);
    uint8_t* (*pipe_buf)(UsbPipe* p);
    // Synchronous transfer (dual-context). Returns the final result; on
    // TIMEOUT the core has already forced the transfer to complete (halt +
    // flush + host-side clear) so the pipe is reusable.
    UsbXferResult (*pipe_xfer)(UsbPipe* p, uint32_t len, uint32_t timeout_ms);
    // Asynchronous submit; cb fires in usb_task. Resubmitting from inside
    // the callback is the intended idiom for interrupt-IN loops.
    bool (*pipe_submit)(UsbPipe* p, uint32_t len,
                        void (*cb)(UsbPipe* p, UsbXferResult res,
                                   uint32_t actual, void* ctx),
                        void* ctx);
    int  (*pipe_in_flight)(UsbPipe* p);       // 0 or 1
    // Force a pending transfer to complete (halt+flush, host side only).
    // The completion arrives as CANCELED through the normal path.
    void (*pipe_cancel)(UsbPipe* p);
    // Recover a halted endpoint on BOTH sides (wire CLEAR_FEATURE + host
    // endpoint clear — resets the data toggles). Does not resubmit.
    void (*pipe_reset)(UsbPipe* p);
    // actual_num_bytes of the pipe's most recent completed transfer.
    uint32_t (*pipe_actual)(UsbPipe* p);
    // Retarget an IDLE pipe to another endpoint (shares the buffer between
    // a bulk IN/OUT pair the way BOT reuses one transfer). No-op in flight.
    void (*pipe_set_ep)(UsbPipe* p, uint8_t ep_addr);

    // Input sockets (feed the firmware's two input pipelines).
    void (*input_key)(uint8_t code, bool pressed);         // → ELF module kq
    void (*input_set_held)(const bool held[128], int n);   // → UI held image
    void (*input_nav)(int up, int down, int left, int right, int click);
    // True while an ELF module owns input: route keys as input_key edges and
    // suppress input_nav (games consume the raw counters differently).
    bool (*input_module_active)(void);

    // Block-device socket.
    bool (*block_register)(const UsbBlockOps* ops);
    void (*block_unregister)(void);

    // Misc.
    uint32_t (*ticks_ms)(void);
    void     (*delay_ms)(uint32_t ms);
    bool     (*flash_guard_pending)(void);
    // Pump the host event handlers once (blocking up to ms). usb_task
    // context ONLY (no-op elsewhere) — for stop() paths that must keep
    // completions flowing while waiting out a foreign-task transaction.
    void     (*pump)(uint32_t ms);

    // ── Per-driver config + app channel ─────────────────────────────────────
    // Both take the driver's OWN exported desc as `self` so the shared
    // vtable can attribute the call (no hidden current-driver bookkeeping).
    //
    // config(): the optional `conf` file from the driver's install dir,
    // slurped by the core at attach (before probe) and freed at unload.
    // Re-read on every attach — REPLUGGING THE DEVICE APPLIES NEW CONFIG
    // (same lifecycle as the match manifest and .disabled). NULL/len 0 when
    // absent, and always for built-ins. Cap 1KB.
    const uint8_t* (*config)(const void* self, uint32_t* len);
    // publish(): driver → Lua app channel. The core keeps the LATEST blob
    // (cap 64 bytes) + a sequence counter per dynamic driver; apps poll it
    // with _usb_drv_read(name) -> blob, seq. Publish CHANGES, not every
    // report — this is a diagnostic/learning feed, not a data path.
    void (*publish)(const void* self, const void* data, uint32_t len);

    // ── T-Deck peer-link socket (mirror of the block socket) ────────────────
    // link_register in start(), link_unregister in stop(); link_rx feeds the
    // firmware bridge with received bytes (usb_task context, e.g. from a
    // bulk-IN resubmit loop's completion callback).
    bool (*link_register)(const UsbLinkOps* ops);
    void (*link_unregister)(void);
    void (*link_rx)(const uint8_t* data, uint32_t len);
} UsbHostApi;

// A class driver. Built-ins register this via the core; modules export it
// as the symbol `usbdrv_ops` (fetched with elf_lookup after load).
typedef struct {
    uint32_t    abi;                 // must equal USB_DRIVER_ABI_VERSION
    const char* name;                // short + unique: "audio", "kbd", "msc"

    // Enumeration (usb_task): inspect config_desc(), return true to attach
    // to this device. The driver owns its profile capture (walk the raw
    // descriptor bytes; usb_parse_next_descriptor is available to built-ins,
    // modules walk manually — descriptors are plain packed structs).
    // MUST fully reset the driver's own state at entry (probe is also the
    // per-connect state reset).
    bool (*probe)(const UsbHostApi* api);

    // Optional: when non-NULL, the core starts the driver only while
    // want() is true and stops it (dev_present=true) when it goes false —
    // e.g. audio's routing/tone prefs. NULL = want whenever probed.
    bool (*want)(void);

    bool (*start)(const UsbHostApi* api);     // claim + arm; false = failed
    void (*stop)(const UsbHostApi* api, bool dev_present);

    // Flash-guard quiesce. ALL three NULL = exempt (pure bulk drivers).
    // busy(): in-flight periodic transfers; park(): force them to drain
    // (may be a no-op when the completion callback drains naturally by
    // declining resubmit); resume(): re-arm after the write.
    int  (*busy)(void);
    void (*park)(void);
    void (*resume)(void);

    // Optional per-pump-loop hook (LED updates, repeat timers). usb_task.
    void (*tick)(const UsbHostApi* api);

    // Short status fragment for the UI ("1.9GB", "kbd"). Optional.
    void (*status)(char* out, uint32_t n);
} UsbDriverDesc;

#ifdef __cplusplus
}
#endif
