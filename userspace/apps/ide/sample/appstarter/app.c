/*
 * userspace/apps/ide/sample/appstarter/app.c
 * ===========================================
 * GUI APPLICATION STARTER TEMPLATE
 * AutomationOS ring-3 windowed app -- freestanding, no libc.
 *
 * What this template demonstrates (in ~300 well-commented lines):
 *
 *   1. Creating a window with a coloured title bar.
 *   2. Two clickable buttons: [+1] increments a counter, [Reset] resets it.
 *   3. A live counter label updated from the button callbacks.
 *   4. A single-line text input box (type here -> echoed below it).
 *   5. A label that mirrors whatever the user typed.
 *   6. A per-frame tick callback that shows elapsed seconds.
 *   7. A [Quit] button that calls SYS_EXIT (syscall 0).
 *
 * HOW TO ADAPT THIS TEMPLATE
 *   - Replace on_increment / on_reset with your own logic.
 *   - Add more ui_button(), ui_label(), ui_panel() widgets.
 *   - Use ui_app_set_tick() for animations / live data polling.
 *   - Keep all mutable state in file-static structs so pointers
 *     into them remain valid after _start() enters ui_app_run().
 *
 * WINDOW SIZE: 360 x 280 (easy to change -- just update WIN_W / WIN_H).
 *
 * BUILD (all flags on the command line -- no shell variables):
 *
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin \
 *       -fno-stack-protector -fno-pic -fno-pie -mno-red-zone -O2 \
 *       -I userspace \
 *       -c userspace/apps/ide/sample/appstarter/app.c -o /tmp/app.o
 *
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin \
 *       -fno-stack-protector -fno-pic -fno-pie -mno-red-zone -O2 \
 *       -I userspace -c userspace/lib/ui/ui.c     -o /tmp/ui.o
 *
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin \
 *       -fno-stack-protector -fno-pic -fno-pie -mno-red-zone -O2 \
 *       -I userspace -c userspace/lib/wl/wl_client.c -o /tmp/wlc.o
 *
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin \
 *       -fno-stack-protector -fno-pic -fno-pie -mno-red-zone -O2 \
 *       -I userspace -c userspace/lib/font/bitfont.c -o /tmp/bf.o
 *
 *   ld -nostdlib -static -n -no-pie -e _start \
 *       -T userspace/userspace.ld \
 *       /tmp/app.o /tmp/ui.o /tmp/wlc.o /tmp/bf.o -o /tmp/app.elf
 *
 *   # Verify no stack-canary symbol (must produce NO output):
 *   objdump -d /tmp/app.elf | grep fs:0x28
 */

/* ---- UI toolkit (panels, labels, buttons, textbox, tick loop) ---- */
#include "../../../../lib/ui/ui.h"

/* ==================================================================
 * SYSCALL NUMBERS  (from kernel/include/syscall.h)
 * AOS syscall numbers are NOT Linux numbers.  The three you need
 * most in a simple app are:
 *   SYS_WRITE        3  -- write bytes to fd (serial debug: fd=1)
 *   SYS_GET_TICKS_MS 40 -- monotonic ms since boot
 *   SYS_EXIT         0  -- terminate this process
 * ================================================================== */
#define SYS_WRITE        3
#define SYS_GET_TICKS_MS 40
#define SYS_EXIT         0

/* ==================================================================
 * MINIMAL INLINE SYSCALL HELPER
 * rax=syscall number, rdi/rsi/rdx = first three arguments.
 * ui.c uses the same 3-arg form internally, so this is the idiomatic
 * freestanding pattern for simple apps.
 * ================================================================== */
static inline long sc3(long nr, long a1, long a2, long a3)
{
    long ret;
    asm volatile("syscall"
                 : "=a"(ret)
                 : "a"(nr), "D"(a1), "S"(a2), "d"(a3)
                 : "rcx", "r11", "memory");
    return ret;
}

/* ==================================================================
 * FREESTANDING STRING HELPERS
 * No libc, so we bring our own tiny helpers.
 * ================================================================== */

/* Return the length of a NUL-terminated string. */
static unsigned long app_strlen(const char *s)
{
    unsigned long n = 0;
    while (s[n]) n++;
    return n;
}

/* Print a string to fd 1 (compositor serial / kernel log). */
static void dbg(const char *msg)
{
    sc3(SYS_WRITE, 1, (long)msg, (long)app_strlen(msg));
}

/*
 * Format a signed integer into buf[] (must be >= 22 bytes).
 * Returns a pointer to the first digit in buf (not always buf[0]).
 */
static char *fmt_int(long val, char *buf, int bufsz)
{
    int neg  = (val < 0);
    unsigned long uv = neg ? (unsigned long)(-(unsigned long)val)
                           : (unsigned long)val;
    int i = bufsz - 1;
    buf[i] = '\0';
    i--;
    if (uv == 0) { buf[i--] = '0'; }
    else { while (uv > 0 && i >= 0) { buf[i--] = (char)('0' + (int)(uv % 10)); uv /= 10; } }
    if (neg && i >= 0) buf[i--] = '-';
    return &buf[i + 1];
}

/* Append src onto dst (dst must have room). */
static void app_append(char *dst, const char *src)
{
    while (*dst) dst++;
    while (*src) *dst++ = *src++;
    *dst = '\0';
}

/* Copy src into dst (dst must have room). */
static void app_copy(char *dst, const char *src)
{
    while (*src) *dst++ = *src++;
    *dst = '\0';
}

/* ==================================================================
 * WINDOW GEOMETRY
 * Change WIN_W / WIN_H to resize the window.  All other layout
 * constants below derive from these two values.
 * ================================================================== */
#define WIN_W  360
#define WIN_H  280

/* Colours (ARGB32: 0xFF_RR_GG_BB) */
#define COL_TITLEBAR  0xFF1A237E   /* deep indigo */
#define COL_BODY      0xFF1C1C1E   /* near-black (Aether Dark bg) */
#define COL_SECTION   0xFF2C2C2E   /* slightly lighter panel */
#define COL_WHITE     0xFFFFFFFF
#define COL_CYAN      0xFF00D4FF
#define COL_GREY      0xFFB0B0B0
#define COL_GREEN     0xFF4CAF50
#define COL_ORANGE    0xFFFF9800
#define COL_RED       0xFFEF5350

/* ==================================================================
 * APPLICATION STATE
 * Keep ALL mutable state in file-static structs.  Their addresses
 * remain valid forever because _start() never returns -- it hands
 * control to ui_app_run() which loops internally.
 * ================================================================== */
typedef struct {
    /* Counter section */
    long          counter;       /* current count value             */
    ui_widget_t  *lbl_counter;   /* label widget that shows it      */

    /* Echo section */
    ui_widget_t  *txt_input;     /* textbox the user types into     */
    ui_widget_t  *lbl_echo;      /* label that mirrors the textbox  */

    /* Tick section */
    ui_widget_t  *lbl_time;      /* "Elapsed: Xs" label             */
    long          start_ms;      /* ticks value at launch           */
} app_state_t;

/* One global instance -- safe to take the address and pass to callbacks. */
static app_state_t g_app;

/* ==================================================================
 * HELPER: rebuild the counter label text from g_app.counter.
 * Call this whenever the counter value changes.
 * ================================================================== */
static void refresh_counter(app_state_t *st)
{
    char buf[32];
    char tmp[24];
    buf[0] = '\0';
    app_append(buf, "Count: ");
    app_append(buf, fmt_int(st->counter, tmp, sizeof(tmp)));
    ui_label_set_text(st->lbl_counter, buf);
}

/* ==================================================================
 * BUTTON CALLBACKS
 * Signature: void my_handler(void *ud)
 * ud is the pointer you passed as the last argument to ui_button().
 * ================================================================== */

/* [+1] button -- increment the counter and refresh the label. */
static void on_increment(void *ud)
{
    app_state_t *st = (app_state_t *)ud;
    st->counter++;              /* ---- your button handler here ---- */
    refresh_counter(st);
    dbg("[APP] increment\n");
}

/* [Reset] button -- reset the counter to zero. */
static void on_reset(void *ud)
{
    app_state_t *st = (app_state_t *)ud;
    st->counter = 0;            /* ---- your button handler here ---- */
    refresh_counter(st);
    dbg("[APP] reset\n");
}

/* [Echo] button -- copy the textbox content into the echo label. */
static void on_echo(void *ud)
{
    app_state_t *st = (app_state_t *)ud;
    /*
     * ui_textbox_text() returns a read-only pointer to the internal
     * buffer.  Pass it to ui_label_set_text() to display it.
     */
    const char *typed = ui_textbox_text(st->txt_input);
    ui_label_set_text(st->lbl_echo, typed);
    dbg("[APP] echo: ");
    dbg(typed);
    dbg("\n");
}

/* [Quit] button -- call SYS_EXIT(0) to terminate this process. */
static void on_quit(void *ud)
{
    (void)ud;
    dbg("[APP] quit\n");
    sc3(SYS_EXIT, 0, 0, 0);   /* does not return */
    __builtin_unreachable();
}

/* ==================================================================
 * PER-FRAME TICK CALLBACK
 * Registered with ui_app_set_tick().  Called once per frame before
 * the toolkit renders the widget tree.  Use it to:
 *   - poll live data (uptime, sensor readings, etc.)
 *   - animate values
 *   - update labels that change over time
 * ================================================================== */
static void on_tick(void *ud)
{
    app_state_t *st = (app_state_t *)ud;

    /* Read monotonic clock (AOS syscall 40 = SYS_GET_TICKS_MS). */
    long now_ms = sc3(SYS_GET_TICKS_MS, 0, 0, 0);
    long elapsed_s = (now_ms - st->start_ms) / 1000L;

    /* Build "Elapsed: Xs" string without printf. */
    char buf[40];
    char tmp[24];
    buf[0] = '\0';
    app_append(buf, "Elapsed: ");
    app_append(buf, fmt_int(elapsed_s, tmp, sizeof(tmp)));
    app_append(buf, "s");

    ui_label_set_text(st->lbl_time, buf);

    /* ---- your per-frame update code here ---- */
}

/* ==================================================================
 * _start  (entry point -- replaces main() in freestanding builds)
 * ==================================================================
 * Build the widget tree, register the tick, then hand off to the
 * toolkit event loop.  ui_app_run() never returns.
 *
 * Widget layout:
 *
 *   y=  0 ┌─────────────────────────────────┐
 *         │  Starter App               [Quit]│  title bar
 *   y= 36 ├─────────────────────────────────┤
 *         │ Counter panel                   │
 *         │  Count: 0            [+1][Reset] │
 *   y=100 ├─────────────────────────────────┤
 *         │ Text input panel                │
 *         │  [___________________] [Echo]   │
 *         │  Echo: (nothing yet)            │
 *   y=180 ├─────────────────────────────────┤
 *         │ Elapsed: 0s                     │  tick label
 *   y=280 └─────────────────────────────────┘
 * ================================================================== */
void _start(void)
{
    dbg("[APP] starting\n");

    /* ---- Initialise state ---- */
    g_app.counter  = 0;
    g_app.start_ms = sc3(SYS_GET_TICKS_MS, 0, 0, 0);

    /* ---- Create the window ---- */
    ui_app_t    *app  = ui_app_create("Starter App", WIN_W, WIN_H);
    if (!app) {
        dbg("[APP] ui_app_create failed\n");
        sc3(SYS_EXIT, 1, 0, 0);
        __builtin_unreachable();
    }
    ui_widget_t *root = ui_app_root(app);

    /* ==============================================================
     * TITLE BAR
     * A coloured panel spanning the full window width.
     * ui_panel(parent, x, y, w, h, colour)
     * ============================================================== */
    ui_widget_t *title_bar = ui_panel(root, 0, 0, WIN_W, 34, COL_TITLEBAR);

    /* Title text -- ui_label(parent, x, y, text, colour) */
    ui_label(title_bar, 10, 9, "Starter App", COL_CYAN);

    /* [Quit] button in the title bar.
     * ui_button(parent, x, y, w, h, text, callback, userdata) */
    ui_button(title_bar, WIN_W - 58, 5, 50, 24, "Quit", on_quit, 0);

    /* ==============================================================
     * COUNTER SECTION
     * Demonstrates: a label whose text is replaced by callbacks.
     * ============================================================== */
    ui_widget_t *cnt_panel = ui_panel(root, 8, 42, WIN_W - 16, 52, COL_SECTION);

    ui_label(cnt_panel, 10, 6, "Counter demo", COL_GREY);

    /* The counter label -- we save a pointer so on_increment / on_reset
     * can call ui_label_set_text() later.                             */
    g_app.lbl_counter = ui_label(cnt_panel, 10, 24, "Count: 0", COL_WHITE);

    /* [+1] and [Reset] buttons pass &g_app as userdata. */
    ui_button(cnt_panel, WIN_W - 128, 16, 52, 26, "+1",    on_increment, &g_app);
    ui_button(cnt_panel, WIN_W - 70,  16, 54, 26, "Reset", on_reset,     &g_app);

    /* ==============================================================
     * TEXT INPUT SECTION
     * Demonstrates: ui_textbox for typed input + echo via button.
     * ============================================================== */
    ui_widget_t *txt_panel = ui_panel(root, 8, 102, WIN_W - 16, 72, COL_SECTION);

    ui_label(txt_panel, 10, 6, "Text input demo", COL_GREY);

    /*
     * ui_textbox(parent, x, y, width, max_chars)
     * Click to focus. Printable keys append; Backspace deletes.
     * Read the content with ui_textbox_text(widget).
     */
    g_app.txt_input = ui_textbox(txt_panel, 10, 24, WIN_W - 100, 30);

    /* [Echo] button -- reads the textbox and updates the echo label. */
    ui_button(txt_panel, WIN_W - 82, 24, 64, 26, "Echo", on_echo, &g_app);

    /* Echo output label (shows what the user typed after pressing Echo). */
    g_app.lbl_echo = ui_label(txt_panel, 10, 52, "Echo: (type above, press Echo)", COL_GREY);

    /* ==============================================================
     * ELAPSED TIME SECTION
     * Demonstrates: per-frame tick callback updating a label live.
     * ============================================================== */
    ui_widget_t *time_panel = ui_panel(root, 8, 182, WIN_W - 16, 30, COL_SECTION);

    /* This label is refreshed every frame by on_tick(). */
    g_app.lbl_time = ui_label(time_panel, 10, 8, "Elapsed: 0s", COL_GREY);

    /* ==============================================================
     * HINT BAR
     * A simple static label at the bottom -- nothing dynamic.
     * ---- draw your UI here ----
     * ============================================================== */
    ui_widget_t *hint_panel = ui_panel(root, 0, WIN_H - 30, WIN_W, 30, COL_TITLEBAR);
    ui_label(hint_panel, 10, 8,
             "Click +1 to count  |  type + Echo to echo  |  Quit exits",
             0xFF7986CB);

    /* ==============================================================
     * TICK CALLBACK
     * Called once per frame (before rendering) so labels stay live.
     * ============================================================== */
    ui_app_set_tick(app, on_tick, &g_app);

    dbg("[APP] window ready\n");

    /* ---- Enter the event loop -- NEVER returns ---- */
    ui_app_run(app);

    __builtin_unreachable();
}
