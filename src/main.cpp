#include "boards/board_pins.h"
#include <Arduino.h>
#include <HTTPClient.h>
#include <LittleFS.h>
#include <SD.h>
#include <Ticker.h> // Include ticker for LVGL timing
#include <WiFi.h>
#include <Wire.h>
#include <esp_heap_caps.h> // DMA-capable buffer allocation for LVGL
#include <esp_sleep.h>     // standby light sleep (standby_run)
#include <driver/gpio.h>   // gpio_wakeup_enable for the standby wake pins
#include <driver/rtc_io.h> // RTC-domain pull for the standby ext0 wake pin
#include <mbedtls/platform.h> // runtime override of mbedTLS allocator (TLS -> PSRAM)
#include "multi_heap.h" // ESP-IDF arena allocator for the Lua PSRAM arena
#include <lvgl.h>
#include "theme/lv_theme_meshpunk.h"
#include "emoji_font.h"
#include "theme_font.h"
#include "audio/audio_dev.h"
#include "display/display_dev.h"
#include "gps/gps_dev.h"
#include "input/input_dev.h"
#include "input/input_ui.h"
#include "input/input_zones.h"
#include "power/power_dev.h"
#include "meshpunk_sync.h"
#include "Audio.h"
#include "sound.h"
#include "notify.h"
#include "ble_companion.h"
#include "version.h"
#include "elf_host.h"
#include "meshpunk_fs.h"
#include "fs_bridge.h"
#include "img_bridge.h"
#include "usb_manager.h"
#include "usb_fs.h"
#include "tdeck_link.h"

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
// Resets GPS state and re-opens serial for a fresh sync cycle. `manual` picks
// the longer location-hunt budget (boot / user-triggered syncs).
void gps_sync_restart(bool manual);


#include <esp_random.h>

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
RADIO_CLASS radio = new Module(PIN_LORA_CS, PIN_LORA_DIO1, PIN_LORA_RST, PIN_LORA_BUSY);

// Meshcore
StdRNG fast_rng;
SimpleMeshTables tables;

#if defined(BOARD_HELTEC_V4)
#include "boards/punk_heltec_board.h"
PunkHeltecBoard board;   // FEM TX/RX switching + Heltec battery circuit
#else
ESP32Board board;
#endif
PunkSX1262Wrapper radio_driver(radio, board);

// TX power is RADIATED dBm everywhere in the UI and prefs; boards with a PA
// front-end map it to the chip's own output at the two setOutputPower choke
// points below.
static inline int8_t board_tx_dbm_to_chip(int8_t radiated) {
#if defined(BOARD_HELTEC_V4)
  return heltec_radiated_to_chip_dbm(radiated);
#else
  return radiated;
#endif
}
PunkMesh* the_mesh = nullptr;

// Live radio reconfiguration (declared in meshpunk_sync.h). Holding SPI_LOCK
// across the whole sequence keeps the dispatcher's recvRaw() from re-arming
// RX between the standby and the last set* call; once released, the next
// dispatcher pass re-enters RX with the new params (state was forced IDLE).
void radio_apply_params(float freq_mhz, float bw_khz, uint8_t sf, uint8_t cr) {
  SPI_LOCK();
  radio_driver.standbyForConfig();
  int16_t s1 = radio.setFrequency(freq_mhz);
  int16_t s2 = radio.setBandwidth(bw_khz);
  int16_t s3 = radio.setSpreadingFactor(sf);
  int16_t s4 = radio.setCodingRate(cr);
  SPI_UNLOCK();
  SLog.printf("[RADIO] live params: %.3f MHz BW=%.1f SF=%u CR=%u (%d,%d,%d,%d)\n",
              freq_mhz, bw_khz, sf, cr, s1, s2, s3, s4);
}

void radio_apply_tx_power(int8_t dbm) {
  int8_t chip = board_tx_dbm_to_chip(dbm);
  SPI_LOCK();
  int16_t s = radio.setOutputPower(chip);
  SPI_UNLOCK();
  SLog.printf("[RADIO] live tx power: %d dBm radiated (chip %d) (%d)\n",
              (int)dbm, (int)chip, s);
}

// One-shot GPS time sync: poll in loop() until first fix, then stop.
// The UART itself lives in the GPS transport backend (gps/gps_dev.h); the
// clock-authority tiers, sky detector and cadence below are device-neutral.
static TinyGPSPlus gps_tinygps;
// Satellites-in-view via GSV field 3 per talker (the built-in `satellites`
// field is GGA sats-USED — zero until a fix exists, useless as a sky signal).
// NOTE: gps_sync_restart() placement-news gps_tinygps, which wipes custom
// registrations — each restart must re-begin() these.
static TinyGPSCustom gps_gsv_inview_gp;
static TinyGPSCustom gps_gsv_inview_ga;
static TinyGPSCustom gps_gsv_inview_gb;
static uint16_t gps_inview_val[3] = {0, 0, 0};
static uint32_t gps_inview_ms[3]  = {0, 0, 0};
static uint32_t gps_sky_ok_ms = 0;          // last time >=4 sats were in view
// Best position candidate this cycle (for the HDOP gate's best-effort path)
static bool   gps_have_cand = false;
static double gps_cand_lat = 0.0, gps_cand_lng = 0.0;
static float  gps_cand_hdop = 99.0f;
static bool gps_sync_done = false;
static uint32_t gps_sync_start_ms = 0;
static uint32_t gps_last_stats_ms = 0;
static uint32_t gps_last_chars = 0;
static uint32_t gps_fix_acquired_ms = 0;    // when the time fix was captured (location hunt starts here)
static const uint32_t GPS_SYNC_TIMEOUT_MS = 600000;   // 10 min cold-start budget
static const uint32_t GPS_STATS_INTERVAL_MS = 5000;   // print status every 5s
static const uint32_t GPS_POST_FIX_MS = 2000;         // grace after a location fix to collect sat count
// Location-hunt budget after the time fix. RMC time alone can come from the
// module's free-running clock with zero satellites tracked (status V), so the
// receiver needs real airtime to acquire a position. (No standby command is
// sent at cycle end: hw-verified 2026-07-13 that this module rejects every
// known standby dialect and the receiver is rail-powered — it never sleeps.)
static const uint32_t GPS_LOC_HUNT_MANUAL_MS = 120000; // boot / user-triggered sync
static const uint32_t GPS_LOC_HUNT_AUTO_MS   = 60000;  // background cycle
// Sky detector: with <4 satellites in view a position fix is impossible.
// Abort hunts early instead of burning the full budget indoors.
static const uint32_t GPS_NO_SKY_LOC_ABORT_MS  = 20000;  // during location hunt
static const uint32_t GPS_NO_SKY_TIME_ABORT_MS = 60000;  // during time hunt (time is mesh-critical, longer leash)
static const float    GPS_HDOP_ACCEPT = 5.0f;  // accept a fix outright below this

// Timezone state — "auto" uses longitude-from-GPS; otherwise a fixed offset in minutes.
static bool    gps_location_valid_at_fix = false;
static double  gps_lng_at_fix = 0.0;
static double  gps_lat_at_fix = 0.0;
static bool    gps_time_fix_valid = false;   // true if last cycle got a time fix (not timeout)
static bool    gps_manual_time_override = false;  // informational for the Settings UI; gating lives in the clock tiers
static uint32_t gps_sats_at_fix = 0;
static uint32_t gps_hdop_at_fix = 0;        // HDOP * 100 (TinyGPSPlus integer representation)
static uint32_t gps_loc_hunt_ms = GPS_LOC_HUNT_MANUAL_MS; // this cycle's hunt budget
static bool     gps_loc_fixed_this_cycle = false;  // location fix landed THIS cycle
static uint32_t gps_loc_fix_ms = 0;                // when it landed (for sat-count grace)
static bool     gps_no_sky_this_cycle = false;     // cycle ended via the sky detector
static uint8_t  gps_fail_streak = 0;               // consecutive cycles without a location fix

// ── Clock authority tiers (see meshpunk_sync.h for the tier table) ─────────
// State is guarded by MESH_LOCK inside meshpunk_set_clock(); everything else
// only reads it for logging.
static int8_t   clock_cur_tier    = -1;    // -1 = clock never tier-set this boot
static uint32_t clock_tier_set_ms = 0;
static uint32_t gps_last_saved_epoch = 0;  // time persisted in last_gps = lower bound on reality
static const uint32_t CLOCK_TIER_DECAY_MS = 12UL * 3600UL * 1000UL;
static const uint32_t CLOCK_EPOCH_FLOOR   = 1704067200UL;  // 2024-01-01: anything earlier is garbage

static int8_t clock_effective_tier() {
  if (clock_cur_tier < 0) return -1;
  uint32_t steps = (millis() - clock_tier_set_ms) / CLOCK_TIER_DECAY_MS;
  if (steps > 4) steps = 4;
  int8_t eff = clock_cur_tier - (int8_t)steps;
  return eff < 0 ? 0 : eff;
}

bool meshpunk_set_clock(uint8_t tier, uint32_t epoch, const char* src) {
  if (!the_mesh) return false;
  if (epoch < CLOCK_EPOCH_FLOOR) {
    SLog.printf("[CLOCK] %s tier%u REJECTED: implausible epoch %u\n", src, tier, (unsigned)epoch);
    return false;
  }

  MESH_LOCK();
  uint32_t cur = the_mesh->getRTCClock()->getCurrentTime();
  int8_t eff = clock_effective_tier();
  bool accept;

  if ((int8_t)tier > eff) {
    accept = true;
    // Seeds are stale by definition: never move an already-plausible clock
    // backwards (covers the contacts-bootstrap value that lands pre-tier).
    if (tier == CLOCK_TIER_SEED && cur >= CLOCK_EPOCH_FLOOR && epoch <= cur) accept = false;
  } else if ((int8_t)tier == eff) {
    // GPS-fix and manual re-apply freely (continuous refinement / user says
    // so); phone, V-time and seeds are forward-only with a small slack.
    accept = (tier >= CLOCK_TIER_GPSFIX) || (epoch + 5 >= cur);
  } else {
    accept = false;
  }

  // V-time garbage gates: the module's free-running clock must not sit below
  // persisted reality, nor step a running V-time/better clock backwards.
  if (accept && tier == CLOCK_TIER_VTIME) {
    if (gps_last_saved_epoch >= CLOCK_EPOCH_FLOOR && epoch + 60 < gps_last_saved_epoch) {
      accept = false;
    } else if (eff >= CLOCK_TIER_VTIME && epoch + 5 < cur) {
      accept = false;
    }
  }

  if (accept) {
    the_mesh->getRTCClock()->setCurrentTime(epoch);
    clock_cur_tier = (int8_t)tier;
    clock_tier_set_ms = millis();
  }
  MESH_UNLOCK();

  SLog.printf("[CLOCK] %s tier%u %s %u (delta %+lds, eff tier was %d)\n",
              src, tier, accept ? "set" : "REJECTED", (unsigned)epoch,
              (long)((int64_t)epoch - (int64_t)cur), (int)eff);
  return accept;
}
static bool    tz_is_auto = true;
static int32_t tz_manual_minutes = 0;
static String  tz_setting_str = "auto";
static bool    dst_enabled = false;

// Firmware-level preferences (unified in /firmware_prefs)
static bool   use_sd_pref = true;
static String clock_fmt_str = "12";
static bool   ble_enabled_pref = true;
bool   ble_bond_clear_pref = false;
// Newest messages the companion sync serves per conversation file (0 = all).
static uint16_t ble_sync_max_per_channel = 100;
static bool   wifi_enabled_pref = true;
// Saved WiFi networks (multi-slot). /wifi_creds holds alternating ssid/pass
// lines, so the legacy single-network file (2 lines) reads as one entry.
#define WIFI_MAX_NETS 8
static String wifi_saved_ssid[WIFI_MAX_NETS];
static String wifi_saved_pass[WIFI_MAX_NETS];
static int    wifi_saved_count = 0;

// ── Audio ─────────────────────────────────────────────────────────────────
static Audio*    audio = nullptr;          // ESP32-audioI2S player, created in setup()

// ── Keyboard Backlight ─────────────────────────────────────────────────────
static uint8_t kbd_brightness = 200;  // 0–255, persisted

// ── Display Backlight ──────────────────────────────────────────────────────
static uint8_t display_brightness = 16;  // 0–16, persisted

// ── Inactivity Timeouts ───────────────────────────────────────────────────
static uint16_t screen_timeout_secs  = 60;  // 0 = never, persisted
static uint16_t kbd_timeout_secs     = 55;  // 0 = never, persisted
static uint16_t msg_retain_days      = 30;  // days of message/routing history (0 = unlimited)

static uint32_t last_activity_ms     = 0;
static bool     screen_timed_out     = false;
static bool     kbd_timed_out        = false;

// ── Power menu requests ───────────────────────────────────────────────────
// Set by the _system_poweroff/_system_standby bindings, handled at the top
// of loop() so the action runs on a clean Core-0 stack after the Lua caller
// has returned and painted its farewell. Handlers: system_shutdown() /
// standby_run(), defined above loop().
static volatile bool s_poweroff_request = false;
static volatile bool s_standby_request  = false;
extern volatile bool mesh_task_paused;   // meshpunk_tasks.cpp (link/USB sessions)
extern void gps_notify_wake();           // meshpunk_tasks.cpp

// ── Notification Preferences ─────────────────────────────────────────────
static bool     standby_heartbeat    = true;   // periodic kbd glow in standby
                                               // (device setting)
static uint16_t standby_heartbeat_secs = 30;   // min seconds between glows
static bool     auto_standby         = false;  // enter standby on idle timeout
static uint16_t auto_standby_mins    = 15;     // idle minutes before auto standby
                                               // (15/30/60, device setting)
static bool     notify_kbd_enabled   = true;   // keyboard blink on DM / @mention
static bool     notify_sound_enabled = true;   // melody on DM / @mention

// Accessors for notify.cpp (the C-side alert path) — these globals are
// file-static, and the alert fires from the mesh task, so it reads the live
// values through here rather than snapshotting them.
bool    firmware_notify_kbd_enabled()   { return notify_kbd_enabled; }
bool    firmware_notify_sound_enabled() { return notify_sound_enabled; }
uint8_t firmware_kbd_brightness()       { return kbd_brightness; }
bool    firmware_kbd_timed_out()        { return kbd_timed_out; }

// Timestamp source for notify_post record stamps — the same RTC the _rtc_time
// binding reads (our clock authority; a sender's timestamp is never used).
uint32_t firmware_rtc_epoch() {
  return the_mesh ? the_mesh->getRTCClock()->getCurrentTime() : 0;
}

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

// User-default runtime fonts per role ("" = the bundled Noto Sans). Set from
// Settings > Fonts; a theme's own set_font overrides these while active.
static String font_ui_pref = "";
static String font_text_pref = "";

static void write_firmware_prefs(fs::FS& fs, const char* path) {
  File f = fs.open(path, "w", true);
  if (!f) { SLog.printf("[FW_PREFS] cannot write %s\n", path); return; }
  f.printf("use_sd=%d\n", use_sd_pref ? 1 : 0);
  f.printf("tz=%s\n", tz_setting_str.c_str());
  f.printf("clock_fmt=%s\n", clock_fmt_str.c_str());
  f.printf("dst=%d\n", dst_enabled ? 1 : 0);
  f.printf("sound_vol=%d\n",   sound_get_volume());
  f.printf("sound_muted=%d\n", sound_get_muted() ? 1 : 0);
  f.printf("usb_audio=%d\n",   usb_audio_pref_get() ? 1 : 0);
  f.printf("usb_speaker=%d\n", usb_speaker_pref_get() ? 1 : 0);
  f.printf("kbd_bright=%d\n", kbd_brightness);
  f.printf("disp_bright=%d\n", display_brightness);
  f.printf("screen_timeout=%d\n", screen_timeout_secs);
  f.printf("kbd_timeout=%d\n", kbd_timeout_secs);
  f.printf("msg_retain_days=%d\n", msg_retain_days);
  f.printf("standby_heartbeat=%d\n", standby_heartbeat ? 1 : 0);
  f.printf("standby_heartbeat_secs=%d\n", standby_heartbeat_secs);
  f.printf("auto_standby=%d\n", auto_standby ? 1 : 0);
  f.printf("auto_standby_mins=%d\n", auto_standby_mins);
  f.printf("notify_kbd=%d\n", notify_kbd_enabled ? 1 : 0);
  f.printf("notify_sound=%d\n", notify_sound_enabled ? 1 : 0);
  f.printf("ble_enabled=%d\n", ble_enabled_pref ? 1 : 0);
  f.printf("ble_bond_clear=%d\n", ble_bond_clear_pref ? 1 : 0);
  f.printf("ble_sync_max=%d\n", ble_sync_max_per_channel);
  f.printf("wifi_enabled=%d\n", wifi_enabled_pref ? 1 : 0);
  f.printf("trackball_sens=%d\n", input_ui_trackball_sens_get());
  f.printf("trackball_roll=%d\n", input_ui_trackball_roll_get());
  f.printf("sym_toggle=%d\n", input_ui_sym_toggle_get() ? 1 : 0);
  f.printf("alt_toggle=%d\n", input_ui_alt_toggle_get() ? 1 : 0);
  f.printf("kb_legacy=%d\n", input_dev_kbd_legacy_get() ? 1 : 0);
  f.printf("theme=%s\n", theme_pref_str.c_str());
  f.printf("font_ui=%s\n", font_ui_pref.c_str());
  f.printf("font_text=%s\n", font_text_pref.c_str());
  f.printf("topbar_transparant=%d\n", topbar_transparant ? 1 : 0);
  f.printf("sel_solid=%d\n", theme_focus_solid ? 1 : 0);
  f.printf("sel_darken=%d\n", theme_focus_darken ? 1 : 0);

  f.close();
  SLog.printf("[FW_PREFS] saved to %s\n", path);
}

static void firmware_prefs_save() {
  // The LittleFS (internal flash) write must be bracketed by the USB flash
  // guard: a flash write disables the cache and stalls both cores for ms,
  // which crashes an active USB host audio stream. The guard drains/pauses the
  // ISO stream around it (no-op when USB isn't streaming). The SD copy is SPI
  // (no cache stall), so it keeps streaming normally.
  //
  // Written ATOMICALLY (tmp + rename; littlefs rename replaces the target in
  // one commit): a crash/reset mid-write must never leave a truncated
  // /firmware_prefs — that reset every setting to defaults once (2026-07-07,
  // device crashed during a volume save while USB audio was wedged).
  {
    UsbFlashGuard _g;
    write_firmware_prefs(LittleFS, "/firmware_prefs.tmp");
    if (!LittleFS.rename("/firmware_prefs.tmp", "/firmware_prefs"))
      SLog.println("[FW_PREFS] rename failed — prefs NOT updated");
  }
  if (sd_mounted && use_sd_pref) {
    sd_spi_take();
    write_firmware_prefs(SD, "/meshpunk/firmware_prefs");
    sd_spi_release();
  }
}

static void write_wifi_creds(fs::FS& fs, const char* path) {
  File f = fs.open(path, "w", true);
  if (!f) { SLog.printf("[WIFI_CREDS] cannot write %s\n", path); return; }
  for (int i = 0; i < wifi_saved_count; i++) {
    f.println(wifi_saved_ssid[i].c_str());
    f.println(wifi_saved_pass[i].c_str());
  }
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
  wifi_saved_count = 0;
  while (f.available() && wifi_saved_count < WIFI_MAX_NETS) {
    String ssid = f.readStringUntil('\n'); ssid.trim();
    String pass = f.readStringUntil('\n'); pass.trim();
    if (ssid.length() == 0) continue;   // blank line / trailing newline
    wifi_saved_ssid[wifi_saved_count] = ssid;
    wifi_saved_pass[wifi_saved_count] = pass;
    wifi_saved_count++;
  }
  f.close();
  if (wifi_saved_count > 0) {
    SLog.printf("[WIFI_CREDS] loaded %d saved network(s)\n", wifi_saved_count);
  }
}

static void wifi_creds_clear() {
  for (int i = 0; i < wifi_saved_count; i++) {
    wifi_saved_ssid[i] = "";
    wifi_saved_pass[i] = "";
  }
  wifi_saved_count = 0;
  LittleFS.remove("/wifi_creds");
  if (sd_mounted && use_sd_pref) {
    sd_spi_take();
    SD.remove("/meshpunk/wifi_creds");
    sd_spi_release();
  }
}

static int wifi_creds_find(const char *ssid) {
  for (int i = 0; i < wifi_saved_count; i++) {
    if (wifi_saved_ssid[i].equals(ssid)) return i;
  }
  return -1;
}

// Add or update a saved network. At capacity the oldest entry is evicted.
static void wifi_creds_upsert(const char *ssid, const char *pass) {
  int idx = wifi_creds_find(ssid);
  if (idx < 0) {
    if (wifi_saved_count >= WIFI_MAX_NETS) {
      SLog.printf("[WIFI_CREDS] full — dropping oldest (%s)\n", wifi_saved_ssid[0].c_str());
      for (int i = 1; i < wifi_saved_count; i++) {
        wifi_saved_ssid[i - 1] = wifi_saved_ssid[i];
        wifi_saved_pass[i - 1] = wifi_saved_pass[i];
      }
      wifi_saved_count--;
    }
    idx = wifi_saved_count++;
    wifi_saved_ssid[idx] = ssid;
  }
  wifi_saved_pass[idx] = pass;
  wifi_creds_save();
}

static bool wifi_creds_forget(const char *ssid) {
  int idx = wifi_creds_find(ssid);
  if (idx < 0) return false;
  for (int i = idx + 1; i < wifi_saved_count; i++) {
    wifi_saved_ssid[i - 1] = wifi_saved_ssid[i];
    wifi_saved_pass[i - 1] = wifi_saved_pass[i];
  }
  wifi_saved_count--;
  wifi_saved_ssid[wifi_saved_count] = "";
  wifi_saved_pass[wifi_saved_count] = "";
  wifi_creds_save();
  return true;
}

// ── WiFi auto-connect: bounded rounds ───────────────────────────────────────
// The Arduino stack's own auto-reconnect retries an unreachable network
// forever — nonstop scan+auth attempts that drain the battery, and while the
// STA is mid-connect esp_wifi_scan_start() fails, which is why the Wireless
// app showed "No networks found" whenever a network was saved. So:
// auto-reconnect is OFF (setup() calls WiFi.setAutoReconnect(false)) and all
// connect policy lives here as bounded rounds: one async scan, then one
// begin() per known network heard in the scan, strongest first. If nothing
// connects the radio is parked (WIFI_OFF) until the next trigger — boot,
// WiFi toggled on, a scan/join in the Wireless app, or an app calling
// _wifi_auto_connect (the downloader does before fetching). After an
// unexpected AP loss one grace round runs ~10s later; if that fails the
// radio parks rather than retrying forever.
//
// Everything here runs on Core 0 (loop()/Lua context) — the same thread as
// the Lua WiFi bindings, so no locking is needed.
enum WifiAutoState : uint8_t { WA_IDLE, WA_SCANNING, WA_CONNECTING };
static WifiAutoState wa_state = WA_IDLE;
static uint32_t wa_deadline = 0;         // current phase timeout (millis)
static int      wa_cand[WIFI_MAX_NETS];  // saved-cred indices, strongest first
static int      wa_cand_count = 0;
static int      wa_cand_next = 0;
static uint8_t  wa_scan_retries = 0;
static bool     wa_user_scan = false;     // Wireless app is waiting on this scan
static bool     wa_was_connected = false; // successful connect since last failure
static uint32_t wa_reconnect_at = 0;      // pending grace round after AP loss

static void wifi_radio_park() {
  UsbFlashGuard _g;
  WiFi.disconnect(true);   // true = radio off too
  SLog.println("[WIFI] no known network reachable — radio parked");
}

// Start a connect round (scan phase). Returns false when there is nothing to
// do (disabled / no saved networks); true when connected or a round is going.
static bool wifi_auto_kick() {
  if (!wifi_enabled_pref || wifi_saved_count == 0) return false;
  if (WiFi.status() == WL_CONNECTED) return true;
  if (wa_state != WA_IDLE) return true;   // round already in the works
  {
    UsbFlashGuard _g;          // mode/begin can write PHY cal to NVS
    WiFi.mode(WIFI_STA);       // radio may be parked
    WiFi.disconnect();         // abort any in-flight begin() so the scan can start
    WiFi.scanNetworks(true);   // async; tick retries if it couldn't start yet
  }
  wa_state = WA_SCANNING;
  wa_deadline = millis() + 12000;
  wa_scan_retries = 0;
  SLog.println("[WIFI] connect round: scanning for known networks");
  return true;
}

static void wifi_auto_fail_round() {
  wa_state = WA_IDLE;
  wa_was_connected = false;
  wa_reconnect_at = 0;
  wifi_radio_park();
}

// Scan finished with n results still in the driver: pick known networks,
// strongest first, and start connecting. Does NOT scanDelete — the caller
// owns the results (lua_wifi_scan_results also reads them for the UI).
static void wifi_auto_on_scan_done(int n) {
  wa_cand_count = 0;
  wa_cand_next = 0;
  bool seen[WIFI_MAX_NETS] = {false};
  int32_t rssi[WIFI_MAX_NETS];
  for (int i = 0; i < n; i++) {
    int idx = wifi_creds_find(WiFi.SSID(i).c_str());
    if (idx < 0 || seen[idx]) continue;
    seen[idx] = true;
    int32_t r = WiFi.RSSI(i);
    int pos = wa_cand_count++;
    while (pos > 0 && rssi[pos - 1] < r) {   // insertion sort, RSSI desc
      wa_cand[pos] = wa_cand[pos - 1];
      rssi[pos] = rssi[pos - 1];
      pos--;
    }
    wa_cand[pos] = idx;
    rssi[pos] = r;
  }
  if (WiFi.status() == WL_CONNECTED) {   // user scan while connected — done
    wa_state = WA_IDLE;
    return;
  }
  if (wa_cand_count == 0) {
    wifi_auto_fail_round();
    return;
  }
  int idx = wa_cand[wa_cand_next++];
  SLog.printf("[WIFI] connecting to %s (%d known network(s) in range)\n",
              wifi_saved_ssid[idx].c_str(), wa_cand_count);
  { UsbFlashGuard _g; WiFi.begin(wifi_saved_ssid[idx].c_str(), wifi_saved_pass[idx].c_str()); }
  wa_state = WA_CONNECTING;
  wa_deadline = millis() + 10000;
}

static void wifi_auto_tick() {
  static uint32_t next_ms = 0;
  uint32_t now = millis();
  if ((int32_t)(now - next_ms) < 0) return;
  next_ms = now + 250;
  if (!wifi_enabled_pref) return;

  switch (wa_state) {
  case WA_IDLE: {
    if (WiFi.status() == WL_CONNECTED) {
      wa_was_connected = true;
      wa_reconnect_at = 0;
    } else if (wa_was_connected && wifi_saved_count > 0) {
      // AP dropped on us: one grace round after a short settle, then park.
      if (wa_reconnect_at == 0) {
        wa_reconnect_at = now + 10000;
        SLog.println("[WIFI] connection lost — grace round in 10s");
      } else if ((int32_t)(now - wa_reconnect_at) >= 0) {
        wa_reconnect_at = 0;
        wa_was_connected = false;   // the grace round is one-shot
        wifi_auto_kick();
      }
    }
    break;
  }
  case WA_SCANNING: {
    int n = WiFi.scanComplete();
    bool expired = (int32_t)(now - wa_deadline) >= 0;
    if (n >= 0) {
      // A user scan's results are consumed by lua_wifi_scan_results (which
      // feeds them back here); only take over if the app never collects.
      if (!wa_user_scan || expired) {
        wa_user_scan = false;
        wifi_auto_on_scan_done(n);
        WiFi.scanDelete();
      }
    } else if (n == WIFI_SCAN_FAILED) {
      // Couldn't start (STA still tearing down a connect attempt) — retry.
      if (wa_scan_retries++ < 8) {
        WiFi.scanNetworks(true);
      } else {
        wa_user_scan = false;
        wifi_auto_fail_round();
      }
    } else if (expired) {   // stuck in WIFI_SCAN_RUNNING
      WiFi.scanDelete();
      wa_user_scan = false;
      wifi_auto_fail_round();
    }
    break;
  }
  case WA_CONNECTING: {
    wl_status_t st = WiFi.status();
    if (st == WL_CONNECTED) {
      wa_state = WA_IDLE;
      wa_was_connected = true;
      wa_reconnect_at = 0;
      SLog.printf("[WIFI] connected to %s (%s)\n",
                  WiFi.SSID().c_str(), WiFi.localIP().toString().c_str());
    } else if (st == WL_CONNECT_FAILED || st == WL_NO_SSID_AVAIL ||
               (int32_t)(now - wa_deadline) >= 0) {
      if (wa_cand_next < wa_cand_count) {
        int idx = wa_cand[wa_cand_next++];
        SLog.printf("[WIFI] trying next candidate: %s\n", wifi_saved_ssid[idx].c_str());
        { UsbFlashGuard _g; WiFi.begin(wifi_saved_ssid[idx].c_str(), wifi_saved_pass[idx].c_str()); }
        wa_deadline = now + 10000;
      } else {
        wifi_auto_fail_round();
      }
    }
    break;
  }
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
    } else if (strcmp(key, "usb_audio") == 0) {
      usb_audio_pref_set(atoi(val) == 1);
    } else if (strcmp(key, "usb_speaker") == 0) {
      usb_speaker_pref_set(atoi(val) == 1);
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
    } else if (strcmp(key, "standby_heartbeat") == 0) {
      standby_heartbeat = (atoi(val) == 1);
    } else if (strcmp(key, "standby_heartbeat_secs") == 0) {
      int v = atoi(val);
      if (v >= 5 && v <= 600) standby_heartbeat_secs = (uint16_t)v;
    } else if (strcmp(key, "auto_standby") == 0) {
      auto_standby = (atoi(val) == 1);
    } else if (strcmp(key, "auto_standby_mins") == 0) {
      int v = atoi(val);
      if (v >= 1 && v <= 600) auto_standby_mins = (uint16_t)v;
    } else if (strcmp(key, "notify_kbd") == 0) {
      notify_kbd_enabled = (atoi(val) == 1);
    } else if (strcmp(key, "notify_sound") == 0) {
      notify_sound_enabled = (atoi(val) == 1);
    } else if (strcmp(key, "ble_enabled") == 0) {
      ble_enabled_pref = (atoi(val) == 1);
    } else if (strcmp(key, "ble_bond_clear") == 0) {
      ble_bond_clear_pref = (atoi(val) == 1);
    } else if (strcmp(key, "ble_sync_max") == 0) {
      int v = atoi(val);
      if (v >= 0 && v <= 5000) ble_sync_max_per_channel = (uint16_t)v;
    } else if (strcmp(key, "wifi_enabled") == 0) {
      wifi_enabled_pref = (atoi(val) == 1);
    } else if (strcmp(key, "trackball_sens") == 0) {
      int v = atoi(val);
      if (v >= 0 && v <= 500) input_ui_trackball_sens_set((uint16_t)v);
    } else if (strcmp(key, "trackball_roll") == 0) {
      int v = atoi(val);
      if (v >= 0 && v <= 500) input_ui_trackball_roll_set((uint16_t)v);
    } else if (strcmp(key, "sym_toggle") == 0) {
      input_ui_sym_toggle_set(atoi(val) == 1);
    } else if (strcmp(key, "alt_toggle") == 0) {
      input_ui_alt_toggle_set(atoi(val) == 1);
    } else if (strcmp(key, "kb_legacy") == 0) {
      input_dev_kbd_legacy_load(atoi(val) == 1);
    } else if (strcmp(key, "theme") == 0) {
      theme_pref_str = String(val);
      theme_pref_str.trim();
    } else if (strcmp(key, "font_ui") == 0) {
      font_ui_pref = String(val);
      font_ui_pref.trim();
    } else if (strcmp(key, "font_text") == 0) {
      font_text_pref = String(val);
      font_text_pref.trim();
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

// GPS module identity: a u-blox MIA-M10Q, read off the T-Deck-GPS daughter
// board's U1 designator. GPS+GAL+BDS+QZSS, no GLONASS, NMEA 4.1 at 38400.
// Text command dialects do not work on it — a since-retired boot probe tried
// PMTK, PCAS, PAIR, PQTM and PDTINFO (hw 2026-07-13) and got nothing usable
// back. Its command language is UBX, which is what gps_dev_power_down /
// gps_dev_wake speak (gps_tdeck.cpp). The receiver is rail-powered with no
// control GPIO, so the UART is the only power lever.

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
                  tag, gps_dev_rx_pin());
  } else if (gps_tinygps.passedChecksum() == 0 && chars > 200 && gps_baud_locked) {
    SLog.printf("[GPS %s]   !! bytes flowing but 0 valid sentences at locked baud %u\n",
                  tag, (unsigned)GPS_BAUD_CANDIDATES[gps_baud_idx]);
  }
}

static void gps_start_probe_at_current_baud() {
  uint32_t baud = GPS_BAUD_CANDIDATES[gps_baud_idx];
  if (gps_serial_active) {
    gps_dev_update_baud(baud);
  } else {
    gps_dev_begin(baud);
    gps_dev_chip_init();
    gps_serial_active = true;
  }
  gps_dev_stream().flush();
  gps_baud_probe_start_ms = millis();
  gps_baud_probe_chars_start = gps_tinygps.charsProcessed();
  SLog.printf("[GPS] probing baud=%u (candidate %u/%u)\n",
                (unsigned)baud, (unsigned)(gps_baud_idx + 1), (unsigned)GPS_BAUD_COUNT);
}

static void gps_sync_begin() {
  SLog.printf("[GPS] Listening on UART1 RX=%d TX=%d\n",
              gps_dev_rx_pin(), gps_dev_tx_pin());
  gps_sync_restart(true);
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
  if (t >= CLOCK_EPOCH_FLOOR) gps_last_saved_epoch = t;   // fresh reality lower bound
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

  if (t > 86400) {  // >1 day past epoch = a real saved time
    gps_last_saved_epoch = t;
    meshpunk_set_clock(CLOCK_TIER_SEED, t, "last_gps-seed");
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
  { Stream& gs = gps_dev_stream();
    while (gs.available()) gps_tinygps.encode(gs.read()); }

  uint32_t now = millis();

  // Sky detector: freshest satellites-in-view across the GSV talkers.
  if (gps_gsv_inview_gp.isUpdated()) { gps_inview_val[0] = atoi(gps_gsv_inview_gp.value()); gps_inview_ms[0] = now; }
  if (gps_gsv_inview_ga.isUpdated()) { gps_inview_val[1] = atoi(gps_gsv_inview_ga.value()); gps_inview_ms[1] = now; }
  if (gps_gsv_inview_gb.isUpdated()) { gps_inview_val[2] = atoi(gps_gsv_inview_gb.value()); gps_inview_ms[2] = now; }
  uint16_t sky_inview = 0;
  for (int i = 0; i < 3; i++) {
    if (gps_inview_ms[i] && now - gps_inview_ms[i] < 5000 && gps_inview_val[i] > sky_inview)
      sky_inview = gps_inview_val[i];
  }
  if (sky_inview >= 4) gps_sky_ok_ms = now;

  // ── Location hunt: clock is set; keep reading until a real position fix ──
  // The time fix alone is NOT proof of acquisition: TinyGPSPlus commits RMC
  // date/time even with status V (the module's free-running clock, zero sats
  // tracked). Location only commits on status A / GGA quality > 0, so hunt
  // for that, bounded by the budget and the sky detector.
  if (gps_time_fix_valid) {
    if (gps_tinygps.satellites.isValid() && gps_tinygps.satellites.value() > 0) {
      gps_sats_at_fix = gps_tinygps.satellites.value();
      gps_hdop_at_fix = gps_tinygps.hdop.isValid() ? gps_tinygps.hdop.value() : 0;
    }

    if (!gps_loc_fixed_this_cycle && gps_tinygps.location.isValid()) {
      float hd = gps_tinygps.hdop.isValid() ? gps_tinygps.hdop.hdop() : 99.0f;
      // Track the best candidate seen; accept outright only below the HDOP
      // gate so one sloppy first fix can't stamp a bad position.
      if (!gps_have_cand || hd < gps_cand_hdop) {
        gps_cand_lat = gps_tinygps.location.lat();
        gps_cand_lng = gps_tinygps.location.lng();
        gps_cand_hdop = hd;
        gps_have_cand = true;
      }
      if (hd < GPS_HDOP_ACCEPT) {
        gps_lat_at_fix = gps_tinygps.location.lat();
        gps_lng_at_fix = gps_tinygps.location.lng();
        gps_location_valid_at_fix = true;
        gps_loc_fixed_this_cycle = true;
        gps_loc_fix_ms = now;
        // Fix-true time upgrades the earlier V-time authority.
        if (gps_tinygps.date.isValid() && gps_tinygps.time.isValid()) {
          DateTime utc(gps_tinygps.date.year(), gps_tinygps.date.month(), gps_tinygps.date.day(),
                       gps_tinygps.time.hour(), gps_tinygps.time.minute(), gps_tinygps.time.second());
          meshpunk_set_clock(CLOCK_TIER_GPSFIX, utc.unixtime(), "gps-fix");
        }
        SLog.printf("[GPS] location fix after %lus (hdop=%.2f)\n",
                      (unsigned long)((now - gps_sync_start_ms) / 1000UL), hd);
        SLog.printf("[TZ] captured lat=%.5f lng=%.5f -> auto offset=%d min\n",
                      gps_lat_at_fix, gps_lng_at_fix, (int)tz_auto_offset_minutes());
      }
    }

    if (now - gps_last_stats_ms >= GPS_STATS_INTERVAL_MS) {
      gps_last_stats_ms = now;
      // gps_print_stats("loc-hunt");
    }

    bool no_sky   = (now - gps_sky_ok_ms >= GPS_NO_SKY_LOC_ABORT_MS);
    bool loc_done = gps_loc_fixed_this_cycle
                    && (gps_sats_at_fix > 0 || now - gps_loc_fix_ms >= GPS_POST_FIX_MS);
    bool gave_up  = !gps_loc_fixed_this_cycle
                    && ((now - gps_fix_acquired_ms >= gps_loc_hunt_ms) || no_sky);
    if (loc_done || gave_up) {
      if (gave_up) {
        // Salvage the best high-HDOP candidate rather than report nothing.
        if (gps_have_cand) {
          gps_lat_at_fix = gps_cand_lat;
          gps_lng_at_fix = gps_cand_lng;
          gps_location_valid_at_fix = true;
          gps_loc_fixed_this_cycle = true;
          SLog.printf("[GPS] best-effort fix accepted at hunt end (hdop=%.2f)\n", gps_cand_hdop);
        } else if (no_sky) {
          gps_no_sky_this_cycle = true;
          SLog.printf("[GPS] no usable sky for %lus; ending location hunt early.\n",
                        (unsigned long)(GPS_NO_SKY_LOC_ABORT_MS / 1000UL));
        } else {
          SLog.printf("[GPS] no location fix within %lus.\n",
                        (unsigned long)(gps_loc_hunt_ms / 1000UL));
        }
      }
      // Persist for the next cold boot: fresh time + best location we have
      // (this cycle's fix, or the carried-over last known). Outside MESH_LOCK.
      uint32_t rtc_now;
      MESH_LOCK();
      rtc_now = the_mesh->getRTCClock()->getCurrentTime();
      MESH_UNLOCK();
      gps_last_save(gps_lat_at_fix, gps_lng_at_fix, gps_location_valid_at_fix, rtc_now);
      gps_print_stats("fix-final");
      gps_sync_done = true;
    }
    return;
  }

  // ── Normal hunt phase ──
  gps_baud_probe_tick();

  if (now - gps_last_stats_ms >= GPS_STATS_INTERVAL_MS) {
    gps_last_stats_ms = now;
    // gps_print_stats("stat");
  }

  if (gps_tinygps.date.isValid() && gps_tinygps.time.isValid()
      && gps_tinygps.date.year() >= 2024) {
    DateTime utc(gps_tinygps.date.year(), gps_tinygps.date.month(), gps_tinygps.date.day(),
                 gps_tinygps.time.hour(), gps_tinygps.time.minute(), gps_tinygps.time.second());
    // This time may be the module's free-running clock (RMC status V), not a
    // satellite fix — mesh needs time ASAP, so take it, but only at V-time
    // authority: the tier engine keeps it from stomping phone/fix/manual time
    // and from stepping the clock backwards. A real fix upgrades it below.
    meshpunk_set_clock(CLOCK_TIER_VTIME, utc.unixtime(), "gps-vtime");

    gps_fix_acquired_ms = now;
    gps_time_fix_valid = true;
    gps_sats_at_fix = gps_tinygps.satellites.isValid() ? gps_tinygps.satellites.value() : 0;
    gps_hdop_at_fix  = gps_tinygps.hdop.isValid()      ? gps_tinygps.hdop.value()       : 0;

    SLog.println("[GPS] ======== TIME ACQUIRED — hunting for location fix ========");
    gps_print_stats("time-fix");
    SLog.printf("[GPS] gps time %04u-%02u-%02u %02u:%02u:%02u after %lus; loc hunt up to %lus\n",
                  gps_tinygps.date.year(), gps_tinygps.date.month(), gps_tinygps.date.day(),
                  gps_tinygps.time.hour(), gps_tinygps.time.minute(), gps_tinygps.time.second(),
                  (unsigned long)((now - gps_sync_start_ms) / 1000UL),
                  (unsigned long)(gps_loc_hunt_ms / 1000UL));
    return;
  }

  // No usable sky for a while and still no time → stop wasting the cycle.
  // (Time needs only one satellite, so this leash is longer than the
  // location hunt's, but with zero birds in view nothing can decode.)
  if (now - gps_sky_ok_ms >= GPS_NO_SKY_TIME_ABORT_MS) {
    SLog.printf("[GPS] no usable sky for %lus and no time — ending cycle early.\n",
                  (unsigned long)(GPS_NO_SKY_TIME_ABORT_MS / 1000UL));
    gps_print_stats("no-sky");
    gps_no_sky_this_cycle = true;
    gps_sync_done = true;
    return;
  }

  if (now - gps_sync_start_ms > GPS_SYNC_TIMEOUT_MS) {
    SLog.println("[GPS] ======== TIMEOUT ========");
    gps_print_stats("timeout");
    SLog.printf("[GPS] No fix after %us. Move to open sky for cold start (can take 30s-5min+).\n",
                  (unsigned)(GPS_SYNC_TIMEOUT_MS / 1000));
    gps_sync_done = true;
  }
}

// Last known GPS location (most recent real fix, or the boot seed; persists
// across sync cycles until a new fix replaces it). Read by the mesh task to
// stamp messages — see meshpunk_sync.h. Unlocked read of values that change
// only once per sync cycle; a rare torn read just yields a slightly-off
// coordinate, acceptable here.
bool meshpunk_gps_last_fix(double* lat, double* lon) {
  if (!gps_location_valid_at_fix) return false;
  if (lat) *lat = gps_lat_at_fix;
  if (lon) *lon = gps_lng_at_fix;
  return true;
}

// Next-cycle delay for gps_task, from this cycle's outcome. Since the
// receiver never sleeps (rail-powered, no standby — hw-verified), cycles
// cost nothing GPS-side; the backoff is CPU/log hygiene, and it means a
// device that CAN fix keeps its 5-minute cadence while one buried indoors
// backs off to half-hourly checks.
uint32_t gps_next_cycle_delay_ms() {
  if (gps_loc_fixed_this_cycle) {
    gps_fail_streak = 0;
    return 5UL * 60UL * 1000UL;
  }
  if (gps_fail_streak < 255) gps_fail_streak++;
  if (gps_time_fix_valid && !gps_no_sky_this_cycle)
    return 15UL * 60UL * 1000UL;   // time served, some sky — moderate cadence
  uint32_t mins = (gps_fail_streak >= 3) ? 30 : (gps_fail_streak == 2 ? 20 : 10);
  return mins * 60UL * 1000UL;
}

void gps_sync_restart(bool manual) {
  new (&gps_tinygps) TinyGPSPlus();
  // Placement-new wiped the custom-field registrations — re-attach them.
  gps_gsv_inview_gp.begin(gps_tinygps, "GPGSV", 3);
  gps_gsv_inview_ga.begin(gps_tinygps, "GAGSV", 3);
  gps_gsv_inview_gb.begin(gps_tinygps, "GBGSV", 3);
  if (gps_serial_active) {
    gps_dev_stream().write(0xFF);
    gps_dev_stream().flush();
  }
  gps_sync_done = false;
  gps_sync_start_ms = millis();
  gps_last_stats_ms = gps_sync_start_ms;
  gps_last_chars = 0;
  gps_fix_acquired_ms = 0;
  gps_baud_idx = 0;
  gps_baud_locked = false;
  gps_sky_ok_ms = gps_sync_start_ms;
  gps_inview_val[0] = gps_inview_val[1] = gps_inview_val[2] = 0;
  gps_inview_ms[0] = gps_inview_ms[1] = gps_inview_ms[2] = 0;
  gps_no_sky_this_cycle = false;
  gps_have_cand = false;
  gps_cand_hdop = 99.0f;
  if (manual) gps_fail_streak = 0;   // user asked: reset the backoff ladder
  // gps_location_valid_at_fix / lat / lng deliberately survive the restart:
  // they are the last-known position (map, message stamping, auto-tz) until a
  // new fix replaces them. Only per-cycle state resets here.
  gps_time_fix_valid = false;
  gps_loc_fixed_this_cycle = false;
  gps_loc_fix_ms = 0;
  gps_sats_at_fix = 0;
  gps_hdop_at_fix = 0;
  gps_loc_hunt_ms = manual ? GPS_LOC_HUNT_MANUAL_MS : GPS_LOC_HUNT_AUTO_MS;
  SLog.printf("[GPS] Restarting sync (%s, auto-baud, timeout=%us, loc hunt=%us)\n",
                manual ? "manual" : "auto",
                (unsigned)(GPS_SYNC_TIMEOUT_MS / 1000),
                (unsigned)(gps_loc_hunt_ms / 1000));
  gps_start_probe_at_current_baud();
}

// Data directory paths
#define LUA_PATH "/lua/"
#define SOUNDS_PATH "/sounds/"
#define IMAGES_PATH "/images/"

// Ticker for LVGL timing
Ticker lvgl_ticker;

// LuaVGL state
lua_State *L = NULL;

// Filesystem variables
bool fs_mounted = false;
bool sd_mounted = false;
// Label LittleFS actually mounted under: "assets" on current builds (both the
// CSV and merge_bin.py declare that label), "spiffs" only on devices whose
// partition table predates the rename. Internal size queries MUST use this, not
// the Arduino LittleFS wrapper's stored label (which the LVGL esp-littlefs
// driver clobbers to "spiffs"). See mp_littlefs_df.
const char* g_lfs_mount_label = "assets";


// sd_spi_take() / sd_spi_release() — mutex-based.
// TAKE (inline in meshpunk_sync.h) acquires SPI_LOCK.
// RELEASE releases SPI_LOCK. The historical TFT-reinit poke (SLPOUT/DISPON)
// that used to live here is gone: the bus mutex now serializes TFT access
// against SD and radio, so the display never observes a mid-transaction bus.
void sd_spi_release() {
  SPI_UNLOCK();
}

// Mount (or remount) the SD card and set sd_mounted. Called at boot and by
// USB drive mode's stop path (usb_msc_dev.cpp) after the PC releases the
// card. The SPI bus is shared with the TFT (80 MHz) and SX1262, but every
// device sets its own per-transaction SPISettings, so this clock only
// applies to SD transfers. 40 MHz cuts a 131KB map-tile read from ~400ms
// (4 MHz Arduino default) to ~50ms. Probe descending; 4 MHz floor = old
// behavior.
bool meshpunk_sd_mount() {
#if defined(PIN_SD_CS)
  static const uint32_t sd_freqs[] = {40000000U, 25000000U, 4000000U};
  sd_mounted = false;
  for (uint32_t freq : sd_freqs) {
    if (SD.begin(PIN_SD_CS, board_sd_spi(), freq)) {
      sd_mounted = true;
      SLog.printf("[SD] Mounted at %lu Hz\n", (unsigned long)freq);
      break;
    }
    SD.end();
    SLog.printf("[SD] Mount failed at %lu Hz\n", (unsigned long)freq);
  }
#else
  // No confirmed SD wiring for this board.
  sd_mounted = false;
  SLog.println("[SD] no SD pins defined for this board");
#endif
  return sd_mounted;
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

// File handle struct to track which filesystem a file belongs to.
// is_sd gates the SPI bus lock; is_flash gates the USB flash guard (LittleFS
// only — SD and USB writes never stall the cache). A U: file has both false.
// NOTE: sound.cpp carries a duplicate definition — keep them in sync.
struct LuaFileHandle {
    fs::File* file;
    bool is_sd;
    bool is_flash;
    bool is_write;   // opened "w"/"a": close/flush can write internal flash
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
  ud->is_flash = false;
  ud->is_write = false;
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
  ud->is_flash = mf.is_flash;
  // "r+" opens for update too — anything but a plain read can write flash.
  ud->is_write = (mode[0] != 'r') || (strchr(mode, '+') != nullptr);
  return 1;
}

// LVGL flush: the panel write (scanline tear-sync, bus lock, pixel push)
// lives in the display backend; this wrapper only unpacks the LVGL area and
// signals completion.
static void disp_flush_cb(lv_display_t *disp, const lv_area_t *area,
                          uint8_t *px_map) {
  display_dev_flush_rect(area->x1, area->y1,
                         area->x2 - area->x1 + 1,
                         area->y2 - area->y1 + 1,
                         (const uint16_t *)px_map);
  lv_display_flush_ready(disp);
}


// Reset inactivity timer and restore backlights — called after a native module
// exits so the screen/keyboard don't appear timed-out to the user.
void wake_activity() {
  last_activity_ms = millis();
  if (screen_timed_out) { display_dev_brightness(display_brightness); screen_timed_out = false; }
  if (kbd_timed_out)    { input_dev_kbd_backlight(kbd_brightness); kbd_timed_out = false; }
}

// ── Hooks for the input layer (declared in input/input_ui.h) ───────────────
// The indev callbacks live in input_ui.cpp; the activity/timeout policy and
// its state stay here.
void firmware_note_activity(void) {
  last_activity_ms = millis();
}

void firmware_wake_restore(void) {
  if (screen_timed_out) { display_dev_brightness(display_brightness); screen_timed_out = false; }
  if (kbd_timed_out)    { input_dev_kbd_backlight(kbd_brightness); kbd_timed_out = false; }
}

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
    // Release the messenger's gridnav edge-lock — a full nav teardown means we're
    // leaving that scope (view swap or app exit); never let the global flag leak
    // into another app's gridnav. (See lv_gridnav.c MESHPUNK.)
    extern bool meshpunk_gridnav_edge_lock;
    meshpunk_gridnav_edge_lock = false;
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

// ── Nav-scope services for the input layer (declared in input/input_ui.h) ──
// The indev callbacks moved to input_ui.cpp; the NavScope stack stays here.

// Housekeeping at the top of every indev read.
void nav_input_tick_begin(void) {
  flush_pending_gridnav();
  nav_check_valid();
}

// ── Re-enable gridnav on trackball input (top scope only) ──
void nav_rearm_on_trackball(void) {
  NavScope *kb_top = nav_top();
  if (kb_top && kb_top->cont &&
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
}

// Touch disarms gridnav on the top scope so the finger scrolls instead of
// moving focus; the trackball re-arms it (above).
void nav_disarm_on_touch(void) {
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

// The emoji imgfont every style points at; theme_font.cpp swaps its ->fallback
// to splice runtime TTFs into the chain (emoji -> TTF -> montserrat). NULL
// until setupLvgl(); may equal &lv_font_montserrat_14 (const — never written)
// when emoji-font creation failed, which disables the splice.
lv_font_t *g_ui_font = nullptr;

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
  const size_t BUF_SIZE =
      (size_t)display_dev_width() * BUF_LINES * sizeof(lv_color_t);

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
  lv_display_t *disp = lv_display_create(display_dev_width(), display_dev_height());

  // Set theme. Emoji font wraps montserrat_14 as fallback so ASCII/Latin still
  // render from the bitmap font; codepoints >= 0x2600 are loaded as PNGs from
  // S:/emoji/<hex>.png on the SD card.
  static lv_font_t * ui_font = emoji_font_create(16, &lv_font_montserrat_14);
  if (!ui_font) ui_font = (lv_font_t *)&lv_font_montserrat_14;
  g_ui_font = ui_font;   // theme_font.cpp splices runtime TTFs via ->fallback

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

  // Register the input devices (touch pointer + keypad when present)
  input_ui_setup_indevs(disp);
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

  wa_state = WA_IDLE;   // manual join overrides any auto round
  wa_reconnect_at = 0;
  {
    UsbFlashGuard _g;   // WiFi init can write PHY cal to NVS
    WiFi.mode(WIFI_STA);   // radio may be parked
    WiFi.begin(network, pass);
  }

  return 0;
}

// WiFi status function for Lua
static int lua_wifi_status(lua_State *L) {
  wl_status_t status = WiFi.status();
  const char *status_str = "unknown";

  if (WiFi.getMode() == WIFI_MODE_NULL) {
    // Radio parked (no known network reachable) — not an error state.
    lua_pushstring(L, "off");
    lua_pushstring(L, "");
    lua_pushstring(L, "");
    return 3;
  }
  if (status != WL_CONNECTED && wa_state == WA_CONNECTING) {
    // A connect round is joining a known network — report it as one state so
    // the UI doesn't flicker through disconnected/failed between candidates.
    lua_pushstring(L, "connecting");
    lua_pushstring(L, "");
    lua_pushstring(L, "");
    return 3;
  }

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
  wa_state = WA_IDLE;
  wa_was_connected = false;   // intentional disconnect — no grace round
  wa_reconnect_at = 0;
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
  // LittleFS target: every chunk write below is an internal-flash write, so
  // hold the USB flash guard across the whole body (pausing USB audio for the
  // download beats crashing the host stack; downloads are user-initiated and
  // rare). SD and USB targets skip it — neither write stalls the cache.
  UsbFlashGuardIf _dl_guard(mf.is_flash);
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
  {
    UsbFlashGuard _g;
    WiFi.mode(WIFI_STA);   // radio may be parked
    // esp_wifi_scan_start() fails while the STA is mid-connect — this is why
    // scans "found nothing" whenever a saved network was busy (re)connecting.
    // Abort the attempt first; the connect round restarts from these results.
    if (WiFi.status() != WL_CONNECTED) WiFi.disconnect();   // no-op when idle
    if (WiFi.scanComplete() != WIFI_SCAN_RUNNING) WiFi.scanNetworks(true);
  }
  // Fold the user scan into the state machine: it retries starts that failed
  // (disconnect needs a beat to land) and auto-joins known networks after.
  wa_state = WA_SCANNING;
  wa_deadline = millis() + 12000;
  wa_scan_retries = 0;
  wa_user_scan = true;
  lua_pushboolean(L, 1);
  return 1;
}

static int lua_wifi_scan_results(lua_State *L) {
  int n = WiFi.scanComplete();
  if (n == WIFI_SCAN_RUNNING) {
    lua_pushnil(L);
    return 1;
  }
  if (n == WIFI_SCAN_FAILED && wa_state == WA_SCANNING) {
    // Scan hasn't started yet (wifi_auto_tick is retrying) — report "still
    // scanning", not a bogus empty result. Once retries are exhausted the
    // machine leaves WA_SCANNING and this returns the empty table below.
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
  if (n >= 0) {
    if (wa_state == WA_SCANNING) wifi_auto_on_scan_done(n);
    WiFi.scanDelete();
  }
  wa_user_scan = false;
  return 1;
}

static int lua_wifi_get_enabled(lua_State *L) {
  lua_pushboolean(L, wifi_enabled_pref);
  return 1;
}

// _wifi_set_enabled(on [, persist])
// persist defaults to true. With persist = false the live radio and the
// in-RAM pref both change but nothing is written, so the next boot restores
// the saved state. Used to free internal SRAM for one ELF module run.
static int lua_wifi_set_enabled(lua_State *L) {
  wifi_enabled_pref = lua_toboolean(L, 1);
  bool persist = lua_isnoneornil(L, 2) ? true : (bool)lua_toboolean(L, 2);
  {
    // WiFi mode/connect can write PHY calibration to NVS (internal flash) —
    // pause any USB audio stream around it so the cache stall can't crash it.
    UsbFlashGuard _g;
    if (wifi_enabled_pref) {
      WiFi.mode(WIFI_STA);
    } else {
      wa_state = WA_IDLE;
      wa_was_connected = false;
      wa_reconnect_at = 0;
      WiFi.disconnect();
      WiFi.mode(WIFI_OFF);
    }
  }
  if (wifi_enabled_pref) wifi_auto_kick();
  if (persist) firmware_prefs_save();
  return 0;
}

// Returns the saved-network list: { {ssid=..., has_password=...}, ... }
static int lua_wifi_get_saved_creds(lua_State *L) {
  lua_newtable(L);
  for (int i = 0; i < wifi_saved_count; i++) {
    lua_newtable(L);
    lua_pushstring(L, wifi_saved_ssid[i].c_str());
    lua_setfield(L, -2, "ssid");
    lua_pushboolean(L, wifi_saved_pass[i].length() > 0);
    lua_setfield(L, -2, "has_password");
    lua_rawseti(L, -2, i + 1);
  }
  return 1;
}

static int lua_wifi_save_creds(lua_State *L) {
  const char *ssid = luaL_checkstring(L, 1);
  const char *pass = luaL_optstring(L, 2, "");
  wifi_creds_upsert(ssid, pass);
  return 0;
}

static int lua_wifi_clear_creds(lua_State *L) {
  wifi_creds_clear();
  return 0;
}

// _wifi_forget_cred(ssid) -> bool. Also drops the link if we're on that net.
static int lua_wifi_forget_cred(lua_State *L) {
  const char *ssid = luaL_checkstring(L, 1);
  bool removed = wifi_creds_forget(ssid);
  if (removed && WiFi.status() == WL_CONNECTED && WiFi.SSID().equals(ssid)) {
    wa_state = WA_IDLE;
    wa_was_connected = false;
    wa_reconnect_at = 0;
    WiFi.disconnect();
  }
  lua_pushboolean(L, removed ? 1 : 0);
  return 1;
}

// _wifi_connect_saved(ssid) -> bool. Join a saved network with its stored
// password (Lua never sees stored passwords, only has_password).
static int lua_wifi_connect_saved(lua_State *L) {
  const char *ssid = luaL_checkstring(L, 1);
  int idx = wifi_creds_find(ssid);
  if (idx < 0) {
    lua_pushboolean(L, 0);
    return 1;
  }
  SLog.printf("Connecting to saved WiFi: %s\n", ssid);
  wa_state = WA_IDLE;   // manual join overrides any auto round
  wa_reconnect_at = 0;
  {
    UsbFlashGuard _g;
    WiFi.mode(WIFI_STA);   // radio may be parked
    WiFi.begin(wifi_saved_ssid[idx].c_str(), wifi_saved_pass[idx].c_str());
  }
  lua_pushboolean(L, 1);
  return 1;
}

static int lua_wifi_auto_connect(lua_State *L) {
  if (WiFi.status() == WL_CONNECTED) {
    lua_pushboolean(L, 1);
    return 1;
  }
  // Async: kicks a connect round; callers poll _wifi_status() for "connected"
  // (downloader.wifi_wait already does exactly that).
  lua_pushboolean(L, wifi_auto_kick() ? 1 : 0);
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
                             r.code == MSG_SEND_SENT_DIRECT, r.est_timeout);
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
  auto pkt = the_mesh->buildSelfAdvert();
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
    radio_apply_params(the_mesh->_prefs.freq, the_mesh->_prefs.bandwidth,
                       the_mesh->_prefs.spreading_factor, the_mesh->_prefs.coding_rate);
    SLog.printf("Frequency set to: %.3f (applied)\n", the_mesh->_prefs.freq);
    lua_pushboolean(L, 1);
  } else if (strcmp(key, "tx") == 0) {
    the_mesh->_prefs.tx_power_dbm = atoi(value);
    the_mesh->savePrefs();
    radio_apply_tx_power(the_mesh->_prefs.tx_power_dbm);
    SLog.printf("TX power set to: %d dBm (applied)\n", the_mesh->_prefs.tx_power_dbm);
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
    radio_apply_params(the_mesh->_prefs.freq, the_mesh->_prefs.bandwidth,
                       the_mesh->_prefs.spreading_factor, the_mesh->_prefs.coding_rate);
    SLog.printf("Bandwidth set to: %.1f kHz (applied)\n", the_mesh->_prefs.bandwidth);
    lua_pushboolean(L, 1);
  } else if (strcmp(key, "sf") == 0) {
    the_mesh->_prefs.spreading_factor = atoi(value);
    the_mesh->savePrefs();
    radio_apply_params(the_mesh->_prefs.freq, the_mesh->_prefs.bandwidth,
                       the_mesh->_prefs.spreading_factor, the_mesh->_prefs.coding_rate);
    SLog.printf("Spreading factor set to: %d (applied)\n", the_mesh->_prefs.spreading_factor);
    lua_pushboolean(L, 1);
  } else if (strcmp(key, "cr") == 0) {
    the_mesh->_prefs.coding_rate = atoi(value);
    the_mesh->savePrefs();
    radio_apply_params(the_mesh->_prefs.freq, the_mesh->_prefs.bandwidth,
                       the_mesh->_prefs.spreading_factor, the_mesh->_prefs.coding_rate);
    SLog.printf("Coding rate set to: %d (applied)\n", the_mesh->_prefs.coding_rate);
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
  int result = the_mesh->sendCommandTracked(*c, timestamp, 0, text, est_timeout);
  char hash_hex[MAX_HASH_SIZE * 2 + 1] = {0};
  if (result != MSG_SEND_FAILED) {
    // Both routes set _last_tx_hash (sendFloodScoped / sendDirectTracked), so
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

// ── Raw packet capture (Packets monitor app) ─────────────────────────
// Arm/disarm the capture ring. Allocating on arm and freeing on disarm keeps
// the ~14KB out of PSRAM whenever nothing is watching.
// Usage: local ok = _mesh_pkt_capture(true)
static int lua_mesh_pkt_capture(lua_State *L) {
  bool on = lua_toboolean(L, 1);
  MESH_LOCK();
  bool ok = true;
  if (on) ok = the_mesh->pktCaptureStart(); else the_mesh->pktCaptureStop();
  MESH_UNLOCK();
  lua_pushboolean(L, ok);
  return 1;
}

// Drain up to `max` captured frames, oldest first.
// Usage: local pkts, dropped = _mesh_pkt_poll(32)
// Each entry: { seq, ts, ms, dir, parsed, snr, rssi, score, len, hash, raw }
// dir is "rx" / "tx" / "txfail"; score is nil when the hook had none; hash is
// nil on an unparsed frame; raw is the full wire frame as a hex string.
static int lua_mesh_pkt_poll(lua_State *L) {
  int max = (int)luaL_optinteger(L, 1, 32);
  if (max < 1) max = 1;
  if (max > PKT_CAP_RING_SIZE) max = PKT_CAP_RING_SIZE;

  lua_newtable(L);
  int out = 0;
  uint32_t dropped = 0;

  MESH_LOCK();
  dropped = the_mesh->_pkt_dropped;
  the_mesh->_pkt_dropped = 0;

  while (out < max && the_mesh->_pkt_count > 0 && the_mesh->_pkt_ring) {
    uint16_t tail = (the_mesh->_pkt_head + PKT_CAP_RING_SIZE - the_mesh->_pkt_count)
                    % PKT_CAP_RING_SIZE;
    // Copy out of the ring before touching Lua: a lua_* call can longjmp on
    // OOM, and the radio core must never find a half-consumed ring.
    PktCapture e = the_mesh->_pkt_ring[tail];
    the_mesh->_pkt_count--;
    MESH_UNLOCK();

    char raw_hex[MAX_TRANS_UNIT * 2 + 1];
    mesh::Utils::toHex(raw_hex, e.raw, e.len);   // toHex null-terminates

    lua_newtable(L);
    lua_pushinteger(L, e.seq);           lua_setfield(L, -2, "seq");
    lua_pushinteger(L, e.ts);            lua_setfield(L, -2, "ts");
    lua_pushinteger(L, e.ms);            lua_setfield(L, -2, "ms");
    lua_pushstring(L, e.dir == PKT_CAP_DIR_TX ? "tx"
                    : e.dir == PKT_CAP_DIR_TX_FAIL ? "txfail" : "rx");
                                         lua_setfield(L, -2, "dir");
    lua_pushboolean(L, e.parsed);        lua_setfield(L, -2, "parsed");
    lua_pushinteger(L, e.len);           lua_setfield(L, -2, "len");
    lua_pushstring(L, raw_hex);          lua_setfield(L, -2, "raw");

    if (e.dir == PKT_CAP_DIR_RX) {
      lua_pushnumber(L, e.snr_q4 / 4.0f); lua_setfield(L, -2, "snr");
      lua_pushinteger(L, e.rssi);         lua_setfield(L, -2, "rssi");
    }
    if (e.score_q10 >= 0) {
      lua_pushnumber(L, e.score_q10 / 1000.0f); lua_setfield(L, -2, "score");
    }
    if (e.parsed) {
      char hash_hex[MAX_HASH_SIZE * 2 + 1];
      mesh::Utils::toHex(hash_hex, e.hash, MAX_HASH_SIZE);
      lua_pushstring(L, hash_hex);        lua_setfield(L, -2, "hash");
    }

    lua_rawseti(L, -2, ++out);
    MESH_LOCK();
  }
  MESH_UNLOCK();

  lua_pushinteger(L, dropped);
  return 2;
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
  int remaining = 0, total = 0;
  MESH_LOCK();
  int status = the_mesh->getRepeatStatus(hash, &remaining, &total);
  MESH_UNLOCK();
  // (status, remaining, total): remaining = re-airs still to go, total =
  // configured max; both 0 unless status==1 (actively repeating). The Messenger
  // renders "repeating N/M" from these (N = total - remaining + 1).
  lua_pushinteger(L, status);
  lua_pushinteger(L, remaining);
  lua_pushinteger(L, total);
  return 3;
}

// ── Persistent message history bridge ────────────────────────────

// Read stored messages for a channel slot.
// Usage: local msgs = _mesh_get_channel_messages(0 [, max_records])
// max_records omitted/0 = whole log; N = only the newest N records.
// Returns array of { from, peer, text, timestamp, hops, snr, rssi, direct, is_dm, channel_idx }
// No MESH_LOCK here: the pusher locks internally just for its channel-name
// snapshot; the file read + Lua pushes must run unlocked (see punkmesh.h).
static int lua_mesh_get_channel_messages(lua_State *L) {
  int ch_idx = luaL_checkinteger(L, 1);
  int max_records = (int)luaL_optinteger(L, 2, 0);
  return the_mesh->pushChannelMessagesToLua(L, ch_idx, max_records);
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

// Read stored messages for a DM thread.
// Usage: local msgs = _mesh_get_dm_messages("alice" [, max_records])
// max_records omitted/0 = whole log; N = only the newest N records.
// No MESH_LOCK: the DM pusher touches no mesh state and the read + Lua
// pushes must run unlocked (see punkmesh.h).
static int lua_mesh_get_dm_messages(lua_State *L) {
  const char *peer = luaL_checkstring(L, 1);
  int max_records = (int)luaL_optinteger(L, 2, 0);
  return the_mesh->pushDMMessagesToLua(L, peer, max_records);
}

// Chat pager for the Messenger's windowed scroll (see punkmesh.h).
// _mesh_chat_page_channel(idx, mode, cursor, count) -> { list = {...}, size = N }
// _mesh_chat_page_dm(peer, mode, cursor, count)     -> { list = {...}, size = N }
// mode: 0 tail (newest count) / 1 older (before byte cursor) / 2 newer (from cursor).
// No MESH_LOCK here: the pushers lock internally only for the channel-name snapshot.
static int lua_mesh_chat_page_channel(lua_State *L) {
  int idx = luaL_checkinteger(L, 1);
  int mode = (int)luaL_optinteger(L, 2, 0);
  uint32_t cursor = (uint32_t)luaL_optinteger(L, 3, 0);
  int count = (int)luaL_optinteger(L, 4, 20);
  return the_mesh->pushChatPageChannel(L, idx, mode, cursor, count);
}

static int lua_mesh_chat_page_dm(lua_State *L) {
  const char *peer = luaL_checkstring(L, 1);
  int mode = (int)luaL_optinteger(L, 2, 0);
  uint32_t cursor = (uint32_t)luaL_optinteger(L, 3, 0);
  int count = (int)luaL_optinteger(L, 4, 20);
  return the_mesh->pushChatPageDM(L, peer, mode, cursor, count);
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

// ── Notification history (C-side generic store, survives Lua teardown) ──
// Thin wrappers over the notify.cpp ring (its own mutex — no MESH_LOCK).
// The topbar polls _notify_log_unseen for the bell badge and renders the
// drop-down list from _notify_log_get.

// Usage: local n = _notify_log_unseen()
static int lua_notify_log_unseen(lua_State *L) {
  lua_pushinteger(L, notify_log_unseen());
  return 1;
}

// Usage: local list = _notify_log_get()  -- { {text=, ts=}, ... } newest-first
static int lua_notify_log_get(lua_State *L) {
  lua_newtable(L);
  int n = notify_log_count();
  for (int i = 0; i < n; i++) {
    uint32_t ts = 0;
    char buf[192];
    if (!notify_log_get(i, &ts, buf, sizeof(buf))) break;
    lua_newtable(L);
    lua_pushstring(L, buf);
    lua_setfield(L, -2, "text");
    lua_pushinteger(L, (lua_Integer)ts);
    lua_setfield(L, -2, "ts");
    lua_rawseti(L, -2, i + 1);
  }
  return 1;
}

// Usage: _notify_log_seen()  -- zero the unseen counter, keep the list
static int lua_notify_log_seen(lua_State *L) {
  notify_log_seen();
  return 0;
}

// Usage: _notify_log_clear()  -- empty the list
static int lua_notify_log_clear(lua_State *L) {
  notify_log_clear();
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

  // Is a USB thumb drive mounted? (fileman.drives() gates the U: root on it)
  lua_pushboolean(L, usb_fs_mounted() ? 1 : 0);
  lua_setfield(L, -2, "usb_available");

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

// Deferred first-boot extraction: set at LittleFS mount time in setup(),
// consumed in setupLuaVGL() once LVGL is up and a splash can be shown.
// Extraction used to run before display init, and the minutes-long dark
// screen made users think the boot hung and power-cycle mid-extract.
static bool s_pack_extract_pending = false;

// Progress label on the unpack splash (non-null only while it is showing).
// Updated per file with a synchronous repaint so the count visibly advances.
static lv_obj_t *s_pack_splash_label = nullptr;

static void pack_splash_progress(uint32_t done, uint32_t total) {
  if (!s_pack_splash_label) return;
  lv_label_set_text_fmt(s_pack_splash_label,
      "First-time setup\n\n"
      "Unpacking filesystem: %u / %u\n\n"
      "This can take a few minutes.\n"
      "Do NOT power off or restart.",
      (unsigned)done, (unsigned)total);
  lv_refr_now(NULL);
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
    pack_splash_progress(done, count);
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
// copy only the app, leaving the device without any data partition; without one
// there is nowhere to extract the pack. This appends an "assets" entry into the
// free flash after the last used partition (same 0x8000 table write the
// Launcher itself performs), fixes up the table's MD5 entry, and reboots so the
// bootloader and esp_partition see the new table. Hard guards: only runs when
// NO spiffs/littlefs data partition exists, never moves or resizes existing
// entries, and aborts on anything unexpected. Note the guard is by subtype, so
// a data partition belonging to another firmware also suppresses this.
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
  memcpy(ne + 12, "assets", 6);

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

  SLog.printf("[PART] adding assets partition at 0x%06X size 0x%06X, rebooting\n",
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
    // LittleFS target (L:) — internal flash, no SPI-bus contention, but the
    // writes/rename below stall the flash cache: hold the USB flash guard.
    UsbFlashGuardIf _g(true);
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

// ── Map tile pool (16 independent 128KB slots) ──────────────────────────────
// The map shows a 4x4 = 16 grid. Instead of 16 LVGL Image widgets each loading a
// .bin FILE (which LVGL decodes into a SCATTERED 128KB image-cache buffer per
// tile — the prime PSRAM fragmenter), we keep 16 fixed 128KB slot buffers and
// display each tile via an in-memory RGB565 lv_image_dsc that LVGL draws DIRECTLY
// (use_directly path, lv_bin_decoder.c — no copy, no cache buffer). The 16 grid
// widgets map 1:1 to slots (slot = grid index). Allocated when the map opens,
// freed on close (lua_tile_pool_free) once the widgets are hidden.
// PER-SLOT, NOT ONE CONTIGUOUS 2MB BLOCK (2026-07-13): a single 2MB alloc is
// the most fragmentation-sensitive demand in the firmware — persistent session
// churn (TLS, caches, mesh buffers) bisects the big free region, and the
// re-alloc on a second Map open fails (hw-confirmed: 2047KB hole vs 2048KB
// need). Each slot only needs 128KB contiguous, which succeeds even on a
// heavily fragmented heap (worst observed mid-session largest block: 335KB).
#define TILE_POOL_SLOTS      16
#define TILE_POOL_SLOT_BYTES (256 * 256 * 2)   // 131072 (RGB565)
static uint8_t *s_tile_slots[TILE_POOL_SLOTS] = {nullptr};
static lv_image_dsc_t s_tile_dsc[TILE_POOL_SLOTS];

// Fill any missing slots; true when all 16 are present.
static bool tile_slots_fill(void) {
  bool ok = true;
  for (int i = 0; i < TILE_POOL_SLOTS; i++) {
    if (!s_tile_slots[i])
      s_tile_slots[i] = (uint8_t *)heap_caps_malloc(TILE_POOL_SLOT_BYTES,
                                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_tile_slots[i]) ok = false;
  }
  return ok;
}

// _tile_pool_alloc() -> bool. Fill any missing slots; on failure reclaim the
// emoji glyph cache + image cache (freeable churn) and retry the missing ones.
// All-or-nothing: a partial set is freed so ~1.9MB is never held uselessly.
static int lua_tile_pool_alloc(lua_State *L) {
  bool complete = true;
  for (int i = 0; i < TILE_POOL_SLOTS; i++)
    if (!s_tile_slots[i]) { complete = false; break; }
  if (!complete) {
    SLog.printf("[tile_pool] pre-alloc: psram free=%uKB largest=%uKB (need %ux%uKB)\n",
                (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024),
                (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) / 1024),
                (unsigned)TILE_POOL_SLOTS, (unsigned)(TILE_POOL_SLOT_BYTES / 1024));
    if (!tile_slots_fill()) {
      // Clear emoji glyphs first (lv_image_cache_drop never touches that
      // cache), then drop the image cache so no decoder entry dangles at a
      // freed glyph dsc, then repaint so freed visible glyphs re-decode.
      emoji_font_cache_clear();
      lv_image_cache_drop(NULL);
      lv_obj_invalidate(lv_screen_active());
      if (!tile_slots_fill()) {
        for (int i = 0; i < TILE_POOL_SLOTS; i++) {
          if (s_tile_slots[i]) { heap_caps_free(s_tile_slots[i]); s_tile_slots[i] = nullptr; }
        }
        SLog.println("[tile_pool] alloc FAILED");
        lua_pushboolean(L, 0);
        return 1;
      }
    }
    lv_memzero(s_tile_dsc, sizeof(s_tile_dsc));
    SLog.printf("[tile_pool] allocated %ux%uKB; psram now free=%uKB largest=%uKB\n",
                (unsigned)TILE_POOL_SLOTS, (unsigned)(TILE_POOL_SLOT_BYTES / 1024),
                (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024),
                (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) / 1024));
  }
  lua_pushboolean(L, 1);
  return 1;
}

// _tile_pool_free(). Caller MUST hide/clear the tile widgets first so nothing
// draws from slot memory after it's freed. Drops the image cache (in case a
// descriptor was cached pointing at slot data) then frees every slot.
static int lua_tile_pool_free(lua_State *L) {
  if (s_tile_slots[0]) {
    lv_image_cache_drop(NULL);
    for (int i = 0; i < TILE_POOL_SLOTS; i++) {
      if (s_tile_slots[i]) { heap_caps_free(s_tile_slots[i]); s_tile_slots[i] = nullptr; }
    }
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
  if (slot < 0 || slot >= TILE_POOL_SLOTS || !s_tile_slots[slot]) { lua_pushboolean(L, 0); return 1; }

  uint8_t *dst = s_tile_slots[slot];
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


// lvgl.Font(name, size) — luavgl's font-extension hook. "ui" (alias "theme")
// resolves to the ui role, "text" to the text role — both emoji-wrapped at
// any size (see theme_font.cpp). Anything else returns NULL so luavgl's
// builtin resolution raises its normal "cannot create font" error.
static const lv_font_t *meshpunk_make_font(const char *name, int size, int weight) {
  (void)weight;
  if (!name) return NULL;
  if (strcasecmp(name, "ui") == 0 || strcasecmp(name, "theme") == 0)
    return theme_font_sized(THEME_FONT_UI, size);
  if (strcasecmp(name, "text") == 0)
    return theme_font_sized(THEME_FONT_TEXT, size);
  return NULL;
}

// "ui"/"text" role-string parse shared by the font bindings. Returns false on
// anything else (the binding then reports failure to Lua).
static bool parse_font_role(const char *s, theme_font_role_t *out) {
  if (!s) return false;
  if (strcasecmp(s, "ui") == 0)   { *out = THEME_FONT_UI;   return true; }
  if (strcasecmp(s, "text") == 0) { *out = THEME_FONT_TEXT; return true; }
  return false;
}

// _gblink_status() -> int. T-Deck peer-link status for Lua launchers:
// 0 none, 1 cable session up, 2 session + peer's game attached (low 2 bits);
// bit 0x4 = this deck is the USB-host side. The GameBoy launcher uses >0 to
// warn that launching will pause the mesh radio for the linked session.
static int lua_gblink_status(lua_State *Ls) {
  lua_pushinteger(Ls, tdeck_link_status());
  return 1;
}

// ── USB drive mode bindings (usb_msc_dev.cpp; Tools/"USB Drive" app) ────────
static int lua_usbdrive_start(lua_State *Ls) {
  lua_pushboolean(Ls, usbdrive_start() ? 1 : 0);
  return 1;
}
static int lua_usbdrive_stop(lua_State *Ls) {
  usbdrive_stop();
  return 0;
}
static int lua_usbdrive_ping(lua_State *Ls) {
  usbdrive_ping();
  return 0;
}
static int lua_usbdrive_status(lua_State *Ls) {
  UsbDriveStatus st;
  usbdrive_status(&st);
  lua_newtable(Ls);
  lua_pushboolean(Ls, st.active);        lua_setfield(Ls, -2, "active");
  lua_pushboolean(Ls, st.connected);     lua_setfield(Ls, -2, "connected");
  lua_pushboolean(Ls, st.ejected);       lua_setfield(Ls, -2, "ejected");
  lua_pushboolean(Ls, st.host_latched);  lua_setfield(Ls, -2, "host_latched");
  lua_pushinteger(Ls, (lua_Integer)st.reads);  lua_setfield(Ls, -2, "reads");
  lua_pushinteger(Ls, (lua_Integer)st.writes); lua_setfield(Ls, -2, "writes");
  lua_pushnumber(Ls, (lua_Number)st.bytes);    lua_setfield(Ls, -2, "bytes");
  lua_pushstring(Ls, st.fail ? st.fail : "");  lua_setfield(Ls, -2, "fail");
  return 1;
}

void setupLuaVGL() {
  // (Runtime TTF fonts are initialized in luaBringUp(), BEFORE the Lua arena —
  // the ~430KB buffers must land below the gap, not inside it. See luaBringUp.)

  // Create Lua state with PSRAM allocator (5.5 needs an explicit string-hash
  // seed; esp_random() is the hardware TRNG)
  L = lua_newstate(lua_psram_alloc, NULL, esp_random());
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
  luavgl_set_font_extension(L, meshpunk_make_font, NULL);

  // T-Deck peer link (gblink)
  lua_register(L, "_gblink_status", lua_gblink_status);

  // USB drive mode (share the SD card with a PC)
  lua_register(L, "_usbdrive_start", lua_usbdrive_start);
  lua_register(L, "_usbdrive_stop", lua_usbdrive_stop);
  lua_register(L, "_usbdrive_ping", lua_usbdrive_ping);
  lua_register(L, "_usbdrive_status", lua_usbdrive_status);

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
  lua_register(L, "_wifi_forget_cred", lua_wifi_forget_cred);
  lua_register(L, "_wifi_connect_saved", lua_wifi_connect_saved);
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
  lua_register(L, "_mesh_pkt_capture", lua_mesh_pkt_capture);
  lua_register(L, "_mesh_pkt_poll", lua_mesh_pkt_poll);
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
  // Per-channel flood scope / region override, keyed by channel NAME.
  // "" = no override (the channel inherits the default scope above).
  lua_register(L, "_mesh_get_channel_scope", [](lua_State *L) -> int {
    const char* chan = luaL_checkstring(L, 1);
    MESH_LOCK();
    const char* n = the_mesh ? the_mesh->getChannelScope(chan) : "";
    lua_pushstring(L, n ? n : "");
    MESH_UNLOCK();
    return 1;
  });
  lua_register(L, "_mesh_set_channel_scope", [](lua_State *L) -> int {
    const char* chan = luaL_checkstring(L, 1);
    const char* region = luaL_optstring(L, 2, "");
    MESH_LOCK();
    if (the_mesh) the_mesh->setChannelScope(chan, region);
    MESH_UNLOCK();
    lua_pushboolean(L, 1);
    return 1;
  });
  // True while the phone app's session-only scope key (CMD_SET_FLOOD_SCOPE_KEY)
  // is set. It's a raw key with no name, so the UI can only flag its presence.
  lua_register(L, "_mesh_ble_scope_active", [](lua_State *L) -> int {
    bool on = false;
    MESH_LOCK();
    if (the_mesh) {
      for (size_t i = 0; i < sizeof(the_mesh->_ble_send_scope_key); i++) {
        if (the_mesh->_ble_send_scope_key[i]) { on = true; break; }
      }
    }
    MESH_UNLOCK();
    lua_pushboolean(L, on);
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
  lua_register(L, "_mesh_chat_page_channel", lua_mesh_chat_page_channel);
  lua_register(L, "_mesh_chat_page_dm", lua_mesh_chat_page_dm);
  lua_register(L, "_mesh_get_dm_threads", lua_mesh_get_dm_threads);
  lua_register(L, "_mesh_get_msg_summaries", lua_mesh_get_msg_summaries);
  lua_register(L, "_mesh_set_max_messages", lua_mesh_set_max_messages);

  // Unread counters (C-side so they survive Lua teardown during ELF runs)
  lua_register(L, "_mesh_unread_total", lua_mesh_unread_total);
  lua_register(L, "_mesh_unread_channel", lua_mesh_unread_channel);
  lua_register(L, "_mesh_unread_dm", lua_mesh_unread_dm);
  lua_register(L, "_mesh_unread_clear_channel", lua_mesh_unread_clear_channel);
  lua_register(L, "_mesh_unread_clear_dm", lua_mesh_unread_clear_dm);

  // Notification history (C-side generic store, survives Lua teardown)
  lua_register(L, "_notify_log_unseen", lua_notify_log_unseen);
  lua_register(L, "_notify_log_get", lua_notify_log_get);
  lua_register(L, "_notify_log_seen", lua_notify_log_seen);
  lua_register(L, "_notify_log_clear", lua_notify_log_clear);

  // Identity management
  lua_register(L, "_mesh_export_private_key", lua_mesh_export_private_key);
  lua_register(L, "_mesh_import_private_key", lua_mesh_import_private_key);
  lua_register(L, "_mesh_generate_identity", lua_mesh_generate_identity);

  lua_register(L, "_get_battery_mv", [](lua_State *L) -> int {
    lua_pushinteger(L, power_dev_battery_mv());
    return 1;
  });

  // Device capability table for Lua (apps adapt per board; input specifics
  // come from _input_caps in the input layer).
  lua_register(L, "_device_caps", [](lua_State *L) -> int {
    lua_newtable(L);
    lua_pushstring(L, MESHPUNK_BOARD_NAME);        lua_setfield(L, -2, "name");
    lua_pushinteger(L, display_dev_width());       lua_setfield(L, -2, "screen_w");
    lua_pushinteger(L, display_dev_height());      lua_setfield(L, -2, "screen_h");
    switch (audio_dev_kind()) {
      case AUDIO_DEV_I2S:    lua_pushstring(L, "i2s");    break;
      case AUDIO_DEV_BUZZER: lua_pushstring(L, "buzzer"); break;
      default:               lua_pushstring(L, "none");   break;
    }
    lua_setfield(L, -2, "audio");
    lua_pushboolean(L, power_dev_battery_mv() > 0);  lua_setfield(L, -2, "battery");
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

  // Client repeat ("repeater mode"): re-transmit other nodes' packets. Same
  // pref the BLE companion app sets via CMD_SET_RADIO_PARAMS.
  lua_register(L, "_mesh_get_client_repeat", [](lua_State *L) -> int {
    lua_pushboolean(L, the_mesh && the_mesh->_prefs.client_repeat);
    return 1;
  });
  lua_register(L, "_mesh_set_client_repeat", [](lua_State *L) -> int {
    bool on = lua_toboolean(L, 1);
    if (!the_mesh) {
      lua_pushboolean(L, 0);
      lua_pushstring(L, "mesh not ready");
      return 2;
    }
    the_mesh->_prefs.client_repeat = on ? 1 : 0;
    the_mesh->savePrefs();
    SLog.printf("Client repeat set to: %s\n", on ? "ON" : "OFF");
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

  // In-memory image buffers (_img_*, img_bridge.cpp) — used by lib/imgview.lua
  img_bridge_register(L);

  // USB-OTG host manager (_usb_*) — used by Tools/USB (also PHY boot self-heal)
  usb_manager_register_lua(L);

  // System
  lua_register(L, "_system_reboot", [](lua_State *L) -> int {
    SLog.println("[SYSTEM] Reboot requested from Lua");
    delay(100);
    ESP.restart();
    return 0;
  });

  // Power menu (topbar battery drop-down). Both defer to the top of loop():
  // the Lua caller returns and paints its farewell first, then the action
  // runs on a clean Core-0 stack. Both return false (refused) while a USB
  // drive session or a link session (mesh paused) owns the hardware.
  lua_register(L, "_system_poweroff", [](lua_State *L) -> int {
    if (usbdrive_active() || mesh_task_paused) { lua_pushboolean(L, false); return 1; }
    SLog.println("[SYSTEM] Power off requested from Lua");
    s_poweroff_request = true;
    lua_pushboolean(L, true);
    return 1;
  });
  lua_register(L, "_system_standby", [](lua_State *L) -> int {
    if (usbdrive_active() || mesh_task_paused) { lua_pushboolean(L, false); return 1; }
    SLog.println("[SYSTEM] Standby requested from Lua");
    s_standby_request = true;
    lua_pushboolean(L, true);
    return 1;
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

  // _gps_sync_status() — returns done:bool, has_location:bool.
  // has_location reports THIS cycle's outcome; the carried-over last-known
  // position (which survives failed cycles) is exposed via _gps_info instead.
  lua_register(L, "_gps_sync_status", [](lua_State *L) -> int {
    lua_pushboolean(L, gps_sync_done ? 1 : 0);
    lua_pushboolean(L, gps_loc_fixed_this_cycle ? 1 : 0);
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

  // _gps_state() — live sync-cycle state for the Settings status panel:
  //   state:int  0=standby  1=probing baud  2=waiting for time
  //              3=hunting location  4=finishing (fix landed, sat-count grace)
  //   elapsed_s:int  seconds since this cycle started
  //   hunt_s:int     seconds since the time fix (location-hunt clock)
  //   budget_s:int   this cycle's location-hunt budget
  //   time_fix:bool, loc_fix:bool — THIS cycle's outcomes (last cycle's when
  //   standby; both reset on cycle restart)
  // Unlocked cross-core reads, same policy as _gps_info: torn values are
  // harmless for a 1 Hz status display.
  lua_register(L, "_gps_state", [](lua_State *L) -> int {
    int state;
    if (gps_sync_done)                  state = 0;
    else if (!gps_baud_locked)          state = 1;
    else if (!gps_time_fix_valid)       state = 2;
    else if (!gps_loc_fixed_this_cycle) state = 3;
    else                                state = 4;
    uint32_t now = millis();
    lua_pushinteger(L, state);
    lua_pushinteger(L, (lua_Integer)((now - gps_sync_start_ms) / 1000UL));
    lua_pushinteger(L, (lua_Integer)(gps_time_fix_valid ? (now - gps_fix_acquired_ms) / 1000UL : 0));
    lua_pushinteger(L, (lua_Integer)(gps_loc_hunt_ms / 1000UL));
    lua_pushboolean(L, gps_time_fix_valid ? 1 : 0);
    lua_pushboolean(L, gps_loc_fixed_this_cycle ? 1 : 0);
    return 6;
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

  // _theme_font_set(role, path, px) -> bool. Theme-supplied runtime TTF for
  // one role ("ui"/"text"; drive-prefixed path; px 0 = default UI size).
  // Idempotent per path+size — themes re-apply on every show_background().
  // Failure keeps the role's current resolution.
  lua_register(L, "_theme_font_set", [](lua_State *L) -> int {
    theme_font_role_t role;
    if (!parse_font_role(luaL_checkstring(L, 1), &role)) {
      lua_pushboolean(L, 0);
      return 1;
    }
    const char *path = luaL_checkstring(L, 2);
    int px = (int)luaL_optinteger(L, 3, 0);
    lua_pushboolean(L, theme_font_set(role, path, px) ? 1 : 0);
    return 1;
  });
  // _theme_font_clear() — drop both theme font roles (back to the user/
  // bundled defaults). Called by lib/theme before every theme apply.
  lua_register(L, "_theme_font_clear", [](lua_State *L) -> int {
    theme_font_clear();
    return 0;
  });
  // _font_default_set(role, path) -> bool. The user's default font for a role
  // ("" or nil path = revert to the bundled Noto Sans). Persisted; a theme's
  // own font overrides it while that theme is active.
  lua_register(L, "_font_default_set", [](lua_State *L) -> int {
    theme_font_role_t role;
    if (!parse_font_role(luaL_checkstring(L, 1), &role)) {
      lua_pushboolean(L, 0);
      return 1;
    }
    const char *path = luaL_optstring(L, 2, "");
    if (!font_default_set(role, path)) {
      lua_pushboolean(L, 0);
      return 1;
    }
    if (role == THEME_FONT_UI) font_ui_pref = String(path);
    else                       font_text_pref = String(path);
    firmware_prefs_save();
    lua_pushboolean(L, 1);
    return 1;
  });
  // _font_default_get(role) -> path string ("" = bundled default).
  lua_register(L, "_font_default_get", [](lua_State *L) -> int {
    theme_font_role_t role;
    if (!parse_font_role(luaL_checkstring(L, 1), &role)) {
      lua_pushstring(L, "");
      return 1;
    }
    lua_pushstring(L, role == THEME_FONT_UI ? font_ui_pref.c_str()
                                            : font_text_pref.c_str());
    return 1;
  });

  lua_register(L, "_rtc_set_time", [](lua_State *L) -> int {
    lua_Integer ts = luaL_checkinteger(L, 1);
    if (ts < 0) {
      lua_pushboolean(L, 0);
      return 1;
    }
    bool ok = meshpunk_set_clock(CLOCK_TIER_MANUAL, (uint32_t)ts, "manual");
    if (ok) gps_manual_time_override = true;   // informational (Settings UI)
    lua_pushboolean(L, ok ? 1 : 0);
    return 1;
  });

  lua_register(L, "_rtc_manual_override_get", [](lua_State *L) -> int {
    lua_pushboolean(L, gps_manual_time_override ? 1 : 0);
    return 1;
  });

  lua_register(L, "_rtc_manual_override_clear", [](lua_State *L) -> int {
    gps_manual_time_override = false;
    // Drop the manual tier so the next source (GPS/phone/seed) wins again.
    if (clock_cur_tier >= CLOCK_TIER_MANUAL) clock_cur_tier = CLOCK_TIER_SEED;
    SLog.println("[RTC] Manual override cleared; GPS time updates re-enabled.");
    lua_pushboolean(L, 1);
    return 1;
  });

  // ── Sound ──────────────────────────────────────────────────────────────────
  sound_register_lua(L);

  // ── Input (keyboard/trackball/touch policy + capture + emoji keymap) ──────
  input_ui_register_lua(L);

  // ── ELF module loader ─────────────────────────────────────────────────────
  elf_host_register_lua(L);

  // ── Keyboard backlight ────────────────────────────────────────────────────
  lua_register(L, "_kbd_set_brightness", [](lua_State* L) -> int {
    int v = luaL_checkinteger(L, 1);
    if (v < 0) v = 0; if (v > 255) v = 255;
    kbd_brightness = (uint8_t)v;
    input_dev_kbd_backlight(kbd_brightness);
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
    input_dev_kbd_backlight((uint8_t)v);
    lua_pushinteger(L, v);
    return 1;
  });
  lua_register(L, "_kbd_is_timed_out", [](lua_State* L) -> int {
    lua_pushboolean(L, kbd_timed_out);
    return 1;
  });

  // ── Standby heartbeat (kbd backlight glow each drain window) ───────────
  lua_register(L, "_standby_heartbeat_get", [](lua_State* L) -> int {
    lua_pushboolean(L, standby_heartbeat);
    return 1;
  });
  lua_register(L, "_standby_heartbeat_set", [](lua_State* L) -> int {
    standby_heartbeat = lua_toboolean(L, 1);
    firmware_prefs_save();
    lua_pushboolean(L, standby_heartbeat);
    return 1;
  });
  lua_register(L, "_standby_heartbeat_secs_get", [](lua_State* L) -> int {
    lua_pushinteger(L, standby_heartbeat_secs);
    return 1;
  });
  lua_register(L, "_standby_heartbeat_secs_set", [](lua_State* L) -> int {
    int v = (int)luaL_checkinteger(L, 1);
    if (v < 5) v = 5;
    if (v > 600) v = 600;
    standby_heartbeat_secs = (uint16_t)v;
    firmware_prefs_save();
    lua_pushinteger(L, standby_heartbeat_secs);
    return 1;
  });
  lua_register(L, "_auto_standby_get", [](lua_State* L) -> int {
    lua_pushboolean(L, auto_standby);
    return 1;
  });
  lua_register(L, "_auto_standby_set", [](lua_State* L) -> int {
    auto_standby = lua_toboolean(L, 1);
    firmware_prefs_save();
    lua_pushboolean(L, auto_standby);
    return 1;
  });
  lua_register(L, "_auto_standby_mins_get", [](lua_State* L) -> int {
    lua_pushinteger(L, auto_standby_mins);
    return 1;
  });
  lua_register(L, "_auto_standby_mins_set", [](lua_State* L) -> int {
    int v = (int)luaL_checkinteger(L, 1);
    if (v < 1) v = 1;
    if (v > 600) v = 600;
    auto_standby_mins = (uint16_t)v;
    firmware_prefs_save();
    lua_pushinteger(L, auto_standby_mins);
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
  // _ble_set_enabled(on [, persist]) — persist defaults to true. With
  // persist = false the companion stops (BLEDevice::deinit frees the stack)
  // and the in-RAM pref changes, but nothing is written: the next boot
  // restores the saved state. Used to free internal SRAM for one module run.
  lua_register(L, "_ble_set_enabled", [](lua_State* L) -> int {
    bool v = lua_toboolean(L, 1);
    bool persist = lua_isnoneornil(L, 2) ? true : (bool)lua_toboolean(L, 2);
    ble_enabled_pref = v;
    MESH_LOCK();
    if (v && !ble_serial) {
      ble_companion_init_early();
      ble_companion_start(*the_mesh);
    } else if (!v && ble_serial) {
      ble_companion_stop();
    }
    MESH_UNLOCK();
    if (persist) firmware_prefs_save();
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
  lua_register(L, "_ble_get_sync_limit", [](lua_State* L) -> int {
    lua_pushinteger(L, ble_sync_max_per_channel);
    return 1;
  });
  lua_register(L, "_ble_set_sync_limit", [](lua_State* L) -> int {
    int v = (int)luaL_checkinteger(L, 1);
    if (v < 0) v = 0;
    if (v > 5000) v = 5000;
    ble_sync_max_per_channel = (uint16_t)v;
    if (the_mesh) the_mesh->_ble_sync_max_per_channel = ble_sync_max_per_channel;
    firmware_prefs_save();
    lua_pushinteger(L, ble_sync_max_per_channel);
    return 1;
  });
#endif

  // ── Display backlight ─────────────────────────────────────────────────────
  lua_register(L, "_disp_set_brightness", [](lua_State* L) -> int {
    int v = luaL_checkinteger(L, 1);
    if (v < 0) v = 0; if (v > 16) v = 16;
    display_brightness = (uint8_t)v;
    display_dev_brightness(display_brightness);
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
    if (screen_timed_out) { display_dev_brightness(display_brightness); screen_timed_out = false; }
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
    if (kbd_timed_out) { input_dev_kbd_backlight(kbd_brightness); kbd_timed_out = false; }
    firmware_prefs_save();
    lua_pushinteger(L, kbd_timeout_secs);
    return 1;
  });
  lua_register(L, "_kbd_timeout_get", [](lua_State* L) -> int {
    lua_pushinteger(L, kbd_timeout_secs);
    return 1;
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

  // _gridnav_edge_lock(on): when on, gridnav won't walk focus OUT of the current
  // container at an edge (skips lv_group_focus_prev/next). The messenger sets it
  // during chat message-select so the trackball stays on the bubbles at the top/
  // bottom instead of jumping to the Home/Send buttons. See lv_gridnav.c MESHPUNK.
  lua_register(L, "_gridnav_edge_lock", [](lua_State *L) -> int {
    extern bool meshpunk_gridnav_edge_lock;
    meshpunk_gridnav_edge_lock = lua_toboolean(L, 1);
    return 0;
  });

  // _touch_pressed() -> bool: is any pointer (touchscreen) indev currently held
  // down? The messenger uses it to defer chat paging until the finger lifts — a
  // window mutation mid-touch turns the held press into a spurious bubble click.
  lua_register(L, "_touch_pressed", [](lua_State *L) -> int {
    bool pressed = false;
    for (lv_indev_t *i = lv_indev_get_next(NULL); i; i = lv_indev_get_next(i)) {
      if (lv_indev_get_type(i) == LV_INDEV_TYPE_POINTER &&
          lv_indev_get_state(i) == LV_INDEV_STATE_PRESSED) { pressed = true; break; }
    }
    lua_pushboolean(L, pressed);
    return 1;
  });

  // Gridnav flag constants for Lua
  lua_pushinteger(L, LV_GRIDNAV_CTRL_NONE);
  lua_setglobal(L, "GRIDNAV_NONE");
  lua_pushinteger(L, LV_GRIDNAV_CTRL_ROLLOVER);
  lua_setglobal(L, "GRIDNAV_ROLLOVER");
  lua_pushinteger(L, LV_GRIDNAV_CTRL_SCROLL_FIRST);
  lua_setglobal(L, "GRIDNAV_SCROLL_FIRST");

  // Firmware identity for the app store's min_fw gating (see version.h).
  // Absent on older firmware — Lua reads nil and treats it as API level 0.
  lua_pushinteger(L, MESHPUNK_FW_API);
  lua_setglobal(L, "_FW_API");
  lua_pushstring(L, MESHPUNK_FW_VERSION);
  lua_setglobal(L, "_FW_VERSION");

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

  // ── On-screen keyboard (lib/osk.lua; keyboardless boards) ────────────────
  // The OSK types into its own PREVIEW textarea; these bindings bridge it to
  // the app textarea input_ui captured at focus time. Every use re-validates
  // both pointers — the app underneath can rebuild its views while the OSK
  // is up (same discipline as _emoji_popup_insert).
  lua_register(L, "_osk_initial_text", [](lua_State *L) -> int {
    lv_obj_t *ta = input_ui_osk_target();
    if (ta && lv_obj_is_valid(ta) && lv_obj_check_type(ta, &lv_textarea_class)) {
      lua_pushstring(L, lv_textarea_get_text(ta));
    } else {
      lua_pushstring(L, "");
    }
    return 1;
  });
  lua_register(L, "_osk_commit", [](lua_State *L) -> int {
    const char *s = luaL_checkstring(L, 1);
    lv_obj_t *ta = input_ui_osk_target();
    bool ok = ta && lv_obj_is_valid(ta) &&
              lv_obj_check_type(ta, &lv_textarea_class);
    if (ok) {
      lv_textarea_set_text(ta, s);
    }
    lua_pushboolean(L, ok ? 1 : 0);
    return 1;
  });
  lua_register(L, "_osk_set_active", [](lua_State *L) -> int {
    input_ui_osk_set_active(lua_toboolean(L, 1));
    return 0;
  });
  lua_register(L, "_osk_release", [](lua_State *L) -> int {
    input_ui_osk_set_active(false);
    input_ui_osk_release();
    return 0;
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

  // _snapshot_take(obj) -> lightuserdata draw_buf (or nil on failure).
  // Renders the object + children into an ARGB8888 image via the normal draw
  // pipeline (lv_snapshot_take). The Messenger bakes each chat bubble once and
  // shows the result as an Image{ src = <this pointer> } so scrolling blits a
  // finished bitmap instead of re-drawing the bubble's rects+TTF text per frame.
  // The caller OWNS the returned buffer — free it with _snapshot_free on prune.
  lua_register(L, "_snapshot_take", [](lua_State *L) -> int {
    luavgl_obj_t *lobj = (luavgl_obj_t *)lua_touserdata(L, 1);
    if (!lobj || !lobj->obj) { lua_pushnil(L); return 1; }
    lv_draw_buf_t *buf = lv_snapshot_take(lobj->obj, LV_COLOR_FORMAT_ARGB8888);
    if (!buf) { lua_pushnil(L); return 1; }
    lua_pushlightuserdata(L, buf);
    return 1;
  });

  // _snapshot_free(ptr) -> nil. Destroy a draw_buf returned by _snapshot_take.
  // Safe on nil / a non-userdata arg (lua_touserdata yields NULL).
  lua_register(L, "_snapshot_free", [](lua_State *L) -> int {
    lv_draw_buf_t *buf = (lv_draw_buf_t *)lua_touserdata(L, 1);
    if (buf) lv_draw_buf_destroy(buf);
    return 0;
  });

  // _snapshot_attach_free(obj, buf): free `buf` when `obj` is deleted, via a
  // C-LEVEL LV_EVENT_DELETE handler. A Lua obj:onevent(DELETE,..) does NOT work
  // for this: luavgl's own obj_delete_cb runs first on delete and unrefs every
  // Lua event handler (obj.c), so the Lua one never fires. A C event_cb is not
  // touched by that cleanup — same pattern luavgl's canvas.c uses to free its
  // own draw_buf. Fires on EVERY deletion path (prune, clear_all, view teardown),
  // so it's the single owner of the buffer's lifetime — no leak, no double-free.
  lua_register(L, "_snapshot_attach_free", [](lua_State *L) -> int {
    luavgl_obj_t *lobj = (luavgl_obj_t *)lua_touserdata(L, 1);
    lv_draw_buf_t *buf = (lv_draw_buf_t *)lua_touserdata(L, 2);
    if (lobj && lobj->obj && buf) {
      lv_obj_add_event_cb(lobj->obj, [](lv_event_t *e) {
        lv_draw_buf_t *b = (lv_draw_buf_t *)lv_event_get_user_data(e);
        if (b) lv_draw_buf_destroy(b);
      }, LV_EVENT_DELETE, buf);
    }
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
    if (!ud->file) { lua_pushnil(L); lua_pushstring(L, "file closed"); return 2; }
    // LittleFS write = internal-flash write: pause USB audio around it or the
    // cache stall crashes the host stack (no-op when USB is idle / target is SD).
    UsbFlashGuardIf _g(ud->is_flash && ud->is_write);
    if (ud->is_sd) sd_spi_take();
    // write(buf, len), not print(str): binary-safe past embedded NULs
    size_t written = ud->file->write((const uint8_t *)str, len);
    if (ud->is_sd) sd_spi_release();
    lua_pushinteger(L, written);
    return 1;
  });
  lua_setfield(L, -2, "write");

  // file:seek([whence[, offset]]) — Lua io semantics. whence "set"|"cur"|"end"
  // (default "cur"), offset default 0. Returns the new absolute position, or
  // nil+message on error. Needed for tail reads (e.g. ID3v1 in the last 128B).
  lua_pushcfunction(L, [](lua_State *L) -> int {
    LuaFileHandle *ud = (LuaFileHandle *)luaL_checkudata(L, 1, "esp32_file");
    if (!ud->file) { lua_pushnil(L); lua_pushstring(L, "file closed"); return 2; }
    const char *whence = luaL_optstring(L, 2, "cur");
    long offset = (long)luaL_optinteger(L, 3, 0);
    if (ud->is_sd) sd_spi_take();
    size_t sz  = ud->file->size();
    size_t cur = ud->file->position();
    long base;
    if      (strcmp(whence, "set") == 0) base = 0;
    else if (strcmp(whence, "end") == 0) base = (long)sz;
    else                                 base = (long)cur;   // "cur" / default
    long target = base + offset;
    if (target < 0) target = 0;
    if (target > (long)sz) target = (long)sz;
    bool ok = ud->file->seek((uint32_t)target);
    size_t newpos = ud->file->position();
    if (ud->is_sd) sd_spi_release();
    if (!ok) { lua_pushnil(L); lua_pushstring(L, "seek failed"); return 2; }
    lua_pushinteger(L, (lua_Integer)newpos);
    return 1;
  });
  lua_setfield(L, -2, "seek");

  // file:flush()
  lua_pushcfunction(L, [](lua_State *L) -> int {
    LuaFileHandle *ud = (LuaFileHandle *)luaL_checkudata(L, 1, "esp32_file");
    if (!ud->file) return 0;
    UsbFlashGuardIf _g(ud->is_flash && ud->is_write); // LittleFS flush writes flash
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
      // Closing a written LittleFS file commits data/metadata to flash.
      UsbFlashGuardIf _g(ud->is_flash && ud->is_write);
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
      // Permanent leak detector: a GC-close only happens for a handle that
      // was ABANDONED (never close()d, never consumed by loadFile). Every
      // one of these lines is a bug sighting in some Lua file-handling path.
      SLog.printf("[FS] GC-close ud=%p f=%p sd=%d\n",
                  (void*)ud, (void*)ud->file, (int)ud->is_sd);
      // Same flash-commit hazard as close() when the file was written.
      UsbFlashGuardIf _g(ud->is_flash && ud->is_write);
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

#ifdef MESHPUNK_EMBED_PACK
  // Deferred first-boot extraction (flag set at mount time in setup). LVGL is
  // up, but the backlight normally turns on only after createUI() — force it
  // on now so the splash is visible. setupLuaVGL() also re-runs when Lua is
  // rebuilt after an ELF module exits; the flag is only ever set during boot,
  // so this is a no-op there.
  if (s_pack_extract_pending) {
    s_pack_extract_pending = false;
    display_dev_backlight_init();
    display_dev_brightness(display_brightness);

    s_pack_splash_label = lv_label_create(lv_scr_act());
    lv_obj_set_style_text_align(s_pack_splash_label, LV_TEXT_ALIGN_CENTER, 0);
    // Force plain white — the theme's default text color is grey and unreadable
    // on the black boot screen.
    lv_obj_set_style_text_color(s_pack_splash_label, lv_color_white(), 0);
    lv_obj_center(s_pack_splash_label);
    lv_label_set_text(s_pack_splash_label,
        "First-time setup\n\n"
        "Unpacking filesystem...\n\n"
        "This can take a few minutes.\n"
        "Do NOT power off or restart.");
    lv_refr_now(NULL);

    bool pack_ok = extract_data_pack();

    if (pack_ok) {
      // The emoji font's blob open (L:/emojis.bin) ran in setupLvgl, before
      // the file existed on a fresh filesystem; re-open it now that it does.
      emoji_font_reload(false);
    } else {
      // The marker is written last, so a failed pass retries on the next boot
      // (see extract_data_pack). Tell the user instead of silently moving on.
      lv_label_set_text(s_pack_splash_label,
          "Unpack FAILED\n\n"
          "It will retry on the next boot.\n"
          "If this repeats, reflash the firmware.");
      lv_refr_now(NULL);
      delay(3000);
    }
    lv_obj_delete(s_pack_splash_label);
    s_pack_splash_label = nullptr;
  }
#endif

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
  if (the_mesh) {
    the_mesh->lua_runtime = NULL;             // drop the stale-state handle
    // The only reader of the capture ring dies with Lua; free it here so an
    // armed capture can't hold ~14KB of PSRAM through the ELF run.
    MESH_LOCK();
    the_mesh->pktCaptureStop();
    MESH_UNLOCK();
  }
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
  // Runtime TTF fonts: buffers + glyph caches are the same class of resident
  // PSRAM cluster — release them all (chain reverts to montserrat); the
  // post-ELF luaBringUp() -> setupLuaVGL() -> theme_font_init() reloads the
  // default, and the theme re-apply in main.lua restores any theme font.
  theme_font_release_all();
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
  // Runtime TTF fonts FIRST, then the arena. TLSF is good-fit: with the arena
  // already up, the ~430KB font buffers land in the freshly-made 1MB gap (the
  // smallest block that fits) and permanently eat ~86% of the churn shield —
  // session churn then overflows into the reserve above the arena and bisects
  // it (the root cause of the Map tile-pool failures, hw-confirmed
  // 2026-07-13). Loading fonts first puts them at the bottom of the pristine
  // region instead; the gap + arena stack ABOVE them and the gap stays fully
  // empty for churn. At ELF launch luaTearDown frees fonts + arena together,
  // so fonts + gap + arena still coalesce into one block for the module.
  // Also the post-ELF restore point — luaTearDown released every font; this
  // reloads the defaults and main.lua's theme re-apply restores theme fonts.
  theme_font_init(font_ui_pref.c_str(), font_text_pref.c_str());
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
  // precede begin(). 8 KB gives the tdeck-link device-role TX headroom (its
  // SYNC2 answers must not drop when the USB host is briefly not draining).
  // ~8 KB ≈ 700 ms of SLog backlog at 115200 before any line drops.
  Serial.setTxBufferSize(8192);
  // RX likewise: as a tdeck-link device role, peer frames arrive here, and the
  // default 256-byte ring overflowed (dropping bytes MID-FRAME) whenever the
  // Core-1 link task starved a few hundred ms. 4 KB rides out multi-second
  // stalls; the link layer's CRC + retransmit covers whatever still drops.
  Serial.setRxBufferSize(4096);
  Serial.begin(115200);
  SLog.println("Delaying for 50ms...");
  delay(50);

  SLog.println("MeshPunk");

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

  // A panic reboot is a CPU-only reset (RTC_SW_CPU_RST): the GPIO matrix and
  // a latched radio IRQ line survive it. If a level-typed standby wake was
  // armed at the crash, the first attachInterrupt below would install the
  // GPIO ISR service onto an already-asserted status bit and storm the
  // interrupt watchdog at every boot (symbolized gpio_intr_service loop) —
  // a boot loop only a full power cycle escapes. Neutralize the wake pins'
  // interrupt config before any ISR service can exist.
  gpio_intr_disable((gpio_num_t)PIN_LORA_DIO1);
  gpio_set_intr_type((gpio_num_t)PIN_LORA_DIO1, GPIO_INTR_DISABLE);
  gpio_intr_disable((gpio_num_t)PIN_BOOT_BTN);
  gpio_set_intr_type((gpio_num_t)PIN_BOOT_BTN, GPIO_INTR_DISABLE);

  // Input pins + ISRs that need no bus/peripheral power (board input backend),
  // and the input layer's prefs-save hook (same pattern as sound_init).
  input_dev_preinit();
  input_ui_init(firmware_prefs_save);

  // Peripheral power rail (board power backend)
  power_dev_init();

  // Kick off one-shot GPS time sync; gps_sync_poll() runs it to completion in loop().
  gps_sync_begin();

  // Set CS on all SPI buses to high level during initialization
  // Park every chip select this board hangs on the shared SPI bus. Boards
  // where the radio has the bus to itself (Heltec) define only PIN_LORA_CS.
  pinMode(PIN_LORA_CS, OUTPUT);
  digitalWrite(PIN_LORA_CS, HIGH);
#if defined(PIN_SD_CS)
  pinMode(PIN_SD_CS, OUTPUT);
  digitalWrite(PIN_SD_CS, HIGH);
#endif
#if defined(PIN_TFT_CS)
  pinMode(PIN_TFT_CS, OUTPUT);
  digitalWrite(PIN_TFT_CS, HIGH);
#endif

  pinMode(PIN_SPI_MISO, INPUT_PULLUP);
  SPI.begin(PIN_SPI_SCK, PIN_SPI_MISO, PIN_SPI_MOSI); // radio (+ SD/TFT where shared)

  pinMode(PIN_BOOT_BTN, INPUT_PULLUP);


  SLog.println("Initializing display");

  // Initialize filesystem. Both the CSV and merge_bin.py declare our data
  // partition as "assets"; "spiffs" is only reached on devices whose partition
  // table predates that rename (an app-only update does not rewrite the table).
  // Select on which PARTITION EXISTS, never on which mount succeeds: "spiffs"
  // is the ESP32 default name, so on a multi-firmware device it can belong to
  // another firmware, and mounting with format-on-fail would erase its data.
#ifdef MESHPUNK_EMBED_PACK
  ensure_data_partition();  // reboots if it had to create one
#endif
  const bool have_assets =
      esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, "assets") ||
      esp_partition_find_first(ESP_PARTITION_TYPE_DATA, (esp_partition_subtype_t)0x83, "assets");
  if (!have_assets)
    SLog.println("LittleFS: no \"assets\" partition, using legacy \"spiffs\" (pre-rename table)");
  const char* fs_label = have_assets ? "assets" : "spiffs";
  if (LittleFS.begin(true, "/littlefs", 10, fs_label)) {
    fs_mounted = true;
    g_lfs_mount_label = fs_label;
    SLog.printf("LittleFS mounted successfully (label: %s)\n", g_lfs_mount_label);
#ifdef MESHPUNK_EMBED_PACK
    // Fresh/wiped filesystem or a firmware update with new bundled files: the
    // pack embedded in this binary must be extracted. Deferred to setupLuaVGL()
    // so the display is up and a progress splash shows during the minutes-long
    // unpack (see s_pack_extract_pending).
    s_pack_extract_pending = pack_needs_extract();
    if (s_pack_extract_pending)
      SLog.println("[PACK] extraction needed - deferred until display is up");
#endif

    SLog.println("LittleFS contents:");
    listDir(LittleFS, "/lua");

    // Load firmware preferences (tz, use_sd, clock_fmt)
    firmware_prefs_load();
    // Alt emoji layer per-key map (defaults + /emoji_keymap overrides)
    input_ui_emoji_map_load();
    // Push the selection-highlight preferences into the live theme (the theme is
    // already inited; Lua applies the palette later and re-reads these).
    lv_theme_meshpunk_set_focus_solid(theme_focus_solid);
    lv_theme_meshpunk_set_focus_darken(theme_focus_darken);
  } else {
    SLog.println("Error mounting LittleFS!!");
  }

  // Initialize SD card for persistent mesh data (survives LittleFS reflash)
  SLog.println("===== SD CARD INIT =====");
  meshpunk_sd_mount();

  if (sd_mounted) {
    uint64_t cardSize = SD.cardSize() / (1024 * 1024);
    SLog.printf("[SD] Card mounted, size: %llu MB\n", cardSize);

    const char* required_dirs[] = {
      "/meshpunk",
      "/meshpunk/apps",
      "/meshpunk/messages",
      "/meshpunk/fonts",     // user-droppable .ttf files for Settings > Fonts
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
  the_mesh->_ble_sync_max_per_channel = ble_sync_max_per_channel;  // BLE backlog cap

  wifi_creds_load();
  // Creds live in LittleFS and we always call WiFi.begin() explicitly — stop
  // Arduino from ALSO writing SSID/pass to NVS on every begin(). That's an
  // internal-flash write that would crash an active USB audio stream, and it's
  // pure redundancy here. Process-wide setting, so once at boot covers all
  // later WiFi.begin() calls. (No UsbFlashGuard needed here: USB host is
  // manually started from the launcher, well after this boot code runs.)
  WiFi.persistent(false);
  // The stack's own auto-reconnect retries an unreachable network forever
  // (nonstop scan+auth = battery drain, and scans fail while it churns).
  // All reconnect policy lives in wifi_auto_tick()'s bounded rounds instead.
  WiFi.setAutoReconnect(false);
  if (wifi_enabled_pref) {
    WiFi.mode(WIFI_STA);
    if (!wifi_auto_kick()) {
      SLog.println("WiFi initialized in station mode (no saved network)");
    }
  } else {
    WiFi.mode(WIFI_OFF);
    SLog.println("WiFi disabled by preference");
  }
  log_boot_mem("after wifi");

  // I2C bus + touch controller + keyboard probe/mode (board input backend);
  // kbd_brightness is the persisted backlight level applied on probe success.
  input_dev_init(kbd_brightness);
  // Touch-input default, derived from what the backend just reported: live
  // on a keyboardless board, off (chord-reachable) where a keyboard exists.
  input_ui_touch_mode_init();

  // Initialize I2S audio output on T-Deck speaker
  audio = audio_dev_init();
  sound_init(audio, firmware_prefs_save);
  usb_manager_init(firmware_prefs_save);   // USB audio route/speaker prefs persist here
  notify_init();   // pre-render the melody + pre-alloc the notification log
                   // (both C-owned PSRAM, placed BEFORE the Lua arena, survive lua_close)
  // Boot watermark: every sound id below this is C-owned (the notify melody)
  // and survives every sweep; everything at/above it is Lua-created and gets
  // swept by luaTearDown on ELF launch (Lua handles die with lua_close anyway).
  s_boot_sound_mark = sound_mark();
  if (audio) audio->setVolume(sound_get_muted() ? 0 : sound_get_volume());
  SLog.printf("[AUDIO] %s init: vol=%d muted=%d\n",
              audio ? "I2S" : "no-I2S (buzzer/none backend)",
              sound_get_volume(), sound_get_muted() ? 1 : 0);
  log_boot_mem("after audio");

  // Initialize LORA Radio
  SLog.println(F("===== RADIO INIT ====="));

#if defined(BOARD_HELTEC_V4)
  // FEM LDO + type auto-detect + receive path, before the first radio access.
  // (PIN_BOARD_SDA/SCL=-1 in platformio.ini is load-bearing here: without
  // them ESP32Board::begin() runs Wire.begin() on the generic variant's
  // default I2C pins 8/9 — the radio's NSS/SCK — and kills all radio SPI.)
  board.begin();
#endif

  int16_t state = radio.begin();
  SLog.printf("[RADIO] begin() = %d %s\n", state, state == RADIOLIB_ERR_NONE ? "OK" : "FAILED");

#if defined(BOARD_HELTEC_V4)
  // SX1262 module wiring on this board: 1.8V TCXO on DIO3, the FEM's TX/RX
  // switch driven from DIO2, 140mA chip limit (the PA has its own LDO).
  state = radio.setTCXO(1.8);
  SLog.printf("[RADIO] setTCXO(1.8) = %d %s\n", state, state == RADIOLIB_ERR_NONE ? "OK" : "FAILED");
  state = radio.setDio2AsRfSwitch(true);
  SLog.printf("[RADIO] setDio2AsRfSwitch = %d %s\n", state, state == RADIOLIB_ERR_NONE ? "OK" : "FAILED");
  radio.setCurrentLimit(140);
#endif

  delay(100);

  SLog.println(F("===== MESHCORE INIT ====="));
  // fast_rng stays unseeded: StdRNG wraps Arduino ::random(), which reads the
  // esp_random() hardware TRNG until randomSeed() is called — seeding would
  // permanently switch every ::random() in the firmware to srand()-based rand().
  the_mesh->begin();
  the_mesh->showWelcome();

  // Radio params come from _prefs, which the_mesh->begin() loads from
  // /node_prefs; configuring the radio before that point programs the
  // constructor defaults instead of the saved settings.
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

  state = radio.setOutputPower(board_tx_dbm_to_chip(tx_pwr));
  SLog.printf("[RADIO] setOutputPower(chip %d for %d radiated) = %d %s\n",
              (int)board_tx_dbm_to_chip(tx_pwr), (int)tx_pwr,
              state, state == RADIOLIB_ERR_NONE ? "OK" : "FAILED");

  if (the_mesh->_prefs.rx_boost) {
    SPI_LOCK();
    radio_driver.setRxBoostedGainMode(true);
    SPI_UNLOCK();
    SLog.println("[RADIO] RX Boost restored from prefs: ON");
  }

  state = radio.startReceive();
  SLog.printf("[RADIO] startReceive = %d %s\n", state, state == RADIOLIB_ERR_NONE ? "OK" : "FAILED");

  // Seed the clock + own-position from the last saved GPS fix until live GPS
  // syncs (or the user manually sets the time). Storage is configured above.
  // The seeded location survives sync restarts (only a new fix replaces it),
  // so ordering vs. gps_sync_begin()/the gps_task no longer matters.
  gps_last_load();

  // Flag a boot catch-up retention sweep; pruneStep runs it incrementally from
  // loop() once the clock is valid (seeded above, or after the first GPS fix).
  the_mesh->_prune_due = true;

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
  display_dev_init();

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

  // Reserve the USB dynamic-driver pool NOW — before the first luaBringUp()
  // ever runs — so it lands at the bottom of PSRAM below the fonts, the 1MB
  // gap and the Lua arena. Driver modules then load/unload into the pool at
  // any session time without fragmenting the block ELF games coalesce (the
  // same long-lived-before-arena rule the font buffers follow; see
  // luaBringUp). The arena sizes itself dynamically, so the pool is
  // absorbed automatically.
  usb_driver_pool_init();
  log_boot_mem("after usb driver pool");

  // T-Deck↔T-Deck peer link bridge (Core-1 pump task; device-role serial +
  // session timers — the USB-host backend registers later via the tdeck
  // driver's link socket). See src/tdeck_link.cpp.
  tdeck_link_init();

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
  display_dev_backlight_init();
  display_dev_brightness(display_brightness);
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

// Dispatch the mic-key notifications shortcut into Lua (topbar.on_shortcut).
// The keyboard reader (LVGL indev callback) only sets the flag; the Lua call
// happens here, outside indev processing. During ELF runs L is NULL and the
// press is dropped — the melody/blink already announce notifications
// mid-module, and the store is reviewed after the run.
static void dispatch_topbar_shortcut() {
  if (!input_ui_take_topbar_shortcut()) return;
  if (!L) return;
  lua_getglobal(L, "require");
  lua_pushstring(L, "lib/topbar");
  if (lua_pcall(L, 1, 1, 0) != LUA_OK) { lua_pop(L, 1); return; }
  lua_getfield(L, -1, "on_shortcut");
  lua_remove(L, -2);   // drop the module table
  if (!lua_isfunction(L, -1)) { lua_pop(L, 1); return; }
  if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
    SLog.printf("[topbar] on_shortcut error: %s\n", lua_tostring(L, -1));
    lua_pop(L, 1);
  }
}

// Dispatch the alt+backspace home chord into Lua (apps.home_shortcut: closes
// any parentless popups, then go_home). Same deferral as the shortcuts above;
// during ELF runs L is NULL and the hold is the ELF host's own exit chord.
static void dispatch_home_shortcut() {
  if (!input_ui_take_home_shortcut()) return;
  if (!L) return;
  lua_getglobal(L, "require");
  lua_pushstring(L, "lib/apps");
  if (lua_pcall(L, 1, 1, 0) != LUA_OK) { lua_pop(L, 1); return; }
  lua_getfield(L, -1, "home_shortcut");
  lua_remove(L, -2);   // drop the module table
  if (!lua_isfunction(L, -1)) { lua_pop(L, 1); return; }
  if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
    SLog.printf("[home] home_shortcut error: %s\n", lua_tostring(L, -1));
    lua_pop(L, 1);
  }
}

// Auto-enable legacy ASCII keyboard mode when the backend's detection
// heuristic (input_tdeck.cpp, run from the UI keyboard poll) flagged old
// keyboard-MCU firmware. Runs here so the prefs flash write happens outside
// LVGL indev processing.
static void dispatch_kb_legacy_autoswitch() {
  if (!input_dev_kbd_legacy_autoswitch_pending()) return;
  input_ui_set_legacy(true, /*save=*/true);
  notify_post("Old keyboard firmware detected - compatibility mode enabled: "
              "keys register one at a time, key combos and holds won't work "
              "(restart device to exit games). Toggle in Settings > Device");
  SLog.println("[KB] old keyboard firmware detected, legacy ASCII mode auto-enabled");
  // On-screen toast on top of the bell notification (apps.kb_legacy_popup →
  // utils.createNotification). Same Lua-call shape as the dispatches above.
  if (!L) return;
  lua_getglobal(L, "require");
  lua_pushstring(L, "lib/apps");
  if (lua_pcall(L, 1, 1, 0) != LUA_OK) { lua_pop(L, 1); return; }
  lua_getfield(L, -1, "kb_legacy_popup");
  lua_remove(L, -2);   // drop the module table
  if (!lua_isfunction(L, -1)) { lua_pop(L, 1); return; }
  if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
    SLog.printf("[KB] kb_legacy_popup error: %s\n", lua_tostring(L, -1));
    lua_pop(L, 1);
  }
}

// Dispatch the alt+mic emoji-popup shortcut into Lua (lib/emoji_popup
// .on_shortcut). Same deferral as dispatch_topbar_shortcut above.
// Touch-input mode trigger — the board's aux button (the Heltec IO key), the
// Shift+Alt chord on boards with a keyboard, or a tap on an on-screen MODE
// zone. All three land in lib/touchlayout.lua's mode manager, which cycles
// the shared mode and applies it to the running app.
static void dispatch_mode_button() {
  bool from_btn   = input_dev_aux_btn_take();
  bool from_chord = input_ui_take_touch_chord();
  bool from_zone  = input_zones_mode_toggle_take();
  if (!from_btn && !from_chord && !from_zone) return;
  if (!L) return;
  lua_getglobal(L, "require");
  lua_pushstring(L, "lib/touchlayout");
  if (lua_pcall(L, 1, 1, 0) != LUA_OK) { lua_pop(L, 1); return; }
  lua_getfield(L, -1, "on_mode_button");
  lua_remove(L, -2);   // drop the module table
  if (!lua_isfunction(L, -1)) { lua_pop(L, 1); return; }
  if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
    SLog.printf("[touchlayout] on_mode_button error: %s\n", lua_tostring(L, -1));
    lua_pop(L, 1);
  }
}

// On-screen keyboard (keyboardless boards): a textarea focus/tap queued the
// OSK in input_ui; open lib/osk.lua outside indev processing. Same pattern
// as the emoji popup below.
static void dispatch_osk() {
  if (!input_ui_take_osk()) return;
  if (!L) return;
  lua_getglobal(L, "require");
  lua_pushstring(L, "lib/osk");
  if (lua_pcall(L, 1, 1, 0) != LUA_OK) { lua_pop(L, 1); return; }
  lua_getfield(L, -1, "open");
  lua_remove(L, -2);   // drop the module table
  if (!lua_isfunction(L, -1)) { lua_pop(L, 1); return; }
  if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
    SLog.printf("[osk] open error: %s\n", lua_tostring(L, -1));
    lua_pop(L, 1);
  }
}

static void dispatch_emoji_popup() {
  if (!input_ui_take_emoji_popup()) return;
  if (!L) return;
  lua_getglobal(L, "require");
  lua_pushstring(L, "lib/emoji_popup");
  if (lua_pcall(L, 1, 1, 0) != LUA_OK) { lua_pop(L, 1); return; }
  lua_getfield(L, -1, "on_shortcut");
  lua_remove(L, -2);   // drop the module table
  if (!lua_isfunction(L, -1)) { lua_pop(L, 1); return; }
  if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
    SLog.printf("[emoji_popup] on_shortcut error: %s\n", lua_tostring(L, -1));
    lua_pop(L, 1);
  }
}

// ── Power off ──────────────────────────────────────────────────────────────
// Device-neutral teardown, then the board backend parks its rails and deep
// sleeps (power_dev_shutdown never returns; GPIO0 reboots). Runs from the
// top of loop() via s_poweroff_request — see the _system_poweroff binding.
static void system_shutdown() {
  SLog.println("[POWER] shutting down");
  MESH_LOCK();
  the_mesh->flushForShutdown();
  MESH_UNLOCK();
  mesh_task_paused = true;
  delay(60);   // let an in-flight dispatcher tick finish before the radio sleeps
#if BLE_COMPANION_ENABLED
  if (ble_serial) { MESH_LOCK(); ble_companion_stop(); MESH_UNLOCK(); }
#endif
  WiFi.mode(WIFI_OFF);
  MESH_LOCK();
  radio_driver.powerOff();   // SX1262 -> sleep (SPI-locked inside)
  MESH_UNLOCK();
  if (sd_mounted) {
    sd_spi_take();
    SD.end();
    sd_mounted = false;
    sd_spi_release();
  }
  input_dev_shutdown_prepare();   // kbd backlight off; T-Deck: GT911 sleep
  gps_dev_power_down();           // T-Deck: CFG-SLEEP (always-on rail); Heltec: no-op
  display_dev_fill_black();
  display_dev_sleep(true);
  display_dev_brightness(0);
  SLog.println("[POWER] entering deep sleep (wake: GPIO0)");
  delay(20);   // let the serial line drain
  power_dev_shutdown();   // never returns
}

// ── Standby: screen off, mesh alive, wake on notification or button ────────
// Manual light-sleep loop. Lua/LVGL stay frozen in RAM (no teardown); the
// mesh task keeps running in the awake windows between sleeps, so the node
// still ACKs and syncs. Wakes fully when notify classifies an RX as alert-
// worthy (notify_standby_* — the same per-channel/DM gates as the melody),
// or when GPIO0 (trackball click / USER) is pressed. Runs from the top of
// loop() via s_standby_request.
static void standby_run() {
  SLog.println("[POWER] standby: wake on notification or GPIO0");
  uint32_t t0 = millis();

  // REQUIRED before the loop freezes: the tap that chose Standby deleted the
  // drop-down from inside its own event handler, and that interaction's
  // indev/render work is still pending when the request flag lands here one
  // tick later. Light-sleeping on top of that pending work hangs
  // esp_light_sleep_start — LVGL must be pumped to completion first.
  for (int i = 0; i < 3; i++) { lv_timer_handler(); delay(25); }

  notify_standby_defer(true);   // alerts are recorded, replayed on exit
#if BLE_COMPANION_ENABLED
  bool ble_was_on = (ble_serial != nullptr);
  if (ble_was_on) { MESH_LOCK(); ble_companion_stop(); MESH_UNLOCK(); }
#endif
  bool wifi_was_on = (WiFi.getMode() != WIFI_OFF);
  if (wifi_was_on) WiFi.mode(WIFI_OFF);
  // Halt the I2S DMA + sound task (the ELF-takeover seam) — no peripheral
  // DMA runs across the light-sleep freezes, and any playing music stops
  // cleanly instead of stuttering through the drain windows. The deferred
  // alert replays after sound_resume() on exit.
  sound_suspend();

  display_dev_brightness(0);
  screen_timed_out = true;
  input_dev_kbd_backlight(0);
  kbd_timed_out = true;
  display_dev_sleep(true);      // frame memory survives; wake shows the old screen
  power_dev_standby_enter();    // Heltec: GNSS rail off

  // Keep the wake pins' runtime input config through light sleep instead of
  // letting the pads switch to their sleep configuration at entry.
  gpio_sleep_sel_dis((gpio_num_t)PIN_LORA_DIO1);
  gpio_sleep_sel_dis((gpio_num_t)PIN_BOOT_BTN);
  // The ext0 wake below switches GPIO0 to its RTC function during sleep,
  // where the digital INPUT_PULLUP doesn't apply — enable the RTC-domain
  // pull (the shutdown recipe) so the pad can't float LOW and insta-wake.
  rtc_gpio_pullup_en((gpio_num_t)PIN_BOOT_BTN);
  rtc_gpio_pulldown_dis((gpio_num_t)PIN_BOOT_BTN);
  // Keep the RTC peripherals powered through light sleep — the wake logic
  // depends on them.
  esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);
  // Hand GPIO0 over to wake duty: its click ISR is detached for the whole
  // standby; the drain windows read the pin level directly instead.
  input_dev_wake_pin_release();
  gps_dev_power_down();         // T-Deck: CFG-SLEEP (always-on rail); Heltec: no-op

  // A press is visible two ways: the wake-status/level right after a sleep,
  // and the click ISR counter during the awake drain windows. Clear stale
  // counts so the entry tap can't instantly bounce us back out.
  trackball_up = trackball_down = trackball_left = trackball_right = 0;
  trackball_click = 0;


  uint32_t hb_last = millis();   // heartbeat pacing (first glow after one interval)

  bool user_wake = false;
  while (true) {
    // An alert can fire before the first sleep (packet during the entry
    // window) — never sleep past one.
    if (notify_standby_alert_pending()) break;
    // Quiesce the mesh before freezing the chip: holding MESH_LOCK means the
    // dispatcher's current tick has COMPLETED — Core 1 is never frozen
    // mid-SPI, mid-TX-start or mid-persist, and the RX-mode check below
    // cannot be raced by a new TX between check and sleep. The mesh task
    // resumes at MESH_UNLOCK to drain whatever woke us.
    MESH_LOCK();
    bool slept = false;
    if (radio_driver.isInRecvMode()) {
      // Level wakes: DIO1 idles LOW and latches HIGH on RX-done; GPIO0
      // idles HIGH (its pull survives light sleep) and reads LOW pressed.
      // The timer is a safety net: if the DIO1 level wake ever fails to
      // fire, the next timer wake polls the radio's IRQ register instead,
      // bounding notification latency at ~15s.
      //
      // STORM GUARD: DIO1's interrupt-enable must be OFF while its type is
      // level-HIGH. A packet holds DIO1 HIGH until the mesh task services
      // the radio over SPI; with the interrupt enabled, the unmask at sleep
      // exit fires a level ISR whose handler never clears the SOURCE — an
      // infinite gpio_intr_service loop that trips the interrupt watchdog
      // and that panic's CPU-only reset preserves the armed state, so the
      // device boot-loops on the stale asserted status. The gpio WAKE logic
      // works with the interrupt disabled. (GPIO0's ISR is already detached
      // for the whole standby.)
      gpio_intr_disable((gpio_num_t)PIN_LORA_DIO1);
      gpio_wakeup_enable((gpio_num_t)PIN_LORA_DIO1, GPIO_INTR_HIGH_LEVEL);
      gpio_wakeup_enable((gpio_num_t)PIN_BOOT_BTN,  GPIO_INTR_LOW_LEVEL);
      esp_sleep_enable_gpio_wakeup();
      // The button additionally wakes through ext0 — the RTC-domain path the
      // shutdown wake already proved on this exact pin. DIO1 (GPIO45) is not
      // an RTC pad, so it can only use the digital gpio wake + the timer net.
      esp_sleep_enable_ext0_wakeup((gpio_num_t)PIN_BOOT_BTN, 0);
      esp_sleep_enable_timer_wakeup(15ULL * 1000000ULL);
      esp_light_sleep_start();
      gpio_wakeup_disable((gpio_num_t)PIN_LORA_DIO1);
      gpio_wakeup_disable((gpio_num_t)PIN_BOOT_BTN);
      // Restore RadioLib's edge semantics before re-enabling: POSEDGE on an
      // already-high line does not fire, and a packet latched during the
      // sleep is picked up by the IRQ-register fallback armed below.
      gpio_set_intr_type((gpio_num_t)PIN_LORA_DIO1, GPIO_INTR_POSEDGE);
      gpio_intr_enable((gpio_num_t)PIN_LORA_DIO1);
      slept = true;
    }
    MESH_UNLOCK();
    if (slept) {

      // A press outlasts the microseconds from wake to this read, so the
      // live level identifies a button wake. (esp_sleep_get_gpio_wakeup_
      // status() is C-series-only — not available on the S3.)
      if (digitalRead(PIN_BOOT_BTN) == LOW) {
        user_wake = true;
        break;
      }
      // Any wake may carry a latched-but-unsignalled RX: the RX-done edge is
      // lost with the gated clocks, so arm the wrapper's IRQ-register
      // fallback for the dispatcher's next recvRaw().
      radio_driver.noteLightSleepWake();
    }
    // Drain window: the mesh task pulls the packet, ACKs, and classifies.
    // Heartbeat: a periodic aliveness glow, decoupled from the wake rate —
    // it rides existing drain windows (never creates a wake of its own) and
    // lights at most once per standby_heartbeat_secs. The 15s timer wake
    // bounds how far past due it can run (device setting; no kbd = no-op).
    bool hb_glow = standby_heartbeat &&
                   (millis() - hb_last >= (uint32_t)standby_heartbeat_secs * 1000UL);
    if (hb_glow) { input_dev_kbd_backlight(40); hb_last = millis(); }
    uint32_t drain_start = millis();
    while (millis() - drain_start < 400) {
      if (notify_standby_alert_pending()) break;
      if (trackball_click > 0 || digitalRead(PIN_BOOT_BTN) == LOW) { user_wake = true; break; }
      delay(10);
    }
    if (hb_glow) input_dev_kbd_backlight(0);
    if (user_wake || notify_standby_alert_pending()) break;
    // Not alert-worthy (advert / foreign traffic / ACK): back to sleep. If
    // the radio is mid-TX, loop without sleeping until it returns to RX.
    if (!radio_driver.isInRecvMode()) delay(20);
  }

  // ── Restore ──
  // gpio_wakeup_enable/disable rewrote these pins' interrupt TYPE, which
  // kills RadioLib's DIO1 RX-done edge ISR and the GPIO0 click ISR for good
  // — restore both (the registered handlers themselves were untouched).
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_TIMER);
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_EXT0);
  gpio_set_intr_type((gpio_num_t)PIN_LORA_DIO1, GPIO_INTR_POSEDGE);
  gpio_intr_enable((gpio_num_t)PIN_LORA_DIO1);
  input_dev_wake_pin_restore();   // re-attach the GPIO0 click ISR (edge config included)
  power_dev_standby_exit();     // Heltec: GNSS rail back on
  display_dev_sleep(false);
  // Full backlight re-init, not the incremental path: after minutes held in
  // shutdown the pulse chip's state cannot be trusted to match the driver's
  // tracking, and a mismatch leaves the screen black with no self-heal.
  display_dev_backlight_reset(display_brightness);
  wake_activity();              // backlights + inactivity timers
  trackball_up = trackball_down = trackball_left = trackball_right = 0;
  trackball_click = 0;          // the wake press must not click whatever was focused
  if (wifi_was_on && wifi_enabled_pref) { WiFi.mode(WIFI_STA); wifi_auto_kick(); }
#if BLE_COMPANION_ENABLED
  if (ble_was_on && ble_enabled_pref) {
    MESH_LOCK();
    ble_companion_init_early();
    ble_companion_start(*the_mesh);
    MESH_UNLOCK();
  }
#endif
  gps_dev_wake();               // T-Deck: RXD activity wake; Heltec: no-op
  gps_notify_wake();            // fresh GPS sync cycle
  sound_resume();               // I2S back before the deferred alert replays
  notify_standby_defer(false);
  if (notify_standby_take_alert()) notify_message_alert();
  SLog.printf("[POWER] standby end (%s) after %lus\n",
              user_wake ? "button" : "notification",
              (unsigned long)((millis() - t0) / 1000UL));
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

  // Power menu actions, deferred here from their Lua bindings so they run on
  // a clean stack after the farewell painted (see system_shutdown/standby_run).
  if (s_poweroff_request) {
    s_poweroff_request = false;
    system_shutdown();   // never returns
  }
  if (s_standby_request) {
    s_standby_request = false;
    standby_run();       // blocks here until woken
    return;   // fresh tick for the restored UI
  }

  // Handle LVGL tasks
  lv_timer_handler();

  // Audio file decode (ESP32-audioI2S) now runs on Core 1 inside sound_task —
  // moved off this loop so a heavy MP3/FLAC decode can't stutter LVGL/Lua.
  // See sound.cpp: the s_audio->isRunning() branch pumps audio->loop() there.

  // GPS one-shot time sync runs on Core 1 (gps_task). Nothing to do here.

  // Flush mesh RX events into Lua. lua_State is single-threaded — always
  // touched from Core 0.
  drain_rx_events();

  // Mic-key notifications shortcut (flag set by the keyboard reader).
  dispatch_topbar_shortcut();

  // Alt+mic emoji search popup (same flag pattern).
  dispatch_emoji_popup();

  // On-screen keyboard open request (keyboardless boards; same flag pattern).
  dispatch_osk();

  // Input-mode button (IO key / MODE zone tap; same flag pattern).
  dispatch_mode_button();

  // Alt+backspace home chord (same flag pattern).
  dispatch_home_shortcut();

  // Old-keyboard-firmware auto-switch (flag set by the keyboard reader).
  dispatch_kb_legacy_autoswitch();

  // USB drive mode watchdog: force-stops a session when the Tools/"USB
  // Drive" app stops pinging (any teardown path). Cheap no-op when idle.
  usbdrive_tick();

  // Incremental message/routing retention sweep (flagged on a new-day record or
  // at boot). One file per iteration; cheap no-op when nothing is due.
  if (the_mesh) the_mesh->pruneStep();

  // Bounded WiFi auto-connect rounds (scan → join known networks → park the
  // radio when nothing is reachable). Self-rate-limited to 4 Hz.
  wifi_auto_tick();

  // ── Inactivity timeouts ─────────────────────────────────────────────────
  if (screen_timeout_secs > 0 && !screen_timed_out) {
    if (millis() - last_activity_ms > (uint32_t)screen_timeout_secs * 1000UL) {
      display_dev_brightness(0);
      screen_timed_out = true;
    }
  }
  if (kbd_timeout_secs > 0 && !kbd_timed_out) {
    if (millis() - last_activity_ms > (uint32_t)kbd_timeout_secs * 1000UL) {
      input_dev_kbd_backlight(0);
      kbd_timed_out = true;
    }
  }

  // Auto standby: enter the low-power state after the configured idle time,
  // on the same activity clock as the timeouts above. The standby binding's
  // guards apply here too (never while a USB drive or link session owns the
  // hardware). wake_activity() at standby exit resets the idle clock, so
  // the next idle period re-arms naturally.
  if (auto_standby && auto_standby_mins > 0 && !s_standby_request &&
      !usbdrive_active() && !mesh_task_paused &&
      millis() - last_activity_ms > (uint32_t)auto_standby_mins * 60000UL) {
    SLog.println("[POWER] auto standby (idle timeout)");
    s_standby_request = true;
  }
}
