// Rung 8: WiFi through the C6. The P4 runs the esp-hosted host side
// (libraries/esp_hosted: 2.12.13 + the MESHPUNK reset-callback patch) and
// esp_wifi_remote, which turns the standard esp_wifi_* calls into RPCs
// executed by the C6's esp-hosted firmware (written by rung 7d).
//
// The C6 link is set explicitly before esp_hosted_init(): SDIO slot 1,
// 4-bit, CLK18 CMD19 D0-D3 14-17 (rung 7's wiring). esp-hosted resets the
// C6 during init through c6_reset_level(), which drives the C6 EN line
// (expander IO14); without the callback it would pulse its configured
// reset GPIO, 54 by default on the P4, which on this board goes to the
// 2x8 user header.
//
// Sequence: event loop and handlers → transport config + reset callback →
// esp_hosted_init() → esp_hosted_connect_to_slave() →
// ESP_HOSTED_EVENT_TRANSPORT_UP → C6 firmware version
// → netif + esp_wifi_init (WiFi settings in RAM only) → station start →
// one scan → connect with /sdcard/wifi.txt (line 1 SSID, line 2 password,
// omitted for an open network) → wait for an IP. Credentials stay on the
// SD card; the password is never printed.

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "esp_hosted.h"
#include "esp_hosted_transport_config.h"
#include "p4_shared.h"

#define WIFI_CREDS_PATH  "/sdcard/wifi.txt"
#define TRANSPORT_UP_MS  10000
#define SCAN_WAIT_MS     10000
#define CONNECT_WAIT_MS  20000
#define SCAN_PRINT_MAX   8

#define BIT_TRANSPORT_UP BIT0
#define BIT_SCAN_DONE    BIT1
#define BIT_GOT_IP       BIT2
#define BIT_DISCONNECTED BIT3

static EventGroupHandle_t s_ev;
static uint8_t s_disc_reason;

// esp-hosted's reset callback: `level` is the C6 EN level (high = run).
static esp_err_t c6_reset_level(void *arg, bool level)
{
    (void)arg;
    return p4_xl_set(IO_C6_EN, level) ? ESP_OK : ESP_FAIL;
}

static void on_event(void *arg, esp_event_base_t base, int32_t id,
                     void *data)
{
    (void)arg;
    if (base == ESP_HOSTED_EVENT && id == ESP_HOSTED_EVENT_TRANSPORT_UP) {
        xEventGroupSetBits(s_ev, BIT_TRANSPORT_UP);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_SCAN_DONE) {
        xEventGroupSetBits(s_ev, BIT_SCAN_DONE);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *d = data;
        s_disc_reason = d->reason;
        xEventGroupSetBits(s_ev, BIT_DISCONNECTED);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *g = data;
        printf("[wifi] got IP " IPSTR "  gw " IPSTR "  mask " IPSTR "\n",
               IP2STR(&g->ip_info.ip), IP2STR(&g->ip_info.gw),
               IP2STR(&g->ip_info.netmask));
        xEventGroupSetBits(s_ev, BIT_GOT_IP);
    }
}

// SSID from line 1, password from line 2 (empty when absent); trailing CR
// and LF removed. false = no file or an empty SSID.
static bool read_creds(char *ssid, size_t ssid_len, char *pass,
                       size_t pass_len)
{
    FILE *f = fopen(WIFI_CREDS_PATH, "r");
    if (!f) return false;
    pass[0] = 0;
    bool ok = fgets(ssid, ssid_len, f) != NULL;
    if (ok && !fgets(pass, pass_len, f)) pass[0] = 0;
    fclose(f);
    if (!ok) return false;
    ssid[strcspn(ssid, "\r\n")] = 0;
    pass[strcspn(pass, "\r\n")] = 0;
    return ssid[0] != 0;
}

static long long ms_since(int64_t t0)
{
    return (long long)((esp_timer_get_time() - t0) / 1000);
}

void rung8_wifi(void)
{
    printf("[wifi] rung 8: WiFi through the C6 (esp-hosted + "
           "esp_wifi_remote)\n");
    if (!g_p4_i2c0) {
        printf("[wifi] SKIP: no expander (rung 2 stopped early)\n");
        return;
    }

    s_ev = xEventGroupCreate();
    esp_err_t err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        printf("[wifi] FAIL: event loop: %s\n", esp_err_to_name(err));
        return;
    }
    esp_event_handler_register(ESP_HOSTED_EVENT, ESP_EVENT_ANY_ID, on_event,
                               NULL);
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL);

    printf("[wifi] stage: transport config + C6 reset callback\n");
    struct esp_hosted_sdio_config sdio = INIT_DEFAULT_HOST_SDIO_CONFIG();
    sdio.slot = 1;
    sdio.bus_width = 4;
    sdio.pin_clk.pin = 18;
    sdio.pin_cmd.pin = 19;
    sdio.pin_d0.pin = 14;
    sdio.pin_d1.pin = 15;
    sdio.pin_d2.pin = 16;
    sdio.pin_d3.pin = 17;
    esp_hosted_transport_err_t terr = esp_hosted_sdio_set_config(&sdio);
    if (terr != ESP_TRANSPORT_OK) {
        printf("[wifi] FAIL: sdio config: %s\n", esp_err_to_name(terr));
        return;
    }
    terr = esp_hosted_sdio_set_reset_callback(c6_reset_level, NULL);
    if (terr != ESP_TRANSPORT_OK) {
        printf("[wifi] FAIL: reset callback: %s\n", esp_err_to_name(terr));
        return;
    }
    printf("[wifi] sdio: slot %u, %u-bit, %lu kHz, rx mode %u, queues "
           "tx %u rx %u\n", sdio.slot, sdio.bus_width,
           (unsigned long)sdio.clock_freq_khz, sdio.rx_mode,
           sdio.tx_queue_size, sdio.rx_queue_size);

    printf("[wifi] stage: esp_hosted_init\n");
    int64_t t0 = esp_timer_get_time();
    int r = esp_hosted_init();
    if (r != ESP_OK) {
        printf("[wifi] FAIL: esp_hosted_init: %s\n", esp_err_to_name(r));
        return;
    }
    // esp_hosted_init() only starts the driver tasks; the C6 reset, card
    // init and handshake run in esp_hosted_connect_to_slave(), which
    // returns ESP_OK whatever happens, so the transport-up event decides.
    printf("[wifi] stage: connect to the C6\n");
    esp_hosted_connect_to_slave();
    EventBits_t bits = xEventGroupWaitBits(s_ev, BIT_TRANSPORT_UP, pdFALSE,
                                           pdFALSE,
                                           pdMS_TO_TICKS(TRANSPORT_UP_MS));
    if (!(bits & BIT_TRANSPORT_UP)) {
        printf("[wifi] FAIL: transport not up %d ms after init\n",
               TRANSPORT_UP_MS);
        return;
    }
    printf("[wifi] transport UP %lld ms after init\n", ms_since(t0));

    esp_hosted_coprocessor_fwver_t ver = { 0 };
    r = esp_hosted_get_coprocessor_fwversion(&ver);
    if (r == ESP_OK) {
        printf("[wifi] C6 esp-hosted firmware %lu.%lu.%lu\n",
               (unsigned long)ver.major1, (unsigned long)ver.minor1,
               (unsigned long)ver.patch1);
    } else {
        printf("[wifi] C6 firmware version: %s\n", esp_err_to_name(r));
    }

    printf("[wifi] stage: netif + esp_wifi_init\n");
    err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        printf("[wifi] FAIL: esp_netif_init: %s\n", esp_err_to_name(err));
        return;
    }
    if (!esp_netif_create_default_wifi_sta()) {
        printf("[wifi] FAIL: station netif\n");
        return;
    }
    wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
    wcfg.nvs_enable = false;
    err = esp_wifi_init(&wcfg);
    if (err == ESP_OK) err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (err == ESP_OK) err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err == ESP_OK) err = esp_wifi_start();
    if (err != ESP_OK) {
        printf("[wifi] FAIL: wifi init/start: %s\n", esp_err_to_name(err));
        return;
    }

    printf("[wifi] stage: scan\n");
    t0 = esp_timer_get_time();
    wifi_scan_config_t scan_cfg = { 0 };
    err = esp_wifi_scan_start(&scan_cfg, false);
    if (err != ESP_OK) {
        printf("[wifi] FAIL: scan start: %s\n", esp_err_to_name(err));
        return;
    }
    bits = xEventGroupWaitBits(s_ev, BIT_SCAN_DONE, pdFALSE, pdFALSE,
                               pdMS_TO_TICKS(SCAN_WAIT_MS));
    if (!(bits & BIT_SCAN_DONE)) {
        printf("[wifi] FAIL: no scan result %d ms after start\n",
               SCAN_WAIT_MS);
        return;
    }
    uint16_t found = 0;
    esp_wifi_scan_get_ap_num(&found);
    static wifi_ap_record_t recs[SCAN_PRINT_MAX];
    uint16_t shown = SCAN_PRINT_MAX;
    err = esp_wifi_scan_get_ap_records(&shown, recs);
    printf("[wifi] scan: %u networks in %lld ms%s\n", found, ms_since(t0),
           err == ESP_OK ? "" : " (records unreadable)");
    for (uint16_t i = 0; err == ESP_OK && i < shown; i++) {
        printf("[wifi]   %-32.32s ch %2u rssi %d\n",
               (const char *)recs[i].ssid, recs[i].primary, recs[i].rssi);
    }

    printf("[wifi] stage: connect\n");
    char ssid[33];
    char pass[65];
    if (!read_creds(ssid, sizeof(ssid), pass, sizeof(pass))) {
        printf("[wifi] no %s (line 1 SSID, line 2 password) — not "
               "connecting\n", WIFI_CREDS_PATH);
        return;
    }
    wifi_config_t cfg = { 0 };
    memcpy(cfg.sta.ssid, ssid, strnlen(ssid, sizeof(cfg.sta.ssid)));
    memcpy(cfg.sta.password, pass, strnlen(pass, sizeof(cfg.sta.password)));
    err = esp_wifi_set_config(WIFI_IF_STA, &cfg);
    if (err == ESP_OK) err = esp_wifi_connect();
    if (err != ESP_OK) {
        printf("[wifi] FAIL: connect: %s\n", esp_err_to_name(err));
        return;
    }
    t0 = esp_timer_get_time();
    bits = xEventGroupWaitBits(s_ev, BIT_GOT_IP | BIT_DISCONNECTED, pdFALSE,
                               pdFALSE, pdMS_TO_TICKS(CONNECT_WAIT_MS));
    if (bits & BIT_GOT_IP) {
        wifi_ap_record_t ap = { 0 };
        esp_wifi_sta_get_ap_info(&ap);
        printf("[wifi] verdict: WiFi UP through the C6 — '%.32s' rssi %d "
               "ch %u, IP %lld ms after connect\n", (const char *)ap.ssid,
               ap.rssi, ap.primary, ms_since(t0));
    } else if (bits & BIT_DISCONNECTED) {
        printf("[wifi] verdict: connect FAILED to '%s' — disconnect reason "
               "%u\n", ssid, s_disc_reason);
    } else {
        printf("[wifi] verdict: no IP %d ms after connect to '%s'\n",
               CONNECT_WAIT_MS, ssid);
    }
}
