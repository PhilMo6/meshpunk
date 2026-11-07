// Dynamic USB driver: ESPSERIAL — the USB serial port of an Espressif device
// (match 0A/*/*, idVendor 0x303A). The ESP32-S3 USB-Serial-JTAG presents its
// CDC-data interface as 0A/02, so the manifest wildcards subclass/protocol
// and the probe's vendor check keeps the driver off other CDC gadgets.
//
// Two jobs on the one port:
//
//  PEER LINK (default). This side runs USB host mode and enumerates the other
//  Meshpunk device's serial port. The driver is a thin transport: a bulk-IN
//  resubmit loop feeds received bytes to the firmware's bridge (api->link_rx)
//  and the registered link socket's send() carries the bridge's frames out.
//  All protocol lives in src/tdeck_link.cpp.
//
//  FLASHER. While an app holds a command blob for this driver
//  (_usb_drv_write("espserial", ...)), the link socket is not registered and
//  the port speaks the ESP32-S3 ROM bootloader's serial protocol: SLIP
//  frames, register access, flash erase/write and the ROM's MD5 of flash.
//  No flasher stub is uploaded. The image comes from a file read through
//  api->file_*. Command numbers, argument layouts, register addresses and
//  the reset sequences follow esptool (loader.py, targets/esp32s3.py,
//  reset.py).
//
// The command blob is kept by the core per driver name, so an instance that
// loads after the target re-enumerates still sees it and stays off the link.
// A command ACTS only when its sequence number differs from the one seen at
// start(): a blob left from before a re-attach holds the port and nothing
// more.
//
// Everything here runs in usb_task except link_send (bridge tasks).

#include <string.h>
#include <stdio.h>
#include "usb_shim.h"
#include "../../src/usb/usb_driver_abi.h"

extern "C" const UsbDriverDesc usbdrv_ops;

#define ulog(...) do { if (s_hapi) s_hapi->log(__VA_ARGS__); } while (0)

#define VID_ESPRESSIF 0x303A
#define VID_SILABS    0x10C4   // CP210x USB-UART bridges (Heltec V3 and others)
#define OUT_BUF       4096

// Transports: the chip's own USB port (CDC: line state through the comm
// interface), or a CP210x bridge chip on the board (one vendor interface,
// Silicon Labs AN571 requests). The peer link exists only on the native port.
enum { TR_NATIVE = 0, TR_CP210X = 1 };

// CP210x vendor requests (bmRequestType 0x41, wIndex = interface).
#define CP_IFC_ENABLE    0x00
#define CP_SET_LINE_CTL  0x03
#define CP_SET_MHS       0x07
#define CP_PURGE         0x12
#define CP_SET_BAUDRATE  0x1E

#define BRIDGE_BAUD      921600  // after SYNC on a bridge (ROM starts at 115200)

struct PortProfile {
    bool     valid;
    uint8_t  tr;           // TR_NATIVE / TR_CP210X
    uint8_t  data_if;      // interface with the bulk pipes
    uint8_t  comm_if;      // CDC-comm interface (native line-state target)
    bool     have_comm;
    uint8_t  ep_in, ep_out;
    uint16_t mps_in, mps_out;
};
static PortProfile s_prof;

static const UsbHostApi* s_hapi     = 0;
static UsbPipe*          s_pipe_in  = 0;
static UsbPipe*          s_pipe_out = 0;
static volatile bool     s_on       = false;
static volatile bool     s_linked   = false;   // link socket registered
static volatile bool     s_hold     = false;   // flasher holds the port

static uint32_t now(void) { return s_hapi->ticks_ms(); }

// ── Link socket: bridge -> wire ──────────────────────────────────────────────
// The bridge (src/tdeck_link.cpp) serializes every call to send() under its
// own mutex: there is one pipe buffer and one transfer object here.
// pipe_xfer is dual-context (blocks a foreign task on a waiter; pumps inline
// on usb_task).

static bool link_send(const uint8_t* d, uint32_t n) {
    if (s_hold || !s_on || !s_pipe_out || !s_hapi || n == 0) return false;
    if (n > s_prof.mps_out) return false;
    memcpy(s_hapi->pipe_buf(s_pipe_out), d, n);
    return s_hapi->pipe_xfer(s_pipe_out, n, 250) == USB_XFER_OK;
}

static const UsbLinkOps s_link_ops = { &link_send };

// ── App channel ──────────────────────────────────────────────────────────────

enum { OP_HOLD = 1, OP_BOOT = 2, OP_CHECK = 3, OP_WRITE = 4, OP_RESET = 5, OP_CANCEL = 6,
       OP_READ = 7 };
enum { ST_IDLE = 0, ST_BUSY = 1, ST_OK = 2, ST_FAIL = 3 };
enum { STAGE_NONE = 0, STAGE_CONNECT = 1, STAGE_IMAGE = 2, STAGE_TARGET = 3,
       STAGE_WRITE = 4, STAGE_RESET = 5, STAGE_READ = 6 };
// CHECK kinds: FULL = a whole-flash image written at 0 after the chip is
// erased; UPDATE = an app image written into an app partition; RAW = any
// file at the offset the app gives (bootloader, partition table...).
enum { KIND_FULL = 1, KIND_UPDATE = 2, KIND_RAW = 3 };

// Command blob: [0] op, [1] nonce chosen by the app. CHECK adds [2] kind,
// [3..6] offset (RAW: where to write; UPDATE: 0 = automatic, else the app
// slot chosen from the "slots" answer) and [7..] the NUL-terminated path.
// READ (flash -> SD file, MD5-verified) adds [2..5] offset, [6..9] length
// and [10..] the NUL-terminated path.
// Status blob (64 bytes): [0] state, bit 7 = flasher holds the port; [1] op;
// [2] stage; [3] percent; [4..7] nonce of the command it answers;
// [8..11] instance stamp (ticks at start, differs per driver load);
// [12..63] NUL-terminated text.
static uint8_t  s_status[64];
static uint8_t  s_status_sent[64];
static uint32_t s_stamp;

static uint8_t  s_cmd[256];
static uint32_t s_cmd_len;
static uint32_t s_cmd_seq;        // sequence of the blob last looked at
static uint32_t s_job_seq;        // nonce of the command the status answers
static uint8_t  s_pending;        // op waiting to start, 0 = none
static char     s_path[200];      // CHECK: the image file
static uint8_t  s_want_kind;      // CHECK: the kind the app expects
static uint32_t s_want_off;       // CHECK: offset (RAW), or a chosen app slot (UPDATE); READ: offset
static uint32_t s_want_len;       // READ: length
static uint32_t s_settle;         // out pipe is not used before this time
static bool     s_guard;          // a flash guard was seen since the last tick

static void put32(uint8_t* p, uint32_t v) {
    p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24;
}
static uint32_t get32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void status_push(void) {
    if (memcmp(s_status, s_status_sent, sizeof(s_status)) == 0) return;
    memcpy(s_status_sent, s_status, sizeof(s_status));
    s_hapi->publish(&usbdrv_ops, s_status, sizeof(s_status));
}

static void status_set(uint8_t state, uint8_t op, uint8_t stage, uint8_t pct, const char* text) {
    s_status[0] = state | (s_hold ? 0x80 : 0);
    s_status[1] = op;
    s_status[2] = stage;
    s_status[3] = pct;
    put32(s_status + 4, s_job_seq);
    put32(s_status + 8, s_stamp);
    if (text) snprintf((char*)s_status + 12, sizeof(s_status) - 12, "%s", text);
    status_push();
}

// ── ROM bootloader transport ─────────────────────────────────────────────────

#define ROM_FLASH_BEGIN     0x02
#define ROM_FLASH_DATA      0x03
#define ROM_SYNC            0x08
#define ROM_WRITE_REG       0x09
#define ROM_READ_REG        0x0A
#define ROM_SPI_SET_PARAMS  0x0B
#define ROM_SPI_ATTACH      0x0D
#define ROM_READ_FLASH_SLOW 0x0E
#define ROM_CHANGE_BAUD     0x0F
#define ROM_SPI_FLASH_MD5   0x13

#define ROM_BLOCK   1024          // FLASH_WRITE_SIZE of the ROM loader
#define SEG_BYTES   0x40000       // one FLASH_BEGIN (erase) + MD5 unit
#define RX_MAX      160

static uint8_t  s_rx[RX_MAX];
static uint32_t s_rx_len;
static bool     s_rx_in, s_rx_esc, s_rx_over;

static uint8_t  s_resp[RX_MAX];   // the answer to the request in flight
static uint32_t s_resp_len;
static bool     s_resp_ready;
static bool     s_waiting;
static bool     s_soft;           // a timeout is an answer, not a failure
static bool     s_timed_out;
static uint8_t  s_wait_op;
static uint32_t s_deadline, s_timeout;

static void slip_reset(void) {
    s_rx_len = 0;
    s_rx_in = s_rx_esc = s_rx_over = false;
}

static void slip_frame(void) {
    if (s_rx_over || s_rx_len < 8) return;
    if (!s_waiting || s_resp_ready) return;
    if (s_rx[0] != 0x01 || s_rx[1] != s_wait_op) return;
    memcpy(s_resp, s_rx, s_rx_len);
    s_resp_len   = s_rx_len;
    s_resp_ready = true;
    s_waiting    = false;
}

static void slip_feed(const uint8_t* d, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        uint8_t b = d[i];
        if (b == 0xC0) {
            if (s_rx_in && s_rx_len > 0) { slip_frame(); s_rx_in = false; }
            else                         { s_rx_in = true; }
            s_rx_len = 0; s_rx_esc = false; s_rx_over = false;
            continue;
        }
        if (!s_rx_in) continue;
        if (s_rx_esc) {
            s_rx_esc = false;
            b = (b == 0xDC) ? 0xC0 : (b == 0xDD) ? 0xDB : b;
        } else if (b == 0xDB) {
            s_rx_esc = true;
            continue;
        }
        if (s_rx_len < RX_MAX) s_rx[s_rx_len++] = b;
        else                   s_rx_over = true;
    }
}

static uint32_t slip_put(uint8_t* out, uint32_t n, const uint8_t* d, uint32_t len) {
    for (uint32_t i = 0; i < len; i++) {
        uint8_t b = d[i];
        if (b == 0xC0)      { out[n++] = 0xDB; out[n++] = 0xDC; }
        else if (b == 0xDB) { out[n++] = 0xDB; out[n++] = 0xDD; }
        else                out[n++] = b;
    }
    return n;
}

// One request: header + a (+ b). The answer is awaited by the caller through
// PT_REQ. Largest frame: 2 + 2 * (8 + 16 + ROM_BLOCK) = 2098 bytes < OUT_BUF.
static bool rom_cmd(uint8_t op, const uint8_t* a, uint32_t alen,
                    const uint8_t* b, uint32_t blen, uint32_t chk,
                    uint32_t timeout_ms, bool soft) {
    uint8_t hdr[8];
    hdr[0] = 0x00; hdr[1] = op;
    hdr[2] = (alen + blen) & 0xFF; hdr[3] = (alen + blen) >> 8;
    put32(hdr + 4, chk);

    uint8_t* out = s_hapi->pipe_buf(s_pipe_out);
    uint32_t n = 0;
    out[n++] = 0xC0;
    n = slip_put(out, n, hdr, 8);
    if (alen) n = slip_put(out, n, a, alen);
    if (blen) n = slip_put(out, n, b, blen);
    out[n++] = 0xC0;

    // Armed before the transfer: the answer can arrive while pipe_xfer pumps.
    s_wait_op    = op;
    s_resp_ready = false;
    s_timed_out  = false;
    s_soft       = soft;
    s_timeout    = timeout_ms;
    s_deadline   = now() + timeout_ms;
    s_waiting    = true;
    if (s_hapi->pipe_xfer(s_pipe_out, n, 2000) != USB_XFER_OK) {
        s_waiting = false;
        return false;
    }
    return true;
}

static bool rom_read_reg(uint32_t addr, bool soft = false) {
    uint8_t a[4];
    put32(a, addr);
    return rom_cmd(ROM_READ_REG, a, 4, 0, 0, 0, soft ? 500 : 3000, soft);
}

static bool rom_write_reg(uint32_t addr, uint32_t value) {
    uint8_t a[16];
    put32(a, addr); put32(a + 4, value); put32(a + 8, 0xFFFFFFFF); put32(a + 12, 0);
    return rom_cmd(ROM_WRITE_REG, a, 16, 0, 0, 0, 3000, false);
}

// The ROM ends every answer's data with 4 status bytes; the first is nonzero
// on failure.
static uint32_t resp_dlen(void)        { return s_resp_len - 8; }
static const uint8_t* resp_data(void)  { return s_resp + 8; }
static uint32_t resp_val(void)         { return get32(s_resp + 4); }
static bool resp_ok(void) {
    return resp_dlen() >= 4 && resp_data()[resp_dlen() - 4] == 0;
}

// ── MD5 (RFC 1321) ───────────────────────────────────────────────────────────

struct Md5 { uint32_t a, b, c, d; uint32_t lo, hi; uint8_t buf[64]; };
static Md5 s_md5;

static const uint32_t kMd5K[64] = {
    0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
    0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
    0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
    0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
    0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
    0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
    0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
    0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391,
};
static const uint8_t kMd5S[16] = { 7, 12, 17, 22, 5, 9, 14, 20, 4, 11, 16, 23, 6, 10, 15, 21 };

static void md5_block(Md5* m, const uint8_t* p) {
    uint32_t w[16];
    for (int i = 0; i < 16; i++) w[i] = get32(p + i * 4);
    uint32_t a = m->a, b = m->b, c = m->c, d = m->d;
    for (int i = 0; i < 64; i++) {
        uint32_t f;
        int g;
        if (i < 16)      { f = (b & c) | (~b & d); g = i; }
        else if (i < 32) { f = (d & b) | (~d & c); g = (5 * i + 1) & 15; }
        else if (i < 48) { f = b ^ c ^ d;          g = (3 * i + 5) & 15; }
        else             { f = c ^ (b | ~d);       g = (7 * i) & 15; }
        uint32_t x = a + f + kMd5K[i] + w[g];
        int s = kMd5S[(i >> 4) * 4 + (i & 3)];
        a = d; d = c; c = b;
        b = b + ((x << s) | (x >> (32 - s)));
    }
    m->a += a; m->b += b; m->c += c; m->d += d;
}

static void md5_init(Md5* m) {
    m->a = 0x67452301; m->b = 0xefcdab89; m->c = 0x98badcfe; m->d = 0x10325476;
    m->lo = m->hi = 0;
}

static void md5_update(Md5* m, const uint8_t* p, uint32_t n) {
    uint32_t have = m->lo & 63;
    m->lo += n;
    if (m->lo < n) m->hi++;
    if (have) {
        uint32_t take = 64 - have;
        if (take > n) take = n;
        memcpy(m->buf + have, p, take);
        p += take; n -= take;
        if (have + take < 64) return;
        md5_block(m, m->buf);
    }
    while (n >= 64) { md5_block(m, p); p += 64; n -= 64; }
    if (n) memcpy(m->buf, p, n);
}

// Writes the digest as 32 lowercase hex characters (the form the ROM answers
// SPI_FLASH_MD5 with).
static void md5_hex(Md5* m, char out[32]) {
    uint8_t len[8];
    put32(len, m->lo << 3);
    put32(len + 4, (m->hi << 3) | (m->lo >> 29));
    static const uint8_t pad[64] = { 0x80 };
    uint32_t have = m->lo & 63;
    md5_update(m, pad, have < 56 ? 56 - have : 120 - have);
    md5_update(m, len, 8);
    uint8_t dig[16];
    put32(dig, m->a); put32(dig + 4, m->b); put32(dig + 8, m->c); put32(dig + 12, m->d);
    static const char kHex[] = "0123456789abcdef";
    for (int i = 0; i < 16; i++) {
        out[i * 2]     = kHex[dig[i] >> 4];
        out[i * 2 + 1] = kHex[dig[i] & 15];
    }
}

// ── Chips ────────────────────────────────────────────────────────────────────
// Per-chip ROM facts, from esptool's targets/esp32*.py (4.5.1 for the magic
// values and console registers, 5.4.0 for the watchdog registers). A chip is
// recognised by the value at CHIP_DETECT_MAGIC_REG (0x40001000).

struct Chip {
    const char* name;
    uint32_t image_id;            // chip id in the ESP image header
    uint32_t magic[2];
    bool     begin_enc_arg;       // FLASH_BEGIN takes the 5th "encrypted" word
    uint32_t sec_reg, sec_mask;   // secure boot efuse
    uint32_t crypt_reg;           // flash-encryption counter efuse
    uint8_t  crypt_shift, crypt_bits;
    uint32_t uart_reg;            // ROM's console selector; 0 = no USB console
    uint8_t  uart_jtag;           // its value for USB-Serial-JTAG (watchdogs need taming)
    uint32_t wdt_prot, wdt_cfg0, wdt_key;
    uint32_t swd_prot, swd_conf, swd_key, swd_feed;   // swd_conf 0 = no SWD
    uint32_t spi_base;
    uint8_t  spi_usr, spi_usr2, spi_miso_dlen, spi_w0;
    uint32_t strap_reg, option1_reg;                   // 0 = no manual-mode check
};

static const Chip kChips[] = {
    { "ESP32-S3", 9, { 0x9, 0 }, true,
      0x60007038, 1u << 20, 0x60007034, 18, 3,
      0x3FCEF14C, 4,
      0x600080B0, 0x60008090, 0x50D83AA1,
      0x600080B8, 0x600080B4, 0x8F1D312A, 1u << 31,
      0x60002000, 0x18, 0x20, 0x28, 0x58,
      0x60004038, 0x6000812C },
    { "ESP32-C3", 5, { 0x6921506F, 0x1B31506F }, true,
      0x60008838, 1u << 20, 0x60008834, 18, 3,
      0x3FCDF07C, 3,
      0x600080A8, 0x60008090, 0x50D83AA1,
      0x600080B0, 0x600080AC, 0x8F1D312A, 1u << 31,
      0x60002000, 0x18, 0x20, 0x28, 0x58,
      0, 0 },
    { "ESP32-C6", 13, { 0x2CE0806F, 0 }, true,
      0x600B0838, 1u << 20, 0x600B0834, 18, 3,
      0x4087F580, 3,
      0x600B1C18, 0x600B1C00, 0x50D83AA1,
      0x600B1C20, 0x600B1C1C, 0x50D83AA1, 1u << 18,
      0x60003000, 0x18, 0x20, 0x28, 0x58,
      0, 0 },
    { "ESP32-S2", 2, { 0x000007C6, 0 }, true,
      0x3F41A038, 1u << 20, 0x3F41A034, 18, 3,
      0, 0,                         // USB-OTG console: esptool tames no watchdog there
      0, 0, 0,
      0, 0, 0, 0,
      0x3F402000, 0x18, 0x20, 0x28, 0x58,
      0x3F404038, 0x3F408128 },
    { "ESP32", 0, { 0x00F01D83, 0 }, false,
      0x3FF5A018, (1u << 4) | (1u << 5), 0x3FF5A000, 20, 7,
      0, 0,                         // UART only
      0, 0, 0,
      0, 0, 0, 0,
      0x3FF42000, 0x1C, 0x24, 0x2C, 0x80,
      0, 0 },
};
static const Chip* s_chip;

static const Chip* chip_by_magic(uint32_t magic) {
    for (unsigned i = 0; i < sizeof(kChips) / sizeof(kChips[0]); i++)
        if (kChips[i].magic[0] == magic || (kChips[i].magic[1] && kChips[i].magic[1] == magic))
            return &kChips[i];
    return 0;
}

// FLASH_BEGIN arguments; the ESP32 ROM takes four words, later ROMs five.
static bool rom_flash_begin(uint32_t erase, uint32_t nblocks, uint32_t off) {
    uint8_t a[20];
    put32(a, erase); put32(a + 4, nblocks); put32(a + 8, ROM_BLOCK); put32(a + 12, off);
    put32(a + 16, 0);
    return rom_cmd(ROM_FLASH_BEGIN, a, s_chip->begin_enc_arg ? 20 : 16, 0, 0, 0, 20000, false);
}

// ── Jobs (protothreads: one step per tick, resumed at the saved line) ────────
// No `switch` and no initialized locals inside a job body.

#define PT_BEGIN()  switch (s_pc) { case 0:
#define PT_END()    }
#define PT_YIELD()  do { s_pc = __LINE__; return; case __LINE__:; } while (0)
#define PT_WAIT_MS(ms) \
    do { s_wake = now() + (ms); s_pc = __LINE__; return; \
         case __LINE__: if ((int32_t)(now() - s_wake) < 0) return; } while (0)
// Send a request, then wait for its answer. Does not return between the two
// when the answer arrived during the send.
#define PT_REQ(call) \
    do { if (!(call)) { job_end(false, "USB send failed"); return; } \
         s_pc = __LINE__; \
         case __LINE__: if (!s_resp_ready) return; } while (0)
#define FAIL(...) \
    do { snprintf(s_text, sizeof(s_text), __VA_ARGS__); job_end(false, s_text); return; } while (0)
// Register access with the status checked. Statement level only.
#define PT_READ(addr) \
    PT_REQ(rom_read_reg(addr)); if (!resp_ok()) FAIL("target refused a register read")
#define PT_WRITE(addr, value) \
    PT_REQ(rom_write_reg(addr, value)); if (!resp_ok()) FAIL("target refused a register write")

// Longest stretch of file-only work in one tick. The pump loop sleeps up to
// 50 ms between ticks when no USB event is pending.
#define SLICE_MS 100

static uint8_t  s_job;            // running op, 0 = none
static uint8_t  s_stage;
static int      s_pc;
static uint32_t s_wake;
static uint32_t s_tick_t0;
static char     s_text[52];

static uint8_t  s_blk[64 + ROM_BLOCK];   // [0..63] = carry for the tag scan

// CHECK results, consumed by WRITE.
static bool     s_checked;
static bool     s_synced;
static uint8_t  s_kind;
static uint32_t s_file_size;
static uint32_t s_write_off;
static uint32_t s_flash_bytes;
static char     s_slug[32];
static bool     s_is_meshpunk, s_have_e9, s_have_table;
static uint32_t s_chip_id;        // the target's image chip id (ESP32-S3 = 9)
static const char* s_chip_name;
static uint32_t s_app_n, s_app_off[4], s_app_size[4];
static char     s_md5_ff[32];     // MD5 of SEG_BYTES of 0xFF (erased flash)

// Scratch that must survive a yield.
static uint32_t s_i, s_n, s_pos, s_reg0, s_reg1;
static uint32_t s_seg, s_nseg, s_seg_len, s_seg_blocks, s_total;
static int32_t  s_last_data;      // last block of the segment that is not all 0xFF
static char     s_md5hex[32];
static uint32_t s_ota_n, s_ota_off, s_ota_size;
static bool     s_have_factory, s_have_assets, s_tbl_end;

static void job_end(bool ok, const char* text) {
    uint8_t op = s_job;
    s_job = 0;
    s_pc  = 0;
    s_waiting = false;
    if (op == OP_WRITE || op == OP_READ || !ok) s_hapi->file_close(&usbdrv_ops);
    if (op == OP_WRITE || op == OP_READ) s_checked = false;   // READ reused the one file handle
    status_set(ok ? ST_OK : ST_FAIL, op, s_stage, ok ? 100 : s_status[3], text);
    ulog("espserial: %s %s", ok ? "ok" : "FAILED", text);
}

static void progress(uint8_t stage, uint8_t pct, const char* text) {
    s_stage = stage;
    status_set(ST_BUSY, s_job, stage, pct, text);
}

// DTR = bit 0, RTS = bit 1 (1 = asserted). Native: CDC SET_CONTROL_LINE_STATE;
// CP210x: SET_MHS with both mask bits set so each call writes both lines.
static void line_state(uint16_t v) {
    if (s_prof.tr == TR_CP210X)
        s_hapi->control(0x41, CP_SET_MHS, 0x0300 | (v & 3), s_prof.data_if, 0, 0);
    else if (s_prof.have_comm)
        s_hapi->control(0x21, 0x22, v, s_prof.comm_if, 0, 0);
}

static bool cp210x_set_baud(uint32_t baud) {
    uint8_t b[4];
    put32(b, baud);
    return s_hapi->control(0x41, CP_SET_BAUDRATE, 0, s_prof.data_if, b, 4);
}

// Into the bootloader. Native port: esptool's USBJTAGSerialReset. Bridge:
// esptool's ClassicReset through the usual two-transistor circuit (RTS ->
// EN, DTR -> IO0).
static void job_boot(void) {
    PT_BEGIN();
    progress(STAGE_CONNECT, 0, "resetting target");
    if (s_prof.tr == TR_NATIVE && !s_prof.have_comm) FAIL("no control interface");
    if (s_prof.tr == TR_CP210X) {
        line_state(0x2);                          // EN low, IO0 high
        PT_WAIT_MS(100);
        line_state(0x1);                          // IO0 low, EN high: boots into download
        PT_WAIT_MS(50);
        line_state(0x0);
        if (!cp210x_set_baud(115200)) FAIL("bridge refused the baud rate");
        s_hapi->control(0x41, CP_PURGE, 0x000F, s_prof.data_if, 0, 0);
    } else {
        line_state(0x0);
        PT_WAIT_MS(100);
        line_state(0x1);
        PT_WAIT_MS(100);
        line_state(0x3);
        line_state(0x2);
        PT_WAIT_MS(100);
        line_state(0x0);
    }
    slip_reset();
    s_synced  = false;
    s_checked = false;
    job_end(true, "");
    PT_END();
}

// esptool's HardReset (RTS pulse). The strap check is ESP32S3ROM's
// _check_if_can_reset: download mode entered through GPIO0 and not forced
// over USB.
static void job_reset(void) {
    PT_BEGIN();
    progress(STAGE_RESET, 0, "restarting target");
    s_reg0 = 0x8; s_reg1 = 0;
    if (s_synced && s_chip && s_chip->strap_reg) {
        PT_REQ(rom_read_reg(s_chip->strap_reg, true));     // GPIO_STRAP_REG
        if (!s_timed_out && resp_ok()) s_reg0 = resp_val();
        PT_REQ(rom_read_reg(s_chip->option1_reg, true));   // RTC_CNTL_OPTION1_REG
        if (!s_timed_out && resp_ok()) s_reg1 = resp_val();
    }
    line_state(0x2);
    PT_WAIT_MS(100);
    line_state(0x0);
    s_synced  = false;
    s_checked = false;
    job_end(true, ((s_reg0 & 0x8) == 0 && (s_reg1 & 0x1) == 0) ? "manual" : "");
    PT_END();
}

static const char* kind_name(uint8_t k) {
    return k == KIND_FULL ? "full" : k == KIND_RAW ? "raw" : "update";
}

// Finds "<prefix><slug>\0" in p[0..len) and stores the slug. The prefix is
// assembled at run time (src/ota_tag.h: no program contains a tag prefix it
// does not own).
static bool tag_search(const uint8_t* p, uint32_t len) {
    char prefix[20];
    snprintf(prefix, sizeof(prefix), "%s%s", "MESHPUNK-", "BOARD:");
    uint32_t pl = strlen(prefix);
    for (uint32_t i = 0; i + pl < len; i++) {
        if (p[i] != (uint8_t)prefix[0] || memcmp(p + i, prefix, pl) != 0) continue;
        uint32_t j = i + pl, k = 0;
        while (j < len && k < sizeof(s_slug) - 1 && p[j]) s_slug[k++] = (char)p[j++];
        if (j < len && p[j] == 0 && k > 0) { s_slug[k] = 0; return true; }
    }
    s_slug[0] = 0;
    return false;
}

static uint8_t pct_of(uint32_t done, uint32_t total) {
    return (uint8_t)(done / (total / 100 + 1));
}

static void job_check(void) {
    int32_t got;
    uint32_t keep, v;
    const char* path;
    const char* base;
    const uint8_t* e;

    PT_BEGIN();
    s_checked = false;
    progress(STAGE_CONNECT, 0, "connecting");

    // SYNC: the ROM answers one request with several frames; the extras are
    // discarded by the wait that follows.
    for (s_i = 0; s_i < 8; s_i++) {
        {
            static const uint8_t kSync[36] = {
                0x07, 0x07, 0x12, 0x20,
                0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55,
                0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55,
            };
            slip_reset();
            PT_REQ(rom_cmd(ROM_SYNC, kSync, sizeof(kSync), 0, 0, 0, 150, true));
        }
        if (!s_timed_out) break;
    }
    if (s_timed_out) FAIL("target is not in the bootloader");
    PT_WAIT_MS(200);
    s_synced = true;

    // A bridge runs the ROM's default 115200 baud; ask the ROM for more, then
    // follow it (esptool's change_baud: the answer comes at the old rate).
    if (s_prof.tr == TR_CP210X) {
        {
            uint8_t a[8];
            put32(a, BRIDGE_BAUD); put32(a + 4, 0);
            PT_REQ(rom_cmd(ROM_CHANGE_BAUD, a, 8, 0, 0, 0, 3000, false));
        }
        if (!resp_ok()) FAIL("target refused the baud change");
        if (!cp210x_set_baud(BRIDGE_BAUD)) FAIL("bridge refused the baud rate");
        PT_WAIT_MS(50);
        s_hapi->control(0x41, CP_PURGE, 0x000F, s_prof.data_if, 0, 0);
        slip_reset();
    }

    PT_READ(0x40001000);                          // CHIP_DETECT_MAGIC_REG_ADDR
    s_chip = chip_by_magic(resp_val());
    if (!s_chip) FAIL("unknown chip (magic %08X)", (unsigned)resp_val());
    s_chip_id   = s_chip->image_id;
    s_chip_name = s_chip->name;

    PT_READ(s_chip->sec_reg);
    if (resp_val() & s_chip->sec_mask) FAIL("target has secure boot enabled");
    PT_READ(s_chip->crypt_reg);
    // Flash encryption is on when the counter has an odd number of set bits.
    s_n = (resp_val() >> s_chip->crypt_shift) & ((1u << s_chip->crypt_bits) - 1);
    for (keep = 0; s_n; s_n >>= 1) keep += s_n & 1;
    if (keep & 1) FAIL("target has flash encryption enabled");

    // Watchdogs (esptool's disable_watchdogs, USB-Serial-JTAG only): nothing
    // feeds them while the ROM erases.
    if (s_chip->uart_reg) {
        PT_READ(s_chip->uart_reg);
        if ((resp_val() & 0xFF) == s_chip->uart_jtag) {
            PT_WRITE(s_chip->wdt_prot, s_chip->wdt_key);
            PT_WRITE(s_chip->wdt_cfg0, 0);
            PT_WRITE(s_chip->wdt_prot, 0);
            if (s_chip->swd_conf) {
                PT_WRITE(s_chip->swd_prot, s_chip->swd_key);
                PT_READ(s_chip->swd_conf);
                s_reg0 = resp_val();
                PT_WRITE(s_chip->swd_conf, s_reg0 | s_chip->swd_feed);
                PT_WRITE(s_chip->swd_prot, 0);
            }
        }
    }

    {
        static const uint8_t kZero8[8] = { 0 };
        PT_REQ(rom_cmd(ROM_SPI_ATTACH, kZero8, 8, 0, 0, 0, 3000, false));
    }
    if (!resp_ok()) FAIL("SPI attach refused");

    // Flash ID: SPI command 0x9F through the SPI1 registers
    // (ESPLoader.run_spiflash_command).
    PT_READ(s_chip->spi_base + s_chip->spi_usr);          // SPI_USR
    s_reg0 = resp_val();
    PT_READ(s_chip->spi_base + s_chip->spi_usr2);         // SPI_USR2
    s_reg1 = resp_val();
    PT_WRITE(s_chip->spi_base + s_chip->spi_miso_dlen, 23);        // 24 bits in
    PT_WRITE(s_chip->spi_base + s_chip->spi_usr, 0x90000000);      // USR_COMMAND | USR_MISO
    PT_WRITE(s_chip->spi_base + s_chip->spi_usr2, 0x7000009F);     // 8-bit command 0x9F
    PT_WRITE(s_chip->spi_base + s_chip->spi_w0, 0);
    PT_WRITE(s_chip->spi_base, 1u << 18);                 // SPI_CMD_USR
    for (s_i = 0; s_i < 10; s_i++) {
        PT_READ(s_chip->spi_base);
        if ((resp_val() & (1u << 18)) == 0) break;
    }
    if (s_i == 10) FAIL("flash ID read timed out");
    PT_READ(s_chip->spi_base + s_chip->spi_w0);
    s_n = resp_val();
    PT_WRITE(s_chip->spi_base + s_chip->spi_usr, s_reg0);
    PT_WRITE(s_chip->spi_base + s_chip->spi_usr2, s_reg1);
    s_i = (s_n >> 16) & 0xFF;
    if (s_i < 0x12 || s_i > 0x1F) FAIL("unknown flash size (id %06X)", (unsigned)(s_n & 0xFFFFFF));
    s_flash_bytes = 1u << s_i;

    {
        uint8_t a[24];
        put32(a, 0); put32(a + 4, s_flash_bytes); put32(a + 8, 0x10000);
        put32(a + 12, 0x1000); put32(a + 16, 0x100); put32(a + 20, 0xFFFF);
        PT_REQ(rom_cmd(ROM_SPI_SET_PARAMS, a, 24, 0, 0, 0, 3000, false));
    }
    if (!resp_ok()) FAIL("SPI parameters refused");

    // ── READ: a flash range into a file on the SD card, 64 bytes a request
    // (the ROM's READ_FLASH_SLOW), then the ROM's MD5 of the range against
    // the MD5 of what was received.
    if (s_job == OP_READ) {
        progress(STAGE_READ, 0, "reading");
        if (s_want_len == 0 || s_want_off + s_want_len < s_want_off || s_want_off + s_want_len > s_flash_bytes)
            FAIL("range runs past the target's flash");
        if (!s_hapi->file_create(&usbdrv_ops, s_path)) FAIL("cannot create the file (SD card only)");
        md5_init(&s_md5);
        for (s_pos = 0; s_pos < s_want_len; s_pos += 64) {
            {
                uint8_t a[8];
                v = s_want_len - s_pos;
                if (v > 64) v = 64;
                put32(a, s_want_off + s_pos); put32(a + 4, v);
                PT_REQ(rom_cmd(ROM_READ_FLASH_SLOW, a, 8, 0, 0, 0, 3000, false));
            }
            v = s_want_len - s_pos;                   // locals do not survive the wait
            if (v > 64) v = 64;
            if (!resp_ok() || resp_dlen() < 68) FAIL("cannot read the target's flash");
            md5_update(&s_md5, resp_data(), v);
            if (s_hapi->file_write(&usbdrv_ops, resp_data(), v) != (int32_t)v) FAIL("SD card write failed");
            if ((s_pos & 0x3FF) == 0) progress(STAGE_READ, pct_of(s_pos, s_want_len), "reading");
        }
        md5_hex(&s_md5, s_md5hex);
        {
            uint8_t a[16];
            put32(a, s_want_off); put32(a + 4, s_want_len); put32(a + 8, 0); put32(a + 12, 0);
            PT_REQ(rom_cmd(ROM_SPI_FLASH_MD5, a, 16, 0, 0, 0, 15000, false));
        }
        if (!resp_ok() || resp_dlen() != 36) FAIL("verify refused");
        if (memcmp(resp_data(), s_md5hex, 32) != 0) FAIL("read check failed - try again");
        // Parsed by the app: offset, length, chip, flash size.
        snprintf(s_text, sizeof(s_text), "read %X %u %s %X", (unsigned)s_want_off,
                 (unsigned)s_want_len, s_chip_name, (unsigned)s_flash_bytes);
        job_end(true, s_text);
        return;
    }

    // ── The image file ──
    progress(STAGE_IMAGE, 0, "checking image");
    if (!s_hapi->file_open(&usbdrv_ops, s_path)) FAIL("cannot open the file");
    s_file_size = s_hapi->file_size(&usbdrv_ops);
    if (s_file_size < 16) FAIL("file is too small");
    if (s_file_size > s_flash_bytes) FAIL("image is larger than the target's flash");
    s_kind = s_want_kind;
    s_slug[0] = 0;

    // The ESP image header: 0xE9 magic, chip id at bytes 12-13 (0xFFFF in
    // images built for any chip). A full image starts with the bootloader,
    // itself an ESP image, and carries the partition table at 0x8000.
    got = s_hapi->file_read(&usbdrv_ops, s_blk, 16);
    if (got != 16) FAIL("file read error");
    s_have_e9 = s_blk[0] == 0xE9;
    v = (uint32_t)s_blk[12] | ((uint32_t)s_blk[13] << 8);
    s_have_table = false;
    if (s_file_size > 0x8002) {
        if (!s_hapi->file_seek(&usbdrv_ops, 0x8000)) FAIL("file read error");
        got = s_hapi->file_read(&usbdrv_ops, s_blk, 2);
        if (got != 2) FAIL("file read error");
        s_have_table = s_blk[0] == 0xAA && s_blk[1] == 0x50;
    }

    // The content decides between FULL and UPDATE, whatever the name made
    // the app ask for: a file that carries a bootloader and a partition
    // table is a whole-flash image (a Meshtastic .factory.bin, a merged
    // image under any name) and is never written into an app slot.
    if (s_kind == KIND_UPDATE && s_have_e9 && s_have_table) s_kind = KIND_FULL;

    if (s_kind == KIND_RAW) {
        if (s_want_off & 0xFFF) FAIL("offset must be a multiple of 4096");
        if (s_want_off + s_file_size > s_flash_bytes || s_want_off + s_file_size < s_want_off)
            FAIL("offset + size runs past the target's flash");
        s_write_off = s_want_off;
    } else {
        if (!s_have_e9) FAIL("not an ESP firmware image");
        if (s_kind == KIND_FULL && !s_have_table) FAIL("not a full image: no partition table at 0x8000");
        if (v != 0xFFFF && v != s_chip_id)
            FAIL("image is for chip id %u, the target is %s", (unsigned)v, s_chip_name);
    }

    // Meshpunk images (named meshpunk-<board>-...) carry a board tag that
    // must match the board in the file name. Other firmware has no such tag
    // and gets no board check: the user picked the device, as on every
    // other flasher.
    base = s_path;
    for (path = s_path; *path; path++) if (*path == '/') base = path + 1;
    s_is_meshpunk = strncmp(base, "meshpunk-", 9) == 0 && s_kind != KIND_RAW;
    if (s_is_meshpunk) {
        s_pos = (s_kind == KIND_FULL) ? 0x10000 : 0;   // the app starts at 0x10000 in a full image
        if (!s_hapi->file_seek(&usbdrv_ops, s_pos)) FAIL("file read error");
        s_n = 0;                                      // carry length
        while (s_pos < s_file_size) {
            got = s_hapi->file_read(&usbdrv_ops, s_blk + 64, ROM_BLOCK);
            if (got <= 0) FAIL("file read error");
            s_pos += got;
            // The last 63 bytes of each chunk are searched again with the
            // next: a tag is at most 15 + 31 + 1 bytes.
            if (tag_search(s_blk + 64 - s_n, s_n + got)) break;
            keep = got < 63 ? (uint32_t)got : 63;
            memmove(s_blk + 64 - keep, s_blk + 64 + got - keep, keep);
            s_n = keep;
            if (now() - s_tick_t0 > SLICE_MS) {
                progress(STAGE_IMAGE, pct_of(s_pos, s_file_size), "checking image");
                PT_YIELD();
            }
        }
        if (!s_slug[0]) FAIL("no Meshpunk board tag in the image");
        keep = strlen(s_slug);
        if (strncmp(base + 9, s_slug, keep) != 0 || base[9 + keep] != '-')
            FAIL("image is for %s, not what the file name says", s_slug);
    }

    // ── Where an update goes: the target's own partition table ──
    if (s_kind == KIND_FULL) s_write_off = 0;
    if (s_kind == KIND_UPDATE) {
        progress(STAGE_TARGET, 0, "reading target");
        s_app_n = 0;
        s_ota_n = 0;
        s_have_factory = s_have_assets = s_tbl_end = false;
        for (s_i = 0; s_i < 48 && !s_tbl_end; s_i++) {
            {
                uint8_t a[8];
                put32(a, 0x8000 + s_i * 64); put32(a + 4, 64);
                PT_REQ(rom_cmd(ROM_READ_FLASH_SLOW, a, 8, 0, 0, 0, 3000, false));
            }
            if (!resp_ok() || resp_dlen() < 68) FAIL("cannot read the target's flash");
            for (s_n = 0; s_n < 2; s_n++) {
                e = resp_data() + s_n * 32;
                if (e[0] != 0xAA || e[1] != 0x50) { s_tbl_end = true; break; }
                if (e[2] == 0 && s_app_n < 4) {           // an app partition
                    s_app_off[s_app_n]  = get32(e + 4);
                    s_app_size[s_app_n] = get32(e + 8);
                    s_app_n++;
                }
                if (e[2] == 0 && e[3] == 0x10) {
                    s_ota_n++;
                    s_ota_off  = get32(e + 4);
                    s_ota_size = get32(e + 8);
                }
                if (e[2] == 0 && e[3] == 0x00) s_have_factory = true;
                if (e[2] == 1 && e[3] == 0x82 && memcmp(e + 12, "assets", 7) == 0)
                    s_have_assets = true;
            }
        }
        if (s_app_n == 0) FAIL("target has no partition table - use a full image");

        if (s_is_meshpunk) {
            // Meshpunk layout: main = the one ota_0, beside the updater
            // (factory) and the assets partition.
            if (s_ota_n != 1 || !s_have_factory || !s_have_assets)
                FAIL("target has no Meshpunk install - use a full image");
            s_write_off = s_ota_off;
            v = s_ota_size;
        } else {
            // 0x10000 (every standard layout), or the slot the app asked for
            // after being shown the choices.
            s_write_off = s_want_off ? s_want_off : 0x10000;
            v = 0;
            for (s_n = 0; s_n < s_app_n; s_n++)
                if (s_app_off[s_n] == s_write_off) v = s_app_size[s_n];
            if (v == 0) {
                // No app slot there: list what the target has, for the app
                // to offer. The text is parsed: "slots" then hex offsets.
                keep = snprintf(s_text, sizeof(s_text), "slots");
                for (s_n = 0; s_n < s_app_n && keep < sizeof(s_text) - 8; s_n++)
                    keep += snprintf(s_text + keep, sizeof(s_text) - keep, " %X", (unsigned)s_app_off[s_n]);
                job_end(false, s_text);
                return;
            }
        }
        if (s_file_size > v) FAIL("image is larger than the target's app slot");
        if (s_write_off + v > s_flash_bytes) FAIL("target partition table is not valid");
    }

    // Parsed by the app: kind, offset (hex), size, chip, board slug (Meshpunk
    // images only).
    s_checked = true;
    snprintf(s_text, sizeof(s_text), "%s %X %u %s %s", kind_name(s_kind),
             (unsigned)s_write_off, (unsigned)s_file_size, s_chip_name, s_slug);
    job_end(true, s_text);
    PT_END();
}

// Per SEG_BYTES segment: read it once for the MD5 and to find where its data
// ends, FLASH_BEGIN (the ROM erases the whole segment there), send the blocks
// up to the last one that is not all 0xFF, then compare the ROM's MD5 of the
// flash with the file's. A segment that is all 0xFF is erased and verified
// with no data sent.
static void job_write(void) {
    int32_t  got;
    uint32_t want, chk, k, addr;
    uint8_t* blk;

    PT_BEGIN();
    if (!s_checked) FAIL("no checked image");
    // A full image replaces the whole chip: everything past the image is
    // erased too (below), so the progress runs over the flash size.
    s_total = (s_kind == KIND_FULL) ? s_flash_bytes : s_file_size;
    s_nseg  = (s_file_size + SEG_BYTES - 1) / SEG_BYTES;
    for (s_seg = 0; s_seg < s_nseg; s_seg++) {
        s_pos        = s_seg * SEG_BYTES;
        s_seg_len    = s_file_size - s_pos < SEG_BYTES ? s_file_size - s_pos : SEG_BYTES;
        s_seg_blocks = (s_seg_len + ROM_BLOCK - 1) / ROM_BLOCK;
        progress(STAGE_WRITE, pct_of(s_pos, s_total), "writing");

        if (!s_hapi->file_seek(&usbdrv_ops, s_pos)) FAIL("file read error");
        md5_init(&s_md5);
        s_last_data = -1;
        for (s_i = 0; s_i < s_seg_blocks; s_i++) {
            blk  = s_blk + 64;
            want = s_seg_len - s_i * ROM_BLOCK;
            if (want > ROM_BLOCK) want = ROM_BLOCK;
            got = s_hapi->file_read(&usbdrv_ops, blk, want);
            if (got != (int32_t)want) FAIL("file read error");
            md5_update(&s_md5, blk, want);
            for (k = 0; k < want; k++)
                if (blk[k] != 0xFF) { s_last_data = (int32_t)s_i; break; }
            if (now() - s_tick_t0 > SLICE_MS) PT_YIELD();
        }
        md5_hex(&s_md5, s_md5hex);

        PT_REQ(rom_flash_begin(s_seg_blocks * ROM_BLOCK, s_seg_blocks, s_write_off + s_pos));
        addr = s_write_off + s_pos;
        if (!resp_ok()) FAIL("erase failed at 0x%X", (unsigned)addr);

        if (s_last_data >= 0) {
            if (!s_hapi->file_seek(&usbdrv_ops, s_pos)) FAIL("file read error");
            for (s_i = 0; (int32_t)s_i <= s_last_data; s_i++) {
                blk = s_blk + 64;
                got = s_hapi->file_read(&usbdrv_ops, blk, ROM_BLOCK);
                if (got <= 0) FAIL("file read error");
                if (got < ROM_BLOCK) memset(blk + got, 0xFF, ROM_BLOCK - got);
                chk = 0xEF;
                for (k = 0; k < ROM_BLOCK; k++) chk ^= blk[k];
                {
                    uint8_t a[16];
                    put32(a, ROM_BLOCK); put32(a + 4, s_i); put32(a + 8, 0); put32(a + 12, 0);
                    PT_REQ(rom_cmd(ROM_FLASH_DATA, a, 16, blk, ROM_BLOCK, chk, 5000, false));
                }
                addr = s_write_off + s_pos + s_i * ROM_BLOCK;
                if (!resp_ok()) FAIL("write failed at 0x%X", (unsigned)addr);
                if ((s_i & 15) == 15)
                    progress(STAGE_WRITE, pct_of(s_pos + s_i * ROM_BLOCK, s_total), "writing");
            }
        }

        {
            uint8_t a[16];
            put32(a, s_write_off + s_pos); put32(a + 4, s_seg_len); put32(a + 8, 0); put32(a + 12, 0);
            PT_REQ(rom_cmd(ROM_SPI_FLASH_MD5, a, 16, 0, 0, 0, 15000, false));
        }
        addr = s_write_off + s_pos;
        if (!resp_ok() || resp_dlen() != 36) FAIL("verify refused at 0x%X", (unsigned)addr);
        if (memcmp(resp_data(), s_md5hex, 32) != 0) FAIL("verify failed at 0x%X", (unsigned)addr);
    }

    // Full image: erase the rest of the chip, one segment at a time, and
    // verify each against the MD5 of an erased segment.
    if (s_kind == KIND_FULL) {
        md5_init(&s_md5);
        memset(s_blk + 64, 0xFF, ROM_BLOCK);
        for (s_i = 0; s_i < SEG_BYTES / ROM_BLOCK; s_i++) md5_update(&s_md5, s_blk + 64, ROM_BLOCK);
        md5_hex(&s_md5, s_md5_ff);
        for (s_pos = s_nseg * SEG_BYTES; s_pos < s_flash_bytes; s_pos += SEG_BYTES) {
            progress(STAGE_WRITE, pct_of(s_pos, s_total), "erasing the rest");
            PT_REQ(rom_flash_begin(SEG_BYTES, SEG_BYTES / ROM_BLOCK, s_pos));
            if (!resp_ok()) FAIL("erase failed at 0x%X", (unsigned)s_pos);
            {
                uint8_t a[16];
                put32(a, s_pos); put32(a + 4, SEG_BYTES); put32(a + 8, 0); put32(a + 12, 0);
                PT_REQ(rom_cmd(ROM_SPI_FLASH_MD5, a, 16, 0, 0, 0, 15000, false));
            }
            if (!resp_ok() || resp_dlen() != 36) FAIL("verify refused at 0x%X", (unsigned)s_pos);
            if (memcmp(resp_data(), s_md5_ff, 32) != 0) FAIL("erase check failed at 0x%X", (unsigned)s_pos);
        }
    }
    job_end(true, "written");
    PT_END();
}

// ── Bulk-IN resubmit loop ────────────────────────────────────────────────────

static void in_pipe_cb(UsbPipe* p, UsbXferResult res, uint32_t actual, void*) {
    if (res == USB_XFER_OK && actual > 0) {
        if (s_hold) slip_feed(s_hapi->pipe_buf(p), actual);
        else        s_hapi->link_rx(s_hapi->pipe_buf(p), actual);
    }
    if (s_on && !s_hapi->flash_guard_pending()
             && res != USB_XFER_NO_DEVICE
             && res != USB_XFER_CANCELED) {
        if (!s_hapi->pipe_submit(p, s_prof.mps_in, in_pipe_cb, 0))
            ulog("espserial: resubmit failed");
    }
}

// ── Lifecycle ────────────────────────────────────────────────────────────────

static bool es_probe(const UsbHostApi* api) {
    s_hapi = api;
    memset(&s_prof, 0, sizeof(s_prof));
    s_on = false;
    if (s_pipe_in)  { api->pipe_close(s_pipe_in);  s_pipe_in = 0; }
    if (s_pipe_out) { api->pipe_close(s_pipe_out); s_pipe_out = 0; }

    s_hold = false;
    s_job = 0; s_pc = 0; s_pending = 0;
    s_waiting = false; s_resp_ready = false;
    s_checked = false; s_synced = false;
    s_guard = false;
    slip_reset();
    memset(s_status, 0, sizeof(s_status));
    memset(s_status_sent, 0, sizeof(s_status_sent));

    const usb_device_desc_t* dd = (const usb_device_desc_t*)api->device_desc();
    if (!dd) return false;
    if (dd->idVendor == VID_ESPRESSIF)   s_prof.tr = TR_NATIVE;
    else if (dd->idVendor == VID_SILABS) s_prof.tr = TR_CP210X;
    else return false;

    const usb_config_desc_t* cfg = (const usb_config_desc_t*)api->config_desc();
    if (!cfg) return false;

    bool    cur_data = false;
    uint8_t cur_if   = 0;
    const usb_standard_desc_t* d = (const usb_standard_desc_t*)cfg;
    int offset = 0;
    while ((d = usb_parse_next_descriptor(d, cfg->wTotalLength, &offset)) != 0) {
        if (d->bDescriptorType == USB_B_DESCRIPTOR_TYPE_INTERFACE) {
            const usb_intf_desc_t* i = (const usb_intf_desc_t*)d;
            cur_if   = i->bInterfaceNumber;
            cur_data = false;
            if (i->bAlternateSetting != 0) continue;
            if (s_prof.tr == TR_CP210X) {
                cur_data = i->bInterfaceClass == 0xFF;   // the bridge's one vendor interface
            } else if (i->bInterfaceClass == 0x02) {   // CDC comm
                if (!s_prof.have_comm) {
                    s_prof.comm_if   = cur_if;
                    s_prof.have_comm = true;
                }
            } else if (i->bInterfaceClass == 0x0A) {   // CDC data
                cur_data = true;
            }
        } else if (d->bDescriptorType == USB_B_DESCRIPTOR_TYPE_ENDPOINT) {
            const usb_ep_desc_t* e = (const usb_ep_desc_t*)d;
            if (!cur_data || s_prof.valid) continue;
            if ((e->bmAttributes & 0x03) != 2 /*bulk*/) continue;
            uint16_t mps = USB_EP_DESC_GET_MPS(e);
            if (mps > 64) mps = 64;
            if (USB_EP_DESC_GET_EP_DIR(e) != 0) {
                s_prof.ep_in  = e->bEndpointAddress;
                s_prof.mps_in = mps;
            } else {
                s_prof.ep_out  = e->bEndpointAddress;
                s_prof.mps_out = mps;
            }
            if (s_prof.ep_in && s_prof.ep_out) {
                s_prof.data_if = cur_if;
                s_prof.valid   = true;
            }
        }
    }
    if (!s_prof.valid) return false;

    if (s_prof.tr == TR_CP210X)
        ulog("  espserial: CP210x bridge IF %u ep in %02X out %02X",
             s_prof.data_if, s_prof.ep_in, s_prof.ep_out);
    else
        ulog("  espserial: Espressif serial IF %u ep in %02X out %02X (comm IF %u)",
             s_prof.data_if, s_prof.ep_in, s_prof.ep_out,
             s_prof.have_comm ? s_prof.comm_if : 0xFF);
    return true;
}

// Bring a CP210x up as 115200 8N1 with DTR and RTS released. (A bridge left
// with the lines asserted holds the target in reset or in download mode.)
static bool cp210x_open(const UsbHostApi* api) {
    if (!api->control(0x41, CP_IFC_ENABLE, 1, s_prof.data_if, 0, 0)) return false;
    api->control(0x41, CP_SET_LINE_CTL, 0x0800, s_prof.data_if, 0, 0);
    if (!cp210x_set_baud(115200)) return false;
    api->control(0x41, CP_SET_MHS, 0x0300, s_prof.data_if, 0, 0);
    api->control(0x41, CP_PURGE, 0x000F, s_prof.data_if, 0, 0);
    return true;
}

static bool es_want(void) { return s_prof.valid; }

static void link_up(const UsbHostApi* api) {
    // CDC SET_CONTROL_LINE_STATE DTR|RTS: the peer's HWCDC gates its TX on DTR.
    if (s_prof.have_comm && !api->control(0x21, 0x22, 0x0003, s_prof.comm_if, 0, 0))
        ulog("espserial: line-state failed (continuing)");
    s_linked = api->link_register(&s_link_ops);
    if (!s_linked) ulog("espserial: link socket busy - transport idle");
    else           ulog(">>> Peer link up.");
}

static bool es_start(const UsbHostApi* api) {
    if (s_on) return true;
    if (!api->claim_interface(s_prof.data_if, 0)) {
        s_prof.valid = false;
        return false;
    }
    if (s_prof.have_comm) api->claim_interface(s_prof.comm_if, 0);   // tolerated-fail

    s_pipe_in  = api->pipe_open(s_prof.ep_in,  s_prof.mps_in,  s_prof.mps_in);
    s_pipe_out = api->pipe_open(s_prof.ep_out, s_prof.mps_out, OUT_BUF);
    if (!s_pipe_in || !s_pipe_out) {
        ulog("espserial: pipe alloc failed");
        if (s_pipe_in)  { api->pipe_close(s_pipe_in);  s_pipe_in = 0; }
        if (s_pipe_out) { api->pipe_close(s_pipe_out); s_pipe_out = 0; }
        api->release_interface(s_prof.data_if);
        if (s_prof.have_comm) api->release_interface(s_prof.comm_if);
        s_prof.valid = false;
        return false;
    }

    // The blob present now only decides who owns the port (see the header).
    s_cmd_len = api->command(&usbdrv_ops, s_cmd, sizeof(s_cmd), &s_cmd_seq);
    s_hold    = s_cmd_len > 0;
    s_settle  = now();

    if (s_prof.tr == TR_CP210X && !cp210x_open(api)) {
        ulog("espserial: CP210x setup failed");
        api->pipe_close(s_pipe_in);  s_pipe_in = 0;
        api->pipe_close(s_pipe_out); s_pipe_out = 0;
        api->release_interface(s_prof.data_if);
        s_prof.valid = false;
        return false;
    }

    s_on = true;
    if (!api->pipe_submit(s_pipe_in, s_prof.mps_in, in_pipe_cb, 0)) {
        ulog("espserial: submit failed");
        s_on = false;
        api->pipe_close(s_pipe_in);  s_pipe_in = 0;
        api->pipe_close(s_pipe_out); s_pipe_out = 0;
        api->release_interface(s_prof.data_if);
        if (s_prof.have_comm) api->release_interface(s_prof.comm_if);
        s_prof.valid = false;
        return false;
    }

    if (s_hold)                          ulog(">>> Serial port held for the flasher.");
    else if (s_prof.tr == TR_NATIVE)     link_up(api);
    else                                 ulog(">>> USB-UART bridge: flasher only, no peer link.");

    s_stamp   = now() | 1;
    s_job_seq = 0;
    status_set(ST_IDLE, 0, STAGE_NONE, 0, "");
    return true;
}

static void es_stop(const UsbHostApi* api, bool) {
    if (s_linked) { api->link_unregister(); s_linked = false; }
    s_on = false;
    s_job = 0; s_pending = 0; s_waiting = false;
    api->file_close(&usbdrv_ops);
    if (s_pipe_in)  { api->pipe_close(s_pipe_in);  s_pipe_in = 0; }
    if (s_pipe_out) { api->pipe_close(s_pipe_out); s_pipe_out = 0; }
    api->release_interface(s_prof.data_if);
    if (s_prof.have_comm) api->release_interface(s_prof.comm_if);
    ulog("Serial port closed.");
}

static void on_command(const UsbHostApi* api) {
    if (s_cmd_len == 0) {                         // released: back to the peer link
        if (s_job) job_end(false, "released");
        s_pending = 0;
        s_checked = false;
        api->file_close(&usbdrv_ops);
        if (s_hold) {
            s_hold = false;
            if (s_prof.tr == TR_NATIVE) link_up(api);
        }
        s_job_seq = 0;
        status_set(ST_IDLE, 0, STAGE_NONE, 0, "");
        return;
    }

    if (!s_hold) {
        // link_send refuses from here on; a send already under way finishes
        // inside the settle time (its pipe_xfer is capped at 250 ms, and the
        // out pipe is also checked idle before the first request).
        s_hold = true;
        if (s_linked) { api->link_unregister(); s_linked = false; }
        s_settle = now() + 50;
        slip_reset();
        ulog(">>> Serial port held for the flasher.");
    }

    uint8_t op    = s_cmd[0];
    uint8_t nonce = s_cmd_len >= 2 ? s_cmd[1] : 0;
    if (op == OP_CANCEL) {
        if (s_job) job_end(false, "cancelled");
        s_pending = 0;
    } else if (op == OP_HOLD) {
        if (!s_job) {
            s_job_seq = nonce;
            status_set(ST_OK, OP_HOLD, STAGE_NONE, 0, "");
        }
    } else if ((op >= OP_BOOT && op <= OP_RESET) || op == OP_READ) {
        if (s_job) { ulog("espserial: busy, command %u ignored", op); return; }
        if (op == OP_CHECK) {
            if (s_cmd_len < 9 || s_cmd_len - 7 > sizeof(s_path) || s_cmd[s_cmd_len - 1] != 0) {
                ulog("espserial: bad CHECK command");
                return;
            }
            s_want_kind = s_cmd[2];
            s_want_off  = get32(s_cmd + 3);
            memcpy(s_path, s_cmd + 7, s_cmd_len - 7);
        } else if (op == OP_READ) {
            if (s_cmd_len < 12 || s_cmd_len - 10 > sizeof(s_path) || s_cmd[s_cmd_len - 1] != 0) {
                ulog("espserial: bad READ command");
                return;
            }
            s_want_off = get32(s_cmd + 2);
            s_want_len = get32(s_cmd + 6);
            memcpy(s_path, s_cmd + 10, s_cmd_len - 10);
        }
        s_pending = op;
        s_job_seq = nonce;
        status_set(ST_BUSY, op, STAGE_NONE, 0, "");
    }
}

static void es_tick(const UsbHostApi* api) {
    s_tick_t0 = now();

    uint32_t seq = 0;
    uint32_t n = api->command(&usbdrv_ops, s_cmd, sizeof(s_cmd), &seq);
    if (seq != s_cmd_seq) {
        s_cmd_seq = seq;
        s_cmd_len = n;
        on_command(api);
    }

    // No request can be answered while the IN loop is parked for an
    // internal-flash write; give the one in flight its full time again after.
    if (api->flash_guard_pending()) { s_guard = true; return; }
    if (s_guard) {
        s_guard = false;
        if (s_waiting) s_deadline = now() + s_timeout;
    }

    if (!s_hold) return;
    if (!s_job) {
        if (!s_pending) return;
        if ((int32_t)(now() - s_settle) < 0 || api->pipe_in_flight(s_pipe_out)) return;
        s_job = s_pending;
        s_pending = 0;
        s_pc = 0;
        s_stage = STAGE_NONE;
    }

    if (s_waiting && (int32_t)(now() - s_deadline) >= 0) {
        s_waiting = false;
        if (!s_soft) {
            snprintf(s_text, sizeof(s_text), "no answer from the target (command %02X)", s_wait_op);
            job_end(false, s_text);
            return;
        }
        s_timed_out  = true;
        s_resp_ready = true;
    }

    if (s_job == OP_BOOT)       job_boot();
    else if (s_job == OP_CHECK || s_job == OP_READ) job_check();   // READ shares the connect steps
    else if (s_job == OP_WRITE) job_write();
    else if (s_job == OP_RESET) job_reset();
}

static int es_busy(void) {
    return (s_pipe_in && s_hapi) ? s_hapi->pipe_in_flight(s_pipe_in) : 0;
}

static void es_park(void) {
    if (s_pipe_in && s_hapi) s_hapi->pipe_cancel(s_pipe_in);
}

static void es_resume(void) {
    if (!s_pipe_in || !s_hapi) return;
    if (s_hapi->pipe_in_flight(s_pipe_in)) return;
    s_hapi->pipe_reset(s_pipe_in);
    if (!s_hapi->pipe_submit(s_pipe_in, s_prof.mps_in, in_pipe_cb, 0))
        ulog("espserial: resume resubmit failed");
}

static void es_status(char* out, uint32_t n) {
    snprintf(out, n, s_hold ? "flasher" : s_linked ? "peer link"
                   : s_prof.tr == TR_CP210X ? "bridge" : "idle");
}

extern "C" const UsbDriverDesc usbdrv_ops = {
    USB_DRIVER_ABI_VERSION,
    "espserial",
    &es_probe,
    &es_want,
    &es_start,
    &es_stop,
    &es_busy,
    &es_park,
    &es_resume,
    &es_tick,
    &es_status,
};
