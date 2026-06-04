/*
 * aiconsole.c -- AI Console: a single control surface for an AI agent to
 *                observe and drive the whole OS.
 * ==========================================================================
 *
 * Creates a 720x500 window titled "AI Console" via the M3 Wayland-lite client
 * library.  The upper portion is a scrolling output area; the bottom line is
 * an editable command line prefixed with "ai> ".
 *
 * Every command and its return code are printed to serial (fd 1) as structured
 * lines:
 *   [AICONSOLE] cmd=<verb args> rc=<number>
 * so a host AI process reading the serial port can parse structured output
 * without screen-scraping.
 *
 * Command vocabulary (AI OS-control interface):
 *   ps                       -- list all processes (PROCLIST + brief state)
 *   top                      -- ps + per-process detail (PROC_QUERY) + sysinfo
 *   info                     -- system stats: mem, uptime, proc count (SYSINFO)
 *   query <pid>              -- full proc_detail_t dump for one process
 *   spawn <app>              -- launch sbin/<app>  (SYS_SPAWN)
 *   suspend <pid>            -- suspend process    (PROC_CTL verb 0)
 *   resume  <pid>            -- resume  process    (PROC_CTL verb 1)
 *   kill    <pid>            -- kill    process    (PROC_CTL verb 2; guarded)
 *   nice    <pid> <n>        -- set priority n     (PROC_CTL verb 3)
 *   notify  <text>           -- desktop notification (SYS_NOTIFY)
 *   clip set <text>          -- write clipboard    (SYS_CLIP_SET)
 *   clip get                 -- read  clipboard    (SYS_CLIP_GET)
 *   help                     -- list commands
 *
 * Scripted one-liner form accepted on the command line: the same syntax
 * without special quoting, e.g.  "nice 7 5" or "notify hello world".
 *
 * Build (flags DIRECTLY on the command line -- never via shell variable,
 * or -fno-stack-protector is silently dropped and the binary faults at
 * CR2=0x28 / fs:0x28):
 *
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -O2 \
 *       -c userspace/apps/aiconsole/aiconsole.c -o aiconsole.o
 *
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -O2 \
 *       -c userspace/lib/wl/wl_client.c -o wl_client.o
 *
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -O2 \
 *       -c userspace/lib/font/bitfont.c -o bitfont.o
 *
 *   ld -nostdlib -static -n -no-pie -e _start -T userspace/userspace.ld \
 *       aiconsole.o wl_client.o bitfont.o -o build/aiconsole
 *
 *   objdump -d build/aiconsole | grep 'fs:0x28'   # MUST be empty
 */

#include "../../lib/wl/wl_client.h"
#include "../../lib/font/bitfont.h"

/* =========================================================================
 * Syscall numbers (must match kernel/include/syscall.h)
 * ========================================================================= */
#define SYS_WRITE         3
#define SYS_YIELD         15
#define SYS_SPAWN         16
#define SYS_KILL          26
#define SYS_GETPID        8
#define SYS_GETTIME       42
#define SYS_GET_TICKS_MS  40
#define SYS_PROCLIST      44
#define SYS_PROC_QUERY    60
#define SYS_PROC_CTL      61
#define SYS_SYSINFO       62
#define SYS_CLIP_SET      63
#define SYS_CLIP_GET      64
#define SYS_NOTIFY        65

/* SYS_PROC_CTL verbs */
#define PCTL_SUSPEND  0
#define PCTL_RESUME   1
#define PCTL_KILL     2
#define PCTL_SETPRIO  3

/* =========================================================================
 * Types (mirror kernel ABI exactly)
 * ========================================================================= */
typedef unsigned int       u32;
typedef unsigned long long u64;
typedef int                i32;

/* procinfo_t -- returned by SYS_PROCLIST=44, 64 bytes each */
typedef struct {
    u32  pid;
    u32  parent_pid;
    u32  state;
    u32  flags;
    char name[32];
    u64  cpu_ticks;
    u64  ctx_switches;
} procinfo_t;

/* proc_detail_t -- returned by SYS_PROC_QUERY=60 */
typedef struct {
    u32  pid;
    u32  ppid;
    u32  state;
    u32  prio;
    u64  cpu_ticks;
    u32  mem_pages;
    u32  vma_count;
    char name[32];
} proc_detail_t;

/* sysinfo_t -- returned by SYS_SYSINFO=62 */
typedef struct {
    u64 total_mem;
    u64 free_mem;
    u64 uptime_ms;
    u32 proc_count;
} sysinfo_t;

/* =========================================================================
 * Inline syscall wrapper (4-arg form matching the task spec)
 * ========================================================================= */
static inline long sc(long n, long a1, long a2, long a3) {
    long r;
    asm volatile("syscall"
                 : "=a"(r)
                 : "a"(n), "D"(a1), "S"(a2), "d"(a3)
                 : "rcx", "r11", "memory");
    return r;
}

/* =========================================================================
 * Freestanding string helpers (no libc)
 * ========================================================================= */
static unsigned long k_strlen(const char *s) {
    unsigned long n = 0;
    while (s[n]) n++;
    return n;
}

static int k_streq(const char *a, const char *b) {
    while (*a && *b) {
        if (*a != *b) return 0;
        a++; b++;
    }
    return *a == *b;
}

static int k_strneq(const char *a, const char *b, int n) {
    for (int i = 0; i < n; i++) {
        if (a[i] != b[i]) return 0;
        if (a[i] == '\0') return 1;
    }
    return 1;
}

/* Copy src into dst[cap]; NUL-terminates; returns chars written (excl NUL). */
static int k_strlcpy(char *dst, const char *src, int cap) {
    int i = 0;
    if (cap <= 0) return 0;
    while (src[i] && i < cap - 1) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
    return i;
}

/* Zero n bytes. */
static void k_memset(void *p, int v, unsigned long n) {
    unsigned char *b = (unsigned char *)p;
    while (n--) *b++ = (unsigned char)v;
}

/* Parse unsigned decimal from s; returns -1 on empty/non-digit. */
static long k_atoi(const char *s) {
    while (*s == ' ' || *s == '\t') s++;
    if (!*s || *s < '0' || *s > '9') return -1;
    long v = 0;
    while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; }
    return v;
}

/* =========================================================================
 * Serial / structured output helpers (write to fd 1)
 * ========================================================================= */
static void ser_str(const char *s) {
    sc(SYS_WRITE, 1, (long)s, (long)k_strlen(s));
}

static void ser_num(long n) {
    char b[24]; int i = 0;
    if (n < 0) { ser_str("-"); n = -n; }
    if (n == 0) { char z = '0'; sc(SYS_WRITE, 1, (long)&z, 1); return; }
    while (n > 0) { b[i++] = (char)('0' + (n % 10)); n /= 10; }
    while (i > 0) { char ch = b[--i]; sc(SYS_WRITE, 1, (long)&ch, 1); }
}

static void ser_unum(unsigned long n) {
    char b[24]; int i = 0;
    if (n == 0) { char z = '0'; sc(SYS_WRITE, 1, (long)&z, 1); return; }
    while (n > 0) { b[i++] = (char)('0' + (n % 10)); n /= 10; }
    while (i > 0) { char ch = b[--i]; sc(SYS_WRITE, 1, (long)&ch, 1); }
}

/*
 * Emit a structured log line readable by the host AI:
 *   [AICONSOLE] cmd=<cmdline> rc=<rc>
 */
static void ser_log(const char *cmdline, long rc) {
    ser_str("[AICONSOLE] cmd=");
    ser_str(cmdline);
    ser_str(" rc=");
    ser_num(rc);
    ser_str("\n");
}

/* Log a key=value pair for structured detail output. */
static void ser_kv_str(const char *key, const char *val) {
    ser_str("[AICONSOLE] ");
    ser_str(key);
    ser_str("=");
    ser_str(val);
    ser_str("\n");
}

static void ser_kv_num(const char *key, long val) {
    ser_str("[AICONSOLE] ");
    ser_str(key);
    ser_str("=");
    ser_num(val);
    ser_str("\n");
}

static void ser_kv_unum(const char *key, unsigned long val) {
    ser_str("[AICONSOLE] ");
    ser_str(key);
    ser_str("=");
    ser_unum(val);
    ser_str("\n");
}

/* =========================================================================
 * Window / grid geometry
 * ========================================================================= */
#define WIN_W      720
#define WIN_H      500

/* Reserve bottom 2 rows for the input area; the rest is the output log. */
#define MAX_COLS    (WIN_W / FONT_W)       /* 720/8  = 90  */
#define MAX_ROWS    (WIN_H / FONT_H)       /* 500/16 = 31  */
#define INPUT_ROWS  2                       /* rows reserved for input area  */
#define OUT_ROWS    (MAX_ROWS - INPUT_ROWS) /* 29 output rows                */

/* Colors */
#define COLOR_BG        0xFF0D1117u   /* near-black background        */
#define COLOR_OUT_BG    0xFF0D1117u   /* same for output area         */
#define COLOR_INPUT_BG  0xFF161B22u   /* slightly lighter input bg    */
#define COLOR_FG        0xFFCDD9E5u   /* cool-white text              */
#define COLOR_PROMPT    0xFF58A6FFu   /* blue prompt text             */
#define COLOR_CURSOR    0xFF3FB950u   /* green cursor block           */
#define COLOR_HLINE     0xFF30363Du   /* separator line               */
#define COLOR_BANNER    0xFF79C0FFu   /* banner / title color         */
#define COLOR_LABEL     0xFFF78166u   /* key labels in output         */
#define COLOR_VALUE     0xFFAFF5B4u   /* values in output             */
#define COLOR_WARN      0xFFFFA657u   /* warnings / errors            */

/* =========================================================================
 * Grid state: separate output grid and input line.
 * ========================================================================= */

/* Output grid: OUT_ROWS x MAX_COLS */
static char  out_grid[OUT_ROWS][MAX_COLS];
/* Per-cell color for the output grid (defaults to COLOR_FG). */
static u32   out_color[OUT_ROWS][MAX_COLS];

static int   g_cols;     /* actual cols used (derived from win->w) */
static int   g_out_rows; /* actual output rows                     */
static int   out_row;    /* current cursor row in output grid      */
static int   out_col;    /* current cursor col in output grid      */
static u32   cur_color;  /* color for the next characters written  */

/* Input line */
#define LINE_MAX 256
static char  line_buf[LINE_MAX];
static int   line_len;

/* History ring */
#define HIST_MAX   16
#define HIST_ENTRY 256
static char hist_ring[HIST_MAX][HIST_ENTRY];
static int  hist_count;
static int  hist_nav;   /* -1 = not navigating */

/* =========================================================================
 * Key definitions (evdev / kernel keycodes)
 * ========================================================================= */
#define KEY_ESC        1
#define KEY_1          2
#define KEY_2          3
#define KEY_3          4
#define KEY_4          5
#define KEY_5          6
#define KEY_6          7
#define KEY_7          8
#define KEY_8          9
#define KEY_9          10
#define KEY_0          11
#define KEY_MINUS      12
#define KEY_EQUAL      13
#define KEY_BACKSPACE  14
#define KEY_TAB        15
#define KEY_Q          16
#define KEY_W          17
#define KEY_E          18
#define KEY_R          19
#define KEY_T          20
#define KEY_Y          21
#define KEY_U          22
#define KEY_I          23
#define KEY_O          24
#define KEY_P          25
#define KEY_LEFTBRACE  26
#define KEY_RIGHTBRACE 27
#define KEY_ENTER      28
#define KEY_LEFTCTRL   29
#define KEY_A          30
#define KEY_S          31
#define KEY_D          32
#define KEY_F          33
#define KEY_G          34
#define KEY_H          35
#define KEY_J          36
#define KEY_K          37
#define KEY_L          38
#define KEY_SEMICOLON  39
#define KEY_APOSTROPHE 40
#define KEY_GRAVE      41
#define KEY_LEFTSHIFT  42
#define KEY_BACKSLASH  43
#define KEY_Z          44
#define KEY_X          45
#define KEY_C          46
#define KEY_V          47
#define KEY_B          48
#define KEY_N          49
#define KEY_M          50
#define KEY_COMMA      51
#define KEY_DOT        52
#define KEY_SLASH      53
#define KEY_RIGHTSHIFT 54
#define KEY_SPACE      57
#define KEY_UP         103
#define KEY_DOWN       108

/* =========================================================================
 * keycode -> ASCII (US layout)
 * ========================================================================= */
static char keycode_to_ascii(int kc, int shift) {
    switch (kc) {
        case KEY_1: return shift ? '!' : '1';
        case KEY_2: return shift ? '@' : '2';
        case KEY_3: return shift ? '#' : '3';
        case KEY_4: return shift ? '$' : '4';
        case KEY_5: return shift ? '%' : '5';
        case KEY_6: return shift ? '^' : '6';
        case KEY_7: return shift ? '&' : '7';
        case KEY_8: return shift ? '*' : '8';
        case KEY_9: return shift ? '(' : '9';
        case KEY_0: return shift ? ')' : '0';
        case KEY_MINUS:      return shift ? '_' : '-';
        case KEY_EQUAL:      return shift ? '+' : '=';
        case KEY_Q:  return shift ? 'Q' : 'q';
        case KEY_W:  return shift ? 'W' : 'w';
        case KEY_E:  return shift ? 'E' : 'e';
        case KEY_R:  return shift ? 'R' : 'r';
        case KEY_T:  return shift ? 'T' : 't';
        case KEY_Y:  return shift ? 'Y' : 'y';
        case KEY_U:  return shift ? 'U' : 'u';
        case KEY_I:  return shift ? 'I' : 'i';
        case KEY_O:  return shift ? 'O' : 'o';
        case KEY_P:  return shift ? 'P' : 'p';
        case KEY_LEFTBRACE:  return shift ? '{' : '[';
        case KEY_RIGHTBRACE: return shift ? '}' : ']';
        case KEY_A:  return shift ? 'A' : 'a';
        case KEY_S:  return shift ? 'S' : 's';
        case KEY_D:  return shift ? 'D' : 'd';
        case KEY_F:  return shift ? 'F' : 'f';
        case KEY_G:  return shift ? 'G' : 'g';
        case KEY_H:  return shift ? 'H' : 'h';
        case KEY_J:  return shift ? 'J' : 'j';
        case KEY_K:  return shift ? 'K' : 'k';
        case KEY_L:  return shift ? 'L' : 'l';
        case KEY_SEMICOLON:  return shift ? ':' : ';';
        case KEY_APOSTROPHE: return shift ? '"' : '\'';
        case KEY_GRAVE:      return shift ? '~' : '`';
        case KEY_BACKSLASH:  return shift ? '|' : '\\';
        case KEY_Z:  return shift ? 'Z' : 'z';
        case KEY_X:  return shift ? 'X' : 'x';
        case KEY_C:  return shift ? 'C' : 'c';
        case KEY_V:  return shift ? 'V' : 'v';
        case KEY_B:  return shift ? 'B' : 'b';
        case KEY_N:  return shift ? 'N' : 'n';
        case KEY_M:  return shift ? 'M' : 'm';
        case KEY_COMMA: return shift ? '<' : ',';
        case KEY_DOT:   return shift ? '>' : '.';
        case KEY_SLASH: return shift ? '?' : '/';
        case KEY_SPACE: return ' ';
        default:        return 0;
    }
}

/* =========================================================================
 * Output grid helpers
 * ========================================================================= */
static void out_clear(void) {
    for (int r = 0; r < OUT_ROWS; r++)
        for (int c = 0; c < MAX_COLS; c++) {
            out_grid[r][c]  = ' ';
            out_color[r][c] = COLOR_FG;
        }
    out_row = 0;
    out_col = 0;
    cur_color = COLOR_FG;
}

static void out_scroll_up(void) {
    for (int r = 1; r < g_out_rows; r++)
        for (int c = 0; c < g_cols; c++) {
            out_grid[r-1][c]  = out_grid[r][c];
            out_color[r-1][c] = out_color[r][c];
        }
    for (int c = 0; c < g_cols; c++) {
        out_grid[g_out_rows-1][c]  = ' ';
        out_color[g_out_rows-1][c] = COLOR_FG;
    }
}

static void out_newline(void) {
    out_col = 0;
    out_row++;
    if (out_row >= g_out_rows) {
        out_scroll_up();
        out_row = g_out_rows - 1;
    }
}

static void out_putchar_col(char ch, u32 color) {
    if (ch == '\n') { out_newline(); return; }
    if (ch == '\t') { do { out_putchar_col(' ', color); } while (out_col % 4 != 0); return; }
    if (out_col >= g_cols) out_newline();
    out_grid[out_row][out_col]  = ch;
    out_color[out_row][out_col] = color;
    out_col++;
    if (out_col >= g_cols) out_newline();
}

static void out_putchar(char ch) { out_putchar_col(ch, cur_color); }

static void out_puts_col(const char *s, u32 color) {
    for (; *s; s++) out_putchar_col(*s, color);
}

static void out_puts(const char *s) { out_puts_col(s, cur_color); }

/* Print unsigned decimal in output grid. */
static void out_unum(unsigned long n) {
    char b[24]; int i = 0;
    if (n == 0) { out_putchar('0'); return; }
    while (n > 0) { b[i++] = (char)('0' + (n % 10)); n /= 10; }
    while (i > 0) out_putchar(b[--i]);
}

static void out_num(long n) {
    if (n < 0) { out_putchar('-'); out_unum((unsigned long)-n); }
    else        out_unum((unsigned long)n);
}

/* Print n right-aligned in a field of `width` characters. */
static void out_unum_w(unsigned long n, int width) {
    char b[24]; int i = 0;
    if (n == 0) { b[i++] = '0'; }
    else while (n > 0) { b[i++] = (char)('0' + (n % 10)); n /= 10; }
    for (int sp = width - i; sp > 0; sp--) out_putchar(' ');
    while (i > 0) out_putchar(b[--i]);
}

/* Print a labeled key=value pair: label in COLOR_LABEL, value in COLOR_VALUE. */
static void out_kv(const char *key, const char *val) {
    out_puts_col(key, COLOR_LABEL);
    out_putchar_col('=', COLOR_FG);
    out_puts_col(val, COLOR_VALUE);
    out_putchar('\n');
}

static void out_kv_num(const char *key, long val) {
    char b[24]; int i = 0;
    long tmp = val < 0 ? -val : val;
    if (tmp == 0) { b[i++] = '0'; }
    else while (tmp > 0) { b[i++] = (char)('0' + (tmp % 10)); tmp /= 10; }
    if (val < 0) b[i++] = '-';
    char buf[24]; int j = 0;
    while (i > 0) buf[j++] = b[--i];
    buf[j] = '\0';
    out_kv(key, buf);
}

/* =========================================================================
 * Static path buffers (kernel copies the full MAX_PATH_LEN = 4096 bytes,
 * or 127 for spawn; always use these zeroed buffers, never bare literals).
 * ========================================================================= */
#define KPATH_MAX   4096
#define SPAWN_MAX   128
#define CLIP_MAX    512
#define NOTIFY_MAX  256

static char spawn_path[SPAWN_MAX];
static char clip_buf[CLIP_MAX];
static char notify_buf[NOTIFY_MAX];

/* =========================================================================
 * Process state name helper
 * ========================================================================= */
static const char *state_name(u32 s) {
    switch (s) {
        case 0: return "READY ";
        case 1: return "RUN   ";
        case 2: return "SLEEP ";
        case 3: return "WAIT  ";
        case 4: return "ZOMBIE";
        case 5: return "STOP  ";
        default: return "?     ";
    }
}

/* =========================================================================
 * Command: skip_spaces / token helpers
 * ========================================================================= */
static const char *skip_sp(const char *p) {
    while (*p == ' ' || *p == '\t') p++;
    return p;
}

/* Extract next whitespace-delimited token from *p into out[cap]. Returns len. */
static int next_tok(const char **p, char *out, int cap) {
    const char *s = skip_sp(*p);
    int n = 0;
    while (*s && *s != ' ' && *s != '\t' && n < cap - 1) out[n++] = *s++;
    out[n] = '\0';
    *p = s;
    return n;
}

/* =========================================================================
 * Large static buffers for PS/TOP (avoid stack allocation)
 * ========================================================================= */
#define PS_MAX  64
static procinfo_t   ps_buf[PS_MAX];
static proc_detail_t q_detail;
static sysinfo_t     sys_info;

/* =========================================================================
 * Command implementations
 * ========================================================================= */

/* ---- help ---- */
static void cmd_help(void) {
    cur_color = COLOR_BANNER;
    out_puts("AI Console -- OS control vocabulary\n");
    cur_color = COLOR_FG;
    out_puts("  ps                     list all processes\n");
    out_puts("  top                    ps + per-proc detail + sysinfo\n");
    out_puts("  info                   system stats (mem, uptime, procs)\n");
    out_puts("  query <pid>            full detail dump for one process\n");
    out_puts("  spawn <app>            launch sbin/<app>\n");
    out_puts("  suspend <pid>          suspend a process\n");
    out_puts("  resume  <pid>          resume  a process\n");
    out_puts("  kill    <pid>          kill    a process (guarded: not 0/1/self)\n");
    out_puts("  nice    <pid> <n>      set priority n\n");
    out_puts("  notify  <text>         post desktop notification\n");
    out_puts("  clip set <text>        write clipboard\n");
    out_puts("  clip get               read  clipboard\n");
    out_puts("  help                   show this list\n");
}

/* ---- ps ---- */
static void cmd_ps(const char *cmdline) {
    long count = sc(SYS_PROCLIST, (long)ps_buf, PS_MAX, 0);
    ser_log(cmdline, count);
    if (count < 0) {
        cur_color = COLOR_WARN;
        out_puts("ps: SYS_PROCLIST err "); out_num(count); out_putchar('\n');
        cur_color = COLOR_FG;
        return;
    }
    cur_color = COLOR_BANNER;
    out_puts("  PID  PPID  STATE   NAME\n");
    cur_color = COLOR_FG;
    out_puts("-----  ----  ------  --------------------\n");
    for (long i = 0; i < count; i++) {
        procinfo_t *pi = &ps_buf[i];
        pi->name[31] = '\0';
        out_unum_w((unsigned long)pi->pid,        5); out_puts("  ");
        out_unum_w((unsigned long)pi->parent_pid, 4); out_puts("  ");
        cur_color = COLOR_VALUE;
        out_puts(state_name(pi->state));
        cur_color = COLOR_FG;
        out_puts("  ");
        out_puts(pi->name[0] ? pi->name : "(unnamed)");
        out_putchar('\n');
        /* serial: one line per process */
        ser_str("[AICONSOLE] proc pid="); ser_unum((unsigned long)pi->pid);
        ser_str(" ppid="); ser_unum((unsigned long)pi->parent_pid);
        ser_str(" state="); ser_unum((unsigned long)pi->state);
        ser_str(" name="); ser_str(pi->name[0] ? pi->name : "(unnamed)");
        ser_str("\n");
    }
    out_puts("total="); out_unum((unsigned long)count); out_putchar('\n');
}

/* ---- info ---- */
static void cmd_info(const char *cmdline) {
    long r = sc(SYS_SYSINFO, (long)&sys_info, 0, 0);
    ser_log(cmdline, r);
    if (r < 0) {
        cur_color = COLOR_WARN;
        out_puts("info: SYS_SYSINFO err "); out_num(r); out_putchar('\n');
        cur_color = COLOR_FG;
        return;
    }
    cur_color = COLOR_BANNER;
    out_puts("-- System Info --\n");
    cur_color = COLOR_FG;
    out_kv_num("total_mem_kb",  (long)(sys_info.total_mem / 1024UL));
    out_kv_num("free_mem_kb",   (long)(sys_info.free_mem  / 1024UL));
    out_kv_num("used_mem_kb",   (long)((sys_info.total_mem - sys_info.free_mem) / 1024UL));
    out_kv_num("uptime_sec",    (long)(sys_info.uptime_ms / 1000UL));
    out_kv_num("uptime_ms",     (long)sys_info.uptime_ms);
    out_kv_num("proc_count",    (long)sys_info.proc_count);
    /* serial structured */
    ser_kv_unum("total_mem_kb",  (unsigned long)(sys_info.total_mem / 1024UL));
    ser_kv_unum("free_mem_kb",   (unsigned long)(sys_info.free_mem  / 1024UL));
    ser_kv_unum("uptime_ms",     (unsigned long)sys_info.uptime_ms);
    ser_kv_unum("proc_count",    (unsigned long)sys_info.proc_count);
}

/* ---- query <pid> ---- */
static void cmd_query(const char *args, const char *cmdline) {
    const char *p = args;
    char tok[32];
    if (!next_tok(&p, tok, sizeof(tok))) {
        cur_color = COLOR_WARN;
        out_puts("usage: query <pid>\n");
        cur_color = COLOR_FG;
        ser_log(cmdline, -1);
        return;
    }
    long pid = k_atoi(tok);
    if (pid < 0) {
        cur_color = COLOR_WARN;
        out_puts("query: invalid pid\n");
        cur_color = COLOR_FG;
        ser_log(cmdline, -1);
        return;
    }
    long r = sc(SYS_PROC_QUERY, pid, (long)&q_detail, 0);
    ser_log(cmdline, r);
    if (r < 0) {
        cur_color = COLOR_WARN;
        out_puts("query: SYS_PROC_QUERY err "); out_num(r); out_putchar('\n');
        cur_color = COLOR_FG;
        return;
    }
    q_detail.name[31] = '\0';
    cur_color = COLOR_BANNER;
    out_puts("-- Process Detail --\n");
    cur_color = COLOR_FG;
    out_kv_num("pid",       (long)q_detail.pid);
    out_kv_num("ppid",      (long)q_detail.ppid);
    out_kv_num("state",     (long)q_detail.state);
    out_kv_num("prio",      (long)q_detail.prio);
    out_kv_num("mem_pages", (long)q_detail.mem_pages);
    out_kv_num("vma_count", (long)q_detail.vma_count);
    /* cpu_ticks is u64; print low 32 bits */
    out_kv_num("cpu_ticks", (long)(q_detail.cpu_ticks & 0xFFFFFFFFUL));
    out_kv("name", q_detail.name[0] ? q_detail.name : "(unnamed)");
    /* serial */
    ser_kv_num("pid",       (long)q_detail.pid);
    ser_kv_num("ppid",      (long)q_detail.ppid);
    ser_kv_num("state",     (long)q_detail.state);
    ser_kv_num("prio",      (long)q_detail.prio);
    ser_kv_num("mem_pages", (long)q_detail.mem_pages);
    ser_kv_num("vma_count", (long)q_detail.vma_count);
    ser_kv_str("name", q_detail.name[0] ? q_detail.name : "(unnamed)");
}

/* ---- top ---- */
static void cmd_top(const char *cmdline) {
    /* sysinfo header */
    long si_r = sc(SYS_SYSINFO, (long)&sys_info, 0, 0);
    long count = sc(SYS_PROCLIST, (long)ps_buf, PS_MAX, 0);
    ser_log(cmdline, count);

    if (si_r == 0) {
        cur_color = COLOR_BANNER;
        out_puts("mem:");
        cur_color = COLOR_VALUE;
        out_unum(sys_info.free_mem / 1024UL);
        cur_color = COLOR_FG;
        out_puts("KB/");
        cur_color = COLOR_VALUE;
        out_unum(sys_info.total_mem / 1024UL);
        cur_color = COLOR_FG;
        out_puts("KB  procs:");
        cur_color = COLOR_VALUE;
        out_unum((unsigned long)sys_info.proc_count);
        cur_color = COLOR_FG;
        out_puts("  up:");
        cur_color = COLOR_VALUE;
        out_unum(sys_info.uptime_ms / 1000UL);
        cur_color = COLOR_FG;
        out_puts("s\n");
    }

    if (count < 0) {
        cur_color = COLOR_WARN;
        out_puts("top: PROCLIST err "); out_num(count); out_putchar('\n');
        cur_color = COLOR_FG;
        return;
    }

    /* Try SYS_PROC_QUERY on first entry to detect support. */
    int have_detail = 0;
    if (count > 0) {
        long r2 = sc(SYS_PROC_QUERY, (long)ps_buf[0].pid, (long)&q_detail, 0);
        have_detail = (r2 == 0);
    }

    if (have_detail) {
        cur_color = COLOR_BANNER;
        out_puts("  PID PPID PRI STATE   PAGES VMA  NAME\n");
        cur_color = COLOR_FG;
        out_puts("-----  --- ---  ------  -----  ---  --------------------\n");
        for (long i = 0; i < count; i++) {
            u32 pid = ps_buf[i].pid;
            long r2 = sc(SYS_PROC_QUERY, (long)pid, (long)&q_detail, 0);
            if (r2 != 0) {
                out_unum_w((unsigned long)pid, 5);
                cur_color = COLOR_WARN;
                out_puts(" (query err "); out_num(r2); out_puts(")\n");
                cur_color = COLOR_FG;
                continue;
            }
            q_detail.name[31] = '\0';
            out_unum_w((unsigned long)q_detail.pid,       5); out_puts("  ");
            out_unum_w((unsigned long)q_detail.ppid,      3); out_puts("  ");
            out_unum_w((unsigned long)q_detail.prio,      3); out_puts("  ");
            cur_color = COLOR_VALUE;
            out_puts(state_name(q_detail.state));
            cur_color = COLOR_FG;
            out_puts("  ");
            out_unum_w((unsigned long)q_detail.mem_pages, 5); out_puts("  ");
            out_unum_w((unsigned long)q_detail.vma_count, 3); out_puts("  ");
            out_puts(q_detail.name[0] ? q_detail.name : "(unnamed)");
            out_putchar('\n');
        }
    } else {
        /* Fallback: plain ps output */
        cur_color = COLOR_WARN;
        out_puts("(proc_query unavailable; showing ps)\n");
        cur_color = COLOR_FG;
        out_puts("  PID  PPID  STATE   NAME\n");
        for (long i = 0; i < count; i++) {
            procinfo_t *pi = &ps_buf[i];
            pi->name[31] = '\0';
            out_unum_w((unsigned long)pi->pid,        5); out_puts("  ");
            out_unum_w((unsigned long)pi->parent_pid, 4); out_puts("  ");
            out_puts(state_name(pi->state)); out_puts("  ");
            out_puts(pi->name[0] ? pi->name : "(unnamed)");
            out_putchar('\n');
        }
    }
    out_puts("total="); out_unum((unsigned long)count); out_putchar('\n');
}

/* ---- spawn <app> ---- */
static void cmd_spawn(const char *args, const char *cmdline) {
    const char *p = args;
    char tok[64];
    if (!next_tok(&p, tok, sizeof(tok))) {
        cur_color = COLOR_WARN;
        out_puts("usage: spawn <app>  (e.g. spawn shell)\n");
        cur_color = COLOR_FG;
        ser_log(cmdline, -1);
        return;
    }

    /* Build "sbin/<app>" in spawn_path (zeroed SPAWN_MAX buffer). */
    k_memset(spawn_path, 0, SPAWN_MAX);
    const char *pre = "sbin/";
    int pi = 0;
    while (pre[pi] && pi < SPAWN_MAX - 1) spawn_path[pi] = pre[pi++];
    k_strlcpy(spawn_path + pi, tok, SPAWN_MAX - pi);

    long pid = sc(SYS_SPAWN, (long)spawn_path, 0, 0);
    ser_log(cmdline, pid);
    if (pid <= 0) {
        cur_color = COLOR_WARN;
        out_puts("spawn: failed '"); out_puts(spawn_path); out_puts("' err=");
        out_num(pid); out_putchar('\n');
        cur_color = COLOR_FG;
        return;
    }
    cur_color = COLOR_VALUE;
    out_puts("spawned '"); out_puts(spawn_path); out_puts("' pid=");
    out_unum((unsigned long)pid); out_putchar('\n');
    cur_color = COLOR_FG;
    ser_kv_unum("spawned_pid", (unsigned long)pid);
}

/* ---- proc_ctl helper: suspend/resume/kill/nice ---- */
static long my_pid;  /* cached at startup to guard self-kill */

static int guard_pid(long pid) {
    if (pid == 0) {
        cur_color = COLOR_WARN;
        out_puts("refused: pid 0 is protected\n");
        cur_color = COLOR_FG;
        return 1;
    }
    if (pid == 1) {
        cur_color = COLOR_WARN;
        out_puts("refused: pid 1 is protected\n");
        cur_color = COLOR_FG;
        return 1;
    }
    if (pid == my_pid) {
        cur_color = COLOR_WARN;
        out_puts("refused: cannot target self\n");
        cur_color = COLOR_FG;
        return 1;
    }
    return 0;
}

static void cmd_proc_ctl(int verb, const char *args, const char *cmdline) {
    const char *p = args;
    char tok[32];

    if (!next_tok(&p, tok, sizeof(tok))) {
        cur_color = COLOR_WARN;
        out_puts("usage: suspend/resume/kill/nice <pid> [n]\n");
        cur_color = COLOR_FG;
        ser_log(cmdline, -1);
        return;
    }
    long pid = k_atoi(tok);
    if (pid < 0) {
        cur_color = COLOR_WARN;
        out_puts("invalid pid\n");
        cur_color = COLOR_FG;
        ser_log(cmdline, -1);
        return;
    }

    /* extra arg for setprio */
    long arg = 0;
    if (verb == PCTL_SETPRIO) {
        char tok2[32];
        if (!next_tok(&p, tok2, sizeof(tok2))) {
            cur_color = COLOR_WARN;
            out_puts("nice: usage: nice <pid> <priority>\n");
            cur_color = COLOR_FG;
            ser_log(cmdline, -1);
            return;
        }
        arg = k_atoi(tok2);
        if (arg < 0) {
            cur_color = COLOR_WARN;
            out_puts("nice: invalid priority\n");
            cur_color = COLOR_FG;
            ser_log(cmdline, -1);
            return;
        }
    }

    /* Guard: kill + all proc_ctl verbs honor the protection check. */
    if (guard_pid(pid)) {
        ser_log(cmdline, -2);
        return;
    }

    long r = sc(SYS_PROC_CTL, pid, verb, arg);
    ser_log(cmdline, r);
    if (r < 0) {
        cur_color = COLOR_WARN;
        out_puts("proc_ctl err="); out_num(r); out_putchar('\n');
        cur_color = COLOR_FG;
        return;
    }
    cur_color = COLOR_VALUE;
    switch (verb) {
        case PCTL_SUSPEND: out_puts("suspended pid="); break;
        case PCTL_RESUME:  out_puts("resumed pid=");   break;
        case PCTL_KILL:    out_puts("killed pid=");    break;
        case PCTL_SETPRIO: out_puts("setprio pid=");   break;
        default:           out_puts("proc_ctl ok pid="); break;
    }
    out_unum((unsigned long)pid);
    if (verb == PCTL_SETPRIO) { out_puts(" prio="); out_unum((unsigned long)arg); }
    out_putchar('\n');
    cur_color = COLOR_FG;
}

/* ---- notify <text> ---- */
static void cmd_notify(const char *args, const char *cmdline) {
    const char *text = skip_sp(args);
    if (!*text) {
        cur_color = COLOR_WARN;
        out_puts("usage: notify <text>\n");
        cur_color = COLOR_FG;
        ser_log(cmdline, -1);
        return;
    }

    /*
     * SYS_NOTIFY: sc(65, (long)"title\0body", len, 0)
     * We pass the full notify_buf as "title\0body":
     *   title = "AI Console"  (10 chars + NUL)
     *   body  = <text>
     */
    k_memset(notify_buf, 0, NOTIFY_MAX);
    /* Write title */
    const char *title = "AI Console";
    int ti = 0;
    while (title[ti] && ti < NOTIFY_MAX - 2) { notify_buf[ti] = title[ti]; ti++; }
    notify_buf[ti++] = '\0';
    /* Write body */
    int bi = 0;
    while (text[bi] && ti + bi < NOTIFY_MAX - 1) {
        notify_buf[ti + bi] = text[bi];
        bi++;
    }
    int total_len = ti + bi + 1; /* include trailing NUL */
    if (total_len > NOTIFY_MAX) total_len = NOTIFY_MAX;

    long r = sc(SYS_NOTIFY, (long)notify_buf, total_len, 0);
    ser_log(cmdline, r);
    if (r < 0) {
        cur_color = COLOR_WARN;
        out_puts("notify: err="); out_num(r); out_putchar('\n');
        cur_color = COLOR_FG;
        return;
    }
    cur_color = COLOR_VALUE;
    out_puts("notification sent: "); out_puts(text); out_putchar('\n');
    cur_color = COLOR_FG;
}

/* ---- clip set <text> / clip get ---- */
static void cmd_clip(const char *args, const char *cmdline) {
    const char *p = args;
    char sub[8];
    next_tok(&p, sub, sizeof(sub));

    if (k_streq(sub, "set")) {
        const char *text = skip_sp(p);
        k_memset(clip_buf, 0, CLIP_MAX);
        k_strlcpy(clip_buf, text, CLIP_MAX);
        long len = (long)k_strlen(clip_buf);
        long r = sc(SYS_CLIP_SET, (long)clip_buf, len, 0);
        ser_log(cmdline, r);
        if (r < 0) {
            cur_color = COLOR_WARN;
            out_puts("clip set: err="); out_num(r); out_putchar('\n');
            cur_color = COLOR_FG;
            return;
        }
        cur_color = COLOR_VALUE;
        out_puts("clipboard set ("); out_unum((unsigned long)len); out_puts(" bytes)\n");
        cur_color = COLOR_FG;

    } else if (k_streq(sub, "get")) {
        k_memset(clip_buf, 0, CLIP_MAX);
        long r = sc(SYS_CLIP_GET, (long)clip_buf, CLIP_MAX, 0);
        ser_log(cmdline, r);
        if (r < 0) {
            cur_color = COLOR_WARN;
            out_puts("clip get: err="); out_num(r); out_putchar('\n');
            cur_color = COLOR_FG;
            return;
        }
        clip_buf[CLIP_MAX - 1] = '\0';
        cur_color = COLOR_LABEL;
        out_puts("clipboard: ");
        cur_color = COLOR_VALUE;
        out_puts(clip_buf[0] ? clip_buf : "(empty)");
        out_putchar('\n');
        cur_color = COLOR_FG;
        ser_kv_str("clipboard", clip_buf[0] ? clip_buf : "(empty)");

    } else {
        cur_color = COLOR_WARN;
        out_puts("usage: clip set <text>  |  clip get\n");
        cur_color = COLOR_FG;
        ser_log(cmdline, -1);
    }
}

/* =========================================================================
 * Command dispatcher
 * ========================================================================= */
static void dispatch(const char *line) {
    const char *p = skip_sp(line);
    if (!*p) return;

    char verb[32];
    next_tok(&p, verb, sizeof(verb));

    /* Echo the command to output area */
    cur_color = COLOR_PROMPT;
    out_puts("ai> ");
    cur_color = COLOR_FG;
    out_puts(line);
    out_putchar('\n');

    if      (k_streq(verb, "help"))    { cmd_help(); ser_log(line, 0); }
    else if (k_streq(verb, "ps"))      cmd_ps(line);
    else if (k_streq(verb, "top"))     cmd_top(line);
    else if (k_streq(verb, "info"))    cmd_info(line);
    else if (k_streq(verb, "query"))   cmd_query(p, line);
    else if (k_streq(verb, "spawn"))   cmd_spawn(p, line);
    else if (k_streq(verb, "suspend")) cmd_proc_ctl(PCTL_SUSPEND, p, line);
    else if (k_streq(verb, "resume"))  cmd_proc_ctl(PCTL_RESUME,  p, line);
    else if (k_streq(verb, "kill"))    cmd_proc_ctl(PCTL_KILL,    p, line);
    else if (k_streq(verb, "nice"))    cmd_proc_ctl(PCTL_SETPRIO, p, line);
    else if (k_streq(verb, "notify"))  cmd_notify(p, line);
    else if (k_streq(verb, "clip"))    cmd_clip(p, line);
    else {
        cur_color = COLOR_WARN;
        out_puts(verb); out_puts(": unknown command (try 'help')\n");
        cur_color = COLOR_FG;
        ser_log(line, -127);
    }
}

/* =========================================================================
 * History helpers
 * ========================================================================= */
static void hist_push(const char *ln) {
    if (!ln || !ln[0]) return;
    int slot = hist_count % HIST_MAX;
    k_strlcpy(hist_ring[slot], ln, HIST_ENTRY);
    hist_count++;
}

static const char *hist_get(int offset) {
    if (offset < 0 || offset >= hist_count || offset >= HIST_MAX) return (void *)0;
    int idx = (hist_count - 1 - offset) % HIST_MAX;
    return hist_ring[idx];
}

/* =========================================================================
 * Rendering
 * ========================================================================= */
static void fill_rect(u32 *buf, u32 bw, u32 bh, u32 stride_px,
                      i32 x, i32 y, i32 w, i32 h, u32 color) {
    i32 x1 = x < 0 ? 0 : x;
    i32 y1 = y < 0 ? 0 : y;
    i32 x2 = x + w; if (x2 > (i32)bw) x2 = (i32)bw;
    i32 y2 = y + h; if (y2 > (i32)bh) y2 = (i32)bh;
    if (x1 >= x2 || y1 >= y2) return;
    for (i32 yy = y1; yy < y2; yy++) {
        u32 *row = buf + (u32)yy * stride_px;
        for (i32 xx = x1; xx < x2; xx++) row[xx] = color;
    }
}

static void render(wl_window *win, u32 stride_px) {
    /* Background */
    fill_rect(win->pixels, win->w, win->h, stride_px,
              0, 0, (i32)win->w, (i32)win->h, COLOR_BG);

    /* Output area */
    for (int r = 0; r < g_out_rows; r++) {
        for (int c = 0; c < g_cols; c++) {
            char ch = out_grid[r][c];
            if (ch == ' ' || ch == 0) continue;
            font_draw_char(win->pixels, (int)stride_px,
                           (int)win->w, (int)win->h,
                           c * FONT_W, r * FONT_H,
                           ch, out_color[r][c]);
        }
    }

    /* Separator line between output and input */
    int sep_y = g_out_rows * FONT_H;
    fill_rect(win->pixels, win->w, win->h, stride_px,
              0, sep_y, (i32)win->w, 1, COLOR_HLINE);

    /* Input area background */
    fill_rect(win->pixels, win->w, win->h, stride_px,
              0, sep_y + 1, (i32)win->w, INPUT_ROWS * FONT_H, COLOR_INPUT_BG);

    /* "ai> " prompt in blue */
    const char *prompt = "ai> ";
    int prompt_px = (int)k_strlen(prompt) * FONT_W;
    font_draw_string(win->pixels, (int)stride_px,
                     (int)win->w, (int)win->h,
                     0, sep_y + 1,
                     prompt, COLOR_PROMPT);

    /* Typed input in foreground color */
    if (line_len > 0) {
        /* Render char-by-char since line_buf is not NUL-terminated while
         * editing; we have a valid NUL only after enter.  Use a small
         * scratch string. */
        char scratch[LINE_MAX + 1];
        k_strlcpy(scratch, line_buf, line_len + 1);
        scratch[line_len] = '\0';
        font_draw_string(win->pixels, (int)stride_px,
                         (int)win->w, (int)win->h,
                         prompt_px, sep_y + 1,
                         scratch, COLOR_FG);
    }

    /* Cursor block at end of typed text */
    int cur_px = prompt_px + line_len * FONT_W;
    fill_rect(win->pixels, win->w, win->h, stride_px,
              cur_px, sep_y + 1, FONT_W, FONT_H, COLOR_CURSOR);

    wl_commit(win);
}

/* =========================================================================
 * Erase the visually echoed input line (used for history navigation).
 * ========================================================================= */
static void erase_input(void) {
    line_len = 0;
    /* The input area is entirely re-rendered on each render() call;
     * just clearing line_len is sufficient. */
}

/* =========================================================================
 * Entry point
 * ========================================================================= */
void _start(void) {
    ser_str("[AICONSOLE] starting\n");

    /* Cache our own PID so we can guard self-kill. */
    my_pid = sc(SYS_GETPID, 0, 0, 0);
    ser_str("[AICONSOLE] pid="); ser_num(my_pid); ser_str("\n");

    if (wl_connect() != 0) {
        ser_str("[AICONSOLE] wl_connect FAILED\n");
        for (;;) sc(SYS_YIELD, 0, 0, 0);
    }

    wl_window *win = wl_create_window(WIN_W, WIN_H, "AI Console");
    if (!win) {
        ser_str("[AICONSOLE] wl_create_window FAILED\n");
        for (;;) sc(SYS_YIELD, 0, 0, 0);
    }

    ser_str("[AICONSOLE] window="); ser_num(win->win_id); ser_str("\n");

    /* Derive actual grid dimensions from granted window size. */
    g_cols    = (int)(win->w / FONT_W);
    g_out_rows = (int)(win->h / FONT_H) - INPUT_ROWS;
    if (g_cols < 1)     g_cols     = 1;
    if (g_cols > MAX_COLS)   g_cols     = MAX_COLS;
    if (g_out_rows < 1) g_out_rows = 1;
    if (g_out_rows > OUT_ROWS) g_out_rows = OUT_ROWS;

    u32 stride_px = win->stride / 4u;

    /* Initialize grid and draw banner. */
    out_clear();
    cur_color = COLOR_BANNER;
    out_puts("AI Console -- OS control surface\n");
    cur_color = COLOR_FG;
    out_puts("Type 'help' for the command vocabulary.\n");
    out_puts("Serial structured output: [AICONSOLE] cmd=<...> rc=<...>\n");
    out_putchar('\n');

    line_len = 0;
    hist_count = 0;
    hist_nav   = -1;

    render(win, stride_px);

    long last_ms = sc(SYS_GET_TICKS_MS, 0, 0, 0);
    int shift_down = 0;

    for (;;) {
        int dirty = 0;
        int kind, a, b, c;

        while (wl_poll_event(win, &kind, &a, &b, &c)) {
            if (kind == WL_EVENT_RESIZE) {
                /*
                 * The library has ALREADY reallocated the buffer and updated
                 * win->{w,h,stride,pixels}.  Refresh the cached stride and
                 * recompute the text grid for the new size so the full new
                 * surface is painted and content reflows.  Clamp to the
                 * static array bounds (MAX_COLS/OUT_ROWS) and clamp the
                 * cursor so every subsequent write stays in-bounds.
                 */
                stride_px  = win->stride / 4u;
                g_cols     = (int)(win->w / FONT_W);
                g_out_rows = (int)(win->h / FONT_H) - INPUT_ROWS;
                if (g_cols < 1)            g_cols     = 1;
                if (g_cols > MAX_COLS)     g_cols     = MAX_COLS;
                if (g_out_rows < 1)        g_out_rows = 1;
                if (g_out_rows > OUT_ROWS) g_out_rows = OUT_ROWS;
                if (out_col >= g_cols)     out_col    = g_cols - 1;
                if (out_row >= g_out_rows) out_row    = g_out_rows - 1;
                dirty = 1;
                continue;
            }
            if (kind != WL_EVENT_KEY) continue;
            int keycode = a;
            int pressed = b;

            if (keycode == KEY_LEFTSHIFT || keycode == KEY_RIGHTSHIFT) {
                shift_down = pressed ? 1 : 0;
                continue;
            }
            if (!pressed) continue;

            if (keycode == KEY_UP) {
                /* Scroll history back. */
                int next_nav = hist_nav + 1;
                int available = hist_count < HIST_MAX ? hist_count : HIST_MAX;
                if (next_nav >= available) next_nav = available - 1;
                if (next_nav < 0) continue;
                const char *entry = hist_get(next_nav);
                if (!entry) continue;
                erase_input();
                int n = k_strlcpy(line_buf, entry, LINE_MAX);
                line_len = n;
                hist_nav = next_nav;
                dirty = 1;

            } else if (keycode == KEY_DOWN) {
                if (hist_nav < 0) continue;
                hist_nav--;
                erase_input();
                if (hist_nav >= 0) {
                    const char *entry = hist_get(hist_nav);
                    if (entry) {
                        int n = k_strlcpy(line_buf, entry, LINE_MAX);
                        line_len = n;
                    }
                }
                dirty = 1;

            } else if (keycode == KEY_ENTER) {
                line_buf[line_len] = '\0';
                hist_push(line_buf);
                hist_nav = -1;
                if (line_len > 0) dispatch(line_buf);
                erase_input();
                dirty = 1;

            } else if (keycode == KEY_BACKSPACE) {
                if (line_len > 0) {
                    line_len--;
                    hist_nav = -1;
                    dirty = 1;
                }

            } else {
                char ascii = keycode_to_ascii(keycode, shift_down);
                if (ascii && line_len < LINE_MAX - 1) {
                    line_buf[line_len++] = ascii;
                    hist_nav = -1;
                    dirty = 1;
                }
            }
        }

        if (dirty) render(win, stride_px);

        /* Light pacing: yield every iteration; re-render at ~60 Hz. */
        long now = sc(SYS_GET_TICKS_MS, 0, 0, 0);
        if (now - last_ms >= 16) {
            last_ms = now;
            /* Cursor blink is handled by the static cursor block; no extra
             * re-render needed unless there was input. */
        }
        sc(SYS_YIELD, 0, 0, 0);
    }
}
