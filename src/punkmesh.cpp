#include "punkmesh.h"
#include <LittleFS.h>
#include "meshpunk_sync.h"

// Shared-SPI-bus lock pair (defined in main.cpp).
// sd_spi_take()    — acquire spi_bus_mutex before any SD operation.
// sd_spi_release() — release spi_bus_mutex after the SD file handle is closed.
// sd_spi_take() is inline in meshpunk_sync.h (just SPI_LOCK); no extern decl needed.
extern void sd_spi_release();
extern PunkMesh the_mesh;

// Storage helpers
void PunkMesh::setStorage(fs::FS* fs, const char* prefix) {
    _storage = fs;
    _storage_prefix = String(prefix);
    Serial.printf("[STORAGE] Set to %s, prefix=\"%s\"\n",
        (fs == &LittleFS) ? "LittleFS" : "SD", prefix);
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
        Serial.printf("require failed: %s\n", lua_tostring(L, -1));
        lua_pop(L, 1);
        return;
    }

    lua_getfield(L, -1, "__dispatch");
    if (!lua_isfunction(L, -1)) {
        Serial.println("__dispatch not a function!");
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
    lua_pushboolean(L, contains_mention(text, the_mesh._prefs.node_name)); // arg9: is_mention
    push_path_table(L, path_len, path);         // arg10: path
    if (pkt_hash) {                              // arg11: hash (hex string)
        char hex[MAX_HASH_SIZE * 2 + 1];
        mesh::Utils::toHex(hex, pkt_hash, MAX_HASH_SIZE);
        lua_pushstring(L, hex);
    } else {
        lua_pushnil(L);
    }

    if (lua_pcall(L, 11, 0, 0) != LUA_OK) {
        Serial.printf("__dispatch failed: %s\n", lua_tostring(L, -1));
        lua_pop(L, 1);
    }

    lua_pop(L, 1);
}

// Dispatch a direct message to Lua. See channel variant above.
void lua_mesh_push_direct_message(lua_State* L, const char* sender_name, uint8_t hops, bool direct, uint32_t timestamp, const char *text, float snr, float rssi, uint16_t path_len, const uint8_t* path, const uint8_t* pkt_hash) {
    lua_getglobal(L, "require");
    lua_pushstring(L, "lib/mesh/messages");

    if (lua_pcall(L, 1, 1, 0) != LUA_OK) {
        Serial.printf("require failed: %s\n", lua_tostring(L, -1));
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
        Serial.printf("__dispatch_dm failed: %s\n", lua_tostring(L, -1));
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
        Serial.printf("__dispatch_contact failed: %s\n", lua_tostring(L, -1));
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

void PunkMesh::loadContacts()
{
    bool is_sd = (_storage != &LittleFS);
    if (is_sd) sd_spi_take();

    String path = storagePath(_storage_prefix, "/contacts");
    if (_storage->exists(path.c_str()))
    {
        File file = _storage->open(path.c_str());
        if (file)
        {
            char line[320];
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

                // Parse tab-separated: pubkey_hex \t name \t type \t flags \t path_len \t advert_ts \t path_hex
                char *fields[7];
                int nf = 0;
                fields[0] = line;
                for (int i = 0; i < len && nf < 6; i++) {
                    if (line[i] == '\t') {
                        line[i] = '\0';
                        fields[++nf] = &line[i + 1];
                    }
                }
                if (nf < 1) continue; // need at least pubkey + name

                ContactInfo c;
                memset(&c, 0, sizeof(c));

                uint8_t pub_key[32];
                if (!mesh::Utils::fromHex(pub_key, 32, fields[0])) continue;
                c.id = mesh::Identity(pub_key);

                strncpy(c.name, fields[1], sizeof(c.name) - 1);
                if (nf >= 2) c.type = atoi(fields[2]);
                if (nf >= 3) c.flags = atoi(fields[3]);
                if (nf >= 4) c.out_path_len = atoi(fields[4]);
                if (nf >= 5) c.last_advert_timestamp = strtoul(fields[5], nullptr, 10);
                if (nf >= 6) mesh::Utils::fromHex(c.out_path, 64, fields[6]);
                c.lastmod = 0;

                if (!addContact(c)) break;
            }
            file.close();
        }
    }

    if (is_sd) sd_spi_release();
}

void PunkMesh::saveContacts()
{
    bool is_sd = (_storage != &LittleFS);
    if (is_sd) sd_spi_take();

    String path = storagePath(_storage_prefix, "/contacts");
    File file = _storage->open(path.c_str(), "w", true);
    if (file)
    {
        ContactsIterator iter;
        ContactInfo c;

        while (iter.hasNext(this, c))
        {
            char pubkey_hex[65];
            mesh::Utils::toHex(pubkey_hex, c.id.pub_key, 32);

            char path_hex[129];
            mesh::Utils::toHex(path_hex, c.out_path, 64);

            file.printf("%s\t%s\t%d\t%d\t%d\t%u\t%s\n",
                pubkey_hex, c.name, c.type, c.flags,
                c.out_path_len, c.last_advert_timestamp, path_hex);
        }
        file.close();
    }

    if (is_sd) sd_spi_release();
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
                Serial.printf("[MESH INIT] Restored channel[%d]: %s\n", slot, cd.name);
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
                            const uint8_t* pkt_hash = nullptr) {
    memset(&m, 0, sizeof(m));
    m.timestamp   = timestamp;
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
}

// Push path hashes as a Lua table of hex strings.
static void push_path_table(lua_State* L, uint16_t path_len, const uint8_t* path) {
    lua_newtable(L);
    uint8_t hash_size = (path_len >> 6) + 1;
    uint8_t hash_count = path_len & 63;
    char hex[7];
    for (int j = 0; j < hash_count; j++) {
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
    char hex[7];
    for (int j = 0; j < hash_count; j++) {
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
    char hex[7];
    for (int j = 0; j < hash_count; j++) {
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

static void trim_msg_text_file(fs::FS* storage, const String& path, int cap) {
    if (!storage) return;
    File f = storage->open(path.c_str(), "r");
    if (!f) return;
    int count = count_text_records(f);
    int max_count = (cap > 0 ? cap : 100) * 2;
    if (count <= max_count) { f.close(); return; }

    int keep = cap > 0 ? cap : 100;
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

    File wf = storage->open(path.c_str(), "w", true);
    if (wf) {
        wf.write(buf, got);
        wf.close();
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
        Serial.printf("[STORAGE] Cleared old binary log: %s\n", fpath.c_str());
    }

    File f = storage->open(fpath.c_str(), "a", true);
    if (f) {
        f.printf("ts=%u\n", m.timestamp);
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
        f.print("---\n");
        f.close();
    }
    trim_msg_text_file(storage, fpath, cap);
    if (is_sd) sd_spi_release();
}

void PunkMesh::appendChannelMessage(int channel_idx, const char* from, const char* text,
                                    uint32_t timestamp, float snr, float rssi,
                                    uint8_t hops, bool direct,
                                    uint16_t path_len, const uint8_t* path,
                                    const uint8_t* pkt_hash) {
    if (channel_idx < 0) return;
    StoredMsg m;
    fill_stored_msg(m, channel_idx, from, /*peer*/ "", text,
                    timestamp, snr, rssi, hops, direct, /*is_dm*/ false,
                    path_len, path, pkt_hash);
    String ch_name = channel_name_for_idx(*this, channel_idx);
    append_msg_text(_storage, _storage_prefix,
                    channel_msg_path(_storage_prefix, ch_name.c_str()),
                    m, _max_messages);
}

void PunkMesh::appendDMMessage(const char* peer, const char* from, const char* text,
                               uint32_t timestamp, float snr, float rssi,
                               uint8_t hops, bool direct,
                               uint16_t path_len, const uint8_t* path,
                               const uint8_t* pkt_hash) {
    if (!peer || peer[0] == '\0') return;
    StoredMsg m;
    fill_stored_msg(m, /*ch_idx*/ -1, from, peer, text,
                    timestamp, snr, rssi, hops, direct, /*is_dm*/ true,
                    path_len, path, pkt_hash);
    append_msg_text(_storage, _storage_prefix,
                    dm_msg_path(_storage_prefix, peer),
                    m, _max_messages);
}

static void push_stored_msg_table(lua_State* L, const StoredMsg& m) {
    lua_newtable(L);
    lua_pushstring(L, m.from);      lua_setfield(L, -2, "from");
    lua_pushstring(L, m.peer);      lua_setfield(L, -2, "peer");
    lua_pushstring(L, m.text);      lua_setfield(L, -2, "text");
    lua_pushinteger(L, m.timestamp); lua_setfield(L, -2, "timestamp");
    lua_pushinteger(L, m.hops);      lua_setfield(L, -2, "hops");
    lua_pushnumber(L, m.snr);        lua_setfield(L, -2, "snr");
    lua_pushnumber(L, m.rssi);       lua_setfield(L, -2, "rssi");
    lua_pushboolean(L, (m.flags & 0x01) != 0); lua_setfield(L, -2, "direct");
    lua_pushboolean(L, (m.flags & 0x02) != 0); lua_setfield(L, -2, "is_dm");
    lua_pushinteger(L, m.channel_idx); lua_setfield(L, -2, "channel_idx");
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
        if (hash_size == 0 || hash_size > 3) break;
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
    File entry = root.openNextFile();
    while (entry) {
        if (!entry.isDirectory()) {
            String name = entry.name();
            // Basename check: endsWith(".log") and contains "dm_"
            int slash = name.lastIndexOf('/');
            String base = (slash >= 0) ? name.substring(slash + 1) : name;
            if (base.startsWith("dm_") && base.endsWith(".log")) {
                StoredMsg m;
                if (entry.read((uint8_t*)&m, sizeof(m)) == sizeof(m)
                    && m.peer[0] != '\0') {
                    lua_pushstring(L, m.peer);
                    lua_rawseti(L, -2, idx++);
                }
            }
        }
        entry = root.openNextFile();
    }
    root.close();
    if (is_sd) sd_spi_release();
    return 1;
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
        Serial.println("   (OK - clock set!)");
    }
    else
    {
        Serial.println("   (ERR: clock cannot go backwards)");
    }
}

void PunkMesh::importCard(const char *command)
{
    while (*command == ' ')
        command++; // skip leading spaces
    if (memcmp(command, "meshcore://", 11) == 0)
    {
        command += 11;                 // skip the prefix
        char *ep = strchr(command, 0); // find end of string
        while (ep > command)
        {
            ep--;
            if (mesh::Utils::isHexChar(*ep))
                break; // found tail end of card
            *ep = 0;   // remove trailing spaces and other junk
        }
        int len = strlen(command);
        if (len % 2 == 0)
        {
            len >>= 1; // halve, for num bytes
            if (mesh::Utils::fromHex(tmp_buf, len, command))
            {
                importContact(tmp_buf, len);
                return;
            }
        }
    }
    Serial.println("   error: invalid format");
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
    Serial.println("[MESH RX] ========== ADVERT RECEIVED ==========");
    Serial.printf("[MESH RX] Name: %s (%s)\n", contact.name, is_new ? "NEW" : "known");
    Serial.printf("[MESH RX] Type: %s, path_len: %d\n", getTypeName(contact.type), path_len);
    Serial.print("[MESH RX] Public key: ");
    mesh::Utils::printHex(Serial, contact.id.pub_key, PUB_KEY_SIZE);
    Serial.println();
    Serial.printf("[MESH RX] Total contacts now: %d\n", getNumContacts() + (is_new ? 1 : 0));

    recordPath(contact.id.pub_key, path_len, path,
               last_rx_snr, last_rx_rssi, PATH_SRC_ADVERT, false);

    saveContacts();

    if (rx_event_queue) {
        RxEvent ev = {};
        ev.kind = RxEvent::CONTACT_UPDATE;
        strncpy(ev.sender, contact.name, sizeof(ev.sender) - 1);
        ev.hops = contact.type;
        xQueueSend(rx_event_queue, &ev, 0);
    }
}

void PunkMesh::onContactPathUpdated(const ContactInfo &contact)
{
    Serial.printf("PATH to: %s, path_len=%d\n", contact.name, (int32_t)contact.out_path_len);
    recordPath(contact.id.pub_key, contact.out_path_len, contact.out_path,
               0, 0, PATH_SRC_PATH_UPDATE, true);
    saveContacts();
}

ContactInfo* PunkMesh::processAck(const uint8_t *data)
{
    if (memcmp(data, &expected_ack_crc, 4) == 0)
    {
        uint32_t rtt = _ms->getMillis() - last_msg_sent;
        Serial.printf("   Got ACK! (round trip: %d millis)\n", rtt);
        expected_ack_crc = 0;
        if (curr_recipient) {
            recordPathSuccess(curr_recipient->id.pub_key, rtt);
        }
        return curr_recipient;
    }
    return nullptr;
}

void PunkMesh::onMessageRecv(const ContactInfo &from, mesh::Packet *pkt, uint32_t sender_timestamp, const char *text)
{
    Serial.println("[MESH RX] ========== DIRECT MSG RECEIVED ==========");
    Serial.printf("[MESH RX] From: %s, route: %s, hops: %d\n",
        from.name, pkt->isRouteDirect() ? "DIRECT" : "FLOOD", pkt->getPathHashCount());
    Serial.printf("[MESH RX] Text: \"%s\"\n", text);
    Serial.printf("[MESH RX] Sender timestamp: %u\n", sender_timestamp);

    if (strcmp(text, "clock sync") == 0)
    { // special text command
        setClock(sender_timestamp + 1);
        return;
    }

    // Normalize UTF-8 smart quotes to ASCII so they render from montserrat
    // instead of tofu. Do it once, before both persistence and Lua dispatch.
    char norm_text[160];
    normalize_smart_quotes(text, norm_text, sizeof(norm_text));

    // Persist incoming DM — peer and from are both the sender for incoming.
    appendDMMessage(from.name, from.name, norm_text, sender_timestamp,
                    last_rx_snr, last_rx_rssi, pkt->getPathHashCount(),
                    pkt->isRouteDirect(), pkt->path_len, pkt->path,
                    _last_pkt_hash);

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
        ev.timestamp = sender_timestamp;
        ev.snr       = last_rx_snr;
        ev.rssi      = last_rx_rssi;
        ev.path_len  = pkt->path_len;
        memcpy(ev.path, pkt->path, pkt->getPathByteLen());
        memcpy(ev.pkt_hash, _last_pkt_hash, MAX_HASH_SIZE);
        if (xQueueSend(rx_event_queue, &ev, 0) != pdTRUE) {
            Serial.println("[MESH RX] WARNING: rx_event_queue full, dropping DM");
        }
    }
}

void PunkMesh::onCommandDataRecv(const ContactInfo &from, mesh::Packet *pkt, uint32_t sender_timestamp, const char *text)
{
}
void PunkMesh::onSignedMessageRecv(const ContactInfo &from, mesh::Packet *pkt, uint32_t sender_timestamp, const uint8_t *sender_prefix, const char *text)
{
}

void PunkMesh::onChannelMessageRecv(const mesh::GroupChannel &channel, mesh::Packet *pkt, uint32_t timestamp, const char *text)
{
    Serial.println("[MESH RX] ========== CHANNEL MSG RECEIVED ==========");
    Serial.printf("[MESH RX] Raw text: \"%s\"\n", text);
    Serial.printf("[MESH RX] Route: %s, hops: %d, timestamp: %u\n",
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
    Serial.printf("[MESH RX] Parsed sender: \"%s\", msg: \"%s\", channel_idx: %d\n", sender_name, msg_text, channel_idx);

    // Normalize UTF-8 smart quotes to ASCII in the message body so they
    // render from montserrat rather than as tofu.
    char norm_msg[160];
    normalize_smart_quotes(msg_text, norm_msg, sizeof(norm_msg));

    // Persist to disk (no-op if channel_idx < 0)
    appendChannelMessage(channel_idx, sender_name, norm_msg, timestamp,
                         last_rx_snr, last_rx_rssi, pkt->getPathHashCount(),
                         pkt->isRouteDirect(), pkt->path_len, pkt->path,
                         _last_pkt_hash);

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
        ev.timestamp = timestamp;
        ev.snr       = last_rx_snr;
        ev.rssi      = last_rx_rssi;
        ev.path_len  = pkt->path_len;
        memcpy(ev.path, pkt->path, pkt->getPathByteLen());
        memcpy(ev.pkt_hash, _last_pkt_hash, MAX_HASH_SIZE);
        if (xQueueSend(rx_event_queue, &ev, 0) != pdTRUE) {
            Serial.println("[MESH RX] WARNING: rx_event_queue full, dropping channel msg");
        }
    }
}

uint8_t PunkMesh::onContactRequest(const ContactInfo &contact, uint32_t sender_timestamp, const uint8_t *data, uint8_t len, uint8_t *reply)
{
    return 0; // unknown
}

void PunkMesh::onContactResponse(const ContactInfo &contact, const uint8_t *data, uint8_t len)
{
    // not supported
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
    Serial.println("   ERROR: timed out, no ACK.");
    if (curr_recipient) {
        recordPathFailure(curr_recipient->id.pub_key);
    }
}

// ── Path history tracker ──────────────────────────────────────────

static bool paths_equal(uint16_t a_len, const uint8_t* a_path,
                        uint16_t b_len, const uint8_t* b_path) {
    if (a_len != b_len) return false;
    uint8_t byte_len = (a_len & 63) * ((a_len >> 6) + 1);
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
    char hex[7];
    for (int j = 0; j < hc; j++) {
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

void PunkMesh::sendFloodScoped(const ContactInfo& recipient, mesh::Packet* pkt, uint32_t delay_millis) {
    pkt->calculatePacketHash(_last_tx_hash);
    BaseChatMesh::sendFloodScoped(recipient, pkt, delay_millis);
}

void PunkMesh::sendFloodScoped(const mesh::GroupChannel& channel, mesh::Packet* pkt, uint32_t delay_millis) {
    pkt->calculatePacketHash(_last_tx_hash);
    BaseChatMesh::sendFloodScoped(channel, pkt, delay_millis);
}

// ─────────────────────────────────────────────────────────────────

PunkMesh::PunkMesh(mesh::Radio &radio, StdRNG &rng, mesh::RTCClock &rtc, SimpleMeshTables &tables)
    : BaseChatMesh(radio, *new ArduinoMillis(), rng, rtc, *new StaticPoolPacketManager(16), tables)
{
    // defaults
    memset(&_prefs, 0, sizeof(_prefs));
    _prefs.airtime_factor = 2.0; // one third
    strcpy(_prefs.node_name, "NONAME");
    _prefs.freq = LORA_FREQ;
    _prefs.tx_power_dbm = LORA_TX_POWER;
    _prefs.bandwidth = LORA_BW;
    _prefs.spreading_factor = LORA_SF;
    _prefs.coding_rate = LORA_CR;

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
    String contactsPath = storagePath(_storage_prefix, "/contacts");
    Serial.printf("[STORAGE] Identity path: %s\n", idPath.c_str());
    Serial.printf("[STORAGE] Prefs path: %s\n", prefsPath.c_str());

    bool identity_loaded = false;
    bool id_is_sd = (_storage != &LittleFS);
    if (id_is_sd) sd_spi_take();
    if (_storage->exists(idPath.c_str())) {
        File file = _storage->open(idPath.c_str());
        if (file) {
            identity_loaded = self_id.readFrom(file);
            file.close();
            if (identity_loaded) {
                Serial.printf("[STORAGE] Loaded identity from %s\n", idPath.c_str());
            } else {
                Serial.printf("[STORAGE] WARNING: Failed to read %s\n", idPath.c_str());
            }
        }
    } else {
        Serial.printf("[STORAGE] No identity file at %s\n", idPath.c_str());
    }
    if (id_is_sd) sd_spi_release();

    if (id_is_sd) {
        if (identity_loaded && !LittleFS.exists("/identity")) {
            File lfs_file = LittleFS.open("/identity", "w", true);
            if (lfs_file) {
                self_id.writeTo(lfs_file);
                lfs_file.close();
                Serial.println("[STORAGE] Copied identity from SD to LittleFS");
            }
        } else if (!identity_loaded && LittleFS.exists("/identity")) {
            File lfs_file = LittleFS.open("/identity");
            if (lfs_file) {
                identity_loaded = self_id.readFrom(lfs_file);
                lfs_file.close();
                if (identity_loaded) {
                    Serial.println("[STORAGE] Loaded identity from LittleFS fallback");
                    sd_spi_take();
                    File sd_file = _storage->open(idPath.c_str(), "w", true);
                    if (sd_file) {
                        self_id.writeTo(sd_file);
                        sd_file.close();
                        Serial.println("[STORAGE] Copied identity from LittleFS to SD");
                    }
                    sd_spi_release();
                }
            }
        }
    }

    // If no saved identity, generate a new one
    if (!identity_loaded) {
        Serial.println("[STORAGE] Generating new identity...");
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
            Serial.printf("[STORAGE] Identity saved to %s: %s\n", idPath.c_str(), ok ? "OK" : "FAILED");
        } else {
            Serial.printf("[STORAGE] ERROR: Cannot open %s for writing!\n", idPath.c_str());
        }
        if (is_sd) {
            sd_spi_release();
            File lfs_file = LittleFS.open("/identity", "w", true);
            if (lfs_file) {
                self_id.writeTo(lfs_file);
                lfs_file.close();
                Serial.println("[STORAGE] Identity also saved to LittleFS");
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
            }
            file.close();
            Serial.printf("[STORAGE] Loaded prefs from %s (name=%s, freq=%.3f)\n",
                prefsPath.c_str(), _prefs.node_name, _prefs.freq);
        }
    } else {
        Serial.printf("[STORAGE] No prefs file at %s, using defaults\n", prefsPath.c_str());
    }

    loadContacts();
    Serial.printf("[MESH INIT] Loaded %d contacts from flash\n", getNumContacts());

    _public = addChannel("Public", PUBLIC_GROUP_PSK);
    if (_public) {
        Serial.println("[MESH INIT] Public channel created OK");
        Serial.print("[MESH INIT] Channel hash: ");
        mesh::Utils::printHex(Serial, _public->channel.hash, 6);
        Serial.println();
    } else {
        Serial.println("[MESH INIT] ERROR: addChannel returned NULL!");
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

    Serial.println("[RADIO RX] ---- packet received ----");
    Serial.printf("[RADIO RX] len=%d, type=%d, route=%s, payload_len=%d\n",
        len, pkt->getPayloadType(),
        pkt->isRouteDirect() ? "DIRECT" : "FLOOD",
        pkt->payload_len);
    Serial.printf("[RADIO RX] SNR=%d, RSSI=%d, score=%d\n",
        (int)last_rx_snr, (int)last_rx_rssi, (int)(score*1000));
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
        file.close();
        Serial.printf("[STORAGE] Prefs saved to %s\n", path);
    } else {
        Serial.printf("[STORAGE] ERROR: Cannot save prefs to %s\n", path);
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

void PunkMesh::showWelcome()
{
    Serial.println("===== MeshCore Chat Terminal =====");
    Serial.println();
    Serial.printf("WELCOME  %s\n", _prefs.node_name);

    // Serial.print("Public key: ");
    // mesh::Utils::printHex(Serial, self_id.pub_key, PUB_KEY_SIZE);

    // Serial.print("Private key: ");
    // mesh::Utils::printHex(Serial, self_id.prv_key, PRIV_KEY_SIZE);

    self_id.printTo(Serial);

    Serial.println();
    Serial.println("   (enter 'help' for basic commands)");
    Serial.println();
}

void PunkMesh::sendSelfAdvert(int delay_millis)
{
    Serial.printf("[MESH TX] sendSelfAdvert: name=%s, delay=%d ms\n", _prefs.node_name, delay_millis);
    auto pkt = createSelfAdvert(_prefs.node_name, _prefs.node_lat, _prefs.node_lon);
    if (pkt)
    {
        Serial.printf("[MESH TX] Advert packet created, payload_len=%d, sending flood...\n", pkt->payload_len);
        sendFlood(pkt, delay_millis);
        Serial.println("[MESH TX] Advert flood sent.");
    }
    else
    {
        Serial.println("[MESH TX] ERROR: createSelfAdvert returned NULL!");
    }
}

// ContactVisitor
void PunkMesh::onContactVisit(const ContactInfo &contact)
{
    Serial.printf("   %s - ", contact.name);
    char tmp[40];
    int32_t secs = contact.last_advert_timestamp - getRTCClock()->getCurrentTime();
    AdvertTimeHelper::formatRelativeTimeDiff(tmp, secs, false);
    Serial.println(tmp);
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
                Serial.println("   ERROR: unable to send.");
            }
            else
            {
                last_msg_sent = _ms->getMillis();
                Serial.printf("   (message sent - %s)\n", result == MSG_SEND_SENT_FLOOD ? "FLOOD" : "DIRECT");
            }
        }
        else
        {
            Serial.println("   ERROR: no recipient selected (use 'to' cmd).");
        }
    }
    else if (memcmp(command, "public ", 7) == 0)
    { // send GroupChannel msg
        Serial.printf("[MESH TX] Serial 'public' command, text=\"%s\"\n", &command[7]);

        if (!_public) {
            Serial.println("[MESH TX] ERROR: _public channel is NULL!");
        } else {
            uint32_t timestamp = getRTCClock()->getCurrentTime();
            Serial.printf("[MESH TX] timestamp=%u, sender=%s\n", timestamp, _prefs.node_name);

            bool ok = sendGroupMessage(timestamp, _public->channel, _prefs.node_name, &command[7], strlen(&command[7]));
            Serial.printf("[MESH TX] sendGroupMessage returned %s\n", ok ? "true" : "false");
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
        Serial.printf("%02d:%02d - %d/%d/%d UTC\n", dt.hour(), dt.minute(), dt.day(), dt.month(), dt.year());
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
            Serial.printf("   Recipient %s now selected.\n", curr_recipient->name);
        }
        else
        {
            Serial.println("   Error: Name prefix not found.");
        }
    }
    else if (strcmp(command, "to") == 0)
    { // show current recipient
        if (curr_recipient)
        {
            Serial.printf("   Current: %s\n", curr_recipient->name);
        }
        else
        {
            Serial.println("   Err: no recipient selected");
        }
    }
    else if (strcmp(command, "advert") == 0)
    {
        auto pkt = createSelfAdvert(_prefs.node_name, _prefs.node_lat, _prefs.node_lon);
        if (pkt)
        {
            sendZeroHop(pkt);
            Serial.println("   (advert sent, zero hop).");
        }
        else
        {
            Serial.println("   ERR: unable to send");
        }
    }
    else if (strcmp(command, "reset path") == 0)
    {
        if (curr_recipient)
        {
            resetPathTo(*curr_recipient);
            saveContacts();
            Serial.println("   Done.");
        }
    }
    else if (memcmp(command, "card", 4) == 0)
    {
        Serial.printf("Hello %s\n", _prefs.node_name);
        auto pkt = createSelfAdvert(_prefs.node_name, _prefs.node_lat, _prefs.node_lon);
        if (pkt)
        {
            uint8_t len = pkt->writeTo(tmp_buf);
            releasePacket(pkt); // undo the obtainNewPacket()

            mesh::Utils::toHex(hex_buf, tmp_buf, len);
            Serial.println("Your MeshCore biz card:");
            Serial.print("meshcore://");
            Serial.println(hex_buf);
            Serial.println();
        }
        else
        {
            Serial.println("  Error");
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
            Serial.println("  OK");
        }
        else if (memcmp(config, "name ", 5) == 0)
        {
            StrHelper::strncpy(_prefs.node_name, &config[5], sizeof(_prefs.node_name));
            savePrefs();
            Serial.println("  OK");
        }
        else if (memcmp(config, "lat ", 4) == 0)
        {
            _prefs.node_lat = atof(&config[4]);
            savePrefs();
            Serial.println("  OK");
        }
        else if (memcmp(config, "lon ", 4) == 0)
        {
            _prefs.node_lon = atof(&config[4]);
            savePrefs();
            Serial.println("  OK");
        }
        else if (memcmp(config, "tx ", 3) == 0)
        {
            _prefs.tx_power_dbm = atoi(&config[3]);
            savePrefs();
            Serial.println("  OK - reboot to apply");
        }
        else if (memcmp(config, "freq ", 5) == 0)
        {
            _prefs.freq = atof(&config[5]);
            savePrefs();
            Serial.println("  OK - reboot to apply");
        }
        else if (memcmp(config, "bw ", 3) == 0)
        {
            _prefs.bandwidth = atof(&config[3]);
            savePrefs();
            Serial.println("  OK - reboot to apply");
        }
        else if (memcmp(config, "sf ", 3) == 0)
        {
            _prefs.spreading_factor = atoi(&config[3]);
            savePrefs();
            Serial.println("  OK - reboot to apply");
        }
        else if (memcmp(config, "cr ", 3) == 0)
        {
            _prefs.coding_rate = atoi(&config[3]);
            savePrefs();
            Serial.println("  OK - reboot to apply");
        }
        else
        {
            Serial.printf("  ERROR: unknown config: %s\n", config);
        }
    }
    else if (memcmp(command, "ver", 3) == 0)
    {
        Serial.println(FIRMWARE_VER_TEXT);
    }
    else if (strcmp(command, "diag") == 0)
    {
        Serial.println("===== MESH DIAGNOSTICS =====");
        Serial.printf("  Storage: %s, prefix: \"%s\"\n",
            (_storage == &LittleFS) ? "LittleFS" : "SD", _storage_prefix.c_str());
        Serial.printf("  Node name: %s\n", _prefs.node_name);
        Serial.printf("  Freq pref (runtime): %.3f MHz\n", _prefs.freq);
        Serial.printf("  Freq (build-time):   %.3f MHz\n", (float)LORA_FREQ);
        Serial.printf("  BW pref: %.1f kHz (build: %d)\n", _prefs.bandwidth, LORA_BW);
        Serial.printf("  SF pref: %d (build: %d)\n", _prefs.spreading_factor, LORA_SF);
        Serial.printf("  CR pref: %d (build: %d)\n", _prefs.coding_rate, LORA_CR);
        Serial.printf("  TX power pref: %d dBm (build: %d)\n", _prefs.tx_power_dbm, LORA_TX_POWER);
        Serial.printf("  Airtime factor: %.2f\n", _prefs.airtime_factor);
        Serial.printf("  GPS: %.4f, %.4f\n", _prefs.node_lat, _prefs.node_lon);
        if (_prefs.freq != (float)LORA_FREQ) {
            Serial.println("  *** WARNING: runtime freq != build freq! Old /node_prefs? ***");
        }
        Serial.print("  Pub key: ");
        mesh::Utils::printHex(Serial, self_id.pub_key, PUB_KEY_SIZE);
        Serial.println();
        Serial.printf("  Num contacts: %d\n", getNumContacts());
        Serial.printf("  Public channel: %s\n", _public ? "configured" : "NULL (PROBLEM!)");
        Serial.printf("  Lua runtime: %s\n", lua_runtime ? "attached" : "NULL (PROBLEM!)");
        Serial.printf("  RTC clock: %u\n", getRTCClock()->getCurrentTime());
        Serial.printf("  Uptime: %lu ms\n", millis());
        Serial.println();
        Serial.println("  Contacts:");
        if (getNumContacts() == 0) {
            Serial.println("    (none)");
        } else {
            scanRecentContacts(0, this);
        }
        Serial.println("============================");
    }
    else if (memcmp(command, "help", 4) == 0)
    {
        Serial.println("Commands:");
        Serial.println("   set {name|lat|lon|freq|tx|bw|sf|cr|af} {value}");
        Serial.println("   card");
        Serial.println("   import {biz card}");
        Serial.println("   clock");
        Serial.println("   time <epoch-seconds>");
        Serial.println("   list {n}");
        Serial.println("   to <recipient name or prefix>");
        Serial.println("   to");
        Serial.println("   send <text>");
        Serial.println("   advert");
        Serial.println("   reset path");
        Serial.println("   public <text>");
        Serial.println("   diag");
    }
    else
    {
        Serial.print("   ERROR: unknown command: ");
        Serial.println(command);
    }
}

void PunkMesh::loop()
{
    BaseChatMesh::loop();

    int len = strlen(command);
    while (Serial.available() && len < sizeof(command) - 1)
    {
        char c = Serial.read();
        if (c != '\n')
        {
            command[len++] = c;
            command[len] = 0;
        }
        Serial.print(c);
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
