#pragma once

#include <stdint.h>

// ── C-side message notifications ─────────────────────────────────────────────
// The alert path (melody + keyboard blink) lives entirely in C so it keeps
// working while Lua is torn down and an ELF module owns Core 0: the trigger
// sites are the mesh RX handlers (mesh task, Core 1, which never stops), the
// melody is a C-owned tone object the Core-1 sound task mixes straight into
// I2S, and the blink state machine is ticked from the mesh task loop.

// Per-channel notification mode, stored by channel NAME (slots shift when
// channels are added/removed; the name is a channel's identity). A channel
// with no stored entry uses NOTIFY_CHAN_MENTION.
enum NotifyChannelMode : uint8_t {
    NOTIFY_CHAN_OFF     = 0,  // never alert
    NOTIFY_CHAN_MENTION = 1,  // alert on @[node_name] mention only (default)
    NOTIFY_CHAN_ALL     = 2,  // alert on every message
};

// Pre-render the notification melody. Call once in setup(), after sound_init().
void notify_init();

// Fire the DM/mention alert (melody + keyboard blink, each behind its user
// pref). Called from the mesh RX handlers on the mesh task; the blink state
// machine is single-task (armed here, stepped by notify_tick on that task).
void notify_message_alert();

// Step the keyboard-blink state machine. Called from mesh_task_body every loop.
void notify_tick();
