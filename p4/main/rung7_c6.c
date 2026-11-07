// Rung 7a: C6 coprocessor SDIO enumeration probe (plan §6.15 WiFi rung,
// step 1). Protocol-neutral: brings up SDMMC slot 1 on the C6's SDIO
// pins and asks whether ANY SDIO function card answers — true for both
// firmwares the C6 can carry (LilyGo factory = ESP-AT v4.0.0.0 SDIO
// slave per the T-Display-P4 repo docs and examples; an esp-hosted
// slave if reflashed). This isolates power/reset/pin problems from the
// protocol-layer decision before any host stack is committed.
//
// C6 SDIO = slot 1 via GPIO matrix, CLK18 CMD19 D0-D3 14-17 (LilyGo's
// AT host example and the wadamesh config name the same pins); C6
// power/enable = expander IO14, power-cycled here so every boot probes a
// freshly started C6. The SD card lives on slot 0 (rung 4), so the two
// never collide.

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/sdmmc_host.h"
#include "sdmmc_cmd.h"
#include "sd_protocol_defs.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_serial_slave_link/essl.h"
#include "esp_serial_slave_link/essl_sdio.h"
#include "p4_shared.h"

static void print_pkt(const uint8_t *b, size_t len)
{
    printf("[c6] pkt %u bytes:", (unsigned)len);
    for (size_t i = 0; i < len && i < 48; i++) printf(" %02X", b[i]);
    printf("%s\n[c6] ascii: \"", len > 48 ? " ..." : "");
    for (size_t i = 0; i < len && i < 96; i++) {
        putchar((b[i] >= 0x20 && b[i] <= 0x7E) ? b[i]
                : (b[i] == '\r' || b[i] == '\n') ? '.' : '?');
    }
    printf("\"\n");
}

// Reads stay under 512 bytes: ESSL switches to CMD53 block mode at 512
// (essl_sdio.c), and block mode bypasses the driver's aligned bounce
// buffer, so a 512-byte read into this stack buffer fails its DMA check.
#define RX_MAX 511

// Reads every packet the slave has pending until quiet_ms passes with
// nothing new; returns whether any ASCII fragment contained `needle`.
// Read errors other than "no data" print once per distinct code.
static bool drain_packets(essl_handle_t h, uint32_t quiet_ms,
                          const char *needle)
{
    bool found = false;
    esp_err_t last_err = ESP_OK;
    int64_t last = esp_timer_get_time();
    while (esp_timer_get_time() - last < (int64_t)quiet_ms * 1000) {
        uint8_t buf[RX_MAX + 1];
        size_t got = 0;
        esp_err_t err = essl_get_packet(h, buf, RX_MAX, &got, 100);
        if ((err == ESP_OK || err == ESP_ERR_NOT_FINISHED) && got > 0) {
            print_pkt(buf, got);
            buf[got] = 0;
            if (needle && strstr((char *)buf, needle)) found = true;
            last = esp_timer_get_time();
        } else if (err != ESP_OK && err != ESP_ERR_NOT_FOUND &&
                   err != last_err) {
            printf("[c6] get_packet: %s\n", esp_err_to_name(err));
            last_err = err;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return found;
}

// Collects every packet until quiet_ms passes with nothing new, appending
// into acc (always NUL-terminated; bytes past cap-1 are counted in *total
// but not stored). Returns the stored length.
static size_t collect_packets(essl_handle_t h, uint32_t quiet_ms,
                              char *acc, size_t cap, size_t *total)
{
    size_t len = 0;
    *total = 0;
    esp_err_t last_err = ESP_OK;
    int64_t last = esp_timer_get_time();
    while (esp_timer_get_time() - last < (int64_t)quiet_ms * 1000) {
        uint8_t buf[RX_MAX];
        size_t got = 0;
        esp_err_t err = essl_get_packet(h, buf, RX_MAX, &got, 100);
        if ((err == ESP_OK || err == ESP_ERR_NOT_FINISHED) && got > 0) {
            size_t room = cap - 1 - len;
            size_t n = got < room ? got : room;
            memcpy(acc + len, buf, n);
            len += n;
            *total += got;
            last = esp_timer_get_time();
        } else if (err != ESP_OK && err != ESP_ERR_NOT_FOUND &&
                   err != last_err) {
            printf("[c6] get_packet: %s\n", esp_err_to_name(err));
            last_err = err;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    acc[len] = 0;
    return len;
}

// Prints the full line of `text` that contains position p.
static void print_line_at(const char *text, const char *p)
{
    const char *s = p;
    while (s > text && s[-1] != '\n' && s[-1] != '\r') s--;
    const char *e = p;
    while (*e && *e != '\r' && *e != '\n') e++;
    printf("%.*s", (int)(e - s), s);
}

// SDIO host + slot 1 for the C6 link (CLK18 CMD19 D0-D3 14-17, 4-bit),
// configured once and shared by rungs 7 and 7c. The host itself is
// already running when the SD card (slot 0, rung 4) mounted first.
bool p4_c6_sdio_init(void)
{
    static bool s_done;
    if (s_done) return true;
    esp_err_t err = sdmmc_host_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        printf("[c6] FAIL: sdmmc_host_init: %s\n", esp_err_to_name(err));
        return false;
    }
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.clk = GPIO_NUM_18;
    slot.cmd = GPIO_NUM_19;
    slot.d0 = GPIO_NUM_14;
    slot.d1 = GPIO_NUM_15;
    slot.d2 = GPIO_NUM_16;
    slot.d3 = GPIO_NUM_17;
    slot.width = 4;
    err = sdmmc_host_init_slot(SDMMC_HOST_SLOT_1, &slot);
    if (err != ESP_OK) {
        printf("[c6] FAIL: slot init: %s\n", esp_err_to_name(err));
        return false;
    }
    s_done = true;
    return true;
}

// Link timeline limits (diagnostic).
#define LINK_MAX_ENUMS   5
#define LINK_DEADLINE_MS 8000
#define LINK_STABLE_MS   500
#define CCCR_FN1         0x02   // function 1 bit in CCCR I/O-Enable/I/O-Ready

static long long ms_since(int64_t t0)
{
    return (long long)((esp_timer_get_time() - t0) / 1000);
}

void rung7_c6(void)
{
    printf("[c6] rung 7a: SDIO enumeration probe "
           "(slot 1, CLK18/CMD19/D0-3 14-17)\n");
    if (!g_p4_i2c0) {
        printf("[c6] SKIP: no expander (rung 2 stopped early)\n");
        return;
    }

    printf("[c6] stage: power cycle (IO14)\n");
    bool ok = p4_xl_set(IO_C6_EN, false);
    vTaskDelay(pdMS_TO_TICKS(100));
    ok = ok && p4_xl_set(IO_C6_EN, true);
    if (!ok) {
        printf("[c6] FAIL: expander write\n");
        return;
    }
    const int64_t t0 = esp_timer_get_time();
    vTaskDelay(pdMS_TO_TICKS(1000));

    printf("[c6] stage: sdio host + slot\n");
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = SDMMC_FREQ_PROBING;
    // Mandatory on this chip (IDF sdmmc_cmd.h: required when buffers sit
    // behind the cache, and P4 internal RAM sits behind the L1 cache).
    // Without it every CMD53 transfer from a buffer that fails the
    // cache-line alignment check — including ESSL's own register reads —
    // returns ESP_ERR_INVALID_ARG with no log line (sdmmc_io.c). With it,
    // sdmmc_card_init allocates a 512-byte DMA-aligned bounce buffer that
    // byte-mode transfers (under 512 bytes) route through.
    host.flags |= SDMMC_HOST_FLAG_ALLOC_ALIGNED_BUF;
    if (!p4_c6_sdio_init()) return;
    esp_err_t err = ESP_OK;

    // Link timeline (diagnostic). Espressif's SDIO AT guide: when host and
    // slave restart together, the host must wait until the slave's startup
    // is done before negotiating; it names no signal for that. Every step
    // prints its time since the C6 enable edge: enumeration, the CCCR
    // I/O-Enable/I/O-Ready reads, function-1 enable, and I/O-Ready polling
    // until the firmware's function-1 ready bit holds for LINK_STABLE_MS.
    // A failed CMD52 prints as a link drop and the card re-enumerates, at
    // most LINK_MAX_ENUMS times within LINK_DEADLINE_MS. sdmmc_card_init
    // allocates a 512-byte bounce buffer per call and nothing in the
    // driver frees it, so each re-enumeration frees the previous one.
    printf("[c6] stage: link timeline (times from C6 enable edge)\n");
    static sdmmc_card_t card;
    int enums = 0;
    bool stable = false;
    int64_t ready_at = 0;
    while (!stable && enums < LINK_MAX_ENUMS &&
           ms_since(t0) < LINK_DEADLINE_MS) {
        if (enums > 0) {
            heap_caps_free(card.host.dma_aligned_buffer);
            card.host.dma_aligned_buffer = NULL;
            vTaskDelay(pdMS_TO_TICKS(200));
        }
        enums++;
        err = sdmmc_card_init(&host, &card);
        printf("[c6] +%lldms enumerate #%d: %s\n", ms_since(t0), enums,
               esp_err_to_name(err));
        if (err != ESP_OK) continue;

        uint8_t ioe = 0;
        uint8_t ior = 0;
        err = sdmmc_io_read_byte(&card, 0, SD_IO_CCCR_FN_ENABLE, &ioe);
        if (err == ESP_OK) {
            err = sdmmc_io_read_byte(&card, 0, SD_IO_CCCR_FN_READY, &ior);
        }
        if (err != ESP_OK) {
            printf("[c6] +%lldms link DROPPED on CCCR read: %s\n",
                   ms_since(t0), esp_err_to_name(err));
            continue;
        }
        printf("[c6] +%lldms IOE=0x%02X IOR=0x%02X\n", ms_since(t0), ioe,
               ior);
        uint8_t ioe_out = 0;
        err = sdmmc_io_write_byte(&card, 0, SD_IO_CCCR_FN_ENABLE,
                                  ioe | CCCR_FN1, &ioe_out);
        if (err != ESP_OK) {
            printf("[c6] +%lldms link DROPPED on function-1 enable: %s\n",
                   ms_since(t0), esp_err_to_name(err));
            continue;
        }
        printf("[c6] +%lldms function 1 enabled (IOE=0x%02X)\n",
               ms_since(t0), ioe_out);

        uint8_t last_ior = ior;
        ready_at = 0;
        while (ms_since(t0) < LINK_DEADLINE_MS) {
            err = sdmmc_io_read_byte(&card, 0, SD_IO_CCCR_FN_READY, &ior);
            if (err != ESP_OK) {
                printf("[c6] +%lldms link DROPPED on I/O-Ready poll: %s\n",
                       ms_since(t0), esp_err_to_name(err));
                break;
            }
            if (ior != last_ior) {
                printf("[c6] +%lldms IOR 0x%02X -> 0x%02X\n", ms_since(t0),
                       last_ior, ior);
                last_ior = ior;
            }
            if (ior & CCCR_FN1) {
                if (ready_at == 0) ready_at = esp_timer_get_time();
                if (esp_timer_get_time() - ready_at >=
                    LINK_STABLE_MS * 1000LL) {
                    stable = true;
                    break;
                }
            } else {
                ready_at = 0;
            }
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }
    if (!stable) {
        printf("[c6] link NOT stable after %d enumeration(s), +%lldms — "
               "stop\n", enums, ms_since(t0));
        return;
    }
    printf("[c6] +%lldms link STABLE — function 1 ready since +%lldms, "
           "%d enumeration(s)\n", ms_since(t0),
           (long long)((ready_at - t0) / 1000), enums);
    sdmmc_card_print_info(stdout, &card);

    // Identity: both candidate firmwares (ESP-AT slave, esp-hosted
    // slave) ride the same Espressif SDIO slave link layer, so ESSL
    // comes up either way. An ESP-AT slave answers AT text; an
    // esp-hosted slave ignores AT strings but pushes its own boot
    // frames. Everything received prints raw, so whatever firmware this
    // production batch shipped names itself.
    printf("[c6] stage: essl link\n");
    essl_sdio_config_t ecfg = {
        .card = &card,
        .recv_buffer_size = 512,
    };
    essl_handle_t essl = NULL;
    err = essl_sdio_init_dev(&essl, &ecfg);
    if (err != ESP_OK || essl == NULL) {
        printf("[c6] FAIL: essl_sdio_init_dev: %s\n", esp_err_to_name(err));
        return;
    }
    err = essl_init(essl, 1000);
    printf("[c6] essl_init = %s\n", esp_err_to_name(err));
    if (err != ESP_OK) return;
    err = essl_wait_for_ready(essl, 2000);
    printf("[c6] essl_wait_for_ready = %s\n", esp_err_to_name(err));

    printf("[c6] stage: drain boot frames (1s)\n");
    bool banner = drain_packets(essl, 1000, "ready");
    printf("[c6] boot banner \"ready\": %s\n",
           banner ? "received" : "not received");

    printf("[c6] stage: identity — ATE0 then AT+GMR\n");
    bool sent = true;
    const char *ate0 = "ATE0\r\n";
    err = essl_send_packet(essl, ate0, strlen(ate0), 500);
    if (err != ESP_OK) {
        printf("[c6] send ATE0: %s\n", esp_err_to_name(err));
        sent = false;
    }
    drain_packets(essl, 700, NULL);
    const char *gmr = "AT+GMR\r\n";
    err = essl_send_packet(essl, gmr, strlen(gmr), 500);
    if (err != ESP_OK) {
        printf("[c6] send AT+GMR: %s\n", esp_err_to_name(err));
        sent = false;
    }
    bool at_ok = drain_packets(essl, 2500, "AT version");

    if (at_ok) {
        printf("[c6] verdict: ESP-AT firmware CONFIRMED — version text "
               "above\n");
    } else if (!sent) {
        printf("[c6] verdict: identity NOT tested — a send failed "
               "(error above)\n");
    } else {
        printf("[c6] verdict: no AT answer — if frames printed above, "
               "the slave speaks something else (esp-hosted era?); if "
               "nothing printed, the link is up but the firmware is "
               "unidentified\n");
    }
    if (!at_ok) return;

    // Command inventory: AT+CMD? lists every command compiled into this
    // build, one +CMD:<index>,"<name>",<test>,<query>,<set>,<execute>
    // line each. Read-only. The listing is collected rather than printed;
    // two sample lines (format check) and the lines for the commands below
    // print in full. AT+USEROTA is compiled in only when the build sets
    // CONFIG_AT_USER_COMMAND_SUPPORT (esp-at at_user_cmd.c).
    printf("[c6] stage: command inventory (AT+CMD?)\n");
    const size_t cap = 32 * 1024;
    char *acc = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM);
    if (!acc) {
        printf("[c6] FAIL: inventory buffer\n");
        return;
    }
    const char *cmdq = "AT+CMD?\r\n";
    err = essl_send_packet(essl, cmdq, strlen(cmdq), 500);
    if (err != ESP_OK) {
        printf("[c6] send AT+CMD?: %s\n", esp_err_to_name(err));
        heap_caps_free(acc);
        return;
    }
    size_t total = 0;
    size_t len = collect_packets(essl, 1500, acc, cap, &total);
    int count = 0;
    for (const char *p = strstr(acc, "+CMD:"); p; p = strstr(p + 5, "+CMD:")) {
        count++;
    }
    printf("[c6] inventory: %u bytes, %d +CMD lines%s\n", (unsigned)total,
           count, total > len ? " (TRUNCATED)" : "");
    if (count == 0) {
        printf("[c6] raw: \"%.200s\"\n", acc);
    }
    const char *sample = strstr(acc, "+CMD:");
    for (int i = 0; sample && i < 2; i++) {
        printf("[c6]   sample: ");
        print_line_at(acc, sample);
        printf("\n");
        sample = strstr(sample + 5, "+CMD:");
    }

    static const char *const k_names[] = {
        "+USEROTA", "+CIUPDATE", "+SYSROLLBACK", "+SYSFLASH", "+CWJAP",
        "+CIPMUX", "+CIPSTART", "+CIPSSLCCONF", "+CIPRECVMODE",
        "+CIPRECVDATA", "+HTTPCLIENT",
    };
    bool userota = false;
    for (size_t i = 0; i < sizeof(k_names) / sizeof(k_names[0]); i++) {
        char needle[24];
        snprintf(needle, sizeof(needle), "%s\"", k_names[i]);
        const char *hit = strstr(acc, needle);
        if (hit) {
            printf("[c6]   AT%-14s YES  ", k_names[i]);
            print_line_at(acc, hit);
            printf("\n");
            if (strcmp(k_names[i], "+USEROTA") == 0) userota = true;
        } else {
            printf("[c6]   AT%-14s no\n", k_names[i]);
        }
    }
    printf("[c6] AT+USEROTA in this build: %s\n", userota ? "YES" : "NO");
    heap_caps_free(acc);
}
