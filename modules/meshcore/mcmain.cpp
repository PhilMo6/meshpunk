// meshcore.loraproto.elf — the MeshCore protocol package.
//
// lib/MeshCore core + Crypto compile and link against the host classes below
// (radio/clock/RNG/board over the MeshHostApi); punkmesh.cpp supplies the
// Mesh subclass (the_mesh) and mclua.cpp its Lua surface.
//
// Host classes:
//   McPolledRadio  mesh::Radio over the polled host radio ops — the package
//                  counterpart of src/radio/punk_polled_radio.h (R-1's
//                  hw-proven state machine, H->radio_* instead of radio_hal_*)
//   McClock        MillisecondClock over H->millis32
//   McRTC          RTCClock: reads the host clock; setCurrentTime routes to
//                  clock_suggest (the host authority decides — tier PHONE)
//   McRNG          mesh::RNG over the host TRNG
//   McBoard        MainBoard: battery via H->batt_mv; TX FEM hooks are a
//                  no-op (DIO2-as-RF-switch is programmed by the host HAL,
//                  the mtlite-proven arrangement); reboot is refused loudly —
//                  a protocol module must not restart the device.

#include "lora_proto_abi.h"
#include "mc_internal.h"

#include <Mesh.h>

// SX126x IRQ register bits (the host's poll_irq returns the chip register
// verbatim; values match RadioLib's RADIOLIB_SX126X_IRQ_*).
#define MC_IRQ_TX_DONE   0x0001u
#define MC_IRQ_RX_DONE   0x0002u
#define MC_IRQ_PREAMBLE  0x0004u
#define MC_IRQ_HDR_VALID 0x0010u

// ── mesh::Radio over the polled host ops ────────────────────────────────────
class McPolledRadio : public mesh::Radio {
    static const uint16_t NUM_FLOOR_SAMPLES = 64;   // RadioLibWrapper's values
    static const int      FLOOR_SAMPLING_THRESHOLD = 14;

    bool     _tx_wait = false;
    bool     _in_recv = false;   // module-tracked: it drives every transition
    uint32_t n_recv = 0, n_sent = 0, n_recv_errors = 0;
    int16_t  _noise_floor = 0;
    int16_t  _threshold = 0;
    uint16_t _num_floor_samples = 0;
    int32_t  _floor_sample_sum = 0;
    uint8_t  _sf = 10;              // as configured (packetScore)

    bool isReceivingPacket() {
        uint32_t irq = MCH->radio_poll_irq();
        return (irq & MC_IRQ_PREAMBLE) || (irq & MC_IRQ_HDR_VALID);
    }

public:
    void setSpreadingFactor(uint8_t sf) { _sf = sf; }

    void begin() override {
        _noise_floor = 0;
        _threshold = 0;
        _num_floor_samples = 0;
        _floor_sample_sum = 0;
    }

    int recvRaw(uint8_t* bytes, int sz) override {
        if (_tx_wait) return 0;
        int len = 0;
        if (MCH->radio_poll_irq() & MC_IRQ_RX_DONE) {
            int r = MCH->radio_read_packet(bytes, sz);   // re-arms RX either way
            _in_recv = true;
            if (r > 0) { n_recv++; len = r; }
            else       { n_recv_errors++; }
        } else if (!_in_recv) {
            _in_recv = MCH->radio_start_receive();
        }
        return len;
    }

    bool startSendRaw(const uint8_t* bytes, int len) override {
        if (MCH->radio_start_send(bytes, len)) {
            _tx_wait = true;
            _in_recv = false;
            return true;
        }
        _in_recv = MCH->radio_start_receive();
        return false;
    }

    bool isSendComplete() override {
        if (!_tx_wait) return false;
        if (MCH->radio_poll_irq() & MC_IRQ_TX_DONE) {
            n_sent++;
            return true;
        }
        return false;
    }

    void onSendFinished() override {
        _tx_wait = false;
        MCH->radio_send_finished();   // finishTransmit (clears IRQ) + re-enter RX
        _in_recv = true;
    }

    bool isInRecvMode() const override { return _in_recv && !_tx_wait; }

    bool isReceiving() override {
        if (isReceivingPacket()) return true;
        return _threshold != 0 &&
               MCH->radio_current_rssi() > _noise_floor + _threshold;
    }

    uint32_t getEstAirtimeFor(int len_bytes) override {
        return MCH->radio_time_on_air_ms(len_bytes);
    }

    float packetScore(float snr, int packet_len) override {
        static const float snr_threshold[] = { -7.5f, -10.0f, -12.5f,
                                               -15.0f, -17.5f, -20.0f };
        if (_sf < 7 || _sf > 12) return 0.0f;
        if (snr < snr_threshold[_sf - 7]) return 0.0f;
        float success = (snr - snr_threshold[_sf - 7]) / 10.0f;
        float collision_penalty = 1.0f - (packet_len / 256.0f);
        float score = success * collision_penalty;
        if (score < 0.0f) score = 0.0f;
        if (score > 1.0f) score = 1.0f;
        return score;
    }

    void loop() override {
        if (!_tx_wait && _num_floor_samples < NUM_FLOOR_SAMPLES) {
            if (!isReceivingPacket()) {
                int rssi = (int)MCH->radio_current_rssi();
                if (rssi < _noise_floor + FLOOR_SAMPLING_THRESHOLD) {
                    _num_floor_samples++;
                    _floor_sample_sum += rssi;
                }
            }
        } else if (_num_floor_samples >= NUM_FLOOR_SAMPLES &&
                   _floor_sample_sum != 0) {
            _noise_floor = (int16_t)(_floor_sample_sum / NUM_FLOOR_SAMPLES);
            if (_noise_floor < -120) _noise_floor = -120;
            _floor_sample_sum = 0;
        }
    }

    int getNoiseFloor() const override { return _noise_floor; }

    void triggerNoiseFloorCalibrate(int threshold) override {
        _threshold = (int16_t)threshold;
        if (_num_floor_samples >= NUM_FLOOR_SAMPLES) {
            _num_floor_samples = 0;
            _floor_sample_sum = 0;
        }
    }

    void resetAGC() override {
        uint32_t irq = MCH->radio_poll_irq();
        if (irq & (MC_IRQ_RX_DONE | MC_IRQ_TX_DONE |
                   MC_IRQ_PREAMBLE | MC_IRQ_HDR_VALID)) return;
        MCH->radio_reset_agc();
        _noise_floor = 0;
        _num_floor_samples = 0;
        _floor_sample_sum = 0;
        _in_recv = MCH->radio_start_receive();
    }

    float getLastRSSI() const override { return MCH->radio_last_rssi(); }
    float getLastSNR() const override { return MCH->radio_last_snr(); }
};

// ── Clock / RNG / board over the host ───────────────────────────────────────
class McRTC : public mesh::RTCClock {
public:
    uint32_t getCurrentTime() override { return MCH->clock_now(); }
    void setCurrentTime(uint32_t t) override {
        // Companion/CLI time-set path; the host clock authority arbitrates
        // (tier 2 = phone-grade, GPS and manual outrank).
        MCH->clock_suggest(t, 2);
    }
};

class McBoard : public mesh::MainBoard {
public:
    uint16_t getBattMilliVolts() override { return MCH->batt_mv(); }
    const char* getManufacturerName() const override { return "Meshpunk"; }
    void reboot() override { MCH->log("REFUSED: protocol module reboot request"); }
    uint8_t getStartupReason() const override { return BD_STARTUP_NORMAL; }
};

static McPolledRadio s_radio;
static McRTC         s_rtc;
static McBoard       s_board;

// ── PunkMesh — the protocol itself ──────────────────────────────────────────
#include "punkmesh.h"
#include "shim/meshpunk_sync.h"   // RxEvent + the module-owned queue

// punkmesh.cpp's extern (main.cpp's global in the firmware): defined HERE —
// the package owns its protocol object. Allocated from the pool at init
// (operator new → mem_alloc).
PunkMesh* the_mesh = nullptr;

// The companion dispatch point (ble_companion.h iface): owned by THIS elf so
// PunkMesh's RX pushes never import the optional companion elf — that elf
// imports this global and sets it while attached.
class BleCompanionIface;
BleCompanionIface* ble_companion = nullptr;

// Unseeded ON PURPOSE: StdRNG wraps ::random(), which is the host TRNG via
// the shim; seeding would switch it to a PRNG (the firmware-side lesson).
static StdRNG          fast_rng;
static SimpleMeshTables tables;

// RadioLib begin() defaults, programmed explicitly by the host HAL
// (RADIO_HAL_SYNC_WORD_DEFAULT/PREAMBLE — values are the wire contract).
#define MC_SYNC_WORD    0x12
#define MC_PREAMBLE_LEN 8

// ── LoraProtoOps ────────────────────────────────────────────────────────────
static bool mc_init(const MeshHostApi* host, const char* proto_dir) {
    (void)proto_dir;
    if (!host || host->abi < 3) return false;   // needs the v3 radio ops
    MCH = host;

    // RX→UI events (drained by lua_tick, step 5d).
    rx_event_queue = xQueueCreate(32, sizeof(RxEvent));

    // MeshCore's data lives at the MESH-STORAGE ROOT (zero migration: the
    // package finds everything the built-in wrote). The loader hands the
    // root directly for this package; stripping a "/meshcore" tail is a
    // belt for a host that handed the generic per-id dir.
    static char root[128];
    snprintf(root, sizeof(root), "%s", MCH->data_dir());
    size_t rl = strlen(root);
    const char* tail = "/meshcore";
    size_t tl = strlen(tail);
    if (rl > tl && strcmp(root + rl - tl, tail) == 0) root[rl - tl] = '\0';

    the_mesh = new PunkMesh(s_radio, fast_rng, s_rtc, tables);
    the_mesh->setStorage(strncmp(root, "/sd", 3) == 0 ? &SD : &LittleFS, root);
    MCH->log("meshcore package: init (data root %s)", root);
    return true;
}

static bool mc_start(void) {
    // The firmware MESHCORE INIT sequence, over the host ops: prefs load in
    // begin() FIRST (radio params come from _prefs — configuring earlier
    // programs constructor defaults), then params, boost, RX.
    the_mesh->begin();
    the_mesh->showWelcome();

    bool ok = MCH->radio_config(the_mesh->getFreqPref(),
                                the_mesh->getBandwidthPref(),
                                the_mesh->getSpreadingFactorPref(),
                                the_mesh->getCodingRatePref(),
                                (int8_t)the_mesh->getTxPowerPref(),
                                true, MC_SYNC_WORD, MC_PREAMBLE_LEN);
    s_radio.setSpreadingFactor(the_mesh->getSpreadingFactorPref());
    if (the_mesh->_prefs.rx_boost) MCH->radio_set_rx_boost(true);
    bool rx = MCH->radio_start_receive();
    MCH->log("meshcore package: start config=%d rx=%d node='%s'",
             (int)ok, (int)rx, the_mesh->_prefs.node_name);
    return ok && rx;
}

static void mc_loop(void) {
    if (the_mesh) the_mesh->loop();
}

static void mc_stop(void) {}

static void mc_flush(void) {
    if (the_mesh) the_mesh->flushForShutdown();
}

// Generic name-keyed send/peer surface — same logic the deleted built-in
// vtable had, the_mesh being module-local here.
static bool mc_send_channel_text(const char* channel, const char* text) {
    if (!the_mesh || !channel || !text) return false;
    int idx = -1;
    for (int i = 0; i < MAX_GROUP_CHANNELS; i++) {
        ChannelDetails cd;
        if (the_mesh->getChannel(i, cd) && strcmp(cd.name, channel) == 0) { idx = i; break; }
    }
    if (idx < 0) return false;
    uint32_t ts = the_mesh->getRTCClock()->getCurrentTime();
    return the_mesh->sendAndPersistChannelMsg(idx, ts, text, strlen(text));
}

static bool mc_send_text(const char* peer, const char* text) {
    if (!the_mesh || !peer || !text) return false;
    ContactInfo* recipient = the_mesh->searchContactsByPrefix(peer);
    if (!recipient) return false;
    uint32_t ts = the_mesh->getRTCClock()->getCurrentTime();
    auto r = the_mesh->sendAndPersistDM(*recipient, ts, 0, text);
    return r.code != MSG_SEND_FAILED;
}

static int mc_get_peers(LoraProtoPeer* out, int max) {
    if (!the_mesh || !out || max <= 0) return 0;
    int n = the_mesh->getNumContacts();
    int filled = 0;
    for (int i = 0; i < n && filled < max; i++) {
        ContactInfo c;
        if (!the_mesh->getContactByIdx(i, c)) continue;
        LoraProtoPeer& p = out[filled++];
        memset(&p, 0, sizeof(p));
        static const char* HEXC = "0123456789ABCDEF";
        for (int b = 0; b < 6; b++) {
            p.id[b * 2]     = HEXC[c.id.pub_key[b] >> 4];
            p.id[b * 2 + 1] = HEXC[c.id.pub_key[b] & 0x0F];
        }
        p.id[12] = '\0';
        strncpy(p.name, c.name, sizeof(p.name) - 1);
        p.last_heard = c.lastmod;   // our clock (last_advert_timestamp is theirs)
        p.snr = 0; p.rssi = 0;
    }
    return filled;
}

// Live radio reconfiguration for the settings bindings (mclua.cpp's
// radio_apply_* divergences call these). mesh_lock blocks the dispatcher
// (its loop runs under the mesh mutex) across the standby+config pair —
// the atomicity the firmware version got from holding SPI_LOCK. The
// dispatcher's next recvRaw re-arms RX with the new params.
void mc_apply_radio_params(float freq, float bw, uint8_t sf, uint8_t cr,
                           int8_t tx_dbm) {
    mesh_lock();
    MCH->radio_standby();
    MCH->radio_config(freq, bw, sf, cr, tx_dbm, true,
                      MC_SYNC_WORD, MC_PREAMBLE_LEN);
    s_radio.setSpreadingFactor(sf);
    mesh_unlock();
}

// The package Lua surface (mclua.cpp): every _mesh_* binding + the RX drain.
extern void mc_lua_register(lua_State* L);
extern void mc_drain_rx_events(lua_State* L);

static void mc_lua_open(void* L) {
    mc_lua_register((lua_State*)L);
    MCH->log("meshcore package: %s", "_mesh_ bindings registered");
}

static void mc_lua_tick(void* L) {
    mc_drain_rx_events((lua_State*)L);
}

static int mc_get_config(const char* key, char* out, int out_sz) {
    (void)key; (void)out; (void)out_sz;
    return 0;   // rich config flows through the lua_open bindings (5d)
}

// Radiated TX-power cap, forwarded by the host at boot ("max_tx_dbm" —
// per-board build flag, 20 tdeck / 22 heltec). The baked MAX_LORA_TX_POWER
// is only the fallback under a firmware that predates the key. Consumed by
// ble_companion.cpp (DEVICE_INFO cap byte + the set-power clamp).
int8_t mc_max_tx_dbm = MAX_LORA_TX_POWER;

static bool mc_set_config(const char* key, const char* val) {
    if (!key || !val) return false;
    // BLE companion sync backlog cap (0 = serve whole files). The host
    // forwards its persisted pref here at boot and on Settings change —
    // the config seam is the only way into a protocol module.
    if (strcmp(key, "ble_sync_max") == 0) {
        if (!the_mesh) return false;
        unsigned long v = strtoul(val, nullptr, 10);
        if (v > 5000) v = 5000;
        the_mesh->_ble_sync_max_per_channel = (uint16_t)v;
        return true;
    }
    if (strcmp(key, "max_tx_dbm") == 0) {
        long v = strtol(val, nullptr, 10);
        if (v < 1 || v > 30) return false;   // implausible cap: refuse loudly
        mc_max_tx_dbm = (int8_t)v;
        return true;
    }
    return false;
}

extern "C" const LoraProtoOps loraproto_ops = {
    LORA_PROTO_ABI_VERSION,
    "meshcore",
    "MeshCore",
    mc_init,
    mc_start,
    mc_loop,
    mc_stop,
    mc_send_channel_text,
    mc_send_text,
    mc_get_peers,
    mc_get_config,
    mc_set_config,
    nullptr, nullptr,            // prepare_sleep, note_wake (polled model)
    mc_flush,
    mc_lua_open,
    nullptr,                     // lua_close: no lua_State held (drain gets L
                                 // per tick; queued events wait for rebuild,
                                 // the firmware's exact ELF-run behavior)
    mc_lua_tick,
};
