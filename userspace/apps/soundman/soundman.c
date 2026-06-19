/*
 * soundman.c -- Sound Manager GUI app (freestanding, ring 3).
 * ==========================================================
 *
 * A small, polished system-audio panel for AutomationOS, built on the M4
 * retained-mode UI toolkit (userspace/lib/ui) over the M3 "Wayland-lite"
 * compositor stack and the 8x16 bitmap font. It mirrors the netman app's
 * structure: a bare _start entry, a ui_app + per-frame on_tick callback, an
 * inline 6-arg sc() syscall helper, and serial markers via SYS_WRITE.
 *
 * Controls (all over the new SYS_AUDIO_* syscalls):
 *
 *   - Title "Sound".
 *   - A "Volume" label + a ui_slider 0..100. Its on_change drives
 *     SYS_AUDIO_VOLUME (118); a "<N>%" label tracks the value live.
 *   - A "Mute" ui_toggle (animated). Its on_change drives SYS_AUDIO_MUTE (119).
 *   - A "Test Tone" ui_button -> SYS_AUDIO_TEST (122, 440 Hz, 250 ms).
 *   - A live status line refreshed each tick from SYS_AUDIO_STATUS (123):
 *       "Codec: present | Vol: N% | Mute: on/off | vendor 0x........"
 *     or "No audio device" when present == 0 (e.g. ENODEV / audioless boot).
 *
 * The slider starts at the kernel's reported volume (read once at startup via
 * SYS_AUDIO_STATUS; default 80 if the read fails), and the mute toggle starts
 * at the kernel's reported mute state, so the panel opens already in sync.
 *
 * No libc: pure inline syscalls + tiny freestanding helpers + the OS UI/wl/
 * font libraries. Fixed-size buffers, bounded loops, integer-only (no float --
 * the toolkit owns the Q8 easing).
 *
 * Build (flags DIRECTLY on the command line -- never via a shell variable; a
 * shell var empties through `wsl.exe ... bash -lc` and gcc falls back to the
 * canary-on defaults):
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -mstackrealign -O2 \
 *       -c userspace/apps/soundman/soundman.c -o soundman.o
 *   ld -nostdlib -static -n -no-pie -e _start -T userspace/userspace.ld \
 *       soundman.o ui.o wl_client.o bitfont.o font2.o -o build/soundman
 *   objdump -d build/soundman | grep fs:0x28   # MUST be empty (no canary)
 *
 * Serial output (fd 1, for the boot smoke test):
 *   [SOUNDMAN] window created
 *   [SOUNDMAN] audio present=1 vol=80 mute=0 vendor=0x........   (one-shot)
 */

#include "../../lib/ui/ui.h"

/* -----------------------------------------------------------------------
 * Syscall numbers and inline syscall helper.
 * --------------------------------------------------------------------- */
#define SYS_WRITE         3
#define SYS_AUDIO_VOLUME  118  /* sc(118, vol 0..100) -> 0 ok, <0 (ENODEV) no codec */
#define SYS_AUDIO_MUTE    119  /* sc(119, 0|1)        -> 0/err                       */
#define SYS_AUDIO_TEST    122  /* sc(122, freq_hz, ms)-> plays a tone (ms capped 2000)*/
#define SYS_AUDIO_STATUS  123  /* sc(123, &audio_status_t) -> 0/err                  */

/*
 * 6-arg raw inline syscall wrapper.
 *   nr -> rax ; args -> rdi, rsi, rdx, r10, r8 ; ret -> rax
 */
static long sc(long n, long a1, long a2, long a3, long a4, long a5)
{
    long r;
    register long r10 asm("r10") = a4;
    register long r8  asm("r8")  = a5;
    asm volatile("syscall"
                 : "=a"(r)
                 : "a"(n), "D"(a1), "S"(a2), "d"(a3), "r"(r10), "r"(r8)
                 : "rcx", "r11", "memory");
    return r;
}

/* -----------------------------------------------------------------------
 * Kernel payload for SYS_AUDIO_STATUS -- MUST match the kernel
 * audio_status_t (kernel/include/syscall.h) byte-for-byte: 8 bytes,
 * naturally aligned.  Defined LOCALLY so soundman.c stays self-contained.
 * --------------------------------------------------------------------- */
typedef struct {
    unsigned char present;       /* 1 = an HDA controller + codec is up, else 0   */
    unsigned char volume;        /* last volume set via SYS_AUDIO_VOLUME (0..100) */
    unsigned char muted;         /* 1 = muted (last SYS_AUDIO_MUTE), else 0       */
    unsigned char _pad;          /* alignment padding                             */
    unsigned int  codec_vendor;  /* codec vendor/device id (HDA VENDOR_ID)        */
} audio_status_t;                /* 8 bytes */

/* -----------------------------------------------------------------------
 * Freestanding helpers (no libc).
 * --------------------------------------------------------------------- */

static unsigned long k_strlen(const char *s)
{
    unsigned long n = 0;
    while (s[n]) n++;
    return n;
}

static void serial_print(const char *m)
{
    sc(SYS_WRITE, 1, (long)m, (long)k_strlen(m), 0, 0);
}

/* Append NUL-terminated src to dst (already NUL-terminated). Returns new len. */
static int str_append(char *dst, int len, const char *src)
{
    while (*src) dst[len++] = *src++;
    dst[len] = '\0';
    return len;
}

/* Append a 1-3 digit unsigned decimal (0..255) at dst[len]. Returns new len. */
static int append_u8_dec(char *dst, int len, unsigned int v)
{
    char tmp[4];
    int  i = 0;
    if (v > 255) v = 255;
    do { tmp[i++] = (char)('0' + (v % 10)); v /= 10; } while (v);
    while (i > 0) dst[len++] = tmp[--i];
    dst[len] = '\0';
    return len;
}

/* Append an 8-digit lowercase "0x........" hex word at dst[len]. */
static int append_u32_hex(char *dst, int len, unsigned int v)
{
    static const char H[] = "0123456789abcdef";
    dst[len++] = '0';
    dst[len++] = 'x';
    for (int sh = 28; sh >= 0; sh -= 4)
        dst[len++] = H[(v >> sh) & 0xF];
    dst[len] = '\0';
    return len;
}

/* -----------------------------------------------------------------------
 * Application state.
 * --------------------------------------------------------------------- */

/* Colors (ARGB) consistent with the Aether Dark theme. */
#define COL_TITLE   0xFFFFFFFFu
#define COL_DIM     0xFFAEAEB2u
#define COL_ACCENT  0xFF0A84FFu
#define COL_GREEN   0xFF30D158u
#define COL_RED     0xFFFF453Au
#define COL_FIELD   0xFF1C1C1Eu

/* Live widgets (updated by callbacks / the tick). */
static ui_widget_t *g_vol_value  = 0;   /* "<N>%" next to the slider           */
static ui_widget_t *g_slider     = 0;   /* volume slider 0..100                */
static ui_widget_t *g_mute       = 0;   /* animated mute toggle                */
static ui_widget_t *g_status     = 0;   /* live status line                    */

/* One-shot smoke-test serial line guard. */
static int g_smoke_emitted = 0;

/* App handles. */
static ui_app_t    *g_app  = 0;
static ui_widget_t *g_root = 0;

/* -----------------------------------------------------------------------
 * Read SYS_AUDIO_STATUS into a caller struct.  Returns 1 on success
 * (rc == 0), 0 otherwise (and zeroes the struct so callers render sane).
 * --------------------------------------------------------------------- */
static int read_status(audio_status_t *st)
{
    for (unsigned i = 0; i < sizeof(*st); i++) ((unsigned char *)st)[i] = 0;
    long rc = sc(SYS_AUDIO_STATUS, (long)st, 0, 0, 0, 0);
    return (rc == 0);
}

/* -----------------------------------------------------------------------
 * Volume slider callback: push the new value to the kernel and update the
 * "<N>%" readout label.  Integer only.
 * --------------------------------------------------------------------- */
static void on_volume(int value, void *ud)
{
    (void)ud;
    if (value < 0)   value = 0;
    if (value > 100) value = 100;

    sc(SYS_AUDIO_VOLUME, (long)value, 0, 0, 0, 0);

    char line[8];
    int  l = append_u8_dec(line, 0, (unsigned int)value);
    line[l++] = '%';
    line[l]   = '\0';
    ui_label_set_text(g_vol_value, line);
}

/* -----------------------------------------------------------------------
 * Mute toggle callback (animated): drive SYS_AUDIO_MUTE.
 * --------------------------------------------------------------------- */
static void on_mute(int state, void *ud)
{
    (void)ud;
    sc(SYS_AUDIO_MUTE, state ? 1 : 0, 0, 0, 0, 0);
}

/* -----------------------------------------------------------------------
 * Test-tone button: play a 440 Hz tone for 250 ms (kernel caps the ms).
 * --------------------------------------------------------------------- */
static void on_test_tone(void *ud)
{
    (void)ud;
    sc(SYS_AUDIO_TEST, 440, 250, 0, 0, 0);
}

/* -----------------------------------------------------------------------
 * Per-frame tick: refresh the live status line from SYS_AUDIO_STATUS.
 *
 *   present: "Codec: present | Vol: N% | Mute: on/off | vendor 0x........"
 *   absent:  "No audio device"
 * --------------------------------------------------------------------- */
static void tick_cb(void *ud)
{
    (void)ud;

    audio_status_t st;
    int ok = read_status(&st);

    if (ok && st.present) {
        char line[80];
        int  l = str_append(line, 0, "Codec: present | Vol: ");
        l = append_u8_dec(line, l, st.volume);
        l = str_append(line, l, "% | Mute: ");
        l = str_append(line, l, st.muted ? "on" : "off");
        l = str_append(line, l, " | vendor ");
        l = append_u32_hex(line, l, st.codec_vendor);
        ui_label_set_text(g_status, line);
        ui_widget_set_fg(g_status, st.muted ? COL_RED : COL_GREEN);
    } else {
        ui_label_set_text(g_status, "No audio device");
        ui_widget_set_fg(g_status, COL_DIM);
    }

    /* One-shot smoke-test line:
     *   [SOUNDMAN] audio present=1 vol=80 mute=0 vendor=0x........ */
    if (!g_smoke_emitted) {
        char sline[80];
        int  s = str_append(sline, 0, "[SOUNDMAN] audio present=");
        s = append_u8_dec(sline, s, (ok && st.present) ? 1u : 0u);
        s = str_append(sline, s, " vol=");
        s = append_u8_dec(sline, s, st.volume);
        s = str_append(sline, s, " mute=");
        s = append_u8_dec(sline, s, st.muted ? 1u : 0u);
        s = str_append(sline, s, " vendor=");
        s = append_u32_hex(sline, s, st.codec_vendor);
        sline[s++] = '\n';
        sline[s]   = '\0';
        serial_print(sline);
        g_smoke_emitted = 1;
    }
}

/* -----------------------------------------------------------------------
 * Entry point.
 * --------------------------------------------------------------------- */
void _start(void)
{
    /* Read the current audio state so the panel opens already in sync. */
    audio_status_t st0;
    int   have   = read_status(&st0);
    int   vol0   = (have ? st0.volume : 80);   /* kernel default is 80    */
    int   mute0  = (have ? (st0.muted ? 1 : 0) : 0);
    if (vol0 < 0)   vol0 = 0;
    if (vol0 > 100) vol0 = 100;

    /* Window: 360 x 300, titled "Sound". */
    ui_app_t    *app  = ui_app_create("Sound", 360, 300);
    ui_widget_t *root = ui_app_root(app);
    g_app  = app;
    g_root = root;

    serial_print("[SOUNDMAN] window created\n");

    /*
     * Layout (all x/y relative to root = window origin):
     *
     *   y=16   "Sound"                title                [Mute toggle @ x=300]
     *   y=44   "Mute"                 toggle label
     *   y=80   "Volume"               section label        ["<N>%" @ x=312]
     *   y=104  [ volume slider, 320 wide, 0..100 ]
     *   y=150  [ Test Tone button ]
     *   y=210  "Status" header
     *   y=232  [ status panel ] -> live status line
     */
    ui_label(root, 16, 16, "Sound", COL_TITLE);

    /* Animated mute toggle (top-right) + its label. */
    ui_label(root, 16, 48, "Mute", COL_DIM);
    g_mute = ui_toggle(root, 300, 44, mute0, on_mute, 0);

    /* Volume section: label, live "<N>%" readout, then the slider. */
    ui_label(root, 16, 80, "Volume", COL_TITLE);
    {
        char vbuf[8];
        int  vl = append_u8_dec(vbuf, 0, (unsigned int)vol0);
        vbuf[vl++] = '%';
        vbuf[vl]   = '\0';
        g_vol_value = ui_label(root, 312, 80, vbuf, COL_ACCENT);
    }
    g_slider = ui_slider(root, 16, 104, 320, 0, 100, vol0, on_volume, 0);

    /* Test-tone button. */
    ui_button(root, 16, 150, 130, 32, "Test Tone", on_test_tone, 0);

    /* Live status section. */
    ui_label(root, 16, 210, "Status", COL_TITLE);
    ui_panel(root, 16, 232, 328, 44, COL_FIELD);
    g_status = ui_label(root, 24, 248, "Codec: ...", COL_DIM);

    /* Populate the status immediately and emit the smoke-test line. */
    tick_cb(0);

    /* Keep the status live; the toolkit paces the loop. */
    ui_app_set_tick(app, tick_cb, 0);

    ui_app_run(app);   /* never returns */
}
