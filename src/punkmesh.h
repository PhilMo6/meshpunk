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
  // Advert location-sharing policy (MeshCore companion "advert_loc_policy"):
  // 0 = omit GPS location from self-adverts, 1 = share it. Default 0 (privacy).
  uint8_t advert_loc_policy;
  char default_scope_name[31];
  uint8_t default_scope_key[16];
  uint8_t msg_repeat_enabled;
  uint8_t msg_repeat_max;
  uint8_t msg_repeat_interval_secs;
  // Auto-add mode (matches the MeshCore companion "manual_add_contacts" byte):
  // bit0 clear = auto-add ALL advert types; bit0 set = auto-add only the types
  // selected in autoadd_config (chat 0x02 / repeater 0x04 / room 0x08 /
  // sensor 0x10). 0 = add all. Appended last so older (shorter) prefs files
  // just keep the zero default.
  uint8_t manual_add_contacts;
  // Archive contacts to file as they leave the active table (evict/remove) so
  // they can be re-added later. 1 = on (default behaviour). When on AND
  // overwrite-when-full is off, a new contact that can't fit is archived
  // instead of discarded. Defaulted to 1 in begin() (memset would make it 0).
  uint8_t archive_contacts;
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
  uint32_t timestamp;    // AUTHORITATIVE time: our RX clock (received) or send clock (sent).
                         // Used for ALL time logic (bucketing, sort, window, retention).
  uint32_t sender_ts;    // sender's claimed clock — RECORDED ONLY, never used for time logic.
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
  double   lat, lon;     // our GPS location when the msg was sent/received
  bool     has_loc;      // false when no GPS fix was available (lat/lon omitted)
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
  bool _public_deleted = false;  // user deleted Public; persisted via the channels file ("pubdel")
  unsigned long last_msg_sent;
  ContactInfo *curr_recipient;
  char command[512 + 10];
  uint8_t tmp_buf[256];
  char hex_buf[512];
  lua_State *lua_runtime = NULL;

  // Last received packet radio info (updated in logRx)
  float last_rx_snr = 0;
  float last_rx_rssi = 0;

  // Bumped on every contact mutation — they all funnel through
  // saveContacts(). Lets the Lua _mesh_get_contacts binding cache its
  // table between changes instead of rebuilding ~500 entries per call.
  volatile uint32_t contacts_generation = 0;

  // ── Contact archive ────────────────────────────────────────────
  // Contacts that fall out of the live table (overwritten when it is full,
  // or removed by the user) are preserved in <storage>/contacts_arch so they
  // can still be shown on the map and re-added later — mirrors the MeshCore
  // Android app's contact history. DISK-ONLY (2026-06-19): the archive lives
  // ONLY in the append-only log on disk — there is NO in-RAM array and NO cap,
  // so it is permanent and costs ZERO PSRAM unless the user opens "show
  // archived", which reads it on demand. The hot path just appends one line;
  // duplicates are resolved newest-wins when read (re-adds rely on the union
  // skipping live contacts, so a re-added contact's stale line is harmless).
  volatile uint32_t archive_generation = 0;  // bumps on every archive change

  void appendArchiveEntry(const ContactInfo& c);
  void archiveContact(const ContactInfo& c);
  bool readdArchivedContact(const uint8_t* pub_key);
  // Read the archive log into `out` (deduped, newest line per pubkey wins),
  // up to max_out entries; returns the count. Used by the "show archived" map
  // union. Caller owns the (transient) buffer; disk keeps everything regardless
  // of max_out (only the on-map display is bounded).
  int readArchivedDeduped(ContactInfo* out, int max_out);

  // Read up to max_count archived contacts starting at byte `offset` in the log
  // into `out`; sets *next_offset to resume from and *done at EOF; returns the
  // count. Stateless (re-open + seek per call) so the mesh task can keep
  // appending between batches. Drives the Map's progressive "show archived"
  // loader — no dedup here (raw lines, caller decides).
  int readArchiveBatch(uint32_t offset, int max_count, ContactInfo* out,
                       uint32_t* next_offset, bool* done);

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
  void saveContacts();                          // full rewrite (removal/clear/bulk)
  void saveOneContact(const ContactInfo& c);    // O(1) in-place single-slot write
  void loadChannels();
  void saveChannels();
  // Public chat is slot 0 but, like any channel, can be deleted and re-added.
  // The deletion persists (channels-file "pubdel" marker) so boot won't recreate it.
  void deletePublic();
  void restorePublic();
  bool isPublicDeleted() const { return _public_deleted; }
  int  publicChannelIdx();   // slot of the channel named "Public", or -1 if none

  // ── Per-channel notification mode ───────────────────────────
  // NotifyChannelMode (notify.h), keyed by channel NAME — slots shift when
  // channels are added/removed, so a slot-keyed pref could silently attach to
  // the wrong channel. Missing entry = NOTIFY_CHAN_MENTION (the default).
  // Entries persist for deleted channels on purpose: re-adding a channel
  // under the same name restores its preference.
  struct ChannelNotifyPref { char name[32]; uint8_t mode; };
  static const int MAX_CHANNEL_NOTIFY_PREFS = 40;
  ChannelNotifyPref _chan_notify[MAX_CHANNEL_NOTIFY_PREFS];
  int _chan_notify_count = 0;
  void    loadChannelNotify();
  void    saveChannelNotify();
  uint8_t getChannelNotifyMode(const char* name);
  void    setChannelNotifyMode(const char* name, uint8_t mode);

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
  uint16_t _msg_retain_days = 30;  // days of message/routing history kept (0 = unlimited)
  // Incremental retention sweep (driven by pruneStep on the Core-0 loop).
  volatile bool _prune_due = false;        // set on new-day record / boot
  bool     _sweep_active = false;          // cursor below is pruneStep-only (Core 0)
  int      _sweep_idx = 0, _sweep_count = 0;
  uint32_t _sweep_cutoff = 0;
  String   _sweep_files[64];
  void setMaxMessages(int n);

  // Write paths (called from RX handlers and Lua send bindings).
  // `channel_idx < 0` on appendChannelMessage is a no-op (unknown channel).
  // `timestamp` is the AUTHORITATIVE time (our RX/send clock); `sender_ts` is the
  // sender's claimed clock, stored as a labeled extra and never used for time logic.
  void appendChannelMessage(int channel_idx, const char* from, const char* text,
                            uint32_t timestamp, float snr, float rssi,
                            uint8_t hops, bool direct,
                            uint16_t path_len = 0, const uint8_t* path = nullptr,
                            const uint8_t* pkt_hash = nullptr,
                            uint32_t sender_ts = 0);
  void appendDMMessage(const char* peer, const char* from, const char* text,
                       uint32_t timestamp, float snr, float rssi,
                       uint8_t hops, bool direct,
                       uint16_t path_len = 0, const uint8_t* path = nullptr,
                       const uint8_t* pkt_hash = nullptr,
                       const uint8_t* sender_pub_key = nullptr,
                       uint32_t sender_ts = 0);

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
  // Routing store (Phase 3): pushes {from,timestamp,lat,lon,path} records for a
  // sender (empty/null = all) within [since_ts, until_ts] (0 = open bound).
  int pushRoutingQuery(lua_State* L, const char* sender, uint32_t since_ts, uint32_t until_ts);
  // Distinct sender names from the routing index, matching an optional lowercased
  // substring (nullptr/"" = all). Streams one .idx at a time; bounded memory.
  int pushRoutingSenders(lua_State* L, const char* query, int max);
  // Incremental days-based retention sweep (routing + text logs), driven by
  // _prune_due and run one file per call from the Core-0 main loop, so it never
  // blocks the radio/UI for more than a single file's rewrite.
  void pruneStep();
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
  // "Do not add" exclusions: gate which advert types get auto-added (defined in
  // punkmesh.cpp where the ADV_TYPE_* constants are in scope).
  bool shouldAutoAddContactType(uint8_t type) const override;
  // Auto-add hop limit: 0 = no limit, 1 = direct only (0 hops), N = up to N-1
  // hops. The base mesh consults this when deciding whether to add a contact.
  uint8_t getAutoAddMaxHops() const override { return _prefs.autoadd_max_hops; }
  void clearContacts() { resetContacts(); }

  // Multi-byte path hash ("path hash mode"): each repeater appends
  // path_hash_mode+1 bytes (1/2/3) of its key to a flood path. Apply the
  // configured size to every flood we originate; RX/forward reads the size
  // from each packet, so receiving is already size-agnostic.
  uint8_t pathHashSize() const { return (uint8_t)(_prefs.path_hash_mode + 1); }

  // Build a self-advert, honoring the advert location-sharing policy
  // (advert_loc_policy 0 = omit GPS location from the advert).
  mesh::Packet* buildSelfAdvert() {
    return _prefs.advert_loc_policy
      ? createSelfAdvert(_prefs.node_name, _prefs.node_lat, _prefs.node_lon)
      : createSelfAdvert(_prefs.node_name);
  }

  // Default flood scope ("region"): the name derives a 16-byte transport key
  // via SHA256 (matches MeshCore's hashtag-region derivation). Empty = global.
  const char* getDefaultScopeName() const { return _prefs.default_scope_name; }
  void setDefaultScope(const char* name);

protected:
  void sendFloodScoped(const ContactInfo& recipient, mesh::Packet* pkt, uint32_t delay_millis=0) override;
  void sendFloodScoped(const mesh::GroupChannel& channel, mesh::Packet* pkt, uint32_t delay_millis=0) override;
  // Flood through the configured default transport scope (region key) when one
  // is set, else an unscoped flood. Applies the multi-byte path size.
  void sendFloodWithScope(mesh::Packet* pkt, uint32_t delay_millis);
  void logRx(mesh::Packet *pkt, int len, float score) override;
  float getAirtimeBudgetFactor() const override;
  int calcRxDelay(float score, uint32_t air_time) const override;
  bool allowPacketForward(const mesh::Packet *packet) override;
  void onDiscoveredContact(ContactInfo &contact, bool is_new, uint8_t path_len, const uint8_t *path) override;
  void onContactOverwrite(const uint8_t *pub_key) override;
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