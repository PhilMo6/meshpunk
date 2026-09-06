#pragma once

#if BLE_COMPANION_ENABLED

#include <Arduino.h>
#include <Mesh.h>
#include <helpers/BaseChatMesh.h>
#include <helpers/BaseSerialInterface.h>
// MC-PKG divergence: no concrete NimBLE serial here (the esp32/
// SerialBLEInterface include) — the Base abstraction only.
#include <helpers/AdvertDataHelpers.h>
#include <helpers/TxtDataHelpers.h>

// Forward declarations
class PunkMesh;

// ── Companion protocol version ───────────────────────────────────
// FW_VER_CODE / FW_VERSION / FW_BUILD_DATE live in version.h (single
// bump site, shared with the Lua _FW_API/_FW_VERSION globals).
#include "version.h"
#include "boards/board_pins.h"   // MESHPUNK_BOARD_LABEL
#define MESHPUNK_MODEL_NAME      "MeshPunk " MESHPUNK_BOARD_LABEL

// ── Command codes (app → device) ─────────────────────────────────
#define CMD_APP_START                 1
#define CMD_SEND_TXT_MSG              2
#define CMD_SEND_CHANNEL_TXT_MSG      3
#define CMD_GET_CONTACTS              4
#define CMD_GET_DEVICE_TIME           5
#define CMD_SET_DEVICE_TIME           6
#define CMD_SEND_SELF_ADVERT          7
#define CMD_SET_ADVERT_NAME           8
#define CMD_ADD_UPDATE_CONTACT        9
#define CMD_SYNC_NEXT_MESSAGE         10
#define CMD_SET_RADIO_PARAMS          11
#define CMD_SET_RADIO_TX_POWER        12
#define CMD_RESET_PATH                13
#define CMD_SET_ADVERT_LATLON         14
#define CMD_REMOVE_CONTACT            15
#define CMD_SHARE_CONTACT             16
#define CMD_EXPORT_CONTACT            17
#define CMD_IMPORT_CONTACT            18
#define CMD_REBOOT                    19
#define CMD_GET_BATT_AND_STORAGE      20
#define CMD_SET_TUNING_PARAMS         21
#define CMD_DEVICE_QEURY              22
#define CMD_EXPORT_PRIVATE_KEY        23
#define CMD_IMPORT_PRIVATE_KEY        24
#define CMD_SEND_RAW_DATA             25
#define CMD_SEND_LOGIN                26
#define CMD_SEND_STATUS_REQ           27
#define CMD_HAS_CONNECTION            28
#define CMD_LOGOUT                    29
#define CMD_GET_CONTACT_BY_KEY        30
#define CMD_GET_CHANNEL               31
#define CMD_SET_CHANNEL               32
#define CMD_SIGN_START                33
#define CMD_SIGN_DATA                 34
#define CMD_SIGN_FINISH               35
#define CMD_SEND_TRACE_PATH           36
#define CMD_SET_DEVICE_PIN            37
#define CMD_SET_OTHER_PARAMS          38
#define CMD_SEND_TELEMETRY_REQ        39
#define CMD_GET_CUSTOM_VARS           40
#define CMD_SET_CUSTOM_VAR            41
#define CMD_GET_ADVERT_PATH           42
#define CMD_GET_TUNING_PARAMS         43
#define CMD_SEND_BINARY_REQ           50
#define CMD_FACTORY_RESET             51
#define CMD_SEND_PATH_DISCOVERY_REQ   52
#define CMD_SET_FLOOD_SCOPE_KEY       54
#define CMD_SEND_CONTROL_DATA         55
#define CMD_GET_STATS                 56
#define CMD_SEND_ANON_REQ             57
#define CMD_SET_AUTOADD_CONFIG        58
#define CMD_GET_AUTOADD_CONFIG        59
#define CMD_GET_ALLOWED_REPEAT_FREQ   60
#define CMD_SET_PATH_HASH_MODE        61
#define CMD_SEND_CHANNEL_DATA         62
#define CMD_SET_DEFAULT_FLOOD_SCOPE   63
#define CMD_GET_DEFAULT_FLOOD_SCOPE   64
#define CMD_GENERATE_IDENTITY         70

// ── Response codes (device → app) ─────────���──────────────────────
#define RESP_CODE_OK                  0
#define RESP_CODE_ERR                 1
#define RESP_CODE_CONTACTS_START      2
#define RESP_CODE_CONTACT             3
#define RESP_CODE_END_OF_CONTACTS     4
#define RESP_CODE_SELF_INFO           5
#define RESP_CODE_SENT                6
#define RESP_CODE_CONTACT_MSG_RECV_V3 16
#define RESP_CODE_CHANNEL_MSG_RECV_V3 17
#define RESP_CODE_CHANNEL_INFO        18
#define RESP_CODE_CURR_TIME           9
#define RESP_CODE_NO_MORE_MESSAGES    10
#define RESP_CODE_EXPORT_CONTACT      11
#define RESP_CODE_BATT_AND_STORAGE    12
#define RESP_CODE_DEVICE_INFO         13
#define RESP_CODE_PRIVATE_KEY         14
#define RESP_CODE_DISABLED            15
#define RESP_CODE_SIGN_START              19
#define RESP_CODE_SIGNATURE               20
#define RESP_CODE_CUSTOM_VARS             21
#define RESP_CODE_ADVERT_PATH             22
#define RESP_CODE_TUNING_PARAMS           23
#define RESP_CODE_STATS                   24
#define RESP_CODE_AUTOADD_CONFIG          25
#define RESP_ALLOWED_REPEAT_FREQ          26
#define RESP_CODE_CHANNEL_DATA_RECV       27
#define RESP_CODE_DEFAULT_FLOOD_SCOPE     28

// ── Push codes (device → app, async) ─────────────────────────────
#define PUSH_CODE_ADVERT              0x80
#define PUSH_CODE_PATH_UPDATED        0x81
#define PUSH_CODE_SEND_CONFIRMED      0x82
#define PUSH_CODE_MSG_WAITING         0x83
#define PUSH_CODE_LOGIN_SUCCESS       0x85
#define PUSH_CODE_LOGIN_FAIL          0x86
#define PUSH_CODE_STATUS_RESPONSE     0x87
#define PUSH_CODE_RAW_DATA                0x84
#define PUSH_CODE_LOG_RX_DATA             0x88
#define PUSH_CODE_TRACE_DATA              0x89
#define PUSH_CODE_NEW_ADVERT              0x8A
#define PUSH_CODE_TELEMETRY_RESPONSE      0x8B
#define PUSH_CODE_BINARY_RESPONSE         0x8C
#define PUSH_CODE_PATH_DISCOVERY_RESPONSE 0x8D
#define PUSH_CODE_CONTROL_DATA            0x8E

// ── Error codes ──────────────────────────────────────────────────
#define ERR_CODE_UNSUPPORTED_CMD      1
#define ERR_CODE_NOT_FOUND            2
#define ERR_CODE_TABLE_FULL           3
#define ERR_CODE_BAD_STATE            4
#define ERR_CODE_FILE_IO_ERROR        5
#define ERR_CODE_ILLEGAL_ARG          6

// ── Stats sub-types ─────────────────────────────────────────────
#define STATS_TYPE_CORE    0
#define STATS_TYPE_RADIO   1
#define STATS_TYPE_PACKETS 2

// ── Signing ─────────────────────────────────────────────────────
#define MAX_SIGN_DATA_LEN  (8 * 1024)

// ── Channel data ────────────────────────────────────────────────
#define MAX_CHANNEL_DATA_LENGTH  (MAX_FRAME_SIZE - 9)

// ── Telemetry request type ──────────────────────────────────────
#define REQ_TYPE_GET_TELEMETRY_DATA  0x03

// ── ACK tracking ─────────────────────────────────────────────────
#define EXPECTED_ACK_TABLE_SIZE       8

struct AckTableEntry {
  unsigned long msg_sent;
  uint32_t ack;
};

// ── Message sync ─────────────────────────────────────────────────
// Per-file ledger engine (see ble_msg_sync.h). No full-store scans: the RX
// path marks dirty files, commands read only those, idle polls open nothing.
#include "ble_msg_sync.h"

// MC-PKG divergence: the RX-push surface PunkMesh dispatches into, as PURE
// VIRTUALS. The LoRa elf calls only through this vtable — it never imports
// the BLE elf's code (load order: LoRa first, BLE optional and later). The
// concrete handler lives in the companion elf and registers itself via
// ble_companion (the LoRa elf owns that global).
class BleCompanionIface {
public:
  virtual ~BleCompanionIface() {}
  virtual void loop() = 0;
  virtual void queueReceivedDM(const ContactInfo& from, mesh::Packet* pkt,
                        uint32_t timestamp, const char* text) = 0;
  virtual void queueReceivedChannelMsg(const mesh::GroupChannel& channel, mesh::Packet* pkt,
                                uint32_t timestamp, const char* text, int channel_idx) = 0;
  virtual void queueCliResponse(const ContactInfo& from, mesh::Packet* pkt,
                         uint32_t timestamp, const char* text) = 0;
  virtual void queueReceivedSigned(const ContactInfo& from, mesh::Packet* pkt,
                            uint32_t timestamp, const uint8_t* sender_prefix,
                            const char* text) = 0;
  virtual void pushAdvert(const ContactInfo& contact, bool is_new, uint8_t path_len, const uint8_t* path) = 0;
  virtual void pushSendConfirmed(uint32_t ack_crc) = 0;
  virtual void pushPathUpdated(const ContactInfo& contact) = 0;
  virtual void pushLogRxData(mesh::Packet* pkt, float snr, float rssi) = 0;
  virtual void pushContactResponse(const ContactInfo& contact, const uint8_t* data, uint8_t len) = 0;
  virtual void pushRawData(mesh::Packet* pkt, float snr, float rssi) = 0;
  virtual void pushTraceData(mesh::Packet* pkt, uint32_t tag, uint32_t auth_code, uint8_t flags,
                     const uint8_t* path_snrs, const uint8_t* path_hashes, uint8_t path_len) = 0;
  virtual void pushControlData(mesh::Packet* pkt, float snr, float rssi) = 0;
  virtual void pushChannelDataRecv(const mesh::GroupChannel& channel, mesh::Packet* pkt,
                           uint16_t data_type, const uint8_t* data, size_t data_len) = 0;
  virtual bool checkPendingDiscovery(ContactInfo& contact, uint8_t* in_path, uint8_t in_path_len,
                             uint8_t* out_path, uint8_t out_path_len,
                             uint8_t extra_type, uint8_t* extra, uint8_t extra_len) = 0;
};

class BleCompanionHandler : public BleCompanionIface {
public:
  // MC-PKG divergence: the ABSTRACT serial (the handler only ever used the
  // Base surface); the concrete NimBLE class stays firmware behind the
  // BLE-slot transport.
  BleCompanionHandler(PunkMesh& mesh, BaseSerialInterface& serial);
  ~BleCompanionHandler();

  void loop();

  // Called from PunkMesh RX handlers to queue messages for the companion app
  void queueReceivedDM(const ContactInfo& from, mesh::Packet* pkt,
                        uint32_t timestamp, const char* text);
  void queueReceivedChannelMsg(const mesh::GroupChannel& channel, mesh::Packet* pkt,
                                uint32_t timestamp, const char* text, int channel_idx);
  void queueCliResponse(const ContactInfo& from, mesh::Packet* pkt,
                         uint32_t timestamp, const char* text);
  void queueReceivedSigned(const ContactInfo& from, mesh::Packet* pkt,
                            uint32_t timestamp, const uint8_t* sender_prefix,
                            const char* text);
  void pushAdvert(const ContactInfo& contact, bool is_new, uint8_t path_len, const uint8_t* path);
  // Called for EVERY received ack (self-filters against expected_ack_table
  // and computes the round-trip from the entry's own send time).
  void pushSendConfirmed(uint32_t ack_crc);
  void pushPathUpdated(const ContactInfo& contact);
  void pushLogRxData(mesh::Packet* pkt, float snr, float rssi);
  void pushContactResponse(const ContactInfo& contact, const uint8_t* data, uint8_t len);
  void pushRawData(mesh::Packet* pkt, float snr, float rssi);
  void pushTraceData(mesh::Packet* pkt, uint32_t tag, uint32_t auth_code, uint8_t flags,
                     const uint8_t* path_snrs, const uint8_t* path_hashes, uint8_t path_len);
  void pushControlData(mesh::Packet* pkt, float snr, float rssi);
  void pushChannelDataRecv(const mesh::GroupChannel& channel, mesh::Packet* pkt,
                           uint16_t data_type, const uint8_t* data, size_t data_len);
  bool checkPendingDiscovery(ContactInfo& contact, uint8_t* in_path, uint8_t in_path_len,
                             uint8_t* out_path, uint8_t out_path_len,
                             uint8_t extra_type, uint8_t* extra, uint8_t extra_len);

  bool isConnected() const { return _serial.isConnected(); }

private:
  void handleCmdFrame(size_t len);

  void writeOKFrame();
  void writeErrFrame(uint8_t err_code);
  void writeContactRespFrame(uint8_t code, const ContactInfo& contact);

  // Responses (replies the app is awaiting) get a single retry slot — the
  // protocol is strictly sequential, so one slot suffices. Pushes are
  // best-effort and yield to a pending response.
  bool sendResp(size_t len);                        // sends out_frame[0..len)
  bool pushFrame(const uint8_t* frame, size_t len);

  PunkMesh& _mesh;
  BaseSerialInterface& _serial;   // MC-PKG divergence (see ctor)

  ContactsIterator _iter;
  uint32_t _iter_filter_since;
  uint32_t _most_recent_lastmod;
  bool _iter_started;
  uint8_t app_target_ver;

  uint8_t cmd_frame[MAX_FRAME_SIZE + 1];
  uint8_t out_frame[MAX_FRAME_SIZE + 1];

  BleMsgSync _msg_sync;

  uint8_t _resp_retry[MAX_FRAME_SIZE];
  size_t  _resp_retry_len;

  AckTableEntry expected_ack_table[EXPECTED_ACK_TABLE_SIZE];
  int next_ack_idx;

  uint32_t pending_login;
  uint32_t pending_status;
  uint32_t pending_telemetry;
  uint32_t pending_discovery;
  uint32_t pending_req;

  uint8_t* sign_data;
  uint32_t sign_data_len;

  void clearPendingReqs() { pending_login = pending_status = pending_telemetry = pending_discovery = pending_req = 0; }
};

// MC-PKG divergence: package lifecycle — mc_bleproto.cpp owns the transport;
// these only construct/destroy the handler over the serial it is handed.
void ble_companion_attach(PunkMesh& mesh, BaseSerialInterface& serial);
void ble_companion_detach();

// Owned by the LORA elf (mcmain.cpp): PunkMesh dispatches into it through
// the vtable; the companion elf's attach/detach set and clear it.
extern BleCompanionIface* ble_companion;

#endif // BLE_COMPANION_ENABLED
