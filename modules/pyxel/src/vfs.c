// Game files: .pyxapp (ZIP) mounts, path resolution, file reads and writes.
// A mounted .pyxapp is inflated into PSRAM once and served under the virtual
// directory PX_APP_ROOT; every other path goes to the firmware's fopen.

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "px.h"
#include "lodepng.h"

#define PX_APP_ROOT "/pyxapp"
#define PX_PATH_MAX 256

typedef struct {
    char* name;            // path inside the archive, no leading '/'
    unsigned char* data;   // inflated contents + NUL
    size_t size;
} px_zip_entry;

static px_zip_entry* s_entries = NULL;
static int s_entry_count = 0;
static char s_cwd[PX_PATH_MAX] = "/";

// ---------------------------------------------------------------------------
// Paths
// ---------------------------------------------------------------------------

// Collapse "//", "." and ".." in an absolute path, in place.
static void path_normalize(char* p) {
    char out[PX_PATH_MAX];
    size_t n = 0;
    const char* s = p;
    out[n++] = '/';
    while (*s) {
        while (*s == '/') s++;
        if (!*s) break;
        const char* seg = s;
        while (*s && *s != '/') s++;
        size_t len = (size_t)(s - seg);
        if (len == 1 && seg[0] == '.') continue;
        if (len == 2 && seg[0] == '.' && seg[1] == '.') {
            if (n > 1) {
                n--;                                  // drop trailing '/'
                while (n > 1 && out[n - 1] != '/') n--;
            }
            continue;
        }
        if (n > 1 && out[n - 1] != '/') out[n++] = '/';
        if (n + len >= sizeof(out) - 1) break;
        memcpy(out + n, seg, len);
        n += len;
    }
    if (n > 1 && out[n - 1] == '/') n--;
    out[n] = 0;
    memcpy(p, out, n + 1);
}

void px_vfs_resolve(const char* path, char* out, size_t cap) {
    if (path[0] == '/') {
        snprintf(out, cap, "%s", path);
    } else {
        snprintf(out, cap, "%s/%s", s_cwd, path);
    }
    path_normalize(out);
}

void px_path_dirname(const char* path, char* out, size_t cap) {
    snprintf(out, cap, "%s", path);
    char* slash = strrchr(out, '/');
    if (!slash) {
        snprintf(out, cap, ".");
    } else if (slash == out) {
        out[1] = 0;
    } else {
        *slash = 0;
    }
}

void px_vfs_set_cwd(const char* dir) {
    px_vfs_resolve(dir, s_cwd, sizeof(s_cwd));
}

const char* px_vfs_cwd(void) {
    return s_cwd;
}

// Archive-relative name for an absolute path under PX_APP_ROOT, or NULL.
static const char* app_rel(const char* abs) {
    size_t n = strlen(PX_APP_ROOT);
    if (strncmp(abs, PX_APP_ROOT, n) != 0) return NULL;
    if (abs[n] == 0) return "";
    if (abs[n] != '/') return NULL;
    return abs + n + 1;
}

// ---------------------------------------------------------------------------
// Reads
// ---------------------------------------------------------------------------
static char* read_real_file(const char* path, size_t* size) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0) { fclose(f); return NULL; }
    char* buf = (char*)malloc((size_t)n + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t got = n > 0 ? fread(buf, 1, (size_t)n, f) : 0;
    fclose(f);
    buf[got] = 0;
    *size = got;
    return buf;
}

char* px_vfs_read(const char* path, size_t* size) {
    char abs[PX_PATH_MAX];
    px_vfs_resolve(path, abs, sizeof(abs));
    const char* rel = app_rel(abs);
    if (rel) {
        for (int i = 0; i < s_entry_count; i++) {
            if (strcmp(s_entries[i].name, rel) == 0) {
                char* buf = (char*)malloc(s_entries[i].size + 1);
                if (!buf) return NULL;
                memcpy(buf, s_entries[i].data, s_entries[i].size);
                buf[s_entries[i].size] = 0;
                *size = s_entries[i].size;
                return buf;
            }
        }
        return NULL;
    }
    return read_real_file(abs, size);
}

int px_vfs_stat(const char* path) {
    char abs[PX_PATH_MAX];
    px_vfs_resolve(path, abs, sizeof(abs));
    const char* rel = app_rel(abs);
    if (rel) {
        size_t n = strlen(rel);
        for (int i = 0; i < s_entry_count; i++) {
            const char* name = s_entries[i].name;
            if (strcmp(name, rel) == 0) return 1;
            if (n == 0 || (strncmp(name, rel, n) == 0 && name[n] == '/')) return 2;
        }
        return 0;
    }
    FILE* f = fopen(abs, "rb");
    if (f) { fclose(f); return 1; }
    // No stat/opendir export: a directory is recognised by its __init__.py,
    // which is exactly what a package import needs.
    char init[PX_PATH_MAX + 16];
    snprintf(init, sizeof(init), "%s/__init__.py", abs);
    f = fopen(init, "rb");
    if (f) { fclose(f); return 2; }
    return 0;
}

int px_vfs_listdir(const char* path, void (*cb)(void* ctx, const char* name, size_t len), void* ctx) {
    char abs[PX_PATH_MAX];
    px_vfs_resolve(path, abs, sizeof(abs));
    const char* rel = app_rel(abs);
    if (!rel) return -1;
    if (px_vfs_stat(abs) != 2) return px_vfs_stat(abs) == 1 ? ENOTDIR : ENOENT;
    size_t n = strlen(rel);
    for (int i = 0; i < s_entry_count; i++) {
        const char* name = s_entries[i].name;
        if (n) {
            if (strncmp(name, rel, n) != 0 || name[n] != '/') continue;
            name += n + 1;
        }
        const char* slash = strchr(name, '/');
        cb(ctx, name, slash ? (size_t)(slash - name) : strlen(name));
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Writes
// ---------------------------------------------------------------------------

// errno value for a write to the absolute path `abs` that cannot succeed, or 0.
static int write_target_error(const char* abs) {
    if (app_rel(abs)) {
        if (s_entry_count == 0) return ENOENT;       // no .pyxapp mounted
        return px_vfs_stat(abs) == 2 ? EISDIR : 0;
    }
    return strncmp(abs, "/sd/", 4) == 0 ? 0 : EROFS;
}

int px_vfs_write(const char* path, const void* data, size_t n) {
    char abs[PX_PATH_MAX];
    px_vfs_resolve(path, abs, sizeof(abs));
    int e = write_target_error(abs);
    if (e) return e;
    if (!data) data = "";
    const char* rel = app_rel(abs);
    if (!rel) return host_write_file(abs, data, (uint32_t)n) == 0 ? 0 : EIO;

    unsigned char* copy = (unsigned char*)malloc(n + 1);
    if (!copy) return ENOMEM;
    memcpy(copy, data, n);
    copy[n] = 0;
    for (int i = 0; i < s_entry_count; i++) {
        if (strcmp(s_entries[i].name, rel) != 0) continue;
        free(s_entries[i].data);
        s_entries[i].data = copy;
        s_entries[i].size = n;
        return 0;
    }
    size_t rn = strlen(rel);
    char* name = (char*)malloc(rn + 1);
    px_zip_entry* grown = name ? (px_zip_entry*)realloc(s_entries, sizeof(px_zip_entry) *
                                                        (size_t)(s_entry_count + 1))
                               : NULL;
    if (!grown) { free(name); free(copy); return ENOMEM; }
    s_entries = grown;
    memcpy(name, rel, rn + 1);
    s_entries[s_entry_count].name = name;
    s_entries[s_entry_count].data = copy;
    s_entries[s_entry_count].size = n;
    s_entry_count++;
    return 0;
}

#define PX_WFILES 16

typedef struct {
    char* path;               // absolute
    unsigned char* data;
    size_t len, cap;
    bool dirty;               // contents differ from what storage holds
    bool used;
} px_wfile;

static px_wfile s_wfiles[PX_WFILES];

static px_wfile* wfile_get(int h) {
    return (h >= 0 && h < PX_WFILES && s_wfiles[h].used) ? &s_wfiles[h] : NULL;
}

static void wfile_release(px_wfile* f) {
    free(f->path);
    free(f->data);
    memset(f, 0, sizeof(*f));
}

int px_wfile_open(const char* path, char mode, int* err) {
    char abs[PX_PATH_MAX];
    px_vfs_resolve(path, abs, sizeof(abs));
    *err = write_target_error(abs);
    if (*err) return -1;
    bool exists = px_vfs_stat(abs) == 1;
    if (mode == 'x' && exists) { *err = EEXIST; return -1; }
    int h = 0;
    while (h < PX_WFILES && s_wfiles[h].used) h++;
    if (h == PX_WFILES) { *err = EMFILE; return -1; }
    px_wfile* f = &s_wfiles[h];
    size_t pn = strlen(abs);
    f->path = (char*)malloc(pn + 1);
    if (!f->path) { *err = ENOMEM; return -1; }
    memcpy(f->path, abs, pn + 1);
    if (mode == 'a' && exists) {
        f->data = (unsigned char*)px_vfs_read(abs, &f->len);
        if (!f->data) { wfile_release(f); *err = EIO; return -1; }
        f->cap = f->len;
    }
    // "w" and "x" empty the file even when nothing is written; "a" creates it.
    f->dirty = mode != 'a' || !exists;
    f->used = true;
    return h;
}

int px_wfile_write(int h, const void* data, size_t n) {
    px_wfile* f = wfile_get(h);
    if (!f) return EBADF;
    if (f->len + n > f->cap) {
        size_t ncap = f->cap ? f->cap : 256;
        while (ncap < f->len + n) ncap *= 2;
        unsigned char* g = (unsigned char*)realloc(f->data, ncap);
        if (!g) return ENOMEM;
        f->data = g;
        f->cap = ncap;
    }
    memcpy(f->data + f->len, data, n);
    f->len += n;
    f->dirty = true;
    return 0;
}

int px_wfile_flush(int h) {
    px_wfile* f = wfile_get(h);
    if (!f) return EBADF;
    if (!f->dirty) return 0;
    int e = px_vfs_write(f->path, f->data, f->len);
    if (!e) f->dirty = false;
    return e;
}

int px_wfile_close(int h) {
    px_wfile* f = wfile_get(h);
    if (!f) return EBADF;
    int e = px_wfile_flush(h);
    wfile_release(f);
    return e;
}

void px_wfile_close_all(void) {
    for (int h = 0; h < PX_WFILES; h++) {
        if (!s_wfiles[h].used) continue;
        char path[PX_PATH_MAX];
        snprintf(path, sizeof(path), "%s", s_wfiles[h].path);
        int e = px_wfile_close(h);
        if (e) printf("[pyxel] could not write %s (errno %d)\n", path, e);
    }
}

// ---------------------------------------------------------------------------
// ZIP
// ---------------------------------------------------------------------------
static uint16_t rd16(const unsigned char* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const unsigned char* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

unsigned char* px_inflate(const unsigned char* in, size_t in_size, size_t expect, size_t* out_size) {
    unsigned char* out = NULL;
    size_t n = 0;
    unsigned err = lodepng_inflate(&out, &n, in, in_size, &lodepng_default_decompress_settings);
    if (err || (expect && n != expect)) { free(out); return NULL; }
    *out_size = n;
    return out;
}

static void free_entries(void) {
    for (int i = 0; i < s_entry_count; i++) {
        free(s_entries[i].name);
        free(s_entries[i].data);
    }
    free(s_entries);
    s_entries = NULL;
    s_entry_count = 0;
}

bool px_zip_walk(const unsigned char* z, size_t zn, px_zip_cb cb, void* ctx,
                 char* err, size_t errlen) {
    // End of central directory: scan back over a possible archive comment.
    long eocd = -1;
    for (long i = (long)zn - 22; i >= 0 && i >= (long)zn - 22 - 65535; i--) {
        if (rd32(z + i) == 0x06054b50) { eocd = i; break; }
    }
    if (eocd < 0) { snprintf(err, errlen, "not a zip archive"); return false; }
    int count = rd16(z + eocd + 10);
    size_t p = rd32(z + eocd + 16);
    for (int i = 0; i < count; i++) {
        if (p + 46 > zn || rd32(z + p) != 0x02014b50) {
            snprintf(err, errlen, "corrupt zip directory");
            return false;
        }
        px_zip_file f;
        f.method = rd16(z + p + 10);
        f.csize = rd32(z + p + 20);
        f.usize = rd32(z + p + 24);
        int nlen = rd16(z + p + 28), xlen = rd16(z + p + 30), clen = rd16(z + p + 32);
        uint32_t loff = rd32(z + p + 42);
        f.name = (const char*)(z + p + 46);
        f.name_len = nlen;
        p += 46 + (size_t)nlen + (size_t)xlen + (size_t)clen;
        if (nlen == 0 || f.name[nlen - 1] == '/') continue;             // directory
        if (loff + 30 > zn || rd32(z + loff) != 0x04034b50) {
            snprintf(err, errlen, "corrupt zip entry");
            return false;
        }
        size_t data = loff + 30 + rd16(z + loff + 26) + rd16(z + loff + 28);
        if (data + f.csize > zn) { snprintf(err, errlen, "truncated zip entry"); return false; }
        f.data = z + data;
        if (!cb(ctx, &f, err, errlen)) return false;
    }
    return true;
}

unsigned char* px_zip_file_contents(const px_zip_file* f, size_t* out_size, char* err, size_t errlen) {
    unsigned char* out = NULL;
    if (f->method == 0) {
        out = (unsigned char*)malloc((size_t)f->csize + 1);
        if (out) {
            memcpy(out, f->data, f->csize);
            *out_size = f->csize;
        }
    } else if (f->method == 8) {
        size_t n = 0;
        unsigned char* raw = px_inflate(f->data, f->csize, f->usize, &n);
        if (!raw) {
            snprintf(err, errlen, "cannot inflate %.*s", f->name_len, f->name);
            return NULL;
        }
        // Room for the NUL terminator the text parsers rely on.
        out = (unsigned char*)realloc(raw, n + 1);
        if (!out) { free(raw); }
        else *out_size = n;
    } else {
        snprintf(err, errlen, "unsupported zip method %d (%.*s)", f->method, f->name_len, f->name);
        return NULL;
    }
    if (!out) { snprintf(err, errlen, "out of memory"); return NULL; }
    out[*out_size] = 0;
    return out;
}

static bool mount_entry(void* ctx, const px_zip_file* f, char* err, size_t errlen) {
    int* cap = (int*)ctx;
    if (f->name_len >= 9 && strncmp(f->name, "__MACOSX/", 9) == 0) return true;
    if (s_entry_count == *cap) {
        int ncap = *cap ? *cap * 2 : 32;
        px_zip_entry* grown = (px_zip_entry*)realloc(s_entries, sizeof(px_zip_entry) * (size_t)ncap);
        if (!grown) { snprintf(err, errlen, "out of memory"); return false; }
        s_entries = grown;
        *cap = ncap;
    }
    px_zip_entry* e = &s_entries[s_entry_count];
    e->name = (char*)malloc((size_t)f->name_len + 1);
    if (!e->name) { snprintf(err, errlen, "out of memory"); return false; }
    memcpy(e->name, f->name, (size_t)f->name_len);
    e->name[f->name_len] = 0;
    e->data = px_zip_file_contents(f, &e->size, err, errlen);
    if (!e->data) { free(e->name); return false; }
    s_entry_count++;
    return true;
}

static bool zip_mount(const unsigned char* z, size_t zn, char* err, size_t errlen) {
    int cap = 0;
    return px_zip_walk(z, zn, mount_entry, &cap, err, errlen);
}

static bool ends_with(const char* s, const char* suffix) {
    size_t a = strlen(s), b = strlen(suffix);
    if (a < b) return false;
    for (size_t i = 0; i < b; i++) {
        char c = s[a - b + i];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if (c != suffix[i]) return false;
    }
    return true;
}

bool px_vfs_mount(const char* game_path, char* startup, size_t cap, char* err, size_t errlen) {
    px_vfs_unmount();
    if (!ends_with(game_path, ".pyxapp") && !ends_with(game_path, ".zip")) {
        px_vfs_resolve(game_path, startup, cap);
        return true;
    }
    size_t zn = 0;
    unsigned char* z = (unsigned char*)read_real_file(game_path, &zn);
    if (!z) { snprintf(err, errlen, "cannot read %s", game_path); return false; }
    bool ok = zip_mount(z, zn, err, errlen);
    free(z);
    if (!ok) { free_entries(); return false; }

    // The startup marker holds the script path relative to the marker's own
    // directory (python/pyxel/cli.py _extract_pyxel_app).
    for (int i = 0; i < s_entry_count; i++) {
        const char* base = strrchr(s_entries[i].name, '/');
        base = base ? base + 1 : s_entries[i].name;
        if (strcmp(base, ".pyxapp_startup_script") != 0) continue;
        char rel[PX_PATH_MAX];
        size_t n = s_entries[i].size < sizeof(rel) - 1 ? s_entries[i].size : sizeof(rel) - 1;
        memcpy(rel, s_entries[i].data, n);
        rel[n] = 0;
        while (n > 0 && (rel[n - 1] == '\n' || rel[n - 1] == '\r' || rel[n - 1] == ' ')) rel[--n] = 0;
        char dir[PX_PATH_MAX];
        snprintf(dir, sizeof(dir), "%s/%s", PX_APP_ROOT, s_entries[i].name);
        char* slash = strrchr(dir, '/');
        *slash = 0;
        char joined[PX_PATH_MAX * 2];
        snprintf(joined, sizeof(joined), "%s/%s", dir, rel);
        snprintf(startup, cap, "%s", joined);
        path_normalize(startup);
        return true;
    }
    snprintf(err, errlen, "no .pyxapp_startup_script in %s", game_path);
    free_entries();
    return false;
}

// Write files still open here were not closed by px_wfile_close_all (the
// exit()/abort() path); their contents are dropped.
void px_vfs_unmount(void) {
    for (int h = 0; h < PX_WFILES; h++) {
        if (s_wfiles[h].used) wfile_release(&s_wfiles[h]);
    }
    free_entries();
    snprintf(s_cwd, sizeof(s_cwd), "/");
}
