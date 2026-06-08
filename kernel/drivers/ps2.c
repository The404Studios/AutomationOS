#include "../include/x86_64.h"
#include "../include/types.h"
#include "../include/kernel.h"
#include "../include/input.h"
#include "../include/irq.h"
#include "../include/spinlock.h"
#include "../include/drivers.h"   // timer_get_ticks / timer_get_frequency (bounded waits)
#include "../include/sched.h"     // process_get_by_name, process types (Ctrl+Alt+R recovery)

/* Gate verbose init chatter. The PS/2 controller init prints ~20 diagnostic
 * lines on the happy path; each costs ~0.5ms on real serial hardware. Keep
 * runtime hotkey messages (Ctrl+Alt+Del, recovery) ungated. */
#ifdef BOOT_QUIET
#define PS2_LOG(...) ((void)0)
#else
#define PS2_LOG(...) kprintf(__VA_ARGS__)
#endif

// PS/2 controller ports
#define PS2_DATA_PORT    0x60
#define PS2_STATUS_PORT  0x64
#define PS2_COMMAND_PORT 0x64

// PS/2 controller status bits
#define PS2_STATUS_OUTPUT_FULL  0x01
#define PS2_STATUS_INPUT_FULL   0x02
#define PS2_STATUS_MOUSE_DATA   0x20  // Bit 5: mouse data available

// PS/2 controller commands
#define PS2_CMD_READ_CONFIG     0x20
#define PS2_CMD_WRITE_CONFIG    0x60
#define PS2_CMD_DISABLE_PORT2   0xA7
#define PS2_CMD_ENABLE_PORT2    0xA8
#define PS2_CMD_ENABLE_PORT1    0xAE
#define PS2_CMD_TEST_PORT1      0xAB
#define PS2_CMD_TEST_PORT2      0xA9
#define PS2_CMD_WRITE_PORT2     0xD4

// PS/2 controller self-test
#define PS2_CMD_SELF_TEST       0xAA
#define PS2_SELFTEST_PASS       0x55

// Keyboard commands
#define KB_CMD_SET_TYPEMATIC    0xF3  // Set typematic rate/delay
#define KB_CMD_ENABLE_SCAN      0xF4
#define KB_CMD_RESET            0xFF

// Mouse commands
#define MOUSE_CMD_SET_DEFAULTS  0xF6
#define MOUSE_CMD_ENABLE        0xF4
#define MOUSE_CMD_SET_SAMPLE    0xF3
#define MOUSE_CMD_GET_ID        0xF2
#define MOUSE_CMD_RESET         0xFF

// Keyboard buffer
#define KB_BUFFER_SIZE 256

static char keyboard_buffer[KB_BUFFER_SIZE];
static volatile uint32_t kb_read_pos = 0;
static volatile uint32_t kb_write_pos = 0;
static bool ps2_initialized = false;

// Scancode to ASCII translation table (US QWERTY, scancode set 1)
// 0 means no ASCII equivalent (control keys, etc.)
static const char scancode_to_ascii[128] = {
    0,    27,  '1', '2', '3', '4', '5', '6',  // 0x00-0x07
    '7', '8', '9', '0', '-', '=', '\b', '\t', // 0x08-0x0F
    'q', 'w', 'e', 'r', 't', 'y', 'u', 'i',   // 0x10-0x17
    'o', 'p', '[', ']', '\n', 0,   'a', 's',  // 0x18-0x1F
    'd', 'f', 'g', 'h', 'j', 'k', 'l', ';',   // 0x20-0x27
    '\'', '`', 0,   '\\', 'z', 'x', 'c', 'v', // 0x28-0x2F
    'b', 'n', 'm', ',', '.', '/', 0,   '*',   // 0x30-0x37
    0,   ' ', 0,   0,   0,   0,   0,   0,     // 0x38-0x3F (Alt, Space, Caps, F1-F5)
    0,   0,   0,   0,   0,   0,   0,   '7',   // 0x40-0x47 (F6-F10, Num Lock, Scroll, Keypad 7)
    '8', '9', '-', '4', '5', '6', '+', '1',   // 0x48-0x4F (Keypad)
    '2', '3', '0', '.', 0,   0,   0,   0,     // 0x50-0x57 (Keypad, F11, F12)
    0,   0,   0,   0,   0,   0,   0,   0      // 0x58-0x5F
};

// Shifted characters
static const char scancode_to_ascii_shift[128] = {
    0,    27,  '!', '@', '#', '$', '%', '^',  // 0x00-0x07
    '&', '*', '(', ')', '_', '+', '\b', '\t', // 0x08-0x0F
    'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I',   // 0x10-0x17
    'O', 'P', '{', '}', '\n', 0,   'A', 'S',  // 0x18-0x1F
    'D', 'F', 'G', 'H', 'J', 'K', 'L', ':',   // 0x20-0x27
    '"', '~', 0,   '|', 'Z', 'X', 'C', 'V',   // 0x28-0x2F
    'B', 'N', 'M', '<', '>', '?', 0,   '*',   // 0x30-0x37
    0,   ' ', 0,   0,   0,   0,   0,   0,     // 0x38-0x3F
    0,   0,   0,   0,   0,   0,   0,   '7',   // 0x40-0x47
    '8', '9', '-', '4', '5', '6', '+', '1',   // 0x48-0x4F
    '2', '3', '0', '.', 0,   0,   0,   0,     // 0x50-0x57
    0,   0,   0,   0,   0,   0,   0,   0      // 0x58-0x5F
};

// Shift state tracking
static bool shift_pressed = false;
static bool ctrl_pressed = false;
static bool alt_pressed = false;
static bool caps_lock = false;

// Mouse state
static bool mouse_initialized = false;
static int32_t mouse_x = 400;  // Default center position
static int32_t mouse_y = 300;
static uint8_t mouse_buttons = 0;
static uint8_t mouse_packet[3];
static uint8_t mouse_packet_index = 0;

// Input devices
static input_device_t* keyboard_device = NULL;
static input_device_t* mouse_device = NULL;

// ---------------------------------------------------------------------------
// Bounded 8042 waits — the PS/2 controller on real hardware (e.g. a ThinkPad
// T410's 8042) can be slow, quirky, or have no device attached. EVERY wait on
// the status register MUST be time-bounded so a non-responding controller can
// never hang the boot. We use a PIT tick deadline when the timer is running
// (pit_init() runs long before ps2_init()), and fall back to a bounded
// iteration cap when the timer frequency is not yet known. This mirrors the
// ahci_wait_until() pattern in kernel/drivers/storage/ahci.c.
// ---------------------------------------------------------------------------

// Per-wait timeout. Generous enough for a sluggish real 8042 (datasheet
// command turnaround is microseconds; device resets answer within ~100ms), but
// small enough that a dead device only costs a fraction of a second of boot.
#define PS2_TIMEOUT_MS 200

// Iteration cap used only when the PIT frequency is unavailable. Tuned to be
// in the same ballpark as the previous unconditional 100000-spin so the QEMU
// happy path is unchanged, scaled by the requested millisecond budget.
#define PS2_SPIN_PER_MS 100000UL

// Wait until (status & mask) == want, bounded by PS2_TIMEOUT_MS.
// Returns true if the condition was met, false on timeout (caller degrades).
static bool ps2_wait_status(uint8_t mask, uint8_t want) {
    uint32_t freq = timer_get_frequency();

    // The iteration cap ALWAYS applies as a hard backstop. ps2_init() runs
    // before sti(), so IRQ0 never fires and timer_get_ticks() is FROZEN -- a
    // pure wall-clock timeout could never elapse. On a real 8042 (slow, and with
    // the T410 BIOS's SMM/USB-legacy emulation intercepting ports 0x60/0x64) the
    // status bit may never reach `want`, so without an iteration bound this loop
    // hangs the boot. The wall-clock check is kept as a SECONDARY bound that is
    // only effective once IRQ0 is live and ticks are advancing. (Same dual-bound
    // discipline as the AHCI frozen-tick fix.)
    uint64_t iter_cap       = PS2_SPIN_PER_MS * PS2_TIMEOUT_MS;
    uint64_t start          = freq ? timer_get_ticks() : 0;
    uint64_t timeout_ticks  = freq ? (((uint64_t)PS2_TIMEOUT_MS * freq) / 1000) : 0;
    if (freq && timeout_ticks == 0) timeout_ticks = 1;  // sub-tick budget -> 1 tick

    for (uint64_t i = 0; i < iter_cap; i++) {
        if ((inb(PS2_STATUS_PORT) & mask) == want) {
            return true;
        }
        if (freq && (timer_get_ticks() - start) > timeout_ticks) {
            return false;   // wall-clock bound (only fires once IRQ0 is live)
        }
    }
    return false;           // iteration bound (always effective, incl. pre-sti)
}

// Wait for PS/2 controller to be ready for input (input buffer empty).
// Returns false on timeout so writers can give up instead of stalling.
static bool ps2_wait_input(void) {
    return ps2_wait_status(PS2_STATUS_INPUT_FULL, 0);
}

// Wait for PS/2 controller to have output available (output buffer full).
// Returns false on timeout so readers can report "no response".
static bool ps2_wait_output(void) {
    return ps2_wait_status(PS2_STATUS_OUTPUT_FULL, PS2_STATUS_OUTPUT_FULL);
}

// Sentinel returned by ps2_read_data_timeout() when no byte arrived before the
// timeout. 0xFF is never a valid ACK (0xFA) / self-test (0xAA / 0x55) /
// port-test (0x00) response, so callers that compare against those values treat
// it as failure even if they ignore *ok.
#define PS2_NO_DATA 0xFF

// Read a byte from the PS/2 data port, reporting whether a byte was actually
// available. *ok is set false on timeout (the returned value is then PS2_NO_DATA
// and must not be trusted as a device response). This is the ONLY data-read
// primitive in the driver: every read goes through this bounded path, so a
// missing/slow device can never block boot by spinning on the status register.
static uint8_t ps2_read_data_timeout(bool* ok) {
    if (!ps2_wait_output()) {
        if (ok) *ok = false;
        return PS2_NO_DATA;
    }
    if (ok) *ok = true;
    return inb(PS2_DATA_PORT);
}

// Write byte to PS/2 data port. Bounded wait; the write is skipped on timeout
// (a wedged controller that never empties its input buffer cannot block boot).
static void ps2_write_data(uint8_t data) {
    if (!ps2_wait_input()) {
        return;  // controller never became ready; give up (degraded)
    }
    outb(PS2_DATA_PORT, data);
}

// Write command to PS/2 command port. Same bounded-wait contract as above.
static void ps2_write_command(uint8_t cmd) {
    if (!ps2_wait_input()) {
        return;  // controller never became ready; give up (degraded)
    }
    outb(PS2_COMMAND_PORT, cmd);
}

// Write byte to PS/2 mouse (via second port)
static void ps2_write_mouse(uint8_t data) {
    ps2_write_command(PS2_CMD_WRITE_PORT2);
    ps2_write_data(data);
}

// Read a mouse response, reporting timeout via *ok (mirrors ps2_read_data_timeout).
// (The bare ps2_read_mouse() wrapper was removed: every mouse read now goes
// through the timeout-aware path so an absent device is detected, not blocked on.)
static uint8_t ps2_read_mouse_timeout(bool* ok) {
    return ps2_read_data_timeout(ok);
}

// Add character to keyboard buffer
// CRITICAL: Disable interrupts to prevent TOCTOU race condition.
// Use save/restore (not unconditional sti) because this is also called from the
// IRQ1 handler: a blind sti() would re-enable interrupts mid-handler before EOI,
// allowing IRQ nesting. Save/restore keeps interrupts off when the caller had
// them off and re-enables only when the caller (polled path) had them on.
static void kb_buffer_put(char c) {
    uint64_t flags = save_flags_cli();

    uint32_t next_pos = (kb_write_pos + 1) % KB_BUFFER_SIZE;
    if (next_pos != kb_read_pos) {
        keyboard_buffer[kb_write_pos] = c;
        kb_write_pos = next_pos;
    }

    restore_flags(flags);
}

// F11/F12 keycodes (scancodes 0x57/0x58). These are beyond the range defined
// in input.h (which stops at KEY_SCROLLLOCK=70), so define them here with
// their standard Linux evdev values.
#define KC_F11       87
#define KC_F12       88

// Scancode to Linux keycode mapping
static const uint16_t scancode_to_keycode[128] = {
    0, KEY_ESC, KEY_1, KEY_2, KEY_3, KEY_4, KEY_5, KEY_6,
    KEY_7, KEY_8, KEY_9, KEY_0, KEY_MINUS, KEY_EQUAL, KEY_BACKSPACE, KEY_TAB,
    KEY_Q, KEY_W, KEY_E, KEY_R, KEY_T, KEY_Y, KEY_U, KEY_I,
    KEY_O, KEY_P, KEY_LEFTBRACE, KEY_RIGHTBRACE, KEY_ENTER, KEY_LEFTCTRL, KEY_A, KEY_S,
    KEY_D, KEY_F, KEY_G, KEY_H, KEY_J, KEY_K, KEY_L, KEY_SEMICOLON,
    KEY_APOSTROPHE, KEY_GRAVE, KEY_LEFTSHIFT, KEY_BACKSLASH, KEY_Z, KEY_X, KEY_C, KEY_V,
    KEY_B, KEY_N, KEY_M, KEY_COMMA, KEY_DOT, KEY_SLASH, KEY_RIGHTSHIFT, KEY_KPASTERISK,
    KEY_LEFTALT, KEY_SPACE, KEY_CAPSLOCK, KEY_F1, KEY_F2, KEY_F3, KEY_F4, KEY_F5,
    KEY_F6, KEY_F7, KEY_F8, KEY_F9, KEY_F10, KEY_NUMLOCK, KEY_SCROLLLOCK, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, KC_F11,   // 0x57 = F11
    KC_F12, 0, 0, 0, 0, 0, 0, 0    // 0x58 = F12
};

// ---------------------------------------------------------------------------
// Extended (0xE0-prefixed) scancode -> evdev keycode mapping.
//
// In scancode set 1, the gray navigation cluster (arrows, Home/End/PgUp/PgDn,
// Ins/Del), the right-hand modifiers (R-Ctrl, R-Alt), keypad-Enter and
// keypad-'/' all share their low 7 bits with OTHER keys (e.g. extended 0x48 ==
// keypad-8, extended 0x4B == keypad-4). The controller distinguishes them by
// sending a 0xE0 lead byte FIRST. Without tracking that prefix the navigation
// keys were looked up in scancode_to_keycode[] as their non-extended twins:
// the up arrow (E0 48) collapsed onto plain 0x48, etc. -- the root cause of
// "dead arrow keys" (and R-Ctrl/R-Alt aliasing the left modifiers).
//
// The numeric values here are the standard Linux input-event-codes the rest of
// the system already speaks: the compositor forwards e->code verbatim and the
// IDE / editor / terminal switch on KEY_UP==103, KEY_DOWN==108, KEY_LEFT==105,
// KEY_RIGHT==106 (see userspace/apps/ide/ide.c, editor.c, terminal_m3.c). The
// kernel input.h only defines keycodes up to KEY_SCROLLLOCK(70), so the >70
// codes are spelled as literals with their evdev names in comments. 0 == "no
// keycode" (entry not an extended key we forward).
#define KC_KPENTER   96   // keypad Enter
#define KC_RIGHTCTRL 97
#define KC_KPSLASH   98   // keypad '/'
#define KC_RIGHTALT  100
#define KC_HOME      102
#define KC_UP        103
#define KC_PAGEUP    104
#define KC_LEFT      105
#define KC_RIGHT     106
#define KC_END       107
#define KC_DOWN      108
#define KC_PAGEDOWN  109
#define KC_INSERT    110
#define KC_DELETE    111
#define KC_LEFTMETA  125  // left Super/Windows
#define KC_RIGHTMETA 126  // right Super/Windows
#define KC_MENU      127  // application/menu key

// ThinkPad/multimedia extended scancodes (E0-prefixed).
// These Fn combos produce E0 xx on ThinkPad keyboards (T410, etc.).
// We map them to standard Linux evdev codes so userspace can act on them.
#define KC_MUTE          113
#define KC_VOLUMEDOWN    114
#define KC_VOLUMEUP      115
#define KC_NEXTSONG      163
#define KC_PREVIOUSSONG  165
#define KC_PLAYPAUSE     164
#define KC_BRIGHTNESSDOWN 224
#define KC_BRIGHTNESSUP   225
#define KC_SLEEP         142
#define KC_WLAN          238  // wireless toggle (Fn+F5 on ThinkPad)

static const uint16_t scancode_to_keycode_ext[128] = {
    [0x1C] = KC_KPENTER,   // keypad Enter
    [0x1D] = KC_RIGHTCTRL, // right Ctrl
    [0x35] = KC_KPSLASH,   // keypad '/'
    [0x38] = KC_RIGHTALT,  // right Alt (AltGr)
    [0x47] = KC_HOME,
    [0x48] = KC_UP,
    [0x49] = KC_PAGEUP,
    [0x4B] = KC_LEFT,
    [0x4D] = KC_RIGHT,
    [0x4F] = KC_END,
    [0x50] = KC_DOWN,
    [0x51] = KC_PAGEDOWN,
    [0x52] = KC_INSERT,
    [0x53] = KC_DELETE,
    [0x5B] = KC_LEFTMETA,
    [0x5C] = KC_RIGHTMETA,
    [0x5D] = KC_MENU,

    // ThinkPad / multimedia extended scancodes (Fn key combos).
    // These are safe: unknown scancodes map to 0 and are silently dropped.
    [0x20] = KC_MUTE,          // Fn+F1 (ThinkPad) or dedicated mute key
    [0x2E] = KC_VOLUMEDOWN,    // Fn+F2 (ThinkPad) or dedicated vol-
    [0x30] = KC_VOLUMEUP,      // Fn+F3 (ThinkPad) or dedicated vol+
    [0x22] = KC_PLAYPAUSE,     // media play/pause
    [0x19] = KC_NEXTSONG,      // media next
    [0x10] = KC_PREVIOUSSONG,  // media prev
    [0x5F] = KC_SLEEP,         // sleep key
    // Brightness (Fn+Home / Fn+End on some ThinkPads; E0 maps vary by model)
    // On the T410 these are typically ACPI events, not PS/2 scancodes, so they
    // won't arrive here. The entries are harmless and ready if they do.

    // everything else 0 (e.g. 0x2A / 0x36 fake-shift, already filtered below)
};

// ASCII for the handful of extended keys that genuinely produce a character on
// the legacy stdin path (ps2_getchar): keypad-Enter -> '\n', keypad-'/' -> '/'.
// The navigation keys (arrows, Home/End/PgUp/PgDn, Ins/Del) deliberately yield
// 0 here -- they have no ASCII and previously LEAKED keypad digits ('8','4',...)
// into stdin because the non-extended table was consulted. Returning 0 means
// "no character", which is exactly how a bare terminal behaves for a naked
// cursor key (the real char comes from an escape sequence the simple stdin
// consumers don't parse). This keeps the bug (digit injection) fixed without
// inventing bytes.
static char ext_scancode_to_ascii(uint8_t sc) {
    switch (sc) {
        case 0x1C: return '\n';  // keypad Enter
        case 0x35: return '/';   // keypad '/'
        default:   return 0;     // navigation / right-modifiers: no ASCII
    }
}

// ---------------------------------------------------------------------------
// Ctrl+Alt+Del — immediate system reboot (standard PC shortcut).
//
// Handled in the keyboard IRQ handler (ring 0) so it works even when the
// desktop is frozen.  Three reboot layers for maximum reliability:
//   1. Print a serial message so the event is logged.
//   2. Issue the 8042 keyboard controller reset command (0xFE to port 0x64).
//   3. Fallback: triple-fault via a null IDT — guarantees a CPU reset even
//      if the 8042 ignores the pulse.
// Delete = scancode 0x53 (non-extended, scancode set 1).
// ---------------------------------------------------------------------------
static void ps2_reboot_handler(void) {
    kprintf("\n[REBOOT] Ctrl+Alt+Del pressed — rebooting system\n");

    // Layer 1: 8042 keyboard controller system reset
    outb(0x64, 0xFE);

    // Layer 2: triple-fault via null IDT (reliable last resort)
    {
        struct { uint16_t limit; uint64_t base; } __attribute__((packed))
            zero_idt = {0, 0};
        __asm__ volatile("lidt %0" : : "m"(zero_idt));
        __asm__ volatile("int $3");   // #BP -> #DF -> reset
    }

    // Should never reach here
    while (1) { cli(); hlt(); }
}

// ---------------------------------------------------------------------------
// Ctrl+Alt+Backspace — kill the compositor ("zap X server").
//
// Kills ONLY the compositor process.  init (PID 1) detects the death via its
// waitpid loop and respawns sbin/compositor automatically, giving the user a
// fresh desktop.  Mirrors the first-press behaviour of Ctrl+Alt+R but on its
// own dedicated shortcut (the traditional X11 "kill server" chord).
// Backspace = scancode 0x0E (non-extended, scancode set 1).
//
// Runs entirely in IRQ context — no sleeping, no blocking locks. All
// functions called here (process_get_by_name, process_on_terminate,
// scheduler_remove_process, process_unref) use spin_lock_irqsave internally,
// which is safe from hard-IRQ context.
// ---------------------------------------------------------------------------
static void ps2_compositor_kill_handler(void) {
    process_t* compositor = process_get_by_name("compositor");
    if (!compositor) {
        kprintf("\n[RECOVERY] Ctrl+Alt+Backspace: no compositor process found\n");
        return;
    }

    if (compositor->state == PROCESS_TERMINATED) {
        kprintf("\n[RECOVERY] Ctrl+Alt+Backspace: compositor already terminated\n");
        process_unref(compositor);
        return;
    }

    kprintf("\n[RECOVERY] Ctrl+Alt+Backspace: killing compositor (PID %u), init will respawn\n",
            compositor->pid);

    // Mirror the SIGKILL path from kill.c — safe from IRQ context because every
    // function here uses spin_lock_irqsave internally.
    // 1. If blocked on a wait_object, force-unlink it first.
    {
        struct wait_object* wo = compositor->wait_on;
        if (compositor->state == PROCESS_BLOCKED && wo) {
            wait_object_abort(wo, compositor);
        }
    }
    // 2. Mark terminated and set conventional "killed by signal" exit status.
    compositor->state = PROCESS_TERMINATED;
    compositor->exit_status = 128 + 9;  // 128 + SIGKILL
    // 3. Wake the parent (init) so its waitpid loop notices the death and respawns.
    process_on_terminate(compositor);
    // 4. Remove from scheduler ready queue (if it was queued).
    scheduler_remove_process(compositor);
    // 5. Drop the ref taken by process_get_by_name.
    process_unref(compositor);
}

// ---------------------------------------------------------------------------
// Ctrl+Alt+R — system recovery shortcut (SysRq-like).
//
// Handled in the keyboard IRQ handler (ring 0) so it works even when the
// desktop compositor is frozen. Runs entirely in IRQ context — no sleeping,
// no blocking locks. All functions called here (process_get_by_name,
// process_on_terminate, scheduler_remove_process, process_unref) use
// spin_lock_irqsave internally, which is safe from hard-IRQ context.
//
//   First press:  kill the compositor process; init (PID 1) detects the death
//                 via its waitpid loop and respawns it automatically.
//   Second press within 3 seconds: hard reboot via the 8042 keyboard
//                 controller reset command (0xFE to port 0x64).
// ---------------------------------------------------------------------------
static uint64_t recovery_last_tick = 0;  // tick stamp of the last Ctrl+Alt+R

static void ps2_recovery_handler(void) {
    uint64_t now = timer_get_ticks();
    uint32_t freq = timer_get_frequency();

    // Double-press detection: if the last recovery keypress was < 3 seconds ago,
    // the user wants a full reboot.
    if (recovery_last_tick != 0 && freq != 0) {
        uint64_t elapsed_ticks = now - recovery_last_tick;
        uint64_t threshold = (uint64_t)freq * 3;  // 3 seconds worth of ticks
        if (elapsed_ticks < threshold) {
            kprintf("\n[RECOVERY] Ctrl+Alt+R double-press: REBOOTING\n");
            // Hard reboot via 8042 keyboard controller reset
            outb(0x64, 0xFE);
            // If that didn't work (some emulators ignore it), triple-fault
            while (1) { cli(); hlt(); }
        }
    }

    // First press (or >3s since last): kill the compositor.
    recovery_last_tick = now;

    process_t* compositor = process_get_by_name("compositor");
    if (!compositor) {
        kprintf("\n[RECOVERY] Ctrl+Alt+R: no compositor process found\n");
        return;
    }

    if (compositor->state == PROCESS_TERMINATED) {
        kprintf("\n[RECOVERY] Ctrl+Alt+R: compositor already terminated\n");
        process_unref(compositor);
        return;
    }

    kprintf("\n[RECOVERY] Ctrl+Alt+R: killing compositor (PID %u), init will respawn\n",
            compositor->pid);

    // Mirror the SIGKILL path from kill.c — safe from IRQ context because every
    // function here uses spin_lock_irqsave internally.
    // 1. If blocked on a wait_object, force-unlink it first.
    {
        struct wait_object* wo = compositor->wait_on;
        if (compositor->state == PROCESS_BLOCKED && wo) {
            wait_object_abort(wo, compositor);
        }
    }
    // 2. Mark terminated and set conventional "killed by signal" exit status.
    compositor->state = PROCESS_TERMINATED;
    compositor->exit_status = 128 + 9;  // 128 + SIGKILL
    // 3. Wake the parent (init) so its waitpid loop notices the death and respawns.
    process_on_terminate(compositor);
    // 4. Remove from scheduler ready queue (if it was queued).
    scheduler_remove_process(compositor);
    // 5. Drop the ref taken by process_get_by_name.
    process_unref(compositor);
}

// Handle keyboard scancode
static void ps2_handle_scancode(uint8_t scancode) {
    // --- 0xE1 (Pause/Break) prefix: a fixed 6-byte (E1 1D 45 E1 9D C5) burst.
    // It has no useful keycode and, left untracked, its body bytes (0x1D ==
    // Ctrl, 0x45 == NumLock, ...) would be misread as real key events. Swallow
    // the whole sequence. e1_remaining counts the bytes still to discard. ---
    static int  e1_remaining = 0;
    if (e1_remaining > 0) {
        e1_remaining--;
        return;
    }
    if (scancode == 0xE1) {
        e1_remaining = 5;  // discard the 5 bytes that follow the 0xE1 lead
        return;
    }

    // --- 0xE0 extended prefix: the NEXT byte is an extended scancode. Latch the
    // flag and wait for that byte; do not treat 0xE0 itself as a key. ---
    static bool saw_e0 = false;
    if (scancode == 0xE0) {
        saw_e0 = true;
        return;
    }

    bool extended = saw_e0;
    saw_e0 = false;  // consume the latch exactly once, for THIS byte only

    // Check for key release (high bit set)
    bool key_released = (scancode & 0x80) != 0;
    scancode &= 0x7F;  // Clear release bit

    if (extended) {
        // Fake-shift: with NumLock on, the BIOS brackets gray navigation keys
        // with a synthetic shift (E0 2A press / E0 AA release, also E0 36/B6).
        // These are not real shift presses -- ignore them entirely so they
        // neither toggle shift_pressed nor latch a stuck modifier.
        if (scancode == 0x2A || scancode == 0x36) {
            return;
        }

        // Right-hand modifiers (distinct from their left twins thanks to 0xE0).
        if (scancode == 0x1D) {  // Right Ctrl
            ctrl_pressed = !key_released;
        } else if (scancode == 0x38) {  // Right Alt (AltGr)
            alt_pressed = !key_released;
        }

        // Ctrl+Alt+Del — immediate reboot. The gray Delete key (navigation
        // cluster) sends E0 53, which is the code most users will press.
        if (!key_released && scancode == 0x53 && ctrl_pressed && alt_pressed) {
            ps2_reboot_handler();
            return;  // consume (never reached — handler reboots)
        }

        // Forward the extended keycode (arrows, Home/End/PgUp/PgDn, Ins/Del,
        // keypad-Enter, keypad-'/', R-Ctrl, R-Alt, Super, Menu).
        if (keyboard_device) {
            uint16_t keycode = scancode_to_keycode_ext[scancode];
            if (keycode != 0) {
                int32_t value = key_released ? KEY_STATE_RELEASED
                                             : KEY_STATE_PRESSED;
                input_report_key(keyboard_device, keycode, value);
                input_sync(keyboard_device);
            }
        }

        // Legacy stdin path: only on press, only for keys with real ASCII
        // (keypad-Enter / keypad-'/'); navigation keys yield 0 and are dropped.
        if (!key_released) {
            char ec = ext_scancode_to_ascii(scancode);
            if (ec != 0) {
                kb_buffer_put(ec);
            }
        }
        return;  // extended bytes never fall through to the non-extended path
    }

    // ===================== non-extended (common) path =====================
    // Update modifier state
    if (scancode == 0x2A || scancode == 0x36) {  // Left/Right Shift
        shift_pressed = !key_released;
    }
    if (scancode == 0x1D) {  // Ctrl
        ctrl_pressed = !key_released;
    }
    if (scancode == 0x38) {  // Alt
        alt_pressed = !key_released;
    }
    if (scancode == 0x3A && !key_released) {  // Caps Lock (toggle on press)
        caps_lock = !caps_lock;
    }

    // Ctrl+Alt+R — system recovery shortcut. Checked BEFORE the input subsystem
    // so the compositor (which may be frozen) never sees the keypress. Only fires
    // on key-DOWN (not release). R = scancode 0x13.
    if (!key_released && scancode == 0x13 && ctrl_pressed && alt_pressed) {
        ps2_recovery_handler();
        return;  // consume the keypress — do not forward to input/stdin
    }

    // Ctrl+Alt+Del — immediate system reboot (standard PC shortcut).
    // Delete = scancode 0x53 (non-extended, scancode set 1). Key-DOWN only.
    if (!key_released && scancode == 0x53 && ctrl_pressed && alt_pressed) {
        ps2_reboot_handler();
        return;  // consume (never reached — handler reboots)
    }

    // Ctrl+Alt+Backspace — kill compositor ("zap X server"). init respawns it.
    // Backspace = scancode 0x0E (non-extended, scancode set 1). Key-DOWN only.
    if (!key_released && scancode == 0x0E && ctrl_pressed && alt_pressed) {
        ps2_compositor_kill_handler();
        return;  // consume the keypress — do not forward to input/stdin
    }

    // Report to input subsystem
    if (keyboard_device && scancode < 128) {
        uint16_t keycode = scancode_to_keycode[scancode];
        if (keycode != 0) {
            int32_t value = key_released ? KEY_STATE_RELEASED : KEY_STATE_PRESSED;
            input_report_key(keyboard_device, keycode, value);
            input_sync(keyboard_device);
        }
    }

    // Legacy: also add to keyboard buffer for backward compatibility
    // Only process key press events (not releases) for regular keys
    if (key_released) {
        return;
    }

    // Translate scancode to ASCII
    char c;
    if (shift_pressed) {
        c = scancode_to_ascii_shift[scancode];
    } else {
        c = scancode_to_ascii[scancode];
    }

    // Apply caps lock to letters
    if (caps_lock && c >= 'a' && c <= 'z') {
        c = c - 'a' + 'A';
    } else if (caps_lock && c >= 'A' && c <= 'Z') {
        c = c - 'A' + 'a';
    }

    // Handle Ctrl+key combinations
    if (ctrl_pressed && c >= 'a' && c <= 'z') {
        c = c - 'a' + 1;  // Convert to control character
    } else if (ctrl_pressed && c >= 'A' && c <= 'Z') {
        c = c - 'A' + 1;
    }

    // Add to buffer if it's a valid character
    if (c != 0) {
        kb_buffer_put(c);
    }
}

// Handle mouse packet (3-byte PS/2 mouse protocol)
static void ps2_handle_mouse_packet(void) {
    if (mouse_packet_index < 3) return;

    // Parse mouse packet
    uint8_t flags = mouse_packet[0];
    int16_t dx = (int16_t)mouse_packet[1];
    int16_t dy = (int16_t)mouse_packet[2];

    // Check for overflow or invalid packet
    if (!(flags & 0x08)) {
        // Bit 3 should always be 1 in valid packets
        mouse_packet_index = 0;
        return;
    }

    // Handle sign extension for 9-bit values
    if (flags & 0x10) dx |= 0xFF00;  // X sign bit
    if (flags & 0x20) dy |= 0xFF00;  // Y sign bit

    // Invert Y axis (PS/2 mouse has inverted Y)
    dy = -dy;

    // Update button state
    uint8_t new_buttons = flags & 0x07;  // Bits 0-2: left, right, middle

    // Update cursor position
    mouse_x += dx;
    mouse_y += dy;

    // Clamp to screen bounds (assume 800x600 for now)
    if (mouse_x < 0) mouse_x = 0;
    if (mouse_x > 799) mouse_x = 799;
    if (mouse_y < 0) mouse_y = 0;
    if (mouse_y > 599) mouse_y = 599;

    // Report events via input system
    if (mouse_device) {
        // Report relative movement
        if (dx != 0) {
            input_report_rel(mouse_device, REL_X, dx);
        }
        if (dy != 0) {
            input_report_rel(mouse_device, REL_Y, dy);
        }

        // Report button changes
        if (new_buttons != mouse_buttons) {
            if ((new_buttons & 0x01) != (mouse_buttons & 0x01)) {
                input_report_key(mouse_device, BTN_LEFT, (new_buttons & 0x01) ? 1 : 0);
            }
            if ((new_buttons & 0x02) != (mouse_buttons & 0x02)) {
                input_report_key(mouse_device, BTN_RIGHT, (new_buttons & 0x02) ? 1 : 0);
            }
            if ((new_buttons & 0x04) != (mouse_buttons & 0x04)) {
                input_report_key(mouse_device, BTN_MIDDLE, (new_buttons & 0x04) ? 1 : 0);
            }
            mouse_buttons = new_buttons;
        }

        input_sync(mouse_device);
    }

    // Reset packet index for next packet
    mouse_packet_index = 0;
}

// Keyboard interrupt handler (IRQ1)
void ps2_keyboard_irq_handler(uint32_t irq, void* dev_id) {
    (void)irq;
    (void)dev_id;

    if (inb(PS2_STATUS_PORT) & PS2_STATUS_OUTPUT_FULL) {
        uint8_t scancode = inb(PS2_DATA_PORT);
        ps2_handle_scancode(scancode);
    }
}

// Simple IRQ handler wrapper (matches void (*handler)(void) signature)
void ps2_irq_handler(void) {
    if (inb(PS2_STATUS_PORT) & PS2_STATUS_OUTPUT_FULL) {
        uint8_t scancode = inb(PS2_DATA_PORT);
        ps2_handle_scancode(scancode);
    }
}

// Mouse interrupt handler (IRQ12)
void ps2_mouse_irq_handler(uint32_t irq, void* dev_id) {
    (void)irq;
    (void)dev_id;

    // Only consume the byte if it actually came from the mouse (bit 5 of the
    // status register). Otherwise we would steal keyboard bytes that arrive
    // on the shared controller.
    uint8_t status = inb(PS2_STATUS_PORT);
    if (!(status & PS2_STATUS_OUTPUT_FULL)) {
        return;
    }
    if (!(status & PS2_STATUS_MOUSE_DATA)) {
        return;
    }

    uint8_t data = inb(PS2_DATA_PORT);

    // Byte-0 sync guard: a valid PS/2 packet header always has bit 3 set. If we
    // are at the start of a packet and bit 3 is clear, this byte is a mid-stream
    // movement byte (the stream is offset) — discard it instead of storing it as
    // a header. Without this, a single mid-stream IRQ12 permanently offsets the
    // 3-byte boundary so every assembled "packet" fails the bit-3 check in
    // ps2_handle_mouse_packet() and is dropped -> cursor frozen, no clicks.
    if (mouse_packet_index == 0 && !(data & 0x08)) {
        return;
    }

    // Collect 3-byte packet
    if (mouse_packet_index < 3) {
        mouse_packet[mouse_packet_index++] = data;

        if (mouse_packet_index == 3) {
            ps2_handle_mouse_packet();
        }
    }
}

// Simple IRQ12 wrapper matching simple_irq_handler_t (void (*)(void)),
// which is what irq_register_handler() expects. Delegates to the real
// mouse interrupt handler above.
void ps2_mouse_irq_wrapper(void) {
    ps2_mouse_irq_handler(12, NULL);
}

// (Legacy duplicate removed - using the handler defined above)

// Initialize PS/2 mouse
static int ps2_mouse_init(void) {
    PS2_LOG("[PS/2] Initializing mouse...\n");

    // Enable second PS/2 port
    ps2_write_command(PS2_CMD_ENABLE_PORT2);

    // Every mouse read below is bounded (ps2_read_mouse_timeout). A real T410
    // Synaptics touchpad normally answers, but an absent/unresponsive device
    // (or a quirky 8042 second port) must SKIP cleanly — we return -1 and the
    // caller (ps2_init) continues with the mouse unavailable. The happy path is
    // unchanged: when the expected 0x00 / 0xFA bytes arrive promptly, behavior
    // matches the original code.
    bool m_ok;

    // Test second port
    ps2_write_command(PS2_CMD_TEST_PORT2);
    uint8_t test_result = ps2_read_data_timeout(&m_ok);
    if (!m_ok) {
        PS2_LOG("[PS/2] Mouse port test timed out (no device?); skipping mouse\n");
        return -1;
    }
    if (test_result != 0x00) {
        PS2_LOG("[PS/2] Mouse port test failed (0x%x)\n", test_result);
        return -1;
    }

    // Set mouse defaults
    ps2_write_mouse(MOUSE_CMD_SET_DEFAULTS);
    uint8_t response = ps2_read_mouse_timeout(&m_ok);
    if (!m_ok) {
        PS2_LOG("[PS/2] Mouse set defaults timed out (no ACK); skipping mouse\n");
        return -1;
    }
    if (response != 0xFA) {
        PS2_LOG("[PS/2] Mouse set defaults failed (0x%x)\n", response);
        return -1;
    }

    // Enable mouse data reporting
    ps2_write_mouse(MOUSE_CMD_ENABLE);
    response = ps2_read_mouse_timeout(&m_ok);
    if (!m_ok) {
        PS2_LOG("[PS/2] Mouse enable timed out (no ACK); skipping mouse\n");
        return -1;
    }
    if (response != 0xFA) {
        PS2_LOG("[PS/2] Mouse enable failed (0x%x)\n", response);
        return -1;
    }

    // Read controller configuration and enable mouse interrupts. If the config
    // read times out, fall back to the known-desired bits rather than writing a
    // garbage config byte back to the controller.
    ps2_write_command(PS2_CMD_READ_CONFIG);
    uint8_t config = ps2_read_data_timeout(&m_ok);
    if (!m_ok) {
        PS2_LOG("[PS/2] Mouse config read timed out; using defaults\n");
        config = 0;
    }
    config |= 0x02;  // Enable mouse interrupt (IRQ12)
    config &= ~0x20; // Enable mouse clock
    ps2_write_command(PS2_CMD_WRITE_CONFIG);
    ps2_write_data(config);

    mouse_initialized = true;
    mouse_packet_index = 0;

    PS2_LOG("[PS/2] Mouse initialized\n");
    return 0;
}

// Initialize PS/2 keyboard and mouse
void ps2_init(void) {
    PS2_LOG("[PS/2] Initializing PS/2 controller...\n");

    PS2_LOG("[PS/2] Pre-registering IRQ handlers early\n");
    // Note: irq_register_handler will unmask, but that's OK since
    // PIC was initialized with all IRQs masked (0xFF) in idt_init()

    // Disable both PS/2 ports
    ps2_write_command(0xAD);  // Disable port 1 (keyboard)
    ps2_write_command(PS2_CMD_DISABLE_PORT2);  // Disable port 2 (mouse)

    // Flush output buffer. BOUND the drain: a flaky 8042 can hold OBF asserted
    // (continuously re-presenting data), which would spin this loop forever on
    // real hardware. Cap the number of bytes we drain — anything still pending
    // after this is harmless (the IRQ path will consume it later).
    for (uint32_t flush = 0; flush < 256; flush++) {
        if (!(inb(PS2_STATUS_PORT) & PS2_STATUS_OUTPUT_FULL)) {
            break;
        }
        inb(PS2_DATA_PORT);
    }

    // ---- Controller self-test (command 0xAA) ----
    // The 8042 resets its internal state and responds with 0x55 on success.
    // On the T410, this clears any leftover USB-legacy emulation state that
    // the BIOS may have left in the controller. If the self-test fails or
    // times out, we continue degraded — the controller may still work.
    bool st_ok;
    ps2_write_command(PS2_CMD_SELF_TEST);
    uint8_t st_result = ps2_read_data_timeout(&st_ok);
    if (!st_ok) {
        PS2_LOG("[PS/2] Warning: controller self-test timed out; continuing\n");
    } else if (st_result != PS2_SELFTEST_PASS) {
        PS2_LOG("[PS/2] Warning: controller self-test failed (0x%x, expected 0x55)\n",
                st_result);
    } else {
        PS2_LOG("[PS/2] Controller self-test passed\n");
    }

    // NOTE: The controller self-test (0xAA) resets the controller and may
    // re-disable both ports. Re-disable them explicitly to be safe, then
    // flush any bytes the self-test may have generated.
    ps2_write_command(0xAD);  // Disable port 1 (keyboard)
    ps2_write_command(PS2_CMD_DISABLE_PORT2);  // Disable port 2 (mouse)
    for (uint32_t flush2 = 0; flush2 < 64; flush2++) {
        if (!(inb(PS2_STATUS_PORT) & PS2_STATUS_OUTPUT_FULL)) break;
        inb(PS2_DATA_PORT);
    }

    // Read controller configuration. If the controller never answers, fall back
    // to a safe default config rather than continuing with a garbage byte.
    bool cfg_ok;
    ps2_write_command(PS2_CMD_READ_CONFIG);
    uint8_t config = ps2_read_data_timeout(&cfg_ok);
    if (!cfg_ok) {
        PS2_LOG("[PS/2] Warning: controller config read timed out; using defaults\n");
        config = 0;  // start from a clean slate; we set the bits we need below
    }

    // Enable interrupts for both ports, ensure translation mode is ON.
    // Bit 6 (0x40) = scancode translation: the 8042 translates scancode set 2
    // (what the keyboard natively speaks) into set 1 (what this driver expects).
    // Most BIOSes leave this on, but on some T410 configurations it can be off
    // after the controller self-test resets the config byte.
    config |= 0x01;  // Enable keyboard interrupt (IRQ1)
    config |= 0x02;  // Enable mouse interrupt (IRQ12)
    config |= 0x40;  // Enable scancode translation (set 2 -> set 1)
    config &= ~0x10; // Enable keyboard clock
    config &= ~0x20; // Enable mouse clock

    // Write modified configuration
    ps2_write_command(PS2_CMD_WRITE_CONFIG);
    ps2_write_data(config);

    // Enable first PS/2 port (keyboard)
    ps2_write_command(PS2_CMD_ENABLE_PORT1);

    // Test keyboard port. Every read below is bounded (ps2_read_data_timeout):
    // an absent or unresponsive keyboard (real T410 with nothing on the port,
    // or a wedged 8042) is logged and SKIPPED — none of these steps can block
    // the boot. The happy path is unchanged: when the real ACK/self-test bytes
    // arrive promptly, behavior matches the original code exactly.
    bool kb_ok;
    ps2_write_command(PS2_CMD_TEST_PORT1);
    uint8_t test_result = ps2_read_data_timeout(&kb_ok);
    if (!kb_ok) {
        PS2_LOG("[PS/2] Warning: Keyboard port test timed out (no response)\n");
    } else if (test_result != 0x00) {
        PS2_LOG("[PS/2] Warning: Keyboard port test failed (0x%x)\n", test_result);
    }

    // Reset and enable keyboard
    ps2_write_data(KB_CMD_RESET);
    uint8_t reset_response = ps2_read_data_timeout(&kb_ok);
    if (!kb_ok) {
        PS2_LOG("[PS/2] Warning: Keyboard reset timed out (no ACK); continuing\n");
    } else if (reset_response != 0xFA) {  // 0xFA = ACK
        PS2_LOG("[PS/2] Warning: Keyboard reset failed (0x%x)\n", reset_response);
    }

    // Wait for self-test result (0xAA = passed). Only meaningful if the reset
    // ACK arrived; if reset already timed out, this read just times out too and
    // is skipped — still bounded, still non-blocking.
    uint8_t self_test = ps2_read_data_timeout(&kb_ok);
    if (!kb_ok) {
        PS2_LOG("[PS/2] Warning: Keyboard self-test timed out (no response)\n");
    } else if (self_test != 0xAA) {
        PS2_LOG("[PS/2] Warning: Keyboard self-test failed (0x%x)\n", self_test);
    }

    // Enable scanning
    ps2_write_data(KB_CMD_ENABLE_SCAN);
    uint8_t enable_response = ps2_read_data_timeout(&kb_ok);
    if (!kb_ok) {
        PS2_LOG("[PS/2] Warning: Enable scan timed out (no ACK); continuing\n");
    } else if (enable_response != 0xFA) {
        PS2_LOG("[PS/2] Warning: Enable scan failed (0x%x)\n", enable_response);
    }

    // ---- Set typematic rate/delay ----
    // On real hardware (T410), the BIOS default repeat rate can be sluggish
    // (e.g. 500ms delay, 10.9 cps). Set a responsive rate for desktop use.
    //   Command 0xF3 followed by a parameter byte:
    //     Bits 6-5: delay   (00=250ms, 01=500ms, 10=750ms, 11=1000ms)
    //     Bits 4-0: rate    (0x00=30.0cps fastest ... 0x1F=2.0cps slowest)
    //   0x00 = 250ms delay, 30.0 characters/sec — snappy desktop feel.
    ps2_write_data(KB_CMD_SET_TYPEMATIC);
    uint8_t typo_ack = ps2_read_data_timeout(&kb_ok);
    if (!kb_ok) {
        PS2_LOG("[PS/2] Warning: Set typematic timed out; using BIOS default\n");
    } else if (typo_ack != 0xFA) {
        PS2_LOG("[PS/2] Warning: Set typematic rejected (0x%x); using BIOS default\n",
                typo_ack);
    } else {
        // ACK received — now send the rate/delay parameter byte.
        ps2_write_data(0x00);  // 250ms delay, 30.0 cps
        uint8_t typo_ack2 = ps2_read_data_timeout(&kb_ok);
        if (!kb_ok || typo_ack2 != 0xFA) {
            PS2_LOG("[PS/2] Warning: Typematic rate parameter not acknowledged\n");
        } else {
            PS2_LOG("[PS/2] Typematic rate set: 250ms delay, 30.0 cps\n");
        }
    }

    // Register keyboard as input device
    keyboard_device = input_allocate_device("PS/2 Keyboard");
    if (keyboard_device) {
        keyboard_device->supports_key = true;
        keyboard_device->supports_rel = false;
        keyboard_device->supports_abs = false;
        keyboard_device->supports_led = true;
        input_register_device(keyboard_device);
        PS2_LOG("[PS/2] Registered keyboard as input device\n");
    }

    ps2_initialized = true;
    PS2_LOG("[PS/2] Keyboard initialized\n");

    // Register keyboard IRQ handler
    extern void irq_register_handler(uint8_t irq, void (*handler)(void));
    PS2_LOG("[PS/2] Registering IRQ 1 (keyboard)...\n");
    irq_register_handler(1, ps2_irq_handler);

    PS2_LOG("[PS/2] PS/2 keyboard ready\n");

    // ---- Mouse bring-up (second PS/2 port) ----
    if (ps2_mouse_init() == 0) {
        // Register mouse as an input device. Mice produce relative-motion
        // and button (key) events, so advertise REL + KEY capabilities.
        mouse_device = input_allocate_device("PS/2 Mouse");
        if (mouse_device) {
            mouse_device->supports_key = true;
            mouse_device->supports_rel = true;
            mouse_device->supports_abs = false;
            mouse_device->supports_led = false;
            input_register_device(mouse_device);
            PS2_LOG("[PS/2] Registered mouse as input device\n");
        }

        // Register mouse IRQ handler (IRQ12) via the void(void) wrapper.
        PS2_LOG("[PS/2] Registering IRQ 12 (mouse)...\n");
        irq_register_handler(12, ps2_mouse_irq_wrapper);
        PS2_LOG("[PS/2] PS/2 mouse ready\n");
    } else {
        PS2_LOG("[PS/2] Mouse not present or init failed; continuing\n");
    }
}

// Get a character from keyboard buffer (non-blocking)
// Returns 0 if no character available
// CRITICAL: Disable interrupts to prevent TOCTOU race condition
char ps2_getchar(void) {
    if (!ps2_initialized) {
        return 0;
    }

    // Save/restore the caller's interrupt state instead of unconditionally
    // re-enabling: ps2_getchar runs inside syscalls (IF=0 under SFMASK), so a
    // blind sti() would re-enable IRQs mid-syscall. We poll the controller port
    // directly, so no IRQ needs to be enabled during the read.
    uint64_t flags = save_flags_cli();

    // Check if buffer is empty
    if (kb_read_pos == kb_write_pos) {
        // Polled fallback: read the controller port directly.
        if (inb(PS2_STATUS_PORT) & PS2_STATUS_OUTPUT_FULL) {
            uint8_t scancode = inb(PS2_DATA_PORT);
            ps2_handle_scancode(scancode);
        }
        if (kb_read_pos == kb_write_pos) {
            restore_flags(flags);
            return 0;
        }
    }

    // Get character from buffer
    char c = keyboard_buffer[kb_read_pos];
    kb_read_pos = (kb_read_pos + 1) % KB_BUFFER_SIZE;

    restore_flags(flags);
    return c;
}

// Blocking version - wait for a character
char ps2_getchar_blocking(void) {
    char c;
    while ((c = ps2_getchar()) == 0) {
        // Wait for the next keyboard IRQ. Enable interrupts before halting —
        // hlt with IF=0 would hang forever (this is a blocking wait, so
        // enabling IRQs for the duration is the intended behavior).
        __asm__ volatile("sti; hlt" ::: "memory");
    }
    return c;
}

// Get mouse position
void ps2_get_mouse_position(int32_t* x, int32_t* y) {
    if (x) *x = mouse_x;
    if (y) *y = mouse_y;
}

// Get mouse button state
uint8_t ps2_get_mouse_buttons(void) {
    return mouse_buttons;
}

// Set mouse position (for initialization or boundary enforcement)
void ps2_set_mouse_position(int32_t x, int32_t y) {
    mouse_x = x;
    mouse_y = y;
}
