/**
 * keymap.h  --  Shared US-QWERTY keyboard layout + modifier state for AutomationOS
 *
 * Every GUI app (and the compositor) receives raw evdev-style key events as a
 * (keycode/scancode, pressed) pair. Historically each app carried its own tiny
 * "letters + digits, unshifted only" table, so there was no caps-lock and no
 * shifted symbols. This library centralises the full US-QWERTY translation:
 *
 *   - A modifier STATE struct (keymap_state_t) tracking the two shift keys, the
 *     caps-lock toggle, and ctrl / alt.
 *   - keymap_update()  -- fold one (scancode, pressed) event into that state.
 *   - keymap_resolve() -- update the state AND return the printable ASCII char
 *                         produced by a key-DOWN (0 for key-up, modifiers, and
 *                         non-printing keys).
 *
 * Layout rules (US-QWERTY):
 *   - Letters:        caps-lock XOR shift selects uppercase.
 *   - Digits/symbols: shift alone selects the shifted glyph (caps-lock has NO
 *                     effect on these, matching every real keyboard).
 *
 * The scancode values are the evdev/Linux keycodes the compositor forwards
 * (see kernel/include/input.h: KEY_LEFTSHIFT==42==0x2A, KEY_RIGHTSHIFT==54==
 * 0x36, KEY_CAPSLOCK==58==0x3A, KEY_LEFTCTRL==29==0x1D, KEY_LEFTALT==56==0x38).
 * These happen to coincide with the PS/2 set-1 make codes for the same keys.
 *
 * Freestanding-safe: pure C11, integer-only, no libc / libm / syscalls. The
 * only dependency is fixed-width integer types from <stdint.h> (supplied by
 * the freestanding compiler). Builds clean under the userspace flags:
 *   -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector
 *   -fno-pic -fno-pie -mno-red-zone -mstackrealign -O2
 */

#ifndef KEYMAP_H
#define KEYMAP_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------- *
 *  Modifier scancodes (evdev keycodes; mirror of kernel/include/input.h).
 *  Provided here so callers/the library never need a kernel header.
 * ------------------------------------------------------------------------- */
#define KEYMAP_SC_LEFTSHIFT   0x2A   /* 42  KEY_LEFTSHIFT  */
#define KEYMAP_SC_RIGHTSHIFT  0x36   /* 54  KEY_RIGHTSHIFT */
#define KEYMAP_SC_CAPSLOCK    0x3A   /* 58  KEY_CAPSLOCK   */
#define KEYMAP_SC_LEFTCTRL    0x1D   /* 29  KEY_LEFTCTRL   */
#define KEYMAP_SC_LEFTALT     0x38   /* 56  KEY_LEFTALT    */

/* ------------------------------------------------------------------------- *
 *  Modifier state. Zero-initialise (e.g. `keymap_state_t st = {0};`) for the
 *  natural power-on state: no keys held, caps-lock off.
 * ------------------------------------------------------------------------- */
typedef struct keymap_state {
    uint8_t shift_l;    /* left shift currently held down  (0/1) */
    uint8_t shift_r;    /* right shift currently held down (0/1) */
    uint8_t caps_lock;  /* caps-lock TOGGLE state          (0/1) */
    uint8_t ctrl;       /* either ctrl currently held down (0/1) */
    uint8_t alt;        /* either alt currently held down  (0/1) */
} keymap_state_t;

/* Convenience predicates (header-only, no link dependency). */
static inline int keymap_shift_down(const keymap_state_t *st) {
    return st && (st->shift_l || st->shift_r);
}
static inline int keymap_ctrl_down(const keymap_state_t *st) {
    return st && st->ctrl;
}
static inline int keymap_alt_down(const keymap_state_t *st) {
    return st && st->alt;
}

/* Reset every modifier (including the caps-lock toggle) to the off state. */
void keymap_reset(keymap_state_t *st);

/**
 * Fold one key event into the modifier state.
 *
 *   scancode -- evdev keycode of the key.
 *   pressed  -- non-zero for key-DOWN, zero for key-UP.
 *   st       -- state to update (NULL is a safe no-op).
 *
 * Shift/ctrl/alt track the physical held state (set on down, clear on up).
 * Caps-lock TOGGLES on each key-DOWN and ignores key-UP. Returns non-zero iff
 * `scancode` was a modifier key (so callers can decide whether to swallow it).
 */
int keymap_update(uint8_t scancode, int pressed, keymap_state_t *st);

/**
 * Resolve a key event to a printable ASCII character.
 *
 * This ALSO updates *st (it calls keymap_update internally), so a caller can
 * drive everything through this single entry point. It returns:
 *   - the printable ASCII glyph for a key-DOWN of a character key, honouring
 *     the current shift / caps-lock state;
 *   - 0 for key-UP events, modifier keys, and any key with no printable glyph
 *     (Enter, Backspace, Tab, arrows, function keys, etc.).
 *
 * Caps-lock affects ONLY letters (caps XOR shift); digits and symbols use
 * shift alone.
 */
char keymap_resolve(uint8_t scancode, int pressed, keymap_state_t *st);

/**
 * Pure lookup: the ASCII glyph a character key produces for an explicit
 * `shift` flag, with NO state and NO caps-lock handling. Useful for callers
 * that already track their own shift bit (e.g. the legacy terminal path).
 * Returns 0 for non-character keys. Letters return lower/upper per `shift`.
 */
char keymap_lookup(uint8_t scancode, int shift);

#ifdef __cplusplus
}
#endif

#endif /* KEYMAP_H */
