#pragma once

// Firmware bridge functions (elf_host.cpp mcs_*) for the mstore calls whose
// firmware signatures carry String/fs::FS — the shim types are not
// layout-compatible, so paths cross as char* and the storage backend stays
// firmware-side. StoredMsg/ObservedPath are shared PODs (mesh_store.h).

#include <stdint.h>

struct StoredMsg;
struct ObservedPath;

extern "C" {
int  mcs_channel_msg_path(const char* name, char* out, int out_sz);
int  mcs_dm_msg_path(const char* peer, char* out, int out_sz);
int  mcs_messages_dir_path(char* out, int out_sz);
void mcs_append_extra_path(const char* msg_log_path, const uint8_t* hash,
                           const ObservedPath* op);
int  mcs_read_one_stored_msg(const char* path, uint32_t offset, StoredMsg* m);
}

// ── Package-internal cross-TU helpers ───────────────────────────────────────

// Live radio reconfiguration over the host ops (mcmain.cpp — mesh-locked
// across standby+config, re-sets the polled radio's SF for packetScore).
void mc_apply_radio_params(float freq, float bw, uint8_t sf, uint8_t cr,
                           int8_t tx_dbm);

// User-initiated device restart via the BLE slot's host (mc_bleproto.cpp).
void mc_device_reboot(void);

// Board radiated TX-power cap (mcmain.cpp): host-forwarded "max_tx_dbm"
// config at boot; baked MAX_LORA_TX_POWER only under older firmware.
extern int8_t mc_max_tx_dbm;

// MC-PKG divergence: the firmware's live-reapply helpers under their
// firmware NAMES, so copied binding/companion bodies stand verbatim. TX
// power rides the full config re-apply (no lone power setter in the api).
class PunkMesh;
extern PunkMesh* the_mesh;

#include "punkmesh.h"   // _prefs access for the inline pair

static inline void radio_apply_params(float freq, float bw, uint8_t sf,
                                      uint8_t cr) {
    mc_apply_radio_params(freq, bw, sf, cr,
                          (int8_t)(the_mesh ? the_mesh->_prefs.tx_power_dbm
                                            : 20));
}
static inline void radio_apply_tx_power(int8_t dbm) {
    if (!the_mesh) return;
    mc_apply_radio_params(the_mesh->_prefs.freq, the_mesh->_prefs.bandwidth,
                          the_mesh->_prefs.spreading_factor,
                          the_mesh->_prefs.coding_rate, dbm);
}
