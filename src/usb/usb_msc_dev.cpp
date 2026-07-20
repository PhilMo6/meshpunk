// USB drive mode — expose the SD card to a PC as a Mass Storage device.
//
// DEVICE-mode counterpart of the host stack in usb_core.cpp: the OTG port
// enumerates as a thumb drive whose sectors are the SD card's, via
// SD.readRAW/writeRAW. The PC owns the FAT volume for the whole session; two
// filesystems must never write one FAT volume concurrently, so a session
// holds three locks at once:
//   1. sd_mounted = false — every firmware FS path (Lua _fs_*, io.open,
//      meshpunk_open, the _list_dir_sd helpers) already treats an unmounted
//      card as absent; the Arduino SD object itself stays initialized so the
//      raw-sector callbacks keep working.
//   2. mesh_task_paused = true — punkmesh persists contacts/messages/
//      channels to SD from the mesh task; parking the dispatcher stops
//      those writes at the source (and frees the shared SPI bus).
//      tdeck_link.cpp writes the same flag for GameBoy link sessions; the
//      two writers are mutually exclusive (see meshpunk_sync.h).
//   3. emoji_font_reload(true) — the extended emoji set can hold an SD file
//      open across sessions; close-only release before the PC takes over.
//
// The pins: while a session runs the OTG peripheral owns the USB pads, so
// Serial-JTAG (SLog, link cable, flashing) is dead — identical to host mode.
// HWCDC writes drop non-blocking when the pins are gone, so SLog is safe to
// call throughout.
//
// TinyUSB here has no device-stack uninstall (tud_disconnect only, no
// tud_deinit): USB.begin() runs once per boot and later sessions reconnect
// the resident stack. Consequence: after drive mode has run, the OTG
// controller can't be handed to the host library — usb_manager_start()
// refuses until reboot (usbdrive_used_this_boot).
//
// Watchdog: the Tools/"USB Drive" app pings while it is alive; usbdrive_tick
// (called from loop()) force-stops a session ~3s after pings stop, so no
// teardown path (home chord, app crash) can leave the card latched to a PC
// with the mesh paused.

#include "usb_manager.h"
#include "usb_core_int.h"          // restore_serial_jtag_phy()
#include "meshpunk_sync.h"         // SLog, sd_spi_take/SPI lock, mesh_task_paused
#include "emoji_font.h"            // emoji_font_reload(close_only)

#include <Arduino.h>
#include <SD.h>

#include "sdkconfig.h"
#if CONFIG_TINYUSB_MSC_ENABLED

#include "USB.h"
#include "USBMSC.h"
#include "esp32-hal-tinyusb.h"     // tud_connect/tud_disconnect/tud_mounted
                                   // (public core header; orders tusb.h +
                                   // tusb_option.h + tusb_config.h correctly)
#include <soc/usb_serial_jtag_struct.h>
#include <soc/rtc_cntl_struct.h>

extern bool sd_mounted;            // main.cpp
extern bool meshpunk_sd_mount();   // main.cpp: boot/remount probe
void sd_spi_release();             // main.cpp (take is inline in meshpunk_sync.h)

static USBMSC s_msc;

static volatile bool     s_active     = false;
static bool              s_used_boot  = false;   // OTG device stack resident
static volatile bool     s_ejected    = false;
static volatile uint32_t s_reads      = 0;       // sectors
static volatile uint32_t s_writes     = 0;       // sectors
static volatile uint64_t s_bytes      = 0;
static volatile uint32_t s_last_ping  = 0;
static const char*       s_fail       = "";

#define USBDRIVE_PING_TIMEOUT_MS 3000

// ── MSC callbacks (TinyUSB device task context) ─────────────────────────────
// The core hands sector-aligned bursts (CONFIG_TINYUSB_MSC_BUFSIZE = 4096 →
// 8 sectors); anything unaligned is refused rather than guessed at. SD access
// shares the TFT/radio SPI bus — same take/release pair as every SD user.

static int32_t on_msc_read(uint32_t lba, uint32_t offset, void* buffer, uint32_t bufsize) {
    if (!s_active || offset != 0 || (bufsize % 512) != 0) return -1;
    uint8_t* p = (uint8_t*)buffer;
    uint32_t sectors = bufsize / 512;
    sd_spi_take();
    for (uint32_t i = 0; i < sectors; i++) {
        if (!SD.readRAW(p + i * 512, lba + i)) { sd_spi_release(); return -1; }
    }
    sd_spi_release();
    s_reads += sectors;
    s_bytes += bufsize;
    return (int32_t)bufsize;
}

static int32_t on_msc_write(uint32_t lba, uint32_t offset, uint8_t* buffer, uint32_t bufsize) {
    if (!s_active || offset != 0 || (bufsize % 512) != 0) return -1;
    uint32_t sectors = bufsize / 512;
    sd_spi_take();
    for (uint32_t i = 0; i < sectors; i++) {
        if (!SD.writeRAW(buffer + i * 512, lba + i)) { sd_spi_release(); return -1; }
    }
    sd_spi_release();
    s_writes += sectors;
    s_bytes += bufsize;
    return (int32_t)bufsize;
}

static bool on_msc_start_stop(uint8_t power_condition, bool start, bool load_eject) {
    (void)power_condition;
    if (load_eject && !start) s_ejected = true;   // PC-side "safely remove"
    return true;
}

// ── PHY claim (mirror of restore_serial_jtag_phy in usb_core.cpp) ───────────
// Re-entry path only: USB.begin()'s HAL does this itself on first start. Same
// hand-inlined register pokes, opposite direction — hand the pads to the OTG
// PHY (RTC-domain, survives warm reset).
static void claim_otg_phy() {
    USB_SERIAL_JTAG.conf0.usb_pad_enable = 0;
    RTCCNTL.usb_conf.sw_hw_usb_phy_sel = 1;
    RTCCNTL.usb_conf.sw_usb_phy_sel    = 1;
}

// ── Session lifecycle (Lua-binding context, Core 0) ─────────────────────────

bool usbdrive_start(void) {
    if (s_active) return true;
    if (usb_manager_running()) { s_fail = "USB host mode is running"; return false; }
    if (!sd_mounted)           { s_fail = "no SD card";               return false; }

    uint32_t sectors = 0;
    uint16_t secsize = 0;
    sd_spi_take();
    sectors = (uint32_t)SD.numSectors();
    secsize = (uint16_t)SD.sectorSize();
    sd_spi_release();
    if (!sectors || !secsize) { s_fail = "card geometry read failed"; return false; }

    SLog.printf("[usbdrive] start: %lu sectors x %u (USB serial off until stop)\n",
                (unsigned long)sectors, (unsigned)secsize);

    // Order matters: stop the SD writers before hiding the card, hide the
    // card before the PC can touch it.
    emoji_font_reload(true);                    // close-only: drop the SD blob handle
    mesh_task_paused = true;
    vTaskDelay(pdMS_TO_TICKS(100));             // let an in-flight dispatcher pass finish
    sd_mounted = false;

    s_ejected = false;
    s_reads = s_writes = 0;
    s_bytes = 0;
    s_fail  = "";
    s_last_ping = millis();
    s_active = true;

    if (!s_used_boot) {
        s_msc.vendorID("MeshPnk");
        s_msc.productID("T-Deck SD");
        s_msc.productRevision("1.0");
        s_msc.onRead(on_msc_read);
        s_msc.onWrite(on_msc_write);
        s_msc.onStartStop(on_msc_start_stop);
        s_msc.mediaPresent(true);
        s_msc.begin(sectors, secsize);
        USB.begin();                            // HAL muxes the pads to OTG
        s_used_boot = true;
    } else {
        claim_otg_phy();
        tud_connect();
    }
    return true;
}

void usbdrive_stop(void) {
    if (!s_active) return;
    s_active = false;                           // callbacks refuse from here on

    tud_disconnect();
    restore_serial_jtag_phy();                  // pads back to Serial-JTAG

    // Remount: drop every FAT structure the PC's writes invalidated.
    sd_spi_take();
    SD.end();
    sd_spi_release();
    meshpunk_sd_mount();

    mesh_task_paused = false;
    SLog.printf("[usbdrive] stop: %lu reads / %lu writes, remount %s\n",
                (unsigned long)s_reads, (unsigned long)s_writes,
                sd_mounted ? "ok" : "FAILED");
}

bool usbdrive_active(void)         { return s_active; }
bool usbdrive_used_this_boot(void) { return s_used_boot; }
void usbdrive_ping(void)           { s_last_ping = millis(); }

void usbdrive_tick(void) {
    if (s_active && millis() - s_last_ping > USBDRIVE_PING_TIMEOUT_MS) {
        SLog.println("[usbdrive] watchdog: app stopped pinging - forcing stop");
        usbdrive_stop();
    }
}

void usbdrive_status(UsbDriveStatus* out) {
    out->active       = s_active;
    out->connected    = s_active && tud_mounted();
    out->ejected      = s_ejected;
    out->host_latched = s_used_boot;
    out->reads        = s_reads;
    out->writes       = s_writes;
    out->bytes        = s_bytes;
    out->fail         = s_fail;
}

#else  // !CONFIG_TINYUSB_MSC_ENABLED

bool usbdrive_start(void)          { return false; }
void usbdrive_stop(void)           {}
bool usbdrive_active(void)         { return false; }
bool usbdrive_used_this_boot(void) { return false; }
void usbdrive_ping(void)           {}
void usbdrive_tick(void)           {}
void usbdrive_status(UsbDriveStatus* out) {
    *out = UsbDriveStatus{};
    out->fail = "MSC not built in";
}

#endif  // CONFIG_TINYUSB_MSC_ENABLED
