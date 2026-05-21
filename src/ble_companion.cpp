#include "ble_companion.h"

#if BLE_COMPANION_ENABLED

#include "punkmesh.h"
#include "meshpunk_sync.h"
#include "punk_ble_interface.h"
#include <BLEDevice.h>
#include <LittleFS.h>
#include <esp_heap_caps.h>

// sd_spi_take() is inline in meshpunk_sync.h (just SPI_LOCK); no extern needed.
// sd_spi_release() is defined in main.cpp — need the extern decl here.
extern void sd_spi_release();

BleCompanionHandler* ble_companion = nullptr;
SerialBLEInterface*  ble_serial    = nullptr;

// ── Lifecycle ────────────────────────────────────────────────────

static char ble_dev_name[13] = "@@MAC";

void ble_companion_init_early() {
  if (ble_serial) return;

  Serial.printf("[BLE] Early init (free heap: %u)...\n", ESP.getFreeHeap());

  ble_serial = new PunkBLEInterface();
  ble_serial->begin(BLE_NAME_PREFIX, ble_dev_name, BLE_PIN_CODE);
  ble_serial->enable();

  Serial.printf("[BLE] BLE stack up, advertising. PIN=%d, name=%s%s (free heap: %u)\n",
                BLE_PIN_CODE, BLE_NAME_PREFIX, ble_dev_name, ESP.getFreeHeap());
}

void ble_companion_start(PunkMesh& mesh) {
  if (!ble_serial || ble_companion) return;

  void* mem = heap_caps_malloc(sizeof(BleCompanionHandler), MALLOC_CAP_SPIRAM);
  if (mem) {
    ble_companion = new (mem) BleCompanionHandler(mesh, *ble_serial);
  } else {
    Serial.println("[BLE] FATAL: cannot allocate companion handler");
    return;
  }
  Serial.printf("[BLE] Companion handler ready (free heap: %u)\n", ESP.getFreeHeap());
}

void ble_companion_stop() {
  if (!ble_serial) return;

  Serial.println("[BLE] Stopping companion interface...");

  ble_serial->disable();
  if (ble_companion) {
    ble_companion->~BleCompanionHandler();
    heap_caps_free(ble_companion);
  }
  ble_companion = nullptr;
  delete ble_serial;
  ble_serial = nullptr;

  BLEDevice::deinit(false);
  Serial.println("[BLE] Companion stopped, BLE resources freed.");
}

// ── BleCompanionHandler ──────────────────────────────────────────

BleCompanionHandler::BleCompanionHandler(PunkMesh& mesh, SerialBLEInterface& serial)
  : _mesh(mesh), _serial(serial)
{
  _iter_started = false;
  _iter_filter_since = 0;
  _most_recent_lastmod = 0;
  app_target_ver = 0;
  next_ack_idx = 0;
  memset(expected_ack_table, 0, sizeof(expected_ack_table));
  _msg_sync.active = false;
  _msg_sync.file_count = 0;
  _msg_sync.current_file = 0;
  _msg_sync.since = 0;
  _msg_sync.most_recent_ts = 0;
  _msg_sync.frames = nullptr;
  _msg_sync.frame_count = 0;
  _msg_sync.frame_idx = 0;
}

BleCompanionHandler::~BleCompanionHandler() {
  freeSyncFrames();
}

void BleCompanionHandler::loop() {
  size_t len = _serial.checkRecvFrame(cmd_frame);

  if (len > 0) {
    MESH_LOCK();
    handleCmdFrame(len);
    MESH_UNLOCK();
  } else if (_iter_started && !_serial.isWriteBusy()) {
    ContactInfo contact;
    bool has_next;

    MESH_LOCK();
    has_next = _iter.hasNext(&_mesh, contact);
    MESH_UNLOCK();

    if (has_next) {
      if (contact.lastmod >= _iter_filter_since) {
        writeContactRespFrame(RESP_CODE_CONTACT, contact);
        if (contact.lastmod > _most_recent_lastmod) {
          _most_recent_lastmod = contact.lastmod;
        }
      }
    } else {
      out_frame[0] = RESP_CODE_END_OF_CONTACTS;
      memcpy(&out_frame[1], &_most_recent_lastmod, 4);
      _serial.writeFrame(out_frame, 5);
      _iter_started = false;
    }
  }
}

// ── Sync timestamp persistence ───────────────────────────────────

static String sync_ts_path(PunkMesh& mesh) {
  return mesh._storage_prefix + "/messages/ble_sync_ts";
}

static uint32_t load_sync_timestamp(PunkMesh& mesh) {
  if (!mesh._storage) return 0;
  bool is_sd = (mesh._storage != &LittleFS);
  if (is_sd) sd_spi_take();
  File f = mesh._storage->open(sync_ts_path(mesh).c_str(), "r");
  if (!f) {
    if (is_sd) sd_spi_release();
    return 0;
  }
  uint32_t ts = 0;
  f.read((uint8_t*)&ts, 4);
  f.close();
  if (is_sd) sd_spi_release();
  return ts;
}

static void save_sync_timestamp(PunkMesh& mesh, uint32_t ts) {
  if (!mesh._storage || ts == 0) return;
  bool is_sd = (mesh._storage != &LittleFS);
  if (is_sd) sd_spi_take();
  File f = mesh._storage->open(sync_ts_path(mesh).c_str(), "w");
  if (!f) {
    if (is_sd) sd_spi_release();
    return;
  }
  f.write((uint8_t*)&ts, 4);
  f.close();
  if (is_sd) sd_spi_release();
}

// ── Command dispatch ─────────────────────────────────────────────

void BleCompanionHandler::handleCmdFrame(size_t len) {
  if (cmd_frame[0] == CMD_DEVICE_QEURY && len >= 2) {
    app_target_ver = cmd_frame[1];

    int i = 0;
    out_frame[i++] = RESP_CODE_DEVICE_INFO;
    out_frame[i++] = MESHPUNK_FW_VER_CODE;
    out_frame[i++] = MAX_CONTACTS / 2;
    out_frame[i++] = MAX_GROUP_CHANNELS;

    uint32_t pin = BLE_PIN_CODE;
    memcpy(&out_frame[i], &pin, 4);
    i += 4;

    memset(&out_frame[i], 0, 12);
    strncpy((char*)&out_frame[i], MESHPUNK_FW_BUILD_DATE, 12);
    i += 12;

    StrHelper::strzcpy((char*)&out_frame[i], MESHPUNK_MODEL_NAME, 40);
    i += 40;

    StrHelper::strzcpy((char*)&out_frame[i], MESHPUNK_FW_VERSION, 20);
    i += 20;

    out_frame[i++] = 0; // client_repeat (not used)
    out_frame[i++] = 0; // path_hash_mode (default)

    _serial.writeFrame(out_frame, i);

  } else if (cmd_frame[0] == CMD_APP_START && len >= 8) {
    char* app_name = (char*)&cmd_frame[8];
    cmd_frame[len] = 0;
    Serial.printf("[BLE] App '%s' connected\n", app_name);

    _iter_started = false;

    freeSyncFrames();
    _msg_sync.file_count = _mesh.enumerateMessageFiles(
        _msg_sync.files, MsgSyncState::MAX_FILES);
    _msg_sync.current_file = 0;
    _msg_sync.since = load_sync_timestamp(_mesh);
    _msg_sync.most_recent_ts = 0;
    _msg_sync.active = false;
    if (_msg_sync.file_count > 0)
      loadNextFileFrames();
    Serial.printf("[BLE] Message sync: %d files, %d frames ready\n",
                  _msg_sync.file_count, _msg_sync.frame_count);

    int i = 0;
    out_frame[i++] = RESP_CODE_SELF_INFO;
    out_frame[i++] = ADV_TYPE_CHAT;
    out_frame[i++] = _mesh._prefs.tx_power_dbm;
    out_frame[i++] = MAX_LORA_TX_POWER;

    memcpy(&out_frame[i], _mesh.self_id.pub_key, PUB_KEY_SIZE);
    i += PUB_KEY_SIZE;

    int32_t lat = (int32_t)(_mesh._prefs.node_lat * 1000000.0);
    int32_t lon = (int32_t)(_mesh._prefs.node_lon * 1000000.0);
    memcpy(&out_frame[i], &lat, 4); i += 4;
    memcpy(&out_frame[i], &lon, 4); i += 4;

    out_frame[i++] = 0; // multi_acks (not used in meshpunk)
    out_frame[i++] = 0; // advert_loc_policy
    out_frame[i++] = 0; // telemetry modes
    out_frame[i++] = 0; // manual_add_contacts

    uint32_t freq = (uint32_t)(_mesh._prefs.freq * 1000);
    memcpy(&out_frame[i], &freq, 4); i += 4;

    uint32_t bw = (uint32_t)(_mesh._prefs.bandwidth * 1000);
    memcpy(&out_frame[i], &bw, 4); i += 4;

    out_frame[i++] = _mesh._prefs.spreading_factor;
    out_frame[i++] = _mesh._prefs.coding_rate;

    int tlen = strlen(_mesh._prefs.node_name);
    memcpy(&out_frame[i], _mesh._prefs.node_name, tlen);
    i += tlen;

    _serial.writeFrame(out_frame, i);

  } else if (cmd_frame[0] == CMD_GET_CONTACTS && len >= 1) {
    _iter = _mesh.startContactsIterator();
    _iter_filter_since = 0;
    _most_recent_lastmod = 0;
    if (len >= 5) {
      memcpy(&_iter_filter_since, &cmd_frame[1], 4);
    }

    int num = _mesh.getNumContacts();
    out_frame[0] = RESP_CODE_CONTACTS_START;
    memcpy(&out_frame[1], &num, 4);
    _serial.writeFrame(out_frame, 5);
    _iter_started = true;

  } else if (cmd_frame[0] == CMD_GET_CHANNEL && len >= 2) {
    uint8_t ch_idx = cmd_frame[1];
    ChannelDetails ch;
    if (_mesh.getChannel(ch_idx, ch)) {
      int i = 0;
      out_frame[i++] = RESP_CODE_CHANNEL_INFO;
      out_frame[i++] = ch_idx;
      strcpy((char*)&out_frame[i], ch.name);
      i += 32;
      memcpy(&out_frame[i], ch.channel.secret, 16);
      i += 16;
      _serial.writeFrame(out_frame, i);
    } else {
      writeErrFrame(ERR_CODE_NOT_FOUND);
    }

  } else if (cmd_frame[0] == CMD_GET_DEVICE_TIME) {
    uint32_t now = _mesh.getRTCClock()->getCurrentTime();
    out_frame[0] = RESP_CODE_CURR_TIME;
    memcpy(&out_frame[1], &now, 4);
    _serial.writeFrame(out_frame, 5);

  } else if (cmd_frame[0] == CMD_SET_DEVICE_TIME && len >= 5) {
    uint32_t t;
    memcpy(&t, &cmd_frame[1], 4);
    _mesh.getRTCClock()->setCurrentTime(t);
    writeOKFrame();

  } else if (cmd_frame[0] == CMD_GET_BATT_AND_STORAGE && len >= 1) {
    uint16_t batt_mv = analogReadMilliVolts(PIN_VBAT_READ) * 2;
    out_frame[0] = RESP_CODE_BATT_AND_STORAGE;
    memcpy(&out_frame[1], &batt_mv, 2);
    // Storage: total and free in KB (use LittleFS for now)
    uint32_t total_kb = 0, free_kb = 0;
    out_frame[3] = 0; // charging state unknown
    memcpy(&out_frame[4], &total_kb, 4);
    memcpy(&out_frame[8], &free_kb, 4);
    _serial.writeFrame(out_frame, 12);

  } else if (cmd_frame[0] == CMD_SYNC_NEXT_MESSAGE) {
    if (!_msg_sync.active) {
      freeSyncFrames();
      _msg_sync.file_count = _mesh.enumerateMessageFiles(
          _msg_sync.files, MsgSyncState::MAX_FILES);
      _msg_sync.current_file = 0;
      _msg_sync.since = load_sync_timestamp(_mesh);
      _msg_sync.most_recent_ts = 0;
      if (_msg_sync.file_count > 0)
        loadNextFileFrames();
    }

    if (_msg_sync.active && _msg_sync.frame_idx >= _msg_sync.frame_count)
      loadNextFileFrames();

    if (_msg_sync.active && _msg_sync.frame_idx < _msg_sync.frame_count) {
      SyncFrame& sf = _msg_sync.frames[_msg_sync.frame_idx++];
      _serial.writeFrame(sf.data, sf.len);
    } else {
      if (_msg_sync.most_recent_ts > 0)
        save_sync_timestamp(_mesh, _msg_sync.most_recent_ts);
      freeSyncFrames();
      _msg_sync.active = false;
      out_frame[0] = RESP_CODE_NO_MORE_MESSAGES;
      _serial.writeFrame(out_frame, 1);
    }

  } else if (cmd_frame[0] == CMD_SEND_TXT_MSG && len >= 14) {
    int i = 1;
    uint8_t txt_type = cmd_frame[i++];
    uint8_t attempt  = cmd_frame[i++];
    uint32_t msg_timestamp;
    memcpy(&msg_timestamp, &cmd_frame[i], 4); i += 4;
    uint8_t* pub_key_prefix = &cmd_frame[i]; i += 6;

    ContactInfo* recipient = _mesh.lookupContactByPubKey(pub_key_prefix, 6);
    if (recipient && txt_type == TXT_TYPE_PLAIN) {
      int tlen = len - i;
      cmd_frame[i + tlen] = 0;
      char text[160];
      normalize_smart_quotes((const char*)&cmd_frame[i], text, sizeof(text));

      auto r = _mesh.sendAndPersistDM(*recipient, msg_timestamp, attempt, text);
      if (r.code == MSG_SEND_FAILED) {
        writeErrFrame(ERR_CODE_TABLE_FULL);
      } else {
        if (r.expected_ack) {
          expected_ack_table[next_ack_idx].msg_sent = millis();
          expected_ack_table[next_ack_idx].ack = r.expected_ack;
          expected_ack_table[next_ack_idx].contact = recipient;
          next_ack_idx = (next_ack_idx + 1) % EXPECTED_ACK_TABLE_SIZE;
        }
        out_frame[0] = RESP_CODE_SENT;
        out_frame[1] = (r.code == MSG_SEND_SENT_FLOOD) ? 1 : 0;
        memcpy(&out_frame[2], &r.expected_ack, 4);
        memcpy(&out_frame[6], &r.est_timeout, 4);
        _serial.writeFrame(out_frame, 10);
      }
    } else {
      writeErrFrame(recipient == nullptr ? ERR_CODE_NOT_FOUND : ERR_CODE_UNSUPPORTED_CMD);
    }

  } else if (cmd_frame[0] == CMD_SEND_CHANNEL_TXT_MSG && len >= 8) {
    int i = 1;
    uint8_t txt_type    = cmd_frame[i++];
    uint8_t channel_idx = cmd_frame[i++];
    uint32_t msg_timestamp;
    memcpy(&msg_timestamp, &cmd_frame[i], 4); i += 4;
    int tlen = len - i;

    if (txt_type != TXT_TYPE_PLAIN) {
      writeErrFrame(ERR_CODE_UNSUPPORTED_CMD);
    } else {
      cmd_frame[i + tlen] = 0;
      char text[160];
      normalize_smart_quotes((const char*)&cmd_frame[i], text, sizeof(text));

      bool ok = _mesh.sendAndPersistChannelMsg(channel_idx, msg_timestamp, text, strlen(text));
      if (ok) {
        if (rx_event_queue) {
          RxEvent ev = {};
          ev.kind        = RxEvent::CHANNEL_MSG;
          ev.hops        = 0;
          ev.channel_idx = (int8_t)channel_idx;
          ev.direct      = false;
          strncpy(ev.sender, _mesh._prefs.node_name, sizeof(ev.sender) - 1);
          strncpy(ev.text, text, sizeof(ev.text) - 1);
          ev.timestamp   = msg_timestamp;
          xQueueSend(rx_event_queue, &ev, 0);
        }
        writeOKFrame();
      } else {
        writeErrFrame(ERR_CODE_TABLE_FULL);
      }
    }

  } else if (cmd_frame[0] == CMD_GET_CONTACT_BY_KEY) {
    uint8_t *pub_key = &cmd_frame[1];
    ContactInfo *contact = _mesh.lookupContactByPubKey(pub_key, PUB_KEY_SIZE);
    if (contact) {
      writeContactRespFrame(RESP_CODE_CONTACT, *contact);
    } else {
      writeErrFrame(ERR_CODE_NOT_FOUND);
    }

  } else if (cmd_frame[0] == CMD_HAS_CONNECTION && len >= 1 + PUB_KEY_SIZE) {
    uint8_t *pub_key = &cmd_frame[1];
    if (_mesh.hasConnectionToContact(pub_key)) {
      writeOKFrame();
    } else {
      writeErrFrame(ERR_CODE_NOT_FOUND);
    }

  // ── Channel management ─���──────────────────────────────────────
  } else if (cmd_frame[0] == CMD_SET_CHANNEL && len >= 2 + 32 + 32) {
    writeErrFrame(ERR_CODE_UNSUPPORTED_CMD); // 256-bit keys not supported
  } else if (cmd_frame[0] == CMD_SET_CHANNEL && len >= 2 + 32 + 16) {
    uint8_t channel_idx = cmd_frame[1];
    if (channel_idx == 0) {
      writeErrFrame(ERR_CODE_ILLEGAL_ARG);
    } else {
      ChannelDetails channel;
      StrHelper::strncpy(channel.name, (char*)&cmd_frame[2], 32);
      memset(channel.channel.secret, 0, sizeof(channel.channel.secret));
      memcpy(channel.channel.secret, &cmd_frame[2 + 32], 16);
      if (_mesh.setChannel(channel_idx, channel)) {
        _mesh.saveChannels();
        writeOKFrame();
      } else {
        writeErrFrame(ERR_CODE_NOT_FOUND);
      }
    }

  // ── Advertisement & identity ────────────────────────────────────
  } else if (cmd_frame[0] == CMD_SEND_SELF_ADVERT) {
    auto pkt = _mesh.createSelfAdvert(_mesh._prefs.node_name,
                                       _mesh._prefs.node_lat, _mesh._prefs.node_lon);
    if (pkt) {
      if (len >= 2 && cmd_frame[1] == 1) {
        _mesh.sendFlood(pkt, (uint32_t)0);
      } else {
        _mesh.sendZeroHop(pkt);
      }
      writeOKFrame();
    } else {
      writeErrFrame(ERR_CODE_TABLE_FULL);
    }

  } else if (cmd_frame[0] == CMD_SET_ADVERT_NAME && len >= 2) {
    int nlen = len - 1;
    if (nlen > (int)sizeof(_mesh._prefs.node_name) - 1)
      nlen = sizeof(_mesh._prefs.node_name) - 1;
    memcpy(_mesh._prefs.node_name, &cmd_frame[1], nlen);
    _mesh._prefs.node_name[nlen] = 0;
    _mesh.savePrefs();
    writeOKFrame();

  } else if (cmd_frame[0] == CMD_SET_ADVERT_LATLON && len >= 9) {
    int32_t lat, lon;
    memcpy(&lat, &cmd_frame[1], 4);
    memcpy(&lon, &cmd_frame[5], 4);
    if (lat <= 90000000 && lat >= -90000000 && lon <= 180000000 && lon >= -180000000) {
      _mesh._prefs.node_lat = ((double)lat) / 1000000.0;
      _mesh._prefs.node_lon = ((double)lon) / 1000000.0;
      _mesh.savePrefs();
      writeOKFrame();
    } else {
      writeErrFrame(ERR_CODE_ILLEGAL_ARG);
    }

  // ── Contact management ──────────────────────────────────────────
  } else if (cmd_frame[0] == CMD_ADD_UPDATE_CONTACT && len >= 1 + PUB_KEY_SIZE + 2) {
    uint8_t* pub_key = &cmd_frame[1];
    ContactInfo* existing = _mesh.lookupContactByPubKey(pub_key, PUB_KEY_SIZE);
    if (existing) {
      int i = 1 + PUB_KEY_SIZE;
      if ((int)len > i + 32) {
        StrHelper::strncpy(existing->name, (char*)&cmd_frame[i], 32);
        i += 32;
      }
      if ((int)len > i) existing->type = cmd_frame[i++];
      if ((int)len > i) existing->flags = cmd_frame[i++];
      existing->lastmod = _mesh.getRTCClock()->getCurrentTime();
      _mesh.saveContacts();
      writeOKFrame();
    } else {
      ContactInfo contact;
      memset(&contact, 0, sizeof(contact));
      memcpy(contact.id.pub_key, pub_key, PUB_KEY_SIZE);
      int i = 1 + PUB_KEY_SIZE;
      if ((int)len > i + 32) {
        StrHelper::strncpy(contact.name, (char*)&cmd_frame[i], 32);
        i += 32;
      }
      if ((int)len > i) contact.type = cmd_frame[i++];
      if ((int)len > i) contact.flags = cmd_frame[i++];
      contact.out_path_len = OUT_PATH_UNKNOWN;
      contact.lastmod = _mesh.getRTCClock()->getCurrentTime();
      if (_mesh.addContact(contact)) {
        _mesh.saveContacts();
        writeOKFrame();
      } else {
        writeErrFrame(ERR_CODE_TABLE_FULL);
      }
    }

  } else if (cmd_frame[0] == CMD_REMOVE_CONTACT && len >= 1 + PUB_KEY_SIZE) {
    uint8_t* pub_key = &cmd_frame[1];
    ContactInfo* contact = _mesh.lookupContactByPubKey(pub_key, PUB_KEY_SIZE);
    if (contact && _mesh.removeContact(*contact)) {
      _mesh.saveContacts();
      writeOKFrame();
    } else {
      writeErrFrame(ERR_CODE_NOT_FOUND);
    }

  } else if (cmd_frame[0] == CMD_RESET_PATH && len >= 1 + PUB_KEY_SIZE) {
    uint8_t* pub_key = &cmd_frame[1];
    ContactInfo* contact = _mesh.lookupContactByPubKey(pub_key, PUB_KEY_SIZE);
    if (contact) {
      contact->out_path_len = OUT_PATH_UNKNOWN;
      _mesh.saveContacts();
      writeOKFrame();
    } else {
      writeErrFrame(ERR_CODE_NOT_FOUND);
    }

  } else if (cmd_frame[0] == CMD_SHARE_CONTACT && len >= 1 + PUB_KEY_SIZE) {
    uint8_t* pub_key = &cmd_frame[1];
    ContactInfo* contact = _mesh.lookupContactByPubKey(pub_key, PUB_KEY_SIZE);
    if (contact) {
      if (_mesh.shareContactZeroHop(*contact)) {
        writeOKFrame();
      } else {
        writeErrFrame(ERR_CODE_TABLE_FULL);
      }
    } else {
      writeErrFrame(ERR_CODE_NOT_FOUND);
    }

  } else if (cmd_frame[0] == CMD_EXPORT_CONTACT) {
    if (len < 1 + PUB_KEY_SIZE) {
      auto pkt = _mesh.createSelfAdvert(_mesh._prefs.node_name,
                                         _mesh._prefs.node_lat, _mesh._prefs.node_lon);
      if (pkt) {
        pkt->header |= ROUTE_TYPE_FLOOD;
        out_frame[0] = RESP_CODE_EXPORT_CONTACT;
        uint8_t out_len = pkt->writeTo(&out_frame[1]);
        _mesh.releasePacket(pkt);
        _serial.writeFrame(out_frame, out_len + 1);
      } else {
        writeErrFrame(ERR_CODE_TABLE_FULL);
      }
    } else {
      uint8_t* pub_key = &cmd_frame[1];
      ContactInfo* contact = _mesh.lookupContactByPubKey(pub_key, PUB_KEY_SIZE);
      uint8_t out_len;
      if (contact && (out_len = _mesh.exportContact(*contact, &out_frame[1])) > 0) {
        out_frame[0] = RESP_CODE_EXPORT_CONTACT;
        _serial.writeFrame(out_frame, out_len + 1);
      } else {
        writeErrFrame(ERR_CODE_NOT_FOUND);
      }
    }

  } else if (cmd_frame[0] == CMD_IMPORT_CONTACT && len > 2 + 32 + 64) {
    if (_mesh.importContact(&cmd_frame[1], len - 1)) {
      _mesh.saveContacts();
      writeOKFrame();
    } else {
      writeErrFrame(ERR_CODE_ILLEGAL_ARG);
    }

  // ── Radio configuration ─────────────────────────────────────────
  } else if (cmd_frame[0] == CMD_SET_RADIO_PARAMS && len >= 11) {
    int i = 1;
    uint32_t freq, bw;
    memcpy(&freq, &cmd_frame[i], 4); i += 4;
    memcpy(&bw, &cmd_frame[i], 4); i += 4;
    uint8_t sf = cmd_frame[i++];
    uint8_t cr = cmd_frame[i++];
    if (freq >= 150000 && freq <= 2500000 && sf >= 5 && sf <= 12
        && cr >= 5 && cr <= 8 && bw >= 7000 && bw <= 500000) {
      _mesh._prefs.freq = (float)freq / 1000.0f;
      _mesh._prefs.bandwidth = (float)bw / 1000.0f;
      _mesh._prefs.spreading_factor = sf;
      _mesh._prefs.coding_rate = cr;
      _mesh.savePrefs();
      // TODO: radio_reconfigure() once radio access helper is added
      writeOKFrame();
    } else {
      writeErrFrame(ERR_CODE_ILLEGAL_ARG);
    }

  } else if (cmd_frame[0] == CMD_SET_RADIO_TX_POWER && len >= 2) {
    int8_t power = (int8_t)cmd_frame[1];
    if (power < -9 || power > MAX_LORA_TX_POWER) {
      writeErrFrame(ERR_CODE_ILLEGAL_ARG);
    } else {
      _mesh._prefs.tx_power_dbm = power;
      _mesh.savePrefs();
      // TODO: radio_set_tx_power() once radio access helper is added
      writeOKFrame();
    }

  // ── Tuning & misc config ────────────────────────────────────────
  } else if (cmd_frame[0] == CMD_SET_TUNING_PARAMS && len >= 9) {
    uint32_t rx, af;
    memcpy(&rx, &cmd_frame[1], 4);
    memcpy(&af, &cmd_frame[5], 4);
    _mesh._prefs.airtime_factor = ((float)af) / 1000.0f;
    _mesh.savePrefs();
    writeOKFrame();

  } else if (cmd_frame[0] == CMD_GET_TUNING_PARAMS) {
    uint32_t rx = 0;
    uint32_t af = (uint32_t)(_mesh._prefs.airtime_factor * 1000);
    int i = 0;
    out_frame[i++] = RESP_CODE_TUNING_PARAMS;
    memcpy(&out_frame[i], &rx, 4); i += 4;
    memcpy(&out_frame[i], &af, 4); i += 4;
    _serial.writeFrame(out_frame, i);

  } else if (cmd_frame[0] == CMD_SET_OTHER_PARAMS && len >= 2) {
    _mesh.savePrefs();
    writeOKFrame();

  // ── Device operations ───────────────────────────────────────────
  } else if (cmd_frame[0] == CMD_REBOOT && len >= 7
             && memcmp(&cmd_frame[1], "reboot", 6) == 0) {
    _mesh.saveContacts();
    writeOKFrame();
    delay(500);
    ESP.restart();

  } else if (cmd_frame[0] == CMD_FACTORY_RESET && len >= 6
             && memcmp(&cmd_frame[1], "reset", 5) == 0) {
    _serial.disable();
    LittleFS.format();
    delay(500);
    ESP.restart();

  } else if (cmd_frame[0] == CMD_SEND_TRACE_PATH && len > 10) {
    uint8_t path_len = len - 10;
    uint8_t flags = cmd_frame[9];
    uint32_t tag, auth;
    memcpy(&tag, &cmd_frame[1], 4);
    memcpy(&auth, &cmd_frame[5], 4);
    auto pkt = _mesh.createTrace(tag, auth, flags);
    if (pkt) {
      _mesh.sendDirect(pkt, &cmd_frame[10], path_len);
      out_frame[0] = RESP_CODE_SENT;
      out_frame[1] = 0;
      memcpy(&out_frame[2], &tag, 4);
      uint32_t est_timeout = 10000;
      memcpy(&out_frame[6], &est_timeout, 4);
      _serial.writeFrame(out_frame, 10);
    } else {
      writeErrFrame(ERR_CODE_TABLE_FULL);
    }

  } else if (cmd_frame[0] == CMD_GET_ADVERT_PATH && len >= PUB_KEY_SIZE + 2) {
    uint8_t* pub_key = &cmd_frame[2];
    ContactInfo* contact = _mesh.lookupContactByPubKey(pub_key, PUB_KEY_SIZE);
    if (contact && contact->out_path_len != OUT_PATH_UNKNOWN) {
      int i = 0;
      out_frame[i++] = RESP_CODE_ADVERT_PATH;
      memcpy(&out_frame[i], &contact->last_advert_timestamp, 4); i += 4;
      out_frame[i++] = contact->out_path_len;
      memcpy(&out_frame[i], contact->out_path, contact->out_path_len);
      i += contact->out_path_len;
      _serial.writeFrame(out_frame, i);
    } else {
      writeErrFrame(ERR_CODE_NOT_FOUND);
    }

  } else if (cmd_frame[0] == CMD_LOGOUT && len >= 1 + PUB_KEY_SIZE) {
    uint8_t* pub_key = &cmd_frame[1];
    _mesh.stopConnectionToContact(pub_key);
    writeOKFrame();

  } else if (cmd_frame[0] == CMD_EXPORT_PRIVATE_KEY) {
    out_frame[0] = RESP_CODE_PRIVATE_KEY;
    memcpy(&out_frame[1], _mesh.getPrivateKey(), 64);
    _serial.writeFrame(out_frame, 65);

  } else if (cmd_frame[0] == CMD_IMPORT_PRIVATE_KEY && len >= 65) {
    if (!mesh::LocalIdentity::validatePrivateKey(&cmd_frame[1])) {
      writeErrFrame(ERR_CODE_ILLEGAL_ARG);
    } else {
      _mesh.self_id.readFrom(&cmd_frame[1], PRV_KEY_SIZE);
      if (_mesh.saveIdentity()) {
        writeOKFrame();
        delay(500);
        ESP.restart();
      } else {
        writeErrFrame(ERR_CODE_FILE_IO_ERROR);
      }
    }

  } else if (cmd_frame[0] == CMD_GENERATE_IDENTITY && len >= 9
             && memcmp(&cmd_frame[1], "generate", 8) == 0) {
    ((StdRNG*)_mesh.getRNG())->begin(esp_random());
    _mesh.self_id = mesh::LocalIdentity(_mesh.getRNG());
    int count = 0;
    while (count < 10 && (_mesh.self_id.pub_key[0] == 0x00 || _mesh.self_id.pub_key[0] == 0xFF)) {
      _mesh.self_id = mesh::LocalIdentity(_mesh.getRNG());
      count++;
    }
    if (_mesh.saveIdentity()) {
      writeOKFrame();
      delay(500);
      ESP.restart();
    } else {
      writeErrFrame(ERR_CODE_FILE_IO_ERROR);
    }

  } else if (cmd_frame[0] == CMD_SEND_LOGIN && len >= 1 + PUB_KEY_SIZE) {
    uint8_t* pub_key = &cmd_frame[1];
    ContactInfo* recipient = _mesh.lookupContactByPubKey(pub_key, PUB_KEY_SIZE);
    char* password = (char*)&cmd_frame[1 + PUB_KEY_SIZE];
    cmd_frame[len] = 0;
    if (recipient) {
      uint32_t est_timeout;
      int result = _mesh.sendLogin(*recipient, password, est_timeout);
      if (result == MSG_SEND_FAILED) {
        writeErrFrame(ERR_CODE_TABLE_FULL);
      } else {
        out_frame[0] = RESP_CODE_SENT;
        out_frame[1] = (result == MSG_SEND_SENT_FLOOD) ? 1 : 0;
        uint32_t tag = 0;
        memcpy(&out_frame[2], &tag, 4);
        memcpy(&out_frame[6], &est_timeout, 4);
        _serial.writeFrame(out_frame, 10);
      }
    } else {
      writeErrFrame(ERR_CODE_NOT_FOUND);
    }

  } else if (cmd_frame[0] == CMD_SEND_STATUS_REQ && len >= 1 + PUB_KEY_SIZE) {
    uint8_t* pub_key = &cmd_frame[1];
    ContactInfo* recipient = _mesh.lookupContactByPubKey(pub_key, PUB_KEY_SIZE);
    if (recipient) {
      uint32_t tag, est_timeout;
      int result = _mesh.sendRequest(*recipient, REQ_TYPE_GET_STATUS, tag, est_timeout);
      if (result == MSG_SEND_FAILED) {
        writeErrFrame(ERR_CODE_TABLE_FULL);
      } else {
        out_frame[0] = RESP_CODE_SENT;
        out_frame[1] = (result == MSG_SEND_SENT_FLOOD) ? 1 : 0;
        memcpy(&out_frame[2], &tag, 4);
        memcpy(&out_frame[6], &est_timeout, 4);
        _serial.writeFrame(out_frame, 10);
      }
    } else {
      writeErrFrame(ERR_CODE_NOT_FOUND);
    }

  } else if (cmd_frame[0] == CMD_SEND_RAW_DATA && len >= 6) {
    int8_t path_len = cmd_frame[1];
    if (path_len >= 0 && 2 + path_len + 4 <= (int)len) {
      auto pkt = _mesh.createRawData(&cmd_frame[2 + path_len], len - 2 - path_len);
      if (pkt) {
        _mesh.sendDirect(pkt, &cmd_frame[2], path_len);
        writeOKFrame();
      } else {
        writeErrFrame(ERR_CODE_TABLE_FULL);
      }
    } else {
      writeErrFrame(ERR_CODE_UNSUPPORTED_CMD);
    }

  } else {
    writeErrFrame(ERR_CODE_UNSUPPORTED_CMD);
  }
}

// ── Helper frames ────────────────────────────────────────────────

void BleCompanionHandler::writeOKFrame() {
  out_frame[0] = RESP_CODE_OK;
  _serial.writeFrame(out_frame, 1);
}

void BleCompanionHandler::writeErrFrame(uint8_t err_code) {
  out_frame[0] = RESP_CODE_ERR;
  out_frame[1] = err_code;
  _serial.writeFrame(out_frame, 2);
}

void BleCompanionHandler::writeContactRespFrame(uint8_t code, const ContactInfo& contact) {
  int i = 0;
  out_frame[i++] = code;
  memcpy(&out_frame[i], contact.id.pub_key, PUB_KEY_SIZE); i += PUB_KEY_SIZE;
  out_frame[i++] = contact.type;
  out_frame[i++] = contact.flags;
  out_frame[i++] = contact.out_path_len;
  memcpy(&out_frame[i], contact.out_path, MAX_PATH_SIZE); i += MAX_PATH_SIZE;
  StrHelper::strzcpy((char*)&out_frame[i], contact.name, 32); i += 32;
  memcpy(&out_frame[i], &contact.last_advert_timestamp, 4); i += 4;
  memcpy(&out_frame[i], &contact.gps_lat, 4); i += 4;
  memcpy(&out_frame[i], &contact.gps_lon, 4); i += 4;
  memcpy(&out_frame[i], &contact.lastmod, 4); i += 4;
  _serial.writeFrame(out_frame, i);
}

// ── Disk-based message sync ──────────────────────────────────────

void BleCompanionHandler::freeSyncFrames() {
  if (_msg_sync.frames) {
    heap_caps_free(_msg_sync.frames);
    _msg_sync.frames = nullptr;
  }
  _msg_sync.frame_count = 0;
  _msg_sync.frame_idx = 0;
}

bool BleCompanionHandler::loadNextFileFrames() {
  freeSyncFrames();

  static const int SYNC_BATCH = 200;

  while (_msg_sync.current_file < _msg_sync.file_count) {
    const char* path = _msg_sync.files[_msg_sync.current_file];
    _msg_sync.current_file++;

    StoredMsg* records = (StoredMsg*)heap_caps_malloc(
        SYNC_BATCH * sizeof(StoredMsg), MALLOC_CAP_SPIRAM);
    if (!records) continue;

    int n = _mesh.readStoredMsgsSince(path, _msg_sync.since, records, SYNC_BATCH);
    if (n <= 0) { heap_caps_free(records); continue; }

    _msg_sync.frames = (SyncFrame*)heap_caps_malloc(
        n * sizeof(SyncFrame), MALLOC_CAP_SPIRAM);
    if (!_msg_sync.frames) { heap_caps_free(records); continue; }

    int fc = 0;
    for (int i = 0; i < n; i++) {
      // Always advance the watermark — even for self-sent messages
      // that won't generate a sync frame. This avoids re-reading
      // them on every subsequent sync.
      if (records[i].timestamp > _msg_sync.most_recent_ts)
        _msg_sync.most_recent_ts = records[i].timestamp;

      int flen = buildSyncFrame(records[i], _msg_sync.frames[fc].data);
      if (flen > 0) {
        _msg_sync.frames[fc].len = (uint8_t)flen;
        fc++;
      }
    }
    heap_caps_free(records);

    if (fc > 0) {
      _msg_sync.frame_count = fc;
      _msg_sync.frame_idx = 0;
      _msg_sync.active = true;
      // Save watermark after each file so a BLE disconnect doesn't
      // lose all progress — next sync resumes from here.
      if (_msg_sync.most_recent_ts > 0)
        save_sync_timestamp(_mesh, _msg_sync.most_recent_ts);
      return true;
    }
    heap_caps_free(_msg_sync.frames);
    _msg_sync.frames = nullptr;
  }

  _msg_sync.active = false;
  return false;
}

int BleCompanionHandler::buildSyncFrame(const StoredMsg& m, uint8_t* frame) {
  if (strcmp(m.from, _mesh._prefs.node_name) == 0) return 0;

  int i = 0;
  bool is_dm = (m.flags & 0x02) != 0;
  int8_t snr4 = (int8_t)(m.snr * 4);

  if (is_dm) {
    if (!m.has_pub_key) return 0;
    frame[i++] = RESP_CODE_CONTACT_MSG_RECV_V3;
    frame[i++] = (uint8_t)snr4;
    frame[i++] = 0;
    frame[i++] = 0;
    memcpy(&frame[i], m.sender_pub_key, 6); i += 6;
    frame[i++] = 0xFF;
    frame[i++] = TXT_TYPE_PLAIN;
    memcpy(&frame[i], &m.timestamp, 4); i += 4;
  } else {
    frame[i++] = RESP_CODE_CHANNEL_MSG_RECV_V3;
    frame[i++] = (uint8_t)snr4;
    frame[i++] = 0;
    frame[i++] = 0;
    frame[i++] = (uint8_t)m.channel_idx;
    frame[i++] = 0xFF;
    frame[i++] = TXT_TYPE_PLAIN;
    memcpy(&frame[i], &m.timestamp, 4); i += 4;
  }

  if (!is_dm && m.from[0]) {
    int nlen = strlen(m.from);
    if (nlen + 2 > MAX_FRAME_SIZE - i) return 0;
    memcpy(&frame[i], m.from, nlen); i += nlen;
    frame[i++] = ':'; frame[i++] = ' ';
  }

  int tlen = strlen(m.text);
  if (tlen > MAX_FRAME_SIZE - i) tlen = MAX_FRAME_SIZE - i;
  memcpy(&frame[i], m.text, tlen); i += tlen;
  return i;
}

// ── Push methods (called from PunkMesh RX handlers on Core 1) ───

void BleCompanionHandler::queueReceivedDM(const ContactInfo& from,
                                           mesh::Packet* pkt,
                                           uint32_t timestamp,
                                           const char* text) {
  if (_serial.isConnected()) {
    uint8_t push[1] = { PUSH_CODE_MSG_WAITING };
    _serial.writeFrame(push, 1);
  }
}

void BleCompanionHandler::queueReceivedChannelMsg(const mesh::GroupChannel& channel,
                                                    mesh::Packet* pkt,
                                                    uint32_t timestamp,
                                                    const char* text,
                                                    int channel_idx) {
  if (_serial.isConnected()) {
    uint8_t push[1] = { PUSH_CODE_MSG_WAITING };
    _serial.writeFrame(push, 1);
  }
}

void BleCompanionHandler::pushAdvert(const ContactInfo& contact, bool is_new,
                                      uint8_t path_len, const uint8_t* path) {
  if (!_serial.isConnected()) return;

  if (is_new) {
    writeContactRespFrame(PUSH_CODE_NEW_ADVERT, contact);
  } else {
    uint8_t buf[1 + PUB_KEY_SIZE];
    buf[0] = PUSH_CODE_ADVERT;
    memcpy(&buf[1], contact.id.pub_key, PUB_KEY_SIZE);
    _serial.writeFrame(buf, 1 + PUB_KEY_SIZE);
  }
}

void BleCompanionHandler::pushSendConfirmed(uint32_t ack_crc, uint32_t trip_time_ms) {
  if (!_serial.isConnected()) return;

  for (int j = 0; j < EXPECTED_ACK_TABLE_SIZE; j++) {
    if (expected_ack_table[j].ack == ack_crc && expected_ack_table[j].ack != 0) {
      uint8_t buf[11];
      buf[0] = PUSH_CODE_SEND_CONFIRMED;
      memcpy(&buf[1], expected_ack_table[j].contact->id.pub_key, 6);
      memcpy(&buf[7], &trip_time_ms, 4);
      _serial.writeFrame(buf, 11);
      expected_ack_table[j].ack = 0;
      return;
    }
  }
}

void BleCompanionHandler::pushPathUpdated(const ContactInfo& contact) {
  if (!_serial.isConnected()) return;

  uint8_t buf[2 + PUB_KEY_SIZE];
  int i = 0;
  buf[i++] = PUSH_CODE_PATH_UPDATED;
  buf[i++] = contact.out_path_len;
  memcpy(&buf[i], contact.id.pub_key, 6); i += 6;
  _serial.writeFrame(buf, i);
}

void BleCompanionHandler::pushLogRxData(mesh::Packet* pkt, float snr, float rssi) {
  if (!_serial.isConnected()) return;
  int raw_len = pkt->getRawLength();
  if (raw_len > (int)MAX_FRAME_SIZE - 3) return;

  uint8_t buf[MAX_FRAME_SIZE];
  buf[0] = PUSH_CODE_LOG_RX_DATA;
  buf[1] = (int8_t)(snr * 4);
  buf[2] = (int8_t)rssi;
  pkt->writeTo(&buf[3]);
  _serial.writeFrame(buf, 3 + raw_len);
}

#endif // BLE_COMPANION_ENABLED
