#include "theme_font.h"

#include <cstring>
#include <cstdio>

#include <esp_heap_caps.h>

#include "../lib/lvgl/src/libs/tiny_ttf/lv_tiny_ttf.h"
#include "emoji_font.h"      // emoji_font_create/destroy — the chain heads
#include "meshpunk_fs.h"
#include "meshpunk_sync.h"   // SLog

// The ui-role chain head: the emoji imgfont created in setupLvgl() (main.cpp)
// that every style in the system points at. NULL (or == montserrat, the
// emoji-create-failed fallback, which is const and must never be written)
// disables ui-role swapping. The text-role head is owned here.
extern lv_font_t *g_ui_font;

namespace {

constexpr const char * BUNDLED_TTF_PATH = "L:/fonts/NotoSans-Regular.ttf";
constexpr int    DEFAULT_PX    = 16;        // matches the imgfont heads' height
constexpr size_t TTF_MAX_BYTES = 2u * 1024u * 1024u;
constexpr int    SIZED_CAP     = 6;
constexpr int    PX_MIN = 8, PX_MAX = 64;

struct FontSlot {
    uint8_t   * buf  = nullptr;   // PSRAM TTF bytes
    size_t      size = 0;
    lv_font_t * font = nullptr;   // tiny_ttf instance at `px`
    int         px   = 0;
    char        path[128] = {0};
};

// Emoji-wrapped instance of a role at a non-default size. `head` is the
// stable pointer handed to Lua; `ttf` is rebuilt in place whenever the role's
// resolution changes, so app-held heads never dangle.
struct SizedEnt {
    int         px   = 0;
    lv_font_t * head = nullptr;   // emoji imgfont (ours)
    lv_font_t * ttf  = nullptr;   // instance on the role's active buffer (may be null)
};

struct Role {
    FontSlot theme;               // set by the active theme (t.set_font)
    FontSlot user;                // user default (Settings > Fonts pref)
    lv_font_t * head = nullptr;   // ui: g_ui_font (not owned); text: owned
    const lv_font_t * resolved = nullptr;  // last applied resolution target
    SizedEnt sized[SIZED_CAP];
    int sized_count = 0;
};

FontSlot s_bundled;
bool s_bundled_tried = false;   // re-armed by theme_font_release_all()
Role s_roles[2];

bool head_writable(const lv_font_t * h)
{
    return h && h != &lv_font_montserrat_14;
}

// ---- Deferred destruction ---------------------------------------------------
// Swapped-out fonts may still be referenced by deferred draw units and LVGL
// draw caches for the current refresh, so they die from a one-shot lv_timer —
// the same safe point the emoji cache recycle uses. A batch holds the fonts
// built on one buffer plus the buffer itself (instances before bytes).

constexpr int PENDING_CAP = 6;
struct PendingBatch {
    lv_font_t * fonts[1 + SIZED_CAP] = {nullptr};
    int         n   = 0;
    uint8_t   * buf = nullptr;
};
PendingBatch s_pending[PENDING_CAP];
int s_pending_count = 0;
bool s_pending_timer = false;

void batch_free_now(PendingBatch & b)
{
    for (int i = 0; i < b.n; i++) {
        if (b.fonts[i]) lv_tiny_ttf_destroy(b.fonts[i]);
    }
    if (b.buf) heap_caps_free(b.buf);
    b = PendingBatch();
}

void pending_flush()
{
    for (int i = 0; i < s_pending_count; i++) batch_free_now(s_pending[i]);
    s_pending_count = 0;
}

void pending_timer_cb(lv_timer_t * t)
{
    LV_UNUSED(t);   // repeat_count 1: LVGL deletes the timer after this call
    pending_flush();
    s_pending_timer = false;
}

PendingBatch * pending_grab()
{
    if (s_pending_count >= PENDING_CAP) {
        // Pathological rapid flipping: the oldest batch has survived at least
        // one full refresh — free it immediately to make room.
        batch_free_now(s_pending[0]);
        for (int i = 1; i < s_pending_count; i++) s_pending[i - 1] = s_pending[i];
        s_pending_count--;
    }
    PendingBatch * b = &s_pending[s_pending_count++];
    *b = PendingBatch();

    if (!s_pending_timer) {
        lv_timer_t * t = lv_timer_create(pending_timer_cb, 20, nullptr);
        if (t) {
            lv_timer_set_repeat_count(t, 1);
            s_pending_timer = true;
        }
        // OOM: entries stay queued; a later swap's timer (or release_all)
        // frees them.
    }
    return b;
}

void batch_add_font(PendingBatch * b, lv_font_t * f)
{
    if (b && f && b->n < (int)(sizeof(b->fonts) / sizeof(b->fonts[0])))
        b->fonts[b->n++] = f;
}

// Queue a slot's font + buffer for deferred destruction.
void slot_free_deferred(FontSlot & s)
{
    if (!s.font && !s.buf) { s = FontSlot(); return; }
    PendingBatch * b = pending_grab();
    if (b) {
        batch_add_font(b, s.font);
        b->buf = s.buf;
    } else {
        if (s.font) lv_tiny_ttf_destroy(s.font);
        if (s.buf)  heap_caps_free(s.buf);
    }
    s = FontSlot();
}

// ---- Loading ----------------------------------------------------------------

// Read a TTF into PSRAM and create the base instance. Fills `out` on success.
// The one-time file read holds the SPI lock for an SD path (meshpunk_open
// convention) — a few hundred ms for a ~500KB font, acceptable for an
// explicit theme switch / settings change.
bool slot_load(FontSlot & out, const char * path, int px)
{
    MeshpunkFile mf = meshpunk_open(path, "r", false);
    if (!mf.valid) {
        SLog.printf("[FONT] open failed: %s\n", path);
        return false;
    }
    size_t sz = mf.file.size();
    if (sz < 12 || sz > TTF_MAX_BYTES) {
        SLog.printf("[FONT] bad size %u: %s\n", (unsigned)sz, path);
        meshpunk_close(mf);
        return false;
    }
    auto * buf = static_cast<uint8_t *>(
        heap_caps_malloc(sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!buf) {
        SLog.printf("[FONT] no PSRAM for %u bytes\n", (unsigned)sz);
        meshpunk_close(mf);
        return false;
    }
    size_t rd = mf.file.read(buf, sz);
    meshpunk_close(mf);
    if (rd != sz) {
        SLog.printf("[FONT] short read %u/%u: %s\n", (unsigned)rd, (unsigned)sz, path);
        heap_caps_free(buf);
        return false;
    }

    lv_font_t * f = lv_tiny_ttf_create_data_ex(buf, sz, px, LV_FONT_KERNING_NORMAL,
                                               LV_TINY_TTF_CACHE_GLYPH_CNT);
    if (!f) {
        // Unparseable — commonly a CFF-flavored .otf; tiny_ttf is glyf-only.
        SLog.printf("[FONT] tiny_ttf rejected %s (not a glyf .ttf?)\n", path);
        heap_caps_free(buf);
        return false;
    }
    f->fallback = &lv_font_montserrat_14;   // FontAwesome symbols + last resort

    out = FontSlot();
    out.buf  = buf;
    out.size = sz;
    out.font = f;
    out.px   = px;
    strlcpy(out.path, path, sizeof(out.path));
    SLog.printf("[FONT] loaded %s (%u bytes, %dpx)\n", path, (unsigned)sz, px);
    return true;
}

// ---- Resolution + chain application ------------------------------------------

FontSlot * role_active_slot(Role & r)
{
    if (r.theme.font)   return &r.theme;
    if (r.user.font)    return &r.user;
    if (s_bundled.font) return &s_bundled;
    return nullptr;
}

// Re-point one role's head + its sized instances at the role's resolved slot.
// Old sized ttf instances are defer-freed (their source buffer may itself be
// in a pending batch from the same swap — both die at the same timer tick,
// instances before bytes). Returns true if anything visible changed.
bool role_apply(Role & r)
{
    FontSlot * a = role_active_slot(r);
    const lv_font_t * target = a ? a->font : &lv_font_montserrat_14;
    if (r.resolved == target) return false;
    r.resolved = target;

    if (head_writable(r.head))
        r.head->fallback = target;

    if (r.sized_count > 0) {
        PendingBatch * b = pending_grab();
        for (int i = 0; i < r.sized_count; i++) {
            SizedEnt & e = r.sized[i];
            lv_font_t * fresh = nullptr;
            if (a) {
                fresh = lv_tiny_ttf_create_data_ex(a->buf, a->size, e.px,
                                                   LV_FONT_KERNING_NORMAL,
                                                   LV_TINY_TTF_CACHE_GLYPH_CNT);
                if (fresh) fresh->fallback = &lv_font_montserrat_14;
            }
            if (head_writable(e.head))
                e.head->fallback = fresh ? fresh : &lv_font_montserrat_14;
            batch_add_font(b, e.ttf);
            e.ttf = fresh;
        }
    }
    return true;
}

void apply_active()
{
    bool changed = false;
    if (role_apply(s_roles[THEME_FONT_UI]))   changed = true;
    if (role_apply(s_roles[THEME_FONT_TEXT])) changed = true;
    if (changed) {
        lv_obj_report_style_change(NULL);
        lv_obj_invalidate(lv_screen_active());
    }
}

// A path naming the bundled font at stock size means "use the bundled slot" —
// never load a duplicate of the 431KB buffer.
bool is_bundled_ref(const char * path, int px)
{
    return px == DEFAULT_PX && strcmp(path, BUNDLED_TTF_PATH) == 0;
}

// Replace `slot` with the font at path/px. Empty path clears the slot.
// Shared by theme_font_set and font_default_set.
bool slot_assign(FontSlot & slot, const char * path, int px)
{
    if (!path || !path[0] || is_bundled_ref(path, px <= 0 ? DEFAULT_PX : px)) {
        slot_free_deferred(slot);   // resolve to the next tier (user/bundled)
        return true;
    }
    if (px <= 0) px = DEFAULT_PX;

    // Idempotent: themes re-apply on every show_background().
    if (slot.font && slot.px == px && strcmp(slot.path, path) == 0)
        return true;

    FontSlot tmp;
    if (!slot_load(tmp, path, px)) return false;   // keep current resolution

    slot_free_deferred(slot);
    slot = tmp;
    return true;
}

} // anonymous namespace

extern "C" void theme_font_init(const char * ui_pref, const char * text_pref)
{
    if (!s_bundled.font && !s_bundled_tried) {
        s_bundled_tried = true;
        FontSlot tmp;
        if (slot_load(tmp, BUNDLED_TTF_PATH, DEFAULT_PX)) s_bundled = tmp;
        // Missing bundled font is fine — chains just end at montserrat.
    }

    s_roles[THEME_FONT_UI].head = g_ui_font;

    // The text-role chain head: our own emoji imgfont (shares the global
    // emoji glyph cache). Created once per Lua lifetime; released with
    // everything else at ELF launch.
    Role & tr = s_roles[THEME_FONT_TEXT];
    if (!tr.head) {
        tr.head = emoji_font_create(DEFAULT_PX, &lv_font_montserrat_14);
        if (!tr.head) {
            // Degenerate: no emoji layer for the text role; serve the raw
            // resolved TTF instead (theme_font_sized falls back to it).
            SLog.println("[FONT] text head create failed");
        }
    }

    // User defaults from the persisted prefs (best effort — a missing file
    // just leaves that role on the bundled font).
    if (ui_pref && ui_pref[0])
        slot_assign(s_roles[THEME_FONT_UI].user, ui_pref, 0);
    if (text_pref && text_pref[0])
        slot_assign(s_roles[THEME_FONT_TEXT].user, text_pref, 0);

    apply_active();
}

extern "C" bool theme_font_set(theme_font_role_t role, const char * path, int px)
{
    if (role != THEME_FONT_UI && role != THEME_FONT_TEXT) return false;
    if (!path || !path[0]) return false;
    bool ok = slot_assign(s_roles[role].theme, path, px);
    apply_active();
    return ok && role_active_slot(s_roles[role]) != nullptr;
}

extern "C" void theme_font_clear(void)
{
    slot_free_deferred(s_roles[THEME_FONT_UI].theme);
    slot_free_deferred(s_roles[THEME_FONT_TEXT].theme);
    apply_active();
}

extern "C" bool font_default_set(theme_font_role_t role, const char * path)
{
    if (role != THEME_FONT_UI && role != THEME_FONT_TEXT) return false;
    bool ok = slot_assign(s_roles[role].user, path ? path : "", 0);
    apply_active();
    return ok;
}

extern "C" const lv_font_t * theme_font_sized(theme_font_role_t role, int px)
{
    if (role != THEME_FONT_UI && role != THEME_FONT_TEXT) return nullptr;
    Role & r = s_roles[role];
    if (px <= 0) px = DEFAULT_PX;
    if (px < PX_MIN) px = PX_MIN;
    if (px > PX_MAX) px = PX_MAX;

    if (px == DEFAULT_PX) {
        if (head_writable(r.head)) return r.head;
        // No head (emoji create failed / ui degenerate): raw resolved TTF.
        FontSlot * a = role_active_slot(r);
        return a ? a->font : nullptr;
    }

    for (int i = 0; i < r.sized_count; i++) {
        if (r.sized[i].px == px) return r.sized[i].head ? r.sized[i].head
                                                        : r.sized[i].ttf;
    }
    if (r.sized_count >= SIZED_CAP) return nullptr;

    FontSlot * a = role_active_slot(r);
    lv_font_t * ttf = nullptr;
    if (a) {
        ttf = lv_tiny_ttf_create_data_ex(a->buf, a->size, px,
                                         LV_FONT_KERNING_NORMAL,
                                         LV_TINY_TTF_CACHE_GLYPH_CNT);
        if (ttf) ttf->fallback = &lv_font_montserrat_14;
    }
    // Emoji-wrap the sized instance (glyph bitmaps stay 16px inside the
    // taller line — small emoji in big text, better than tofu).
    lv_font_t * head = emoji_font_create(px, ttf ? (const lv_font_t *)ttf
                                                 : &lv_font_montserrat_14);
    if (!head && !ttf) return nullptr;

    SizedEnt & e = r.sized[r.sized_count++];
    e.px   = px;
    e.head = head;
    e.ttf  = ttf;
    return head ? head : ttf;
}

extern "C" void theme_font_release_all(void)
{
    // Teardown context (ELF launch): detach the chains FIRST so nothing
    // dangles, then free synchronously — same context that synchronously
    // clears the emoji glyph cache right before this.
    for (int ri = 0; ri < 2; ri++) {
        Role & r = s_roles[ri];
        if (head_writable(r.head)) r.head->fallback = &lv_font_montserrat_14;
        for (int i = 0; i < r.sized_count; i++) {
            if (r.sized[i].head) emoji_font_destroy(r.sized[i].head);
            if (r.sized[i].ttf)  lv_tiny_ttf_destroy(r.sized[i].ttf);
            r.sized[i] = SizedEnt();
        }
        r.sized_count = 0;
        r.resolved = nullptr;

        auto free_slot = [](FontSlot & s) {
            if (s.font) lv_tiny_ttf_destroy(s.font);
            if (s.buf)  heap_caps_free(s.buf);
            s = FontSlot();
        };
        free_slot(r.theme);
        free_slot(r.user);
    }
    pending_flush();

    // The text head is ours (the ui head belongs to setupLvgl and persists).
    Role & tr = s_roles[THEME_FONT_TEXT];
    if (tr.head) { emoji_font_destroy(tr.head); tr.head = nullptr; }
    s_roles[THEME_FONT_UI].head = nullptr;

    if (s_bundled.font) lv_tiny_ttf_destroy(s_bundled.font);
    if (s_bundled.buf)  heap_caps_free(s_bundled.buf);
    s_bundled = FontSlot();
    s_bundled_tried = false;   // the post-ELF setupLuaVGL() re-init reloads
}
