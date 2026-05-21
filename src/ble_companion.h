#pragma once

#if BLE_COMPANION_ENABLED

#include <Arduino.h>
#include <Mesh.h>
#include <helpers/BaseChatMesh.h>
#include <helpers/BaseSerialInterface.h>
#include <helpers/esp32/SerialBLEInterface.h>
#include <helpers/AdvertDataHelpers.h>
#include <helpers/TxtDataHelpers.h>

// Forward declarations
class PunkMesh;
struct StoredMsg;

// ── Companion protocol version ───────────────────────────────────
#define MESHPUNK_FW_VER_CODE     11
#define MESHPUNK_FW_VERSION      "v1.15.0"
#define MESHPUNK_FW_BUILD_DATE   "16 May 2026"
#define MESHPUNK_MODEL_NAME      "MeshPunk T-Deck"

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
#define CMD_SEND_TRACE_PATH           36
#define CMD_SET_DEVICE_PIN            37
#define CMD_SET_OTHER_PARAMS          38
#define CMD_GET_ADVERT_PATH           42
#define CMD_GET_TUNING_PARAMS         43
#define CMD_FACTORY_RESET             51
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
#define RESP_CODE_ADVERT_PATH         22
#define RESP_CODE_TUNING_PARAMS       23

// ── Push codes (device → app, async) ─────────────────────────────
#define PUSH_CODE_ADVERT              0x80
#define PUSH_CODE_PATH_UPDATED        0x81
#define PUSH_CODE_SEND_CONFIRMED      0x82
#define PUSH_CODE_MSG_WAITING         0x83
#define PUSH_CODE_LOG_RX_DATA         0x88
#define PUSH_CODE_NEW_ADVERT          0x8A

// ── Error codes ──────────────────────────────────────────────────
#define ERR_CODE_UNSUPPORTED_CMD      1
#define ERR_CODE_NOT_FOUND            2
#define ERR_CODE_TABLE_FULL           3
#define ERR_CODE_BAD_STATE            4
#define ERR_CODE_FILE_IO_ERROR        5
#define ERR_CODE_ILLEGAL_ARG          6

// ── ACK tracking ─────────────────────────────────────────────────
#define EXPECTED_ACK_TABLE_SIZE       8

struct AckTableEntry {
  unsigned long msg_sent;
  uint32_t ack;
  ContactInfo* contact;
};

// ── Disk-based message sync state ────────────────────────────────
struct SyncFrame {
  uint8_t len;
  uint8_t data[MAX_FRAME_SIZE];
};

struct MsgSyncState {
  static const int MAX_FILES = 40;
  static const int MAX_FRAMES = 200;
  static const int MAX_PATH_LEN = 64;
  char files[MAX_FILES][MAX_PATH_LEN];
  int file_count;
  int current_file;
  uint32_t since;
  uint32_t most_recent_ts;
  bool active;
  SyncFrame* frames;
  int frame_count;
  int frame_idx;
};

class BleCompanionHandler {
public:
  BleCompanionHandler(PunkMesh& mesh, SerialBLEInterface& serial);
  ~BleCompanionHandler();

  void loop();

  // Called from PunkMesh RX handlers to queue messages for the companion app
  void queueReceivedDM(const ContactInfo& from, mesh::Packet* pkt,
                        uint32_t timestamp, const char* text);
  void queueReceivedChannelMsg(const mesh::GroupChannel& channel, mesh::Packet* pkt,
                                uint32_t timestamp, const char* text, int channel_idx);
  void pushAdvert(const ContactInfo& contact, bool is_new, uint8_t path_len, const uint8_t* path);
  void pushSendConfirmed(uint32_t ack_crc, uint32_t trip_time_ms);
  void pushPathUpdated(const ContactInfo& contact);
  void pushLogRxData(mesh::Packet* pkt, float snr, float rssi);

  bool isConnected() const { return _serial.isConnected(); }

private:
  void handleCmdFrame(size_t len);

  void writeOKFrame();
  void writeErrFrame(uint8_t err_code);
  void writeContactRespFrame(uint8_t code, const ContactInfo& contact);

  int buildSyncFrame(const StoredMsg& m, uint8_t* frame);
  bool loadNextFileFrames();
  void freeSyncFrames();

  PunkMesh& _mesh;
  SerialBLEInterface& _serial;

  ContactsIterator _iter;
  uint32_t _iter_filter_since;
  uint32_t _most_recent_lastmod;
  bool _iter_started;
  uint8_t app_target_ver;

  uint8_t cmd_frame[MAX_FRAME_SIZE + 1];
  uint8_t out_frame[MAX_FRAME_SIZE + 1];

  MsgSyncState _msg_sync;

  AckTableEntry expected_ack_table[EXPECTED_ACK_TABLE_SIZE];
  int next_ack_idx;
};

// Global lifecycle functions
void ble_companion_init_early();       // call early in setup() before LVGL/Lua
void ble_companion_start(PunkMesh& mesh); // call after the_mesh->begin()
void ble_companion_stop();

extern BleCompanionHandler* ble_companion;
extern SerialBLEInterface*  ble_serial;

#endif // BLE_COMPANION_ENABLED
