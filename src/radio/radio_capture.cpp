#include "radio_capture.h"

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <string.h>

#include "../meshpunk_sync.h"   // SLog; MESH_LOCK is the callers' contract

extern uint32_t firmware_rtc_epoch();   // main.cpp; host clock authority

namespace rcap {

static PktCapture*   s_ring = nullptr;
static uint16_t      s_head = 0;       // next write slot
static uint16_t      s_count = 0;      // unread entries held
static uint32_t      s_seq = 0;        // next seq to hand out
static uint32_t      s_dropped = 0;    // overwritten-before-read, since last take
static volatile bool s_armed = false;

bool start() {
    if (s_ring) { s_armed = true; return true; }
    s_ring = (PktCapture*)heap_caps_malloc(sizeof(PktCapture) * PKT_CAP_RING_SIZE,
                                           MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_ring) {
        SLog.println("[PKTCAP] ERROR: ring alloc failed");
        return false;
    }
    s_head = s_count = 0;
    s_seq = 0;
    s_dropped = 0;
    s_armed = true;
    SLog.printf("[PKTCAP] capture on (%u entries, %u bytes)\n",
        (unsigned)PKT_CAP_RING_SIZE, (unsigned)(sizeof(PktCapture) * PKT_CAP_RING_SIZE));
    return true;
}

void stop() {
    s_armed = false;           // hooks bail before the free below
    if (s_ring) {
        heap_caps_free(s_ring);
        s_ring = nullptr;
        SLog.println("[PKTCAP] capture off");
    }
    s_head = s_count = 0;
}

bool armed() { return s_armed; }

PktCapture* push(uint8_t dir) {
    if (!s_armed || !s_ring) return nullptr;

    PktCapture* e = &s_ring[s_head];
    s_head = (s_head + 1) % PKT_CAP_RING_SIZE;
    if (s_count < PKT_CAP_RING_SIZE) {
        s_count++;
    } else {
        s_dropped++;           // just overwrote an unread entry
    }

    e->seq       = s_seq++;
    e->ts        = firmware_rtc_epoch();
    e->ms        = millis();
    e->snr_q4    = 0;
    e->rssi      = 0;
    e->score_q10 = -1;
    e->dir       = dir;
    e->parsed    = 0;
    e->len       = 0;
    memset(e->hash, 0, RCAP_HASH_SIZE);
    return e;
}

PktCapture* newest() {
    if (!s_armed || !s_ring || s_count == 0) return nullptr;
    return &s_ring[(s_head + PKT_CAP_RING_SIZE - 1) % PKT_CAP_RING_SIZE];
}

bool pop_oldest(PktCapture* out) {
    if (!s_ring || s_count == 0) return false;
    uint16_t tail = (s_head + PKT_CAP_RING_SIZE - s_count) % PKT_CAP_RING_SIZE;
    *out = s_ring[tail];
    s_count--;
    return true;
}

uint16_t count() { return s_count; }

uint32_t take_dropped() {
    uint32_t d = s_dropped;
    s_dropped = 0;
    return d;
}

void frame(uint8_t dir, const uint8_t* raw, int len, float snr, float rssi) {
    PktCapture* e = push(dir);
    if (!e || !raw || len <= 0) return;
    if (len > RCAP_FRAME_MAX) len = RCAP_FRAME_MAX;
    e->snr_q4 = (int16_t)(snr * 4.0f);
    e->rssi   = (int16_t)rssi;
    e->len    = (uint8_t)len;
    e->parsed = 1;
    memcpy(e->raw, raw, len);
}

}  // namespace rcap
