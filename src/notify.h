#pragma once

#include <stdint.h>
#include <stddef.h>

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

// ── Generic notification store ───────────────────────────────────────────────
// A RAM ring of single-string notification records, so the user can review
// what notified — including everything received while Lua is torn down for an
// ELF run. Each record is one pre-formatted line (the caller piles sender/
// channel/whatever into it); nothing here is mesh-specific, so any subsystem
// (USB, battery, apps...) can post. The ring is pre-allocated in PSRAM by
// notify_init() (boot, before the Lua arena) and guarded by its own mutex:
// writers may be any task (mesh task today), readers are the Core-0 Lua
// bindings. RAM-only by design — resets on reboot; full messages are already
// persisted in the message logs.

// Record `text` (truncated to the ring's slot size, stamped with our RTC
// clock) and fire the melody/blink alert. Callers gate — a post always both
// records and alerts (the sound/kbd user prefs gate delivery, not recording).
void notify_post(const char* text);

// Store accessors, all mutex-guarded. Index 0 = newest.
int      notify_log_count();
uint16_t notify_log_unseen();
bool     notify_log_get(int i, uint32_t* ts, char* buf, size_t buflen);
void     notify_log_seen();   // zero the unseen counter, keep the list
void     notify_log_clear();  // empty the ring
