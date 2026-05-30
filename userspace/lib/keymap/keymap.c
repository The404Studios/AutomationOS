/**
 * keymap.c  --  Shared US-QWERTY layout + modifier-state engine.
 *
 * See keymap.h for the API contract. This file is the single source of truth
 * for scancode->ASCII translation across every AutomationOS userspace app and
 * the compositor, replacing the per-app "letters + digits only" tables.
 *
 * Implementation notes
 * --------------------
 *   - Two static lookup tables index by evdev keycode: g_base[] (unshifted)
 *     and g_shift[] (shifted). The shift table holds the upper-row symbols and
 *     uppercase letters. A 0 entry means "no printable glyph".
 *   - keymap_lookup() picks base vs shift purely from the `shift` argument.
 *   - keymap_resolve() derives the effective shift: for LETTERS it is
 *     (shift XOR caps_lock); for everything else it is shift alone. This is the
 *     standard PC behaviour -- caps-lock inverts letter case but leaves the
 *     number row and symbol keys untouched.
 *
 * Freestanding: no libc, no syscalls, integer-only. The arrays are read-only
 * 'static const' so they sit in .rodata (no runtime init, no relocations that
 * would matter for a -fno-pie static binary).
 */

#include "keymap.h"

/* Evdev keycodes we map. Kept local (mirror of kernel/include/input.h) so the
 * library has zero header dependencies beyond <stdint.h>. */
#define KC_ESC          1
#define KC_1            2
#define KC_2            3
#define KC_3            4
#define KC_4            5
#define KC_5            6
#define KC_6            7
#define KC_7            8
#define KC_8            9
#define KC_9            10
#define KC_0            11
#define KC_MINUS        12
#define KC_EQUAL        13
#define KC_BACKSPACE    14
#define KC_TAB          15
#define KC_Q            16
#define KC_W            17
#define KC_E            18
#define KC_R            19
#define KC_T            20
#define KC_Y            21
#define KC_U            22
#define KC_I            23
#define KC_O            24
#define KC_P            25
#define KC_LEFTBRACE    26
#define KC_RIGHTBRACE   27
#define KC_ENTER        28
#define KC_LEFTCTRL     29
#define KC_A            30
#define KC_S            31
#define KC_D            32
#define KC_F            33
#define KC_G            34
#define KC_H            35
#define KC_J            36
#define KC_K            37
#define KC_L            38
#define KC_SEMICOLON    39
#define KC_APOSTROPHE   40
#define KC_GRAVE        41
#define KC_LEFTSHIFT    42
#define KC_BACKSLASH    43
#define KC_Z            44
#define KC_X            45
#define KC_C            46
#define KC_V            47
#define KC_B            48
#define KC_N            49
#define KC_M            50
#define KC_COMMA        51
#define KC_DOT          52
#define KC_SLASH        53
#define KC_RIGHTSHIFT   54
#define KC_KPASTERISK   55
#define KC_LEFTALT      56
#define KC_SPACE        57
#define KC_CAPSLOCK     58

/* Highest keycode present in the tables; anything >= this is non-printing. */
#define KEYMAP_TABLE_SIZE 64

/* Unshifted US-QWERTY glyphs, indexed by evdev keycode. 0 == non-printing. */
static const char g_base[KEYMAP_TABLE_SIZE] = {
    [KC_1] = '1', [KC_2] = '2', [KC_3] = '3', [KC_4] = '4', [KC_5] = '5',
    [KC_6] = '6', [KC_7] = '7', [KC_8] = '8', [KC_9] = '9', [KC_0] = '0',
    [KC_MINUS] = '-', [KC_EQUAL] = '=',
    [KC_Q] = 'q', [KC_W] = 'w', [KC_E] = 'e', [KC_R] = 'r', [KC_T] = 't',
    [KC_Y] = 'y', [KC_U] = 'u', [KC_I] = 'i', [KC_O] = 'o', [KC_P] = 'p',
    [KC_LEFTBRACE] = '[', [KC_RIGHTBRACE] = ']',
    [KC_A] = 'a', [KC_S] = 's', [KC_D] = 'd', [KC_F] = 'f', [KC_G] = 'g',
    [KC_H] = 'h', [KC_J] = 'j', [KC_K] = 'k', [KC_L] = 'l',
    [KC_SEMICOLON] = ';', [KC_APOSTROPHE] = '\'', [KC_GRAVE] = '`',
    [KC_BACKSLASH] = '\\',
    [KC_Z] = 'z', [KC_X] = 'x', [KC_C] = 'c', [KC_V] = 'v', [KC_B] = 'b',
    [KC_N] = 'n', [KC_M] = 'm',
    [KC_COMMA] = ',', [KC_DOT] = '.', [KC_SLASH] = '/',
    [KC_KPASTERISK] = '*',
    [KC_SPACE] = ' ',
};

/* Shifted US-QWERTY glyphs, indexed by evdev keycode. 0 == non-printing.
 * Letters are uppercase; the number row and symbol keys carry their shifted
 * symbols (!@#$%^&*()_+ {} | : " < > ? ~ etc.). */
static const char g_shift[KEYMAP_TABLE_SIZE] = {
    [KC_1] = '!', [KC_2] = '@', [KC_3] = '#', [KC_4] = '$', [KC_5] = '%',
    [KC_6] = '^', [KC_7] = '&', [KC_8] = '*', [KC_9] = '(', [KC_0] = ')',
    [KC_MINUS] = '_', [KC_EQUAL] = '+',
    [KC_Q] = 'Q', [KC_W] = 'W', [KC_E] = 'E', [KC_R] = 'R', [KC_T] = 'T',
    [KC_Y] = 'Y', [KC_U] = 'U', [KC_I] = 'I', [KC_O] = 'O', [KC_P] = 'P',
    [KC_LEFTBRACE] = '{', [KC_RIGHTBRACE] = '}',
    [KC_A] = 'A', [KC_S] = 'S', [KC_D] = 'D', [KC_F] = 'F', [KC_G] = 'G',
    [KC_H] = 'H', [KC_J] = 'J', [KC_K] = 'K', [KC_L] = 'L',
    [KC_SEMICOLON] = ':', [KC_APOSTROPHE] = '"', [KC_GRAVE] = '~',
    [KC_BACKSLASH] = '|',
    [KC_Z] = 'Z', [KC_X] = 'X', [KC_C] = 'C', [KC_V] = 'V', [KC_B] = 'B',
    [KC_N] = 'N', [KC_M] = 'M',
    [KC_COMMA] = '<', [KC_DOT] = '>', [KC_SLASH] = '?',
    [KC_KPASTERISK] = '*',
    [KC_SPACE] = ' ',
};

/* True iff this keycode is an alphabetic key (case-affected by caps-lock). */
static int is_letter(uint8_t sc) {
    char b = (sc < KEYMAP_TABLE_SIZE) ? g_base[sc] : 0;
    return (b >= 'a' && b <= 'z');
}

void keymap_reset(keymap_state_t *st) {
    if (!st) return;
    st->shift_l = 0;
    st->shift_r = 0;
    st->caps_lock = 0;
    st->ctrl = 0;
    st->alt = 0;
}

int keymap_update(uint8_t scancode, int pressed, keymap_state_t *st) {
    if (!st) return 0;
    int down = (pressed != 0);
    switch (scancode) {
        case KC_LEFTSHIFT:
            st->shift_l = (uint8_t)down;
            return 1;
        case KC_RIGHTSHIFT:
            st->shift_r = (uint8_t)down;
            return 1;
        case KC_LEFTCTRL:
            st->ctrl = (uint8_t)down;
            return 1;
        case KC_LEFTALT:
            st->alt = (uint8_t)down;
            return 1;
        case KC_CAPSLOCK:
            /* Toggle on the DOWN edge only; ignore the key-up. */
            if (down) st->caps_lock = (uint8_t)(st->caps_lock ^ 1u);
            return 1;
        default:
            return 0;
    }
}

char keymap_lookup(uint8_t scancode, int shift) {
    if (scancode >= KEYMAP_TABLE_SIZE) return 0;
    return shift ? g_shift[scancode] : g_base[scancode];
}

char keymap_resolve(uint8_t scancode, int pressed, keymap_state_t *st) {
    /* Always fold the event into the modifier state, even modifiers/key-ups. */
    keymap_update(scancode, pressed, st);

    /* Only key-DOWN events of character keys produce a glyph. */
    if (!pressed) return 0;
    if (scancode >= KEYMAP_TABLE_SIZE) return 0;

    int shift = keymap_shift_down(st);
    int eff;
    if (is_letter(scancode)) {
        /* Letters: caps-lock XOR shift selects uppercase. */
        int caps = (st && st->caps_lock) ? 1 : 0;
        eff = shift ^ caps;
    } else {
        /* Digits/symbols/space: shift alone; caps-lock has no effect. */
        eff = shift;
    }
    return eff ? g_shift[scancode] : g_base[scancode];
}
