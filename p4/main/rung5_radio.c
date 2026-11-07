// Rung 5: SX1262 probe + SPI clock sweep (plan §6.15). Read-only — reset,
// GetStatus, and register reads at each candidate clock; no RF, no TX.
//
// Wiring (LilyGo lilygo_device_driver v1 config and wadamesh target.cpp
// agree byte-for-byte): SPI SCLK=2 MOSI=3 MISO=4 CS=24, BUSY=6 (direct
// GPIO), RESET = expander IO16, DIO1 = expander IO17 (input; the wadamesh
// port runs with DIO1 unconnected and polls for done-flags).
//
// The clock ladder brackets the two vendor values — RadioLib's default
// 2MHz (what wadamesh's working radio runs on this board; its findChip
// also retries up to 10x) and LilyGo's 10MHz driver constant — up to the
// SX1262 SPI maximum of 16MHz. Per clock: GetStatus (0xC0; chip mode
// bits 6:4 = 2 = STBY_RC after reset), the ASCII version string at
// 0x0320 (RadioLib's chip-detect register, prefix "SX126", two
// attempts), and the LoRa sync word 0x0740/41 (documented reset value
// 0x14 0x24). The summary names the highest clock whose version read
// passed on the first attempt.
//
// ReadRegister framing (Semtech sx126x driver): 4 command bytes — opcode
// 0x1D, addr hi, addr lo, one NOP — then data clocks out; the chip
// returns a status byte during each command byte. Every command waits
// for BUSY low first.

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "p4_shared.h"

#define PIN_SCLK 2
#define PIN_MOSI 3
#define PIN_MISO 4
#define PIN_CS   24
#define PIN_BUSY 6

bool g_p4_spi2_ready;

static spi_device_handle_t s_dev;

static bool busy_wait_low(void)
{
    int64_t t0 = esp_timer_get_time();
    while (gpio_get_level(PIN_BUSY)) {
        if (esp_timer_get_time() - t0 > 100000) return false;
        vTaskDelay(1);
    }
    return true;
}

static bool xfer(const uint8_t *tx, uint8_t *rx, size_t len)
{
    spi_transaction_t t = {
        .length = len * 8,
        .tx_buffer = tx,
        .rx_buffer = rx,
    };
    return spi_device_transmit(s_dev, &t) == ESP_OK;
}

void rung5_radio(void)
{
    printf("[radio] rung 5: SX1262 probe + spi clock sweep\n");

    printf("[radio] stage: busy gpio\n");
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << PIN_BUSY,
        .mode = GPIO_MODE_INPUT,
    };
    if (gpio_config(&io) != ESP_OK) {
        printf("[radio] FAIL: busy gpio config\n");
        return;
    }

    printf("[radio] stage: spi bus\n");
    spi_bus_config_t bus = {
        .sclk_io_num = PIN_SCLK,
        .mosi_io_num = PIN_MOSI,
        .miso_io_num = PIN_MISO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 512,
    };
    if (spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO) != ESP_OK) {
        printf("[radio] FAIL: spi bus init\n");
        return;
    }
    g_p4_spi2_ready = true;

    // Reset: RST low >100us then high (expander IO16, high since the
    // rung-2 power sequence), then BUSY falls when the chip reaches
    // STBY_RC. One reset for the whole sweep — reads are stateless.
    printf("[radio] stage: reset\n");
    if (!p4_xl_set(IO_SX1262_RST, false)) {
        printf("[radio] FAIL: expander write\n");
        return;
    }
    vTaskDelay(pdMS_TO_TICKS(10));
    if (!p4_xl_set(IO_SX1262_RST, true)) {
        printf("[radio] FAIL: expander write\n");
        return;
    }
    vTaskDelay(pdMS_TO_TICKS(20));
    if (!busy_wait_low()) {
        printf("[radio] FAIL: BUSY stuck high after reset\n");
        return;
    }

    printf("[radio] stage: clock sweep\n");
    static const unsigned mhz[] = { 1, 2, 4, 6, 8, 10, 12, 16 };
    int best = -1;
    bool any = false;
    char best_ver[17] = { 0 };

    for (unsigned c = 0; c < sizeof(mhz) / sizeof(mhz[0]); c++) {
        spi_device_interface_config_t dev = {
            .clock_speed_hz = (int)(mhz[c] * 1000000),
            .mode = 0,
            .spics_io_num = PIN_CS,
            .queue_size = 1,
        };
        if (spi_bus_add_device(SPI2_HOST, &dev, &s_dev) != ESP_OK) {
            printf("[radio] FAIL: device add @%uMHz\n", mhz[c]);
            return;
        }

        uint8_t st = 0;
        bool st_ok = false;
        if (busy_wait_low()) {
            uint8_t tx[2] = { 0xC0, 0x00 };
            uint8_t rx[2] = { 0 };
            if (xfer(tx, rx, 2)) {
                st = rx[1];
                st_ok = true;
            }
        }

        int ver_attempt = 0;
        char ver[17] = { 0 };
        uint8_t rxv[20] = { 0 };
        for (int a = 1; a <= 2 && ver_attempt == 0; a++) {
            if (!busy_wait_low()) break;
            uint8_t txv[20] = { 0x1D, 0x03, 0x20, 0x00 };
            memset(rxv, 0, sizeof(rxv));
            if (!xfer(txv, rxv, sizeof(rxv))) break;
            if (memcmp(&rxv[4], "SX126", 5) == 0) {
                ver_attempt = a;
                for (int i = 0; i < 16; i++) {
                    char ch = (char)rxv[4 + i];
                    ver[i] = (ch >= 0x20 && ch <= 0x7E) ? ch : '.';
                }
            }
        }

        uint8_t sw[2] = { 0xEE, 0xEE };
        if (busy_wait_low()) {
            uint8_t tx2[6] = { 0x1D, 0x07, 0x40, 0x00, 0x00, 0x00 };
            uint8_t rx2[6] = { 0 };
            if (xfer(tx2, rx2, 6)) {
                sw[0] = rx2[4];
                sw[1] = rx2[5];
            }
        }

        spi_bus_remove_device(s_dev);
        s_dev = NULL;

        if (ver_attempt > 0) {
            printf("[radio] %2uMHz: status %02X, version OK (attempt %d) "
                   "\"%s\", sync %02X %02X\n",
                   mhz[c], st, ver_attempt, ver, sw[0], sw[1]);
            any = true;
            if (ver_attempt == 1) {
                best = (int)c;
                memcpy(best_ver, ver, sizeof(best_ver));
            }
        } else {
            printf("[radio] %2uMHz: status %02X%s, version FAIL, "
                   "sync %02X %02X, raw:",
                   mhz[c], st, st_ok ? "" : " (status read failed)",
                   sw[0], sw[1]);
            for (unsigned i = 0; i < sizeof(rxv); i++) {
                printf(" %02X", rxv[i]);
            }
            printf("\n");
        }
    }

    if (any && best >= 0) {
        printf("[radio] SX1262 alive — probe PASS; highest first-attempt "
               "clock = %uMHz (version \"%s\")\n", mhz[best], best_ver);
    } else if (any) {
        printf("[radio] SX1262 alive — probe PASS, but no clock passed on "
               "the first attempt (retries only)\n");
    } else {
        printf("[radio] no SX126x version string at any clock — see the "
               "lr2021 stage below\n");
    }

    // Chip-identity cross-check: the board also ships in an LR2021 SKU.
    // LR2021 commands are 16-bit (RadioLib LR2021_commands.h; GetVersion
    // = 0x0101 answering fw major.minor) and command + response ride
    // SEPARATE chip-select frames with a BUSY wait between — the
    // LR11xx-family transaction shape. Read-only, no RF. An SX1262
    // echoes its status byte on EVERY response byte, so: all response
    // bytes equal = SX126x-style echo; varied structured bytes = an
    // LR-family answer.
    printf("[radio] stage: lr2021 getversion (2MHz)\n");
    spi_device_interface_config_t lrdev = {
        .clock_speed_hz = 2000000,
        .mode = 0,
        .spics_io_num = PIN_CS,
        .queue_size = 1,
    };
    if (spi_bus_add_device(SPI2_HOST, &lrdev, &s_dev) != ESP_OK) {
        printf("[radio] FAIL: device add (lr probe)\n");
        return;
    }
    if (!busy_wait_low()) {
        printf("[radio] FAIL: BUSY high before lr command\n");
        return;
    }
    uint8_t lr_cmd[2] = { 0x01, 0x01 };
    uint8_t lr_cmd_rx[2] = { 0 };
    if (!xfer(lr_cmd, lr_cmd_rx, sizeof(lr_cmd))) {
        printf("[radio] FAIL: spi transfer (lr command)\n");
        return;
    }
    printf("[radio] lr command frame rx: %02X %02X\n",
           lr_cmd_rx[0], lr_cmd_rx[1]);
    if (!busy_wait_low()) {
        printf("[radio] FAIL: BUSY high before lr response\n");
        return;
    }
    uint8_t lr_rsp_tx[4] = { 0 };
    uint8_t lr_rsp[4] = { 0 };
    if (!xfer(lr_rsp_tx, lr_rsp, sizeof(lr_rsp))) {
        printf("[radio] FAIL: spi transfer (lr response)\n");
        return;
    }
    spi_bus_remove_device(s_dev);
    s_dev = NULL;
    printf("[radio] lr response frame rx: %02X %02X %02X %02X\n",
           lr_rsp[0], lr_rsp[1], lr_rsp[2], lr_rsp[3]);
    bool lr_echo = (lr_rsp[0] == lr_rsp[1] && lr_rsp[1] == lr_rsp[2] &&
                    lr_rsp[2] == lr_rsp[3]);
    printf("[radio] lr2021 verdict: %s\n",
           lr_echo ? "uniform echo — SX126x-style, not an LR answer"
                   : "structured response — LR-family chip answered "
                     "(stat, fw major, fw minor)");
}
