#pragma once

#include "lora_proto_abi.h"

// ── LoRa-protocol selection, module loading, and dispatch ───────────────────
// Boot flow (main.cpp setup order):
//   1. proto_pool_init()                        — very early, low PSRAM
//   2. lora_proto_select_and_load()             — resolves firmware_prefs
//      lora_protocol= to an active protocol: .loraproto.elf load from
//      L:/meshpunk/lora_protos/<id>/, ABI + id validation, ops->init().
//      A missing or invalid elf boots the no-radio floor with a bell notice
//      (the pref is left untouched).
//   3. lora_proto_start() (the protocol programs the radio via the host API
//      and enters RX).
//   4. mesh_task calls lora_proto_loop() every tick (Core 1, MESH_LOCK held).

void lora_proto_select_and_load(void);
const LoraProtoOps* lora_proto_ops(void);   // never NULL

bool lora_proto_start(void);           // false = radio not running
void lora_proto_loop(void);
void lora_proto_stop(void);
void lora_proto_flush(void);

// Protocol Lua surface (ABI v2; no-ops for protocols without one). Call open
// after every Lua bring-up's binding registration, close before every
// teardown, tick from the Core-0 loop while Lua is up. L is the lua_State*.
void lora_proto_lua_open(void* L);
void lora_proto_lua_close(void);
void lora_proto_lua_tick(void* L);

// One-shot: the deferred user-visible notice from a load or start failure,
// for notify_post once the notify system is up. NULL when the boot was clean.
const char* lora_proto_boot_notice(void);

// Offline protocol settings: read/write an INACTIVE protocol's cfg + notify
// files on the mesh storage (<prefix>/<id>/). The protocol re-validates at
// its next boot. Get returns the value length (0 = no value); set is atomic
// (tmp + rename) and preserves keys it does not touch. Never call these for
// the ACTIVE protocol — route through its vtable instead.
int  lora_proto_offline_config_get(const char* id, const char* key,
                                   char* out, int out_sz);
bool lora_proto_offline_config_set(const char* id, const char* key,
                                   const char* val);
