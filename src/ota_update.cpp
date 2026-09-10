// ota_update.cpp — OTA update, main-firmware side. Stages a release image on
// the SD card or LittleFS, verifies it, and hands off to the updater
// partition, which writes it into the main partition (job-file contract and
// exit paths: src/updater/main_updater.cpp).
//
// Lua bindings (Settings/Firmware). Every binding returns a table; a failure
// carries ok=false and error=<text>.
//   _ota_info()             {mode, reason, running, main, updater, installed,
//                            board, staged, phase}
//   _ota_check()            {ok, tag, url, installed, newer}: the latest
//                            GitHub release, read from the /releases/latest
//                            redirect (no JSON), plus this board's asset URL
//   _ota_begin(source, arg) source "url" (asset URL, downloaded to the staging
//                            path) or "file" (an S:/L: path); {ok, path, total}
//   _ota_step()             one bounded slice of the download or the verify:
//                            {phase, done, total, path, tag, error}
//   _ota_install()          writes L:/.ota_job and boots the updater; returns
//                            only on failure
//   _ota_abort()            stops an in-flight session (a partial download is
//                            deleted, a finished staged image is kept)
//   _ota_cancel()           abort, then delete the staged images and the job
//
// Layout modes (_ota_info().mode):
//   "ota"      running from ota_0 next to the `updater` factory partition
//   "single"   running from a factory partition: the pre-OTA table
//   "launcher" a `test` app partition, or a factory partition that is not
//              ours, exists: installed through bmorcelli's Launcher, whose
//              other OTA slots belong to other firmwares
//   "unknown"  anything else; refused

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <FS.h>
#include <SD.h>
#include <LittleFS.h>
extern "C" {
#include <lua.h>
#include <lauxlib.h>
}
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_spi_flash.h"   // esp_partition_mmap handle type, spi_flash_munmap
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "mbedtls/sha256.h"

#include "ota_update.h"
#include "meshpunk_fs.h"
#include "meshpunk_sync.h"       // SLog, sd_spi_take, MESH_LOCK, mesh_task_paused
#include "usb_manager.h"         // UsbFlashGuard(If), usbdrive_active
#include "boards/board_pins.h"   // MESHPUNK_BOARD_NAME
#include "radio/proto_loader.h"  // lora_proto_flush
#include "ota_tag.h"

extern bool sd_mounted;        // main.cpp
extern void sd_spi_release();  // main.cpp

// This image's board tag (ota_tag.h); both verifiers look for it in a
// candidate image, and _ota_info reports it.
static const char kBoardTag[] = "MESHPUNK-BOARD:" MESHPUNK_BOARD_NAME;

// The PlatformIO env that builds this board's updater, for the refusal text.
#if defined(BOARD_TDECK)
static const char* kUpdaterEnv = "meshpunk_updater";
#elif defined(BOARD_HELTEC_V4)
static const char* kUpdaterEnv = "meshpunk_heltec_updater";
#else
static const char* kUpdaterEnv = "<board>_updater";
#endif

static const char* kRepoLatest   = "https://github.com/PhilMo6/meshpunk/releases/latest";
static const char* kRepoDownload = "https://github.com/PhilMo6/meshpunk/releases/download/";
static const char* kJobPath      = "/.ota_job";       // LittleFS
static const char* kMarkerPath   = "/.pack_version";  // LittleFS, written by the pack extractor
static const char* kStageSd      = "S:/meshpunk/ota/firmware.bin";
static const char* kStageLfs     = "L:/ota/firmware.bin";
static const size_t kBufSize     = 16384;

// ── Partition layout ─────────────────────────────────────────────────────────

enum OtaMode { OTA_MODE_OTA, OTA_MODE_SINGLE, OTA_MODE_LAUNCHER, OTA_MODE_UNKNOWN };

struct OtaLayout {
  OtaMode mode;
  const char* reason;
  const esp_partition_t* running;
  const esp_partition_t* main;
  const esp_partition_t* updater;
};

static const char* mode_name(OtaMode m) {
  switch (m) {
    case OTA_MODE_OTA:      return "ota";
    case OTA_MODE_SINGLE:   return "single";
    case OTA_MODE_LAUNCHER: return "launcher";
    default:                return "unknown";
  }
}

static const char* part_label(const esp_partition_t* p) { return p ? p->label : "none"; }

static bool part_has_image(const esp_partition_t* p) {
  uint8_t magic = 0;
  return p && esp_partition_read(p, 0, &magic, 1) == ESP_OK && magic == 0xE9;
}

// esp_partition_next() frees the iterator itself when the list ends and
// returns NULL (spi_flash/partition.c), so a walk to the end owns nothing
// afterwards; releasing the original handle again is a double free.
static int count_app_partitions(void) {
  int n = 0;
  esp_partition_iterator_t it =
      esp_partition_find(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, NULL);
  while (it) {
    n++;
    it = esp_partition_next(it);
  }
  return n;
}

static OtaLayout ota_layout(void) {
  OtaLayout l;
  l.mode = OTA_MODE_UNKNOWN;
  l.reason = "unrecognized partition table";
  l.running = esp_ota_get_running_partition();
  l.main = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, NULL);
  l.updater = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, "updater");
  const esp_partition_t* test =
      esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_TEST, NULL);
  const esp_partition_t* factory_any =
      esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, NULL);
  bool assets =
      esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, "assets") ||
      esp_partition_find_first(ESP_PARTITION_TYPE_DATA, (esp_partition_subtype_t)0x83, "assets");

  if (!l.running) {
    l.reason = "running partition unknown";
    return l;
  }
  if (l.running->subtype == ESP_PARTITION_SUBTYPE_APP_FACTORY) {
    l.mode = OTA_MODE_SINGLE;
    l.reason = "This partition layout predates on-device updates. Flash the -merged.bin once by USB.";
    return l;
  }
  if (test || (factory_any && !l.updater)) {
    l.mode = OTA_MODE_LAUNCHER;
    l.reason = "Installed through the Launcher. Update there: power on into the Launcher and install the new -launcher.bin.";
    return l;
  }
  if (l.running->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_0 && l.updater && l.main && assets &&
      count_app_partitions() == 2) {
    if (!part_has_image(l.updater)) {
      l.reason = "The updater partition is empty. Flash the -merged.bin once by USB.";
      return l;
    }
    // The updater's board tag, read from the mapped partition: a T-Deck
    // updater flashed onto a Heltec (or the reverse) drives the wrong pins
    // and never mounts the card, so it is refused here instead.
    static char reason[160];
    const void* mapped = nullptr;
    spi_flash_mmap_handle_t handle = 0;
    if (esp_partition_mmap(l.updater, 0, l.updater->size, SPI_FLASH_MMAP_DATA, &mapped, &handle) != ESP_OK) {
      l.reason = "The updater partition could not be mapped for its board tag.";
      return l;
    }
    OtaTagScan tag;
    ota_tag_scan_init(tag, true);
    ota_tag_scan_feed(tag, (const uint8_t*)mapped, l.updater->size);
    ota_tag_scan_finish(tag);
    spi_flash_munmap(handle);
    if (!tag.found) {
      snprintf(reason, sizeof reason,
               "The updater partition holds no tagged MeshPunk updater. Upload the %s env.", kUpdaterEnv);
      l.reason = reason;
      return l;
    }
    if (strcmp(tag.slug, MESHPUNK_BOARD_NAME) != 0) {
      snprintf(reason, sizeof reason,
               "The updater partition holds the %s updater. Upload the %s env.", tag.slug, kUpdaterEnv);
      l.reason = reason;
      return l;
    }
    l.mode = OTA_MODE_OTA;
    l.reason = "Ready for on-device updates.";
  }
  return l;
}

// ── Installed version and staged image ───────────────────────────────────────

static String read_installed_version(void) {
  fs::File f = LittleFS.open(kMarkerPath, "r");
  if (!f) return String();
  String s = f.readString();
  f.close();
  s.trim();
  return s;
}

static bool sd_path_exists(const char* bare) {
  if (!sd_mounted) return false;
  sd_spi_take();
  bool e = SD.exists(bare);
  sd_spi_release();
  return e;
}

static void remove_staged(const char* path) {
  if (path[0] == 'S') {
    if (!sd_mounted) return;
    sd_spi_take();
    SD.remove(path + 2);
    sd_spi_release();
  } else {
    UsbFlashGuard g;
    LittleFS.remove(path + 2);
  }
}

static String staged_path(void) {
  if (sd_path_exists(kStageSd + 2)) return String(kStageSd);
  if (LittleFS.exists(kStageLfs + 2)) return String(kStageLfs);
  return String();
}

static bool is_staging_path(const String& p) { return p == kStageSd || p == kStageLfs; }

// Release asset names are meshpunk-<board>-<tag>-firmware.bin; the tag is
// what sits between the board slug and the suffix ("" when the name does not
// follow the pattern).
static String tag_from_name(const String& name) {
  int slash = name.lastIndexOf('/');
  String base = slash >= 0 ? name.substring(slash + 1) : name;
  String prefix = String("meshpunk-") + MESHPUNK_BOARD_NAME + "-";
  const char* suffix = "-firmware.bin";
  if (!base.startsWith(prefix) || !base.endsWith(suffix)) return String();
  return base.substring(prefix.length(), base.length() - strlen(suffix));
}

// First three numeric fields of a version string ("v0.4.1" -> 0 4 1).
static int parse_version(const String& s, int out[3]) {
  int n = 0;
  int i = 0, len = s.length();
  while (i < len && n < 3) {
    if (isdigit((unsigned char)s[i])) {
      int v = 0;
      while (i < len && isdigit((unsigned char)s[i])) {
        v = v * 10 + (s[i] - '0');
        i++;
      }
      out[n++] = v;
    } else {
      i++;
    }
  }
  return n;
}

static bool tag_newer(const String& tag, const String& installed) {
  int a[3] = {0, 0, 0}, b[3] = {0, 0, 0};
  if (parse_version(tag, a) == 0 || parse_version(installed, b) == 0) return false;
  for (int i = 0; i < 3; i++) {
    if (a[i] != b[i]) return a[i] > b[i];
  }
  return false;
}

// ── Session: download, then verify, one bounded slice per _ota_step ──────────

enum OtaPhase { PH_IDLE, PH_DOWNLOAD, PH_VERIFY, PH_READY, PH_ERROR };

static const char* phase_name(OtaPhase p) {
  switch (p) {
    case PH_DOWNLOAD: return "download";
    case PH_VERIFY:   return "verify";
    case PH_READY:    return "ready";
    case PH_ERROR:    return "error";
    default:          return "idle";
  }
}

struct OtaSession {
  OtaPhase phase = PH_IDLE;
  String   path;            // staged image (S:/L: path)
  String   tag;
  bool     downloaded = false;   // staged by this session: deleted on abort
  uint32_t total = 0;
  uint32_t done = 0;
  String   error;
  // download
  HTTPClient*  http = nullptr;
  WiFiClient*  stream = nullptr;
  MeshpunkFile out;
  bool         out_open = false;
  uint32_t     stall_deadline = 0;
  // verify
  MeshpunkFile in;
  bool         in_open = false;
  mbedtls_sha256_context sha;
  bool         sha_live = false;
  char         sha_hex[65] = {0};
  OtaTagScan   board_tag;
};

static OtaSession s;
static uint8_t*   s_buf = nullptr;   // PSRAM transfer buffer, allocated once

static void session_close_files(void) {
  if (s.out_open) {
    if (s.out.is_sd) sd_spi_take();   // meshpunk_close releases it
    meshpunk_close(s.out);
    s.out_open = false;
  }
  if (s.in_open) {
    if (s.in.is_sd) sd_spi_take();
    meshpunk_close(s.in);
    s.in_open = false;
  }
  if (s.sha_live) {
    mbedtls_sha256_free(&s.sha);
    s.sha_live = false;
  }
  if (s.http) {
    s.http->end();
    delete s.http;
    s.http = nullptr;
    s.stream = nullptr;
  }
}

// Stops the session. A download that did not finish leaves a partial file,
// which is deleted; a finished (verified or verifying) staged image stays.
static void session_abort(void) {
  bool partial = (s.phase == PH_DOWNLOAD) && s.downloaded;
  String path = s.path;
  session_close_files();
  if (partial && path.length()) {
    remove_staged(path.c_str());
    SLog.printf("[OTA] partial download removed: %s\n", path.c_str());
  }
  s.phase = PH_IDLE;
  s.path = "";
  s.tag = "";
  s.downloaded = false;
  s.total = s.done = 0;
  s.error = "";
}

static void session_fail(const char* why) {
  SLog.printf("[OTA] failed: %s\n", why);
  bool partial = (s.phase == PH_DOWNLOAD) && s.downloaded;
  session_close_files();
  if (partial && s.path.length()) remove_staged(s.path.c_str());
  s.error = why;
  s.phase = PH_ERROR;
}

static bool ensure_buf(void) {
  if (!s_buf) s_buf = (uint8_t*)heap_caps_malloc(kBufSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  return s_buf != nullptr;
}

// Opens the staged image for the verify phase and checks the header. The
// checks mirror the updater's (magic 0xE9, ESP32-S3 chip id, appended hash,
// fits the main slot); the SHA-256 itself is accumulated by the steps.
static const char* verify_open(const OtaLayout& lay) {
  s.in = meshpunk_open(s.path.c_str(), "r", false);
  if (!s.in.valid) return "staged file not found";
  s.in_open = true;
  if (s.in.is_sd) sd_spi_release();

  if (s.in.is_sd) sd_spi_take();
  uint32_t size = s.in.file.size();
  uint8_t hdr[24];
  bool ok = s.in.file.seek(0) && s.in.file.read(hdr, sizeof hdr) == sizeof hdr && s.in.file.seek(0);
  if (s.in.is_sd) sd_spi_release();

  if (size < 24 + 32) return "file too small for an app image";
  if (size > lay.main->size) return "image larger than the main partition";
  if (!ok) return "header read failed";
  if (hdr[0] != 0xE9) return "not an ESP32 app image (magic byte)";
  uint16_t chip = (uint16_t)hdr[12] | ((uint16_t)hdr[13] << 8);
  if (chip != 0x0009) return "not an ESP32-S3 image (chip id)";
  if (hdr[23] != 1) return "image carries no appended hash";

  mbedtls_sha256_init(&s.sha);
  mbedtls_sha256_starts_ret(&s.sha, 0);
  s.sha_live = true;
  ota_tag_scan_init(s.board_tag, false);
  s.total = size;
  s.done = 0;
  s.phase = PH_VERIFY;
  return nullptr;
}

static void step_download(void) {
  uint32_t t0 = millis();
  size_t moved = 0;
  UsbFlashGuardIf guard(s.out.is_flash);
  while (s.done < s.total && moved < 65536 && (millis() - t0) < 40) {
    int avail = s.stream->available();
    if (avail <= 0) {
      if (!s.http->connected()) {
        session_fail("connection lost");
        return;
      }
      if ((int32_t)(millis() - s.stall_deadline) >= 0) {
        session_fail("download stalled");
        return;
      }
      return;   // nothing buffered yet; the next step continues
    }
    size_t want = (size_t)avail;
    if (want > kBufSize) want = kBufSize;
    if (want > s.total - s.done) want = s.total - s.done;
    int rd = s.stream->readBytes(s_buf, want);
    if (rd <= 0) return;
    if (s.out.is_sd) sd_spi_take();
    size_t wr = s.out.file.write(s_buf, rd);
    if (s.out.is_sd) sd_spi_release();
    if (wr != (size_t)rd) {
      session_fail("write failed (staging drive full?)");
      return;
    }
    s.done += rd;
    moved += rd;
    s.stall_deadline = millis() + 20000;
  }
  if (s.done >= s.total) {
    if (s.out.is_sd) sd_spi_take();
    meshpunk_close(s.out);
    s.out_open = false;
    s.http->end();
    delete s.http;
    s.http = nullptr;
    s.stream = nullptr;
    SLog.printf("[OTA] downloaded %u bytes to %s\n", (unsigned)s.total, s.path.c_str());
    OtaLayout lay = ota_layout();
    const char* why = verify_open(lay);
    if (why) session_fail(why);
  }
}

static void step_verify(void) {
  uint32_t t0 = millis();
  size_t moved = 0;
  uint32_t body = s.total - 32;
  while (s.done < body && moved < 131072 && (millis() - t0) < 60) {
    size_t want = body - s.done;
    if (want > kBufSize) want = kBufSize;
    if (s.in.is_sd) sd_spi_take();
    size_t rd = s.in.file.read(s_buf, want);
    if (s.in.is_sd) sd_spi_release();
    if (rd != want) {
      session_fail("read failed while hashing");
      return;
    }
    mbedtls_sha256_update_ret(&s.sha, s_buf, want);
    ota_tag_scan_feed(s.board_tag, s_buf, want);
    s.done += want;
    moved += want;
  }
  if (s.done >= body) {
    uint8_t calc[32], tail[32];
    mbedtls_sha256_finish_ret(&s.sha, calc);
    mbedtls_sha256_free(&s.sha);
    s.sha_live = false;
    if (s.in.is_sd) sd_spi_take();   // held through the read; meshpunk_close releases it
    size_t rd = s.in.file.read(tail, sizeof tail);
    meshpunk_close(s.in);
    s.in_open = false;
    if (rd != sizeof tail) {
      session_fail("digest read failed");
      return;
    }
    if (memcmp(calc, tail, sizeof tail) != 0) {
      session_fail("appended SHA-256 mismatch (file corrupt)");
      return;
    }
    // Board tag (ota_tag.h): both boards are ESP32-S3, so this is the only
    // thing that tells their images apart.
    ota_tag_scan_finish(s.board_tag);
    if (!s.board_tag.found) {
      session_fail("image carries no board tag (built before the on-device updater)");
      return;
    }
    if (strcmp(s.board_tag.slug, MESHPUNK_BOARD_NAME) != 0) {
      static char reason[96];
      snprintf(reason, sizeof reason, "image is for board %s, this is %s", s.board_tag.slug,
               MESHPUNK_BOARD_NAME);
      session_fail(reason);
      return;
    }
    static const char* digits = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
      s.sha_hex[2 * i] = digits[calc[i] >> 4];
      s.sha_hex[2 * i + 1] = digits[calc[i] & 15];
    }
    s.sha_hex[64] = 0;
    s.done = s.total;
    s.phase = PH_READY;
    SLog.printf("[OTA] verified %s (%u bytes, sha256 %s)\n", s.path.c_str(), (unsigned)s.total, s.sha_hex);
  }
}

// ── Boot report ──────────────────────────────────────────────────────────────

void ota_init_report(void) {
  OtaLayout lay = ota_layout();
  String installed = read_installed_version();
  bool stale_job = LittleFS.exists(kJobPath);
  if (stale_job) {
    UsbFlashGuard g;
    LittleFS.remove(kJobPath);
  }
  String staged = staged_path();
  SLog.printf("[OTA] mode=%s running=%s main=%s updater=%s installed=%s staged=%s%s\n",
              mode_name(lay.mode), part_label(lay.running), part_label(lay.main),
              part_label(lay.updater), installed.length() ? installed.c_str() : "dev",
              staged.length() ? staged.c_str() : "none",
              stale_job ? " (stale job removed)" : "");
  if (lay.mode != OTA_MODE_OTA) SLog.printf("[OTA] %s\n", lay.reason);
}

// ── Lua bindings ─────────────────────────────────────────────────────────────

static void set_str(lua_State* L, const char* k, const char* v) {
  lua_pushstring(L, v);
  lua_setfield(L, -2, k);
}
static void set_int(lua_State* L, const char* k, lua_Integer v) {
  lua_pushinteger(L, v);
  lua_setfield(L, -2, k);
}
static void set_bool(lua_State* L, const char* k, bool v) {
  lua_pushboolean(L, v ? 1 : 0);
  lua_setfield(L, -2, k);
}
static int push_fail(lua_State* L, const char* msg) {
  lua_newtable(L);
  set_bool(L, "ok", false);
  set_str(L, "error", msg);
  return 1;
}

static int lua_ota_info(lua_State* L) {
  OtaLayout lay = ota_layout();
  String installed = read_installed_version();
  String staged = staged_path();
  lua_newtable(L);
  set_bool(L, "ok", true);
  set_str(L, "mode", mode_name(lay.mode));
  set_str(L, "reason", lay.reason);
  set_str(L, "running", part_label(lay.running));
  set_str(L, "main", part_label(lay.main));
  set_str(L, "updater", part_label(lay.updater));
  set_str(L, "installed", installed.c_str());
  set_str(L, "board", MESHPUNK_BOARD_NAME);
  set_str(L, "image_tag", kBoardTag);
  set_str(L, "staged", staged.c_str());
  set_str(L, "phase", phase_name(s.phase));
  return 1;
}

static int lua_ota_check(lua_State* L) {
  if (WiFi.status() != WL_CONNECTED) return push_fail(L, "WiFi not connected");
  HTTPClient http;
  http.setUserAgent("meshpunk/1.0");
  http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
  http.setTimeout(10000);
  if (!http.begin(kRepoLatest)) return push_fail(L, "bad URL");
  int code = http.GET();
  String location = http.getLocation();
  http.end();
  if (code != 301 && code != 302) {
    String msg = code > 0 ? String("HTTP ") + code : http.errorToString(code);
    return push_fail(L, msg.c_str());
  }
  // https://github.com/PhilMo6/meshpunk/releases/tag/<tag>
  int slash = location.lastIndexOf('/');
  String tag = slash >= 0 ? location.substring(slash + 1) : String();
  tag.trim();
  if (tag.length() < 2 || tag[0] != 'v' || location.indexOf("/releases/tag/") < 0)
    return push_fail(L, "no release tag in the GitHub redirect");
  String installed = read_installed_version();
  String url = String(kRepoDownload) + tag + "/meshpunk-" + MESHPUNK_BOARD_NAME + "-" + tag + "-firmware.bin";
  SLog.printf("[OTA] latest release %s (installed %s)\n", tag.c_str(),
              installed.length() ? installed.c_str() : "dev");
  lua_newtable(L);
  set_bool(L, "ok", true);
  set_str(L, "tag", tag.c_str());
  set_str(L, "url", url.c_str());
  set_str(L, "installed", installed.c_str());
  set_bool(L, "newer", tag_newer(tag, installed));
  return 1;
}

static int lua_ota_begin(lua_State* L) {
  const char* source = luaL_checkstring(L, 1);
  const char* arg = luaL_checkstring(L, 2);
  if (s.phase == PH_DOWNLOAD || s.phase == PH_VERIFY) return push_fail(L, "an update is already in progress");
  OtaLayout lay = ota_layout();
  if (lay.mode != OTA_MODE_OTA) return push_fail(L, lay.reason);
  if (usbdrive_active()) return push_fail(L, "USB drive mode is active");
  if (mesh_task_paused) return push_fail(L, "a link session is active");
  if (!ensure_buf()) return push_fail(L, "buffer allocation failed");
  session_abort();

  if (strcmp(source, "url") == 0) {
    if (WiFi.status() != WL_CONNECTED) return push_fail(L, "WiFi not connected");
    String tag = tag_from_name(String(arg));
    if (tag.length() == 0) return push_fail(L, "release file is not for this board");
    String path = sd_mounted ? kStageSd : kStageLfs;

    s.http = new HTTPClient();
    s.http->setUserAgent("meshpunk/1.0");
    s.http->setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    s.http->setTimeout(15000);
    if (!s.http->begin(arg)) {
      session_close_files();
      return push_fail(L, "bad URL");
    }
    int code = s.http->GET();
    if (code != 200) {
      String msg = code > 0 ? String("HTTP ") + code : s.http->errorToString(code);
      session_close_files();
      return push_fail(L, msg.c_str());
    }
    int len = s.http->getSize();
    if (len <= 0) {
      session_close_files();
      return push_fail(L, "download has no length");
    }
    if ((uint32_t)len > lay.main->size) {
      session_close_files();
      return push_fail(L, "image larger than the main partition");
    }
    if (sd_mounted) {
      sd_spi_take();
      uint64_t free_bytes = SD.totalBytes() - SD.usedBytes();
      sd_spi_release();
      if (free_bytes < (uint64_t)len + 65536) {
        session_close_files();
        return push_fail(L, "not enough free space on the SD card");
      }
    } else {
      size_t total = 0, used = 0;
      if (!mp_littlefs_df(&total, &used) || total - used < (size_t)len + 65536) {
        session_close_files();
        return push_fail(L, "not enough free space on internal flash");
      }
    }
    meshpunk_mkdirs(path.c_str(), false);
    s.out = meshpunk_open(path.c_str(), "w", false);
    if (!s.out.valid) {
      session_close_files();
      return push_fail(L, "cannot create the staging file");
    }
    s.out_open = true;
    if (s.out.is_sd) sd_spi_release();
    s.stream = s.http->getStreamPtr();
    s.path = path;
    s.tag = tag;
    s.downloaded = true;
    s.total = (uint32_t)len;
    s.done = 0;
    s.error = "";
    s.stall_deadline = millis() + 20000;
    s.phase = PH_DOWNLOAD;
    SLog.printf("[OTA] downloading %s (%u bytes) to %s\n", tag.c_str(), (unsigned)len, path.c_str());
  } else if (strcmp(source, "file") == 0) {
    String path = arg;
    if (!(path.startsWith("S:/") || path.startsWith("L:/"))) return push_fail(L, "path must start with S:/ or L:/");
    bool staging = is_staging_path(path);
    String tag = tag_from_name(path);
    if (!staging && tag.length() == 0) return push_fail(L, "file name is not a release firmware for this board");
    s.path = path;
    s.tag = staging ? "staged" : tag;
    s.downloaded = staging;
    s.error = "";
    const char* why = verify_open(lay);
    if (why) {
      session_close_files();
      s.phase = PH_IDLE;
      return push_fail(L, why);
    }
    SLog.printf("[OTA] verifying %s (%u bytes)\n", path.c_str(), (unsigned)s.total);
  } else {
    return push_fail(L, "unknown source");
  }

  lua_newtable(L);
  set_bool(L, "ok", true);
  set_str(L, "path", s.path.c_str());
  set_str(L, "tag", s.tag.c_str());
  set_int(L, "total", s.total);
  return 1;
}

static int lua_ota_step(lua_State* L) {
  if (s.phase == PH_DOWNLOAD) step_download();
  else if (s.phase == PH_VERIFY) step_verify();
  lua_newtable(L);
  set_bool(L, "ok", s.phase != PH_ERROR);
  set_str(L, "phase", phase_name(s.phase));
  set_int(L, "done", s.done);
  set_int(L, "total", s.total);
  set_str(L, "path", s.path.c_str());
  set_str(L, "tag", s.tag.c_str());
  set_str(L, "error", s.error.c_str());
  return 1;
}

static int lua_ota_install(lua_State* L) {
  if (s.phase != PH_READY) return push_fail(L, "no verified image");
  OtaLayout lay = ota_layout();
  if (lay.mode != OTA_MODE_OTA) return push_fail(L, lay.reason);
  if (usbdrive_active()) return push_fail(L, "USB drive mode is active");
  if (mesh_task_paused) return push_fail(L, "a link session is active");

  {
    UsbFlashGuard g;
    fs::File f = LittleFS.open(kJobPath, "w");
    if (!f) return push_fail(L, "cannot write the job file");
    f.printf("path=%s\nsize=%u\nsha256=%s\ntag=%s\n", s.path.c_str(), (unsigned)s.total, s.sha_hex,
             s.tag.c_str());
    f.close();
  }
  esp_err_t err;
  {
    UsbFlashGuard g;
    err = esp_ota_set_boot_partition(lay.updater);   // verifies the updater image, erases otadata
  }
  if (err != ESP_OK) {
    UsbFlashGuard g;
    LittleFS.remove(kJobPath);
    SLog.printf("[OTA] updater handoff refused: %s\n", esp_err_to_name(err));
    return push_fail(L, esp_err_to_name(err));
  }
  SLog.printf("[OTA] handoff: %s (%u bytes, %s) -> updater, restarting\n", s.path.c_str(),
              (unsigned)s.total, s.tag.c_str());
  // Same quiesce order as system_shutdown(): flush, stop the mesh task, then
  // unmount the card so no SD transaction is in flight at the reset.
  MESH_LOCK();
  lora_proto_flush();   // pending mesh writes hit disk first
  MESH_UNLOCK();
  mesh_task_paused = true;
  delay(60);   // let an in-flight dispatcher tick finish
  if (sd_mounted) {
    sd_spi_take();
    SD.end();
    sd_mounted = false;
    sd_spi_release();
  }
  delay(100);
  esp_restart();
  return 0;
}

static int lua_ota_abort(lua_State* L) {
  session_abort();
  lua_newtable(L);
  set_bool(L, "ok", true);
  return 1;
}

static int lua_ota_cancel(lua_State* L) {
  session_abort();
  if (sd_path_exists(kStageSd + 2)) remove_staged(kStageSd);
  if (LittleFS.exists(kStageLfs + 2)) remove_staged(kStageLfs);
  if (LittleFS.exists(kJobPath)) {
    UsbFlashGuard g;
    LittleFS.remove(kJobPath);
  }
  SLog.println("[OTA] staged images and job removed");
  lua_newtable(L);
  set_bool(L, "ok", true);
  return 1;
}

void ota_register_lua(lua_State* L) {
  lua_register(L, "_ota_info",    lua_ota_info);
  lua_register(L, "_ota_check",   lua_ota_check);
  lua_register(L, "_ota_begin",   lua_ota_begin);
  lua_register(L, "_ota_step",    lua_ota_step);
  lua_register(L, "_ota_install", lua_ota_install);
  lua_register(L, "_ota_abort",   lua_ota_abort);
  lua_register(L, "_ota_cancel",  lua_ota_cancel);
}
