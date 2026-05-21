#pragma once

#if BLE_COMPANION_ENABLED

#include <helpers/esp32/SerialBLEInterface.h>
#include <esp_gap_ble_api.h>

extern bool ble_bond_clear_pref;

class PunkBLEInterface : public SerialBLEInterface {
  esp_bd_addr_t _peer_addr;
  bool _has_peer = false;

protected:
  void onAuthenticationComplete(esp_ble_auth_cmpl_t cmpl) override {
    if (cmpl.success) {
      memcpy(_peer_addr, cmpl.bd_addr, sizeof(esp_bd_addr_t));
      _has_peer = true;

      esp_ble_conn_update_params_t params = {};
      memcpy(params.bda, cmpl.bd_addr, sizeof(esp_bd_addr_t));
      params.min_int = 24;   // 30ms  (units of 1.25ms)
      params.max_int = 40;   // 50ms
      params.latency = 4;    // skip up to 4 events during idle
      params.timeout = 600;  // 6s    (units of 10ms)
      esp_err_t err = esp_ble_gap_update_conn_params(&params);
      Serial.printf("[BLE] Conn param update request: %s\n",
                    err == ESP_OK ? "sent" : "failed");
    }
    SerialBLEInterface::onAuthenticationComplete(cmpl);
  }

  void onDisconnect(BLEServer* pServer) override {
    if (_has_peer && ble_bond_clear_pref) {
      esp_ble_remove_bond_device(_peer_addr);
      _has_peer = false;
    }
    SerialBLEInterface::onDisconnect(pServer);
  }
};

#endif
