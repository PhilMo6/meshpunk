// BleHostApi serial transport — the firmware half of the BLE slot's module
// seam. Wraps its OWN PunkBLEInterface instance (NimBLE stays firmware).
// Polled + frame-based, one-to-one with MeshCore's
// BaseSerialInterface: the ported companion's McBleSerial calls straight
// through these.

#include "ble_proto.h"

#include <Arduino.h>

#include "../meshpunk_sync.h"   // SLog

#if BLE_COMPANION_ENABLED

#include "../punk_ble_interface.h"
#include <BLEDevice.h>

static PunkBLEInterface* s_bt = nullptr;

bool ble_transport_open(const char* name_prefix, const char* dev_name,
                        uint32_t pin) {
    if (s_bt) return true;
    // begin() takes a MUTABLE name: it rewrites "@@MAC" in place with the
    // MAC digits (the firmware companion kept a static buffer for the same
    // reason). Sized like its ble_dev_name[13].
    static char name_buf[13];
    snprintf(name_buf, sizeof(name_buf), "%s",
             dev_name ? dev_name : "@@MAC");
    s_bt = new PunkBLEInterface();
    s_bt->begin(name_prefix ? name_prefix : "", name_buf, pin);
    s_bt->enable();
    SLog.printf("[BLEPROTO] transport up: %s%s pin=%u\n",
                name_prefix ? name_prefix : "", name_buf, (unsigned)pin);
    return true;
}

void ble_transport_close(void) {
    if (!s_bt) return;
    s_bt->disable();
    delete s_bt;
    s_bt = nullptr;
    BLEDevice::deinit(false);
    SLog.println("[BLEPROTO] transport closed, BLE resources freed");
}

void ble_transport_enable(void)  { if (s_bt) s_bt->enable(); }
void ble_transport_disable(void) { if (s_bt) s_bt->disable(); }

bool ble_transport_connected(void)  { return s_bt && s_bt->isConnected(); }
bool ble_transport_write_busy(void) { return s_bt && s_bt->isWriteBusy(); }

int ble_transport_write(const uint8_t* frame, int len) {
    if (!s_bt || !frame || len <= 0) return 0;
    return (int)s_bt->writeFrame(frame, (size_t)len);
}

int ble_transport_read(uint8_t* buf) {
    if (!s_bt || !buf) return 0;
    return (int)s_bt->checkRecvFrame(buf);
}

#else  // !BLE_COMPANION_ENABLED

bool ble_transport_open(const char*, const char*, uint32_t) { return false; }
void ble_transport_close(void) {}
void ble_transport_enable(void) {}
void ble_transport_disable(void) {}
bool ble_transport_connected(void) { return false; }
bool ble_transport_write_busy(void) { return false; }
int  ble_transport_write(const uint8_t*, int) { return 0; }
int  ble_transport_read(uint8_t*) { return 0; }

#endif
