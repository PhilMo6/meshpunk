#ifndef PUNKMESH_H
#define PUNKMESH_H

#include <Arduino.h>
#include <Mesh.h>
#include <helpers/BaseChatMesh.h>
#include <helpers/SimpleMeshTables.h>
#include <helpers/StaticPoolPacketManager.h>
#include <helpers/ArduinoHelpers.h>
#include <helpers/IdentityStore.h>
#include <RTClib.h>
#include <FS.h>

extern "C" {
#include <lua.h>
#include <lualib.h>
#include <luavgl.h>
}

// Forward declarations
// class PunkMesh;

// NodePrefs is shared between Lua + C++ config
struct NodePrefs
{
  float airtime_factor;
  char node_name[32];
  double node_lat, node_lon;
  float freq;
  uint8_t tx_power_dbm;
  uint8_t unused[3];
  // v2 fields — old prefs files are shorter, so these keep constructor defaults
  float bandwidth;
  uint8_t spreading_factor;
  uint8_t coding_rate;
  uint8_t contact_overwrite;
  uint8_t rx_boost;
  uint32_t ble_pin;
  uint8_t path_hash_mode;
  uint8_t autoadd_config;
  uint8_t autoadd_max_hops;
  char default_scope_name[31];
  uint8_t default_scope_key[16];
  uint8_t msg_repeat_enabled;
  uint8_t msg_repeat_max;
  uint8_t msg_repeat_interval_secs;
};

struct MeshMessage {
    char from[32];     // short pubkey or addr
    char text[128];    // payload
    uint32_t timestamp;
    uint8_t hops;
    bool direct;
};

#define MAX_MESSAGES 64   // tweak as needed

// ── Per-message multi-path tracking ─────────────────────────────
#define MAX_MSG_PATH_ENTRIES 32
#define MAX_PATHS_PER_MSG    8

struct ObservedPath {
  uint16_t path_len;
  uint8_t  path[MAX_PATH_SIZE];
  float    snr;
  float    rssi;
  bool     is_direct;
};

struct MsgPathEntry {
  uint8_t       pkt_hash[MAX_HASH_SIZE];
  uint8_t       path_count;
  bool          is_message;
  bool          is_dm;
  int8_t        channel_idx;
  char          peer[32];
  ObservedPath  paths[MAX_PATHS_PER_MSG];
};

// In-memory message record. Persisted as human-readable key=value text
// (see append_msg_text / read_msg_text_file in punkmesh.cpp).
// Channel logs: <storage_prefix>/messages/ch_<idx>.log
// DM logs:      <storage_prefix>/messages/dm_<sanitized_peer>.log
struct StoredMsg {
  uint32_t timestamp;
  float    snr;
  float    rssi;
  int8_t   channel_idx;  // -1 for DM, 0..N for channel slot
  uint8_t  hops;
  uint8_t  flags;        // bit0=direct, bit1=is_dm
  char     from[32];
  char     peer[32];     // for DMs, the OTHER party (thread key); empty for channels
  char     text[160];
  uint16_t path_len;     // encoded: upper bits = hash size, lower 6 = hop count
  uint8_t  path[MAX_PATH_SIZE];
  uint8_t  pkt_hash[MAX_HASH_SIZE];
  bool     has_hash;
  uint8_t  sender_pub_key[6];
  bool     has_pub_key;
  uint8_t  rpath_count;
  ObservedPath rpaths[MAX_PATHS_PER_MSG];
};

// ── Per-contact path history (in-memory, managed on radio core) ──
#define MAX_PATH_RECORDS   8
#define MAX_PATH_CONTACTS  32

#define PATH_SRC_MSG_RX      0
#define PATH_SRC_ACK         1
#define PATH_SRC_PATH_UPDATE 2
#define PATH_SRC_ADVERT      3

struct PathRecord {
  uint16_t path_len;
  uint8_t  path[MAX_PATH_SIZE];
  uint32_t timestamp;
  uint32_t trip_time_ms;
  float    snr;
  float    rssi;
  uint16_t success_count;
  uint16_t failure_count;
  uint8_t  source;       // PATH_SRC_*
  bool     is_direct;
};

struct ContactPathHistory {
  uint8_t  pub_key[PUB_KEY_SIZE];
  uint8_t  count;
  PathRecord records[MAX_PATH_RECORDS];
};

#define MAX_PENDING_REPEATS 4
#define MAX_REPEAT_HISTORY  8

struct PendingRepeat {
  uint8_t header;
  uint8_t payload[MAX_PACKET_PAYLOAD];
  uint16_t payload_len;
  uint8_t pkt_hash[MAX_HASH_SIZE];
  uint8_t attempts_remaining;
  unsigned long next_retry_time;
  bool active;
};

struct RepeatOutcome {
  uint8_t pkt_hash[MAX_HASH_SIZE];
  uint8_t status; // 2=confirmed, 3=exhausted
};

// Class declaration
class PunkMesh : public BaseChatMesh, ContactVisitor
{
public:
  NodePrefs _prefs;
  uint32_t expected_ack_crc;
  ChannelDetails *_public;
  unsigned long last_msg_sent;
  ContactInfo *curr_recipient;
  char command[512 + 10];
  uint8_t tmp_buf[256];
  char hex_buf[512];
  lua_State *lua_runtime = NULL;

  // Last received packet radio info (updated in logRx)
  float last_rx_snr = 0;
  float last_rx_rssi = 0;

  // Persistent storage filesystem (SD card if available, else LittleFS)
  fs::FS* _storage = nullptr;
  String _storage_prefix = ""; // e.g. "/meshpunk" for SD subdirectory

  void setStorage(fs::FS* fs, const char* prefix = "");

  // Message history (moved from file-scope statics)
  MeshMessage message_history[MAX_MESSAGES];
  int msg_head = 0;
  int msg_count = 0;

  PunkMesh(mesh::Radio &radio, StdRNG &rng, mesh::RTCClock &rtc, SimpleMeshTables &tables);

  void begin();
  void loop();
  void handleCommand(const char *command);
  void sendSelfAdvert(int delay_millis);
  void showWelcome();
  void savePrefs();
  const char *getTypeName(uint8_t type) const;
  void loadContacts();
  void saveContacts();
  void loadChannels();
  void saveChannels();

  // ── Unified send + persist helpers ──────────────────────────
  struct SendResult {
    int code;              // MSG_SEND_FAILED / MSG_SEND_SENT_FLOOD / MSG_SEND_SENT_DIRECT
    uint32_t expected_ack;
    uint32_t est_timeout;
    uint8_t tx_hash[MAX_HASH_SIZE];
    bool has_hash;
  };

  SendResult sendAndPersistDM(ContactInfo& recipient, uint32_t timestamp,
                              uint8_t attempt, const char* text);
  bool sendAndPersistChannelMsg(int channel_idx, uint32_t timestamp,
                                const char* text, int tlen,
                                uint8_t* out_hash = nullptr);

  // ── Persistent message history ───────────────────────────────
  // Max records kept per file (channel or DM). Compaction fires at
  // cap + 100 records and trims back to cap.
  int _max_messages = 400;
  void setMaxMessages(int n);

  // Write paths (called from RX handlers and Lua send bindings).
  // `channel_idx < 0` on appendChannelMessage is a no-op (unknown channel).
  void appendChannelMessage(int channel_idx, const char* from, const char* text,
                            uint32_t timestamp, float snr, float rssi,
                            uint8_t hops, bool direct,
                            uint16_t path_len = 0, const uint8_t* path = nullptr,
                            const uint8_t* pkt_hash = nullptr);
  void appendDMMessage(const char* peer, const char* from, const char* text,
                       uint32_t timestamp, float snr, float rssi,
                       uint8_t hops, bool direct,
                       uint16_t path_len = 0, const uint8_t* path = nullptr,
                       const uint8_t* pkt_hash = nullptr,
                       const uint8_t* sender_pub_key = nullptr);

  // ── Per-contact path history ────────────────────────────────────
  ContactPathHistory _path_history[MAX_PATH_CONTACTS];
  int _path_history_count = 0;

  ContactPathHistory* findOrCreatePathHistory(const uint8_t* pub_key);
  void recordPath(const uint8_t* pub_key, uint16_t path_len, const uint8_t* path,
                  float snr, float rssi, uint8_t source, bool is_direct);
  void recordPathSuccess(const uint8_t* pub_key, uint32_t trip_time_ms);
  void recordPathFailure(const uint8_t* pub_key);

  // ── Per-message multi-path tracking ─────────────────────────────
  uint8_t       _last_pkt_hash[MAX_HASH_SIZE];
  uint8_t       _last_tx_hash[MAX_HASH_SIZE];   // hash of last packet sent via sendFloodScoped
  MsgPathEntry  _msg_paths[MAX_MSG_PATH_ENTRIES];
  int           _msg_path_next  = 0;
  int           _msg_path_count = 0;

  MsgPathEntry* findMsgPaths(const uint8_t* hash);
  MsgPathEntry* recordMsgPath(const uint8_t* hash, uint16_t path_len,
                              const uint8_t* path, float snr, float rssi,
                              bool is_direct);
  void persistExtraPath(const uint8_t* hash, const ObservedPath& op);
  void preRegisterSentHash(const uint8_t* hash, bool is_dm, int8_t channel_idx, const char* peer);

  // ── Message repeat (retransmit until echo heard) ────────────────
  PendingRepeat  _pending_repeats[MAX_PENDING_REPEATS];
  RepeatOutcome  _repeat_history[MAX_REPEAT_HISTORY];
  int            _repeat_history_next = 0;

  void registerPendingRepeat(const uint8_t* hash, uint8_t header,
                             const uint8_t* payload, uint16_t payload_len);
  void checkPendingRepeats();
  int  getRepeatStatus(const uint8_t* hash);

  // Read paths. Each pushes a Lua table (array of message tables) and
  // returns 1 (the number of Lua stack values). Safe to call even if
  // the file doesn't exist — you get an empty table.
  int pushChannelMessagesToLua(lua_State* L, int channel_idx);
  int pushDMMessagesToLua(lua_State* L, const char* peer);
  int pushDMThreadNamesToLua(lua_State* L);
  int lookupPersistedPaths(lua_State* L, const char* hash_hex, int channel_idx, const char* peer);

  bool hasConnectionToContact(const uint8_t* pub_key) { return hasConnectionTo(pub_key); }
  void stopConnectionToContact(const uint8_t* pub_key) { stopConnection(pub_key); }
  bool startConnectionToContact(const ContactInfo& contact, uint16_t keep_alive_secs) { return startConnection(contact, keep_alive_secs); }
  const uint8_t* getPrivateKey() const { return ((const uint8_t*)&self_id) + PUB_KEY_SIZE; }
  bool saveIdentity();

  // BLE sync: enumerate message files and read records.
  static const int MAX_SYNC_FILES = 40;
  static const int MAX_SYNC_PATH_LEN = 64;
  int enumerateMessageFiles(char paths[][MAX_SYNC_PATH_LEN], int max_paths);
  static int readOneStoredMsg(fs::FS* storage, const char* path,
                              size_t offset, StoredMsg& m);
  int readAllStoredMsgs(const char* path, StoredMsg* out, int max_count);
  int readStoredMsgsSince(const char* path, uint32_t since, StoredMsg* out, int max_count);

  // Path helpers (used by BLE companion for targeted sync)
  String channelMsgPath(int channel_idx);
  String dmMsgPath(const char* peer);

  void setClock(uint32_t timestamp);
  void importCard(const char *command);

  float getFreqPref() const;
  uint8_t getTxPowerPref() const;
  float getBandwidthPref() const;
  uint8_t getSpreadingFactorPref() const;
  uint8_t getCodingRatePref() const;

  // Stats wrappers for BLE companion
  int getRadioNoiseFloor() const { return _radio->getNoiseFloor(); }
  uint16_t getErrorFlags() const { return _err_flags; }
  uint8_t getQueueLength() const { return (uint8_t)_mgr->getOutboundTotal(); }

  bool shouldOverwriteWhenFull() const override { return _prefs.contact_overwrite != 0; }
  void clearContacts() { resetContacts(); }

protected:
  void sendFloodScoped(const ContactInfo& recipient, mesh::Packet* pkt, uint32_t delay_millis=0) override;
  void sendFloodScoped(const mesh::GroupChannel& channel, mesh::Packet* pkt, uint32_t delay_millis=0) override;
  void logRx(mesh::Packet *pkt, int len, float score) override;
  float getAirtimeBudgetFactor() const override;
  int calcRxDelay(float score, uint32_t air_time) const override;
  bool allowPacketForward(const mesh::Packet *packet) override;
  void onDiscoveredContact(ContactInfo &contact, bool is_new, uint8_t path_len, const uint8_t *path) override;
  void onContactPathUpdated(const ContactInfo &contact) override;
  ContactInfo* processAck(const uint8_t *data) override;
  void onMessageRecv(const ContactInfo &from, mesh::Packet *pkt, uint32_t sender_timestamp, const char *text) override;
  void onCommandDataRecv(const ContactInfo &from, mesh::Packet *pkt, uint32_t sender_timestamp, const char *text) override;
  void onSignedMessageRecv(const ContactInfo &from, mesh::Packet *pkt, uint32_t sender_timestamp, const uint8_t *sender_prefix, const char *text) override;
  void onChannelMessageRecv(const mesh::GroupChannel &channel, mesh::Packet *pkt, uint32_t timestamp, const char *text) override;
  uint8_t onContactRequest(const ContactInfo &contact, uint32_t sender_timestamp, const uint8_t *data, uint8_t len, uint8_t *reply) override;
  void onContactResponse(const ContactInfo &contact, const uint8_t *data, uint8_t len) override;
  void onSendTimeout() override;
  uint32_t calcFloodTimeoutMillisFor(uint32_t pkt_airtime_millis) const override;
  uint32_t calcDirectTimeoutMillisFor(uint32_t pkt_airtime_millis, uint8_t path_len) const override;

  void onControlDataRecv(mesh::Packet* packet) override;
  void onRawDataRecv(mesh::Packet* packet) override;
  void onTraceRecv(mesh::Packet* packet, uint32_t tag, uint32_t auth_code, uint8_t flags,
                   const uint8_t* path_snrs, const uint8_t* path_hashes, uint8_t path_len) override;
  void onChannelDataRecv(const mesh::GroupChannel& channel, mesh::Packet* pkt, uint16_t data_type,
                         const uint8_t* data, size_t data_len) override;
  bool onContactPathRecv(ContactInfo& contact, uint8_t* in_path, uint8_t in_path_len,
                         uint8_t* out_path, uint8_t out_path_len, uint8_t extra_type,
                         uint8_t* extra, uint8_t extra_len) override;

  void onContactVisit(const ContactInfo &contact) override;

  // Punk stuff
  void store_message(const char* from, const char* text, uint32_t timestamp, uint8_t hops, bool direct);

};

// Optional: declare global instance if using one
// extern MyMesh the_mesh;

// ─── Text utilities ──────────────────────────────────────────────
// Copy `in` into `out` while replacing UTF-8 smart-quote sequences with
// their ASCII equivalents so they render from lv_font_montserrat_14
// (which otherwise draws tofu for U+2018/U+2019/U+201C/U+201D):
//   U+2018 / U+2019 (E2 80 98 / 99) → '
//   U+201C / U+201D (E2 80 9C / 9D) → "
// Always NUL-terminates `out`. Safe if `in == nullptr`. Returns the
// output byte length (excluding NUL).
size_t normalize_smart_quotes(const char* in, char* out, size_t outlen);

#endif