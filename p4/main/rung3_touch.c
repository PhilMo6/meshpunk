// Rung 3: GT9895 touch controller (plan §6.15) — polled coordinate prints,
// no LVGL yet.
//
// Protocol facts from LilyGo's cpp_bus_driver gt9895 driver (the factory
// driver for this board), cross-checked against the mainline Linux
// goodix_berlin driver (same chip generation):
//  - I2C address 0x5D at 400kHz on the same bus as the XL9535; registers
//    are 32-bit addresses sent big-endian.
//  - Firmware info block at 0x00010014, 28 bytes, 16-bit byte-sum checksum
//    little-endian in the last 2 bytes; the block carries the ASCII
//    product id "9895" (the two sources disagree by one byte on its
//    offset, so the alive-check scans the block and the boot log prints
//    it raw).
//  - Runtime info block at 0x00010070 (first 2 bytes = total length)
//    carries the touch-data address the chip itself reports. Documented
//    offsets for it disagree between sources and do not match this
//    chip's actual block, so it is located by signature: a little-endian
//    0x0001xxxx address immediately followed by the u16 touch-data head
//    length and u16 point-struct length, both 8 on this chip generation
//    (those two fields are defined to follow the address). On this unit
//    the address is 0x10308 at block offset 91.
//  - Touch frame at the touch-data address: 8-byte head (own checksum in
//    bytes 6-7; byte 0 bit7 = touch event; byte 2 low nibble = contact
//    count) + 8 bytes per contact + 2-byte checksum over the contact
//    block. Per contact: byte 0 high nibble = id, bytes 2-3 = x LE,
//    4-5 = y LE, 6-7 = pressure. A 1-byte 0x00 write to the same address
//    acknowledges the frame.
//  - Raw coordinate space is 1060x2400 (LilyGo t_display_p4 config), NOT
//    panel pixels; mapping to 568x1232 belongs to the LVGL rung.
// Reset: TOUCH_RST = expander IO3, low 30ms then high 100ms (LilyGo
// init). INT (expander IO4) is unused — pure polling.

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"
#include "esp_ldo_regulator.h"
#include "p4_shared.h"

#define GT9895_ADDR      0x5D
#define REG_FW_INFO      0x00010014u
#define REG_RUNTIME_INFO 0x00010070u

#define MAX_CONTACTS 10
#define FRAME_MAX (8 + MAX_CONTACTS * 8 + 2)

static i2c_master_dev_handle_t s_gt;
static uint32_t s_touch_addr;
static bool s_ready;
static bool s_was_down;
static bool s_read_failing;

static bool gt_read(uint32_t reg, uint8_t *buf, size_t len)
{
    uint8_t addr[4] = { reg >> 24, reg >> 16, reg >> 8, reg };
    return i2c_master_transmit_receive(s_gt, addr, sizeof(addr),
                                       buf, len, 100) == ESP_OK;
}

static bool gt_write_byte(uint32_t reg, uint8_t val)
{
    uint8_t buf[5] = { reg >> 24, reg >> 16, reg >> 8, reg, val };
    return i2c_master_transmit(s_gt, buf, sizeof(buf), 100) == ESP_OK;
}

// 16-bit little-endian byte-sum checksum over a block whose last 2 bytes
// hold the stored sum.
static bool sum16_ok(const uint8_t *b, size_t len)
{
    uint16_t sum = 0;
    for (size_t i = 0; i < len - 2; i++) sum += b[i];
    return sum == (uint16_t)(b[len - 2] | (b[len - 1] << 8));
}

static void hex_dump(const char *tag, const uint8_t *b, size_t len)
{
    printf("%s", tag);
    for (size_t i = 0; i < len; i++) printf(" %02X", b[i]);
    printf("\n");
}

void rung3_touch_init(void)
{
    printf("[touch] rung 3: GT9895 (polled)\n");
    if (!g_p4_i2c0) {
        printf("[touch] SKIP: no I2C bus (rung 2 stopped early)\n");
        return;
    }

    // Second on-chip LDO rail: LilyGo's board init powers channel 4 at
    // 3300mV alongside the display's channel 3 (P4 SDMMC IO power is an
    // external supply per SOC_SDMMC_IO_POWER_EXTERNAL). Acquired once
    // here; the SD rung runs off the same rail.
    printf("[touch] stage: peripheral ldo (ch4 3300mV)\n");
    esp_ldo_channel_handle_t ldo = NULL;
    esp_ldo_channel_config_t ldo_cfg = { .chan_id = 4, .voltage_mv = 3300 };
    if (esp_ldo_acquire_channel(&ldo_cfg, &ldo) != ESP_OK) {
        printf("[touch] FAIL: ldo channel 4\n");
        return;
    }

    printf("[touch] stage: reset\n");
    if (!p4_xl_set(IO_TOUCH_RST, false)) {
        printf("[touch] FAIL: expander write\n");
        return;
    }
    vTaskDelay(pdMS_TO_TICKS(30));
    if (!p4_xl_set(IO_TOUCH_RST, true)) {
        printf("[touch] FAIL: expander write\n");
        return;
    }
    vTaskDelay(pdMS_TO_TICKS(100));

    i2c_device_config_t dev_cfg = {
        .device_address = GT9895_ADDR,
        .scl_speed_hz = 400000,
    };
    if (i2c_master_bus_add_device(g_p4_i2c0, &dev_cfg, &s_gt) != ESP_OK) {
        printf("[touch] FAIL: device add\n");
        return;
    }

    printf("[touch] stage: fw info\n");
    uint8_t fw[28];
    if (!gt_read(REG_FW_INFO, fw, sizeof(fw))) {
        printf("[touch] FAIL: fw info read (no GT9895 at 0x5D?)\n");
        return;
    }
    hex_dump("[touch] fw info:", fw, sizeof(fw));
    if (!sum16_ok(fw, sizeof(fw))) {
        printf("[touch] FAIL: fw info checksum\n");
        return;
    }
    bool id_ok = false;
    for (size_t i = 0; i + 4 <= sizeof(fw) - 2; i++) {
        if (memcmp(&fw[i], "9895", 4) == 0) {
            printf("[touch] product id \"9895\" at fw-info offset %u\n",
                   (unsigned)i);
            id_ok = true;
            break;
        }
    }
    if (!id_ok) {
        printf("[touch] FAIL: no \"9895\" id in fw info\n");
        return;
    }

    printf("[touch] stage: runtime info\n");
    uint8_t ri[128];
    if (!gt_read(REG_RUNTIME_INFO, ri, sizeof(ri))) {
        printf("[touch] FAIL: runtime info read\n");
        return;
    }
    unsigned ri_len = ri[0] | (ri[1] << 8);
    printf("[touch] runtime info length %u\n", ri_len);
    hex_dump("[touch] runtime info[0..111]:", ri, 112);
    unsigned found_at = 0, matches = 0;
    for (unsigned i = 4; i + 8 <= sizeof(ri); i++) {
        uint32_t a = ri[i] | (ri[i + 1] << 8) | ((uint32_t)ri[i + 2] << 16) |
                     ((uint32_t)ri[i + 3] << 24);
        unsigned head_len = ri[i + 4] | (ri[i + 5] << 8);
        unsigned point_len = ri[i + 6] | (ri[i + 7] << 8);
        if ((a >> 16) == 0x0001 && head_len == 8 && point_len == 8) {
            s_touch_addr = a;
            found_at = i;
            matches++;
        }
    }
    if (matches != 1) {
        printf("[touch] FAIL: touch data addr signature matched %u times\n",
               matches);
        return;
    }
    printf("[touch] touch data addr 0x%05lX (runtime info offset +%u)\n",
           (unsigned long)s_touch_addr, found_at);

    s_ready = true;
    printf("[touch] ready — tap the panel (raw space ~1060x2400)\n");
}

// Called from the main loop every 20ms. One full-size frame read per poll
// (raceless vs staged reads). Between events the area holds stale
// non-frame bytes with the event bit clear (constant on hardware), so the
// event flag gates everything; checksums apply only to posted frames.
void rung3_touch_poll(void)
{
    if (!s_ready) return;

    uint8_t f[FRAME_MAX];
    if (!gt_read(s_touch_addr, f, sizeof(f))) {
        if (!s_read_failing) {
            printf("[touch] frame read FAILING\n");
            s_read_failing = true;
        }
        return;
    }
    if (s_read_failing) {
        printf("[touch] frame read recovered\n");
        s_read_failing = false;
    }

    if (!(f[0] & 0x80)) {
        if (s_was_down) {
            printf("[touch] up\n");
            s_was_down = false;
        }
        return;
    }

    if (!sum16_ok(f, 8)) {
        hex_dump("[touch] head checksum mismatch:", f, 8);
        gt_write_byte(s_touch_addr, 0x00);
        return;
    }

    unsigned count = f[2] & 0x0F;
    if (count > MAX_CONTACTS) {
        hex_dump("[touch] bad contact count, head:", f, 8);
        gt_write_byte(s_touch_addr, 0x00);
        return;
    }
    if (count == 0) {
        if (s_was_down) {
            printf("[touch] up\n");
            s_was_down = false;
        }
        gt_write_byte(s_touch_addr, 0x00);
        return;
    }

    if (!sum16_ok(&f[8], count * 8 + 2)) {
        hex_dump("[touch] contact checksum mismatch:", f, 8 + count * 8 + 2);
        gt_write_byte(s_touch_addr, 0x00);
        return;
    }

    printf("[touch] %u contact(s):", count);
    for (unsigned i = 0; i < count && i < 3; i++) {
        const uint8_t *c = &f[8 + i * 8];
        printf(" id%u x=%u y=%u p=%u", c[0] >> 4,
               c[2] | (c[3] << 8), c[4] | (c[5] << 8), c[6] | (c[7] << 8));
    }
    printf("\n");
    s_was_down = true;
    gt_write_byte(s_touch_addr, 0x00);
}
