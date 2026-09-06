#pragma once

// Notifications route through the host (bell log + melody + blink, the
// always-alive C path). Same enum values as the firmware's notify.h — the
// per-channel mode file format depends on them.

#include <stdint.h>
#include "../mc_internal.h"

enum NotifyChannelMode : uint8_t {
    NOTIFY_CHAN_OFF     = 0,
    NOTIFY_CHAN_MENTION = 1,
    NOTIFY_CHAN_ALL     = 2,
};

static inline void notify_post(const char* text) {
    if (MCH) MCH->notify_post(text);
}
static inline void notify_message_alert(void) {
    if (MCH) MCH->notify_message_alert();
}
