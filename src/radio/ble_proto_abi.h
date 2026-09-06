#pragma once

// ── Meshpunk BLE protocol-slot ABI ───────────────────────────────────────────
// The second protocol slot (docs/PROTOCOL_ABI.md §6): the device has two
// independent protocol radios, so it has two independent slots. `ble_protocol=`
// in firmware_prefs picks this one ("none" is first-class); it is orthogonal
// to `lora_protocol=` — the slots share no hardware and run simultaneously.
// A protocol that proxies a LoRa protocol declares it in requires_lora and
// the selector refuses mismatches loudly; a standalone BLE protocol (a BLE
// mesh) combines with any LoRa protocol.
//
// Plain C, same append-only versioning discipline as lora_proto_abi.h: the
// selector accepts abi <= BLE_PROTO_ABI_VERSION and gates appended fields on
// the protocol's declared abi.

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BLE_PROTO_ABI_VERSION 1

// Host services for MODULE BLE protocols. The NimBLE stack stays firmware
// (internal-SRAM budget); a module protocol drives it through the serial
// transport below — POLLED and FRAME-based, mirroring MeshCore's
// BaseSerialInterface one-to-one (the shape its first client, the ported
// companion, actually consumes). Frames are at most 172 bytes
// (BaseSerialInterface MAX_FRAME_SIZE — the wire contract).
typedef struct BleHostApi {
    uint32_t abi;                    // == BLE_PROTO_ABI_VERSION

    // ── Serial transport ──
    // open: bring the BLE stack up, start the secure serial service and
    // advertise as <name_prefix><dev_name> ("@@MAC" in dev_name expands to
    // the MAC tail) with the numeric pairing pin. close: full teardown —
    // the BLE stack's internal RAM is freed (module runs borrow it).
    bool (*transport_open)(const char* name_prefix, const char* dev_name,
                           uint32_t pin);
    void (*transport_close)(void);
    void (*transport_enable)(void);
    void (*transport_disable)(void);
    bool (*transport_connected)(void);
    bool (*transport_write_busy)(void);
    int  (*transport_write)(const uint8_t* frame, int len);
    // One whole frame into a >=172-byte buffer; 0 = none pending.
    int  (*transport_read)(uint8_t* buf);

    // ── Shared services (same semantics as MeshHostApi) ──
    void     (*notify_post)(const char* text);
    uint32_t (*clock_now)(void);
    uint32_t (*millis32)(void);
    void*    (*mem_alloc)(size_t size);   // the boot-reserved protocol pool
    void     (*mem_free)(void* p);
    void     (*log)(const char* fmt, ...);
    // USER-initiated device restart carried by the protocol (the phone
    // app's explicit reboot command, identity changes the app requested).
    // Never for a protocol's own purposes — the host logs and restarts.
    void     (*device_reboot)(void);
} BleHostApi;

// The BLE protocol's vtable, exported from the module elf as `bleproto_ops`
// (the meshcore package's companion is the first client).
typedef struct BleProtoOps {
    uint32_t    abi;                 // == BLE_PROTO_ABI_VERSION
    const char* id;                  // lowercase [a-z0-9_]
    const char* name;                // display name
    // LoRa-slot protocol this one proxies (the companion fronts the MeshCore
    // mesh); NULL = standalone. Enforced at selection AND at boot, loudly —
    // never silently degraded.
    const char* requires_lora;

    // init runs early in boot (before LVGL/Lua) so BLE-stack allocations
    // land low in internal SRAM; start runs after the LoRa protocol is up.
    // loop ticks on the mesh task (Core 1). stop is a FULL stop — the BLE
    // stack is deinitialized and its RAM freed (module runs borrow it);
    // init+start bring it back.
    bool (*init)(const BleHostApi* host);
    bool (*start)(void);
    void (*loop)(void);
    void (*stop)(void);
    void (*flush)(void);             // persist state before power-off/restart
} BleProtoOps;

#ifdef __cplusplus
}
#endif
