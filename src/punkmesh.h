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
  uint8_t _pad2[2];
};

struct MeshMessage {
    char from[32];     // short pubkey or addr
    char text[128];    // payload
    uint32_t timestamp;
    uint8_t hops;
    bool direct;
};

#define MAX_MESSAGES 64   // tweak as needed

// Fixed-size on-disk record for persistent message history.
// One StoredMsg per record; files are flat arrays of these.
// Channel logs: <storage_prefix>/messages/ch_<idx>.log
// DM logs:      <storage_prefix>/messages/dm_<sanitized_peer>.log
struct StoredMsg {
  uint32_t timestamp;    // 4
  float    snr;          // 4
  float    rssi;         // 4
  int8_t   channel_idx;  // 1  — -1 for DM, 0..N for channel slot
  uint8_t  hops;         // 1
  uint8_t  flags;        // 1  — bit0=direct, bit1=is_dm
  uint8_t  _pad;         // 1
  char     from[32];     // 32 — sender display name ("me" == us for outgoing)
  char     peer[32];     // 32 — for DMs, the OTHER party (thread key); empty for channels
  char     text[128];    // 128 — null-padded, truncated if longer
};                        // total: 208 bytes, fixed-size for easy trim
static_assert(sizeof(StoredMsg) == 208, "StoredMsg size changed; disk format must bump");

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

  // ── Persistent message history ───────────────────────────────
  // Max records kept per file (channel or DM). The file is compacted
  // when it grows past ~2x this value.
  int _max_messages = 100;
  void setMaxMessages(int n);

  // Write paths (called from RX handlers and Lua send bindings).
  // `channel_idx < 0` on appendChannelMessage is a no-op (unknown channel).
  void appendChannelMessage(int channel_idx, const char* from, const char* text,
                            uint32_t timestamp, float snr, float rssi,
                            uint8_t hops, bool direct);
  void appendDMMessage(const char* peer, const char* from, const char* text,
                       uint32_t timestamp, float snr, float rssi,
                       uint8_t hops, bool direct);

  // Read paths. Each pushes a Lua table (array of message tables) and
  // returns 1 (the number of Lua stack values). Safe to call even if
  // the file doesn't exist — you get an empty table.
  int pushChannelMessagesToLua(lua_State* L, int channel_idx);
  int pushDMMessagesToLua(lua_State* L, const char* peer);
  int pushDMThreadNamesToLua(lua_State* L);

  void setClock(uint32_t timestamp);
  void importCard(const char *command);

  float getFreqPref() const;
  uint8_t getTxPowerPref() const;
  float getBandwidthPref() const;
  uint8_t getSpreadingFactorPref() const;
  uint8_t getCodingRatePref() const;

protected:
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