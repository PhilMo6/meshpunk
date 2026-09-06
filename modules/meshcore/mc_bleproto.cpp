// The MeshCore phone companion as the package's BLE-slot component
// (bleproto_ops — its own elf beside the package's loraproto_ops elf).
// Replaces the firmware lifecycle trio (ble_companion_init_early/start/stop):
// the NimBLE stack stays firmware, driven through the BleHostApi serial
// transport; McBleSerial adapts it back to the BaseSerialInterface the
// companion handler consumes.

#include "punkmesh.h"

#if BLE_COMPANION_ENABLED

#include "ble_proto_abi.h"
#include "ble_companion.h"
#include "mc_internal.h"
#include "shim/meshpunk_sync.h"

extern PunkMesh* the_mesh;   // mcmain.cpp

static const BleHostApi* MCB = nullptr;

// User-initiated restart (phone app commands) — the host performs it.
void mc_device_reboot(void) {
    if (MCB && MCB->device_reboot) MCB->device_reboot();
}

// ── BaseSerialInterface over the host transport ─────────────────────────────
class McBleSerial : public BaseSerialInterface {
    bool _enabled = false;
public:
    void enable() override  { ble_transport_up(); }
    void disable() override { if (MCB) MCB->transport_disable(); _enabled = false; }
    bool isEnabled() const override { return _enabled; }
    bool isConnected() const override {
        return MCB && MCB->transport_connected();
    }
    bool isWriteBusy() const override {
        return MCB && MCB->transport_write_busy();
    }
    size_t writeFrame(const uint8_t src[], size_t len) override {
        return MCB ? (size_t)MCB->transport_write(src, (int)len) : 0;
    }
    size_t checkRecvFrame(uint8_t dest[]) override {
        return MCB ? (size_t)MCB->transport_read(dest) : 0;
    }

    void ble_transport_up() {
        if (!MCB) return;
        // Same identity the firmware companion advertised: prefix + @@MAC
        // (the transport expands the MAC), the build's pairing pin.
        MCB->transport_open("MeshCore-", "@@MAC", BLE_PIN_CODE);
        MCB->transport_enable();
        _enabled = true;
    }
};

static McBleSerial s_serial;

// ── BleProtoOps ─────────────────────────────────────────────────────────────
static bool bp_init(const BleHostApi* host) {
    if (!host || host->abi < 1) return false;
    MCB = host;
    return true;
}

static bool bp_start(void) {
    if (!the_mesh) return false;   // requires_lora belt — selector enforces
    s_serial.enable();
    ble_companion_attach(*the_mesh, s_serial);
    MCB->log("companion up (package)");
    return true;
}

static void bp_loop(void) {
    if (ble_companion) ble_companion->loop();
}

static void bp_stop(void) {
    ble_companion_detach();
    if (MCB) MCB->transport_close();
}

extern "C" const BleProtoOps bleproto_ops = {
    BLE_PROTO_ABI_VERSION,
    "meshcore_companion",
    "MeshCore app link",
    "meshcore",     // proxies this package's mesh
    bp_init,
    bp_start,
    bp_loop,
    bp_stop,
    nullptr,        // flush: the companion holds no unpersisted state
};

#endif  // BLE_COMPANION_ENABLED
