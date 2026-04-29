#include "punkmesh.h"
#include <LittleFS.h>
#include "meshpunk_sync.h"

// Shared-SPI-bus lock pair (defined in main.cpp).
// sd_spi_take()    — acquire spi_bus_mutex before any SD operation.
// sd_spi_release() — release spi_bus_mutex after the SD file handle is closed.
// sd_spi_take() is inline in meshpunk_sync.h (just SPI_LOCK); no extern decl needed.
extern void sd_spi_release();

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

// Dispatch a channel (public) message to Lua with parsed sender name.
// Called from the UI core (drain_rx_events) — the packet object is gone by
// the time we run, so hops/direct come from the enqueued RxEvent.
void lua_mesh_push_channel_message(lua_State* L, const char* sender_name, uint8_t hops, bool direct, uint32_t timestamp, const char *text, float snr, float rssi, int channel_idx) {
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
    lua_pushstring(L, text);                    // arg2: text (lua_pushstring copies internally)
    lua_pushinteger(L, timestamp);              // arg3: timestamp
    lua_pushboolean(L, direct);                 // arg4: direct
    lua_pushinteger(L, hops);                   // arg5: hops
    lua_pushnumber(L, snr);                     // arg6: snr
    lua_pushnumber(L, rssi);                    // arg7: rssi
    lua_pushinteger(L, channel_idx);            // arg8: channel_idx (-1 if unknown)

    if (lua_pcall(L, 8, 0, 0) != LUA_OK) {
        Serial.printf("__dispatch failed: %s\n", lua_tostring(L, -1));
        lua_pop(L, 1);
    }

    lua_pop(L, 1); // pop module
}

// Dispatch a direct message to Lua. See channel variant above.
void lua_mesh_push_direct_message(lua_State* L, const char* sender_name, uint8_t hops, bool direct, uint32_t timestamp, const char *text, float snr, float rssi) {
    lua_getglobal(L, "require");
    lua_pushstring(L, "lib/mesh/messages");

    if (lua_pcall(L, 1, 1, 0) != LUA_OK) {
        Serial.printf("require failed: %s\n", lua_tostring(L, -1));
        lua_pop(L, 1);
        return;
    }

    lua_getfield(L, -1, "__dispatch_dm");
    if (!lua_isfunction(L, -1)) {
        // __dispatch_dm not defined yet, fall back silently
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

    if (lua_pcall(L, 7, 0, 0) != LUA_OK) {
        Serial.printf("__dispatch_dm failed: %s\n", lua_tostring(L, -1));
        lua_pop(L, 1);
    }

    lua_pop(L, 1); // pop module
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

static String channel_msg_path(const String& prefix, int ch_idx) {
    return messages_dir(prefix) + "/ch_" + String(ch_idx) + ".log";
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

// Fill a StoredMsg from the common fields. Truncates long strings silently.
static void fill_stored_msg(StoredMsg& m, int channel_idx, const char* from,
                            const char* peer, const char* text,
                            uint32_t timestamp, float snr, float rssi,
                            uint8_t hops, bool direct, bool is_dm) {
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
}

// Trim a message log file down to the last _max_messages records if it
// has grown past ~2x the cap. Cheap no-op when under threshold.
static void trim_msg_file(fs::FS* storage, const String& path, int cap) {
    if (!storage) return;
    File f = storage->open(path.c_str(), "r");
    if (!f) return;
    size_t sz = f.size();
    size_t rec = sizeof(StoredMsg);
    size_t count = sz / rec;
    size_t max_count = (size_t)(cap > 0 ? cap : 100) * 2;
    if (count <= max_count) { f.close(); return; }

    size_t keep = (size_t)(cap > 0 ? cap : 100);
    size_t skip = count - keep;
    f.seek(skip * rec);

    size_t remaining = keep * rec;
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

// Core append: one record written in "a" mode, then trim check.
static void append_stored_msg(fs::FS* storage, const String& prefix,
                              const String& path, const StoredMsg& m, int cap) {
    if (!storage) return;
    bool is_sd = (storage != &LittleFS);
    if (is_sd) sd_spi_take();
    ensure_messages_dir(storage, prefix);
    File f = storage->open(path.c_str(), "a", true);
    if (f) {
        f.write((const uint8_t*)&m, sizeof(m));
        f.close();
    }
    trim_msg_file(storage, path, cap);
    if (is_sd) sd_spi_release();
}

void PunkMesh::appendChannelMessage(int channel_idx, const char* from, const char* text,
                                    uint32_t timestamp, float snr, float rssi,
                                    uint8_t hops, bool direct) {
    if (channel_idx < 0) return;  // unknown channel — don't persist
    StoredMsg m;
    fill_stored_msg(m, channel_idx, from, /*peer*/ "", text,
                    timestamp, snr, rssi, hops, direct, /*is_dm*/ false);
    append_stored_msg(_storage, _storage_prefix,
                      channel_msg_path(_storage_prefix, channel_idx),
                      m, _max_messages);
}

void PunkMesh::appendDMMessage(const char* peer, const char* from, const char* text,
                               uint32_t timestamp, float snr, float rssi,
                               uint8_t hops, bool direct) {
    if (!peer || peer[0] == '\0') return;
    StoredMsg m;
    fill_stored_msg(m, /*ch_idx*/ -1, from, peer, text,
                    timestamp, snr, rssi, hops, direct, /*is_dm*/ true);
    append_stored_msg(_storage, _storage_prefix,
                      dm_msg_path(_storage_prefix, peer),
                      m, _max_messages);
}

// Push one StoredMsg as a Lua table on the top of the stack.
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
}

static int push_msg_file_to_lua(lua_State* L, fs::FS* storage, const String& path) {
    lua_newtable(L);
    if (!storage) return 1;
    bool is_sd = (storage != &LittleFS);
    if (is_sd) sd_spi_take();
    if (!storage->exists(path.c_str())) {
        if (is_sd) sd_spi_release();
        return 1;
    }
    File f = storage->open(path.c_str(), "r");
    if (!f) {
        if (is_sd) sd_spi_release();
        return 1;
    }
    int idx = 1;
    StoredMsg m;
    while (f.read((uint8_t*)&m, sizeof(m)) == sizeof(m)) {
        push_stored_msg_table(L, m);
        lua_rawseti(L, -2, idx++);
    }
    f.close();
    if (is_sd) sd_spi_release();
    return 1;
}

int PunkMesh::pushChannelMessagesToLua(lua_State* L, int channel_idx) {
    if (channel_idx < 0) { lua_newtable(L); return 1; }
    return push_msg_file_to_lua(L, _storage, channel_msg_path(_storage_prefix, channel_idx));
}

int PunkMesh::pushDMMessagesToLua(lua_State* L, const char* peer) {
    if (!peer || peer[0] == '\0') { lua_newtable(L); return 1; }
    return push_msg_file_to_lua(L, _storage, dm_msg_path(_storage_prefix, peer));
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
    saveContacts();
}

ContactInfo* PunkMesh::processAck(const uint8_t *data)
{
    if (memcmp(data, &expected_ack_crc, 4) == 0)
    { // got an ACK from recipient
        Serial.printf("   Got ACK! (round trip: %d millis)\n", _ms->getMillis() - last_msg_sent);
        // NOTE: the same ACK can be received multiple times!
        expected_ack_crc = 0; // reset our expected hash, now that we have received ACK
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
                    pkt->isRouteDirect());

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
        // Non-blocking. If the UI is behind, we'd rather drop than
        // stall the mesh task.
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
                         pkt->isRouteDirect());

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
}

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
        // Store radio info for Lua access
        last_rx_snr = _radio->getLastSNR();
        last_rx_rssi = _radio->getLastRSSI();

        Serial.println("[RADIO RX] ---- packet received ----");
        Serial.printf("[RADIO RX] len=%d, type=%d, route=%s, payload_len=%d\n",
            len, pkt->getPayloadType(),
            pkt->isRouteDirect() ? "DIRECT" : "FLOOD",
            pkt->payload_len);
        Serial.printf("[RADIO RX] SNR=%d, RSSI=%d, score=%d\n",
            (int)last_rx_snr,
            (int)last_rx_rssi,
            (int)(score*1000));

    //     if (pkt->getPayloadType() == PAYLOAD_TYPE_PATH || pkt->getPayloadType() == PAYLOAD_TYPE_REQ
    //       || pkt->getPayloadType() == PAYLOAD_TYPE_RESPONSE || pkt->getPayloadType() == PAYLOAD_TYPE_TXT_MSG) {
    //       Serial.printf(" [%02X -> %02X]\n", (uint32_t)pkt->payload[1], (uint32_t)pkt->payload[0]);
    //     } else {
    //       Serial.printf("\n");
    //     }
    //     f.close();
    //   
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
