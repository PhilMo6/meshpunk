// Rung 2: RM69A10 AMOLED over MIPI-DSI — color bars, no LVGL yet
// (plan §6.15: "DSI panel + LVGL smoke", step A).
//
// The bring-up sequence replicates wadamesh's variants/tdisplay_p4 port
// (RM69A10Display.cpp + Xl9535.cpp — the implementation PROVEN on this
// exact board), cross-checked against LilyGo's lilygo_device_driver
// t_display_p4/v1 config and the IDF 5.5.5 headers on disk. The
// hardware-won facts that differ from first-principles defaults:
//  - DSI PHY LDO channel 3 runs at 1830 mV on this board — 2500 mV (the
//    IDF example value) leaves the panel dark.
//  - The expander power rails are power-CYCLED with 200 ms phases, with
//    mixed final polarities: VCCA(IO10)=LOW, 5V(IO6)=HIGH, 3V3(IO0)=LOW.
//  - Screen reset (IO2) is HIGH->LOW->HIGH with 200 ms per phase, after
//    the LDO, before the DSI bus.
//  - The DPI stream starts (with a blanked frame buffer) BEFORE the panel
//    init commands, so display-on never shows garbage.
//  - The vendor init table parks brightness at zero; 0x51=0xFF goes out
//    after display-on or a working panel stays black.
// Every stage prints BEFORE the work so a hang or failure names itself.

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"
#include "esp_ldo_regulator.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_cache.h"
#include "driver/gpio.h"
#include "esp_rom_sys.h"
#include "p4_shared.h"

#define V1_I2C_SDA 7
#define V1_I2C_SCL 8
#define V2_I2C_SDA 9
#define V2_I2C_SCL 10
#define XL9535_ADDR 0x20

// XL9535 registers (PCA9535-compatible): output ports 0x02/0x03, config
// ports 0x06/0x07 (config bit 1 = input). The IO map lives in p4_shared.h
// (wadamesh Xl9535.h numbering, identical to LilyGo's v1 config where
// both name a line).
#define XL_REG_OUT0 0x02
#define XL_REG_OUT1 0x03
#define XL_REG_CFG0 0x06
#define XL_REG_CFG1 0x07

#define PANEL_W 568
#define PANEL_H 1232

typedef struct {
    uint8_t  cmd;
    uint8_t  len;
    uint8_t  data[4];
    uint16_t delay_ms;
} dcs_cmd_t;

// LilyGo's current vendor init table (cpp_bus_driver rm69a10.h), verbatim:
// manufacturer pages, CASET 0-567, RASET 0-1231, 0x31/0x30 windows, 0x12,
// TE on, brightness 0 (raised after display-on), SLPOUT + 120 ms, DISPON.
static const dcs_cmd_t k_rm69a10_init[] = {
    { 0xFE, 1, {0xFD}, 0 },
    { 0x80, 1, {0xFC}, 0 },
    { 0xFE, 1, {0x00}, 0 },
    { 0x2A, 4, {0x00, 0x00, 0x02, 0x37}, 0 },
    { 0x2B, 4, {0x00, 0x00, 0x04, 0xCF}, 0 },
    { 0x31, 4, {0x00, 0x03, 0x02, 0x34}, 0 },
    { 0x30, 4, {0x00, 0x00, 0x04, 0xCF}, 0 },
    { 0x12, 1, {0x00}, 0 },
    { 0x35, 1, {0x00}, 0 },
    { 0x51, 1, {0x00}, 0 },
    { 0x11, 0, {0}, 120 },
    { 0x29, 0, {0}, 20 },
};

// Rung 2 owns the bus; later rungs add their own devices on it.
i2c_master_bus_handle_t g_p4_i2c0;

// Expander state shadows; all writes go through p4_xl_set so the two
// output registers stay coherent.
static i2c_master_dev_handle_t s_xl;
static uint8_t s_out[2];

static bool xl_reg(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    return i2c_master_transmit(s_xl, buf, sizeof(buf), 100) == ESP_OK;
}

bool p4_xl_set(uint8_t io, bool level)
{
    uint8_t port = (io >= 10) ? 1 : 0;
    uint8_t bit = (io >= 10) ? (io - 10) : io;
    if (level) s_out[port] |= (1u << bit);
    else       s_out[port] &= (uint8_t)~(1u << bit);
    return xl_reg(port ? XL_REG_OUT1 : XL_REG_OUT0, s_out[port]);
}

void rung2_display(void)
{
    printf("[disp] rung 2: RM69A10 over MIPI-DSI (wadamesh-proven sequence)\n");

    // Expander probe: v1 pins first; a v2 board (different chip set) is
    // identified and reported, not driven.
    printf("[disp] stage: i2c probe\n");

    // I2C-spec bus clear before first use: a chip reset that lands inside
    // an I2C transaction (upload resets hit the 20ms touch poll) leaves
    // the addressed slave driving SDA low until it sees clock edges —
    // observed as a boot-to-boot bus timeout cleared only by power loss.
    // Up to nine SCL pulses let the slave finish its byte; the
    // START+STOP after releases every slave's address decoder.
    gpio_config_t od = {
        .pin_bit_mask = (1ULL << V1_I2C_SDA) | (1ULL << V1_I2C_SCL),
        .mode = GPIO_MODE_INPUT_OUTPUT_OD,
    };
    gpio_config(&od);
    gpio_set_level(V1_I2C_SDA, 1);
    gpio_set_level(V1_I2C_SCL, 1);
    esp_rom_delay_us(10);
    if (!gpio_get_level(V1_I2C_SDA)) {
        int pulses = 0;
        while (pulses < 9 && !gpio_get_level(V1_I2C_SDA)) {
            gpio_set_level(V1_I2C_SCL, 0);
            esp_rom_delay_us(5);
            gpio_set_level(V1_I2C_SCL, 1);
            esp_rom_delay_us(5);
            pulses++;
        }
        printf("[disp] bus-clear: SDA was held low, %s after %d pulses\n",
               gpio_get_level(V1_I2C_SDA) ? "released" : "STILL HELD",
               pulses);
    }
    gpio_set_level(V1_I2C_SDA, 0);
    esp_rom_delay_us(5);
    gpio_set_level(V1_I2C_SDA, 1);
    esp_rom_delay_us(5);

    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = 0,
        .sda_io_num = V1_I2C_SDA,
        .scl_io_num = V1_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus = NULL;
    if (i2c_new_master_bus(&bus_cfg, &bus) != ESP_OK) {
        printf("[disp] FAIL: i2c bus create\n");
        return;
    }
    if (i2c_master_probe(bus, XL9535_ADDR, 100) != ESP_OK) {
        i2c_del_master_bus(bus);
        bus_cfg.sda_io_num = V2_I2C_SDA;
        bus_cfg.scl_io_num = V2_I2C_SCL;
        bus = NULL;
        if (i2c_new_master_bus(&bus_cfg, &bus) == ESP_OK &&
            i2c_master_probe(bus, XL9535_ADDR, 100) == ESP_OK) {
            printf("[disp] STOP: XL9535 answers on v2 pins (SDA9/SCL10) — "
                   "this is a v2 board; rung 2 is wired for v1\n");
            return;
        }
        printf("[disp] STOP: no XL9535 at 0x20 on v1 (7/8) or v2 (9/10) "
               "pins — bus diagnostics:\n");
        if (bus) i2c_del_master_bus(bus);

        // Raw v1 line levels. Both high = idle bus with pullups working;
        // SDA low = a slave wedged mid-byte is holding it (a chip reset
        // that lands inside an I2C transaction leaves the addressed
        // slave driving SDA until it sees clock edges).
        gpio_config_t lines = {
            .pin_bit_mask = (1ULL << V1_I2C_SDA) | (1ULL << V1_I2C_SCL),
            .mode = GPIO_MODE_INPUT,
        };
        gpio_config(&lines);
        int sda = gpio_get_level(V1_I2C_SDA);
        int scl = gpio_get_level(V1_I2C_SCL);
        printf("[disp] v1 line levels: SDA=%d SCL=%d (1/1 = idle)\n",
               sda, scl);
        if (!sda || !scl) {
            printf("[disp] bus held low — wedged; scan skipped\n");
            return;
        }

        bus_cfg.sda_io_num = V1_I2C_SDA;
        bus_cfg.scl_io_num = V1_I2C_SCL;
        bus = NULL;
        if (i2c_new_master_bus(&bus_cfg, &bus) == ESP_OK) {
            int found = 0;
            printf("[disp] v1 bus scan:");
            for (uint8_t a = 0x08; a <= 0x77; a++) {
                if (i2c_master_probe(bus, a, 20) == ESP_OK) {
                    printf(" 0x%02X", a);
                    found++;
                }
            }
            printf("%s (%d device%s)\n", found ? "" : " none", found,
                   found == 1 ? "" : "s");
        }
        return;
    }
    printf("[disp] XL9535 found on v1 pins (SDA7/SCL8)\n");
    g_p4_i2c0 = bus;

    i2c_device_config_t dev_cfg = {
        .device_address = XL9535_ADDR,
        .scl_speed_hz = 400000,
    };
    if (i2c_master_bus_add_device(bus, &dev_cfg, &s_xl) != ESP_OK) {
        printf("[disp] FAIL: expander device add\n");
        return;
    }

    // Expander base state (wadamesh Xl9535::begin): every pin an output at
    // HIGH, then the four interrupt lines back to inputs.
    printf("[disp] stage: expander base state\n");
    s_out[0] = 0xFF;
    s_out[1] = 0xFF;
    bool ok = xl_reg(XL_REG_OUT0, s_out[0]) && xl_reg(XL_REG_OUT1, s_out[1]) &&
              xl_reg(XL_REG_CFG0, (1u << IO_TOUCH_INT) |
                                  (1u << IO_SENSOR_INT)) &&
              xl_reg(XL_REG_CFG1, (1u << (IO_RTC_INT - 10)) |
                                  (1u << (IO_SX1262_DIO1 - 10)));
    if (!ok) {
        printf("[disp] FAIL: expander base writes\n");
        return;
    }

    // Power-rail cycle (wadamesh powerOnSequence, 200 ms phases; final
    // states VCCA=LOW, 5V=HIGH, 3V3=LOW).
    printf("[disp] stage: power rail cycle\n");
    ok = p4_xl_set(IO_GPS_WAKE, true) &&
         p4_xl_set(IO_VCCA_EN, false) &&
         p4_xl_set(IO_5V_EN, true);
    vTaskDelay(pdMS_TO_TICKS(200));
    ok = ok && p4_xl_set(IO_5V_EN, false);
    vTaskDelay(pdMS_TO_TICKS(200));
    ok = ok && p4_xl_set(IO_5V_EN, true) &&
               p4_xl_set(IO_3V3_EN, false);
    vTaskDelay(pdMS_TO_TICKS(200));
    ok = ok && p4_xl_set(IO_3V3_EN, true);
    vTaskDelay(pdMS_TO_TICKS(200));
    ok = ok && p4_xl_set(IO_3V3_EN, false);
    vTaskDelay(pdMS_TO_TICKS(200));
    ok = ok && p4_xl_set(IO_SD_EN, false) &&      // active low: slot powered
               p4_xl_set(IO_SX1262_RST, true) &&
               p4_xl_set(IO_TOUCH_RST, true);
    // C6 power cycle (kept for full sequence parity; rung 2 does not talk
    // to the C6).
    ok = ok && p4_xl_set(IO_C6_EN, true);
    vTaskDelay(pdMS_TO_TICKS(100));
    ok = ok && p4_xl_set(IO_C6_EN, false);
    vTaskDelay(pdMS_TO_TICKS(100));
    ok = ok && p4_xl_set(IO_C6_EN, true);
    vTaskDelay(pdMS_TO_TICKS(100));
    if (!ok) {
        printf("[disp] FAIL: power sequence writes\n");
        return;
    }
    printf("[disp] rails cycled (VCCA low, 5V high, 3V3 low)\n");

    // DSI PHY power: channel 3 at 1830 mV on this board (2500 = dark
    // panel), then 100 ms.
    printf("[disp] stage: phy ldo (1830mV)\n");
    esp_ldo_channel_handle_t phy_ldo = NULL;
    esp_ldo_channel_config_t ldo_cfg = { .chan_id = 3, .voltage_mv = 1830 };
    if (esp_ldo_acquire_channel(&ldo_cfg, &phy_ldo) != ESP_OK) {
        printf("[disp] FAIL: ldo channel 3\n");
        return;
    }
    vTaskDelay(pdMS_TO_TICKS(100));

    // Screen reset: HIGH -> LOW -> HIGH, 200 ms per phase.
    printf("[disp] stage: panel reset\n");
    ok = p4_xl_set(IO_SCREEN_RST, true);
    vTaskDelay(pdMS_TO_TICKS(200));
    ok = ok && p4_xl_set(IO_SCREEN_RST, false);
    vTaskDelay(pdMS_TO_TICKS(200));
    ok = ok && p4_xl_set(IO_SCREEN_RST, true);
    vTaskDelay(pdMS_TO_TICKS(200));
    if (!ok) {
        printf("[disp] FAIL: reset writes\n");
        return;
    }

    // DSI bus + DBI command channel.
    printf("[disp] stage: dsi bus\n");
    esp_lcd_dsi_bus_handle_t dsi_bus = NULL;
    esp_lcd_dsi_bus_config_t dsi_cfg = {
        .bus_id = 0,
        .num_data_lanes = 2,
        .phy_clk_src = MIPI_DSI_PHY_CLK_SRC_DEFAULT,
        .lane_bit_rate_mbps = 1000,
    };
    if (esp_lcd_new_dsi_bus(&dsi_cfg, &dsi_bus) != ESP_OK) {
        printf("[disp] FAIL: dsi bus\n");
        return;
    }
    printf("[disp] stage: dbi io\n");
    esp_lcd_panel_io_handle_t dbi = NULL;
    esp_lcd_dbi_io_config_t dbi_cfg = {
        .virtual_channel = 0,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    if (esp_lcd_new_panel_io_dbi(dsi_bus, &dbi_cfg, &dbi) != ESP_OK) {
        printf("[disp] FAIL: dbi io\n");
        return;
    }

    // DPI video panel first, frame buffer blanked, so the stream is clean
    // pixels before the panel ever turns on.
    printf("[disp] stage: dpi panel\n");
    esp_lcd_panel_handle_t panel = NULL;
    esp_lcd_dpi_panel_config_t dpi_cfg = {
        .virtual_channel = 0,
        .dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT,
        .dpi_clock_freq_mhz = 60,
        .in_color_format = LCD_COLOR_FMT_RGB565,
        .num_fbs = 1,
        .video_timing = {
            .h_size = PANEL_W,
            .v_size = PANEL_H,
            .hsync_pulse_width = 50,
            .hsync_back_porch = 150,
            .hsync_front_porch = 50,
            .vsync_pulse_width = 40,
            .vsync_back_porch = 120,
            .vsync_front_porch = 80,
        },
        .flags.use_dma2d = true,
    };
    if (esp_lcd_new_panel_dpi(dsi_bus, &dpi_cfg, &panel) != ESP_OK) {
        printf("[disp] FAIL: dpi panel create\n");
        return;
    }
    if (esp_lcd_panel_init(panel) != ESP_OK) {
        printf("[disp] FAIL: dpi panel init\n");
        return;
    }
    void *fb = NULL;
    if (esp_lcd_dpi_panel_get_frame_buffer(panel, 1, &fb) != ESP_OK || !fb) {
        printf("[disp] FAIL: get frame buffer\n");
        return;
    }
    memset(fb, 0, (size_t)PANEL_W * PANEL_H * 2);
    esp_cache_msync(fb, (size_t)PANEL_W * PANEL_H * 2,
                    ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    printf("[disp] dpi streaming (blanked)\n");

    // Panel init commands over DBI, then brightness (the table parks it at
    // zero).
    printf("[disp] stage: init sequence\n");
    for (size_t i = 0; i < sizeof(k_rm69a10_init) / sizeof(dcs_cmd_t); i++) {
        const dcs_cmd_t *c = &k_rm69a10_init[i];
        if (esp_lcd_panel_io_tx_param(dbi, c->cmd,
                                      c->len ? c->data : NULL,
                                      c->len) != ESP_OK) {
            printf("[disp] FAIL: init cmd 0x%02X (index %u)\n", c->cmd,
                   (unsigned)i);
            return;
        }
        if (c->delay_ms) vTaskDelay(pdMS_TO_TICKS(c->delay_ms));
    }
    uint8_t bright = 0xFF;
    if (esp_lcd_panel_io_tx_param(dbi, 0x51, &bright, 1) != ESP_OK) {
        printf("[disp] FAIL: brightness\n");
        return;
    }
    printf("[disp] init sent, brightness 0xFF\n");

    // Eight vertical color bars into the live stream.
    static const uint16_t bars[8] = {
        0xF800, 0x07E0, 0x001F, 0xFFFF, 0xFFE0, 0x07FF, 0xF81F, 0x0000
    };
    uint16_t *px = (uint16_t *)fb;
    for (int y = 0; y < PANEL_H; y++) {
        for (int x = 0; x < PANEL_W; x++) {
            px[y * PANEL_W + x] = bars[(x * 8) / PANEL_W];
        }
    }
    esp_cache_msync(fb, (size_t)PANEL_W * PANEL_H * 2,
                    ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    printf("[disp] color bars up — 568x1232 @ 60MHz DPI\n");
}
