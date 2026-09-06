#include "ble_companion.h"

#if BLE_COMPANION_ENABLED

// MC-PKG divergence (whole include set + lifecycle): the NimBLE stack and
// its PunkBLEInterface stay firmware, behind the BleHostApi transport —
// mc_bleproto.cpp owns transport lifecycle and the handler runs over the
// BaseSerialInterface it is handed. Telemetry/battery/fs details route
// through the host (divergences below at their sites).
#include "punkmesh.h"
#include "meshpunk_sync.h"
#include "mc_internal.h"
#include "mc_bridges.h"    // radio_apply_*, mc_device_reboot
#include <LittleFS.h>
#include <esp_heap_caps.h>
#include <CayenneLPP.h>    // shim: exact-wire voltage encode, no lib deps

// Firmware L:-stats reader (meshpunk_fs; exported by mangled name — the
// stats the phone app shows are the device's real internal flash).
bool mp_littlefs_df(size_t* total, size_t* used);
#include <helpers/SensorManager.h>   // TELEM_* constants
#include <helpers/TxtDataHelpers.h>

// ble_companion (the iface pointer) is DEFINED in the LoRa elf; this elf
// imports it and points it at the concrete handler while attached.
static BleCompanionHandler* s_handler = nullptr;

// ── Lifecycle (package form: transport handled by mc_bleproto.cpp) ──────────

void ble_companion_attach(PunkMesh& mesh, BaseSerialInterface& serial) {
  if (s_handler) return;
  void* mem = heap_caps_malloc(sizeof(BleCompanionHandler), MALLOC_CAP_SPIRAM);
  if (mem) {
    s_handler = new (mem) BleCompanionHandler(mesh, serial);
    ble_companion = s_handler;
  } else {
    SLog.println("[BLE] FATAL: cannot allocate companion handler");
  }
}

void ble_companion_detach() {
  if (!s_handler) return;
  ble_companion = nullptr;   // PunkMesh stops dispatching first
  s_handler->~BleCompanionHandler();
  heap_caps_free(s_handler);
  s_handler = nullptr;
}

// ── BleCompanionHandler ──────────────────────────────────────────

BleCompanionHandler::BleCompanionHandler(PunkMesh& mesh, BaseSerialInterface& serial)
  : _mesh(mesh), _serial(serial)
{
  _iter_started = false;
  _iter_filter_since = 0;
  _most_recent_lastmod = 0;
  app_target_ver = 0;
  next_ack_idx = 0;
  pending_login = 0;
  pending_status = 0;
  pending_telemetry = 0;
  pending_discovery = 0;
  pending_req = 0;
  sign_data = nullptr;
  sign_data_len = 0;
  memset(expected_ack_table, 0, sizeof(expected_ack_table));
  _resp_retry_len = 0;
  _msg_sync.begin(&mesh);
}

BleCompanionHandler::~BleCompanionHandler() {
  _msg_sync.end();
  if (sign_data) { free(sign_data); sign_data = nullptr; }
}

// Response with a retry slot: the app awaits every reply, so a queue-full
// drop would strand it until timeout. Retried from loop() until it lands.
bool BleCompanionHandler::sendResp(size_t len) {
  if (_serial.writeFrame(out_frame, len) > 0) return true;
  if (_resp_retry_len > 0) {
    SLog.printf("[BLE TX] retry slot overwritten (old=0x%02X new=0x%02X)\n",
                _resp_retry[0], out_frame[0]);
  }
  memcpy(_resp_retry, out_frame, len);
  _resp_retry_len = len;
  return false;
}

// Push (async notification): best-effort, and yields while a response is
// waiting on the retry slot so pushes can never starve a reply.
bool BleCompanionHandler::pushFrame(const uint8_t* frame, size_t len) {
  if (_resp_retry_len > 0) return false;
  return _serial.writeFrame(frame, len) > 0;
}

void BleCompanionHandler::loop() {
  // 1. A response that hit a full send queue retries until it lands.
  if (_resp_retry_len > 0) {
    if (_serial.writeFrame(_resp_retry, _resp_retry_len) > 0) {
      _resp_retry_len = 0;
    }
  }

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
      // >= not > (reference uses >): meshpunk lastmod is in-RAM and 0 until a
      // contact's first post-boot advert, so with `>` a full sync (since=0)
      // would skip every un-readverted contact. Cost: the newest contact
      // re-sends on each incremental sync — harmless.
      if (contact.lastmod >= _iter_filter_since) {
        writeContactRespFrame(RESP_CODE_CONTACT, contact);
        if (contact.lastmod > _most_recent_lastmod) {
          _most_recent_lastmod = contact.lastmod;
        }
      }
    } else {
      out_frame[0] = RESP_CODE_END_OF_CONTACTS;
      memcpy(&out_frame[1], &_most_recent_lastmod, 4);
      sendResp(5);
      _iter_started = false;
    }
  } else {
    // Idle: one bounded unit of background sync work (reconcile step or
    // lazy ledger persist), then tell the app if new data turned up.
    _msg_sync.step();
    if (_msg_sync.takeTickle() && _serial.isConnected()) {
      uint8_t push[1] = { PUSH_CODE_MSG_WAITING };
      pushFrame(push, 1);
    }
  }
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

    uint32_t pin = _mesh._prefs.ble_pin;
    memcpy(&out_frame[i], &pin, 4);
    i += 4;

    memset(&out_frame[i], 0, 12);
    strncpy((char*)&out_frame[i], MESHPUNK_FW_BUILD_DATE, 12);
    i += 12;

    StrHelper::strzcpy((char*)&out_frame[i], MESHPUNK_MODEL_NAME, 40);
    i += 40;

    StrHelper::strzcpy((char*)&out_frame[i], MESHPUNK_FW_VERSION, 20);
    i += 20;

    out_frame[i++] = _mesh._prefs.client_repeat;  // v9+
    out_frame[i++] = _mesh._prefs.path_hash_mode; // v10+

    sendResp(i);

  } else if (cmd_frame[0] == CMD_APP_START && len >= 8) {
    char* app_name = (char*)&cmd_frame[8];
    cmd_frame[len] = 0;
    SLog.printf("[BLE] App '%s' connected\n", app_name);

    _iter_started = false;

    // No message-store work here — SELF_INFO must go out immediately (the
    // app's command queue times out in ~5s and every settings screen
    // re-sends APP_START). Sync serving is pull-driven via SYNC_NEXT.
    _msg_sync.onAppStart();

    int i = 0;
    out_frame[i++] = RESP_CODE_SELF_INFO;
    out_frame[i++] = ADV_TYPE_CHAT;
    out_frame[i++] = _mesh._prefs.tx_power_dbm;
    out_frame[i++] = (uint8_t)mc_max_tx_dbm;   // board cap, host-forwarded

    memcpy(&out_frame[i], _mesh.self_id.pub_key, PUB_KEY_SIZE);
    i += PUB_KEY_SIZE;

    int32_t lat = (int32_t)(_mesh._prefs.node_lat * 1000000.0);
    int32_t lon = (int32_t)(_mesh._prefs.node_lon * 1000000.0);
    memcpy(&out_frame[i], &lat, 4); i += 4;
    memcpy(&out_frame[i], &lon, 4); i += 4;

    out_frame[i++] = 0; // multi_acks (not used in meshpunk)
    out_frame[i++] = _mesh._prefs.advert_loc_policy; // 0=omit location in adverts, 1=share
    out_frame[i++] = 0; // telemetry modes
    out_frame[i++] = _mesh._prefs.manual_add_contacts; // auto-add mode (0=all, bit0=selected)

    uint32_t freq = (uint32_t)(_mesh._prefs.freq * 1000);
    memcpy(&out_frame[i], &freq, 4); i += 4;

    uint32_t bw = (uint32_t)(_mesh._prefs.bandwidth * 1000);
    memcpy(&out_frame[i], &bw, 4); i += 4;

    out_frame[i++] = _mesh._prefs.spreading_factor;
    out_frame[i++] = _mesh._prefs.coding_rate;

    int tlen = strlen(_mesh._prefs.node_name);
    memcpy(&out_frame[i], _mesh._prefs.node_name, tlen);
    i += tlen;

    sendResp(i);

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
    sendResp(5);
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
      sendResp(i);
    } else {
      writeErrFrame(ERR_CODE_NOT_FOUND);
    }

  } else if (cmd_frame[0] == CMD_GET_DEVICE_TIME) {
    uint32_t now = _mesh.getRTCClock()->getCurrentTime();
    out_frame[0] = RESP_CODE_CURR_TIME;
    memcpy(&out_frame[1], &now, 4);
    sendResp(5);

  } else if (cmd_frame[0] == CMD_SET_DEVICE_TIME && len >= 5) {
    uint32_t t;
    memcpy(&t, &cmd_frame[1], 4);
    // Routed through the clock-authority tiers: phone time beats seeds and
    // GPS V-time (even backwards — it's truer than module-clock drift) but
    // never a real GPS fix or a manual set. Rejections log a [CLOCK] line.
    // MC-PKG divergence: via the host authority (quality 2 = the PHONE tier).
    if (MCH->clock_suggest(t, 2)) {
      writeOKFrame();
    } else {
      writeErrFrame(ERR_CODE_ILLEGAL_ARG);
    }

  } else if (cmd_frame[0] == CMD_GET_BATT_AND_STORAGE && len >= 1) {
    // Standard layout: [batt_mv 2][used_kb 4][total_kb 4] = 11 bytes.
    // Storage figures are the internal LittleFS (same role as the
    // reference's UserData FS); SD is not reported here. usedBytes() walks
    // the whole partition (~600ms on 6MB, measured on hw) so it's cached
    // for 30s; totalBytes is constant after mount.
    uint16_t batt_mv = MCH->batt_mv();
    static uint32_t cached_used_kb = 0, cached_total_kb = 0;
    static uint32_t storage_cache_ms = 0;
    if (cached_total_kb == 0 || millis() - storage_cache_ms > 30000) {
      size_t total = 0, used = 0;
      mp_littlefs_df(&total, &used);
      cached_used_kb  = used  / 1024;
      cached_total_kb = total / 1024;
      storage_cache_ms = millis();
    }
    uint32_t used_kb = cached_used_kb, total_kb = cached_total_kb;
    out_frame[0] = RESP_CODE_BATT_AND_STORAGE;
    memcpy(&out_frame[1], &batt_mv, 2);
    memcpy(&out_frame[3], &used_kb, 4);
    memcpy(&out_frame[7], &total_kb, 4);
    sendResp(11);

  } else if (cmd_frame[0] == CMD_SYNC_NEXT_MESSAGE) {
    // Ledger-driven: only dirty files are opened, seek-positioned at their
    // synced offset. An idle poll answers NO_MORE with zero file opens.
    if (!_msg_sync.hasFrame()) _msg_sync.tryLoadMore();

    if (_msg_sync.hasFrame()) {
      int flen = _msg_sync.nextFrame(out_frame);
      sendResp(flen);
    } else {
      _msg_sync.onSyncSessionEnd();
      out_frame[0] = RESP_CODE_NO_MORE_MESSAGES;
      sendResp(1);
    }

  } else if (cmd_frame[0] == CMD_SEND_TXT_MSG && len >= 14) {
    int i = 1;
    uint8_t txt_type = cmd_frame[i++];
    uint8_t attempt  = cmd_frame[i++];
    uint32_t msg_timestamp;
    memcpy(&msg_timestamp, &cmd_frame[i], 4); i += 4;
    uint8_t* pub_key_prefix = &cmd_frame[i]; i += 6;

    ContactInfo* recipient = _mesh.lookupContactByPubKey(pub_key_prefix, 6);
    if (recipient && (txt_type == TXT_TYPE_PLAIN || txt_type == TXT_TYPE_CLI_DATA)) {
      int tlen = len - i;
      cmd_frame[i + tlen] = 0;
      char text[160];
      normalize_smart_quotes((const char*)&cmd_frame[i], text, sizeof(text));

      uint32_t est_timeout;
      uint32_t expected_ack = 0;
      int result;

      if (txt_type == TXT_TYPE_CLI_DATA) {
        // CLI commands use node's RTC (not app timestamp) to avoid replay protection
        msg_timestamp = _mesh.getRTCClock()->getCurrentTimeUnique();
        // Tracked variant: identical wire behavior + repeat-until-heard on
        // direct routes (and no spurious ack timeout — CLI has no acks).
        result = _mesh.sendCommandTracked(*recipient, msg_timestamp, attempt, text, est_timeout);
      } else {
        auto r = _mesh.sendAndPersistDM(*recipient, msg_timestamp, attempt, text);
        result = r.code;
        expected_ack = r.expected_ack;
        est_timeout = r.est_timeout;
      }

      if (result == MSG_SEND_FAILED) {
        writeErrFrame(ERR_CODE_TABLE_FULL);
      } else {
        if (expected_ack) {
          expected_ack_table[next_ack_idx].msg_sent = millis();
          expected_ack_table[next_ack_idx].ack = expected_ack;
          next_ack_idx = (next_ack_idx + 1) % EXPECTED_ACK_TABLE_SIZE;
        }
        out_frame[0] = RESP_CODE_SENT;
        out_frame[1] = (result == MSG_SEND_SENT_FLOOD) ? 1 : 0;
        memcpy(&out_frame[2], &expected_ack, 4);
        memcpy(&out_frame[6], &est_timeout, 4);
        sendResp(10);
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
    auto pkt = _mesh.buildSelfAdvert();
    if (pkt) {
      if (len >= 2 && cmd_frame[1] == 1) {
        _mesh.sendFlood(pkt, (uint32_t)0, _mesh.pathHashSize());
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
  // Frame layout (from companion_radio updateContactFromFrame):
  //   [cmd 1B][pub_key 32B][type 1B][flags 1B][out_path_len 1B]
  //   [out_path 64B][name 32B][advert_ts 4B]
  //   [gps_lat 4B?][gps_lon 4B?][lastmod 4B?]
  } else if (cmd_frame[0] == CMD_ADD_UPDATE_CONTACT &&
             len >= 1 + PUB_KEY_SIZE + 3 + MAX_PATH_SIZE + 32 + 4) {
    uint8_t* pub_key = &cmd_frame[1];
    uint32_t last_mod = _mesh.getRTCClock()->getCurrentTime();
    ContactInfo* existing = _mesh.lookupContactByPubKey(pub_key, PUB_KEY_SIZE);
    if (existing) {
      int i = 1;
      memcpy(existing->id.pub_key, &cmd_frame[i], PUB_KEY_SIZE); i += PUB_KEY_SIZE;
      existing->type = cmd_frame[i++];
      existing->flags = cmd_frame[i++];
      existing->out_path_len = cmd_frame[i++];
      memcpy(existing->out_path, &cmd_frame[i], MAX_PATH_SIZE); i += MAX_PATH_SIZE;
      memcpy(existing->name, &cmd_frame[i], 32); i += 32;
      memcpy(&existing->last_advert_timestamp, &cmd_frame[i], 4); i += 4;
      if ((int)len >= i + 8) {
        memcpy(&existing->gps_lat, &cmd_frame[i], 4); i += 4;
        memcpy(&existing->gps_lon, &cmd_frame[i], 4); i += 4;
        if ((int)len >= i + 4) {
          memcpy(&last_mod, &cmd_frame[i], 4);
        }
      }
      existing->lastmod = last_mod;
      _mesh.saveContacts();
      writeOKFrame();
    } else {
      ContactInfo contact;
      memset(&contact, 0, sizeof(contact));
      int i = 1;
      memcpy(contact.id.pub_key, &cmd_frame[i], PUB_KEY_SIZE); i += PUB_KEY_SIZE;
      contact.type = cmd_frame[i++];
      contact.flags = cmd_frame[i++];
      contact.out_path_len = cmd_frame[i++];
      memcpy(contact.out_path, &cmd_frame[i], MAX_PATH_SIZE); i += MAX_PATH_SIZE;
      memcpy(contact.name, &cmd_frame[i], 32); i += 32;
      memcpy(&contact.last_advert_timestamp, &cmd_frame[i], 4); i += 4;
      if ((int)len >= i + 8) {
        memcpy(&contact.gps_lat, &cmd_frame[i], 4); i += 4;
        memcpy(&contact.gps_lon, &cmd_frame[i], 4); i += 4;
        if ((int)len >= i + 4) {
          memcpy(&last_mod, &cmd_frame[i], 4);
        }
      }
      contact.lastmod = last_mod;
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
      auto pkt = _mesh.buildSelfAdvert();
      if (pkt) {
        pkt->header |= ROUTE_TYPE_FLOOD;
        out_frame[0] = RESP_CODE_EXPORT_CONTACT;
        uint8_t out_len = pkt->writeTo(&out_frame[1]);
        _mesh.releasePacket(pkt);
        sendResp(out_len + 1);
      } else {
        writeErrFrame(ERR_CODE_TABLE_FULL);
      }
    } else {
      uint8_t* pub_key = &cmd_frame[1];
      ContactInfo* contact = _mesh.lookupContactByPubKey(pub_key, PUB_KEY_SIZE);
      uint8_t out_len;
      if (contact && (out_len = _mesh.exportContact(*contact, &out_frame[1])) > 0) {
        out_frame[0] = RESP_CODE_EXPORT_CONTACT;
        sendResp(out_len + 1);
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
    uint8_t repeat = 0;  // client_repeat rides this frame since app ver 9
    if ((int)len > i) {
      repeat = cmd_frame[i++];
    }
    if (freq >= 150000 && freq <= 2500000 && sf >= 5 && sf <= 12
        && cr >= 5 && cr <= 8 && bw >= 7000 && bw <= 500000) {
      _mesh._prefs.freq = (float)freq / 1000.0f;
      _mesh._prefs.bandwidth = (float)bw / 1000.0f;
      _mesh._prefs.spreading_factor = sf;
      _mesh._prefs.coding_rate = cr;
      _mesh._prefs.client_repeat = repeat;
      _mesh.savePrefs();
      radio_apply_params(_mesh._prefs.freq, _mesh._prefs.bandwidth, sf, cr);
      writeOKFrame();
    } else {
      writeErrFrame(ERR_CODE_ILLEGAL_ARG);
    }

  } else if (cmd_frame[0] == CMD_SET_RADIO_TX_POWER && len >= 2) {
    int8_t power = (int8_t)cmd_frame[1];
    if (power < -9 || power > mc_max_tx_dbm) {
      writeErrFrame(ERR_CODE_ILLEGAL_ARG);
    } else {
      _mesh._prefs.tx_power_dbm = power;
      _mesh.savePrefs();
      radio_apply_tx_power(power);
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
    sendResp(i);

  } else if (cmd_frame[0] == CMD_SET_OTHER_PARAMS && len >= 2) {
    // cmd_frame[1] = manual_add_contacts (bit0: auto-add ALL=0 / SELECTED=1).
    // cmd_frame[3] = advert_loc_policy (0 = omit GPS location from adverts).
    // The other fields (telemetry / multi-acks) aren't used by meshpunk.
    _mesh._prefs.manual_add_contacts = cmd_frame[1];
    if (len >= 4) _mesh._prefs.advert_loc_policy = cmd_frame[3];
    _mesh.savePrefs();
    writeOKFrame();

  // ── Device operations ───────────────────────────────────────────
  } else if (cmd_frame[0] == CMD_REBOOT && len >= 7
             && memcmp(&cmd_frame[1], "reboot", 6) == 0) {
    _mesh.saveContacts();
    writeOKFrame();
    // MC-PKG divergence: user-initiated restart via the host (it delays and
    // restarts; the OK frame above goes out first).
    mc_device_reboot();

  } else if (cmd_frame[0] == CMD_FACTORY_RESET && len >= 6
             && memcmp(&cmd_frame[1], "reset", 5) == 0) {
    // MC-PKG divergence: REFUSED for now — formatting internal flash from a
    // protocol package awaits an explicit host-side decision (campaign
    // ledger). The firmware build performed it here.
    MCH->log("factory reset via BLE refused (package) - pending host support");
    writeErrFrame(ERR_CODE_ILLEGAL_ARG);

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
      sendResp(10);
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
      sendResp(i);
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
    sendResp(65);

  } else if (cmd_frame[0] == CMD_IMPORT_PRIVATE_KEY && len >= 65) {
    if (!mesh::LocalIdentity::validatePrivateKey(&cmd_frame[1])) {
      writeErrFrame(ERR_CODE_ILLEGAL_ARG);
    } else {
      _mesh.self_id.readFrom(&cmd_frame[1], PRV_KEY_SIZE);
      if (_mesh.saveIdentity()) {
        writeOKFrame();
        mc_device_reboot();   // MC-PKG divergence: user-initiated, host-run
      } else {
        writeErrFrame(ERR_CODE_FILE_IO_ERROR);
      }
    }

  } else if (cmd_frame[0] == CMD_GENERATE_IDENTITY && len >= 9
             && memcmp(&cmd_frame[1], "generate", 8) == 0) {
    _mesh.self_id = mesh::LocalIdentity(_mesh.getRNG());
    int count = 0;
    while (count < 10 && (_mesh.self_id.pub_key[0] == 0x00 || _mesh.self_id.pub_key[0] == 0xFF)) {
      _mesh.self_id = mesh::LocalIdentity(_mesh.getRNG());
      count++;
    }
    if (_mesh.saveIdentity()) {
      writeOKFrame();
      mc_device_reboot();   // MC-PKG divergence: user-initiated, host-run
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
        clearPendingReqs();
        memcpy(&pending_login, recipient->id.pub_key, 4);
        out_frame[0] = RESP_CODE_SENT;
        out_frame[1] = (result == MSG_SEND_SENT_FLOOD) ? 1 : 0;
        memcpy(&out_frame[2], &pending_login, 4);
        memcpy(&out_frame[6], &est_timeout, 4);
        sendResp(10);
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
        clearPendingReqs();
        memcpy(&pending_status, recipient->id.pub_key, 4);
        out_frame[0] = RESP_CODE_SENT;
        out_frame[1] = (result == MSG_SEND_SENT_FLOOD) ? 1 : 0;
        memcpy(&out_frame[2], &tag, 4);
        memcpy(&out_frame[6], &est_timeout, 4);
        sendResp(10);
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

  // ── Anon request ─────────────────────────────────────────────────
  } else if (cmd_frame[0] == CMD_SEND_ANON_REQ && len > 1 + PUB_KEY_SIZE) {
    uint8_t* pub_key = &cmd_frame[1];
    ContactInfo* recipient = _mesh.lookupContactByPubKey(pub_key, PUB_KEY_SIZE);
    if (recipient) {
      uint8_t* data = &cmd_frame[1 + PUB_KEY_SIZE];
      uint32_t tag, est_timeout;
      int result = _mesh.sendAnonReq(*recipient, data, len - (1 + PUB_KEY_SIZE), tag, est_timeout);
      if (result == MSG_SEND_FAILED) {
        writeErrFrame(ERR_CODE_TABLE_FULL);
      } else {
        clearPendingReqs();
        pending_req = tag;
        out_frame[0] = RESP_CODE_SENT;
        out_frame[1] = (result == MSG_SEND_SENT_FLOOD) ? 1 : 0;
        memcpy(&out_frame[2], &tag, 4);
        memcpy(&out_frame[6], &est_timeout, 4);
        sendResp(10);
      }
    } else {
      writeErrFrame(ERR_CODE_NOT_FOUND);
    }

  // ── Binary request ──────────────────────────────────────────────
  } else if (cmd_frame[0] == CMD_SEND_BINARY_REQ && len >= 2 + PUB_KEY_SIZE) {
    uint8_t* pub_key = &cmd_frame[1];
    ContactInfo* recipient = _mesh.lookupContactByPubKey(pub_key, PUB_KEY_SIZE);
    if (recipient) {
      uint8_t* req_data = &cmd_frame[1 + PUB_KEY_SIZE];
      uint32_t tag, est_timeout;
      int result = _mesh.sendRequest(*recipient, req_data, len - (1 + PUB_KEY_SIZE), tag, est_timeout);
      if (result == MSG_SEND_FAILED) {
        writeErrFrame(ERR_CODE_TABLE_FULL);
      } else {
        clearPendingReqs();
        pending_req = tag;
        out_frame[0] = RESP_CODE_SENT;
        out_frame[1] = (result == MSG_SEND_SENT_FLOOD) ? 1 : 0;
        memcpy(&out_frame[2], &tag, 4);
        memcpy(&out_frame[6], &est_timeout, 4);
        sendResp(10);
      }
    } else {
      writeErrFrame(ERR_CODE_NOT_FOUND);
    }

  // ── Telemetry request (remote) ──────────────────────────────────
  } else if (cmd_frame[0] == CMD_SEND_TELEMETRY_REQ && len >= 4 + PUB_KEY_SIZE) {
    uint8_t* pub_key = &cmd_frame[4];
    ContactInfo* recipient = _mesh.lookupContactByPubKey(pub_key, PUB_KEY_SIZE);
    if (recipient) {
      uint32_t tag, est_timeout;
      int result = _mesh.sendRequest(*recipient, REQ_TYPE_GET_TELEMETRY_DATA, tag, est_timeout);
      if (result == MSG_SEND_FAILED) {
        writeErrFrame(ERR_CODE_TABLE_FULL);
      } else {
        clearPendingReqs();
        pending_telemetry = tag;
        out_frame[0] = RESP_CODE_SENT;
        out_frame[1] = (result == MSG_SEND_SENT_FLOOD) ? 1 : 0;
        memcpy(&out_frame[2], &tag, 4);
        memcpy(&out_frame[6], &est_timeout, 4);
        sendResp(10);
      }
    } else {
      writeErrFrame(ERR_CODE_NOT_FOUND);
    }

  // ── Telemetry request (self) ────────────────────────────────────
  } else if (cmd_frame[0] == CMD_SEND_TELEMETRY_REQ && len == 4) {
    uint16_t batt_mv = MCH->batt_mv();
    CayenneLPP telemetry(51);
    telemetry.reset();
    telemetry.addVoltage(TELEM_CHANNEL_SELF, (float)batt_mv / 1000.0f);

    int i = 0;
    out_frame[i++] = PUSH_CODE_TELEMETRY_RESPONSE;
    out_frame[i++] = 0;
    memcpy(&out_frame[i], _mesh.self_id.pub_key, 6); i += 6;
    uint8_t tlen = telemetry.getSize();
    memcpy(&out_frame[i], telemetry.getBuffer(), tlen); i += tlen;
    sendResp(i);

  // ── Path discovery request ──────────────────────────────────────
  } else if (cmd_frame[0] == CMD_SEND_PATH_DISCOVERY_REQ && cmd_frame[1] == 0 && len >= 2 + PUB_KEY_SIZE) {
    uint8_t* pub_key = &cmd_frame[2];
    ContactInfo* recipient = _mesh.lookupContactByPubKey(pub_key, PUB_KEY_SIZE);
    if (recipient) {
      uint32_t tag, est_timeout;
      uint8_t req_data[9];
      req_data[0] = REQ_TYPE_GET_TELEMETRY_DATA;
      req_data[1] = ~(TELEM_PERM_BASE);
      memset(&req_data[2], 0, 3);
      _mesh.getRNG()->random(&req_data[5], 4);
      auto save = recipient->out_path_len;
      recipient->out_path_len = OUT_PATH_UNKNOWN;
      int result = _mesh.sendRequest(*recipient, req_data, sizeof(req_data), tag, est_timeout);
      recipient->out_path_len = save;
      if (result == MSG_SEND_FAILED) {
        writeErrFrame(ERR_CODE_TABLE_FULL);
      } else {
        clearPendingReqs();
        pending_discovery = tag;
        out_frame[0] = RESP_CODE_SENT;
        out_frame[1] = (result == MSG_SEND_SENT_FLOOD) ? 1 : 0;
        memcpy(&out_frame[2], &tag, 4);
        memcpy(&out_frame[6], &est_timeout, 4);
        sendResp(10);
      }
    } else {
      writeErrFrame(ERR_CODE_NOT_FOUND);
    }

  // ── Control data ────────────────────────────────────────────────
  } else if (cmd_frame[0] == CMD_SEND_CONTROL_DATA && len >= 2 && (cmd_frame[1] & 0x80) != 0) {
    auto resp = _mesh.createControlData(&cmd_frame[1], len - 1);
    if (resp) {
      _mesh.sendZeroHop(resp);
      writeOKFrame();
    } else {
      writeErrFrame(ERR_CODE_TABLE_FULL);
    }

  // ── Channel data ────────────────────────────────────────────────
  } else if (cmd_frame[0] == CMD_SEND_CHANNEL_DATA) {
    if (len < 4) {
      writeErrFrame(ERR_CODE_ILLEGAL_ARG);
    } else {
      int i = 1;
      uint8_t channel_idx = cmd_frame[i++];
      uint8_t path_len = cmd_frame[i++];

      if (!mesh::Packet::isValidPathLen(path_len) && path_len != OUT_PATH_UNKNOWN) {
        writeErrFrame(ERR_CODE_ILLEGAL_ARG);
      } else {
        uint8_t path[MAX_PATH_SIZE];
        if (path_len != OUT_PATH_UNKNOWN) {
          i += mesh::Packet::writePath(path, &cmd_frame[i], path_len);
        }
        uint16_t data_type = ((uint16_t)cmd_frame[i]) | (((uint16_t)cmd_frame[i + 1]) << 8);
        i += 2;
        const uint8_t* payload = &cmd_frame[i];
        int payload_len = (len > (size_t)i) ? (int)(len - i) : 0;

        ChannelDetails channel;
        if (!_mesh.getChannel(channel_idx, channel)) {
          writeErrFrame(ERR_CODE_NOT_FOUND);
        } else if (data_type == DATA_TYPE_RESERVED) {
          writeErrFrame(ERR_CODE_ILLEGAL_ARG);
        } else if (payload_len > MAX_CHANNEL_DATA_LENGTH) {
          writeErrFrame(ERR_CODE_ILLEGAL_ARG);
        } else if (_mesh.sendGroupData(channel.channel, path, path_len, data_type, payload, payload_len)) {
          writeOKFrame();
        } else {
          writeErrFrame(ERR_CODE_TABLE_FULL);
        }
      }
    }

  // ── Message signing ─────────────────────────────────────────────
  } else if (cmd_frame[0] == CMD_SIGN_START) {
    out_frame[0] = RESP_CODE_SIGN_START;
    out_frame[1] = 0;
    uint32_t max_len = MAX_SIGN_DATA_LEN;
    memcpy(&out_frame[2], &max_len, 4);
    sendResp(6);

    if (sign_data) free(sign_data);
    sign_data = (uint8_t*)malloc(MAX_SIGN_DATA_LEN);
    sign_data_len = 0;

  } else if (cmd_frame[0] == CMD_SIGN_DATA && len > 1) {
    if (sign_data == NULL || sign_data_len + (len - 1) > MAX_SIGN_DATA_LEN) {
      writeErrFrame(sign_data == NULL ? ERR_CODE_BAD_STATE : ERR_CODE_TABLE_FULL);
    } else {
      memcpy(&sign_data[sign_data_len], &cmd_frame[1], len - 1);
      sign_data_len += (len - 1);
      writeOKFrame();
    }

  } else if (cmd_frame[0] == CMD_SIGN_FINISH) {
    if (sign_data) {
      _mesh.self_id.sign(&out_frame[1], sign_data, sign_data_len);
      free(sign_data);
      sign_data = NULL;
      out_frame[0] = RESP_CODE_SIGNATURE;
      sendResp(1 + SIGNATURE_SIZE);
    } else {
      writeErrFrame(ERR_CODE_BAD_STATE);
    }

  // ── Device PIN ──────────────────────────────────────────────────
  } else if (cmd_frame[0] == CMD_SET_DEVICE_PIN && len >= 5) {
    uint32_t pin;
    memcpy(&pin, &cmd_frame[1], 4);
    if (pin == 0 || (pin >= 100000 && pin <= 999999)) {
      _mesh._prefs.ble_pin = pin;
      _mesh.savePrefs();
      writeOKFrame();
    } else {
      writeErrFrame(ERR_CODE_ILLEGAL_ARG);
    }

  // ── Path hash mode ──────────────────────────────────────────────
  } else if (cmd_frame[0] == CMD_SET_PATH_HASH_MODE && len >= 3 && cmd_frame[1] == 0) {
    uint8_t mode = cmd_frame[2];
    if (mode < 3) {
      _mesh._prefs.path_hash_mode = mode;
      _mesh.savePrefs();
      writeOKFrame();
    } else {
      writeErrFrame(ERR_CODE_ILLEGAL_ARG);
    }

  // ── Autoadd config ──────────────────────────────────────────────
  } else if (cmd_frame[0] == CMD_SET_AUTOADD_CONFIG) {
    // MeshCore spec auto-add bitmask (set = DO auto-add that type):
    //   0x01 overwrite-oldest, 0x02 chat, 0x04 repeater, 0x08 room, 0x10 sensor.
    // Type bits store directly in autoadd_config (consulted only in "auto-add
    // selected" mode); the overwrite-oldest bit drives contact_overwrite.
    uint8_t cfg = cmd_frame[1];
    _mesh._prefs.autoadd_config = cfg & 0x1E;  // keep only the four type bits
    _mesh._prefs.contact_overwrite = (cfg & 0x01) ? 1 : 0;
    if (len >= 3) {
      _mesh._prefs.autoadd_max_hops = min(cmd_frame[2], (uint8_t)64);
    }
    _mesh.savePrefs();
    writeOKFrame();

  } else if (cmd_frame[0] == CMD_GET_AUTOADD_CONFIG) {
    int i = 0;
    out_frame[i++] = RESP_CODE_AUTOADD_CONFIG;
    // Spec byte = stored type bits + the overwrite-oldest bit from contact_overwrite.
    uint8_t cfg = _mesh._prefs.autoadd_config & 0x1E;
    if (_mesh._prefs.contact_overwrite) cfg |= 0x01;  // overwrite-oldest
    out_frame[i++] = cfg;
    out_frame[i++] = _mesh._prefs.autoadd_max_hops;
    sendResp(i);

  // ── Flood scope key (runtime) ───────────────────────────────────
  } else if (cmd_frame[0] == CMD_SET_FLOOD_SCOPE_KEY && len >= 2 && cmd_frame[1] == 0) {
    // Session-only scope (companion_radio parity): overrides the persisted
    // default for all sends until cleared or reboot; a per-channel region
    // outranks it. Short frame = clear.
    if (len >= 2 + 16) {
      memcpy(_mesh._ble_send_scope_key, &cmd_frame[2], sizeof(_mesh._ble_send_scope_key));
    } else {
      memset(_mesh._ble_send_scope_key, 0, sizeof(_mesh._ble_send_scope_key));
    }
    writeOKFrame();

  // ── Default flood scope (persisted) ─────────────────────────────
  } else if (cmd_frame[0] == CMD_SET_DEFAULT_FLOOD_SCOPE && len >= 1) {
    if (len >= 1 + 31 + 16) {
      int n = strlen((char*)&cmd_frame[1]);
      if (n > 0 && n < 31) {
        strcpy(_mesh._prefs.default_scope_name, (char*)&cmd_frame[1]);
        memcpy(_mesh._prefs.default_scope_key, &cmd_frame[1 + 31], 16);
        _mesh.savePrefs();
        writeOKFrame();
      } else {
        writeErrFrame(ERR_CODE_ILLEGAL_ARG);
      }
    } else {
      memset(_mesh._prefs.default_scope_name, 0, sizeof(_mesh._prefs.default_scope_name));
      memset(_mesh._prefs.default_scope_key, 0, sizeof(_mesh._prefs.default_scope_key));
      _mesh.savePrefs();
      writeOKFrame();
    }

  } else if (cmd_frame[0] == CMD_GET_DEFAULT_FLOOD_SCOPE) {
    out_frame[0] = RESP_CODE_DEFAULT_FLOOD_SCOPE;
    if (strlen(_mesh._prefs.default_scope_name) > 0) {
      memcpy(&out_frame[1], _mesh._prefs.default_scope_name, 31);
      memcpy(&out_frame[1 + 31], _mesh._prefs.default_scope_key, 16);
      sendResp(1 + 31 + 16);
    } else {
      sendResp(1);
    }

  // ── Stats ───────────────────────────────────────────────────────
  } else if (cmd_frame[0] == CMD_GET_STATS && len >= 2) {
    uint8_t stats_type = cmd_frame[1];
    if (stats_type == STATS_TYPE_CORE) {
      int i = 0;
      out_frame[i++] = RESP_CODE_STATS;
      out_frame[i++] = STATS_TYPE_CORE;
      uint16_t battery_mv = MCH->batt_mv();
      uint32_t uptime_secs = millis() / 1000;
      uint16_t err_flags = _mesh.getErrorFlags();
      uint8_t queue_len = _mesh.getQueueLength();
      memcpy(&out_frame[i], &battery_mv, 2); i += 2;
      memcpy(&out_frame[i], &uptime_secs, 4); i += 4;
      memcpy(&out_frame[i], &err_flags, 2); i += 2;
      out_frame[i++] = queue_len;
      sendResp(i);
    } else if (stats_type == STATS_TYPE_RADIO) {
      int i = 0;
      out_frame[i++] = RESP_CODE_STATS;
      out_frame[i++] = STATS_TYPE_RADIO;
      int16_t noise_floor = (int16_t)_mesh.getRadioNoiseFloor();
      int8_t last_rssi = (int8_t)_mesh.last_rx_rssi;
      int8_t last_snr = (int8_t)(_mesh.last_rx_snr * 4);
      uint32_t tx_air_secs = _mesh.getTotalAirTime() / 1000;
      uint32_t rx_air_secs = _mesh.getReceiveAirTime() / 1000;
      memcpy(&out_frame[i], &noise_floor, 2); i += 2;
      out_frame[i++] = last_rssi;
      out_frame[i++] = last_snr;
      memcpy(&out_frame[i], &tx_air_secs, 4); i += 4;
      memcpy(&out_frame[i], &rx_air_secs, 4); i += 4;
      sendResp(i);
    } else if (stats_type == STATS_TYPE_PACKETS) {
      int i = 0;
      out_frame[i++] = RESP_CODE_STATS;
      out_frame[i++] = STATS_TYPE_PACKETS;
      uint32_t n_sent_flood = _mesh.getNumSentFlood();
      uint32_t n_sent_direct = _mesh.getNumSentDirect();
      uint32_t n_recv_flood = _mesh.getNumRecvFlood();
      uint32_t n_recv_direct = _mesh.getNumRecvDirect();
      uint32_t recv = n_recv_flood + n_recv_direct;
      uint32_t sent = n_sent_flood + n_sent_direct;
      uint32_t n_recv_errors = 0;
      memcpy(&out_frame[i], &recv, 4); i += 4;
      memcpy(&out_frame[i], &sent, 4); i += 4;
      memcpy(&out_frame[i], &n_sent_flood, 4); i += 4;
      memcpy(&out_frame[i], &n_sent_direct, 4); i += 4;
      memcpy(&out_frame[i], &n_recv_flood, 4); i += 4;
      memcpy(&out_frame[i], &n_recv_direct, 4); i += 4;
      memcpy(&out_frame[i], &n_recv_errors, 4); i += 4;
      sendResp(i);
    } else {
      writeErrFrame(ERR_CODE_ILLEGAL_ARG);
    }

  // ── Allowed repeat frequencies ──────────────────────────────────
  } else if (cmd_frame[0] == CMD_GET_ALLOWED_REPEAT_FREQ) {
    struct FreqRange { uint32_t lower, upper; };
    static const FreqRange ranges[] = {
      { 433000, 433000 }, { 869000, 869000 }, { 918000, 918000 }
    };
    int i = 0;
    out_frame[i++] = RESP_ALLOWED_REPEAT_FREQ;
    for (int k = 0; k < (int)(sizeof(ranges)/sizeof(ranges[0])) && i + 8 < (int)sizeof(out_frame); k++) {
      memcpy(&out_frame[i], &ranges[k].lower, 4); i += 4;
      memcpy(&out_frame[i], &ranges[k].upper, 4); i += 4;
    }
    sendResp(i);

  // ── Custom variables (stub — no SensorManager) ──────────────────
  } else if (cmd_frame[0] == CMD_GET_CUSTOM_VARS) {
    out_frame[0] = RESP_CODE_CUSTOM_VARS;
    sendResp(1);

  } else if (cmd_frame[0] == CMD_SET_CUSTOM_VAR) {
    writeErrFrame(ERR_CODE_UNSUPPORTED_CMD);

  } else {
    writeErrFrame(ERR_CODE_UNSUPPORTED_CMD);
  }
}

// ── Helper frames ────────────────────────────────────────────────

void BleCompanionHandler::writeOKFrame() {
  out_frame[0] = RESP_CODE_OK;
  sendResp(1);
}

void BleCompanionHandler::writeErrFrame(uint8_t err_code) {
  out_frame[0] = RESP_CODE_ERR;
  out_frame[1] = err_code;
  sendResp(2);
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
  // Same layout serves two roles: 0x80+ codes are async pushes, the rest
  // are awaited responses (GET_CONTACTS stream, GET_CONTACT_BY_KEY).
  if (code >= 0x80) pushFrame(out_frame, i);
  else              sendResp(i);
}

// ── Push methods (called from PunkMesh RX handlers on Core 1) ───

void BleCompanionHandler::queueReceivedDM(const ContactInfo& from,
                                           mesh::Packet* pkt,
                                           uint32_t timestamp,
                                           const char* text) {
  String path = _mesh.dmMsgPath(from.name);
  _msg_sync.markDirty(path.c_str());   // track even while app is away
  if (_serial.isConnected()) {
    uint8_t push[1] = { PUSH_CODE_MSG_WAITING };
    pushFrame(push, 1);
  }
}

void BleCompanionHandler::queueReceivedChannelMsg(const mesh::GroupChannel& channel,
                                                    mesh::Packet* pkt,
                                                    uint32_t timestamp,
                                                    const char* text,
                                                    int channel_idx) {
  String path = _mesh.channelMsgPath(channel_idx);
  _msg_sync.markDirty(path.c_str());   // track even while app is away
  if (_serial.isConnected()) {
    uint8_t push[1] = { PUSH_CODE_MSG_WAITING };
    pushFrame(push, 1);
  }
}

void BleCompanionHandler::queueCliResponse(const ContactInfo& from, mesh::Packet* pkt,
                                            uint32_t timestamp, const char* text) {
  if (!_serial.isConnected()) return;
  int i = 0;
  out_frame[i++] = RESP_CODE_CONTACT_MSG_RECV_V3;
  out_frame[i++] = (int8_t)(pkt->getSNR() * 4);
  out_frame[i++] = 0; // reserved
  out_frame[i++] = 0; // reserved
  memcpy(&out_frame[i], from.id.pub_key, 6); i += 6;
  out_frame[i++] = pkt->isRouteFlood() ? pkt->path_len : 0xFF;
  out_frame[i++] = TXT_TYPE_CLI_DATA;
  memcpy(&out_frame[i], &timestamp, 4); i += 4;
  int tlen = strlen(text);
  if (i + tlen > MAX_FRAME_SIZE) tlen = MAX_FRAME_SIZE - i;
  memcpy(&out_frame[i], text, tlen); i += tlen;
  pushFrame(out_frame, i);
}

// Room server post (signed message). Same live frame as queueCliResponse but
// typed TXT_TYPE_SIGNED_PLAIN with the author's 4-byte pubkey prefix between
// the timestamp and the text — matches upstream MyMesh::queueMessage framing.
void BleCompanionHandler::queueReceivedSigned(const ContactInfo& from, mesh::Packet* pkt,
                                               uint32_t timestamp, const uint8_t* sender_prefix,
                                               const char* text) {
  if (!_serial.isConnected()) return;
  int i = 0;
  out_frame[i++] = RESP_CODE_CONTACT_MSG_RECV_V3;
  out_frame[i++] = (int8_t)(pkt->getSNR() * 4);
  out_frame[i++] = 0; // reserved
  out_frame[i++] = 0; // reserved
  memcpy(&out_frame[i], from.id.pub_key, 6); i += 6;
  out_frame[i++] = pkt->isRouteFlood() ? pkt->path_len : 0xFF;
  out_frame[i++] = TXT_TYPE_SIGNED_PLAIN;
  memcpy(&out_frame[i], &timestamp, 4); i += 4;
  memcpy(&out_frame[i], sender_prefix, 4); i += 4;
  int tlen = strlen(text);
  if (i + tlen > MAX_FRAME_SIZE) tlen = MAX_FRAME_SIZE - i;
  memcpy(&out_frame[i], text, tlen); i += tlen;
  pushFrame(out_frame, i);
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
    pushFrame(buf, 1 + PUB_KEY_SIZE);
  }
}

void BleCompanionHandler::pushSendConfirmed(uint32_t ack_crc) {
  if (!_serial.isConnected()) return;

  for (int j = 0; j < EXPECTED_ACK_TABLE_SIZE; j++) {
    if (expected_ack_table[j].ack == ack_crc && expected_ack_table[j].ack != 0) {
      uint32_t trip_time_ms = millis() - expected_ack_table[j].msg_sent;
      // Standard layout: [ack_crc 4][trip_time 4] — the app matches the CRC
      // against the expected_ack it got in RESP_CODE_SENT.
      uint8_t buf[9];
      buf[0] = PUSH_CODE_SEND_CONFIRMED;
      memcpy(&buf[1], &ack_crc, 4);
      memcpy(&buf[5], &trip_time_ms, 4);
      pushFrame(buf, 9);
      expected_ack_table[j].ack = 0;
      return;
    }
  }
}

void BleCompanionHandler::pushPathUpdated(const ContactInfo& contact) {
  if (!_serial.isConnected()) return;

  // Standard layout: full 32-byte pubkey (clients read exactly 32 — the old
  // 8-byte [out_path_len][pubkey_prefix 6] frame under-ran their parsers).
  uint8_t buf[1 + PUB_KEY_SIZE];
  buf[0] = PUSH_CODE_PATH_UPDATED;
  memcpy(&buf[1], contact.id.pub_key, PUB_KEY_SIZE);
  pushFrame(buf, 1 + PUB_KEY_SIZE);
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
  pushFrame(buf, 3 + raw_len);
}

void BleCompanionHandler::pushRawData(mesh::Packet* pkt, float snr, float rssi) {
  if (!_serial.isConnected()) return;
  if (pkt->payload_len + 4 > (int)sizeof(out_frame)) return;

  int i = 0;
  out_frame[i++] = PUSH_CODE_RAW_DATA;
  out_frame[i++] = (int8_t)(snr * 4);
  out_frame[i++] = (int8_t)rssi;
  out_frame[i++] = 0xFF;
  memcpy(&out_frame[i], pkt->payload, pkt->payload_len);
  i += pkt->payload_len;
  pushFrame(out_frame, i);
}

void BleCompanionHandler::pushTraceData(mesh::Packet* pkt, uint32_t tag, uint32_t auth_code,
                                         uint8_t flags, const uint8_t* path_snrs,
                                         const uint8_t* path_hashes, uint8_t path_len) {
  if (!_serial.isConnected()) return;
  uint8_t path_sz = flags & 0x03;
  if (12 + path_len + (path_len >> path_sz) + 1 > sizeof(out_frame)) return;

  int i = 0;
  out_frame[i++] = PUSH_CODE_TRACE_DATA;
  out_frame[i++] = 0;
  out_frame[i++] = path_len;
  out_frame[i++] = flags;
  memcpy(&out_frame[i], &tag, 4); i += 4;
  memcpy(&out_frame[i], &auth_code, 4); i += 4;
  memcpy(&out_frame[i], path_hashes, path_len); i += path_len;
  memcpy(&out_frame[i], path_snrs, path_len >> path_sz); i += (path_len >> path_sz);
  out_frame[i++] = (int8_t)(pkt->getSNR() * 4);
  pushFrame(out_frame, i);
}

void BleCompanionHandler::pushControlData(mesh::Packet* pkt, float snr, float rssi) {
  if (!_serial.isConnected()) return;
  if (pkt->payload_len + 4 > (int)sizeof(out_frame)) return;

  int i = 0;
  out_frame[i++] = PUSH_CODE_CONTROL_DATA;
  out_frame[i++] = (int8_t)(snr * 4);
  out_frame[i++] = (int8_t)rssi;
  out_frame[i++] = pkt->path_len;
  memcpy(&out_frame[i], pkt->payload, pkt->payload_len);
  i += pkt->payload_len;
  pushFrame(out_frame, i);
}

void BleCompanionHandler::pushChannelDataRecv(const mesh::GroupChannel& channel, mesh::Packet* pkt,
                                               uint16_t data_type, const uint8_t* data, size_t data_len) {
  if (!_serial.isConnected()) return;
  if (data_len > MAX_CHANNEL_DATA_LENGTH) return;

  int i = 0;
  out_frame[i++] = RESP_CODE_CHANNEL_DATA_RECV;
  out_frame[i++] = (int8_t)(pkt->getSNR() * 4);
  out_frame[i++] = 0;
  out_frame[i++] = 0;

  uint8_t channel_idx = _mesh.findChannelIdx(channel);
  out_frame[i++] = channel_idx;
  out_frame[i++] = pkt->isRouteFlood() ? pkt->path_len : 0xFF;
  out_frame[i++] = (uint8_t)(data_type & 0xFF);
  out_frame[i++] = (uint8_t)(data_type >> 8);
  out_frame[i++] = (uint8_t)data_len;

  if (data_len > 0) {
    memcpy(&out_frame[i], data, data_len);
    i += data_len;
  }

  pushFrame(out_frame, i);
}

bool BleCompanionHandler::checkPendingDiscovery(ContactInfo& contact, uint8_t* in_path,
                                                 uint8_t in_path_len, uint8_t* out_path,
                                                 uint8_t out_path_len, uint8_t extra_type,
                                                 uint8_t* extra, uint8_t extra_len) {
  if (extra_type != PAYLOAD_TYPE_RESPONSE || extra_len <= 4) return false;

  uint32_t tag;
  memcpy(&tag, extra, 4);
  if (tag != pending_discovery) return false;

  pending_discovery = 0;

  if (!mesh::Packet::isValidPathLen(in_path_len) || !mesh::Packet::isValidPathLen(out_path_len))
    return true;

  int i = 0;
  out_frame[i++] = PUSH_CODE_PATH_DISCOVERY_RESPONSE;
  out_frame[i++] = 0;
  memcpy(&out_frame[i], contact.id.pub_key, 6); i += 6;
  out_frame[i++] = out_path_len;
  i += mesh::Packet::writePath(&out_frame[i], out_path, out_path_len);
  out_frame[i++] = in_path_len;
  i += mesh::Packet::writePath(&out_frame[i], in_path, in_path_len);

  if (_serial.isConnected()) {
    pushFrame(out_frame, i);
  }
  return true;
}

void BleCompanionHandler::pushContactResponse(const ContactInfo& contact, const uint8_t* data, uint8_t len) {
  if (!_serial.isConnected()) return;

  uint32_t tag;
  memcpy(&tag, data, 4);

  if (pending_login && memcmp(&pending_login, contact.id.pub_key, 4) == 0) {
    pending_login = 0;

    int i = 0;
    if (len > 4 && memcmp(&data[4], "OK", 2) == 0) {
      out_frame[i++] = PUSH_CODE_LOGIN_SUCCESS;
      out_frame[i++] = 0;
      memcpy(&out_frame[i], contact.id.pub_key, 6); i += 6;
    } else if (len > 4 && data[4] == 0) {
      uint16_t keep_alive_secs = ((uint16_t)data[5]) * 16;
      if (keep_alive_secs > 0) {
        _mesh.startConnectionToContact(contact, keep_alive_secs);
      }
      out_frame[i++] = PUSH_CODE_LOGIN_SUCCESS;
      out_frame[i++] = data[6];
      memcpy(&out_frame[i], contact.id.pub_key, 6); i += 6;
      memcpy(&out_frame[i], &tag, 4); i += 4;
      out_frame[i++] = data[7];
      out_frame[i++] = (len > 12) ? data[12] : 0;
    } else {
      out_frame[i++] = PUSH_CODE_LOGIN_FAIL;
      out_frame[i++] = 0;
      memcpy(&out_frame[i], contact.id.pub_key, 6); i += 6;
    }
    pushFrame(out_frame, i);

  } else if (len > 4 && pending_status && memcmp(&pending_status, contact.id.pub_key, 4) == 0) {
    pending_status = 0;

    int i = 0;
    out_frame[i++] = PUSH_CODE_STATUS_RESPONSE;
    out_frame[i++] = 0;
    memcpy(&out_frame[i], contact.id.pub_key, 6); i += 6;
    memcpy(&out_frame[i], &data[4], len - 4); i += (len - 4);
    pushFrame(out_frame, i);

  } else if (len > 4 && tag == pending_telemetry) {
    pending_telemetry = 0;

    int i = 0;
    out_frame[i++] = PUSH_CODE_TELEMETRY_RESPONSE;
    out_frame[i++] = 0;
    memcpy(&out_frame[i], contact.id.pub_key, 6); i += 6;
    memcpy(&out_frame[i], &data[4], len - 4); i += (len - 4);
    pushFrame(out_frame, i);

  } else if (len > 4 && tag == pending_req) {
    pending_req = 0;

    int i = 0;
    out_frame[i++] = PUSH_CODE_BINARY_RESPONSE;
    out_frame[i++] = 0;
    memcpy(&out_frame[i], &tag, 4); i += 4;
    memcpy(&out_frame[i], &data[4], len - 4); i += (len - 4);
    pushFrame(out_frame, i);
  }
}

#endif // BLE_COMPANION_ENABLED
