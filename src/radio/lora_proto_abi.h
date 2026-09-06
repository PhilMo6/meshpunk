#pragma once

// ── Meshpunk LoRa-protocol ABI ───────────────────────────────────────────────
// The contract between the firmware host and a LoRa protocol package, loaded
// at boot as a .loraproto.elf module from L:/meshpunk/lora_protos/<id>/.
// Plain C: this header is included verbatim by protocol-module builds, which
// have no Arduino headers.
//
// VERSIONING: both structs are APPEND-ONLY. The loader accepts any module
// with abi <= LORA_PROTO_ABI_VERSION and gates every access to an appended
// field on the module's DECLARED abi (an older struct physically ends before
// the appended fields). The MeshHostApi handed to an older module is the
// current struct — safe, it reads only its known prefix. A module built
// against a NEWER abi than the firmware is refused at load, loudly.
// History: v1 = the original surface; v2 appends lua_open/lua_close/lua_tick
// (protocol-registered Lua bindings) and clock_suggest; v3 appends the radio
// airtime/CSMA/recovery ops + batt_mv the MeshCore package's dispatcher
// needs (radio_time_on_air_ms, radio_current_rssi, radio_reset_agc,
// radio_set_rx_boost, batt_mv). Pre-v0.4.0 (nothing fielded) the vocabulary
// was renamed stack→protocol: the export symbol is loraproto_ops (was
// meshstack_ops) — a same-layout, new-name contract.
//
// RULES:
//  - Exactly ONE LoRa protocol is active per boot, chosen by the
//    firmware_prefs lora_protocol= key. Switching protocols is a reboot.
//  - loop() runs on mesh_task (Core 1) ONLY, under MESH_LOCK, with bounded
//    work per call (the task ticks every 2ms). Protocols create no tasks and
//    install no ISRs in v1 — RX is polled via host->radio_poll_irq().
//  - send_* / get_peers / get_config / set_config are called from the Lua
//    core (Core 0) under MESH_LOCK.
//  - prepare_sleep/note_wake/flush integrate with the power system. NULL
//    means the protocol needs no sleep-specific work there — correct for
//    polled protocols (light-sleep standby works: the chip stays armed in
//    RX, DIO1 level-wakes the host, and the poll loop reads the IRQ register).
//  - Memory: mem_alloc/mem_free draw from the boot-reserved protocol pool
//    (PSRAM, frag-safe). Files: modules use the exported fopen family with
//    VFS paths; init() receives the protocol's own install dir.

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LORA_PROTO_ABI_VERSION 3

// One row of get_peers() output — the generic peer surface the launcher/apps
// can show for any protocol (a protocol's own apps may know richer data via
// get_config blobs).
typedef struct {
    char     id[24];       // protocol-native node id (hex/short form)
    char     name[32];     // display name
    uint32_t last_heard;   // device epoch seconds (0 = never)
    float    snr;
    float    rssi;
} LoraProtoPeer;

// Host services passed to init(). All function pointers valid for the
// lifetime of the boot.
typedef struct MeshHostApi {
    uint32_t abi;                    // == LORA_PROTO_ABI_VERSION

    // ── Radio HAL (exclusive chip ownership while a protocol is active) ──
    bool     (*radio_config)(float freq_mhz, float bw_khz, uint8_t sf, uint8_t cr,
                             int8_t tx_dbm_radiated, bool crc,
                             uint8_t sync_word, uint16_t preamble_len);
    bool     (*radio_start_receive)(void);
    uint32_t (*radio_poll_irq)(void);                       // SX126x IRQ flag snapshot
    int      (*radio_read_packet)(uint8_t* buf, int max_len); // after RX_DONE; re-arms RX
    bool     (*radio_start_send)(const uint8_t* buf, int len);
    void     (*radio_send_finished)(void);                  // after TX_DONE; re-enters RX
    float    (*radio_last_rssi)(void);
    float    (*radio_last_snr)(void);
    bool     (*radio_standby)(void);
    bool     (*radio_sleep)(void);

    // ── Shared message store (mstore) ──
    // Callers get paged chat, unread badges, notifications and retention for
    // free by landing messages here. channel_idx is a record field only
    // (paths are name-keyed); pass the protocol's own channel ordinal.
    void (*store_channel_msg)(const char* ch_name, int channel_idx,
                              const char* from, const char* text,
                              uint32_t timestamp, float snr, float rssi,
                              uint8_t hops, bool direct);
    void (*store_dm_msg)(const char* peer, const char* from, const char* text,
                         uint32_t timestamp, float snr, float rssi,
                         uint8_t hops, bool direct);
    // Unread counters (call from loop() — MESH_LOCK is already held there).
    void (*unread_bump_channel)(const char* ch_name);
    void (*unread_bump_dm)(const char* name);

    // ── Notifications (safe from the mesh task; C-side, work during ELF runs) ──
    void (*notify_message_alert)(void);
    void (*notify_post)(const char* text);

    // ── Clock / misc ──
    uint32_t (*clock_now)(void);         // device epoch seconds (host clock authority)
    uint32_t (*millis32)(void);
    void     (*random_bytes)(uint8_t* out, int len);   // hardware TRNG
    void     (*log)(const char* fmt, ...);             // serial log (line-atomic)

    // ── Memory (boot-reserved PSRAM protocol pool; frag-safe load/unload) ──
    void* (*mem_alloc)(size_t size);
    void  (*mem_free)(void* p);

    // ── Packet capture (Tools/Packets monitor) ──
    // Push one complete wire frame into the capture ring (no-op unless the
    // user armed capture). Call from loop() — MESH_LOCK is already held
    // there, which is the ring's contract. dir: 0=rx 1=tx 2=tx_fail.
    void (*capture_frame)(uint8_t dir, const uint8_t* raw, int len,
                          float snr, float rssi);

    // ── Protocol data home ──
    // VFS path of this protocol's DATA directory on the CHOSEN mesh storage
    // (SD when the user selected it, else internal) — created by the host
    // before init(). Reflash-surviving state (peers, config, the identity
    // copy) belongs here, the way MeshCore keeps contacts on the user's
    // storage. init()'s proto_dir stays the internal-always install dir:
    // keep the identity keypair in BOTH and heal whichever copy is missing
    // at boot (the MeshCore identity rule — fixes new-identity-per-reflash).
    const char* (*data_dir)(void);

    // ── Device GPS ──
    // Last-known fix (most recent real fix or the boot seed); false = none
    // yet, lat/lon untouched. For position broadcasts — the protocol never
    // drives the GPS hardware, it reads the host's fix.
    bool (*gps_fix)(double* lat, double* lon);

    // ── v2 appends below (read only when host/module abi >= 2) ──

    // Mesh-derived time offered to the host's clock authority. quality 0-4
    // maps onto the host tier table (0 seed .. 3 GPS-fix-grade .. 4 user-
    // typed); higher existing authority wins and the host decides — the
    // protocol NEVER sets the clock directly. Returns true if accepted.
    bool (*clock_suggest)(uint32_t epoch, uint8_t quality);

    // ── v3 appends below (read only when host/module abi >= 3) ──

    // Estimated air time for a packet of len_bytes at the CURRENT radio
    // params, in milliseconds (dispatcher airtime budgets).
    uint32_t (*radio_time_on_air_ms)(int len_bytes);
    // Instantaneous channel RSSI — CSMA checks and noise-floor sampling
    // (radio_last_rssi is the LAST PACKET's).
    float    (*radio_current_rssi)(void);
    // Full receiver recovery (warm sleep + recalibration). Leaves standby —
    // the protocol re-enters RX afterwards.
    void     (*radio_reset_agc)(void);
    void     (*radio_set_rx_boost)(bool en);
    // Battery in millivolts; 0 = unknown on this board.
    uint16_t (*batt_mv)(void);
} MeshHostApi;

// The protocol's vtable. A module exports this as `loraproto_ops`.
typedef struct LoraProtoOps {
    uint32_t    abi;                 // == LORA_PROTO_ABI_VERSION
    const char* id;                  // lowercase [a-z0-9_], must match the install dir
    const char* name;                // display name

    // Boot: init (host services + the protocol's install dir, VFS path form,
    // e.g. "/littlefs/meshpunk/lora_protos/mtlite") then start (radio is
    // begun; program params and enter RX). false from either = loud notice
    // and the radio stays off for the boot (nothing is substituted).
    bool (*init)(const MeshHostApi* host, const char* proto_dir);
    bool (*start)(void);
    // One bounded slice of protocol work (mesh_task, Core 1, MESH_LOCK held).
    void (*loop)(void);
    void (*stop)(void);

    // TX surface (Core 0, MESH_LOCK held). Return false = not sent.
    bool (*send_channel_text)(const char* channel, const char* text);
    bool (*send_text)(const char* peer, const char* text);

    // Peer table snapshot; returns rows filled.
    int  (*get_peers)(LoraProtoPeer* out, int max);

    // Opaque per-protocol config (the protocol's own settings app owns the
    // schema). get returns bytes written (0 = unknown key), set returns
    // accepted.
    int  (*get_config)(const char* key, char* out, int out_sz);
    bool (*set_config)(const char* key, const char* val);

    // Power integration. NULL = no protocol-specific sleep work needed
    // (polled protocols: standby just works — see the RULES note above).
    void (*prepare_sleep)(void);
    void (*note_wake)(void);
    void (*flush)(void);             // persist state before power-off/restart

    // ── v2 appends below (host reads them only when abi >= 2) ──

    // Protocol-registered Lua bindings: the package ships its Lua surface
    // the same way it ships its apps. `L` is a lua_State* (void* keeps this
    // header Lua-free); the lua_* C API resolves via proto_exports[].
    //  - lua_open: a fresh Lua state is up — register this protocol's
    //    bindings. Called at every Lua bring-up (Lua is torn down for ELF
    //    game runs and rebuilt after).
    //  - lua_close: the state is about to die — drop every lua_State
    //    reference. Registered C functions need no cleanup (they die with
    //    the state); queues feeding lua_tick must simply stop being drained.
    //  - lua_tick: Core-0 pump, called continuously while Lua is up. Drain
    //    protocol->UI events here (push received messages into open views).
    // All three NULL = no Lua surface (config keys only), the v1 behavior.
    void (*lua_open)(void* L);
    void (*lua_close)(void);
    void (*lua_tick)(void* L);
} LoraProtoOps;

#ifdef __cplusplus
}
#endif
