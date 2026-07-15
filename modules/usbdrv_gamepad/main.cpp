// Dynamic USB driver: HID GAMEPAD, descriptor-driven (match 03/00/*).
//
// At probe the driver fetches the interface's HID REPORT DESCRIPTOR and
// parses it into an exact field table (buttons / hat switch / axes, each
// with bit position, size and logical range) — no report-layout guessing
// anywhere. Pads whose descriptor can't be parsed are REJECTED with the
// reason logged; there is deliberately no heuristic fallback.
//
// Per input report the driver extracts every field, normalizes it (axes ->
// 0..255, hats -> dir 0-7 or 0xFF null), publishes a 4-byte semantic EVENT
// on every field state change ('E', kind, id, val) — the Gamepad app's
// mapping feed — and evaluates the mapping rules:
//
// conf format (text, one rule per line, numbers hex, '#' comments):
//   BTN <n> <code>            button n (descriptor numbering, 1-based)
//   HAT <dir> <code>          dir 0-7 clockwise from up; cardinal (even)
//                             dirs match +-45° so dpad diagonals fire both
//                             components; diagonal (odd) dirs match exactly
//   AX <usage> <half> <code>  axis by usage (30=X 31=Y 32=Z 33=Rx 34=Ry
//                             35=Rz 36=Slider 37=Dial); half L or H; held
//                             when deflected past the midpoint between the
//                             axis's rest value (first report) and that rail
// Old byte-oriented rules (B/H/T lines) are ignored and counted in the log.
//
// With no conf, a DEFAULT mapping is generated from the fields the pad
// actually has (dpad hat -> WASD, X/Y axes -> trackball nav, buttons ->
// Enter/'m'/'n'/'e'/Space/Click/Backspace/Enter — see gen_defaults) so a
// fresh pad works in the launcher and WASD games unmapped.
//
// Emitted codes are the firmware pseudo-code space, routed like the kbd
// driver: in-game input_key edges (pre-keymap, so launcher bindings apply);
// keyboard codes < 0x80 additionally ride the UI held-image
// (input_set_held -> kb_key_state); trackball codes -> input_nav (+repeat).

#include <string.h>
#include <stdio.h>
#include "usb_shim.h"
#include "../../src/usb/usb_driver_abi.h"

extern "C" const UsbDriverDesc usbdrv_ops;   // self — for config()/publish()

#define ulog(...) do { if (s_hapi) s_hapi->log(__VA_ARGS__); } while (0)

#define MAX_FIELDS    40
#define MAX_RULES     32
#define MAX_HELD      16
#define MAX_USAGES    32
#define MAX_REPIDS    4
#define GSTACK        4
#define RDESC_MAX     512
#define REPEAT_DELAY  400
#define REPEAT_MS     150

#define HID_DESC_TYPE   0x21
#define FIELD_ABSENT    ((int32_t)0x80000000)

struct PadProfile {
    bool     valid;
    uint8_t  ifnum;
    uint8_t  ep;
    uint16_t mps;
};
static PadProfile s_prof;

// ── Field table (from the HID report descriptor) ─────────────────────────────

enum { FK_BTN = 0, FK_HAT = 1, FK_AXIS = 2 };

struct PadField {
    uint8_t  kind;      // FK_*
    uint8_t  rep_id;    // 0 when the pad uses no report IDs
    uint8_t  bit_size;
    uint8_t  id;        // BTN: button number; AXIS: usage 0x30..; HAT: index
    uint16_t bit_off;   // within the payload (after the ID byte, if any)
    int32_t  lmin, lmax;
};
static PadField s_fields[MAX_FIELDS];
static int      s_nfields   = 0;
static bool     s_has_repid = false;

static uint8_t  s_rdesc[RDESC_MAX];
static uint16_t s_rdesc_len = 0;

// ── Rules ────────────────────────────────────────────────────────────────────

struct Rule {
    uint8_t kind;    // FK_*
    uint8_t id;      // BTN: button number; AXIS: usage; HAT: unused
    uint8_t p;       // HAT: dir 0-7; AXIS: 0=low 1=high
    uint8_t code;
    int8_t  fidx;    // bound field index, -1 = unbound (rule inert)
};
static Rule s_rules[MAX_RULES];
static int  s_nrules   = 0;
static bool s_defaults = false;

// ── Runtime ──────────────────────────────────────────────────────────────────

static const UsbHostApi* s_hapi = 0;
static UsbPipe*          s_pipe = 0;
static volatile bool     s_on   = false;

// Per-field state: BTN 0/1; HAT dir or 0xFF; AXIS zone 0=center 1=low 2=high.
static uint8_t s_fstate[MAX_FIELDS];
static uint8_t s_frest[MAX_FIELDS];      // axis rest (normalized, 1st report)
static bool    s_frest_ok[MAX_FIELDS];

static uint8_t  s_held[MAX_HELD];        // codes currently held (deduped)
static int      s_nheld = 0;
static uint8_t  s_arrows = 0;            // bit0..3 up/down/left/right (UI repeat)
static uint32_t s_arr_t0 = 0, s_arr_last = 0;

// ── HID report descriptor parser ─────────────────────────────────────────────
// Short items only. Tracks the globals/locals state machine and allocates
// input-report bits in declaration order; Constant and Array items advance
// the bit cursor without creating fields, Output/Feature items don't touch
// it. The bit cursor math here is the load-bearing part: one miscounted
// padding field shifts everything after it — the probe log prints the whole
// table so a misparse is visible on first plug.

struct HidGlobals {
    uint16_t page;
    int32_t  lmin, lmax;
    uint32_t lmax_raw;
    uint8_t  rsize, rep_id;
    uint16_t rcount;
};

static bool parse_rdesc(const uint8_t* d, uint16_t len, const char** err) {
    HidGlobals g; memset(&g, 0, sizeof(g));
    HidGlobals stk[GSTACK]; int sp = 0;
    uint32_t usages[MAX_USAGES]; int nus = 0;
    uint32_t umin = 0; bool have_umin = false;
    struct { uint8_t id; uint16_t bits; } cur[MAX_REPIDS];
    int ncur = 0, nhats = 0;

    s_nfields   = 0;
    s_has_repid = false;

    const uint8_t* p   = d;
    const uint8_t* end = d + len;
    while (p < end) {
        uint8_t pre = *p++;
        if (pre == 0xFE) { *err = "long item"; return false; }
        uint8_t tag  = pre >> 4;
        uint8_t type = (pre >> 2) & 3;
        uint8_t sz   = pre & 3;
        if (sz == 3) sz = 4;
        if (p + sz > end) { *err = "truncated item"; return false; }
        uint32_t v = 0;
        for (uint8_t i = 0; i < sz; i++) v |= (uint32_t)p[i] << (8 * i);
        int32_t sv = 0;
        if      (sz == 1) sv = (int8_t)v;
        else if (sz == 2) sv = (int16_t)v;
        else              sv = (int32_t)v;
        p += sz;

        if (type == 1) {                                   // Global
            switch (tag) {
                case 0x0: g.page  = (uint16_t)v; break;
                case 0x1: g.lmin  = sv; break;
                case 0x2: g.lmax  = sv; g.lmax_raw = v; break;
                case 0x7: g.rsize = (uint8_t)v; break;
                case 0x8: g.rep_id = (uint8_t)v; s_has_repid = true; break;
                case 0x9: g.rcount = (uint16_t)v; break;
                case 0xA: if (sp < GSTACK) stk[sp++] = g; break;
                case 0xB: if (sp > 0) g = stk[--sp]; break;
                default:  break;
            }
        } else if (type == 2) {                            // Local
            // 4-byte usages carry their page in the high 16 bits.
            uint32_t qu = (sz == 4) ? v : (((uint32_t)g.page << 16) | v);
            switch (tag) {
                case 0x0:
                    if (nus < MAX_USAGES) usages[nus++] = qu;
                    break;
                case 0x1: umin = qu; have_umin = true; break;
                case 0x2:
                    if (have_umin) {
                        for (uint32_t u = umin; u <= qu && nus < MAX_USAGES; u++)
                            usages[nus++] = u;
                        have_umin = false;
                    }
                    break;
                default: break;
            }
        } else if (type == 0) {                            // Main
            if (tag == 0x8) {                              // Input
                int ci = -1;
                for (int i = 0; i < ncur; i++)
                    if (cur[i].id == g.rep_id) { ci = i; break; }
                if (ci < 0) {
                    if (ncur >= MAX_REPIDS) { *err = "too many report ids"; return false; }
                    cur[ncur].id = g.rep_id; cur[ncur].bits = 0; ci = ncur++;
                }
                bool constant = (v & 1) != 0;
                bool variable = (v & 2) != 0;
                // 0xFF-style unsigned logical max sign-extends negative;
                // when lmin >= 0 the field is unsigned — use the raw value.
                int32_t lmax = g.lmax;
                if (g.lmin >= 0 && lmax < g.lmin) lmax = (int32_t)g.lmax_raw;

                if (constant || !variable) {
                    // Padding, or array-style fields we don't map:
                    // bits are consumed either way.
                    cur[ci].bits = (uint16_t)(cur[ci].bits + g.rsize * g.rcount);
                } else {
                    for (uint16_t k = 0; k < g.rcount; k++) {
                        uint32_t u = 0;
                        if (nus > 0)
                            u = usages[(k < (uint16_t)nus) ? k : (uint16_t)(nus - 1)];
                        uint16_t up = (uint16_t)(u >> 16);
                        uint16_t us = (uint16_t)(u & 0xFFFF);
                        int kind = -1; uint8_t id = 0;
                        if (up == 0x09 && g.rsize == 1 && us >= 1 && us <= 0xFF) {
                            kind = FK_BTN;  id = (uint8_t)us;
                        } else if (up == 0x01 && us == 0x39 &&
                                   g.rsize >= 2 && g.rsize <= 8) {
                            kind = FK_HAT;  id = (uint8_t)nhats++;
                        } else if (up == 0x01 && us >= 0x30 && us <= 0x37 &&
                                   g.rsize >= 2 && g.rsize <= 16 && lmax > g.lmin) {
                            kind = FK_AXIS; id = (uint8_t)us;
                        }
                        if (kind >= 0 && s_nfields < MAX_FIELDS) {
                            PadField* f = &s_fields[s_nfields++];
                            f->kind     = (uint8_t)kind;
                            f->rep_id   = g.rep_id;
                            f->bit_off  = cur[ci].bits;
                            f->bit_size = g.rsize;
                            f->id       = id;
                            f->lmin     = g.lmin;
                            f->lmax     = lmax;
                        }
                        cur[ci].bits = (uint16_t)(cur[ci].bits + g.rsize);
                    }
                }
                if (cur[ci].bits > 2048) { *err = "bit cursor overflow"; return false; }
            }
            // Every Main item (Input/Output/Feature/Collection/End) resets
            // the locals; Output/Feature never touch the input cursors.
            nus = 0; have_umin = false;
        }
    }
    if (s_nfields == 0) { *err = "no usable fields"; return false; }
    return true;
}

static void log_fields(void) {
    ulog("gamepad: %d field(s)%s", s_nfields, s_has_repid ? " [report IDs]" : "");
    for (int i = 0; i < s_nfields; i++) {
        const PadField* f = &s_fields[i];
        const char* kn = (f->kind == FK_BTN) ? "btn "
                       : (f->kind == FK_HAT) ? "hat " : "axis";
        ulog("  %s %02X: rep %u bits %u+%u range %d..%d",
             kn, f->id, f->rep_id, f->bit_off, f->bit_size,
             (int)f->lmin, (int)f->lmax);
    }
}

// ── Field extraction + normalization ─────────────────────────────────────────

static uint32_t get_bits(const uint8_t* r, uint32_t len, uint16_t off, uint8_t n) {
    uint32_t v = 0;
    for (uint8_t i = 0; i < n; i++) {
        uint16_t b  = (uint16_t)(off + i);
        uint32_t by = b >> 3;
        if (by >= len) break;
        v |= (uint32_t)((r[by] >> (b & 7)) & 1) << i;
    }
    return v;
}

// Raw (sign-extended) field value, or FIELD_ABSENT when this report doesn't
// carry the field (different report ID / too short).
static int32_t field_val(const PadField* f, const uint8_t* r, uint32_t len) {
    if (s_has_repid) {
        if (len < 1 || r[0] != f->rep_id) return FIELD_ABSENT;
        r++; len--;
    }
    if ((uint32_t)((f->bit_off + f->bit_size + 7) >> 3) > len) return FIELD_ABSENT;
    uint32_t raw = get_bits(r, len, f->bit_off, f->bit_size);
    int32_t v = (int32_t)raw;
    if (f->lmin < 0 && f->bit_size < 32) {
        uint32_t sb = 1u << (f->bit_size - 1);
        if (raw & sb) v = (int32_t)(raw - (sb << 1));
    }
    return v;
}

static uint8_t axis_norm(const PadField* f, int32_t v) {
    if (v < f->lmin) v = f->lmin;
    if (v > f->lmax) v = f->lmax;
    return (uint8_t)(((v - f->lmin) * 255) / (f->lmax - f->lmin));
}

// Zone thresholds sit at the midpoint between rest and each rail, so they're
// far from both rest jitter and the rail; no hysteresis needed in practice.
// A rail-resting axis (trigger) only has the opposite zone.
static uint8_t axis_zone(uint8_t norm, uint8_t rest) {
    if (rest >= 0x20 && norm <= (uint8_t)(rest / 2)) return 1;
    uint8_t hi = (uint8_t)(rest + (uint8_t)((255 - rest) / 2));
    if (rest <= 0xE0 && norm >= hi) return 2;
    return 0;
}

// ── Conf parsing / defaults / binding ────────────────────────────────────────

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int hextok(const char** p) {
    const char* s = *p;
    while (*s == ' ' || *s == '\t') s++;
    int v = -1, d;
    while ((d = hexval(*s)) >= 0) { v = (v < 0 ? 0 : v * 16) + d; s++; }
    *p = s;
    return v;
}

// Case-insensitive keyword at p, with a word boundary after it.
static bool word_at(const char* p, const char* w) {
    while (*w) {
        char c = *p++;
        if (c >= 'a' && c <= 'z') c = (char)(c - 32);
        if (c != *w++) return false;
    }
    char c = *p;
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
        (c >= '0' && c <= '9')) return false;
    return true;
}

static void rule_add(uint8_t kind, uint8_t id, uint8_t pp, uint8_t code) {
    if (s_nrules >= MAX_RULES) return;
    s_rules[s_nrules].kind = kind;
    s_rules[s_nrules].id   = id;
    s_rules[s_nrules].p    = pp;
    s_rules[s_nrules].code = code;
    s_rules[s_nrules].fidx = -1;
    s_nrules++;
}

static void parse_conf(const char* conf, int* old_lines) {
    const char* p = conf;
    while (*p && s_nrules < MAX_RULES) {
        while (*p == ' ' || *p == '\t') p++;
        if (word_at(p, "BTN")) {
            const char* q = p + 3;
            int n = hextok(&q), code = hextok(&q);
            if (n >= 1 && n < 256 && code > 0 && code < 256)
                rule_add(FK_BTN, (uint8_t)n, 0, (uint8_t)code);
        } else if (word_at(p, "HAT")) {
            const char* q = p + 3;
            int dir = hextok(&q), code = hextok(&q);
            if (dir >= 0 && dir <= 7 && code > 0 && code < 256)
                rule_add(FK_HAT, 0, (uint8_t)dir, (uint8_t)code);
        } else if (word_at(p, "AX")) {
            const char* q = p + 2;
            int usage = hextok(&q);
            while (*q == ' ' || *q == '\t') q++;
            int half = (*q == 'H' || *q == 'h') ? 1
                     : (*q == 'L' || *q == 'l') ? 0 : -1;
            if (half >= 0) q++;
            int code = hextok(&q);
            if (usage >= 0x30 && usage <= 0x37 && half >= 0 &&
                code > 0 && code < 256)
                rule_add(FK_AXIS, (uint8_t)usage, (uint8_t)half, (uint8_t)code);
        } else if (*p == 'B' || *p == 'b' || *p == 'H' || *p == 'h' ||
                   *p == 'T' || *p == 't') {
            (*old_lines)++;                  // pre-descriptor rule format
        }
        while (*p && *p != '\n') p++;        // '#' and blank lines skip here
        if (*p == '\n') p++;
    }
}

// Default mapping (no conf): Phil's baseline profile — dpad -> WASD,
// sticks -> trackball nav, buttons -> Enter/'m'/'n'/'e'/Space/Click/
// Backspace/Enter. Only rules whose control exists on the pad are
// generated. Mirrored by DEFAULT_RULES in
// data/lua/apps/Games/Gamepad/main.lua — KEEP THE TWO IN SYNC.
static void gen_defaults(void) {
    bool hat_done = false, x_done = false, y_done = false;
    static const uint8_t btn_map[][2] = {
        { 0x01, 0x0D },   // Enter
        { 0x02, 0x6D },   // 'm'
        { 0x03, 0x6E },   // 'n'
        { 0x04, 0x65 },   // 'e'
        { 0x06, 0x20 },   // Space
        { 0x08, 0x85 },   // Click
        { 0x09, 0x08 },   // Backspace
        { 0x0A, 0x0D },   // Enter
    };
    for (int i = 0; i < s_nfields; i++) {
        const PadField* f = &s_fields[i];
        if (f->kind == FK_HAT && !hat_done) {
            hat_done = true;
            rule_add(FK_HAT, 0, 0, 0x77);    // up    -> 'w'
            rule_add(FK_HAT, 0, 2, 0x64);    // right -> 'd'
            rule_add(FK_HAT, 0, 4, 0x73);    // down  -> 's'
            rule_add(FK_HAT, 0, 6, 0x61);    // left  -> 'a'
        } else if (f->kind == FK_AXIS && f->id == 0x30 && !x_done) {
            x_done = true;
            rule_add(FK_AXIS, 0x30, 0, 0x83);    // X low  -> trackball left
            rule_add(FK_AXIS, 0x30, 1, 0x84);    // X high -> trackball right
        } else if (f->kind == FK_AXIS && f->id == 0x31 && !y_done) {
            y_done = true;
            rule_add(FK_AXIS, 0x31, 0, 0x81);    // Y low  -> trackball up
            rule_add(FK_AXIS, 0x31, 1, 0x82);    // Y high -> trackball down
        } else if (f->kind == FK_BTN) {
            for (unsigned m = 0; m < sizeof(btn_map) / sizeof(btn_map[0]); m++) {
                if (btn_map[m][0] == f->id) {
                    rule_add(FK_BTN, f->id, 0, btn_map[m][1]);
                    break;
                }
            }
        }
    }
}

static void bind_rules(void) {
    for (int i = 0; i < s_nrules; i++) {
        Rule* ru = &s_rules[i];
        ru->fidx = -1;
        for (int j = 0; j < s_nfields; j++) {
            const PadField* f = &s_fields[j];
            if (f->kind != ru->kind) continue;
            if (ru->kind == FK_HAT || f->id == ru->id) {   // first hat wins
                ru->fidx = (int8_t)j;
                break;
            }
        }
        if (ru->fidx < 0)
            ulog("gamepad: rule for missing control ignored (kind %u id %02X)",
                 ru->kind, ru->id);
    }
}

// ── Held-set evaluation + edge routing ───────────────────────────────────────

static bool in_set(const uint8_t* set, int n, uint8_t c) {
    for (int i = 0; i < n; i++) if (set[i] == c) return true;
    return false;
}

static void route_edge(uint8_t code, bool pressed, bool game) {
    if (game) {
        s_hapi->input_key(code, pressed);
        return;
    }
    // UI: arrows step focus (with held-repeat via tick); click = Enter.
    if (code >= 0x81 && code <= 0x84) {
        uint8_t bit = (uint8_t)(1 << (code - 0x81));   // up/down/left/right
        if (pressed) {
            s_arrows |= bit;
            s_hapi->input_nav(code == 0x81 ? 1 : 0, code == 0x82 ? 1 : 0,
                              code == 0x83 ? 1 : 0, code == 0x84 ? 1 : 0, 0);
            s_arr_t0 = s_hapi->ticks_ms();
            s_arr_last = s_arr_t0;
        } else {
            s_arrows &= (uint8_t)~bit;
        }
    } else if (code == 0x85 && pressed) {
        s_hapi->input_nav(0, 0, 0, 0, 1);
    }
    // other codes >= 0x80 are module-only; < 0x80 ride the held-image below
}

static bool hat_match(uint8_t dir, uint8_t s) {
    if (s > 7) return false;                    // null / centered
    if (dir & 1) return s == dir;               // diagonal: exact
    return s == dir || s == ((uint8_t)(dir + 1) & 7)
                    || s == ((uint8_t)(dir + 7) & 7);
}

static void ev_publish(uint8_t kind, uint8_t id, uint8_t val) {
    uint8_t e[4] = { 'E', kind, id, val };
    s_hapi->publish(&usbdrv_ops, e, 4);
}

static void pad_report(const uint8_t* r, uint32_t len) {
    if (len > 64) len = 64;

    // Extract every field; publish an event per state change.
    for (int i = 0; i < s_nfields; i++) {
        const PadField* f = &s_fields[i];
        int32_t v = field_val(f, r, len);
        if (v == FIELD_ABSENT) continue;
        uint8_t ns;
        if (f->kind == FK_BTN) {
            ns = (uint8_t)(v & 1);
        } else if (f->kind == FK_HAT) {
            int32_t dir  = v - f->lmin;
            int32_t span = f->lmax - f->lmin + 1;
            if (v < f->lmin || dir >= span || dir > 7) ns = 0xFF;
            else ns = (uint8_t)((span == 4) ? dir * 2 : dir);
        } else {
            uint8_t nv = axis_norm(f, v);
            if (!s_frest_ok[i]) { s_frest[i] = nv; s_frest_ok[i] = true; }
            ns = axis_zone(nv, s_frest[i]);
        }
        if (ns != s_fstate[i]) {
            s_fstate[i] = ns;
            ev_publish(f->kind, f->id, ns);
        }
    }

    // Evaluate rules -> desired held-code set (deduped).
    uint8_t want[MAX_HELD];
    int nwant = 0;
    for (int i = 0; i < s_nrules && nwant < MAX_HELD; i++) {
        const Rule* ru = &s_rules[i];
        if (ru->fidx < 0) continue;
        uint8_t s = s_fstate[ru->fidx];
        bool hit;
        if      (ru->kind == FK_BTN) hit = (s == 1);
        else if (ru->kind == FK_HAT) hit = hat_match(ru->p, s);
        else                         hit = (s == (uint8_t)(ru->p ? 2 : 1));
        if (hit && !in_set(want, nwant, ru->code)) want[nwant++] = ru->code;
    }

    // UI image: keyboard codes ride the same held-image channel the USB
    // keyboard uses (merged into kb_key_state by keyboard_read_cb), so a
    // pad-mapped key behaves exactly like that key on the T-Deck in the UI
    // (WASD nav, typing, Enter). Codes >= 0x80 (trackball) stay on the
    // nav path in route_edge.
    bool held[128] = {};
    int  nheld = 0;
    for (int i = 0; i < nwant; i++)
        if (want[i] < 128) { held[want[i]] = true; nheld++; }
    s_hapi->input_set_held(held, nheld);

    // Diff vs previous held set -> edges.
    bool game = s_hapi->input_module_active();
    for (int i = 0; i < s_nheld; i++)
        if (!in_set(want, nwant, s_held[i])) route_edge(s_held[i], false, game);
    for (int i = 0; i < nwant; i++)
        if (!in_set(s_held, s_nheld, want[i])) route_edge(want[i], true, game);
    memcpy(s_held, want, nwant);
    s_nheld = nwant;
}

static void pad_all_up(void) {
    static const bool none[128] = {};
    s_hapi->input_set_held(none, 0);
    bool game = s_hapi->input_module_active();
    for (int i = 0; i < s_nheld; i++) route_edge(s_held[i], false, game);
    s_nheld = 0;
    s_arrows = 0;
}

// ── Report pipe ──────────────────────────────────────────────────────────────

static void pad_pipe_cb(UsbPipe* p, UsbXferResult res, uint32_t actual, void*) {
    if (res == USB_XFER_OK && actual >= 1)
        pad_report(s_hapi->pipe_buf(p), actual);
    if (s_on && !s_hapi->flash_guard_pending()
             && res != USB_XFER_NO_DEVICE
             && res != USB_XFER_CANCELED) {
        if (!s_hapi->pipe_submit(p, s_prof.mps, pad_pipe_cb, 0))
            ulog("pad: resubmit failed");
    }
}

// ── Lifecycle ────────────────────────────────────────────────────────────────

static bool pad_probe(const UsbHostApi* api) {
    s_hapi = api;
    memset(&s_prof, 0, sizeof(s_prof));
    s_on = false;
    s_nheld = 0;
    s_arrows = 0;
    s_rdesc_len = 0;
    if (s_pipe) { api->pipe_close(s_pipe); s_pipe = 0; }

    const usb_config_desc_t* cfg = (const usb_config_desc_t*)api->config_desc();
    if (!cfg) return false;

    bool     cur_pad   = false;
    uint8_t  cur_if    = 0;
    uint16_t cur_rdesc = 0;
    const usb_standard_desc_t* d = (const usb_standard_desc_t*)cfg;
    int offset = 0;
    while ((d = usb_parse_next_descriptor(d, cfg->wTotalLength, &offset)) != 0) {
        if (d->bDescriptorType == USB_B_DESCRIPTOR_TYPE_INTERFACE) {
            const usb_intf_desc_t* i = (const usb_intf_desc_t*)d;
            // HID, NON-boot subclass (keyboards/mice claim subclass 1).
            cur_pad = (i->bInterfaceClass == USB_CLASS_HID &&
                       i->bInterfaceSubClass == 0 &&
                       i->bAlternateSetting == 0);
            cur_if = i->bInterfaceNumber;
            cur_rdesc = 0;
        } else if (d->bDescriptorType == HID_DESC_TYPE && cur_pad) {
            // HID class descriptor: [6] = report desc type, [7..8] = length.
            const uint8_t* hb = (const uint8_t*)d;
            if (hb[0] >= 9 && hb[6] == 0x22 && cur_rdesc == 0)
                cur_rdesc = (uint16_t)(hb[7] | ((uint16_t)hb[8] << 8));
        } else if (d->bDescriptorType == USB_B_DESCRIPTOR_TYPE_ENDPOINT) {
            const usb_ep_desc_t* e = (const usb_ep_desc_t*)d;
            if (cur_pad && !s_prof.valid &&
                USB_EP_DESC_GET_XFERTYPE(e) == USB_TRANSFER_TYPE_INTR &&
                USB_EP_DESC_GET_EP_DIR(e) != 0) {
                s_prof.valid = true;
                s_prof.ifnum = cur_if;
                s_prof.ep    = e->bEndpointAddress;
                s_prof.mps   = USB_EP_DESC_GET_MPS(e);
                if (s_prof.mps > 64) s_prof.mps = 64;
                s_rdesc_len  = cur_rdesc;
                ulog("  gamepad: IF %u ep %02X mps %u rdesc %u",
                     cur_if, s_prof.ep, s_prof.mps, s_rdesc_len);
            }
        }
    }
    if (!s_prof.valid) return false;

    // Descriptor-driven, strictly: no descriptor -> no driver.
    if (s_rdesc_len == 0 || s_rdesc_len > RDESC_MAX) {
        ulog("gamepad: REJECTED - report descriptor length %u unusable",
             s_rdesc_len);
        s_prof.valid = false;
        return false;
    }
    if (!api->control(0x81, 0x06 /*GET_DESCRIPTOR*/, 0x2200, s_prof.ifnum,
                      s_rdesc, s_rdesc_len)) {
        ulog("gamepad: REJECTED - report descriptor fetch failed");
        s_prof.valid = false;
        return false;
    }
    const char* err = "?";
    if (!parse_rdesc(s_rdesc, s_rdesc_len, &err)) {
        ulog("gamepad: REJECTED - descriptor parse: %s", err);
        s_prof.valid = false;
        return false;
    }
    log_fields();

    // Runtime field state: buttons up, hats centered, axes await rest.
    for (int i = 0; i < s_nfields; i++) {
        s_fstate[i]   = (s_fields[i].kind == FK_HAT) ? 0xFF : 0;
        s_frest_ok[i] = false;
    }

    // Rules: conf if present, defaults otherwise; bind to fields.
    uint32_t clen = 0;
    const uint8_t* conf = api->config(&usbdrv_ops, &clen);
    s_nrules = 0;
    s_defaults = false;
    int old_lines = 0;
    if (conf) parse_conf((const char*)conf, &old_lines);
    if (old_lines)
        ulog("gamepad: %d old-format rule line(s) ignored - remap in the app",
             old_lines);
    if (s_nrules == 0) {
        gen_defaults();
        s_defaults = true;
    }
    bind_rules();
    ulog("gamepad: %d rule(s)%s", s_nrules,
         s_defaults ? " (default mapping - Gamepad app customizes)" : "");
    return true;
}

static bool pad_want(void) { return s_prof.valid; }

static bool pad_start(const UsbHostApi* api) {
    if (s_on) return true;
    if (!api->claim_interface(s_prof.ifnum, 0)) {
        s_prof.valid = false;
        return false;
    }
    if (!api->control(0x21, 0x0A /*SET_IDLE*/, 0, s_prof.ifnum, 0, 0))
        ulog("pad SET_IDLE failed (continuing)");

    s_pipe = api->pipe_open(s_prof.ep, s_prof.mps, s_prof.mps);
    if (!s_pipe) {
        ulog("pad pipe alloc failed");
        api->release_interface(s_prof.ifnum);
        s_prof.valid = false;
        return false;
    }
    s_on = true;
    if (!api->pipe_submit(s_pipe, s_prof.mps, pad_pipe_cb, 0)) {
        ulog("pad submit failed");
        s_on = false;
        api->pipe_close(s_pipe); s_pipe = 0;
        api->release_interface(s_prof.ifnum);
        s_prof.valid = false;
        return false;
    }
    ulog(">>> Gamepad ready.");
    return true;
}

static void pad_stop(const UsbHostApi* api, bool) {
    s_on = false;
    if (s_pipe) { api->pipe_close(s_pipe); s_pipe = 0; }
    api->release_interface(s_prof.ifnum);
    pad_all_up();
    ulog("Gamepad stopped.");
}

static int pad_busy(void) {
    return (s_pipe && s_hapi) ? s_hapi->pipe_in_flight(s_pipe) : 0;
}

static void pad_park(void) {
    if (s_pipe && s_hapi) s_hapi->pipe_cancel(s_pipe);
}

static void pad_resume(void) {
    if (!s_pipe || !s_hapi) return;
    if (s_hapi->pipe_in_flight(s_pipe)) return;
    s_hapi->pipe_reset(s_pipe);
    if (!s_hapi->pipe_submit(s_pipe, s_prof.mps, pad_pipe_cb, 0))
        ulog("pad: resume resubmit failed");
}

static void pad_tick(const UsbHostApi* api) {
    if (!s_arrows || api->input_module_active()) return;
    uint32_t now = api->ticks_ms();
    if (now - s_arr_t0 < REPEAT_DELAY) return;
    if (now - s_arr_last < REPEAT_MS) return;
    s_arr_last = now;
    api->input_nav((s_arrows & 1) ? 1 : 0, (s_arrows & 2) ? 1 : 0,
                   (s_arrows & 4) ? 1 : 0, (s_arrows & 8) ? 1 : 0, 0);
}

static void pad_status(char* out, uint32_t n) {
    snprintf(out, n, "%d rules, %d fields%s", s_nrules, s_nfields,
             s_defaults ? " (default)" : "");
}

extern "C" const UsbDriverDesc usbdrv_ops = {
    USB_DRIVER_ABI_VERSION,
    "gamepad",
    &pad_probe,
    &pad_want,
    &pad_start,
    &pad_stop,
    &pad_busy,
    &pad_park,
    &pad_resume,
    &pad_tick,
    &pad_status,
};
