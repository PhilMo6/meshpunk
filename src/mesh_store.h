#pragma once

// ── Shared message store (mstore) ────────────────────────────────────────────
// Protocol-agnostic persistence for conversations: text-format message logs,
// the .paths sidecars, the daily routing store, unread counters, retention
// sweep, chat pager and the Lua readers. Lifted from PunkMesh so every LoRa
// protocol writes and reads the same files; PunkMesh keeps thin idx->name
// wrappers for the existing Lua bindings and BLE sync.
//
// Keying: conversations are keyed by CHANNEL NAME / PEER NAME (the file layer
// always was — ch_<name>.log / dm_<peer>.log). MeshCore's channel-index
// surface translates in punkmesh.cpp via channel_name_for_idx.
//
// Locking contract (unchanged from the PunkMesh originals):
//  * unread_* functions do NOT lock — callers hold MESH_LOCK (mesh task holds
//    it around loop(); the Lua binding wrappers take it).
//  * push_channel_messages/push_dm_messages/push_chat_page_* must be called
//    WITHOUT MESH_LOCK held: the Lua pushes can longjmp on OOM and must not
//    strand the mesh task. Name resolution happens in the caller.
//  * File-writing functions self-manage the SD lock / USB flash guard.
//
// Wire constants are FROZEN copies of the MeshCore values current at the
// lift (static_asserts in punkmesh.cpp pin them): the on-disk record format
// must never drift with a lib/MeshCore submodule update.

#include <Arduino.h>
#include <FS.h>

typedef struct lua_State lua_State;

#define MSTORE_HASH_SIZE     8    // == MeshCore MAX_HASH_SIZE at lift time
#define MSTORE_PATH_MAX      64   // == MeshCore MAX_PATH_SIZE at lift time
#define MSTORE_MAX_CHANNELS  20   // == MAX_GROUP_CHANNELS (platformio.ini -D)
#define MSTORE_RPATHS_MAX    8    // == MAX_PATHS_PER_MSG at lift time

struct ObservedPath {
  uint16_t path_len;
  uint8_t  path[MSTORE_PATH_MAX];
  float    snr;
  float    rssi;
  bool     is_direct;
};

// In-memory message record. Persisted as human-readable key=value text
// (see append_msg_text / read_msg_text_file in mesh_store.cpp).
// Channel logs: <prefix>/messages/ch_<name>.log
// DM logs:      <prefix>/messages/dm_<sanitized_peer>.log
struct StoredMsg {
  uint32_t timestamp;    // AUTHORITATIVE time: our RX clock (received) or send clock (sent).
                         // Used for ALL time logic (bucketing, sort, window, retention).
  uint32_t sender_ts;    // sender's claimed clock — RECORDED ONLY, never used for time logic.
  float    snr;
  float    rssi;
  int8_t   channel_idx;  // -1 for DM, 0..N for channel slot (record field only;
                         // file paths are name-keyed)
  uint8_t  hops;
  uint8_t  flags;        // bit0=direct, bit1=is_dm
  char     from[32];
  char     peer[32];     // for DMs, the OTHER party (thread key); empty for channels
  char     text[160];
  uint16_t path_len;     // encoded: upper bits = hash size, lower 6 = hop count
  uint8_t  path[MSTORE_PATH_MAX];
  uint8_t  pkt_hash[MSTORE_HASH_SIZE];
  bool     has_hash;
  uint8_t  sender_pub_key[6];
  bool     has_pub_key;
  double   lat, lon;     // our GPS location when the msg was sent/received
  bool     has_loc;      // false when no GPS fix was available (lat/lon omitted)
  uint8_t  rpath_count;
  ObservedPath rpaths[MSTORE_RPATHS_MAX];
};

// Directory metadata for BLE sync file enumeration (names + sizes only).
struct MsgFileInfo {
  char     path[64];
  uint32_t size;
};

// Channel reference for push_msg_summaries: built by the caller from the
// active protocol's channel table (under that protocol's lock).
struct MStoreChanRef {
  int  idx;
  char name[32];
};

// Normalize UTF-8 smart quotes to ASCII (montserrat_14 has no U+2018-201D).
// Global (not namespaced): callers in punkmesh.cpp, main.cpp, ble_companion.
size_t normalize_smart_quotes(const char* in, char* out, size_t outlen);

namespace mstore {

// Backend + prefix for every store file. Recovers orphaned .tmp files in the
// messages dir. State lives in PSRAM (allocated on first use).
void set_storage(fs::FS* fs, const char* prefix);
fs::FS*       storage();
const String& prefix();

// Per-protocol namespace: "" (default) = the legacy root, where MeshCore's
// existing files live; another protocol's id folders its messages
// and route under <prefix>/<ns>/ so protocols never share conversation
// files. Set by the protocol loader at boot, before any append.
void set_namespace(const char* ns);
const String& ns();

// Path builders (name/peer-keyed).
String messages_dir_path();
String channel_msg_path_for(const char* ch_name);
String dm_msg_path_for(const char* peer);

// Retention / caps.
void     set_max_messages(int n);          // size backstop hint (records kept)
void     set_retain_days(uint16_t days);   // 0 = unlimited
uint16_t retain_days();
void     prune_mark_due();                 // schedule a sweep (boot / new day)
// One incremental sweep step; call from the Core-0 loop. now_ts = device
// epoch seconds (clock authority is the caller's — see feedback_time_truth).
void     prune_step(uint32_t now_ts);

// Write paths. Self-lock SD / flash guard. append_channel_message also feeds
// the daily routing store and flags the retention sweep on a new day.
void append_channel_message(const char* ch_name, int channel_idx,
                            const char* from, const char* text,
                            uint32_t timestamp, float snr, float rssi,
                            uint8_t hops, bool direct,
                            uint16_t path_len = 0, const uint8_t* path = nullptr,
                            const uint8_t* pkt_hash = nullptr,
                            uint32_t sender_ts = 0);
void append_dm_message(const char* peer, const char* from, const char* text,
                       uint32_t timestamp, float snr, float rssi,
                       uint8_t hops, bool direct,
                       uint16_t path_len = 0, const uint8_t* path = nullptr,
                       const uint8_t* pkt_hash = nullptr,
                       const uint8_t* sender_pub_key = nullptr,
                       uint32_t sender_ts = 0);
// O(1) append of one observed extra path to the conversation's .paths sidecar.
void append_extra_path(const String& msg_log_path, const uint8_t* hash,
                       const ObservedPath& op);

// Unread counters (RAM, name-keyed; survive Lua teardown). NO internal lock —
// see the contract above.
void     unread_bump_channel(const char* ch_name);
void     unread_bump_dm(const char* name);
void     unread_clear_channel(const char* ch_name);
void     unread_clear_dm(const char* name);
uint16_t unread_channel(const char* ch_name);
uint16_t unread_dm(const char* name);
uint32_t unread_total();

// Lua readers. Each pushes one table and returns 1 (empty table if the file
// doesn't exist). Call WITHOUT MESH_LOCK held.
int push_channel_messages(lua_State* L, const char* ch_name, int max_records);
int push_dm_messages(lua_State* L, const char* peer, int max_records);
int push_dm_thread_names(lua_State* L);
int push_msg_summaries(lua_State* L, const MStoreChanRef* chans, int nch);
int push_chat_page_channel(lua_State* L, const char* ch_name, int mode,
                           uint32_t cursor, int count);
int push_chat_page_dm(lua_State* L, const char* peer, int mode,
                      uint32_t cursor, int count);
// Observed routes for one stored message; ch_name selects a channel log,
// else peer selects a DM log.
int lookup_persisted_paths(lua_State* L, const char* hash_hex,
                           const char* ch_name, const char* peer);
// Routing store queries (Map/meshprint). sender empty/null = all;
// since/until 0 = open bound.
int push_routing_query(lua_State* L, const char* sender,
                       uint32_t since_ts, uint32_t until_ts);
int push_routing_senders(lua_State* L, const char* query, int max);

// Raw record access (BLE sync).
int      enumerate_message_files(MsgFileInfo* out, int max_paths);
int      read_one_stored_msg(fs::FS* storage, const char* path,
                             size_t offset, StoredMsg& m);
int      read_all_stored_msgs(const char* path, StoredMsg* out, int max_count);
int      read_stored_msgs_from(const char* path, uint32_t start_offset,
                               StoredMsg* out, uint32_t* end_offsets,
                               int max_count, uint32_t* next_offset,
                               uint32_t* file_size);
uint32_t offset_of_newest_records(const char* path, uint32_t start_offset, int n);

// Push path hashes as a Lua table of hex strings (shared with the RxEvent
// pushers in punkmesh.cpp).
void push_path_table(lua_State* L, uint16_t path_len, const uint8_t* path);

}  // namespace mstore
