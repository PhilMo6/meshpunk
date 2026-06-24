#include "punkmesh.h"
#include <LittleFS.h>
#include <esp_heap_caps.h>
#include <helpers/TransportKeyStore.h>
#include <SHA256.h>
#include "meshpunk_sync.h"
#include "ble_companion.h"

// Shared-SPI-bus lock pair (defined in main.cpp).
// sd_spi_take()    — acquire spi_bus_mutex before any SD operation.
// sd_spi_release() — release spi_bus_mutex after the SD file handle is closed.
// sd_spi_take() is inline in meshpunk_sync.h (just SPI_LOCK); no extern decl needed.
extern void sd_spi_release();
extern PunkMesh* the_mesh;

// Forward declaration — defined further down with the other path helpers.
static String messages_dir(const String& prefix);

// Recover orphaned .tmp files left by interrupted compaction.
static void recover_tmp_files(fs::FS* storage, const String& dir) {
    if (!storage) return;
    File root = storage->open(dir.c_str());
    if (!root || !root.isDirectory()) return;

    File entry = root.openNextFile();
    while (entry) {
        if (!entry.isDirectory()) {
            String name = entry.name();
            if (name.endsWith(".tmp")) {
                String log_path = name.substring(0, name.length() - 4);
                if (storage->exists(log_path.c_str())) {
                    storage->remove(name.c_str());
                    SLog.printf("[STORAGE] Removed stale tmp: %s\n", name.c_str());
                } else {
                    storage->rename(name.c_str(), log_path.c_str());
                    SLog.printf("[STORAGE] Recovered tmp: %s -> %s\n",
                                  name.c_str(), log_path.c_str());
                }
            }
        }
        entry = root.openNextFile();
    }
    root.close();
}

// Storage helpers
void PunkMesh::setStorage(fs::FS* fs, const char* prefix) {
    _storage = fs;
    _storage_prefix = String(prefix);
    SLog.printf("[STORAGE] Set to %s, prefix=\"%s\"\n",
        (fs == &LittleFS) ? "LittleFS" : "SD", prefix);
    recover_tmp_files(_storage, messages_dir(_storage_prefix));
}

// Helper to build a full path with storage prefix
static String storagePath(const String& prefix, const char* name) {
    return prefix + name;
}

// Identity is now generated on first boot and persisted
// Storage location: SD card if available, else LittleFS

/* ---------------------------------- CONFIGURATION ------------------------------------- */

#define FIRMWARE_VER_TEXT "v2 (build: 4 Feb 2025)"

#ifndef LORA_FREQ
#define LORA_FREQ 915.0
#endif
#ifndef LORA_BW
#define LORA_BW 250
#endif
#ifndef LORA_SF
#define LORA_SF 10
#endif
#ifndef LORA_CR
#define LORA_CR 5
#endif
#ifndef LORA_TX_POWER
#define LORA_TX_POWER 20
#endif

#ifndef MAX_CONTACTS
#define MAX_CONTACTS 300  // set globally via -D in platformio.ini
#endif

#include <helpers/BaseChatMesh.h>

#define SEND_TIMEOUT_BASE_MILLIS 500
#define FLOOD_SEND_TIMEOUT_FACTOR 16.0f
#define DIRECT_SEND_PERHOP_FACTOR 6.0f
#define DIRECT_SEND_PERHOP_EXTRA_MILLIS 250

#define PUBLIC_GROUP_PSK "izOH6cXN6mrJ5e26oRXNcg=="

// Punk<->Lua bridge

static bool contains_mention(const char* text, const char* name) {
    if (!text || !name || name[0] == '\0') return false;
    size_t name_len = strlen(name);
    const char* p = text;
    while ((p = strchr(p, '@')) != NULL) {
        p++;
        if (*p == '[') {
            p++;
            if (strncasecmp(p, name, name_len) == 0 && p[name_len] == ']')
                return true;
        }
    }
    return false;
}

static void push_path_table(lua_State* L, uint16_t path_len, const uint8_t* path);

// Dispatch a channel (public) message to Lua with parsed sender name.
// Called from the UI core (drain_rx_events) — the packet object is gone by
// the time we run, so hops/direct come from the enqueued RxEvent.
void lua_mesh_push_channel_message(lua_State* L, const char* sender_name, uint8_t hops, bool direct, uint32_t timestamp, const char *text, float snr, float rssi, int channel_idx, uint16_t path_len, const uint8_t* path, const uint8_t* pkt_hash) {
    lua_getglobal(L, "require");
    lua_pushstring(L, "lib/mesh/messages");

    if (lua_pcall(L, 1, 1, 0) != LUA_OK) {
        SLog.printf("require failed: %s\n", lua_tostring(L, -1));
        lua_pop(L, 1);
        return;
    }

    lua_getfield(L, -1, "__dispatch");
    if (!lua_isfunction(L, -1)) {
        SLog.println("__dispatch not a function!");
        lua_pop(L, 2);
        return;
    }

    lua_pushstring(L, sender_name);             // arg1: from
    lua_pushstring(L, text);                    // arg2: text
    lua_pushinteger(L, timestamp);              // arg3: timestamp
    lua_pushboolean(L, direct);                 // arg4: direct
    lua_pushinteger(L, hops);                   // arg5: hops
    lua_pushnumber(L, snr);                     // arg6: snr
    lua_pushnumber(L, rssi);                    // arg7: rssi
    lua_pushinteger(L, channel_idx);            // arg8: channel_idx (-1 if unknown)
    lua_pushboolean(L, contains_mention(text, the_mesh->_prefs.node_name)); // arg9: is_mention
    push_path_table(L, path_len, path);         // arg10: path
    if (pkt_hash) {                              // arg11: hash (hex string)
        char hex[MAX_HASH_SIZE * 2 + 1];
        mesh::Utils::toHex(hex, pkt_hash, MAX_HASH_SIZE);
        lua_pushstring(L, hex);
    } else {
        lua_pushnil(L);
    }

    if (lua_pcall(L, 11, 0, 0) != LUA_OK) {
        SLog.printf("__dispatch failed: %s\n", lua_tostring(L, -1));
        lua_pop(L, 1);
    }

    lua_pop(L, 1);
}

// Dispatch a direct message to Lua. See channel variant above.
void lua_mesh_push_direct_message(lua_State* L, const char* sender_name, uint8_t hops, bool direct, uint32_t timestamp, const char *text, float snr, float rssi, uint16_t path_len, const uint8_t* path, const uint8_t* pkt_hash) {
    lua_getglobal(L, "require");
    lua_pushstring(L, "lib/mesh/messages");

    if (lua_pcall(L, 1, 1, 0) != LUA_OK) {
        SLog.printf("require failed: %s\n", lua_tostring(L, -1));
        lua_pop(L, 1);
        return;
    }

    lua_getfield(L, -1, "__dispatch_dm");
    if (!lua_isfunction(L, -1)) {
        lua_pop(L, 2);
        return;
    }

    lua_pushstring(L, sender_name);             // arg1: from
    lua_pushstring(L, text);                    // arg2: text
    lua_pushinteger(L, timestamp);              // arg3: timestamp
    lua_pushboolean(L, direct);                 // arg4: direct
    lua_pushinteger(L, hops);                   // arg5: hops
    lua_pushnumber(L, snr);                     // arg6: snr
    lua_pushnumber(L, rssi);                    // arg7: rssi
    push_path_table(L, path_len, path);         // arg8: path
    if (pkt_hash) {                              // arg9: hash (hex string)
        char hex[MAX_HASH_SIZE * 2 + 1];
        mesh::Utils::toHex(hex, pkt_hash, MAX_HASH_SIZE);
        lua_pushstring(L, hex);
    } else {
        lua_pushnil(L);
    }

    if (lua_pcall(L, 9, 0, 0) != LUA_OK) {
        SLog.printf("__dispatch_dm failed: %s\n", lua_tostring(L, -1));
        lua_pop(L, 1);
    }

    lua_pop(L, 1);
}

void lua_mesh_push_contact_update(lua_State* L, const char* name, uint8_t contact_type) {
    lua_getglobal(L, "require");
    lua_pushstring(L, "lib/mesh/messages");

    if (lua_pcall(L, 1, 1, 0) != LUA_OK) {
        lua_pop(L, 1);
        return;
    }

    lua_getfield(L, -1, "__dispatch_contact");
    if (!lua_isfunction(L, -1)) {
        lua_pop(L, 2);
        return;
    }

    lua_pushstring(L, name);
    lua_pushinteger(L, contact_type);

    if (lua_pcall(L, 2, 0, 0) != LUA_OK) {
        SLog.printf("__dispatch_contact failed: %s\n", lua_tostring(L, -1));
        lua_pop(L, 1);
    }

    lua_pop(L, 1); // pop module
}

// Dispatch a delivery result (ACK / send-timeout) to Lua. The UI correlates it
// to the sent DM by the expected-ack CRC returned from _mesh_send_direct.
// rtt >= 0 => delivered (round-trip ms); rtt < 0 => failed/no-ack.
void lua_mesh_push_ack(lua_State* L, uint32_t ack, int32_t rtt) {
    lua_getglobal(L, "require");
    lua_pushstring(L, "lib/mesh/messages");
    if (lua_pcall(L, 1, 1, 0) != LUA_OK) {
        lua_pop(L, 1);
        return;
    }
    lua_getfield(L, -1, "__dispatch_ack");
    if (!lua_isfunction(L, -1)) {
        lua_pop(L, 2);
        return;
    }
    lua_pushinteger(L, (lua_Integer)ack);
    lua_pushinteger(L, (lua_Integer)rtt);
    if (lua_pcall(L, 2, 0, 0) != LUA_OK) {
        SLog.printf("__dispatch_ack failed: %s\n", lua_tostring(L, -1));
        lua_pop(L, 1);
    }
    lua_pop(L, 1); // pop module
}

void PunkMesh::store_message(const char* from, const char* text, uint32_t timestamp, uint8_t hops, bool direct) {
    int idx = (msg_head + msg_count) % MAX_MESSAGES;

    if (msg_count == MAX_MESSAGES) {
        // overwrite oldest
        idx = msg_head;
        msg_head = (msg_head + 1) % MAX_MESSAGES;
    } else {
        msg_count++;
    }

    strncpy(message_history[idx].from, from, sizeof(message_history[idx].from) - 1);
    strncpy(message_history[idx].text, text, sizeof(message_history[idx].text) - 1);
    message_history[idx].timestamp = timestamp;
    message_history[idx].hops = hops;
    message_history[idx].direct = direct;
}

// Meshcore...

// Believe it or not, this std C function is busted on some platforms!
static uint32_t _atoi(const char *sp)
{
    uint32_t n = 0;
    while (*sp && *sp >= '0' && *sp <= '9')
    {
        n *= 10;
        n += (*sp++ - '0');
    }
    return n;
}

const char *PunkMesh::getTypeName(uint8_t type) const
{
    if (type == ADV_TYPE_CHAT)
        return "Chat";
    if (type == ADV_TYPE_REPEATER)
        return "Repeater";
    if (type == ADV_TYPE_ROOM)
        return "Room";
    return "??"; // unknown
}

// Auto-add gate: return false to stop the base mesh from auto-adding a
// discovered contact of this type. Mirrors the MeshCore companion model —
// "auto-add all" (manual_add_contacts bit0 clear) adds every type; "auto-add
// selected" (bit0 set) adds only the types whose bit is set in autoadd_config
// (chat 0x02 / repeater 0x04 / room 0x08 / sensor 0x10). Only affects NEW
// adverts; already added contacts keep updating.
bool PunkMesh::shouldAutoAddContactType(uint8_t type) const
{
    if ((_prefs.manual_add_contacts & 0x01) == 0) return true;  // auto-add all
    switch (type) {
        case ADV_TYPE_CHAT:     return (_prefs.autoadd_config & 0x02) != 0;
        case ADV_TYPE_REPEATER: return (_prefs.autoadd_config & 0x04) != 0;
        case ADV_TYPE_ROOM:     return (_prefs.autoadd_config & 0x08) != 0;
        case ADV_TYPE_SENSOR:   return (_prefs.autoadd_config & 0x10) != 0;
        default:                return false;
    }
}


// Fixed-size binary contact record (little-endian, on-device). Mirrors the
// persisted fields the old TSV held. Used by BOTH the live store (slot = array
// index, in-place O(1) updates) and the archive (append-only). lastmod/sync_since
// are runtime-only (not persisted), matching the old format.
//   pubkey(32) name(32) type(1) flags(1) out_path_len(1) out_path(64)
//   last_advert_ts(4) gps_lat(4) gps_lon(4)  = 143 bytes
static const int CONTACT_REC = 143;

static void serialize_contact(const ContactInfo& c, uint8_t* b) {
    int p = 0;
    memcpy(b + p, c.id.pub_key, 32); p += 32;
    memset(b + p, 0, 32); strncpy((char*)(b + p), c.name, 31); p += 32;  // null-padded
    b[p++] = c.type;
    b[p++] = c.flags;
    b[p++] = c.out_path_len;
    memcpy(b + p, c.out_path, 64); p += 64;
    memcpy(b + p, &c.last_advert_timestamp, 4); p += 4;
    memcpy(b + p, &c.gps_lat, 4); p += 4;
    memcpy(b + p, &c.gps_lon, 4); p += 4;
}

static void deserialize_contact(const uint8_t* b, ContactInfo& c) {
    memset(&c, 0, sizeof(c));
    int p = 0;
    uint8_t pk[32]; memcpy(pk, b + p, 32); p += 32;
    c.id = mesh::Identity(pk);
    memcpy(c.name, b + p, 32); c.name[31] = '\0'; p += 32;
    c.type = b[p++];
    c.flags = b[p++];
    c.out_path_len = b[p++];
    memcpy(c.out_path, b + p, 64); p += 64;
    memcpy(&c.last_advert_timestamp, b + p, 4); p += 4;
    memcpy(&c.gps_lat, b + p, 4); p += 4;
    memcpy(&c.gps_lon, b + p, 4); p += 4;
    c.lastmod = 0;
    c.shared_secret_valid = false;
}

void PunkMesh::loadContacts()
{
    bool is_sd = (_storage != &LittleFS);
    if (is_sd) sd_spi_take();

    String path = storagePath(_storage_prefix, "/contacts.bin");
    if (_storage->exists(path.c_str()))
    {
        File file = _storage->open(path.c_str());
        if (file)
        {
            uint8_t rec[CONTACT_REC];
            while (file.available() >= CONTACT_REC)
            {
                if (file.read(rec, CONTACT_REC) != CONTACT_REC) break;
                ContactInfo c;
                deserialize_contact(rec, c);
                if (!addContact(c)) break;  // live table full
            }
            file.close();
        }
    }

    if (is_sd) sd_spi_release();
}

// Full rewrite of the live store, in ARRAY-INDEX order (getContactByIdx, so file
// slot i == contacts[i]). Used on removal/clear (the array compacts) and as the
// bulk fallback. Frequent single-contact mutations use saveOneContact instead.
void PunkMesh::saveContacts()
{
    contacts_generation++;  // invalidate the Lua-side contacts cache

    bool is_sd = (_storage != &LittleFS);
    if (is_sd) sd_spi_take();

    String path = storagePath(_storage_prefix, "/contacts.bin");
    File file = _storage->open(path.c_str(), "w", true);
    if (file)
    {
        uint8_t rec[CONTACT_REC];
        int n = getNumContacts();
        for (int i = 0; i < n; i++)
        {
            ContactInfo c;
            if (!getContactByIdx(i, c)) break;
            serialize_contact(c, rec);
            file.write(rec, CONTACT_REC);

            if (is_sd && (i + 1) % 50 == 0) {
                file.flush();
                sd_spi_release();
                vTaskDelay(1);
                sd_spi_take();
            }
        }
        file.close();
    }

    if (is_sd) sd_spi_release();
}

// O(1) single-contact persist: find c's array index, seek to that slot, write
// just its record (~1 sector vs the whole file). Used by the frequent mutation
// paths (advert, path update, favorite, re-add). Falls back to a full write if
// the contact isn't found or the file doesn't exist yet.
void PunkMesh::saveOneContact(const ContactInfo& c)
{
    contacts_generation++;
    int idx = -1, n = getNumContacts();
    for (int i = 0; i < n; i++) {
        ContactInfo tmp;
        if (getContactByIdx(i, tmp) &&
            memcmp(tmp.id.pub_key, c.id.pub_key, PUB_KEY_SIZE) == 0) { idx = i; break; }
    }
    if (idx < 0) { saveContacts(); return; }

    bool is_sd = (_storage != &LittleFS);
    if (is_sd) sd_spi_take();
    String path = storagePath(_storage_prefix, "/contacts.bin");
    File f = _storage->open(path.c_str(), "r+");
    if (!f) { if (is_sd) sd_spi_release(); saveContacts(); return; }  // no file yet
    uint8_t rec[CONTACT_REC];
    serialize_contact(c, rec);
    f.seek((uint32_t)idx * CONTACT_REC);
    f.write(rec, CONTACT_REC);
    f.close();
    if (is_sd) sd_spi_release();
}

// ── Contact archive ──────────────────────────────────────────────────
// DISK-ONLY: the archive lives only in the <storage>/contacts_arch append-only
// log — no RAM array, no cap. See punkmesh.h. Read on demand (transient) for
// the "show archived" map union and re-add.

// Read the log into `out` (deduped — the newest line per pubkey wins, since the
// file is append-order), up to max_out entries; returns the count. The whole
// archive stays on disk regardless of max_out — only the on-map display is
// bounded so the transient buffer can't blow up PSRAM on a huge mesh.
int PunkMesh::readArchivedDeduped(ContactInfo* out, int max_out)
{
    int n = 0;
    bool is_sd = (_storage != &LittleFS);
    if (is_sd) sd_spi_take();

    String path = storagePath(_storage_prefix, "/contacts_arch.bin");
    if (_storage->exists(path.c_str()))
    {
        File file = _storage->open(path.c_str());
        if (file)
        {
            uint8_t rec[CONTACT_REC];
            ContactInfo entry;
            int cnt = 0;
            while (file.available() >= CONTACT_REC)
            {
                if (file.read(rec, CONTACT_REC) != CONTACT_REC) break;
                deserialize_contact(rec, entry);

                int slot = -1;
                for (int i = 0; i < n; i++) {
                    if (memcmp(out[i].id.pub_key, entry.id.pub_key, PUB_KEY_SIZE) == 0) {
                        slot = i;
                        break;
                    }
                }
                if (slot >= 0) out[slot] = entry;        // newer record supersedes
                else if (n < max_out) out[n++] = entry;  // display cap; disk keeps all

                if (is_sd && ++cnt % 200 == 0) { sd_spi_release(); vTaskDelay(1); sd_spi_take(); }
            }
            file.close();
        }
    }

    if (is_sd) sd_spi_release();
    return n;
}

int PunkMesh::readArchiveBatch(uint32_t offset, int max_count, ContactInfo* out,
                               uint32_t* next_offset, bool* done)
{
    int n = 0;
    *done = true;
    *next_offset = offset;
    bool is_sd = (_storage != &LittleFS);
    if (is_sd) sd_spi_take();

    String path = storagePath(_storage_prefix, "/contacts_arch.bin");
    if (_storage->exists(path.c_str()))
    {
        File file = _storage->open(path.c_str());
        if (file)
        {
            if (offset > 0) file.seek(offset);
            uint8_t rec[CONTACT_REC];
            ContactInfo entry;
            while (n < max_count && file.available() >= CONTACT_REC)
            {
                if (file.read(rec, CONTACT_REC) != CONTACT_REC) break;
                deserialize_contact(rec, entry);
                out[n++] = entry;
            }
            *next_offset = (uint32_t)file.position();
            *done = (file.available() < CONTACT_REC);
            file.close();
        }
    }

    if (is_sd) sd_spi_release();
    return n;
}

// Hot-path persistence: append ONE binary contact record (CONTACT_REC bytes).
// Keeping the archive append-only (vs rewriting) is what keeps eviction during
// advert storms cheap on the shared SD/TFT SPI bus. Duplicates (re-archived
// pubkeys) are resolved newest-wins when the log is read.
void PunkMesh::appendArchiveEntry(const ContactInfo& c)
{
    archive_generation++;  // invalidate the Lua-side union cache
    bool is_sd = (_storage != &LittleFS);
    if (is_sd) sd_spi_take();

    String path = storagePath(_storage_prefix, "/contacts_arch.bin");
    File file = _storage->open(path.c_str(), "a", true);
    if (file) {
        uint8_t rec[CONTACT_REC];
        serialize_contact(c, rec);
        file.write(rec, CONTACT_REC);
        file.close();
    }

    if (is_sd) sd_spi_release();
}

void PunkMesh::archiveContact(const ContactInfo& c)
{
    if (!_prefs.archive_contacts) return;  // archiving disabled by setting
    // Disk-only: just append. A re-archived pubkey gets a fresh line that
    // supersedes the old one on read (newest-wins) — no RAM lookup, no cap.
    appendArchiveEntry(c);
    SLog.printf("[ARCH] Archived contact: %s\n", c.name);
}

bool PunkMesh::readdArchivedContact(const uint8_t* pub_key)
{
    // Scan the binary log for this pubkey's newest record (fixed stride, no parse).
    ContactInfo found;
    bool have = false;
    bool is_sd = (_storage != &LittleFS);
    if (is_sd) sd_spi_take();

    String path = storagePath(_storage_prefix, "/contacts_arch.bin");
    if (_storage->exists(path.c_str()))
    {
        File file = _storage->open(path.c_str());
        if (file)
        {
            uint8_t rec[CONTACT_REC];
            ContactInfo entry;
            int cnt = 0;
            while (file.available() >= CONTACT_REC)
            {
                if (file.read(rec, CONTACT_REC) != CONTACT_REC) break;
                deserialize_contact(rec, entry);
                if (memcmp(entry.id.pub_key, pub_key, PUB_KEY_SIZE) == 0) {
                    found = entry;  // keep scanning — the last match is newest
                    have = true;
                }
                if (is_sd && ++cnt % 200 == 0) { sd_spi_release(); vTaskDelay(1); sd_spi_take(); }
            }
            file.close();
        }
    }

    if (is_sd) sd_spi_release();
    if (!have) return false;

    // The stored route is stale by definition — rediscover via flood.
    found.out_path_len = OUT_PATH_UNKNOWN;
    memset(found.out_path, 0, sizeof(found.out_path));
    found.shared_secret_valid = false;
    found.lastmod = getRTCClock()->getCurrentTime();

    if (!addContact(found)) {
        // live table full (and overwrite disabled, or all favorites)
        return false;
    }

    // No need to rewrite the log: the contact is live now, and the map union
    // skips contacts that are live, so the stale archive line is harmless (and
    // is superseded by a fresh line if it's ever evicted again).
    archive_generation++;  // drop the now-live entry from the union cache
    saveOneContact(found);
    SLog.printf("[ARCH] Re-added contact: %s\n", found.name);
    return true;
}

void PunkMesh::onContactOverwrite(const uint8_t* pub_key)
{
    // Called just before the slot is reused — the contact data is intact.
    ContactInfo* c = lookupContactByPubKey(pub_key, PUB_KEY_SIZE);
    if (c) {
        SLog.printf("[ARCH] Live table full — archiving evicted contact: %s\n", c->name);
        archiveContact(*c);
    }
}

void PunkMesh::loadChannels()
{
    bool is_sd = (_storage != &LittleFS);
    if (is_sd) sd_spi_take();

    String path = storagePath(_storage_prefix, "/channels");
    if (_storage->exists(path.c_str()))
    {
        File file = _storage->open(path.c_str());
        if (file)
        {
            char line[128];
            while (file.available())
            {
                int len = 0;
                while (file.available() && len < (int)sizeof(line) - 1) {
                    char ch = file.read();
                    if (ch == '\n' || ch == '\r') break;
                    line[len++] = ch;
                }
                line[len] = '\0';
                if (len == 0) continue;

                // Format: slot_idx \t name \t secret_hex
                char *fields[3];
                int nf = 0;
                fields[0] = line;
                for (int i = 0; i < len && nf < 2; i++) {
                    if (line[i] == '\t') {
                        line[i] = '\0';
                        fields[++nf] = &line[i + 1];
                    }
                }
                if (nf < 2) continue;

                int slot = atoi(fields[0]);
                if (slot < 1 || slot >= MAX_GROUP_CHANNELS) continue;

                ChannelDetails cd;
                memset(&cd, 0, sizeof(cd));
                strncpy(cd.name, fields[1], sizeof(cd.name) - 1);
                mesh::Utils::fromHex(cd.channel.secret, 32, fields[2]);
                setChannel(slot, cd);
                SLog.printf("[MESH INIT] Restored channel[%d]: %s\n", slot, cd.name);
            }
            file.close();
        }
    }

    if (is_sd) sd_spi_release();
}

void PunkMesh::saveChannels()
{
    bool is_sd = (_storage != &LittleFS);
    if (is_sd) sd_spi_take();

    String path = storagePath(_storage_prefix, "/channels");
    File file = _storage->open(path.c_str(), "w", true);
    if (file)
    {
        for (int i = 1; i < MAX_GROUP_CHANNELS; i++) {
            ChannelDetails cd;
            getChannel(i, cd);
            if (cd.name[0] == '\0') continue;

            char secret_hex[65];
            mesh::Utils::toHex(secret_hex, cd.channel.secret, 32);
            file.printf("%d\t%s\t%s\n", i, cd.name, secret_hex);
        }
        file.close();
    }

    if (is_sd) sd_spi_release();
}

// ══════════════════════════════════════════════════════════════════
// Persistent message history
// ══════════════════════════════════════════════════════════════════

void PunkMesh::setMaxMessages(int n) {
    if (n > 0 && n <= 5000) _max_messages = n;
}

// Normalize UTF-8 smart quotes to ASCII equivalents. See punkmesh.h for
// rationale — montserrat_14 doesn't cover U+2018-U+201D, so leaving them
// in place renders tofu in LVGL labels.
size_t normalize_smart_quotes(const char* in, char* out, size_t outlen) {
    if (outlen == 0) return 0;
    if (!in) { out[0] = '\0'; return 0; }

    size_t w = 0;
    size_t i = 0;
    while (in[i] && w + 1 < outlen) {
        unsigned char c0 = (unsigned char)in[i];
        // Smart quotes are U+2018..U+201D, encoded as 0xE2 0x80 0x98..0x9D.
        if (c0 == 0xE2 && in[i+1] && in[i+2]) {
            unsigned char c1 = (unsigned char)in[i+1];
            unsigned char c2 = (unsigned char)in[i+2];
            if (c1 == 0x80 && (c2 == 0x98 || c2 == 0x99)) {
                out[w++] = '\'';
                i += 3;
                continue;
            }
            if (c1 == 0x80 && (c2 == 0x9C || c2 == 0x9D)) {
                out[w++] = '"';
                i += 3;
                continue;
            }
        }
        out[w++] = in[i++];
    }
    out[w] = '\0';
    return w;
}

// Sanitize a peer name into something safe for a filename fragment.
// Letters / digits / dash / underscore pass through; anything else → '_'.
// Capped at 32 chars so the filename stays short.
static void sanitize_peer_name(const char* in, char* out, size_t outlen) {
    if (outlen == 0) return;
    size_t n = 0;
    for (size_t i = 0; in && in[i] && n + 1 < outlen; i++) {
        char c = in[i];
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '-' || c == '_';
        out[n++] = ok ? c : '_';
    }
    out[n] = '\0';
    if (n == 0) {
        // All-invalid input (e.g. pure emoji); give it a placeholder
        // to avoid empty filenames.
        const char* fb = "peer";
        for (size_t i = 0; i < 4 && i + 1 < outlen; i++) out[i] = fb[i];
        out[4 < outlen ? 4 : outlen - 1] = '\0';
    }
}

// Directory + path helpers. Storage follows _storage_prefix so SD installs
// get "/meshpunk/messages/..." and LittleFS gets "/messages/...".
static String messages_dir(const String& prefix) {
    return prefix + "/messages";
}

static String channel_msg_path(const String& prefix, const char* ch_name) {
    char safe[33];
    sanitize_peer_name(ch_name, safe, sizeof(safe));
    return messages_dir(prefix) + "/ch_" + String(safe) + ".log";
}

// Resolve channel name for a given index. Falls back to "ch<idx>" if unnamed.
static String channel_name_for_idx(PunkMesh& mesh, int ch_idx) {
    ChannelDetails cd;
    if (mesh.getChannel(ch_idx, cd) && cd.name[0] != '\0')
        return String(cd.name);
    return String("ch") + String(ch_idx);
}

static String dm_msg_path(const String& prefix, const char* peer) {
    char safe[33];
    sanitize_peer_name(peer, safe, sizeof(safe));
    return messages_dir(prefix) + "/dm_" + String(safe) + ".log";
}

// Public wrappers so BLE companion can build paths for targeted sync
// without needing access to the static sanitize_peer_name helper.
String PunkMesh::channelMsgPath(int channel_idx) {
    String name = channel_name_for_idx(*this, channel_idx);
    return channel_msg_path(_storage_prefix, name.c_str());
}

String PunkMesh::dmMsgPath(const char* peer) {
    return dm_msg_path(_storage_prefix, peer);
}

static void ensure_messages_dir(fs::FS* fs, const String& prefix) {
    if (!fs) return;
    String dir = messages_dir(prefix);
    if (!fs->exists(dir.c_str())) {
        fs->mkdir(dir.c_str());
    }
}

static void fill_stored_msg(StoredMsg& m, int channel_idx, const char* from,
                            const char* peer, const char* text,
                            uint32_t timestamp, float snr, float rssi,
                            uint8_t hops, bool direct, bool is_dm,
                            uint16_t path_len = 0, const uint8_t* path_data = nullptr,
                            const uint8_t* pkt_hash = nullptr,
                            const uint8_t* pub_key = nullptr,
                            uint32_t sender_ts = 0) {
    memset(&m, 0, sizeof(m));
    m.timestamp   = timestamp;   // authoritative (our RX/send clock)
    m.sender_ts   = sender_ts;   // sender's claimed clock — recorded only
    m.snr         = snr;
    m.rssi        = rssi;
    m.channel_idx = (int8_t)channel_idx;
    m.hops        = hops;
    m.flags       = (direct ? 0x01 : 0x00) | (is_dm ? 0x02 : 0x00);
    if (from) strncpy(m.from, from, sizeof(m.from) - 1);
    if (peer) strncpy(m.peer, peer, sizeof(m.peer) - 1);
    if (text) strncpy(m.text, text, sizeof(m.text) - 1);
    m.path_len = path_len;
    if (path_data && path_len) {
        uint8_t byte_len = ((path_len & 63) * ((path_len >> 6) + 1));
        if (byte_len > MAX_PATH_SIZE) byte_len = MAX_PATH_SIZE;
        memcpy(m.path, path_data, byte_len);
    }
    if (pkt_hash) {
        memcpy(m.pkt_hash, pkt_hash, MAX_HASH_SIZE);
        m.has_hash = true;
    }
    if (pub_key) {
        memcpy(m.sender_pub_key, pub_key, 6);
        m.has_pub_key = true;
    }
    // Stamp our last known GPS location (if any). Omitted entirely when there's
    // no fix, so those records save exactly like before.
    double glat, glon;
    if (meshpunk_gps_last_fix(&glat, &glon)) {
        m.lat = glat;
        m.lon = glon;
        m.has_loc = true;
    }
}

// Push path hashes as a Lua table of hex strings.
static void push_path_table(lua_State* L, uint16_t path_len, const uint8_t* path) {
    lua_newtable(L);
    uint8_t hash_size = (path_len >> 6) + 1;
    uint8_t hash_count = path_len & 63;
    char hex[9];  // up to 4-byte hashes (8 hex chars + NUL)
    for (int j = 0; j < hash_count && (j + 1) * hash_size <= MAX_PATH_SIZE; j++) {
        mesh::Utils::toHex(hex, &path[j * hash_size], hash_size);
        lua_pushstring(L, hex);
        lua_rawseti(L, -2, j + 1);
    }
}

// ── Text-format message persistence ──────────────────────────────
// Each record is a block of key=value lines terminated by "---".
// Unknown keys are ignored on load (forward-compatible).

static bool is_old_binary_file(fs::FS* storage, const String& path) {
    File f = storage->open(path.c_str(), "r");
    if (!f || f.size() == 0) { if (f) f.close(); return false; }
    uint8_t first;
    f.read(&first, 1);
    f.close();
    return !(first >= 0x20 && first <= 0x7E) && first != '\n' && first != '\r';
}

static void write_path_field(File& f, uint16_t path_len, const uint8_t* path) {
    uint8_t hash_size = (path_len >> 6) + 1;
    uint8_t hash_count = path_len & 63;
    if (hash_count == 0) return;
    f.print("path=");
    char hex[9];  // up to 4-byte hashes
    for (int j = 0; j < hash_count && (j + 1) * hash_size <= MAX_PATH_SIZE; j++) {
        if (j > 0) f.print(",");
        mesh::Utils::toHex(hex, &path[j * hash_size], hash_size);
        f.print(hex);
    }
    f.print("\n");
}

static void write_rpath_line(File& f, uint16_t path_len, const uint8_t* path,
                             float snr, float rssi, bool is_direct) {
    uint8_t hash_size = (path_len >> 6) + 1;
    uint8_t hash_count = path_len & 63;
    f.print("rpath=");
    char hex[9];  // up to 4-byte hashes
    for (int j = 0; j < hash_count && (j + 1) * hash_size <= MAX_PATH_SIZE; j++) {
        if (j > 0) f.print(",");
        mesh::Utils::toHex(hex, &path[j * hash_size], hash_size);
        f.print(hex);
    }
    f.printf(";%.2f;%.2f;%d\n", snr, rssi, is_direct ? 1 : 0);
}

static int count_text_records(File& f) {
    f.seek(0);
    int count = 0;
    char line[256];
    while (f.available()) {
        int len = 0;
        while (f.available() && len < (int)sizeof(line) - 1) {
            char ch = f.read();
            if (ch == '\n' || ch == '\r') break;
            line[len++] = ch;
        }
        line[len] = '\0';
        if (len == 3 && line[0] == '-' && line[1] == '-' && line[2] == '-') count++;
    }
    return count;
}

// Per-append safety valve only — days-based retention is handled by the periodic
// age prune (prune_msg_file_by_age via pruneStep). Gated on file SIZE so the common
// path is an O(1) size check (no per-append record-count scan); only a pathologically large
// file (one runaway day) gets rewritten, keeping the newest records.
static const size_t MSG_FILE_SAFETY_BYTES = 768 * 1024;  // ~3000 msgs before it acts
static const int    MSG_FILE_SAFETY_KEEP  = 2000;
static void trim_msg_text_file(fs::FS* storage, const String& path, int cap) {
    (void)cap;  // retention is by days now; this is just a size-gated backstop
    if (!storage) return;
    File f = storage->open(path.c_str(), "r");
    if (!f) return;
    if ((size_t)f.size() < MSG_FILE_SAFETY_BYTES) { f.close(); return; }
    int count = count_text_records(f);
    int keep = MSG_FILE_SAFETY_KEEP;
    int max_count = keep + 100;
    if (count <= max_count) { f.close(); return; }
    int skip = count - keep;
    f.seek(0);
    char line[256];
    int skipped = 0;
    while (f.available() && skipped < skip) {
        int len = 0;
        while (f.available() && len < (int)sizeof(line) - 1) {
            char ch = f.read();
            if (ch == '\n' || ch == '\r') break;
            line[len++] = ch;
        }
        line[len] = '\0';
        if (len == 3 && line[0] == '-' && line[1] == '-' && line[2] == '-') skipped++;
    }

    size_t keep_start = f.position();
    size_t remaining = f.size() - keep_start;
    uint8_t* buf = (uint8_t*)malloc(remaining);
    if (!buf) { f.close(); return; }
    size_t got = f.read(buf, remaining);
    f.close();

    String tmp = path + ".tmp";
    File wf = storage->open(tmp.c_str(), "w", true);
    if (wf) {
        wf.write(buf, got);
        wf.close();
        storage->remove(path.c_str());
        if (!storage->rename(tmp.c_str(), path.c_str())) {
            SLog.printf("[STORAGE] rename failed: %s -> %s\n",
                          tmp.c_str(), path.c_str());
        }
    }
    free(buf);
}

static void append_msg_text(fs::FS* storage, const String& prefix,
                            const String& fpath, const StoredMsg& m, int cap) {
    if (!storage) return;
    bool is_sd = (storage != &LittleFS);
    if (is_sd) sd_spi_take();
    ensure_messages_dir(storage, prefix);

    if (storage->exists(fpath.c_str()) && is_old_binary_file(storage, fpath)) {
        storage->remove(fpath.c_str());
        SLog.printf("[STORAGE] Cleared old binary log: %s\n", fpath.c_str());
    }

    File f = storage->open(fpath.c_str(), "a", true);
    if (f) {
        f.printf("ts=%u\n", m.timestamp);          // authoritative (our RX/send clock)
        if (m.sender_ts) f.printf("sender_ts=%u\n", m.sender_ts);  // sender's claimed clock
        f.printf("from=%s\n", m.from);
        if (m.peer[0]) f.printf("peer=%s\n", m.peer);
        f.printf("text=%s\n", m.text);
        f.printf("ch=%d\n", (int)m.channel_idx);
        f.printf("hops=%d\n", (int)m.hops);
        f.printf("snr=%.2f\n", m.snr);
        f.printf("rssi=%.2f\n", m.rssi);
        f.printf("direct=%d\n", (m.flags & 0x01) ? 1 : 0);
        f.printf("dm=%d\n", (m.flags & 0x02) ? 1 : 0);
        write_path_field(f, m.path_len, m.path);
        if (m.has_hash) {
            char hash_hex[MAX_HASH_SIZE * 2 + 1];
            mesh::Utils::toHex(hash_hex, m.pkt_hash, MAX_HASH_SIZE);
            f.printf("hash=%s\n", hash_hex);
            write_rpath_line(f, m.path_len, m.path, m.snr, m.rssi,
                             (m.flags & 0x01) != 0);
        }
        if (m.has_pub_key) {
            char pk_hex[13];
            mesh::Utils::toHex(pk_hex, m.sender_pub_key, 6);
            f.printf("pubkey=%s\n", pk_hex);
        }
        if (m.has_loc) {
            f.printf("lat=%.6f\n", m.lat);
            f.printf("lon=%.6f\n", m.lon);
        }
        f.print("---\n");
        f.close();
    }
    trim_msg_text_file(storage, fpath, cap);
    if (is_sd) sd_spi_release();
}

// ── Routing store (daily, sender-indexed) ───────────────────────────────────
// A compact, denormalized projection of channel traffic for meshprint/replay:
// one tiny binary record per channel message — { from, ts, lat, lon, path } — in
// a per-day log, with a per-day {sender,offset} index for fast sender lookup. No
// message text or channel; DMs excluded. Daily files make retention a file
// delete (no rewrite) and give a free time index. On-device little-endian.
//   <prefix>/route/YYYY-MM-DD.log : u16 reclen | u8 from_len | from |
//                                   u32 ts | i32 lat_e6 | i32 lon_e6 |
//                                   u16 path_len | u8 path_nbytes | path
//   <prefix>/route/YYYY-MM-DD.idx : u8 from_len | from | u32 offset
static String route_dir(const String& prefix) { return prefix + "/route"; }

static void route_date_str(uint32_t ts, char* out /* >= 11 bytes */) {
    // RTClib's DateTime(uint32_t) subtracts SECONDS_FROM_1970_TO_2000, so any ts
    // below that offset underflows into the far future (~2106). Bucket those under
    // a sentinel that sorts before every real date instead of misdating them.
    if (ts < 946684800u) { strcpy(out, "0000-00-00"); return; }  // SECONDS_FROM_1970_TO_2000
    DateTime dt = DateTime(ts);
    snprintf(out, 11, "%04d-%02d-%02d", dt.year(), dt.month(), dt.day());
}

// Delete route .log/.idx whose date is older than retain_days. Lexicographic
// compare works since YYYY-MM-DD sorts chronologically. 0 = keep all. Caller
// holds the SD lock; runs once per day (first record of a new day file).
static void prune_routing_logs(fs::FS* storage, const String& prefix, uint32_t cutoff_ts) {
    if (!storage || cutoff_ts == 0) return;
    char cutoff[11];
    route_date_str(cutoff_ts, cutoff);

    String dir = route_dir(prefix);
    File root = storage->open(dir.c_str());
    if (!root || !root.isDirectory()) { if (root) root.close(); return; }

    // Collect victims first; deleting during openNextFile iteration is unsafe.
    String victims[64];
    int nv = 0;
    File entry = root.openNextFile();
    while (entry && nv < 64) {
        if (!entry.isDirectory()) {
            String name = entry.name();
            int slash = name.lastIndexOf('/');
            String base = (slash >= 0) ? name.substring(slash + 1) : name;
            if ((base.endsWith(".log") || base.endsWith(".idx")) && base.length() >= 10
                && base.substring(0, 10) < String(cutoff)) {
                victims[nv++] = dir + "/" + base;
            }
        }
        entry = root.openNextFile();
    }
    root.close();
    for (int i = 0; i < nv; i++) {
        storage->remove(victims[i].c_str());
        SLog.printf("[ROUTE] pruned %s\n", victims[i].c_str());
    }
}

// Age-prune one text-message log: rewrite it keeping only records with ts >=
// cutoff. Records ("ts=..\n..\n---\n") are append/oldest-first, so we find the
// first keeper's byte offset and copy the tail (chunked — no big buffer). Caller
// holds the SD lock. .tmp + rename keeps it crash-safe like the count trim.
static void prune_msg_file_by_age(fs::FS* storage, const String& path, uint32_t cutoff_ts) {
    if (!storage || cutoff_ts == 0) return;
    File f = storage->open(path.c_str(), "r");
    if (!f) return;

    long keep_start = -1;
    long rec_start = 0;
    uint32_t cur_ts = 0;
    char line[256];
    while (f.available()) {
        int len = 0;
        while (f.available() && len < (int)sizeof(line) - 1) {
            char ch = f.read();
            if (ch == '\n' || ch == '\r') break;
            line[len++] = ch;
        }
        line[len] = '\0';
        if (len == 3 && line[0] == '-' && line[1] == '-' && line[2] == '-') {
            if (cur_ts >= cutoff_ts) { keep_start = rec_start; break; }
            rec_start = f.position();   // next record begins after this terminator
            cur_ts = 0;
        } else if (strncmp(line, "ts=", 3) == 0) {
            cur_ts = strtoul(line + 3, nullptr, 10);
        }
    }
    if (keep_start < 0) keep_start = f.size();   // every record older than cutoff
    if (keep_start == 0) { f.close(); return; }  // nothing to drop

    f.seek(keep_start);
    String tmp = path + ".tmp";
    File wf = storage->open(tmp.c_str(), "w", true);
    if (!wf) { f.close(); return; }
    uint8_t chunk[512];
    while (f.available()) {
        int n = f.read(chunk, sizeof(chunk));
        if (n <= 0) break;
        wf.write(chunk, n);
    }
    wf.close();
    f.close();
    storage->remove(path.c_str());
    if (!storage->rename(tmp.c_str(), path.c_str()))
        SLog.printf("[STORAGE] age-prune rename failed: %s\n", path.c_str());
}

// Collect channel/DM text-log paths into `out` (up to `max`); returns the count.
// Caller holds the SD lock. Used by the incremental retention sweep (pruneStep),
// which then rewrites each file under its OWN lock so the bus is freed between
// files (never held for the whole multi-file sweep).
static int collect_message_logs(fs::FS* storage, const String& prefix,
                                String* out, int max) {
    int nf = 0;
    String dir = messages_dir(prefix);
    File root = storage->open(dir.c_str());
    if (!root || !root.isDirectory()) { if (root) root.close(); return 0; }
    File e = root.openNextFile();
    while (e && nf < max) {
        if (!e.isDirectory()) {
            String name = e.name();
            int slash = name.lastIndexOf('/');
            String base = (slash >= 0) ? name.substring(slash + 1) : name;
            if (base.endsWith(".log") && (base.startsWith("ch_") || base.startsWith("dm_")))
                out[nf++] = dir + "/" + base;
        }
        e = root.openNextFile();
    }
    root.close();
    return nf;
}

// Lock-free core: write one routing record (+idx) for `m`. CALLER HOLDS the SD
// lock. Returns true if this was the first record of a new day file. Shared by
// the live append path and the one-shot history backfill.
static bool route_write_record(fs::FS* storage, const String& prefix, const StoredMsg& m) {
    if (!storage) return false;
    if (m.timestamp < 86400) return false;   // no valid clock / bad ts — would misbucket
    if (m.from[0] == '\0') return false;

    char datestr[11];
    route_date_str(m.timestamp, datestr);
    String dir = route_dir(prefix);
    String logpath = dir + "/" + datestr + ".log";
    String idxpath = dir + "/" + datestr + ".idx";

    if (!storage->exists(dir.c_str())) storage->mkdir(dir.c_str());
    bool new_day = !storage->exists(logpath.c_str());

    // ── Record body (single write; on-device little-endian) ──
    uint8_t from_len = strlen(m.from);
    if (from_len > 31) from_len = 31;
    uint8_t path_nbytes = (uint8_t)((m.path_len & 63) * ((m.path_len >> 6) + 1));
    if (path_nbytes > MAX_PATH_SIZE) path_nbytes = MAX_PATH_SIZE;
    int32_t lat_e6 = m.has_loc ? (int32_t)(m.lat * 1000000.0 + (m.lat >= 0 ? 0.5 : -0.5)) : 0;
    int32_t lon_e6 = m.has_loc ? (int32_t)(m.lon * 1000000.0 + (m.lon >= 0 ? 0.5 : -0.5)) : 0;
    uint16_t body = (uint16_t)(1 + from_len + 4 + 4 + 4 + 2 + 1 + path_nbytes);

    uint8_t buf[2 + 1 + 31 + 4 + 4 + 4 + 2 + 1 + MAX_PATH_SIZE];
    int q = 0;
    memcpy(buf + q, &body, 2);            q += 2;
    buf[q++] = from_len;
    memcpy(buf + q, m.from, from_len);    q += from_len;
    memcpy(buf + q, &m.timestamp, 4);     q += 4;
    memcpy(buf + q, &lat_e6, 4);          q += 4;
    memcpy(buf + q, &lon_e6, 4);          q += 4;
    memcpy(buf + q, &m.path_len, 2);      q += 2;
    buf[q++] = path_nbytes;
    if (path_nbytes) { memcpy(buf + q, m.path, path_nbytes); q += path_nbytes; }

    uint32_t offset = 0;
    File lf = storage->open(logpath.c_str(), "a", true);
    if (lf) {
        offset = lf.size();   // append position = start of this record
        lf.write(buf, q);
        lf.close();
    }

    // Index record: {from_len, from, offset}
    uint8_t ibuf[1 + 31 + 4];
    int ip = 0;
    ibuf[ip++] = from_len;
    memcpy(ibuf + ip, m.from, from_len);  ip += from_len;
    memcpy(ibuf + ip, &offset, 4);        ip += 4;
    File xf = storage->open(idxpath.c_str(), "a", true);
    if (xf) { xf.write(ibuf, ip); xf.close(); }

    return new_day;
}

// Locking wrapper for the live append path. new_day → caller flags a deferred
// retention sweep (run off this locked path on the device clock). See pruneStep.
static bool append_routing_record(fs::FS* storage, const String& prefix,
                                  const StoredMsg& m) {
    if (!storage) return false;
    bool is_sd = (storage != &LittleFS);
    if (is_sd) sd_spi_take();
    bool new_day = route_write_record(storage, prefix, m);
    if (is_sd) sd_spi_release();
    return new_day;
}

void PunkMesh::appendChannelMessage(int channel_idx, const char* from, const char* text,
                                    uint32_t timestamp, float snr, float rssi,
                                    uint8_t hops, bool direct,
                                    uint16_t path_len, const uint8_t* path,
                                    const uint8_t* pkt_hash, uint32_t sender_ts) {
    if (channel_idx < 0) return;
    StoredMsg m;
    fill_stored_msg(m, channel_idx, from, /*peer*/ "", text,
                    timestamp, snr, rssi, hops, direct, /*is_dm*/ false,
                    path_len, path, pkt_hash, /*pub_key*/ nullptr, sender_ts);
    String ch_name = channel_name_for_idx(*this, channel_idx);
    append_msg_text(_storage, _storage_prefix,
                    channel_msg_path(_storage_prefix, ch_name.c_str()),
                    m, _max_messages);
    // Compact routing projection for meshprint/replay (sender-indexed, daily).
    if (append_routing_record(_storage, _storage_prefix, m)) _prune_due = true;
}

static void str_tolower_buf(char* s) { for (; *s; ++s) if (*s >= 'A' && *s <= 'Z') *s += 32; }

// Decode one routing record at the file's current position; advances past it.
// Pushes {from,timestamp,lat,lon,path} at out_idx when the record matches the
// optional lowercased sender `want_lc` and ts is in [since,until] (0 = open).
// Returns: 1 pushed, 0 skipped (filtered, or internally malformed but framing
// intact — caller continues), -1 unrecoverable (EOF / lost framing — caller stops).
static int route_decode_and_push(lua_State* L, File& f, const char* want_lc,
                                 uint32_t since_ts, uint32_t until_ts, int out_idx) {
    uint16_t reclen = 0;
    if (f.read((uint8_t*)&reclen, 2) != 2) return -1;                // EOF
    if (reclen < (1 + 4 + 4 + 4 + 2 + 1) || reclen > 200) return -1; // framing lost
    uint8_t body[200];
    if (f.read(body, reclen) != (int)reclen) return -1;             // partial last record
    // Past this point exactly `reclen` bytes were consumed, so a record with a
    // bad interior can be SKIPPED (return 0) without losing later records' framing.
    int p = 0;
    uint8_t from_len = body[p++];
    if (from_len > 31 || p + from_len + 4 + 4 + 4 + 2 + 1 > reclen) return 0;
    char from[32];
    memcpy(from, body + p, from_len); from[from_len] = '\0'; p += from_len;
    uint32_t ts;       memcpy(&ts, body + p, 4);       p += 4;
    int32_t  lat_e6;   memcpy(&lat_e6, body + p, 4);   p += 4;
    int32_t  lon_e6;   memcpy(&lon_e6, body + p, 4);   p += 4;
    uint16_t path_len; memcpy(&path_len, body + p, 2); p += 2;
    uint8_t  path_nbytes = body[p++];
    if (p + path_nbytes > reclen) return 0;

    if (want_lc && want_lc[0]) {
        char fl[32]; strncpy(fl, from, sizeof(fl) - 1); fl[sizeof(fl) - 1] = '\0';
        str_tolower_buf(fl);
        if (strcmp(fl, want_lc) != 0) return 0;
    }
    if (since_ts && ts < since_ts) return 0;
    if (until_ts && ts > until_ts) return 0;

    lua_newtable(L);
    lua_pushstring(L, from);                lua_setfield(L, -2, "from");
    lua_pushinteger(L, ts);                 lua_setfield(L, -2, "timestamp");
    lua_pushnumber(L, lat_e6 / 1000000.0);  lua_setfield(L, -2, "lat");
    lua_pushnumber(L, lon_e6 / 1000000.0);  lua_setfield(L, -2, "lon");
    push_path_table(L, path_len, body + p); lua_setfield(L, -2, "path");
    lua_rawseti(L, -2, out_idx);
    return 1;
}

// Query the routing store. Day files in [since_date, until_date] are walked in
// directory order (caller/Lua sorts if it needs chronology). With a sender, the
// per-day .idx supplies offsets so non-matching records are never decoded and a
// day without the sender is skipped entirely. The .log handle IS the iterated
// dir entry, so only the .idx is opened separately. Output is capped to bound
// PSRAM (meshprint tallies; replay trims to its own max).
int PunkMesh::pushRoutingQuery(lua_State* L, const char* sender,
                               uint32_t since_ts, uint32_t until_ts) {
    lua_newtable(L);
    if (!_storage) return 1;
    bool is_sd = (_storage != &LittleFS);

    char want[32] = {0};
    bool filter_sender = sender && sender[0];
    if (filter_sender) { strncpy(want, sender, sizeof(want) - 1); str_tolower_buf(want); }

    // 0 means "open" on each side. Do NOT feed 0 to route_date_str: RTClib's
    // DateTime(uint32_t) subtracts the 1970→2000 offset, so 0 underflows to ~2106
    // and would exclude every real file. Use bound strings that sort past any date.
    char since_date[11], until_date[11];
    if (since_ts == 0) strcpy(since_date, "0000-00-00");
    else               route_date_str(since_ts, since_date);
    if (until_ts == 0) strcpy(until_date, "9999-99-99");
    else               route_date_str(until_ts, until_date);

    if (is_sd) sd_spi_take();
    String dir = route_dir(_storage_prefix);
    File root = _storage->open(dir.c_str());
    if (!root || !root.isDirectory()) {
        if (root) root.close();
        if (is_sd) sd_spi_release();
        return 1;
    }

    const int MAX_OUT = 2000;
    int out_idx = 1, produced = 0, yctr = 0;

    File entry = root.openNextFile();
    while (entry && produced < MAX_OUT) {
        if (!entry.isDirectory()) {
            String name = entry.name();
            int slash = name.lastIndexOf('/');
            String base = (slash >= 0) ? name.substring(slash + 1) : name;
            if (base.endsWith(".log") && base.length() >= 14) {   // YYYY-MM-DD.log
                String d = base.substring(0, 10);
                if (d >= String(since_date) && d <= String(until_date)) {
                    if (filter_sender) {
                        String idxpath = dir + "/" + d + ".idx";
                        File xf = _storage->open(idxpath.c_str(), "r");
                        if (xf) {
                            while (xf.available() && produced < MAX_OUT) {
                                uint8_t fl = 0;
                                if (xf.read(&fl, 1) != 1) break;
                                if (fl > 31) fl = 31;
                                char fn[32] = {0};
                                if (xf.read((uint8_t*)fn, fl) != fl) break;
                                fn[fl] = '\0';
                                uint32_t off = 0;
                                if (xf.read((uint8_t*)&off, 4) != 4) break;
                                str_tolower_buf(fn);
                                if (strcmp(fn, want) != 0) continue;
                                entry.seek(off);
                                if (route_decode_and_push(L, entry, want, since_ts, until_ts, out_idx) == 1) {
                                    out_idx++; produced++;
                                }
                                if (is_sd && (++yctr % 50 == 0)) { sd_spi_release(); vTaskDelay(1); sd_spi_take(); }
                            }
                            xf.close();
                        } else {
                            // No .idx (e.g. a crash between the .log and .idx writes):
                            // sequential scan filtering by sender so the day's records
                            // aren't silently dropped.
                            entry.seek(0);
                            while (entry.available() && produced < MAX_OUT) {
                                int r = route_decode_and_push(L, entry, want, since_ts, until_ts, out_idx);
                                if (r < 0) break;
                                if (r == 1) { out_idx++; produced++; }
                                if (is_sd && (++yctr % 50 == 0)) { sd_spi_release(); vTaskDelay(1); sd_spi_take(); }
                            }
                        }
                    } else {
                        entry.seek(0);
                        while (entry.available() && produced < MAX_OUT) {
                            int r = route_decode_and_push(L, entry, nullptr, since_ts, until_ts, out_idx);
                            if (r < 0) break;
                            if (r == 1) { out_idx++; produced++; }
                            if (is_sd && (++yctr % 50 == 0)) { sd_spi_release(); vTaskDelay(1); sd_spi_take(); }
                        }
                    }
                }
            }
        }
        entry = root.openNextFile();
    }
    root.close();
    if (is_sd) sd_spi_release();
    return 1;
}

// Distinct sender names from the routing-store index, matching the optional
// lowercased substring `query` (nullptr/"" = all). Built for the meshprint node
// search: the .idx files are {from_len, from, offset} per record, so we read just
// the names — never message bodies. Streams ONE .idx at a time and drops it before
// opening the next; only the bounded distinct-name set is held. Returns a Lua array
// of up to `max` original-case names. .idx files are tiny, so the whole sweep is a
// quick bounded read (no per-record yield needed).
int PunkMesh::pushRoutingSenders(lua_State* L, const char* query, int max) {
    lua_newtable(L);
    if (!_storage) return 1;
    bool is_sd = (_storage != &LittleFS);

    char want[32] = {0};
    bool filter = query && query[0];
    if (filter) { strncpy(want, query, sizeof(want) - 1); str_tolower_buf(want); }

    if (max <= 0 || max > 128) max = 64;
    const int NAMESZ = 32;
    char* seen = (char*)malloc((size_t)max * NAMESZ);   // distinct-name set (lowercased)
    if (!seen) return 1;
    int nseen = 0, out_idx = 1;

    if (is_sd) sd_spi_take();
    String dir = route_dir(_storage_prefix);
    File root = _storage->open(dir.c_str());
    if (!root || !root.isDirectory()) {
        if (root) root.close();
        if (is_sd) sd_spi_release();
        free(seen);
        return 1;
    }

    File entry = root.openNextFile();
    while (entry && nseen < max) {
        if (!entry.isDirectory()) {
            String name = entry.name();
            int slash = name.lastIndexOf('/');
            String base = (slash >= 0) ? name.substring(slash + 1) : name;
            if (base.endsWith(".idx")) {
                // Index record: u8 from_len | from | u32 offset.
                while (entry.available() && nseen < max) {
                    uint8_t fl = 0;
                    if (entry.read(&fl, 1) != 1) break;
                    if (fl > 31) fl = 31;
                    char fn[32] = {0};
                    if (entry.read((uint8_t*)fn, fl) != (int)fl) break;
                    fn[fl] = '\0';
                    uint32_t off;
                    if (entry.read((uint8_t*)&off, 4) != 4) break;  // advance past offset

                    char lc[32];
                    strncpy(lc, fn, sizeof(lc) - 1); lc[sizeof(lc) - 1] = '\0';
                    str_tolower_buf(lc);
                    if (filter && !strstr(lc, want)) continue;
                    bool dup = false;
                    for (int i = 0; i < nseen; i++) {
                        if (strcmp(seen + (size_t)i * NAMESZ, lc) == 0) { dup = true; break; }
                    }
                    if (dup) continue;
                    strncpy(seen + (size_t)nseen * NAMESZ, lc, NAMESZ - 1);
                    seen[(size_t)nseen * NAMESZ + NAMESZ - 1] = '\0';
                    nseen++;
                    lua_pushstring(L, fn);            // original case for display
                    lua_rawseti(L, -2, out_idx++);
                }
            }
        }
        entry = root.openNextFile();   // drop this file before the next
    }
    root.close();
    if (is_sd) sd_spi_release();
    free(seen);
    return 1;
}

// Incremental retention sweep, driven by _prune_due (set on a new-day routing
// record or at boot). Called from the Core-0 main loop. Phase 1 (first call
// after the flag) deletes old routing day-files and snapshots the text-log list;
// each later call rewrites ONE text log. Every disk step takes its own
// MESH_LOCK+SPI and releases it before returning, so the radio and UI are never
// blocked for more than a single file's rewrite (bounded by the size-safety cap).
void PunkMesh::pruneStep() {
    if (!_sweep_active) {
        if (!_prune_due) return;
        _prune_due = false;
        if (!_storage || _msg_retain_days == 0) return;  // unlimited: nothing to do
        uint32_t now_ts = getRTCClock()->getCurrentTime();
        if (now_ts < 86400) { _prune_due = true; return; }  // no clock yet — retry later
        _sweep_cutoff = (now_ts > (uint32_t)_msg_retain_days * 86400u)
                        ? now_ts - (uint32_t)_msg_retain_days * 86400u : 0;
        if (_sweep_cutoff == 0) return;
        bool is_sd = (_storage != &LittleFS);
        MESH_LOCK(); if (is_sd) sd_spi_take();
        prune_routing_logs(_storage, _storage_prefix, _sweep_cutoff);  // quick deletes
        _sweep_count = collect_message_logs(_storage, _storage_prefix, _sweep_files, 64);
        if (is_sd) sd_spi_release(); MESH_UNLOCK();
        _sweep_idx = 0;
        _sweep_active = (_sweep_count > 0);
        return;
    }
    if (_sweep_idx >= _sweep_count) { _sweep_active = false; return; }
    bool is_sd = (_storage != &LittleFS);
    MESH_LOCK(); if (is_sd) sd_spi_take();
    prune_msg_file_by_age(_storage, _sweep_files[_sweep_idx], _sweep_cutoff);
    if (is_sd) sd_spi_release(); MESH_UNLOCK();
    _sweep_files[_sweep_idx] = String();   // free the path now
    _sweep_idx++;
    if (_sweep_idx >= _sweep_count) _sweep_active = false;
}

void PunkMesh::appendDMMessage(const char* peer, const char* from, const char* text,
                               uint32_t timestamp, float snr, float rssi,
                               uint8_t hops, bool direct,
                               uint16_t path_len, const uint8_t* path,
                               const uint8_t* pkt_hash,
                               const uint8_t* sender_pub_key, uint32_t sender_ts) {
    if (!peer || peer[0] == '\0') return;
    StoredMsg m;
    fill_stored_msg(m, /*ch_idx*/ -1, from, peer, text,
                    timestamp, snr, rssi, hops, direct, /*is_dm*/ true,
                    path_len, path, pkt_hash, sender_pub_key, sender_ts);
    append_msg_text(_storage, _storage_prefix,
                    dm_msg_path(_storage_prefix, peer),
                    m, _max_messages);
}

static void push_stored_msg_table(lua_State* L, const StoredMsg& m) {
    lua_newtable(L);
    lua_pushstring(L, m.from);      lua_setfield(L, -2, "from");
    lua_pushstring(L, m.peer);      lua_setfield(L, -2, "peer");
    lua_pushstring(L, m.text);      lua_setfield(L, -2, "text");
    lua_pushinteger(L, m.timestamp); lua_setfield(L, -2, "timestamp");   // authoritative
    lua_pushinteger(L, m.sender_ts); lua_setfield(L, -2, "sender_ts");   // recorded extra
    lua_pushinteger(L, m.hops);      lua_setfield(L, -2, "hops");
    lua_pushnumber(L, m.snr);        lua_setfield(L, -2, "snr");
    lua_pushnumber(L, m.rssi);       lua_setfield(L, -2, "rssi");
    lua_pushboolean(L, (m.flags & 0x01) != 0); lua_setfield(L, -2, "direct");
    lua_pushboolean(L, (m.flags & 0x02) != 0); lua_setfield(L, -2, "is_dm");
    lua_pushinteger(L, m.channel_idx); lua_setfield(L, -2, "channel_idx");
    if (m.has_loc) {
        lua_pushnumber(L, m.lat); lua_setfield(L, -2, "lat");
        lua_pushnumber(L, m.lon); lua_setfield(L, -2, "lon");
    }
    push_path_table(L, m.path_len, m.path);
    lua_setfield(L, -2, "path");
    if (m.has_hash) {
        char hex[MAX_HASH_SIZE * 2 + 1];
        mesh::Utils::toHex(hex, m.pkt_hash, MAX_HASH_SIZE);
        lua_pushstring(L, hex);
        lua_setfield(L, -2, "hash");
    }
    if (m.rpath_count > 0) {
        lua_newtable(L);
        for (int i = 0; i < m.rpath_count; i++) {
            const ObservedPath& rp = m.rpaths[i];
            lua_newtable(L);
            push_path_table(L, rp.path_len, rp.path);
            lua_setfield(L, -2, "path");
            uint8_t hc = rp.path_len & 63;
            lua_pushinteger(L, hc);
            lua_setfield(L, -2, "hops");
            lua_pushboolean(L, rp.is_direct);
            lua_setfield(L, -2, "direct");
            lua_pushnumber(L, rp.snr);
            lua_setfield(L, -2, "snr");
            lua_pushnumber(L, rp.rssi);
            lua_setfield(L, -2, "rssi");
            lua_rawseti(L, -2, i + 1);
        }
        lua_setfield(L, -2, "rpaths");
    }
}

// Parse a "path=aa,bb,cc" value into path[] and return encoded path_len.
static uint16_t parse_path_field(const char* val, uint8_t* path_out) {
    if (!val || val[0] == '\0') return 0;
    uint8_t count = 0;
    const char* p = val;
    while (*p && count < 63) {
        const char* comma = strchr(p, ',');
        size_t tok_len = comma ? (size_t)(comma - p) : strlen(p);
        uint8_t hash_size = tok_len / 2;
        if (hash_size == 0 || hash_size > 4) break;
        // never write past the fixed path buffer (corrupt/oversized lines)
        if ((size_t)(count + 1) * hash_size > MAX_PATH_SIZE) break;
        for (size_t i = 0; i < hash_size; i++) {
            char byte_hex[3] = { p[i*2], p[i*2+1], '\0' };
            path_out[count * hash_size + i] = (uint8_t)strtoul(byte_hex, nullptr, 16);
        }
        count++;
        if (!comma) break;
        p = comma + 1;
    }
    if (count == 0) return 0;
    size_t first_tok_len = strchr(val, ',') ? (size_t)(strchr(val, ',') - val) : strlen(val);
    uint8_t hash_size = first_tok_len / 2;
    return ((hash_size - 1) << 6) | count;
}

static int read_msg_text_file(lua_State* L, fs::FS* storage, const String& fpath) {
    lua_newtable(L);
    if (!storage) return 1;
    bool is_sd = (storage != &LittleFS);
    if (is_sd) sd_spi_take();
    if (!storage->exists(fpath.c_str())) {
        if (is_sd) sd_spi_release();
        return 1;
    }
    File f = storage->open(fpath.c_str(), "r");
    if (!f) {
        if (is_sd) sd_spi_release();
        return 1;
    }

    int idx = 1;
    int line_count = 0;
    StoredMsg m;
    memset(&m, 0, sizeof(m));
    char line[256];

    while (f.available()) {
        int len = 0;
        while (f.available() && len < (int)sizeof(line) - 1) {
            char ch = f.read();
            if (ch == '\n' || ch == '\r') break;
            line[len++] = ch;
        }
        line[len] = '\0';
        if (len == 0) continue;

        // Yield every 100 lines to prevent task watchdog timeout on large log files
        if (is_sd && ++line_count % 100 == 0) {
            sd_spi_release();
            vTaskDelay(1);
            sd_spi_take();
        }

        if (len == 3 && line[0] == '-' && line[1] == '-' && line[2] == '-') {
            push_stored_msg_table(L, m);
            lua_rawseti(L, -2, idx++);
            memset(&m, 0, sizeof(m));
            continue;
        }

        char* eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        const char* key = line;
        const char* val = eq + 1;

        if (strcmp(key, "ts") == 0) m.timestamp = strtoul(val, nullptr, 10);
        else if (strcmp(key, "sender_ts") == 0) m.sender_ts = strtoul(val, nullptr, 10);
        else if (strcmp(key, "from") == 0) strncpy(m.from, val, sizeof(m.from) - 1);
        else if (strcmp(key, "peer") == 0) strncpy(m.peer, val, sizeof(m.peer) - 1);
        else if (strcmp(key, "text") == 0) strncpy(m.text, val, sizeof(m.text) - 1);
        else if (strcmp(key, "ch") == 0) m.channel_idx = (int8_t)atoi(val);
        else if (strcmp(key, "hops") == 0) m.hops = (uint8_t)atoi(val);
        else if (strcmp(key, "snr") == 0) m.snr = atof(val);
        else if (strcmp(key, "rssi") == 0) m.rssi = atof(val);
        else if (strcmp(key, "direct") == 0) { if (atoi(val)) m.flags |= 0x01; }
        else if (strcmp(key, "dm") == 0) { if (atoi(val)) m.flags |= 0x02; }
        else if (strcmp(key, "path") == 0) m.path_len = parse_path_field(val, m.path);
        else if (strcmp(key, "hash") == 0) {
            if (strlen(val) == MAX_HASH_SIZE * 2) {
                mesh::Utils::fromHex(m.pkt_hash, MAX_HASH_SIZE, val);
                m.has_hash = true;
            }
        }
        else if (strcmp(key, "pubkey") == 0 && strlen(val) == 12) {
            mesh::Utils::fromHex(m.sender_pub_key, 6, val);
            m.has_pub_key = true;
        }
        else if (strcmp(key, "lat") == 0) { m.lat = atof(val); m.has_loc = true; }
        else if (strcmp(key, "lon") == 0) { m.lon = atof(val); m.has_loc = true; }
        else if (strcmp(key, "rpath") == 0 && m.rpath_count < MAX_PATHS_PER_MSG) {
            // Format: hop_hashes;snr;rssi;direct
            char rval[256];
            strncpy(rval, val, sizeof(rval) - 1);
            rval[sizeof(rval) - 1] = '\0';
            char* semi1 = strchr(rval, ';');
            if (semi1) {
                *semi1 = '\0';
                ObservedPath& rp = m.rpaths[m.rpath_count];
                memset(&rp, 0, sizeof(rp));
                rp.path_len = parse_path_field(rval, rp.path);
                char* semi2 = strchr(semi1 + 1, ';');
                if (semi2) {
                    *semi2 = '\0';
                    rp.snr = atof(semi1 + 1);
                    char* semi3 = strchr(semi2 + 1, ';');
                    if (semi3) {
                        *semi3 = '\0';
                        rp.rssi = atof(semi2 + 1);
                        rp.is_direct = atoi(semi3 + 1) != 0;
                    } else {
                        rp.rssi = atof(semi2 + 1);
                    }
                } else {
                    rp.snr = atof(semi1 + 1);
                }
                m.rpath_count++;
            }
        }
    }
    f.close();
    if (is_sd) sd_spi_release();
    return 1;
}

int PunkMesh::pushChannelMessagesToLua(lua_State* L, int channel_idx) {
    if (channel_idx < 0) { lua_newtable(L); return 1; }
    String ch_name = channel_name_for_idx(*this, channel_idx);
    return read_msg_text_file(L, _storage, channel_msg_path(_storage_prefix, ch_name.c_str()));
}

int PunkMesh::pushDMMessagesToLua(lua_State* L, const char* peer) {
    if (!peer || peer[0] == '\0') { lua_newtable(L); return 1; }
    return read_msg_text_file(L, _storage, dm_msg_path(_storage_prefix, peer));
}

// Enumerate dm_*.log files and return an array of the real peer names
// (recovered from the `peer` field stored inside the file, not the
// sanitized filename).
int PunkMesh::pushDMThreadNamesToLua(lua_State* L) {
    lua_newtable(L);
    if (!_storage) return 1;

    bool is_sd = (_storage != &LittleFS);
    if (is_sd) sd_spi_take();

    String dir = messages_dir(_storage_prefix);
    File root = _storage->open(dir.c_str());
    if (!root || !root.isDirectory()) {
        if (is_sd) sd_spi_release();
        return 1;
    }

    int idx = 1;
    int iter = 0;
    File entry = root.openNextFile();
    while (entry) {
        if (!entry.isDirectory()) {
            String name = entry.name();
            // Basename check: endsWith(".log") and contains "dm_"
            int slash = name.lastIndexOf('/');
            String base = (slash >= 0) ? name.substring(slash + 1) : name;
            if (base.startsWith("dm_") && base.endsWith(".log")) {
                char line[256];
                char peer[32] = {0};
                while (entry.available()) {
                    int len = 0;
                    while (entry.available() && len < (int)sizeof(line) - 1) {
                        char ch = entry.read();
                        if (ch == '\n' || ch == '\r') break;
                        line[len++] = ch;
                    }
                    line[len] = '\0';
                    if (len == 0) continue;
                    if (len == 3 && line[0] == '-' && line[1] == '-' && line[2] == '-') break;
                    if (strncmp(line, "peer=", 5) == 0) {
                        strncpy(peer, line + 5, sizeof(peer) - 1);
                        break;
                    }
                }
                if (peer[0] != '\0') {
                    lua_pushstring(L, peer);
                    lua_rawseti(L, -2, idx++);
                }
            }
        }
        // Yield every 10 files to prevent task watchdog timeout
        if (is_sd && ++iter % 10 == 0) {
            sd_spi_release();
            vTaskDelay(1);
            sd_spi_take();
        }
        entry = root.openNextFile();
    }
    root.close();
    if (is_sd) sd_spi_release();
    return 1;
}

int PunkMesh::enumerateMessageFiles(char paths[][MAX_SYNC_PATH_LEN], int max_paths) {
    if (!_storage || max_paths <= 0) return 0;
    bool is_sd = (_storage != &LittleFS);
    if (is_sd) sd_spi_take();

    String dir = messages_dir(_storage_prefix);
    File root = _storage->open(dir.c_str());
    if (!root || !root.isDirectory()) {
        if (is_sd) sd_spi_release();
        return 0;
    }

    int count = 0;
    File entry = root.openNextFile();
    while (entry && count < max_paths) {
        if (!entry.isDirectory()) {
            String name = entry.name();
            int slash = name.lastIndexOf('/');
            String base = (slash >= 0) ? name.substring(slash + 1) : name;
            if (base.endsWith(".log") &&
                (base.startsWith("dm_") || base.startsWith("ch_"))) {
                String full = dir + "/" + base;
                strncpy(paths[count], full.c_str(), MAX_SYNC_PATH_LEN - 1);
                paths[count][MAX_SYNC_PATH_LEN - 1] = '\0';
                count++;
            }
        }
        entry = root.openNextFile();
    }
    root.close();
    if (is_sd) sd_spi_release();
    return count;
}

static bool read_one_record(File& f, StoredMsg& m) {
    memset(&m, 0, sizeof(m));
    char line[256];
    bool has_data = false;

    while (f.available()) {
        int len = 0;
        while (f.available() && len < (int)sizeof(line) - 1) {
            char ch = f.read();
            if (ch == '\n' || ch == '\r') break;
            line[len++] = ch;
        }
        line[len] = '\0';
        if (len == 0) continue;

        if (len == 3 && line[0] == '-' && line[1] == '-' && line[2] == '-')
            return has_data;

        char* eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        const char* key = line;
        const char* val = eq + 1;
        has_data = true;

        if (strcmp(key, "ts") == 0) m.timestamp = strtoul(val, nullptr, 10);
        else if (strcmp(key, "sender_ts") == 0) m.sender_ts = strtoul(val, nullptr, 10);
        else if (strcmp(key, "from") == 0) strncpy(m.from, val, sizeof(m.from) - 1);
        else if (strcmp(key, "peer") == 0) strncpy(m.peer, val, sizeof(m.peer) - 1);
        else if (strcmp(key, "text") == 0) strncpy(m.text, val, sizeof(m.text) - 1);
        else if (strcmp(key, "ch") == 0) m.channel_idx = (int8_t)atoi(val);
        else if (strcmp(key, "hops") == 0) m.hops = (uint8_t)atoi(val);
        else if (strcmp(key, "snr") == 0) m.snr = atof(val);
        else if (strcmp(key, "rssi") == 0) m.rssi = atof(val);
        else if (strcmp(key, "direct") == 0) { if (atoi(val)) m.flags |= 0x01; }
        else if (strcmp(key, "dm") == 0) { if (atoi(val)) m.flags |= 0x02; }
        else if (strcmp(key, "path") == 0) m.path_len = parse_path_field(val, m.path);
        else if (strcmp(key, "pubkey") == 0 && strlen(val) == 12) {
            mesh::Utils::fromHex(m.sender_pub_key, 6, val);
            m.has_pub_key = true;
        }
    }
    return false;
}

int PunkMesh::readOneStoredMsg(fs::FS* storage, const char* path,
                               size_t offset, StoredMsg& m) {
    if (!storage) return -1;
    bool is_sd = (storage != &LittleFS);
    if (is_sd) sd_spi_take();

    File f = storage->open(path, "r");
    if (!f) {
        if (is_sd) sd_spi_release();
        return -1;
    }
    if (offset >= f.size()) {
        f.close();
        if (is_sd) sd_spi_release();
        return -1;
    }
    f.seek(offset);

    bool ok = read_one_record(f, m);
    size_t end_pos = f.position();
    f.close();
    if (is_sd) sd_spi_release();
    return ok ? (int)end_pos : -1;
}

int PunkMesh::readAllStoredMsgs(const char* path, StoredMsg* out, int max_count) {
    if (!_storage || max_count <= 0) return 0;
    bool is_sd = (_storage != &LittleFS);
    if (is_sd) sd_spi_take();

    File f = _storage->open(path, "r");
    if (!f) {
        if (is_sd) sd_spi_release();
        return 0;
    }

    int count = 0;
    while (f.available() && count < max_count) {
        StoredMsg m;
        if (read_one_record(f, m))
            out[count++] = m;
        else
            break;
    }

    f.close();
    if (is_sd) sd_spi_release();
    return count;
}

int PunkMesh::readStoredMsgsSince(const char* path, uint32_t since,
                                  StoredMsg* out, int max_count) {
    if (!_storage || max_count <= 0) return 0;
    bool is_sd = (_storage != &LittleFS);
    if (is_sd) sd_spi_take();

    File f = _storage->open(path, "r");
    if (!f) {
        if (is_sd) sd_spi_release();
        return 0;
    }

    int count = 0;
    while (f.available() && count < max_count) {
        StoredMsg m;
        if (read_one_record(f, m)) {
            if (m.timestamp > since)
                out[count++] = m;
        } else
            break;
    }

    f.close();
    if (is_sd) sd_spi_release();
    return count;
}

int PunkMesh::lookupPersistedPaths(lua_State* L, const char* hash_hex,
                                    int channel_idx, const char* peer) {
    lua_newtable(L);
    if (!_storage || strlen(hash_hex) != MAX_HASH_SIZE * 2) return 1;

    String fpath;
    if (channel_idx >= 0) {
        String ch_name = channel_name_for_idx(*this, channel_idx);
        fpath = channel_msg_path(_storage_prefix, ch_name.c_str());
    } else if (peer && peer[0]) {
        fpath = dm_msg_path(_storage_prefix, peer);
    } else {
        return 1;
    }

    bool is_sd = (_storage != &LittleFS);
    if (is_sd) sd_spi_take();

    File f = _storage->open(fpath.c_str(), "r");
    if (!f) { if (is_sd) sd_spi_release(); return 1; }

    // Build target line to match
    char target[6 + MAX_HASH_SIZE * 2 + 1];
    snprintf(target, sizeof(target), "hash=%s", hash_hex);

    char line[256];
    bool found_hash = false;
    int rpath_idx = 1;

    while (f.available()) {
        int len = 0;
        while (f.available() && len < (int)sizeof(line) - 1) {
            char ch = f.read();
            if (ch == '\n' || ch == '\r') break;
            line[len++] = ch;
        }
        line[len] = '\0';
        if (len == 0) continue;

        if (found_hash) {
            if (len == 3 && line[0] == '-' && line[1] == '-' && line[2] == '-')
                break;  // end of matching record
            if (strncmp(line, "rpath=", 6) == 0) {
                char rval[256];
                strncpy(rval, line + 6, sizeof(rval) - 1);
                rval[sizeof(rval) - 1] = '\0';
                char* semi1 = strchr(rval, ';');
                if (semi1) {
                    *semi1 = '\0';
                    uint8_t path_buf[MAX_PATH_SIZE];
                    uint16_t path_len_enc = parse_path_field(rval, path_buf);
                    float snr = 0, rssi = 0;
                    bool is_direct = false;
                    char* semi2 = strchr(semi1 + 1, ';');
                    if (semi2) {
                        *semi2 = '\0';
                        snr = atof(semi1 + 1);
                        char* semi3 = strchr(semi2 + 1, ';');
                        if (semi3) {
                            *semi3 = '\0';
                            rssi = atof(semi2 + 1);
                            is_direct = atoi(semi3 + 1) != 0;
                        } else {
                            rssi = atof(semi2 + 1);
                        }
                    } else {
                        snr = atof(semi1 + 1);
                    }
                    lua_newtable(L);
                    push_path_table(L, path_len_enc, path_buf);
                    lua_setfield(L, -2, "path");
                    lua_pushinteger(L, path_len_enc & 63);
                    lua_setfield(L, -2, "hops");
                    lua_pushboolean(L, is_direct);
                    lua_setfield(L, -2, "direct");
                    lua_pushnumber(L, snr);
                    lua_setfield(L, -2, "snr");
                    lua_pushnumber(L, rssi);
                    lua_setfield(L, -2, "rssi");
                    lua_rawseti(L, -2, rpath_idx++);
                }
            }
        } else if (strcmp(line, target) == 0) {
            found_hash = true;
        }
    }

    f.close();
    if (is_sd) sd_spi_release();
    return 1;
}

void PunkMesh::setClock(uint32_t timestamp)
{
    uint32_t curr = getRTCClock()->getCurrentTime();
    if (timestamp > curr)
    {
        getRTCClock()->setCurrentTime(timestamp);
        SLog.println("   (OK - clock set!)");
    }
    else
    {
        SLog.println("   (ERR: clock cannot go backwards)");
    }
}

// URL-decode src into dst ('+' -> space, %XX -> byte). dst is null-terminated.
static void url_decode(const char *src, char *dst, size_t dst_sz)
{
    size_t di = 0;
    for (size_t si = 0; src[si] && di + 1 < dst_sz; si++) {
        char c = src[si];
        if (c == '+') {
            dst[di++] = ' ';
        } else if (c == '%' && src[si + 1] && src[si + 2]) {
            char hx[3] = { src[si + 1], src[si + 2], 0 };
            dst[di++] = (char)strtoul(hx, nullptr, 16);
            si += 2;
        } else {
            dst[di++] = c;
        }
    }
    dst[di] = 0;
}

void PunkMesh::importCard(const char *command)
{
    while (*command == ' ')
        command++; // skip leading spaces
    if (memcmp(command, "meshcore://", 11) != 0) {
        SLog.println("   error: invalid format");
        return;
    }
    char *body = (char *)command + 11;  // after the scheme

    // ── MeshCore app contact URI ──────────────────────────────────────
    // meshcore://contact/add?name=<urlenc>&public_key=<64hex>&type=<1-4>
    // (this is what the phone app's QR / clipboard share produces)
    if (memcmp(body, "contact/add?", 12) == 0) {
        char name[40] = {0};
        char pubhex[80] = {0};
        int ctype = 1;
        char *p = body + 12;
        while (p && *p) {                 // body is writable; tokenise in place
            char *amp = strchr(p, '&');
            if (amp) *amp = 0;
            char *eq = strchr(p, '=');
            if (eq) {
                *eq = 0;
                const char *key = p;
                const char *val = eq + 1;
                if (strcmp(key, "name") == 0)            url_decode(val, name, sizeof(name));
                else if (strcmp(key, "public_key") == 0) strncpy(pubhex, val, sizeof(pubhex) - 1);
                else if (strcmp(key, "type") == 0)       ctype = atoi(val);
            }
            p = amp ? amp + 1 : nullptr;
        }
        if (strlen(pubhex) != PUB_KEY_SIZE * 2) {
            SLog.println("   error: bad public_key in contact URI");
            return;
        }
        mesh::Identity id(pubhex);  // construct from 64-hex pubkey
        ContactInfo *existing = lookupContactByPubKey(id.pub_key, PUB_KEY_SIZE);
        if (existing) {
            strncpy(existing->name, name, sizeof(existing->name) - 1);
            existing->name[sizeof(existing->name) - 1] = 0;
            existing->type = (uint8_t)ctype;
            existing->lastmod = getRTCClock()->getCurrentTime();
            saveOneContact(*existing);
            SLog.printf("   updated contact from URI: %s\n", name);
        } else {
            ContactInfo ci;
            memset(&ci, 0, sizeof(ci));
            ci.id = id;
            ci.out_path_len = OUT_PATH_UNKNOWN;  // no route yet -> flood
            strncpy(ci.name, name, sizeof(ci.name) - 1);
            ci.type = (uint8_t)ctype;
            ci.lastmod = getRTCClock()->getCurrentTime();
            if (addContact(ci)) {
                saveOneContact(ci);
                SLog.printf("   imported contact from URI: %s\n", name);
            } else {
                SLog.println("   error: contact list full");
            }
        }
        return;
    }

    // ── Legacy biz-card: meshcore://<hex of raw advert packet> ────────
    {
        char *ep = strchr(body, 0); // find end of string
        while (ep > body) {
            ep--;
            if (mesh::Utils::isHexChar(*ep))
                break; // found tail end of card
            *ep = 0;   // remove trailing spaces and other junk
        }
        int len = strlen(body);
        if (len % 2 == 0) {
            len >>= 1; // halve, for num bytes
            if (mesh::Utils::fromHex(tmp_buf, len, body)) {
                importContact(tmp_buf, len);
                return;
            }
        }
    }
    SLog.println("   error: invalid format");
}

float PunkMesh::getAirtimeBudgetFactor() const
{
    return _prefs.airtime_factor;
}

int PunkMesh::calcRxDelay(float score, uint32_t air_time) const
{
    return 0; // disable rxdelay
}

bool PunkMesh::allowPacketForward(const mesh::Packet *packet)
{
    return true;
}

void PunkMesh::onDiscoveredContact(ContactInfo &contact, bool is_new, uint8_t path_len, const uint8_t *path)
{
    // The base notifies us about excluded ("do not add") types too, with a temp
    // contact it didn't add. Ignore those completely so they don't pollute path
    // history or the UI contact list.
    if (is_new && !shouldAutoAddContactType(contact.type)) {
        SLog.printf("[MESH RX] Advert from excluded type (%s) — not adding\n",
                      getTypeName(contact.type));
        return;
    }

    // Auto-add hop limit (autoadd_max_hops): the base mesh declines to add a
    // contact whose advert arrived too far away and notifies us with a temp
    // contact. Ignore those — don't archive distant nodes we deliberately chose
    // not to auto-add. (path_len's low 6 bits are the hop count.)
    if (is_new && _prefs.autoadd_max_hops > 0
        && (path_len & 0x3F) >= _prefs.autoadd_max_hops) {
        SLog.printf("[MESH RX] Advert beyond %u-hop auto-add limit — not adding\n",
                      _prefs.autoadd_max_hops);
        return;
    }

    // New advert the base mesh did NOT add to the live table — i.e. the list is
    // full and overwrite-when-full is off (excluded types and over-hop-limit
    // adverts returned above). Archive it (a no-op when archiving is off) so it
    // can be re-added later, then stop: it's not a live contact, so don't
    // record path / persist / notify the UI.
    if (is_new && !lookupContactByPubKey(contact.id.pub_key, PUB_KEY_SIZE)) {
        SLog.printf("[MESH RX] List full — %s discarded new contact: %s\n",
                      _prefs.archive_contacts ? "archiving" : "dropping", contact.name);
        archiveContact(contact);
        return;
    }

    // One atomic line — separate prints could interleave / be partially dropped.
    char pk_hex[PUB_KEY_SIZE * 2 + 1];
    mesh::Utils::toHex(pk_hex, contact.id.pub_key, PUB_KEY_SIZE);
    SLog.printf("[MESH RX] ADVERT %s (%s) type=%s path_len=%d pubkey=%s contacts=%d\n",
        contact.name, is_new ? "NEW" : "known", getTypeName(contact.type), path_len,
        pk_hex, getNumContacts() + (is_new ? 1 : 0));

    recordPath(contact.id.pub_key, path_len, path,
               last_rx_snr, last_rx_rssi, PATH_SRC_ADVERT, false);

    // "Last seen" = when WE heard this advert (our clock), not the sender's advert
    // timestamp. lastmod is in-RAM (reset to 0 on boot), so it reads 0 until the
    // first advert post-boot — consumers treat 0 as "unknown".
    contact.lastmod = getRTCClock()->getCurrentTime();

    saveOneContact(contact);  // O(1): just this contact's slot, not the whole file

    if (rx_event_queue) {
        RxEvent ev = {};
        ev.kind = RxEvent::CONTACT_UPDATE;
        strncpy(ev.sender, contact.name, sizeof(ev.sender) - 1);
        ev.hops = contact.type;
        xQueueSend(rx_event_queue, &ev, 0);
    }

#if BLE_COMPANION_ENABLED
    if (ble_companion) ble_companion->pushAdvert(contact, is_new, path_len, path);
#endif
}

void PunkMesh::onContactPathUpdated(const ContactInfo &contact)
{
    SLog.printf("PATH to: %s, path_len=%d\n", contact.name, (int32_t)contact.out_path_len);
    recordPath(contact.id.pub_key, contact.out_path_len, contact.out_path,
               0, 0, PATH_SRC_PATH_UPDATE, true);
    saveOneContact(contact);  // O(1): just this contact's slot
#if BLE_COMPANION_ENABLED
    if (ble_companion) ble_companion->pushPathUpdated(contact);
#endif
}

ContactInfo* PunkMesh::processAck(const uint8_t *data)
{
    if (memcmp(data, &expected_ack_crc, 4) == 0)
    {
        uint32_t rtt = _ms->getMillis() - last_msg_sent;
        uint32_t acked_crc = expected_ack_crc;
        SLog.printf("   Got ACK! (round trip: %d millis)\n", rtt);
        expected_ack_crc = 0;
        if (curr_recipient) {
            recordPathSuccess(curr_recipient->id.pub_key, rtt);
        }
#if BLE_COMPANION_ENABLED
        uint32_t ack_crc;
        memcpy(&ack_crc, data, 4);
        if (ble_companion) ble_companion->pushSendConfirmed(ack_crc, rtt);
#endif
        // Notify the Lua UI so it can mark the sent DM delivered.
        if (rx_event_queue) {
            RxEvent ev;
            memset(&ev, 0, sizeof(ev));
            ev.kind = RxEvent::ACK;
            ev.ack  = acked_crc;
            ev.rtt  = (int32_t)rtt;
            xQueueSend(rx_event_queue, &ev, 0);
        }
        return curr_recipient;
    }
    return nullptr;
}

void PunkMesh::onMessageRecv(const ContactInfo &from, mesh::Packet *pkt, uint32_t sender_timestamp, const char *text)
{
    SLog.println("[MESH RX] ========== DIRECT MSG RECEIVED ==========");
    SLog.printf("[MESH RX] From: %s, route: %s, hops: %d\n",
        from.name, pkt->isRouteDirect() ? "DIRECT" : "FLOOD", pkt->getPathHashCount());
    SLog.printf("[MESH RX] Text: \"%s\"\n", text);
    SLog.printf("[MESH RX] Sender timestamp: %u\n", sender_timestamp);

    if (strcmp(text, "clock sync") == 0)
    { // special text command
        setClock(sender_timestamp + 1);
        return;
    }

    // Normalize UTF-8 smart quotes to ASCII so they render from montserrat
    // instead of tofu. Do it once, before both persistence and Lua dispatch.
    char norm_text[160];
    normalize_smart_quotes(text, norm_text, sizeof(norm_text));

    // Authoritative time = OUR clock at receipt; the sender's value is recorded only.
    uint32_t rx_ts = getRTCClock()->getCurrentTime();

    // Persist incoming DM — peer and from are both the sender for incoming.
    appendDMMessage(from.name, from.name, norm_text, rx_ts,
                    last_rx_snr, last_rx_rssi, pkt->getPathHashCount(),
                    pkt->isRouteDirect(), pkt->path_len, pkt->path,
                    _last_pkt_hash, from.id.pub_key, /*sender_ts=*/sender_timestamp);

    recordPath(from.id.pub_key, pkt->path_len, pkt->path,
               last_rx_snr, last_rx_rssi, PATH_SRC_MSG_RX, pkt->isRouteDirect());

    MsgPathEntry* mpe = findMsgPaths(_last_pkt_hash);
    if (mpe) {
        mpe->is_message   = true;
        mpe->is_dm        = true;
        mpe->channel_idx  = -1;
        strncpy(mpe->peer, from.name, sizeof(mpe->peer) - 1);
        mpe->peer[sizeof(mpe->peer) - 1] = '\0';
    }

    // Hand off to the UI core via rx_event_queue. The UI loop on Core 0
    // picks this up in drain_rx_events() and calls into Lua there —
    // lua_State must only be touched from one thread.
    if (rx_event_queue) {
        RxEvent ev = {};
        ev.kind        = RxEvent::DIRECT_MSG;
        ev.hops        = pkt->getPathHashCount();
        ev.channel_idx = -1;
        ev.direct      = pkt->isRouteDirect();
        strncpy(ev.sender, from.name, sizeof(ev.sender) - 1);
        ev.sender[sizeof(ev.sender) - 1] = '\0';
        strncpy(ev.text, norm_text, sizeof(ev.text) - 1);
        ev.text[sizeof(ev.text) - 1] = '\0';
        ev.timestamp = rx_ts;   // our RX clock; the live UI sorts/displays on this
        ev.snr       = last_rx_snr;
        ev.rssi      = last_rx_rssi;
        ev.path_len  = pkt->path_len;
        memcpy(ev.path, pkt->path, pkt->getPathByteLen());
        memcpy(ev.pkt_hash, _last_pkt_hash, MAX_HASH_SIZE);
        if (xQueueSend(rx_event_queue, &ev, 0) != pdTRUE) {
            SLog.println("[MESH RX] WARNING: rx_event_queue full, dropping DM");
        }
    }

#if BLE_COMPANION_ENABLED
    if (ble_companion) ble_companion->queueReceivedDM(from, pkt, sender_timestamp, text);
#endif
}

void PunkMesh::onCommandDataRecv(const ContactInfo &from, mesh::Packet *pkt, uint32_t sender_timestamp, const char *text)
{
#if BLE_COMPANION_ENABLED
    if (ble_companion) ble_companion->queueCliResponse(from, pkt, sender_timestamp, text);
#endif
}
void PunkMesh::onSignedMessageRecv(const ContactInfo &from, mesh::Packet *pkt, uint32_t sender_timestamp, const uint8_t *sender_prefix, const char *text)
{
}

void PunkMesh::onChannelMessageRecv(const mesh::GroupChannel &channel, mesh::Packet *pkt, uint32_t timestamp, const char *text)
{
    SLog.println("[MESH RX] ========== CHANNEL MSG RECEIVED ==========");
    SLog.printf("[MESH RX] Raw text: \"%s\"\n", text);
    SLog.printf("[MESH RX] Route: %s, hops: %d, timestamp: %u\n",
        pkt->isRouteDirect() ? "DIRECT" : "FLOOD", pkt->getPathHashCount(), timestamp);

    // Parse "sender: message" format used by group messages
    const char *colon = strstr(text, ": ");
    char sender_name[32] = "unknown";
    const char *msg_text = text;

    if (colon && (colon - text) < (int)sizeof(sender_name)) {
        size_t name_len = colon - text;
        memcpy(sender_name, text, name_len);
        sender_name[name_len] = '\0';
        msg_text = colon + 2;
    }

    // Identify which channel slot decrypted this packet (-1 if not in our table)
    int channel_idx = findChannelIdx(channel);
    SLog.printf("[MESH RX] Parsed sender: \"%s\", msg: \"%s\", channel_idx: %d\n", sender_name, msg_text, channel_idx);

    // Normalize UTF-8 smart quotes to ASCII in the message body so they
    // render from montserrat rather than as tofu.
    char norm_msg[160];
    normalize_smart_quotes(msg_text, norm_msg, sizeof(norm_msg));

    // Authoritative time = OUR clock at receipt. The sender's `timestamp` is
    // unreliable (often unset/0 across the mesh), so it's recorded only.
    uint32_t rx_ts = getRTCClock()->getCurrentTime();

    // Persist to disk (no-op if channel_idx < 0)
    appendChannelMessage(channel_idx, sender_name, norm_msg, rx_ts,
                         last_rx_snr, last_rx_rssi, pkt->getPathHashCount(),
                         pkt->isRouteDirect(), pkt->path_len, pkt->path,
                         _last_pkt_hash, /*sender_ts=*/timestamp);

    MsgPathEntry* mpe = findMsgPaths(_last_pkt_hash);
    if (mpe) {
        mpe->is_message   = true;
        mpe->is_dm        = false;
        mpe->channel_idx  = (int8_t)channel_idx;
        mpe->peer[0]      = '\0';
    }

    // Hand off to UI core. See onMessageRecv for the why.
    if (rx_event_queue) {
        RxEvent ev = {};
        ev.kind        = RxEvent::CHANNEL_MSG;
        ev.hops        = pkt->getPathHashCount();
        ev.channel_idx = (int8_t)channel_idx;
        ev.direct      = pkt->isRouteDirect();
        strncpy(ev.sender, sender_name, sizeof(ev.sender) - 1);
        ev.sender[sizeof(ev.sender) - 1] = '\0';
        strncpy(ev.text, norm_msg, sizeof(ev.text) - 1);
        ev.text[sizeof(ev.text) - 1] = '\0';
        ev.timestamp = rx_ts;   // our RX clock; the live UI sorts/displays on this
        ev.snr       = last_rx_snr;
        ev.rssi      = last_rx_rssi;
        ev.path_len  = pkt->path_len;
        memcpy(ev.path, pkt->path, pkt->getPathByteLen());
        memcpy(ev.pkt_hash, _last_pkt_hash, MAX_HASH_SIZE);
        if (xQueueSend(rx_event_queue, &ev, 0) != pdTRUE) {
            SLog.println("[MESH RX] WARNING: rx_event_queue full, dropping channel msg");
        }
    }

#if BLE_COMPANION_ENABLED
    if (ble_companion) ble_companion->queueReceivedChannelMsg(channel, pkt, timestamp, text, channel_idx);
#endif
}

uint8_t PunkMesh::onContactRequest(const ContactInfo &contact, uint32_t sender_timestamp, const uint8_t *data, uint8_t len, uint8_t *reply)
{
    return 0; // unknown
}

void PunkMesh::onContactResponse(const ContactInfo &contact, const uint8_t *data, uint8_t len)
{
#if BLE_COMPANION_ENABLED
    if (ble_companion) ble_companion->pushContactResponse(contact, data, len);
#endif
}

void PunkMesh::onControlDataRecv(mesh::Packet* packet)
{
#if BLE_COMPANION_ENABLED
    if (ble_companion) ble_companion->pushControlData(packet, _radio->getLastSNR(), _radio->getLastRSSI());
#endif
}

void PunkMesh::onRawDataRecv(mesh::Packet* packet)
{
#if BLE_COMPANION_ENABLED
    if (ble_companion) ble_companion->pushRawData(packet, _radio->getLastSNR(), _radio->getLastRSSI());
#endif
}

void PunkMesh::onTraceRecv(mesh::Packet* packet, uint32_t tag, uint32_t auth_code, uint8_t flags,
                           const uint8_t* path_snrs, const uint8_t* path_hashes, uint8_t path_len)
{
#if BLE_COMPANION_ENABLED
    if (ble_companion) ble_companion->pushTraceData(packet, tag, auth_code, flags, path_snrs, path_hashes, path_len);
#endif
}

void PunkMesh::onChannelDataRecv(const mesh::GroupChannel& channel, mesh::Packet* pkt,
                                 uint16_t data_type, const uint8_t* data, size_t data_len)
{
#if BLE_COMPANION_ENABLED
    if (ble_companion) ble_companion->pushChannelDataRecv(channel, pkt, data_type, data, data_len);
#endif
}

bool PunkMesh::onContactPathRecv(ContactInfo& contact, uint8_t* in_path, uint8_t in_path_len,
                                 uint8_t* out_path, uint8_t out_path_len, uint8_t extra_type,
                                 uint8_t* extra, uint8_t extra_len)
{
#if BLE_COMPANION_ENABLED
    if (ble_companion && ble_companion->checkPendingDiscovery(contact, in_path, in_path_len,
            out_path, out_path_len, extra_type, extra, extra_len)) {
        return false;
    }
#endif
    return BaseChatMesh::onContactPathRecv(contact, in_path, in_path_len, out_path, out_path_len,
                                           extra_type, extra, extra_len);
}

uint32_t PunkMesh::calcFloodTimeoutMillisFor(uint32_t pkt_airtime_millis) const
{
    return SEND_TIMEOUT_BASE_MILLIS + (FLOOD_SEND_TIMEOUT_FACTOR * pkt_airtime_millis);
}
uint32_t PunkMesh::calcDirectTimeoutMillisFor(uint32_t pkt_airtime_millis, uint8_t path_len) const
{
    return SEND_TIMEOUT_BASE_MILLIS +
           ((pkt_airtime_millis * DIRECT_SEND_PERHOP_FACTOR + DIRECT_SEND_PERHOP_EXTRA_MILLIS) * (path_len + 1));
}

void PunkMesh::onSendTimeout()
{
    SLog.println("   ERROR: timed out, no ACK.");
    if (curr_recipient) {
        recordPathFailure(curr_recipient->id.pub_key);
    }
    // Tell the Lua UI the send failed (only if there is still a pending ack —
    // a successful processAck clears expected_ack_crc, so this won't fire a
    // false failure after a delivery).
    if (rx_event_queue && expected_ack_crc != 0) {
        RxEvent ev;
        memset(&ev, 0, sizeof(ev));
        ev.kind = RxEvent::ACK;
        ev.ack  = expected_ack_crc;
        ev.rtt  = -1;
        xQueueSend(rx_event_queue, &ev, 0);
    }
}

// ── Path history tracker ──────────────────────────────────────────

static bool paths_equal(uint16_t a_len, const uint8_t* a_path,
                        uint16_t b_len, const uint8_t* b_path) {
    if (a_len != b_len) return false;
    // Clamp: a sentinel/corrupt encoding (e.g. OUT_PATH_UNKNOWN 0xFF decodes
    // as 63×4 = 252) must not read past the 64-byte path buffers.
    uint16_t byte_len = (uint16_t)(a_len & 63) * ((a_len >> 6) + 1);
    if (byte_len > MAX_PATH_SIZE) byte_len = MAX_PATH_SIZE;
    return memcmp(a_path, b_path, byte_len) == 0;
}

ContactPathHistory* PunkMesh::findOrCreatePathHistory(const uint8_t* pub_key) {
    for (int i = 0; i < _path_history_count; i++) {
        if (memcmp(_path_history[i].pub_key, pub_key, PUB_KEY_SIZE) == 0)
            return &_path_history[i];
    }
    if (_path_history_count < MAX_PATH_CONTACTS) {
        ContactPathHistory* h = &_path_history[_path_history_count++];
        memset(h, 0, sizeof(*h));
        memcpy(h->pub_key, pub_key, PUB_KEY_SIZE);
        return h;
    }
    // Evict oldest entry
    int oldest_idx = 0;
    uint32_t oldest_ts = UINT32_MAX;
    for (int i = 0; i < _path_history_count; i++) {
        for (int j = 0; j < _path_history[i].count; j++) {
            if (_path_history[i].records[j].timestamp < oldest_ts) {
                oldest_ts = _path_history[i].records[j].timestamp;
                oldest_idx = i;
            }
        }
    }
    ContactPathHistory* h = &_path_history[oldest_idx];
    memset(h, 0, sizeof(*h));
    memcpy(h->pub_key, pub_key, PUB_KEY_SIZE);
    return h;
}

void PunkMesh::recordPath(const uint8_t* pub_key, uint16_t path_len,
                          const uint8_t* path, float snr, float rssi,
                          uint8_t source, bool is_direct) {
    uint8_t hash_count = path_len & 63;
    if (hash_count == 0 && source != PATH_SRC_PATH_UPDATE) return;

    ContactPathHistory* h = findOrCreatePathHistory(pub_key);
    if (!h) return;

    uint32_t now = getRTCClock()->getCurrentTime();

    for (int i = 0; i < h->count; i++) {
        if (paths_equal(h->records[i].path_len, h->records[i].path, path_len, path)) {
            h->records[i].timestamp = now;
            if (snr != 0) h->records[i].snr = snr;
            if (rssi != 0) h->records[i].rssi = rssi;
            h->records[i].source = source;
            h->records[i].is_direct = is_direct;
            return;
        }
    }

    PathRecord* slot;
    if (h->count < MAX_PATH_RECORDS) {
        slot = &h->records[h->count++];
    } else {
        int weakest = 0;
        for (int i = 1; i < MAX_PATH_RECORDS; i++) {
            if (h->records[i].success_count < h->records[weakest].success_count ||
                (h->records[i].success_count == h->records[weakest].success_count &&
                 h->records[i].timestamp < h->records[weakest].timestamp)) {
                weakest = i;
            }
        }
        slot = &h->records[weakest];
    }

    memset(slot, 0, sizeof(*slot));
    slot->path_len = path_len;
    uint8_t byte_len = hash_count * ((path_len >> 6) + 1);
    if (byte_len > MAX_PATH_SIZE) byte_len = MAX_PATH_SIZE;
    if (path) memcpy(slot->path, path, byte_len);
    slot->timestamp = now;
    slot->snr = snr;
    slot->rssi = rssi;
    slot->source = source;
    slot->is_direct = is_direct;
}

void PunkMesh::recordPathSuccess(const uint8_t* pub_key, uint32_t trip_time_ms) {
    ContactPathHistory* h = findOrCreatePathHistory(pub_key);
    if (!h || !curr_recipient) return;
    // No learned route — nothing to credit (and 0xFF must not be treated
    // as a path encoding; it used to create garbage 63×4-hash records).
    if (curr_recipient->out_path_len == OUT_PATH_UNKNOWN) return;

    for (int i = 0; i < h->count; i++) {
        if (paths_equal(h->records[i].path_len, h->records[i].path,
                        curr_recipient->out_path_len, curr_recipient->out_path)) {
            h->records[i].success_count++;
            h->records[i].trip_time_ms = trip_time_ms;
            h->records[i].timestamp = getRTCClock()->getCurrentTime();
            return;
        }
    }
    // Path not tracked yet — record it now
    recordPath(pub_key, curr_recipient->out_path_len, curr_recipient->out_path,
               0, 0, PATH_SRC_ACK, true);
    // Set the success on the newly added record
    for (int i = h->count - 1; i >= 0; i--) {
        if (paths_equal(h->records[i].path_len, h->records[i].path,
                        curr_recipient->out_path_len, curr_recipient->out_path)) {
            h->records[i].success_count = 1;
            h->records[i].trip_time_ms = trip_time_ms;
            break;
        }
    }
}

void PunkMesh::recordPathFailure(const uint8_t* pub_key) {
    ContactPathHistory* h = findOrCreatePathHistory(pub_key);
    if (!h || !curr_recipient) return;
    if (curr_recipient->out_path_len == OUT_PATH_UNKNOWN) return;

    for (int i = 0; i < h->count; i++) {
        if (paths_equal(h->records[i].path_len, h->records[i].path,
                        curr_recipient->out_path_len, curr_recipient->out_path)) {
            h->records[i].failure_count++;
            return;
        }
    }
}

// ── Per-message multi-path tracking ──────────────────────────────

MsgPathEntry* PunkMesh::findMsgPaths(const uint8_t* hash) {
    for (int i = 0; i < _msg_path_count; i++) {
        if (memcmp(_msg_paths[i].pkt_hash, hash, MAX_HASH_SIZE) == 0)
            return &_msg_paths[i];
    }
    return nullptr;
}

MsgPathEntry* PunkMesh::recordMsgPath(const uint8_t* hash, uint16_t path_len,
                                       const uint8_t* path, float snr,
                                       float rssi, bool is_direct) {
    MsgPathEntry* entry = findMsgPaths(hash);
    if (!entry) {
        entry = &_msg_paths[_msg_path_next];
        memset(entry, 0, sizeof(*entry));
        memcpy(entry->pkt_hash, hash, MAX_HASH_SIZE);
        _msg_path_next = (_msg_path_next + 1) % MAX_MSG_PATH_ENTRIES;
        if (_msg_path_count < MAX_MSG_PATH_ENTRIES) _msg_path_count++;
    }

    for (int i = 0; i < entry->path_count; i++) {
        if (paths_equal(entry->paths[i].path_len, entry->paths[i].path,
                        path_len, path)) {
            if (snr > entry->paths[i].snr) entry->paths[i].snr = snr;
            if (rssi > entry->paths[i].rssi) entry->paths[i].rssi = rssi;
            return entry;
        }
    }

    if (entry->path_count >= MAX_PATHS_PER_MSG) return entry;

    ObservedPath* op = &entry->paths[entry->path_count++];
    memset(op, 0, sizeof(*op));
    op->path_len  = path_len;
    op->snr       = snr;
    op->rssi      = rssi;
    op->is_direct = is_direct;
    uint8_t byte_len = (path_len & 63) * ((path_len >> 6) + 1);
    if (byte_len > MAX_PATH_SIZE) byte_len = MAX_PATH_SIZE;
    if (path) memcpy(op->path, path, byte_len);

    return entry;
}

void PunkMesh::persistExtraPath(const uint8_t* hash, const ObservedPath& op) {
    if (!_storage) return;
    MsgPathEntry* entry = findMsgPaths(hash);
    if (!entry || !entry->is_message) return;

    String fpath;
    if (entry->is_dm)
        fpath = dm_msg_path(_storage_prefix, entry->peer);
    else {
        String ch_name = channel_name_for_idx(*this, entry->channel_idx);
        fpath = channel_msg_path(_storage_prefix, ch_name.c_str());
    }

    char hash_hex[MAX_HASH_SIZE * 2 + 1];
    mesh::Utils::toHex(hash_hex, hash, MAX_HASH_SIZE);
    String hash_line = String("hash=") + hash_hex;

    bool is_sd = (_storage != &LittleFS);
    if (is_sd) sd_spi_take();

    File f = _storage->open(fpath.c_str(), "r");
    if (!f) { if (is_sd) sd_spi_release(); return; }

    size_t fsize = f.size();
    uint8_t* buf = (uint8_t*)malloc(fsize);
    if (!buf) { f.close(); if (is_sd) sd_spi_release(); return; }
    f.read(buf, fsize);
    f.close();

    // Scan for the hash= line, then find its following --- delimiter
    const char* data = (const char*)buf;
    const char* found_hash = nullptr;
    const char* p = data;
    while (p < data + fsize) {
        const char* eol = (const char*)memchr(p, '\n', fsize - (p - data));
        if (!eol) eol = data + fsize;
        size_t line_len = eol - p;
        if (line_len == hash_line.length() &&
            memcmp(p, hash_line.c_str(), line_len) == 0) {
            found_hash = p;
            break;
        }
        p = eol + 1;
    }

    if (!found_hash) { free(buf); if (is_sd) sd_spi_release(); return; }

    // Find the --- after the hash line
    p = found_hash;
    const char* insert_pos = nullptr;
    while (p < data + fsize) {
        const char* eol = (const char*)memchr(p, '\n', fsize - (p - data));
        if (!eol) eol = data + fsize;
        size_t line_len = eol - p;
        if (line_len == 3 && p[0] == '-' && p[1] == '-' && p[2] == '-') {
            insert_pos = p;
            break;
        }
        p = eol + 1;
    }

    if (!insert_pos) { free(buf); if (is_sd) sd_spi_release(); return; }

    // Build the rpath line to insert
    char rpath_buf[256];
    int rlen = 0;
    rlen += snprintf(rpath_buf + rlen, sizeof(rpath_buf) - rlen, "rpath=");
    uint8_t hs = (op.path_len >> 6) + 1;
    uint8_t hc = op.path_len & 63;
    if (hs > 4) hc = 0;  // invalid encoding — write no hops
    char hex[9];  // up to 4-byte hashes (8 hex chars + NUL)
    for (int j = 0; j < hc && (j + 1) * hs <= MAX_PATH_SIZE; j++) {
        // leave room for this token plus the ;snr;rssi;d\n tail
        if (rlen + (int)hs * 2 + 32 >= (int)sizeof(rpath_buf)) break;
        if (j > 0) rpath_buf[rlen++] = ',';
        mesh::Utils::toHex(hex, &op.path[j * hs], hs);
        memcpy(rpath_buf + rlen, hex, hs * 2);
        rlen += hs * 2;
    }
    rlen += snprintf(rpath_buf + rlen, sizeof(rpath_buf) - rlen,
                     ";%.2f;%.2f;%d\n", op.snr, op.rssi, op.is_direct ? 1 : 0);

    // Rewrite file: before insert_pos + rpath line + from insert_pos onward
    File wf = _storage->open(fpath.c_str(), "w", true);
    if (wf) {
        size_t before = insert_pos - data;
        wf.write(buf, before);
        wf.write((const uint8_t*)rpath_buf, rlen);
        wf.write(buf + before, fsize - before);
        wf.close();
    }
    free(buf);
    if (is_sd) sd_spi_release();
}

void PunkMesh::preRegisterSentHash(const uint8_t* hash, bool is_dm,
                                    int8_t channel_idx, const char* peer) {
    MsgPathEntry* entry = findMsgPaths(hash);
    if (!entry) {
        entry = &_msg_paths[_msg_path_next];
        memset(entry, 0, sizeof(*entry));
        memcpy(entry->pkt_hash, hash, MAX_HASH_SIZE);
        _msg_path_next = (_msg_path_next + 1) % MAX_MSG_PATH_ENTRIES;
        if (_msg_path_count < MAX_MSG_PATH_ENTRIES) _msg_path_count++;
    }
    entry->is_message  = true;
    entry->is_dm       = is_dm;
    entry->channel_idx = channel_idx;
    entry->path_count  = 0;
    if (peer) {
        strncpy(entry->peer, peer, sizeof(entry->peer) - 1);
        entry->peer[sizeof(entry->peer) - 1] = '\0';
    } else {
        entry->peer[0] = '\0';
    }
}

void PunkMesh::setDefaultScope(const char* name) {
    if (!name || name[0] == '\0') {
        memset(_prefs.default_scope_name, 0, sizeof(_prefs.default_scope_name));
        memset(_prefs.default_scope_key, 0, sizeof(_prefs.default_scope_key));
    } else {
        strncpy(_prefs.default_scope_name, name, sizeof(_prefs.default_scope_name) - 1);
        _prefs.default_scope_name[sizeof(_prefs.default_scope_name) - 1] = '\0';
        // Region key = SHA256(name) truncated to 16 bytes — same derivation as
        // MeshCore TransportKeyStore::getAutoKeyFor for a hashtag-region name.
        SHA256 sha;
        sha.update((const uint8_t*)_prefs.default_scope_name, strlen(_prefs.default_scope_name));
        sha.finalize(_prefs.default_scope_key, sizeof(_prefs.default_scope_key));
    }
    savePrefs();
}

void PunkMesh::sendFloodWithScope(mesh::Packet* pkt, uint32_t delay_millis) {
    TransportKey scope;
    memcpy(scope.key, _prefs.default_scope_key, sizeof(scope.key));
    if (scope.isNull()) {
        sendFlood(pkt, delay_millis, pathHashSize());
    } else {
        uint16_t codes[2];
        codes[0] = scope.calcTransportCode(pkt);
        codes[1] = 0;  // single scope; second code reserved (region/return)
        sendFlood(pkt, codes, delay_millis, pathHashSize());
    }
}

void PunkMesh::sendFloodScoped(const ContactInfo& recipient, mesh::Packet* pkt, uint32_t delay_millis) {
    pkt->calculatePacketHash(_last_tx_hash);

    uint8_t saved_header = pkt->header;
    uint8_t saved_payload[MAX_PACKET_PAYLOAD];
    uint16_t saved_len = pkt->payload_len;
    if (_prefs.msg_repeat_enabled) {
        memcpy(saved_payload, pkt->payload, pkt->payload_len);
    }

    // Base sendFloodScoped ignores the recipient and floods unscoped with a
    // 1-byte path hash; route through sendFloodWithScope so the multi-byte path
    // size AND the configured default transport scope (region) are applied.
    sendFloodWithScope(pkt, delay_millis);

    if (_prefs.msg_repeat_enabled) {
        registerPendingRepeat(_last_tx_hash, saved_header, saved_payload, saved_len);
    }
}

void PunkMesh::sendFloodScoped(const mesh::GroupChannel& channel, mesh::Packet* pkt, uint32_t delay_millis) {
    pkt->calculatePacketHash(_last_tx_hash);

    uint8_t saved_header = pkt->header;
    uint8_t saved_payload[MAX_PACKET_PAYLOAD];
    uint16_t saved_len = pkt->payload_len;
    if (_prefs.msg_repeat_enabled) {
        memcpy(saved_payload, pkt->payload, pkt->payload_len);
    }

    // Base sendFloodScoped ignores the channel and floods unscoped with a
    // 1-byte path hash; route through sendFloodWithScope so the multi-byte path
    // size AND the configured default transport scope (region) are applied.
    sendFloodWithScope(pkt, delay_millis);

    if (_prefs.msg_repeat_enabled) {
        registerPendingRepeat(_last_tx_hash, saved_header, saved_payload, saved_len);
    }
}

// ── Message repeat ───────────────────────────────────────────────

void PunkMesh::registerPendingRepeat(const uint8_t* hash, uint8_t header,
                                     const uint8_t* payload, uint16_t payload_len) {
    int slot = -1;
    for (int i = 0; i < MAX_PENDING_REPEATS; i++) {
        if (!_pending_repeats[i].active) { slot = i; break; }
    }
    if (slot < 0) {
        slot = 0;
        for (int i = 1; i < MAX_PENDING_REPEATS; i++) {
            if (_pending_repeats[i].next_retry_time < _pending_repeats[slot].next_retry_time)
                slot = i;
        }
    }

    PendingRepeat& pr = _pending_repeats[slot];
    pr.header = header;
    memcpy(pr.payload, payload, payload_len);
    pr.payload_len = payload_len;
    memcpy(pr.pkt_hash, hash, MAX_HASH_SIZE);
    pr.attempts_remaining = _prefs.msg_repeat_max;
    pr.next_retry_time = millis() + (unsigned long)_prefs.msg_repeat_interval_secs * 1000UL;
    pr.active = true;

    SLog.printf("[MSG REPEAT] registered, %d retries, interval %ds\n",
                  pr.attempts_remaining, _prefs.msg_repeat_interval_secs);
}

void PunkMesh::checkPendingRepeats() {
    unsigned long now = millis();
    for (int i = 0; i < MAX_PENDING_REPEATS; i++) {
        PendingRepeat& pr = _pending_repeats[i];
        if (!pr.active) continue;

        MsgPathEntry* mpe = findMsgPaths(pr.pkt_hash);
        if (mpe && mpe->path_count > 0) {
            pr.active = false;
            RepeatOutcome& ro = _repeat_history[_repeat_history_next];
            memcpy(ro.pkt_hash, pr.pkt_hash, MAX_HASH_SIZE);
            ro.status = 2;
            _repeat_history_next = (_repeat_history_next + 1) % MAX_REPEAT_HISTORY;
            SLog.println("[MSG REPEAT] echo heard, confirmed");
            continue;
        }

        if (now < pr.next_retry_time) continue;

        if (pr.attempts_remaining == 0) {
            pr.active = false;
            RepeatOutcome& ro = _repeat_history[_repeat_history_next];
            memcpy(ro.pkt_hash, pr.pkt_hash, MAX_HASH_SIZE);
            ro.status = 3;
            _repeat_history_next = (_repeat_history_next + 1) % MAX_REPEAT_HISTORY;
            SLog.println("[MSG REPEAT] exhausted, no echo heard");
            continue;
        }

        auto pkt = obtainNewPacket();
        if (!pkt) continue;

        pkt->header = pr.header;
        memcpy(pkt->payload, pr.payload, pr.payload_len);
        pkt->payload_len = pr.payload_len;
        sendFlood(pkt, (uint32_t)0, pathHashSize());

        pr.attempts_remaining--;
        pr.next_retry_time = now + (unsigned long)_prefs.msg_repeat_interval_secs * 1000UL;

        SLog.printf("[MSG REPEAT] retransmit, %d remaining\n", pr.attempts_remaining);
    }
}

int PunkMesh::getRepeatStatus(const uint8_t* hash) {
    for (int i = 0; i < MAX_PENDING_REPEATS; i++) {
        if (_pending_repeats[i].active &&
            memcmp(_pending_repeats[i].pkt_hash, hash, MAX_HASH_SIZE) == 0) {
            return 1;
        }
    }
    for (int i = 0; i < MAX_REPEAT_HISTORY; i++) {
        if (memcmp(_repeat_history[i].pkt_hash, hash, MAX_HASH_SIZE) == 0 &&
            _repeat_history[i].status != 0) {
            return _repeat_history[i].status;
        }
    }
    MsgPathEntry* mpe = findMsgPaths(hash);
    if (mpe && mpe->path_count > 0) return 2;
    return 0;
}

// ─────────────────────────────────────────────────────────────────

PunkMesh::PunkMesh(mesh::Radio &radio, StdRNG &rng, mesh::RTCClock &rtc, SimpleMeshTables &tables)
    : BaseChatMesh(radio, *new ArduinoMillis(), rng, rtc, *new StaticPoolPacketManager(16), tables)
{
    // defaults
    memset(&_prefs, 0, sizeof(_prefs));
    _prefs.airtime_factor = 2.0; // one third
    strcpy(_prefs.node_name, "Meshpunk T-deck");
    _prefs.freq = LORA_FREQ;
    _prefs.tx_power_dbm = LORA_TX_POWER;
    _prefs.bandwidth = LORA_BW;
    _prefs.spreading_factor = LORA_SF;
    _prefs.coding_rate = LORA_CR;
    _prefs.ble_pin = BLE_PIN_CODE;
    _prefs.path_hash_mode = 0;
    _prefs.autoadd_config = 0;
    _prefs.autoadd_max_hops = 0;
    _prefs.manual_add_contacts = 0;  // auto-add all advert types (default)
    _prefs.advert_loc_policy = 0;    // don't share GPS location in adverts (privacy default)
    _prefs.msg_repeat_enabled = 0;
    _prefs.msg_repeat_max = 3;
    _prefs.msg_repeat_interval_secs = 30;
    _prefs.archive_contacts = 1;  // default on (preserves prior always-archive)
    _prefs.contact_overwrite = 1; // default on: overwrite oldest non-fav when full

    memset(_pending_repeats, 0, sizeof(_pending_repeats));
    memset(_repeat_history, 0, sizeof(_repeat_history));

    command[0] = 0;
    curr_recipient = NULL;
    _storage = &LittleFS; // default, overridden by setStorage() if SD available
    _storage_prefix = "";
}

float PunkMesh::getFreqPref() const { return _prefs.freq; }
uint8_t PunkMesh::getTxPowerPref() const { return _prefs.tx_power_dbm; }
float PunkMesh::getBandwidthPref() const { return _prefs.bandwidth; }
uint8_t PunkMesh::getSpreadingFactorPref() const { return _prefs.spreading_factor; }
uint8_t PunkMesh::getCodingRatePref() const { return _prefs.coding_rate; }

void PunkMesh::begin()
{
    BaseChatMesh::begin();

    // Try to load saved identity from storage (SD or LittleFS)
    String idPath = storagePath(_storage_prefix, "/identity");
    String prefsPath = storagePath(_storage_prefix, "/node_prefs");
    SLog.printf("[STORAGE] Identity path: %s\n", idPath.c_str());
    SLog.printf("[STORAGE] Prefs path: %s\n", prefsPath.c_str());

    bool identity_loaded = false;
    bool id_is_sd = (_storage != &LittleFS);
    if (id_is_sd) sd_spi_take();
    if (_storage->exists(idPath.c_str())) {
        File file = _storage->open(idPath.c_str());
        if (file) {
            identity_loaded = self_id.readFrom(file);
            file.close();
            if (identity_loaded) {
                SLog.printf("[STORAGE] Loaded identity from %s\n", idPath.c_str());
            } else {
                SLog.printf("[STORAGE] WARNING: Failed to read %s\n", idPath.c_str());
            }
        }
    } else {
        SLog.printf("[STORAGE] No identity file at %s\n", idPath.c_str());
    }
    if (id_is_sd) sd_spi_release();

    if (id_is_sd) {
        if (identity_loaded && !LittleFS.exists("/identity")) {
            File lfs_file = LittleFS.open("/identity", "w", true);
            if (lfs_file) {
                self_id.writeTo(lfs_file);
                lfs_file.close();
                SLog.println("[STORAGE] Copied identity from SD to LittleFS");
            }
        } else if (!identity_loaded && LittleFS.exists("/identity")) {
            File lfs_file = LittleFS.open("/identity");
            if (lfs_file) {
                identity_loaded = self_id.readFrom(lfs_file);
                lfs_file.close();
                if (identity_loaded) {
                    SLog.println("[STORAGE] Loaded identity from LittleFS fallback");
                    sd_spi_take();
                    File sd_file = _storage->open(idPath.c_str(), "w", true);
                    if (sd_file) {
                        self_id.writeTo(sd_file);
                        sd_file.close();
                        SLog.println("[STORAGE] Copied identity from LittleFS to SD");
                    }
                    sd_spi_release();
                }
            }
        }
    }

    // If no saved identity, generate a new one
    if (!identity_loaded) {
        SLog.println("[STORAGE] Generating new identity...");
        ((StdRNG *)getRNG())->begin(esp_random());

        self_id = mesh::LocalIdentity(getRNG());
        int count = 0;
        while (count < 10 && (self_id.pub_key[0] == 0x00 || self_id.pub_key[0] == 0xFF)) {
            self_id = mesh::LocalIdentity(getRNG());
            count++;
        }

        bool is_sd = (_storage != &LittleFS);
        if (is_sd) sd_spi_take();
        File file = _storage->open(idPath.c_str(), "w", true);
        if (file) {
            bool ok = self_id.writeTo(file);
            file.close();
            SLog.printf("[STORAGE] Identity saved to %s: %s\n", idPath.c_str(), ok ? "OK" : "FAILED");
        } else {
            SLog.printf("[STORAGE] ERROR: Cannot open %s for writing!\n", idPath.c_str());
        }
        if (is_sd) {
            sd_spi_release();
            File lfs_file = LittleFS.open("/identity", "w", true);
            if (lfs_file) {
                self_id.writeTo(lfs_file);
                lfs_file.close();
                SLog.println("[STORAGE] Identity also saved to LittleFS");
            }
        }
    }

    // Load persisted prefs (key=value text format)
    if (_storage->exists(prefsPath.c_str()))
    {
        File file = _storage->open(prefsPath.c_str());
        if (file)
        {
            char line[128];
            while (file.available()) {
                int len = 0;
                while (file.available() && len < (int)sizeof(line) - 1) {
                    char ch = file.read();
                    if (ch == '\n' || ch == '\r') break;
                    line[len++] = ch;
                }
                line[len] = '\0';
                if (len == 0) continue;

                char *eq = strchr(line, '=');
                if (!eq) continue;
                *eq = '\0';
                const char *key = line;
                const char *val = eq + 1;

                if (strcmp(key, "name") == 0) strncpy(_prefs.node_name, val, sizeof(_prefs.node_name) - 1);
                else if (strcmp(key, "freq") == 0) _prefs.freq = atof(val);
                else if (strcmp(key, "tx_power") == 0) _prefs.tx_power_dbm = atoi(val);
                else if (strcmp(key, "bandwidth") == 0) _prefs.bandwidth = atof(val);
                else if (strcmp(key, "spreading_factor") == 0) _prefs.spreading_factor = atoi(val);
                else if (strcmp(key, "coding_rate") == 0) _prefs.coding_rate = atoi(val);
                else if (strcmp(key, "airtime_factor") == 0) _prefs.airtime_factor = atof(val);
                else if (strcmp(key, "lat") == 0) _prefs.node_lat = atof(val);
                else if (strcmp(key, "lon") == 0) _prefs.node_lon = atof(val);
                else if (strcmp(key, "contact_overwrite") == 0) _prefs.contact_overwrite = atoi(val);
                else if (strcmp(key, "rx_boost") == 0) _prefs.rx_boost = atoi(val);
                else if (strcmp(key, "ble_pin") == 0) _prefs.ble_pin = strtoul(val, NULL, 10);
                else if (strcmp(key, "path_hash_mode") == 0) _prefs.path_hash_mode = atoi(val);
                else if (strcmp(key, "autoadd_config") == 0) _prefs.autoadd_config = atoi(val);
                else if (strcmp(key, "autoadd_max_hops") == 0) _prefs.autoadd_max_hops = atoi(val);
                else if (strcmp(key, "manual_add_contacts") == 0) _prefs.manual_add_contacts = atoi(val);
                else if (strcmp(key, "advert_loc_policy") == 0) _prefs.advert_loc_policy = atoi(val);
                else if (strcmp(key, "archive_contacts") == 0) _prefs.archive_contacts = atoi(val);
                else if (strcmp(key, "default_scope_name") == 0) strncpy(_prefs.default_scope_name, val, 30);
                else if (strcmp(key, "default_scope_key") == 0) {
                    for (int dk = 0; dk < 16 && val[dk*2] && val[dk*2+1]; dk++) {
                        char hex[3] = { val[dk*2], val[dk*2+1], 0 };
                        _prefs.default_scope_key[dk] = (uint8_t)strtoul(hex, NULL, 16);
                    }
                }
                else if (strcmp(key, "msg_repeat_enabled") == 0) _prefs.msg_repeat_enabled = atoi(val);
                else if (strcmp(key, "msg_repeat_max") == 0) _prefs.msg_repeat_max = atoi(val);
                else if (strcmp(key, "msg_repeat_interval") == 0) _prefs.msg_repeat_interval_secs = atoi(val);
            }
            file.close();
            SLog.printf("[STORAGE] Loaded prefs from %s (name=%s, freq=%.3f)\n",
                prefsPath.c_str(), _prefs.node_name, _prefs.freq);
        }
    } else {
        SLog.printf("[STORAGE] No prefs file at %s, using defaults\n", prefsPath.c_str());
    }

    loadContacts();
    SLog.printf("[MESH INIT] Loaded %d contacts from flash\n", getNumContacts());

    _public = addChannel("Public", PUBLIC_GROUP_PSK);
    if (_public) {
        SLog.println("[MESH INIT] Public channel created OK");
        char ch_hex[13];
        mesh::Utils::toHex(ch_hex, _public->channel.hash, 6);
        SLog.printf("[MESH INIT] Channel hash: %s\n", ch_hex);
    } else {
        SLog.println("[MESH INIT] ERROR: addChannel returned NULL!");
    }

    loadChannels();
}

void PunkMesh::logRx(mesh::Packet* pkt, int len, float score) {
    last_rx_snr = _radio->getLastSNR();
    last_rx_rssi = _radio->getLastRSSI();

    pkt->calculatePacketHash(_last_pkt_hash);
    MsgPathEntry* entry = findMsgPaths(_last_pkt_hash);
    uint8_t old_count = entry ? entry->path_count : 0;

    recordMsgPath(_last_pkt_hash, pkt->path_len, pkt->path,
                  last_rx_snr, last_rx_rssi, pkt->isRouteDirect());

    entry = findMsgPaths(_last_pkt_hash);
    if (entry && entry->is_message && entry->path_count > old_count) {
        persistExtraPath(_last_pkt_hash,
                         entry->paths[entry->path_count - 1]);
    }

    // One atomic line — separate prints could interleave / be partially dropped.
    SLog.printf("[RADIO RX] rx len=%d type=%d route=%s payload_len=%d | SNR=%d RSSI=%d score=%d\n",
        len, pkt->getPayloadType(),
        pkt->isRouteDirect() ? "DIRECT" : "FLOOD",
        pkt->payload_len,
        (int)last_rx_snr, (int)last_rx_rssi, (int)(score * 1000));

#if BLE_COMPANION_ENABLED
    if (ble_companion) ble_companion->pushLogRxData(pkt, last_rx_snr, last_rx_rssi);
#endif
}

static void writePrefsToFile(fs::FS* fs, const char* path, const NodePrefs& p)
{
    File file = fs->open(path, "w", true);
    if (file) {
        file.printf("name=%s\n", p.node_name);
        file.printf("freq=%.3f\n", p.freq);
        file.printf("tx_power=%d\n", p.tx_power_dbm);
        file.printf("bandwidth=%g\n", p.bandwidth);
        file.printf("spreading_factor=%d\n", p.spreading_factor);
        file.printf("coding_rate=%d\n", p.coding_rate);
        file.printf("airtime_factor=%g\n", p.airtime_factor);
        file.printf("lat=%.6f\n", p.node_lat);
        file.printf("lon=%.6f\n", p.node_lon);
        file.printf("contact_overwrite=%d\n", p.contact_overwrite);
        file.printf("rx_boost=%d\n", p.rx_boost);
        file.printf("ble_pin=%u\n", p.ble_pin);
        file.printf("path_hash_mode=%d\n", p.path_hash_mode);
        file.printf("autoadd_config=%d\n", p.autoadd_config);
        file.printf("autoadd_max_hops=%d\n", p.autoadd_max_hops);
        file.printf("msg_repeat_enabled=%d\n", p.msg_repeat_enabled);
        file.printf("msg_repeat_max=%d\n", p.msg_repeat_max);
        file.printf("msg_repeat_interval=%d\n", p.msg_repeat_interval_secs);
        file.printf("manual_add_contacts=%d\n", p.manual_add_contacts);
        file.printf("advert_loc_policy=%d\n", p.advert_loc_policy);
        file.printf("archive_contacts=%d\n", p.archive_contacts);
        if (p.default_scope_name[0]) {
            file.printf("default_scope_name=%s\n", p.default_scope_name);
            file.print("default_scope_key=");
            for (int dk = 0; dk < 16; dk++) file.printf("%02x", p.default_scope_key[dk]);
            file.print("\n");
        }
        file.close();
        SLog.printf("[STORAGE] Prefs saved to %s\n", path);
    } else {
        SLog.printf("[STORAGE] ERROR: Cannot save prefs to %s\n", path);
    }
}

void PunkMesh::savePrefs()
{
    bool is_sd = (_storage != &LittleFS);
    if (is_sd) sd_spi_take();

    String path = storagePath(_storage_prefix, "/node_prefs");
    writePrefsToFile(_storage, path.c_str(), _prefs);

    if (is_sd) {
        sd_spi_release();
        writePrefsToFile(&LittleFS, "/node_prefs", _prefs);
    }
}

bool PunkMesh::saveIdentity() {
    String idPath = storagePath(_storage_prefix, "/identity");
    bool is_sd = (_storage != &LittleFS);
    if (is_sd) sd_spi_take();
    File file = _storage->open(idPath.c_str(), "w", true);
    bool ok = false;
    if (file) {
        ok = self_id.writeTo(file);
        file.close();
    }
    if (is_sd) sd_spi_release();
    if (is_sd) {
        File lfs_file = LittleFS.open("/identity", "w", true);
        if (lfs_file) {
            self_id.writeTo(lfs_file);
            lfs_file.close();
        }
    }
    return ok;
}

PunkMesh::SendResult PunkMesh::sendAndPersistDM(ContactInfo& recipient,
                                                  uint32_t timestamp,
                                                  uint8_t attempt,
                                                  const char* text) {
    SendResult r;
    r.expected_ack = 0;
    r.est_timeout = 0;
    r.has_hash = false;

    r.code = sendMessage(recipient, timestamp, attempt, text,
                         r.expected_ack, r.est_timeout);
    if (r.code == MSG_SEND_FAILED) return r;

    bool is_flood = (r.code == MSG_SEND_SENT_FLOOD);
    if (is_flood) {
        memcpy(r.tx_hash, _last_tx_hash, MAX_HASH_SIZE);
        r.has_hash = true;
    }

    appendDMMessage(recipient.name, _prefs.node_name, text,
                   timestamp, 0.0f, 0.0f, 0,
                   r.code == MSG_SEND_SENT_DIRECT,
                   0, nullptr, is_flood ? r.tx_hash : nullptr);
    if (is_flood) {
        preRegisterSentHash(r.tx_hash, true, -1, recipient.name);
    }
    return r;
}

bool PunkMesh::sendAndPersistChannelMsg(int channel_idx, uint32_t timestamp,
                                         const char* text, int tlen,
                                         uint8_t* out_hash) {
    ChannelDetails cd;
    if (!getChannel(channel_idx, cd) || cd.name[0] == '\0') return false;

    bool ok = sendGroupMessage(timestamp, cd.channel,
                               _prefs.node_name, text, tlen);
    if (!ok) return false;

    uint8_t local_hash[MAX_HASH_SIZE];
    memcpy(local_hash, _last_tx_hash, MAX_HASH_SIZE);
    appendChannelMessage(channel_idx, _prefs.node_name, text, timestamp,
                        0.0f, 0.0f, 0, false, 0, nullptr, local_hash);
    preRegisterSentHash(local_hash, false, (int8_t)channel_idx, nullptr);
    if (out_hash) memcpy(out_hash, local_hash, MAX_HASH_SIZE);
    return true;
}

void PunkMesh::showWelcome()
{
    SLog.println("===== MeshCore Chat Terminal =====");
    SLog.println();
    SLog.printf("WELCOME  %s\n", _prefs.node_name);

    // SLog.print("Public key: ");
    // mesh::Utils::printHex(Serial, self_id.pub_key, PUB_KEY_SIZE);

    // SLog.print("Private key: ");
    // mesh::Utils::printHex(Serial, self_id.prv_key, PRIV_KEY_SIZE);

    self_id.printTo(Serial);

    SLog.println();
    SLog.println("   (enter 'help' for basic commands)");
    SLog.println();
}

void PunkMesh::sendSelfAdvert(int delay_millis)
{
    SLog.printf("[MESH TX] sendSelfAdvert: name=%s, delay=%d ms\n", _prefs.node_name, delay_millis);
    auto pkt = buildSelfAdvert();
    if (pkt)
    {
        SLog.printf("[MESH TX] Advert packet created, payload_len=%d, sending flood...\n", pkt->payload_len);
        sendFlood(pkt, delay_millis, pathHashSize());
        SLog.println("[MESH TX] Advert flood sent.");
    }
    else
    {
        SLog.println("[MESH TX] ERROR: createSelfAdvert returned NULL!");
    }
}

// ContactVisitor
void PunkMesh::onContactVisit(const ContactInfo &contact)
{
    SLog.printf("   %s - ", contact.name);
    char tmp[40];
    int32_t secs = contact.last_advert_timestamp - getRTCClock()->getCurrentTime();
    AdvertTimeHelper::formatRelativeTimeDiff(tmp, secs, false);
    SLog.println(tmp);
}

void PunkMesh::handleCommand(const char *command)
{
    while (*command == ' ')
        command++; // skip leading spaces

    if (memcmp(command, "send ", 5) == 0)
    {
        if (curr_recipient)
        {
            const char *text = &command[5];
            uint32_t est_timeout;

            int result = sendMessage(*curr_recipient, getRTCClock()->getCurrentTime(), 0, text, expected_ack_crc, est_timeout);
            if (result == MSG_SEND_FAILED)
            {
                SLog.println("   ERROR: unable to send.");
            }
            else
            {
                last_msg_sent = _ms->getMillis();
                SLog.printf("   (message sent - %s)\n", result == MSG_SEND_SENT_FLOOD ? "FLOOD" : "DIRECT");
            }
        }
        else
        {
            SLog.println("   ERROR: no recipient selected (use 'to' cmd).");
        }
    }
    else if (memcmp(command, "public ", 7) == 0)
    { // send GroupChannel msg
        SLog.printf("[MESH TX] Serial 'public' command, text=\"%s\"\n", &command[7]);

        if (!_public) {
            SLog.println("[MESH TX] ERROR: _public channel is NULL!");
        } else {
            uint32_t timestamp = getRTCClock()->getCurrentTime();
            SLog.printf("[MESH TX] timestamp=%u, sender=%s\n", timestamp, _prefs.node_name);

            bool ok = sendGroupMessage(timestamp, _public->channel, _prefs.node_name, &command[7], strlen(&command[7]));
            SLog.printf("[MESH TX] sendGroupMessage returned %s\n", ok ? "true" : "false");
        }
    }
    else if (memcmp(command, "list", 4) == 0)
    { // show Contact list, by most recent
        int n = 0;
        if (command[4] == ' ')
        { // optional param, last 'N'
            n = atoi(&command[5]);
        }
        scanRecentContacts(n, this);
    }
    else if (strcmp(command, "clock") == 0)
    { // show current time
        uint32_t now = getRTCClock()->getCurrentTime();
        DateTime dt = DateTime(now);
        SLog.printf("%02d:%02d - %d/%d/%d UTC\n", dt.hour(), dt.minute(), dt.day(), dt.month(), dt.year());
    }
    else if (memcmp(command, "time ", 5) == 0)
    { // set time (to epoch seconds)
        uint32_t secs = _atoi(&command[5]);
        setClock(secs);
    }
    else if (memcmp(command, "to ", 3) == 0)
    { // set current recipient
        curr_recipient = searchContactsByPrefix(&command[3]);
        if (curr_recipient)
        {
            SLog.printf("   Recipient %s now selected.\n", curr_recipient->name);
        }
        else
        {
            SLog.println("   Error: Name prefix not found.");
        }
    }
    else if (strcmp(command, "to") == 0)
    { // show current recipient
        if (curr_recipient)
        {
            SLog.printf("   Current: %s\n", curr_recipient->name);
        }
        else
        {
            SLog.println("   Err: no recipient selected");
        }
    }
    else if (strcmp(command, "advert") == 0)
    {
        auto pkt = buildSelfAdvert();
        if (pkt)
        {
            sendZeroHop(pkt);
            SLog.println("   (advert sent, zero hop).");
        }
        else
        {
            SLog.println("   ERR: unable to send");
        }
    }
    else if (strcmp(command, "reset path") == 0)
    {
        if (curr_recipient)
        {
            resetPathTo(*curr_recipient);
            saveOneContact(*curr_recipient);
            SLog.println("   Done.");
        }
    }
    else if (memcmp(command, "card", 4) == 0)
    {
        SLog.printf("Hello %s\n", _prefs.node_name);
        auto pkt = buildSelfAdvert();
        if (pkt)
        {
            uint8_t len = pkt->writeTo(tmp_buf);
            releasePacket(pkt); // undo the obtainNewPacket()

            mesh::Utils::toHex(hex_buf, tmp_buf, len);
            SLog.println("Your MeshCore biz card:");
            SLog.print("meshcore://");
            SLog.println(hex_buf);
            SLog.println();
        }
        else
        {
            SLog.println("  Error");
        }
    }
    else if (memcmp(command, "import ", 7) == 0)
    {
        importCard(&command[7]);
    }
    else if (memcmp(command, "set ", 4) == 0)
    {
        const char *config = &command[4];
        if (memcmp(config, "af ", 3) == 0)
        {
            _prefs.airtime_factor = atof(&config[3]);
            savePrefs();
            SLog.println("  OK");
        }
        else if (memcmp(config, "name ", 5) == 0)
        {
            StrHelper::strncpy(_prefs.node_name, &config[5], sizeof(_prefs.node_name));
            savePrefs();
            SLog.println("  OK");
        }
        else if (memcmp(config, "lat ", 4) == 0)
        {
            _prefs.node_lat = atof(&config[4]);
            savePrefs();
            SLog.println("  OK");
        }
        else if (memcmp(config, "lon ", 4) == 0)
        {
            _prefs.node_lon = atof(&config[4]);
            savePrefs();
            SLog.println("  OK");
        }
        else if (memcmp(config, "tx ", 3) == 0)
        {
            _prefs.tx_power_dbm = atoi(&config[3]);
            savePrefs();
            SLog.println("  OK - reboot to apply");
        }
        else if (memcmp(config, "freq ", 5) == 0)
        {
            _prefs.freq = atof(&config[5]);
            savePrefs();
            SLog.println("  OK - reboot to apply");
        }
        else if (memcmp(config, "bw ", 3) == 0)
        {
            _prefs.bandwidth = atof(&config[3]);
            savePrefs();
            SLog.println("  OK - reboot to apply");
        }
        else if (memcmp(config, "sf ", 3) == 0)
        {
            _prefs.spreading_factor = atoi(&config[3]);
            savePrefs();
            SLog.println("  OK - reboot to apply");
        }
        else if (memcmp(config, "cr ", 3) == 0)
        {
            _prefs.coding_rate = atoi(&config[3]);
            savePrefs();
            SLog.println("  OK - reboot to apply");
        }
        else
        {
            SLog.printf("  ERROR: unknown config: %s\n", config);
        }
    }
    else if (memcmp(command, "ver", 3) == 0)
    {
        SLog.println(FIRMWARE_VER_TEXT);
    }
    else if (strcmp(command, "diag") == 0)
    {
        SLog.println("===== MESH DIAGNOSTICS =====");
        SLog.printf("  Storage: %s, prefix: \"%s\"\n",
            (_storage == &LittleFS) ? "LittleFS" : "SD", _storage_prefix.c_str());
        SLog.printf("  Node name: %s\n", _prefs.node_name);
        SLog.printf("  Freq pref (runtime): %.3f MHz\n", _prefs.freq);
        SLog.printf("  Freq (build-time):   %.3f MHz\n", (float)LORA_FREQ);
        SLog.printf("  BW pref: %.1f kHz (build: %d)\n", _prefs.bandwidth, LORA_BW);
        SLog.printf("  SF pref: %d (build: %d)\n", _prefs.spreading_factor, LORA_SF);
        SLog.printf("  CR pref: %d (build: %d)\n", _prefs.coding_rate, LORA_CR);
        SLog.printf("  TX power pref: %d dBm (build: %d)\n", _prefs.tx_power_dbm, LORA_TX_POWER);
        SLog.printf("  Airtime factor: %.2f\n", _prefs.airtime_factor);
        SLog.printf("  GPS: %.4f, %.4f\n", _prefs.node_lat, _prefs.node_lon);
        if (_prefs.freq != (float)LORA_FREQ) {
            SLog.println("  *** WARNING: runtime freq != build freq! Old /node_prefs? ***");
        }
        char pk_hex[PUB_KEY_SIZE * 2 + 1];
        mesh::Utils::toHex(pk_hex, self_id.pub_key, PUB_KEY_SIZE);
        SLog.printf("  Pub key: %s\n", pk_hex);
        SLog.printf("  Num contacts: %d\n", getNumContacts());
        SLog.printf("  Public channel: %s\n", _public ? "configured" : "NULL (PROBLEM!)");
        SLog.printf("  Lua runtime: %s\n", lua_runtime ? "attached" : "NULL (PROBLEM!)");
        SLog.printf("  RTC clock: %u\n", getRTCClock()->getCurrentTime());
        SLog.printf("  Uptime: %lu ms\n", millis());
        SLog.println();
        SLog.println("  Contacts:");
        if (getNumContacts() == 0) {
            SLog.println("    (none)");
        } else {
            scanRecentContacts(0, this);
        }
        SLog.println("============================");
    }
    else if (memcmp(command, "help", 4) == 0)
    {
        SLog.println("Commands:");
        SLog.println("   set {name|lat|lon|freq|tx|bw|sf|cr|af} {value}");
        SLog.println("   card");
        SLog.println("   import {biz card}");
        SLog.println("   clock");
        SLog.println("   time <epoch-seconds>");
        SLog.println("   list {n}");
        SLog.println("   to <recipient name or prefix>");
        SLog.println("   to");
        SLog.println("   send <text>");
        SLog.println("   advert");
        SLog.println("   reset path");
        SLog.println("   public <text>");
        SLog.println("   diag");
    }
    else
    {
        SLog.print("   ERROR: unknown command: ");
        SLog.println(command);
    }
}

void PunkMesh::loop()
{
    BaseChatMesh::loop();
    if (_prefs.msg_repeat_enabled) checkPendingRepeats();

    int len = strlen(command);
    while (Serial.available() && len < sizeof(command) - 1)
    {
        char c = Serial.read();
        if (c != '\n')
        {
            command[len++] = c;
            command[len] = 0;
        }
        SLog.print(c);
    }
    if (len == sizeof(command) - 1)
    { // command buffer full
        command[sizeof(command) - 1] = '\r';
    }

    if (len > 0 && command[len - 1] == '\r')
    {                         // received complete line
        command[len - 1] = 0; // replace newline with C string null terminator

        handleCommand(command);
        command[0] = 0; // reset command buffer
    }
}
