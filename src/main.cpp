#include "TouchDrvGT911.hpp"
#include "utilities.h"
#include <Arduino.h>
#include <HTTPClient.h>
#include <LittleFS.h>
#include <SD.h>
#include <TFT_eSPI.h>
#include <Ticker.h> // Include ticker for LVGL timing
#include <WiFi.h>
#include <Wire.h>
#include <esp_heap_caps.h> // DMA-capable buffer allocation for LVGL
#include <mbedtls/platform.h> // runtime override of mbedTLS allocator (TLS -> PSRAM)
#include "multi_heap.h" // ESP-IDF arena allocator for the Lua PSRAM arena
#include <lvgl.h>
#include "theme/lv_theme_meshpunk.h"
#include "emoji_font.h"
#include "tdeck-pins.h"
#include "meshpunk_sync.h"
#include "Audio.h"
#include "sound.h"
#include "notify.h"
#include "ble_companion.h"
#include "elf_host.h"
#include "meshpunk_fs.h"
#include "fs_bridge.h"

// Meshcore
#include "punkmesh.h"
#include "../../lib/MeshCore/src/helpers/ESP32Board.h"
#include "punk_radio_wrapper.h"
#include <Mesh.h>
#include <helpers/ArduinoHelpers.h>
#include <helpers/StaticPoolPacketManager.h>
#include <helpers/SimpleMeshTables.h>
#include <helpers/IdentityStore.h>
#include <RTClib.h>
#include <RadioLib.h>
#include <TinyGPSPlus.h>
#include <new>

// GPS time sync (defined below, after the_mesh is declared).
static void gps_sync_begin();
// Exposed so meshpunk_tasks.cpp's gps_task can drive it from Core 1.
void gps_sync_poll();
bool gps_sync_is_done();
// Resets GPS state and re-opens serial for a fresh sync cycle.
void gps_sync_restart();


extern "C" {
#include <lua.h>
#include <lualib.h>
#include <luavgl.h>

// Streaming Lua chunk reader: feeds an open file to lua_load() in fixed 512-byte
// blocks so we never hold an entire source file in one contiguous RAM buffer.
// (Used by both luaL_loadfilex below and the require() searcher.) Slurping the
// whole file into one malloc'd buffer was ~169KB for the Map app; freed after
// compile, but in a post-meshprint heap that hole couldn't coalesce and became
// the contiguous-block "wall" that starved Doom's zone after a Map session.
struct LuaFileChunkReader {
  fs::File file;
  char buf[512];
};

static const char *lua_file_chunk_reader(lua_State *L, void *ud, size_t *size) {
  (void)L;
  LuaFileChunkReader *st = (LuaFileChunkReader *)ud;
  size_t n = st->file.read((uint8_t *)st->buf, sizeof(st->buf));
  if (n == 0) {
    *size = 0;
    return NULL;
  }
  *size = n;
  return st->buf;
}

int luaL_loadfilex(lua_State *L, const char *filename, const char *mode) {
    LuaFileChunkReader rdr;
    rdr.file = LittleFS.open(filename, "r");
    if (!rdr.file || rdr.file.isDirectory()) {
      if (rdr.file) rdr.file.close();
      lua_pushfstring(L, "cannot open %s", filename);
      return LUA_ERRFILE;
    }
    // Stream to lua_load() in 512-byte blocks (no whole-file buffer). `mode` is
    // passed through; the require() searcher does the same lua_load call.
    int status = lua_load(L, lua_file_chunk_reader, &rdr, filename, mode);
    rdr.file.close();
    return status;
  }
}

extern "C" unsigned lodepng_decode32(unsigned char **out, unsigned *w, unsigned *h,
                                     const unsigned char *in, size_t insize);

// Radio
RADIO_CLASS radio = new Module(RADIO_CS_PIN, RADIO_DIO1_PIN, RADIO_RST_PIN, RADIO_BUSY_PIN);

// Meshcore
StdRNG fast_rng;
SimpleMeshTables tables;

ESP32Board board;
PunkSX1262Wrapper radio_driver(radio, board);
PunkMesh* the_mesh = nullptr;

// One-shot GPS time sync: poll in loop() until first fix, then stop.
static TinyGPSPlus gps_tinygps;
static HardwareSerial GPSSerial(1);
static bool gps_sync_done = false;
static uint32_t gps_sync_start_ms = 0;
static uint32_t gps_last_stats_ms = 0;
static uint32_t gps_last_chars = 0;
static uint32_t gps_fix_acquired_ms = 0;    // when time fix was captured (for post-fix window)
static const uint32_t GPS_SYNC_TIMEOUT_MS = 600000;   // 10 min cold-start budget
static const uint32_t GPS_STATS_INTERVAL_MS = 5000;   // print status every 5s
static const uint32_t GPS_POST_FIX_MS = 2000;         // keep reading after fix to collect sat count

// Timezone state — "auto" uses longitude-from-GPS; otherwise a fixed offset in minutes.
static bool    gps_location_valid_at_fix = false;
static double  gps_lng_at_fix = 0.0;
static double  gps_lat_at_fix = 0.0;
static bool    gps_time_fix_valid = false;   // true if last cycle got a time fix (not timeout)
static bool    gps_manual_time_override = false;
static uint32_t gps_sats_at_fix = 0;
static uint32_t gps_hdop_at_fix = 0;        // HDOP * 100 (TinyGPSPlus integer representation)
static bool    tz_is_auto = true;
static int32_t tz_manual_minutes = 0;
static String  tz_setting_str = "auto";
static bool    dst_enabled = false;

// Firmware-level preferences (unified in /firmware_prefs)
static bool   use_sd_pref = true;
static String clock_fmt_str = "12";
static bool   ble_enabled_pref = true;
bool   ble_bond_clear_pref = false;
static bool   wifi_enabled_pref = true;
static String wifi_saved_ssid = "";
static String wifi_saved_pass = "";

// ── Audio ─────────────────────────────────────────────────────────────────
static Audio*    audio = nullptr;          // ESP32-audioI2S player, created in setup()

// ── Keyboard Backlight ─────────────────────────────────────────────────────
static uint8_t kbd_brightness = 200;  // 0–255, persisted

// ── Keyboard sym behavior ───────────────────────────────────────────────────
// false = sym is a plain hold modifier. true = a clean tap of sym (press and
// release with nothing typed in between) latches the symbol layer until the
// next tap, while holding sym still works as a momentary modifier. Persisted.
static bool kb_sym_toggle_pref = false;
static bool kb_sym_latched = false;
static bool kb_sym_phys_prev = false;
static bool kb_sym_used_while_held = false;

// ── Keyboard alt emoji layer ────────────────────────────────────────────────
// alt+key types an emoji while a textarea is focused (the layer is inert
// outside text fields, so a latched alt never hijacks WASD nav). Same
// tap-toggle latch machinery as sym above, behind its own persisted pref.
// The per-key map is keyed by the key's normal-layer char and persisted to
// /emoji_keymap on LittleFS (kb_emoji_map_load/save below the matrices).
static bool kb_alt_toggle_pref = false;
static bool kb_alt_latched = false;
static bool kb_alt_phys_prev = false;
static bool kb_alt_used_while_held = false;
static bool kb_alt_layer_active = false;

// ── Display Backlight ──────────────────────────────────────────────────────
static uint8_t display_brightness = 16;  // 0–16, persisted

// ── Inactivity Timeouts ───────────────────────────────────────────────────
static uint16_t screen_timeout_secs  = 60;  // 0 = never, persisted
static uint16_t kbd_timeout_secs     = 55;  // 0 = never, persisted
static uint16_t msg_retain_days      = 30;  // days of message/routing history (0 = unlimited)

// ── Trackball Sensitivity ────────────────────────────────────────────────
static uint16_t trackball_sensitivity_ms = 75;  // ms between accepted direction pulses, persisted
static uint16_t trackball_roll_ms = 0;                   // drain interval: 0 = instant (no momentum), persisted
static uint32_t last_activity_ms     = 0;
static bool     screen_timed_out     = false;
static bool     kbd_timed_out        = false;

// ── Notification Preferences ─────────────────────────────────────────────
static bool     notify_kbd_enabled   = true;   // keyboard blink on DM / @mention
static bool     notify_sound_enabled = true;   // melody on DM / @mention

// Accessors for notify.cpp (the C-side alert path) — these globals are
// file-static, and the alert fires from the mesh task, so it reads the live
// values through here rather than snapshotting them.
bool    firmware_notify_kbd_enabled()   { return notify_kbd_enabled; }
bool    firmware_notify_sound_enabled() { return notify_sound_enabled; }
uint8_t firmware_kbd_brightness()       { return kbd_brightness; }
bool    firmware_kbd_timed_out()        { return kbd_timed_out; }

// ── Topbar Preferences ─────────────────────────────────────────────
static bool     topbar_transparant = false;   // can you see the background though the topbar

// ── Theme Preferences ─────────────────────────────────────────────
static bool     theme_focus_solid  = false;   // selection highlight: false=translucent fill, true=opaque
static bool     theme_focus_darken = false;   // selection tint: false=brighten, true=darken

static int32_t tz_auto_offset_minutes() {
  if (!gps_location_valid_at_fix) return 0;
  // 1° longitude = 4 minutes of solar time.
  int32_t m = (int32_t)lround(gps_lng_at_fix * 4.0);
  if (m < -14 * 60) m = -14 * 60;
  if (m >  14 * 60) m =  14 * 60;
  // Round to nearest whole hour. Longitude is a rough proxy for civil time zones,
  // and the overwhelming majority of zones sit on hour boundaries — finer rounding
  // (e.g. 15 min) produces offsets like -8:15 for locations that are really -8:00.
  m = (int32_t)lround((double)m / 60.0) * 60;
  return m;
}

static int32_t tz_effective_offset_minutes() {
  int32_t base = tz_is_auto ? tz_auto_offset_minutes() : tz_manual_minutes;
  return base + (dst_enabled ? 60 : 0);
}

extern bool sd_mounted;
void sd_spi_release();

// Selected UI theme id (a folder name under /lua/themes; see lib/theme). Empty
// means "use the default theme". The palette + background it maps to live in
// Lua; only this id is persisted here.
static String theme_pref_str = "";

static void write_firmware_prefs(fs::FS& fs, const char* path) {
  File f = fs.open(path, "w", true);
  if (!f) { SLog.printf("[FW_PREFS] cannot write %s\n", path); return; }
  f.printf("use_sd=%d\n", use_sd_pref ? 1 : 0);
  f.printf("tz=%s\n", tz_setting_str.c_str());
  f.printf("clock_fmt=%s\n", clock_fmt_str.c_str());
  f.printf("dst=%d\n", dst_enabled ? 1 : 0);
  f.printf("sound_vol=%d\n",   sound_get_volume());
  f.printf("sound_muted=%d\n", sound_get_muted() ? 1 : 0);
  f.printf("kbd_bright=%d\n", kbd_brightness);
  f.printf("disp_bright=%d\n", display_brightness);
  f.printf("screen_timeout=%d\n", screen_timeout_secs);
  f.printf("kbd_timeout=%d\n", kbd_timeout_secs);
  f.printf("msg_retain_days=%d\n", msg_retain_days);
  f.printf("notify_kbd=%d\n", notify_kbd_enabled ? 1 : 0);
  f.printf("notify_sound=%d\n", notify_sound_enabled ? 1 : 0);
  f.printf("ble_enabled=%d\n", ble_enabled_pref ? 1 : 0);
  f.printf("ble_bond_clear=%d\n", ble_bond_clear_pref ? 1 : 0);
  f.printf("wifi_enabled=%d\n", wifi_enabled_pref ? 1 : 0);
  f.printf("trackball_sens=%d\n", trackball_sensitivity_ms);
  f.printf("trackball_roll=%d\n", trackball_roll_ms);
  f.printf("sym_toggle=%d\n", kb_sym_toggle_pref ? 1 : 0);
  f.printf("alt_toggle=%d\n", kb_alt_toggle_pref ? 1 : 0);
  f.printf("theme=%s\n", theme_pref_str.c_str());
  f.printf("topbar_transparant=%d\n", topbar_transparant ? 1 : 0);
  f.printf("sel_solid=%d\n", theme_focus_solid ? 1 : 0);
  f.printf("sel_darken=%d\n", theme_focus_darken ? 1 : 0);

  f.close();
  SLog.printf("[FW_PREFS] saved to %s\n", path);
}

static void firmware_prefs_save() {
  write_firmware_prefs(LittleFS, "/firmware_prefs");
  if (sd_mounted && use_sd_pref) {
    sd_spi_take();
    write_firmware_prefs(SD, "/meshpunk/firmware_prefs");
    sd_spi_release();
  }
}

static void write_wifi_creds(fs::FS& fs, const char* path) {
  File f = fs.open(path, "w", true);
  if (!f) { SLog.printf("[WIFI_CREDS] cannot write %s\n", path); return; }
  f.println(wifi_saved_ssid.c_str());
  f.println(wifi_saved_pass.c_str());
  f.close();
}

static void wifi_creds_save() {
  write_wifi_creds(LittleFS, "/wifi_creds");
  if (sd_mounted && use_sd_pref) {
    sd_spi_take();
    write_wifi_creds(SD, "/meshpunk/wifi_creds");
    sd_spi_release();
    SLog.println("[WIFI_CREDS] Saved to SD");
  }
}

static void wifi_creds_load() {
  File f = LittleFS.open("/wifi_creds", "r");
  if (!f) return;
  wifi_saved_ssid = f.readStringUntil('\n');
  wifi_saved_ssid.trim();
  wifi_saved_pass = f.readStringUntil('\n');
  wifi_saved_pass.trim();
  f.close();
  if (wifi_saved_ssid.length() > 0) {
    SLog.printf("[WIFI_CREDS] loaded SSID: %s\n", wifi_saved_ssid.c_str());
  }
}

static void wifi_creds_clear() {
  wifi_saved_ssid = "";
  wifi_saved_pass = "";
  LittleFS.remove("/wifi_creds");
  if (sd_mounted && use_sd_pref) {
    sd_spi_take();
    SD.remove("/meshpunk/wifi_creds");
    sd_spi_release();
  }
}

static void firmware_prefs_load() {
  File f = LittleFS.open("/firmware_prefs", "r");
  if (!f) {
    SLog.println("[FW_PREFS] no /firmware_prefs, using defaults");
    return;
  }
  char line[128];
  while (f.available()) {
    int len = 0;
    while (f.available() && len < (int)sizeof(line) - 1) {
      char ch = f.read();
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

    if (strcmp(key, "use_sd") == 0) {
      use_sd_pref = (atoi(val) == 1);
    } else if (strcmp(key, "tz") == 0) {
      String s(val);
      s.trim();
      if (s.length() == 0 || s.equalsIgnoreCase("auto")) {
        tz_is_auto = true; tz_setting_str = "auto";
      } else {
        tz_manual_minutes = (int32_t)s.toInt();
        tz_is_auto = false;
        tz_setting_str = String(tz_manual_minutes);
      }
    } else if (strcmp(key, "clock_fmt") == 0) {
      clock_fmt_str = (strcmp(val, "12") == 0) ? "12" : "24";
    } else if (strcmp(key, "dst") == 0) {
      dst_enabled = (atoi(val) == 1);
    } else if (strcmp(key, "sound_vol") == 0) {
      int v = atoi(val);
      if (v >= 0 && v <= 21) sound_set_volume((uint8_t)v);
    } else if (strcmp(key, "sound_muted") == 0) {
      sound_set_muted(atoi(val) == 1);
    } else if (strcmp(key, "kbd_bright") == 0) {
      int v = atoi(val);
      if (v >= 0 && v <= 255) kbd_brightness = (uint8_t)v;
    } else if (strcmp(key, "disp_bright") == 0) {
      int v = atoi(val);
      if (v >= 0 && v <= 16) display_brightness = (uint8_t)v;
    } else if (strcmp(key, "screen_timeout") == 0) {
      int v = atoi(val);
      if (v >= 0 && v <= 65535) screen_timeout_secs = (uint16_t)v;
    } else if (strcmp(key, "msg_retain_days") == 0) {
      int v = atoi(val);
      if (v >= 0 && v <= 3650) msg_retain_days = (uint16_t)v;
    } else if (strcmp(key, "kbd_timeout") == 0) {
      int v = atoi(val);
      if (v >= 0 && v <= 65535) kbd_timeout_secs = (uint16_t)v;
    } else if (strcmp(key, "notify_kbd") == 0) {
      notify_kbd_enabled = (atoi(val) == 1);
    } else if (strcmp(key, "notify_sound") == 0) {
      notify_sound_enabled = (atoi(val) == 1);
    } else if (strcmp(key, "ble_enabled") == 0) {
      ble_enabled_pref = (atoi(val) == 1);
    } else if (strcmp(key, "ble_bond_clear") == 0) {
      ble_bond_clear_pref = (atoi(val) == 1);
    } else if (strcmp(key, "wifi_enabled") == 0) {
      wifi_enabled_pref = (atoi(val) == 1);
    } else if (strcmp(key, "trackball_sens") == 0) {
      int v = atoi(val);
      if (v >= 0 && v <= 500) trackball_sensitivity_ms = (uint16_t)v;
    } else if (strcmp(key, "trackball_roll") == 0) {
      int v = atoi(val);
      if (v >= 0 && v <= 500) trackball_roll_ms = (uint16_t)v;
    } else if (strcmp(key, "sym_toggle") == 0) {
      kb_sym_toggle_pref = (atoi(val) == 1);
    } else if (strcmp(key, "alt_toggle") == 0) {
      kb_alt_toggle_pref = (atoi(val) == 1);
    } else if (strcmp(key, "theme") == 0) {
      theme_pref_str = String(val);
      theme_pref_str.trim();
    } else if (strcmp(key, "topbar_transparant") == 0) {
      topbar_transparant = (atoi(val) == 1);
    } else if (strcmp(key, "sel_solid") == 0) {
      theme_focus_solid = (atoi(val) == 1);
    } else if (strcmp(key, "sel_darken") == 0) {
      theme_focus_darken = (atoi(val) == 1);
    }
  }
  f.close();
  SLog.printf("[FW_PREFS] loaded: use_sd=%d tz=%s clock=%s\n",
                use_sd_pref ? 1 : 0, tz_setting_str.c_str(), clock_fmt_str.c_str());
}

// Auto-baud: T-Deck Plus has shipped with several GPS modules over time.
// Try common rates until one produces valid NMEA checksums.
static const uint32_t GPS_BAUD_CANDIDATES[] = { 9600, 38400, 115200, 19200, 57600, 4800 };
static const uint8_t  GPS_BAUD_COUNT = sizeof(GPS_BAUD_CANDIDATES) / sizeof(GPS_BAUD_CANDIDATES[0]);
static const uint32_t GPS_BAUD_PROBE_MS = 3000;       // try each rate for 3s
static uint8_t        gps_baud_idx = 0;
static uint32_t       gps_baud_probe_start_ms = 0;
static bool           gps_baud_locked = false;
static uint32_t       gps_baud_probe_chars_start = 0;
static bool           gps_serial_active = false;

static void gps_print_stats(const char* tag) {
  uint32_t elapsed = millis() - gps_sync_start_ms;
  uint32_t chars = gps_tinygps.charsProcessed();
  uint32_t delta = chars - gps_last_chars;
  gps_last_chars = chars;

  SLog.printf("[GPS %s] t=%lus chars=%lu(+%lu) sent_with_fix=%lu csum_ok=%lu csum_fail=%lu\n",
                tag,
                (unsigned long)(elapsed / 1000UL),
                (unsigned long)chars,
                (unsigned long)delta,
                (unsigned long)gps_tinygps.sentencesWithFix(),
                (unsigned long)gps_tinygps.passedChecksum(),
                (unsigned long)gps_tinygps.failedChecksum());

  // Satellites in view (from GSV/GGA)
  if (gps_tinygps.satellites.isValid()) {
    SLog.printf("[GPS %s]   sats=%lu (age=%lums)\n",
                  tag,
                  (unsigned long)gps_tinygps.satellites.value(),
                  (unsigned long)gps_tinygps.satellites.age());
  } else {
    SLog.printf("[GPS %s]   sats=--\n", tag);
  }

  // HDOP — lower is better; <5 is usable, <2 is good
  if (gps_tinygps.hdop.isValid()) {
    SLog.printf("[GPS %s]   hdop=%.2f\n", tag, gps_tinygps.hdop.hdop());
  }

  // Date (often appears before full position fix)
  if (gps_tinygps.date.isValid()) {
    SLog.printf("[GPS %s]   date=%04u-%02u-%02u (age=%lums)\n",
                  tag,
                  gps_tinygps.date.year(), gps_tinygps.date.month(), gps_tinygps.date.day(),
                  (unsigned long)gps_tinygps.date.age());
  } else {
    SLog.printf("[GPS %s]   date=INVALID\n", tag);
  }

  // Time
  if (gps_tinygps.time.isValid()) {
    SLog.printf("[GPS %s]   time=%02u:%02u:%02u (age=%lums)\n",
                  tag,
                  gps_tinygps.time.hour(), gps_tinygps.time.minute(), gps_tinygps.time.second(),
                  (unsigned long)gps_tinygps.time.age());
  } else {
    SLog.printf("[GPS %s]   time=INVALID\n", tag);
  }

  // Location (not required for time sync, but useful signal)
  if (gps_tinygps.location.isValid()) {
    SLog.printf("[GPS %s]   loc=%.5f,%.5f (age=%lums)\n",
                  tag,
                  gps_tinygps.location.lat(), gps_tinygps.location.lng(),
                  (unsigned long)gps_tinygps.location.age());
  } else {
    SLog.printf("[GPS %s]   loc=NO FIX YET\n", tag);
  }

  // Diagnostic hint
  if (delta == 0) {
    SLog.printf("[GPS %s]   !! no new bytes — check power/TX pin (expected RX=%d)\n",
                  tag, TDECK_GPS_RX);
  } else if (gps_tinygps.passedChecksum() == 0 && chars > 200 && gps_baud_locked) {
    SLog.printf("[GPS %s]   !! bytes flowing but 0 valid sentences at locked baud %u\n",
                  tag, (unsigned)GPS_BAUD_CANDIDATES[gps_baud_idx]);
  }
}

static void gps_start_probe_at_current_baud() {
  uint32_t baud = GPS_BAUD_CANDIDATES[gps_baud_idx];
  if (gps_serial_active) {
    GPSSerial.updateBaudRate(baud);
  } else {
    GPSSerial.begin(baud, SERIAL_8N1, TDECK_GPS_RX, TDECK_GPS_TX);
    gps_serial_active = true;
  }
  GPSSerial.flush(false);
  gps_baud_probe_start_ms = millis();
  gps_baud_probe_chars_start = gps_tinygps.charsProcessed();
  SLog.printf("[GPS] probing baud=%u (candidate %u/%u)\n",
                (unsigned)baud, (unsigned)(gps_baud_idx + 1), (unsigned)GPS_BAUD_COUNT);
}

static void gps_sync_begin() {
  SLog.printf("[GPS] Listening on UART1 RX=%d TX=%d\n", TDECK_GPS_RX, TDECK_GPS_TX);
  gps_sync_restart();
}

// Returns true once a working baud is locked in.
static bool gps_baud_probe_tick() {
  if (gps_baud_locked) return true;

  uint32_t now = millis();
  uint32_t ok = gps_tinygps.passedChecksum();
  uint32_t fail = gps_tinygps.failedChecksum();

  // Lock as soon as we see ≥2 clean sentences at this rate.
  if (ok >= 2) {
    SLog.printf("[GPS] baud LOCKED at %u (csum_ok=%lu csum_fail=%lu)\n",
                  (unsigned)GPS_BAUD_CANDIDATES[gps_baud_idx],
                  (unsigned long)ok, (unsigned long)fail);
    gps_baud_locked = true;
    return true;
  }

  // Advance to next candidate after the probe window expires.
  if (now - gps_baud_probe_start_ms >= GPS_BAUD_PROBE_MS) {
    uint32_t delta = gps_tinygps.charsProcessed() - gps_baud_probe_chars_start;
    SLog.printf("[GPS] baud %u rejected: chars=+%lu csum_ok=%lu csum_fail=%lu\n",
                  (unsigned)GPS_BAUD_CANDIDATES[gps_baud_idx],
                  (unsigned long)delta, (unsigned long)ok, (unsigned long)fail);
    gps_baud_idx = (gps_baud_idx + 1) % GPS_BAUD_COUNT;
    gps_start_probe_at_current_baud();
  }
  return false;
}

bool gps_sync_is_done() { return gps_sync_done; }

// ── Last-known GPS / clock fallback ─────────────────────────────────────────
// Persisted to the user's default filesystem (the same _storage the message
// logs use). On a cold boot before the GPS gets a fix, this lets us seed the
// RTC with a plausible (if stale) time and prime a last-known location instead
// of starting at the 1970 epoch with no position. Live GPS and a manual time
// set both override it (see gps_sync_poll / _rtc_set_time). Written once per
// sync cycle when a fix lands; tiny key=value file.
static String gps_last_path() {
  return String(the_mesh ? the_mesh->_storage_prefix.c_str() : "") + "/last_gps";
}

static void gps_last_save(double lat, double lon, bool has_loc, uint32_t t) {
  if (!the_mesh || !the_mesh->_storage) return;
  bool is_sd = (the_mesh->_storage != &LittleFS);
  String path = gps_last_path();
  if (is_sd) sd_spi_take();
  File f = the_mesh->_storage->open(path.c_str(), "w", true);
  if (f) {
    f.printf("time=%u\n", (unsigned)t);
    f.printf("hasloc=%d\n", has_loc ? 1 : 0);
    if (has_loc) {
      f.printf("lat=%.6f\n", lat);
      f.printf("lon=%.6f\n", lon);
    }
    f.close();
  }
  if (is_sd) sd_spi_release();
}

// Boot seed: if a saved fix exists, set the RTC (unless a manual override is
// already in effect) and prime the last-known location so own-position lookups
// have something before the first live fix. Does NOT mark a *live* time/location
// fix — the GPS sync keeps running and overwrites these when it succeeds.
static void gps_last_load() {
  if (!the_mesh || !the_mesh->_storage) return;
  bool is_sd = (the_mesh->_storage != &LittleFS);
  String path = gps_last_path();
  if (is_sd) sd_spi_take();
  File f = the_mesh->_storage->open(path.c_str(), "r");
  if (!f) { if (is_sd) sd_spi_release(); return; }

  uint32_t t = 0;
  bool has_loc = false;
  double lat = 0, lon = 0;
  char line[64];
  while (f.available()) {
    int len = 0;
    while (f.available() && len < (int)sizeof(line) - 1) {
      char ch = f.read();
      if (ch == '\n' || ch == '\r') break;
      line[len++] = ch;
    }
    line[len] = '\0';
    if (len == 0) continue;
    char* eq = strchr(line, '=');
    if (!eq) continue;
    *eq = '\0';
    const char* key = line; const char* val = eq + 1;
    if      (strcmp(key, "time") == 0)   t = strtoul(val, nullptr, 10);
    else if (strcmp(key, "hasloc") == 0) has_loc = (atoi(val) == 1);
    else if (strcmp(key, "lat") == 0)    lat = atof(val);
    else if (strcmp(key, "lon") == 0)    lon = atof(val);
  }
  f.close();
  if (is_sd) sd_spi_release();

  if (t > 86400 && !gps_manual_time_override) {  // >1 day past epoch = a real saved time
    MESH_LOCK();
    the_mesh->getRTCClock()->setCurrentTime(t);
    MESH_UNLOCK();
    SLog.printf("[GPS] Seeded RTC from last_gps: %u UTC\n", (unsigned)t);
  }
  if (has_loc && (lat != 0.0 || lon != 0.0)) {
    gps_lat_at_fix = lat;
    gps_lng_at_fix = lon;
    gps_location_valid_at_fix = true;
    SLog.printf("[GPS] Last-known location primed: %.5f, %.5f\n", lat, lon);
  }
}

void gps_sync_poll() {
  if (gps_sync_done) return;
  while (GPSSerial.available()) gps_tinygps.encode(GPSSerial.read());

  uint32_t now = millis();

  // ── Post-fix window: keep reading to collect satellite count from GPGGA ──
  // The time fix often comes from GPRMC before GPGGA (which carries sat count)
  // has been parsed. Wait up to GPS_POST_FIX_MS for a satellite reading to arrive.
  if (gps_time_fix_valid) {
    if (gps_tinygps.satellites.isValid() && gps_tinygps.satellites.value() > 0) {
      gps_sats_at_fix = gps_tinygps.satellites.value();
      gps_hdop_at_fix = gps_tinygps.hdop.isValid() ? gps_tinygps.hdop.value() : 0;
    }
    bool sats_ready = gps_sats_at_fix > 0;
    bool window_expired = (now - gps_fix_acquired_ms >= GPS_POST_FIX_MS);
    if (sats_ready || window_expired) {
      if (window_expired && !sats_ready) {
        SLog.println("[GPS] post-fix window expired; no satellite count received.");
      }
      gps_print_stats("fix-final");
      GPSSerial.println("$PMTK161,0*28");
      gps_sync_done = true;
    }
    return;
  }

  // ── Normal hunt phase ──
  gps_baud_probe_tick();

  if (now - gps_last_stats_ms >= GPS_STATS_INTERVAL_MS) {
    gps_last_stats_ms = now;
    gps_print_stats("stat");
  }

  if (gps_tinygps.date.isValid() && gps_tinygps.time.isValid()
      && gps_tinygps.date.year() >= 2024) {
    DateTime utc(gps_tinygps.date.year(), gps_tinygps.date.month(), gps_tinygps.date.day(),
                 gps_tinygps.time.hour(), gps_tinygps.time.minute(), gps_tinygps.time.second());
    if (!gps_manual_time_override) {
      MESH_LOCK();
      the_mesh->getRTCClock()->setCurrentTime(utc.unixtime());
      MESH_UNLOCK();
    } else {
      SLog.println("[GPS] Manual time override active; skipping RTC update.");
    }

    gps_fix_acquired_ms = now;
    gps_time_fix_valid = true;
    gps_sats_at_fix = gps_tinygps.satellites.isValid() ? gps_tinygps.satellites.value() : 0;
    gps_hdop_at_fix  = gps_tinygps.hdop.isValid()      ? gps_tinygps.hdop.value()       : 0;

    if (gps_tinygps.location.isValid()) {
      gps_lat_at_fix = gps_tinygps.location.lat();
      gps_lng_at_fix = gps_tinygps.location.lng();
      gps_location_valid_at_fix = true;
      SLog.printf("[TZ] captured lat=%.5f lng=%.5f -> auto offset=%d min\n",
                    gps_lat_at_fix, gps_lng_at_fix, (int)tz_auto_offset_minutes());
    } else {
      SLog.println("[TZ] no location at time-fix; auto-tz falls back to UTC");
    }

    // Persist this fix as the last-known GPS for the next cold boot (clock seed
    // + location fallback). Outside MESH_LOCK; runs once per sync cycle.
    gps_last_save(gps_lat_at_fix, gps_lng_at_fix, gps_location_valid_at_fix, utc.unixtime());

    SLog.println("[GPS] ======== FIX ACQUIRED — entering post-fix window ========");
    gps_print_stats("fix");
    SLog.printf("[GPS] RTC set to %u UTC (%04u-%02u-%02u %02u:%02u:%02u) after %lus\n",
                  (unsigned)utc.unixtime(),
                  gps_tinygps.date.year(), gps_tinygps.date.month(), gps_tinygps.date.day(),
                  gps_tinygps.time.hour(), gps_tinygps.time.minute(), gps_tinygps.time.second(),
                  (unsigned long)((now - gps_sync_start_ms) / 1000UL));
    return;
  }

  if (now - gps_sync_start_ms > GPS_SYNC_TIMEOUT_MS) {
    SLog.println("[GPS] ======== TIMEOUT ========");
    gps_print_stats("timeout");
    SLog.printf("[GPS] No fix after %us. Move to open sky for cold start (can take 30s-5min+).\n",
                  (unsigned)(GPS_SYNC_TIMEOUT_MS / 1000));
    GPSSerial.println("$PMTK161,0*28");
    gps_sync_done = true;
  }
}

// Last known GPS location (captured at the most recent time-fix; held until the
// next sync restart). Read by the mesh task to stamp messages — see
// meshpunk_sync.h. Unlocked read of values that change only once per sync cycle;
// a rare torn read just yields a slightly-off coordinate, acceptable here.
bool meshpunk_gps_last_fix(double* lat, double* lon) {
  if (!gps_location_valid_at_fix) return false;
  if (lat) *lat = gps_lat_at_fix;
  if (lon) *lon = gps_lng_at_fix;
  return true;
}

void gps_sync_restart() {
  new (&gps_tinygps) TinyGPSPlus();
  if (gps_serial_active) {
    GPSSerial.write(0xFF);
    GPSSerial.flush(false);
  }
  gps_sync_done = false;
  gps_sync_start_ms = millis();
  gps_last_stats_ms = gps_sync_start_ms;
  gps_last_chars = 0;
  gps_fix_acquired_ms = 0;
  gps_baud_idx = 0;
  gps_baud_locked = false;
  gps_location_valid_at_fix = false;
  gps_time_fix_valid = false;
  gps_sats_at_fix = 0;
  gps_hdop_at_fix = 0;
  SLog.printf("[GPS] Restarting sync (auto-baud, timeout=%us)\n",
                (unsigned)(GPS_SYNC_TIMEOUT_MS / 1000));
  gps_start_probe_at_current_baud();
}

// Trackball click — fed into LVGL as LV_KEY_ENTER via keyboard_read_cb
volatile int trackball_click = 0;

void IRAM_ATTR ISR_click() {
  static uint32_t last_click_ms = 0;
  uint32_t now = millis();
  if (now - last_click_ms < 1) return;
  last_click_ms = now;
  trackball_click = 1;
}

// Trackball direction counters — each ISR fires on a FALLING edge pulse
// from the T-Deck trackball. The keyboard_read_cb consumes these as
// LV_KEY_UP/DOWN/LEFT/RIGHT presses.
volatile int trackball_up = 0;
volatile int trackball_down = 0;
volatile int trackball_left = 0;
volatile int trackball_right = 0;

void IRAM_ATTR ISR_trackball_up()    { trackball_up++; }
void IRAM_ATTR ISR_trackball_down()  { trackball_down++; }
void IRAM_ATTR ISR_trackball_left()  { trackball_left++; }
void IRAM_ATTR ISR_trackball_right() { trackball_right++; }

// Keyboard I2C defines
#define LILYGO_KB_SLAVE_ADDRESS 0x55
#define LILYGO_KB_BRIGHTNESS_CMD 0x01
#define LILYGO_KB_ALT_B_BRIGHTNESS_CMD 0x02
#define LILYGO_KB_MODE_RAW_CMD 0x03
#define LILYGO_KB_MODE_KEY_CMD 0x04

// Keyboard matrix dimensions (from stock ESP32-C3 firmware)
#define KB_COLS 5
#define KB_ROWS 7

// Modifier key positions in the matrix
#define KB_MOD_SYM_COL    0
#define KB_MOD_SYM_ROW    2
#define KB_MOD_ALT_COL    0
#define KB_MOD_ALT_ROW    4
#define KB_MOD_LSHIFT_COL 1
#define KB_MOD_LSHIFT_ROW 6
#define KB_MOD_RSHIFT_COL 2
#define KB_MOD_RSHIFT_ROW 3
#define KB_KEY_ENTER_COL  3
#define KB_KEY_ENTER_ROW  3
#define KB_KEY_BS_COL     4
#define KB_KEY_BS_ROW     3
#define KB_KEY_SPACE_COL  0
#define KB_KEY_SPACE_ROW  5

// Normal character layer (col × row) — from stock C3 firmware Keyboard_ESP32C3.ino
static const char kb_matrix[KB_COLS][KB_ROWS] = {
  {'q','w',  0, 'a',  0, ' ',  0 },
  {'e','s','d','p','x','z',  0 },
  {'r','g','t',  0, 'v','c','f'},
  {'u','h','y',  0, 'b','n','j'},
  {'o','l','i',  0, '$','m','k'},
};

// Symbol character layer
static const char kb_matrix_symbol[KB_COLS][KB_ROWS] = {
  {'#','1',  0, '*',  0,   0, '0'},
  {'2','4','5','@','8','7',  0 },
  {'3','/',  '(',  0, '?','9','6'},
  {'_',':',')',  0, '!',',',';'},
  {'+','"','-',  0,   0, '.','\''},
};

// ── Alt emoji layer map ─────────────────────────────────────────────────────
// Default emoji per key (normal-layer char -> Unicode codepoint), roughly the
// most-used emojis with a few mnemonics (z=sleep, $=money, h=haha). Space is
// deliberately absent so latched emoji runs can still be space-separated.
// Sequence emojis are assignable too: the picker stores their PUA codepoint
// and the send path decomposes it to real Unicode (see prepare_outgoing_text).
struct KbEmojiDefault { char key; uint32_t cp; };
static const KbEmojiDefault kb_emoji_defaults[] = {
  {'q',0x1F923},{'w',0x1F609},{'e',0x1F60D},{'r',0x1F917},{'t',0x1F44D},
  {'y',0x1F642},{'u',0x1F937},{'i',0x1F60A},{'o',0x1F618},{'p',0x1F970},
  {'a',0x2764}, {'s',0x263A}, {'d',0x1F62D},{'f',0x1F525},{'g',0x1F601},
  {'h',0x1F602},{'j',0x1F605},{'k',0x1F64F},{'l',0x1F606},
  {'z',0x1F634},{'x',0x1F926},{'c',0x1F97A},{'v',0x1F495},{'b',0x1F382},
  {'n',0x1F644},{'m',0x1F914},{'$',0x1F4B0},
};

// Active map, indexed by the key's normal-layer char. 0 = no emoji (the key
// falls through to its normal char under alt).
static uint32_t kb_emoji_map[128] = {0};

static void kb_emoji_apply_defaults() {
  memset(kb_emoji_map, 0, sizeof(kb_emoji_map));
  for (auto &d : kb_emoji_defaults) kb_emoji_map[(uint8_t)d.key] = d.cp;
}

// /emoji_keymap on LittleFS: one "c=1F602" line per key (hex codepoint; 0
// clears the key). Defaults apply first, then the file overrides — so a
// missing file or a key the file doesn't mention means the compiled default.
static void kb_emoji_map_load() {
  kb_emoji_apply_defaults();
  File f = LittleFS.open("/emoji_keymap", "r");
  if (!f) return;
  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.length() < 3 || line[1] != '=') continue;
    uint8_t c = (uint8_t)line[0];
    if (c >= 128) continue;
    kb_emoji_map[c] = (uint32_t)strtoul(line.c_str() + 2, nullptr, 16);
  }
  f.close();
}

static void kb_emoji_map_save() {
  File f = LittleFS.open("/emoji_keymap", "w", true);
  if (!f) { SLog.println("[KB_EMOJI] cannot write /emoji_keymap"); return; }
  for (int c = 32; c < 128; c++) {
    bool is_default_key = false;
    for (auto &d : kb_emoji_defaults) {
      if ((uint8_t)d.key == c) { is_default_key = true; break; }
    }
    // Write every assigned key, plus explicit "=0" lines for cleared default
    // keys so a cleared key doesn't resurrect its default on the next boot.
    if (kb_emoji_map[c] || is_default_key)
      f.printf("%c=%lX\n", (char)c, (unsigned long)kb_emoji_map[c]);
  }
  f.close();
}

// Pack a codepoint's UTF-8 bytes into a uint32 with the FIRST byte in the
// LOW byte (U+1F602 = F0 9F 98 82 -> 0x82989FF0). lv_textarea_add_char()
// reinterprets the uint32's memory as the char sequence
// (letter_buf = (char *)&u32_buf), and the ESP32-S3 is little-endian, so
// memory order = low byte first.
static uint32_t utf8_pack_key(uint32_t cp) {
  if (cp < 0x80u) return cp;
  if (cp < 0x800u)
    return  (0xC0u | (cp >> 6)) |
           ((0x80u | (cp & 0x3Fu)) << 8);
  if (cp < 0x10000u)
    return  (0xE0u | (cp >> 12)) |
           ((0x80u | ((cp >> 6) & 0x3Fu)) << 8) |
           ((0x80u | (cp & 0x3Fu)) << 16);
  return  (0xF0u | (cp >> 18)) |
         ((0x80u | ((cp >> 12) & 0x3Fu)) << 8) |
         ((0x80u | ((cp >> 6) & 0x3Fu)) << 16) |
         ((0x80u | (cp & 0x3Fu)) << 24);
}

// Data directory paths
#define LUA_PATH "/lua/"
#define SOUNDS_PATH "/sounds/"
#define IMAGES_PATH "/images/"

// Ticker for LVGL timing
Ticker lvgl_ticker;

// LVGL display and touch globals
TFT_eSPI tft;
TouchDrvGT911 touch;

// LuaVGL state
lua_State *L = NULL;

// Keyboard variables
bool keyboard_available = false;
char last_key = 0;

// Filesystem variables
bool fs_mounted = false;
bool sd_mounted = false;


// sd_spi_take() / sd_spi_release() — mutex-based.
// TAKE (inline in meshpunk_sync.h) acquires SPI_LOCK.
// RELEASE releases SPI_LOCK. The historical TFT-reinit poke (SLPOUT/DISPON)
// that used to live here is gone: the bus mutex now serializes TFT access
// against SD and radio, so the display never observes a mid-transaction bus.
void sd_spi_release() {
  SPI_UNLOCK();
}

// List dir helper
void listDir(fs::FS &fs, const char *dirname, int level = 0) {
  File root = fs.open(dirname);
  if (!root || !root.isDirectory()) {
    SLog.print("Failed to open directory: ");
    SLog.println(dirname);
    return;
  }

  File file = root.openNextFile();
  while (file) {
    for (int i = 0; i < level; i++) SLog.print("  ");
    SLog.print(dirname);
    SLog.print("/");
    SLog.print(file.name());
    SLog.print(":");
    SLog.print(file.size());
    SLog.println("b");

    if (file.isDirectory()) {
      String path = String(dirname);
      if (!path.endsWith("/")) path += "/";
      path += file.name();
      listDir(fs, path.c_str(), level + 1);
    }

    file = root.openNextFile();
  }
}

// Helper functions for Lua file loading
String readFile(const char *filename) {
  if (!fs_mounted) {
    SLog.println("Filesystem not mounted!");
    return "";
  }

  fs::File file = LittleFS.open(filename, "r");
  if (!file) {
    SLog.print("Failed to open file: ");
    SLog.println(filename);
    return "";
  }

  String content = "";
  while (file.available()) {
    content += (char)file.read();
  }
  file.close();

  return content;
}

// (LuaFileChunkReader + lua_file_chunk_reader moved up near luaL_loadfilex so
// both the loadfile override and the require() searcher share the streaming path.)

// -- Replaced by safe_open version that parses L:/S: prefix
// static int lua_io_open(lua_State *L) {
//   const char *filename = luaL_checkstring(L, 1);
//   const char *mode = luaL_optstring(L, 2, "r");
//
//   SLog.print("io.open: ");
//   SLog.print(filename);
//   SLog.print(" mode: ");
//   SLog.println(mode);
//
//   const char *fs_mode;
//   if (strcmp(mode, "r") == 0) {
//     fs_mode = "r";
//   } else if (strcmp(mode, "w") == 0) {
//     fs_mode = "w";
//   } else {
//     lua_pushnil(L);
//     lua_pushstring(L, "Only 'r' and 'w' modes supported");
//     return 2;
//   }
//
//   fs::File f = LittleFS.open(filename, fs_mode);
//   if (!f) {
//     lua_pushnil(L);
//     lua_pushstring(L, "Failed to open file");
//     return 2;
//   }
//
//   fs::File *file = new fs::File(f);
//   fs::File **ud = (fs::File **)lua_newuserdata(L, sizeof(fs::File *));
//   *ud = file;
//
//   luaL_getmetatable(L, "esp32_file");
//   lua_setmetatable(L, -2);
//   return 1;
// }

// File handle struct to track which filesystem a file belongs to
struct LuaFileHandle {
    fs::File* file;
    bool is_sd;
};

// Safe io.open that parses L: (LittleFS) or S: (SD) prefix
// Usage: io.open("L:/lua/apps/myapp/save.txt", "r")
//        io.open("S:/meshpunk/apps/myapp/save.txt", "w")
//        io.open("/lua/apps/myapp/save.txt", "r")  -- defaults to LittleFS
static int lua_io_open(lua_State *L) {
  const char *filename = luaL_checkstring(L, 1);
  const char *mode = luaL_optstring(L, 2, "r");

  // Validate mode
  if (strcmp(mode, "r") != 0 && strcmp(mode, "w") != 0 && strcmp(mode, "a") != 0) {
    lua_pushnil(L);
    lua_pushstring(L, "Only 'r', 'w', and 'a' modes supported");
    return 2;
  }

  // Create the userdata BEFORE opening the file: lua_newuserdata can longjmp
  // on OOM, and a longjmp skips C++ destructors — an already-open File (and
  // its held SPI lock) would leak. With the userdata first, a failed open just
  // leaves a dead wrapper for GC (__gc sees file == nullptr, a no-op).
  LuaFileHandle *ud = (LuaFileHandle *)lua_newuserdata(L, sizeof(LuaFileHandle));
  ud->file = nullptr;
  ud->is_sd = false;
  luaL_getmetatable(L, "esp32_file");
  lua_setmetatable(L, -2);

  // Lua io.open defaults to LittleFS (L:) when no prefix given
  MeshpunkFile mf = meshpunk_open(filename, mode, /*default_sd=*/false);
  if (!mf.valid) {
    lua_pushnil(L);
    lua_pushstring(L, mf.is_sd ? "Failed to open file on SD"
                                : "Failed to open file");
    return 2;
  }

  // meshpunk_open holds the SPI lock for SD files. The Lua file handle
  // tracks is_sd so the read/write/close methods release it properly.
  // Release the SPI lock now — Lua file ops re-acquire per-call.
  if (mf.is_sd) sd_spi_release();

  fs::File *file = new (std::nothrow) fs::File(mf.file);
  if (!file) {
    mf.file.close();
    lua_pushnil(L);
    lua_pushstring(L, "Out of memory");
    return 2;
  }
  ud->file = file;
  ud->is_sd = mf.is_sd;
  return 1;
}

// LilyGo T-Deck control backlight chip has 16 levels of adjustment range
// The adjustable range is 0~15, 0 is the minimum brightness, 15 is the maximum
// brightness
void setBrightness(uint8_t value) {
  static uint8_t level = 0;
  static uint8_t steps = 16;
  if (value == 0) {
    digitalWrite(BOARD_BL_PIN, 0);
    delay(3);
    level = 0;
    return;
  }
  if (level == 0) {
    digitalWrite(BOARD_BL_PIN, 1);
    level = steps;
    delayMicroseconds(30);
  }
  int from = steps - level;
  int to = steps - value;
  int num = (steps + to - from) % steps;
  for (int i = 0; i < num; i++) {
    digitalWrite(BOARD_BL_PIN, 0);
    digitalWrite(BOARD_BL_PIN, 1);
  }
  level = value;
}

// Helper: read the ILI9341 current scanline position via command 0x45.
// Returns 0–319 indicating the gate line the panel is currently refreshing.
static uint16_t ili9341_get_scanline() {
  uint8_t hi = tft.readcommand8(0x45, 1); // GTS[8]
  uint8_t lo = tft.readcommand8(0x45, 2); // GTS[7:0]
  return ((hi & 0x01) << 8) | lo;
}

// Scanline-tracking flush callback.
//
// The ILI9341 physically scans gate lines 0→319 regardless of MADCTL
// rotation settings. In landscape rotation 1 (MADCTL MV|MX), the gate
// scan sweeps horizontally across the screen, so the scanline value
// approximately maps to the LVGL x-coordinate.
//
// Strategy: before writing pixels, read the current scanline. If it is
// inside (or just ahead of) the flush area, busy-wait for it to pass.
// This makes our SPI writes trail behind the panel's read pointer,
// preventing the display from showing a mix of old and new data.
//
// The SCANLINE_MARGIN adds a safety buffer — we wait until the scanline
// is at least this many lines past the end of our flush area before
// writing, to account for SPI transaction setup time.

#define SCANLINE_MARGIN 8

static void disp_flush_cb(lv_display_t *disp, const lv_area_t *area,
                          uint8_t *px_map) {
  uint32_t w = (area->x2 - area->x1 + 1);
  uint32_t h = (area->y2 - area->y1 + 1);

  SPI_LOCK();

  // Read current scanline position.
  // In rotation 1 the gate scan maps to the y-axis of the flush area
  // (the ILI9341's 320 native rows become the 240-pixel vertical axis
  // after MV swap + rotation). Try y1/y2 first; if tearing persists,
  // switch flush_start/flush_end to use x1/x2 instead.
  uint16_t scanline = ili9341_get_scanline();
  uint16_t flush_start = area->y1;
  uint16_t flush_end   = area->y2 + SCANLINE_MARGIN;

  // Busy-wait if the scanline is inside (or about to enter) the flush
  // area.  Timeout after ~8 ms to avoid blocking the system forever
  // if readcommand8 returns garbage (e.g. MISO not connected).
  int wait_us = 0;
  while (scanline >= flush_start && scanline <= flush_end && wait_us < 8000) {
    delayMicroseconds(10);
    wait_us += 10;
    scanline = ili9341_get_scanline();
  }
  tft.startWrite();
  tft.setAddrWindow(area->x1, area->y1, w, h);
  tft.pushColors((uint16_t *)px_map, w * h, false);
  tft.endWrite();

  SPI_UNLOCK();

  lv_display_flush_ready(disp);
}


// Touch handling
int16_t x[5], y[5];

// Debug flag for touch
bool touch_debug = true;
unsigned long last_touch_debug = 0;

// Keyboard functions
void setKeyboardBrightness(uint8_t value) {
  if (!keyboard_available)
    return;

  Wire.beginTransmission(LILYGO_KB_SLAVE_ADDRESS);
  Wire.write(LILYGO_KB_BRIGHTNESS_CMD);
  Wire.write(value);
  Wire.endTransmission();
}

void setKeyboardDefaultBrightness(uint8_t value) {
  if (!keyboard_available)
    return;

  Wire.beginTransmission(LILYGO_KB_SLAVE_ADDRESS);
  Wire.write(LILYGO_KB_ALT_B_BRIGHTNESS_CMD);
  Wire.write(value);
  Wire.endTransmission();
}

// Reset inactivity timer and restore backlights — called after a native module
// exits so the screen/keyboard don't appear timed-out to the user.
void wake_activity() {
  last_activity_ms = millis();
  if (screen_timed_out) { setBrightness(display_brightness); screen_timed_out = false; }
  if (kbd_timed_out)    { setKeyboardBrightness(kbd_brightness); kbd_timed_out = false; }
}

// Keyboard state tracking variables
static uint32_t last_key_code = 0;
static uint8_t prev_matrix[KB_COLS] = {0};
static bool kb_key_state[128] = {0};
static bool kb_key_prev[128] = {0};
static uint32_t kb_key_press_time[128] = {0};
static const uint32_t KEY_HOLD_THRESHOLD_MS = 400;
static bool kb_shift_active = false;
static bool kb_lshift_active = false;
static bool kb_rshift_active = false;
static bool kb_sym_active = false;
static bool kb_alt_active = false;

// Navigation controller state — a STACK of navigable scopes, not a single
// container. The TOP scope is the interactive one (in the focus group, gridnav
// armed for trackball); scopes beneath are suspended (a popup over a view, or a
// row-select list over its controls). Pushing suspends the scope below; popping
// resumes it. A single global container could not represent nesting, so apps
// hand-rolled a save/restore dance and a single deferred-removal slot that could
// clobber itself — this replaces both. `armed` tracks whether gridnav is
// currently added to the top container (touch removes it so the finger can
// scroll; trackball re-adds it), exactly as the old `nav_gridnav_active` did.
struct NavScope {
    lv_obj_t *cont;
    lv_gridnav_ctrl_t flags;
    bool armed;
};
#define NAV_STACK_MAX 6
static NavScope nav_stack[NAV_STACK_MAX];
static int nav_depth = 0;
static lv_obj_t *pending_gridnav_remove = NULL;

static inline NavScope *nav_top() {
    return nav_depth > 0 ? &nav_stack[nav_depth - 1] : NULL;
}

static void flush_pending_gridnav() {
    if (pending_gridnav_remove) {
        if (lv_obj_is_valid(pending_gridnav_remove)) {
            lv_gridnav_remove(pending_gridnav_remove);
        }
        pending_gridnav_remove = NULL;
    }
}

// Drop any scopes whose container was freed (an app deleted its view without
// resetting nav). Gridnav's own LV_EVENT_DELETE handler frees its resources; we
// just compact the stack so nav_top() never points at freed memory.
static void nav_check_valid() {
    int w = 0;
    for (int i = 0; i < nav_depth; i++) {
        if (nav_stack[i].cont && lv_obj_is_valid(nav_stack[i].cont)) {
            nav_stack[w++] = nav_stack[i];
        }
    }
    nav_depth = w;
}

static lv_obj_t *nav_find_visible_child(lv_obj_t *cont) {
    int32_t scroll_top = lv_obj_get_scroll_top(cont);
    int32_t cont_h = lv_obj_get_content_height(cont);
    uint32_t cnt = lv_obj_get_child_count(cont);
    for (uint32_t i = 0; i < cnt; i++) {
        lv_obj_t *child = lv_obj_get_child(cont, i);
        if (lv_obj_has_flag(child, LV_OBJ_FLAG_HIDDEN)) continue;
        if (!lv_obj_has_flag(child, LV_OBJ_FLAG_CLICKABLE)) continue;
        int32_t cy = lv_obj_get_y(child);
        int32_t ch = lv_obj_get_height(child);
        if (cy + ch > scroll_top && cy < scroll_top + cont_h) return child;
    }
    return NULL;
}

// Arm a scope: add it to the focus group, focus it, and attach gridnav. This is
// the exact, proven sequence the old _nav_setup used. preserve_scroll matters
// when re-entering a scrolled list (resume / row-select): add to the group and
// focus BEFORE lv_gridnav_add so the FOCUSED event doesn't snap to child 0, then
// pin focus to the first on-screen child instead of scrolling back to the top.
static void nav_install(NavScope *s, bool preserve_scroll) {
    if (!s || !s->cont || !lv_obj_is_valid(s->cont)) return;
    if (preserve_scroll) {
        lv_group_add_obj(lv_group_get_default(), s->cont);
        lv_group_focus_obj(s->cont);
        lv_gridnav_add(s->cont, s->flags);
        lv_obj_t *vis = nav_find_visible_child(s->cont);
        if (vis) lv_gridnav_set_focused(s->cont, vis, LV_ANIM_OFF);
    } else {
        lv_gridnav_add(s->cont, s->flags);
        lv_group_add_obj(lv_group_get_default(), s->cont);
        lv_group_focus_obj(s->cont);
    }
    s->armed = true;
}

// Reset the whole nav stack (app exit / full teardown). Mirrors the old
// _nav_clear's immediate gridnav removal, applied to every live scope.
// Registered under both _nav_clear (back-compat) and _nav_reset.
static int lua_nav_reset(lua_State *L) {
    (void)L;
    flush_pending_gridnav();
    nav_check_valid();
    for (int i = 0; i < nav_depth; i++) {
        if (nav_stack[i].cont && lv_obj_is_valid(nav_stack[i].cont)) {
            lv_gridnav_remove(nav_stack[i].cont);
        }
        nav_stack[i].cont = NULL;
        nav_stack[i].armed = false;
    }
    nav_depth = 0;
    return 0;
}

// LVGL keyboard read callback
static bool trackball_btn_pressed = false;

static void keyboard_read_cb(lv_indev_t *indev, lv_indev_data_t *data) {
  flush_pending_gridnav();
  nav_check_valid();

  static uint32_t kb_mapped_key = 0;
  bool any_new = false;
  bool key_from_trackball = false;

  // ── Read raw keyboard matrix (5 bytes) ──
  uint8_t cur_matrix[KB_COLS] = {0};
  Wire.requestFrom(LILYGO_KB_SLAVE_ADDRESS, KB_COLS);
  for (int c = 0; c < KB_COLS && Wire.available(); c++) {
    cur_matrix[c] = Wire.read();
  }

  // ── Decode modifier key states ──
  kb_lshift_active = cur_matrix[KB_MOD_LSHIFT_COL] & (1 << KB_MOD_LSHIFT_ROW);
  kb_rshift_active = cur_matrix[KB_MOD_RSHIFT_COL] & (1 << KB_MOD_RSHIFT_ROW);
  kb_shift_active = kb_lshift_active || kb_rshift_active;

  // alt: same tap-toggle latch as sym below, behind its own pref. The emoji
  // layer it drives only substitutes while typing in a textarea (see the
  // substitution block after the resolver), so a latched alt never pauses
  // WASD nav the way a latched sym does.
  bool alt_phys = cur_matrix[KB_MOD_ALT_COL] & (1 << KB_MOD_ALT_ROW);
  kb_alt_active = alt_phys;
  if (kb_alt_toggle_pref) {
    if (alt_phys && !kb_alt_phys_prev) {
      kb_alt_used_while_held = false;             // new hold begins
    } else if (!alt_phys && kb_alt_phys_prev && !kb_alt_used_while_held) {
      kb_alt_latched = !kb_alt_latched;           // clean tap — toggle latch
    }
    kb_alt_layer_active = alt_phys || kb_alt_latched;
  } else {
    kb_alt_layer_active = alt_phys;
    kb_alt_latched = false;
  }
  kb_alt_phys_prev = alt_phys;

  // sym: plain hold by default. In toggle mode a clean tap (press + release
  // with nothing typed during the hold) latches the symbol layer until the
  // next tap; holding sym while typing still works as a momentary modifier.
  bool sym_phys = cur_matrix[KB_MOD_SYM_COL] & (1 << KB_MOD_SYM_ROW);
  if (kb_sym_toggle_pref) {
    if (sym_phys && !kb_sym_phys_prev) {
      kb_sym_used_while_held = false;             // new hold begins
    } else if (!sym_phys && kb_sym_phys_prev && !kb_sym_used_while_held) {
      kb_sym_latched = !kb_sym_latched;           // clean tap — toggle latch
    }
    kb_sym_active = sym_phys || kb_sym_latched;
  } else {
    kb_sym_active = sym_phys;
    kb_sym_latched = false;
  }
  kb_sym_phys_prev = sym_phys;

  // ── Snapshot previous key state and clear current ──
  memcpy(kb_key_prev, kb_key_state, 128);
  memset(kb_key_state, 0, 128);

  // ── Resolve ALL pressed keys from matrix ──
  uint32_t resolved_key = 0;

  for (int c = 0; c < KB_COLS; c++) {
    if (cur_matrix[c] == 0) continue;
    for (int r = 0; r < KB_ROWS; r++) {
      if (!(cur_matrix[c] & (1 << r))) continue;
      if (c == KB_MOD_SYM_COL && r == KB_MOD_SYM_ROW) continue;
      if (c == KB_MOD_ALT_COL && r == KB_MOD_ALT_ROW) continue;
      if (c == KB_MOD_LSHIFT_COL && r == KB_MOD_LSHIFT_ROW) continue;
      if (c == KB_MOD_RSHIFT_COL && r == KB_MOD_RSHIFT_ROW) continue;
      // (0,6) is the mic key: no normal-layer character (the ch==0 check
      // below keeps the bare press dead), but its symbol layer is '0' —
      // it must flow through so sym+mic can type the digit zero.

      uint8_t ch = 0;
      if (c == KB_KEY_ENTER_COL && r == KB_KEY_ENTER_ROW) {
        ch = 0x0D;
      } else if (c == KB_KEY_BS_COL && r == KB_KEY_BS_ROW) {
        ch = 0x08;
      } else {
        ch = kb_sym_active ? kb_matrix_symbol[c][r] : kb_matrix[c][r];
        if (ch == 0) continue;
        if (kb_shift_active && ch >= 'a' && ch <= 'z') ch -= 32;
      }

      kb_key_state[ch] = true;
      if (!kb_key_prev[ch]) {
        kb_key_press_time[ch] = millis();
      }
      if (sym_phys) kb_sym_used_while_held = true;  // hold was used — not a tap
      if (alt_phys) kb_alt_used_while_held = true;

      if (resolved_key == 0) {
        if (ch == 0x0D) resolved_key = LV_KEY_ENTER;
        else if (ch == 0x08) resolved_key = LV_KEY_BACKSPACE;
        else resolved_key = ch;
      }
    }
  }

  bool kb_active = (resolved_key != 0);

  // ── WASD intercept — treat as direction, not character (unless typing) ──
  uint32_t wasd_dir = 0;
  lv_obj_t *focused = lv_group_get_focused(lv_group_get_default());
  bool typing = focused && lv_obj_is_valid(focused) && lv_obj_check_type(focused, &lv_textarea_class);

  // ── Alt emoji layer — substitute AFTER resolution, only while typing ──
  // kb_key_state[] above stays indexed by the base char (it's a 128-slot
  // array); outside a textarea the layer is inert. Sym wins when both are
  // active, shift is ignored (alt+shift+A = same emoji as alt+a), and an
  // unmapped key falls through to its normal char. The key value carries the
  // emoji's UTF-8 bytes packed low-byte-first (see utf8_pack_key).
  if (typing && kb_alt_layer_active && !kb_sym_active &&
      resolved_key >= 0x20 && resolved_key < 0x80) {
    uint32_t base = resolved_key;
    if (base >= 'A' && base <= 'Z') base += 32;
    uint32_t cp = kb_emoji_map[base];
    // emoji_preload gates on the ACTIVE blob: a mapped emoji the current set
    // can't render (e.g. a sequence PUA after the SD extended set was
    // removed) falls through to the plain char instead of emitting a
    // codepoint that would draw tofu here and on the receiving device.
    if (cp && emoji_preload(cp)) resolved_key = utf8_pack_key(cp);
  }

  if (!typing) {
    if      (resolved_key == 'w') wasd_dir = LV_KEY_UP;
    else if (resolved_key == 'a') wasd_dir = LV_KEY_LEFT;
    else if (resolved_key == 's') wasd_dir = LV_KEY_DOWN;
    else if (resolved_key == 'd') wasd_dir = LV_KEY_RIGHT;
    if (wasd_dir) kb_active = false;
  }

  // ── LVGL state tracking (single-key for LVGL reporting) ──
  if (kb_active) {
    if (resolved_key != last_key_code) {
      last_key_code = resolved_key;
      kb_mapped_key = resolved_key;
      any_new = true;
      last_activity_ms = millis();
    }
  } else {
    if (last_key_code != 0) last_key_code = 0;
  }

  memcpy(prev_matrix, cur_matrix, KB_COLS);

  // ── Direction navigation — WASD + trackball, shared sensitivity ──
  if (!kb_active) {
    if (trackball_click > 0) {
      trackball_click = 0;
      last_key_code = LV_KEY_ENTER;
      any_new = true;
      key_from_trackball = true;
      trackball_btn_pressed = true;
    } else {
      uint32_t nav_dir = wasd_dir;
      if (!nav_dir) {
        if      (trackball_up > 0)    nav_dir = LV_KEY_UP;
        else if (trackball_down > 0)  nav_dir = LV_KEY_DOWN;
        else if (trackball_left > 0)  nav_dir = LV_KEY_LEFT;
        else if (trackball_right > 0) nav_dir = LV_KEY_RIGHT;
      }

      if (nav_dir) {
        static uint32_t last_nav_ms = 0;
        uint32_t now = millis();
        uint16_t interval = (!wasd_dir && trackball_roll_ms > 0)
                            ? trackball_roll_ms : trackball_sensitivity_ms;
        if (now - last_nav_ms >= interval) {
          last_nav_ms = now;
          if (!wasd_dir) {
            if (trackball_roll_ms > 0) {
              if      (nav_dir == LV_KEY_UP)    trackball_up--;
              else if (nav_dir == LV_KEY_DOWN)  trackball_down--;
              else if (nav_dir == LV_KEY_LEFT)  trackball_left--;
              else if (nav_dir == LV_KEY_RIGHT) trackball_right--;
            } else {
              if      (nav_dir == LV_KEY_UP)    trackball_up = 0;
              else if (nav_dir == LV_KEY_DOWN)  trackball_down = 0;
              else if (nav_dir == LV_KEY_LEFT)  trackball_left = 0;
              else if (nav_dir == LV_KEY_RIGHT) trackball_right = 0;
            }
          }
          last_key_code = nav_dir;
          kb_mapped_key = nav_dir;
          any_new = true;
          key_from_trackball = true;
        }
      }
    }
    if (key_from_trackball) last_activity_ms = millis();
  }

  // ── Re-enable gridnav on trackball input (top scope only) ──
  NavScope *kb_top = nav_top();
  if (key_from_trackball && kb_top && kb_top->cont &&
      lv_obj_is_valid(kb_top->cont) && !kb_top->armed) {
    uint32_t cnt = lv_obj_get_child_count(kb_top->cont);
    for (uint32_t i = 0; i < cnt; i++) {
      lv_obj_remove_state(lv_obj_get_child(kb_top->cont, i),
                          LV_STATE_FOCUSED | LV_STATE_FOCUS_KEY | LV_STATE_EDITED);
    }
    lv_group_focus_obj(kb_top->cont);
    lv_gridnav_add(kb_top->cont, kb_top->flags);
    kb_top->armed = true;
    lv_obj_t *vis = nav_find_visible_child(kb_top->cont);
    if (vis) {
      lv_gridnav_set_focused(kb_top->cont, vis, LV_ANIM_OFF);
    }
  }

  // ── Wake from timeout ──
  if (any_new) {
    if (screen_timed_out) { setBrightness(display_brightness); screen_timed_out = false; }
    if (kbd_timed_out)    { setKeyboardBrightness(kbd_brightness); kbd_timed_out = false; }
  }

  // ── Report to LVGL ──
  if (kb_active) {
    data->state = LV_INDEV_STATE_PRESSED;
    data->key = kb_mapped_key;
  } else if (any_new) {
    data->state = LV_INDEV_STATE_PRESSED;
    data->key = last_key_code;
  } else if (trackball_btn_pressed) {
    if (digitalRead(TDECK_TRACKBALL_CLICK) == LOW) {
      data->state = LV_INDEV_STATE_PRESSED;
      data->key = LV_KEY_ENTER;
    } else {
      data->state = LV_INDEV_STATE_RELEASED;
      trackball_btn_pressed = false;
    }
  } else {
    data->state = LV_INDEV_STATE_RELEASED;
  }
}

static void touchpad_read_cb(lv_indev_t *indev, lv_indev_data_t *data) {
  flush_pending_gridnav();
  nav_check_valid();

  uint8_t touched = touch.getPoint(x, y, touch.getSupportTouchPoint());
  if (touched > 0) {
    data->state = LV_INDEV_STATE_PRESSED;
    data->point.x = x[0];
    data->point.y = y[0];
    last_activity_ms = millis();
    if (screen_timed_out) { setBrightness(display_brightness); screen_timed_out = false; }
    if (kbd_timed_out)    { setKeyboardBrightness(kbd_brightness); kbd_timed_out = false; }

    // Touch disarms gridnav on the top scope so the finger scrolls instead of
    // moving focus; trackball re-arms it (above). Safe point: indev callback,
    // not inside a gridnav event dispatch.
    NavScope *tp_top = nav_top();
    if (tp_top && tp_top->cont && tp_top->armed) {
      uint32_t cnt = lv_obj_get_child_count(tp_top->cont);
      for (uint32_t i = 0; i < cnt; i++) {
        lv_obj_remove_state(lv_obj_get_child(tp_top->cont, i),
                            LV_STATE_FOCUSED | LV_STATE_FOCUS_KEY);
      }
      lv_gridnav_remove(tp_top->cont);
      tp_top->armed = false;
    }
  } else {
    data->state = LV_INDEV_STATE_RELEASED;
  }
}

// Setup Serial Protocol

void handleWebSerialCommands() {
  if (Serial.available()) {
    String cmd = Serial.readStringUntil('\n');
    cmd.trim();

    if (cmd.startsWith("READ ")) {
      String path = cmd.substring(5);
      File f = LittleFS.open(path, "r");
      if (!f) {
        SLog.println("ERR: Cannot open file");
        return;
      }

      while (f.available()) {
        Serial.write(f.read());
      }
      f.close();
      SLog.println(); // newline after file content
      SLog.println("OK");
    }

    else if (cmd.startsWith("WRITE ")) {
      String path = cmd.substring(6);
      File f = LittleFS.open(path, "w");
      if (!f) {
        SLog.println("ERR: Cannot open file for writing");
        return;
      }

      while (!Serial.available()); // wait for next line (start of file content)
      String content = Serial.readStringUntil(0x1A); // end with CTRL+Z (ASCII 26)
      f.print(content);
      f.close();
      SLog.println("OK");
    }

    else if (cmd.startsWith("LS")) {
      File root = LittleFS.open("/lua");
      File file = root.openNextFile();
      while (file) {
        SLog.println(file.name());
        file = root.openNextFile();
      }
      SLog.println("OK");
    }

    else if (cmd == "REBOOT") {
      SLog.println("REBOOTING...");
      ESP.restart();
    }

    else {
      SLog.println("ERR: Unknown command");
    }
  }
}

// Setup LVGL
void setupLvgl() {

  // [COMMENTED OUT] Single full-frame PSRAM buffer — caused DMA assert failure
  // because PSRAM is not DMA-accessible on ESP32-S3. Replaced with double
  // buffers allocated from internal DMA-capable RAM (Option B).
  //#define LVGL_BUFFER_SIZE (TFT_WIDTH * TFT_HEIGHT * sizeof(lv_color_t))
  //
  //static uint8_t *buf = (uint8_t *)ps_malloc(LVGL_BUFFER_SIZE);
  //if (!buf) {
  //  SLog.println("Memory allocation failed!");
  //  delay(5000);
  //  assert(buf);
  //}

#define BUF_LINES 48
#define BUF_SIZE (TFT_HEIGHT * BUF_LINES * sizeof(lv_color_t))

  static uint8_t *buf1 = (uint8_t *)ps_malloc(BUF_SIZE);
  static uint8_t *buf2 = (uint8_t *)ps_malloc(BUF_SIZE);
  if (!buf1 || !buf2) {
    SLog.println("LVGL buffer allocation failed!");
    delay(5000);
    assert(buf1 && buf2);
  }

  lv_init();

  // Create a default group for focusable objects
  lv_group_t *default_group = lv_group_create();
  lv_group_set_default(default_group);

  // Create a display
  lv_display_t *disp = lv_display_create(TFT_HEIGHT, TFT_WIDTH);

  // Set theme. Emoji font wraps montserrat_14 as fallback so ASCII/Latin still
  // render from the bitmap font; codepoints >= 0x2600 are loaded as PNGs from
  // S:/emoji/<hex>.png on the SD card.
  static lv_font_t * ui_font = emoji_font_create(16, &lv_font_montserrat_14);
  if (!ui_font) ui_font = (lv_font_t *)&lv_font_montserrat_14;

  lv_theme_t *custom_theme = lv_theme_meshpunk_init(
    disp,
    lv_color_make(0x10, 0x10, 0x10),   // Primary color
    lv_color_make(0x30, 0x30, 0x30),   // Secondary color
    true,                              // Dark mode
    ui_font                            // Font (emoji + ASCII fallback)
  );
  
  lv_disp_set_theme(disp, custom_theme);

  // Force the emoji font onto the active screen so all descendants (including
  // luavgl-created labels) inherit it. The theme sets it on a style object,
  // but inheritance can be shadowed by other styles — setting it as a local
  // property on the screen guarantees it's the default for every child.
  lv_obj_set_style_text_font(lv_display_get_screen_active(disp), ui_font, 0);

  lv_display_set_buffers(disp, buf1, buf2, BUF_SIZE,
                         LV_DISPLAY_RENDER_MODE_PARTIAL);

  // Set display properties
  lv_display_set_flush_cb(disp, disp_flush_cb);
  lv_display_set_rotation(disp, LV_DISPLAY_ROTATION_0);

  // Register a touchscreen input device
  lv_indev_t *touch_indev = lv_indev_create();
  lv_indev_set_type(touch_indev, LV_INDEV_TYPE_POINTER);
  lv_indev_set_read_cb(touch_indev, touchpad_read_cb);
  lv_indev_set_display(touch_indev, disp);

  // Register keyboard input device if available
  if (keyboard_available) {
    lv_indev_t *kb_indev = lv_indev_create();
    lv_indev_set_type(kb_indev, LV_INDEV_TYPE_KEYPAD);
    lv_indev_set_read_cb(kb_indev, keyboard_read_cb);

    // Connect keyboard to the default group
    lv_indev_set_group(kb_indev, lv_group_get_default());

    SLog.println("Keyboard input device registered with LVGL");
  }
}

// LVGL UI elements
static lv_obj_t *label;

// Event handler for button
static void btn_event_handler(lv_event_t *e) {
  lv_event_code_t code = lv_event_get_code(e);
  if (code == LV_EVENT_CLICKED) {
    lv_label_set_text(label, "Button was clicked!");
  }
}

// Create a simple UI
void createUI() {
}

// WiFi function for Lua
static int lua_wifi_connect(lua_State *L) {
  const char *network = luaL_checkstring(L, 1);
  const char *pass = luaL_checkstring(L, 2);

  SLog.print("Connecting to WiFi: ");
  SLog.println(network);

  WiFi.begin(network, pass);

  return 0;
}

// WiFi status function for Lua
static int lua_wifi_status(lua_State *L) {
  wl_status_t status = WiFi.status();
  const char *status_str = "unknown";

  switch (status) {
  case WL_CONNECTED:
    status_str = "connected";
    break;
  case WL_IDLE_STATUS:
    status_str = "idle";
    break;
  case WL_DISCONNECTED:
    status_str = "disconnected";
    break;
  case WL_CONNECT_FAILED:
    status_str = "failed";
    break;
  case WL_CONNECTION_LOST:
    status_str = "lost";
    break;
  case WL_NO_SSID_AVAIL:
    status_str = "no_ssid";
    break;
  default:
    status_str = "unknown";
    break;
  }

  lua_pushstring(L, status_str);
  if (status == WL_CONNECTED) {
    lua_pushstring(L, WiFi.localIP().toString().c_str());
    lua_pushstring(L, WiFi.SSID().c_str());
  } else {
    lua_pushstring(L, "");
    lua_pushstring(L, "");
  }

  return 3; // Return status, IP, and SSID
}

// WiFi disconnect function for Lua
static int lua_wifi_disconnect(lua_State *L) {
  WiFi.disconnect();
  return 0;
}

// HTTP fetch function for Lua
static int lua_wifi_fetch(lua_State *L) {
  const char *url = luaL_checkstring(L, 1);
  const char *method = luaL_optstring(L, 2, "GET");

  // Parse headers if provided (table)
  lua_newtable(L); // Create result table

  if (WiFi.status() != WL_CONNECTED) {
    lua_pushboolean(L, 0); // success = false
    lua_setfield(L, -2, "success");

    lua_pushstring(L, "WiFi not connected");
    lua_setfield(L, -2, "error");

    return 1;
  }

  HTTPClient http;
  http.begin(url);

  // Add headers if available (3rd parameter is a table)
  if (!lua_isnoneornil(L, 3) && lua_istable(L, 3)) {
    lua_pushnil(L); // First key
    while (lua_next(L, 3) != 0) {
      // Key at -2, value at -1
      if (lua_isstring(L, -2) && lua_isstring(L, -1)) {
        const char *headerName = lua_tostring(L, -2);
        const char *headerValue = lua_tostring(L, -1);
        http.addHeader(headerName, headerValue);
      }
      lua_pop(L, 1); // Remove value, keep key for next iteration
    }
  }

  int httpCode = 0;
  String payload = "";

  if (strcmp(method, "GET") == 0) {
    httpCode = http.GET();
  } else if (strcmp(method, "POST") == 0) {
    const char *body = luaL_optstring(L, 4, "");
    httpCode = http.POST(body);
  } else if (strcmp(method, "PUT") == 0) {
    const char *body = luaL_optstring(L, 4, "");
    httpCode = http.PUT(body);
  } else if (strcmp(method, "DELETE") == 0) {
    httpCode = http.sendRequest("DELETE");
  } else {
    // Unknown method
    lua_pushboolean(L, 0); // success = false
    lua_setfield(L, -2, "success");

    lua_pushstring(L, "Unsupported HTTP method");
    lua_setfield(L, -2, "error");

    http.end();
    return 1;
  }

  if (httpCode > 0) {
    // HTTP header has been sent and server response header has been handled
    payload = http.getString();

    lua_pushboolean(L, 1); // success = true
    lua_setfield(L, -2, "success");

    lua_pushinteger(L, httpCode);
    lua_setfield(L, -2, "status");

    lua_pushstring(L, payload.c_str());
    lua_setfield(L, -2, "body");
  } else {
    lua_pushboolean(L, 0); // success = false
    lua_setfield(L, -2, "success");

    lua_pushstring(L, http.errorToString(httpCode).c_str());
    lua_setfield(L, -2, "error");
  }

  http.end();
  return 1; // Return the result table
}

// _wifi_download_file(url, filepath) -> {success=bool, error=string|nil, size=int|nil}
// Downloads binary data directly to a file, bypassing Lua strings (which truncate at null bytes).
// filepath uses S:/L: prefix convention (see meshpunk_fs).
//
// The HTTPClient (and its TLS session) persists between calls: consecutive
// downloads from the same host reuse the socket and skip DNS + TCP + TLS
// setup (~1-2s per https request — the bulk of a map tile's total cost).
// Call _wifi_download_end() when a burst finishes to drop the socket and
// free the TLS buffers (~45KB internal RAM).
static HTTPClient *s_dl_http = nullptr;
static uint8_t *s_dl_buf = nullptr;  // PSRAM transfer buffer, allocated once
static const size_t DL_BUF_SIZE = 4096;

static void wifi_dl_client_close() {
  if (!s_dl_http) return;
  s_dl_http->setReuse(false);  // make end() actually drop the socket
  s_dl_http->end();
  delete s_dl_http;
  s_dl_http = nullptr;
}

static bool wifi_dl_client_open(const char *url) {
  if (!s_dl_http) {
    s_dl_http = new HTTPClient();
    s_dl_http->setUserAgent("meshpunk/1.0");
    s_dl_http->setReuse(true);
  }
  return s_dl_http->begin(url);
}

static int lua_wifi_download_file(lua_State *L) {
  const char *url = luaL_checkstring(L, 1);
  const char *filepath = luaL_checkstring(L, 2);

  lua_newtable(L);

  if (WiFi.status() != WL_CONNECTED) {
    wifi_dl_client_close();
    lua_pushboolean(L, 0);
    lua_setfield(L, -2, "success");
    lua_pushstring(L, "WiFi not connected");
    lua_setfield(L, -2, "error");
    return 1;
  }

  if (!s_dl_buf) {
    s_dl_buf = (uint8_t *)heap_caps_malloc(DL_BUF_SIZE,
                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_dl_buf) {
      lua_pushboolean(L, 0);
      lua_setfield(L, -2, "success");
      lua_pushstring(L, "Buffer alloc failed");
      lua_setfield(L, -2, "error");
      return 1;
    }
  }

  if (!wifi_dl_client_open(url)) {
    wifi_dl_client_close();
    lua_pushboolean(L, 0);
    lua_setfield(L, -2, "success");
    lua_pushstring(L, "Bad URL");
    lua_setfield(L, -2, "error");
    return 1;
  }

  int httpCode = s_dl_http->GET();

  // Negative code on a kept-alive client usually means the server closed the
  // idle socket — rebuild the connection and retry once.
  if (httpCode < 0) {
    wifi_dl_client_close();
    if (wifi_dl_client_open(url)) {
      httpCode = s_dl_http->GET();
    }
  }

  if (httpCode != 200) {
    lua_pushboolean(L, 0);
    lua_setfield(L, -2, "success");
    if (httpCode > 0) {
      char msg[48];
      snprintf(msg, sizeof(msg), "HTTP %d", httpCode);
      lua_pushstring(L, msg);
    } else {
      lua_pushstring(L, s_dl_http->errorToString(httpCode).c_str());
    }
    lua_setfield(L, -2, "error");
    s_dl_http->end();
    return 1;
  }

  int len = s_dl_http->getSize();
  WiFiClient *stream = s_dl_http->getStreamPtr();

  MeshpunkFile mf = meshpunk_open(filepath, "w", false);
  if (!mf.valid) {
    lua_pushboolean(L, 0);
    lua_setfield(L, -2, "success");
    lua_pushstring(L, "Failed to open file for writing");
    lua_setfield(L, -2, "error");
    s_dl_http->end();
    return 1;
  }

  // meshpunk_open returns HOLDING the SPI lock for SD paths (meshpunk_close
  // releases it) — fine for quick writes, but holding the shared bus across a
  // whole multi-MB body (e.g. the 3.7MB extended emoji blob) starves the
  // radio and freezes TFT flushes. Yield it between chunks instead — keeping
  // the File open across release/retake is the fs_bridge copy-loop pattern.
  if (mf.is_sd) sd_spi_release();

  int total = 0;
  // Stall detector, not a total-time cap: big files legitimately take longer
  // than any fixed budget, so the deadline resets on every received chunk.
  uint32_t deadline = millis() + 20000;
  while (len > 0 || len == -1) {
    if ((int32_t)(millis() - deadline) >= 0) break;
    int avail = stream->available();
    if (avail <= 0) {
      if (!s_dl_http->connected()) break;
      delay(1);
      continue;
    }
    int toRead = (avail < (int)DL_BUF_SIZE) ? avail : (int)DL_BUF_SIZE;
    int rd = stream->readBytes(s_dl_buf, toRead);
    if (rd <= 0) break;
    if (mf.is_sd) sd_spi_take();
    mf.file.write(s_dl_buf, rd);
    if (mf.is_sd) sd_spi_release();
    total += rd;
    if (len > 0) len -= rd;
    deadline = millis() + 20000;   // progress made — reset the stall clock
  }

  if (mf.is_sd) sd_spi_take();   // rebalance for meshpunk_close's release
  meshpunk_close(mf);

  if (len > 0) {
    // Short read: deadline hit or connection lost mid-body. The file is
    // truncated and the keep-alive framing is unusable — drop the socket and
    // report failure so the caller can retry.
    wifi_dl_client_close();
    lua_pushboolean(L, 0);
    lua_setfield(L, -2, "success");
    lua_pushstring(L, "Truncated download");
    lua_setfield(L, -2, "error");
    return 1;
  }

  if (len == -1) {
    // No Content-Length: body was read until close/stall, so this connection
    // can't be trusted for another request.
    wifi_dl_client_close();
  } else {
    s_dl_http->end();  // keeps the socket alive when the server allows reuse
  }

  lua_pushboolean(L, 1);
  lua_setfield(L, -2, "success");
  lua_pushinteger(L, total);
  lua_setfield(L, -2, "size");
  return 1;
}

// _wifi_download_end() — close the persistent download connection and free
// its TLS buffers. Call when a download burst finishes; no-op when closed.
static int lua_wifi_download_end(lua_State *L) {
  wifi_dl_client_close();
  return 0;
}

static int lua_wifi_scan_start(lua_State *L) {
  if (!wifi_enabled_pref) {
    lua_pushboolean(L, 0);
    return 1;
  }
  WiFi.scanNetworks(true);
  lua_pushboolean(L, 1);
  return 1;
}

static int lua_wifi_scan_results(lua_State *L) {
  int n = WiFi.scanComplete();
  if (n == WIFI_SCAN_RUNNING) {
    lua_pushnil(L);
    return 1;
  }
  lua_newtable(L);
  if (n > 0) {
    for (int i = 0; i < n && i < 16; i++) {
      lua_newtable(L);
      lua_pushstring(L, WiFi.SSID(i).c_str());
      lua_setfield(L, -2, "ssid");
      lua_pushinteger(L, WiFi.RSSI(i));
      lua_setfield(L, -2, "rssi");
      lua_pushboolean(L, WiFi.encryptionType(i) != WIFI_AUTH_OPEN);
      lua_setfield(L, -2, "secure");
      lua_rawseti(L, -2, i + 1);
    }
  }
  WiFi.scanDelete();
  return 1;
}

static int lua_wifi_get_enabled(lua_State *L) {
  lua_pushboolean(L, wifi_enabled_pref);
  return 1;
}

static int lua_wifi_set_enabled(lua_State *L) {
  wifi_enabled_pref = lua_toboolean(L, 1);
  if (wifi_enabled_pref) {
    WiFi.mode(WIFI_STA);
    if (wifi_saved_ssid.length() > 0) {
      WiFi.begin(wifi_saved_ssid.c_str(), wifi_saved_pass.c_str());
    }
  } else {
    WiFi.disconnect();
    WiFi.mode(WIFI_OFF);
  }
  firmware_prefs_save();
  return 0;
}

static int lua_wifi_get_saved_creds(lua_State *L) {
  lua_newtable(L);
  lua_pushstring(L, wifi_saved_ssid.c_str());
  lua_setfield(L, -2, "ssid");
  lua_pushboolean(L, wifi_saved_pass.length() > 0);
  lua_setfield(L, -2, "has_password");
  return 1;
}

static int lua_wifi_save_creds(lua_State *L) {
  const char *ssid = luaL_checkstring(L, 1);
  const char *pass = luaL_optstring(L, 2, "");
  wifi_saved_ssid = ssid;
  wifi_saved_pass = pass;
  wifi_creds_save();
  return 0;
}

static int lua_wifi_clear_creds(lua_State *L) {
  wifi_creds_clear();
  return 0;
}

static int lua_wifi_auto_connect(lua_State *L) {
  if (wifi_enabled_pref && wifi_saved_ssid.length() > 0) {
    WiFi.begin(wifi_saved_ssid.c_str(), wifi_saved_pass.c_str());
    lua_pushboolean(L, 1);
  } else {
    lua_pushboolean(L, 0);
  }
  return 1;
}

// ── Mesh bridge: Lua → C++ ──────────────────────────────────────

// Prepare outgoing message text for the wire: expand composed PUA emoji back
// to their real Unicode sequences (peers must receive standard emoji — the
// PUA form only exists in this device's UI space), then normalize smart
// quotes into the fixed wire buffer. Finally trim any multi-byte codepoint
// split by the byte-wise 160-cap truncation so the wire text stays valid
// UTF-8 (decompose expansion makes hitting the cap likelier).
static void prepare_outgoing_text(const char *raw, char *out, size_t outlen) {
  char *expanded = emoji_decompose(raw);
  normalize_smart_quotes(expanded ? expanded : raw, out, outlen);
  if (expanded) free(expanded);

  size_t w = strlen(out);
  if (w == 0) return;
  size_t lead = w;
  while (lead > 0 && ((unsigned char)out[lead - 1] & 0xC0) == 0x80) lead--;
  if (lead == 0) return;                      // all continuation bytes — leave it
  unsigned char lb = (unsigned char)out[lead - 1];
  size_t need = (lb & 0x80) == 0    ? 1 :
                (lb & 0xE0) == 0xC0 ? 2 :
                (lb & 0xF0) == 0xE0 ? 3 :
                (lb & 0xF8) == 0xF0 ? 4 : 1;
  if (lead - 1 + need > w) out[lead - 1] = '\0';   // drop the partial tail
}

// Send a public/group channel message from Lua
// Usage from Lua: _mesh_send_public("Hello mesh!")
static int lua_mesh_send_public(lua_State *L) {
  const char *raw = luaL_checkstring(L, 1);
  // Normalize smart quotes so both the wire message and the local echo
  // render cleanly on receivers whose base font lacks U+2018-U+201D.
  char text[160];
  prepare_outgoing_text(raw, text, sizeof(text));

  SLog.printf("[MESH TX] lua_mesh_send_public called, text=\"%s\"\n", text);

  MESH_LOCK();
  int pub_idx = the_mesh->publicChannelIdx();   // Public is a normal channel; resolve by name
  if (pub_idx < 0) {
    MESH_UNLOCK();
    SLog.println("[MESH TX] ERROR: No public channel configured!");
    lua_pushboolean(L, 0);
    lua_pushstring(L, "No public channel configured");
    return 2;
  }

  uint32_t timestamp = the_mesh->getRTCClock()->getCurrentTime();
  uint8_t tx_hash[MAX_HASH_SIZE];
  bool ok = the_mesh->sendAndPersistChannelMsg(pub_idx, timestamp, text, strlen(text), tx_hash);
  MESH_UNLOCK();

  lua_pushboolean(L, ok ? 1 : 0);
  if (ok) {
    char hex[MAX_HASH_SIZE * 2 + 1];
    mesh::Utils::toHex(hex, tx_hash, MAX_HASH_SIZE);
    lua_pushstring(L, hex);
  } else {
    lua_pushnil(L);
  }
  return 2;
}

// Send a direct message to a contact by name prefix
// Usage from Lua: _mesh_send_direct("alice", "Hey!")
static int lua_mesh_send_direct(lua_State *L) {
  const char *name_prefix = luaL_checkstring(L, 1);
  const char *raw = luaL_checkstring(L, 2);
  char text[160];
  prepare_outgoing_text(raw, text, sizeof(text));

  MESH_LOCK();
  ContactInfo *recipient = the_mesh->searchContactsByPrefix(name_prefix);
  if (!recipient) {
    MESH_UNLOCK();
    lua_pushboolean(L, 0);
    lua_pushstring(L, "Contact not found");
    return 2;
  }

  uint32_t timestamp = the_mesh->getRTCClock()->getCurrentTime();

  auto r = the_mesh->sendAndPersistDM(*recipient, timestamp, 0, text);
  if (r.code != MSG_SEND_FAILED && r.expected_ack != 0) {
    // Track this send in the retry ladder (3 tries via path, then the path
    // resets and 2 more go flooded). Device-UI sends only — BLE sends run
    // the phone app's own retry logic.
    the_mesh->armPendingSend(*recipient, r.expected_ack, timestamp, text,
                             r.code == MSG_SEND_SENT_DIRECT);
  }
  MESH_UNLOCK();

  if (r.code == MSG_SEND_FAILED) {
    lua_pushboolean(L, 0);
    lua_pushstring(L, "Send failed");
    return 2;
  }

  // Returns: ok, route("flood"/"direct"), expected_ack (uint32, 0 if none),
  // hash (hex string for flood, else nil). The UI uses expected_ack to match
  // the delivery result delivered later via messages.__dispatch_ack().
  bool is_flood = (r.code == MSG_SEND_SENT_FLOOD);
  lua_pushboolean(L, 1);
  lua_pushstring(L, is_flood ? "flood" : "direct");
  lua_pushinteger(L, (lua_Integer)r.expected_ack);
  if (r.has_hash) {
    char hex[MAX_HASH_SIZE * 2 + 1];
    mesh::Utils::toHex(hex, r.tx_hash, MAX_HASH_SIZE);
    lua_pushstring(L, hex);
  } else {
    lua_pushnil(L);
  }
  return 4;
}

// Get this node's info (name, pubkey hex, freq, tx power)
// Usage from Lua: local info = _mesh_get_node_info()
static int lua_mesh_get_node_info(lua_State *L) {
  lua_newtable(L);

  MESH_LOCK();
  lua_pushstring(L, the_mesh->_prefs.node_name);
  lua_setfield(L, -2, "name");

  // Public key as hex string
  char hex[PUB_KEY_SIZE * 2 + 1];
  mesh::Utils::toHex(hex, the_mesh->self_id.pub_key, PUB_KEY_SIZE);
  lua_pushstring(L, hex);
  lua_setfield(L, -2, "pubkey");

  lua_pushnumber(L, the_mesh->_prefs.freq);
  lua_setfield(L, -2, "freq");

  lua_pushinteger(L, the_mesh->_prefs.tx_power_dbm);
  lua_setfield(L, -2, "tx_power");

  lua_pushnumber(L, the_mesh->_prefs.node_lat);
  lua_setfield(L, -2, "lat");

  lua_pushnumber(L, the_mesh->_prefs.node_lon);
  lua_setfield(L, -2, "lon");

  lua_pushnumber(L, the_mesh->_prefs.bandwidth);
  lua_setfield(L, -2, "bandwidth");

  lua_pushinteger(L, the_mesh->_prefs.spreading_factor);
  lua_setfield(L, -2, "spreading_factor");

  lua_pushinteger(L, the_mesh->_prefs.coding_rate);
  lua_setfield(L, -2, "coding_rate");

  lua_pushboolean(L, the_mesh->_prefs.contact_overwrite != 0);
  lua_setfield(L, -2, "contact_overwrite");

  lua_pushboolean(L, the_mesh->_prefs.archive_contacts != 0);
  lua_setfield(L, -2, "archive_contacts");
  MESH_UNLOCK();

  return 1;
}

static int lua_mesh_export_private_key(lua_State *L) {
  MESH_LOCK();
  char hex[PRV_KEY_SIZE * 2 + 1];
  mesh::Utils::toHex(hex, the_mesh->getPrivateKey(), PRV_KEY_SIZE);
  hex[PRV_KEY_SIZE * 2] = '\0';
  MESH_UNLOCK();
  lua_pushstring(L, hex);
  return 1;
}

static int lua_mesh_import_private_key(lua_State *L) {
  const char* hex = luaL_checkstring(L, 1);
  if (strlen(hex) != PRV_KEY_SIZE * 2) {
    lua_pushboolean(L, 0);
    lua_pushstring(L, "key must be 128 hex chars");
    return 2;
  }
  uint8_t prv[PRV_KEY_SIZE];
  if (!mesh::Utils::fromHex(prv, PRV_KEY_SIZE, hex)) {
    lua_pushboolean(L, 0);
    lua_pushstring(L, "invalid hex");
    return 2;
  }
  if (!mesh::LocalIdentity::validatePrivateKey(prv)) {
    lua_pushboolean(L, 0);
    lua_pushstring(L, "key validation failed");
    return 2;
  }
  MESH_LOCK();
  the_mesh->self_id.readFrom(prv, PRV_KEY_SIZE);
  bool ok = the_mesh->saveIdentity();
  MESH_UNLOCK();
  if (ok) {
    delay(100);
    ESP.restart();
  }
  lua_pushboolean(L, 0);
  lua_pushstring(L, "file write failed");
  return 2;
}

static int lua_mesh_generate_identity(lua_State *L) {
  MESH_LOCK();
  ((StdRNG*)the_mesh->getRNG())->begin(esp_random());
  the_mesh->self_id = mesh::LocalIdentity(the_mesh->getRNG());
  int count = 0;
  while (count < 10 && (the_mesh->self_id.pub_key[0] == 0x00 || the_mesh->self_id.pub_key[0] == 0xFF)) {
    the_mesh->self_id = mesh::LocalIdentity(the_mesh->getRNG());
    count++;
  }
  bool ok = the_mesh->saveIdentity();
  MESH_UNLOCK();
  if (ok) {
    delay(100);
    ESP.restart();
  }
  lua_pushboolean(L, 0);
  lua_pushstring(L, "file write failed");
  return 2;
}

// Get contact list
// Push one contact as a Lua table (shared by the live and union caches).
static void push_contact_table(lua_State *L, const ContactInfo &c, bool archived) {
  lua_newtable(L);

  lua_pushstring(L, c.name);
  lua_setfield(L, -2, "name");

  lua_pushinteger(L, c.type);
  lua_setfield(L, -2, "type");

  lua_pushinteger(L, c.out_path_len);
  lua_setfield(L, -2, "path_len");

  lua_pushinteger(L, c.lastmod);   // "last seen" = our RX clock (0 = unheard since boot)
  lua_setfield(L, -2, "last_seen");

  lua_pushinteger(L, c.lastmod);
  lua_setfield(L, -2, "lastmod");

  lua_pushinteger(L, c.last_advert_timestamp);   // sender's advert clock — recorded only
  lua_setfield(L, -2, "sender_advert_ts");

  char hex[PUB_KEY_SIZE * 2 + 1];
  mesh::Utils::toHex(hex, c.id.pub_key, PUB_KEY_SIZE);
  lua_pushstring(L, hex);
  lua_setfield(L, -2, "pubkey");

  lua_pushstring(L, the_mesh->getTypeName(c.type));
  lua_setfield(L, -2, "type_name");

  lua_pushboolean(L, (c.flags & 0x01) != 0);
  lua_setfield(L, -2, "favorite");

  // out_path as array of hex hashes. out_path_len 0xFF is the
  // OUT_PATH_UNKNOWN sentinel (no route learned) — it must NOT be decoded
  // as size/count (it reads as 63 hashes of 4 bytes and used to overflow
  // the hex buffer); unknown routes get an empty path table.
  {
    lua_newtable(L);
    if (c.out_path_len != OUT_PATH_UNKNOWN) {
      uint8_t hash_size = (c.out_path_len >> 6) + 1;
      uint8_t hash_count = c.out_path_len & 63;
      char h[9];  // up to 4-byte hashes (8 hex chars + NUL)
      for (int j = 0; j < hash_count && (j + 1) * hash_size <= MAX_PATH_SIZE; j++) {
        mesh::Utils::toHex(h, &c.out_path[j * hash_size], hash_size);
        lua_pushstring(L, h);
        lua_rawseti(L, -2, j + 1);
      }
    }
    lua_setfield(L, -2, "path");
  }

  lua_pushnumber(L, c.gps_lat / 1000000.0);
  lua_setfield(L, -2, "lat");
  lua_pushnumber(L, c.gps_lon / 1000000.0);
  lua_setfield(L, -2, "lon");

  if (archived) {
    lua_pushboolean(L, 1);
    lua_setfield(L, -2, "archived");
  }
}

// Usage from Lua: local contacts = _mesh_get_contacts([include_archived])
//
// Cached: rebuilding ~500 contact tables (pubkey hex, path arrays, ...)
// costs ~10ms under MESH_LOCK, and the Map app asks on every marker redraw.
// PunkMesh bumps contacts_generation on every mutation (they all funnel
// through saveContacts), so between changes this returns a cheap copy of a
// cached master table. The OUTER array is fresh per call — callers may
// table.sort it in place (Messenger does) — while the per-contact subtables
// are shared with the cache and must be treated as read-only. A 10s TTL
// backstops any mutation path that might miss the generation bump (e.g.
// BLE companion ops run outside MESH_LOCK, so a bump could in theory race).
//
// With include_archived = true the result also contains archived contacts
// (those evicted from the live table or removed; marked archived=true),
// deduped by pubkey with the live entry winning. That variant has its own
// cached master keyed on both generation counters.
static int s_contacts_ref = LUA_NOREF;       // live-only master
static uint32_t s_contacts_gen = 0;
static uint32_t s_contacts_built_ms = 0;
static int s_contacts_count = 0;

static int s_union_ref = LUA_NOREF;          // live + archived master
static uint32_t s_union_gen = 0;
static uint32_t s_union_arch_gen = 0;
static uint32_t s_union_built_ms = 0;
static int s_union_count = 0;

// Copy the live contact table into a transient PSRAM snapshot under MESH_LOCK.
// Returns the buffer (caller frees) or NULL; *out_n = contacts copied. Exists
// so the Lua pushes below run with NO locks held: lua_push* can longjmp on a
// true OOM, and an escape while MESH_LOCK is held would deadlock the mesh task
// permanently — strictly worse than the OOM itself. Bonus: the lock is now held
// only for a memcpy loop, not table pushes + archive-file I/O.
static ContactInfo* snapshot_live_contacts(int* out_n) {
  *out_n = 0;
  MESH_LOCK();
  int n = the_mesh->getNumContacts();
  ContactInfo* live = (ContactInfo*)heap_caps_malloc(
      sizeof(ContactInfo) * (n > 0 ? n : 1), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (live) {
    ContactInfo c;
    int nlive = 0;
    for (int i = 0; i < n; i++) {
      if (the_mesh->getContactByIdx(i, c)) live[nlive++] = c;
    }
    *out_n = nlive;
  }
  MESH_UNLOCK();
  return live;
}

static int lua_mesh_get_contacts(lua_State *L) {
  bool include_archived = lua_toboolean(L, 1);

  int ref;
  int count;

  if (!include_archived) {
    MESH_LOCK();
    uint32_t gen = the_mesh->contacts_generation;
    MESH_UNLOCK();
    bool fresh = (s_contacts_ref != LUA_NOREF) && (gen == s_contacts_gen) &&
                 (millis() - s_contacts_built_ms < 10000);
    if (!fresh) {
      int nlive = 0;
      ContactInfo* live = snapshot_live_contacts(&nlive);
      if (!live) {
        // No snapshot memory: serve the stale cache if one exists, else empty.
        if (s_contacts_ref == LUA_NOREF) {
          lua_newtable(L);
          return 1;
        }
      } else {
        lua_newtable(L);
        for (int i = 0; i < nlive; i++) {
          push_contact_table(L, live[i], false);
          lua_rawseti(L, -2, i + 1);
        }
        heap_caps_free(live);

        if (s_contacts_ref != LUA_NOREF) {
          luaL_unref(L, LUA_REGISTRYINDEX, s_contacts_ref);
        }
        s_contacts_count = nlive;
        s_contacts_ref = luaL_ref(L, LUA_REGISTRYINDEX);  // pops the master
        s_contacts_gen = gen;
        s_contacts_built_ms = millis();
      }
    }
    ref = s_contacts_ref;
    count = s_contacts_count;
  } else {
    MESH_LOCK();
    uint32_t gen = the_mesh->contacts_generation;
    uint32_t agen = the_mesh->archive_generation;
    MESH_UNLOCK();
    bool fresh = (s_union_ref != LUA_NOREF) && (gen == s_union_gen) &&
                 (agen == s_union_arch_gen) &&
                 (millis() - s_union_built_ms < 10000);
    if (!fresh) {
      int nlive = 0;
      ContactInfo* live = snapshot_live_contacts(&nlive);
      if (!live) {
        if (s_union_ref == LUA_NOREF) {
          lua_newtable(L);
          return 1;
        }
      } else {
        // Archived contacts live on disk only. Read a transient, deduped view
        // here (freed immediately after) so the archive costs ZERO steady-state
        // PSRAM — this whole branch only runs when the user has "show archived"
        // on, and is cached for 10s. The on-map display is bounded; the disk
        // archive keeps everything (re-add can still pull back any contact).
        // readArchivedDeduped does its own SPI locking — no MESH_LOCK needed.
        const int ARCH_DISPLAY_MAX = 1000;
        ContactInfo* abuf = (ContactInfo*)heap_caps_malloc(
            sizeof(ContactInfo) * ARCH_DISPLAY_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        int na = 0;
        if (abuf) na = the_mesh->readArchivedDeduped(abuf, ARCH_DISPLAY_MAX);

        lua_newtable(L);
        int idx = 1;
        for (int i = 0; i < nlive; i++) {
          push_contact_table(L, live[i], false);
          lua_rawseti(L, -2, idx++);
        }
        if (abuf) {
          for (int i = 0; i < na; i++) {
            // Live wins by pubkey (checked against the snapshot) — also
            // self-heals entries left behind when a contact re-adverted in.
            bool is_live = false;
            for (int j = 0; j < nlive; j++) {
              if (memcmp(live[j].id.pub_key, abuf[i].id.pub_key, PUB_KEY_SIZE) == 0) {
                is_live = true;
                break;
              }
            }
            if (!is_live) {
              push_contact_table(L, abuf[i], true);
              lua_rawseti(L, -2, idx++);
            }
          }
          heap_caps_free(abuf);
        }
        heap_caps_free(live);

        if (s_union_ref != LUA_NOREF) {
          luaL_unref(L, LUA_REGISTRYINDEX, s_union_ref);
        }
        s_union_count = idx - 1;
        s_union_ref = luaL_ref(L, LUA_REGISTRYINDEX);  // pops the master
        s_union_gen = gen;
        s_union_arch_gen = agen;
        s_union_built_ms = millis();
      }
    }
    ref = s_union_ref;
    count = s_union_count;
  }

  // Hand out a fresh outer array sharing the cached per-contact tables.
  lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
  lua_createtable(L, count, 0);
  for (int i = 1; i <= count; i++) {
    lua_rawgeti(L, -2, i);
    lua_rawseti(L, -2, i);
  }
  lua_remove(L, -2);  // drop the master, leave the copy
  return 1;
}

// _mesh_drop_contacts_cache(): release the cached contact master tables (live-only
// + live+archived). They're pinned in the Lua registry via luaL_ref, so they
// survive app teardown and GC — ~388KB for 500 contacts, parked mid-heap. Only the
// Map and Messenger consume them, so the launcher drops them on app launch to give
// a heavy app (Doom/PICO-8) the contiguous PSRAM back. The next _mesh_get_contacts
// call rebuilds from scratch (the generation/TTL logic is unchanged — clearing the
// refs just forces a fresh build). Lua-state only (Core 0), so no MESH_LOCK needed.
static int lua_mesh_drop_contacts_cache(lua_State *L) {
  if (s_contacts_ref != LUA_NOREF) {
    luaL_unref(L, LUA_REGISTRYINDEX, s_contacts_ref);
    s_contacts_ref = LUA_NOREF;
    s_contacts_count = 0;
    s_contacts_gen = 0;
    s_contacts_built_ms = 0;
  }
  if (s_union_ref != LUA_NOREF) {
    luaL_unref(L, LUA_REGISTRYINDEX, s_union_ref);
    s_union_ref = LUA_NOREF;
    s_union_count = 0;
    s_union_gen = 0;
    s_union_arch_gen = 0;
    s_union_built_ms = 0;
  }
  return 0;
}

// _mesh_archive_read(offset, max) -> contacts_table, next_offset, done
// One batch of archived contacts from the disk log, for the Map's progressive
// "show archived" loader. Stateless (byte-offset based) so the mesh task keeps
// appending between batches. Raw lines (no dedup/live-skip) — caller decides.
static int lua_mesh_archive_read(lua_State *L) {
  uint32_t offset = (uint32_t)luaL_optinteger(L, 1, 0);
  int max_count = (int)luaL_optinteger(L, 2, 150);
  if (max_count < 1) max_count = 1;
  if (max_count > 300) max_count = 300;  // bound the transient buffer

  ContactInfo *buf = (ContactInfo *)heap_caps_malloc(
      sizeof(ContactInfo) * max_count, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!buf) {
    lua_newtable(L);
    lua_pushinteger(L, offset);
    lua_pushboolean(L, true);
    return 3;
  }

  uint32_t next_offset = offset;
  bool done = true;
  // No MESH_LOCK: only touches the archive file (sd_spi serialized inside),
  // not the live contact table.
  int n = the_mesh->readArchiveBatch(offset, max_count, buf, &next_offset, &done);

  lua_newtable(L);
  for (int i = 0; i < n; i++) {
    const ContactInfo &c = buf[i];
    // LEAN entry — only what the Map needs to draw a gray dot and open the
    // re-add popup (name/pubkey/type/last_seen/lat/lon). NO path array / lastmod
    // / favorite, so thousands can be held for a fraction of the PSRAM the full
    // push_contact_table would cost.
    lua_newtable(L);
    lua_pushstring(L, c.name);                          lua_setfield(L, -2, "name");
    char hex[PUB_KEY_SIZE * 2 + 1];
    mesh::Utils::toHex(hex, c.id.pub_key, PUB_KEY_SIZE);
    lua_pushstring(L, hex);                             lua_setfield(L, -2, "pubkey");
    lua_pushstring(L, the_mesh->getTypeName(c.type));   lua_setfield(L, -2, "type_name");
    lua_pushinteger(L, (lua_Integer)c.lastmod); lua_setfield(L, -2, "last_seen");  // our RX clock
    lua_pushnumber(L, c.gps_lat / 1000000.0);           lua_setfield(L, -2, "lat");
    lua_pushnumber(L, c.gps_lon / 1000000.0);           lua_setfield(L, -2, "lon");
    lua_pushboolean(L, 1);                              lua_setfield(L, -2, "archived");
    lua_rawseti(L, -2, i + 1);
  }
  heap_caps_free(buf);

  lua_pushinteger(L, (lua_Integer)next_offset);
  lua_pushboolean(L, done);
  return 3;
}

// _mesh_archive_compact() -> before, after (record counts) | nil, errcode
// Streaming dedup rewrite of the archive log (one record per pubkey, newest
// wins, live contacts dropped) + index rebuild. No MESH_LOCK here —
// compactArchive manages its own bounded lock windows so the radio never
// stalls for the whole rewrite. Nothing crosses into Lua but two integers.
static int lua_mesh_archive_compact(lua_State *L) {
  uint32_t before = 0, after = 0;
  int rc = the_mesh->compactArchive(&before, &after);
  if (rc != 0) {
    lua_pushnil(L);
    lua_pushinteger(L, rc);
    return 2;
  }
  lua_pushinteger(L, (lua_Integer)before);
  lua_pushinteger(L, (lua_Integer)after);
  return 2;
}

// _mesh_archive_count() -> records currently in the log (duplicates included).
// One file stat — no scan, no lock beyond the SD bus.
static int lua_mesh_archive_count(lua_State *L) {
  lua_pushinteger(L, (lua_Integer)the_mesh->archiveRecordCount());
  return 1;
}

// Helper for _mesh_search_contact_names: ASCII-lowercase `name`, and if it
// contains `q` (already lowercased) and isn't a name we've already collected,
// append the ORIGINAL-case name to the result table (at the top of the Lua stack)
// and record its lowercased form in `seen`. Returns the new match count.
static int search_try_add_name(lua_State *L, const char *name, const char *q,
                               char seen[][32], int count, int max) {
  if (count >= max) return count;
  char low[32];
  int ln = 0;
  for (const char *p = name; *p && ln < 31; p++) {
    char ch = *p;
    if (ch >= 'A' && ch <= 'Z') ch += 32;   // ASCII lower (matches Lua :lower())
    low[ln++] = ch;
  }
  low[ln] = '\0';
  if (!strstr(low, q)) return count;                 // no substring match
  for (int j = 0; j < count; j++)
    if (strcmp(seen[j], low) == 0) return count;     // name already collected
  strncpy(seen[count], low, 31);
  seen[count][31] = '\0';
  lua_pushstring(L, name);                            // original-case name
  lua_rawseti(L, -2, count + 1);                      // result[count+1] = name
  return count + 1;
}

// _mesh_search_contact_names(query, include_archived, max) -> { name, ... }
// Case-insensitive (ASCII) substring search over contact names, returning ONLY
// the matching names (deduped by name, <= max). Replaces the Map search's old
// _mesh_get_contacts(true) + Lua filter, which materialized the entire ~1500-
// entry union table (~1MB — the worst single PSRAM fragmenter) just to pull out
// a few names. Live names matched under MESH_LOCK; archived streamed from the
// disk log in batches (no lock — archive file only, sd_spi serialized inside).
static int lua_mesh_search_contact_names(lua_State *L) {
  const char *query = luaL_checkstring(L, 1);
  bool inc_arch = lua_toboolean(L, 2);
  int max = (int)luaL_optinteger(L, 3, 40);
  if (max < 1) max = 1;
  if (max > 64) max = 64;          // bounds the on-stack dedup table

  char q[48];
  int qn = 0;
  for (const char *p = query; *p && qn < (int)sizeof(q) - 1; p++) {
    char ch = *p;
    if (ch >= 'A' && ch <= 'Z') ch += 32;
    q[qn++] = ch;
  }
  q[qn] = '\0';

  lua_newtable(L);                 // result array — stays at the stack top
  if (qn == 0 || !the_mesh) return 1;

  char seen[64][32];               // lowercased collected names (dedup)
  int count = 0;

  // Live contacts.
  MESH_LOCK();
  ContactInfo c;
  int nlive = the_mesh->getNumContacts();
  for (int i = 0; i < nlive && count < max; i++) {
    if (the_mesh->getContactByIdx(i, c)) {
      count = search_try_add_name(L, c.name, q, seen, count, max);
    }
  }
  MESH_UNLOCK();

  // Archived contacts (streamed from disk; raw lines, name-deduped above so a
  // re-archived/duplicate pubkey can't show the same name twice).
  if (inc_arch && count < max) {
    const int BATCH = 48;
    ContactInfo *abuf = (ContactInfo *)heap_caps_malloc(
        sizeof(ContactInfo) * BATCH, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (abuf) {
      uint32_t off = 0;
      bool done = false;
      while (!done && count < max) {
        uint32_t next = off;
        int n = the_mesh->readArchiveBatch(off, BATCH, abuf, &next, &done);
        for (int i = 0; i < n && count < max; i++) {
          count = search_try_add_name(L, abuf[i].name, q, seen, count, max);
        }
        if (n <= 0 || next == off) break;   // no progress -> stop
        off = next;
      }
      heap_caps_free(abuf);
    }
  }
  return 1;
}

// _mesh_readd_contact(pubkey_hex) -> bool
// Move an archived contact back into the live mesh table (route reset to
// flood; it re-establishes on the next path exchange).
static int lua_mesh_readd_contact(lua_State *L) {
  const char *pubkey_hex = luaL_checkstring(L, 1);

  uint8_t pub_key[PUB_KEY_SIZE];
  if (strlen(pubkey_hex) < PUB_KEY_SIZE * 2 ||
      !mesh::Utils::fromHex(pub_key, PUB_KEY_SIZE, pubkey_hex)) {
    lua_pushboolean(L, 0);
    return 1;
  }

  MESH_LOCK();
  bool ok = the_mesh->readdArchivedContact(pub_key);
  MESH_UNLOCK();

  lua_pushboolean(L, ok ? 1 : 0);
  return 1;
}

static int lua_mesh_get_contact_paths(lua_State *L) {
  const char *pubkey_hex = luaL_checkstring(L, 1);

  uint8_t pub_key[PUB_KEY_SIZE];
  mesh::Utils::fromHex(pub_key, PUB_KEY_SIZE, pubkey_hex);

  lua_newtable(L);

  MESH_LOCK();
  // The live contact (if any) — used to flag which record is the CURRENT
  // out_path, so the picker can mark it and offer the others.
  ContactInfo *contact = the_mesh->lookupContactByPubKey(pub_key, PUB_KEY_SIZE);

  ContactPathHistory *h = nullptr;
  for (int i = 0; i < the_mesh->_path_history_count; i++) {
    if (memcmp(the_mesh->_path_history[i].pub_key, pub_key, PUB_KEY_SIZE) == 0) {
      h = &the_mesh->_path_history[i];
      break;
    }
  }
  if (h && h->count > 0) {
    static const char *src_names[] = { "msg", "ack", "path_update", "advert" };
    for (int i = 0; i < h->count; i++) {
      PathRecord &r = h->records[i];
      lua_newtable(L);

      uint8_t hash_size = (r.path_len >> 6) + 1;
      uint8_t hash_count = r.path_len & 63;
      uint16_t byte_len = (uint16_t)hash_count * hash_size;
      if (byte_len > MAX_PATH_SIZE) byte_len = MAX_PATH_SIZE;

      // path as array of hex hashes
      {
        lua_newtable(L);
        char hex[7];
        for (int j = 0; j < hash_count && (j + 1) * hash_size <= MAX_PATH_SIZE; j++) {
          mesh::Utils::toHex(hex, &r.path[j * hash_size], hash_size);
          lua_pushstring(L, hex);
          lua_rawseti(L, -2, j + 1);
        }
        lua_setfield(L, -2, "path");

        lua_pushinteger(L, hash_count);
        lua_setfield(L, -2, "hops");
      }

      // Raw round-trip form for _mesh_set_contact_path ("Use this path").
      lua_pushinteger(L, r.path_len);
      lua_setfield(L, -2, "path_len");
      {
        char phex[MAX_PATH_SIZE * 2 + 1];
        mesh::Utils::toHex(phex, r.path, byte_len);
        lua_pushstring(L, phex);
        lua_setfield(L, -2, "path_hex");
      }

      // Is this record the contact's CURRENT out_path?
      bool is_current = contact &&
                        contact->out_path_len != OUT_PATH_UNKNOWN &&
                        (uint16_t)contact->out_path_len == r.path_len &&
                        memcmp(contact->out_path, r.path, byte_len) == 0;
      lua_pushboolean(L, is_current ? 1 : 0);
      lua_setfield(L, -2, "current");

      lua_pushboolean(L, r.is_direct);
      lua_setfield(L, -2, "direct");

      lua_pushnumber(L, r.snr);
      lua_setfield(L, -2, "snr");

      lua_pushnumber(L, r.rssi);
      lua_setfield(L, -2, "rssi");

      lua_pushinteger(L, r.trip_time_ms);
      lua_setfield(L, -2, "trip_time_ms");

      lua_pushinteger(L, r.success_count);
      lua_setfield(L, -2, "success");

      lua_pushinteger(L, r.failure_count);
      lua_setfield(L, -2, "failure");

      lua_pushinteger(L, r.timestamp);
      lua_setfield(L, -2, "timestamp");

      int src_idx = r.source < 4 ? r.source : 0;
      lua_pushstring(L, src_names[src_idx]);
      lua_setfield(L, -2, "source");

      lua_rawseti(L, -2, i + 1);
    }
  }
  MESH_UNLOCK();

  return 1;
}

// Set a contact's CURRENT out_path from a history record ("Use this path"
// in the Paths picker). Takes the raw round-trip form that
// _mesh_get_contact_paths exposes per record (path_len + path_hex).
// path_len 0 with empty hex = zero-hop direct. Persisted via saveOneContact.
// Usage: local ok, err = _mesh_set_contact_path(pubkey_hex, path_len, path_hex)
static int lua_mesh_set_contact_path(lua_State *L) {
  const char *pubkey_hex = luaL_checkstring(L, 1);
  int path_len = luaL_checkinteger(L, 2);
  const char *path_hex = luaL_optstring(L, 3, "");

  if (strlen(pubkey_hex) != PUB_KEY_SIZE * 2) {
    lua_pushboolean(L, 0);
    lua_pushstring(L, "Bad pubkey");
    return 2;
  }
  if (path_len < 0 || path_len >= OUT_PATH_UNKNOWN) {
    lua_pushboolean(L, 0);
    lua_pushstring(L, "Bad path_len");
    return 2;
  }
  uint16_t byte_len = (uint16_t)(path_len & 63) * ((path_len >> 6) + 1);
  if (byte_len > MAX_PATH_SIZE || strlen(path_hex) != (size_t)byte_len * 2) {
    lua_pushboolean(L, 0);
    lua_pushstring(L, "Bad path");
    return 2;
  }

  uint8_t pub_key[PUB_KEY_SIZE];
  mesh::Utils::fromHex(pub_key, PUB_KEY_SIZE, pubkey_hex);
  uint8_t path_bytes[MAX_PATH_SIZE];
  if (byte_len > 0) mesh::Utils::fromHex(path_bytes, byte_len, path_hex);

  MESH_LOCK();
  ContactInfo *c = the_mesh->lookupContactByPubKey(pub_key, PUB_KEY_SIZE);
  if (!c) {
    MESH_UNLOCK();
    lua_pushboolean(L, 0);
    lua_pushstring(L, "Contact not found");
    return 2;
  }
  memset(c->out_path, 0, sizeof(c->out_path));
  if (byte_len > 0) memcpy(c->out_path, path_bytes, byte_len);
  c->out_path_len = (uint8_t)path_len;
  c->lastmod = the_mesh->getRTCClock()->getCurrentTime();
  the_mesh->saveOneContact(*c);   // persists + bumps contacts_generation
  MESH_UNLOCK();

  lua_pushboolean(L, 1);
  return 1;
}

// Get all observed paths for a message by hash.
// Tries RAM buffer first; falls back to persisted log file.
// Usage: _mesh_get_message_paths(hash_hex)                     -- RAM only
//        _mesh_get_message_paths(hash_hex, channel_idx)        -- RAM → channel file
//        _mesh_get_message_paths(hash_hex, -1, peer_name)      -- RAM → DM file
static int lua_mesh_get_message_paths(lua_State *L) {
  const char *hash_hex = luaL_checkstring(L, 1);
  int channel_idx = luaL_optinteger(L, 2, 0);
  const char *peer = luaL_optstring(L, 3, nullptr);

  if (strlen(hash_hex) != MAX_HASH_SIZE * 2) {
    lua_newtable(L);
    return 1;
  }
  uint8_t hash[MAX_HASH_SIZE];
  mesh::Utils::fromHex(hash, MAX_HASH_SIZE, hash_hex);

  MESH_LOCK();

  // Try RAM buffer first
  MsgPathEntry *e = the_mesh->findMsgPaths(hash);
  if (e && e->path_count > 0) {
    lua_newtable(L);
    for (int i = 0; i < e->path_count; i++) {
      ObservedPath &op = e->paths[i];
      lua_newtable(L);

      uint8_t hash_size = (op.path_len >> 6) + 1;
      uint8_t hop_count = op.path_len & 63;
      lua_newtable(L);
      char hex[7];
      for (int j = 0; j < hop_count && (j + 1) * hash_size <= MAX_PATH_SIZE; j++) {
        mesh::Utils::toHex(hex, &op.path[j * hash_size], hash_size);
        lua_pushstring(L, hex);
        lua_rawseti(L, -2, j + 1);
      }
      lua_setfield(L, -2, "path");

      lua_pushinteger(L, hop_count);
      lua_setfield(L, -2, "hops");

      lua_pushboolean(L, op.is_direct);
      lua_setfield(L, -2, "direct");

      lua_pushnumber(L, op.snr);
      lua_setfield(L, -2, "snr");

      lua_pushnumber(L, op.rssi);
      lua_setfield(L, -2, "rssi");

      lua_rawseti(L, -2, i + 1);
    }
    MESH_UNLOCK();
    return 1;
  }

  // RAM miss — fall back to persisted log file
  int r = the_mesh->lookupPersistedPaths(L, hash_hex, channel_idx, peer);
  MESH_UNLOCK();
  return r;
}

// Send self advertisement
// Usage from Lua: _mesh_send_advert()          -- flood (default)
//                  _mesh_send_advert("zerohop") -- zero-hop only
static int lua_mesh_send_advert(lua_State *L) {
  const char *mode = luaL_optstring(L, 1, "flood");
  MESH_LOCK();
  auto pkt = the_mesh->createSelfAdvert(the_mesh->_prefs.node_name,
                                       the_mesh->_prefs.node_lat,
                                       the_mesh->_prefs.node_lon);
  if (pkt) {
    if (strcmp(mode, "zerohop") == 0) {
      the_mesh->sendZeroHop(pkt, (uint32_t)0);
    } else {
      the_mesh->sendFlood(pkt, (uint32_t)0, the_mesh->pathHashSize());
    }
  }
  MESH_UNLOCK();
  lua_pushboolean(L, pkt ? 1 : 0);
  return 1;
}

// Get number of contacts
static int lua_mesh_get_num_contacts(lua_State *L) {
  MESH_LOCK();
  int n = the_mesh->getNumContacts();
  MESH_UNLOCK();
  lua_pushinteger(L, n);
  return 1;
}

// Set a node config value
// Usage from Lua: _mesh_set_config("name", "MyNode")
//                 _mesh_set_config("freq", "915.525")
//                 _mesh_set_config("tx", "20")
//                 _mesh_set_config("bw", "250")
//                 _mesh_set_config("sf", "10")
//                 _mesh_set_config("cr", "5")
//                 _mesh_set_config("lat", "37.7749")
//                 _mesh_set_config("lon", "-122.4194")
static int lua_mesh_set_config(lua_State *L) {
  const char *key = luaL_checkstring(L, 1);
  const char *value = luaL_checkstring(L, 2);

  MESH_LOCK();
  if (strcmp(key, "name") == 0) {
    strncpy(the_mesh->_prefs.node_name, value, sizeof(the_mesh->_prefs.node_name) - 1);
    the_mesh->_prefs.node_name[sizeof(the_mesh->_prefs.node_name) - 1] = '\0';
    the_mesh->savePrefs();
    SLog.printf("Node name set to: %s\n", the_mesh->_prefs.node_name);
    lua_pushboolean(L, 1);
  } else if (strcmp(key, "freq") == 0) {
    the_mesh->_prefs.freq = atof(value);
    the_mesh->savePrefs();
    SLog.printf("Frequency set to: %.3f (reboot to apply)\n", the_mesh->_prefs.freq);
    lua_pushboolean(L, 1);
  } else if (strcmp(key, "tx") == 0) {
    the_mesh->_prefs.tx_power_dbm = atoi(value);
    the_mesh->savePrefs();
    SLog.printf("TX power set to: %d dBm (reboot to apply)\n", the_mesh->_prefs.tx_power_dbm);
    lua_pushboolean(L, 1);
  } else if (strcmp(key, "lat") == 0) {
    the_mesh->_prefs.node_lat = atof(value);
    the_mesh->savePrefs();
    lua_pushboolean(L, 1);
  } else if (strcmp(key, "lon") == 0) {
    the_mesh->_prefs.node_lon = atof(value);
    the_mesh->savePrefs();
    lua_pushboolean(L, 1);
  } else if (strcmp(key, "bw") == 0) {
    the_mesh->_prefs.bandwidth = atof(value);
    the_mesh->savePrefs();
    SLog.printf("Bandwidth set to: %.1f kHz (reboot to apply)\n", the_mesh->_prefs.bandwidth);
    lua_pushboolean(L, 1);
  } else if (strcmp(key, "sf") == 0) {
    the_mesh->_prefs.spreading_factor = atoi(value);
    the_mesh->savePrefs();
    SLog.printf("Spreading factor set to: %d (reboot to apply)\n", the_mesh->_prefs.spreading_factor);
    lua_pushboolean(L, 1);
  } else if (strcmp(key, "cr") == 0) {
    the_mesh->_prefs.coding_rate = atoi(value);
    the_mesh->savePrefs();
    SLog.printf("Coding rate set to: %d (reboot to apply)\n", the_mesh->_prefs.coding_rate);
    lua_pushboolean(L, 1);
  } else if (strcmp(key, "contact_overwrite") == 0) {
    the_mesh->_prefs.contact_overwrite = (atoi(value) != 0) ? 1 : 0;
    the_mesh->savePrefs();
    SLog.printf("Contact overwrite set to: %s\n", the_mesh->_prefs.contact_overwrite ? "ON" : "OFF");
    lua_pushboolean(L, 1);
  } else if (strcmp(key, "archive_contacts") == 0) {
    the_mesh->_prefs.archive_contacts = (atoi(value) != 0) ? 1 : 0;
    the_mesh->savePrefs();
    SLog.printf("Archive contacts set to: %s\n", the_mesh->_prefs.archive_contacts ? "ON" : "OFF");
    lua_pushboolean(L, 1);
  } else {
    MESH_UNLOCK();
    lua_pushboolean(L, 0);
    lua_pushstring(L, "Unknown config key");
    return 2;
  }
  MESH_UNLOCK();

  return 1;
}

// ── New Mesh bridge functions for full MeshCore integration ──────

// Get all channels
// Usage: local channels = _mesh_get_channels()
// Returns: {{idx=0, name="Public", has_key=true}, ...}
static int lua_mesh_get_channels(lua_State *L) {
  lua_newtable(L);
  int idx = 1;

  MESH_LOCK();
  for (int i = 0; i < MAX_GROUP_CHANNELS; i++) {
    ChannelDetails cd;
    if (the_mesh->getChannel(i, cd)) {
      // Check if channel has a non-empty name
      if (cd.name[0] != '\0') {
        lua_newtable(L);

        lua_pushinteger(L, i);
        lua_setfield(L, -2, "idx");

        lua_pushstring(L, cd.name);
        lua_setfield(L, -2, "name");

        // Check if secret is non-zero
        bool has_key = false;
        for (int j = 0; j < PUB_KEY_SIZE; j++) {
          if (cd.channel.secret[j] != 0) { has_key = true; break; }
        }
        lua_pushboolean(L, has_key ? 1 : 0);
        lua_setfield(L, -2, "has_key");

        lua_rawseti(L, -2, idx++);
      }
    }
  }
  MESH_UNLOCK();

  return 1;
}

// Set a channel by index
// Usage: _mesh_set_channel(1, "MyChannel", "base64psk")
//        _mesh_set_channel(1, "", "")  -- delete channel
static int lua_mesh_set_channel(lua_State *L) {
  int ch_idx = luaL_checkinteger(L, 1);
  const char *name = luaL_checkstring(L, 2);
  const char *psk = luaL_optstring(L, 3, "");

  if (ch_idx < 0 || ch_idx >= MAX_GROUP_CHANNELS) {
    lua_pushboolean(L, 0);
    lua_pushstring(L, "Channel index out of range");
    return 2;
  }

  MESH_LOCK();
  if (strlen(name) == 0) {
    // Delete channel: set empty name and zero secret
    ChannelDetails cd;
    memset(&cd, 0, sizeof(cd));
    the_mesh->setChannel(ch_idx, cd);
    the_mesh->saveChannels();
    MESH_UNLOCK();
    lua_pushboolean(L, 1);
    return 1;
  }

  // Check if it's a hashtag channel (name starts with #)
  if (name[0] == '#') {
    // Hashtag channel: secret = first 16 bytes of sha256(name)
    ChannelDetails cd;
    memset(&cd, 0, sizeof(cd));
    strncpy(cd.name, name, sizeof(cd.name) - 1);
    // Compute sha256 of the channel name to derive key
    uint8_t hash[32];
    mesh::Utils::sha256(hash, 32, (const uint8_t*)name, strlen(name));
    memcpy(cd.channel.secret, hash, 16);
    mesh::Utils::sha256(cd.channel.hash, sizeof(cd.channel.hash), cd.channel.secret, 16);
    the_mesh->setChannel(ch_idx, cd);
    the_mesh->saveChannels();
    MESH_UNLOCK();
    lua_pushboolean(L, 1);
    return 1;
  }

  // Normal channel with PSK
  ChannelDetails *result = the_mesh->addChannel(name, psk);
  if (!result) {
    // addChannel only works for new slots, try setChannel directly
    // Parse the base64 PSK manually
    ChannelDetails cd;
    memset(&cd, 0, sizeof(cd));
    strncpy(cd.name, name, sizeof(cd.name) - 1);
    // Use the existing setChannel which will compute the hash
    // But we need to decode base64 first
    extern unsigned int decode_base64(unsigned char const *src, unsigned int slen, unsigned char *target);
    int len = decode_base64((unsigned char *)psk, strlen(psk), cd.channel.secret);
    if (len != 16 && len != 32) {
      MESH_UNLOCK();
      lua_pushboolean(L, 0);
      lua_pushstring(L, "Invalid PSK length (need 16 or 32 bytes)");
      return 2;
    }
    bool ok = the_mesh->setChannel(ch_idx, cd);
    if (ok) the_mesh->saveChannels();
    MESH_UNLOCK();
    lua_pushboolean(L, ok ? 1 : 0);
    return 1;
  }

  the_mesh->saveChannels();
  MESH_UNLOCK();
  lua_pushboolean(L, 1);
  return 1;
}

// Public chat (slot 0) delete / restore / state. Public is now a normal deletable
// channel; the deletion persists (channels-file marker) so it survives reboot, and
// it can be re-added with its well-known PSK.
static int lua_mesh_delete_public(lua_State *L) {
  MESH_LOCK();
  the_mesh->deletePublic();
  MESH_UNLOCK();
  lua_pushboolean(L, 1);
  return 1;
}
static int lua_mesh_restore_public(lua_State *L) {
  MESH_LOCK();
  the_mesh->restorePublic();
  MESH_UNLOCK();
  lua_pushboolean(L, 1);
  return 1;
}
static int lua_mesh_public_deleted(lua_State *L) {
  MESH_LOCK();
  bool d = the_mesh->isPublicDeleted();
  MESH_UNLOCK();
  lua_pushboolean(L, d ? 1 : 0);
  return 1;
}

// Send a message to a specific channel by index
// Usage: _mesh_send_channel(1, "Hello channel!")
static int lua_mesh_send_channel(lua_State *L) {
  int ch_idx = luaL_checkinteger(L, 1);
  const char *raw = luaL_checkstring(L, 2);
  char text[160];
  prepare_outgoing_text(raw, text, sizeof(text));

  MESH_LOCK();
  uint32_t timestamp = the_mesh->getRTCClock()->getCurrentTime();
  uint8_t tx_hash[MAX_HASH_SIZE];
  bool ok = the_mesh->sendAndPersistChannelMsg(ch_idx, timestamp, text, strlen(text), tx_hash);
  MESH_UNLOCK();

  lua_pushboolean(L, ok ? 1 : 0);
  if (ok) {
    char hex[MAX_HASH_SIZE * 2 + 1];
    mesh::Utils::toHex(hex, tx_hash, MAX_HASH_SIZE);
    lua_pushstring(L, hex);
  } else {
    lua_pushnil(L);
  }
  return 2;
}

// Remove a contact by name prefix
// Usage: _mesh_remove_contact("alice")
static int lua_mesh_remove_contact(lua_State *L) {
  const char *name_prefix = luaL_checkstring(L, 1);

  MESH_LOCK();
  ContactInfo *c = the_mesh->searchContactsByPrefix(name_prefix);
  if (!c) {
    MESH_UNLOCK();
    lua_pushboolean(L, 0);
    lua_pushstring(L, "Contact not found");
    return 2;
  }

  // Preserve in the archive before removal so it can be re-added later
  the_mesh->archiveContact(*c);
  bool ok = the_mesh->removeContact(*c);
  if (ok) the_mesh->saveContacts();
  MESH_UNLOCK();

  lua_pushboolean(L, ok ? 1 : 0);
  return 1;
}

// Clear all contacts
// Usage: _mesh_clear_contacts()
static int lua_mesh_clear_contacts(lua_State *L) {
  MESH_LOCK();
  the_mesh->clearContacts();
  the_mesh->saveContacts();
  MESH_UNLOCK();
  SLog.println("[MESH] All contacts cleared");
  lua_pushboolean(L, 1);
  return 1;
}

// Reset path to a contact (force flood routing next time)
// Usage: _mesh_reset_path("alice")
static int lua_mesh_reset_path(lua_State *L) {
  const char *name_prefix = luaL_checkstring(L, 1);

  MESH_LOCK();
  ContactInfo *c = the_mesh->searchContactsByPrefix(name_prefix);
  if (!c) {
    MESH_UNLOCK();
    lua_pushboolean(L, 0);
    lua_pushstring(L, "Contact not found");
    return 2;
  }

  the_mesh->resetPathTo(*c);
  the_mesh->saveOneContact(*c);
  MESH_UNLOCK();

  lua_pushboolean(L, 1);
  return 1;
}

// Set or clear the favourite flag (bit 0) on a contact
// Usage: _mesh_set_contact_favorite("alice", true)
static int lua_mesh_set_contact_favorite(lua_State *L) {
  const char *name_prefix = luaL_checkstring(L, 1);
  bool fav = lua_toboolean(L, 2);

  MESH_LOCK();
  ContactInfo *c = the_mesh->searchContactsByPrefix(name_prefix);
  if (!c) {
    MESH_UNLOCK();
    lua_pushboolean(L, 0);
    lua_pushstring(L, "Contact not found");
    return 2;
  }

  if (fav) c->flags |= 0x01;
  else     c->flags &= ~0x01;
  the_mesh->saveOneContact(*c);
  MESH_UNLOCK();

  lua_pushboolean(L, 1);
  return 1;
}

// Export a contact as hex biz card string
// Usage: local hex = _mesh_export_contact("alice")
static int lua_mesh_export_contact(lua_State *L) {
  const char *name_prefix = luaL_checkstring(L, 1);

  MESH_LOCK();
  ContactInfo *c = the_mesh->searchContactsByPrefix(name_prefix);
  if (!c) {
    MESH_UNLOCK();
    lua_pushnil(L);
    lua_pushstring(L, "Contact not found");
    return 2;
  }

  uint8_t buf[256];
  uint8_t len = the_mesh->exportContact(*c, buf);
  MESH_UNLOCK();
  if (len == 0) {
    lua_pushnil(L);
    lua_pushstring(L, "No advert data for contact");
    return 2;
  }

  char hex[513];
  mesh::Utils::toHex(hex, buf, len);

  // Return "meshcore://" prefixed hex string
  String card = "meshcore://" + String(hex);
  lua_pushstring(L, card.c_str());
  return 1;
}

// Import a contact from hex biz card string
// Usage: _mesh_import_contact("meshcore://abcdef...")
static int lua_mesh_import_contact(lua_State *L) {
  const char *card = luaL_checkstring(L, 1);

  MESH_LOCK();
  the_mesh->importCard(card);
  MESH_UNLOCK();
  lua_pushboolean(L, 1);
  return 1;
}

// Share a contact via zero-hop broadcast
// Usage: _mesh_share_contact("alice")
static int lua_mesh_share_contact(lua_State *L) {
  const char *name_prefix = luaL_checkstring(L, 1);

  MESH_LOCK();
  ContactInfo *c = the_mesh->searchContactsByPrefix(name_prefix);
  if (!c) {
    MESH_UNLOCK();
    lua_pushboolean(L, 0);
    lua_pushstring(L, "Contact not found");
    return 2;
  }

  bool ok = the_mesh->shareContactZeroHop(*c);
  MESH_UNLOCK();
  lua_pushboolean(L, ok ? 1 : 0);
  return 1;
}

// Login to a room server or repeater (sendLogin handles both types; rooms get
// their sync_since cursor in the request, repeaters just the password).
// Usage: local ok, route, est_timeout = _mesh_login("myroom", "password123")
// The result arrives later via messages.__dispatch_login (LOGIN_RESULT event);
// est_timeout (ms) is how long the UI should wait before declaring no response.
static int lua_mesh_login_room(lua_State *L) {
  const char *name_prefix = luaL_checkstring(L, 1);
  const char *password = luaL_checkstring(L, 2);

  MESH_LOCK();
  ContactInfo *c = the_mesh->searchContactsByPrefix(name_prefix);
  if (!c) {
    MESH_UNLOCK();
    lua_pushboolean(L, 0);
    lua_pushstring(L, "Contact not found");
    return 2;
  }

  uint32_t est_timeout = 0;
  int result = the_mesh->sendLogin(*c, password, est_timeout);
  if (result != MSG_SEND_FAILED) {
    memcpy(&the_mesh->pending_login_prefix, c->id.pub_key, 4);
  }
  MESH_UNLOCK();

  if (result == MSG_SEND_FAILED) {
    lua_pushboolean(L, 0);
    lua_pushstring(L, "Login send failed");
    return 2;
  }

  lua_pushboolean(L, 1);
  lua_pushstring(L, result == MSG_SEND_SENT_DIRECT ? "direct" : "flood");
  lua_pushinteger(L, est_timeout);
  return 3;
}

// Drop the keep-alive connection to a logged-in server (local only — MeshCore
// has no logout packet; the server just stops hearing our keep-alives).
// Usage: _mesh_logout("myroom")
static int lua_mesh_logout(lua_State *L) {
  const char *name_prefix = luaL_checkstring(L, 1);

  MESH_LOCK();
  ContactInfo *c = the_mesh->searchContactsByPrefix(name_prefix);
  if (c) the_mesh->stopConnectionToContact(c->id.pub_key);
  MESH_UNLOCK();

  lua_pushboolean(L, c != nullptr);
  return 1;
}

// True while a keep-alive connection to this server is live (only servers
// that returned a keep-alive interval at login appear here).
// Usage: local up = _mesh_is_connected("myroom")
static int lua_mesh_is_connected(lua_State *L) {
  const char *name_prefix = luaL_checkstring(L, 1);

  MESH_LOCK();
  ContactInfo *c = the_mesh->searchContactsByPrefix(name_prefix);
  bool up = c && the_mesh->hasConnectionToContact(c->id.pub_key);
  MESH_UNLOCK();

  lua_pushboolean(L, up ? 1 : 0);
  return 1;
}

// Send a CLI command to a logged-in repeater (TXT_TYPE_CLI_DATA — no ack on
// the reply). The command is persisted into the repeater's thread first so
// the console history reads like a chat.
// Usage: local ok, route = _mesh_send_command("repeater1", "ver")
static int lua_mesh_send_command(lua_State *L) {
  const char *name_prefix = luaL_checkstring(L, 1);
  const char *raw = luaL_checkstring(L, 2);
  char text[160];
  prepare_outgoing_text(raw, text, sizeof(text));

  MESH_LOCK();
  ContactInfo *c = the_mesh->searchContactsByPrefix(name_prefix);
  if (!c) {
    MESH_UNLOCK();
    lua_pushboolean(L, 0);
    lua_pushstring(L, "Contact not found");
    return 2;
  }

  uint32_t est_timeout = 0;
  uint32_t timestamp = the_mesh->getRTCClock()->getCurrentTime();
  int result = the_mesh->sendCommandData(*c, timestamp, 0, text, est_timeout);
  char hash_hex[MAX_HASH_SIZE * 2 + 1] = {0};
  if (result != MSG_SEND_FAILED) {
    // Both routes set _last_tx_hash (sendFloodScoped / sendDirectScoped), so
    // the console echo gets the repeat-until-heard indicator like DMs do.
    the_mesh->appendDMMessage(c->name, the_mesh->_prefs.node_name, text, timestamp,
                              0, 0, 0, result == MSG_SEND_SENT_DIRECT,
                              0, nullptr, the_mesh->_last_tx_hash);
    the_mesh->preRegisterSentHash(the_mesh->_last_tx_hash, true, -1, c->name);
    mesh::Utils::toHex(hash_hex, the_mesh->_last_tx_hash, MAX_HASH_SIZE);
  }
  MESH_UNLOCK();

  if (result == MSG_SEND_FAILED) {
    lua_pushboolean(L, 0);
    lua_pushstring(L, "Command send failed");
    return 2;
  }

  lua_pushboolean(L, 1);
  lua_pushstring(L, result == MSG_SEND_SENT_DIRECT ? "direct" : "flood");
  lua_pushstring(L, hash_hex);
  return 3;
}

// Send a request to a contact (e.g. get stats from repeater/room)
// Usage: local ok, route = _mesh_send_request("repeater1", 1)  -- 1=GET_STATUS
// A GET_STATUS response comes back decoded via messages.__dispatch_status.
static int lua_mesh_send_request(lua_State *L) {
  const char *name_prefix = luaL_checkstring(L, 1);
  int req_type = luaL_checkinteger(L, 2);

  MESH_LOCK();
  ContactInfo *c = the_mesh->searchContactsByPrefix(name_prefix);
  if (!c) {
    MESH_UNLOCK();
    lua_pushboolean(L, 0);
    lua_pushstring(L, "Contact not found");
    return 2;
  }

  uint32_t tag = 0;
  uint32_t est_timeout = 0;
  int result = the_mesh->sendRequest(*c, (uint8_t)req_type, tag, est_timeout);
  if (result != MSG_SEND_FAILED && req_type == REQ_TYPE_GET_STATUS) {
    memcpy(&the_mesh->pending_status_prefix, c->id.pub_key, 4);
  }
  MESH_UNLOCK();

  if (result == MSG_SEND_FAILED) {
    lua_pushboolean(L, 0);
    lua_pushstring(L, "Request send failed");
    return 2;
  }

  lua_pushboolean(L, 1);
  lua_pushstring(L, result == MSG_SEND_SENT_DIRECT ? "direct" : "flood");
  return 2;
}

// Get last RX radio info (SNR/RSSI from most recent received packet)
// Usage: local info = _mesh_get_rx_info()
static int lua_mesh_get_rx_info(lua_State *L) {
  lua_newtable(L);

  MESH_LOCK();
  float snr  = the_mesh->last_rx_snr;
  float rssi = the_mesh->last_rx_rssi;
  MESH_UNLOCK();

  lua_pushnumber(L, snr);
  lua_setfield(L, -2, "snr");

  lua_pushnumber(L, rssi);
  lua_setfield(L, -2, "rssi");

  return 1;
}

// Usage: local enabled = _mesh_get_rx_boost()
static int lua_mesh_get_rx_boost(lua_State *L) {
  SPI_LOCK();
  bool en = radio_driver.getRxBoostedGainMode();
  SPI_UNLOCK();
  lua_pushboolean(L, en);
  return 1;
}

// Usage: _mesh_set_rx_boost(true)
// Applies the setting to the radio and persists it to LittleFS.
static int lua_mesh_set_rx_boost(lua_State *L) {
  bool en = lua_toboolean(L, 1);
  SPI_LOCK();
  radio_driver.setRxBoostedGainMode(en);
  SPI_UNLOCK();

  the_mesh->_prefs.rx_boost = en ? 1 : 0;
  the_mesh->savePrefs();
  SLog.printf("[RADIO] RX Boost preference saved: %d\n", en ? 1 : 0);

  return 0;
}

// ── Auto-add contact config (matches the BLE companion model) ─────
// _mesh_get_autoadd() → selected_mode, chat, repeater, room, sensor.
//   selected_mode false = "auto-add all"; true = "auto-add selected" (the four
//   type booleans say which types are added). Type bits map to the MeshCore
//   spec: chat 0x02 / repeater 0x04 / room 0x08 / sensor 0x10.
static int lua_mesh_get_autoadd(lua_State *L) {
  MESH_LOCK();
  uint8_t mode = the_mesh->_prefs.manual_add_contacts;
  uint8_t cfg  = the_mesh->_prefs.autoadd_config;
  MESH_UNLOCK();
  lua_pushboolean(L, (mode & 0x01) != 0);
  lua_pushboolean(L, (cfg & 0x02) != 0);
  lua_pushboolean(L, (cfg & 0x04) != 0);
  lua_pushboolean(L, (cfg & 0x08) != 0);
  lua_pushboolean(L, (cfg & 0x10) != 0);
  return 5;
}

// _mesh_set_autoadd(selected_mode, chat, repeater, room, sensor)
static int lua_mesh_set_autoadd(lua_State *L) {
  uint8_t mode = lua_toboolean(L, 1) ? 0x01 : 0x00;
  uint8_t cfg = 0;
  if (lua_toboolean(L, 2)) cfg |= 0x02;
  if (lua_toboolean(L, 3)) cfg |= 0x04;
  if (lua_toboolean(L, 4)) cfg |= 0x08;
  if (lua_toboolean(L, 5)) cfg |= 0x10;
  MESH_LOCK();
  the_mesh->_prefs.manual_add_contacts = mode;
  the_mesh->_prefs.autoadd_config = cfg;
  the_mesh->savePrefs();
  MESH_UNLOCK();
  SLog.printf("[MESH] autoadd mode=%s cfg=0x%02X\n", mode ? "selected" : "all", cfg);
  return 0;
}

// ── Message repeat settings bridge ───────────────────────────────

static int lua_mesh_get_msg_repeat(lua_State *L) {
  lua_newtable(L);
  lua_pushboolean(L, the_mesh->_prefs.msg_repeat_enabled);
  lua_setfield(L, -2, "enabled");
  lua_pushinteger(L, the_mesh->_prefs.msg_repeat_max);
  lua_setfield(L, -2, "max_repeats");
  lua_pushinteger(L, the_mesh->_prefs.msg_repeat_interval_secs);
  lua_setfield(L, -2, "interval");
  return 1;
}

static int lua_mesh_set_msg_repeat(lua_State *L) {
  bool en = lua_toboolean(L, 1);
  int max_rep = luaL_optinteger(L, 2, 3);
  int interval = luaL_optinteger(L, 3, 30);
  if (max_rep < 1) max_rep = 1;
  if (max_rep > 10) max_rep = 10;
  if (interval < 5) interval = 5;
  if (interval > 60) interval = 60;

  the_mesh->_prefs.msg_repeat_enabled = en ? 1 : 0;
  the_mesh->_prefs.msg_repeat_max = (uint8_t)max_rep;
  the_mesh->_prefs.msg_repeat_interval_secs = (uint8_t)interval;
  the_mesh->savePrefs();
  return 0;
}

static int lua_mesh_get_repeat_status(lua_State *L) {
  const char *hex = luaL_checkstring(L, 1);
  uint8_t hash[MAX_HASH_SIZE];
  memset(hash, 0, MAX_HASH_SIZE);
  size_t hlen = strlen(hex);
  for (size_t i = 0; i < hlen / 2 && i < MAX_HASH_SIZE; i++) {
    char hb[3] = { hex[i*2], hex[i*2+1], 0 };
    hash[i] = (uint8_t)strtoul(hb, NULL, 16);
  }
  MESH_LOCK();
  int status = the_mesh->getRepeatStatus(hash);
  MESH_UNLOCK();
  lua_pushinteger(L, status);
  return 1;
}

// ── Persistent message history bridge ────────────────────────────

// Read all stored messages for a channel slot.
// Usage: local msgs = _mesh_get_channel_messages(0)
// Returns array of { from, peer, text, timestamp, hops, snr, rssi, direct, is_dm, channel_idx }
static int lua_mesh_get_channel_messages(lua_State *L) {
  int ch_idx = luaL_checkinteger(L, 1);
  MESH_LOCK();
  int n = the_mesh->pushChannelMessagesToLua(L, ch_idx);
  MESH_UNLOCK();
  return n;
}

// _mesh_routing_query(sender_or_nil, since_ts, until_ts) -> array of
// { from, timestamp, lat, lon, path } from the routing store. sender nil/"" = all.
// No MESH_LOCK: only touches the routing files (sd_spi serialized inside).
static int lua_mesh_routing_query(lua_State *L) {
  const char *sender = lua_isnoneornil(L, 1) ? nullptr : luaL_checkstring(L, 1);
  uint32_t since = (uint32_t)luaL_optinteger(L, 2, 0);
  uint32_t until = (uint32_t)luaL_optinteger(L, 3, 0);
  return the_mesh->pushRoutingQuery(L, sender, since, until);
}

// _mesh_routing_senders(query_or_nil, max) -> array of distinct sender names from
// the routing index matching the (case-insensitive substring) query. Streams the
// .idx files in C — no message bodies loaded into Lua.
static int lua_mesh_routing_senders(lua_State *L) {
  const char *query = lua_isnoneornil(L, 1) ? nullptr : luaL_checkstring(L, 1);
  int max = (int)luaL_optinteger(L, 2, 64);
  return the_mesh->pushRoutingSenders(L, query, max);
}

// Read all stored messages for a DM thread.
// Usage: local msgs = _mesh_get_dm_messages("alice")
static int lua_mesh_get_dm_messages(lua_State *L) {
  const char *peer = luaL_checkstring(L, 1);
  MESH_LOCK();
  int n = the_mesh->pushDMMessagesToLua(L, peer);
  MESH_UNLOCK();
  return n;
}

// Enumerate all DM thread peer names that have stored messages.
// Usage: local names = _mesh_get_dm_threads()
static int lua_mesh_get_dm_threads(lua_State *L) {
  MESH_LOCK();
  int n = the_mesh->pushDMThreadNamesToLua(L);
  MESH_UNLOCK();
  return n;
}

// One summary entry per stored conversation for the Messenger inbox:
// { kind="channel", idx, name, count, last } / { kind="dm", name, count, last }.
// Usage: local sums = _mesh_get_msg_summaries()
// No MESH_LOCK here — pushMsgSummariesToLua takes it internally just for the
// channel-table snapshot and does all file I/O outside it.
static int lua_mesh_get_msg_summaries(lua_State *L) {
  return the_mesh->pushMsgSummariesToLua(L);
}

// ── Unread counters (C-side, survive Lua teardown during ELF runs) ──
// The mesh task bumps these at RX (punkmesh.cpp); Lua only reads/clears.
// messages.lua wraps them so the topbar/Messenger API is unchanged.

// Usage: local n = _mesh_unread_total()
static int lua_mesh_unread_total(lua_State *L) {
  MESH_LOCK();
  uint32_t n = the_mesh->unreadTotal();
  MESH_UNLOCK();
  lua_pushinteger(L, (lua_Integer)n);
  return 1;
}

// Usage: local n = _mesh_unread_channel(idx)
static int lua_mesh_unread_channel(lua_State *L) {
  int idx = luaL_checkinteger(L, 1);
  MESH_LOCK();
  uint16_t n = the_mesh->unreadChannel(idx);
  MESH_UNLOCK();
  lua_pushinteger(L, n);
  return 1;
}

// Usage: local n = _mesh_unread_dm(name)
static int lua_mesh_unread_dm(lua_State *L) {
  const char *name = luaL_checkstring(L, 1);
  MESH_LOCK();
  uint16_t n = the_mesh->unreadDM(name);
  MESH_UNLOCK();
  lua_pushinteger(L, n);
  return 1;
}

// Usage: _mesh_unread_clear_channel(idx)
static int lua_mesh_unread_clear_channel(lua_State *L) {
  int idx = luaL_checkinteger(L, 1);
  MESH_LOCK();
  the_mesh->unreadClearChannel(idx);
  MESH_UNLOCK();
  return 0;
}

// Usage: _mesh_unread_clear_dm(name)
static int lua_mesh_unread_clear_dm(lua_State *L) {
  const char *name = luaL_checkstring(L, 1);
  MESH_LOCK();
  the_mesh->unreadClearDM(name);
  MESH_UNLOCK();
  return 0;
}

// Configure the max records retained per message log file.
// Usage: _mesh_set_max_messages(100)
static int lua_mesh_set_max_messages(lua_State *L) {
  int n = luaL_checkinteger(L, 1);
  MESH_LOCK();
  the_mesh->setMaxMessages(n);
  MESH_UNLOCK();
  lua_pushboolean(L, 1);
  return 1;
}

// ── Storage bridge: Lua → C++ ────────────────────────────────────

// Get storage info for the settings UI
// Returns: { type="SD"|"LittleFS", sd_available=bool, use_sd=bool }
static int lua_storage_get_info(lua_State *L) {
  lua_newtable(L);

  // Current active storage type
  MESH_LOCK();
  bool is_sd = (the_mesh->_storage != &LittleFS);
  MESH_UNLOCK();
  lua_pushstring(L, is_sd ? "SD" : "LittleFS");
  lua_setfield(L, -2, "type");

  // Is SD card physically present?
  lua_pushboolean(L, sd_mounted ? 1 : 0);
  lua_setfield(L, -2, "sd_available");

  // Report actual current state so the toggle matches reality
  lua_pushboolean(L, is_sd ? 1 : 0);
  lua_setfield(L, -2, "use_sd");

  return 1;
}

static int lua_emoji_preload(lua_State *L) {
  uint32_t cp = (uint32_t)luaL_checkinteger(L, 1);
  lua_pushboolean(L, emoji_preload(cp));
  return 1;
}

// _emoji_compose(str) -> str: replace known emoji sequences with their PUA
// codepoints (the form the UI renders as one glyph). Returns the input
// unchanged when nothing matched.
static int lua_emoji_compose(lua_State *L) {
  const char *in = luaL_checkstring(L, 1);
  char *out = emoji_compose(in);
  if (out) { lua_pushstring(L, out); free(out); }
  else     { lua_pushvalue(L, 1); }
  return 1;
}

// _emoji_decompose(str) -> str: expand PUA codepoints back to the real
// Unicode sequences (the wire/disk form). Lua uses this to measure the true
// on-wire byte length of composed text before sending.
static int lua_emoji_decompose(lua_State *L) {
  const char *in = luaL_checkstring(L, 1);
  char *out = emoji_decompose(in);
  if (out) { lua_pushstring(L, out); free(out); }
  else     { lua_pushvalue(L, 1); }
  return 1;
}

// _emoji_blob_count() -> int: glyphs in the emoji blob (0 = blob unavailable).
static int lua_emoji_blob_count(lua_State *L) {
  lua_pushinteger(L, (lua_Integer)emoji_blob_count());
  return 1;
}

// _emoji_font_reload([close_only]) -> int: re-open the blob (SD extended set
// preferred) after a download/removal; returns the new glyph count.
// _emoji_font_reload(true) only RELEASES the blob (returns 0) so the caller
// can remove/rename the file on disk, then calls _emoji_font_reload() again.
static int lua_emoji_font_reload(lua_State *L) {
  bool close_only = lua_toboolean(L, 1);
  lua_pushinteger(L, (lua_Integer)emoji_font_reload(close_only));
  return 1;
}

// _emoji_blob_list(start, count) -> array of codepoints (1-based start into
// the blob's sorted index). For the Settings emoji picker's paged grid.
static int lua_emoji_blob_list(lua_State *L) {
  uint32_t total = emoji_blob_count();
  lua_Integer start = luaL_checkinteger(L, 1);
  lua_Integer count = luaL_checkinteger(L, 2);
  if (start < 1) start = 1;
  if (count < 0) count = 0;
  lua_newtable(L);
  int n = 0;
  for (lua_Integer i = 0; i < count; i++) {
    uint32_t idx = (uint32_t)(start - 1 + i);
    if (idx >= total) break;
    lua_pushinteger(L, (lua_Integer)emoji_blob_cp_at(idx));
    lua_rawseti(L, -2, ++n);
  }
  return 1;
}

// Helper: copy a file from one FS to another
// NOTE: If either srcFS or dstFS is SD, the caller must have already called
static bool copyFile(fs::FS &srcFS, const char* srcPath, fs::FS &dstFS, const char* dstPath) {
  if (!srcFS.exists(srcPath)) return false;
  File src = srcFS.open(srcPath);
  if (!src) return false;

  File dst = dstFS.open(dstPath, "w", true);
  if (!dst) { src.close(); return false; }

  uint8_t buf[256];
  while (src.available()) {
    int n = src.read(buf, sizeof(buf));
    if (n > 0) dst.write(buf, n);
  }
  src.close();
  dst.close();
  return true;
}

#ifdef MESHPUNK_EMBED_PACK
// ==== Self-contained release build ==========================================
// pack/data_pack.bin (built by make_data_pack.py, linked in via
// board_build.embed_files) carries the whole data/ tree. On first boot with an
// empty filesystem the firmware extracts it into LittleFS, so the app binary
// alone is a complete install: no littlefs payload has to survive a Launcher
// or web-flasher install. Pack format: "MPK1" | u32 version | u32 count |
// u32 index_size, then count entries (u16 path_len | u16 flags | u32 raw_size
// | u32 stored_size | u32 offset | path). flags bit0 = raw DEFLATE, inflated
// with the ESP32-S3 ROM's tinfl; the pack is read in place from mapped flash.
#include "esp_flash.h"
#include "esp_partition.h"
#include "rom/miniz.h"
#include "rom/md5_hash.h"

// Symbol names come from objcopy mangling the project-relative source path
// ("pack/data_pack.bin"), directories included - verified with nm on the
// generated .txt.o. If the pack file moves, these must change with it.
extern const uint8_t data_pack_start[] asm("_binary_pack_data_pack_bin_start");
extern const uint8_t data_pack_end[]   asm("_binary_pack_data_pack_bin_end");

static bool pack_inflate_to_file(const uint8_t *src, size_t stored, File &out, uint32_t raw_size) {
  tinfl_decompressor *inf =
      (tinfl_decompressor *)heap_caps_malloc(sizeof(tinfl_decompressor), MALLOC_CAP_SPIRAM);
  uint8_t *dict = (uint8_t *)heap_caps_malloc(TINFL_LZ_DICT_SIZE, MALLOC_CAP_SPIRAM);
  if (!inf || !dict) {
    free(inf);
    free(dict);
    return false;
  }
  tinfl_init(inf);
  size_t in_pos = 0, dict_pos = 0;
  uint32_t written = 0;
  bool ok = true;
  while (true) {
    size_t in_bytes = stored - in_pos;
    size_t out_bytes = TINFL_LZ_DICT_SIZE - dict_pos;
    // Raw deflate, all input present, 32K wrapping output dictionary.
    tinfl_status st = tinfl_decompress(inf, src + in_pos, &in_bytes, dict, dict + dict_pos, &out_bytes, 0);
    in_pos += in_bytes;
    if (out_bytes) {
      if (out.write(dict + dict_pos, out_bytes) != out_bytes) { ok = false; break; }
      written += out_bytes;
      dict_pos = (dict_pos + out_bytes) & (TINFL_LZ_DICT_SIZE - 1);
    }
    if (st == TINFL_STATUS_DONE) break;
    if (st < TINFL_STATUS_DONE) { ok = false; break; }
    if (st == TINFL_STATUS_NEEDS_MORE_INPUT && in_pos >= stored) { ok = false; break; }
  }
  free(inf);
  free(dict);
  return ok && written == raw_size;
}

static void pack_mkdirs(const String &path) {
  for (int i = 1; i < (int)path.length(); i++) {
    if (path[i] == '/') LittleFS.mkdir(path.substring(0, i));
  }
}

static bool pack_write_file(const String &path, const uint8_t *stored, uint32_t stored_size,
                            uint32_t raw_size, uint16_t flags) {
  pack_mkdirs(path);
  File out = LittleFS.open(path, "w", true);
  if (!out) {
    SLog.printf("[PACK] open failed: %s\n", path.c_str());
    return false;
  }
  bool ok = true;
  if (flags & 1) {
    ok = pack_inflate_to_file(stored, stored_size, out, raw_size);
  } else {
    uint32_t w = 0;
    while (w < stored_size) {
      uint32_t n = stored_size - w;
      if (n > 4096) n = 4096;
      if (out.write(stored + w, n) != n) { ok = false; break; }
      w += n;
    }
  }
  out.close();
  if (!ok) {
    SLog.printf("[PACK] write failed: %s\n", path.c_str());
    LittleFS.remove(path);
  }
  return ok;
}

// Extract the embedded pack into LittleFS. The /.pack_version marker (git
// version, injected by make_data_pack.py) is written last and only after a
// clean pass: pack_needs_extract() compares it against the pack's copy, so an
// interrupted extraction retries on the next boot and a firmware update with
// new bundled files re-extracts automatically. Existing files are overwritten;
// runtime-created files (prefs, messages) are not in the pack and survive.
static bool extract_data_pack() {
  const uint8_t *p = data_pack_start;
  const size_t pack_len = (size_t)(data_pack_end - data_pack_start);
  if (pack_len < 16 || memcmp(p, "MPK1", 4) != 0) {
    SLog.println("[PACK] bad magic");
    return false;
  }
  uint32_t version, count, index_size;
  memcpy(&version, p + 4, 4);
  memcpy(&count, p + 8, 4);
  memcpy(&index_size, p + 12, 4);
  if (version != 1 || 16 + (size_t)index_size > pack_len) {
    SLog.println("[PACK] bad header");
    return false;
  }
  SLog.printf("[PACK] extracting %u files to LittleFS...\n", (unsigned)count);
  uint32_t t0 = millis();
  const uint8_t *idx = p + 16;
  const uint8_t *idx_end = idx + index_size;
  uint32_t marker_raw = 0, marker_stored = 0, marker_off = 0;
  uint16_t marker_flags = 0;
  bool have_marker = false, ok = true;
  int done = 0;
  for (uint32_t i = 0; i < count && ok; i++) {
    if (idx + 16 > idx_end) { ok = false; break; }
    uint16_t path_len, flags;
    uint32_t raw_size, stored_size, off;
    memcpy(&path_len, idx, 2);
    memcpy(&flags, idx + 2, 2);
    memcpy(&raw_size, idx + 4, 4);
    memcpy(&stored_size, idx + 8, 4);
    memcpy(&off, idx + 12, 4);
    idx += 16;
    if (idx + path_len > idx_end || (size_t)off + stored_size > pack_len) { ok = false; break; }
    String path;
    path.reserve(path_len);
    for (uint16_t c = 0; c < path_len; c++) path += (char)idx[c];
    idx += path_len;
    if (path == "/.pack_version") {  // marker: written last, see above
      marker_raw = raw_size;
      marker_stored = stored_size;
      marker_off = off;
      marker_flags = flags;
      have_marker = true;
      continue;
    }
    if (!pack_write_file(path, p + off, stored_size, raw_size, flags)) { ok = false; break; }
    done++;
  }
  if (ok && have_marker) {
    ok = pack_write_file("/.pack_version", p + marker_off, marker_stored, marker_raw, marker_flags);
    if (ok) done++;
  }
  SLog.printf("[PACK] %s: %d/%u files in %lus\n", ok ? "done" : "FAILED", done, (unsigned)count,
              (unsigned long)((millis() - t0) / 1000));
  return ok;
}

// True when the pack should be unpacked: fresh/wiped filesystem, an
// interrupted extraction, or a firmware update whose bundled files differ
// (marker mismatch). Cheap when up to date: one index scan + small file read.
static bool pack_needs_extract() {
  const uint8_t *p = data_pack_start;
  const size_t pack_len = (size_t)(data_pack_end - data_pack_start);
  if (pack_len < 16 || memcmp(p, "MPK1", 4) != 0) return false;
  uint32_t count, index_size;
  memcpy(&count, p + 8, 4);
  memcpy(&index_size, p + 12, 4);
  if (16 + (size_t)index_size > pack_len) return false;
  const uint8_t *idx = p + 16;
  const uint8_t *idx_end = idx + index_size;
  for (uint32_t i = 0; i < count; i++) {
    if (idx + 16 > idx_end) return false;
    uint16_t path_len, flags;
    uint32_t stored_size, off;
    memcpy(&path_len, idx, 2);
    memcpy(&flags, idx + 2, 2);
    memcpy(&stored_size, idx + 8, 4);
    memcpy(&off, idx + 12, 4);
    idx += 16;
    if (idx + path_len > idx_end) return false;
    if (path_len == 14 && memcmp(idx, "/.pack_version", 14) == 0) {
      if ((flags & 1) || (size_t)off + stored_size > pack_len) return true;
      File f = LittleFS.open("/.pack_version", "r");
      if (!f) return true;
      bool match = ((uint32_t)f.size() == stored_size);
      uint32_t pos = 0;
      while (match && pos < stored_size) {
        uint8_t buf[64];
        int n = f.read(buf, sizeof(buf));
        if (n <= 0 || memcmp(buf, p + off + pos, n) != 0) { match = false; break; }
        pos += n;
      }
      f.close();
      return !match;
    }
    idx += path_len;
  }
  // Pack has no marker (shouldn't happen): fall back to the coarse check so a
  // populated filesystem doesn't re-extract every boot.
  return !LittleFS.exists("/lua/main.lua");
}

// Create a data partition when the table has none. Launcher 2.7.2 OTA installs
// copy only the app, leaving the device without any spiffs partition; without
// one there is nowhere to extract the pack. This appends a "spiffs" entry into
// the free flash after the last used partition (same 0x8000 table write the
// Launcher itself performs), fixes up the table's MD5 entry, and reboots so the
// bootloader and esp_partition see the new table. Hard guards: only runs when
// NO spiffs/littlefs data partition exists, never moves or resizes existing
// entries, and aborts on anything unexpected.
static void ensure_data_partition() {
  if (esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, NULL))
    return;
  if (esp_partition_find_first(ESP_PARTITION_TYPE_DATA, (esp_partition_subtype_t)0x83, NULL))
    return;  // littlefs subtype used by some tools

  uint8_t table[0xC00];
  if (esp_flash_read(NULL, table, 0x8000, sizeof(table)) != ESP_OK) {
    SLog.println("[PART] table read failed");
    return;
  }
  if (table[0] != 0xAA || table[1] != 0x50) {
    SLog.println("[PART] bad table magic");
    return;
  }
  uint32_t flash_size = 0;
  if (esp_flash_get_size(NULL, &flash_size) != ESP_OK || flash_size < 0x800000) {
    SLog.println("[PART] flash size unavailable");
    return;
  }

  int md5_at = -1;
  int end_at = -1;
  uint32_t max_end = 0x10000;
  for (int n = 0; n < (int)sizeof(table); n += 32) {
    uint8_t *e = table + n;
    if (e[0] == 0xEB && e[1] == 0xEB) { md5_at = n; break; }
    if (e[0] == 0xFF && e[1] == 0xFF) { end_at = n; break; }
    if (e[0] != 0xAA || e[1] != 0x50) {
      SLog.println("[PART] unexpected entry, aborting");
      return;
    }
    uint32_t off, sz;
    memcpy(&off, e + 4, 4);
    memcpy(&sz, e + 8, 4);
    if (off + sz > max_end) max_end = off + sz;
  }
  int insert_at = (md5_at >= 0) ? md5_at : end_at;
  if (insert_at < 0 || insert_at + 64 > (int)sizeof(table)) {
    SLog.println("[PART] no room in table");
    return;
  }

  uint32_t part_off = (max_end + 0xFFFF) & ~0xFFFFu;  // 64 KB align
  if (part_off + 0x400000 > flash_size) {             // need >= 4 MB for the data
    SLog.println("[PART] not enough free flash for data partition");
    return;
  }
  uint32_t part_size = flash_size - part_off;
  if (part_size > 0x600000) part_size = 0x600000;  // match normal builds

  if (md5_at >= 0) memmove(table + insert_at + 32, table + insert_at, 32);
  uint8_t *ne = table + insert_at;
  memset(ne, 0, 32);
  ne[0] = 0xAA;
  ne[1] = 0x50;
  ne[2] = 0x01;  // type: data
  ne[3] = 0x82;  // subtype: spiffs
  memcpy(ne + 4, &part_off, 4);
  memcpy(ne + 8, &part_size, 4);
  memcpy(ne + 12, "spiffs", 6);

  if (md5_at >= 0) {
    int md5_new = insert_at + 32;
    struct MD5Context md5ctx;
    MD5Init(&md5ctx);
    MD5Update(&md5ctx, table, md5_new);
    uint8_t *m = table + md5_new;
    memset(m, 0xFF, 32);
    m[0] = 0xEB;
    m[1] = 0xEB;
    MD5Final(m + 16, &md5ctx);
  }

  SLog.printf("[PART] adding spiffs partition at 0x%06X size 0x%06X, rebooting\n",
              (unsigned)part_off, (unsigned)part_size);
  if (esp_flash_erase_region(NULL, 0x8000, 0x1000) != ESP_OK) {
    SLog.println("[PART] table erase failed");
    return;
  }
  if (esp_flash_write(NULL, table, 0x8000, sizeof(table)) != ESP_OK) {
    SLog.println("[PART] table write failed");
    return;
  }
  delay(100);
  esp_restart();
}
#endif  // MESHPUNK_EMBED_PACK

// Set whether to use SD card for mesh data storage
// Usage: _storage_set_use_sd(true)  -- switch to SD
//        _storage_set_use_sd(false) -- switch to LittleFS
// Migrates existing data to the new location and saves preference
static int lua_storage_set_use_sd(lua_State *L) {
  bool want_sd = lua_toboolean(L, 1);

  SLog.printf("[STORAGE] User requested: use_sd=%s\n", want_sd ? "true" : "false");

  if (want_sd && !sd_mounted) {
    SLog.println("[STORAGE] Cannot use SD — card not mounted");
    lua_pushboolean(L, 0);
    lua_pushstring(L, "SD card not available");
    return 2;
  }

  // Determine source and destination
  MESH_LOCK();
  fs::FS* oldFS = the_mesh->_storage;
  String oldPrefix = the_mesh->_storage_prefix;
  MESH_UNLOCK();

  fs::FS* newFS;
  String newPrefix;

  if (want_sd) {
    newFS = &SD;
    newPrefix = "/meshpunk";

    if (!SD.exists("/meshpunk")) SD.mkdir("/meshpunk");
  } else {
    newFS = &LittleFS;
    newPrefix = "";
  }

  // Migrate data files if switching to a different FS
  if (newFS != oldFS) {
    SLog.println("[STORAGE] Migrating mesh data...");
    // Migration may touch SD (either source or destination) plus LittleFS;
    // holding the SPI mutex across the whole loop is simpler and safe.
    sd_spi_take();
    const char* files[] = { "/identity", "/node_prefs", "/contacts" };
    for (int i = 0; i < 3; i++) {
      String srcPath = oldPrefix + files[i];
      String dstPath = newPrefix + files[i];
      if (oldFS->exists(srcPath.c_str())) {
        bool ok = copyFile(*oldFS, srcPath.c_str(), *newFS, dstPath.c_str());
        SLog.printf("[STORAGE]   %s -> %s: %s\n", srcPath.c_str(), dstPath.c_str(), ok ? "OK" : "FAILED");
      }
    }
    sd_spi_release();
  }

  // Switch active storage
  MESH_LOCK();
  the_mesh->setStorage(newFS, newPrefix.c_str());
  MESH_UNLOCK();

  use_sd_pref = want_sd;
  firmware_prefs_save();

  lua_pushboolean(L, 1);
  return 1;
}

// ── Filesystem bridge: Lua → C++ ─────────────────────────────────

// Helper: extract just the last component from a path
// e.g. "/lua/apps/calculator" -> "calculator", "calculator" -> "calculator"
static const char* pathBasename(const char* path) {
  const char* last = strrchr(path, '/');
  return last ? last + 1 : path;
}

// List subdirectory names in a LittleFS directory
// Usage: local dirs = _list_dir("/lua/apps")
// Returns: {"calculator", "messenger", ...} (directories only, names only)
static int lua_list_dir(lua_State *L) {
  const char *path = luaL_checkstring(L, 1);

  lua_newtable(L);
  int idx = 1;

  File root = LittleFS.open(path);
  if (!root || !root.isDirectory()) {
    SLog.printf("[FS] _list_dir: cannot open %s\n", path);
    return 1; // return empty table
  }

  File entry = root.openNextFile();
  while (entry) {
    if (entry.isDirectory()) {
      const char *name = pathBasename(entry.name());
      if (name[0] != '\0') {
        lua_pushstring(L, name);
        lua_rawseti(L, -2, idx++);
      }
    }
    entry = root.openNextFile();
  }

  SLog.printf("[FS] _list_dir(%s): found %d dirs\n", path, idx - 1);
  return 1;
}

// List subdirectory names on SD card
// Usage: local dirs = _list_dir_sd("/meshpunk/apps")
static int lua_list_dir_sd(lua_State *L) {
  const char *path = luaL_checkstring(L, 1);

  lua_newtable(L);
  int idx = 1;

  if (!sd_mounted) {
    SLog.println("[FS] _list_dir_sd: SD not mounted");
    return 1; // return empty table
  }

  MESH_LOCK();
  sd_spi_take();
  File root = SD.open(path);

  if (!root || !root.isDirectory()) {
    SLog.printf("[FS] _list_dir_sd: cannot open %s\n", path);
    sd_spi_release();
    MESH_UNLOCK();
    return 1;
  }

  File entry = root.openNextFile();
  int iter = 0;
  while (entry) {
    if (entry.isDirectory()) {
      const char *name = pathBasename(entry.name());
      if (name[0] != '\0') {
        lua_pushstring(L, name);
        lua_rawseti(L, -2, idx++);
      }
    }
    sd_spi_release();
    vTaskDelay(1);
    sd_spi_take();
    entry = root.openNextFile();
    iter++;
  }
  root.close();

  sd_spi_release();
  MESH_UNLOCK();

  SLog.printf("[FS] _list_dir_sd(%s): found %d dirs, scanned %d entries\n", path, idx - 1, iter);
  return 1;
}

// List ALL entries (files and directories) in a LittleFS directory
// Usage: local entries = _list_all("/lua/apps")
// Returns: {{name="calculator", type="dir", size=0}, {name="main.lua", type="file", size=1234}, ...}
static int lua_list_all(lua_State *L) {
  const char *path = luaL_checkstring(L, 1);

  lua_newtable(L);
  int idx = 1;

  File root = LittleFS.open(path);
  if (!root || !root.isDirectory()) {
    SLog.printf("[FS] _list_all: cannot open %s\n", path);
    return 1; // return empty table
  }

  File entry = root.openNextFile();
  while (entry) {
    lua_newtable(L);

    const char *name = pathBasename(entry.name());
    if (name[0] != '\0') {
      lua_pushstring(L, name);
      lua_setfield(L, -2, "name");

      lua_pushstring(L, entry.isDirectory() ? "dir" : "file");
      lua_setfield(L, -2, "type");

      lua_pushinteger(L, entry.size());
      lua_setfield(L, -2, "size");

      lua_rawseti(L, -2, idx++);
    } else {
      lua_pop(L, 1); // pop empty entry table
    }

    entry = root.openNextFile();
  }

  SLog.printf("[FS] _list_all(%s): found %d entries\n", path, idx - 1);
  return 1;
}

// List ALL entries (files and directories) on SD card
// Usage: local entries = _list_all_sd("/meshpunk/apps")
static int lua_list_all_sd(lua_State *L) {
  const char *path = luaL_checkstring(L, 1);

  lua_newtable(L);
  int idx = 1;

  if (!sd_mounted) {
    SLog.println("[FS] _list_all_sd: SD not mounted");
    return 1;
  }

  MESH_LOCK();
  sd_spi_take();
  File root = SD.open(path);
  if (!root || !root.isDirectory()) {
    SLog.printf("[FS] _list_all_sd: cannot open %s\n", path);
    sd_spi_release();
    MESH_UNLOCK();
    return 1;
  }

  File entry = root.openNextFile();
  int iter = 0;
  while (entry) {
    lua_newtable(L);

    const char *name = pathBasename(entry.name());
    if (name[0] != '\0') {
      lua_pushstring(L, name);
      lua_setfield(L, -2, "name");

      lua_pushstring(L, entry.isDirectory() ? "dir" : "file");
      lua_setfield(L, -2, "type");

      lua_pushinteger(L, entry.size());
      lua_setfield(L, -2, "size");

      lua_rawseti(L, -2, idx++);
    } else {
      lua_pop(L, 1);
    }

    entry = root.openNextFile();
    if (++iter % 20 == 0) {
      sd_spi_release();
      vTaskDelay(1);
      sd_spi_take();
    }
  }
  root.close();

  sd_spi_release();
  MESH_UNLOCK();

  SLog.printf("[FS] _list_all_sd(%s): found %d entries\n", path, idx - 1);
  return 1;
}

// Check if a file exists on SD card
// _mkdir_sd(path) — create a directory on SD (no-op if it already exists)
static int lua_mkdir_sd(lua_State *L) {
  const char *path = luaL_checkstring(L, 1);
  if (!sd_mounted) {
    lua_pushboolean(L, 0);
    return 1;
  }
  sd_spi_take();
  bool ok = SD.exists(path) || SD.mkdir(path);
  sd_spi_release();
  lua_pushboolean(L, ok ? 1 : 0);
  return 1;
}

// Usage: local exists = _file_exists_sd("/meshpunk/apps/myapp/main.lua")
static int lua_file_exists_sd(lua_State *L) {
  const char *path = luaL_checkstring(L, 1);
  if (!sd_mounted) {
    lua_pushboolean(L, 0);
    return 1;
  }

  sd_spi_take();
  bool exists = SD.exists(path);
  sd_spi_release();

  lua_pushboolean(L, exists ? 1 : 0);
  return 1;
}

// Persistent RGB565 conversion buffer — allocated once, reused across calls.
// Eliminates hundreds of 128KB alloc/free cycles during bulk tile downloads
// that fragment PSRAM and eventually cause decode failures.
static uint16_t *s_rgb565_buf = nullptr;
static const uint32_t RGB565_BUF_SIZE = 256 * 256 * 2;  // 131072 bytes

// Serializes conversions: the Core-1 fetch worker and the legacy _png_to_bin
// Lua binding (Core 0) share s_rgb565_buf. Created from the Lua thread before
// the worker can exist, so creation never races.
static SemaphoreHandle_t s_convert_mutex = nullptr;
static void ensure_convert_mutex() {
  if (!s_convert_mutex) s_convert_mutex = xSemaphoreCreateMutex();
}
struct ConvertLock {
  explicit ConvertLock(SemaphoreHandle_t m) : m_(m) {
    if (m_) xSemaphoreTake(m_, portMAX_DELAY);
  }
  ~ConvertLock() {
    if (m_) xSemaphoreGive(m_);
  }
  SemaphoreHandle_t m_;
};

// Core PNG -> .bin conversion: decode a PNG from memory, pack native
// little-endian RGB565, write dst_path atomically. Shared by _png_to_bin
// (file source, LVGL thread) and the Core-1 tile fetch worker (network
// source). Does NOT free png_data — the caller owns it.
// Returns nullptr on success, else a stage string:
//   "frag"   PSRAM too fragmented to decode. The caller may drop the LVGL
//            image cache *on the LVGL thread* and retry once.
//   "oom" | "decode" | "sd"
// allow_lvgl_cache_drop: pass true only on the LVGL thread —
// lv_image_cache_drop() is not thread-safe and must never run on Core 1.
static const char *png_buf_to_bin(const uint8_t *png_data, uint32_t png_size,
                                  const char *dst_path,
                                  bool allow_lvgl_cache_drop) {
  ConvertLock lock(s_convert_mutex);

  SLog.printf("[png2bin] dst=%s psram_free=%u largest=%u\n",
                dst_path,
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));

  // Allocate persistent RGB565 buffer on first call
  if (!s_rgb565_buf) {
    s_rgb565_buf = (uint16_t *)heap_caps_malloc(RGB565_BUF_SIZE,
                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_rgb565_buf) {
      SLog.println("[png2bin] FAIL: initial rgb565 buffer alloc");
      return "oom";
    }
  }

  // Bail early if PSRAM is too fragmented for lodepng decode.
  // Decode needs ~256KB contiguous for ARGB8888 + ~192KB for scanlines.
  size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
  if (largest < 512 * 1024) {
    SLog.printf("[png2bin] SKIP: PSRAM fragmented (largest=%u)\n", (unsigned)largest);
    return "frag";
  }

  // IMPORTANT: this is LVGL's *patched* lodepng. lodepng_decode32() does NOT
  // return a raw pixel buffer like upstream — it returns an lv_draw_buf_t*
  // (ARGB8888). The pixels live in decoded->data, and the whole thing must be
  // released with lv_draw_buf_destroy() (struct + data are separate allocs).
  // Treating it as a raw buffer leaks the ~256KB data block every call.
  lv_draw_buf_t *decoded = nullptr;
  unsigned w = 0, h = 0;
  unsigned err = lodepng_decode32((unsigned char **)&decoded, &w, &h,
                                  png_data, png_size);

  // err 83 = lodepng alloc failure. The decode briefly needs a ~256KB (ARGB8888)
  // plus ~192KB (scanline) contiguous block. If the RGB565 tile cache has
  // fragmented PSRAM, that can fail despite ample total free memory. Last-resort:
  // drop the whole image cache to coalesce free space and retry once (dropped
  // tiles transparently re-load from their .bin). The Map app also proactively
  // evicts off-screen / old-zoom tiles, so this should rarely fire.
  if (err == 83 && !decoded) {
    if (allow_lvgl_cache_drop) {
      SLog.printf("[png2bin] err=83 frag fallback: drop cache (largest=%u)\n",
                    (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
      lv_image_cache_drop(NULL);
      err = lodepng_decode32((unsigned char **)&decoded, &w, &h,
                             png_data, png_size);
    } else {
      // Can't touch the LVGL cache from this thread — report frag so the
      // caller drops it on the LVGL thread and retries the tile.
      SLog.printf("[png2bin] err=83 on worker (largest=%u)\n",
                    (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
      return "frag";
    }
  }

  if (err || !decoded) {
    SLog.printf("[png2bin] FAIL: lodepng err=%u decoded=%p psram_free=%u\n",
                  err, (void *)decoded,
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    if (decoded) lv_draw_buf_destroy(decoded);
    return "decode";
  }
  SLog.printf("[png2bin] decoded %ux%u\n", w, h);

  // decoded->data is ARGB8888 in byte order R,G,B,A (lodepng_convert /
  // rgba8ToPixel), stride = 4*w (contiguous). Pack to native little-endian
  // RGB565: LVGL v9 byte-swaps the whole framebuffer at flush for
  // LV_COLOR_16_SWAP, so image data itself stays little-endian (matches
  // the reference scripts/LVGLImage.py output).
  const uint8_t *argb = (const uint8_t *)decoded->data;
  if (!argb) {
    SLog.println("[png2bin] FAIL: decoded->data is NULL");
    lv_draw_buf_destroy(decoded);
    return "decode";
  }

  uint32_t pixel_count = (uint32_t)w * h;
  uint16_t stride = (uint16_t)(w * 2);
  uint32_t data_size = (uint32_t)stride * h;

  if (data_size > RGB565_BUF_SIZE) {
    SLog.printf("[png2bin] FAIL: tile %ux%u exceeds buffer\n", w, h);
    lv_draw_buf_destroy(decoded);
    return "decode";
  }

  uint16_t *rgb565 = s_rgb565_buf;  // reuse persistent buffer

  for (uint32_t i = 0; i < pixel_count; i++) {
    uint8_t r = argb[i * 4 + 0];
    uint8_t g = argb[i * 4 + 1];
    uint8_t b = argb[i * 4 + 2];
    rgb565[i] = ((uint16_t)(r >> 3) << 11) |
                ((uint16_t)(g >> 2) << 5)  |
                (uint16_t)(b >> 3);
  }
  lv_draw_buf_destroy(decoded);

  lv_image_header_t hdr;
  lv_memzero(&hdr, sizeof(hdr));
  hdr.magic  = LV_IMAGE_HEADER_MAGIC;
  hdr.cf     = LV_COLOR_FORMAT_RGB565;
  hdr.w      = w;
  hdr.h      = h;
  hdr.stride = stride;

  // Atomic write: write to .tmp first, then rename to final path.
  // If the device resets mid-write, only the .tmp exists and tile_cached
  // (which checks for .bin) never sees it — no corrupt tiles.
  char tmp_path[256];
  snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", dst_path);

  const char *tmp_sd = (strncmp(tmp_path, "S:", 2) == 0) ? tmp_path + 2 : nullptr;
  const char *dst_sd = (strncmp(dst_path, "S:", 2) == 0) ? dst_path + 2 : nullptr;

  if (tmp_sd && dst_sd) {
    // SD target: write in bounded chunks, releasing the shared SPI mutex
    // between them, so the 131KB write never blocks the display flush or the
    // radio for more than one chunk (~25ms at 40MHz).
    sd_spi_take();
    File f = SD.open(tmp_sd, FILE_WRITE);
    sd_spi_release();
    if (!f) {
      SLog.printf("[png2bin] FAIL: open tmp %s\n", tmp_path);
      return "sd";
    }

    sd_spi_take();
    bool wok = f.write((const uint8_t *)&hdr, sizeof(hdr)) == sizeof(hdr);
    sd_spi_release();

    const uint8_t *src = (const uint8_t *)rgb565;
    const uint32_t CHUNK = 32 * 1024;
    for (uint32_t off = 0; wok && off < data_size; off += CHUNK) {
      uint32_t n = (data_size - off < CHUNK) ? (data_size - off) : CHUNK;
      sd_spi_take();
      wok = f.write(src + off, n) == n;
      sd_spi_release();
    }

    sd_spi_take();
    f.close();
    if (!wok) SD.remove(tmp_sd);
    bool renamed = wok && SD.rename(tmp_sd, dst_sd);
    sd_spi_release();

    if (!wok) {
      SLog.printf("[png2bin] FAIL: short write %s\n", tmp_path);
      return "sd";
    }
    if (!renamed) {
      SLog.printf("[png2bin] FAIL: rename %s -> %s\n", tmp_sd, dst_sd);
      return "sd";
    }
  } else {
    // LittleFS target (L:) — internal flash, no SPI-bus contention.
    MeshpunkFile mf = meshpunk_open(tmp_path, "w", false);
    if (!mf.valid) {
      SLog.printf("[png2bin] FAIL: open tmp %s\n", tmp_path);
      return "sd";
    }
    size_t hw = mf.file.write((const uint8_t *)&hdr, sizeof(hdr));
    size_t dw = mf.file.write((const uint8_t *)rgb565, data_size);
    meshpunk_close(mf);

    const char *tmp_l = (tmp_path[1] == ':') ? tmp_path + 2 : tmp_path;
    const char *dst_l = (dst_path[1] == ':') ? dst_path + 2 : dst_path;
    if (hw != sizeof(hdr) || dw != data_size) {
      SLog.printf("[png2bin] FAIL: short write hdr=%u/%u data=%u/%u\n",
                    (unsigned)hw, (unsigned)sizeof(hdr), (unsigned)dw, (unsigned)data_size);
      LittleFS.remove(tmp_l);
      return "sd";
    }
    if (!LittleFS.rename(tmp_l, dst_l)) {
      SLog.printf("[png2bin] FAIL: rename %s -> %s\n", tmp_l, dst_l);
      return "sd";
    }
  }

  return nullptr;
}

// _png_to_bin(src_path, dst_path) -> bool
// Decodes a PNG file and writes an LVGL RGB565 .bin file.
// Paths use S:/L: prefix convention (meshpunk_fs).
// On success the source PNG is deleted — the .bin fully replaces it.
static int lua_png_to_bin(lua_State *L) {
  const char *src_path = luaL_checkstring(L, 1);
  const char *dst_path = luaL_checkstring(L, 2);

  ensure_convert_mutex();

  uint32_t png_size = 0;
  void *png_data = meshpunk_read_all(src_path, &png_size, false);
  if (!png_data) {
    SLog.println("[png2bin] FAIL: meshpunk_read_all returned NULL");
    lua_pushboolean(L, 0);
    return 1;
  }

  // Runs on the LVGL thread, so the cache-drop fallback is allowed.
  bool ok = png_buf_to_bin((const uint8_t *)png_data, png_size, dst_path,
                           true) == nullptr;
  heap_caps_free(png_data);

  if (ok) {
    // Conversion landed — the source PNG is dead weight now, so consume it.
    if ((src_path[0] == 'S' || src_path[0] == 's') && src_path[1] == ':') {
      sd_spi_take();
      SD.remove(src_path + 2);
      sd_spi_release();
    } else if ((src_path[0] == 'L' || src_path[0] == 'l') && src_path[1] == ':') {
      LittleFS.remove(src_path + 2);
    }
  }

  lua_pushboolean(L, ok ? 1 : 0);
  return 1;
}

// ---------------------------------------------------------------------------
// Core-1 tile fetch worker
// ---------------------------------------------------------------------------
// The whole download→decode→convert→write pipeline runs on a Core-1 task so
// the LVGL/Lua thread never blocks on the network. Lua submits requests with
// _tile_fetch_start() and collects results with _tile_fetch_poll().

// Persistent download buffer (PSRAM, allocated once, Core-1 only). OSM
// raster tiles run 10-40KB typical, ~100KB worst-case urban — 256KB clears
// any real tile and is small next to the 8MB pool.
static uint8_t *s_tile_png_buf = nullptr;
static const uint32_t TILE_PNG_BUF_SIZE = 256 * 1024;

// Worker-owned keep-alive client (never share an HTTPClient across cores —
// _wifi_download_file's client stays on Core 0). Closed after 10s of queue
// idle to free the TLS buffers (~45KB internal RAM).
static HTTPClient *s_tile_http = nullptr;
static volatile bool s_tile_http_close_req = false;  // UI asked to drop TLS now (Map close)

static void tile_http_close() {
  if (!s_tile_http) return;
  s_tile_http->setReuse(false);
  s_tile_http->end();
  delete s_tile_http;
  s_tile_http = nullptr;
}

static bool tile_http_open(const char *url) {
  if (!s_tile_http) {
    s_tile_http = new HTTPClient();
    s_tile_http->setUserAgent("meshpunk/1.0");
    s_tile_http->setReuse(true);
  }
  return s_tile_http->begin(url);
}

// Runs on the worker task. Returns nullptr on success or a stage string:
// "wifi" | "http" | "truncated" (network — count toward connection-loss
// detection) or "size" | "oom" | "frag" | "decode" | "sd" (local).
static const char *do_tile_fetch(const char *url, const char *bin_path) {
  if (WiFi.status() != WL_CONNECTED) {
    tile_http_close();
    return "wifi";
  }

  if (!s_tile_png_buf) {
    s_tile_png_buf = (uint8_t *)heap_caps_malloc(TILE_PNG_BUF_SIZE,
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_tile_png_buf) return "oom";
  }

  if (!tile_http_open(url)) {
    tile_http_close();
    return "http";
  }

  int httpCode = s_tile_http->GET();

  // Negative code on a kept-alive client usually means the server closed the
  // idle socket — rebuild the connection and retry once.
  if (httpCode < 0) {
    tile_http_close();
    if (tile_http_open(url)) {
      httpCode = s_tile_http->GET();
    }
  }

  if (httpCode != 200) {
    s_tile_http->end();
    return "http";
  }

  int len = s_tile_http->getSize();
  if (len > (int)TILE_PNG_BUF_SIZE) {
    tile_http_close();  // body left unread — framing is unusable
    return "size";
  }

  WiFiClient *stream = s_tile_http->getStreamPtr();
  uint32_t total = 0;
  uint32_t deadline = millis() + 20000;  // hard stop for stalled transfers
  while (len > 0 || len == -1) {
    if ((int32_t)(millis() - deadline) >= 0) break;
    int avail = stream->available();
    if (avail <= 0) {
      if (!s_tile_http->connected()) break;
      vTaskDelay(pdMS_TO_TICKS(1));
      continue;
    }
    uint32_t space = TILE_PNG_BUF_SIZE - total;
    if (space == 0) break;  // length-less response outgrew the buffer
    int toRead = (avail < (int)space) ? avail : (int)space;
    int rd = stream->readBytes(s_tile_png_buf + total, toRead);
    if (rd <= 0) break;
    total += rd;
    if (len > 0) len -= rd;
  }

  if (len > 0) {
    // Short read: deadline hit or connection lost mid-body — the keep-alive
    // framing is unusable, drop the socket.
    tile_http_close();
    return "truncated";
  }

  if (len == -1) {
    // No Content-Length: body was read until close/stall, so this connection
    // can't be trusted for another request.
    tile_http_close();
  } else {
    s_tile_http->end();  // keeps the socket alive when the server allows reuse
  }

  if (total == 0) return "http";

  // Never allow the LVGL cache drop from this thread; on "frag" the Lua side
  // drops the cache on the LVGL thread and retries.
  return png_buf_to_bin(s_tile_png_buf, total, bin_path, false);
}

struct TileFetchReq {
  char url[128];
  char bin_path[112];
  char key[32];
};

struct TileFetchRes {
  char key[32];
  bool ok;
  char stage[12];
};

static QueueHandle_t s_tile_req_q = nullptr;
static QueueHandle_t s_tile_res_q = nullptr;
static TaskHandle_t s_tile_task_handle = nullptr;

static void tile_fetch_task(void *param) {
  SLog.printf("[TASK] tile_fetch starting on core=%d\n", xPortGetCoreID());
  for (;;) {
    TileFetchReq req;
    bool got = (xQueueReceive(s_tile_req_q, &req, pdMS_TO_TICKS(10000)) == pdTRUE);

    // Teardown requested by the UI (Map close): close the keep-alive TLS session
    // AND free THIS task's 16KB INTERNAL stack — the dominant reason a heavy app
    // (Doom) launched right after the Map can't get its task stack. That 16KB is
    // carved from the largest internal block, dropping it from ~48KB to ~32KB,
    // one notch under Doom's need. _tile_fetch_start recreates the worker on the
    // next fetch; the shared request queue preserves any pending work for it.
    // Signalled via a flag + an empty-url sentinel that just wakes an idle worker.
    if (s_tile_http_close_req) {
      s_tile_http_close_req = false;
      tile_http_close();
      s_tile_task_handle = nullptr;
      vTaskDelete(NULL);   // frees our stack; never returns
    }

    if (!got) {
      // 10s with no work — drop the keep-alive socket (frees the TLS buffers).
      // The decode arena stays put: it's one contiguous 1MB block, so it doesn't
      // itself fragment anything, and it's freed on teardown below. Loop back.
      tile_http_close();
      continue;
    }
    if (req.url[0] == '\0') continue;   // bare wake sentinel — no work to do

    TileFetchRes res;
    memset(&res, 0, sizeof(res));
    strlcpy(res.key, req.key, sizeof(res.key));
    const char *stage = do_tile_fetch(req.url, req.bin_path);
    res.ok = (stage == nullptr);
    if (stage) strlcpy(res.stage, stage, sizeof(res.stage));

    // Result queue (8) is deeper than request queue (4) + 1 in flight, and
    // the Map app polls every tick — this never blocks in practice.
    xQueueSend(s_tile_res_q, &res, portMAX_DELAY);
  }
}

// _tile_fetch_start(url, bin_path, key) -> bool
// Queue a tile for the Core-1 fetch worker. Returns false when the worker
// queue is full — keep the item and retry on a later tick. Results are
// collected with _tile_fetch_poll(), matched by key.
static int lua_tile_fetch_start(lua_State *L) {
  const char *url = luaL_checkstring(L, 1);
  const char *bin_path = luaL_checkstring(L, 2);
  const char *key = luaL_checkstring(L, 3);

  // Lua thread only — safe to create everything lazily here, and the convert
  // mutex must exist before the worker can race the legacy _png_to_bin.
  ensure_convert_mutex();
  if (!s_tile_req_q) {
    s_tile_req_q = xQueueCreate(4, sizeof(TileFetchReq));
    s_tile_res_q = xQueueCreate(8, sizeof(TileFetchRes));
  }
  if (!s_tile_task_handle) {
    // Priority 1: below mesh_task (2) so radio servicing always preempts
    // TLS/decode work; same tier as gps_task. 16KB stack — the TLS
    // handshake is the deep part.
    xTaskCreatePinnedToCore(tile_fetch_task, "tile_fetch", 16 * 1024,
                            nullptr, 1, &s_tile_task_handle, 1);
  }

  TileFetchReq req;
  memset(&req, 0, sizeof(req));
  if (strlen(url) >= sizeof(req.url) || strlen(bin_path) >= sizeof(req.bin_path)
      || strlen(key) >= sizeof(req.key)) {
    lua_pushboolean(L, 0);
    return 1;
  }
  strlcpy(req.url, url, sizeof(req.url));
  strlcpy(req.bin_path, bin_path, sizeof(req.bin_path));
  strlcpy(req.key, key, sizeof(req.key));

  lua_pushboolean(L, xQueueSend(s_tile_req_q, &req, 0) == pdTRUE ? 1 : 0);
  return 1;
}

// _tile_fetch_close(): tear the Core-1 tile worker all the way down — close its
// keep-alive HTTPS/TLS client AND let the task delete itself, freeing its 16KB
// INTERNAL stack (the block that otherwise leaves <32KB contiguous internal, so
// an ELF module like Doom can't create its task). Both the client and the task
// are worker-owned — never touch them from Core 0 — so we set a flag and post an
// empty-url sentinel to wake an idle worker; it runs the teardown on its own
// thread and is recreated by _tile_fetch_start on the next fetch. No-op before
// the worker exists. Called by the Map on shutdown so the next heavy app fits.
static int lua_tile_fetch_close(lua_State *L) {
  s_tile_http_close_req = true;
  if (s_tile_req_q) {
    TileFetchReq req;
    memset(&req, 0, sizeof(req));  // url[0] == '\0' = wake/close sentinel
    xQueueSend(s_tile_req_q, &req, 0);
  }
  return 0;
}

// _tile_fetch_poll() -> key, ok, stage | nil
// Pop one completed fetch; call in a loop until nil. stage is "" on success.
static int lua_tile_fetch_poll(lua_State *L) {
  if (!s_tile_res_q) {
    lua_pushnil(L);
    return 1;
  }
  TileFetchRes res;
  if (xQueueReceive(s_tile_res_q, &res, 0) != pdTRUE) {
    lua_pushnil(L);
    return 1;
  }
  lua_pushstring(L, res.key);
  lua_pushboolean(L, res.ok ? 1 : 0);
  lua_pushstring(L, res.stage);
  return 3;
}

// _lvgl_image_cache_drop([src]) -> nil
// Evict decoded image(s) from LVGL's image cache to free PSRAM. With no/nil
// argument, drops the entire cache. With a path string (e.g. an "S:/...bin"
// tile), drops just that image (matched by string). Used by the Map app to
// release off-screen and stale-zoom tiles so decoded tiles don't accumulate
// and fragment PSRAM.
static int lua_lvgl_image_cache_drop(lua_State *L) {
  if (lua_isnoneornil(L, 1)) {
    lv_image_cache_drop(NULL);
  } else {
    const char *src = luaL_checkstring(L, 1);
    lv_image_cache_drop(src);
  }
  return 0;
}

// ── Map tile pool (contiguous, fixed) ───────────────────────────────────────
// The map shows a 4x4 = 16 grid. Instead of 16 LVGL Image widgets each loading a
// .bin FILE (which LVGL decodes into a SCATTERED 128KB image-cache buffer per
// tile — the prime PSRAM fragmenter), we keep ONE contiguous 16x128KB block and
// display each tile via an in-memory RGB565 lv_image_dsc that LVGL draws DIRECTLY
// (use_directly path, lv_bin_decoder.c — no copy, no cache buffer). The 16 grid
// widgets map 1:1 to slots (slot = grid index). Allocated when the map opens,
// freed on close (lua_tile_pool_free) once the widgets are hidden.
#define TILE_POOL_SLOTS      16
#define TILE_POOL_SLOT_BYTES (256 * 256 * 2)   // 131072 (RGB565)
static uint8_t *s_tile_pool = nullptr;
static lv_image_dsc_t s_tile_dsc[TILE_POOL_SLOTS];

// _tile_pool_alloc() -> bool. One contiguous 2MB block; cache-drop + retry once
// on failure (the canvas-alloc pattern) so it lands even on a tightish heap.
static int lua_tile_pool_alloc(lua_State *L) {
  if (!s_tile_pool) {
    size_t need = (size_t)TILE_POOL_SLOTS * TILE_POOL_SLOT_BYTES;
    SLog.printf("[tile_pool] pre-alloc: psram free=%uKB largest=%uKB (need %uKB)\n",
                (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024),
                (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) / 1024),
                (unsigned)(need / 1024));
    s_tile_pool = (uint8_t *)heap_caps_malloc(need, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_tile_pool) {
      lv_image_cache_drop(NULL);
      s_tile_pool = (uint8_t *)heap_caps_malloc(need, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (!s_tile_pool) { SLog.println("[tile_pool] alloc FAILED"); lua_pushboolean(L, 0); return 1; }
    lv_memzero(s_tile_dsc, sizeof(s_tile_dsc));
    SLog.printf("[tile_pool] allocated %uKB; psram now free=%uKB largest=%uKB\n",
                (unsigned)(need / 1024),
                (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024),
                (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) / 1024));
  }
  lua_pushboolean(L, 1);
  return 1;
}

// _tile_pool_free(). Caller MUST hide/clear the tile widgets first so nothing
// draws from pool memory after it's freed. Drops the image cache (in case a
// descriptor was cached pointing at pool data) then frees the block.
static int lua_tile_pool_free(lua_State *L) {
  if (s_tile_pool) {
    lv_image_cache_drop(NULL);
    heap_caps_free(s_tile_pool);
    s_tile_pool = nullptr;
    lv_memzero(s_tile_dsc, sizeof(s_tile_dsc));
  }
  return 0;
}

// _tile_show(widget, slot1based, sd_bin_path) -> bool. Reads the .bin (header +
// RGB565 data) into the slot's buffer and points the widget's image src at the
// in-memory descriptor for that slot. Chunked SD read releasing the SPI bus
// between chunks (like png2bin's write) so it never stalls the flush/radio.
static int lua_tile_show(lua_State *L) {
  luavgl_obj_t *lobj = (luavgl_obj_t *)lua_touserdata(L, 1);
  if (!lobj || !lobj->obj) { lua_pushboolean(L, 0); return 1; }
  lv_obj_t *img = lobj->obj;
  int slot = (int)luaL_checkinteger(L, 2) - 1;     // 1-based Lua -> 0-based
  const char *path = luaL_checkstring(L, 3);
  if (!s_tile_pool || slot < 0 || slot >= TILE_POOL_SLOTS) { lua_pushboolean(L, 0); return 1; }

  uint8_t *dst = s_tile_pool + (size_t)slot * TILE_POOL_SLOT_BYTES;
  lv_image_header_t hdr;

  sd_spi_take();
  File f = SD.open(path, "r");
  bool ok = f && (f.read((uint8_t *)&hdr, sizeof(hdr)) == (int)sizeof(hdr));
  sd_spi_release();
  if (!f) { lua_pushboolean(L, 0); return 1; }

  if (ok && (hdr.magic != LV_IMAGE_HEADER_MAGIC || hdr.cf != LV_COLOR_FORMAT_RGB565)) ok = false;
  uint32_t data_size = ok ? (uint32_t)hdr.stride * hdr.h : 0;
  if (data_size == 0 || data_size > TILE_POOL_SLOT_BYTES) ok = false;

  uint32_t off = 0;
  while (ok && off < data_size) {
    uint32_t n = (data_size - off < 32768u) ? (data_size - off) : 32768u;
    sd_spi_take();
    int rd = f.read(dst + off, n);
    sd_spi_release();
    if (rd != (int)n) ok = false;
    off += n;
  }
  sd_spi_take(); f.close(); sd_spi_release();
  if (!ok) { lua_pushboolean(L, 0); return 1; }

  lv_image_dsc_t *d = &s_tile_dsc[slot];
  d->header    = hdr;
  d->data      = dst;
  d->data_size = data_size;

  // Re-point the widget at this slot's (just-updated) descriptor and force a
  // redraw. set_src to the same pointer is a no-op, so clear first; the
  // descriptor is used directly (no decode copy), so this is cheap.
  lv_image_set_src(img, NULL);
  lv_image_set_src(img, d);
  lv_obj_invalidate(img);
  lua_pushboolean(L, 1);
  return 1;
}

// Streaming chunk reader for SD sources: same pattern as lua_file_chunk_reader
// (top of this file), but takes the SPI bus only INSIDE each read — the
// parser's own Lua allocations between chunks never hold the bus, and the
// mesh task gets radio time throughout a large compile.
struct LuaSDChunkReader {
  File file;
  char buf[1024];
};

static const char *lua_sd_chunk_reader(lua_State *L, void *ud, size_t *size) {
  (void)L;
  LuaSDChunkReader *st = (LuaSDChunkReader *)ud;
  sd_spi_take();
  size_t n = st->file.read((uint8_t *)st->buf, sizeof(st->buf));
  sd_spi_release();
  if (n == 0) {
    *size = 0;
    return NULL;
  }
  *size = n;
  return st->buf;
}

// Load and execute a Lua file from the SD card
// Usage: _dofile_sd("/meshpunk/apps/myapp/main.lua")
// This is needed because dofile/loadfile only read from LittleFS.
// Streams the source to lua_load() in 1KB blocks — the old whole-file malloc
// held the entire source contiguously (the same slurp pattern the require()
// searcher dropped in the "lua large file bug" fix, d20dcb3).
static int lua_dofile_sd(lua_State *L) {
  const char *path = luaL_checkstring(L, 1);

  if (!sd_mounted) {
    lua_pushnil(L);
    lua_pushstring(L, "SD card not mounted");
    return 2;
  }

  LuaSDChunkReader rdr;
  sd_spi_take();
  rdr.file = SD.open(path);
  if (!rdr.file || rdr.file.isDirectory()) {
    if (rdr.file) rdr.file.close();
    sd_spi_release();
    lua_pushnil(L);
    lua_pushfstring(L, "Cannot open SD file: %s", path);
    return 2;
  }
  size_t size = rdr.file.size();
  sd_spi_release();

  SLog.printf("[FS] _dofile_sd: loading %s (%d bytes)\n", path, (int)size);

  // Count extra args (everything after the path on the stack)
  int nargs = lua_gettop(L) - 1;

  // lua_load returns a status instead of raising, so the file always closes.
  int status = lua_load(L, lua_sd_chunk_reader, &rdr, path, NULL);
  sd_spi_take();
  rdr.file.close();
  sd_spi_release();

  if (status != LUA_OK) {
    SLog.printf("[FS] _dofile_sd: load error: %s\n", lua_tostring(L, -1));
    return lua_error(L);
  }

  // Stack: [path, arg1, arg2, ..., chunk]
  // Move chunk to position 2 (after path), then remove path
  lua_insert(L, 2);
  lua_remove(L, 1);
  // Stack: [chunk, arg1, arg2, ...]

  if (lua_pcall(L, nargs, LUA_MULTRET, 0) != LUA_OK) {
    SLog.printf("[FS] _dofile_sd: exec error: %s\n", lua_tostring(L, -1));
    return lua_error(L);
  }

  return lua_gettop(L); // return whatever the script returned
}

// ── Lua PSRAM arena (ELF-launch fragmentation fix) ──────────────────────────
// Lua lives in its OWN multi_heap arena (not the shared PSRAM heap), placed above a
// deliberate free GAP. The gap is a sacrificial low region: ESP-IDF's TLSF heap
// serves a small request from the smallest-size-class free block (the gap), so the
// mesh / tile-decode / ESP-IDF churn we can't redirect carves from the gap instead
// of fragmenting the big block a heavy ELF needs. The arena is freed wholesale at
// ELF launch (luaTearDown), coalescing up into one large clean block for the module.
// lua_psram_alloc runs only on Core 0 (Lua is single-threaded) -> no lock needed.
#define LUA_GAP_BYTES      (1024u * 1024u)        // sacrificial: mesh + tile-decode + emoji churn
#define MAIN_HEAP_RESERVE  (3584u * 1024u)        // kept free for the Map (tile pool + canvases) + slack
#define LUA_ARENA_MIN      (1536u * 1024u)        // floor if PSRAM is tight (overflow spills to the gap)
#define LUA_ARENA_MAX      (3u * 1024u * 1024u)   // ceiling: Lua won't need more; don't starve the Map
static multi_heap_handle_t s_lua_heap = NULL;
static uintptr_t s_lua_arena_base = 0;
static size_t    s_lua_arena_size = 0;

// Lua allocations that missed the arena and fell back to the shared PSRAM heap
// (cumulative since boot). A growing number means Lua's live set exceeds the
// arena — exactly the fragmentation the arena exists to prevent — so it's
// surfaced in the mesh task's periodic [HEAP] line rather than failing silently.
// Written on Core 0 (Lua alloc), read on Core 1 (log); aligned 32-bit, no lock.
volatile uint32_t g_lua_arena_spill_count = 0;

static inline bool lua_in_arena(void *p) {
  return s_lua_arena_base && (uintptr_t)p >= s_lua_arena_base &&
         (uintptr_t)p < s_lua_arena_base + s_lua_arena_size;
}

// Create the arena above a free gap (spacer trick — heap_caps_malloc can't take an
// address). Sized dynamically: grab as much as Lua can use, leaving MAIN_HEAP_RESERVE
// free for the Map's big buffers. Call at boot + each bring-up, BEFORE lua_newstate.
static void lua_arena_create() {
  if (s_lua_heap) return;
  void *spacer = heap_caps_malloc(LUA_GAP_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  size_t avail = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);  // block ABOVE the spacer
  size_t want  = (avail > MAIN_HEAP_RESERVE + LUA_ARENA_MIN) ? (avail - MAIN_HEAP_RESERVE)
                                                             : LUA_ARENA_MIN;
  if (want > LUA_ARENA_MAX) want = LUA_ARENA_MAX;
  void *blk = heap_caps_malloc(want, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (spacer) heap_caps_free(spacer);   // leave the GAP free below the arena
  if (!blk) {
    SLog.printf("[lua_arena] create FAILED (want %uKB, avail %uKB) -> Lua uses main heap\n",
                (unsigned)(want / 1024), (unsigned)(avail / 1024));
    return;
  }
  s_lua_heap = multi_heap_register(blk, want);
  if (!s_lua_heap) { heap_caps_free(blk); return; }
  s_lua_arena_base = (uintptr_t)blk;
  s_lua_arena_size = want;
  SLog.printf("[lua_arena] %uKB @0x%08X (gap %uKB reserve %uKB avail %uKB); psram largest now %uKB\n",
              (unsigned)(want / 1024), (unsigned)(uintptr_t)blk,
              (unsigned)(LUA_GAP_BYTES / 1024), (unsigned)(MAIN_HEAP_RESERVE / 1024),
              (unsigned)(avail / 1024),
              (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) / 1024));
}

// Free the whole arena back to the main heap. Call AFTER lua_close (so every arena
// object is already freed); the block coalesces up into the big region for the ELF.
static void lua_arena_destroy() {
  if (!s_lua_heap) return;
  void *blk = (void *)s_lua_arena_base;
  s_lua_heap = NULL;
  s_lua_arena_base = 0;
  s_lua_arena_size = 0;
  heap_caps_free(blk);
  SLog.printf("[lua_arena] freed; psram largest now %uKB\n",
              (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) / 1024));
}

// PSRAM allocator for Lua. Routes through the arena (multi_heap) when it exists,
// falling back to the shared PSRAM heap (small Lua objects -> TLSF puts them in the
// gap). osize is Lua's valid old size (only when ptr != NULL) for cross-heap memcpy.
static void *lua_psram_alloc(void *ud, void *ptr, size_t osize, size_t nsize) {
    (void)ud;
    if (nsize == 0) {                       // free
        if (ptr) {
            if (lua_in_arena(ptr)) multi_heap_free(s_lua_heap, ptr);
            else                   heap_caps_free(ptr);
        }
        return NULL;
    }
    if (s_lua_heap) {
        bool was_in = lua_in_arena(ptr);    // ptr==NULL -> false
        void *p = multi_heap_realloc(s_lua_heap, was_in ? ptr : NULL, nsize);
        if (p) {
            if (ptr && !was_in) {           // pulled a prior fallback obj into the arena
                memcpy(p, ptr, osize < nsize ? osize : nsize);
                heap_caps_free(ptr);
            }
            return p;
        }
        // Arena full -> fall back to the shared heap (small objs land in the gap).
        g_lua_arena_spill_count++;
        void *q = heap_caps_realloc(was_in ? NULL : ptr, nsize, MALLOC_CAP_SPIRAM);
        if (q && was_in) {                  // moved an arena obj out -> copy + free old
            memcpy(q, ptr, osize < nsize ? osize : nsize);
            multi_heap_free(s_lua_heap, ptr);
        }
        return q;
    }
    // No arena (pre-create / create failed): plain shared-heap realloc.
    return heap_caps_realloc(ptr, nsize, MALLOC_CAP_SPIRAM);
}


// Initialize LuaVGL
void setupLuaVGL() {
  // Create Lua state with PSRAM allocator
  L = lua_newstate(lua_psram_alloc, NULL);
  if (!L) {
    SLog.println("Failed to create Lua state");
    return;
  }

  // Set Lua runtime on PunkMesh
  the_mesh->lua_runtime = L;

  // Open standard Lua libraries
  luaL_openlibs(L);

  // Initialize LuaVGL
  luaL_requiref(L, "lvgl", luaopen_lvgl, 1);
  lua_pop(L, 1);

  // Register WiFi functions
  lua_register(L, "_wifi_connect", lua_wifi_connect);
  lua_register(L, "_wifi_status", lua_wifi_status);
  lua_register(L, "_wifi_disconnect", lua_wifi_disconnect);
  lua_register(L, "_wifi_fetch", lua_wifi_fetch);
  lua_register(L, "_wifi_download_file", lua_wifi_download_file);
  lua_register(L, "_wifi_download_end", lua_wifi_download_end);
  lua_register(L, "_wifi_scan_start", lua_wifi_scan_start);
  lua_register(L, "_wifi_scan_results", lua_wifi_scan_results);
  lua_register(L, "_wifi_get_enabled", lua_wifi_get_enabled);
  lua_register(L, "_wifi_set_enabled", lua_wifi_set_enabled);
  lua_register(L, "_wifi_get_saved_creds", lua_wifi_get_saved_creds);
  lua_register(L, "_wifi_save_creds", lua_wifi_save_creds);
  lua_register(L, "_wifi_clear_creds", lua_wifi_clear_creds);
  lua_register(L, "_wifi_auto_connect", lua_wifi_auto_connect);

  // Register Mesh bridge functions
  lua_register(L, "_mesh_send_public", lua_mesh_send_public);
  lua_register(L, "_mesh_send_direct", lua_mesh_send_direct);
  lua_register(L, "_mesh_get_node_info", lua_mesh_get_node_info);
  lua_register(L, "_mesh_get_contacts", lua_mesh_get_contacts);
  lua_register(L, "_mesh_drop_contacts_cache", lua_mesh_drop_contacts_cache);
  lua_register(L, "_mesh_search_contact_names", lua_mesh_search_contact_names);
  lua_register(L, "_mesh_send_advert", lua_mesh_send_advert);
  lua_register(L, "_mesh_get_num_contacts", lua_mesh_get_num_contacts);
  lua_register(L, "_mesh_set_config", lua_mesh_set_config);

  // Register new MeshCore integration bridge functions
  lua_register(L, "_mesh_get_channels", lua_mesh_get_channels);
  lua_register(L, "_mesh_delete_public", lua_mesh_delete_public);
  lua_register(L, "_mesh_restore_public", lua_mesh_restore_public);
  lua_register(L, "_mesh_public_deleted", lua_mesh_public_deleted);
  lua_register(L, "_mesh_set_channel", lua_mesh_set_channel);
  lua_register(L, "_mesh_send_channel", lua_mesh_send_channel);
  lua_register(L, "_mesh_remove_contact", lua_mesh_remove_contact);
  lua_register(L, "_mesh_readd_contact", lua_mesh_readd_contact);
  lua_register(L, "_mesh_archive_read", lua_mesh_archive_read);
  lua_register(L, "_mesh_archive_compact", lua_mesh_archive_compact);
  lua_register(L, "_mesh_archive_count", lua_mesh_archive_count);
  lua_register(L, "_mesh_clear_contacts", lua_mesh_clear_contacts);
  lua_register(L, "_mesh_reset_path", lua_mesh_reset_path);
  lua_register(L, "_mesh_export_contact", lua_mesh_export_contact);
  lua_register(L, "_mesh_import_contact", lua_mesh_import_contact);
  lua_register(L, "_mesh_share_contact", lua_mesh_share_contact);
  lua_register(L, "_mesh_set_contact_favorite", lua_mesh_set_contact_favorite);
  lua_register(L, "_mesh_login_room", lua_mesh_login_room);  // legacy alias of _mesh_login
  lua_register(L, "_mesh_login", lua_mesh_login_room);
  lua_register(L, "_mesh_logout", lua_mesh_logout);
  lua_register(L, "_mesh_is_connected", lua_mesh_is_connected);
  lua_register(L, "_mesh_send_command", lua_mesh_send_command);
  lua_register(L, "_mesh_send_request", lua_mesh_send_request);
  lua_register(L, "_mesh_get_rx_info", lua_mesh_get_rx_info);
  lua_register(L, "_mesh_get_rx_boost", lua_mesh_get_rx_boost);
  lua_register(L, "_mesh_set_rx_boost", lua_mesh_set_rx_boost);
  lua_register(L, "_mesh_get_autoadd", lua_mesh_get_autoadd);
  lua_register(L, "_mesh_set_autoadd", lua_mesh_set_autoadd);
  // Auto-add hop limit (0 = any). Surfaced in the messenger Contact Settings.
  lua_register(L, "_mesh_get_autoadd_max_hops", [](lua_State *L) -> int {
    MESH_LOCK();
    int v = the_mesh ? the_mesh->_prefs.autoadd_max_hops : 0;
    MESH_UNLOCK();
    lua_pushinteger(L, v);
    return 1;
  });
  lua_register(L, "_mesh_set_autoadd_max_hops", [](lua_State *L) -> int {
    int v = (int)luaL_checkinteger(L, 1);
    if (v < 0) v = 0;
    if (v > 64) v = 64;
    MESH_LOCK();
    if (the_mesh) { the_mesh->_prefs.autoadd_max_hops = (uint8_t)v; the_mesh->savePrefs(); }
    MESH_UNLOCK();
    lua_pushboolean(L, 1);
    return 1;
  });
  // Advert location-sharing policy (0 = omit GPS location from self-adverts).
  lua_register(L, "_mesh_get_advert_loc", [](lua_State *L) -> int {
    MESH_LOCK();
    bool on = the_mesh ? (the_mesh->_prefs.advert_loc_policy != 0) : false;
    MESH_UNLOCK();
    lua_pushboolean(L, on);
    return 1;
  });
  lua_register(L, "_mesh_set_advert_loc", [](lua_State *L) -> int {
    bool on = lua_toboolean(L, 1);
    MESH_LOCK();
    if (the_mesh) { the_mesh->_prefs.advert_loc_policy = on ? 1 : 0; the_mesh->savePrefs(); }
    MESH_UNLOCK();
    lua_pushboolean(L, 1);
    return 1;
  });
  // Default flood scope / region (name -> SHA256 transport key; "" clears it).
  lua_register(L, "_mesh_get_flood_scope", [](lua_State *L) -> int {
    MESH_LOCK();
    const char* n = the_mesh ? the_mesh->getDefaultScopeName() : "";
    lua_pushstring(L, n ? n : "");
    MESH_UNLOCK();
    return 1;
  });
  lua_register(L, "_mesh_set_flood_scope", [](lua_State *L) -> int {
    const char* name = luaL_optstring(L, 1, "");
    MESH_LOCK();
    if (the_mesh) the_mesh->setDefaultScope(name);
    MESH_UNLOCK();
    lua_pushboolean(L, 1);
    return 1;
  });
  lua_register(L, "_mesh_get_contact_paths", lua_mesh_get_contact_paths);
  lua_register(L, "_mesh_set_contact_path", lua_mesh_set_contact_path);
  lua_register(L, "_mesh_get_message_paths", lua_mesh_get_message_paths);
  lua_register(L, "_mesh_get_msg_repeat", lua_mesh_get_msg_repeat);
  lua_register(L, "_mesh_set_msg_repeat", lua_mesh_set_msg_repeat);
  lua_register(L, "_mesh_get_repeat_status", lua_mesh_get_repeat_status);

  // Persistent message history APIs — available to any app, not just messenger
  lua_register(L, "_mesh_get_channel_messages", lua_mesh_get_channel_messages);
  lua_register(L, "_mesh_routing_query", lua_mesh_routing_query);
  lua_register(L, "_mesh_routing_senders", lua_mesh_routing_senders);
  lua_register(L, "_mesh_get_dm_messages", lua_mesh_get_dm_messages);
  lua_register(L, "_mesh_get_dm_threads", lua_mesh_get_dm_threads);
  lua_register(L, "_mesh_get_msg_summaries", lua_mesh_get_msg_summaries);
  lua_register(L, "_mesh_set_max_messages", lua_mesh_set_max_messages);

  // Unread counters (C-side so they survive Lua teardown during ELF runs)
  lua_register(L, "_mesh_unread_total", lua_mesh_unread_total);
  lua_register(L, "_mesh_unread_channel", lua_mesh_unread_channel);
  lua_register(L, "_mesh_unread_dm", lua_mesh_unread_dm);
  lua_register(L, "_mesh_unread_clear_channel", lua_mesh_unread_clear_channel);
  lua_register(L, "_mesh_unread_clear_dm", lua_mesh_unread_clear_dm);

  // Identity management
  lua_register(L, "_mesh_export_private_key", lua_mesh_export_private_key);
  lua_register(L, "_mesh_import_private_key", lua_mesh_import_private_key);
  lua_register(L, "_mesh_generate_identity", lua_mesh_generate_identity);

  lua_register(L, "_get_battery_mv", [](lua_State *L) -> int {
    lua_pushinteger(L, board.getBattMilliVolts());
    return 1;
  });

  // Register Storage bridge functions
  lua_register(L, "_storage_get_info", lua_storage_get_info);
  lua_register(L, "_storage_set_use_sd", lua_storage_set_use_sd);

  // Message / routing history retention, in days (0 = unlimited). Drives the
  // routing-log prune and (Phase 4) the text-log age prune.
  lua_register(L, "_msg_retain_get", [](lua_State *L) -> int {
    lua_pushinteger(L, msg_retain_days);
    return 1;
  });
  lua_register(L, "_msg_retain_set", [](lua_State *L) -> int {
    int v = (int)luaL_checkinteger(L, 1);
    if (v < 0) v = 0;
    if (v > 3650) v = 3650;
    msg_retain_days = (uint16_t)v;
    if (the_mesh) the_mesh->_msg_retain_days = msg_retain_days;
    firmware_prefs_save();
    lua_pushboolean(L, 1);
    return 1;
  });

  // Multi-byte path hash ("path hash mode"): bytes of each repeater's key
  // appended per hop in a flood path. 0=1 byte (default), 1=2 bytes, 2=3 bytes.
  // Lives in the mesh's own prefs (savePrefs), same value the BLE companion sets.
  lua_register(L, "_mesh_get_path_hash_mode", [](lua_State *L) -> int {
    lua_pushinteger(L, the_mesh ? the_mesh->_prefs.path_hash_mode : 0);
    return 1;
  });
  lua_register(L, "_mesh_set_path_hash_mode", [](lua_State *L) -> int {
    int v = (int)luaL_checkinteger(L, 1);
    if (v < 0) v = 0;
    if (v > 2) v = 2;
    if (the_mesh) {
      the_mesh->_prefs.path_hash_mode = (uint8_t)v;
      the_mesh->savePrefs();
    }
    lua_pushboolean(L, 1);
    return 1;
  });

  lua_register(L, "_emoji_preload", lua_emoji_preload);
  lua_register(L, "_emoji_compose", lua_emoji_compose);
  lua_register(L, "_emoji_decompose", lua_emoji_decompose);
  lua_register(L, "_emoji_blob_count", lua_emoji_blob_count);
  lua_register(L, "_emoji_blob_list", lua_emoji_blob_list);
  lua_register(L, "_emoji_font_reload", lua_emoji_font_reload);

  // Register Filesystem bridge functions
  lua_register(L, "_list_dir", lua_list_dir);
  // Unified drive-aware _fs_* family (fs_bridge.cpp) — used by lib/fileman.lua
  fs_bridge_register(L);

  // System
  lua_register(L, "_system_reboot", [](lua_State *L) -> int {
    SLog.println("[SYSTEM] Reboot requested from Lua");
    delay(100);
    ESP.restart();
    return 0;
  });

  // Heap stats: free + largest contiguous block for PSRAM and internal RAM.
  // Returns: psram_free, psram_largest, internal_free, internal_largest (bytes).
  // The largest-block value is what matters for big single allocations (e.g. an
  // LVGL canvas buffer), since total free can be fragmented.
  lua_register(L, "_heap_info", [](lua_State *L) -> int {
    lua_pushinteger(L, (lua_Integer)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    lua_pushinteger(L, (lua_Integer)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
    lua_pushinteger(L, (lua_Integer)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    lua_pushinteger(L, (lua_Integer)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    return 4;
  });


  // RTC epoch seconds (seeded from GPS once at boot, then free-running)
  lua_register(L, "_rtc_time", [](lua_State *L) -> int {
    MESH_LOCK();
    lua_Integer t = (lua_Integer)the_mesh->getRTCClock()->getCurrentTime();
    MESH_UNLOCK();
    lua_pushinteger(L, t);
    return 1;
  });

  // Effective timezone offset in minutes (resolves "auto" to longitude-derived offset).
  lua_register(L, "_rtc_tz_offset_minutes", [](lua_State *L) -> int {
    lua_pushinteger(L, (lua_Integer)tz_effective_offset_minutes());
    return 1;
  });

  // Returns current TZ setting: "auto" or a stringified integer (minutes).
  lua_register(L, "_rtc_tz_get", [](lua_State *L) -> int {
    lua_pushstring(L, tz_setting_str.c_str());
    return 1;
  });

  // _rtc_tz_set("auto") | _rtc_tz_set(<minutes:int>)
  // Examples: _rtc_tz_set("auto")  _rtc_tz_set(-300)  _rtc_tz_set(330) -- India
  // Returns: ok:bool, effective_offset:int
  lua_register(L, "_rtc_tz_set", [](lua_State *L) -> int {
    if (lua_isstring(L, 1) && !lua_isnumber(L, 1)) {
      const char* v = lua_tostring(L, 1);
      if (v && strcasecmp(v, "auto") == 0) {
        tz_is_auto = true;
        tz_setting_str = "auto";
        firmware_prefs_save();
        lua_pushboolean(L, 1);
        lua_pushinteger(L, (lua_Integer)tz_effective_offset_minutes());
        return 2;
      }
      lua_pushboolean(L, 0);
      lua_pushinteger(L, 0);
      return 2;
    }
    if (lua_isnumber(L, 1)) {
      int32_t m = (int32_t)lua_tointeger(L, 1);
      if (m < -14 * 60 || m > 14 * 60) {
        lua_pushboolean(L, 0);
        lua_pushinteger(L, 0);
        return 2;
      }
      tz_is_auto = false;
      tz_manual_minutes = m;
      tz_setting_str = String(m);
      firmware_prefs_save();
      lua_pushboolean(L, 1);
      lua_pushinteger(L, (lua_Integer)tz_effective_offset_minutes());
      return 2;
    }
    lua_pushboolean(L, 0);
    lua_pushinteger(L, 0);
    return 2;
  });

  lua_register(L, "_dst_get", [](lua_State *L) -> int {
    lua_pushboolean(L, dst_enabled ? 1 : 0);
    return 1;
  });

  lua_register(L, "_dst_set", [](lua_State *L) -> int {
    dst_enabled = lua_toboolean(L, 1);
    firmware_prefs_save();
    lua_pushboolean(L, 1);
    return 1;
  });

  // _gps_sync_start() — triggers a new GPS sync if one isn't already running.
  // Returns true if started, false if a sync is currently in progress.
  lua_register(L, "_gps_sync_start", [](lua_State *L) -> int {
    if (!gps_sync_done) {
      lua_pushboolean(L, 0);
      return 1;
    }
    gps_notify_wake();
    lua_pushboolean(L, 1);
    return 1;
  });

  // _gps_sync_status() — returns done:bool, has_location:bool
  lua_register(L, "_gps_sync_status", [](lua_State *L) -> int {
    lua_pushboolean(L, gps_sync_done ? 1 : 0);
    lua_pushboolean(L, gps_location_valid_at_fix ? 1 : 0);
    return 2;
  });

  // _gps_info() — returns syncing:bool, got_fix:bool, has_location:bool,
  //               lat:number, lng:number, sats:int, hdop:number
  lua_register(L, "_gps_info", [](lua_State *L) -> int {
    lua_pushboolean(L, !gps_sync_done ? 1 : 0);
    lua_pushboolean(L, gps_time_fix_valid ? 1 : 0);
    lua_pushboolean(L, gps_location_valid_at_fix ? 1 : 0);
    lua_pushnumber(L, gps_lat_at_fix);
    lua_pushnumber(L, gps_lng_at_fix);
    lua_pushinteger(L, (lua_Integer)gps_sats_at_fix);
    lua_pushnumber(L, gps_hdop_at_fix / 100.0);
    return 7;
  });

  lua_register(L, "_clock_fmt_get", [](lua_State *L) -> int {
    lua_pushstring(L, clock_fmt_str.c_str());
    return 1;
  });

  lua_register(L, "_clock_fmt_set", [](lua_State *L) -> int {
    const char *v = luaL_checkstring(L, 1);
    clock_fmt_str = (strcmp(v, "12") == 0) ? "12" : "24";
    firmware_prefs_save();
    lua_pushboolean(L, 1);
    return 1;
  });

  // ── UI Theme ───────────────────────────────────────────────────────────────
  // The selected theme id is persisted here; the palette it maps to is pushed
  // live to the LVGL theme via _theme_apply_palette, and its background is drawn
  // entirely in Lua (lib/theme + lib/background).
  lua_register(L, "_theme_pref_get", [](lua_State *L) -> int {
    lua_pushstring(L, theme_pref_str.c_str());
    return 1;
  });

  lua_register(L, "_theme_pref_set", [](lua_State *L) -> int {
    const char *v = luaL_checkstring(L, 1);
    // Idempotent: theme.apply() runs at every boot, so only touch flash when the
    // selection actually changed (avoids a needless write each power-on).
    if (theme_pref_str != v) {
      theme_pref_str = String(v);
      firmware_prefs_save();
    }
    lua_pushboolean(L, 1);
    return 1;
  });

  // _theme_apply_palette(scr, card, text, grey, accent, btn_text, dark)
  // Each color is a "#rrggbb"/"rrggbb" string or a 0xRRGGBB integer. Re-cascades
  // to every live widget (no reboot); a no-op on the C side if unchanged.
  lua_register(L, "_theme_apply_palette", [](lua_State *L) -> int {
    auto parse = [&](int idx) -> uint32_t {
      if (lua_type(L, idx) == LUA_TNUMBER) {
        return (uint32_t)lua_tointeger(L, idx) & 0xFFFFFFu;
      }
      const char *s = lua_tostring(L, idx);
      if (!s) return 0;
      if (*s == '#') s++;
      return (uint32_t)strtoul(s, nullptr, 16) & 0xFFFFFFu;
    };
    uint32_t scr      = parse(1);
    uint32_t card     = parse(2);
    uint32_t text     = parse(3);
    uint32_t grey     = parse(4);
    uint32_t accent   = parse(5);
    uint32_t btn_text = parse(6);
    bool dark = lua_isnoneornil(L, 7) ? true : (lua_toboolean(L, 7) != 0);
    lv_theme_meshpunk_set_palette(scr, card, text, grey, accent, btn_text, dark);
    lua_pushboolean(L, 1);
    return 1;
  });

  lua_register(L, "_rtc_set_time", [](lua_State *L) -> int {
    lua_Integer ts = luaL_checkinteger(L, 1);
    if (ts < 0) {
      lua_pushboolean(L, 0);
      return 1;
    }
    MESH_LOCK();
    the_mesh->getRTCClock()->setCurrentTime((uint32_t)ts);
    MESH_UNLOCK();
    gps_manual_time_override = true;
    SLog.printf("[RTC] Manual time set to %u UTC, GPS override enabled\n", (unsigned)ts);
    lua_pushboolean(L, 1);
    return 1;
  });

  lua_register(L, "_rtc_manual_override_get", [](lua_State *L) -> int {
    lua_pushboolean(L, gps_manual_time_override ? 1 : 0);
    return 1;
  });

  lua_register(L, "_rtc_manual_override_clear", [](lua_State *L) -> int {
    gps_manual_time_override = false;
    SLog.println("[RTC] Manual override cleared; GPS time updates re-enabled.");
    lua_pushboolean(L, 1);
    return 1;
  });

  // ── Sound ──────────────────────────────────────────────────────────────────
  sound_register_lua(L);

  // ── ELF module loader ─────────────────────────────────────────────────────
  elf_host_register_lua(L);

  // ── Keyboard backlight ────────────────────────────────────────────────────
  lua_register(L, "_kbd_set_brightness", [](lua_State* L) -> int {
    int v = luaL_checkinteger(L, 1);
    if (v < 0) v = 0; if (v > 255) v = 255;
    kbd_brightness = (uint8_t)v;
    setKeyboardBrightness(kbd_brightness);
    firmware_prefs_save();
    lua_pushinteger(L, kbd_brightness);
    return 1;
  });
  lua_register(L, "_kbd_get_brightness", [](lua_State* L) -> int {
    lua_pushinteger(L, kbd_brightness);
    return 1;
  });
  lua_register(L, "_kbd_set_brightness_temp", [](lua_State* L) -> int {
    int v = luaL_checkinteger(L, 1);
    if (v < 0) v = 0; if (v > 255) v = 255;
    setKeyboardBrightness((uint8_t)v);
    lua_pushinteger(L, v);
    return 1;
  });
  lua_register(L, "_kbd_is_timed_out", [](lua_State* L) -> int {
    lua_pushboolean(L, kbd_timed_out);
    return 1;
  });

  // ── Notification preferences ────────────────────────────────────────────
  lua_register(L, "_notify_kbd_get", [](lua_State* L) -> int {
    lua_pushboolean(L, notify_kbd_enabled);
    return 1;
  });
  lua_register(L, "_notify_kbd_set", [](lua_State* L) -> int {
    notify_kbd_enabled = lua_toboolean(L, 1);
    firmware_prefs_save();
    lua_pushboolean(L, notify_kbd_enabled);
    return 1;
  });
  lua_register(L, "_notify_sound_get", [](lua_State* L) -> int {
    lua_pushboolean(L, notify_sound_enabled);
    return 1;
  });
  lua_register(L, "_notify_sound_set", [](lua_State* L) -> int {
    notify_sound_enabled = lua_toboolean(L, 1);
    firmware_prefs_save();
    lua_pushboolean(L, notify_sound_enabled);
    return 1;
  });
  // Per-channel notification mode, keyed by channel NAME (see notify.h):
  // 0 = off, 1 = mention-only (default), 2 = every message.
  lua_register(L, "_notify_channel_get", [](lua_State* L) -> int {
    const char* name = luaL_checkstring(L, 1);
    uint8_t mode = NOTIFY_CHAN_MENTION;
    if (the_mesh) {
      MESH_LOCK();
      mode = the_mesh->getChannelNotifyMode(name);
      MESH_UNLOCK();
    }
    lua_pushinteger(L, mode);
    return 1;
  });
  lua_register(L, "_notify_channel_set", [](lua_State* L) -> int {
    const char* name = luaL_checkstring(L, 1);
    int mode = (int)luaL_checkinteger(L, 2);
    if (mode < 0 || mode > NOTIFY_CHAN_ALL) mode = NOTIFY_CHAN_MENTION;
    if (the_mesh) {
      MESH_LOCK();
      the_mesh->setChannelNotifyMode(name, (uint8_t)mode);
      MESH_UNLOCK();
    }
    lua_pushinteger(L, mode);
    return 1;
  });

  // ── BLE companion ──────────────────────────────────────────────────────────
#if BLE_COMPANION_ENABLED
  lua_register(L, "_ble_get_enabled", [](lua_State* L) -> int {
    lua_pushboolean(L, ble_enabled_pref);
    return 1;
  });
  lua_register(L, "_ble_set_enabled", [](lua_State* L) -> int {
    bool v = lua_toboolean(L, 1);
    ble_enabled_pref = v;
    MESH_LOCK();
    if (v && !ble_serial) {
      ble_companion_init_early();
      ble_companion_start(*the_mesh);
    } else if (!v && ble_serial) {
      ble_companion_stop();
    }
    MESH_UNLOCK();
    firmware_prefs_save();
    lua_pushboolean(L, ble_enabled_pref);
    return 1;
  });
  lua_register(L, "_ble_is_connected", [](lua_State* L) -> int {
    lua_pushboolean(L, ble_companion && ble_companion->isConnected());
    return 1;
  });
  lua_register(L, "_ble_get_bond_clear", [](lua_State* L) -> int {
    lua_pushboolean(L, ble_bond_clear_pref);
    return 1;
  });
  lua_register(L, "_ble_set_bond_clear", [](lua_State* L) -> int {
    ble_bond_clear_pref = lua_toboolean(L, 1);
    firmware_prefs_save();
    lua_pushboolean(L, ble_bond_clear_pref);
    return 1;
  });
#endif

  // ── Display backlight ─────────────────────────────────────────────────────
  lua_register(L, "_disp_set_brightness", [](lua_State* L) -> int {
    int v = luaL_checkinteger(L, 1);
    if (v < 0) v = 0; if (v > 16) v = 16;
    display_brightness = (uint8_t)v;
    setBrightness(display_brightness);
    firmware_prefs_save();
    lua_pushinteger(L, display_brightness);
    return 1;
  });
  lua_register(L, "_disp_get_brightness", [](lua_State* L) -> int {
    lua_pushinteger(L, display_brightness);
    return 1;
  });

  // ── Inactivity timeouts ──────────────────────────────────────────────────
  lua_register(L, "_screen_timeout_set", [](lua_State* L) -> int {
    int v = luaL_checkinteger(L, 1);
    if (v < 0) v = 0; if (v > 65535) v = 65535;
    screen_timeout_secs = (uint16_t)v;
    last_activity_ms = millis();
    if (screen_timed_out) { setBrightness(display_brightness); screen_timed_out = false; }
    firmware_prefs_save();
    lua_pushinteger(L, screen_timeout_secs);
    return 1;
  });
  lua_register(L, "_screen_timeout_get", [](lua_State* L) -> int {
    lua_pushinteger(L, screen_timeout_secs);
    return 1;
  });
  lua_register(L, "_kbd_timeout_set", [](lua_State* L) -> int {
    int v = luaL_checkinteger(L, 1);
    if (v < 0) v = 0; if (v > 65535) v = 65535;
    kbd_timeout_secs = (uint16_t)v;
    last_activity_ms = millis();
    if (kbd_timed_out) { setKeyboardBrightness(kbd_brightness); kbd_timed_out = false; }
    firmware_prefs_save();
    lua_pushinteger(L, kbd_timeout_secs);
    return 1;
  });
  lua_register(L, "_kbd_timeout_get", [](lua_State* L) -> int {
    lua_pushinteger(L, kbd_timeout_secs);
    return 1;
  });

  // ── Trackball sensitivity ───────────────────────────────────────────────
  lua_register(L, "_trackball_sensitivity_set", [](lua_State* L) -> int {
    int v = luaL_checkinteger(L, 1);
    if (v < 0) v = 0; if (v > 500) v = 500;
    trackball_sensitivity_ms = (uint16_t)v;
    firmware_prefs_save();
    lua_pushinteger(L, trackball_sensitivity_ms);
    return 1;
  });
  lua_register(L, "_trackball_sensitivity_get", [](lua_State* L) -> int {
    lua_pushinteger(L, trackball_sensitivity_ms);
    return 1;
  });
  lua_register(L, "_trackball_roll_set", [](lua_State* L) -> int {
    int v = luaL_checkinteger(L, 1);
    if (v < 0) v = 0; if (v > 500) v = 500;
    trackball_roll_ms = (uint16_t)v;
    firmware_prefs_save();
    lua_pushinteger(L, trackball_roll_ms);
    return 1;
  });
  lua_register(L, "_trackball_roll_get", [](lua_State* L) -> int {
    lua_pushinteger(L, trackball_roll_ms);
    return 1;
  });

  // Sym key behavior: false = hold modifier (default), true = tap toggles
  // the symbol layer (hold still works as momentary). Persisted.
  lua_register(L, "_kb_sym_toggle_set", [](lua_State* L) -> int {
    kb_sym_toggle_pref = lua_toboolean(L, 1);
    if (!kb_sym_toggle_pref) kb_sym_latched = false;
    firmware_prefs_save();
    return 0;
  });
  lua_register(L, "_kb_sym_toggle_get", [](lua_State* L) -> int {
    lua_pushboolean(L, kb_sym_toggle_pref ? 1 : 0);
    return 1;
  });

  // Alt key behavior: false = hold modifier (default), true = tap toggles
  // the emoji layer (hold still works as momentary). Persisted.
  lua_register(L, "_kb_alt_toggle_set", [](lua_State* L) -> int {
    kb_alt_toggle_pref = lua_toboolean(L, 1);
    if (!kb_alt_toggle_pref) kb_alt_latched = false;
    firmware_prefs_save();
    return 0;
  });
  lua_register(L, "_kb_alt_toggle_get", [](lua_State* L) -> int {
    lua_pushboolean(L, kb_alt_toggle_pref ? 1 : 0);
    return 1;
  });

  // Alt emoji layer keymap. Keys are identified by their normal-layer char
  // ("a".."z", "$"); codepoints may be blob singles OR sequence PUAs.
  lua_register(L, "_kb_emoji_get", [](lua_State* L) -> int {
    const char *k = luaL_checkstring(L, 1);
    uint8_t c = (uint8_t)k[0];
    lua_pushinteger(L, (c && c < 128) ? (lua_Integer)kb_emoji_map[c] : 0);
    return 1;
  });
  lua_register(L, "_kb_emoji_set", [](lua_State* L) -> int {
    const char *k = luaL_checkstring(L, 1);
    uint32_t cp = (uint32_t)luaL_checkinteger(L, 2);   // 0 clears the key
    uint8_t c = (uint8_t)k[0];
    if (!c || c >= 128) { lua_pushboolean(L, 0); return 1; }
    kb_emoji_map[c] = cp;
    kb_emoji_map_save();
    lua_pushboolean(L, 1);
    return 1;
  });
  lua_register(L, "_kb_emoji_reset", [](lua_State* L) -> int {
    kb_emoji_apply_defaults();
    LittleFS.remove("/emoji_keymap");
    return 0;
  });

  // Top bar transparency: true = the themed wallpaper shows through the status
  // bar, false = a solid (themed card) background. Persisted. Applied live to the
  // running bar by lib/topbar.apply_transparency().
  lua_register(L, "_topbar_transparant_set", [](lua_State* L) -> int {
    topbar_transparant = lua_toboolean(L, 1);
    firmware_prefs_save();
    return 0;
  });
  lua_register(L, "_topbar_transparant_get", [](lua_State* L) -> int {
    lua_pushboolean(L, topbar_transparant ? 1 : 0);
    return 1;
  });

  // Selection/focus highlight fill style (global, applies to every theme):
  // false = translucent "highlighted fill", true = opaque solid. Applies live.
  lua_register(L, "_theme_focus_solid_set", [](lua_State* L) -> int {
    theme_focus_solid = lua_toboolean(L, 1);
    lv_theme_meshpunk_set_focus_solid(theme_focus_solid);
    firmware_prefs_save();
    return 0;
  });
  lua_register(L, "_theme_focus_solid_get", [](lua_State* L) -> int {
    lua_pushboolean(L, theme_focus_solid ? 1 : 0);
    return 1;
  });

  // Selection/focus tint direction (global): false = brighten, true = darken.
  lua_register(L, "_theme_focus_darken_set", [](lua_State* L) -> int {
    theme_focus_darken = lua_toboolean(L, 1);
    lv_theme_meshpunk_set_focus_darken(theme_focus_darken);
    firmware_prefs_save();
    return 0;
  });
  lua_register(L, "_theme_focus_darken_get", [](lua_State* L) -> int {
    lua_pushboolean(L, theme_focus_darken ? 1 : 0);
    return 1;
  });

  // QR code: create an lv_qrcode child inside a Lua object, encoding `text`.
  // Usage: local ok = _qr_create(parent_obj, "meshcore://...", size_px)
  // The QR is centered in the parent; deleting the parent removes it.
  lua_register(L, "_qr_create", [](lua_State *L) -> int {
    luavgl_obj_t *lobj = (luavgl_obj_t *)lua_touserdata(L, 1);
    if (!lobj || !lobj->obj) { lua_pushboolean(L, 0); return 1; }
    const char *text = luaL_checkstring(L, 2);
    int size = luaL_optinteger(L, 3, 180);
    lv_obj_t *qr = lv_qrcode_create(lobj->obj);
    if (!qr) { lua_pushboolean(L, 0); return 1; }
    lv_qrcode_set_size(qr, size);
    lv_qrcode_set_dark_color(qr, lv_color_black());
    lv_qrcode_set_light_color(qr, lv_color_white());
    lv_result_t r = lv_qrcode_update(qr, text, strlen(text));
    if (r != LV_RESULT_OK) {
      lv_obj_delete(qr);
      lua_pushboolean(L, 0);
      return 1;
    }
    lv_obj_center(qr);
    lua_pushboolean(L, 1);
    return 1;
  });

  // Register gridnav bridge
  // Usage: _gridnav_add(obj, flags)
  //   flags: 0=none, 1=rollover, 2=scroll_first
  lua_register(L, "_gridnav_add", [](lua_State *L) -> int {
    luavgl_obj_t *lobj = (luavgl_obj_t *)lua_touserdata(L, 1);
    if (!lobj || !lobj->obj) {
      lua_pushboolean(L, 0);
      return 1;
    }
    int flags = luaL_optinteger(L, 2, 0);
    lv_gridnav_add(lobj->obj, (lv_gridnav_ctrl_t)flags);
    lua_pushboolean(L, 1);
    return 1;
  });
  lua_register(L, "_gridnav_remove", [](lua_State *L) -> int {
    luavgl_obj_t *lobj = (luavgl_obj_t *)lua_touserdata(L, 1);
    if (!lobj || !lobj->obj) return 0;
    lv_gridnav_remove(lobj->obj);
    return 0;
  });

  // Gridnav flag constants for Lua
  lua_pushinteger(L, LV_GRIDNAV_CTRL_NONE);
  lua_setglobal(L, "GRIDNAV_NONE");
  lua_pushinteger(L, LV_GRIDNAV_CTRL_ROLLOVER);
  lua_setglobal(L, "GRIDNAV_ROLLOVER");
  lua_pushinteger(L, LV_GRIDNAV_CTRL_SCROLL_FIRST);
  lua_setglobal(L, "GRIDNAV_SCROLL_FIRST");

  // Navigation controller: a stack of navigable scopes (gridnav + touch/trackball
  // switching). _nav_setup replaces the TOP scope (back-compat with the old
  // single-container model: existing apps stay at stack depth 1); _nav_push /
  // _nav_pop add real nesting for popups and row-select lists.
  lua_register(L, "_nav_setup", [](lua_State *L) -> int {
    luavgl_obj_t *lobj = (luavgl_obj_t *)lua_touserdata(L, 1);
    if (!lobj || !lobj->obj) return 0;
    int flags = luaL_optinteger(L, 2, LV_GRIDNAV_CTRL_ROLLOVER);
    bool preserve_scroll = lua_toboolean(L, 3);

    nav_check_valid();
    NavScope *top = nav_top();

    // Proven-safe timing, preserved exactly: DEFER removing a DIFFERENT outgoing
    // container's gridnav (it may be mid-event-dispatch), but remove immediately
    // when re-setting up the SAME container. flush_pending first so a second
    // deferred removal can't clobber the first (two pending removals = orphaned
    // gridnav = the watchdog hang).
    if (top && top->cont != lobj->obj) {
      flush_pending_gridnav();
      if (top->armed) pending_gridnav_remove = top->cont;
    } else if (top && top->cont == lobj->obj && top->armed) {
      lv_gridnav_remove(top->cont);
    }

    if (!top) { nav_depth = 1; top = &nav_stack[0]; }
    top->cont = lobj->obj;
    top->flags = (lv_gridnav_ctrl_t)flags;
    top->armed = false;
    nav_install(top, preserve_scroll);

    // No nav_delete_cb registration — gridnav's own LV_EVENT_DELETE handler
    // cleans up its resources. Adding a second DELETE handler caused a crash:
    // when nav_delete_cb fired first (preprocess) and called lv_gridnav_remove(),
    // it modified the event array while lv_event_send was iterating it with
    // cached pointers, causing a stale-pointer read (0xbaad5678).
    // nav_check_valid() in keyboard/touch callbacks detects freed containers.
    return 0;
  });

  // Open a nested scope over the current one (a popup, or a row-select list over
  // its controls). Suspends the parent (drops it from the focus group, defers
  // removing its gridnav) and makes `cont` the active scope. Args: cont, [flags],
  // [focus child], [preserve_scroll]. Pop with _nav_pop to resume the parent.
  lua_register(L, "_nav_push", [](lua_State *L) -> int {
    luavgl_obj_t *lobj = (luavgl_obj_t *)lua_touserdata(L, 1);
    if (!lobj || !lobj->obj) return 0;
    int flags = luaL_optinteger(L, 2, LV_GRIDNAV_CTRL_ROLLOVER);
    luavgl_obj_t *fobj = (luavgl_obj_t *)lua_touserdata(L, 3);  // optional focus
    bool preserve_scroll = lua_toboolean(L, 4);

    nav_check_valid();
    NavScope *top = nav_top();
    if (top && top->cont && lv_obj_is_valid(top->cont)) {
      flush_pending_gridnav();
      if (top->armed) pending_gridnav_remove = top->cont;
      lv_group_remove_obj(top->cont);
      top->armed = false;
    }
    if (nav_depth >= NAV_STACK_MAX) nav_depth = NAV_STACK_MAX - 1;  // overflow guard
    NavScope *s = &nav_stack[nav_depth++];
    s->cont = lobj->obj;
    s->flags = (lv_gridnav_ctrl_t)flags;
    s->armed = false;
    nav_install(s, preserve_scroll);
    if (fobj && fobj->obj && lv_obj_is_valid(fobj->obj))
      lv_gridnav_set_focused(s->cont, fobj->obj, LV_ANIM_OFF);
    return 0;
  });

  // Close the top scope and resume the one beneath it. The closing container is
  // usually deleted by the caller right after (its gridnav is deferred-removed
  // here and finalized by its own DELETE handler); the parent is re-armed.
  lua_register(L, "_nav_pop", [](lua_State *L) -> int {
    (void)L;
    nav_check_valid();
    NavScope *top = nav_top();
    if (top) {
      if (top->armed && top->cont && lv_obj_is_valid(top->cont)) {
        flush_pending_gridnav();
        pending_gridnav_remove = top->cont;
      }
      if (top->cont && lv_obj_is_valid(top->cont)) lv_group_remove_obj(top->cont);
      top->cont = NULL;
      top->armed = false;
      nav_depth--;
    }
    NavScope *below = nav_top();
    if (below && below->cont && lv_obj_is_valid(below->cont)) {
      nav_install(below, true);  // resume: preserve scroll, pick the visible child
    }
    return 0;
  });

  lua_register(L, "_nav_set_focused", [](lua_State *L) -> int {
    luavgl_obj_t *lobj = (luavgl_obj_t *)lua_touserdata(L, 1);
    NavScope *top = nav_top();
    if (!lobj || !lobj->obj || !top || !top->cont || !top->armed) return 0;
    lv_gridnav_set_focused(top->cont, lobj->obj, LV_ANIM_OFF);
    return 0;
  });

  lua_register(L, "_nav_is_active", [](lua_State *L) -> int {
    NavScope *top = nav_top();
    lua_pushboolean(L, top && top->armed);
    return 1;
  });

  // _nav_clear (back-compat) and _nav_reset both tear down the whole stack.
  lua_register(L, "_nav_clear", lua_nav_reset);
  lua_register(L, "_nav_reset", lua_nav_reset);

  // Reset all input devices: clear their act/scroll/last object references and
  // bail the in-flight gesture (reset_query). A synchronous lv_obj_delete does
  // this per deleted object (obj_indev_reset); when a view is torn down
  // ASYNCHRONOUSLY (apps.delete_view hides then drains it over ticks) nothing
  // resets the indev, so a lingering touch/scroll on the doomed subtree later
  // dereferences a freed ->parent (LoadProhibited @ 0x4). Call before tearing
  // down the view the user just interacted with.
  lua_register(L, "_indev_reset", [](lua_State *L) -> int {
    (void)L;
    lv_indev_reset(NULL, NULL);
    return 0;
  });

  lua_register(L, "_kb_is_down", [](lua_State *L) -> int {
    int key = luaL_checkinteger(L, 1);
    lua_pushboolean(L, key >= 0 && key < 128 && kb_key_state[key]);
    return 1;
  });

  lua_register(L, "_kb_just_pressed", [](lua_State *L) -> int {
    int key = luaL_checkinteger(L, 1);
    lua_pushboolean(L, key >= 0 && key < 128 && kb_key_state[key] && !kb_key_prev[key]);
    return 1;
  });

  lua_register(L, "_kb_just_released", [](lua_State *L) -> int {
    int key = luaL_checkinteger(L, 1);
    lua_pushboolean(L, key >= 0 && key < 128 && !kb_key_state[key] && kb_key_prev[key]);
    return 1;
  });

  lua_register(L, "_kb_is_held", [](lua_State *L) -> int {
    int key = luaL_checkinteger(L, 1);
    bool held = false;
    if (key >= 0 && key < 128 && kb_key_state[key] && kb_key_press_time[key] > 0) {
      held = (millis() - kb_key_press_time[key]) > KEY_HOLD_THRESHOLD_MS;
    }
    lua_pushboolean(L, held);
    return 1;
  });

  lua_register(L, "_kb_hold_duration", [](lua_State *L) -> int {
    int key = luaL_checkinteger(L, 1);
    if (key >= 0 && key < 128 && kb_key_state[key] && kb_key_press_time[key] > 0) {
      lua_pushinteger(L, millis() - kb_key_press_time[key]);
    } else {
      lua_pushinteger(L, 0);
    }
    return 1;
  });

  lua_register(L, "_kb_shift", [](lua_State *L) -> int {
    lua_pushboolean(L, kb_shift_active);
    return 1;
  });

  lua_register(L, "_kb_lshift", [](lua_State *L) -> int {
    lua_pushboolean(L, kb_lshift_active);
    return 1;
  });

  lua_register(L, "_kb_rshift", [](lua_State *L) -> int {
    lua_pushboolean(L, kb_rshift_active);
    return 1;
  });

  lua_register(L, "_kb_sym", [](lua_State *L) -> int {
    lua_pushboolean(L, kb_sym_active);
    return 1;
  });

  lua_register(L, "_kb_alt", [](lua_State *L) -> int {
    lua_pushboolean(L, kb_alt_active);
    return 1;
  });

  lua_register(L, "_obj_move_foreground", [](lua_State *L) -> int {
    luavgl_obj_t *lobj = (luavgl_obj_t *)lua_touserdata(L, 1);
    if (!lobj || !lobj->obj) return 0;
    lv_obj_t *parent = lv_obj_get_parent(lobj->obj);
    if (parent) {
      int32_t cnt = (int32_t)lv_obj_get_child_count(parent);
      lv_obj_move_to_index(lobj->obj, cnt - 1);
    }
    return 0;
  });

  lua_register(L, "_obj_move_background", [](lua_State *L) -> int {
    luavgl_obj_t *lobj = (luavgl_obj_t *)lua_touserdata(L, 1);
    if (!lobj || !lobj->obj) return 0;
    lv_obj_move_to_index(lobj->obj, 0);
    return 0;
  });

  lua_register(L, "_list_dir_sd", lua_list_dir_sd);
  lua_register(L, "_file_exists_sd", lua_file_exists_sd);
  lua_register(L, "_mkdir_sd", lua_mkdir_sd);
  lua_register(L, "_png_to_bin", lua_png_to_bin);
  lua_register(L, "_tile_fetch_start", lua_tile_fetch_start);
  lua_register(L, "_tile_fetch_poll", lua_tile_fetch_poll);
  lua_register(L, "_tile_fetch_close", lua_tile_fetch_close);
  lua_register(L, "_lvgl_image_cache_drop", lua_lvgl_image_cache_drop);
  lua_register(L, "_tile_pool_alloc", lua_tile_pool_alloc);
  lua_register(L, "_tile_pool_free", lua_tile_pool_free);
  lua_register(L, "_tile_show", lua_tile_show);
  lua_register(L, "_dofile_sd", lua_dofile_sd);
  lua_register(L, "_list_all", lua_list_all);
  lua_register(L, "_list_all_sd", lua_list_all_sd);

  // Add Lua loader for require function
  lua_getglobal(L, "package");
  lua_getfield(L, -1, "searchers");

  // Get the length of the searchers table
  int len = lua_rawlen(L, -1);

  // Custom loader for the filesystem. Streams the file to lua_load() in blocks
  // (see lua_file_chunk_reader) instead of slurping it into one big RAM String,
  // so large modules load reliably regardless of heap fragmentation.
  lua_pushcfunction(L, [](lua_State *L) -> int {
    const char *modname = luaL_checkstring(L, 1);
    String filename = String(LUA_PATH) + modname + ".lua";

    LuaFileChunkReader rdr;
    rdr.file = LittleFS.open(filename, "r");
    if (!rdr.file) {
      lua_pushfstring(L, "\n\tno file '%s' in LittleFS", filename.c_str());
      return 1; // not found -> let require try the next searcher / report it
    }

    int status = lua_load(L, lua_file_chunk_reader, &rdr, filename.c_str(), NULL);
    rdr.file.close();

    if (status != LUA_OK) {
      lua_error(L); // propagate the real syntax error (with file:line)
    }

    return 1; // Return the loaded chunk
  });

  // Add our loader to the searchers table
  lua_rawseti(L, -2, len + 1);
  lua_pop(L, 2); // Pop package.searchers and package

  // Setup print function to redirect to Serial
  luaL_dostring(L, R"(
    local old_print = print
    print = function(...)
      local args = {...}
      local text = ""
      for i, v in ipairs(args) do
        text = text .. tostring(v) .. (i < #args and "\t" or "")
      end
      old_print(text)
    end
  )");

  luaL_newmetatable(L, "esp32_file");

  lua_newtable(L);

  // file:read([mode]) — read(n) for n bytes (binary-safe), read("*a")/read() for all.
  // Both paths read straight into Lua-managed memory (luaL_buffinitsize): one
  // copy total, no malloc bounce buffer, no Arduino String (readString() grew
  // byte-wise — O(n^2) reallocs — and silently TRUNCATED big files; a too-big
  // read now raises a catchable Lua memory error instead). Every Lua call that
  // can longjmp runs while no lock or C allocation is held.
  lua_pushcfunction(L, [](lua_State *L) -> int {
    LuaFileHandle *ud = (LuaFileHandle *)luaL_checkudata(L, 1, "esp32_file");

    // f:read(n) — read n bytes
    if (lua_isnumber(L, 2)) {
      int n = (int)lua_tointeger(L, 2);
      if (n <= 0) { lua_pushstring(L, ""); return 1; }
      luaL_Buffer b;
      char *p = luaL_buffinitsize(L, &b, (size_t)n);
      if (ud->is_sd) sd_spi_take();
      int got = ud->file->read((uint8_t *)p, n);
      if (ud->is_sd) sd_spi_release();
      if (got <= 0) { lua_pushnil(L); return 1; }  // buffer box is GC'd harmlessly
      luaL_pushresultsize(&b, (size_t)got);
      return 1;
    }

    // f:read("*a") or f:read() — read from the current position to EOF
    if (ud->is_sd) sd_spi_take();
    size_t fsize = ud->file->size();
    size_t fpos  = ud->file->position();
    if (ud->is_sd) sd_spi_release();
    size_t remaining = (fsize > fpos) ? fsize - fpos : 0;

    luaL_Buffer b;
    char *p = luaL_buffinitsize(L, &b, remaining);
    size_t off = 0;
    while (off < remaining) {
      // Chunked SPI take/release (like _fs_copy) so a multi-MB SD read never
      // stalls the mesh task for the whole file.
      size_t chunk = remaining - off;
      if (chunk > 32768) chunk = 32768;
      if (ud->is_sd) sd_spi_take();
      int got = ud->file->read((uint8_t *)p + off, chunk);
      if (ud->is_sd) sd_spi_release();
      if (got <= 0) break;  // EOF / IO error: return what we have
      off += (size_t)got;
    }
    luaL_pushresultsize(&b, off);
    return 1;
  });
  lua_setfield(L, -2, "read");

  // file:write(str)
  lua_pushcfunction(L, [](lua_State *L) -> int {
    LuaFileHandle *ud = (LuaFileHandle *)luaL_checkudata(L, 1, "esp32_file");
    size_t len;
    const char *str = luaL_checklstring(L, 2, &len);
    if (ud->is_sd) sd_spi_take();
    // write(buf, len), not print(str): binary-safe past embedded NULs
    size_t written = ud->file->write((const uint8_t *)str, len);
    if (ud->is_sd) sd_spi_release();
    lua_pushinteger(L, written);
    return 1;
  });
  lua_setfield(L, -2, "write");

  // file:flush()
  lua_pushcfunction(L, [](lua_State *L) -> int {
    LuaFileHandle *ud = (LuaFileHandle *)luaL_checkudata(L, 1, "esp32_file");
    if (ud->is_sd) sd_spi_take();
    ud->file->flush();
    if (ud->is_sd) sd_spi_release();
    return 0;
  });
  lua_setfield(L, -2, "flush");

  // file:close()
  lua_pushcfunction(L, [](lua_State *L) -> int {
    LuaFileHandle *ud = (LuaFileHandle *)luaL_checkudata(L, 1, "esp32_file");
    if (ud->file) {
      if (ud->is_sd) sd_spi_take();
      ud->file->close();
      if (ud->is_sd) sd_spi_release();
      delete ud->file;
      ud->file = nullptr;
    }
    return 0;
  });
  lua_setfield(L, -2, "close");

  // Set the __index = method table
  lua_setfield(L, -2, "__index");

  // __gc finalizer
  lua_pushcfunction(L, [](lua_State *L) -> int {
    LuaFileHandle *ud = (LuaFileHandle *)luaL_checkudata(L, 1, "esp32_file");
    if (ud->file) {
      if (ud->is_sd) sd_spi_take();
      ud->file->close();
      if (ud->is_sd) sd_spi_release();
      delete ud->file;
      ud->file = nullptr;
    }
    return 0;
  });
  lua_setfield(L, -2, "__gc");

  lua_pop(L, 1); // pop metatable

  SLog.println("Added esp32_file");


  // Inject our C++-backed io.open into the Lua global 'io' table
  lua_getglobal(L, "io"); // push io table

  if (lua_isnil(L, -1)) {
    lua_newtable(L);           // create io table if not present
    lua_setglobal(L, "io");    // set it
    lua_getglobal(L, "io");    // push it again
  }

  lua_pushcfunction(L, lua_io_open);
  lua_setfield(L, -2, "open"); // io.open = lua_io_open

  lua_pop(L, 1); // pop io table

  SLog.println("Patched IO");

  SLog.println("[LUA] LuaVGL environment initialized");
  SLog.printf("[LUA] Free heap: %d bytes\n", ESP.getFreeHeap());
  SLog.printf("[LUA] Free PSRAM: %d bytes\n", ESP.getFreePsram());

  if (!fs_mounted) {
    SLog.println("Filesystem not mounted, can't load Lua scripts");

    const char *fallbackScript = R"(
    local root = lvgl.Object()
    root:set { w = lvgl.HOR_RES(), h = lvgl.VER_RES() }

    root:Label {
      text = "Filesystem not mounted\nUpload Lua scripts to flash",
      align = lvgl.ALIGN.CENTER
    }

    return root
  )";

    if (luaL_dostring(L, fallbackScript) != 0) {
      SLog.print("Lua fallback script error: ");
      SLog.println(lua_tostring(L, -1));
      lua_pop(L, 1);
    }

    return;
  }

  String scriptPath = String(LUA_PATH) + "main.lua";
  SLog.printf("[LUA] Reading script: %s\n", scriptPath.c_str());
  String script = readFile(scriptPath.c_str());
  SLog.printf("[LUA] Script length: %d bytes\n", script.length());

  if (script.length() == 0) {
    SLog.print("Lua script not found: ");
    SLog.println(scriptPath);

    const char *fallbackScript = R"(
    local root = lvgl.Object()
    root:set { w = lvgl.HOR_RES(), h = lvgl.VER_RES() }

    root:Label {
      text = "Lua script missing",
      align = lvgl.ALIGN.CENTER
    }

    return root
  )";

    luaL_dostring(L, fallbackScript); // no need to recheck error here
    return;
  }

  SLog.print("[LUA] Executing Lua script: ");
  SLog.println(scriptPath);
  SLog.println("[LUA] --- luaL_dostring BEGIN ---");

  int lua_result = luaL_dostring(L, script.c_str());

  SLog.printf("[LUA] --- luaL_dostring END --- result=%d\n", lua_result);

  if (lua_result != 0) {
    const char *luaError = lua_tostring(L, -1);
    SLog.print("Lua execution error: ");
    SLog.println(luaError);

    // Escape any embedded quotes or newlines
    String escapedError = String(luaError);
    escapedError.replace("\\", "\\\\");
    escapedError.replace("\"", "\\\"");
    escapedError.replace("\n", "\\n");

    String fallbackScript = R"(
    local root = lvgl.Object()
    root:set { w = lvgl.HOR_RES(), h = lvgl.VER_RES() }

    root:Label {
      text = ")" + escapedError +
                            R"(",
      align = lvgl.ALIGN.CENTER
    }

    return root
  )";

    if (luaL_dostring(L, fallbackScript.c_str()) != 0) {
      SLog.print("Fallback display error: ");
      SLog.println(lua_tostring(L, -1));
    }

    lua_pop(L, 1);
    return;
  }

  return;
}

// ── Lua teardown / bring-up (ELF-launch fragmentation fix) ──────────────────
// Heavy ELF modules (Doom, PICO-8) need a large CONTIGUOUS PSRAM block, which a
// prior Map+meshprint session fragments by churning small Lua objects through
// the shared heap. Lua never executes while an ELF runs, so we fully shut Lua
// down on launch: lua_close() frees every Lua object back to the heap, the holes
// coalesce, and the ELF loader gets a clean block. Lua + the launcher are
// recreated on ELF exit. See the plan in lua_arena_plan / meshprint_strt_frag.

// Tear the whole Lua world down. Safe preconditions (audited):
//  - The sole C->Lua bridge, drain_rx_events(), is guarded by `!L`; the mesh
//    task (Core 1) only enqueues RxEvents and never calls Lua.
//  - Message persistence is C-side (appendDM/ChannelMessage), so traffic during
//    teardown is saved to disk and reloaded by messages:loadPersisted() later.
//  - lua_close() runs luavgl __gc -> lv_obj_del on the whole widget tree, so
//    LVGL MUST stay initialized here. We do NOT touch LVGL core or buf1/buf2.
//    luavgl's group gc was patched to spare the C-owned default group.

// sound_mark() taken in setup() right after notify_init(): ids below it are
// C-owned residents (the notify melody); ids at/above it were created via the
// Lua bindings and are swept here when Lua dies.
static int s_boot_sound_mark = 0;

void luaTearDown() {
  if (!L) return;
  lua_State *dead = L;
  L = NULL;                                   // drain_rx_events() now bails
  if (the_mesh) the_mesh->lua_runtime = NULL; // drop the stale-state handle
  lua_close(dead);                            // GCs luavgl widgets -> lv_obj_del
  // Free the non-Lua global caches that survive lua_close and otherwise leave a
  // persistent mid-heap cluster capping the largest contiguous block:
  //  - emoji glyph cache: per-glyph PSRAM pixel+descriptor allocs, never freed —
  //    CONFIRMED as the ~33KB wall splitting PSRAM after a Map session (the Map's
  //    emoji contact names decode a burst of glyphs). This is the actual fix.
  //  - LVGL image/draw-buf cache: belt-and-suspenders (decoded icons/markers).
  // Both re-populate on demand when the launcher re-renders. Core-0 only.
  emoji_font_cache_clear();
  lv_image_cache_drop(NULL);
  // Sweep every Lua-created sound object: the handles died with lua_close, and
  // the heavy module wants the contiguous PSRAM their PCM renders occupy. The
  // notify melody sits below the boot mark and survives (alerts during Doom).
  sound_sweep(s_boot_sound_mark, 0);
  lua_arena_destroy();   // free the now-empty Lua arena -> coalesces up for the ELF
}

// Recreate Lua + the launcher (boot path and post-ELF-exit path are identical).
// lua_arena_create() MUST run before setupLuaVGL() (which calls lua_newstate ->
// lua_psram_alloc); setupLuaVGL() then builds the state, registers every binding,
// installs the require searcher, and loads the launcher main.lua at its tail.
void luaBringUp() {
  lua_arena_create();
  setupLuaVGL();
}

volatile bool lora_packet_ready = false;

// Route mbedTLS's heap allocations (the ~32-48 KB of TLS record/handshake/X.509
// buffers per HTTPS session) to PSRAM. The Arduino framework is built with
// CONFIG_MBEDTLS_INTERNAL_MEM_ALLOC, so by default they land in internal SRAM —
// which, alongside resident BLE + WiFi, leaves no room for the handshake's
// hardware-SHA DMA buffer ("esp-sha: Failed to allocate buf memory"), blocking
// all HTTPS map-tile downloads. MBEDTLS_PLATFORM_MEMORY is defined in the
// framework's mbedtls config, so we override the allocator at runtime — the
// equivalent of CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC without a framework rebuild.
// The small hardware SHA/AES DMA buffers are allocated separately by the
// esp_sha/esp_aes drivers and stay internal; moving the big buffers out is what
// frees the internal headroom those DMA allocs need.
static void *mbedtls_psram_calloc(size_t n, size_t size) {
  return heap_caps_calloc(n, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

// Boot-time PSRAM/internal accounting. Prints free + largest-contiguous after each
// init stage so the baseline consumption can be attributed to specific subsystems:
// the DROP in `free` between two consecutive lines is that stage's cost. `lua=` is
// the Lua heap (lua_gc COUNT, PSRAM-routed) — the prime unknown; 0 until the Lua
// state exists, then it jumps when createUI() loads the launcher + libraries.
static void log_boot_mem(const char* stage) {
  unsigned lua_kb = L ? (unsigned)lua_gc(L, LUA_GCCOUNT, 0) : 0;
  SLog.printf("[boot][mem] %-20s psram free=%uKB largest=%uKB | int free=%uKB | lua=%uKB\n",
              stage,
              (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024),
              (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) / 1024),
              (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
              lua_kb);
}

void setup() {
  // Enlarge the UART TX buffer so the ISR drains it in the background and SLog's
  // best-effort writes (availableForWrite-gated) almost never have to drop. Must
  // precede begin(). ~4 KB ≈ 350 ms of backlog at 115200 before any line drops.
  Serial.setTxBufferSize(4096);
  Serial.begin(115200);
  SLog.println("Delaying for 50ms...");
  delay(50);

  SLog.println("MeshPunk LuaVGL Demo");

  // Push all mbedTLS allocations to PSRAM before any subsystem can open a TLS
  // session (BLE/WiFi come up later in setup). Frees the internal SRAM the HTTPS
  // tile handshake's hardware-SHA DMA buffer needs. See mbedtls_psram_calloc.
  mbedtls_platform_set_calloc_free(mbedtls_psram_calloc, heap_caps_free);

  // Create SPI/mesh mutexes and cross-core queues before any subsystem
  // that relies on them. Safe to call before LVGL/TFT init because the
  // macros no-op when the handle is null (not needed — this runs first —
  // but defensive).
  meshpunk_sync_init();

  // Allocate the map tile worker's scratch buffers HERE, at boot, while the PSRAM
  // heap is clean — so they land at stable low addresses instead of dropping into
  // a mid-heap hole on first tile use and fragmenting the heap. They are never
  // freed (Core-1 worker lifetime). The lazy `if (!s_x)` checks at the use sites
  // stay as a fallback in case a boot alloc returns null.
  s_dl_buf       = (uint8_t  *)heap_caps_malloc(DL_BUF_SIZE,       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  s_rgb565_buf   = (uint16_t *)heap_caps_malloc(RGB565_BUF_SIZE,   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  s_tile_png_buf = (uint8_t  *)heap_caps_malloc(TILE_PNG_BUF_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  SLog.printf("[boot] tile scratch: dl=%p rgb565=%p png=%p\n",
              (void*)s_dl_buf, (void*)s_rgb565_buf, (void*)s_tile_png_buf);
  // PSRAM ceiling at boot (before LVGL/Lua/mesh init consume it). total tells us
  // how much we actually have to work with; everything below is carved from this.
  SLog.printf("[boot][mem] psram total=%uKB free=%uKB largest=%uKB | int free=%uKB largest=%uKB\n",
              (unsigned)(heap_caps_get_total_size(MALLOC_CAP_SPIRAM) / 1024),
              (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024),
              (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) / 1024),
              (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
              (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024));

  // Connect trackball / home button
  pinMode(TDECK_TRACKBALL_CLICK, INPUT_PULLUP);
  attachInterrupt(TDECK_TRACKBALL_CLICK, ISR_click, FALLING);

  // The board peripheral power control pin needs to be set to HIGH when using
  // the peripheral
  pinMode(BOARD_POWERON, OUTPUT);
  digitalWrite(BOARD_POWERON, HIGH);

  // Kick off one-shot GPS time sync; gps_sync_poll() runs it to completion in loop().
  gps_sync_begin();

  // Set CS on all SPI buses to high level during initialization
  pinMode(BOARD_SDCARD_CS, OUTPUT);
  pinMode(RADIO_CS_PIN, OUTPUT);
  pinMode(BOARD_TFT_CS, OUTPUT);

  digitalWrite(BOARD_SDCARD_CS, HIGH);
  digitalWrite(RADIO_CS_PIN, HIGH);
  digitalWrite(BOARD_TFT_CS, HIGH);

  pinMode(BOARD_SPI_MISO, INPUT_PULLUP);
  SPI.begin(BOARD_SPI_SCK, BOARD_SPI_MISO, BOARD_SPI_MOSI); // SD

  pinMode(BOARD_BOOT_PIN, INPUT_PULLUP);
  pinMode(BOARD_TBOX_G02, INPUT_PULLUP);
  pinMode(BOARD_TBOX_G01, INPUT_PULLUP);
  pinMode(BOARD_TBOX_G04, INPUT_PULLUP);
  pinMode(BOARD_TBOX_G03, INPUT_PULLUP);

  // Attach trackball direction interrupts
  attachInterrupt(TDECK_TRACKBALL_UP,    ISR_trackball_up,    FALLING);
  attachInterrupt(TDECK_TRACKBALL_DOWN,  ISR_trackball_down,  FALLING);
  attachInterrupt(TDECK_TRACKBALL_LEFT,  ISR_trackball_left,  FALLING);
  attachInterrupt(TDECK_TRACKBALL_RIGHT, ISR_trackball_right, FALLING);

  SLog.println("Initializing display");

  // Initialize filesystem. Normal builds use the partition labeled "spiffs"
  // (the begin() default); installs via bmorcelli's Launcher create the data
  // partition labeled "assets" (its exact-size install path), so fall back.
#ifdef MESHPUNK_EMBED_PACK
  ensure_data_partition();  // reboots if it had to create one
#endif
  bool fs_on_spiffs = LittleFS.begin(true);
  if (!fs_on_spiffs) SLog.println("LittleFS: no \"spiffs\" partition, trying \"assets\" (Launcher install)");
  if (fs_on_spiffs || LittleFS.begin(true, "/littlefs", 10, "assets")) {
    fs_mounted = true;
    SLog.printf("LittleFS mounted successfully (label: %s)\n", fs_on_spiffs ? "spiffs" : "assets");
#ifdef MESHPUNK_EMBED_PACK
    // Fresh/wiped filesystem or a firmware update with new bundled files:
    // populate it from the pack embedded in this binary.
    if (pack_needs_extract()) extract_data_pack();
#endif

    SLog.println("LittleFS contents:");
    listDir(LittleFS, "/lua");

    // Load firmware preferences (tz, use_sd, clock_fmt)
    firmware_prefs_load();
    // Alt emoji layer per-key map (defaults + /emoji_keymap overrides)
    kb_emoji_map_load();
    // Push the selection-highlight preferences into the live theme (the theme is
    // already inited; Lua applies the palette later and re-reads these).
    lv_theme_meshpunk_set_focus_solid(theme_focus_solid);
    lv_theme_meshpunk_set_focus_darken(theme_focus_darken);
  } else {
    SLog.println("Error mounting LittleFS!!");
  }

  // Initialize SD card for persistent mesh data (survives LittleFS reflash)
  SLog.println("===== SD CARD INIT =====");

  // The SPI bus is shared with the TFT (80 MHz) and SX1262, but every device
  // sets its own per-transaction SPISettings, so this clock only applies to SD
  // transfers. 40 MHz cuts a 131KB map-tile read from ~400ms (4 MHz Arduino
  // default) to ~50ms. Probe descending; 4 MHz floor = old behavior.
  static const uint32_t sd_freqs[] = {40000000U, 25000000U, 4000000U};
  for (uint32_t freq : sd_freqs) {
    if (SD.begin(BOARD_SDCARD_CS, SPI, freq)) {
      sd_mounted = true;
      SLog.printf("[SD] Mounted at %lu Hz\n", (unsigned long)freq);
      break;
    }
    SD.end();
    SLog.printf("[SD] Mount failed at %lu Hz\n", (unsigned long)freq);
  }

  if (sd_mounted) {
    uint64_t cardSize = SD.cardSize() / (1024 * 1024);
    SLog.printf("[SD] Card mounted, size: %llu MB\n", cardSize);

    const char* required_dirs[] = {
      "/meshpunk",
      "/meshpunk/apps",
      "/meshpunk/messages",
    };
    for (auto dir : required_dirs) {
      if (!SD.exists(dir)) {
        SD.mkdir(dir);
        SLog.printf("[SD] Created %s\n", dir);
      }
    }

    if (!LittleFS.exists("/firmware_prefs") && SD.exists("/meshpunk/firmware_prefs")) {
      sd_spi_take();
      bool ok = copyFile(SD, "/meshpunk/firmware_prefs", LittleFS, "/firmware_prefs");
      sd_spi_release();
      if (ok) {
        SLog.println("[FW_PREFS] Imported from SD after reflash");
        firmware_prefs_load();
      } else {
        SLog.println("[FW_PREFS] SD import failed, using defaults");
      }
    }

    if (!LittleFS.exists("/wifi_creds") && SD.exists("/meshpunk/wifi_creds")) {
      sd_spi_take();
      bool ok = copyFile(SD, "/meshpunk/wifi_creds", LittleFS, "/wifi_creds");
      sd_spi_release();
      SLog.printf("[WIFI_CREDS] %s from SD after reflash\n", ok ? "Imported" : "Import FAILED");
    }
  } else {
    SLog.println("[SD] Card mount FAILED");
  }

  // Allocate PunkMesh in PSRAM — frees ~115 KB of internal SRAM for BLE stack.
  void* mesh_mem = heap_caps_malloc(sizeof(PunkMesh), MALLOC_CAP_SPIRAM);
  the_mesh = new (mesh_mem) PunkMesh(radio_driver, fast_rng, *new VolatileRTCClock(), tables);
  SLog.printf("[MESH] PunkMesh allocated in PSRAM (%u bytes)\n", sizeof(PunkMesh));
  log_boot_mem("after mesh alloc");

#if BLE_COMPANION_ENABLED
  if (ble_enabled_pref) {
    ble_companion_init_early();
  } else {
    SLog.println("[BLE] Disabled by user preference");
  }
#endif
  log_boot_mem("after BLE early");

  // Decide which storage to use (use_sd_pref loaded by firmware_prefs_load)
  if (sd_mounted && use_sd_pref) {
    the_mesh->setStorage(&SD, "/meshpunk");
    SLog.println("[SD] Mesh storage: SD:/meshpunk/");
  } else {
    the_mesh->setStorage(&LittleFS, "");
    if (sd_mounted) {
      SLog.println("[SD] SD available but user chose LittleFS");
    } else {
      SLog.println("[SD] Using LittleFS (no SD card)");
    }
  }
  the_mesh->_msg_retain_days = msg_retain_days;  // routing/message retention window

  wifi_creds_load();
  if (wifi_enabled_pref) {
    WiFi.mode(WIFI_STA);
    if (wifi_saved_ssid.length() > 0) {
      WiFi.begin(wifi_saved_ssid.c_str(), wifi_saved_pass.c_str());
      SLog.printf("WiFi auto-connecting to: %s\n", wifi_saved_ssid.c_str());
    } else {
      SLog.println("WiFi initialized in station mode (no saved network)");
    }
  } else {
    WiFi.mode(WIFI_OFF);
    SLog.println("WiFi disabled by preference");
  }
  log_boot_mem("after wifi");

  // Set touch int input
  pinMode(BOARD_TOUCH_INT, INPUT);
  delay(20);

  SLog.println("Initializing GT911 touch sensor");

  Wire.begin(BOARD_I2C_SDA, BOARD_I2C_SCL);


  touch.setPins(-1, BOARD_TOUCH_INT);
  if (!touch.begin(Wire, GT911_SLAVE_ADDRESS_L)) {
    while (1) {
      SLog.println("Failed to find GT911 - check your wiring!");
      delay(1000);
    }
  }

  // Set touch max xy
  touch.setMaxCoordinates(320, 240);

  // Set swap xy
  touch.setSwapXY(true);

  // Set mirror xy
  touch.setMirrorXY(false, true);
  touch.setInterruptMode(LOW_LEVEL_QUERY);

  // Initialize keyboard
  Wire.beginTransmission(LILYGO_KB_SLAVE_ADDRESS);
  if (Wire.endTransmission() == 0) {
    keyboard_available = true;
    SLog.println("T-Deck keyboard found!");

    // Set initial keyboard brightness
    setKeyboardDefaultBrightness(127);
    setKeyboardBrightness(kbd_brightness);

    // Switch keyboard to raw matrix mode for hold detection
    Wire.beginTransmission(LILYGO_KB_SLAVE_ADDRESS);
    Wire.write(LILYGO_KB_MODE_RAW_CMD);
    Wire.endTransmission();
    SLog.println("Keyboard switched to raw matrix mode");
  } else {
    SLog.println("T-Deck keyboard not found!");
  }

  // Initialize I2S audio output on T-Deck speaker
  audio = new Audio();
  audio->setPinout(TDECK_I2S_BCK, TDECK_I2S_WS, TDECK_I2S_DOUT);
  sound_init(audio, firmware_prefs_save);
  notify_init();   // pre-render the notification melody (C-owned, survives lua_close)
  // Boot watermark: every sound id below this is C-owned (the notify melody)
  // and survives every sweep; everything at/above it is Lua-created and gets
  // swept by luaTearDown on ELF launch (Lua handles die with lua_close anyway).
  s_boot_sound_mark = sound_mark();
  audio->setVolume(sound_get_muted() ? 0 : sound_get_volume());
  SLog.printf("[AUDIO] I2S init: vol=%d muted=%d\n", sound_get_volume(), sound_get_muted() ? 1 : 0);
  log_boot_mem("after audio");

  tft.fillScreen(TFT_GREEN);

  // Initialize LORA Radio
  SLog.println(F("===== RADIO INIT ====="));

  int16_t state = radio.begin();
  SLog.printf("[RADIO] begin() = %d %s\n", state, state == RADIOLIB_ERR_NONE ? "OK" : "FAILED");

  delay(100);

  float freq = the_mesh->getFreqPref();
  uint8_t tx_pwr = the_mesh->getTxPowerPref();
  float bw = the_mesh->getBandwidthPref();
  uint8_t sf = the_mesh->getSpreadingFactorPref();
  uint8_t cr = the_mesh->getCodingRatePref();
  SLog.printf("[RADIO] Setting freq=%.3f MHz, BW=%.0f kHz, SF=%d, CR=%d, TX=%d dBm\n", freq, bw, sf, cr, tx_pwr);

  state = radio.setFrequency(freq);
  SLog.printf("[RADIO] setFrequency = %d %s\n", state, state == RADIOLIB_ERR_NONE ? "OK" : "FAILED");

  state = radio.setBandwidth(bw);
  SLog.printf("[RADIO] setBandwidth = %d %s\n", state, state == RADIOLIB_ERR_NONE ? "OK" : "FAILED");

  state = radio.setSpreadingFactor(sf);
  SLog.printf("[RADIO] setSpreadingFactor = %d %s\n", state, state == RADIOLIB_ERR_NONE ? "OK" : "FAILED");

  state = radio.setCodingRate(cr);
  SLog.printf("[RADIO] setCodingRate = %d %s\n", state, state == RADIOLIB_ERR_NONE ? "OK" : "FAILED");

  radio.setCRC(true);

  state = radio.setOutputPower(tx_pwr);
  SLog.printf("[RADIO] setOutputPower = %d %s\n", state, state == RADIOLIB_ERR_NONE ? "OK" : "FAILED");

  state = radio.startReceive();
  SLog.printf("[RADIO] startReceive = %d %s\n", state, state == RADIOLIB_ERR_NONE ? "OK" : "FAILED");

  SLog.println(F("===== MESHCORE INIT ====="));
  fast_rng.begin(123456); // fixed seed for testing
  the_mesh->begin();
  the_mesh->showWelcome();

  // Seed the clock + own-position from the last saved GPS fix until live GPS
  // syncs (or the user manually sets the time). Storage is configured above and
  // gps_sync_begin() already ran, so this won't be clobbered by a sync restart.
  gps_last_load();

  // Flag a boot catch-up retention sweep; pruneStep runs it incrementally from
  // loop() once the clock is valid (seeded above, or after the first GPS fix).
  the_mesh->_prune_due = true;

  // Apply RX boost from prefs (loaded in the_mesh->begin())
  if (the_mesh->_prefs.rx_boost) {
    SPI_LOCK();
    radio_driver.setRxBoostedGainMode(true);
    SPI_UNLOCK();
    SLog.println("[RADIO] RX Boost restored from prefs: ON");
  }

  SLog.printf("[MESH] Node name: %s\n", the_mesh->_prefs.node_name);
  SLog.printf("[MESH] Freq pref: %.3f MHz\n", the_mesh->_prefs.freq);
  SLog.printf("[MESH] TX power pref: %d dBm\n", the_mesh->_prefs.tx_power_dbm);
  SLog.printf("[MESH] Contacts loaded: %d\n", the_mesh->getNumContacts());
  SLog.printf("[MESH] Public channel: %s\n", the_mesh->publicChannelIdx() >= 0 ? "YES" : "deleted");
  char pk_hex[PUB_KEY_SIZE * 2 + 1];
  mesh::Utils::toHex(pk_hex, the_mesh->self_id.pub_key, PUB_KEY_SIZE);
  SLog.printf("[MESH] Pub key: %s\n", pk_hex);
  log_boot_mem("after mesh begin");

  //Initialize the disply only after all other spi bus setup is finished
  SLog.println("Initialize display");
  tft.begin();
  tft.setRotation(1);
  tft.fillScreen(TFT_BLACK);

  // LVGL tick function
  lvgl_ticker.attach_ms(5, []() {
    lv_tick_inc(5); // Increment LVGL tick counter every 5ms
  });

  // Initialize LVGL
  setupLvgl();
  log_boot_mem("after setupLvgl");

  // Set LVGL screen to opaque dark background
  // Without this, LVGL objects are transparent and the raw TFT fill color shows through
  lv_obj_set_style_bg_color(lv_scr_act(), lv_color_make(0x10, 0x10, 0x10), 0);
  lv_obj_set_style_bg_opa(lv_scr_act(), LV_OPA_COVER, 0);

  SLog.println("===== LUA INIT =====");

  // Initialize LuaVGL. luaBringUp() creates the Lua PSRAM arena (+ offset gap) and
  // then setupLuaVGL() — same path used to recreate Lua after an ELF exits.
  luaBringUp();

  SLog.println("[LUA] luaBringUp() returned");
  log_boot_mem("after setupLuaVGL");

  // Create UI
  createUI();
  log_boot_mem("after createUI");


  // Adjust backlight
  pinMode(BOARD_BL_PIN, OUTPUT);
  setBrightness(display_brightness);
  last_activity_ms = millis();

  // Start BLE companion interface before spawning mesh task, since
  // ble_companion->loop() runs inside mesh_task_body on Core 1.
#if BLE_COMPANION_ENABLED
  if (ble_enabled_pref && ble_serial) {
    ble_companion_start(*the_mesh);
  }
#endif
  log_boot_mem("after BLE start");

  // Hand off mesh + radio to Core 1 now that the_mesh, Lua, LVGL, and the
  // RX queue are all up. Must happen AFTER createUI / setupLuaVGL so that
  // any RX events arriving from the mesh task have something to drain into.
  SLog.printf("[TASK] setup() running on core=%d; spawning mesh_task on Core 1\n",
                xPortGetCoreID());
  meshpunk_spawn_mesh_task();
  meshpunk_spawn_gps_task();
  log_boot_mem("setup done");
}

// Forward decls for the Lua dispatchers that live in punkmesh.cpp.
// These are called only from the UI core (Core 0) to preserve lua_State
// single-threadedness.
extern void lua_mesh_push_channel_message(lua_State* L, const char* sender_name, uint8_t hops, bool direct, uint32_t timestamp, const char *text, float snr, float rssi, int channel_idx, uint16_t path_len, const uint8_t* path, const uint8_t* pkt_hash);
extern void lua_mesh_push_direct_message(lua_State* L, const char* sender_name, uint8_t hops, bool direct, uint32_t timestamp, const char *text, float snr, float rssi, uint16_t path_len, const uint8_t* path, const uint8_t* pkt_hash);
extern void lua_mesh_push_contact_update(lua_State* L, const char* name, uint8_t contact_type);
extern void lua_mesh_push_ack(lua_State* L, uint32_t ack, int32_t rtt);
extern void lua_mesh_push_room_message(lua_State* L, const char* room_name, const char* author, uint8_t hops, bool direct, uint32_t timestamp, const char *text, float snr, float rssi, uint16_t path_len, const uint8_t* path, const uint8_t* pkt_hash);
extern void lua_mesh_push_cli_response(lua_State* L, const char* name, const char* text, uint32_t timestamp);
extern void lua_mesh_push_login_result(lua_State* L, const char* name, bool ok, uint8_t perms, uint32_t keepalive_secs);
extern void lua_mesh_push_status_text(lua_State* L, const char* name, const char* text);
extern void lua_mesh_push_send_retry(lua_State* L, uint32_t ack, uint8_t n, uint8_t total);
extern void lua_mesh_push_conn_lost(lua_State* L, const char* name);

// Drain RX events posted by the mesh core. Runs every UI tick.
// Bounded per call so a flood on the queue can't starve LVGL.
static void drain_rx_events() {
  if (!rx_event_queue || !L) return;
  RxEvent ev;
  int budget = 8; // cap messages per tick to keep UI responsive
  while (budget-- > 0 && xQueueReceive(rx_event_queue, &ev, 0) == pdTRUE) {
    if (ev.kind == RxEvent::DIRECT_MSG) {
      lua_mesh_push_direct_message(L, ev.sender, ev.hops, ev.direct,
                                   ev.timestamp, ev.text, ev.snr, ev.rssi,
                                   ev.path_len, ev.path, ev.pkt_hash);
    } else if (ev.kind == RxEvent::CHANNEL_MSG) {
      lua_mesh_push_channel_message(L, ev.sender, ev.hops, ev.direct,
                                    ev.timestamp, ev.text, ev.snr, ev.rssi,
                                    ev.channel_idx, ev.path_len, ev.path,
                                    ev.pkt_hash);
    } else if (ev.kind == RxEvent::CONTACT_UPDATE) {
      lua_mesh_push_contact_update(L, ev.sender, ev.hops);
    } else if (ev.kind == RxEvent::ACK) {
      lua_mesh_push_ack(L, ev.ack, ev.rtt);
    } else if (ev.kind == RxEvent::ROOM_MSG) {
      lua_mesh_push_room_message(L, ev.sender, ev.origin, ev.hops, ev.direct,
                                 ev.timestamp, ev.text, ev.snr, ev.rssi,
                                 ev.path_len, ev.path, ev.pkt_hash);
    } else if (ev.kind == RxEvent::CLI_RESPONSE) {
      lua_mesh_push_cli_response(L, ev.sender, ev.text, ev.timestamp);
    } else if (ev.kind == RxEvent::LOGIN_RESULT) {
      lua_mesh_push_login_result(L, ev.sender, ev.channel_idx != 0, ev.hops, ev.ack);
    } else if (ev.kind == RxEvent::STATUS_TEXT) {
      lua_mesh_push_status_text(L, ev.sender, ev.text);
    } else if (ev.kind == RxEvent::SEND_RETRY) {
      lua_mesh_push_send_retry(L, ev.ack, ev.hops, (uint8_t)ev.channel_idx);
    } else if (ev.kind == RxEvent::CONN_LOST) {
      lua_mesh_push_conn_lost(L, ev.sender);
    }
  }
}

void loop() {
  // Core 0 (UI domain) — LVGL + Lua + input. The mesh dispatcher runs on
  // Core 1 via mesh_task (see meshpunk_tasks.cpp).

  // Deferred ELF launch: a Lua game called _launch_elf (which only stashed the
  // request — Lua can't close itself from its own C stack). Tear Lua all the way
  // down so the fragmented Lua heap is freed and the module gets a clean
  // contiguous PSRAM block, run the module to completion (this blocks the loop),
  // then recreate Lua + the launcher. The user lands on the launcher home.
  if (elf_host_pending_take()) {
    luaTearDown();              // frees Lua + its arena (logs [lua_arena] freed)
    elf_host_run_pending();     // runs the module to completion (elf_host logs PSRAM)
    luaBringUp();               // recreate Lua + arena + launcher (logs [lua_arena])
    return;   // skip the rest of this tick; the fresh launcher runs next tick
  }

  // Handle LVGL tasks
  lv_timer_handler();

  // Audio: file streaming (SD needs SPI mutex)
  if (sound_file_is_sd()) {
    sd_spi_take();
    audio->loop();
    sd_spi_release();
  } else {
    audio->loop();
  }

  // GPS one-shot time sync runs on Core 1 (gps_task). Nothing to do here.

  // Flush mesh RX events into Lua. lua_State is single-threaded — always
  // touched from Core 0.
  drain_rx_events();

  // Incremental message/routing retention sweep (flagged on a new-day record or
  // at boot). One file per iteration; cheap no-op when nothing is due.
  if (the_mesh) the_mesh->pruneStep();

  // ── Inactivity timeouts ─────────────────────────────────────────────────
  if (screen_timeout_secs > 0 && !screen_timed_out) {
    if (millis() - last_activity_ms > (uint32_t)screen_timeout_secs * 1000UL) {
      setBrightness(0);
      screen_timed_out = true;
    }
  }
  if (kbd_timeout_secs > 0 && !kbd_timed_out) {
    if (millis() - last_activity_ms > (uint32_t)kbd_timeout_secs * 1000UL) {
      setKeyboardBrightness(0);
      kbd_timed_out = true;
    }
  }
}
