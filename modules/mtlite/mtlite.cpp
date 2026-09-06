// mtlite.cpp — Meshtastic-compatible LoRa protocol module (.loraproto.elf).
//
// Wire/crypto/protobuf layer: vendor/ (meshtastic-lite, BSD-3-Clause, built
// WITHOUT MESH_CRYPTO_USE_MBEDTLS — its software-fallback externs resolve to
// the firmware's proto_crypto.cpp via proto_exports[]). This file is the node
// layer the vendor deliberately doesn't ship: NodeDB, TX queue with CSMA
// delay, periodic NodeInfo broadcast, config + PKI-key persistence, and the
// LoraProtoOps glue over the polled radio HAL.
//
// Router layer: dedup ring -> want_ack ACKs -> role-gated rebroadcast
// (ROLE_CLIENT relays with hop_limit-1; ROLE_CLIENT_MUTE never does). DMs are
// PKI with a 3-attempt retry ladder surfaced as the dm_status config key.
//
// Threading (ABI contract): init/start on the boot core; loop() on mesh_task
// under MESH_LOCK; send_*/get_*/set_config on Core 0 under MESH_LOCK.

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "lora_proto_abi.h"

// Firmware Lua headers (ABI v2 lua_open): same luaconf as the host (LUA_32BITS
// is baked into lib/lua/luaconf.h); the lua_* API resolves via proto_exports[].
extern "C" {
#include <lua.h>
#include <lauxlib.h>
}

#include "vendor/meshtastic.h"

// SX126x IRQ register bits (RadioLib RADIOLIB_SX126X_IRQ_*; the HAL's
// poll_irq returns the chip's IRQ status register verbatim).
#define IRQ_TX_DONE 0x0001u
#define IRQ_RX_DONE 0x0002u

static const MeshHostApi* H = nullptr;
static char s_dir[128];    // install dir (internal, always present)
static char s_data[128];   // data home on the CHOSEN mesh storage (H->data_dir):
                           // peers/config + the identity copy live here so they
                           // survive reflashes, the MeshCore contacts rule

// ── Config (persisted as key=value in <dir>/cfg) ─────────────────────────────
static struct {
    uint8_t region;        // MeshRegion
    uint8_t preset;        // MeshModemPreset
    uint8_t role;          // MeshRole: 0 CLIENT (relays, official default), 1 CLIENT_MUTE
    uint8_t pos_precision; // position precision bits kept: 32 exact, 10..16
                           // blurred (official channel semantics), 0 = never
                           // send position. Default 13 (~3 km) like official.
    uint8_t hop_limit;     // 1..7
    int8_t  tx_power;      // radiated dBm request (clamped by region + board)
    int16_t freq_slot;     // 0 = auto (hash of primary name), 1..N = that slot
                           // (1-based, the Meshtastic channel_num semantics)
    float   freq_override; // explicit center MHz; 0 = off. Wins over the slot.
    uint8_t ok_to_mqtt;    // bitfield bit 0 on outgoing packets: MQTT bridges
                           // may uplink them (0 = the private default)
    uint16_t nodeinfo_mins; // NodeInfo broadcast period, minutes
    uint16_t pos_mins;      // Position broadcast period, minutes (precision 0
                            // still silences position entirely)
    char    long_name[40];
    char    short_name[5];
} CFG;

static void cfg_defaults() {
    memset(&CFG, 0, sizeof(CFG));
    CFG.region    = REGION_US;
    CFG.preset    = MODEM_LONG_FAST;
    CFG.role      = ROLE_CLIENT;
    CFG.pos_precision = 13;
    CFG.hop_limit = MESH_HOP_RELIABLE;
    CFG.tx_power  = 20;
    CFG.freq_slot = 0;
    CFG.freq_override = 0.0f;
    CFG.ok_to_mqtt = 0;
    // Stock Meshtastic cadence (firmware Default.h: node info 3 h; position
    // rides default_broadcast_interval_secs = 1 h for non-routers).
    CFG.nodeinfo_mins = 180;
    CFG.pos_mins = 60;
    strcpy(CFG.long_name, "Meshpunk");
    strcpy(CFG.short_name, "MPK");
}

static void cfg_load() {
    cfg_defaults();
    char path[160];
    snprintf(path, sizeof(path), "%s/cfg", s_data);
    FILE* f = fopen(path, "r");
    if (!f) {
        // Pre-data-home installs kept cfg in the install dir; read it once —
        // the next cfg_save writes the new location.
        snprintf(path, sizeof(path), "%s/cfg", s_dir);
        f = fopen(path, "r");
    }
    if (!f) return;
    char line[96];
    while (fgets(line, sizeof(line), f)) {
        char* nl = strchr(line, '\n'); if (nl) *nl = 0;
        char* cr = strchr(line, '\r'); if (cr) *cr = 0;
        char* eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        const char* k = line;
        const char* v = eq + 1;
        if      (!strcmp(k, "region") && atoi(v) >= 0 && atoi(v) < REGION_COUNT) CFG.region = (uint8_t)atoi(v);
        else if (!strcmp(k, "preset") && atoi(v) >= 0 && atoi(v) < MODEM_PRESET_COUNT) CFG.preset = (uint8_t)atoi(v);
        else if (!strcmp(k, "role")) { int n = atoi(v); if (n >= 0 && n <= 2) CFG.role = (uint8_t)n; }
        else if (!strcmp(k, "pos_precision")) { int n = atoi(v); if (n == 0 || (n >= 10 && n <= 32)) CFG.pos_precision = (uint8_t)n; }
        else if (!strcmp(k, "hop_limit")) { int n = atoi(v); if (n >= 1 && n <= 7) CFG.hop_limit = (uint8_t)n; }
        else if (!strcmp(k, "tx_power"))  { int n = atoi(v); if (n >= 1 && n <= 30) CFG.tx_power = (int8_t)n; }
        else if (!strcmp(k, "freq_slot")) { int n = atoi(v); if (n >= 0 && n <= 1000) CFG.freq_slot = (int16_t)n; }
        else if (!strcmp(k, "freq_override")) { float x = (float)atof(v); if (x == 0.0f || (x >= 150.0f && x <= 960.0f)) CFG.freq_override = x; }
        else if (!strcmp(k, "ok_to_mqtt")) { int n = atoi(v); if (n == 0 || n == 1) CFG.ok_to_mqtt = (uint8_t)n; }
        else if (!strcmp(k, "nodeinfo_mins")) { int n = atoi(v); if (n >= 5 && n <= 1440) CFG.nodeinfo_mins = (uint16_t)n; }
        else if (!strcmp(k, "pos_mins")) { int n = atoi(v); if (n >= 1 && n <= 1440) CFG.pos_mins = (uint16_t)n; }
        else if (!strcmp(k, "long_name") && v[0])  { strncpy(CFG.long_name, v, sizeof(CFG.long_name) - 1); CFG.long_name[sizeof(CFG.long_name)-1] = 0; }
        else if (!strcmp(k, "short_name") && v[0]) { strncpy(CFG.short_name, v, sizeof(CFG.short_name) - 1); CFG.short_name[sizeof(CFG.short_name)-1] = 0; }
    }
    fclose(f);
}

static void cfg_save() {
    char path[160];
    snprintf(path, sizeof(path), "%s/cfg", s_data);
    FILE* f = fopen(path, "w");
    if (!f) { if (H) H->log("cfg save failed: %s", path); return; }
    fprintf(f, "region=%d\n",     (int)CFG.region);
    fprintf(f, "preset=%d\n",     (int)CFG.preset);
    fprintf(f, "role=%d\n",       (int)CFG.role);
    fprintf(f, "pos_precision=%d\n", (int)CFG.pos_precision);
    fprintf(f, "hop_limit=%d\n",  (int)CFG.hop_limit);
    fprintf(f, "tx_power=%d\n",   (int)CFG.tx_power);
    fprintf(f, "freq_slot=%d\n",  (int)CFG.freq_slot);
    fprintf(f, "freq_override=%.3f\n", (double)CFG.freq_override);
    fprintf(f, "ok_to_mqtt=%d\n", (int)CFG.ok_to_mqtt);
    fprintf(f, "nodeinfo_mins=%d\n", (int)CFG.nodeinfo_mins);
    fprintf(f, "pos_mins=%d\n",   (int)CFG.pos_mins);
    fprintf(f, "long_name=%s\n",  CFG.long_name);
    fprintf(f, "short_name=%s\n", CFG.short_name);
    fclose(f);
}

// ── Per-channel notification modes ───────────────────────────────────────────
// MeshCore's convention (punkmesh.cpp /channel_notify), kept file-for-file:
// "name \t mode" lines in <s_data>/notify, only non-default entries written.
// 0 = off, 1 = @[name] mention only (default), 2 = every message. DMs always
// alert. The unread counter is never gated — a muted channel still accrues.
#define NOTIFY_OFF     0
#define NOTIFY_MENTION 1
#define NOTIFY_ALL     2
#define MAX_NOTIFY_PREFS 12

static struct { char name[40]; uint8_t mode; } s_notify[MAX_NOTIFY_PREFS];
static int s_notify_count = 0;

static void notify_prefs_load(void) {
    s_notify_count = 0;
    char path[160];
    snprintf(path, sizeof(path), "%s/notify", s_data);
    FILE* f = fopen(path, "r");
    if (!f) return;
    char line[64];
    while (fgets(line, sizeof(line), f) && s_notify_count < MAX_NOTIFY_PREFS) {
        char* nl = strchr(line, '\n'); if (nl) *nl = 0;
        char* cr = strchr(line, '\r'); if (cr) *cr = 0;
        char* tab = strchr(line, '\t');
        if (!tab) continue;
        *tab = 0;
        int mode = atoi(tab + 1);
        if (!line[0] || mode < 0 || mode > NOTIFY_ALL) continue;
        if (mode == NOTIFY_MENTION) continue;   // default needs no entry
        strncpy(s_notify[s_notify_count].name, line, sizeof(s_notify[0].name) - 1);
        s_notify[s_notify_count].name[sizeof(s_notify[0].name) - 1] = 0;
        s_notify[s_notify_count].mode = (uint8_t)mode;
        s_notify_count++;
    }
    fclose(f);
}

static void notify_prefs_save(void) {
    char path[160];
    snprintf(path, sizeof(path), "%s/notify", s_data);
    FILE* f = fopen(path, "w");
    if (!f) { if (H) H->log("notify save failed: %s", path); return; }
    for (int i = 0; i < s_notify_count; i++)
        fprintf(f, "%s\t%d\n", s_notify[i].name, s_notify[i].mode);
    fclose(f);
}

static uint8_t notify_mode_get(const char* name) {
    if (!name || !name[0]) return NOTIFY_MENTION;
    for (int i = 0; i < s_notify_count; i++)
        if (!strcmp(s_notify[i].name, name)) return s_notify[i].mode;
    return NOTIFY_MENTION;
}

static bool notify_mode_set(const char* name, int mode) {
    if (!name || !name[0] || mode < 0 || mode > NOTIFY_ALL) return false;
    int found = -1;
    for (int i = 0; i < s_notify_count; i++)
        if (!strcmp(s_notify[i].name, name)) { found = i; break; }
    if (mode == NOTIFY_MENTION) {
        // Default mode = no entry; drop an existing one.
        if (found >= 0) {
            s_notify[found] = s_notify[s_notify_count - 1];
            s_notify_count--;
            notify_prefs_save();
        }
        return true;
    }
    if (found < 0) {
        if (s_notify_count >= MAX_NOTIFY_PREFS) { H->log("notify prefs full"); return false; }
        found = s_notify_count++;
        strncpy(s_notify[found].name, name, sizeof(s_notify[0].name) - 1);
        s_notify[found].name[sizeof(s_notify[0].name) - 1] = 0;
    } else if (s_notify[found].mode == (uint8_t)mode) {
        return true;   // no change, skip the write
    }
    s_notify[found].mode = (uint8_t)mode;
    notify_prefs_save();
    return true;
}

// @[name] anywhere in the text, case-insensitive — the Meshpunk mention
// convention (punkmesh.cpp contains_mention, copied verbatim).
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

// ── Session + NodeDB ─────────────────────────────────────────────────────────
static MeshSession S;

// ── Channels (persisted as <s_data>/channels) ────────────────────────────────
// One line per channel: "<name>\t<psk-hex>". Line 1 = primary; an empty name
// means the preset-named primary (the Meshtastic rule). psk-hex is the RAW
// psk (0/1/16/32 bytes) — meshExpandPsk applies the official expansion at
// add time. The vendor table computes hashes and the RX decrypt walk covers
// every enabled row, so populating the table IS multi-channel support.

static int hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int hex_decode(const char* s, uint8_t* out, int max) {
    int n = 0;
    while (s[0] && s[1] && n < max) {
        int hi = hex_nibble(s[0]), lo = hex_nibble(s[1]);
        if (hi < 0 || lo < 0) return -1;
        out[n++] = (uint8_t)((hi << 4) | lo);
        s += 2;
    }
    return s[0] ? -1 : n;   // odd length or overflow = reject
}

static const char* B64A = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static int b64_val(char c) {
    if (c == '-') return 62;   // base64url alphabet (channel URLs)
    if (c == '_') return 63;
    const char* p = strchr(B64A, c);
    return (p && c) ? (int)(p - B64A) : -1;
}

// Standard base64 (the Meshtastic PSK-sharing form, e.g. "AQ=="). Returns
// decoded length or -1.
static int b64_decode(const char* s, uint8_t* out, int max) {
    int n = 0;
    uint32_t acc = 0;
    int bits = 0, pad = 0;
    for (; *s; s++) {
        if (*s == '=') { pad++; continue; }
        if (pad) return -1;               // data after padding
        int v = b64_val(*s);
        if (v < 0) return -1;
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (n >= max) return -1;
            out[n++] = (uint8_t)(acc >> bits);
        }
    }
    return n;
}

// base64url, unpadded — the meshtastic.org/e/# fragment form.
static void b64url_encode(const uint8_t* in, int len, char* out, int out_sz) {
    static const char* A = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    int o = 0;
    for (int i = 0; i < len && o + 5 < out_sz; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16;
        int have = 1;
        if (i + 1 < len) { v |= (uint32_t)in[i + 1] << 8; have++; }
        if (i + 2 < len) { v |= in[i + 2]; have++; }
        out[o++] = A[(v >> 18) & 63];
        out[o++] = A[(v >> 12) & 63];
        if (have > 1) out[o++] = A[(v >> 6) & 63];
        if (have > 2) out[o++] = A[v & 63];
    }
    out[o] = 0;
}

static void b64_encode(const uint8_t* in, int len, char* out, int out_sz) {
    int o = 0;
    for (int i = 0; i < len && o + 5 < out_sz; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16;
        int have = 1;
        if (i + 1 < len) { v |= (uint32_t)in[i + 1] << 8; have++; }
        if (i + 2 < len) { v |= in[i + 2]; have++; }
        out[o++] = B64A[(v >> 18) & 63];
        out[o++] = B64A[(v >> 12) & 63];
        out[o++] = (have > 1) ? B64A[(v >> 6) & 63] : '=';
        out[o++] = (have > 2) ? B64A[v & 63] : '=';
    }
    out[o] = 0;
}

static void channels_save(void) {
    char path[160];
    snprintf(path, sizeof(path), "%s/channels", s_data);
    FILE* f = fopen(path, "w");
    if (!f) { if (H) H->log("channels save failed: %s", path); return; }
    for (uint8_t i = 0; i < S.channels.count; i++) {
        const MeshChannel* ch = &S.channels.channels[i];
        char hex[70];
        int o = 0;
        for (uint8_t b = 0; b < ch->psk_raw_len && o < (int)sizeof(hex) - 3; b++)
            o += snprintf(hex + o, sizeof(hex) - o, "%02x", ch->psk_raw[b]);
        hex[o] = 0;
        fprintf(f, "%s\t%s\n", ch->name, hex);
    }
    fclose(f);
}

static void channels_load(void) {
    S.channels.init((MeshModemPreset)CFG.preset);
    char path[160];
    snprintf(path, sizeof(path), "%s/channels", s_data);
    FILE* f = fopen(path, "r");
    if (f) {
        char line[112];
        while (fgets(line, sizeof(line), f)) {
            char* nl = strchr(line, '\n'); if (nl) *nl = 0;
            char* cr = strchr(line, '\r'); if (cr) *cr = 0;
            char* tab = strchr(line, '\t');
            if (!tab) continue;
            *tab = 0;
            uint8_t psk[34];
            int pl = hex_decode(tab + 1, psk, (int)sizeof(psk));
            if (pl < 0) { H->log("channels: bad psk hex for '%s' — line skipped", line); continue; }
            S.channels.addChannel(line, psk, (uint8_t)pl, S.channels.count == 0);
        }
        fclose(f);
    }
    if (S.channels.count == 0) {
        S.channels.addDefaultChannel();   // LongFast, well-known PSK
        channels_save();                  // file is truth from the first boot
    }
    for (uint8_t i = 0; i < S.channels.count; i++)
        H->log("channel %u '%s' hash %02x psk %uB", (unsigned)i,
               S.channels.effectiveName(i),
               (unsigned)S.channels.channels[i].hash,
               (unsigned)S.channels.channels[i].psk_raw_len);
}

// Channel index by effective name; -1 when absent.
static int channel_idx_by_name(const char* name) {
    if (!name || !name[0]) return -1;
    for (uint8_t i = 0; i < S.channels.count; i++)
        if (!strcmp(S.channels.effectiveName(i), name)) return (int)i;
    return -1;
}

// Rebuild the table from a row list (delete / psk-change edit the rows, then
// re-add so hashes and the inherit-primary rule recompute exactly as at boot).
struct ChanRow { char name[32]; uint8_t psk[34]; uint8_t psk_len; };

static void channels_rebuild(const ChanRow* rows, int n) {
    S.channels.init((MeshModemPreset)CFG.preset);
    for (int i = 0; i < n; i++)
        S.channels.addChannel(rows[i].name, rows[i].psk, rows[i].psk_len, i == 0);
    channels_save();
}

static void channels_snapshot(ChanRow* rows, int* n) {
    *n = S.channels.count;
    for (uint8_t i = 0; i < S.channels.count; i++) {
        const MeshChannel* ch = &S.channels.channels[i];
        memcpy(rows[i].name, ch->name, sizeof(rows[i].name));
        memcpy(rows[i].psk, ch->psk_raw, sizeof(rows[i].psk));
        rows[i].psk_len = ch->psk_raw_len;
    }
}

// ── Channel URL (meshtastic.org/e/#<base64url ChannelSet>) ───────────────────
// ChannelSet (apponly.proto): settings=1 (repeated ChannelSettings),
// lora_config=2. ChannelSettings (channel.proto): psk=2, name=3.
// LoRaConfig (config.proto): use_preset=1, modem_preset=2, region=7,
// channel_num=11 (1-based slot, 0=auto — our freq_slot semantics),
// override_frequency=14 (float).
//
// The PROTOBUF enums are ordered differently from the vendor's internal
// enums — mapped explicitly here (config.proto ModemPreset / RegionCode).
static const uint8_t kPresetToWire[MODEM_PRESET_COUNT] = {
    // vendor order: LONG_FAST, LONG_SLOW, LONG_MODERATE, LONG_TURBO,
    //               MEDIUM_FAST, MEDIUM_SLOW, SHORT_FAST, SHORT_SLOW,
    //               SHORT_TURBO
    0, 1, 7, 9, 4, 3, 6, 5, 8
};
static const uint8_t kRegionToWire[REGION_COUNT] = {
    // vendor order: US, EU_433, EU_868, CN, JP, ANZ, ANZ_433, KR, TW, IN,
    //               NZ_865, TH, RU, UNSET
    1, 2, 3, 4, 5, 6, 22, 7, 8, 10, 11, 12, 9, 0
};
static int wire_to_preset(uint32_t w) {
    for (int i = 0; i < MODEM_PRESET_COUNT; i++)
        if (kPresetToWire[i] == w) return i;
    return -1;
}
static int wire_to_region(uint32_t w) {
    for (int i = 0; i < REGION_COUNT; i++)
        if (kRegionToWire[i] == w) return i;
    return -1;
}

// Build the ChannelSet protobuf for our current channels + LoRa config.
// Returns encoded length, 0 on failure.
static size_t channel_set_encode(uint8_t* out, size_t cap) {
    PbWriter w = pbWriter(out, cap);
    uint8_t tmp[64];
    for (uint8_t i = 0; i < S.channels.count; i++) {
        const MeshChannel* ch = &S.channels.channels[i];
        PbWriter s = pbWriter(tmp, sizeof(tmp));
        if (ch->psk_raw_len > 0)
            if (!s.writeBytes(2, ch->psk_raw, ch->psk_raw_len)) return 0;
        if (ch->name[0])
            if (!s.writeString(3, ch->name)) return 0;
        if (!w.writeBytes(1, tmp, s.written())) return 0;
    }
    // The COMPLETE LoRa config, like official URLs: the importing app applies
    // it to its node wholesale, so an omitted field lands at the proto3 zero
    // default — tx_enabled=false would silently DISABLE the importer's TX.
    PbWriter l = pbWriter(tmp, sizeof(tmp));
    if (!l.writeVarintField(1, 1)) return 0;                       // use_preset
    if (!l.writeVarintField(2, kPresetToWire[CFG.preset])) return 0;
    if (!l.writeVarintField(7, kRegionToWire[CFG.region])) return 0;
    if (!l.writeVarintField(8, (uint64_t)CFG.hop_limit)) return 0;
    if (!l.writeVarintField(9, 1)) return 0;                       // tx_enabled
    if (!l.writeVarintField(10, (uint64_t)(uint8_t)CFG.tx_power)) return 0;
    if (CFG.freq_slot > 0)
        if (!l.writeVarintField(11, (uint64_t)CFG.freq_slot)) return 0;
    if (CFG.freq_override > 0.0f) {
        uint32_t bits;
        memcpy(&bits, &CFG.freq_override, 4);
        if (!l.writeFixed32Field(14, bits)) return 0;
    }
    if (!w.writeBytes(2, tmp, l.written())) return 0;
    return w.written();
}

// Parse a ChannelSet and apply it: REPLACES the whole channel table and the
// URL's LoRa config — the official app's import semantic (the URL is the
// complete network definition). Radio params land at the next boot.
static bool channel_set_import(const uint8_t* pb, size_t pblen) {
    ChanRow rows[MESH_MAX_CHANNELS];
    int nrows = 0;
    bool have_lora = false, w_use_preset = false;
    int64_t w_preset = -1, w_region = -1, w_slot = -1;
    int64_t w_hops = -1, w_txpwr = -1;
    float w_ovr = -1.0f;

    PbCursor c = pbCursor(pb, pblen);
    uint32_t field; uint8_t wtype;
    while (c.readTag(&field, &wtype)) {
        if (field == 1 && wtype == 2) {          // ChannelSettings
            PbCursor sub;
            if (!c.readLengthDelimited(&sub)) return false;
            if (nrows >= MESH_MAX_CHANNELS) continue;   // beyond our table: drop
            ChanRow* r = &rows[nrows];
            memset(r, 0, sizeof(*r));
            uint32_t f2; uint8_t t2;
            while (sub.readTag(&f2, &t2)) {
                if (f2 == 2 && t2 == 2) {
                    size_t pl = 0;
                    if (!sub.readBytes(r->psk, sizeof(r->psk), &pl)) return false;
                    r->psk_len = (uint8_t)(pl > 32 ? 32 : pl);
                } else if (f2 == 3 && t2 == 2) {
                    size_t nl = 0;
                    if (!sub.readString(r->name, sizeof(r->name), &nl)) return false;
                } else if (!sub.skipField(t2)) return false;
            }
            nrows++;
        } else if (field == 2 && wtype == 2) {   // LoRaConfig
            PbCursor sub;
            if (!c.readLengthDelimited(&sub)) return false;
            have_lora = true;
            uint32_t f2; uint8_t t2;
            while (sub.readTag(&f2, &t2)) {
                uint64_t v;
                if (f2 == 1 && t2 == 0)      { if (!sub.readVarint(&v)) return false; w_use_preset = (v != 0); }
                else if (f2 == 2 && t2 == 0) { if (!sub.readVarint(&v)) return false; w_preset = (int64_t)v; }
                else if (f2 == 7 && t2 == 0) { if (!sub.readVarint(&v)) return false; w_region = (int64_t)v; }
                else if (f2 == 8 && t2 == 0) { if (!sub.readVarint(&v)) return false; w_hops = (int64_t)v; }
                else if (f2 == 10 && t2 == 0){ if (!sub.readVarint(&v)) return false; w_txpwr = (int64_t)v; }
                else if (f2 == 11 && t2 == 0){ if (!sub.readVarint(&v)) return false; w_slot = (int64_t)v; }
                else if (f2 == 14 && t2 == 5){ if (!sub.readFloat(&w_ovr)) return false; }
                else if (!sub.skipField(t2)) return false;
            }
        } else if (!c.skipField(wtype)) {
            return false;
        }
    }
    if (nrows == 0) { H->log("channel_url: no channels in the set"); return false; }
    // A lora config WITHOUT use_preset carries custom bandwidth/sf/cr we
    // cannot represent — importing would silently mismatch the modem.
    if (have_lora && !w_use_preset) {
        H->log("channel_url: custom modem params (use_preset=false) not supported");
        return false;
    }

    int preset = -1, region = -1;
    if (w_preset >= 0) {
        preset = wire_to_preset((uint32_t)w_preset);
        if (preset < 0) { H->log("channel_url: modem preset %d not supported", (int)w_preset); return false; }
    }
    if (w_region >= 0) {
        region = wire_to_region((uint32_t)w_region);
        if (region < 0) { H->log("channel_url: region %d not supported", (int)w_region); return false; }
    }
    if (w_ovr > 0.0f && (w_ovr < 150.0f || w_ovr > 960.0f)) {
        H->log("channel_url: override %.3f MHz outside the SX1262 window", (double)w_ovr);
        return false;
    }

    channels_rebuild(rows, nrows);               // live + persisted
    if (have_lora) {
        if (preset >= 0) CFG.preset = (uint8_t)preset;
        if (region >= 0) CFG.region = (uint8_t)region;
        if (w_slot >= 0 && w_slot <= 1000) {
            CFG.freq_slot = (int16_t)w_slot;
            S.freq_slot = w_slot > 0 ? (int32_t)w_slot - 1 : -1;
        }
        if (w_hops >= 1 && w_hops <= 7)   CFG.hop_limit = (uint8_t)w_hops;
        if (w_txpwr >= 1 && w_txpwr <= 30) CFG.tx_power = (int8_t)w_txpwr;
        CFG.freq_override = (w_ovr > 0.0f) ? w_ovr : 0.0f;
        cfg_save();
    }
    H->log("channel_url imported: %d channel(s)%s — radio params at next boot",
           nrows, have_lora ? " + lora config" : "");
    return true;
}

struct Node {
    uint32_t num;
    char     long_name[40];
    char     short_name[5];
    bool     has_user;
    uint32_t last_heard;   // device epoch (host clock)
    float    snr, rssi;
    uint32_t last_ni_tx_ms;  // last NodeInfo request/reply we sent this node
    int32_t  lat_i, lon_i;   // degrees * 1e7 from their POSITION; 0/0 = none
    uint32_t pos_time;       // device epoch when we HEARD it (our clock rules)
    uint8_t  pos_prec;       // their declared precision_bits; 0 = exact/unknown
};
#define MAX_NODES 64
static Node s_nodes[MAX_NODES];
static int  s_node_count = 0;

static void peer_archive(const Node* n);   // defined with the peers file code

static Node* node_touch(uint32_t num, float snr, float rssi) {
    for (int i = 0; i < s_node_count; i++) {
        if (s_nodes[i].num == num) {
            s_nodes[i].last_heard = H->clock_now();
            s_nodes[i].snr = snr; s_nodes[i].rssi = rssi;
            return &s_nodes[i];
        }
    }
    if (s_node_count >= MAX_NODES) {
        // Evict the longest-silent node — archived first, never lost (the
        // MeshCore contacts rule; peers_arch is append-only in the data home).
        int old = 0;
        for (int i = 1; i < MAX_NODES; i++)
            if (s_nodes[i].last_heard < s_nodes[old].last_heard) old = i;
        peer_archive(&s_nodes[old]);
        memmove(&s_nodes[old], &s_nodes[old + 1], sizeof(Node) * (MAX_NODES - 1 - old));
        s_node_count = MAX_NODES - 1;
    }
    Node* n = &s_nodes[s_node_count++];
    memset(n, 0, sizeof(Node));
    n->num = num;
    n->last_heard = H->clock_now();
    n->snr = snr; n->rssi = rssi;
    return n;
}

// Display name for a node: NodeDB long_name, else "!aabbccdd".
static const char* node_name(uint32_t num, char* tmp, size_t tmp_sz) {
    for (int i = 0; i < s_node_count; i++)
        if (s_nodes[i].num == num && s_nodes[i].has_user && s_nodes[i].long_name[0])
            return s_nodes[i].long_name;
    snprintf(tmp, tmp_sz, "!%08x", (unsigned)num);
    return tmp;
}

// Reverse lookup for send_text: long_name, short_name or "!aabbccdd" id.
static bool node_by_name(const char* name, uint32_t* out) {
    if (!name || !name[0]) return false;
    if (name[0] == '!' && strlen(name) == 9) {
        *out = (uint32_t)strtol(name + 1, nullptr, 16);
        return true;
    }
    for (int i = 0; i < s_node_count; i++) {
        if (s_nodes[i].has_user &&
            (!strcmp(s_nodes[i].long_name, name) || !strcmp(s_nodes[i].short_name, name))) {
            *out = s_nodes[i].num;
            return true;
        }
    }
    return false;
}

// ── Peer persistence ─────────────────────────────────────────────────────────
// The Meshpunk pattern (the creed): the FILE <dir>/peers is the peer database;
// s_nodes is its RAM cache and S.node_keys the crypto lookup over the same
// records. Store-style key=value lines with a "---" terminator, written
// atomically (tmp + rename, the firmware_prefs rule). Dirty on MATERIAL
// change only (new user info / learned key) — never on last_heard — saved at
// most every PEERS_SAVE_GAP_MS from loop(), and at flush/stop.
static bool     s_peers_dirty = false;
static uint32_t s_peers_next_save_ms = 0;
#define PEERS_SAVE_GAP_MS 60000u

static void peers_save(void) {
    char path[160], tmp[168];
    snprintf(path, sizeof(path), "%s/peers", s_data);
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE* f = fopen(tmp, "w");
    if (!f) { H->log("peers save failed: %s", tmp); return; }
    int written = 0;
    for (int i = 0; i < s_node_count; i++) {
        const Node* n = &s_nodes[i];
        const uint8_t* key = S.node_keys.getKey(n->num);
        if (!n->has_user && !key) continue;   // nothing durable to keep
        fprintf(f, "num=%08x\n", (unsigned)n->num);
        if (n->long_name[0])  fprintf(f, "long=%s\n", n->long_name);
        if (n->short_name[0]) fprintf(f, "short=%s\n", n->short_name);
        if (key) {
            char hex[65];
            for (int b = 0; b < 32; b++)
                snprintf(hex + b * 2, 3, "%02x", key[b]);
            fprintf(f, "key=%s\n", hex);
        }
        fprintf(f, "heard=%u\n", (unsigned)n->last_heard);
        if (n->lat_i || n->lon_i) {
            fprintf(f, "lat=%d\n", (int)n->lat_i);     // degrees * 1e7
            fprintf(f, "lon=%d\n", (int)n->lon_i);
            fprintf(f, "ptime=%u\n", (unsigned)n->pos_time);
            if (n->pos_prec) fprintf(f, "prec=%u\n", (unsigned)n->pos_prec);
        }
        fprintf(f, "---\n");
        written++;
    }
    fclose(f);
    remove(path);
    rename(tmp, path);
    s_peers_dirty = false;
    H->log("peers saved (%d records)", written);
}

// Append one evicted node to the data home's archive (same record shape as
// the peers file). Only durable records — a node we never identified and hold
// no key for carries nothing worth keeping.
static void peer_archive(const Node* n) {
    const uint8_t* key = S.node_keys.getKey(n->num);
    if (!n->has_user && !key) return;
    char path[160];
    snprintf(path, sizeof(path), "%s/peers_arch", s_data);
    FILE* f = fopen(path, "a");
    if (!f) return;
    fprintf(f, "num=%08x\n", (unsigned)n->num);
    if (n->long_name[0])  fprintf(f, "long=%s\n", n->long_name);
    if (n->short_name[0]) fprintf(f, "short=%s\n", n->short_name);
    if (key) {
        char hex[65];
        for (int b = 0; b < 32; b++)
            snprintf(hex + b * 2, 3, "%02x", key[b]);
        fprintf(f, "key=%s\n", hex);
    }
    fprintf(f, "heard=%u\n", (unsigned)n->last_heard);
    if (n->lat_i || n->lon_i) {
        fprintf(f, "lat=%d\n", (int)n->lat_i);
        fprintf(f, "lon=%d\n", (int)n->lon_i);
        fprintf(f, "ptime=%u\n", (unsigned)n->pos_time);
        if (n->pos_prec) fprintf(f, "prec=%u\n", (unsigned)n->pos_prec);
    }
    fprintf(f, "---\n");
    fclose(f);
}

static void peers_load(void) {
    char path[160];
    snprintf(path, sizeof(path), "%s/peers", s_data);
    FILE* f = fopen(path, "r");
    if (!f) {
        // Migration: pre-data-home builds kept peers in the install dir.
        snprintf(path, sizeof(path), "%s/peers", s_dir);
        f = fopen(path, "r");
    }
    if (!f) return;
    char line[96];
    uint32_t num = 0, heard = 0, ptime = 0, prec = 0;
    int32_t lat = 0, lon = 0;
    char lng[40] = {0}, sht[5] = {0};
    uint8_t key[32];
    bool has_key = false;
    int loaded = 0;
    while (fgets(line, sizeof(line), f)) {
        char* nl = strchr(line, '\n'); if (nl) *nl = 0;
        char* cr = strchr(line, '\r'); if (cr) *cr = 0;
        if (!strcmp(line, "---")) {
            if (num) {
                Node* n = node_touch(num, 0, 0);
                n->last_heard = heard;   // node_touch stamped "now"; restore
                if (lng[0]) {
                    strncpy(n->long_name, lng, sizeof(n->long_name) - 1);
                    n->long_name[sizeof(n->long_name) - 1] = 0;
                    n->has_user = true;
                }
                if (sht[0]) {
                    strncpy(n->short_name, sht, sizeof(n->short_name) - 1);
                    n->short_name[sizeof(n->short_name) - 1] = 0;
                }
                if (has_key) S.node_keys.setKey(num, key);
                n->lat_i = lat; n->lon_i = lon; n->pos_time = ptime;
                n->pos_prec = (uint8_t)prec;
                loaded++;
            }
            num = 0; heard = 0; ptime = 0; prec = 0; lat = 0; lon = 0;
            lng[0] = 0; sht[0] = 0; has_key = false;
            continue;
        }
        char* eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        const char* k = line;
        const char* v = eq + 1;
        if      (!strcmp(k, "num"))   num = (uint32_t)strtol(v, nullptr, 16);
        else if (!strcmp(k, "heard")) heard = (uint32_t)strtol(v, nullptr, 10);
        else if (!strcmp(k, "lat"))   lat = (int32_t)strtol(v, nullptr, 10);
        else if (!strcmp(k, "lon"))   lon = (int32_t)strtol(v, nullptr, 10);
        else if (!strcmp(k, "ptime")) ptime = (uint32_t)strtol(v, nullptr, 10);
        else if (!strcmp(k, "prec"))  prec = (uint32_t)strtol(v, nullptr, 10);
        else if (!strcmp(k, "long"))  { strncpy(lng, v, sizeof(lng) - 1); lng[sizeof(lng) - 1] = 0; }
        else if (!strcmp(k, "short")) { strncpy(sht, v, sizeof(sht) - 1); sht[sizeof(sht) - 1] = 0; }
        else if (!strcmp(k, "key") && strlen(v) == 64) {
            has_key = true;
            for (int b = 0; b < 32; b++) {
                char h[3] = { v[b * 2], v[b * 2 + 1], 0 };
                key[b] = (uint8_t)strtol(h, nullptr, 16);
            }
        }
    }
    fclose(f);
    if (loaded) H->log("peers loaded (%d records)", loaded);
}

// ── PKI keypair persistence (pki.bin: 32B public + 32B private) ──────────────
// The MeshCore identity rule: the keypair lives in BOTH the install dir
// (internal) and the data home (chosen storage). Boot loads whichever copy
// exists and heals the missing one — a firmware+filesystem reflash keeps this
// node's identity as long as either copy survived (on-SD installs always do).
static bool pki_load_from(const char* dir) {
    char path[160];
    snprintf(path, sizeof(path), "%s/pki.bin", dir);
    FILE* f = fopen(path, "r");
    if (!f) return false;
    bool ok = fread(S.pki.public_key, 1, 32, f) == 32 &&
              fread(S.pki.private_key, 1, 32, f) == 32;
    fclose(f);
    S.pki.initialized = ok;
    return ok;
}

static void pki_save_to(const char* dir) {
    char path[160];
    snprintf(path, sizeof(path), "%s/pki.bin", dir);
    FILE* f = fopen(path, "w");
    if (!f) { H->log("identity save failed: %s", path); return; }
    fwrite(S.pki.public_key, 1, 32, f);
    fwrite(S.pki.private_key, 1, 32, f);
    fclose(f);
}

static bool file_present(const char* dir, const char* name) {
    char path[160];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    FILE* f = fopen(path, "r");
    if (f) { fclose(f); return true; }
    return false;
}

// ── TX queue (frames waiting out their CSMA delay) ───────────────────────────
struct TxItem {
    uint8_t  frame[256];
    uint16_t len;
    uint32_t due_ms;
    bool     used;
};
// 8 slots: since the citizenship round this queue also carries relays (one
// per channel packet heard), acks, positions and DM retries — 4 starved it.
#define TXQ 8
static TxItem s_txq[TXQ];
static bool   s_tx_inflight = false;

static bool tx_enqueue(const uint8_t* frame, size_t len, uint32_t delay_ms) {
    for (int i = 0; i < TXQ; i++) {
        if (!s_txq[i].used) {
            memcpy(s_txq[i].frame, frame, len);
            s_txq[i].len   = (uint16_t)len;
            s_txq[i].due_ms = H->millis32() + delay_ms;
            s_txq[i].used  = true;
            return true;
        }
    }
    // Silent drops here cost a debug cycle (a nodeinfo request vanished with
    // no trace) — every drop names itself now.
    H->log("TX QUEUE FULL — frame dropped (%u bytes)", (unsigned)len);
    return false;
}

static uint32_t rand_u32(void) {
    uint32_t v;
    H->random_bytes((uint8_t*)&v, 4);
    return v;
}

// ── RX dedup ─────────────────────────────────────────────────────────────────
// Retransmissions (the peer's want_ack retry ladder) and, later, flood echoes
// carry the same (from, id): one store/notify per id. Ring of the last 32.
struct SeenPkt { uint32_t from, id; };
#define SEEN_CAP 32
static SeenPkt s_seen[SEEN_CAP];
static uint8_t s_seen_head = 0;

static bool seen_before(uint32_t from, uint32_t id) {
    for (int i = 0; i < SEEN_CAP; i++)
        if (s_seen[i].id == id && s_seen[i].from == from) return true;
    s_seen[s_seen_head].from = from;
    s_seen[s_seen_head].id   = id;
    s_seen_head = (s_seen_head + 1) % SEEN_CAP;
    return false;
}

// ACK a want_ack packet addressed to us: ROUTING (port 5) back to the sender,
// Data.request_id = the acked packet's id, payload = Routing{error_reason:
// NONE} (field 3, varint 0 — explicit; it is the oneof selector). PKI DMs are
// acked over PKI (we hold the sender's key — we just decrypted them); channel
// packets over the channel they arrived on. Broadcasts are never unicast-acked
// (official semantics: a broadcast's ack is hearing it rebroadcast).
static void queue_ack(const MeshRxResult* r) {
    static const uint8_t routing_none[] = { 0x18, 0x00 };
    uint8_t frame[256];
    size_t n;
    if (r->is_pki) {
        n = S.buildDmTx(r->packet.from, PORT_ROUTING, routing_none,
                        sizeof(routing_none), false, frame, rand_u32,
                        CFG.hop_limit, r->packet.id);
    } else {
        n = S.buildTx(r->channel_idx >= 0 ? (uint8_t)r->channel_idx : 0,
                      r->packet.from, PORT_ROUTING, routing_none,
                      sizeof(routing_none), false, false, frame,
                      CFG.hop_limit, false, r->packet.id);
    }
    if (n > 0 && tx_enqueue(frame, n, meshTxDelayMs((MeshModemPreset)CFG.preset))) {
        H->log("ack -> !%08x for id %08x",
               (unsigned)r->packet.from, (unsigned)r->packet.id);
    }
}

// ── Reliable DMs (want_ack + retry ladder) ───────────────────────────────────
// Our DMs go out want_ack=1; the same built frame (same packet id, so the
// receiver's dedup collapses copies and the ack matches) re-airs until the
// ROUTING ack lands or the attempts run out. One in flight — chat sends are
// serial. Channel broadcasts stay single-shot (their implicit ack is being
// relayed by the mesh).
struct PendingDm {
    bool     active;
    uint32_t id;
    uint32_t to;
    uint8_t  frame[256];
    uint16_t len;
    uint8_t  attempts_left;
    uint32_t next_ms;
    uint8_t  state;   // 0 idle, 1 pending, 2 delivered, 3 failed
};
static PendingDm s_pdm;
#define DM_ATTEMPTS    3
#define DM_RETRY_MS    6000u

static uint32_t s_next_nodeinfo_ms = 0;
#define NODEINFO_FIRST_MS    20000u      // shortly after start: appear promptly
#define NODEINFO_TX_GAP_MS   30000u      // per-node request/reply rate limit

static uint32_t s_next_pos_ms = 0;
#define POSITION_FIRST_MS    40000u      // after the nodeinfo, once GPS may have a fix
// Repeat periods are CFG.nodeinfo_mins / CFG.pos_mins (user settings).

// Meshtastic HardwareModel for our NodeInfo, host-forwarded at boot
// ("hw_model" config — the per-board value lives with the firmware's board
// definitions; one board-neutral elf must not bake it). 0 = UNSET until the
// host speaks up (older firmware).
static uint16_t s_hw_model = 0;

// Broadcast our position from the host's last GPS fix (silent no-op without
// one, or with precision 0 = position sharing off). Reduced precision follows
// the official channel semantics exactly: keep the top N bits of the i32
// coordinates, add half the dropped cell so the point sits at the cell
// CENTER, and declare precision_bits so receivers draw the accuracy circle.
static void queue_position(void) {
    if (CFG.pos_precision == 0) return;
    double lat, lon;
    if (!H->gps_fix || !H->gps_fix(&lat, &lon)) return;
    int32_t lat_i = (int32_t)(lat * 1e7);
    int32_t lon_i = (int32_t)(lon * 1e7);
    uint32_t prec = CFG.pos_precision;
    if (prec < 32) {
        uint32_t mask = 0xFFFFFFFFu << (32 - prec);
        lat_i = (int32_t)(((uint32_t)lat_i & mask) + (1u << (31 - prec)));
        lon_i = (int32_t)(((uint32_t)lon_i & mask) + (1u << (31 - prec)));
    }
    uint8_t pos_buf[32];
    size_t pos_len = meshEncodePosition(pos_buf, sizeof(pos_buf),
                                        lat_i, lon_i, H->clock_now(),
                                        prec < 32 ? prec : 0);
    if (pos_len == 0) return;
    uint8_t frame[256];
    size_t n = S.buildTx(0, MESH_ADDR_BROADCAST, PORT_POSITION,
                         pos_buf, pos_len, false, CFG.ok_to_mqtt != 0,
                         frame, CFG.hop_limit);
    if (n > 0 && tx_enqueue(frame, n, meshTxDelayMs((MeshModemPreset)CFG.preset)))
        H->log("position broadcast queued");
}

// Send our NodeInfo to `dest` — the official key-bootstrap exchange. NodeInfo
// travels CHANNEL-encrypted even unicast (official firmware exempts it from
// PKI so keys can bootstrap); want_response=1 asks the peer to answer with
// ITS NodeInfo, which carries the public key PKI DMs need. Built by the
// vendor (buildNodeInfoTx grew a want_response param — MESHPUNK patch).
static void queue_nodeinfo_to(uint32_t dest, bool want_response) {
    char id[16];
    snprintf(id, sizeof(id), "!%08x", (unsigned)S.node_num);
    uint8_t frame[256];
    size_t n = S.buildNodeInfoTx(0, dest, id, CFG.long_name, CFG.short_name,
                                 s_hw_model, false, CFG.ok_to_mqtt != 0, frame,
                                 S.pki.public_key, 32, CFG.hop_limit,
                                 want_response);
    if (n > 0 && tx_enqueue(frame, n, meshTxDelayMs((MeshModemPreset)CFG.preset))) {
        H->log("nodeinfo -> !%08x queued (%u bytes%s)", (unsigned)dest,
               (unsigned)n, want_response ? ", response requested" : "");
    }
}

static void queue_nodeinfo(void) {
    queue_nodeinfo_to(MESH_ADDR_BROADCAST, false);
}

// Rate-limited per node: ask a keyless node for its NodeInfo (or answer its
// request). Returns false when inside the per-node gap.
static bool nodeinfo_tx_allowed(uint32_t num) {
    Node* n = node_touch(num, 0, 0);
    uint32_t now = H->millis32();
    if (n->last_ni_tx_ms && (now - n->last_ni_tx_ms) < NODEINFO_TX_GAP_MS) {
        H->log("nodeinfo tx to !%08x rate-limited (%us left)",
               (unsigned)num,
               (unsigned)((NODEINFO_TX_GAP_MS - (now - n->last_ni_tx_ms)) / 1000));
        return false;
    }
    n->last_ni_tx_ms = now;
    return true;
}

// ── RX funnel ────────────────────────────────────────────────────────────────
// Everything a frame becomes passes through here — the router layer hooks in
// at this one spot later.
// Relay one frame we heard (role CLIENT): byte-identical copy with hop_limit
// decremented and relay_node stamped with our low address byte. Runs on the
// RAW frame — official nodes relay traffic they cannot decrypt too (that is
// what carries other channels and other people's DMs across the mesh).
// Drop a queued relay of (from, id) — a CLIENT heard someone else relay it
// first, so airing our copy would only add noise. Queued frames are raw
// packet copies, so the header fields are at their wire offsets.
static void txq_cancel_relay(uint32_t from, uint32_t id) {
    for (int i = 0; i < TXQ; i++) {
        if (!s_txq[i].used || s_txq[i].len < sizeof(MeshPacketHeader)) continue;
        const MeshPacketHeader* h = (const MeshPacketHeader*)s_txq[i].frame;
        if (h->from == from && h->id == id) {
            s_txq[i].used = false;
            H->log("relay id %08x cancelled — overheard another node's", (unsigned)id);
            return;
        }
    }
}

static void queue_rebroadcast(const uint8_t* raw, int len,
                              const MeshRxPacket* p, float snr) {
    uint8_t frame[256];
    if (len > (int)sizeof(frame)) return;
    memcpy(frame, raw, len);
    MeshPacketHeader* hdr = (MeshPacketHeader*)frame;
    hdr->flags = (uint8_t)((hdr->flags & ~MESH_FLAGS_HOP_LIMIT_MASK) |
                           ((p->hop_limit - 1) & MESH_FLAGS_HOP_LIMIT_MASK));
    hdr->relay_node = (uint8_t)(S.node_num & 0xFF);
    uint32_t delay = meshRebroadcastDelayMs((MeshModemPreset)CFG.preset,
                                            (MeshRole)CFG.role, snr);
    if (tx_enqueue(frame, (size_t)len, delay)) {
        H->log("relay id %08x from !%08x hop %u->%u in %ums",
               (unsigned)p->id, (unsigned)p->from,
               (unsigned)p->hop_limit, (unsigned)(p->hop_limit - 1),
               (unsigned)delay);
    }
}

static void on_rx(const uint8_t* raw, int len, float rssi, float snr) {
    if (H->capture_frame) H->capture_frame(0 /*rx*/, raw, len, snr, rssi);

    // ── Raw-header pass: echo drop, dedup, relay ─────────────────────────
    // All three work on the 16-byte header alone, BEFORE any decrypt.
    MeshRxPacket hdr;
    if (!meshParsePacket(raw, (size_t)len, &hdr)) {
        H->log("rx DROP short frame len=%d", len);
        return;
    }
    if (hdr.from == S.node_num) return;      // our own packet relayed back
    bool dup   = seen_before(hdr.from, hdr.id);
    bool to_us = (hdr.to == S.node_num);

    // CLIENT and ROUTER_LATE relay fresh flood traffic not addressed to us
    // (both get the same offset+SNR-weighted delay — the vendor mirrors the
    // official treatment); CLIENT_MUTE never relays. Duplicates are never
    // relayed (the dedup ring is also the flood suppression). The role
    // DIFFERENCE is below: a CLIENT hearing the packet again (someone else
    // relayed first) CANCELS its own queued relay; ROUTER_LATE keeps it and
    // relays regardless — the official gap-filler semantics.
    if ((CFG.role == ROLE_CLIENT || CFG.role == ROLE_ROUTER_LATE) &&
        !dup && !to_us && hdr.hop_limit > 0)
        queue_rebroadcast(raw, len, &hdr, snr);
    // Only a true REBROADCAST dup cancels (hop budget spent: hop_limit <
    // hop_start). The ORIGINATOR's own retransmission arrives with the full
    // budget (hop_start == hop_limit) and must NOT cancel — our relay is the
    // implicit ack the sender is retrying to hear (FloodingRouter.cpp
    // isRepeated semantics).
    bool repeated_orig = (hdr.hop_start > 0 && hdr.hop_start == hdr.hop_limit);
    if (dup && CFG.role == ROLE_CLIENT && !repeated_orig)
        txq_cancel_relay(hdr.from, hdr.id);

    MeshRxResult r;
    if (!S.processRx(raw, (size_t)len, rssi, snr, &r)) {
        if (dup) return;                     // already diagnosed first time
        // Diagnose the drop — a silent drop here cost a debug cycle once.
        if (meshIsPkiCandidate(&hdr, S.node_num)) {
            H->log("rx DROP pki-dm from !%08x id %08x len %u: no key for sender (or auth fail)",
                   (unsigned)hdr.from, (unsigned)hdr.id, (unsigned)hdr.payload_len);
            // Official key bootstrap: our NodeInfo unicast with want_response —
            // the peer answers with its NodeInfo (channel-encrypted, key
            // attached) and the next DM decrypts.
            if (nodeinfo_tx_allowed(hdr.from)) queue_nodeinfo_to(hdr.from, true);
        } else {
            H->log("rx DROP from !%08x to %08x hash %02x len %u: no channel match/decode",
                   (unsigned)hdr.from, (unsigned)hdr.to, (unsigned)hdr.channel_hash,
                   (unsigned)hdr.payload_len);
        }
        return;
    }
    H->log("rx %s from !%08x port %u len %u hops %u/%u",
           r.is_pki ? "pki-dm" : "ch", (unsigned)r.packet.from,
           (unsigned)r.data.portnum, (unsigned)r.data.payload_len,
           (unsigned)r.packet.hop_limit, (unsigned)r.packet.hop_start);

    if (dup) {
        // Retransmission. Re-ack (our previous ack may be the thing that got
        // lost — that is why they resent) but never store/notify twice.
        if (r.packet.want_ack && (r.is_pki || to_us)) queue_ack(&r);
        H->log("rx dup id %08x — suppressed", (unsigned)r.packet.id);
        return;
    }

    if (r.packet.want_ack && (r.is_pki || to_us)) queue_ack(&r);

    uint8_t hops = (r.packet.hop_start >= r.packet.hop_limit)
                   ? (uint8_t)(r.packet.hop_start - r.packet.hop_limit) : 0;
    bool direct = (hops == 0);
    Node* n = node_touch(r.packet.from, snr, rssi);

    char tmp[16];
    const char* from = node_name(r.packet.from, tmp, sizeof(tmp));

    switch (r.data.portnum) {
    case PORT_TEXT_MESSAGE: {
        char text[200];
        size_t tl = r.data.payload_len < sizeof(text) - 1 ? r.data.payload_len : sizeof(text) - 1;
        memcpy(text, r.data.payload, tl);
        text[tl] = 0;
        uint32_t ts = H->clock_now();
        char nbuf[192];
        if (r.is_pki || r.packet.to == S.node_num) {
            // Direct message to us: always alerts + lands in the bell log
            // (the MeshCore DM rule).
            H->store_dm_msg(from, from, text, ts, snr, rssi, hops, direct);
            H->unread_bump_dm(from);
            snprintf(nbuf, sizeof(nbuf), "From %s: %s", from, text);
            H->notify_post(nbuf);
        } else {
            const char* ch = S.channels.effectiveName(
                r.channel_idx >= 0 ? (uint8_t)r.channel_idx : 0);
            H->store_channel_msg(ch, r.channel_idx, from, text, ts, snr, rssi, hops, direct);
            H->unread_bump_channel(ch);
            uint8_t nmode = notify_mode_get(ch);
            if (nmode == NOTIFY_ALL ||
                (nmode == NOTIFY_MENTION && (contains_mention(text, CFG.long_name) ||
                                             contains_mention(text, CFG.short_name)))) {
                snprintf(nbuf, sizeof(nbuf), "#%s %s: %s", ch, from, text);
                H->notify_post(nbuf);
            }
        }
        break;
    }
    case PORT_NODEINFO: {
        MeshUser u;
        if (meshDecodeUser(r.data.payload, r.data.payload_len, &u)) {
            strncpy(n->long_name, u.long_name, sizeof(n->long_name) - 1);
            n->long_name[sizeof(n->long_name) - 1] = 0;
            strncpy(n->short_name, u.short_name, sizeof(n->short_name) - 1);
            n->short_name[sizeof(n->short_name) - 1] = 0;
            n->has_user = true;
            // processRx's meshLearnNodeKey stored the key if it was 32 bytes;
            // report what actually arrived — DMs both ways hinge on it.
            bool have_key = S.node_keys.getKey(r.packet.from) != nullptr;
            H->log("nodeinfo !%08x id='%s' long='%s' short='%s' hw=%u key=%uB -> key %s",
                   (unsigned)r.packet.from, u.id, u.long_name, u.short_name,
                   (unsigned)u.hw_model, (unsigned)u.public_key_len,
                   have_key ? "KNOWN" : "MISSING");
            s_peers_dirty = true;   // name and/or key are durable — schedule a save
            // Key-bootstrap exchange: answer a response request with our own
            // NodeInfo so the peer gets our key without the broadcast wait.
            if (r.data.want_response && nodeinfo_tx_allowed(r.packet.from))
                queue_nodeinfo_to(r.packet.from, false);
        } else {
            H->log("nodeinfo !%08x: USER DECODE FAILED len=%u",
                   (unsigned)r.packet.from, (unsigned)r.data.payload_len);
        }
        break;
    }
    case PORT_ROUTING:
        // ACK/NAK for one of our packets (request_id = which).
        H->log("routing from !%08x acks id %08x",
               (unsigned)r.packet.from, (unsigned)r.data.request_id);
        if (s_pdm.active && r.data.request_id == s_pdm.id) {
            s_pdm.active = false;
            s_pdm.state  = 2;   // delivered — the chat app polls dm_status
            H->log("dm id %08x DELIVERED", (unsigned)s_pdm.id);
        }
        break;
    case PORT_POSITION: {
        MeshPosition pos;
        if (meshDecodePosition(r.data.payload, r.data.payload_len, &pos) &&
            (pos.latitude_i != 0 || pos.longitude_i != 0)) {
            n->lat_i    = pos.latitude_i;
            n->lon_i    = pos.longitude_i;
            n->pos_time = H->clock_now();   // our clock, not the sender's claim
            n->pos_prec = (uint8_t)(pos.precision_bits < 32 ? pos.precision_bits : 0);
            s_peers_dirty = true;           // Map reads positions from the peers file
            H->log("position !%08x %d.%07d %d.%07d",
                   (unsigned)r.packet.from,
                   (int)(pos.latitude_i / 10000000), (int)(pos.latitude_i % 10000000 < 0 ? -(pos.latitude_i % 10000000) : pos.latitude_i % 10000000),
                   (int)(pos.longitude_i / 10000000), (int)(pos.longitude_i % 10000000 < 0 ? -(pos.longitude_i % 10000000) : pos.longitude_i % 10000000));
        }
        break;
    }
    default:
        // TELEMETRY / others: last_heard already updated.
        break;
    }
}

// ── LoraProtoOps ─────────────────────────────────────────────────────────────

static bool mt_init(const MeshHostApi* host, const char* proto_dir) {
    if (!host || host->abi != LORA_PROTO_ABI_VERSION) return false;
    H = host;
    strncpy(s_dir, proto_dir ? proto_dir : "", sizeof(s_dir) - 1);
    s_dir[sizeof(s_dir) - 1] = 0;
    // Data home on the chosen mesh storage; fall back to the install dir if
    // the host gave none (never expected — belt for older hosts).
    const char* dd = (H->data_dir) ? H->data_dir() : nullptr;
    strncpy(s_data, (dd && dd[0]) ? dd : s_dir, sizeof(s_data) - 1);
    s_data[sizeof(s_data) - 1] = 0;
    H->log("data home: %s", s_data);

    // Vendor code (packet ids, CSMA slots) uses rand(); seed it from the TRNG.
    srand((unsigned)rand_u32());

    cfg_load();
    // The file is the config's source of truth from the FIRST boot: a
    // never-configured install otherwise has no cfg file, and the firmware's
    // offline-settings path (inactive-protocol editing) would read nothing.
    if (!file_present(s_data, "cfg")) cfg_save();
    notify_prefs_load();

    // Keypair before node_num: the address derives from the persisted public
    // key (this device has no MAC visible here), so it is stable across boots.
    S.init((MeshRegion)CFG.region, (MeshModemPreset)CFG.preset,
           (MeshRole)CFG.role, /*node_num placeholder*/ 0,
           CFG.freq_slot > 0 ? (int32_t)CFG.freq_slot - 1 : -1);
    bool from_install = pki_load_from(s_dir);
    bool from_data    = !from_install && pki_load_from(s_data);
    if (from_install || from_data) {
        // Heal the missing copy (the MeshCore identity rule).
        if (from_install && !file_present(s_data, "pki.bin")) {
            pki_save_to(s_data);
            H->log("identity copied to data home");
        } else if (from_data) {
            pki_save_to(s_dir);
            H->log("identity restored from data home (reflash survived)");
        }
    } else {
        if (!S.pki.generate()) { H->log("keypair generation failed"); return false; }
        pki_save_to(s_dir);
        pki_save_to(s_data);
        H->log("new PKI keypair generated + saved (both locations)");
    }
    uint8_t digest[32];
    mesh_sha256(S.pki.public_key, 32, digest);
    uint32_t num;
    memcpy(&num, digest, 4);
    if (num == 0 || num == MESH_ADDR_BROADCAST) num = 0x4D504B00 | (digest[4] & 0x7F);
    S.node_num = num;

    channels_load();   // file-backed table; default = LongFast, well-known PSK

    memset(s_nodes, 0, sizeof(s_nodes));
    s_node_count = 0;
    memset(s_txq, 0, sizeof(s_txq));
    s_tx_inflight = false;
    peers_load();   // file is the peer database; this primes the RAM cache + keys

    H->log("mtlite init: node !%08x region %s preset %s name '%s'",
           (unsigned)S.node_num,
           meshGetRegion((MeshRegion)CFG.region)->name,
           meshPresetName((MeshModemPreset)CFG.preset), CFG.long_name);
    return true;
}

// The radio config with the user's frequency overrides applied: the slot
// rides S.freq_slot inside radioConfig; an explicit override MHz wins last.
static MeshRadioConfig effective_radio_config(void) {
    MeshRadioConfig rc = S.radioConfig(CFG.tx_power);
    if (CFG.freq_override > 0.0f) rc.frequency_mhz = CFG.freq_override;
    return rc;
}

static bool mt_start(void) {
    MeshRadioConfig rc = effective_radio_config();
    if (CFG.freq_override > 0.0f)
        H->log("frequency OVERRIDE active: %.3f MHz", rc.frequency_mhz);
    // Board reality on top of the region limit: our PA path tops out at
    // 22 dBm radiated; the HAL maps radiated->chip per board.
    int8_t pwr = rc.tx_power_dbm;
    if (pwr > 22) pwr = 22;
    if (!H->radio_config(rc.frequency_mhz, rc.bandwidth_khz,
                         rc.spreading_factor, rc.coding_rate,
                         pwr, /*crc*/ true,
                         rc.sync_word, rc.preamble_length)) {
        H->log("radio config failed");
        return false;
    }
    if (!H->radio_start_receive()) {
        H->log("start_receive failed");
        return false;
    }
    H->log("radio up: %.3f MHz bw %.0f sf %d cr 4/%d sync 0x%02X pre %u tx %d",
           rc.frequency_mhz, rc.bandwidth_khz, (int)rc.spreading_factor,
           (int)rc.coding_rate, (unsigned)rc.sync_word,
           (unsigned)rc.preamble_length, (int)pwr);
    s_next_nodeinfo_ms = H->millis32() + NODEINFO_FIRST_MS;
    s_next_pos_ms      = H->millis32() + POSITION_FIRST_MS;
    return true;
}

static void mt_loop(void) {
    uint32_t irq = H->radio_poll_irq();

    if (irq & IRQ_RX_DONE) {
        uint8_t buf[256];
        int n = H->radio_read_packet(buf, sizeof(buf));   // re-arms RX either way
        if (n > 0) on_rx(buf, n, H->radio_last_rssi(), H->radio_last_snr());
    }

    if (s_tx_inflight && (irq & IRQ_TX_DONE)) {
        H->radio_send_finished();     // clears TX irq, re-enters RX
        s_tx_inflight = false;
    }

    if (!s_tx_inflight) {
        uint32_t now = H->millis32();
        for (int i = 0; i < TXQ; i++) {
            if (s_txq[i].used && (int32_t)(now - s_txq[i].due_ms) >= 0) {
                if (H->radio_start_send(s_txq[i].frame, s_txq[i].len)) {
                    if (H->capture_frame)
                        H->capture_frame(1 /*tx*/, s_txq[i].frame, s_txq[i].len, 0, 0);
                    s_tx_inflight = true;
                } else if (H->capture_frame) {
                    H->capture_frame(2 /*tx_fail*/, s_txq[i].frame, s_txq[i].len, 0, 0);
                }
                s_txq[i].used = false;
                break;
            }
        }
    }

    uint32_t now = H->millis32();
    if (s_next_nodeinfo_ms && (int32_t)(now - s_next_nodeinfo_ms) >= 0) {
        queue_nodeinfo();
        s_next_nodeinfo_ms = now + (uint32_t)CFG.nodeinfo_mins * 60000u;
    }
    if (s_next_pos_ms && (int32_t)(now - s_next_pos_ms) >= 0) {
        queue_position();
        s_next_pos_ms = now + (uint32_t)CFG.pos_mins * 60000u;
    }

    // DM retry ladder: re-air the SAME frame until acked or out of attempts.
    if (s_pdm.active && (int32_t)(now - s_pdm.next_ms) >= 0) {
        if (s_pdm.attempts_left > 0) {
            s_pdm.attempts_left--;
            s_pdm.next_ms = now + DM_RETRY_MS * 2;
            if (tx_enqueue(s_pdm.frame, s_pdm.len,
                           meshTxDelayMs((MeshModemPreset)CFG.preset)))
                H->log("dm id %08x retry (%u left)",
                       (unsigned)s_pdm.id, (unsigned)s_pdm.attempts_left);
        } else {
            s_pdm.active = false;
            s_pdm.state  = 3;   // failed — no ack after the full ladder
            H->log("dm id %08x FAILED (no ack)", (unsigned)s_pdm.id);
        }
    }

    // Peer DB write-behind: material changes land on file at most once per
    // gap (never per-packet — flash churn), plus at flush/stop.
    if (s_peers_dirty && (int32_t)(now - s_peers_next_save_ms) >= 0) {
        peers_save();
        s_peers_next_save_ms = now + PEERS_SAVE_GAP_MS;
    }
}

static void mt_flush(void) {
    if (s_peers_dirty) peers_save();
}

static void mt_stop(void) {
    mt_flush();
    H->radio_standby();
}

static bool mt_send_channel_text(const char* channel, const char* text) {
    if (!channel || !text || !text[0]) return false;
    int idx = channel_idx_by_name(channel);
    if (idx < 0) { H->log("send: no channel '%s'", channel); return false; }
    uint8_t frame[256];
    size_t n = S.buildTextTx((uint8_t)idx, MESH_ADDR_BROADCAST, text, false,
                             CFG.ok_to_mqtt != 0, frame, CFG.hop_limit);
    if (n == 0) return false;
    if (!tx_enqueue(frame, n, meshTxDelayMs((MeshModemPreset)CFG.preset))) {
        H->log("tx queue full — channel text dropped");
        return false;
    }
    // Own echo into the shared store (we never hear our own frame).
    H->store_channel_msg(S.channels.effectiveName((uint8_t)idx), idx,
                         CFG.long_name, text, H->clock_now(), 0, 0, 0, true);
    return true;
}

static bool mt_send_text(const char* peer, const char* text) {
    if (!peer || !text || !text[0]) return false;
    uint32_t to;
    if (!node_by_name(peer, &to)) { H->log("dm: peer '%s' not in NodeDB", peer); return false; }
    H->log("dm: '%s' -> !%08x key=%s", peer, (unsigned)to,
           S.node_keys.getKey(to) ? "known" : "MISSING");
    uint8_t frame[256];
    uint32_t pkt_id = 0;
    size_t n = S.buildDmTx(to, PORT_TEXT_MESSAGE,
                           (const uint8_t*)text, strlen(text),
                           /*want_ack*/ true, frame, rand_u32,
                           CFG.hop_limit, /*request_id*/ 0, &pkt_id);
    if (n == 0) {
        H->log("dm to %s: no key yet — requesting their nodeinfo, retry shortly", peer);
        if (nodeinfo_tx_allowed(to)) queue_nodeinfo_to(to, true);
        return false;
    }
    if (!tx_enqueue(frame, n, meshTxDelayMs((MeshModemPreset)CFG.preset))) {
        H->log("tx queue full — DM dropped");
        return false;
    }
    // Arm the retry ladder for this frame (replaces any earlier pending one).
    memcpy(s_pdm.frame, frame, n);
    s_pdm.len           = (uint16_t)n;
    s_pdm.id            = pkt_id;
    s_pdm.to            = to;
    s_pdm.attempts_left = DM_ATTEMPTS - 1;
    s_pdm.next_ms       = H->millis32() + DM_RETRY_MS;
    s_pdm.state         = 1;
    s_pdm.active        = true;
    H->store_dm_msg(peer, CFG.long_name, text, H->clock_now(), 0, 0, 0, true);
    return true;
}

static int mt_get_peers(LoraProtoPeer* out, int max) {
    int filled = 0;
    for (int i = 0; i < s_node_count && filled < max; i++) {
        LoraProtoPeer* p = &out[filled++];
        memset(p, 0, sizeof(*p));
        snprintf(p->id, sizeof(p->id), "!%08x", (unsigned)s_nodes[i].num);
        if (s_nodes[i].has_user && s_nodes[i].long_name[0])
            strncpy(p->name, s_nodes[i].long_name, sizeof(p->name) - 1);
        else
            strncpy(p->name, p->id, sizeof(p->name) - 1);
        p->last_heard = s_nodes[i].last_heard;
        p->snr  = s_nodes[i].snr;
        p->rssi = s_nodes[i].rssi;
    }
    return filled;
}

static int mt_get_config(const char* key, char* out, int out_sz) {
    if (!key || !out || out_sz <= 0) return 0;
    int n = 0;
    // Everything we hold on one node ("peer_info:<name-or-!id>"), one field
    // per line — the Peers info view renders this verbatim. Answers "have we
    // received their position yet?" definitively.
    if (!strncmp(key, "peer_info:", 10)) {
        uint32_t num;
        if (!node_by_name(key + 10, &num)) return 0;
        const Node* nd = nullptr;
        for (int i = 0; i < s_node_count; i++)
            if (s_nodes[i].num == num) { nd = &s_nodes[i]; break; }
        if (!nd) return 0;
        uint32_t now = H->clock_now();
        int w = snprintf(out, out_sz,
            "id: !%08x\nname: %s\nshort: %s\nDM key: %s\nheard: %us ago\n",
            (unsigned)num,
            (nd->has_user && nd->long_name[0]) ? nd->long_name : "(none received)",
            nd->short_name[0] ? nd->short_name : "-",
            S.node_keys.getKey(num) ? "yes" : "NO (DMs unavailable)",
            (unsigned)(now > nd->last_heard ? now - nd->last_heard : 0));
        if (w > 0 && w < out_sz) {
            if (nd->lat_i || nd->lon_i) {
                if (nd->pos_prec) {
                    // Half the precision cell, in meters at the equator scale.
                    float rm = (float)(1u << (32 - nd->pos_prec)) * 1e-7f / 2.0f * 111320.0f;
                    w += snprintf(out + w, out_sz - w,
                        "position: %.5f, %.5f\nprecision: ~%.0f m (%u bits)\n"
                        "pos heard: %us ago\nsnr: %.1f  rssi: %.0f",
                        nd->lat_i / 1e7, nd->lon_i / 1e7,
                        rm, (unsigned)nd->pos_prec,
                        (unsigned)(now > nd->pos_time ? now - nd->pos_time : 0),
                        nd->snr, nd->rssi);
                } else {
                    w += snprintf(out + w, out_sz - w,
                        "position: %.5f, %.5f\nprecision: exact/undeclared\n"
                        "pos heard: %us ago\nsnr: %.1f  rssi: %.0f",
                        nd->lat_i / 1e7, nd->lon_i / 1e7,
                        (unsigned)(now > nd->pos_time ? now - nd->pos_time : 0),
                        nd->snr, nd->rssi);
                }
            } else {
                w += snprintf(out + w, out_sz - w,
                    "position: NONE RECEIVED\nsnr: %.1f  rssi: %.0f",
                    nd->snr, nd->rssi);
            }
        }
        return (w > 0 && w < out_sz) ? w : (w > 0 ? out_sz - 1 : 0);
    }
    if      (!strcmp(key, "region"))     n = snprintf(out, out_sz, "%d", (int)CFG.region);
    else if (!strcmp(key, "region_name"))n = snprintf(out, out_sz, "%s", meshGetRegion((MeshRegion)CFG.region)->name);
    else if (!strcmp(key, "preset"))     n = snprintf(out, out_sz, "%d", (int)CFG.preset);
    else if (!strcmp(key, "preset_name"))n = snprintf(out, out_sz, "%s", meshPresetName((MeshModemPreset)CFG.preset));
    else if (!strcmp(key, "long_name"))  n = snprintf(out, out_sz, "%s", CFG.long_name);
    else if (!strcmp(key, "short_name")) n = snprintf(out, out_sz, "%s", CFG.short_name);
    else if (!strcmp(key, "hop_limit"))  n = snprintf(out, out_sz, "%d", (int)CFG.hop_limit);
    else if (!strcmp(key, "tx_power"))   n = snprintf(out, out_sz, "%d", (int)CFG.tx_power);
    else if (!strcmp(key, "node_id"))    n = snprintf(out, out_sz, "!%08x", (unsigned)S.node_num);
    else if (!strcmp(key, "role"))       n = snprintf(out, out_sz, "%d", (int)CFG.role);
    else if (!strcmp(key, "role_name"))  n = snprintf(out, out_sz, "%s",
                                             CFG.role == ROLE_CLIENT ? "Client" :
                                             CFG.role == ROLE_CLIENT_MUTE ? "Client Mute" : "Router Late");
    else if (!strcmp(key, "ok_to_mqtt")) n = snprintf(out, out_sz, "%d", (int)CFG.ok_to_mqtt);
    else if (!strcmp(key, "nodeinfo_mins")) n = snprintf(out, out_sz, "%d", (int)CFG.nodeinfo_mins);
    else if (!strcmp(key, "pos_mins"))   n = snprintf(out, out_sz, "%d", (int)CFG.pos_mins);
    else if (!strcmp(key, "pos_precision")) n = snprintf(out, out_sz, "%d", (int)CFG.pos_precision);
    else if (!strcmp(key, "pos_precision_name")) n = snprintf(out, out_sz, "%s",
                                             CFG.pos_precision == 0  ? "Off" :
                                             CFG.pos_precision >= 32 ? "Exact" :
                                             CFG.pos_precision >= 16 ? "~370 m" :
                                             CFG.pos_precision >= 13 ? "~3 km" :
                                             CFG.pos_precision >= 11 ? "~12 km" : "~23 km");
    else if (!strcmp(key, "dm_status"))  n = snprintf(out, out_sz, "%s",
                                             s_pdm.state == 1 ? "pending" :
                                             s_pdm.state == 2 ? "delivered" :
                                             s_pdm.state == 3 ? "failed" : "idle");
    else if (!strcmp(key, "channel"))    n = snprintf(out, out_sz, "%s", S.channels.effectiveName(0));
    // All channel names, newline-separated, primary first.
    else if (!strcmp(key, "channels")) {
        for (uint8_t i = 0; i < S.channels.count && n < out_sz - 1; i++)
            n += snprintf(out + n, out_sz - n, "%s%s", i ? "\n" : "",
                          S.channels.effectiveName(i));
    }
    // Per-channel detail for the settings UI: hash, psk kind, psk base64.
    else if (!strncmp(key, "channel_info:", 13)) {
        int idx = channel_idx_by_name(key + 13);
        if (idx < 0) return 0;
        const MeshChannel* ch = &S.channels.channels[idx];
        const char* kind = ch->psk_raw_len == 0  ? (idx == 0 ? "none" : "inherit") :
                           ch->psk_raw_len == 1  ? (ch->psk_raw[0] == 0 ? "none" : "default") :
                           ch->psk_raw_len <= 16 ? "custom-128" : "custom-256";
        char b64[48];
        b64_encode(ch->psk_raw, ch->psk_raw_len, b64, sizeof(b64));
        n = snprintf(out, out_sz, "hash=0x%02x\npsk=%s\npsk_b64=%s\nprimary=%d",
                     (unsigned)ch->hash, kind, b64, idx == 0 ? 1 : 0);
    }
    else if (!strncmp(key, "notify_ch:", 10))
        n = snprintf(out, out_sz, "%d", (int)notify_mode_get(key + 10));
    else if (!strcmp(key, "freq")) {
        MeshRadioConfig rc = effective_radio_config();
        n = snprintf(out, out_sz, "%.3f", rc.frequency_mhz);
    }
    else if (!strcmp(key, "freq_slot"))     n = snprintf(out, out_sz, "%d", (int)CFG.freq_slot);
    else if (!strcmp(key, "freq_override")) n = snprintf(out, out_sz, "%.3f", (double)CFG.freq_override);
    // Slot facts for the settings UI: how many slots the region/preset has,
    // and which one is actually in use (1-based; override MHz bypasses slots).
    else if (!strcmp(key, "freq_slots") || !strcmp(key, "freq_slot_active")) {
        MeshFreqConfig fc = meshCalcFrequency(
            meshGetRegion((MeshRegion)CFG.region), (MeshModemPreset)CFG.preset,
            S.channels.effectiveName(0), S.freq_slot);
        n = snprintf(out, out_sz, "%u", !strcmp(key, "freq_slots")
                     ? (unsigned)fc.num_channels : (unsigned)fc.channel_num + 1);
    }
    // The shareable network URL (channels + LoRa config, the app's QR form).
    else if (!strcmp(key, "channel_url")) {
        uint8_t pb[512];
        size_t pblen = channel_set_encode(pb, sizeof(pb));
        if (pblen == 0) { H->log("channel_url: encode failed"); return 0; }
        char b64[700];
        b64url_encode(pb, (int)pblen, b64, (int)sizeof(b64));
        n = snprintf(out, out_sz, "https://meshtastic.org/e/#%s", b64);
    }
    else if (!strcmp(key, "public_key") || !strcmp(key, "private_key")) {
        const uint8_t* k = (key[1] == 'u') ? S.pki.public_key : S.pki.private_key;
        for (int i = 0; i < 32 && n < out_sz - 3; i++)
            n += snprintf(out + n, out_sz - n, "%02x", k[i]);
    }
    return (n > 0 && n < out_sz) ? n : (n > 0 ? out_sz - 1 : 0);
}

static bool mt_set_config(const char* key, const char* val) {
    if (!key || !val) return false;
    // Command keys (never persisted) — user-initiated protocol actions.
    if (!strcmp(key, "request_nodeinfo")) {
        // Peers-list action for an unknown node: ask it to introduce itself
        // (our NodeInfo unicast, want_response). Deliberately user-initiated —
        // auto-requesting every stranger heard would be mesh spam.
        uint32_t to;
        if (!node_by_name(val, &to)) {
            H->log("request_nodeinfo: '%s' not resolvable", val);
            return false;
        }
        H->log("request_nodeinfo: user tap for !%08x", (unsigned)to);
        if (!nodeinfo_tx_allowed(to)) return false;   // per-node 30s rate limit (logs)
        queue_nodeinfo_to(to, true);
        return true;
    }
    if (!strcmp(key, "broadcast_nodeinfo")) {
        // My-node action: announce ourselves now (name + public key), the
        // MeshCore advert equivalent.
        queue_nodeinfo();
        H->log("nodeinfo broadcast: user tap");
        return true;
    }
    if (!strcmp(key, "set_private_key")) {
        // Import a private key (64 hex chars); the public key re-derives as
        // X25519(prv, basepoint). Clamping is the x25519 rule — a no-op on
        // keys that were generated clamped. Disk-only, same rule as
        // regenerate_identity: active after reboot.
        uint8_t prv[32];
        if (strlen(val) != 64 || hex_decode(val, prv, 32) != 32) {
            H->log("set_private_key: need 64 hex chars");
            return false;
        }
        prv[0] &= 248; prv[31] &= 127; prv[31] |= 64;
        uint8_t pub[32];
        static const uint8_t BASE[32] = { 9 };
        if (!mesh_x25519_dh(prv, BASE, pub)) {
            H->log("set_private_key: derivation failed");
            return false;
        }
        MeshPkiIdentity fresh;
        fresh.init();
        memcpy(fresh.private_key, prv, 32);
        memcpy(fresh.public_key, pub, 32);
        fresh.initialized = true;
        MeshPkiIdentity keep = S.pki;
        S.pki = fresh;
        pki_save_to(s_dir);
        pki_save_to(s_data);
        S.pki = keep;
        H->log("private key imported (on disk; active after reboot)");
        return true;
    }
    if (!strcmp(key, "regenerate_identity")) {
        // The new keypair lands on DISK only; the RUNNING identity (and the
        // node number derived from it) stays consistent until reboot — a
        // live swap would broadcast the new key under the old address.
        MeshPkiIdentity fresh;
        fresh.init();
        if (!fresh.generate()) { H->log("regenerate: keypair generation failed"); return false; }
        MeshPkiIdentity keep = S.pki;
        S.pki = fresh;
        pki_save_to(s_dir);
        pki_save_to(s_data);
        S.pki = keep;
        H->log("identity regenerated (on disk; active after reboot)");
        return true;
    }
    // Per-channel notify mode: its own file (<s_data>/notify), not the cfg
    // chain below — same audit-log rule as every accepted config write.
    if (!strncmp(key, "notify_ch:", 10)) {
        if (!notify_mode_set(key + 10, atoi(val))) return false;
        H->log("config saved: %s=%s", key, val);
        return true;
    }
    // Channel management. Applies LIVE (channels never touch radio params:
    // the RX decrypt walk and TX index lookup read the table directly) and
    // persists to <s_data>/channels.
    //   channel_add: "<name>" or "<name>\t<psk-b64>" (empty psk = inherit
    //                primary; "AQ==" = the well-known default key)
    //   channel_del: "<name>" (never the primary)
    //   channel_psk: "<name>\t<psk-b64>" (primary allowed — the private-
    //                primary configuration)
    if (!strcmp(key, "channel_url")) {
        // Accepts the full https://meshtastic.org/e/#<b64> form or the bare
        // fragment; base64url and standard alphabets both decode.
        const char* frag = strrchr(val, '#');
        frag = frag ? frag + 1 : val;
        uint8_t pb[512];
        int pblen = b64_decode(frag, pb, (int)sizeof(pb));
        if (pblen <= 0) { H->log("channel_url: bad base64"); return false; }
        return channel_set_import(pb, (size_t)pblen);
    }
    if (!strcmp(key, "channel_add")) {
        char name[32];
        uint8_t psk[34];
        int psk_len = 0;
        const char* tab = strchr(val, '\t');
        size_t nl = tab ? (size_t)(tab - val) : strlen(val);
        if (nl == 0 || nl >= sizeof(name)) return false;
        memcpy(name, val, nl); name[nl] = 0;
        if (strchr(name, '\n')) return false;
        if (tab && tab[1]) {
            psk_len = b64_decode(tab + 1, psk, (int)sizeof(psk));
            if (psk_len < 0 || psk_len > 32) { H->log("channel_add: bad psk base64"); return false; }
        }
        if (S.channels.count >= MESH_MAX_CHANNELS) { H->log("channel_add: table full (8)"); return false; }
        if (channel_idx_by_name(name) >= 0) { H->log("channel_add: '%s' exists", name); return false; }
        int idx = S.channels.addChannel(name, psk, (uint8_t)psk_len, S.channels.count == 0);
        if (idx < 0) return false;
        channels_save();
        H->log("channel added '%s' idx %d hash %02x", name, idx,
               (unsigned)S.channels.channels[idx].hash);
        return true;
    }
    if (!strcmp(key, "channel_del")) {
        int idx = channel_idx_by_name(val);
        if (idx < 0) return false;
        if (idx == 0) { H->log("channel_del: primary is not deletable"); return false; }
        ChanRow rows[MESH_MAX_CHANNELS];
        int n;
        channels_snapshot(rows, &n);
        for (int i = idx; i < n - 1; i++) rows[i] = rows[i + 1];
        channels_rebuild(rows, n - 1);
        H->log("channel deleted '%s'", val);
        return true;
    }
    if (!strcmp(key, "channel_psk")) {
        const char* tab = strchr(val, '\t');
        if (!tab) return false;
        char name[32];
        size_t nl = (size_t)(tab - val);
        if (nl == 0 || nl >= sizeof(name)) return false;
        memcpy(name, val, nl); name[nl] = 0;
        uint8_t psk[34];
        int psk_len = tab[1] ? b64_decode(tab + 1, psk, (int)sizeof(psk)) : 0;
        if (psk_len < 0 || psk_len > 32) { H->log("channel_psk: bad psk base64"); return false; }
        int idx = channel_idx_by_name(name);
        if (idx < 0) return false;
        ChanRow rows[MESH_MAX_CHANNELS];
        int n;
        channels_snapshot(rows, &n);
        memcpy(rows[idx].psk, psk, sizeof(rows[idx].psk));
        rows[idx].psk_len = (uint8_t)psk_len;
        channels_rebuild(rows, n);
        H->log("channel '%s' psk changed (%dB) hash %02x", name, psk_len,
               (unsigned)S.channels.channels[idx].hash);
        return true;
    }
    if      (!strcmp(key, "region")) { int n = atoi(val); if (n < 0 || n >= REGION_COUNT) return false; CFG.region = (uint8_t)n; }
    else if (!strcmp(key, "preset")) { int n = atoi(val); if (n < 0 || n >= MODEM_PRESET_COUNT) return false; CFG.preset = (uint8_t)n; }
    else if (!strcmp(key, "role")) { int n = atoi(val); if (n < 0 || n > 2) return false; CFG.role = (uint8_t)n; S.role = (MeshRole)CFG.role; }
    else if (!strcmp(key, "pos_precision")) { int n = atoi(val); if (n != 0 && (n < 10 || n > 32)) return false; CFG.pos_precision = (uint8_t)n; }
    else if (!strcmp(key, "hop_limit")) { int n = atoi(val); if (n < 1 || n > 7) return false; CFG.hop_limit = (uint8_t)n; }
    else if (!strcmp(key, "tx_power"))  { int n = atoi(val); if (n < 1 || n > 30) return false; CFG.tx_power = (int8_t)n; }
    else if (!strcmp(key, "freq_slot")) { int n = atoi(val); if (n < 0 || n > 1000) return false; CFG.freq_slot = (int16_t)n; S.freq_slot = n > 0 ? n - 1 : -1; }
    else if (!strcmp(key, "freq_override")) {
        float x = (float)atof(val);
        if (x != 0.0f && (x < 150.0f || x > 960.0f)) return false;  // SX1262 window
        CFG.freq_override = x;
    }
    else if (!strcmp(key, "ok_to_mqtt")) { int n = atoi(val); if (n != 0 && n != 1) return false; CFG.ok_to_mqtt = (uint8_t)n; }
    else if (!strcmp(key, "nodeinfo_mins")) { int n = atoi(val); if (n < 5 || n > 1440) return false; CFG.nodeinfo_mins = (uint16_t)n; }
    else if (!strcmp(key, "pos_mins")) { int n = atoi(val); if (n < 1 || n > 1440) return false; CFG.pos_mins = (uint16_t)n; }
    else if (!strcmp(key, "hw_model")) {
        // Host-forwarded board identity (never persisted — the host is the
        // truth every boot).
        int n = atoi(val); if (n < 0 || n > 1000) return false;
        s_hw_model = (uint16_t)n;
        return true;
    }
    else if (!strcmp(key, "long_name") && val[0])  { strncpy(CFG.long_name, val, sizeof(CFG.long_name) - 1); CFG.long_name[sizeof(CFG.long_name)-1] = 0; }
    else if (!strcmp(key, "short_name") && val[0]) { strncpy(CFG.short_name, val, sizeof(CFG.short_name) - 1); CFG.short_name[sizeof(CFG.short_name)-1] = 0; }
    else return false;
    cfg_save();
    // Audit trail: a region once changed with no record of who or when —
    // every accepted config write names itself now.
    H->log("config saved: %s=%s", key, val);
    // Radio parameters apply on the next boot (the settings app says so).
    return true;
}

// ── Protocol-registered Lua bindings (ABI v2) ────────────────────────────────
// Registered into every fresh lua_State by mt_lua_open. The functions live in
// module text: they die with the PROTOCOL, never with Lua — a teardown drops the
// whole state and the next bring-up re-registers. First surface: the DM
// delivery status as a direct call (the dm_status config key stays for the
// older path).
static int lua_mtlite_dm_status(lua_State* L) {
    const char* s = s_pdm.state == 1 ? "pending" :
                    s_pdm.state == 2 ? "delivered" :
                    s_pdm.state == 3 ? "failed" : "idle";
    lua_pushstring(L, s);
    return 1;
}

// Firmware store summaries push (mesh_store.h): MStoreChanRef is POD by
// contract (int + char[32]); the mangled import resolves via proto_exports.
struct MStoreChanRef { int idx; char name[32]; };
namespace mstore {
    int push_msg_summaries(lua_State* L, const MStoreChanRef* chans, int nch);
}

// Override of the firmware's _store_summaries (registered first with a
// DM-threads-only fallback; the ACTIVE protocol supplies the channel table —
// the Messenger inbox needs channel rows). No MESH_LOCK: the channel table
// is only ever mutated from Core 0 (set_config), same core as Lua, and the
// store contract keeps lua pushes lock-free.
static int lua_mt_store_summaries(lua_State* L) {
    MStoreChanRef chans[MESH_MAX_CHANNELS];
    int nch = 0;
    for (uint8_t i = 0; i < S.channels.count; i++) {
        chans[nch].idx = (int)i;
        strncpy(chans[nch].name, S.channels.effectiveName(i), sizeof(chans[nch].name) - 1);
        chans[nch].name[sizeof(chans[nch].name) - 1] = 0;
        nch++;
    }
    return mstore::push_msg_summaries(L, chans, nch);
}

static void mt_lua_open(void* L_raw) {
    lua_State* L = (lua_State*)L_raw;
    lua_pushcfunction(L, lua_mtlite_dm_status);
    lua_setglobal(L, "_mtlite_dm_status");
    lua_pushcfunction(L, lua_mt_store_summaries);
    lua_setglobal(L, "_store_summaries");
    H->log("lua bindings registered (_mtlite_dm_status, _store_summaries)");
}

extern "C" const LoraProtoOps loraproto_ops = {
    LORA_PROTO_ABI_VERSION,
    "mtlite",
    "MTLite",
    mt_init,
    mt_start,
    mt_loop,
    mt_stop,
    mt_send_channel_text,
    mt_send_text,
    mt_get_peers,
    mt_get_config,
    mt_set_config,
    nullptr,   // prepare_sleep: none needed — polled model, DIO1 wake is chip-level
    nullptr,   // note_wake
    mt_flush,  // peers write-behind lands on file; cfg/pki persist at write time
    mt_lua_open,  // v2: protocol-registered Lua bindings
    nullptr,      // lua_close: nothing holds a lua_State reference
    nullptr,      // lua_tick: no push events yet — apps poll the store
};
