// Key state and host input translation. Key-state semantics are ported from
// pyxel-core input.rs (MIT, Takashi Kitao).

#include <string.h>

#include "px.h"

// Tracked keys map onto slots: ASCII keys use their own code, the rest below.
enum {
    SLOT_UP = 128, SLOT_DOWN, SLOT_LEFT, SLOT_RIGHT,
    SLOT_LSHIFT, SLOT_SHIFT, SLOT_LALT, SLOT_ALT, SLOT_LCTRL, SLOT_CTRL,
    SLOT_MOUSE_LEFT, SLOT_MOUSE_MIDDLE, SLOT_MOUSE_RIGHT,
    SLOT_GAMEPAD1,                                   // + PX_HOST_GAMEPAD_COUNT
    NUM_SLOTS = SLOT_GAMEPAD1 + PX_HOST_GAMEPAD_COUNT
};

enum { ST_NONE, ST_PRESSED, ST_RELEASED, ST_PRESSED_AND_RELEASED, ST_RELEASED_AND_PRESSED };

static uint32_t s_frame[NUM_SLOTS];
static uint8_t s_state[NUM_SLOTS];

// Keys pressed and text typed during the current frame (input_keys / input_text).
#define FRAME_KEYS_MAX 16
static uint32_t s_frame_keys[FRAME_KEYS_MAX];
static int s_frame_key_count;
static char s_frame_text[32];
static int s_frame_text_len;

static int s_mouse_button;          // last trackball button level in mouse mode

// Pixels of mouse travel per trackball tick, in game coordinates.
#define MOUSE_STEP 3

static int key_slot(uint32_t key) {
    if (key < 128) return (int)key;
    switch (key) {
    case PX_KEY_UP: return SLOT_UP;
    case PX_KEY_DOWN: return SLOT_DOWN;
    case PX_KEY_LEFT: return SLOT_LEFT;
    case PX_KEY_RIGHT: return SLOT_RIGHT;
    case PX_KEY_LSHIFT: return SLOT_LSHIFT;
    case PX_KEY_SHIFT: return SLOT_SHIFT;
    case PX_KEY_LALT: return SLOT_LALT;
    case PX_KEY_ALT: return SLOT_ALT;
    case PX_KEY_LCTRL: return SLOT_LCTRL;
    case PX_KEY_CTRL: return SLOT_CTRL;
    case PX_MOUSE_BUTTON_LEFT: return SLOT_MOUSE_LEFT;
    case PX_MOUSE_BUTTON_LEFT + 1: return SLOT_MOUSE_MIDDLE;
    case PX_MOUSE_BUTTON_RIGHT: return SLOT_MOUSE_RIGHT;
    default: break;
    }
    if (key >= PX_GAMEPAD1_BUTTON(0) && key < PX_GAMEPAD1_BUTTON(PX_HOST_GAMEPAD_COUNT))
        return SLOT_GAMEPAD1 + (int)(key - PX_GAMEPAD1_BUTTON(0));
    return -1;
}

void px_input_reset(void) {
    memset(s_frame, 0, sizeof(s_frame));
    memset(s_state, 0, sizeof(s_state));
    s_frame_key_count = 0;
    s_frame_text_len = 0;
    s_frame_text[0] = 0;
    s_mouse_button = 0;
}

static void press_key(uint32_t key) {
    int s = key_slot(key);
    if (s < 0) return;
    bool same_frame = s_state[s] != ST_NONE && s_frame[s] == px.frame_count &&
                      s_state[s] != ST_PRESSED;
    s_state[s] = same_frame ? ST_RELEASED_AND_PRESSED : ST_PRESSED;
    s_frame[s] = px.frame_count;
    if (key < PX_MOUSE_START && s_frame_key_count < FRAME_KEYS_MAX)
        s_frame_keys[s_frame_key_count++] = key;
}

static void release_key(uint32_t key) {
    int s = key_slot(key);
    if (s < 0) return;
    bool same_frame = s_state[s] != ST_NONE && s_frame[s] == px.frame_count &&
                      s_state[s] != ST_RELEASED;
    s_state[s] = same_frame ? ST_PRESSED_AND_RELEASED : ST_RELEASED;
    s_frame[s] = px.frame_count;
}

// Host key byte -> up to two Pyxel keys. Returns the count.
static int host_to_keys(unsigned char b, uint32_t out[2]) {
    if (b >= 'A' && b <= 'Z') { out[0] = (uint32_t)(b - 'A' + 'a'); return 1; }
    if (b >= 0x20 && b <= 0x7F) { out[0] = b; return 1; }
    switch (b) {
    case 0x0D: case 0x08: case 0x09: case 0x1B:
        out[0] = b; return 1;
    case 0x80: out[0] = PX_KEY_LSHIFT; out[1] = PX_KEY_SHIFT; return 2;
    case 0x81: out[0] = PX_KEY_UP; return 1;
    case 0x82: out[0] = PX_KEY_DOWN; return 1;
    case 0x83: out[0] = PX_KEY_LEFT; return 1;
    case 0x84: out[0] = PX_KEY_RIGHT; return 1;
    case 0x85: out[0] = PX_MOUSE_BUTTON_LEFT; return 1;
    case 0x8C: out[0] = PX_KEY_LALT; out[1] = PX_KEY_ALT; return 2;
    default: break;
    }
    if (b >= PX_HOST_GAMEPAD_BASE && b < PX_HOST_GAMEPAD_BASE + PX_HOST_GAMEPAD_COUNT) {
        out[0] = PX_GAMEPAD1_BUTTON(b - PX_HOST_GAMEPAD_BASE);
        return 1;
    }
    return 0;
}

void px_input_poll(void) {
    s_frame_key_count = 0;
    s_frame_text_len = 0;
    px.mouse_wheel = 0;

    int pressed;
    unsigned char b;
    while (host_get_key(&pressed, &b)) {
        uint32_t keys[2];
        int n = host_to_keys(b, keys);
        for (int i = 0; i < n; i++) {
            if (pressed) press_key(keys[i]);
            else release_key(keys[i]);
        }
        if (pressed && b >= 0x20 && b < 0x7F && s_frame_text_len < (int)sizeof(s_frame_text) - 1) {
            char c = (char)b;
            if (c >= 'a' && c <= 'z' && px_btn(PX_KEY_SHIFT)) c = (char)(c - 'a' + 'A');
            s_frame_text[s_frame_text_len++] = c;
        }
    }
    s_frame_text[s_frame_text_len] = 0;

    if (px.mouse_mode) {
        int dx = 0, dy = 0, click = 0;
        host_trackball_read(&dx, &dy, &click);
        px.mouse_x += dx * MOUSE_STEP;
        px.mouse_y += dy * MOUSE_STEP;
        if (px.mouse_x < 0) px.mouse_x = 0;
        if (px.mouse_y < 0) px.mouse_y = 0;
        if (px.mouse_x >= px.width) px.mouse_x = px.width - 1;
        if (px.mouse_y >= px.height) px.mouse_y = px.height - 1;
        int level = host_trackball_button();
        // A click shorter than one poll shows up only in the edge count.
        if (click > 0 && !level && !s_mouse_button) {
            press_key(PX_MOUSE_BUTTON_LEFT);
            release_key(PX_MOUSE_BUTTON_LEFT);
        } else if (level != s_mouse_button) {
            if (level) press_key(PX_MOUSE_BUTTON_LEFT);
            else release_key(PX_MOUSE_BUTTON_LEFT);
        }
        s_mouse_button = level;
    }
}

void px_input_enable_mouse(void) {
    if (px.mouse_mode) return;
    px.mouse_mode = true;
    int dx, dy, click;
    host_trackball_read(&dx, &dy, &click);     // opt in; discard ticks from before
    if (px.mouse_x == 0 && px.mouse_y == 0) {
        px.mouse_x = px.width / 2;
        px.mouse_y = px.height / 2;
    }
}

bool px_btn(uint32_t key) {
    int s = key_slot(key);
    if (s < 0) return false;
    switch (s_state[s]) {
    case ST_PRESSED:
    case ST_RELEASED_AND_PRESSED: return true;
    case ST_PRESSED_AND_RELEASED: return s_frame[s] == px.frame_count;
    default: return false;
    }
}

bool px_btnp(uint32_t key, int hold, int repeat) {
    int s = key_slot(key);
    if (s < 0) return false;
    if (s_state[s] == ST_NONE || s_state[s] == ST_RELEASED) return false;
    if (s_frame[s] == px.frame_count) return true;
    if (s_state[s] == ST_PRESSED_AND_RELEASED) return false;
    if (repeat <= 0) return false;
    int elapsed = (int)px.frame_count - (int)(s_frame[s] + (uint32_t)(hold > 0 ? hold : 0));
    return elapsed >= 0 && elapsed % repeat == 0;
}

bool px_btnr(uint32_t key) {
    int s = key_slot(key);
    if (s < 0) return false;
    if (s_state[s] == ST_NONE || s_state[s] == ST_PRESSED) return false;
    return s_frame[s] == px.frame_count;
}

int px_btnv(uint32_t key) {
    if (key == PX_MOUSE_POS_X) return px.mouse_x;
    if (key == PX_MOUSE_POS_Y) return px.mouse_y;
    if (key == PX_MOUSE_WHEEL_Y) return px.mouse_wheel;
    return 0;
}

void px_input_frame_text(char* out, size_t cap, uint32_t* keys, int* nkeys, int max_keys) {
    size_t n = (size_t)s_frame_text_len < cap - 1 ? (size_t)s_frame_text_len : cap - 1;
    memcpy(out, s_frame_text, n);
    out[n] = 0;
    int k = s_frame_key_count < max_keys ? s_frame_key_count : max_keys;
    memcpy(keys, s_frame_keys, sizeof(uint32_t) * (size_t)k);
    *nkeys = k;
}
