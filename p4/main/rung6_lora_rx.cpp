// Rung 6: LR2021 LoRa RX + TX vs a T-Deck (plan §6.15).
//
// THIS BUILD TRANSMITS. An antenna must be on MMCX1 before flashing —
// keying the PA into no load can damage it. TX runs at 4dBm, five short
// packets, nothing else.
//
// RX is hardware-proven at the preset below (MeshCore packets from the
// T-Deck decode with CRC valid). The module runs a PLAIN CRYSTAL:
// tcxoVoltage must be 0.0f — RadioLib's 1.6V default configures a TCXO
// this module does not have and the receiver hears nothing, while
// begin() returns 0 for both settings, so only received packets
// discriminate.
//
// Driver = RadioLib (IDF component registry, jgromes/radiolib) — the
// LR2021 class MeshCore's CustomLR2021 wrapper builds on. This board
// routes the radio DIO only to the expander, so there is no hardware
// IRQ: RX done and TX done are polled via getIrqStatus() over SPI.
// Reset = expander IO16 (Module gets RADIOLIB_NC for reset), BUSY =
// GPIO6 direct, NSS = GPIO24 driven by RadioLib (the SPI device is added
// without a hardware CS).
//
// Wire contract (mesh side): sync word 0x12 + preamble 8 (modules/
// meshcore mcmain.cpp). The four defines below are the Radio app's
// "USA/Canada" preset. The TX payload is ASCII-tagged so it is
// recognizable in the T-Deck's Tools/Packets capture.
//
// Both directions are hardware-proven; the rung runs as a short per-boot
// regression canary (RX_WINDOW_S seconds of listen, TX_COUNT packets —
// raise for a fuller sweep). A CRC-errored receive must clear its
// latched IRQ flags explicitly: startReceive() alone does not, and the
// same flags then re-read forever. readData() clears them on the
// good-packet path.

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"
#include "p4_shared.h"

#include <RadioLib.h>

#define MESH_FREQ_MHZ 910.525f
#define MESH_BW_KHZ   62.5f
#define MESH_SF       7
#define MESH_CR       5
#define TX_POWER_DBM  4
#define TX_COUNT      1
#define RX_WINDOW_S   6

#define PIN_NSS  24
#define PIN_BUSY 6

static spi_device_handle_t s_spi;

class P4Hal : public RadioLibHal {
public:
    P4Hal()
        : RadioLibHal(GPIO_MODE_INPUT, GPIO_MODE_OUTPUT, 0, 1, 0, 1) {}

    void pinMode(uint32_t pin, uint32_t mode) override
    {
        if (pin == RADIOLIB_NC) return;
        gpio_config_t cfg = {};
        cfg.pin_bit_mask = 1ULL << pin;
        cfg.mode = (gpio_mode_t)mode;
        gpio_config(&cfg);
    }
    void digitalWrite(uint32_t pin, uint32_t value) override
    {
        if (pin == RADIOLIB_NC) return;
        gpio_set_level((gpio_num_t)pin, value);
    }
    uint32_t digitalRead(uint32_t pin) override
    {
        if (pin == RADIOLIB_NC) return 0;
        return gpio_get_level((gpio_num_t)pin);
    }
    void attachInterrupt(uint32_t, void (*)(void), uint32_t) override {}
    void detachInterrupt(uint32_t) override {}
    void delay(RadioLibTime_t ms) override
    {
        TickType_t ticks = pdMS_TO_TICKS(ms);
        vTaskDelay(ticks ? ticks : 1);
    }
    void delayMicroseconds(RadioLibTime_t us) override
    {
        esp_rom_delay_us((uint32_t)us);
    }
    RadioLibTime_t millis() override
    {
        return (RadioLibTime_t)(esp_timer_get_time() / 1000LL);
    }
    RadioLibTime_t micros() override
    {
        return (RadioLibTime_t)esp_timer_get_time();
    }
    long pulseIn(uint32_t, uint32_t, RadioLibTime_t) override { return 0; }
    void spiBegin() override {}
    void spiBeginTransaction() override {}
    void spiTransfer(uint8_t *out, size_t len, uint8_t *in) override
    {
        spi_transaction_t t = {};
        t.length = len * 8;
        t.tx_buffer = out;
        t.rx_buffer = in;
        spi_device_transmit(s_spi, &t);
    }
    void spiEndTransaction() override {}
    void spiEnd() override {}
    void yield() override { taskYIELD(); }
};

static bool radio_reset(void)
{
    if (!p4_xl_set(IO_SX1262_RST, false)) return false;
    vTaskDelay(pdMS_TO_TICKS(10));
    if (!p4_xl_set(IO_SX1262_RST, true)) return false;
    vTaskDelay(pdMS_TO_TICKS(20));
    return true;
}

extern "C" void rung6_lora_rx(void)
{
    printf("[lora] rung 6: LR2021 RX + TX — %.3f MHz, BW %.1f, SF%d, "
           "CR%d, sync 0x12, preamble 8, tx %ddBm\n",
           MESH_FREQ_MHZ, MESH_BW_KHZ, MESH_SF, MESH_CR, TX_POWER_DBM);
    printf("[lora] TX BUILD — antenna on MMCX1 required\n");
    if (!g_p4_i2c0) {
        printf("[lora] SKIP: no expander (rung 2 stopped early)\n");
        return;
    }
    if (!g_p4_spi2_ready) {
        printf("[lora] SKIP: SPI bus not up (rung 5 stopped early)\n");
        return;
    }

    spi_device_interface_config_t dev = {};
    dev.clock_speed_hz = 2000000;
    dev.mode = 0;
    dev.spics_io_num = -1;
    dev.queue_size = 1;
    if (spi_bus_add_device(SPI2_HOST, &dev, &s_spi) != ESP_OK) {
        printf("[lora] FAIL: spi device add\n");
        return;
    }

    static P4Hal hal;
    static Module mod(&hal, PIN_NSS, RADIOLIB_NC, RADIOLIB_NC, PIN_BUSY);
    static LR2021 radio(&mod);

    printf("[lora] stage: reset + begin (crystal, tcxo 0.0V)\n");
    if (!radio_reset()) {
        printf("[lora] FAIL: expander write\n");
        return;
    }
    int16_t st = radio.begin(MESH_FREQ_MHZ, MESH_BW_KHZ, MESH_SF, MESH_CR,
                             RADIOLIB_LR2021_LORA_SYNC_WORD_PRIVATE,
                             TX_POWER_DBM, 8, 0.0f);
    printf("[lora] begin = %d\n", st);
    if (st != RADIOLIB_ERR_NONE) return;

    // RX window first: regression check against the T-Deck before any TX.
    st = radio.startReceive();
    printf("[lora] startReceive = %d\n", st);
    if (st != RADIOLIB_ERR_NONE) return;
    printf("[lora] listening %ds — send from the T-Deck now\n",
           RX_WINDOW_S);
    int rx_pkts = 0;
    int64_t t0 = esp_timer_get_time();
    while (esp_timer_get_time() - t0 < RX_WINDOW_S * 1000000LL) {
        uint32_t irq = radio.getIrqStatus();
        if (irq & RADIOLIB_LR2021_IRQ_CRC_ERROR) {
            printf("[lora] packet with CRC error (irq 0x%08lX)\n",
                   (unsigned long)irq);
            radio.clearIrqFlags(irq);
            radio.startReceive();
        } else if (irq & RADIOLIB_LR2021_IRQ_RX_DONE) {
            uint8_t buf[256] = { 0 };
            size_t len = radio.getPacketLength();
            if (len > sizeof(buf)) len = sizeof(buf);
            int16_t rst = radio.readData(buf, len);
            printf("[lora] RX %u bytes (read=%d) RSSI %.1f SNR %.1f:",
                   (unsigned)len, rst, radio.getRSSI(), radio.getSNR());
            for (size_t b = 0; b < len && b < 24; b++) {
                printf(" %02X", buf[b]);
            }
            printf("%s\n", len > 24 ? " ..." : "");
            rx_pkts++;
            radio.startReceive();
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    printf("[lora] rx window: %d packet(s)\n", rx_pkts);

    // TX phase: five tagged packets, watched from the T-Deck in
    // Tools/Packets. TX done is polled like RX (no IRQ line).
    printf("[lora] stage: tx — %d packets at %ddBm, 2s apart\n",
           TX_COUNT, TX_POWER_DBM);
    radio.standby();
    int tx_ok = 0;
    for (int n = 1; n <= TX_COUNT; n++) {
        char payload[40];
        int plen = snprintf(payload, sizeof(payload),
                            "MESHPUNK P4 TX %d/%d", n, TX_COUNT);
        st = radio.startTransmit((uint8_t *)payload, (size_t)plen);
        if (st != RADIOLIB_ERR_NONE) {
            printf("[lora] tx %d: startTransmit = %d\n", n, st);
            continue;
        }
        int64_t ts = esp_timer_get_time();
        bool done = false;
        while (esp_timer_get_time() - ts < 5000000LL) {
            uint32_t irq = radio.getIrqStatus();
            if (irq & RADIOLIB_LR2021_IRQ_TX_DONE) {
                done = true;
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        radio.finishTransmit();
        if (done) {
            printf("[lora] tx %d/%d done (%lld ms)\n", n, TX_COUNT,
                   (long long)((esp_timer_get_time() - ts) / 1000));
            tx_ok++;
        } else {
            printf("[lora] tx %d/%d TIMEOUT waiting TX_DONE (irq 0x%08lX)"
                   "\n", n, (int)TX_COUNT,
                   (unsigned long)radio.getIrqStatus());
        }
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
    radio.standby();
    printf("[lora] verdict: rx %d packet(s), tx %d/%d done — check "
           "Tools/Packets on the T-Deck for \"MESHPUNK P4 TX\"\n",
           rx_pkts, tx_ok, TX_COUNT);
}
