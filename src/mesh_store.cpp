// mesh_store.cpp — shared message store. See mesh_store.h for the API and
// locking contract. The bulk of this file is the store machinery lifted
// verbatim from punkmesh.cpp (2026-08-24 bisection); PunkMesh keeps thin
// idx->name wrappers over it.

#include "mesh_store.h"

#include <Arduino.h>
#include <FS.h>
#include <LittleFS.h>
#include <RTClib.h>      // DateTime (routing-store day buckets)
extern "C" {
#include <lua.h>
#include <lualib.h>
}
#include <esp_heap_caps.h>
#include <new>

#include "meshpunk_sync.h"   // SLog, sd_spi_take, SPI lock, meshpunk_gps_last_fix
#include "usb_manager.h"     // UsbFlashGuardIf

extern void sd_spi_release();
extern "C" char * emoji_compose(const char * in);

// Hex codec — byte-identical to the MeshCore Utils versions this code used
// before the lift (uppercase output; lowercase accepted on parse), so records
// written by older firmware read back unchanged.
static const char mstore_hex_chars[] = "0123456789ABCDEF";
static void mstore_to_hex(char* dest, const uint8_t* src, size_t len) {
    while (len > 0) {
        uint8_t b = *src++;
        *dest++ = mstore_hex_chars[b >> 4];
        *dest++ = mstore_hex_chars[b & 0x0F];
        len--;
    }
    *dest = 0;
}
static uint8_t mstore_hex_val(char c) {
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= '0' && c <= '9') return c - '0';
    return 0;
}
static bool mstore_from_hex(uint8_t* dest, int dest_size, const char* src_hex) {
    int len = strlen(src_hex);
    if (len != dest_size * 2) return false;
    uint8_t* dp = dest;
    while (dp - dest < dest_size) {
        char ch = *src_hex++;
        char cl = *src_hex++;
        *dp++ = (mstore_hex_val(ch) << 4) | mstore_hex_val(cl);
    }
    return true;
}

// Normalize UTF-8 smart quotes to ASCII equivalents. See mesh_store.h for
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

namespace mstore {

// ── State ────────────────────────────────────────────────────────────────────
#define MSTORE_UNREAD_CH_SLOTS (MSTORE_MAX_CHANNELS + 4)
#define MSTORE_UNREAD_DM_SLOTS 16

struct MStoreState {
    fs::FS*  storage = nullptr;
    String   prefix;
    // Per-protocol namespace: "" = the legacy root (MeshCore keeps its
    // existing files); another protocol id (e.g. "mtlite") folders that
    // protocol's messages/route under <prefix>/<ns>/ so protocols never
    // share or collide on conversation files.
    String   ns;
    int      max_messages = 400;
    uint16_t retain_days = 30;
    volatile bool prune_due = false;
    // Incremental retention sweep cursor (prune_step, Core 0 only).
    bool     sweep_active = false;
    int      sweep_idx = 0, sweep_count = 0;
    uint32_t sweep_cutoff = 0;
    String   sweep_files[64];
    // Unread counters, name-keyed; a slot is reusable once its count is 0.
    struct Unread { char name[32]; uint16_t count; };
    Unread   unread_ch[MSTORE_UNREAD_CH_SLOTS] = {};
    Unread   unread_dm[MSTORE_UNREAD_DM_SLOTS] = {};
};

// Allocated in PSRAM on first touch (setup's set_storage), keeping the Strings
// and counters off the internal pool like the PunkMesh members they replace.
static MStoreState& SS() {
    static MStoreState* p = nullptr;
    if (!p) {
        void* mem = heap_caps_malloc(sizeof(MStoreState),
                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!mem) mem = malloc(sizeof(MStoreState));
        p = new (mem) MStoreState();
    }
    return *p;
}

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

// Directory + path helpers. Storage follows SS().prefix so SD installs
// get "/meshpunk/messages/..." and LittleFS gets "/messages/...".
static String messages_dir(const String& prefix) {
    if (SS().ns.length()) return prefix + "/" + SS().ns + "/messages";
    return prefix + "/messages";
}

static String channel_msg_path(const String& prefix, const char* ch_name) {
    char safe[33];
    sanitize_peer_name(ch_name, safe, sizeof(safe));
    return messages_dir(prefix) + "/ch_" + String(safe) + ".log";
}
static String dm_msg_path(const String& prefix, const char* peer) {
    char safe[33];
    sanitize_peer_name(peer, safe, sizeof(safe));
    return messages_dir(prefix) + "/dm_" + String(safe) + ".log";
}

static void ensure_messages_dir(fs::FS* fs, const String& prefix) {
    if (!fs) return;
    // Cache by FS pointer AND resolved dir so the common path (every message
    // append) skips the exists() stat. setStorage() flips the pointer,
    // set_namespace() changes the dir — either re-creates on first use.
    static fs::FS* ready_for = nullptr;
    static String  ready_dir;
    String dir = messages_dir(prefix);
    if (fs == ready_for && dir == ready_dir) return;
    if (!fs->exists(dir.c_str())) fs->mkdir(dir.c_str());
    ready_for = fs;
    ready_dir = dir;
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
        if (byte_len > MSTORE_PATH_MAX) byte_len = MSTORE_PATH_MAX;
        memcpy(m.path, path_data, byte_len);
    }
    if (pkt_hash) {
        memcpy(m.pkt_hash, pkt_hash, MSTORE_HASH_SIZE);
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
void push_path_table(lua_State* L, uint16_t path_len, const uint8_t* path) {
    lua_newtable(L);
    uint8_t hash_size = (path_len >> 6) + 1;
    uint8_t hash_count = path_len & 63;
    char hex[9];  // up to 4-byte hashes (8 hex chars + NUL)
    for (int j = 0; j < hash_count && (j + 1) * hash_size <= MSTORE_PATH_MAX; j++) {
        mstore_to_hex(hex, &path[j * hash_size], hash_size);
        lua_pushstring(L, hex);
        lua_rawseti(L, -2, j + 1);
    }
}

// ── Text-format message persistence ──────────────────────────────
// Each record is a block of key=value lines terminated by "---".
// Unknown keys are ignored on load (forward-compatible).

// Write path hashes as "aa,bb,cc" (no key, no newline). Shared by the path=
// field, the inline rpath= line, and the .paths sidecar.
static void write_path_hex(File& f, uint16_t path_len, const uint8_t* path) {
    uint8_t hash_size = (path_len >> 6) + 1;
    uint8_t hash_count = path_len & 63;
    char hex[9];  // up to 4-byte hashes
    for (int j = 0; j < hash_count && (j + 1) * hash_size <= MSTORE_PATH_MAX; j++) {
        if (j > 0) f.print(",");
        mstore_to_hex(hex, &path[j * hash_size], hash_size);
        f.print(hex);
    }
}

// The per-conversation path sidecar beside a message log: ch_X.log -> ch_X.paths.
// Append-only "<hash_hex> <pathhex>;snr;rssi;direct" records, joined back to
// messages by hash on read (see read_msg_text_file). Keeps extra-path
// persistence O(1) instead of rewriting the whole message log per repeat heard.
static String paths_sidecar_for(const String& msg_log_path) {
    if (msg_log_path.endsWith(".log"))
        return msg_log_path.substring(0, msg_log_path.length() - 4) + ".paths";
    return msg_log_path + ".paths";
}

static void write_path_field(File& f, uint16_t path_len, const uint8_t* path) {
    if ((path_len & 63) == 0) return;
    f.print("path=");
    write_path_hex(f, path_len, path);
    f.print("\n");
}

static void write_rpath_line(File& f, uint16_t path_len, const uint8_t* path,
                             float snr, float rssi, bool is_direct) {
    f.print("rpath=");
    write_path_hex(f, path_len, path);
    f.printf(";%.2f;%.2f;%d\n", snr, rssi, is_direct ? 1 : 0);
}

// Buffered line reader: pulls a File in 512-byte blocks instead of one byte at a
// time. Per-byte fs::File::read() carries heavy virtual-call overhead that makes
// scanning a large log take seconds; block reads cut that by ~100x. next() fills
// `out` with the next line (NUL-terminated, trailing CR/LF stripped, chars past
// outsz-1 dropped) and returns its length, or -1 at end of file. The SD lock may
// be released between next() calls (the pending bytes live in RAM); the actual
// file read only happens inside next(), under the caller's lock.
struct BlockLineReader {
    File*   f;
    uint8_t buf[512];
    int     len = 0;   // valid bytes in buf
    int     pos = 0;   // next byte to consume
    explicit BlockLineReader(File* file) : f(file) {}
    int next(char* out, int outsz) {
        int n = 0;
        bool saw = false;
        for (;;) {
            if (pos >= len) {
                len = f->read(buf, sizeof(buf));
                pos = 0;
                if (len <= 0) break;            // EOF (or read error)
            }
            saw = true;
            char c = (char)buf[pos++];
            if (c == '\n') { out[n] = '\0'; return n; }
            if (c == '\r') continue;
            if (n < outsz - 1) out[n++] = c;     // overflow chars dropped
        }
        out[n] = '\0';
        return (saw || n > 0) ? n : -1;          // last line, or -1 at true EOF
    }
};

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
    UsbFlashGuardIf _g(!is_sd);   // LittleFS backend: internal-flash write
    if (is_sd) sd_spi_take();
    ensure_messages_dir(storage, prefix);

    // open("a", true) creates on demand — no separate exists() probe. (The old
    // pre-text binary-format detection is long obsolete and was removed.)
    size_t fsize = 0;
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
            char hash_hex[MSTORE_HASH_SIZE * 2 + 1];
            mstore_to_hex(hash_hex, m.pkt_hash, MSTORE_HASH_SIZE);
            f.printf("hash=%s\n", hash_hex);
            write_rpath_line(f, m.path_len, m.path, m.snr, m.rssi,
                             (m.flags & 0x01) != 0);
        }
        if (m.has_pub_key) {
            char pk_hex[13];
            mstore_to_hex(pk_hex, m.sender_pub_key, 6);
            f.printf("pubkey=%s\n", pk_hex);
        }
        if (m.has_loc) {
            f.printf("lat=%.6f\n", m.lat);
            f.printf("lon=%.6f\n", m.lon);
        }
        f.print("---\n");
        fsize = f.size();
        f.close();
    }
    // Size-gated backstop, measured from the append handle so the common path
    // needs no extra open. Retention by days is handled separately (pruneStep).
    if (fsize >= MSG_FILE_SAFETY_BYTES) trim_msg_text_file(storage, fpath, cap);
    if (is_sd) sd_spi_release();
}

// ── Per-message extra-path sidecar (append-only) ────────────────────────────
// When a stored message is heard again via a NEW route, record that path here.
// One append is O(1); the old approach rewrote the whole message log per repeat,
// churning large PSRAM blocks and fragmenting the heap. Joined to messages by
// hash on read (read_msg_text_file).
static const size_t PATHS_FILE_SAFETY_BYTES = 256 * 1024;  // sidecar size backstop

// Keep only the most-recent tail of an oversized sidecar. Streamed in fixed
// chunks (no whole-file buffer). Caller holds the SD lock.
static void trim_paths_sidecar(fs::FS* storage, const String& spath) {
    File f = storage->open(spath.c_str(), "r");
    if (!f) return;
    size_t sz = f.size();
    if (sz < PATHS_FILE_SAFETY_BYTES) { f.close(); return; }
    f.seek(sz - PATHS_FILE_SAFETY_BYTES / 2);            // drop the oldest half
    while (f.available()) { if (f.read() == '\n') break; }  // align to a record start
    String tmp = spath + ".tmp";
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
    storage->remove(spath.c_str());
    if (!storage->rename(tmp.c_str(), spath.c_str()))
        SLog.printf("[STORAGE] paths sidecar trim rename failed: %s\n", spath.c_str());
}

// Append one observed path for `hash` to the conversation's .paths sidecar.
// Self-locks the SD bus; the size backstop is measured from the append handle.
static void append_path_record(fs::FS* storage, const String& msg_log_path,
                               const uint8_t* hash, const ObservedPath& op) {
    if (!storage) return;
    bool is_sd = (storage != &LittleFS);
    UsbFlashGuardIf _g(!is_sd);   // LittleFS backend: internal-flash write
    if (is_sd) sd_spi_take();
    String spath = paths_sidecar_for(msg_log_path);
    size_t sz = 0;
    File f = storage->open(spath.c_str(), "a", true);
    if (f) {
        char hash_hex[MSTORE_HASH_SIZE * 2 + 1];
        mstore_to_hex(hash_hex, hash, MSTORE_HASH_SIZE);
        f.print(hash_hex);
        f.print(" ");
        write_path_hex(f, op.path_len, op.path);
        f.printf(";%.2f;%.2f;%d\n", op.snr, op.rssi, op.is_direct ? 1 : 0);
        sz = f.size();
        f.close();
    }
    if (sz >= PATHS_FILE_SAFETY_BYTES) trim_paths_sidecar(storage, spath);
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
static String route_dir(const String& prefix) {
    if (SS().ns.length()) return prefix + "/" + SS().ns + "/route";
    return prefix + "/route";
}

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
// cutoff. Records ("ts=..\n..\n---\n") are append/oldest-first, so the scan
// finds the first keeper's byte offset and the tail is copied out.
//
// Both phases run in 512-byte chunks. A per-byte File::read()/available()
// pair costs a full stdio/VFS round trip per byte — slow enough that scanning
// the old prefix of a design-normal file (MSG_FILE_SAFETY_BYTES allows 768KB)
// from the Core-0 loop starves IDLE0 past the 5s task watchdog and reboots
// the device; with the sweep restarting on the same file each boot, that is
// a boot loop (hw-observed, ~360KB prefix).
//
// Every read is checked: a short or failed read at either phase leaves the
// file untouched on disk — a partial tail must never be renamed over good
// data. delay(1) every 128KB keeps IDLE0 fed even on a slow card. Caller
// holds MESH_LOCK and the SD lock, so the file cannot grow mid-rewrite.
// .tmp + rename keeps it crash-safe like the count trim.
static void prune_msg_file_by_age(fs::FS* storage, const String& path, uint32_t cutoff_ts) {
    if (!storage || cutoff_ts == 0) return;
    File f = storage->open(path.c_str(), "r");
    if (!f) return;
    const long fsize = (long)f.size();

    long keep_start = -1;
    long rec_start = 0;
    uint32_t cur_ts = 0;
    char line[256];
    int  llen = 0;
    uint8_t buf[512];
    long consumed = 0;         // bytes handed to the parser so far
    long since_yield = 0;
    bool read_err = false;

    while (consumed < fsize && keep_start < 0) {
        int n = f.read(buf, sizeof(buf));
        if (n <= 0) { read_err = true; break; }
        for (int i = 0; i < n && keep_start < 0; i++) {
            char ch = (char)buf[i];
            bool eol = (ch == '\n' || ch == '\r');
            if (!eol && llen < (int)sizeof(line) - 1) line[llen++] = ch;
            // A line ends at its terminator or at the buffer cap; the rest of
            // an over-long line parses as its own line, as it always has.
            if (eol || llen >= (int)sizeof(line) - 1) {
                line[llen] = '\0';
                if (llen == 3 && line[0] == '-' && line[1] == '-' && line[2] == '-') {
                    if (cur_ts >= cutoff_ts) {
                        keep_start = rec_start;
                    } else {
                        rec_start = consumed + i + 1;  // next record starts after '\n'
                        cur_ts = 0;
                    }
                } else if (strncmp(line, "ts=", 3) == 0) {
                    cur_ts = strtoul(line + 3, nullptr, 10);
                }
                llen = 0;
            }
        }
        consumed += n;
        since_yield += n;
        if (since_yield >= 128 * 1024) { since_yield = 0; delay(1); }
    }
    if (read_err) {
        f.close();
        SLog.printf("[PRUNE] scan read failed at %ld/%ld - leaving %s untouched\n",
                    consumed, fsize, path.c_str());
        return;
    }
    if (keep_start < 0) keep_start = fsize;      // every record older than cutoff
    if (keep_start == 0) { f.close(); return; }  // nothing to drop

    if (!f.seek(keep_start)) {
        f.close();
        SLog.printf("[PRUNE] seek failed - leaving %s untouched\n", path.c_str());
        return;
    }
    String tmp = path + ".tmp";
    File wf = storage->open(tmp.c_str(), "w", true);
    if (!wf) { f.close(); return; }
    const long expect = fsize - keep_start;
    long copied = 0;
    since_yield = 0;
    while (copied < expect) {
        int n = f.read(buf, sizeof(buf));
        if (n <= 0) break;
        if ((long)wf.write(buf, n) != (long)n) break;   // write error / card full
        copied += n;
        since_yield += n;
        if (since_yield >= 128 * 1024) { since_yield = 0; delay(1); }
    }
    wf.close();
    f.close();
    if (copied != expect) {
        storage->remove(tmp.c_str());
        SLog.printf("[PRUNE] tail copy short (%ld/%ld) - leaving %s untouched\n",
                    copied, expect, path.c_str());
        return;
    }
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
    if (path_nbytes > MSTORE_PATH_MAX) path_nbytes = MSTORE_PATH_MAX;
    int32_t lat_e6 = m.has_loc ? (int32_t)(m.lat * 1000000.0 + (m.lat >= 0 ? 0.5 : -0.5)) : 0;
    int32_t lon_e6 = m.has_loc ? (int32_t)(m.lon * 1000000.0 + (m.lon >= 0 ? 0.5 : -0.5)) : 0;
    uint16_t body = (uint16_t)(1 + from_len + 4 + 4 + 4 + 2 + 1 + path_nbytes);

    uint8_t buf[2 + 1 + 31 + 4 + 4 + 4 + 2 + 1 + MSTORE_PATH_MAX];
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
    UsbFlashGuardIf _g(!is_sd);   // LittleFS backend: internal-flash write
    if (is_sd) sd_spi_take();
    bool new_day = route_write_record(storage, prefix, m);
    if (is_sd) sd_spi_release();
    return new_day;
}

// ── Backend + accessors ──────────────────────────────────────────────────────

void set_storage(fs::FS* fs, const char* pfx) {
    SS().storage = fs;
    SS().prefix = String(pfx);
    SLog.printf("[STORAGE] Set to %s, prefix=\"%s\"\n",
        (fs == &LittleFS) ? "LittleFS" : "SD", pfx);
    recover_tmp_files(SS().storage, messages_dir(SS().prefix));
}
fs::FS*       storage() { return SS().storage; }
const String& prefix()  { return SS().prefix; }

void set_namespace(const char* ns) {
    SS().ns = String(ns ? ns : "");
    SLog.printf("[STORAGE] store namespace: \"%s\"\n", SS().ns.c_str());
}
const String& ns() { return SS().ns; }

String messages_dir_path()                      { return messages_dir(SS().prefix); }
String channel_msg_path_for(const char* ch_name){ return channel_msg_path(SS().prefix, ch_name); }
String dm_msg_path_for(const char* peer)        { return dm_msg_path(SS().prefix, peer); }

void     set_max_messages(int n)        { if (n > 0 && n <= 5000) SS().max_messages = n; }
void     set_retain_days(uint16_t days) { SS().retain_days = days; }
uint16_t retain_days()                  { return SS().retain_days; }
void     prune_mark_due()               { SS().prune_due = true; }

// ── Unread counters ──────────────────────────────────────────────────────────
// Name-keyed both kinds (the idx-keyed MeshCore surface translates in
// punkmesh.cpp). NO internal lock — callers hold MESH_LOCK (see mesh_store.h).

static MStoreState::Unread* unread_find(MStoreState::Unread* arr, int n,
                                        const char* name, bool alloc) {
    if (!name || !name[0]) return nullptr;
    int free_slot = -1;
    for (int i = 0; i < n; i++) {
        if (arr[i].count > 0 && strcmp(arr[i].name, name) == 0) return &arr[i];
        if (free_slot < 0 && arr[i].count == 0) free_slot = i;
    }
    if (!alloc || free_slot < 0) return nullptr;
    strncpy(arr[free_slot].name, name, sizeof(arr[free_slot].name) - 1);
    arr[free_slot].name[sizeof(arr[free_slot].name) - 1] = '\0';
    arr[free_slot].count = 0;
    return &arr[free_slot];
}

void unread_bump_channel(const char* ch_name) {
    MStoreState::Unread* u = unread_find(SS().unread_ch, MSTORE_UNREAD_CH_SLOTS, ch_name, true);
    if (u && u->count < 0xFFFF) u->count++;
}
void unread_bump_dm(const char* name) {
    MStoreState::Unread* u = unread_find(SS().unread_dm, MSTORE_UNREAD_DM_SLOTS, name, true);
    if (u && u->count < 0xFFFF) u->count++;
}
void unread_clear_channel(const char* ch_name) {
    MStoreState::Unread* u = unread_find(SS().unread_ch, MSTORE_UNREAD_CH_SLOTS, ch_name, false);
    if (u) u->count = 0;
}
void unread_clear_dm(const char* name) {
    MStoreState::Unread* u = unread_find(SS().unread_dm, MSTORE_UNREAD_DM_SLOTS, name, false);
    if (u) u->count = 0;
}
uint16_t unread_channel(const char* ch_name) {
    MStoreState::Unread* u = unread_find(SS().unread_ch, MSTORE_UNREAD_CH_SLOTS, ch_name, false);
    return u ? u->count : 0;
}
uint16_t unread_dm(const char* name) {
    MStoreState::Unread* u = unread_find(SS().unread_dm, MSTORE_UNREAD_DM_SLOTS, name, false);
    return u ? u->count : 0;
}
uint32_t unread_total() {
    uint32_t total = 0;
    for (int i = 0; i < MSTORE_UNREAD_CH_SLOTS; i++) total += SS().unread_ch[i].count;
    for (int i = 0; i < MSTORE_UNREAD_DM_SLOTS; i++) total += SS().unread_dm[i].count;
    return total;
}

// ── Write paths ──────────────────────────────────────────────────────────────

void append_channel_message(const char* ch_name, int channel_idx,
                            const char* from, const char* text,
                            uint32_t timestamp, float snr, float rssi,
                            uint8_t hops, bool direct,
                            uint16_t path_len, const uint8_t* path,
                            const uint8_t* pkt_hash, uint32_t sender_ts) {
    if (!ch_name || !ch_name[0] || channel_idx < 0) return;
    StoredMsg m;
    fill_stored_msg(m, channel_idx, from, /*peer*/ "", text,
                    timestamp, snr, rssi, hops, direct, /*is_dm*/ false,
                    path_len, path, pkt_hash, /*pub_key*/ nullptr, sender_ts);
    append_msg_text(SS().storage, SS().prefix,
                    channel_msg_path(SS().prefix, ch_name),
                    m, SS().max_messages);
    // Compact routing projection for meshprint/replay (sender-indexed, daily).
    if (append_routing_record(SS().storage, SS().prefix, m)) SS().prune_due = true;
}

void append_dm_message(const char* peer, const char* from, const char* text,
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
    append_msg_text(SS().storage, SS().prefix,
                    dm_msg_path(SS().prefix, peer),
                    m, SS().max_messages);
}

void append_extra_path(const String& msg_log_path, const uint8_t* hash,
                       const ObservedPath& op) {
    append_path_record(SS().storage, msg_log_path, hash, op);
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
int push_routing_query(lua_State* L, const char* sender,
                               uint32_t since_ts, uint32_t until_ts) {
    lua_newtable(L);
    if (!SS().storage) return 1;
    bool is_sd = (SS().storage != &LittleFS);

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
    String dir = route_dir(SS().prefix);
    File root = SS().storage->open(dir.c_str());
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
                        File xf = SS().storage->open(idxpath.c_str(), "r");
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
//
// The Lua pushes happen AFTER the walk, with the SD lock released and the dir
// handle closed — a lua_pushstring longjmp on true OOM must not strand the SPI
// lock (mesh-task deadlock). The name buffer leaking on that path is accepted.
int push_routing_senders(lua_State* L, const char* query, int max) {
    if (!SS().storage) {
        lua_newtable(L);
        return 1;
    }
    bool is_sd = (SS().storage != &LittleFS);

    char want[32] = {0};
    bool filter = query && query[0];
    if (filter) { strncpy(want, query, sizeof(want) - 1); str_tolower_buf(want); }

    if (max <= 0 || max > 128) max = 64;
    const int NAMESZ = 32;
    // One block, two halves: [0..max) lowercased dedup set, [max..2*max)
    // original-case names for the deferred push phase.
    char* seen = (char*)malloc((size_t)max * NAMESZ * 2);
    if (!seen) {
        lua_newtable(L);
        return 1;
    }
    char* orig = seen + (size_t)max * NAMESZ;
    int nseen = 0;

    if (is_sd) sd_spi_take();
    String dir = route_dir(SS().prefix);
    File root = SS().storage->open(dir.c_str());
    if (!root || !root.isDirectory()) {
        if (root) root.close();
        if (is_sd) sd_spi_release();
        free(seen);
        lua_newtable(L);
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
                    strncpy(orig + (size_t)nseen * NAMESZ, fn, NAMESZ - 1);
                    orig[(size_t)nseen * NAMESZ + NAMESZ - 1] = '\0';
                    nseen++;
                }
            }
        }
        entry = root.openNextFile();   // drop this file before the next
    }
    root.close();
    if (is_sd) sd_spi_release();

    lua_newtable(L);
    for (int i = 0; i < nseen; i++) {
        lua_pushstring(L, orig + (size_t)i * NAMESZ);   // original case for display
        lua_rawseti(L, -2, i + 1);
    }
    free(seen);
    return 1;
}

// Incremental retention sweep, driven by SS().prune_due (set on a new-day routing
// record or at boot). Called from the Core-0 main loop. Phase 1 (first call
// after the flag) deletes old routing day-files and snapshots the text-log list;
// each later call rewrites ONE text log. Every disk step takes its own
// MESH_LOCK+SPI and releases it before returning, so the radio and UI are never
// blocked for more than a single file's rewrite (bounded by the size-safety cap).
void prune_step(uint32_t now_ts) {
    if (!SS().sweep_active) {
        if (!SS().prune_due) return;
        SS().prune_due = false;
        if (!SS().storage || SS().retain_days == 0) return;  // unlimited: nothing to do
        // now_ts is supplied by the caller (device clock authority).
        if (now_ts < 86400) { SS().prune_due = true; return; }  // no clock yet — retry later
        SS().sweep_cutoff = (now_ts > (uint32_t)SS().retain_days * 86400u)
                        ? now_ts - (uint32_t)SS().retain_days * 86400u : 0;
        if (SS().sweep_cutoff == 0) return;
        bool is_sd = (SS().storage != &LittleFS);
        MESH_LOCK();
        // Guard INSIDE MESH_LOCK — lock order everywhere is MESH_LOCK, then
        // the flash guard (the mesh-task append paths already take the guard
        // under MESH_LOCK; taking them in the other order here would deadlock).
        {
            UsbFlashGuardIf _g(!is_sd);   // LittleFS backend: deletes write flash
            if (is_sd) sd_spi_take();
            prune_routing_logs(SS().storage, SS().prefix, SS().sweep_cutoff);  // quick deletes
            SS().sweep_count = collect_message_logs(SS().storage, SS().prefix, SS().sweep_files, 64);
            if (is_sd) sd_spi_release();
        }
        MESH_UNLOCK();
        SS().sweep_idx = 0;
        SS().sweep_active = (SS().sweep_count > 0);
        return;
    }
    if (SS().sweep_idx >= SS().sweep_count) { SS().sweep_active = false; return; }
    bool is_sd = (SS().storage != &LittleFS);
    MESH_LOCK();
    {
        // Same lock order as above: MESH_LOCK first, flash guard second.
        UsbFlashGuardIf _g(!is_sd);   // LittleFS backend: rewrite writes flash
        if (is_sd) sd_spi_take();
        prune_msg_file_by_age(SS().storage, SS().sweep_files[SS().sweep_idx], SS().sweep_cutoff);
        if (is_sd) sd_spi_release();
    }
    MESH_UNLOCK();
    SS().sweep_files[SS().sweep_idx] = String();   // free the path now
    SS().sweep_idx++;
    if (SS().sweep_idx >= SS().sweep_count) SS().sweep_active = false;
}

// Push one {path, hops, direct, snr, rssi} table for an observed route of a
// message. Leaves the table on the stack. Shared by push_stored_msg_table (for
// inline rpaths) and the .paths-sidecar join in read_msg_text_file.
static void push_one_rpath_entry(lua_State* L, const ObservedPath& rp) {
    lua_newtable(L);
    push_path_table(L, rp.path_len, rp.path);
    lua_setfield(L, -2, "path");
    lua_pushinteger(L, rp.path_len & 63); lua_setfield(L, -2, "hops");
    lua_pushboolean(L, rp.is_direct);     lua_setfield(L, -2, "direct");
    lua_pushnumber(L, rp.snr);            lua_setfield(L, -2, "snr");
    lua_pushnumber(L, rp.rssi);           lua_setfield(L, -2, "rssi");
}

static void push_stored_msg_table(lua_State* L, const StoredMsg& m) {
    lua_newtable(L);
    lua_pushstring(L, m.from);      lua_setfield(L, -2, "from");
    lua_pushstring(L, m.peer);      lua_setfield(L, -2, "peer");
    {   // UI space carries composed (PUA) text; the log files keep real Unicode
        char * comp = emoji_compose(m.text);
        lua_pushstring(L, comp ? comp : m.text);
        if (comp) free(comp);
    }
    lua_setfield(L, -2, "text");
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
        char hex[MSTORE_HASH_SIZE * 2 + 1];
        mstore_to_hex(hex, m.pkt_hash, MSTORE_HASH_SIZE);
        lua_pushstring(L, hex);
        lua_setfield(L, -2, "hash");
    }
    if (m.rpath_count > 0) {
        lua_newtable(L);
        for (int i = 0; i < m.rpath_count; i++) {
            push_one_rpath_entry(L, m.rpaths[i]);
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
        if ((size_t)(count + 1) * hash_size > MSTORE_PATH_MAX) break;
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

// Parse a "<pathhex>;snr;rssi;direct" value (mutates the buffer) into rp.
// Returns false if there's no field separator. Shared by the inline rpath= key
// and the .paths sidecar.
static bool parse_rpath_value(char* val, ObservedPath& rp) {
    char* semi1 = strchr(val, ';');
    if (!semi1) return false;
    *semi1 = '\0';
    memset(&rp, 0, sizeof(rp));
    rp.path_len = parse_path_field(val, rp.path);
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
    return true;
}

// Parse one "key=value" record line into m. Mutates `line` (the '=' is cut).
// Lines without '=' are ignored; the "---" record terminator is handled by the
// callers. Shared by the full-file reader (read_msg_text_file) and the
// summary scanner (scan_msg_records).
static void parse_msg_line(char* line, StoredMsg& m) {
    char* eq = strchr(line, '=');
    if (!eq) return;
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
        if (strlen(val) == MSTORE_HASH_SIZE * 2) {
            mstore_from_hex(m.pkt_hash, MSTORE_HASH_SIZE, val);
            m.has_hash = true;
        }
    }
    else if (strcmp(key, "pubkey") == 0 && strlen(val) == 12) {
        mstore_from_hex(m.sender_pub_key, 6, val);
        m.has_pub_key = true;
    }
    else if (strcmp(key, "lat") == 0) { m.lat = atof(val); m.has_loc = true; }
    else if (strcmp(key, "lon") == 0) { m.lon = atof(val); m.has_loc = true; }
    else if (strcmp(key, "rpath") == 0 && m.rpath_count < MSTORE_RPATHS_MAX) {
        // Inline (legacy) extra path: "hop_hashes;snr;rssi;direct"
        char rval[256];
        strncpy(rval, val, sizeof(rval) - 1);
        rval[sizeof(rval) - 1] = '\0';
        if (parse_rpath_value(rval, m.rpaths[m.rpath_count])) m.rpath_count++;
    }
}

// max_records 0 = read everything; N > 0 = only the newest N records reach Lua
// (count-then-skip: pass 1 counts "---" terminators, pass 2 skips the surplus
// records without parsing). Materializing a whole multi-thousand-record log as
// Lua tables can exceed the Lua arena and silently spill into the shared PSRAM
// heap (lua_spill), fragmenting it — the tail window keeps the transient bounded.
static int read_msg_text_file(lua_State* L, fs::FS* storage, const String& fpath,
                              int max_records) {
    lua_newtable(L);
    int msgs_idx = lua_gettop(L);
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

    // Pass 1 (only when windowing): count complete records, then rewind. Uses
    // the same terminator test as the parse pass below, so count and skip can
    // never disagree on what a record boundary is. No parsing, no Lua here.
    int skip = 0;
    if (max_records > 0) {
        int total = 0, clc = 0, clen;
        char cline[8];   // terminator detection only; longer lines report >3 anyway
        BlockLineReader cr(&f);
        while ((clen = cr.next(cline, sizeof(cline))) >= 0) {
            if (is_sd && ++clc % 100 == 0) {
                sd_spi_release();
                vTaskDelay(1);
                sd_spi_take();
            }
            if (clen == 3 && cline[0] == '-' && cline[1] == '-' && cline[2] == '-') total++;
        }
        if (total > max_records) skip = total - max_records;
        f.seek(0);
    }

    // Only build the hash-hex -> message lookup (used to join the .paths sidecar
    // of extra routes back to each message) when a sidecar actually exists — most
    // conversations have none, so skip both the table and the per-record insert.
    String spath = paths_sidecar_for(fpath);
    bool have_sidecar = storage->exists(spath.c_str());
    int lookup_idx = 0;
    if (have_sidecar) {
        lua_newtable(L);
        lookup_idx = lua_gettop(L);
    }

    int idx = 1;
    int line_count = 0;
    StoredMsg m;
    memset(&m, 0, sizeof(m));
    char line[256];

    BlockLineReader lr(&f);   // fresh reader: pass 1 left its buffer stale
    int len;
    while ((len = lr.next(line, sizeof(line))) >= 0) {
        if (len == 0) continue;

        // Yield every 100 lines so a large log can't trip the task watchdog.
        if (is_sd && ++line_count % 100 == 0) {
            sd_spi_release();
            vTaskDelay(1);
            sd_spi_take();
        }

        // Tail window: fast-skip the surplus head records (no parsing). The
        // terminator that takes skip to 0 ends the last skipped record, so
        // parsing starts at the first line of the first kept record.
        if (skip > 0) {
            if (len == 3 && line[0] == '-' && line[1] == '-' && line[2] == '-') skip--;
            continue;
        }

        if (len == 3 && line[0] == '-' && line[1] == '-' && line[2] == '-') {
            push_stored_msg_table(L, m);                 // msgtbl on top
            if (have_sidecar && m.has_hash) {
                char hx[MSTORE_HASH_SIZE * 2 + 1];
                mstore_to_hex(hx, m.pkt_hash, MSTORE_HASH_SIZE);
                lua_pushvalue(L, -1);                    // dup msgtbl
                lua_setfield(L, lookup_idx, hx);         // lookup[hx] = msgtbl
            }
            lua_rawseti(L, msgs_idx, idx++);             // msgs[idx] = msgtbl
            memset(&m, 0, sizeof(m));
            continue;
        }

        parse_msg_line(line, m);
    }
    f.close();

    // Join the .paths sidecar: each "<hash_hex> <pathhex>;snr;rssi;direct" line
    // appends one observed route to the matching message's rpaths.
    if (have_sidecar) {
        File sf = storage->open(spath.c_str(), "r");
        if (sf) {
            int slc = 0;
            char sline[256];
            BlockLineReader slr(&sf);
            int slen;
            while ((slen = slr.next(sline, sizeof(sline))) >= 0) {
                if (slen == 0) continue;
                if (is_sd && ++slc % 100 == 0) {
                    sd_spi_release(); vTaskDelay(1); sd_spi_take();
                }
                char* sp = strchr(sline, ' ');
                if (!sp) continue;
                *sp = '\0';
                if (strlen(sline) != MSTORE_HASH_SIZE * 2) continue;   // sline = hash hex
                lua_getfield(L, lookup_idx, sline);                 // msgtbl | nil
                if (lua_istable(L, -1)) {
                    ObservedPath rp;
                    if (parse_rpath_value(sp + 1, rp)) {
                        lua_getfield(L, -1, "rpaths");              // msgtbl, rpaths|nil
                        if (!lua_istable(L, -1)) {
                            lua_pop(L, 1);
                            lua_newtable(L);                        // msgtbl, rpaths
                            lua_pushvalue(L, -1);
                            lua_setfield(L, -3, "rpaths");          // msgtbl.rpaths = rpaths
                        }
                        int n = (int)lua_rawlen(L, -1);
                        if (n < MSTORE_RPATHS_MAX) {
                            push_one_rpath_entry(L, rp);            // msgtbl, rpaths, entry
                            lua_rawseti(L, -2, n + 1);
                        }
                        lua_pop(L, 1);                              // pop rpaths
                    }
                }
                lua_pop(L, 1);                                      // pop msgtbl|nil
            }
            sf.close();
        }
    }

    if (is_sd) sd_spi_release();
    if (have_sidecar) lua_remove(L, lookup_idx);   // drop lookup; leaves msgs on top
    return 1;
}

// Call WITHOUT MESH_LOCK held: the file read + Lua pushes run unlocked — a
// lua_push longjmp on true OOM must not strand the lock (mesh-task deadlock),
// and a multi-second read of a large log must not stall the radio. Channel
// name resolution (the one thing that needed the lock) happens in the caller.
int push_channel_messages(lua_State* L, const char* ch_name, int max_records) {
    if (!ch_name || !ch_name[0]) { lua_newtable(L); return 1; }
    return read_msg_text_file(L, SS().storage,
                              channel_msg_path(SS().prefix, ch_name),
                              max_records);
}

int push_dm_messages(lua_State* L, const char* peer, int max_records) {
    if (!peer || peer[0] == '\0') { lua_newtable(L); return 1; }
    return read_msg_text_file(L, SS().storage, dm_msg_path(SS().prefix, peer),
                              max_records);
}

// Enumerate dm_*.log files and return an array of the real peer names
// (recovered from the `peer` field stored inside the file, not the
// sanitized filename).
int push_dm_thread_names(lua_State* L) {
    lua_newtable(L);
    if (!SS().storage) return 1;

    bool is_sd = (SS().storage != &LittleFS);
    if (is_sd) sd_spi_take();

    String dir = messages_dir(SS().prefix);
    File root = SS().storage->open(dir.c_str());
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

// Single-pass summary scan of one open message log: record count + a copy of
// the LAST record. peer_out (nullable) receives the thread's real peer name —
// the first non-empty peer= field seen (DM filenames are sanitized, so the
// name must come from the records). Shares parse_msg_line with the full
// reader but allocates nothing in Lua, so the inbox can list every
// conversation without materializing histories. Caller holds the SD lock when
// the file is on SD; like the full reader, it's released/retaken every 100
// lines so a big log can't starve the bus or trip the watchdog.
// Returns true when the file held at least one complete record.
static bool scan_msg_records(File& f, bool is_sd, StoredMsg& last, int& count,
                             char* peer_out, size_t peer_sz) {
    count = 0;
    if (peer_out && peer_sz) peer_out[0] = '\0';

    StoredMsg m;
    memset(&m, 0, sizeof(m));
    char line[256];
    int line_count = 0;

    BlockLineReader lr(&f);
    int len;
    while ((len = lr.next(line, sizeof(line))) >= 0) {
        if (len == 0) continue;
        if (is_sd && ++line_count % 100 == 0) {
            sd_spi_release();
            vTaskDelay(1);
            sd_spi_take();
        }
        if (len == 3 && line[0] == '-' && line[1] == '-' && line[2] == '-') {
            if (peer_out && peer_sz && peer_out[0] == '\0' && m.peer[0] != '\0') {
                strncpy(peer_out, m.peer, peer_sz - 1);
                peer_out[peer_sz - 1] = '\0';
            }
            last = m;   // struct copy; cheaper than re-seeking the tail
            count++;
            memset(&m, 0, sizeof(m));
            continue;
        }
        parse_msg_line(line, m);
    }
    return count > 0;
}

// One lightweight summary entry per stored conversation, for the Messenger
// inbox:
//   { kind="channel", idx=N, name=..., count=N, last=<msg table> }
//   { kind="dm",      name=peer,       count=N, last=<msg table> }
// Only count + last record are read per log (scan_msg_records) instead of
// materializing whole histories in Lua; the full history of ONE conversation
// loads on demand when its chat opens (pushChannel/DMMessagesToLua).
//
// Two-phase: phase A gathers every summary into a transient PSRAM array with
// the SD lock held (MESH_LOCK only for the channel-table snapshot); phase B
// builds the Lua tables holding NO locks. lua_push* can longjmp on a true OOM,
// and an escape with the SPI lock held would stall the mesh task forever — the
// transient array leaking on that path is the acceptable trade.
// The channel list comes from the caller (built from the active protocol's
// channel table under that protocol's lock) so the store never touches it.
int push_msg_summaries(lua_State* L, const MStoreChanRef* chans, int nch) {
    if (!SS().storage) {
        lua_newtable(L);
        return 1;
    }

    struct SumRec {
        bool is_dm;
        int idx;
        char name[32];
        int count;
        StoredMsg last;
    };
    // Channel cap + a generous DM-thread cap; ~1KB/record, transient.
    const int SUM_MAX = 96;
    SumRec* recs = (SumRec*)heap_caps_malloc(sizeof(SumRec) * SUM_MAX,
                                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!recs) {
        lua_newtable(L);
        return 1;
    }
    int nrec = 0;

    // ── Phase A: file I/O only, no Lua calls ──
    bool is_sd = (SS().storage != &LittleFS);
    if (is_sd) sd_spi_take();

    StoredMsg last;
    int count;
    for (int c = 0; c < nch && nrec < SUM_MAX; c++) {
        String path = channel_msg_path(SS().prefix, chans[c].name);
        if (!SS().storage->exists(path.c_str())) continue;
        File f = SS().storage->open(path.c_str(), "r");
        if (!f) continue;
        bool ok = scan_msg_records(f, is_sd, last, count, nullptr, 0);
        f.close();
        if (!ok) continue;
        SumRec& r = recs[nrec++];
        r.is_dm = false;
        r.idx = chans[c].idx;
        strncpy(r.name, chans[c].name, sizeof(r.name) - 1);
        r.name[sizeof(r.name) - 1] = '\0';
        r.count = count;
        r.last = last;
    }

    // DM logs: directory pass (same shape as push_dm_thread_names), scanning
    // each iterated entry handle directly — no re-open by path.
    String dir = messages_dir(SS().prefix);
    File root = SS().storage->open(dir.c_str());
    if (root && root.isDirectory()) {
        int iter = 0;
        File entry = root.openNextFile();
        while (entry) {
            if (nrec >= SUM_MAX) {
                SLog.printf("[MSG] summary cap (%d) hit - remaining DM threads skipped\n", SUM_MAX);
                break;
            }
            if (!entry.isDirectory()) {
                String name = entry.name();
                int slash = name.lastIndexOf('/');
                String base = (slash >= 0) ? name.substring(slash + 1) : name;
                if (base.startsWith("dm_") && base.endsWith(".log")) {
                    char peer[32];
                    if (scan_msg_records(entry, is_sd, last, count,
                                         peer, sizeof(peer)) && peer[0] != '\0') {
                        SumRec& r = recs[nrec++];
                        r.is_dm = true;
                        r.idx = -1;
                        strncpy(r.name, peer, sizeof(r.name) - 1);
                        r.name[sizeof(r.name) - 1] = '\0';
                        r.count = count;
                        r.last = last;
                    }
                }
            }
            if (is_sd && ++iter % 10 == 0) {
                sd_spi_release();
                vTaskDelay(1);
                sd_spi_take();
            }
            entry = root.openNextFile();
        }
        root.close();
    } else if (root) {
        root.close();
    }

    if (is_sd) sd_spi_release();

    // ── Phase B: build the Lua array, nothing held ──
    lua_newtable(L);
    for (int i = 0; i < nrec; i++) {
        const SumRec& r = recs[i];
        lua_newtable(L);
        lua_pushstring(L, r.is_dm ? "dm" : "channel"); lua_setfield(L, -2, "kind");
        if (!r.is_dm) {
            lua_pushinteger(L, r.idx);                 lua_setfield(L, -2, "idx");
        }
        lua_pushstring(L, r.name);                     lua_setfield(L, -2, "name");
        lua_pushinteger(L, r.count);                   lua_setfield(L, -2, "count");
        push_stored_msg_table(L, r.last);              lua_setfield(L, -2, "last");
        lua_rawseti(L, -2, i + 1);
    }
    heap_caps_free(recs);
    return 1;
}

int enumerate_message_files(MsgFileInfo* out, int max_paths) {
    if (!SS().storage || max_paths <= 0) return 0;
    bool is_sd = (SS().storage != &LittleFS);
    if (is_sd) sd_spi_take();

    String dir = messages_dir(SS().prefix);
    File root = SS().storage->open(dir.c_str());
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
                strncpy(out[count].path, full.c_str(), (int)sizeof(out[0].path) - 1);
                out[count].path[(int)sizeof(out[0].path) - 1] = '\0';
                out[count].size = (uint32_t)entry.size();
                count++;
            }
        }
        entry = root.openNextFile();
    }
    root.close();
    if (is_sd) sd_spi_release();
    return count;
}

// Shared key=value field mapping for stored-message records (used by both
// the byte-wise read_one_record and the block-buffered batch reader).
static void apply_record_field(StoredMsg& m, const char* key, const char* val);

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
        has_data = true;
        apply_record_field(m, line, eq + 1);
    }
    return false;
}

static void apply_record_field(StoredMsg& m, const char* key, const char* val) {
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
            mstore_from_hex(m.sender_pub_key, 6, val);
            m.has_pub_key = true;
        }
}

int read_one_stored_msg(fs::FS* storage, const char* path,
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

int read_all_stored_msgs(const char* path, StoredMsg* out, int max_count) {
    if (!SS().storage || max_count <= 0) return 0;
    bool is_sd = (SS().storage != &LittleFS);
    if (is_sd) sd_spi_take();

    File f = SS().storage->open(path, "r");
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

// Block-buffered line reader that tracks the exact file offset of the next
// unconsumed byte (BlockLineReader drops that information). Offsets returned
// to callers always land on record boundaries.
struct CountingLineReader {
    File*    f;
    uint8_t  buf[512];
    int      len = 0;
    int      pos = 0;
    uint32_t consumed;   // absolute file offset of the next unconsumed byte
    CountingLineReader(File* file, uint32_t start) : f(file), consumed(start) {}
    int next(char* out, int outsz) {
        int n = 0;
        bool saw = false;
        for (;;) {
            if (pos >= len) {
                len = f->read(buf, sizeof(buf));
                pos = 0;
                if (len <= 0) break;             // EOF (or read error)
            }
            saw = true;
            char c = (char)buf[pos++];
            consumed++;
            if (c == '\n') { out[n] = '\0'; return n; }
            if (c == '\r') continue;
            if (n < outsz - 1) out[n++] = c;     // overflow chars dropped
        }
        out[n] = '\0';
        return (saw || n > 0) ? n : -1;          // last line, or -1 at true EOF
    }
};

int read_stored_msgs_from(const char* path, uint32_t start_offset,
                                 StoredMsg* out, uint32_t* end_offsets, int max_count,
                                 uint32_t* next_offset, uint32_t* file_size) {
    *next_offset = start_offset;
    *file_size = 0;
    if (!SS().storage || max_count <= 0) return 0;
    bool is_sd = (SS().storage != &LittleFS);
    if (is_sd) sd_spi_take();

    File f = SS().storage->open(path, "r");
    if (!f) {
        if (is_sd) sd_spi_release();
        return 0;
    }
    uint32_t fsize = (uint32_t)f.size();
    *file_size = fsize;
    if (start_offset >= fsize) {
        f.close();
        if (is_sd) sd_spi_release();
        return 0;   // nothing new (or caller must handle offset > size = compaction)
    }
    f.seek(start_offset);

    CountingLineReader lr(&f, start_offset);
    char line[256];
    StoredMsg m;
    memset(&m, 0, sizeof(m));
    bool has_data = false;
    int count = 0, parsed = 0, llen;

    // Parse at most max_count COMPLETE records per call (keeps every call
    // bounded even when none of them yield a record).
    while (parsed < max_count && (llen = lr.next(line, sizeof(line))) >= 0) {
        if (llen == 0) continue;

        if (llen == 3 && line[0] == '-' && line[1] == '-' && line[2] == '-') {
            parsed++;
            *next_offset = lr.consumed;          // record boundary
            if (has_data) {
                out[count] = m;
                end_offsets[count] = lr.consumed;
                count++;
            }
            memset(&m, 0, sizeof(m));
            has_data = false;
            if (count >= max_count) break;
            continue;
        }

        char* eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        has_data = true;
        apply_record_field(m, line, eq + 1);
    }
    // An unterminated tail record (crash mid-append) is never returned and
    // never advances *next_offset — it re-parses once its "---" lands.

    f.close();
    if (is_sd) sd_spi_release();
    return count;
}


// A record is a run of "key=value" lines closed by a lone "---", so record
// starts are only reachable by scanning forward. Both helpers assume the caller
// holds the SD lock and has seeked; they drop it every 100 lines so a whole-file
// scan cannot monopolise the bus.

// Below the smallest real record on purpose: the size gate in
// offsetOfNewestRecords may then scan when it need not, but never skips a trim
// that was due.
static const uint32_t MSG_MIN_REC_BYTES = 24;

static int count_records_to_eof(File& f, bool is_sd, uint32_t from) {
    CountingLineReader lr(&f, from);
    char line[256];
    int len, lc = 0, total = 0;
    while ((len = lr.next(line, sizeof(line))) >= 0) {
        if (is_sd && ++lc % 100 == 0) { sd_spi_release(); vTaskDelay(1); sd_spi_take(); }
        if (len == 3 && line[0] == '-' && line[1] == '-' && line[2] == '-') total++;
    }
    return total;                                  // complete records only
}

// Offset just past the `skip`-th record terminator at or after `from`.
static uint32_t skip_records(File& f, bool is_sd, uint32_t from, int skip) {
    CountingLineReader lr(&f, from);
    char line[256];
    int len, lc = 0, seen = 0;
    while (seen < skip && (len = lr.next(line, sizeof(line))) >= 0) {
        if (is_sd && ++lc % 100 == 0) { sd_spi_release(); vTaskDelay(1); sd_spi_take(); }
        if (len == 3 && line[0] == '-' && line[1] == '-' && line[2] == '-') {
            if (++seen >= skip) return lr.consumed;
        }
    }
    return from;                                   // fewer boundaries than asked
}

uint32_t offset_of_newest_records(const char* path, uint32_t start_offset, int n) {
    if (!SS().storage || n < 1) return start_offset;
    bool is_sd = (SS().storage != &LittleFS);
    if (is_sd) sd_spi_take();

    uint32_t res = start_offset;
    File f = SS().storage->open(path, "r");
    if (f) {
        uint32_t fsize = (uint32_t)f.size();
        // A span this small cannot hold more than n records, so skip the scan:
        // keeps the steady-state sync (a few appended records) free of file work.
        if (fsize > start_offset &&
            (fsize - start_offset) > (uint32_t)n * MSG_MIN_REC_BYTES) {
            f.seek(start_offset);
            int total = count_records_to_eof(f, is_sd, start_offset);
            if (total > n) {
                f.seek(start_offset);
                res = skip_records(f, is_sd, start_offset, total - n);
            }
        }
        f.close();
    }

    if (is_sd) sd_spi_release();
    return res;
}

// ── Chat pager (Messenger sliding-window scroll) ─────────────────────────────
// Pages a conversation log by BYTE OFFSET so the chat view can hold a bounded
// window of message bubbles and slide it as the user scrolls — the whole thread
// never materializes in Lua. Records are variable-length ("key=value\n..\n---\n"),

// so forward paging is O(1) from an offset but backward paging must locate the
// start of the Nth record before a cursor via a forward scan (chat_find_back_start).
#define CHAT_PAGE_MAX 64   // ring/clamp bound for `count` (a page is ~20)

// Start offset of the `count`-th complete record before byte `end_off` (0 = fewer
// than `count` records precede it, so start at the file head). Scans from 0 with a
// ring of record-start offsets; same "---" terminator test as the parser.
static uint32_t chat_find_back_start(File& f, bool is_sd, uint32_t end_off, int count) {
    if (end_off == 0 || count < 1) return 0;
    if (count > CHAT_PAGE_MAX) count = CHAT_PAGE_MAX;
    f.seek(0);
    CountingLineReader lr(&f, 0);
    uint32_t ring[CHAT_PAGE_MAX];
    int rn = 0, rhead = 0;
    uint32_t rec_start = 0;
    char line[256];
    int len, lc = 0;
    while ((len = lr.next(line, sizeof(line))) >= 0) {
        if (is_sd && ++lc % 100 == 0) { sd_spi_release(); vTaskDelay(1); sd_spi_take(); }
        if (len == 3 && line[0] == '-' && line[1] == '-' && line[2] == '-') {
            uint32_t rec_end = lr.consumed;
            if (rec_end > end_off) break;         // this record isn't before end_off
            ring[rhead] = rec_start;
            rhead = (rhead + 1) % count;
            if (rn < count) rn++;
            rec_start = rec_end;                  // next record starts past this "---"
            if (rec_end >= end_off) break;        // reached the boundary exactly
        }
    }
    if (rn < count) return 0;                     // fewer than count records: read from head
    return ring[rhead];                           // full ring: rhead is the oldest entry
}

// Read up to `count` complete records forward from `start_off`, pushing each as a
// msg table (annotated off0/off1) into the array at stack index `list_idx`. Stops
// early if a record ends past `limit_off` (0 = read to EOF). *idx is the running
// 1-based array index. Caller holds the SD lock.
static void chat_read_forward(lua_State* L, int list_idx, int* idx, File& f, bool is_sd,
                              uint32_t start_off, int count, uint32_t limit_off) {
    if (!f.seek(start_off)) return;
    CountingLineReader lr(&f, start_off);
    StoredMsg m;
    memset(&m, 0, sizeof(m));
    uint32_t rec_start = start_off;
    char line[256];
    int len, lc = 0, got = 0;
    while (got < count && (len = lr.next(line, sizeof(line))) >= 0) {
        if (is_sd && ++lc % 100 == 0) { sd_spi_release(); vTaskDelay(1); sd_spi_take(); }
        if (len == 3 && line[0] == '-' && line[1] == '-' && line[2] == '-') {
            uint32_t rec_end = lr.consumed;
            if (limit_off && rec_end > limit_off) break;   // past the requested window
            push_stored_msg_table(L, m);                   // msgtbl on top
            lua_pushinteger(L, (lua_Integer)rec_start); lua_setfield(L, -2, "off0");
            lua_pushinteger(L, (lua_Integer)rec_end);   lua_setfield(L, -2, "off1");
            lua_rawseti(L, list_idx, (*idx)++);
            memset(&m, 0, sizeof(m));
            rec_start = rec_end;
            got++;
            continue;
        }
        parse_msg_line(line, m);
    }
}

// mode 0 tail (newest count) / 1 older (count before cursor) / 2 newer (from cursor).
// Pushes ONE table { list = { <msg w/ off0,off1>, ... }, size = <file bytes> }.
static int read_chat_page(lua_State* L, fs::FS* storage, const String& fpath,
                          int mode, uint32_t cursor, int count) {
    if (count < 1) count = 1;
    if (count > CHAT_PAGE_MAX) count = CHAT_PAGE_MAX;

    lua_newtable(L);                       // list (filled below, index-stable)
    int list_idx = lua_gettop(L);
    int idx = 1;
    uint32_t fsize = 0;

    if (storage) {
        bool is_sd = (storage != &LittleFS);
        if (is_sd) sd_spi_take();
        if (storage->exists(fpath.c_str())) {
            File f = storage->open(fpath.c_str(), "r");
            if (f) {
                fsize = (uint32_t)f.size();
                uint32_t start_off = 0, limit_off = 0;
                bool ok = true;
                if (mode == 1) {                    // older: `count` records before cursor
                    if (cursor == 0) ok = false;
                    else { start_off = chat_find_back_start(f, is_sd, cursor, count); limit_off = cursor; }
                } else if (mode == 2) {             // newer: forward from cursor
                    if (cursor >= fsize) ok = false;
                    else start_off = cursor;
                } else {                            // tail: newest `count`
                    start_off = chat_find_back_start(f, is_sd, fsize, count);
                }
                if (ok) chat_read_forward(L, list_idx, &idx, f, is_sd, start_off, count, limit_off);
                f.close();
            }
        }
        if (is_sd) sd_spi_release();
    }

    lua_newtable(L);                       // result
    lua_pushvalue(L, list_idx);
    lua_setfield(L, -2, "list");
    lua_pushinteger(L, (lua_Integer)fsize);
    lua_setfield(L, -2, "size");
    lua_remove(L, list_idx);               // drop the bare list; leave result on top
    return 1;
}

static int push_empty_page(lua_State* L) {
    lua_newtable(L);                       // result
    lua_newtable(L); lua_setfield(L, -2, "list");
    lua_pushinteger(L, 0); lua_setfield(L, -2, "size");
    return 1;
}

// Call WITHOUT MESH_LOCK held (same contract as push_channel_messages);
// channel name resolution happens in the caller.
int push_chat_page_channel(lua_State* L, const char* ch_name, int mode,
                           uint32_t cursor, int count) {
    if (!ch_name || !ch_name[0]) return push_empty_page(L);
    return read_chat_page(L, SS().storage, channel_msg_path(SS().prefix, ch_name),
                          mode, cursor, count);
}

int push_chat_page_dm(lua_State* L, const char* peer, int mode,
                      uint32_t cursor, int count) {
    if (!peer || peer[0] == '\0') return push_empty_page(L);
    return read_chat_page(L, SS().storage, dm_msg_path(SS().prefix, peer),
                          mode, cursor, count);
}

int lookup_persisted_paths(lua_State* L, const char* hash_hex,
                           const char* ch_name, const char* peer) {
    lua_newtable(L);
    if (!SS().storage || strlen(hash_hex) != MSTORE_HASH_SIZE * 2) return 1;

    String fpath;
    if (ch_name && ch_name[0]) {
        fpath = channel_msg_path(SS().prefix, ch_name);
    } else if (peer && peer[0]) {
        fpath = dm_msg_path(SS().prefix, peer);
    } else {
        return 1;
    }

    bool is_sd = (SS().storage != &LittleFS);
    if (is_sd) sd_spi_take();

    int rpath_idx = 1;

    // 1) The inline first route: scan the message log for this hash's record and
    //    read its rpath= line. Block-buffered (BlockLineReader) and yields every
    //    100 lines, so even a large log scanned to its end can't starve the UI
    //    thread's watchdog.
    File f = SS().storage->open(fpath.c_str(), "r");
    if (f) {
        char target[6 + MSTORE_HASH_SIZE * 2 + 1];
        snprintf(target, sizeof(target), "hash=%s", hash_hex);
        BlockLineReader lr(&f);
        char line[256];
        bool found_hash = false;
        int line_count = 0, len;
        while ((len = lr.next(line, sizeof(line))) >= 0) {
            if (len == 0) continue;
            if (is_sd && ++line_count % 100 == 0) {
                sd_spi_release(); vTaskDelay(1); sd_spi_take();
            }
            if (found_hash) {
                if (len == 3 && line[0] == '-' && line[1] == '-' && line[2] == '-')
                    break;  // end of matching record
                if (strncmp(line, "rpath=", 6) == 0) {
                    ObservedPath rp;
                    char rval[256];
                    strncpy(rval, line + 6, sizeof(rval) - 1);
                    rval[sizeof(rval) - 1] = '\0';
                    if (parse_rpath_value(rval, rp)) {
                        push_one_rpath_entry(L, rp);
                        lua_rawseti(L, -2, rpath_idx++);
                    }
                }
            } else if (strcmp(line, target) == 0) {
                found_hash = true;
            }
        }
        f.close();
    }

    // 2) Extra routes from the .paths sidecar (where repeats heard via new routes
    //    are now recorded). Each line is "<hash_hex> <pathhex>;snr;rssi;direct";
    //    pick the ones keyed by this hash. Yields on the same cadence.
    String spath = paths_sidecar_for(fpath);
    if (SS().storage->exists(spath.c_str())) {
        File sf = SS().storage->open(spath.c_str(), "r");
        if (sf) {
            size_t hlen = strlen(hash_hex);
            BlockLineReader lr(&sf);
            char sline[256];
            int slc = 0, slen;
            while ((slen = lr.next(sline, sizeof(sline))) >= 0) {
                if (slen == 0) continue;
                if (is_sd && ++slc % 100 == 0) {
                    sd_spi_release(); vTaskDelay(1); sd_spi_take();
                }
                // Match "<hash_hex> ..." (our hash, then a single space).
                if ((size_t)slen <= hlen || sline[hlen] != ' ') continue;
                if (strncmp(sline, hash_hex, hlen) != 0) continue;
                ObservedPath rp;
                if (parse_rpath_value(sline + hlen + 1, rp)) {
                    push_one_rpath_entry(L, rp);
                    lua_rawseti(L, -2, rpath_idx++);
                }
            }
            sf.close();
        }
    }

    if (is_sd) sd_spi_release();
    return 1;
}

}  // namespace mstore
