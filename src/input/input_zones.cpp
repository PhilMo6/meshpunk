// input_zones.cpp — touch-zone input layer (contract: input_zones.h).

#include <string.h>

#include "input_zones.h"

static InputZone s_zones[INPUT_ZONES_MAX];
static int  s_count = 0;
static bool s_enabled = false;

// Held set, one bit per zone index — a touch point per bit, so a d-pad
// direction and a face button coexist. Zone ORDER (not finger order) drives
// every read, so callers see a stable sequence across polls.
static uint8_t s_held_bits[(INPUT_ZONES_MAX + 7) / 8];
static volatile bool s_mode_pending = false;
static bool s_mode_zone_down = false;    // edge detect for the 0xFE zone
static volatile bool s_shot_pending = false;
static bool s_shot_zone_down = false;    // edge detect for the 0xFD zone

// Both reserved zones are edge-triggered, so every path that abandons the
// held set has to forget their press state too or the next touch reads as a
// continuation and never fires.
static inline void reserved_edges_reset(void) {
  s_mode_zone_down = false;
  s_shot_zone_down = false;
}

static inline bool bit_get(int i) {
  return (s_held_bits[i >> 3] >> (i & 7)) & 1;
}
static inline void bit_set(int i) {
  s_held_bits[i >> 3] |= (uint8_t)(1 << (i & 7));
}
static inline void bits_clear(void) {
  memset(s_held_bits, 0, sizeof(s_held_bits));
}

void input_zones_set(const InputZone* z, int n) {
  if (n < 0) n = 0;
  if (n > INPUT_ZONES_MAX) n = INPUT_ZONES_MAX;
  memcpy(s_zones, z, n * sizeof(InputZone));
  s_count = n;
  bits_clear();
  reserved_edges_reset();
}

void input_zones_clear(void) {
  s_count = 0;
  s_enabled = false;
  bits_clear();
  reserved_edges_reset();
}

void input_zones_enable(bool on) {
  s_enabled = on;
  if (!on) { bits_clear(); reserved_edges_reset(); }
}

bool input_zones_enabled(void) { return s_enabled && s_count > 0; }

static int zone_hit(int16_t x, int16_t y) {
  for (int i = 0; i < s_count; i++) {
    const InputZone* z = &s_zones[i];
    if (x >= z->x && x < z->x + z->w && y >= z->y && y < z->y + z->h) {
      return i;
    }
  }
  return -1;
}

bool input_zones_touch_multi(const int16_t* xs, const int16_t* ys, int n) {
  if (!input_zones_enabled()) {
    bits_clear();
    reserved_edges_reset();
    return false;
  }

  if (n <= 0 || !xs || !ys) {
    bits_clear();
    reserved_edges_reset();
    return true;   // controller mode owns the release too
  }

  // Rebuild the held set from scratch every poll: a finger that lifted or
  // slid out of its zone simply isn't in the new set, so releases need no
  // per-point tracking (and the panel reusing slot indices can't strand a
  // key down).
  bits_clear();
  bool mode_touched = false;
  bool shot_touched = false;
  for (int p = 0; p < n; p++) {
    int i = zone_hit(xs[p], ys[p]);
    if (i < 0) continue;                  // outside every zone: still consumed
    if (s_zones[i].out == INPUT_ZONE_MODE) {
      mode_touched = true;                // never held as a key
      continue;
    }
    if (s_zones[i].out == INPUT_ZONE_SHOT) {
      shot_touched = true;                // never held as a key
      continue;
    }
    bit_set(i);
  }

  // Both reserved zones fire once per touch-down, not once per poll.
  if (mode_touched) {
    if (!s_mode_zone_down) {
      s_mode_zone_down = true;
      s_mode_pending = true;
    }
  } else {
    s_mode_zone_down = false;
  }
  if (shot_touched) {
    if (!s_shot_zone_down) {
      s_shot_zone_down = true;
      s_shot_pending = true;
    }
  } else {
    s_shot_zone_down = false;
  }
  return true;
}

bool input_zones_touch(int16_t x, int16_t y, bool pressed) {
  if (!pressed) return input_zones_touch_multi(nullptr, nullptr, 0);
  return input_zones_touch_multi(&x, &y, 1);
}

uint8_t input_zones_held(void) {
  if (!input_zones_enabled()) return 0;
  for (int i = 0; i < s_count; i++)
    if (bit_get(i)) return s_zones[i].out;
  return 0;
}

int input_zones_held_all(uint8_t* outs, int max) {
  if (!outs || max <= 0 || !input_zones_enabled()) return 0;
  int n = 0;
  for (int i = 0; i < s_count && n < max; i++)
    if (bit_get(i)) outs[n++] = s_zones[i].out;
  return n;
}

bool input_zones_out_held(uint8_t out) {
  if (!out || !input_zones_enabled()) return false;
  for (int i = 0; i < s_count; i++)
    if (bit_get(i) && s_zones[i].out == out) return true;
  return false;
}

bool input_zones_mode_toggle_take(void) {
  if (!s_mode_pending) return false;
  s_mode_pending = false;
  return true;
}

bool input_zones_shot_take(void) {
  if (!s_shot_pending) return false;
  s_shot_pending = false;
  return true;
}
