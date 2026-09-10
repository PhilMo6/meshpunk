// main_updater.cpp — MeshPunk updater firmware. Lives in the `updater`
// factory partition of meshpunk_custom_16Mb.csv (envs meshpunk_updater and
// meshpunk_heltec_updater) and writes a staged app image into the `main`
// (ota_0) partition, then boots it.
//
// Handoff from the main firmware (src/ota_update.cpp): it stages the image on
// the SD card or LittleFS, writes L:/.ota_job and calls
// esp_ota_set_boot_partition() on this partition, which erases otadata; a
// blank otadata boots the factory partition, i.e. this program. Job file:
// text, one key=value per line:
//   path=<S:/... or L:/...>   staged image
//   size=<bytes>
//   sha256=<64 hex>           the image's appended SHA-256
//   tag=<release tag or "file">
//
// Exit paths, each printed to serial and drawn on the panel:
//   job verified  -> image written, otadata -> main, job + file deleted, restart
//   job rejected  -> nothing written, job deleted, main booted if it holds an image
//   no job        -> S:/meshpunk/ota/ scanned for a release -firmware.bin of
//                    this board (SD recovery), else main booted
//   main empty, or rolled back by the bootloader (ESP_OTA_IMG_ABORTED) -> halt
//   write failure -> halt with the job kept: the next boot lands here and retries
//
// The panel comes up with the same TFT_eSPI configuration as the main build
// (the env extends the board env), the SD card on the same bus and chip
// select, and nothing else: no WiFi, no BLE, no input, no PSRAM use.

#include <Arduino.h>
#include <SPI.h>
#include <FS.h>
#include <SD.h>
#include <LittleFS.h>
#include <Update.h>
#include <TFT_eSPI.h>
#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "mbedtls/sha256.h"
#include "boards/board_pins.h"
#include "ota_tag.h"

static const char*  kJobPath     = "/.ota_job";      // on the assets LittleFS
static const char*  kRecoveryDir = "/meshpunk/ota";  // on the SD card
static const size_t kChunk       = 16384;

// This program's board tag; the main firmware finds it in the updater
// partition and refuses to hand off to an updater built for another board.
static const char kUpdaterTag[] = "MESHPUNK-UPDATER:" MESHPUNK_BOARD_NAME;

static uint8_t  s_buf[kChunk];
static TFT_eSPI tft;
static bool     s_sd_mounted = false;

// ── Board bring-up ───────────────────────────────────────────────────────────

#if defined(BOARD_TDECK)

// The SD slot shares the FSPI bus with the panel and the radio.
SPIClass& board_sd_spi(void) { return SPI; }

static void board_init(void) {
  if (esp_reset_reason() == ESP_RST_DEEPSLEEP) {
    // The main firmware's power-off holds this pad through deep sleep.
    rtc_gpio_hold_dis((gpio_num_t)BOARD_POWERON);
    rtc_gpio_deinit((gpio_num_t)BOARD_POWERON);
  }
  // V3V peripheral rail (panel + SD): cycled off and on so both start from
  // power-on rather than from the state the main firmware left them in at
  // the software reset.
  pinMode(BOARD_POWERON, OUTPUT);
  digitalWrite(BOARD_POWERON, LOW);
  delay(150);
  digitalWrite(BOARD_POWERON, HIGH);
  pinMode(PIN_LORA_CS, OUTPUT);
  digitalWrite(PIN_LORA_CS, HIGH);
  pinMode(PIN_SD_CS, OUTPUT);
  digitalWrite(PIN_SD_CS, HIGH);
  pinMode(PIN_TFT_CS, OUTPUT);
  digitalWrite(PIN_TFT_CS, HIGH);
  pinMode(PIN_SPI_MISO, INPUT_PULLUP);
  SPI.begin(PIN_SPI_SCK, PIN_SPI_MISO, PIN_SPI_MOSI);
  delay(300);   // rail settle before the first peripheral access
}

// Pulse-counted backlight chip on BOARD_BL_PIN: a LOW of 3 ms or more clears
// it, the following HIGH lights it at full brightness.
static void board_backlight_on(void) {
  pinMode(BOARD_BL_PIN, OUTPUT);
  digitalWrite(BOARD_BL_PIN, LOW);
  delay(4);
  digitalWrite(BOARD_BL_PIN, HIGH);
}

#elif defined(BOARD_HELTEC_V4)

#define HELTEC_VEXT_CTRL  40   // P-FET gate: LOW = Vext rail (panel + SD) on
#define HELTEC_TFT_BL_PIN 44
#define HELTEC_BL_LEDC_CH 1

// The SD slot shares the panel's HSPI bus (SCK 16 / MISO 45 / MOSI 15).
SPIClass& board_sd_spi(void) {
  static SPIClass hspi(HSPI);
  static bool begun = false;
  if (!begun) {
    hspi.begin(16, 45, 15, -1);
    begun = true;
  }
  return hspi;
}

static void board_init(void) {
  if (esp_reset_reason() == ESP_RST_DEEPSLEEP) {
    // The main firmware's power-off holds this pad through deep sleep.
    gpio_hold_dis((gpio_num_t)HELTEC_VEXT_CTRL);
  }
  // Vext rail (panel + SD): cycled off and on so both start from power-on
  // rather than from the state the main firmware left them in at the
  // software reset.
  pinMode(HELTEC_VEXT_CTRL, OUTPUT);
  digitalWrite(HELTEC_VEXT_CTRL, HIGH);
  delay(150);
  digitalWrite(HELTEC_VEXT_CTRL, LOW);
  pinMode(PIN_LORA_CS, OUTPUT);
  digitalWrite(PIN_LORA_CS, HIGH);
  pinMode(PIN_SD_CS, OUTPUT);
  digitalWrite(PIN_SD_CS, HIGH);
  delay(300);   // rail settle before the first peripheral access
  board_sd_spi();
}

static void board_backlight_on(void) {
  ledcSetup(HELTEC_BL_LEDC_CH, 44000, 8);
  ledcAttachPin(HELTEC_TFT_BL_PIN, HELTEC_BL_LEDC_CH);
  ledcWrite(HELTEC_BL_LEDC_CH, 255);
}

#endif

// ── Panel text ───────────────────────────────────────────────────────────────

static void ui_init(void) {
  tft.begin();
  tft.setRotation(1);   // 320 x 240 landscape, as the main firmware
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextDatum(TL_DATUM);
  tft.setTextFont(4);
  tft.drawString("MeshPunk updater", 10, 8);
  tft.setTextFont(2);
}

// Six 20 px text rows under the title (rows 0..5).
static void ui_row(int row, const char* text) {
  int y = 48 + row * 20;
  tft.fillRect(0, y, 320, 20, TFT_BLACK);
  tft.setTextFont(2);
  tft.drawString(text, 10, y);
}

static void say(int row, const char* text) {
  Serial.printf("[UPD] %s\n", text);
  ui_row(row, text);
}

static void ui_progress(uint32_t done, uint32_t total) {
  const int x = 10, y = 176, w = 300, h = 18;
  tft.drawRect(x, y, w, h, TFT_WHITE);
  int fill = total ? (int)((uint64_t)(w - 2) * done / total) : 0;
  tft.fillRect(x + 1, y + 1, fill, h - 2, TFT_GREEN);
  char line[48];
  snprintf(line, sizeof line, "%u / %u KB", (unsigned)(done / 1024), (unsigned)(total / 1024));
  ui_row(5, line);
}

// Loud stop: the three lines stay on the panel and repeat on serial.
static void halt(const char* l1, const char* l2, const char* l3) {
  say(3, l1);
  say(4, l2);
  say(5, l3);
  for (;;) {
    delay(10000);
    Serial.printf("[UPD] halted: %s | %s | %s\n", l1, l2, l3);
  }
}

// ── Files ────────────────────────────────────────────────────────────────────

// Up to four rounds of 25 MHz then 4 MHz, 250 ms apart, each failure logged.
static bool sd_mount(void) {
  if (s_sd_mounted) return true;
  static const uint32_t freqs[] = {25000000U, 4000000U};
  for (int attempt = 1; attempt <= 4; attempt++) {
    for (uint32_t f : freqs) {
      if (SD.begin(PIN_SD_CS, board_sd_spi(), f)) {
        s_sd_mounted = true;
        Serial.printf("[UPD] SD mounted at %lu Hz (attempt %d)\n", (unsigned long)f, attempt);
        return true;
      }
      SD.end();
      Serial.printf("[UPD] SD mount attempt %d at %lu Hz failed\n", attempt, (unsigned long)f);
    }
    delay(250);
  }
  Serial.println("[UPD] SD mount failed");
  return false;
}

static bool is_sd_path(const String& p)  { return p.startsWith("S:/"); }
static bool is_lfs_path(const String& p) { return p.startsWith("L:/"); }

// "S:/x" and "L:/x" carry the same drive prefixes as the main firmware's
// paths; the bare path starts at the '/'.
static fs::File open_image(const String& path) {
  if (is_sd_path(path)) {
    if (!sd_mount()) return fs::File();
    return SD.open(path.c_str() + 2, FILE_READ);
  }
  if (is_lfs_path(path)) return LittleFS.open(path.c_str() + 2, "r");
  return fs::File();
}

static void remove_image(const String& path) {
  if (is_sd_path(path)) {
    if (sd_mount()) SD.remove(path.c_str() + 2);
  } else if (is_lfs_path(path)) {
    LittleFS.remove(path.c_str() + 2);
  }
}

struct Job {
  bool     present = false;
  String   path;
  uint32_t size = 0;
  String   sha256;
  String   tag;
};

static Job read_job(void) {
  Job j;
  fs::File f = LittleFS.open(kJobPath, "r");
  if (!f) return j;
  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    int eq = line.indexOf('=');
    if (eq <= 0) continue;
    String key = line.substring(0, eq);
    String val = line.substring(eq + 1);
    if (key == "path")        j.path = val;
    else if (key == "size")   j.size = (uint32_t)strtoul(val.c_str(), nullptr, 10);
    else if (key == "sha256") j.sha256 = val;
    else if (key == "tag")    j.tag = val;
  }
  f.close();
  j.present = j.path.length() > 0;
  return j;
}

// No job: a release -firmware.bin for this board dropped into S:/meshpunk/ota/
// is installed as a recovery. The first matching name wins.
static String sd_recovery_file(void) {
  if (!sd_mount()) return String();
  fs::File dir = SD.open(kRecoveryDir);
  if (!dir || !dir.isDirectory()) return String();
  String prefix = String("meshpunk-") + MESHPUNK_BOARD_NAME + "-";
  String found;
  for (fs::File e = dir.openNextFile(); e; e = dir.openNextFile()) {
    String name = e.name();
    bool is_dir = e.isDirectory();
    e.close();
    if (!is_dir && name.startsWith(prefix) && name.endsWith("-firmware.bin")) {
      found = String("S:") + kRecoveryDir + "/" + name;
      break;
    }
  }
  dir.close();
  return found;
}

// ── Image checks and the write ───────────────────────────────────────────────

static void to_hex(const uint8_t* in, size_t n, char* out) {
  static const char* digits = "0123456789abcdef";
  for (size_t i = 0; i < n; i++) {
    out[2 * i]     = digits[in[i] >> 4];
    out[2 * i + 1] = digits[in[i] & 15];
  }
  out[2 * n] = 0;
}

// nullptr when the file is an ESP32-S3 app image whose appended SHA-256 (the
// last 32 bytes, covering every byte before them) matches, else the reason.
// expect_size 0 and an empty expect_sha skip the job cross-checks.
static const char* verify_image(fs::File& f, uint32_t expect_size, const String& expect_sha,
                                uint32_t slot_size) {
  uint32_t size = f.size();
  if (size < 24 + 32) return "file too small for an app image";
  if (expect_size && size != expect_size) return "file size differs from the job";
  if (size > slot_size) return "image larger than the main partition";

  uint8_t hdr[24];
  if (!f.seek(0) || f.read(hdr, sizeof hdr) != sizeof hdr) return "header read failed";
  if (hdr[0] != 0xE9) return "not an ESP32 app image (magic byte)";
  uint16_t chip = (uint16_t)hdr[12] | ((uint16_t)hdr[13] << 8);
  if (chip != 0x0009) return "not an ESP32-S3 image (chip id)";
  if (hdr[23] != 1) return "image carries no appended hash";

  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx);
  mbedtls_sha256_starts_ret(&ctx, 0);
  if (!f.seek(0)) {
    mbedtls_sha256_free(&ctx);
    return "seek failed";
  }
  OtaTagScan tag;
  ota_tag_scan_init(tag, false);
  uint32_t left = size - 32;
  while (left) {
    size_t want = left < kChunk ? left : kChunk;
    if (f.read(s_buf, want) != want) {
      mbedtls_sha256_free(&ctx);
      return "read failed while hashing";
    }
    mbedtls_sha256_update_ret(&ctx, s_buf, want);
    ota_tag_scan_feed(tag, s_buf, want);
    left -= want;
  }
  uint8_t calc[32];
  mbedtls_sha256_finish_ret(&ctx, calc);
  mbedtls_sha256_free(&ctx);
  ota_tag_scan_finish(tag);

  uint8_t tail[32];
  if (f.read(tail, sizeof tail) != sizeof tail) return "digest read failed";
  if (memcmp(calc, tail, sizeof tail) != 0) return "appended SHA-256 mismatch (file corrupt)";

  if (expect_sha.length() == 64) {
    char hex[65];
    to_hex(calc, 32, hex);
    if (!expect_sha.equalsIgnoreCase(hex)) return "SHA-256 differs from the job";
  }

  // Board tag (ota_tag.h): both boards are ESP32-S3, so this is the only
  // thing that tells their images apart.
  if (!tag.found) return "image carries no board tag (built before the on-device updater)";
  if (strcmp(tag.slug, MESHPUNK_BOARD_NAME) != 0) {
    static char reason[96];
    snprintf(reason, sizeof reason, "image is for board %s, this is %s", tag.slug, MESHPUNK_BOARD_NAME);
    return reason;
  }
  return nullptr;
}

// Streams the file into the main partition through the Update library, which
// targets the first OTA slot when running from a factory partition, stashes
// the header until the end, and switches otadata to the slot in end().
static const char* write_image(fs::File& f, uint32_t size) {
  const esp_partition_t* target = esp_ota_get_next_update_partition(NULL);
  if (!target || target->subtype != ESP_PARTITION_SUBTYPE_APP_OTA_0)
    return "update target is not the main (ota_0) partition";
  Serial.printf("[UPD] writing %u bytes to %s @0x%06x\n", (unsigned)size, target->label,
                (unsigned)target->address);
  if (!Update.begin(size, U_FLASH)) return Update.errorString();
  if (!f.seek(0)) {
    Update.abort();
    return "seek failed";
  }
  uint32_t done = 0, shown = 0;
  ui_progress(0, size);
  while (done < size) {
    size_t want = (size - done) < kChunk ? (size - done) : kChunk;
    if (f.read(s_buf, want) != want) {
      Update.abort();
      return "read failed while writing";
    }
    if (Update.write(s_buf, want) != want) {
      const char* err = Update.errorString();
      Update.abort();
      return err;
    }
    done += want;
    if (done - shown >= 65536 || done == size) {
      ui_progress(done, size);
      shown = done;
    }
  }
  if (!Update.end()) return Update.errorString();
  return nullptr;
}

// ── Boot the main firmware, or say exactly why not ───────────────────────────

static void boot_main(void) {
  const esp_partition_t* m =
      esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, NULL);
  uint8_t magic = 0;
  if (!m || esp_partition_read(m, 0, &magic, 1) != ESP_OK || magic != 0xE9)
    halt("No firmware in the main partition.",
         "Flash the -merged.bin by USB, or put a release",
         "-firmware.bin in S:/meshpunk/ota/ and restart.");
  esp_ota_img_states_t state;
  if (esp_ota_get_state_partition(m, &state) == ESP_OK && state == ESP_OTA_IMG_ABORTED)
    halt("Main firmware crashed on its first start and",
         "was rolled back by the bootloader.",
         "USB flash, or SD recovery (S:/meshpunk/ota/).");
  esp_err_t err = esp_ota_set_boot_partition(m);
  if (err != ESP_OK)
    halt("Main firmware image failed verification:",
         esp_err_to_name(err),
         "USB flash, or SD recovery (S:/meshpunk/ota/).");
  say(3, "Starting main firmware");
  delay(800);
  esp_restart();
}

// ── Program ──────────────────────────────────────────────────────────────────

void setup() {
  Serial.begin(115200);
  board_init();
  ui_init();
  board_backlight_on();
  Serial.printf("[UPD] MeshPunk updater, board %s (%s)\n", MESHPUNK_BOARD_NAME, kUpdaterTag);
  const esp_partition_t* running = esp_ota_get_running_partition();
  Serial.printf("[UPD] running from %s @0x%06x\n", running ? running->label : "?",
                running ? (unsigned)running->address : 0u);

  bool lfs = LittleFS.begin(false, "/littlefs", 4, "assets");
  if (!lfs) say(0, "LittleFS (assets) mount failed");

  Job job = lfs ? read_job() : Job();
  String path, sha, tag;
  uint32_t size = 0;
  if (job.present) {
    path = job.path;
    size = job.size;
    sha  = job.sha256;
    tag  = job.tag;
    say(0, (String("Update job: ") + tag).c_str());
    say(1, path.c_str());
  } else {
    String rec = sd_recovery_file();
    if (rec.length()) {
      path = rec;
      tag  = "SD recovery";
      say(0, "SD recovery file found");
      say(1, path.c_str());
    }
  }
  if (path.length() == 0) {
    say(0, "No update pending");
    boot_main();
  }

  const esp_partition_t* main_part =
      esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, NULL);
  if (!main_part)
    halt("Partition table has no main (ota_0) slot.", "", "Flash the -merged.bin by USB.");

  fs::File img = open_image(path);
  const char* why = img ? nullptr : "staged file not found";
  if (!why) {
    say(2, "Verifying image...");
    why = verify_image(img, size, sha, main_part->size);
  }
  if (why) {
    if (img) img.close();
    say(2, why);
    say(3, "Nothing was written.");
    if (job.present && lfs) LittleFS.remove(kJobPath);
    delay(3000);
    boot_main();
  }

  say(2, "Image verified, writing main partition");
  uint32_t img_size = img.size();
  why = write_image(img, img_size);
  img.close();
  if (why) {
    say(2, why);
    halt("Write failed, the main partition is incomplete.",
         "Restart to retry this update.",
         "A power cycle is safe: the job is kept.");
  }

  if (job.present && lfs) LittleFS.remove(kJobPath);
  remove_image(path);
  say(3, "Update complete, restarting");
  delay(2000);
  esp_restart();
}

void loop() {
  delay(1000);
}
