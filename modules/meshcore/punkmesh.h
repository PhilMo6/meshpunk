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
#include "mesh_store.h"   // StoredMsg/ObservedPath/MsgFileInfo + the mstore:: store API

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
  // Client repeat (matches the MeshCore companion "client_repeat" pref):
  // 1 = re-transmit other nodes' packets like a repeater. Default 0.
  uint8_t client_repeat;
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

// ObservedPath and StoredMsg moved to mesh_store.h (sized by the frozen
// MSTORE_* constants; static_asserts in punkmesh.cpp pin them to the
// MeshCore values).
struct MsgPathEntry {
  uint8_t       pkt_hash[MAX_HASH_SIZE];
  uint8_t       path_count;
  bool          is_message;
  bool          is_dm;
  int8_t        channel_idx;
  char          peer[32];
  ObservedPath  paths[MAX_PATHS_PER_MSG];
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

// 6 slots: the send retry ladder can register a repeat per attempt (up to 5)
// on top of concurrent channel sends; 4 evicted too eagerly.
#define MAX_PENDING_REPEATS 6
#define MAX_REPEAT_HISTORY  8

struct PendingRepeat {
  uint8_t header;
  uint8_t payload[MAX_PACKET_PAYLOAD];
  uint16_t payload_len;
  uint8_t pkt_hash[MAX_HASH_SIZE];
  uint8_t attempts_remaining;
  unsigned long next_retry_time;
  bool active;
  // Direct-routed packets re-air via sendDirect and need their route kept
  // (the payload doesn't carry it). direct=false → re-air as flood.
  bool direct;
  uint8_t path_len;
  uint8_t path[MAX_PATH_SIZE];
};

struct RepeatOutcome {
  uint8_t pkt_hash[MAX_HASH_SIZE];
  uint8_t status; // 2=confirmed, 3=exhausted
};

// Raw packet capture moved to src/radio/radio_capture.h (rcap::) — the ring
// is protocol-agnostic and works under any protocol. MeshCore's four
// Dispatcher log hooks (logRx/logRxRaw/logTx/logTxFail) push into it.
#include "radio/radio_capture.h"

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

  // (Packet capture lives in rcap:: — see radio/radio_capture.h.)

  // Bumped on every contact mutation — they all funnel through
  // saveContacts(). Lets the Lua _mesh_get_contacts binding cache its
  // table between changes instead of rebuilding ~500 entries per call.
  volatile uint32_t contacts_generation = 0;

  // ── Contact archive ────────────────────────────────────────────
  // Contacts that fall out of the live table (overwritten when it is full,
  // or removed by the user) are preserved in <storage>/contacts_arch.bin so
  // they can still be shown on the map and re-added later — mirrors the
  // MeshCore Android app's contact history. DISK-ONLY records (2026-06-19):
  // the record data lives only in the fixed-stride log on disk, read on
  // demand. Since 2026-07-02 the hot path is an UPSERT: an in-RAM pubkey→
  // offset hash index (192KB, allocated once at boot low in PSRAM — see the
  // dedup-index section in punkmesh.cpp) lets a re-archived contact overwrite
  // its existing record in place, so the log no longer grows without bound on
  // a large mesh. compactArchive() rewrites the log to one record per pubkey
  // (newest wins, live contacts dropped) and rebuilds the index — also the
  // recovery path for a pre-index runaway log, which is too big to index at
  // boot and falls back to plain appends until compacted.
  volatile uint32_t archive_generation = 0;  // bumps on every archive change

  void appendArchiveEntry(const ContactInfo& c);
  void archiveContact(const ContactInfo& c);
  bool readdArchivedContact(const uint8_t* pub_key);
  // Allocate (once) + rebuild the dedup index from this backend's log.
  // Called by setStorage(); safe to call again (idempotent allocation).
  void archiveIndexInit();
  // Streaming dedup rewrite of the log (see punkmesh.cpp for the pass/locking
  // design). Returns 0 with record counts in the out params, negative on error.
  int compactArchive(uint32_t* before_out, uint32_t* after_out);
  // Records currently in the log, duplicates included (file size / stride).
  uint32_t archiveRecordCount();
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
  // Shutdown/reboot: write out everything that persists lazily (contacts,
  // room sync cursors, path history) so a power-off can't lose the deferred
  // tail. Call under MESH_LOCK.
  void flushForShutdown();
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

  // ── Per-channel region / flood scope ────────────────────────
  // Keyed by channel NAME for the same reasons as ChannelNotifyPref (slots
  // shift; Public isn't persisted in /channels). Missing entry = inherit the
  // device-global default scope. The 16-byte transport key is derived from
  // the region name at send time (same derivation as setDefaultScope), so
  // only the names are stored here and in /channel_regions.
  struct ChannelScopePref { char name[32]; char scope[31]; };
  static const int MAX_CHANNEL_SCOPE_PREFS = 40;
  ChannelScopePref _chan_scopes[MAX_CHANNEL_SCOPE_PREFS];
  int _chan_scope_count = 0;
  void        loadChannelScopes();
  void        saveChannelScopes();
  const char* getChannelScope(const char* chan_name);  // "" = inherit global
  void        setChannelScope(const char* chan_name, const char* region);

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
  // Persistence, retention state and the sweep cursor live in mesh_store
  // (mstore::set_max_messages / set_retain_days / prune_step); PunkMesh keeps
  // only the BLE backlog cap below.
  // BLE companion sync: newest messages served per conversation file on a
  // backlog (0 = no limit). Read by BleMsgSync when it picks a file to serve.
  uint16_t _ble_sync_max_per_channel = 0;

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

  // ── Unread counters ─────────────────────────────────────────────
  // The counters live name-keyed in mesh_store (C-side, so they keep counting
  // while Lua is torn down for an ELF run); these are the idx->name wrappers.
  // Bumped in the RX handlers next to the notify calls; own echoes never bump.
  // All access under MESH_LOCK. Reset at reboot.
  void unreadBumpChannel(int channel_idx);
  void unreadBumpDM(const char* name);
  void unreadClearChannel(int channel_idx);
  void unreadClearDM(const char* name);
  uint16_t unreadChannel(int channel_idx);
  uint16_t unreadDM(const char* name);
  uint32_t unreadTotal();

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

  // path != nullptr registers a DIRECT repeat (re-aired via sendDirect on
  // the saved route); nullptr = flood repeat (the original behavior).
  void registerPendingRepeat(const uint8_t* hash, uint8_t header,
                             const uint8_t* payload, uint16_t payload_len,
                             const uint8_t* path = nullptr, uint8_t path_len = 0);
  void checkPendingRepeats();
  // Returns the repeat-until-heard status of a sent packet: 1 = actively
  // repeating, 2 = echo heard (confirmed), 3 = exhausted (never heard), 0 =
  // untracked. For an active repeat (1) the optional out-params report the
  // progress the UI shows as "repeating N/M": *remaining = re-airs still to go,
  // *total = the configured max (msg_repeat_max); both 0 for other states.
  int  getRepeatStatus(const uint8_t* hash, int* remaining = nullptr, int* total = nullptr);

  // Read paths. Each pushes a Lua table (array of message tables) and
  // returns 1 (the number of Lua stack values). Safe to call even if
  // the file doesn't exist — you get an empty table.
  // max_records: 0 = whole log; N = only the newest N records (two-pass
  // count-then-skip, so a multi-thousand-record log never materializes as a
  // whole-file Lua transient). Lock contract: pushChannelMessagesToLua takes
  // MESH_LOCK internally just for the channel-name snapshot and
  // pushDMMessagesToLua takes none — call both WITHOUT the lock held (the
  // unlocked read keeps a lua_push OOM longjmp from stranding the mesh task).
  int pushChannelMessagesToLua(lua_State* L, int channel_idx, int max_records = 0);
  int pushDMMessagesToLua(lua_State* L, const char* peer, int max_records = 0);

  // Chat pager (Messenger sliding-window scroll). Pages a conversation log by
  // byte offset so the chat holds a bounded bubble window and never loads the
  // whole thread. mode: 0 = tail (newest `count`), 1 = older (`count` records
  // before `cursor`), 2 = newer (forward from `cursor`). Pushes ONE table
  // { list = { <msg + integer off0/off1>, ... }, size = <file bytes> }. Same
  // lock contract as pushChannelMessagesToLua (channel-name snapshot only; DM
  // none) — call WITHOUT the lock held.
  int pushChatPageChannel(lua_State* L, int channel_idx, int mode, uint32_t cursor, int count);
  int pushChatPageDM(lua_State* L, const char* peer, int mode, uint32_t cursor, int count);
  int pushDMThreadNamesToLua(lua_State* L);
  // One {kind, idx/name, count, last} summary entry per stored conversation
  // (count + last record only — no full histories). Takes MESH_LOCK internally
  // just for the channel-table snapshot; call it WITHOUT the lock held.
  int pushMsgSummariesToLua(lua_State* L);
  // Routing store (Phase 3): pushes {from,timestamp,lat,lon,path} records for a
  // sender (empty/null = all) within [since_ts, until_ts] (0 = open bound).
  int pushRoutingQuery(lua_State* L, const char* sender, uint32_t since_ts, uint32_t until_ts);
  // Distinct sender names from the routing index, matching an optional lowercased
  // substring (nullptr/"" = all). Streams one .idx at a time; bounded memory.
  int pushRoutingSenders(lua_State* L, const char* query, int max);
  // (The retention sweep is mstore::prune_step, called from main.cpp's loop.)
  int lookupPersistedPaths(lua_State* L, const char* hash_hex, int channel_idx, const char* peer);

  bool hasConnectionToContact(const uint8_t* pub_key) { return hasConnectionTo(pub_key); }
  // Manual logout: unwatch FIRST so the expiry scan in loop() doesn't read
  // the dropped slot as a lost connection.
  void stopConnectionToContact(const uint8_t* pub_key) { unwatchConnection(pub_key); stopConnection(pub_key); }
  bool startConnectionToContact(const ContactInfo& contact, uint16_t keep_alive_secs) {
    bool ok = startConnection(contact, keep_alive_secs);
    if (ok) watchConnection(contact);   // expiry → CONN_LOST event (see loop())
    return ok;
  }

  // ── Room/repeater login (device UI) ─────────────────────────────
  // 4-byte pubkey prefix of the server the Lua UI last sent a login /
  // status request to (0 = none pending). Set under MESH_LOCK by the Lua
  // bindings, consumed in onContactResponse. Mirrors — and coexists with —
  // the BLE companion's own pending_login/pending_status, so a phone login
  // and a device login can be in flight independently.
  uint32_t pending_login_prefix = 0;
  uint32_t pending_status_prefix = 0;

  // sync_since sidecar: ContactInfo.sync_since is runtime-only in the fixed
  // CONTACT_REC record (contacts.bin AND the archive share that stride), so
  // a reboot would make the next room login re-fetch the room's whole
  // retained history over LoRa. /room_sync.bin keeps just the sync cursors
  // (pubkey_prefix(8) + sync_since(4) per record): loaded after
  // loadContacts(), lazily saved from the mesh task ~30s after a change so
  // a chatty room doesn't wear flash with per-message writes.
  bool _room_sync_dirty = false;
  unsigned long _room_sync_save_at = 0;
  void loadRoomSync();
  void saveRoomSync();
  void markRoomSyncDirty();

  // ── DM send retry ladder ─────────────────────────────────────────
  // One outstanding tracked send (mirrors the firmware's single expected-ack
  // model). Armed only by the device-UI send binding — BLE sends are excluded
  // (the phone app runs its own retries). Ladder: the original send plus
  // direct_left retries on the stored path, then the path auto-resets to
  // flood and flood_left retries go out flooded; only then is the send
  // declared failed. `attempt` increments per resend so each wire packet
  // (and its ack hash) is unique; the UI correlates delivery by orig_ack —
  // see armPendingSend / onSendTimeout / processAck.
  struct PendingSend {
    bool     active = false;
    uint8_t  recipient_pub[PUB_KEY_SIZE];
    uint32_t orig_ack;        // the ack the Lua UI indexed at send time
    uint32_t timestamp;       // original msg timestamp (reused on resends)
    char     text[160];
    uint8_t  attempt;         // last attempt number sent (0 = original)
    uint8_t  direct_left;     // remaining retries via the stored path
    uint8_t  flood_left;      // remaining retries via flood
    uint8_t  total_attempts;  // for the UI's "retry n/m"
    // Every attempt's expected ack. A repeat-until-heard re-air can deliver
    // an OLD attempt long after the ladder moved on — its late ack must
    // still count as delivered (level-1 repeats must not corrupt level-2).
    // Note attempt 4 shares attempt 0's ack (composeMsgPacket hashes only
    // 2 attempt bits) — a harmless duplicate entry here.
    uint32_t acks[5];
    uint8_t  ack_count;
    // Our own ack deadline: tracked sends don't arm the base class's
    // (private) txt_send_timeout, so the ladder is driven from loop().
    unsigned long deadline;
  };
  PendingSend _pending_send;
  void armPendingSend(const ContactInfo& recipient, uint32_t orig_ack,
                      uint32_t timestamp, const char* text, bool sent_direct,
                      uint32_t est_timeout_ms);
  void failPendingSend();   // queue failed-ACK event for orig_ack + clear
  void pendingSendLadderStep();   // deadline passed: retry or declare failed

  // ── Tracked sends ────────────────────────────────────────────────
  // Local variants of BaseChatMesh::sendMessage / sendCommandData with the
  // SAME wire behavior (MeshCore is a git submodule and must stay pristine;
  // its versions compose the packet internally, so the direct branch is
  // invisible to us). Differences: the direct branch goes through
  // sendDirectTracked (tx hash + repeat-until-heard, zero-hop excluded) and
  // no base txt_send_timeout is armed — the retry ladder's own deadline
  // covers timeouts, and CLI sends (no acks ever) get no timeout at all.
  // Keep the packet composition in sync with BaseChatMesh.cpp on upgrades.
  int  sendMessageTracked(const ContactInfo& recipient, uint32_t timestamp,
                          uint8_t attempt, const char* text,
                          uint32_t& expected_ack, uint32_t& est_timeout);
  int  sendCommandTracked(const ContactInfo& recipient, uint32_t timestamp,
                          uint8_t attempt, const char* text, uint32_t& est_timeout);
  mesh::Packet* composeTrackedMsgPacket(const ContactInfo& recipient, uint32_t timestamp,
                                        uint8_t attempt, const char* text,
                                        uint32_t& expected_ack);
  void sendDirectTracked(const ContactInfo& recipient, mesh::Packet* pkt);

  // ── Keep-alive session watch ─────────────────────────────────────
  // Servers we (or the phone) logged into with a keep-alive interval.
  // checkConnections() silently frees an expired slot; this watch turns that
  // into a CONN_LOST event for the UI. Manual logout unwatches first, so it
  // never fires a false alarm. Path is NOT reset on expiry (Noah's call —
  // the server may just have been down; the login flood fallback in the
  // Messenger covers the dead-path case on the next login).
  struct ConnWatch { bool active; uint8_t pub_key[PUB_KEY_SIZE]; char name[32]; };
  ConnWatch _conn_watch[16];   // matches BaseChatMesh MAX_CONNECTIONS
  void watchConnection(const ContactInfo& contact);
  void unwatchConnection(const uint8_t* pub_key);

  // ── Path history persistence ─────────────────────────────────────
  // _path_history (below) feeds the Paths picker; persist it so the picker
  // isn't empty after every reboot. /path_hist.bin = version + record size +
  // count + raw array dump; ignored on version/size mismatch. Lazy write:
  // first change arms ~1 min, then at most one write per 10 min (the ring
  // is dirtied by every RX message, so write-through would wear flash).
  bool _path_hist_dirty = false;
  unsigned long _path_hist_save_at = 0;
  void loadPathHistory();
  void savePathHistory();
  void markPathHistDirty();
  const uint8_t* getPrivateKey() const { return ((const uint8_t*)&self_id) + PUB_KEY_SIZE; }
  bool saveIdentity();

  // BLE sync: enumerate message files and read records.
  static const int MAX_SYNC_FILES = 40;
  static const int MAX_SYNC_PATH_LEN = 64;
  // The struct moved to mesh_store.h (path[64] there == MAX_SYNC_PATH_LEN).
  using MsgFileInfo = ::MsgFileInfo;
  // Directory metadata only (names + sizes) — opens no file contents.
  int enumerateMessageFiles(MsgFileInfo* out, int max_paths);
  static int readOneStoredMsg(fs::FS* storage, const char* path,
                              size_t offset, StoredMsg& m);
  int readAllStoredMsgs(const char* path, StoredMsg* out, int max_count);
  // Seek-based batched read: parses up to max_count COMPLETE ("---"-terminated)
  // records starting at byte start_offset.
  // end_offsets[] holds the offset just past each returned record;
  // *next_offset lands just past the last parsed record (= resume cursor);
  // *file_size is the file's current size. Returns records in out[].
  int readStoredMsgsFrom(const char* path, uint32_t start_offset,
                         StoredMsg* out, uint32_t* end_offsets, int max_count,
                         uint32_t* next_offset, uint32_t* file_size);
  // Byte offset at which the newest `n` records begin, never earlier than
  // start_offset. Returns start_offset when [start_offset, EOF) holds <= n
  // records. Readers seek to a stored offset and parse forward (oldest first),
  // so serving only the newest n means advancing that offset past the rest.
  uint32_t offsetOfNewestRecords(const char* path, uint32_t start_offset, int n);
  String messagesDirPath();

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
  // Runtime flood scope set by the phone app (BLE CMD_SET_FLOOD_SCOPE_KEY):
  // session-only — never persisted, zeroed at boot. When non-zero it overrides
  // the default scope for all sends; a per-channel region outranks both.
  // Mirrors companion_radio's 'send_scope' (MyMesh.h).
  uint8_t _ble_send_scope_key[16] = {0};

protected:
  void sendFloodScoped(const ContactInfo& recipient, mesh::Packet* pkt, uint32_t delay_millis=0) override;
  void sendFloodScoped(const mesh::GroupChannel& channel, mesh::Packet* pkt, uint32_t delay_millis=0) override;
  // Flood through a transport scope (region key) when one applies, else an
  // unscoped flood. Applies the multi-byte path size. key_override (16 bytes)
  // replaces the default scope key for this send; nullptr = default scope.
  void sendFloodWithScope(mesh::Packet* pkt, uint32_t delay_millis,
                          const uint8_t* key_override = nullptr);
  void logRx(mesh::Packet *pkt, int len, float score) override;
  // Every frame the radio hands up, before parsing — so the monitor also sees
  // frames tryParsePacket rejects and frames dropped for an empty packet pool.
  void logRxRaw(float snr, float rssi, const uint8_t raw[], int len) override;
  void logTx(mesh::Packet *pkt, int len) override;
  void logTxFail(mesh::Packet *pkt, int len) override;
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

// (normalize_smart_quotes is declared in mesh_store.h, defined in mesh_store.cpp.)

// Host-owned device clock: created in main.cpp before the mesh and shared with
// it (PunkMesh's RTCClock reference points at this object). Read/ticked by
// code that must run whichever protocol is active (mesh_task pause tick,
// RTC bindings, GPS stamps, retention sweep).
extern VolatileRTCClock* host_rtc;

#endif
