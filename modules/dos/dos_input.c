// Keyboard/mouse input for the tiny386 DOS module on T-Deck.
//
// Host key events -> PS/2 set-1 scancodes into the emulated 8042.
// ps2_put_keycode() takes a raw set-1 make code below 96 and adds the break
// bit itself; a value >= 0xE000 is sent as a real E0-prefixed extended key.
//
// The scancode table is carried over from the Faux86 PC-XT module
// (modules/pcxt/main_tdeck.cpp) -- XT set 1 and PS/2 set 1 are the same codes
// across this range -- with one upgrade: the navigation cluster now sends
// genuine E0 extended codes. Faux86 had no E0 support, so that module had to
// fake arrows/Home/End with the keypad scancodes and keep guest NumLock off.
//
// The launcher's -keymap string is applied host-side before we ever see a key
// (see parse_keymap_arg() in src/elf_host.cpp), so rebinding costs nothing here.

#include <stdint.h>
#include <stdbool.h>

#include "tiny386-src/i8042.h"

extern void host_trackball_read(int *dx, int *dy, int *click);
extern int  host_trackball_button(void);
extern int  host_key_mods(void);    // bit0 shift, bit1 alt, bit2 sym
extern void host_kb_blink(void);    // one keyboard-backlight blink
extern uint32_t host_get_ticks_us(void);

#define HOST_MOD_ALT 2

#define KEY_LSHIFT 0x2A

// Returns a set-1 make code (<0x60), or 0xE0xx for an extended key, or 0.
static int ascii_to_scan(unsigned char c)
{
    static const uint8_t letters[26] = {
        0x1E, 0x30, 0x2E, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17, 0x24, // a-j
        0x25, 0x26, 0x32, 0x31, 0x18, 0x19, 0x10, 0x13, 0x1F, 0x14, // k-t
        0x16, 0x2F, 0x11, 0x2D, 0x15, 0x2C                          // u-z
    };
    if (c >= 'a' && c <= 'z') return letters[c - 'a'];
    if (c >= 'A' && c <= 'Z') return letters[c - 'A'];
    if (c >= '1' && c <= '9') return 0x02 + (c - '1');

    switch (c) {
        case '0': return 0x0B;
        // Shifted glyphs map to their base key; the shift state is synthesized
        // separately by char_needs_shift().
        case '!': return 0x02; case '@': return 0x03; case '#': return 0x04;
        case '$': return 0x05; case '%': return 0x06; case '^': return 0x07;
        case '&': return 0x08; case '*': return 0x09; case '(': return 0x0A;
        case ')': return 0x0B;
        case '-': case '_': return 0x0C;
        case '=': case '+': return 0x0D;
        case '[': case '{': return 0x1A;
        case ']': case '}': return 0x1B;
        case ';': case ':': return 0x27;
        case '\'': case '"': return 0x28;
        case '`': case '~': return 0x29;
        case '\\': case '|': return 0x2B;
        case ',': case '<': return 0x33;
        case '.': case '>': return 0x34;
        case '/': case '?': return 0x35;
        // control keys
        case 0x0D: return 0x1C; // Enter
        case 0x08: return 0x0E; // Backspace (exit chord = Alt+Backspace 1.5s)
        case ' ':  return 0x39;
        case 0x09: case 0x99: return 0x0F; // Tab
        case 0x1B: return 0x01;            // Esc
        case 0x80: return KEY_LSHIFT;      // T-Deck/USB shift
        // Navigation: real E0 extended codes. 0x8x/0xAx are the firmware's
        // USB-keyboard pseudo-codes, 0x9x the launcher's binding outputs.
        case 0x81: case 0x91: return 0xE048; // Up
        case 0x82: case 0x92: return 0xE050; // Down
        case 0x83: case 0x93: return 0xE04B; // Left
        case 0x84: case 0x94: return 0xE04D; // Right
        case 0x86: return 0xE047;            // Home
        case 0x87: return 0xE04F;            // End
        case 0x88: return 0xE049;            // PgUp
        case 0x89: return 0xE051;            // PgDn
        case 0x8A: return 0xE052;            // Ins
        case 0x7F: case 0x98: return 0xE053; // Del
        case 0x8B: case 0x96: return 0x1D;   // Ctrl
        case 0x8C: case 0x97: return 0x38;   // Alt
        case 0x8E: return 0x3A;              // CapsLock (guest owns the state)
        case 0xA1: return 0x46;              // ScrollLock
        case 0xBA: return 0x57;              // F11
        case 0xBB: return 0x58;              // F12
        // Deliberately unmapped: 0x8D GUI, 0x8F NumLock, 0xA0 PrtSc,
        // 0xA2 Pause (no single set-1 code), 0xA3 Menu.
        default:
            if (c >= 0xB0 && c <= 0xB9) return 0x3B + (c - 0xB0); // F1-F10
            return 0;
    }
}

// On a PC these glyphs are Shift + a base key. The T-Deck's SYM layer hands us
// the finished glyph, so shift must be held around the base scancode or DOS
// decodes the unshifted character (! -> 1, : -> ;, uppercase -> lowercase).
static bool char_needs_shift(unsigned char c)
{
    if (c >= 'A' && c <= 'Z') return true;
    switch (c) {
        case '!': case '@': case '#': case '$': case '%':
        case '^': case '&': case '*': case '(': case ')':
        case '_': case '+': case '{': case '}': case '|':
        case ':': case '"': case '<': case '>': case '?':
        case '~':
            return true;
    }
    return false;
}

// Live modifier/button levels observed from the key event stream, consumed
// by the mouse poll below. Shift (0x80) is also forwarded to the guest as a
// normal shift key; 0x95 is the launcher's R.Mouse binding output and has no
// scancode -- it exists only as a mouse button.
static int s_shift_down = 0;
static int s_rmouse_down = 0;

void dos_input_key(PS2KbdState *kbd, unsigned char key, int pressed)
{
    if (key == 0x80) s_shift_down = pressed;
    if (key == 0x95) { s_rmouse_down = pressed; return; }

    int code = ascii_to_scan(key);
    if (!code) return;

    bool shift = char_needs_shift(key);
    if (pressed) {
        if (shift) ps2_put_keycode(kbd, 1, KEY_LSHIFT);
        ps2_put_keycode(kbd, 1, code);
    } else {
        ps2_put_keycode(kbd, 0, code);
        if (shift) ps2_put_keycode(kbd, 0, KEY_LSHIFT);
    }
}

// Trackball -> PS/2 mouse. Enabled by the launcher's -mouse N; without it the
// trackball arrives as key codes 0x81-0x84 and drives the arrow keys instead.
static int s_mouse_enabled = 0;
static int s_mouse_speed = 3;
static int s_latch_mode = 0;   // launcher "Click latch": click toggles held/up

void dos_input_mouse_config(int speed)
{
    s_mouse_enabled = (speed > 0);
    s_mouse_speed = (speed > 0) ? speed : 1;
}

void dos_input_latch_config(int on)
{
    s_latch_mode = on ? 1 : 0;
}

// The mouse button follows the REAL trackball button: the press starts on the
// ISR's press edge (host_trackball_read's click count) and ends when the
// physical level (host_trackball_button) releases -- so a tap is a click and
// keeping the ball pressed drags. A 40ms minimum keeps the quickest taps
// visible to software that polls the INT 33h button state between frames.
// Shift at the press edge selects the RIGHT button (bit 1) for that press.
// Latch mode instead toggles the button held/released on each click, for
// dragging without keeping the awkward ball-press held. It starts from the
// launcher's "Click latch" setting and ALT+CLICK flips it mid-game (alt is a
// matrix modifier the module never sees as a key -- host_key_mods() exposes
// the live level for exactly this kind of chord).
// The launcher's R.Mouse key binding (0x95) is an independent right-button
// level.
// If the level is never observed LOW during a press -- a tap shorter than the
// gap between polls, which on a stalled emulation frame can be 30ms+ -- the
// press falls back to a fixed pulse instead of depending on a read that
// cannot be trusted to land. So a tap always clicks, and a real hold still
// holds for as long as the level says it does.
// The click ISR triggers on every falling edge and a mechanical button
// bounces, so one physical press can arrive as several click counts. A press
// path shrugs that off (it just re-arms), but anything that TOGGLES on a
// click -- latch mode, and alt+click switching latch mode -- flips twice and
// lands back where it started. Hence one accepted edge per debounce window.
#define TRK_CLICK_DEBOUNCE_US 150000u
#define TRK_PRESS_MIN_US   40000u
#define TRK_PRESS_PULSE_US 120000u
static uint32_t s_last_click_us = 0;
static uint32_t s_press_start_us = 0;
static int s_pressed = 0;             // physical press in progress
static int s_level_seen = 0;          // level read LOW at least once this press
static int s_btn_mask = 1;            // button chosen at the press edge
static int s_latched = 0;             // latch-mode held-button mask (0 = up)
static int s_last_buttons = 0;        // last button state sent to the guest

void dos_input_poll_mouse(PS2MouseState *mouse)
{
    if (!s_mouse_enabled) return;
    int dx = 0, dy = 0, click = 0;
    host_trackball_read(&dx, &dy, &click);

    uint32_t now = host_get_ticks_us();
    if (click && (uint32_t)(now - s_last_click_us) < TRK_CLICK_DEBOUNCE_US)
        click = 0;                       // contact bounce, not a new click
    if (click) {
        s_last_click_us = now;
        if (host_key_mods() & HOST_MOD_ALT) {
            s_latch_mode = !s_latch_mode;        // alt+click: flip latch mode
            s_latched = 0;
            s_pressed = 0;
            host_kb_blink();                     // one blink = state changed
        } else if (s_latch_mode) {
            s_latched = s_latched ? 0 : (s_shift_down ? 2 : 1);
        } else {
            s_pressed = 1;
            s_press_start_us = now;
            s_level_seen = 0;
            s_btn_mask = s_shift_down ? 2 : 1;   // bit 0 = left, bit 1 = right
        }
    }
    if (s_pressed) {
        uint32_t held = (uint32_t)(now - s_press_start_us);
        if (host_trackball_button()) {
            s_level_seen = 1;                    // the press is observable
        } else if (s_level_seen) {
            if (held >= TRK_PRESS_MIN_US) s_pressed = 0;   // released
        } else if (held >= TRK_PRESS_PULSE_US) {
            s_pressed = 0;                       // never observed: pulse it
        }
    }

    int buttons = (s_rmouse_down ? 2 : 0) | s_latched
                | (s_pressed ? s_btn_mask : 0);

    // Emit on motion or on any button-state change -- a state change with no
    // motion still needs its packet.
    if (!dx && !dy && buttons == s_last_buttons) return;
    s_last_buttons = buttons;

    // PS/2 packets carry a signed 9-bit delta; split anything larger into
    // several events rather than clipping the motion.
    int mx = dx * s_mouse_speed;
    int my = dy * s_mouse_speed;
    do {
        int cx = mx; if (cx > 127) cx = 127; if (cx < -127) cx = -127;
        int cy = my; if (cy > 127) cy = 127; if (cy < -127) cy = -127;
        ps2_mouse_event(mouse, cx, cy, 0, buttons);
        mx -= cx; my -= cy;
    } while (mx || my);
}
