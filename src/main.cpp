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
#include <lvgl.h>
#include "theme/lv_theme_meshpunk.h"
#include "emoji_font.h"
#include "tdeck-pins.h"
#include "meshpunk_sync.h"
#include "Audio.h"

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

int luaL_loadfilex(lua_State *L, const char *filename, const char *mode) {
    File file = LittleFS.open(filename, "r");
    if (!file || file.isDirectory()) {
      lua_pushfstring(L, "cannot open %s", filename);
      return LUA_ERRFILE;
    }

    size_t size = file.size();
    char* buffer = (char*)malloc(size + 1);
    if (!buffer) {
      file.close();
      lua_pushliteral(L, "out of memory");
      return LUA_ERRMEM;
    }

    file.readBytes(buffer, size);
    buffer[size] = '\0';
    file.close();

    int status = luaL_loadbufferx(L, buffer, size, filename, mode);
    free(buffer);
    return status;
  }
}

// Radio
RADIO_CLASS radio = new Module(RADIO_CS_PIN, RADIO_DIO1_PIN, RADIO_RST_PIN, RADIO_BUSY_PIN);

// Meshcore
StdRNG fast_rng;
SimpleMeshTables tables;

ESP32Board board;
PunkSX1262Wrapper radio_driver(radio, board);
PunkMesh the_mesh(radio_driver, fast_rng, *new VolatileRTCClock(), tables); // TODO: test with 'rtc_clock' in target.cpp

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
static uint32_t gps_sats_at_fix = 0;
static uint32_t gps_hdop_at_fix = 0;        // HDOP * 100 (TinyGPSPlus integer representation)
static bool    tz_is_auto = true;
static int32_t tz_manual_minutes = 0;
static String  tz_setting_str = "auto";

// Firmware-level preferences (unified in /firmware_prefs)
static bool   use_sd_pref = true;
static String clock_fmt_str = "12";

// ── Audio / Sound ──────────────────────────────────────────────────────────
static Audio*    audio = nullptr;          // ESP32-audioI2S player, created in setup()
static uint8_t   sound_volume    = 10;    // 0–21 (ESP32-audioI2S native range)
static bool      sound_muted     = false;
static bool      active_file_is_sd = false; // true while streaming from SD card

// Sound object registry (dynamic, no fixed limit)
struct SoundObject {
    int       id;
    enum Type { TONE, AUDIO_FILE } type;

    // TONE fields
    int16_t*  pcm_buffer;     // stereo interleaved 16-bit PCM in PSRAM
    uint32_t  sample_count;   // total stereo frames
    uint32_t  play_pos;       // read cursor
    bool      tone_playing;
    bool      tone_paused;

    // FILE fields
    fs::File* file;
    bool      file_is_sd;
    bool      file_paused;
};

static SoundObject** sound_objects  = nullptr;
static int           sound_obj_count    = 0;
static int           sound_obj_capacity = 0;
static int           next_sound_id  = 1;

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
  return tz_is_auto ? tz_auto_offset_minutes() : tz_manual_minutes;
}

extern bool sd_mounted;
void sd_spi_release();

static void write_firmware_prefs(fs::FS& fs, const char* path) {
  File f = fs.open(path, "w", true);
  if (!f) { Serial.printf("[FW_PREFS] cannot write %s\n", path); return; }
  f.printf("use_sd=%d\n", use_sd_pref ? 1 : 0);
  f.printf("tz=%s\n", tz_setting_str.c_str());
  f.printf("clock_fmt=%s\n", clock_fmt_str.c_str());
  f.printf("sound_vol=%d\n",   sound_volume);
  f.printf("sound_muted=%d\n", sound_muted ? 1 : 0);
  f.close();
  Serial.printf("[FW_PREFS] saved to %s\n", path);
}

static void firmware_prefs_save() {
  write_firmware_prefs(LittleFS, "/firmware_prefs");
  if (sd_mounted && use_sd_pref) {
    sd_spi_take();
    write_firmware_prefs(SD, "/meshpunk/firmware_prefs");
    sd_spi_release();
  }
}

static void firmware_prefs_load() {
  File f = LittleFS.open("/firmware_prefs", "r");
  if (!f) {
    Serial.println("[FW_PREFS] no /firmware_prefs, using defaults");
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
    } else if (strcmp(key, "sound_vol") == 0) {
      int v = atoi(val);
      if (v >= 0 && v <= 21) sound_volume = (uint8_t)v;
    } else if (strcmp(key, "sound_muted") == 0) {
      sound_muted = (atoi(val) == 1);
    }
  }
  f.close();
  Serial.printf("[FW_PREFS] loaded: use_sd=%d tz=%s clock=%s\n",
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

static void gps_print_stats(const char* tag) {
  uint32_t elapsed = millis() - gps_sync_start_ms;
  uint32_t chars = gps_tinygps.charsProcessed();
  uint32_t delta = chars - gps_last_chars;
  gps_last_chars = chars;

  Serial.printf("[GPS %s] t=%lus chars=%lu(+%lu) sent_with_fix=%lu csum_ok=%lu csum_fail=%lu\n",
                tag,
                (unsigned long)(elapsed / 1000UL),
                (unsigned long)chars,
                (unsigned long)delta,
                (unsigned long)gps_tinygps.sentencesWithFix(),
                (unsigned long)gps_tinygps.passedChecksum(),
                (unsigned long)gps_tinygps.failedChecksum());

  // Satellites in view (from GSV/GGA)
  if (gps_tinygps.satellites.isValid()) {
    Serial.printf("[GPS %s]   sats=%lu (age=%lums)\n",
                  tag,
                  (unsigned long)gps_tinygps.satellites.value(),
                  (unsigned long)gps_tinygps.satellites.age());
  } else {
    Serial.printf("[GPS %s]   sats=--\n", tag);
  }

  // HDOP — lower is better; <5 is usable, <2 is good
  if (gps_tinygps.hdop.isValid()) {
    Serial.printf("[GPS %s]   hdop=%.2f\n", tag, gps_tinygps.hdop.hdop());
  }

  // Date (often appears before full position fix)
  if (gps_tinygps.date.isValid()) {
    Serial.printf("[GPS %s]   date=%04u-%02u-%02u (age=%lums)\n",
                  tag,
                  gps_tinygps.date.year(), gps_tinygps.date.month(), gps_tinygps.date.day(),
                  (unsigned long)gps_tinygps.date.age());
  } else {
    Serial.printf("[GPS %s]   date=INVALID\n", tag);
  }

  // Time
  if (gps_tinygps.time.isValid()) {
    Serial.printf("[GPS %s]   time=%02u:%02u:%02u (age=%lums)\n",
                  tag,
                  gps_tinygps.time.hour(), gps_tinygps.time.minute(), gps_tinygps.time.second(),
                  (unsigned long)gps_tinygps.time.age());
  } else {
    Serial.printf("[GPS %s]   time=INVALID\n", tag);
  }

  // Location (not required for time sync, but useful signal)
  if (gps_tinygps.location.isValid()) {
    Serial.printf("[GPS %s]   loc=%.5f,%.5f (age=%lums)\n",
                  tag,
                  gps_tinygps.location.lat(), gps_tinygps.location.lng(),
                  (unsigned long)gps_tinygps.location.age());
  } else {
    Serial.printf("[GPS %s]   loc=NO FIX YET\n", tag);
  }

  // Diagnostic hint
  if (delta == 0) {
    Serial.printf("[GPS %s]   !! no new bytes — check power/TX pin (expected RX=%d)\n",
                  tag, TDECK_GPS_RX);
  } else if (gps_tinygps.passedChecksum() == 0 && chars > 200 && gps_baud_locked) {
    Serial.printf("[GPS %s]   !! bytes flowing but 0 valid sentences at locked baud %u\n",
                  tag, (unsigned)GPS_BAUD_CANDIDATES[gps_baud_idx]);
  }
}

static void gps_start_probe_at_current_baud() {
  uint32_t baud = GPS_BAUD_CANDIDATES[gps_baud_idx];
  GPSSerial.end();
  GPSSerial.begin(baud, SERIAL_8N1, TDECK_GPS_RX, TDECK_GPS_TX);
  gps_baud_probe_start_ms = millis();
  gps_baud_probe_chars_start = gps_tinygps.charsProcessed();
  Serial.printf("[GPS] probing baud=%u (candidate %u/%u)\n",
                (unsigned)baud, (unsigned)(gps_baud_idx + 1), (unsigned)GPS_BAUD_COUNT);
}

static void gps_sync_begin() {
  Serial.printf("[GPS] Listening on UART1 RX=%d TX=%d\n", TDECK_GPS_RX, TDECK_GPS_TX);
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
    Serial.printf("[GPS] baud LOCKED at %u (csum_ok=%lu csum_fail=%lu)\n",
                  (unsigned)GPS_BAUD_CANDIDATES[gps_baud_idx],
                  (unsigned long)ok, (unsigned long)fail);
    gps_baud_locked = true;
    return true;
  }

  // Advance to next candidate after the probe window expires.
  if (now - gps_baud_probe_start_ms >= GPS_BAUD_PROBE_MS) {
    uint32_t delta = gps_tinygps.charsProcessed() - gps_baud_probe_chars_start;
    Serial.printf("[GPS] baud %u rejected: chars=+%lu csum_ok=%lu csum_fail=%lu\n",
                  (unsigned)GPS_BAUD_CANDIDATES[gps_baud_idx],
                  (unsigned long)delta, (unsigned long)ok, (unsigned long)fail);
    gps_baud_idx = (gps_baud_idx + 1) % GPS_BAUD_COUNT;
    gps_start_probe_at_current_baud();
  }
  return false;
}

bool gps_sync_is_done() { return gps_sync_done; }

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
        Serial.println("[GPS] post-fix window expired; no satellite count received.");
      }
      gps_print_stats("fix-final");
      GPSSerial.end();
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
    MESH_LOCK();
    the_mesh.getRTCClock()->setCurrentTime(utc.unixtime());
    MESH_UNLOCK();

    gps_fix_acquired_ms = now;
    gps_time_fix_valid = true;
    gps_sats_at_fix = gps_tinygps.satellites.isValid() ? gps_tinygps.satellites.value() : 0;
    gps_hdop_at_fix  = gps_tinygps.hdop.isValid()      ? gps_tinygps.hdop.value()       : 0;

    if (gps_tinygps.location.isValid()) {
      gps_lat_at_fix = gps_tinygps.location.lat();
      gps_lng_at_fix = gps_tinygps.location.lng();
      gps_location_valid_at_fix = true;
      Serial.printf("[TZ] captured lat=%.5f lng=%.5f -> auto offset=%d min\n",
                    gps_lat_at_fix, gps_lng_at_fix, (int)tz_auto_offset_minutes());
    } else {
      Serial.println("[TZ] no location at time-fix; auto-tz falls back to UTC");
    }

    Serial.println("[GPS] ======== FIX ACQUIRED — entering post-fix window ========");
    gps_print_stats("fix");
    Serial.printf("[GPS] RTC set to %u UTC (%04u-%02u-%02u %02u:%02u:%02u) after %lus\n",
                  (unsigned)utc.unixtime(),
                  gps_tinygps.date.year(), gps_tinygps.date.month(), gps_tinygps.date.day(),
                  gps_tinygps.time.hour(), gps_tinygps.time.minute(), gps_tinygps.time.second(),
                  (unsigned long)((now - gps_sync_start_ms) / 1000UL));
    return;
  }

  if (now - gps_sync_start_ms > GPS_SYNC_TIMEOUT_MS) {
    Serial.println("[GPS] ======== TIMEOUT ========");
    gps_print_stats("timeout");
    Serial.printf("[GPS] No fix after %us. Move to open sky for cold start (can take 30s-5min+).\n",
                  (unsigned)(GPS_SYNC_TIMEOUT_MS / 1000));
    GPSSerial.end();
    gps_sync_done = true;
  }
}

void gps_sync_restart() {
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
  Serial.printf("[GPS] Restarting sync (auto-baud, timeout=%us)\n",
                (unsigned)(GPS_SYNC_TIMEOUT_MS / 1000));
  gps_start_probe_at_current_baud();
}

// Trackball click — fed into LVGL as LV_KEY_ENTER via keyboard_read_cb
volatile int trackball_click = 0;

void IRAM_ATTR ISR_click() {
  trackball_click++;
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
    Serial.print("Failed to open directory: ");
    Serial.println(dirname);
    return;
  }

  File file = root.openNextFile();
  while (file) {
    for (int i = 0; i < level; i++) Serial.print("  ");
    Serial.print(dirname);
    Serial.print("/");
    Serial.print(file.name());
    Serial.print(":");
    Serial.print(file.size());
    Serial.println("b");

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
    Serial.println("Filesystem not mounted!");
    return "";
  }

  fs::File file = LittleFS.open(filename, "r");
  if (!file) {
    Serial.print("Failed to open file: ");
    Serial.println(filename);
    return "";
  }

  String content = "";
  while (file.available()) {
    content += (char)file.read();
  }
  file.close();

  return content;
}

// -- Replaced by safe_open version that parses L:/S: prefix
// static int lua_io_open(lua_State *L) {
//   const char *filename = luaL_checkstring(L, 1);
//   const char *mode = luaL_optstring(L, 2, "r");
//
//   Serial.print("io.open: ");
//   Serial.print(filename);
//   Serial.print(" mode: ");
//   Serial.println(mode);
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

  bool use_sd = false;
  const char *actual_path = filename;

  // Parse drive letter prefix
  if (filename[0] != '\0' && filename[1] == ':') {
    if (filename[0] == 'S' || filename[0] == 's') {
      use_sd = true;
      actual_path = filename + 2;
    } else if (filename[0] == 'L' || filename[0] == 'l') {
      use_sd = false;
      actual_path = filename + 2;
    }
  }

  Serial.print("io.open: ");
  Serial.print(filename);
  Serial.print(" -> ");
  Serial.print(use_sd ? "SD" : "LittleFS");
  Serial.print(":");
  Serial.print(actual_path);
  Serial.print(" mode: ");
  Serial.println(mode);

  const char *fs_mode;
  if (strcmp(mode, "r") == 0) {
    fs_mode = "r";
  } else if (strcmp(mode, "w") == 0) {
    fs_mode = "w";
  // Added append mode support
  } else if (strcmp(mode, "a") == 0) {
    fs_mode = "a";
  } else {
    lua_pushnil(L);
    lua_pushstring(L, "Only 'r', 'w', and 'a' modes supported");
    return 2;
  }

  if (use_sd) {
    if (!sd_mounted) {
      lua_pushnil(L);
      lua_pushstring(L, "SD card not mounted");
      return 2;
    }

    sd_spi_take();
    fs::File f = SD.open(actual_path, fs_mode);
    sd_spi_release();
    if (!f) {
      lua_pushnil(L);
      lua_pushstring(L, "Failed to open file on SD");
      return 2;
    }

    fs::File *file = new fs::File(f);
    LuaFileHandle *ud = (LuaFileHandle *)lua_newuserdata(L, sizeof(LuaFileHandle));
    ud->file = file;
    ud->is_sd = true;
  } else {
    fs::File f = LittleFS.open(actual_path, fs_mode);
    if (!f) {
      lua_pushnil(L);
      lua_pushstring(L, "Failed to open file");
      return 2;
    }

    fs::File *file = new fs::File(f);
    LuaFileHandle *ud = (LuaFileHandle *)lua_newuserdata(L, sizeof(LuaFileHandle));
    ud->file = file;
    ud->is_sd = false;
  }

  luaL_getmetatable(L, "esp32_file");
  lua_setmetatable(L, -2);
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

// Keyboard state tracking variables
static uint32_t last_key_code = 0;
static bool key_is_new = false;
static uint32_t last_key_time = 0;

// Navigation controller state
static lv_obj_t *nav_container = NULL;
static lv_gridnav_ctrl_t nav_flags = LV_GRIDNAV_CTRL_NONE;
static bool nav_gridnav_active = false;

static void nav_delete_cb(lv_event_t *e) {
    if (lv_event_get_target(e) == nav_container) {
        nav_container = NULL;
        nav_gridnav_active = false;
    }
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

// LVGL keyboard read callback
static void keyboard_read_cb(lv_indev_t *indev, lv_indev_data_t *data) {
  static bool was_pressed = false;
  uint32_t current_time = millis();

  // Read key from keyboard
  char keyValue = 0;
  Wire.requestFrom(LILYGO_KB_SLAVE_ADDRESS, 1);
  if (Wire.available() > 0) {
    keyValue = Wire.read();

    if (keyValue != 0) {
      // Check if this is a new key press or key has been held long enough for
      // repeat
      if (!was_pressed || (last_key_code != keyValue) ||
          (current_time - last_key_time > 30)) {

        last_key_code = keyValue;
        last_key_time = current_time;
        key_is_new = true;
        was_pressed = true;
      }
    } else {
      was_pressed = false;
    }
  }

  // Check trackball directions and click if no keyboard key is pending
  bool key_from_trackball = false;
  if (!key_is_new) {
    if (trackball_click > 0) {
      trackball_click--;
      last_key_code = LV_KEY_ENTER;
      key_is_new = true;
      key_from_trackball = true;
    } else if (trackball_up > 0) {
      trackball_up--;
      last_key_code = LV_KEY_UP;
      key_is_new = true;
      key_from_trackball = true;
    } else if (trackball_down > 0) {
      trackball_down--;
      last_key_code = LV_KEY_DOWN;
      key_is_new = true;
      key_from_trackball = true;
    } else if (trackball_left > 0) {
      trackball_left--;
      last_key_code = LV_KEY_LEFT;
      key_is_new = true;
      key_from_trackball = true;
    } else if (trackball_right > 0) {
      trackball_right--;
      last_key_code = LV_KEY_RIGHT;
      key_is_new = true;
      key_from_trackball = true;
    }
  }

  // Re-enable gridnav only on trackball input, not keyboard typing
  if (key_from_trackball && nav_container && !nav_gridnav_active) {
    uint32_t cnt = lv_obj_get_child_count(nav_container);
    for (uint32_t i = 0; i < cnt; i++) {
      lv_obj_remove_state(lv_obj_get_child(nav_container, i),
                          LV_STATE_FOCUSED | LV_STATE_FOCUS_KEY | LV_STATE_EDITED);
    }
    lv_group_focus_obj(nav_container);
    lv_gridnav_add(nav_container, nav_flags);
    nav_gridnav_active = true;
    lv_obj_t *vis = nav_find_visible_child(nav_container);
    if (vis) {
      lv_gridnav_set_focused(nav_container, vis, LV_ANIM_OFF);
    }
  }

  // Report key press to LVGL
  if (key_is_new) {
    data->state = LV_INDEV_STATE_PRESSED;
    key_is_new = false;

    // Map special keys
    if (last_key_code == 13) { // Enter
      data->key = LV_KEY_ENTER;
    } else if (last_key_code == 27) { // Escape
      data->key = LV_KEY_ESC;
    } else if (last_key_code == 8) { // Backspace
      data->key = LV_KEY_BACKSPACE;
    } else if (last_key_code == 9) { // Tab
      data->key = LV_KEY_NEXT;
    } else {
      data->key = last_key_code;
    }
  } else {
    data->state = LV_INDEV_STATE_RELEASED;
  }
}

// Fixed touchpad_read_cb (commented out due to ghost touch issues - see plan for details)
// static int16_t last_touch_x = 0, last_touch_y = 0;
// static uint8_t release_count = 0;
// static const uint8_t RELEASE_DEBOUNCE = 2;
//
// static void touchpad_read_cb(lv_indev_t *indev, lv_indev_data_t *data) {
//   if (touch.isPressed()) {
//     data->state = LV_INDEV_STATE_PRESSED;
//     release_count = 0;
//
//     uint8_t touched = touch.getPoint(x, y, touch.getSupportTouchPoint());
//     if (touched > 0) {
//       last_touch_x = x[0];
//       last_touch_y = y[0];
//     }
//     data->point.x = last_touch_x;
//     data->point.y = last_touch_y;
//   } else {
//     release_count++;
//     if (release_count >= RELEASE_DEBOUNCE) {
//       data->state = LV_INDEV_STATE_RELEASED;
//     } else {
//       data->state = LV_INDEV_STATE_PRESSED;
//       data->point.x = last_touch_x;
//       data->point.y = last_touch_y;
//     }
//   }
// }

static void touchpad_read_cb(lv_indev_t *indev, lv_indev_data_t *data) {
  data->state = LV_INDEV_STATE_RELEASED;

  if (touch.isPressed()) {
    uint8_t touched = touch.getPoint(x, y, touch.getSupportTouchPoint());
    if (touched > 0) {
      data->state = LV_INDEV_STATE_PRESSED;
      data->point.x = x[0];
      data->point.y = y[0];

      if (nav_container && nav_gridnav_active) {
        uint32_t cnt = lv_obj_get_child_count(nav_container);
        for (uint32_t i = 0; i < cnt; i++) {
          lv_obj_remove_state(lv_obj_get_child(nav_container, i),
                              LV_STATE_FOCUSED | LV_STATE_FOCUS_KEY);
        }
        lv_gridnav_remove(nav_container);
        nav_gridnav_active = false;
      }
    }
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
        Serial.println("ERR: Cannot open file");
        return;
      }

      while (f.available()) {
        Serial.write(f.read());
      }
      f.close();
      Serial.println(); // newline after file content
      Serial.println("OK");
    }

    else if (cmd.startsWith("WRITE ")) {
      String path = cmd.substring(6);
      File f = LittleFS.open(path, "w");
      if (!f) {
        Serial.println("ERR: Cannot open file for writing");
        return;
      }

      while (!Serial.available()); // wait for next line (start of file content)
      String content = Serial.readStringUntil(0x1A); // end with CTRL+Z (ASCII 26)
      f.print(content);
      f.close();
      Serial.println("OK");
    }

    else if (cmd.startsWith("LS")) {
      File root = LittleFS.open("/lua");
      File file = root.openNextFile();
      while (file) {
        Serial.println(file.name());
        file = root.openNextFile();
      }
      Serial.println("OK");
    }

    else if (cmd == "REBOOT") {
      Serial.println("REBOOTING...");
      ESP.restart();
    }

    else {
      Serial.println("ERR: Unknown command");
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
  //  Serial.println("Memory allocation failed!");
  //  delay(5000);
  //  assert(buf);
  //}

#define BUF_LINES 48
#define BUF_SIZE (TFT_HEIGHT * BUF_LINES * sizeof(lv_color_t))

  static uint8_t *buf1 = (uint8_t *)ps_malloc(BUF_SIZE);
  static uint8_t *buf2 = (uint8_t *)ps_malloc(BUF_SIZE);
  if (!buf1 || !buf2) {
    Serial.println("LVGL buffer allocation failed!");
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

    Serial.println("Keyboard input device registered with LVGL");
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

  Serial.print("Connecting to WiFi: ");
  Serial.println(network);

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

// ── Mesh bridge: Lua → C++ ──────────────────────────────────────

// Send a public/group channel message from Lua
// Usage from Lua: _mesh_send_public("Hello mesh!")
static int lua_mesh_send_public(lua_State *L) {
  const char *raw = luaL_checkstring(L, 1);
  // Normalize smart quotes so both the wire message and the local echo
  // render cleanly on receivers whose base font lacks U+2018-U+201D.
  char text[160];
  normalize_smart_quotes(raw, text, sizeof(text));

  Serial.printf("[MESH TX] lua_mesh_send_public called, text=\"%s\"\n", text);

  MESH_LOCK();
  if (!the_mesh._public) {
    MESH_UNLOCK();
    Serial.println("[MESH TX] ERROR: No public channel configured!");
    lua_pushboolean(L, 0);
    lua_pushstring(L, "No public channel configured");
    return 2;
  }

  uint32_t timestamp = the_mesh.getRTCClock()->getCurrentTime();
  Serial.printf("[MESH TX] Calling sendGroupMessage, timestamp=%u, sender=%s, text_len=%d\n",
    timestamp, the_mesh._prefs.node_name, (int)strlen(text));

  bool ok = the_mesh.sendGroupMessage(
    timestamp,
    the_mesh._public->channel,
    the_mesh._prefs.node_name,
    text,
    strlen(text)
  );

  Serial.printf("[MESH TX] sendGroupMessage returned %s\n", ok ? "true" : "false");

  if (ok) {
    // Persist local echo — Public is always channel slot 0
    the_mesh.appendChannelMessage(0, the_mesh._prefs.node_name, text, timestamp,
                                  0.0f, 0.0f, 0, false);
  }
  MESH_UNLOCK();

  lua_pushboolean(L, ok ? 1 : 0);
  return 1;
}

// Send a direct message to a contact by name prefix
// Usage from Lua: _mesh_send_direct("alice", "Hey!")
static int lua_mesh_send_direct(lua_State *L) {
  const char *name_prefix = luaL_checkstring(L, 1);
  const char *raw = luaL_checkstring(L, 2);
  char text[160];
  normalize_smart_quotes(raw, text, sizeof(text));

  MESH_LOCK();
  ContactInfo *recipient = the_mesh.searchContactsByPrefix(name_prefix);
  if (!recipient) {
    MESH_UNLOCK();
    lua_pushboolean(L, 0);
    lua_pushstring(L, "Contact not found");
    return 2;
  }

  uint32_t expected_ack = 0;
  uint32_t est_timeout = 0;
  uint32_t timestamp = the_mesh.getRTCClock()->getCurrentTime();

  int result = the_mesh.sendMessage(
    *recipient, timestamp, 0, text, expected_ack, est_timeout
  );

  if (result == MSG_SEND_FAILED) {
    MESH_UNLOCK();
    lua_pushboolean(L, 0);
    lua_pushstring(L, "Send failed");
    return 2;
  }

  // Persist local echo — peer is the recipient, from is us.
  the_mesh.appendDMMessage(recipient->name, the_mesh._prefs.node_name, text,
                           timestamp, 0.0f, 0.0f, 0,
                           result == MSG_SEND_SENT_DIRECT);
  MESH_UNLOCK();

  lua_pushboolean(L, 1);
  lua_pushstring(L, result == MSG_SEND_SENT_DIRECT ? "direct" : "flood");
  return 2;
}

// Get this node's info (name, pubkey hex, freq, tx power)
// Usage from Lua: local info = _mesh_get_node_info()
static int lua_mesh_get_node_info(lua_State *L) {
  lua_newtable(L);

  MESH_LOCK();
  lua_pushstring(L, the_mesh._prefs.node_name);
  lua_setfield(L, -2, "name");

  // Public key as hex string
  char hex[PUB_KEY_SIZE * 2 + 1];
  mesh::Utils::toHex(hex, the_mesh.self_id.pub_key, PUB_KEY_SIZE);
  lua_pushstring(L, hex);
  lua_setfield(L, -2, "pubkey");

  lua_pushnumber(L, the_mesh._prefs.freq);
  lua_setfield(L, -2, "freq");

  lua_pushinteger(L, the_mesh._prefs.tx_power_dbm);
  lua_setfield(L, -2, "tx_power");

  lua_pushnumber(L, the_mesh._prefs.node_lat);
  lua_setfield(L, -2, "lat");

  lua_pushnumber(L, the_mesh._prefs.node_lon);
  lua_setfield(L, -2, "lon");

  lua_pushnumber(L, the_mesh._prefs.bandwidth);
  lua_setfield(L, -2, "bandwidth");

  lua_pushinteger(L, the_mesh._prefs.spreading_factor);
  lua_setfield(L, -2, "spreading_factor");

  lua_pushinteger(L, the_mesh._prefs.coding_rate);
  lua_setfield(L, -2, "coding_rate");

  lua_pushboolean(L, the_mesh._prefs.contact_overwrite != 0);
  lua_setfield(L, -2, "contact_overwrite");
  MESH_UNLOCK();

  return 1;
}

// Get contact list
// Usage from Lua: local contacts = _mesh_get_contacts()
static int lua_mesh_get_contacts(lua_State *L) {
  lua_newtable(L);

  MESH_LOCK();
  ContactInfo c;
  int idx = 1;
  for (int i = 0; i < the_mesh.getNumContacts(); i++) {
    if (the_mesh.getContactByIdx(i, c)) {
      lua_newtable(L);

      lua_pushstring(L, c.name);
      lua_setfield(L, -2, "name");

      lua_pushinteger(L, c.type);
      lua_setfield(L, -2, "type");

      lua_pushinteger(L, c.out_path_len);
      lua_setfield(L, -2, "path_len");

      lua_pushinteger(L, c.last_advert_timestamp);
      lua_setfield(L, -2, "last_seen");

      lua_pushinteger(L, c.lastmod);
      lua_setfield(L, -2, "lastmod");

      char hex[PUB_KEY_SIZE * 2 + 1];
      mesh::Utils::toHex(hex, c.id.pub_key, PUB_KEY_SIZE);
      lua_pushstring(L, hex);
      lua_setfield(L, -2, "pubkey");

      lua_pushstring(L, the_mesh.getTypeName(c.type));
      lua_setfield(L, -2, "type_name");

      lua_pushboolean(L, (c.flags & 0x01) != 0);
      lua_setfield(L, -2, "favorite");

      lua_rawseti(L, -2, idx++);
    }
  }
  MESH_UNLOCK();

  return 1;
}

// Send self advertisement
// Usage from Lua: _mesh_send_advert()          -- flood (default)
//                  _mesh_send_advert("zerohop") -- zero-hop only
static int lua_mesh_send_advert(lua_State *L) {
  const char *mode = luaL_optstring(L, 1, "flood");
  MESH_LOCK();
  auto pkt = the_mesh.createSelfAdvert(the_mesh._prefs.node_name,
                                       the_mesh._prefs.node_lat,
                                       the_mesh._prefs.node_lon);
  if (pkt) {
    if (strcmp(mode, "zerohop") == 0) {
      the_mesh.sendZeroHop(pkt, (uint32_t)0);
    } else {
      the_mesh.sendFlood(pkt, (uint32_t)0);
    }
  }
  MESH_UNLOCK();
  lua_pushboolean(L, pkt ? 1 : 0);
  return 1;
}

// Get number of contacts
static int lua_mesh_get_num_contacts(lua_State *L) {
  MESH_LOCK();
  int n = the_mesh.getNumContacts();
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
    strncpy(the_mesh._prefs.node_name, value, sizeof(the_mesh._prefs.node_name) - 1);
    the_mesh._prefs.node_name[sizeof(the_mesh._prefs.node_name) - 1] = '\0';
    the_mesh.savePrefs();
    Serial.printf("Node name set to: %s\n", the_mesh._prefs.node_name);
    lua_pushboolean(L, 1);
  } else if (strcmp(key, "freq") == 0) {
    the_mesh._prefs.freq = atof(value);
    the_mesh.savePrefs();
    Serial.printf("Frequency set to: %.3f (reboot to apply)\n", the_mesh._prefs.freq);
    lua_pushboolean(L, 1);
  } else if (strcmp(key, "tx") == 0) {
    the_mesh._prefs.tx_power_dbm = atoi(value);
    the_mesh.savePrefs();
    Serial.printf("TX power set to: %d dBm (reboot to apply)\n", the_mesh._prefs.tx_power_dbm);
    lua_pushboolean(L, 1);
  } else if (strcmp(key, "lat") == 0) {
    the_mesh._prefs.node_lat = atof(value);
    the_mesh.savePrefs();
    lua_pushboolean(L, 1);
  } else if (strcmp(key, "lon") == 0) {
    the_mesh._prefs.node_lon = atof(value);
    the_mesh.savePrefs();
    lua_pushboolean(L, 1);
  } else if (strcmp(key, "bw") == 0) {
    the_mesh._prefs.bandwidth = atof(value);
    the_mesh.savePrefs();
    Serial.printf("Bandwidth set to: %.1f kHz (reboot to apply)\n", the_mesh._prefs.bandwidth);
    lua_pushboolean(L, 1);
  } else if (strcmp(key, "sf") == 0) {
    the_mesh._prefs.spreading_factor = atoi(value);
    the_mesh.savePrefs();
    Serial.printf("Spreading factor set to: %d (reboot to apply)\n", the_mesh._prefs.spreading_factor);
    lua_pushboolean(L, 1);
  } else if (strcmp(key, "cr") == 0) {
    the_mesh._prefs.coding_rate = atoi(value);
    the_mesh.savePrefs();
    Serial.printf("Coding rate set to: %d (reboot to apply)\n", the_mesh._prefs.coding_rate);
    lua_pushboolean(L, 1);
  } else if (strcmp(key, "contact_overwrite") == 0) {
    the_mesh._prefs.contact_overwrite = (atoi(value) != 0) ? 1 : 0;
    the_mesh.savePrefs();
    Serial.printf("Contact overwrite set to: %s\n", the_mesh._prefs.contact_overwrite ? "ON" : "OFF");
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
    if (the_mesh.getChannel(i, cd)) {
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
    the_mesh.setChannel(ch_idx, cd);
    the_mesh.saveChannels();
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
    the_mesh.setChannel(ch_idx, cd);
    the_mesh.saveChannels();
    MESH_UNLOCK();
    lua_pushboolean(L, 1);
    return 1;
  }

  // Normal channel with PSK
  ChannelDetails *result = the_mesh.addChannel(name, psk);
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
    bool ok = the_mesh.setChannel(ch_idx, cd);
    if (ok) the_mesh.saveChannels();
    MESH_UNLOCK();
    lua_pushboolean(L, ok ? 1 : 0);
    return 1;
  }

  the_mesh.saveChannels();
  MESH_UNLOCK();
  lua_pushboolean(L, 1);
  return 1;
}

// Send a message to a specific channel by index
// Usage: _mesh_send_channel(1, "Hello channel!")
static int lua_mesh_send_channel(lua_State *L) {
  int ch_idx = luaL_checkinteger(L, 1);
  const char *raw = luaL_checkstring(L, 2);
  char text[160];
  normalize_smart_quotes(raw, text, sizeof(text));

  MESH_LOCK();
  ChannelDetails cd;
  if (!the_mesh.getChannel(ch_idx, cd) || cd.name[0] == '\0') {
    MESH_UNLOCK();
    lua_pushboolean(L, 0);
    lua_pushstring(L, "Channel not found");
    return 2;
  }

  uint32_t timestamp = the_mesh.getRTCClock()->getCurrentTime();
  bool ok = the_mesh.sendGroupMessage(
    timestamp, cd.channel, the_mesh._prefs.node_name, text, strlen(text)
  );

  if (ok) {
    // Persist local echo for this channel slot
    the_mesh.appendChannelMessage(ch_idx, the_mesh._prefs.node_name, text,
                                  timestamp, 0.0f, 0.0f, 0, false);
  }
  MESH_UNLOCK();

  lua_pushboolean(L, ok ? 1 : 0);
  return 1;
}

// Remove a contact by name prefix
// Usage: _mesh_remove_contact("alice")
static int lua_mesh_remove_contact(lua_State *L) {
  const char *name_prefix = luaL_checkstring(L, 1);

  MESH_LOCK();
  ContactInfo *c = the_mesh.searchContactsByPrefix(name_prefix);
  if (!c) {
    MESH_UNLOCK();
    lua_pushboolean(L, 0);
    lua_pushstring(L, "Contact not found");
    return 2;
  }

  bool ok = the_mesh.removeContact(*c);
  if (ok) the_mesh.saveContacts();
  MESH_UNLOCK();

  lua_pushboolean(L, ok ? 1 : 0);
  return 1;
}

// Clear all contacts
// Usage: _mesh_clear_contacts()
static int lua_mesh_clear_contacts(lua_State *L) {
  MESH_LOCK();
  the_mesh.clearContacts();
  the_mesh.saveContacts();
  MESH_UNLOCK();
  Serial.println("[MESH] All contacts cleared");
  lua_pushboolean(L, 1);
  return 1;
}

// Reset path to a contact (force flood routing next time)
// Usage: _mesh_reset_path("alice")
static int lua_mesh_reset_path(lua_State *L) {
  const char *name_prefix = luaL_checkstring(L, 1);

  MESH_LOCK();
  ContactInfo *c = the_mesh.searchContactsByPrefix(name_prefix);
  if (!c) {
    MESH_UNLOCK();
    lua_pushboolean(L, 0);
    lua_pushstring(L, "Contact not found");
    return 2;
  }

  the_mesh.resetPathTo(*c);
  the_mesh.saveContacts();
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
  ContactInfo *c = the_mesh.searchContactsByPrefix(name_prefix);
  if (!c) {
    MESH_UNLOCK();
    lua_pushboolean(L, 0);
    lua_pushstring(L, "Contact not found");
    return 2;
  }

  if (fav) c->flags |= 0x01;
  else     c->flags &= ~0x01;
  the_mesh.saveContacts();
  MESH_UNLOCK();

  lua_pushboolean(L, 1);
  return 1;
}

// Export a contact as hex biz card string
// Usage: local hex = _mesh_export_contact("alice")
static int lua_mesh_export_contact(lua_State *L) {
  const char *name_prefix = luaL_checkstring(L, 1);

  MESH_LOCK();
  ContactInfo *c = the_mesh.searchContactsByPrefix(name_prefix);
  if (!c) {
    MESH_UNLOCK();
    lua_pushnil(L);
    lua_pushstring(L, "Contact not found");
    return 2;
  }

  uint8_t buf[256];
  uint8_t len = the_mesh.exportContact(*c, buf);
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
  the_mesh.importCard(card);
  MESH_UNLOCK();
  lua_pushboolean(L, 1);
  return 1;
}

// Share a contact via zero-hop broadcast
// Usage: _mesh_share_contact("alice")
static int lua_mesh_share_contact(lua_State *L) {
  const char *name_prefix = luaL_checkstring(L, 1);

  MESH_LOCK();
  ContactInfo *c = the_mesh.searchContactsByPrefix(name_prefix);
  if (!c) {
    MESH_UNLOCK();
    lua_pushboolean(L, 0);
    lua_pushstring(L, "Contact not found");
    return 2;
  }

  bool ok = the_mesh.shareContactZeroHop(*c);
  MESH_UNLOCK();
  lua_pushboolean(L, ok ? 1 : 0);
  return 1;
}

// Login to a room server
// Usage: local ok, route = _mesh_login_room("myroom", "password123")
static int lua_mesh_login_room(lua_State *L) {
  const char *name_prefix = luaL_checkstring(L, 1);
  const char *password = luaL_checkstring(L, 2);

  MESH_LOCK();
  ContactInfo *c = the_mesh.searchContactsByPrefix(name_prefix);
  if (!c) {
    MESH_UNLOCK();
    lua_pushboolean(L, 0);
    lua_pushstring(L, "Contact not found");
    return 2;
  }

  uint32_t est_timeout = 0;
  int result = the_mesh.sendLogin(*c, password, est_timeout);
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

// Send a request to a contact (e.g. get stats from repeater/room)
// Usage: local ok, route = _mesh_send_request("repeater1", 1)  -- 1=GET_STATUS
static int lua_mesh_send_request(lua_State *L) {
  const char *name_prefix = luaL_checkstring(L, 1);
  int req_type = luaL_checkinteger(L, 2);

  MESH_LOCK();
  ContactInfo *c = the_mesh.searchContactsByPrefix(name_prefix);
  if (!c) {
    MESH_UNLOCK();
    lua_pushboolean(L, 0);
    lua_pushstring(L, "Contact not found");
    return 2;
  }

  uint32_t tag = 0;
  uint32_t est_timeout = 0;
  int result = the_mesh.sendRequest(*c, (uint8_t)req_type, tag, est_timeout);
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
  float snr  = the_mesh.last_rx_snr;
  float rssi = the_mesh.last_rx_rssi;
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

  the_mesh._prefs.rx_boost = en ? 1 : 0;
  the_mesh.savePrefs();
  Serial.printf("[RADIO] RX Boost preference saved: %d\n", en ? 1 : 0);

  return 0;
}

// ── Persistent message history bridge ────────────────────────────

// Read all stored messages for a channel slot.
// Usage: local msgs = _mesh_get_channel_messages(0)
// Returns array of { from, peer, text, timestamp, hops, snr, rssi, direct, is_dm, channel_idx }
static int lua_mesh_get_channel_messages(lua_State *L) {
  int ch_idx = luaL_checkinteger(L, 1);
  MESH_LOCK();
  int n = the_mesh.pushChannelMessagesToLua(L, ch_idx);
  MESH_UNLOCK();
  return n;
}

// Read all stored messages for a DM thread.
// Usage: local msgs = _mesh_get_dm_messages("alice")
static int lua_mesh_get_dm_messages(lua_State *L) {
  const char *peer = luaL_checkstring(L, 1);
  MESH_LOCK();
  int n = the_mesh.pushDMMessagesToLua(L, peer);
  MESH_UNLOCK();
  return n;
}

// Enumerate all DM thread peer names that have stored messages.
// Usage: local names = _mesh_get_dm_threads()
static int lua_mesh_get_dm_threads(lua_State *L) {
  MESH_LOCK();
  int n = the_mesh.pushDMThreadNamesToLua(L);
  MESH_UNLOCK();
  return n;
}

// Configure the max records retained per message log file.
// Usage: _mesh_set_max_messages(100)
static int lua_mesh_set_max_messages(lua_State *L) {
  int n = luaL_checkinteger(L, 1);
  MESH_LOCK();
  the_mesh.setMaxMessages(n);
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
  bool is_sd = (the_mesh._storage != &LittleFS);
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

// Set whether to use SD card for mesh data storage
// Usage: _storage_set_use_sd(true)  -- switch to SD
//        _storage_set_use_sd(false) -- switch to LittleFS
// Migrates existing data to the new location and saves preference
static int lua_storage_set_use_sd(lua_State *L) {
  bool want_sd = lua_toboolean(L, 1);

  Serial.printf("[STORAGE] User requested: use_sd=%s\n", want_sd ? "true" : "false");

  if (want_sd && !sd_mounted) {
    Serial.println("[STORAGE] Cannot use SD — card not mounted");
    lua_pushboolean(L, 0);
    lua_pushstring(L, "SD card not available");
    return 2;
  }

  // Determine source and destination
  MESH_LOCK();
  fs::FS* oldFS = the_mesh._storage;
  String oldPrefix = the_mesh._storage_prefix;
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
    Serial.println("[STORAGE] Migrating mesh data...");
    // Migration may touch SD (either source or destination) plus LittleFS;
    // holding the SPI mutex across the whole loop is simpler and safe.
    sd_spi_take();
    const char* files[] = { "/identity", "/node_prefs", "/contacts" };
    for (int i = 0; i < 3; i++) {
      String srcPath = oldPrefix + files[i];
      String dstPath = newPrefix + files[i];
      if (oldFS->exists(srcPath.c_str())) {
        bool ok = copyFile(*oldFS, srcPath.c_str(), *newFS, dstPath.c_str());
        Serial.printf("[STORAGE]   %s -> %s: %s\n", srcPath.c_str(), dstPath.c_str(), ok ? "OK" : "FAILED");
      }
    }
    sd_spi_release();
  }

  // Switch active storage
  MESH_LOCK();
  the_mesh.setStorage(newFS, newPrefix.c_str());
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
    Serial.printf("[FS] _list_dir: cannot open %s\n", path);
    return 1; // return empty table
  }

  File entry = root.openNextFile();
  while (entry) {
    if (entry.isDirectory()) {
      // entry.name() may return full path or just name depending on core version
      const char *name = pathBasename(entry.name());
      Serial.printf("[FS] _list_dir: found dir: raw='%s' name='%s'\n", entry.name(), name);
      if (name[0] != '\0') {
        lua_pushstring(L, name);
        lua_rawseti(L, -2, idx++);
      }
    }
    entry = root.openNextFile();
  }

  Serial.printf("[FS] _list_dir(%s): found %d dirs\n", path, idx - 1);
  return 1;
}

// List subdirectory names on SD card
// Usage: local dirs = _list_dir_sd("/meshpunk/apps")
static int lua_list_dir_sd(lua_State *L) {
  const char *path = luaL_checkstring(L, 1);

  lua_newtable(L);
  int idx = 1;

  if (!sd_mounted) {
    Serial.println("[FS] _list_dir_sd: SD not mounted");
    return 1; // return empty table
  }

  sd_spi_take();
  File root = SD.open(path);
  if (!root || !root.isDirectory()) {
    Serial.printf("[FS] _list_dir_sd: cannot open %s\n", path);
    sd_spi_release();
    return 1;
  }

  File entry = root.openNextFile();
  while (entry) {
    if (entry.isDirectory()) {
      const char *name = pathBasename(entry.name());
      Serial.printf("[FS] _list_dir_sd: found dir: raw='%s' name='%s'\n", entry.name(), name);
      if (name[0] != '\0') {
        lua_pushstring(L, name);
        lua_rawseti(L, -2, idx++);
      }
    }
    entry = root.openNextFile();
  }
  root.close();

  sd_spi_release();

  Serial.printf("[FS] _list_dir_sd(%s): found %d dirs\n", path, idx - 1);
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
    Serial.printf("[FS] _list_all: cannot open %s\n", path);
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

  Serial.printf("[FS] _list_all(%s): found %d entries\n", path, idx - 1);
  return 1;
}

// List ALL entries (files and directories) on SD card
// Usage: local entries = _list_all_sd("/meshpunk/apps")
static int lua_list_all_sd(lua_State *L) {
  const char *path = luaL_checkstring(L, 1);

  lua_newtable(L);
  int idx = 1;

  if (!sd_mounted) {
    Serial.println("[FS] _list_all_sd: SD not mounted");
    return 1;
  }

  sd_spi_take();
  File root = SD.open(path);
  if (!root || !root.isDirectory()) {
    Serial.printf("[FS] _list_all_sd: cannot open %s\n", path);
    sd_spi_release();
    return 1;
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
      lua_pop(L, 1);
    }

    entry = root.openNextFile();
  }
  root.close();

  sd_spi_release();

  Serial.printf("[FS] _list_all_sd(%s): found %d entries\n", path, idx - 1);
  return 1;
}

// Check if a file exists on SD card
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

// Load and execute a Lua file from the SD card
// Usage: _dofile_sd("/meshpunk/apps/myapp/main.lua")
// This is needed because dofile/loadfile only read from LittleFS
static int lua_dofile_sd(lua_State *L) {
  const char *path = luaL_checkstring(L, 1);

  if (!sd_mounted) {
    lua_pushnil(L);
    lua_pushstring(L, "SD card not mounted");
    return 2;
  }

  sd_spi_take();
  File file = SD.open(path);
  if (!file || file.isDirectory()) {
    sd_spi_release();
    lua_pushnil(L);
    lua_pushfstring(L, "Cannot open SD file: %s", path);
    return 2;
  }

  size_t size = file.size();
  char* buffer = (char*)malloc(size + 1);
  if (!buffer) {
    file.close();
    sd_spi_release();
    lua_pushnil(L);
    lua_pushstring(L, "Out of memory reading SD file");
    return 2;
  }

  file.readBytes(buffer, size);
  buffer[size] = '\0';
  file.close();

  // Release SD bus back to display BEFORE executing the Lua chunk
  sd_spi_release();

  Serial.printf("[FS] _dofile_sd: loading %s (%d bytes)\n", path, (int)size);

  // Count extra args (everything after the path on the stack)
  int nargs = lua_gettop(L) - 1;

  int status = luaL_loadbuffer(L, buffer, size, path);
  free(buffer);

  if (status != LUA_OK) {
    Serial.printf("[FS] _dofile_sd: load error: %s\n", lua_tostring(L, -1));
    return lua_error(L);
  }

  // Stack: [path, arg1, arg2, ..., chunk]
  // Move chunk to position 2 (after path), then remove path
  lua_insert(L, 2);
  lua_remove(L, 1);
  // Stack: [chunk, arg1, arg2, ...]

  if (lua_pcall(L, nargs, LUA_MULTRET, 0) != LUA_OK) {
    Serial.printf("[FS] _dofile_sd: exec error: %s\n", lua_tostring(L, -1));
    return lua_error(L);
  }

  return lua_gettop(L); // return whatever the script returned
}

// PSRAM allocator for Lua – keeps internal SRAM free for DMA (AES, etc.)
static void *lua_psram_alloc(void *ud, void *ptr, size_t osize, size_t nsize) {
    (void)ud; (void)osize;
    if (nsize == 0) {
        heap_caps_free(ptr);
        return NULL;
    }
    return heap_caps_realloc(ptr, nsize, MALLOC_CAP_SPIRAM);
}

// Forward declarations for sound engine (defined after setupLuaVGL)
static int  sound_create_tone(uint16_t freq_hz, uint16_t duration_ms);
static int  sound_load_file(lua_State* L);
static void sound_play(int id);
static void sound_stop(int id);
static void sound_pause(int id);
static void sound_delete(int id);

// Initialize LuaVGL
void setupLuaVGL() {
  // Create Lua state with PSRAM allocator
  L = lua_newstate(lua_psram_alloc, NULL);
  if (!L) {
    Serial.println("Failed to create Lua state");
    return;
  }

  // Set Lua runtime on PunkMesh
  the_mesh.lua_runtime = L;

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

  // Register Mesh bridge functions
  lua_register(L, "_mesh_send_public", lua_mesh_send_public);
  lua_register(L, "_mesh_send_direct", lua_mesh_send_direct);
  lua_register(L, "_mesh_get_node_info", lua_mesh_get_node_info);
  lua_register(L, "_mesh_get_contacts", lua_mesh_get_contacts);
  lua_register(L, "_mesh_send_advert", lua_mesh_send_advert);
  lua_register(L, "_mesh_get_num_contacts", lua_mesh_get_num_contacts);
  lua_register(L, "_mesh_set_config", lua_mesh_set_config);

  // Register new MeshCore integration bridge functions
  lua_register(L, "_mesh_get_channels", lua_mesh_get_channels);
  lua_register(L, "_mesh_set_channel", lua_mesh_set_channel);
  lua_register(L, "_mesh_send_channel", lua_mesh_send_channel);
  lua_register(L, "_mesh_remove_contact", lua_mesh_remove_contact);
  lua_register(L, "_mesh_clear_contacts", lua_mesh_clear_contacts);
  lua_register(L, "_mesh_reset_path", lua_mesh_reset_path);
  lua_register(L, "_mesh_export_contact", lua_mesh_export_contact);
  lua_register(L, "_mesh_import_contact", lua_mesh_import_contact);
  lua_register(L, "_mesh_share_contact", lua_mesh_share_contact);
  lua_register(L, "_mesh_set_contact_favorite", lua_mesh_set_contact_favorite);
  lua_register(L, "_mesh_login_room", lua_mesh_login_room);
  lua_register(L, "_mesh_send_request", lua_mesh_send_request);
  lua_register(L, "_mesh_get_rx_info", lua_mesh_get_rx_info);
  lua_register(L, "_mesh_get_rx_boost", lua_mesh_get_rx_boost);
  lua_register(L, "_mesh_set_rx_boost", lua_mesh_set_rx_boost);

  // Persistent message history APIs — available to any app, not just messenger
  lua_register(L, "_mesh_get_channel_messages", lua_mesh_get_channel_messages);
  lua_register(L, "_mesh_get_dm_messages", lua_mesh_get_dm_messages);
  lua_register(L, "_mesh_get_dm_threads", lua_mesh_get_dm_threads);
  lua_register(L, "_mesh_set_max_messages", lua_mesh_set_max_messages);

  // Register Storage bridge functions
  lua_register(L, "_storage_get_info", lua_storage_get_info);
  lua_register(L, "_storage_set_use_sd", lua_storage_set_use_sd);
  lua_register(L, "_emoji_preload", lua_emoji_preload);

  // Register Filesystem bridge functions
  lua_register(L, "_list_dir", lua_list_dir);

  // System
  lua_register(L, "_system_reboot", [](lua_State *L) -> int {
    Serial.println("[SYSTEM] Reboot requested from Lua");
    delay(100);
    ESP.restart();
    return 0;
  });

  // RTC epoch seconds (seeded from GPS once at boot, then free-running)
  lua_register(L, "_rtc_time", [](lua_State *L) -> int {
    MESH_LOCK();
    lua_Integer t = (lua_Integer)the_mesh.getRTCClock()->getCurrentTime();
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

  // ── Volume & mute ──────────────────────────────────────────────────────────
  lua_register(L, "_sound_set_volume", [](lua_State* L) -> int {
    int v = luaL_checkinteger(L, 1);
    if (v < 0) v = 0; if (v > 21) v = 21;
    sound_volume = (uint8_t)v;
    if (!sound_muted) audio->setVolume(sound_volume);
    firmware_prefs_save();
    lua_pushinteger(L, sound_volume);
    return 1;
  });
  lua_register(L, "_sound_get_volume", [](lua_State* L) -> int {
    lua_pushinteger(L, sound_volume);
    return 1;
  });
  lua_register(L, "_sound_get_muted", [](lua_State* L) -> int {
    lua_pushboolean(L, sound_muted ? 1 : 0);
    return 1;
  });
  lua_register(L, "_sound_set_muted", [](lua_State* L) -> int {
    sound_muted = lua_toboolean(L, 1);
    audio->setVolume(sound_muted ? 0 : sound_volume);
    firmware_prefs_save();
    lua_pushboolean(L, sound_muted ? 1 : 0);
    return 1;
  });
  lua_register(L, "_sound_is_playing", [](lua_State* L) -> int {
    bool playing = audio->isRunning();
    if (!playing) {
      for (int i = 0; i < sound_obj_count; i++)
        if (sound_objects[i]->type == SoundObject::TONE && sound_objects[i]->tone_playing)
          { playing = true; break; }
    }
    lua_pushboolean(L, playing ? 1 : 0);
    return 1;
  });

  // ── Sound objects ───────────────────────────────────────────────────────────
  lua_register(L, "_sound_generate_tone", [](lua_State* L) -> int {
    int freq = luaL_checkinteger(L, 1);
    int dur  = luaL_optinteger(L, 2, 200);
    if (freq < 20) freq = 20; if (freq > 20000) freq = 20000;
    if (dur  < 10) dur  = 10; if (dur  > 10000) dur  = 10000;
    lua_pushinteger(L, sound_create_tone((uint16_t)freq, (uint16_t)dur));
    return 1;
  });
  lua_register(L, "_sound_load_file", sound_load_file);
  lua_register(L, "_sound_play", [](lua_State* L) -> int {
    sound_play((int)luaL_checkinteger(L, 1));
    return 0;
  });
  lua_register(L, "_sound_stop", [](lua_State* L) -> int {
    sound_stop((int)luaL_checkinteger(L, 1));
    return 0;
  });
  lua_register(L, "_sound_pause", [](lua_State* L) -> int {
    sound_pause((int)luaL_checkinteger(L, 1));
    return 0;
  });
  lua_register(L, "_sound_delete", [](lua_State* L) -> int {
    sound_delete((int)luaL_checkinteger(L, 1));
    return 0;
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

  // Navigation controller: manages gridnav + touch/trackball switching
  lua_register(L, "_nav_setup", [](lua_State *L) -> int {
    luavgl_obj_t *lobj = (luavgl_obj_t *)lua_touserdata(L, 1);
    if (!lobj || !lobj->obj) return 0;
    int flags = luaL_optinteger(L, 2, LV_GRIDNAV_CTRL_ROLLOVER);

    if (nav_container) {
      lv_gridnav_remove(nav_container);
    }

    nav_container = lobj->obj;
    nav_flags = (lv_gridnav_ctrl_t)flags;
    nav_gridnav_active = true;

    lv_gridnav_add(nav_container, nav_flags);
    lv_group_add_obj(lv_group_get_default(), nav_container);
    lv_group_focus_obj(nav_container);

    lv_obj_add_event_cb(nav_container, nav_delete_cb, LV_EVENT_DELETE, NULL);
    return 0;
  });

  lua_register(L, "_nav_clear", [](lua_State *L) -> int {
    if (nav_container) {
      lv_gridnav_remove(nav_container);
      nav_container = NULL;
      nav_gridnav_active = false;
    }
    return 0;
  });

  lua_register(L, "_list_dir_sd", lua_list_dir_sd);
  lua_register(L, "_file_exists_sd", lua_file_exists_sd);
  lua_register(L, "_dofile_sd", lua_dofile_sd);
  lua_register(L, "_list_all", lua_list_all);
  lua_register(L, "_list_all_sd", lua_list_all_sd);

  // Add Lua loader for require function
  lua_getglobal(L, "package");
  lua_getfield(L, -1, "searchers");

  // Get the length of the searchers table
  int len = lua_rawlen(L, -1);

  // Custom loader function for the filesystem
  lua_pushcfunction(L, [](lua_State *L) -> int {
    const char *modname = luaL_checkstring(L, 1);
    String filename = String(LUA_PATH) + modname + ".lua";

    String content = readFile(filename.c_str());
    if (content.length() == 0) {
      lua_pushfstring(L, "\n\tno file '%s' in LittleFS", filename.c_str());
      return 1; // Return the error message
    }

    if (luaL_loadbuffer(L, content.c_str(), content.length(),
                        filename.c_str()) != 0) {
      lua_error(L);
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

  // file:read()
  lua_pushcfunction(L, [](lua_State *L) -> int {
    LuaFileHandle *ud = (LuaFileHandle *)luaL_checkudata(L, 1, "esp32_file");
    if (ud->is_sd) sd_spi_take();
    String content = ud->file->readString();
    if (ud->is_sd) sd_spi_release();
    lua_pushstring(L, content.c_str());
    return 1;
  });
  lua_setfield(L, -2, "read");

  // file:write(str)
  lua_pushcfunction(L, [](lua_State *L) -> int {
    LuaFileHandle *ud = (LuaFileHandle *)luaL_checkudata(L, 1, "esp32_file");
    size_t len;
    const char *str = luaL_checklstring(L, 2, &len);
    if (ud->is_sd) sd_spi_take();
    size_t written = ud->file->print(str);
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

  Serial.println("Added esp32_file");


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

  Serial.println("Patched IO");

  Serial.println("[LUA] LuaVGL environment initialized");
  Serial.printf("[LUA] Free heap: %d bytes\n", ESP.getFreeHeap());
  Serial.printf("[LUA] Free PSRAM: %d bytes\n", ESP.getFreePsram());

  if (!fs_mounted) {
    Serial.println("Filesystem not mounted, can't load Lua scripts");

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
      Serial.print("Lua fallback script error: ");
      Serial.println(lua_tostring(L, -1));
      lua_pop(L, 1);
    }

    return;
  }

  String scriptPath = String(LUA_PATH) + "main.lua";
  Serial.printf("[LUA] Reading script: %s\n", scriptPath.c_str());
  String script = readFile(scriptPath.c_str());
  Serial.printf("[LUA] Script length: %d bytes\n", script.length());

  if (script.length() == 0) {
    Serial.print("Lua script not found: ");
    Serial.println(scriptPath);

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

  Serial.print("[LUA] Executing Lua script: ");
  Serial.println(scriptPath);
  Serial.println("[LUA] --- luaL_dostring BEGIN ---");

  int lua_result = luaL_dostring(L, script.c_str());

  Serial.printf("[LUA] --- luaL_dostring END --- result=%d\n", lua_result);

  if (lua_result != 0) {
    const char *luaError = lua_tostring(L, -1);
    Serial.print("Lua execution error: ");
    Serial.println(luaError);

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
      Serial.print("Fallback display error: ");
      Serial.println(lua_tostring(L, -1));
    }

    lua_pop(L, 1);
    return;
  }

  return;
}

// ── Sound engine ───────────────────────────────────────────────────────────
static const uint32_t TONE_SR = 44100;

static void sound_obj_add(SoundObject* obj) {
  if (sound_obj_count >= sound_obj_capacity) {
    int new_cap = sound_obj_capacity == 0 ? 8 : sound_obj_capacity * 2;
    sound_objects = (SoundObject**)realloc(sound_objects, new_cap * sizeof(SoundObject*));
    sound_obj_capacity = new_cap;
  }
  sound_objects[sound_obj_count++] = obj;
}

static SoundObject* sound_obj_find(int id) {
  for (int i = 0; i < sound_obj_count; i++)
    if (sound_objects[i]->id == id) return sound_objects[i];
  return nullptr;
}

static void sound_obj_remove(int id) {
  for (int i = 0; i < sound_obj_count; i++) {
    if (sound_objects[i]->id != id) continue;
    SoundObject* obj = sound_objects[i];
    if (obj->type == SoundObject::TONE && obj->pcm_buffer) {
      free(obj->pcm_buffer);
    }
    if (obj->type == SoundObject::AUDIO_FILE && obj->file) {
      obj->file->close();
      delete obj->file;
    }
    delete obj;
    sound_objects[i] = sound_objects[--sound_obj_count];
    return;
  }
}

static int sound_create_tone(uint16_t freq_hz, uint16_t duration_ms) {
  const uint32_t SR = TONE_SR;
  uint32_t frames = (SR * duration_ms) / 1000;
  int16_t* buf = (int16_t*)ps_malloc(frames * 2 * sizeof(int16_t));
  if (!buf) return -1;

  const float omega = 2.0f * M_PI * freq_hz / SR;
  const uint32_t fade = SR / 100; // 10ms fade-in/out
  for (uint32_t i = 0; i < frames; i++) {
    float env = 1.0f;
    if (i < fade)           env = (float)i / fade;
    else if (i >= frames - fade) env = (float)(frames - i) / fade;
    int16_t s = (int16_t)(env * 16000.0f * sinf(omega * i));
    buf[i * 2]     = s;
    buf[i * 2 + 1] = s;
  }

  SoundObject* obj = new SoundObject{};
  obj->id           = next_sound_id++;
  obj->type         = SoundObject::TONE;
  obj->pcm_buffer   = buf;
  obj->sample_count = frames * 2;
  obj->play_pos     = 0;
  obj->tone_playing = false;
  obj->tone_paused  = false;
  sound_obj_add(obj);
  return obj->id;
}

static int sound_load_file(lua_State* L) {
  LuaFileHandle* fh = (LuaFileHandle*)luaL_checkudata(L, 1, "esp32_file");
  if (!fh || !fh->file) {
    lua_pushinteger(L, -1);
    return 1;
  }
  SoundObject* obj = new SoundObject{};
  obj->id          = next_sound_id++;
  obj->type        = SoundObject::AUDIO_FILE;
  obj->file        = fh->file;
  obj->file_is_sd  = fh->is_sd;
  obj->file_paused = false;
  fh->file = nullptr;
  sound_obj_add(obj);
  lua_pushinteger(L, obj->id);
  return 1;
}

static void sound_play(int id) {
  SoundObject* obj = sound_obj_find(id);
  if (!obj) return;
  if (obj->type == SoundObject::TONE) {
    obj->play_pos     = 0;
    obj->tone_playing = true;
    obj->tone_paused  = false;
  } else {
    audio->stopSong();
    active_file_is_sd = false;
    obj->file->seek(0);
    obj->file_paused = false;
    audio->connectToFile(*obj->file);
    active_file_is_sd = obj->file_is_sd;
  }
}

static void sound_stop(int id) {
  SoundObject* obj = sound_obj_find(id);
  if (!obj) return;
  if (obj->type == SoundObject::TONE) {
    obj->tone_playing = false;
    obj->play_pos     = 0;
  } else {
    audio->stopSong();
    active_file_is_sd = false;
    obj->file_paused  = false;
  }
}

static void sound_pause(int id) {
  SoundObject* obj = sound_obj_find(id);
  if (!obj) return;
  if (obj->type == SoundObject::TONE) {
    obj->tone_paused = !obj->tone_paused;
  } else {
    audio->pauseResume();
    obj->file_paused = !obj->file_paused;
  }
}

static void sound_delete(int id) {
  sound_obj_remove(id);
}

static bool tone_sr_set = false;

static void sound_tone_tick() {
  if (audio->isRunning()) { tone_sr_set = false; return; }

  bool any_active = false;
  for (int i = 0; i < sound_obj_count; i++) {
    SoundObject* o = sound_objects[i];
    if (o->type == SoundObject::TONE && o->tone_playing && !o->tone_paused)
      any_active = true;
  }
  if (!any_active) { tone_sr_set = false; return; }

  if (!tone_sr_set) {
    i2s_set_sample_rates(I2S_NUM_0, TONE_SR);
    tone_sr_set = true;
  }

  const int CHUNK = 256;
  int32_t mix[CHUNK * 2] = {};

  for (int i = 0; i < sound_obj_count; i++) {
    SoundObject* o = sound_objects[i];
    if (o->type != SoundObject::TONE || !o->tone_playing || o->tone_paused) continue;
    for (int s = 0; s < CHUNK * 2 && o->play_pos < o->sample_count; s++) {
      mix[s] += o->pcm_buffer[o->play_pos++];
    }
    if (o->play_pos >= o->sample_count) {
      o->tone_playing = false;
      o->play_pos     = 0;
    }
  }

  float vol_scale = sound_muted ? 0.0f : (float)sound_volume / 21.0f;
  int16_t out[CHUNK * 2];
  for (int s = 0; s < CHUNK * 2; s++)
    out[s] = (int16_t)(constrain(mix[s], -32768, 32767) * vol_scale);
  size_t written = 0;
  i2s_write(I2S_NUM_0, out, sizeof(out), &written, pdMS_TO_TICKS(50));
}

void audio_process_extern(int16_t* buff, uint16_t len, bool* continueI2S) {
  for (int i = 0; i < sound_obj_count; i++) {
    SoundObject* o = sound_objects[i];
    if (o->type != SoundObject::TONE || !o->tone_playing || o->tone_paused) continue;
    for (uint16_t s = 0; s < len && o->play_pos < o->sample_count; s++) {
      int32_t mixed = (int32_t)buff[s] + (int32_t)o->pcm_buffer[o->play_pos++];
      buff[s] = (int16_t)constrain(mixed, -32768, 32767);
    }
    if (o->play_pos >= o->sample_count) {
      o->tone_playing = false;
      o->play_pos     = 0;
    }
  }
  *continueI2S = true;
}

// ── End sound engine ───────────────────────────────────────────────────────

volatile bool lora_packet_ready = false;

void setup() {
  Serial.begin(115200);
  Serial.println("Delaying for 50ms...");
  delay(50);

  Serial.println("MeshPunk LuaVGL Demo");

  // Create SPI/mesh mutexes and cross-core queues before any subsystem
  // that relies on them. Safe to call before LVGL/TFT init because the
  // macros no-op when the handle is null (not needed — this runs first —
  // but defensive).
  meshpunk_sync_init();

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

  Serial.println("Initializing display");

  // Initialize filesystem
  if (LittleFS.begin(true)) {
    fs_mounted = true;
    Serial.println("LittleFS mounted successfully");

    Serial.println("LittleFS contents:");
    listDir(LittleFS, "/lua");

    // Load firmware preferences (tz, use_sd, clock_fmt)
    firmware_prefs_load();
  } else {
    Serial.println("Error mounting LittleFS!!");
  }

  // Initialize SD card for persistent mesh data (survives LittleFS reflash)
  Serial.println("===== SD CARD INIT =====");

  if (SD.begin(BOARD_SDCARD_CS, SPI)) {
    sd_mounted = true;
    uint64_t cardSize = SD.cardSize() / (1024 * 1024);
    Serial.printf("[SD] Card mounted, size: %llu MB\n", cardSize);

    const char* required_dirs[] = {
      "/meshpunk",
      "/meshpunk/apps",
      "/meshpunk/messages",
    };
    for (auto dir : required_dirs) {
      if (!SD.exists(dir)) {
        SD.mkdir(dir);
        Serial.printf("[SD] Created %s\n", dir);
      }
    }
  } else {
    Serial.println("[SD] Card mount FAILED");
  }

  // Decide which storage to use (use_sd_pref loaded by firmware_prefs_load)
  if (sd_mounted && use_sd_pref) {
    the_mesh.setStorage(&SD, "/meshpunk");
    Serial.println("[SD] Mesh storage: SD:/meshpunk/");
  } else {
    the_mesh.setStorage(&LittleFS, "");
    if (sd_mounted) {
      Serial.println("[SD] SD available but user chose LittleFS");
    } else {
      Serial.println("[SD] Using LittleFS (no SD card)");
    }
  }

  // Initialize WiFi in station mode
  WiFi.mode(WIFI_STA);
  Serial.println("WiFi initialized in station mode");

  // Set touch int input
  pinMode(BOARD_TOUCH_INT, INPUT);
  delay(20);

  Serial.println("Initializing GT911 touch sensor");

  Wire.begin(BOARD_I2C_SDA, BOARD_I2C_SCL);


  touch.setPins(-1, BOARD_TOUCH_INT);
  if (!touch.begin(Wire, GT911_SLAVE_ADDRESS_L)) {
    while (1) {
      Serial.println("Failed to find GT911 - check your wiring!");
      delay(1000);
    }
  }

  // Set touch max xy
  touch.setMaxCoordinates(320, 240);

  // Set swap xy
  touch.setSwapXY(true);

  // Set mirror xy
  touch.setMirrorXY(false, true);

  // Initialize keyboard
  Wire.beginTransmission(LILYGO_KB_SLAVE_ADDRESS);
  if (Wire.endTransmission() == 0) {
    keyboard_available = true;
    Serial.println("T-Deck keyboard found!");

    // Set initial keyboard brightness
    setKeyboardDefaultBrightness(127);
    setKeyboardBrightness(200);
  } else {
    Serial.println("T-Deck keyboard not found!");
  }

  // Initialize I2S audio output on T-Deck speaker
  audio = new Audio();
  audio->setPinout(TDECK_I2S_BCK, TDECK_I2S_WS, TDECK_I2S_DOUT);
  audio->setVolume(sound_muted ? 0 : sound_volume);
  Serial.printf("[AUDIO] I2S init: vol=%d muted=%d\n", sound_volume, sound_muted ? 1 : 0);

  tft.fillScreen(TFT_GREEN);

  // Initialize LORA Radio
  Serial.println(F("===== RADIO INIT ====="));

  int16_t state = radio.begin();
  Serial.printf("[RADIO] begin() = %d %s\n", state, state == RADIOLIB_ERR_NONE ? "OK" : "FAILED");

  delay(100);

  float freq = the_mesh.getFreqPref();
  uint8_t tx_pwr = the_mesh.getTxPowerPref();
  float bw = the_mesh.getBandwidthPref();
  uint8_t sf = the_mesh.getSpreadingFactorPref();
  uint8_t cr = the_mesh.getCodingRatePref();
  Serial.printf("[RADIO] Setting freq=%.3f MHz, BW=%.0f kHz, SF=%d, CR=%d, TX=%d dBm\n", freq, bw, sf, cr, tx_pwr);

  state = radio.setFrequency(freq);
  Serial.printf("[RADIO] setFrequency = %d %s\n", state, state == RADIOLIB_ERR_NONE ? "OK" : "FAILED");

  state = radio.setBandwidth(bw);
  Serial.printf("[RADIO] setBandwidth = %d %s\n", state, state == RADIOLIB_ERR_NONE ? "OK" : "FAILED");

  state = radio.setSpreadingFactor(sf);
  Serial.printf("[RADIO] setSpreadingFactor = %d %s\n", state, state == RADIOLIB_ERR_NONE ? "OK" : "FAILED");

  state = radio.setCodingRate(cr);
  Serial.printf("[RADIO] setCodingRate = %d %s\n", state, state == RADIOLIB_ERR_NONE ? "OK" : "FAILED");

  radio.setCRC(true);

  state = radio.setOutputPower(tx_pwr);
  Serial.printf("[RADIO] setOutputPower = %d %s\n", state, state == RADIOLIB_ERR_NONE ? "OK" : "FAILED");

  state = radio.startReceive();
  Serial.printf("[RADIO] startReceive = %d %s\n", state, state == RADIOLIB_ERR_NONE ? "OK" : "FAILED");

  Serial.println(F("===== MESHCORE INIT ====="));
  fast_rng.begin(123456); // fixed seed for testing
  the_mesh.begin();
  the_mesh.showWelcome();

  // Apply RX boost from prefs (loaded in the_mesh.begin())
  if (the_mesh._prefs.rx_boost) {
    SPI_LOCK();
    radio_driver.setRxBoostedGainMode(true);
    SPI_UNLOCK();
    Serial.println("[RADIO] RX Boost restored from prefs: ON");
  }

  Serial.printf("[MESH] Node name: %s\n", the_mesh._prefs.node_name);
  Serial.printf("[MESH] Freq pref: %.3f MHz\n", the_mesh._prefs.freq);
  Serial.printf("[MESH] TX power pref: %d dBm\n", the_mesh._prefs.tx_power_dbm);
  Serial.printf("[MESH] Contacts loaded: %d\n", the_mesh.getNumContacts());
  Serial.printf("[MESH] Public channel: %s\n", the_mesh._public ? "YES" : "NO (PROBLEM!)");
  Serial.print("[MESH] Pub key: ");
  mesh::Utils::printHex(Serial, the_mesh.self_id.pub_key, PUB_KEY_SIZE);
  Serial.println();

  //Initialize the disply only after all other spi bus setup is finished
  Serial.println("Initialize display");
  tft.begin();
  tft.setRotation(1);
  tft.fillScreen(TFT_BLACK);

  // LVGL tick function
  lvgl_ticker.attach_ms(5, []() {
    lv_tick_inc(5); // Increment LVGL tick counter every 5ms
  });

  // Initialize LVGL
  setupLvgl();

  // Set LVGL screen to opaque dark background
  // Without this, LVGL objects are transparent and the raw TFT fill color shows through
  lv_obj_set_style_bg_color(lv_scr_act(), lv_color_make(0x10, 0x10, 0x10), 0);
  lv_obj_set_style_bg_opa(lv_scr_act(), LV_OPA_COVER, 0);

  Serial.println("===== LUA INIT =====");

  // Initialize LuaVGL
  setupLuaVGL();

  Serial.println("[LUA] setupLuaVGL() returned");

  // Create UI
  createUI();

  // Adjust backlight
  pinMode(BOARD_BL_PIN, OUTPUT);
  setBrightness(16);

  // Hand off mesh + radio to Core 1 now that the_mesh, Lua, LVGL, and the
  // RX queue are all up. Must happen AFTER createUI / setupLuaVGL so that
  // any RX events arriving from the mesh task have something to drain into.
  Serial.printf("[TASK] setup() running on core=%d; spawning mesh_task on Core 1\n",
                xPortGetCoreID());
  meshpunk_spawn_mesh_task();
  meshpunk_spawn_gps_task();
}

// Forward decls for the Lua dispatchers that live in punkmesh.cpp.
// These are called only from the UI core (Core 0) to preserve lua_State
// single-threadedness.
extern void lua_mesh_push_channel_message(lua_State* L, const char* sender_name, uint8_t hops, bool direct, uint32_t timestamp, const char *text, float snr, float rssi, int channel_idx);
extern void lua_mesh_push_direct_message(lua_State* L, const char* sender_name, uint8_t hops, bool direct, uint32_t timestamp, const char *text, float snr, float rssi);
extern void lua_mesh_push_contact_update(lua_State* L, const char* name, uint8_t contact_type);

// Drain RX events posted by the mesh core. Runs every UI tick.
// Bounded per call so a flood on the queue can't starve LVGL.
static void drain_rx_events() {
  if (!rx_event_queue || !L) return;
  RxEvent ev;
  int budget = 8; // cap messages per tick to keep UI responsive
  while (budget-- > 0 && xQueueReceive(rx_event_queue, &ev, 0) == pdTRUE) {
    if (ev.kind == RxEvent::DIRECT_MSG) {
      lua_mesh_push_direct_message(L, ev.sender, ev.hops, ev.direct,
                                   ev.timestamp, ev.text, ev.snr, ev.rssi);
    } else if (ev.kind == RxEvent::CHANNEL_MSG) {
      lua_mesh_push_channel_message(L, ev.sender, ev.hops, ev.direct,
                                    ev.timestamp, ev.text, ev.snr, ev.rssi,
                                    ev.channel_idx);
    } else if (ev.kind == RxEvent::CONTACT_UPDATE) {
      lua_mesh_push_contact_update(L, ev.sender, ev.hops);
    }
  }
}

void loop() {
  // Core 0 (UI domain) — LVGL + Lua + input. The mesh dispatcher runs on
  // Core 1 via mesh_task (see meshpunk_tasks.cpp).

  // Handle LVGL tasks
  lv_timer_handler();

  // Audio: file streaming (SD needs SPI mutex)
  if (active_file_is_sd) {
    sd_spi_take();
    audio->loop();
    sd_spi_release();
  } else {
    audio->loop();
  }

  sound_tone_tick();

  // GPS one-shot time sync runs on Core 1 (gps_task). Nothing to do here.

  // Flush mesh RX events into Lua. lua_State is single-threaded — always
  // touched from Core 0.
  drain_rx_events();
}
