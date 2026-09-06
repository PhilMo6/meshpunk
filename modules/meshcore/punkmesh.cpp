#include "punkmesh.h"
#include "mc_bridges.h"   // MC-PKG divergence: type-boundary mstore bridges
#include "boards/board_pins.h"   // MESHPUNK_BOARD_LABEL — default node name
#include <LittleFS.h>
#include <esp_heap_caps.h>
#include <helpers/TransportKeyStore.h>
#include <SHA256.h>
#include "meshpunk_sync.h"
#include "ble_companion.h"
#include "notify.h"
#include "usb_manager.h"   // UsbFlashGuard — pause USB audio around flash writes

// Shared-SPI-bus lock pair (defined in main.cpp).
// sd_spi_take()    — acquire spi_bus_mutex before any SD operation.
// sd_spi_release() — release spi_bus_mutex after the SD file handle is closed.
// sd_spi_take() is inline in meshpunk_sync.h (just SPI_LOCK); no extern decl needed.
extern void sd_spi_release();
extern PunkMesh* the_mesh;

// emoji_font.cpp — multi-codepoint emoji sequences <-> PUA codepoints.
// Rule: wire + disk carry real Unicode; text pushed up to Lua is composed so
// a ZWJ sequence renders as ONE glyph. Both return a malloc'd string or NULL
// when nothing changed (caller keeps the original); free() the result.
extern "C" char * emoji_compose(const char * in);


// The store's wire constants are frozen copies of the MeshCore values at the
// 2026-08-24 lift; a submodule update that changes them must not silently
// change the on-disk record format.
static_assert(MSTORE_HASH_SIZE == MAX_HASH_SIZE, "store hash size frozen at lift value");
static_assert(MSTORE_PATH_MAX == MAX_PATH_SIZE, "store path size frozen at lift value");
static_assert(MSTORE_MAX_CHANNELS == MAX_GROUP_CHANNELS, "store channel cap frozen at lift value");
static_assert(MSTORE_RPATHS_MAX == MAX_PATHS_PER_MSG, "store rpath cap frozen at lift value");
static_assert(RCAP_HASH_SIZE == MAX_HASH_SIZE, "capture hash size frozen at lift value");
static_assert(RCAP_FRAME_MAX == MAX_TRANS_UNIT, "capture frame size frozen at lift value");

// Storage helpers. Message/routing persistence lives in mesh_store (mstore::);
// PunkMesh keeps its own _storage/_storage_prefix for the MeshCore-side files
// (contacts, channels, prefs, archive) and forwards the choice to the store.
void PunkMesh::setStorage(fs::FS* fs, const char* prefix) {
    _storage = fs;
    _storage_prefix = String(prefix);
    // MC-PKG divergence: the HOST owns the shared store's backend (set before
    // any module loads); prefix here is the full VFS root for OUR files only.
    // (Re)build the archive dedup index for this backend's log. At boot this
    // is also what allocates it — setStorage runs early in setup(), so the
    // 192KB block lands low in PSRAM, before the Lua arena/gap form.
    archiveIndexInit();
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
    {                                           // arg2: text (composed for UI)
        char * comp = emoji_compose(text);
        lua_pushstring(L, comp ? comp : text);
        if (comp) free(comp);
    }
    lua_pushinteger(L, timestamp);              // arg3: timestamp
    lua_pushboolean(L, direct);                 // arg4: direct
    lua_pushinteger(L, hops);                   // arg5: hops
    lua_pushnumber(L, snr);                     // arg6: snr
    lua_pushnumber(L, rssi);                    // arg7: rssi
    lua_pushinteger(L, channel_idx);            // arg8: channel_idx (-1 if unknown)
    lua_pushboolean(L, contains_mention(text, the_mesh->_prefs.node_name)); // arg9: is_mention
    mstore::push_path_table(L, path_len, path);         // arg10: path
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
    {                                           // arg2: text (composed for UI)
        char * comp = emoji_compose(text);
        lua_pushstring(L, comp ? comp : text);
        if (comp) free(comp);
    }
    lua_pushinteger(L, timestamp);              // arg3: timestamp
    lua_pushboolean(L, direct);                 // arg4: direct
    lua_pushinteger(L, hops);                   // arg5: hops
    lua_pushnumber(L, snr);                     // arg6: snr
    lua_pushnumber(L, rssi);                    // arg7: rssi
    mstore::push_path_table(L, path_len, path);         // arg8: path
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

// Room server post: threads under the ROOM's name, displayed as `author`.
void lua_mesh_push_room_message(lua_State* L, const char* room_name, const char* author, uint8_t hops, bool direct, uint32_t timestamp, const char *text, float snr, float rssi, uint16_t path_len, const uint8_t* path, const uint8_t* pkt_hash) {
    lua_getglobal(L, "require");
    lua_pushstring(L, "lib/mesh/messages");
    if (lua_pcall(L, 1, 1, 0) != LUA_OK) {
        lua_pop(L, 1);
        return;
    }
    lua_getfield(L, -1, "__dispatch_room");
    if (!lua_isfunction(L, -1)) {
        lua_pop(L, 2);
        return;
    }

    lua_pushstring(L, room_name);               // arg1: room (thread key)
    lua_pushstring(L, author);                  // arg2: author (display from)
    {                                           // arg3: text (composed for UI)
        char * comp = emoji_compose(text);
        lua_pushstring(L, comp ? comp : text);
        if (comp) free(comp);
    }
    lua_pushinteger(L, timestamp);              // arg4: timestamp
    lua_pushboolean(L, direct);                 // arg5: direct
    lua_pushinteger(L, hops);                   // arg6: hops
    lua_pushnumber(L, snr);                     // arg7: snr
    lua_pushnumber(L, rssi);                    // arg8: rssi
    mstore::push_path_table(L, path_len, path);         // arg9: path
    if (pkt_hash) {                             // arg10: hash (hex string)
        char hex[MAX_HASH_SIZE * 2 + 1];
        mesh::Utils::toHex(hex, pkt_hash, MAX_HASH_SIZE);
        lua_pushstring(L, hex);
    } else {
        lua_pushnil(L);
    }

    if (lua_pcall(L, 10, 0, 0) != LUA_OK) {
        SLog.printf("__dispatch_room failed: %s\n", lua_tostring(L, -1));
        lua_pop(L, 1);
    }
    lua_pop(L, 1); // pop module
}

// Repeater CLI reply: threads under the repeater's name (its chat = console).
void lua_mesh_push_cli_response(lua_State* L, const char* name, const char* text, uint32_t timestamp) {
    lua_getglobal(L, "require");
    lua_pushstring(L, "lib/mesh/messages");
    if (lua_pcall(L, 1, 1, 0) != LUA_OK) {
        lua_pop(L, 1);
        return;
    }
    lua_getfield(L, -1, "__dispatch_cli");
    if (!lua_isfunction(L, -1)) {
        lua_pop(L, 2);
        return;
    }
    lua_pushstring(L, name);
    lua_pushstring(L, text);
    lua_pushinteger(L, timestamp);
    if (lua_pcall(L, 3, 0, 0) != LUA_OK) {
        SLog.printf("__dispatch_cli failed: %s\n", lua_tostring(L, -1));
        lua_pop(L, 1);
    }
    lua_pop(L, 1); // pop module
}

// Login result for a room/repeater login sent from the device UI.
void lua_mesh_push_login_result(lua_State* L, const char* name, bool ok, uint8_t perms, uint32_t keepalive_secs) {
    lua_getglobal(L, "require");
    lua_pushstring(L, "lib/mesh/messages");
    if (lua_pcall(L, 1, 1, 0) != LUA_OK) {
        lua_pop(L, 1);
        return;
    }
    lua_getfield(L, -1, "__dispatch_login");
    if (!lua_isfunction(L, -1)) {
        lua_pop(L, 2);
        return;
    }
    lua_pushstring(L, name);
    lua_pushboolean(L, ok);
    lua_pushinteger(L, perms);
    lua_pushinteger(L, (lua_Integer)keepalive_secs);
    if (lua_pcall(L, 4, 0, 0) != LUA_OK) {
        SLog.printf("__dispatch_login failed: %s\n", lua_tostring(L, -1));
        lua_pop(L, 1);
    }
    lua_pop(L, 1); // pop module
}

// Decoded REQ_TYPE_GET_STATUS response, preformatted as display text.
void lua_mesh_push_status_text(lua_State* L, const char* name, const char* text) {
    lua_getglobal(L, "require");
    lua_pushstring(L, "lib/mesh/messages");
    if (lua_pcall(L, 1, 1, 0) != LUA_OK) {
        lua_pop(L, 1);
        return;
    }
    lua_getfield(L, -1, "__dispatch_status");
    if (!lua_isfunction(L, -1)) {
        lua_pop(L, 2);
        return;
    }
    lua_pushstring(L, name);
    lua_pushstring(L, text);
    if (lua_pcall(L, 2, 0, 0) != LUA_OK) {
        SLog.printf("__dispatch_status failed: %s\n", lua_tostring(L, -1));
        lua_pop(L, 1);
    }
    lua_pop(L, 1); // pop module
}

// Retry-ladder progress: attempt `n` of `total` is now in flight for the
// send the UI indexed under `ack`.
void lua_mesh_push_send_retry(lua_State* L, uint32_t ack, uint8_t n, uint8_t total) {
    lua_getglobal(L, "require");
    lua_pushstring(L, "lib/mesh/messages");
    if (lua_pcall(L, 1, 1, 0) != LUA_OK) {
        lua_pop(L, 1);
        return;
    }
    lua_getfield(L, -1, "__dispatch_retry");
    if (!lua_isfunction(L, -1)) {
        lua_pop(L, 2);
        return;
    }
    lua_pushinteger(L, (lua_Integer)ack);
    lua_pushinteger(L, n);
    lua_pushinteger(L, total);
    if (lua_pcall(L, 3, 0, 0) != LUA_OK) {
        SLog.printf("__dispatch_retry failed: %s\n", lua_tostring(L, -1));
        lua_pop(L, 1);
    }
    lua_pop(L, 1); // pop module
}

// A keep-alive session expired (server stopped acking our pings).
void lua_mesh_push_conn_lost(lua_State* L, const char* name) {
    lua_getglobal(L, "require");
    lua_pushstring(L, "lib/mesh/messages");
    if (lua_pcall(L, 1, 1, 0) != LUA_OK) {
        lua_pop(L, 1);
        return;
    }
    lua_getfield(L, -1, "__dispatch_conn_lost");
    if (!lua_isfunction(L, -1)) {
        lua_pop(L, 2);
        return;
    }
    lua_pushstring(L, name);
    if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
        SLog.printf("__dispatch_conn_lost failed: %s\n", lua_tostring(L, -1));
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

// Shutdown/reboot flush: the room-sync cursor and path history persist on
// lazy timers (see the dirty-flag blocks in loop()) and contacts changes can
// sit between save points — write them all now so power-off loses nothing.
void PunkMesh::flushForShutdown()
{
    saveContacts();
    if (_room_sync_dirty) { _room_sync_dirty = false; saveRoomSync(); }
    if (_path_hist_dirty) { _path_hist_dirty = false; savePathHistory(); }
}

// Full rewrite of the live store, in ARRAY-INDEX order (getContactByIdx, so file
// slot i == contacts[i]). Used on removal/clear (the array compacts) and as the
// bulk fallback. Frequent single-contact mutations use saveOneContact instead.
void PunkMesh::saveContacts()
{
    contacts_generation++;  // invalidate the Lua-side contacts cache

    bool is_sd = (_storage != &LittleFS);
    UsbFlashGuardIf _g(!is_sd);   // LittleFS backend: internal-flash write
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
    UsbFlashGuardIf _g(!is_sd);   // LittleFS backend: internal-flash write
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

// ── Room sync_since sidecar ──────────────────────────────────────────
// ContactInfo.sync_since is runtime-only in CONTACT_REC (live store AND the
// archive share that stride), so a reboot would make the next room login
// re-fetch the room's whole retained history over LoRa. /room_sync.bin keeps
// just the sync cursors: pubkey_prefix(8) + sync_since(4) per record.
// Loaded after loadContacts(); saved lazily from loop() (markRoomSyncDirty)
// so a chatty room doesn't wear flash with a write per post.
static const int ROOM_SYNC_REC = 12;

void PunkMesh::loadRoomSync()
{
    bool is_sd = (_storage != &LittleFS);
    if (is_sd) sd_spi_take();

    String path = storagePath(_storage_prefix, "/room_sync.bin");
    int applied = 0;
    if (_storage->exists(path.c_str())) {
        File file = _storage->open(path.c_str());
        if (file) {
            uint8_t rec[ROOM_SYNC_REC];
            while (file.available() >= ROOM_SYNC_REC) {
                if (file.read(rec, ROOM_SYNC_REC) != ROOM_SYNC_REC) break;
                ContactInfo* c = lookupContactByPubKey(rec, 8);
                if (c) {
                    memcpy(&c->sync_since, &rec[8], 4);
                    applied++;
                }
            }
            file.close();
        }
    }

    if (is_sd) sd_spi_release();
    if (applied) SLog.printf("[MESH INIT] Restored %d room sync cursors\n", applied);
}

void PunkMesh::saveRoomSync()
{
    bool is_sd = (_storage != &LittleFS);
    UsbFlashGuardIf _g(!is_sd);   // LittleFS backend: internal-flash write
    if (is_sd) sd_spi_take();

    String path = storagePath(_storage_prefix, "/room_sync.bin");
    File file = _storage->open(path.c_str(), "w", true);
    if (file) {
        int n = getNumContacts();
        for (int i = 0; i < n; i++) {
            ContactInfo c;
            if (!getContactByIdx(i, c)) break;
            if (c.sync_since == 0) continue;   // no cursor to remember
            uint8_t rec[ROOM_SYNC_REC];
            memcpy(rec, c.id.pub_key, 8);
            memcpy(&rec[8], &c.sync_since, 4);
            file.write(rec, ROOM_SYNC_REC);
        }
        file.close();
    }

    if (is_sd) sd_spi_release();
}

// First change after a save arms a short delay; while the delay from a
// PREVIOUS save hasn't elapsed the pending write keeps that later deadline,
// so a busy room coalesces to at most ~one write per minute (see loop()).
void PunkMesh::markRoomSyncDirty()
{
    if (!_room_sync_dirty) {
        _room_sync_dirty = true;
        unsigned long at = futureMillis(5000);
        if ((long)(at - _room_sync_save_at) > 0 || _room_sync_save_at == 0)
            _room_sync_save_at = at;
    }
}

// ── Keep-alive session watch ─────────────────────────────────────────
// See punkmesh.h. checkConnections() silently frees an expired connection
// slot; the watch turns that into a CONN_LOST event in loop().

void PunkMesh::watchConnection(const ContactInfo& contact)
{
    int free_idx = -1;
    for (int i = 0; i < 16; i++) {
        if (_conn_watch[i].active) {
            if (memcmp(_conn_watch[i].pub_key, contact.id.pub_key, PUB_KEY_SIZE) == 0) {
                strncpy(_conn_watch[i].name, contact.name, sizeof(_conn_watch[i].name) - 1);
                return;   // already watching (re-login refresh)
            }
        } else if (free_idx < 0) {
            free_idx = i;
        }
    }
    if (free_idx < 0) return;   // table full — connection still works, just unwatched
    _conn_watch[free_idx].active = true;
    memcpy(_conn_watch[free_idx].pub_key, contact.id.pub_key, PUB_KEY_SIZE);
    strncpy(_conn_watch[free_idx].name, contact.name, sizeof(_conn_watch[free_idx].name) - 1);
    _conn_watch[free_idx].name[sizeof(_conn_watch[free_idx].name) - 1] = '\0';
}

void PunkMesh::unwatchConnection(const uint8_t* pub_key)
{
    for (int i = 0; i < 16; i++) {
        if (_conn_watch[i].active &&
            memcmp(_conn_watch[i].pub_key, pub_key, PUB_KEY_SIZE) == 0) {
            _conn_watch[i].active = false;
            return;
        }
    }
}

// ── Path history persistence ─────────────────────────────────────────
// See punkmesh.h. Raw dump of _path_history with a version + record-size
// guard so a struct change just invalidates the file instead of corrupting
// the ring.
static const uint8_t PATH_HIST_VER = 1;

void PunkMesh::loadPathHistory()
{
    bool is_sd = (_storage != &LittleFS);
    if (is_sd) sd_spi_take();

    String path = storagePath(_storage_prefix, "/path_hist.bin");
    if (_storage->exists(path.c_str())) {
        File file = _storage->open(path.c_str());
        if (file) {
            uint8_t ver = 0, count = 0;
            uint16_t rec_size = 0;
            bool hdr_ok = file.read(&ver, 1) == 1 &&
                          file.read((uint8_t*)&rec_size, 2) == 2 &&
                          file.read(&count, 1) == 1;
            if (hdr_ok && ver == PATH_HIST_VER && rec_size == sizeof(ContactPathHistory)) {
                if (count > MAX_PATH_CONTACTS) count = MAX_PATH_CONTACTS;
                int n = 0;
                while (n < count &&
                       file.read((uint8_t*)&_path_history[n], sizeof(ContactPathHistory))
                           == sizeof(ContactPathHistory)) {
                    n++;
                }
                _path_history_count = n;
                SLog.printf("[MESH INIT] Restored path history for %d contacts\n", n);
            } else {
                SLog.println("[MESH INIT] path_hist.bin version/size mismatch — ignored");
            }
            file.close();
        }
    }

    if (is_sd) sd_spi_release();
}

void PunkMesh::savePathHistory()
{
    bool is_sd = (_storage != &LittleFS);
    UsbFlashGuardIf _g(!is_sd);   // LittleFS backend: internal-flash write
    if (is_sd) sd_spi_take();

    String path = storagePath(_storage_prefix, "/path_hist.bin");
    File file = _storage->open(path.c_str(), "w", true);
    if (file) {
        uint8_t ver = PATH_HIST_VER;
        uint16_t rec_size = sizeof(ContactPathHistory);
        uint8_t count = (uint8_t)_path_history_count;
        file.write(&ver, 1);
        file.write((uint8_t*)&rec_size, 2);
        file.write(&count, 1);
        for (int i = 0; i < _path_history_count; i++) {
            file.write((uint8_t*)&_path_history[i], sizeof(ContactPathHistory));
            if (is_sd && (i + 1) % 8 == 0) {
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

// The ring is dirtied by every received message (timestamp refreshes), so
// this coalesces hard: first change arms ~1 min, and loop() floors the next
// write to 10 min after each save.
void PunkMesh::markPathHistDirty()
{
    if (!_path_hist_dirty) {
        _path_hist_dirty = true;
        unsigned long at = futureMillis(60000);
        if ((long)(at - _path_hist_save_at) > 0 || _path_hist_save_at == 0)
            _path_hist_save_at = at;
    }
}

// ── Contact archive ──────────────────────────────────────────────────
// DISK-ONLY records: the archive lives in the <storage>/contacts_arch.bin
// fixed-stride log (CONTACT_REC bytes/record). See punkmesh.h. Read on demand
// (transient) for the "show archived" map union and re-add.
//
// ── Archive dedup index ──
// Open-addressed hash (linear probe) mapping every archived pubkey → byte
// offset of its newest record. It turns archiveContact into an UPSERT (an
// in-place record rewrite instead of a blind append), which is what stops the
// log growing without bound on a large mesh: with thousands of contacts on
// multi-day advert cycles and ~500 live slots, nearly every eviction re-archives
// a pubkey that is already in the log.
//   - keys are the FIRST 8 PUBKEY BYTES (ed25519 keys are uniform; collision
//     odds across 10k contacts ~1e-12, and the worst case is one archive
//     record superseding another — self-heals on that contact's next eviction)
//   - one 192KB PSRAM block allocated ONCE from setStorage() at boot, so it
//     lands low among the boot residents, below the Lua arena/gap — a fixed
//     resident, not mid-heap churn (see the meshprint fragmentation work)
//   - offset sentinel UINT32_MAX = empty slot (0 is a valid record offset)
//   - concurrency: every index reader/writer holds MESH_LOCK (all
//     archiveContact callers do; compactArchive's index-mutating phases do).
//     compactArchive's unlocked streaming passes set s_arch_compacting, which
//     routes the hot path to plain appends so it never touches the index.
#define ARCH_IDX_SLOTS     16384u             // power of two; ~11k keys @ 0.7 load
#define ARCH_IDX_MAX_USED  (ARCH_IDX_SLOTS * 7u / 10u)
#define ARCH_IDX_BUILD_MAX (1024u * 1024u)    // boot-scan cap: a legacy runaway log
                                              // stays unindexed until compacted
static uint64_t*     s_arch_key = nullptr;    // [ARCH_IDX_SLOTS] pubkey prefixes
static uint32_t*     s_arch_off = nullptr;    // [ARCH_IDX_SLOTS] record offsets
static uint32_t      s_arch_used = 0;
static bool          s_arch_built = false;    // false → appendArchiveEntry appends blindly
static volatile bool s_arch_compacting = false;

static inline uint64_t arch_key_of(const uint8_t* pub_key_or_rec) {
    uint64_t k;
    memcpy(&k, pub_key_or_rec, 8);   // records start with the pubkey, so a raw
    return k;                        // record pointer works here too
}

static inline uint32_t arch_idx_home(uint64_t key) {
    return (uint32_t)((key * 0x9E3779B97F4A7C15ull) >> 32) & (ARCH_IDX_SLOTS - 1);
}

// Slot holding `key`, or -1 when absent.
static int arch_idx_find(uint64_t key) {
    if (!s_arch_key) return -1;
    uint32_t i = arch_idx_home(key);
    for (uint32_t probes = 0; probes < ARCH_IDX_SLOTS; probes++) {
        if (s_arch_off[i] == UINT32_MAX) return -1;
        if (s_arch_key[i] == key) return (int)i;
        i = (i + 1) & (ARCH_IDX_SLOTS - 1);
    }
    return -1;
}

// Insert or update key → off. Returns false at the load cap (caller reverts to
// append-only mode; a later compaction dedups and rebuilds).
static bool arch_idx_upsert(uint64_t key, uint32_t off) {
    if (!s_arch_key) return false;
    uint32_t i = arch_idx_home(key);
    for (uint32_t probes = 0; probes < ARCH_IDX_SLOTS; probes++) {
        if (s_arch_off[i] == UINT32_MAX) {
            if (s_arch_used >= ARCH_IDX_MAX_USED) return false;
            s_arch_key[i] = key;
            s_arch_off[i] = off;
            s_arch_used++;
            return true;
        }
        if (s_arch_key[i] == key) { s_arch_off[i] = off; return true; }
        i = (i + 1) & (ARCH_IDX_SLOTS - 1);
    }
    return false;
}

static void arch_idx_reset() {
    if (!s_arch_off) return;
    for (uint32_t i = 0; i < ARCH_IDX_SLOTS; i++) s_arch_off[i] = UINT32_MAX;
    s_arch_used = 0;
}

// Allocate (once) and (re)build the archive index by streaming the log.
// Called from setStorage() — at boot (lands low in PSRAM, before the Lua
// arena/gap form) and again on a runtime backend switch (the other backend has
// its own log). A file over ARCH_IDX_BUILD_MAX is left unindexed — appends
// behave exactly as before this index existed — until compactArchive() shrinks
// it and rebuilds; scanning a multi-MB runaway log on every boot helps nobody.
void PunkMesh::archiveIndexInit()
{
    if (!s_arch_key) {
        void* blk = heap_caps_malloc(ARCH_IDX_SLOTS * 12, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!blk) {
            SLog.println("[ARCH] index alloc failed - archive stays append-only");
            return;
        }
        s_arch_key = (uint64_t*)blk;
        s_arch_off = (uint32_t*)((uint8_t*)blk + ARCH_IDX_SLOTS * 8);
    }
    arch_idx_reset();
    s_arch_built = false;
    if (!_storage) return;

    bool is_sd = (_storage != &LittleFS);
    if (is_sd) sd_spi_take();

    String path = storagePath(_storage_prefix, "/contacts_arch.bin");
    String tmp  = path + ".tmp";
    // Compaction crash recovery: interrupted between remove and rename leaves
    // only the finished .tmp — adopt it. Any other leftover .tmp is half
    // written garbage — drop it.
    if (!_storage->exists(path.c_str()) && _storage->exists(tmp.c_str()))
        _storage->rename(tmp.c_str(), path.c_str());
    else if (_storage->exists(tmp.c_str()))
        _storage->remove(tmp.c_str());

    bool too_big = false, overflow = false;
    if (_storage->exists(path.c_str())) {
        File f = _storage->open(path.c_str());
        if (f) {
            if ((uint32_t)f.size() > ARCH_IDX_BUILD_MAX) {
                too_big = true;
                SLog.printf("[ARCH] %uKB archive too large to index - run compaction\n",
                            (unsigned)(f.size() / 1024));
            } else {
                uint8_t rec[CONTACT_REC];
                uint32_t off = 0;
                int cnt = 0;
                while (f.available() >= CONTACT_REC) {
                    if (f.read(rec, CONTACT_REC) != CONTACT_REC) break;
                    if (!arch_idx_upsert(arch_key_of(rec), off)) { overflow = true; break; }
                    off += CONTACT_REC;
                    if (is_sd && ++cnt % 200 == 0) { sd_spi_release(); vTaskDelay(1); sd_spi_take(); }
                }
            }
            f.close();
        }
    }

    if (overflow) {
        arch_idx_reset();
        SLog.println("[ARCH] too many distinct archived contacts for the index - append-only");
    } else if (!too_big) {
        s_arch_built = true;   // empty/missing file = trivially indexed
        SLog.printf("[ARCH] index built: %u archived contacts\n", (unsigned)s_arch_used);
    }

    if (is_sd) sd_spi_release();
}

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

// Hot-path persistence, now an UPSERT: when the index knows this pubkey, its
// record is overwritten in place (fixed CONTACT_REC stride makes that a single
// seek+write) — the log only grows for pubkeys never archived before. Index
// miss / unbuilt / compaction-in-progress fall back to the old blind append,
// so nothing is ever dropped. Callers hold MESH_LOCK (evict/discard run in the
// mesh loop; the Lua remove binding locks), which serializes the index.
void PunkMesh::appendArchiveEntry(const ContactInfo& c)
{
    archive_generation++;  // invalidate the Lua-side union cache
    bool is_sd = (_storage != &LittleFS);
    UsbFlashGuardIf _g(!is_sd);   // LittleFS backend: internal-flash write
    if (is_sd) sd_spi_take();

    String path = storagePath(_storage_prefix, "/contacts_arch.bin");
    uint8_t rec[CONTACT_REC];
    serialize_contact(c, rec);

    if (s_arch_built && !s_arch_compacting) {
        int slot = arch_idx_find(arch_key_of(c.id.pub_key));
        if (slot >= 0) {
            File f = _storage->open(path.c_str(), "r+");
            if (f) {
                uint32_t off = s_arch_off[slot];
                if (off + CONTACT_REC <= (uint32_t)f.size() && f.seek(off)) {
                    f.write(rec, CONTACT_REC);
                    f.close();
                    if (is_sd) sd_spi_release();
                    return;
                }
                f.close();
            }
            // "r+" unsupported or a stale offset: append below instead (never
            // drop the record); the index is re-pointed at the fresh copy.
        }
    }

    File file = _storage->open(path.c_str(), "a", true);
    if (file) {
        uint32_t off = (uint32_t)file.size();   // append position = record offset
        file.write(rec, CONTACT_REC);
        file.close();
        if (s_arch_built && !s_arch_compacting &&
            !arch_idx_upsert(arch_key_of(c.id.pub_key), off)) {
            s_arch_built = false;   // load cap hit: back to append-only
            SLog.println("[ARCH] index full - append-only until next compaction");
        }
    }

    if (is_sd) sd_spi_release();
}

void PunkMesh::archiveContact(const ContactInfo& c)
{
    if (!_prefs.archive_contacts) return;  // archiving disabled by setting
    // Upsert: a re-archived pubkey overwrites its existing record in place via
    // the dedup index; only never-archived pubkeys append (see appendArchiveEntry).
    appendArchiveEntry(c);
    SLog.printf("[ARCH] Archived contact: %s\n", c.name);
}

bool PunkMesh::readdArchivedContact(const uint8_t* pub_key)
{
    ContactInfo found;
    bool have = false;
    bool is_sd = (_storage != &LittleFS);
    if (is_sd) sd_spi_take();

    String path = storagePath(_storage_prefix, "/contacts_arch.bin");

    // Index fast path (caller holds MESH_LOCK): one seek instead of a full
    // scan. The full-pubkey compare guards the astronomical prefix-collision
    // case — on mismatch we fall back to the scan rather than trust the slot.
    if (s_arch_built && !s_arch_compacting) {
        int slot = arch_idx_find(arch_key_of(pub_key));
        if (slot >= 0 && _storage->exists(path.c_str())) {
            File f = _storage->open(path.c_str());
            if (f) {
                uint8_t rec[CONTACT_REC];
                if (s_arch_off[slot] + CONTACT_REC <= (uint32_t)f.size() &&
                    f.seek(s_arch_off[slot]) &&
                    f.read(rec, CONTACT_REC) == CONTACT_REC) {
                    ContactInfo entry;
                    deserialize_contact(rec, entry);
                    if (memcmp(entry.id.pub_key, pub_key, PUB_KEY_SIZE) == 0) {
                        found = entry;
                        have = true;
                    }
                }
                f.close();
            }
        }
        if (!have && slot < 0) {
            // Index is authoritative when built: not indexed = not archived.
            if (is_sd) sd_spi_release();
            return false;
        }
    }

    // Scan the binary log for this pubkey's newest record (fixed stride, no parse).
    if (!have && _storage->exists(path.c_str()))
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

// Records currently in the archive log (including duplicates) — one stat, no scan.
uint32_t PunkMesh::archiveRecordCount()
{
    uint32_t n = 0;
    if (!_storage) return 0;
    bool is_sd = (_storage != &LittleFS);
    if (is_sd) sd_spi_take();
    String path = storagePath(_storage_prefix, "/contacts_arch.bin");
    if (_storage->exists(path.c_str())) {
        File f = _storage->open(path.c_str());
        if (f) { n = (uint32_t)f.size() / CONTACT_REC; f.close(); }
    }
    if (is_sd) sd_spi_release();
    return n;
}

// Rewrite contacts_arch.bin down to ONE record per pubkey (newest wins),
// dropping records whose contact is back in the live table (documented as
// stale/superseded — they re-archive fresh if evicted again). Streaming: two
// sequential passes plus a bounded tail merge; the archive never materializes
// in RAM. The index arrays double as the pass-1 scratch (old offsets) and are
// re-pointed to the new offsets during pass 2, so a finished compaction leaves
// the index built and exact as a byproduct. User-triggered from the Messenger.
//
// Locking: pass 0 snapshots the live table under MESH_LOCK and raises
// s_arch_compacting (concurrent evictions take the plain-append path and land
// past the size snapshot). Passes 1-2 stream WITHOUT the mesh lock. The finish
// phase takes MESH_LOCK *then* the SD lock (same order as the mesh task) to
// fold in the tail, swap the files and mark the index live — bounded work, so
// the radio is never stalled for the whole rewrite.
//
// The pass-2 "newest" test stays valid as offsets are re-pointed because the
// newest record is the LAST occurrence of its key in [0,S): earlier duplicates
// compare against the (not-yet-re-pointed) old offset and miss; once the last
// occurrence matches and the slot is re-pointed, that key never recurs.
//
// Returns 0 on success (record counts in *before_out/*after_out), else:
//   -1 no storage or no archive file, -2 index memory missing (boot alloc
//   failed), -3 too many distinct pubkeys for the index, -4 file I/O error.
int PunkMesh::compactArchive(uint32_t* before_out, uint32_t* after_out)
{
    *before_out = 0;
    *after_out = 0;
    if (!_storage) return -1;
    if (!s_arch_key) return -2;

    String path = storagePath(_storage_prefix, "/contacts_arch.bin");
    String tmp  = path + ".tmp";
    bool is_sd = (_storage != &LittleFS);
    // LittleFS backend: passes 2+ and the swap are internal-flash writes and
    // take the USB flash guard — but per-phase, NOT one guard spanning the
    // whole function: the guard must never be held while ACQUIRING MESH_LOCK
    // (the mesh-task append paths take the guard while holding MESH_LOCK, so
    // the reverse order would deadlock). See the begin/end pairs below.

    // ── Pass 0: live-table pubkey prefixes + enter append-only mode ──
    // Transient heap block (4KB @ MAX_CONTACTS=500), not stack — the binding
    // runs on loopTask whose 16KB also carries the Lua C stack.
    uint64_t* live = (uint64_t*)heap_caps_malloc(
        sizeof(uint64_t) * MAX_CONTACTS, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    int nlive = 0;
    MESH_LOCK();
    s_arch_compacting = true;
    s_arch_built = false;
    arch_idx_reset();
    if (live) {
        int n = getNumContacts();
        ContactInfo ci;
        for (int i = 0; i < n && nlive < MAX_CONTACTS; i++) {
            if (!getContactByIdx(i, ci)) break;
            live[nlive++] = arch_key_of(ci.id.pub_key);
        }
    }
    // live == NULL -> nlive stays 0: live contacts keep their archive records
    // this round (harmless, documented as superseded) instead of failing.
    MESH_UNLOCK();

    int rc = 0;
    uint32_t S = 0;      // size snapshot: passes cover [0,S), the tail merge covers the rest
    uint32_t kept = 0;
    uint32_t woff = 0;   // write offset in .tmp
    uint8_t rec[CONTACT_REC];

    // ── Passes 1-2: stream without the mesh lock ──
    if (!is_sd) usb_flash_guard_begin();   // pass 2 writes .tmp (internal flash)
    if (is_sd) sd_spi_take();
    do {
        if (!_storage->exists(path.c_str())) { rc = -1; break; }

        // Pass 1: newest offset per non-live pubkey into the index arrays.
        File f = _storage->open(path.c_str());
        if (!f) { rc = -4; break; }
        S = ((uint32_t)f.size() / CONTACT_REC) * CONTACT_REC;
        uint32_t off = 0;
        int cnt = 0;
        bool idx_ok = true;
        while (off + CONTACT_REC <= S) {
            if (f.read(rec, CONTACT_REC) != CONTACT_REC) { rc = -4; break; }
            uint64_t key = arch_key_of(rec);
            bool is_live = false;
            for (int i = 0; i < nlive; i++) {
                if (live[i] == key) { is_live = true; break; }
            }
            if (!is_live && !arch_idx_upsert(key, off)) { idx_ok = false; break; }
            off += CONTACT_REC;
            if (is_sd && ++cnt % 200 == 0) { sd_spi_release(); vTaskDelay(1); sd_spi_take(); }
        }
        f.close();
        if (rc) break;
        if (!idx_ok) { rc = -3; break; }

        // Pass 2: write each key's newest record to .tmp, re-pointing its
        // index slot at the record's new offset as it lands.
        File in = _storage->open(path.c_str());
        File out = _storage->open(tmp.c_str(), "w", true);
        if (!in || !out) {
            if (in) in.close();
            if (out) out.close();
            rc = -4;
            break;
        }
        uint32_t off2 = 0;
        cnt = 0;
        while (off2 + CONTACT_REC <= S) {
            if (in.read(rec, CONTACT_REC) != CONTACT_REC) { rc = -4; break; }
            int slot = arch_idx_find(arch_key_of(rec));
            if (slot >= 0 && s_arch_off[slot] == off2) {
                if (out.write(rec, CONTACT_REC) != CONTACT_REC) { rc = -4; break; }
                s_arch_off[slot] = woff;
                woff += CONTACT_REC;
                kept++;
            }
            off2 += CONTACT_REC;
            if (is_sd && ++cnt % 200 == 0) { sd_spi_release(); vTaskDelay(1); sd_spi_take(); }
        }
        in.close();
        out.close();
    } while (0);
    if (is_sd) sd_spi_release();
    if (!is_sd) usb_flash_guard_end();

    if (rc == 0) {
        // ── Finish: tail merge + swap, MESH_LOCK then SD lock ──
        MESH_LOCK();
        if (!is_sd) usb_flash_guard_begin();   // tail writes + swap hit flash
        if (is_sd) sd_spi_take();

        File src = _storage->open(path.c_str());
        File dst = _storage->open(tmp.c_str(), "a");
        bool ok = (src && dst);
        bool idx_ok = true;
        if (ok) {
            // Records archived while we streamed (append-only mode put them
            // past S). Copied verbatim + indexed; a duplicate this creates is
            // superseded newest-wins and cleaned by the next compaction.
            uint32_t end = ((uint32_t)src.size() / CONTACT_REC) * CONTACT_REC;
            *before_out = end / CONTACT_REC;
            uint32_t o = S;
            if (o < end && !src.seek(o)) ok = false;
            while (ok && o + CONTACT_REC <= end) {
                if (src.read(rec, CONTACT_REC) != CONTACT_REC) { ok = false; break; }
                if (dst.write(rec, CONTACT_REC) != CONTACT_REC) { ok = false; break; }
                // Cap hit: the record is safely in the file but NOT indexed. A
                // built index is authoritative for re-add, so it must not go
                // live missing keys — finish the swap, then stay unbuilt.
                if (!arch_idx_upsert(arch_key_of(rec), woff)) idx_ok = false;
                woff += CONTACT_REC;
                kept++;
                o += CONTACT_REC;
            }
        }
        if (src) src.close();
        if (dst) dst.close();

        if (ok) {
            _storage->remove(path.c_str());
            ok = _storage->rename(tmp.c_str(), path.c_str());
            // A crash between remove and rename is recovered by
            // archiveIndexInit (adopts a lone .tmp) on next boot.
        }
        if (ok) {
            s_arch_built = idx_ok;
            if (!idx_ok) {
                arch_idx_reset();
                SLog.println("[ARCH] index overflow during tail merge - append-only until next compaction");
            }
            *after_out = kept;
        } else {
            _storage->remove(tmp.c_str());
            arch_idx_reset();      // offsets are unreliable now
            rc = -4;
        }
        s_arch_compacting = false;
        archive_generation++;      // Map union cache + pager windows are stale

        if (is_sd) sd_spi_release();
        if (!is_sd) usb_flash_guard_end();
        MESH_UNLOCK();
    } else {
        // Failed mid-pass: original untouched, drop the .tmp, stay append-only
        // (the safe state) until the next successful compaction or reboot.
        {
            UsbFlashGuardIf _gf(!is_sd);   // the remove writes flash metadata;
                                           // scoped: must end before MESH_LOCK
            if (is_sd) sd_spi_take();
            if (_storage->exists(tmp.c_str())) _storage->remove(tmp.c_str());
            if (is_sd) sd_spi_release();
        }
        MESH_LOCK();
        arch_idx_reset();
        s_arch_built = false;
        s_arch_compacting = false;
        MESH_UNLOCK();
    }

    if (live) heap_caps_free(live);
    SLog.printf("[ARCH] compact rc=%d: %u -> %u records\n",
                rc, (unsigned)*before_out, (unsigned)*after_out);
    return rc;
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

                // Marker line (no tabs): Public was explicitly deleted by the user.
                if (strcmp(line, "pubdel") == 0) { _public_deleted = true; continue; }

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
    UsbFlashGuardIf _g(!is_sd);   // LittleFS backend: internal-flash write
    if (is_sd) sd_spi_take();

    String path = storagePath(_storage_prefix, "/channels");
    File file = _storage->open(path.c_str(), "w", true);
    if (file)
    {
        // Persist the Public deletion so boot doesn't recreate it.
        if (_public_deleted) file.print("pubdel\n");
        for (int i = 1; i < MAX_GROUP_CHANNELS; i++) {
            ChannelDetails cd;
            getChannel(i, cd);
            if (cd.name[0] == '\0') continue;
            if (strcmp(cd.name, "Public") == 0) continue;  // Public is owned by boot + the pubdel marker, never persisted as a slot

            char secret_hex[65];
            mesh::Utils::toHex(secret_hex, cd.channel.secret, 32);
            file.printf("%d\t%s\t%s\n", i, cd.name, secret_hex);
        }
        file.close();
    }

    if (is_sd) sd_spi_release();
}

// ── Per-channel notification modes ──────────────────────────────────────────
// Stored by channel NAME in /channel_notify ("name \t mode" lines). Only
// non-default modes are written; a missing entry means NOTIFY_CHAN_MENTION
// (the pre-existing behavior). See the member comment in punkmesh.h.

void PunkMesh::loadChannelNotify()
{
    _chan_notify_count = 0;
    bool is_sd = (_storage != &LittleFS);
    if (is_sd) sd_spi_take();

    String path = storagePath(_storage_prefix, "/channel_notify");
    if (_storage->exists(path.c_str()))
    {
        File file = _storage->open(path.c_str());
        if (file)
        {
            char line[64];
            while (file.available() && _chan_notify_count < MAX_CHANNEL_NOTIFY_PREFS)
            {
                int len = 0;
                while (file.available() && len < (int)sizeof(line) - 1) {
                    char ch = file.read();
                    if (ch == '\n' || ch == '\r') break;
                    line[len++] = ch;
                }
                line[len] = '\0';
                if (len == 0) continue;

                // Format: name \t mode
                char* tab = strchr(line, '\t');
                if (!tab) continue;
                *tab = '\0';
                int mode = atoi(tab + 1);
                if (line[0] == '\0' || mode < 0 || mode > NOTIFY_CHAN_ALL) continue;
                if (mode == NOTIFY_CHAN_MENTION) continue;  // default needs no entry

                ChannelNotifyPref& p = _chan_notify[_chan_notify_count++];
                memset(&p, 0, sizeof(p));
                strncpy(p.name, line, sizeof(p.name) - 1);
                p.mode = (uint8_t)mode;
            }
            file.close();
        }
    }

    if (is_sd) sd_spi_release();
}

void PunkMesh::saveChannelNotify()
{
    bool is_sd = (_storage != &LittleFS);
    UsbFlashGuardIf _g(!is_sd);   // LittleFS backend: internal-flash write
    if (is_sd) sd_spi_take();

    String path = storagePath(_storage_prefix, "/channel_notify");
    File file = _storage->open(path.c_str(), "w", true);
    if (file)
    {
        for (int i = 0; i < _chan_notify_count; i++)
            file.printf("%s\t%d\n", _chan_notify[i].name, _chan_notify[i].mode);
        file.close();
    }

    if (is_sd) sd_spi_release();
}

uint8_t PunkMesh::getChannelNotifyMode(const char* name)
{
    if (!name || !name[0]) return NOTIFY_CHAN_MENTION;
    for (int i = 0; i < _chan_notify_count; i++) {
        if (strcmp(_chan_notify[i].name, name) == 0) return _chan_notify[i].mode;
    }
    return NOTIFY_CHAN_MENTION;
}

void PunkMesh::setChannelNotifyMode(const char* name, uint8_t mode)
{
    if (!name || !name[0]) return;
    if (mode > NOTIFY_CHAN_ALL) mode = NOTIFY_CHAN_MENTION;

    int found = -1;
    for (int i = 0; i < _chan_notify_count; i++) {
        if (strcmp(_chan_notify[i].name, name) == 0) { found = i; break; }
    }

    if (mode == NOTIFY_CHAN_MENTION) {
        // Default mode = no entry; drop an existing one.
        if (found >= 0) {
            _chan_notify[found] = _chan_notify[_chan_notify_count - 1];
            _chan_notify_count--;
            saveChannelNotify();
        }
        return;
    }

    if (found >= 0) {
        if (_chan_notify[found].mode == mode) return;   // no change, skip the write
    } else {
        if (_chan_notify_count >= MAX_CHANNEL_NOTIFY_PREFS) {
            SLog.println("[NOTIFY] channel-notify table full, pref not saved");
            return;
        }
        found = _chan_notify_count++;
        memset(&_chan_notify[found], 0, sizeof(_chan_notify[found]));
        strncpy(_chan_notify[found].name, name, sizeof(_chan_notify[found].name) - 1);
    }
    _chan_notify[found].mode = mode;
    saveChannelNotify();
}

// ── Per-channel region / flood scope ─────────────────────────────────────────
// Stored by channel NAME in /channel_regions ("name \t region" lines). Only
// channels with an override are written; a missing entry means the channel
// inherits the device-global default scope. See the member comment in punkmesh.h.

void PunkMesh::loadChannelScopes()
{
    _chan_scope_count = 0;
    bool is_sd = (_storage != &LittleFS);
    if (is_sd) sd_spi_take();

    String path = storagePath(_storage_prefix, "/channel_regions");
    if (_storage->exists(path.c_str()))
    {
        File file = _storage->open(path.c_str());
        if (file)
        {
            char line[80];
            while (file.available() && _chan_scope_count < MAX_CHANNEL_SCOPE_PREFS)
            {
                int len = 0;
                while (file.available() && len < (int)sizeof(line) - 1) {
                    char ch = file.read();
                    if (ch == '\n' || ch == '\r') break;
                    line[len++] = ch;
                }
                line[len] = '\0';
                if (len == 0) continue;

                // Format: name \t region
                char* tab = strchr(line, '\t');
                if (!tab) continue;
                *tab = '\0';
                if (line[0] == '\0' || tab[1] == '\0') continue;

                ChannelScopePref& p = _chan_scopes[_chan_scope_count++];
                memset(&p, 0, sizeof(p));
                strncpy(p.name, line, sizeof(p.name) - 1);
                strncpy(p.scope, tab + 1, sizeof(p.scope) - 1);
            }
            file.close();
        }
    }

    if (is_sd) sd_spi_release();
}

void PunkMesh::saveChannelScopes()
{
    bool is_sd = (_storage != &LittleFS);
    UsbFlashGuardIf _g(!is_sd);   // LittleFS backend: internal-flash write
    if (is_sd) sd_spi_take();

    String path = storagePath(_storage_prefix, "/channel_regions");
    File file = _storage->open(path.c_str(), "w", true);
    if (file)
    {
        for (int i = 0; i < _chan_scope_count; i++)
            file.printf("%s\t%s\n", _chan_scopes[i].name, _chan_scopes[i].scope);
        file.close();
    }

    if (is_sd) sd_spi_release();
}

const char* PunkMesh::getChannelScope(const char* chan_name)
{
    if (!chan_name || !chan_name[0]) return "";
    for (int i = 0; i < _chan_scope_count; i++) {
        if (strcmp(_chan_scopes[i].name, chan_name) == 0) return _chan_scopes[i].scope;
    }
    return "";
}

void PunkMesh::setChannelScope(const char* chan_name, const char* region)
{
    if (!chan_name || !chan_name[0]) return;

    int found = -1;
    for (int i = 0; i < _chan_scope_count; i++) {
        if (strcmp(_chan_scopes[i].name, chan_name) == 0) { found = i; break; }
    }

    if (!region || region[0] == '\0') {
        // Inherit global = no entry; drop an existing one.
        if (found >= 0) {
            _chan_scopes[found] = _chan_scopes[_chan_scope_count - 1];
            _chan_scope_count--;
            saveChannelScopes();
        }
        return;
    }

    if (found >= 0) {
        if (strcmp(_chan_scopes[found].scope, region) == 0) return;   // no change, skip the write
    } else {
        if (_chan_scope_count >= MAX_CHANNEL_SCOPE_PREFS) {
            SLog.println("[MESH] channel-region table full, pref not saved");
            return;
        }
        found = _chan_scope_count++;
        memset(&_chan_scopes[found], 0, sizeof(_chan_scopes[found]));
        strncpy(_chan_scopes[found].name, chan_name, sizeof(_chan_scopes[found].name) - 1);
    }
    memset(_chan_scopes[found].scope, 0, sizeof(_chan_scopes[found].scope));
    strncpy(_chan_scopes[found].scope, region, sizeof(_chan_scopes[found].scope) - 1);
    saveChannelScopes();
}

// Slot of the channel named "Public", or -1 if there isn't one. Public is treated
// as a normal channel (no cached pointer), so callers resolve it by name on demand.
int PunkMesh::publicChannelIdx()
{
    for (int i = 0; i < MAX_GROUP_CHANNELS; i++) {
        ChannelDetails cd;
        if (getChannel(i, cd) && strcmp(cd.name, "Public") == 0) return i;
    }
    return -1;
}

// Delete the Public channel and persist the deletion so boot won't recreate it.
void PunkMesh::deletePublic()
{
    // Find the Public channel by NAME (its slot is incidental) and clear it.
    for (int i = 0; i < MAX_GROUP_CHANNELS; i++) {
        ChannelDetails cd;
        if (getChannel(i, cd) && strcmp(cd.name, "Public") == 0) {
            ChannelDetails empty;
            memset(&empty, 0, sizeof(empty));
            setChannel(i, empty);
        }
    }
    _public_deleted = true;
    saveChannels();
}

// Re-add Public with the well-known PSK and clear the persisted deletion.
void PunkMesh::restorePublic()
{
    // Place Public in the first free slot via setChannel — NOT addChannel, whose
    // num_channels high-water index can overwrite a user channel after a restore.
    // The slot is incidental; callers resolve Public by name (publicChannelIdx).
    ChannelDetails cd;
    memset(&cd, 0, sizeof(cd));
    strncpy(cd.name, "Public", sizeof(cd.name) - 1);
    extern unsigned int decode_base64(unsigned char const *src, unsigned int slen, unsigned char *target);
    decode_base64((unsigned char *)PUBLIC_GROUP_PSK, strlen(PUBLIC_GROUP_PSK), cd.channel.secret);
    for (int i = 0; i < MAX_GROUP_CHANNELS; i++) {
        ChannelDetails ex;
        if (getChannel(i, ex) && ex.name[0] == '\0') {
            setChannel(i, cd);   // computes the channel hash from the secret
            break;
        }
    }
    _public_deleted = false;
    saveChannels();
}

// ══════════════════════════════════════════════════════════════════
// Persistent message history
// ══════════════════════════════════════════════════════════════════


// Resolve channel name for a given index. Falls back to "ch<idx>" if unnamed.
static String channel_name_for_idx(PunkMesh& mesh, int ch_idx) {
    ChannelDetails cd;
    if (mesh.getChannel(ch_idx, cd) && cd.name[0] != '\0')
        return String(cd.name);
    return String("ch") + String(ch_idx);
}

// Public wrappers so BLE companion can build paths for targeted sync.
String PunkMesh::channelMsgPath(int channel_idx) {
    String name = channel_name_for_idx(*this, channel_idx);
    // MC-PKG divergence: String-returning mstore calls go through the C
    // bridges (the shim String is not the firmware's).
    char b[128];
    return String(mcs_channel_msg_path(name.c_str(), b, sizeof(b)) > 0 ? b : "");
}

String PunkMesh::dmMsgPath(const char* peer) {
    char b[128];   // MC-PKG divergence (see channelMsgPath)
    return String(mcs_dm_msg_path(peer, b, sizeof(b)) > 0 ? b : "");
}



// ── Unread + append wrappers ────────────────────────────────────
// The store is name-keyed (mstore); this is the idx->name MeshCore surface.
// Name resolution takes MESH_LOCK itself (recursive — RX-path callers already
// hold it); the store calls need none (unread contract) or self-lock (append).
void PunkMesh::unreadBumpChannel(int channel_idx) {
    if (channel_idx < 0 || channel_idx >= MAX_GROUP_CHANNELS) return;
    MESH_LOCK();
    String n = channel_name_for_idx(*this, channel_idx);
    mstore::unread_bump_channel(n.c_str());
    MESH_UNLOCK();
}
void PunkMesh::unreadBumpDM(const char* name) { mstore::unread_bump_dm(name); }
void PunkMesh::unreadClearChannel(int channel_idx) {
    if (channel_idx < 0 || channel_idx >= MAX_GROUP_CHANNELS) return;
    MESH_LOCK();
    String n = channel_name_for_idx(*this, channel_idx);
    mstore::unread_clear_channel(n.c_str());
    MESH_UNLOCK();
}
void PunkMesh::unreadClearDM(const char* name) { mstore::unread_clear_dm(name); }
uint16_t PunkMesh::unreadChannel(int channel_idx) {
    if (channel_idx < 0 || channel_idx >= MAX_GROUP_CHANNELS) return 0;
    MESH_LOCK();
    String n = channel_name_for_idx(*this, channel_idx);
    uint16_t c = mstore::unread_channel(n.c_str());
    MESH_UNLOCK();
    return c;
}
uint16_t PunkMesh::unreadDM(const char* name) { return mstore::unread_dm(name); }
uint32_t PunkMesh::unreadTotal() { return mstore::unread_total(); }

void PunkMesh::appendChannelMessage(int channel_idx, const char* from, const char* text,
                                    uint32_t timestamp, float snr, float rssi,
                                    uint8_t hops, bool direct,
                                    uint16_t path_len, const uint8_t* path,
                                    const uint8_t* pkt_hash, uint32_t sender_ts) {
    if (channel_idx < 0) return;
    MESH_LOCK();
    String ch_name = channel_name_for_idx(*this, channel_idx);
    MESH_UNLOCK();
    mstore::append_channel_message(ch_name.c_str(), channel_idx, from, text,
                                   timestamp, snr, rssi, hops, direct,
                                   path_len, path, pkt_hash, sender_ts);
}


int PunkMesh::pushRoutingQuery(lua_State* L, const char* sender,
                               uint32_t since_ts, uint32_t until_ts) {
    return mstore::push_routing_query(L, sender, since_ts, until_ts);
}

int PunkMesh::pushRoutingSenders(lua_State* L, const char* query, int max) {
    return mstore::push_routing_senders(L, query, max);
}


void PunkMesh::appendDMMessage(const char* peer, const char* from, const char* text,
                               uint32_t timestamp, float snr, float rssi,
                               uint8_t hops, bool direct,
                               uint16_t path_len, const uint8_t* path,
                               const uint8_t* pkt_hash,
                               const uint8_t* sender_pub_key, uint32_t sender_ts) {
    mstore::append_dm_message(peer, from, text, timestamp, snr, rssi, hops, direct,
                              path_len, path, pkt_hash, sender_pub_key, sender_ts);
}

// Takes MESH_LOCK internally ONLY for the channel-name snapshot — call WITHOUT
// the lock held (full contract at mstore::push_channel_messages).
int PunkMesh::pushChannelMessagesToLua(lua_State* L, int channel_idx, int max_records) {
    if (channel_idx < 0) { lua_newtable(L); return 1; }
    MESH_LOCK();
    String ch_name = channel_name_for_idx(*this, channel_idx);
    MESH_UNLOCK();
    return mstore::push_channel_messages(L, ch_name.c_str(), max_records);
}

int PunkMesh::pushDMMessagesToLua(lua_State* L, const char* peer, int max_records) {
    return mstore::push_dm_messages(L, peer, max_records);
}
int PunkMesh::pushDMThreadNamesToLua(lua_State* L) {
    return mstore::push_dm_thread_names(L);
}


// Messenger inbox summaries: snapshot the channel table under MESH_LOCK, then
// hand the list to the store (file reads + Lua pushes run unlocked there).
int PunkMesh::pushMsgSummariesToLua(lua_State* L) {
    MStoreChanRef chans[MAX_GROUP_CHANNELS];
    int nch = 0;
    MESH_LOCK();
    for (int i = 0; i < MAX_GROUP_CHANNELS; i++) {
        ChannelDetails cd;
        if (getChannel(i, cd) && cd.name[0] != '\0') {
            chans[nch].idx = i;
            strncpy(chans[nch].name, cd.name, sizeof(chans[nch].name) - 1);
            chans[nch].name[sizeof(chans[nch].name) - 1] = '\0';
            nch++;
        }
    }
    MESH_UNLOCK();
    return mstore::push_msg_summaries(L, chans, nch);
}
String PunkMesh::messagesDirPath() {
    char b[128];   // MC-PKG divergence (see channelMsgPath)
    return String(mcs_messages_dir_path(b, sizeof(b)) > 0 ? b : "");
}

int PunkMesh::enumerateMessageFiles(MsgFileInfo* out, int max_paths) {
    return mstore::enumerate_message_files(out, max_paths);
}

int PunkMesh::readOneStoredMsg(fs::FS* storage, const char* path,
                               size_t offset, StoredMsg& m) {
    (void)storage;   // MC-PKG divergence: the bridge reads the HOST's backend
    return mcs_read_one_stored_msg(path, offset, &m);
}

int PunkMesh::readAllStoredMsgs(const char* path, StoredMsg* out, int max_count) {
    return mstore::read_all_stored_msgs(path, out, max_count);
}

int PunkMesh::readStoredMsgsFrom(const char* path, uint32_t start_offset,
                                 StoredMsg* out, uint32_t* end_offsets, int max_count,
                                 uint32_t* next_offset, uint32_t* file_size) {
    return mstore::read_stored_msgs_from(path, start_offset, out, end_offsets,
                                         max_count, next_offset, file_size);
}
// ── Newest-N locator (BLE companion sync) ───────────────────────────────────

uint32_t PunkMesh::offsetOfNewestRecords(const char* path, uint32_t start_offset, int n) {
    return mstore::offset_of_newest_records(path, start_offset, n);
}
// Lock contract mirrors pushChannelMessagesToLua: MESH_LOCK only for the
// channel-name snapshot; the store does the file read + Lua pushes unlocked.
int PunkMesh::pushChatPageChannel(lua_State* L, int channel_idx, int mode,
                                  uint32_t cursor, int count) {
    if (channel_idx < 0) return mstore::push_chat_page_channel(L, nullptr, mode, cursor, count);
    MESH_LOCK();
    String ch_name = channel_name_for_idx(*this, channel_idx);
    MESH_UNLOCK();
    return mstore::push_chat_page_channel(L, ch_name.c_str(), mode, cursor, count);
}

int PunkMesh::pushChatPageDM(lua_State* L, const char* peer, int mode,
                             uint32_t cursor, int count) {
    return mstore::push_chat_page_dm(L, peer, mode, cursor, count);
}
int PunkMesh::lookupPersistedPaths(lua_State* L, const char* hash_hex,
                                    int channel_idx, const char* peer) {
    if (channel_idx >= 0) {
        MESH_LOCK();
        String ch_name = channel_name_for_idx(*this, channel_idx);
        MESH_UNLOCK();
        return mstore::lookup_persisted_paths(L, hash_hex, ch_name.c_str(), nullptr);
    }
    return mstore::lookup_persisted_paths(L, hash_hex, nullptr, peer);
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
    return _prefs.client_repeat != 0;
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

// Arm the retry ladder for a device-UI send. Called from the Lua send
// binding right after a successful sendAndPersistDM — this is also what
// restores expected_ack_crc/last_msg_sent for UI sends (the CLI 'send'
// path was the only thing setting them since the sendAndPersistDM refactor,
// which had silently killed delivered/failed feedback and path stats).
// Tracked sends don't arm the base class's txt_send_timeout — the deadline
// here drives the ladder from loop() (pendingSendLadderStep).
void PunkMesh::armPendingSend(const ContactInfo& recipient, uint32_t orig_ack,
                              uint32_t timestamp, const char* text, bool sent_direct,
                              uint32_t est_timeout_ms)
{
    if (_pending_send.active) failPendingSend();   // abandoned send = failed

    memcpy(_pending_send.recipient_pub, recipient.id.pub_key, PUB_KEY_SIZE);
    _pending_send.orig_ack    = orig_ack;
    _pending_send.timestamp   = timestamp;
    strncpy(_pending_send.text, text, sizeof(_pending_send.text) - 1);
    _pending_send.text[sizeof(_pending_send.text) - 1] = '\0';
    _pending_send.attempt     = 0;
    _pending_send.direct_left = sent_direct ? 2 : 0;   // original was try 1 of 3
    _pending_send.flood_left  = 2;
    _pending_send.total_attempts = 1 + _pending_send.direct_left + _pending_send.flood_left;
    _pending_send.acks[0]     = orig_ack;
    _pending_send.ack_count   = 1;
    _pending_send.deadline    = futureMillis(est_timeout_ms);
    _pending_send.active      = true;

    expected_ack_crc = orig_ack;
    last_msg_sent    = _ms->getMillis();
    curr_recipient   = lookupContactByPubKey(recipient.id.pub_key, PUB_KEY_SIZE);
}

// Declare the tracked send failed: tell the UI (keyed by the ORIGINAL ack,
// the one messages.lua indexed) and clear the slot.
void PunkMesh::failPendingSend()
{
    if (!_pending_send.active) return;
    _pending_send.active = false;
    expected_ack_crc = 0;
    if (rx_event_queue) {
        RxEvent ev;
        memset(&ev, 0, sizeof(ev));
        ev.kind = RxEvent::ACK;
        ev.ack  = _pending_send.orig_ack;
        ev.rtt  = -1;
        xQueueSend(rx_event_queue, &ev, 0);
    }
}

ContactInfo* PunkMesh::processAck(const uint8_t *data)
{
#if BLE_COMPANION_ENABLED
    // BLE-companion sends track their own expected-ack table; let it check
    // every ack (it self-filters and computes rtt from its own send times).
    // Previously this only ran inside the single-slot match below, which UI
    // sends never armed — phone delivery confirmations were dead too.
    if (ble_companion) {
        uint32_t raw_ack;
        memcpy(&raw_ack, data, 4);
        ble_companion->pushSendConfirmed(raw_ack);
    }
#endif

    // Match the tracked send: with the retry ladder active, an ack for ANY
    // attempt counts — repeat-until-heard (level 1) can deliver an older
    // attempt long after the ladder (level 2) moved on, and that late
    // success must stop the ladder instead of burning attempts / resetting
    // a path that just proved itself.
    bool matched = false;
    if (_pending_send.active) {
        for (int i = 0; i < _pending_send.ack_count && !matched; i++) {
            if (memcmp(data, &_pending_send.acks[i], 4) == 0) matched = true;
        }
    } else if (expected_ack_crc != 0 && memcmp(data, &expected_ack_crc, 4) == 0) {
        matched = true;   // untracked send (serial-CLI path)
    }

    if (matched)
    {
        uint32_t rtt = _ms->getMillis() - last_msg_sent;
        uint32_t ui_ack = expected_ack_crc;
        SLog.printf("   Got ACK! (round trip: %d millis)\n", rtt);
        expected_ack_crc = 0;

        if (_pending_send.active) {
            // Whatever attempt landed, the UI indexed the original ack.
            ui_ack = _pending_send.orig_ack;
            _pending_send.active = false;
            curr_recipient = lookupContactByPubKey(_pending_send.recipient_pub, PUB_KEY_SIZE);
        }
        if (curr_recipient) {
            recordPathSuccess(curr_recipient->id.pub_key, rtt);
        }

        // Notify the Lua UI so it can mark the sent DM delivered.
        if (rx_event_queue) {
            RxEvent ev;
            memset(&ev, 0, sizeof(ev));
            ev.kind = RxEvent::ACK;
            ev.ack  = ui_ack;
            ev.rtt  = (int32_t)rtt;
            xQueueSend(rx_event_queue, &ev, 0);
        }
        return curr_recipient;
    }
    // Not our pending DM ack — maybe a keep-alive ack from a logged-in
    // room/repeater (matches MyMesh.cpp:425 upstream behavior).
    return checkConnectionsAck(data);
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

    markConnectionActive(from);   // in case this is a logged-in server talking to us

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

    // C-side alert (melody + kbd blink) + notification record + unread bump.
    // All fire from this mesh-task context so DMs still notify, log, and count
    // while Lua is torn down for an ELF run. The chat view clears the counter
    // when the thread opens; the topbar drop-down shows the logged line.
    unreadBumpDM(from.name);
    {
        char nbuf[192];
        snprintf(nbuf, sizeof(nbuf), "From %s: %s", from.name, norm_text);
        notify_post(nbuf);
    }

#if BLE_COMPANION_ENABLED
    if (ble_companion) ble_companion->queueReceivedDM(from, pkt, sender_timestamp, text);
#endif
}

void PunkMesh::onCommandDataRecv(const ContactInfo &from, mesh::Packet *pkt, uint32_t sender_timestamp, const char *text)
{
    SLog.printf("[MESH RX] CLI reply from %s: \"%s\"\n", from.name, text);
    markConnectionActive(from);

    char norm_text[160];
    normalize_smart_quotes(text, norm_text, sizeof(norm_text));

    // Authoritative time = OUR clock at receipt; the sender's value is recorded only.
    uint32_t rx_ts = getRTCClock()->getCurrentTime();

    // Persist into the repeater's thread — its chat view IS the CLI console.
    // No unread bump / melody: replies arrive while the user drives the console.
    appendDMMessage(from.name, from.name, norm_text, rx_ts,
                    last_rx_snr, last_rx_rssi, pkt->getPathHashCount(),
                    pkt->isRouteDirect(), pkt->path_len, pkt->path,
                    _last_pkt_hash, from.id.pub_key, /*sender_ts=*/sender_timestamp);

    if (rx_event_queue) {
        RxEvent ev = {};
        ev.kind        = RxEvent::CLI_RESPONSE;
        ev.hops        = pkt->getPathHashCount();
        ev.channel_idx = -1;
        ev.direct      = pkt->isRouteDirect();
        strncpy(ev.sender, from.name, sizeof(ev.sender) - 1);
        strncpy(ev.text, norm_text, sizeof(ev.text) - 1);
        ev.timestamp = rx_ts;
        ev.snr       = last_rx_snr;
        ev.rssi      = last_rx_rssi;
        ev.path_len  = pkt->path_len;
        memcpy(ev.path, pkt->path, pkt->getPathByteLen());
        memcpy(ev.pkt_hash, _last_pkt_hash, MAX_HASH_SIZE);
        if (xQueueSend(rx_event_queue, &ev, 0) != pdTRUE) {
            SLog.println("[MESH RX] WARNING: rx_event_queue full, dropping CLI reply");
        }
    }

#if BLE_COMPANION_ENABLED
    if (ble_companion) ble_companion->queueCliResponse(from, pkt, sender_timestamp, text);
#endif
}

// ── Room post dedupe ─────────────────────────────────────────────
// A room server re-sends a post (attempt++) until the client's ACK gets
// through, and every attempt is a distinct packet hash — the mesh-layer
// packet dedupe can't catch it. Remember the last few (room, sender_ts,
// text) tuples and skip re-persisting. RAM-only: after a reboot the lazy
// sync-cursor save (markRoomSyncDirty) bounds any re-fetch overlap to the
// last ~minute before power-off.
struct RoomDedupeEntry { uint32_t room_prefix; uint32_t sender_ts; uint32_t text_hash; };
static RoomDedupeEntry s_room_dedupe[16];
static int s_room_dedupe_next = 0;

static uint32_t fnv1a32(const char* s) {
    uint32_t h = 2166136261u;
    while (*s) { h ^= (uint8_t)*s++; h *= 16777619u; }
    return h;
}

// True if this (room, post) was already seen; records it otherwise.
static bool room_dedupe_seen(const uint8_t* room_pub, uint32_t sender_ts, const char* text) {
    uint32_t prefix; memcpy(&prefix, room_pub, 4);
    uint32_t th = fnv1a32(text);
    for (int i = 0; i < 16; i++) {
        if (s_room_dedupe[i].room_prefix == prefix &&
            s_room_dedupe[i].sender_ts == sender_ts &&
            s_room_dedupe[i].text_hash == th) return true;
    }
    s_room_dedupe[s_room_dedupe_next].room_prefix = prefix;
    s_room_dedupe[s_room_dedupe_next].sender_ts   = sender_ts;
    s_room_dedupe[s_room_dedupe_next].text_hash   = th;
    s_room_dedupe_next = (s_room_dedupe_next + 1) % 16;
    return false;
}

// Room server posts (both the history sync after login and live pushes).
// `sender_prefix` is the 4-byte pubkey prefix of the ORIGINAL author; the
// post is persisted under the ROOM's thread with the author as `from`.
void PunkMesh::onSignedMessageRecv(const ContactInfo &from, mesh::Packet *pkt, uint32_t sender_timestamp, const uint8_t *sender_prefix, const char *text)
{
    SLog.println("[MESH RX] ========== ROOM MSG RECEIVED ==========");
    SLog.printf("[MESH RX] Room: %s, route: %s, hops: %d, sender_ts: %u\n",
        from.name, pkt->isRouteDirect() ? "DIRECT" : "FLOOD", pkt->getPathHashCount(), sender_timestamp);
    SLog.printf("[MESH RX] Text: \"%s\"\n", text);

    markConnectionActive(from);
    // BaseChatMesh already advanced from.sync_since before calling us —
    // schedule the cursor's lazy persist regardless of what we do below.
    markRoomSyncDirty();

#if BLE_COMPANION_ENABLED
    if (ble_companion) ble_companion->queueReceivedSigned(from, pkt, sender_timestamp, sender_prefix, text);
#endif

    // Our own posts echo back (live and on every history sync); they were
    // persisted at send time.
    if (memcmp(sender_prefix, self_id.pub_key, 4) == 0) {
        SLog.println("[MESH RX] Own room post echo — skipped");
        return;
    }

    if (room_dedupe_seen(from.id.pub_key, sender_timestamp, text)) {
        SLog.println("[MESH RX] Duplicate room post (server resend) — skipped");
        return;
    }

    // Resolve the author's display name from the 4-byte key prefix.
    char author[32];
    uint8_t author_key[6] = {0};
    ContactInfo* ac = lookupContactByPubKey(sender_prefix, 4);
    if (ac) {
        strncpy(author, ac->name, sizeof(author) - 1);
        author[sizeof(author) - 1] = '\0';
        memcpy(author_key, ac->id.pub_key, 6);
    } else {
        snprintf(author, sizeof(author), "%02x%02x%02x%02x",
                 sender_prefix[0], sender_prefix[1], sender_prefix[2], sender_prefix[3]);
        memcpy(author_key, sender_prefix, 4);
    }

    char norm_text[160];
    normalize_smart_quotes(text, norm_text, sizeof(norm_text));

    // Authoritative time = OUR clock at receipt; the room's post timestamp
    // (sender_timestamp) is recorded only.
    uint32_t rx_ts = getRTCClock()->getCurrentTime();

    // Persist under the ROOM's thread (peer = room, from = author).
    appendDMMessage(from.name, author, norm_text, rx_ts,
                    last_rx_snr, last_rx_rssi, pkt->getPathHashCount(),
                    pkt->isRouteDirect(), pkt->path_len, pkt->path,
                    _last_pkt_hash, author_key, /*sender_ts=*/sender_timestamp);

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

    // Hand off to the UI core (see onMessageRecv for the why).
    if (rx_event_queue) {
        RxEvent ev = {};
        ev.kind        = RxEvent::ROOM_MSG;
        ev.hops        = pkt->getPathHashCount();
        ev.channel_idx = -1;
        ev.direct      = pkt->isRouteDirect();
        strncpy(ev.sender, from.name, sizeof(ev.sender) - 1);   // thread key = room
        strncpy(ev.origin, author, sizeof(ev.origin) - 1);      // display author
        strncpy(ev.text, norm_text, sizeof(ev.text) - 1);
        ev.timestamp = rx_ts;
        ev.snr       = last_rx_snr;
        ev.rssi      = last_rx_rssi;
        ev.path_len  = pkt->path_len;
        memcpy(ev.path, pkt->path, pkt->getPathByteLen());
        memcpy(ev.pkt_hash, _last_pkt_hash, MAX_HASH_SIZE);
        if (xQueueSend(rx_event_queue, &ev, 0) != pdTRUE) {
            SLog.println("[MESH RX] WARNING: rx_event_queue full, dropping room msg");
        }
    }

    // C-side alert + notification record + unread bump, same rationale as DMs
    // (fires from the mesh task so rooms still notify/log/count while Lua is
    // torn down).
    unreadBumpDM(from.name);
    {
        char nbuf[192];
        snprintf(nbuf, sizeof(nbuf), "%s/%s: %s", from.name, author, norm_text);
        notify_post(nbuf);
    }
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

    // C-side alert, gated by the channel's notify mode. The mode is keyed by
    // NAME — the slot is only a transient handle to resolve the live name at
    // this instant. Unknown channels surface under Public in the UI, so they
    // follow Public's mode. Own echoes never alert.
    if (strcmp(sender_name, _prefs.node_name) != 0) {
        // Unread bump is NOT gated by the notify mode — a muted channel still
        // accrues unread (only the melody/blink is mode-gated below).
        unreadBumpChannel(channel_idx);
        const char* notify_name = "Public";
        ChannelDetails ncd;
        if (channel_idx >= 0 && getChannel(channel_idx, ncd) && ncd.name[0] != '\0')
            notify_name = ncd.name;
        uint8_t nmode = getChannelNotifyMode(notify_name);
        if (nmode == NOTIFY_CHAN_ALL ||
            (nmode == NOTIFY_CHAN_MENTION && contains_mention(norm_msg, _prefs.node_name))) {
            char nbuf[192];
            snprintf(nbuf, sizeof(nbuf), "#%s %s: %s", notify_name, sender_name, norm_msg);
            notify_post(nbuf);
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

// Server status response payload (REQ_TYPE_GET_STATUS, after the 4-byte tag).
// Matches the stats structs in MeshCore's simple_repeater / simple_room_server
// examples — this is their common prefix; the trailing fields differ per
// server type and are left undecoded.
struct ServerStatsCommon {
    uint16_t batt_milli_volts;
    uint16_t curr_tx_queue_len;
    int16_t  noise_floor;
    int16_t  last_rssi;
    uint32_t n_packets_recv;
    uint32_t n_packets_sent;
    uint32_t total_air_time_secs;
    uint32_t total_up_time_secs;
    uint32_t n_sent_flood, n_sent_direct;
    uint32_t n_recv_flood, n_recv_direct;
    uint16_t err_events;
    int16_t  last_snr;   // x 4
    uint16_t n_direct_dups, n_flood_dups;
};

void PunkMesh::onContactResponse(const ContactInfo &contact, const uint8_t *data, uint8_t len)
{
    markConnectionActive(contact);

    // Device-UI pending login? (The BLE companion keeps its own independent
    // pending flags for phone-initiated logins — both paths coexist.)
    if (pending_login_prefix && memcmp(&pending_login_prefix, contact.id.pub_key, 4) == 0) {
        pending_login_prefix = 0;

        bool ok = false;
        uint8_t perms = 0;
        uint16_t keep_alive_secs = 0;
        if (len >= 6 && memcmp(&data[4], "OK", 2) == 0) {          // legacy repeater login
            ok = true;
        } else if (len >= 7 && data[4] == RESP_SERVER_LOGIN_OK) {  // current servers
            ok = true;
            keep_alive_secs = ((uint16_t)data[5]) * 16;
            perms = data[6];
            // The wrapper also registers the conn-watch (CONN_LOST on expiry).
            if (keep_alive_secs > 0) startConnectionToContact(contact, keep_alive_secs);
        }
        SLog.printf("[MESH RX] Login response from %s: %s (perms=%u keepalive=%us)\n",
                    contact.name, ok ? "OK" : "FAIL", perms, keep_alive_secs);

        if (rx_event_queue) {
            RxEvent ev = {};
            ev.kind        = RxEvent::LOGIN_RESULT;
            ev.channel_idx = ok ? 1 : 0;     // success flag
            ev.hops        = perms;          // permissions byte
            ev.ack         = keep_alive_secs;
            ev.timestamp   = getRTCClock()->getCurrentTime();
            strncpy(ev.sender, contact.name, sizeof(ev.sender) - 1);
            xQueueSend(rx_event_queue, &ev, 0);
        }
    } else if (pending_status_prefix && len > 4 &&
               memcmp(&pending_status_prefix, contact.id.pub_key, 4) == 0) {
        pending_status_prefix = 0;

        ServerStatsCommon st = {};
        size_t copy = len - 4;
        if (copy > sizeof(st)) copy = sizeof(st);
        memcpy(&st, &data[4], copy);

        uint32_t up = st.total_up_time_secs;
        if (rx_event_queue) {
            RxEvent ev = {};
            ev.kind      = RxEvent::STATUS_TEXT;
            ev.timestamp = getRTCClock()->getCurrentTime();
            strncpy(ev.sender, contact.name, sizeof(ev.sender) - 1);
            snprintf(ev.text, sizeof(ev.text),
                     "Batt %u.%02uV  Up %ud %uh %um\n"
                     "RX %u  TX %u  Err %u\n"
                     "Air TX %us\n"
                     "Noise %d  RSSI %d  SNR %.1f\n"
                     "Queue %u  Dups d%u f%u",
                     (unsigned)(st.batt_milli_volts / 1000), (unsigned)((st.batt_milli_volts % 1000) / 10),
                     (unsigned)(up / 86400), (unsigned)((up % 86400) / 3600), (unsigned)((up % 3600) / 60),
                     (unsigned)st.n_packets_recv, (unsigned)st.n_packets_sent, (unsigned)st.err_events,
                     (unsigned)st.total_air_time_secs,
                     (int)st.noise_floor, (int)st.last_rssi, st.last_snr / 4.0f,
                     (unsigned)st.curr_tx_queue_len, (unsigned)st.n_direct_dups, (unsigned)st.n_flood_dups);
            // Room servers append n_posted/n_post_push after the common
            // prefix (repeaters have rx-air/recv-errors there instead).
            // These are THE counters for "are posts entering the queue and
            // is the server attempting pushes" when sync misbehaves.
            if (contact.type == ADV_TYPE_ROOM && len >= 4 + (int)sizeof(st) + 4) {
                uint16_t n_posted, n_pushed;
                memcpy(&n_posted, &data[4 + sizeof(st)], 2);
                memcpy(&n_pushed, &data[4 + sizeof(st) + 2], 2);
                size_t l = strlen(ev.text);
                snprintf(ev.text + l, sizeof(ev.text) - l, "\nPosted %u  Pushed %u",
                         (unsigned)n_posted, (unsigned)n_pushed);
            }
            xQueueSend(rx_event_queue, &ev, 0);
        }
    }

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

// Retry ladder for device-UI sends: up to 3 tries via the stored path, then
// the path auto-resets and up to 2 more go out flooded; only after that is
// the send declared failed. Driven from loop() by _pending_send.deadline
// (tracked sends never arm the base class's txt_send_timeout), on the mesh
// task under MESH_LOCK, so resending here is safe.
void PunkMesh::pendingSendLadderStep()
{
    ContactInfo* c = lookupContactByPubKey(_pending_send.recipient_pub, PUB_KEY_SIZE);
    if (!c) {
        SLog.println("   ERROR: timed out and contact gone — send failed.");
        failPendingSend();
        return;
    }
    curr_recipient = c;
    recordPathFailure(c->id.pub_key);   // per-attempt, against the current out_path

    bool resend = false;
    if (_pending_send.direct_left > 0) {
        _pending_send.direct_left--;
        resend = true;
    } else if (_pending_send.flood_left > 0) {
        if (c->out_path_len != OUT_PATH_UNKNOWN) {
            // The auto flood fallback: direct budget exhausted, drop the
            // learned path (persisted) so this and all future sends flood
            // until a fresh path-return re-teaches a route.
            resetPathTo(*c);
            saveOneContact(*c);
            SLog.printf("   no ACK x3 — path to %s reset, falling back to FLOOD\n", c->name);
        }
        _pending_send.flood_left--;
        resend = true;
    }

    if (resend) {
        _pending_send.attempt++;
        uint32_t new_ack = 0;
        uint32_t est_timeout = 0;
        // Send-only resend (the message was persisted at the original send);
        // attempt++ makes the packet + ack hash unique.
        int rc = sendMessageTracked(*c, _pending_send.timestamp, _pending_send.attempt,
                                    _pending_send.text, new_ack, est_timeout);
        if (rc != MSG_SEND_FAILED) {
            expected_ack_crc = new_ack;
            last_msg_sent = _ms->getMillis();
            _pending_send.deadline = futureMillis(est_timeout);
            // Remember every attempt's ack: repeat-until-heard can land
            // an OLD attempt late, and its ack must still count.
            if (_pending_send.ack_count < 5) {
                _pending_send.acks[_pending_send.ack_count++] = new_ack;
            }
            SLog.printf("   no ACK — retry %u/%u via %s\n",
                        (unsigned)_pending_send.attempt + 1,
                        (unsigned)_pending_send.total_attempts,
                        rc == MSG_SEND_SENT_DIRECT ? "path" : "flood");
            if (rx_event_queue) {
                RxEvent ev;
                memset(&ev, 0, sizeof(ev));
                ev.kind        = RxEvent::SEND_RETRY;
                ev.ack         = _pending_send.orig_ack;
                ev.hops        = _pending_send.attempt + 1;      // 1-based try number
                ev.channel_idx = (int8_t)_pending_send.total_attempts;
                xQueueSend(rx_event_queue, &ev, 0);
            }
            return;
        }
        // couldn't compose/send (packet pool empty) — fall through to fail
    }

    SLog.println("   ERROR: retries exhausted, no ACK — send failed.");
    failPendingSend();
}

void PunkMesh::onSendTimeout()
{
    // Only untracked sends (serial-CLI 'send') arm the base timeout that
    // fires this; tracked sends run the ladder from loop() instead.
    SLog.println("   ERROR: timed out, no ACK.");
    if (curr_recipient) {
        recordPathFailure(curr_recipient->id.pub_key);
    }
    // Tell the Lua UI the send failed (only if there is still a pending ack —
    // a successful processAck clears expected_ack_crc, so this won't fire a
    // false failure after a delivery).
    if (rx_event_queue && expected_ack_crc != 0 && !_pending_send.active) {
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
            markPathHistDirty();
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
    markPathHistDirty();
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
            markPathHistDirty();
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
            markPathHistDirty();
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

    // MC-PKG divergence: paths cross the boundary as char* (mcs_ bridges).
    char fpath[128];
    fpath[0] = '\0';
    if (entry->is_dm) {
        if (entry->peer[0] == '\0') return;            // no peer — no DM log to attach to
        mcs_dm_msg_path(entry->peer, fpath, sizeof(fpath));
    } else {
        if (entry->channel_idx < 0) return;            // unknown channel — no log exists
        String ch_name = channel_name_for_idx(*this, entry->channel_idx);
        mcs_channel_msg_path(ch_name.c_str(), fpath, sizeof(fpath));
    }
    if (!fpath[0]) return;

    // O(1) append to the per-conversation .paths sidecar, joined back to this
    // message by hash on read. Replaces the old whole-file read+rewrite, which
    // malloc'd the entire (growing) message log per repeat heard — the source of
    // the PSRAM fragmentation that starved large allocations over uptime.
    mcs_append_extra_path(fpath, hash, &op);
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

void PunkMesh::sendFloodWithScope(mesh::Packet* pkt, uint32_t delay_millis,
                                  const uint8_t* key_override) {
    // Precedence: per-channel override > phone-set runtime scope > default.
    const uint8_t* key = key_override;
    if (!key) {
        key = _prefs.default_scope_key;
        for (size_t i = 0; i < sizeof(_ble_send_scope_key); i++) {
            if (_ble_send_scope_key[i]) { key = _ble_send_scope_key; break; }
        }
    }
    TransportKey scope;
    memcpy(scope.key, key, sizeof(scope.key));
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

    // Per-channel region override: resolve the GroupChannel back to its slot by
    // secret (the base class hands us only hash+secret) and look up a NAME-keyed
    // scope pref. Missing pref = inherit the device-global default scope.
    const uint8_t* key_override = nullptr;
    uint8_t chan_key[16];
    for (int i = 0; i < MAX_GROUP_CHANNELS; i++) {
        ChannelDetails cd;
        if (!getChannel(i, cd) || cd.name[0] == '\0') continue;
        if (memcmp(cd.channel.secret, channel.secret, sizeof(cd.channel.secret)) != 0) continue;

        const char* region = getChannelScope(cd.name);
        if (region[0]) {
            // Region key = SHA256(name) truncated to 16 bytes (same derivation
            // as setDefaultScope / MeshCore hashtag regions).
            SHA256 sha;
            sha.update((const uint8_t*)region, strlen(region));
            sha.finalize(chan_key, sizeof(chan_key));
            key_override = chan_key;
            SLog.printf("[MESH TX] channel '%s' scope '%s'\n", cd.name, region);
        }
        break;
    }

    // Base sendFloodScoped ignores the channel and floods unscoped with a
    // 1-byte path hash; route through sendFloodWithScope so the multi-byte path
    // size AND the transport scope (per-channel or default region) are applied.
    sendFloodWithScope(pkt, delay_millis, key_override);

    if (_prefs.msg_repeat_enabled) {
        registerPendingRepeat(_last_tx_hash, saved_header, saved_payload, saved_len);
    }
}

// ── Tracked sends ────────────────────────────────────────────────
// Local variants of BaseChatMesh::sendMessage / sendCommandData (see the
// punkmesh.h comment: MeshCore is a pristine submodule, and its versions
// compose the packet internally — the direct branch is invisible to us).
// Wire behavior is IDENTICAL; keep the composition in sync on upgrades.

// Every direct-routed tracked TXT send (DM, room post, CLI command) goes
// through here, so repeat-until-heard covers direct sends too. Zero-hop
// routes are excluded: no repeater exists to echo the packet, so "heard"
// could never confirm and the retransmits would be pure noise.
void PunkMesh::sendDirectTracked(const ContactInfo& recipient, mesh::Packet* pkt)
{
    pkt->calculatePacketHash(_last_tx_hash);

    bool has_repeater = recipient.out_path_len != OUT_PATH_UNKNOWN &&
                        (recipient.out_path_len & 63) > 0;
    bool want_repeat = _prefs.msg_repeat_enabled && has_repeater;

    uint8_t saved_header = pkt->header;
    uint8_t saved_payload[MAX_PACKET_PAYLOAD];
    uint16_t saved_len = pkt->payload_len;
    uint8_t saved_path[MAX_PATH_SIZE];
    uint8_t saved_path_len = recipient.out_path_len;
    if (want_repeat) {
        memcpy(saved_payload, pkt->payload, pkt->payload_len);
        memcpy(saved_path, recipient.out_path, MAX_PATH_SIZE);
    }

    sendDirect(pkt, recipient.out_path, recipient.out_path_len);

    if (want_repeat) {
        registerPendingRepeat(_last_tx_hash, saved_header, saved_payload, saved_len,
                              saved_path, saved_path_len);
    }
}

// Mirrors BaseChatMesh::composeMsgPacket (private upstream).
mesh::Packet* PunkMesh::composeTrackedMsgPacket(const ContactInfo& recipient, uint32_t timestamp,
                                                uint8_t attempt, const char* text,
                                                uint32_t& expected_ack)
{
    int text_len = strlen(text);
    if (text_len > MAX_TEXT_LEN) return NULL;
    if (attempt > 3 && text_len > MAX_TEXT_LEN - 2) return NULL;

    uint8_t temp[5 + MAX_TEXT_LEN + 1];
    memcpy(temp, &timestamp, 4);
    temp[4] = (attempt & 3);
    memcpy(&temp[5], text, text_len + 1);

    // calc expected ACK reply
    mesh::Utils::sha256((uint8_t *)&expected_ack, 4, temp, 5 + text_len, self_id.pub_key, PUB_KEY_SIZE);

    int len = 5 + text_len;
    if (attempt > 3) {
        temp[len++] = 0;        // null terminator
        temp[len++] = attempt;  // hide attempt number at tail end of payload
    }

    return createDatagram(PAYLOAD_TYPE_TXT_MSG, recipient.id, recipient.getSharedSecret(self_id), temp, len);
}

int PunkMesh::sendMessageTracked(const ContactInfo& recipient, uint32_t timestamp,
                                 uint8_t attempt, const char* text,
                                 uint32_t& expected_ack, uint32_t& est_timeout)
{
    mesh::Packet* pkt = composeTrackedMsgPacket(recipient, timestamp, attempt, text, expected_ack);
    if (pkt == NULL) return MSG_SEND_FAILED;

    uint32_t t = _radio->getEstAirtimeFor(pkt->getRawLength());
    if (recipient.out_path_len == OUT_PATH_UNKNOWN) {
        sendFloodScoped(recipient, pkt);    // computes _last_tx_hash + flood repeat
        est_timeout = calcFloodTimeoutMillisFor(t);
        return MSG_SEND_SENT_FLOOD;
    }
    sendDirectTracked(recipient, pkt);      // computes _last_tx_hash + direct repeat
    est_timeout = calcDirectTimeoutMillisFor(t, recipient.out_path_len);
    return MSG_SEND_SENT_DIRECT;
}

// Mirrors BaseChatMesh::sendCommandData. Bonus over the base version: no
// ack timeout is armed for CLI (replies carry no ack — the base one fired a
// spurious "timed out, no ACK" after every CLI command).
int PunkMesh::sendCommandTracked(const ContactInfo& recipient, uint32_t timestamp,
                                 uint8_t attempt, const char* text, uint32_t& est_timeout)
{
    int text_len = strlen(text);
    if (text_len > MAX_TEXT_LEN) return MSG_SEND_FAILED;

    uint8_t temp[5 + MAX_TEXT_LEN + 1];
    memcpy(temp, &timestamp, 4);
    temp[4] = (attempt & 3) | (TXT_TYPE_CLI_DATA << 2);
    memcpy(&temp[5], text, text_len + 1);

    auto pkt = createDatagram(PAYLOAD_TYPE_TXT_MSG, recipient.id, recipient.getSharedSecret(self_id), temp, 5 + text_len);
    if (pkt == NULL) return MSG_SEND_FAILED;

    uint32_t t = _radio->getEstAirtimeFor(pkt->getRawLength());
    if (recipient.out_path_len == OUT_PATH_UNKNOWN) {
        sendFloodScoped(recipient, pkt);
        est_timeout = calcFloodTimeoutMillisFor(t);
        return MSG_SEND_SENT_FLOOD;
    }
    sendDirectTracked(recipient, pkt);
    est_timeout = calcDirectTimeoutMillisFor(t, recipient.out_path_len);
    return MSG_SEND_SENT_DIRECT;
}

// ── Message repeat ───────────────────────────────────────────────

void PunkMesh::registerPendingRepeat(const uint8_t* hash, uint8_t header,
                                     const uint8_t* payload, uint16_t payload_len,
                                     const uint8_t* path, uint8_t path_len) {
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
    // Direct packets re-air on their saved route (the payload doesn't carry it).
    pr.direct = (path != nullptr);
    pr.path_len = path_len;
    memset(pr.path, 0, sizeof(pr.path));
    if (path) memcpy(pr.path, path, MAX_PATH_SIZE);
    pr.active = true;

    SLog.printf("[MSG REPEAT] registered (%s), %d retries, interval %ds\n",
                  pr.direct ? "direct" : "flood",
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
        if (pr.direct) {
            sendDirect(pkt, pr.path, pr.path_len);
        } else {
            sendFlood(pkt, (uint32_t)0, pathHashSize());
        }

        pr.attempts_remaining--;
        pr.next_retry_time = now + (unsigned long)_prefs.msg_repeat_interval_secs * 1000UL;

        SLog.printf("[MSG REPEAT] retransmit (%s), %d remaining\n",
                    pr.direct ? "direct" : "flood", pr.attempts_remaining);
    }
}

int PunkMesh::getRepeatStatus(const uint8_t* hash, int* remaining, int* total) {
    if (remaining) *remaining = 0;
    if (total)     *total     = 0;
    for (int i = 0; i < MAX_PENDING_REPEATS; i++) {
        if (_pending_repeats[i].active &&
            memcmp(_pending_repeats[i].pkt_hash, hash, MAX_HASH_SIZE) == 0) {
            if (remaining) *remaining = _pending_repeats[i].attempts_remaining;
            if (total)     *total     = _prefs.msg_repeat_max;
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
    strcpy(_prefs.node_name, "Meshpunk " MESHPUNK_BOARD_LABEL);
    _prefs.freq = LORA_FREQ;
    _prefs.tx_power_dbm = LORA_TX_POWER;
    _prefs.bandwidth = LORA_BW;
    _prefs.spreading_factor = LORA_SF;
    _prefs.coding_rate = LORA_CR;
    _prefs.ble_pin = BLE_PIN_CODE;
    _prefs.path_hash_mode = 0;
    _prefs.client_repeat = 0;   // never act as a repeater unless opted in
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
    // The PunkMesh object lives on the heap, so members are NOT zeroed:
    // without this, garbage `active` flags made loop() expire phantom
    // connections at boot, printing their uninitialized (unterminated)
    // name buffers as gibberish and queueing bogus CONN_LOST events.
    memset(_conn_watch, 0, sizeof(_conn_watch));

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
        // Key material comes from esp_random() via the unseeded StdRNG;
        // StdRNG::begin()/randomSeed() would switch ::random() to software rand().
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
                else if (strcmp(key, "client_repeat") == 0) _prefs.client_repeat = atoi(val);
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
    loadRoomSync();     // restore room sync cursors (sync_since isn't in CONTACT_REC)
    loadPathHistory();  // restore the Paths-picker history ring

    // Restore saved channels (slots 1-7) FIRST — this also sets _public_deleted
    // from the channels-file "pubdel" marker, so we know whether to recreate Public.
    loadChannels();
    loadChannelNotify();
    loadChannelScopes();

    if (!_public_deleted) {
        ChannelDetails* pub = addChannel("Public", PUBLIC_GROUP_PSK);
        if (pub) {
            SLog.println("[MESH INIT] Public channel created OK");
            char ch_hex[13];
            mesh::Utils::toHex(ch_hex, pub->channel.hash, 6);
            SLog.printf("[MESH INIT] Channel hash: %s\n", ch_hex);
        } else {
            SLog.println("[MESH INIT] ERROR: addChannel returned NULL!");
        }
    } else {
        SLog.println("[MESH INIT] Public channel previously deleted — not recreating");
    }

    // Presence in the channel table is authoritative: reconcile the flag so it can
    // never disagree with reality (e.g. a stale channels file that recorded both a
    // Public entry AND a pubdel marker). After this, "deleted" == "not in the table".
    _public_deleted = (publicChannelIdx() < 0);
}

// ── Packet capture hooks ─────────────────────────────────────────────
// The ring itself is rcap:: (src/radio/radio_capture.cpp), shared by every
// protocol. These four Dispatcher log hooks run under MESH_LOCK via
// mesh_task_body's wrapper around the protocol loop — the rcap contract.

void PunkMesh::logRxRaw(float snr, float rssi, const uint8_t raw[], int len) {
    PktCapture* e = rcap::push(PKT_CAP_DIR_RX);
    if (!e) return;
    if (len > MAX_TRANS_UNIT) len = MAX_TRANS_UNIT;
    e->snr_q4 = (int16_t)(snr * 4.0f);
    e->rssi   = (int16_t)rssi;
    e->len    = (uint8_t)len;
    memcpy(e->raw, raw, len);
}

void PunkMesh::logTx(mesh::Packet* pkt, int len) {
    PktCapture* e = rcap::push(PKT_CAP_DIR_TX);
    if (!e) return;
    e->parsed = 1;
    e->len    = pkt->writeTo(e->raw);
    pkt->calculatePacketHash(e->hash);
}

void PunkMesh::logTxFail(mesh::Packet* pkt, int len) {
    PktCapture* e = rcap::push(PKT_CAP_DIR_TX_FAIL);
    if (!e) return;
    e->parsed = 1;
    e->len    = pkt->writeTo(e->raw);
    pkt->calculatePacketHash(e->hash);
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

    // Complete this frame's capture entry. checkRecv() calls logRxRaw() then
    // logRx() for the SAME frame inside one MESH_LOCK section, so the newest
    // ring entry is this packet's — an unparsed RX entry it left behind.
    // Frames that never reach here stay parsed=0, which is how the monitor
    // shows rejected/dropped ones.
    PktCapture* cap = rcap::newest();
    if (cap && cap->dir == PKT_CAP_DIR_RX && !cap->parsed) {
        cap->parsed    = 1;
        cap->score_q10 = (int16_t)(score * 1000);
        memcpy(cap->hash, _last_pkt_hash, MAX_HASH_SIZE);
    }

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
        file.printf("client_repeat=%d\n", p.client_repeat);
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

    if (is_sd) {
        sd_spi_take();
        String path = storagePath(_storage_prefix, "/node_prefs");
        writePrefsToFile(_storage, path.c_str(), _prefs);   // SD (SPI, no cache stall)
        sd_spi_release();
        // The LittleFS fallback copy is an internal-flash write — guard it so a
        // mesh/BLE event firing this mid-USB-stream can't crash the host stack.
        UsbFlashGuard _g;
        writePrefsToFile(&LittleFS, "/node_prefs", _prefs);
    } else {
        UsbFlashGuard _g;   // _storage IS LittleFS — the write is internal flash
        String path = storagePath(_storage_prefix, "/node_prefs");
        writePrefsToFile(_storage, path.c_str(), _prefs);
    }
}

bool PunkMesh::saveIdentity() {
    String idPath = storagePath(_storage_prefix, "/identity");
    bool is_sd = (_storage != &LittleFS);
    bool ok = false;
    if (is_sd) {
        sd_spi_take();
        File file = _storage->open(idPath.c_str(), "w", true);   // SD (SPI)
        if (file) { ok = self_id.writeTo(file); file.close(); }
        sd_spi_release();
        // LittleFS fallback copy = internal flash — guard against USB crash.
        UsbFlashGuard _g;
        File lfs_file = LittleFS.open("/identity", "w", true);
        if (lfs_file) { self_id.writeTo(lfs_file); lfs_file.close(); }
    } else {
        UsbFlashGuard _g;   // _storage IS LittleFS — internal-flash write
        File file = _storage->open(idPath.c_str(), "w", true);
        if (file) { ok = self_id.writeTo(file); file.close(); }
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

    r.code = sendMessageTracked(recipient, timestamp, attempt, text,
                                r.expected_ack, r.est_timeout);
    if (r.code == MSG_SEND_FAILED) return r;

    // Both routes set _last_tx_hash now (sendFloodScoped / sendDirectTracked
    // inside sendMessageTracked), so direct sends get echo tracking + the
    // chat's "repeating..." indicator — repeat-until-heard covers all messages.
    memcpy(r.tx_hash, _last_tx_hash, MAX_HASH_SIZE);
    r.has_hash = true;

    appendDMMessage(recipient.name, _prefs.node_name, text,
                   timestamp, 0.0f, 0.0f, 0,
                   r.code == MSG_SEND_SENT_DIRECT,
                   0, nullptr, r.tx_hash);
    preRegisterSentHash(r.tx_hash, true, -1, recipient.name);
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

        int pub_idx = publicChannelIdx();
        ChannelDetails pub_cd;
        if (pub_idx < 0 || !getChannel(pub_idx, pub_cd)) {
            SLog.println("[MESH TX] ERROR: no public channel");
        } else {
            uint32_t timestamp = getRTCClock()->getCurrentTime();
            SLog.printf("[MESH TX] timestamp=%u, sender=%s\n", timestamp, _prefs.node_name);

            bool ok = sendGroupMessage(timestamp, pub_cd.channel, _prefs.node_name, &command[7], strlen(&command[7]));
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
        SLog.printf("  Public channel: %s\n", publicChannelIdx() >= 0 ? "configured" : "deleted");
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

    // Retry-ladder ack timeout for the tracked send (tracked sends don't arm
    // the base class's private txt_send_timeout — this deadline replaces it).
    if (_pending_send.active && millisHasNowPassed(_pending_send.deadline)) {
        pendingSendLadderStep();
    }

    // Keep-alive pings for logged-in rooms/repeaters (self-rate-limited via
    // each connection's next_ping; the table is empty unless a login succeeded
    // with a keep-alive interval).
    checkConnections();

    // Watched sessions whose connection slot vanished expired (manual logout
    // unwatches first) — tell the UI. Path is deliberately NOT reset here.
    for (int i = 0; i < 16; i++) {
        if (_conn_watch[i].active && !hasConnectionTo(_conn_watch[i].pub_key)) {
            _conn_watch[i].active = false;
            SLog.printf("[MESH] Connection to %s lost (keep-alive expired)\n", _conn_watch[i].name);
            if (rx_event_queue) {
                RxEvent ev = {};
                ev.kind = RxEvent::CONN_LOST;
                strncpy(ev.sender, _conn_watch[i].name, sizeof(ev.sender) - 1);
                xQueueSend(rx_event_queue, &ev, 0);
            }
        }
    }

    // Lazy room sync-cursor persistence (see markRoomSyncDirty).
    if (_room_sync_dirty && millisHasNowPassed(_room_sync_save_at)) {
        _room_sync_dirty = false;
        _room_sync_save_at = futureMillis(60000);   // floor for the next write
        saveRoomSync();
    }

    // Lazy path-history persistence (see markPathHistDirty).
    if (_path_hist_dirty && millisHasNowPassed(_path_hist_save_at)) {
        _path_hist_dirty = false;
        _path_hist_save_at = futureMillis(600000);  // ≥10 min between writes
        savePathHistory();
    }

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
