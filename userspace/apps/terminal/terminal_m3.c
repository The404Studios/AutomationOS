/*
 * terminal_m3.c -- M3b GUI client turned into a real in-process command shell.
 * ===========================================================================
 *
 * Creates a 640x400 window via the M3 "Wayland-lite" client library, renders a
 * monospace character grid with the embedded 8x16 bitmap font, and runs an
 * INTERACTIVE COMMAND SHELL entirely in-process (no PTY, no child /bin/sh).
 *
 * Keyboard events (WL_EVT_KEY) are forwarded by the compositor; we never touch
 * /dev/input. Typed characters accumulate into a line buffer (with Backspace
 * editing) until Enter, at which point the line is parsed and a built-in
 * command is executed, printing its output into the scrolling grid.
 *
 * Built-in commands:
 *   help                 list available commands
 *   echo <args...>       print the arguments
 *   clear                clear the screen
 *   uptime               milliseconds since boot (SYS_GET_TICKS_MS), formatted
 *   ls [path]            list a directory (SYS_OPENDIR/READDIR/CLOSEDIR)
 *   cat <file>           print a file's contents (SYS_OPEN + SYS_READ)
 *   run <path>           spawn a GUI app from the initrd (SYS_SPAWN)
 *   launch <path>        alias for run
 *   spawn <app>          spawn from sbin/<app> (SYS_SPAWN)
 *   ps                   list processes (SYS_PROCLIST=44)
 *   top                  detailed process list + system stats
 *   kill <pid>           send SIGKILL to a process (SYS_KILL=26)
 *   mem / sysinfo        print memory and system info (SYS_SYSINFO=62)
 *   history              show recent command history
 *   cp/grep/wc/head/tail text utilities operating on files
 *   date/uname/whoami    misc info builtins
 *
 * Shell syntax:
 *   cmd > file / >> file output redirection (truncate / append). Captured
 *                         uniformly so it works for EVERY builtin.
 *   cmd1 ; cmd2           run multiple commands in sequence on one line.
 *
 * Scrollback: when the grid fills, lines scroll up one row.
 *
 * Build (EXACT -- flags passed DIRECTLY on the command line, NEVER via a shell
 * variable, or -fno-stack-protector gets dropped and the program faults at
 * CR2=0x28):
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -O2 \
 *       -c userspace/apps/terminal/terminal_m3.c -o terminal_m3.o
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -O2 \
 *       -c userspace/lib/wl/wl_client.c -o wl_client.o
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -O2 \
 *       -c userspace/lib/font/bitfont.c -o bitfont.o
 *   ld -nostdlib -static -n -no-pie -e _start -T userspace/userspace.ld \
 *       terminal_m3.o wl_client.o bitfont.o -o build/terminal_m3
 *   objdump -d build/terminal_m3 | grep fs:0x28   # MUST be empty
 */

#include "../../lib/wl/wl_client.h"
#include "../../lib/font/bitfont.h"
#include "../../lib/keymap/keymap.h"   /* shared US-QWERTY: caps-lock + shift + symbols */
#include "sh_git.h"

/* ---- syscall numbers (must match kernel/include/syscall.h) ---- */
#define SYS_READ          2
#define SYS_WRITE         3
#define SYS_OPEN          4
#define SYS_CLOSE         5
#define SYS_KILL          26
#define SYS_YIELD         15
#define SYS_SPAWN         16
#define SYS_OPENDIR       30
#define SYS_READDIR       31
#define SYS_CLOSEDIR      32
#define SYS_STAT          33
#define SYS_UNLINK        34
#define SYS_RENAME        35
#define SYS_GET_TICKS_MS  40
#define SYS_MKDIR         67
#define SYS_PROCLIST      44
#define SYS_PROC_QUERY    60
#define SYS_SYSINFO       62
#define SYS_NET_INFO      59   /* query IP/MAC/link state (kernel/include/syscall.h) */

/* O_* flags (kernel/include/vfs.h) */
#define O_RDONLY          0x0000
#define O_WRONLY          0x0001
#define O_CREAT           0x0040
#define O_TRUNC           0x0200
#define O_APPEND          0x0400

/*
 * The kernel copies a fixed-size buffer out of the user path pointer:
 *   sys_open / sys_opendir : MAX_PATH_LEN (4096) bytes
 *   sys_spawn              : 127 bytes
 * So any pointer we hand to those syscalls MUST have at least that many
 * readable bytes behind it, or copy_from_user() faults. We therefore always
 * pass paths through statically-sized buffers (never bare string literals
 * shorter than the copy length).
 */
#define KPATH_MAX         4096   /* matches kernel MAX_PATH_LEN copy */

/* ---- raw kernel evdev keycodes (subset of kernel/include/input.h) ---- */
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
/* Arrow key scancodes (evdev/Linux keycodes used by our compositor) */
#define KEY_UP         103
#define KEY_DOWN       108

typedef unsigned int       u32;
typedef unsigned long long u64;
typedef int                i32;

/* dirent layout mirrors `struct dirent` in kernel/include/vfs.h. The kernel
 * copies sizeof(struct dirent) bytes into our buffer in sys_readdir(). */
#define DT_DIR     4
#define DT_REG     8
#define NAME_MAX_  256
struct k_dirent {
    unsigned long long d_ino;            /* uint64_t */
    long long          d_off;            /* int64_t  */
    unsigned short     d_reclen;         /* uint16_t */
    unsigned char      d_type;           /* uint8_t  */
    char               d_name[NAME_MAX_];/* NUL-terminated filename */
};

/*
 * procinfo_t -- returned by SYS_PROCLIST=44
 *   sc(44, (long)buf, max_entries, 0, ...) → count of entries filled
 *   Each entry is 64 bytes: u32 pid, parent_pid, state, flags; char name[32];
 *   u64 cpu_ticks; u64 ctx_switches.
 */
typedef struct {
    u32  pid;
    u32  parent_pid;
    u32  state;
    u32  flags;
    char name[32];
    u64  cpu_ticks;
    u64  ctx_switches;
} procinfo_t;   /* 64 bytes total */

/*
 * proc_detail_t -- returned by SYS_PROC_QUERY=60
 *   sc(60, pid, (long)&d, 0, ...) → 0 on success, <0 on error
 */
typedef struct {
    u32  pid;
    u32  ppid;
    u32  state;
    u32  prio;
    u64  cpu_ticks;
    u32  mem_pages;
    u32  vma_count;
    char name[32];
} proc_detail_t;   /* 64 bytes total */

/*
 * sysinfo_t -- returned by SYS_SYSINFO=62 (must match kernel procapi.h: 32 bytes)
 *   sc(62, (long)&si, 0, 0, ...) → 0 on success, <0 on error
 */
typedef struct {
    u64 total_mem;
    u64 free_mem;
    u64 uptime_ms;
    u32 proc_count;
    u32 _pad;        /* reserved, always 0 */
} sysinfo_t;

/*
 * net_info_t -- filled by SYS_NET_INFO=59
 *   sc(59, (long)&info, 0, 0, ...) -> 0 on success, <0 if networking is down.
 * Layout MUST mirror the kernel's uapi_net_info_t (kernel/include/uapi/net.h).
 * ip and gateway are in host byte order (0xAABBCCDD == A.B.C.D), so octet A is
 * the high byte ((ip >> 24) & 0xFF).  80 bytes total.
 */
typedef struct {
    char          ifname[16];
    unsigned char mac[6];
    unsigned char _pad[2];
    u32           ip;
    u32           netmask;
    u32           gateway;
    u32           dns;
    unsigned char up;
    unsigned char dhcp_active;
    unsigned char _pad2[6];
    unsigned long long tx_packets;
    unsigned long long rx_packets;
    unsigned long long tx_bytes;
    unsigned long long rx_bytes;
} net_info_t;

/* 6-argument inline syscall (args rdi/rsi/rdx/r10/r8/r9). */
static inline long sc(long n, long a1, long a2, long a3, long a4, long a5, long a6) {
    long r;
    register long r10 asm("r10") = a4, r8 asm("r8") = a5, r9 asm("r9") = a6;
    asm volatile("syscall"
                 : "=a"(r)
                 : "a"(n), "D"(a1), "S"(a2), "d"(a3), "r"(r10), "r"(r8), "r"(r9)
                 : "rcx", "r11", "memory");
    return r;
}

/* ---- tiny freestanding string helpers ---- */
static unsigned long k_strlen(const char *s) { unsigned long n = 0; while (s[n]) n++; return n; }

static int k_streq(const char *a, const char *b) {
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}

/* Copy src into dst (cap including NUL); returns length written (excl NUL). */
static int k_strlcpy(char *dst, const char *src, int cap) {
    int i = 0;
    if (cap <= 0) return 0;
    while (src[i] && i < cap - 1) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
    return i;
}

/* atoi for unsigned decimal; stops at non-digit; returns -1 on empty. */
static long k_atoi(const char *s) {
    while (*s == ' ' || *s == '\t') s++;
    if (!*s) return -1;
    long v = 0;
    while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; }
    return v;
}

/* ---- tiny serial diagnostics (fd 1) ---- */
static void print(const char *m) { sc(SYS_WRITE, 1, (long)m, (long)k_strlen(m), 0, 0, 0); }
static void print_num(long n) {
    char b[24]; int i = 0;
    if (n < 0) { print("-"); n = -n; }
    do { b[i++] = (char)('0' + (n % 10)); n /= 10; } while (n > 0);
    while (i > 0) { char ch = b[--i]; sc(SYS_WRITE, 1, (long)&ch, 1, 0, 0, 0); }
}
static void print_char(char ch) { sc(SYS_WRITE, 1, (long)&ch, 1, 0, 0, 0); }

/*
 * Raw-keycode -> ASCII now comes from the shared US-QWERTY keymap library
 * (userspace/lib/keymap), which adds caps-lock and the full Shift+symbol set
 * that the old per-app table lacked. The translation state (shift/caps) lives
 * in a single keymap_state_t driven from the event loop; see below.
 */

/* ---- window / grid geometry ---- */
#define WIN_W      640
#define WIN_H      400
#define MAX_COLS   (WIN_W / FONT_W)   /* 640/8  = 80 */
#define MAX_ROWS   (WIN_H / FONT_H)   /* 400/16 = 25 */

#define BG_COLOR     0xFF101418u   /* dark background          */
#define FG_COLOR     0xFFD8E0E8u   /* light foreground text    */
#define CURSOR_COLOR 0xFF50C878u   /* green cursor block       */

/* =========================================================================
 *  Output redirection capture
 *  When g_cap_on is set, all command output (routed through grid_putchar /
 *  grid_puts) is appended to g_cap instead of being drawn to the window.
 *  shell_execute toggles this around the dispatch body so that the prompt and
 *  the echoed input line are NEVER captured.
 * ========================================================================= */
#define CAP_MAX 8192
static char g_cap[CAP_MAX] __attribute__((aligned(16)));
static int  g_cap_on;    /* 1 while capturing a command's output */
static int  g_cap_len;   /* bytes currently held in g_cap        */

/* =========================================================================
 *  Last-command exit status (for shell control flow: if / while / && style)
 *  Builtins do not return a status, so we keep a single global that the few
 *  status-aware commands set: `test`/`[ ]` set it to 0 (true) / 1 (false), and
 *  an external spawn sets 0 (launched) / 127 (not found). Everything else is
 *  treated as success (0) -- a command "succeeds" unless it explicitly fails.
 *  A COND in `if`/`while` is therefore: run the command, then test g_last_status
 *  == 0. The runner resets g_last_status to 0 before each condition command so a
 *  stale value never leaks across iterations.
 * ========================================================================= */
static int g_last_status;   /* 0 == success, non-zero == failure */

/* Forward decl: the control-flow block interpreter (defined far below, after
 * dispatch_one). Declared up here so the `sh` script-runner builtin can call
 * it even though both live above run_block's definition. */
static void run_block(const char *text);

/* Character grid: spaces are treated as blank cells. */
static char grid[MAX_ROWS][MAX_COLS];
static u32  grid_color[MAX_ROWS][MAX_COLS];   /* per-cell foreground color */
static u32  g_cur_color;     /* color used by the next grid_putchar     */
static int  g_cols;          /* derived from the granted window width  */
static int  g_rows;          /* derived from the granted window height */
static int  cur_row;
static int  cur_col;

/* Color constants for prompt and highlights. */
#define CLR_DEFAULT   FG_COLOR
#define CLR_PROMPT    0xFF50C878u   /* green   -- user prompt   */
#define CLR_ROOT      0xFFE05050u   /* red     -- root prompt   */
#define CLR_PATH      0xFF60A0D0u   /* blue    -- path in prompt */
#define CLR_BANNER    0xFF80B0E0u   /* softer blue -- banner     */
#define CLR_BANNERHI  0xFFE0A050u   /* warm amber -- banner highlight */

static void grid_clear(void) {
    for (int r = 0; r < MAX_ROWS; r++)
        for (int c = 0; c < MAX_COLS; c++) {
            grid[r][c] = ' ';
            grid_color[r][c] = CLR_DEFAULT;
        }
    cur_row = 0;
    cur_col = 0;
    g_cur_color = CLR_DEFAULT;
}

/* Scroll the grid up one line; clear the freed bottom line. */
static void grid_scroll_up(void) {
    for (int r = 1; r < g_rows; r++)
        for (int c = 0; c < g_cols; c++) {
            grid[r - 1][c] = grid[r][c];
            grid_color[r - 1][c] = grid_color[r][c];
        }
    for (int c = 0; c < g_cols; c++) {
        grid[g_rows - 1][c] = ' ';
        grid_color[g_rows - 1][c] = CLR_DEFAULT;
    }
}

/* Move cursor to start of next row, scrolling if needed. */
static void grid_newline(void) {
    cur_col = 0;
    cur_row++;
    if (cur_row >= g_rows) {
        grid_scroll_up();
        cur_row = g_rows - 1;
    }
}

/* Write one printable char at the cursor, advancing with wrap + scroll. */
static void grid_putchar(char ch) {
    /* When capturing for redirection, append to g_cap instead of drawing.
     * Bounded: silently drop once the buffer is full. */
    if (g_cap_on) {
        if (g_cap_len < CAP_MAX) g_cap[g_cap_len++] = ch;
        return;
    }
    if (ch == '\n') { grid_newline(); return; }
    if (ch == '\t') { do { grid_putchar(' '); } while (cur_col % 4 != 0); return; }
    if (cur_col >= g_cols) grid_newline();
    grid[cur_row][cur_col] = ch;
    grid_color[cur_row][cur_col] = g_cur_color;
    cur_col++;
    if (cur_col >= g_cols) grid_newline();
}

/* Erase the character left of the cursor (does not cross row boundaries). */
static void grid_backspace(void) {
    if (cur_col > 0) {
        cur_col--;
        grid[cur_row][cur_col] = ' ';
        grid_color[cur_row][cur_col] = CLR_DEFAULT;
    } else if (cur_row > 0) {
        cur_row--;
        cur_col = g_cols - 1;
        grid[cur_row][cur_col] = ' ';
        grid_color[cur_row][cur_col] = CLR_DEFAULT;
    }
}

/* Write a NUL-terminated string through the grid, honouring '\n' and '\t'. */
static void grid_puts(const char *s) {
    for (; *s; s++) grid_putchar(*s);
}

/* Print an unsigned long to the grid. */
static void grid_put_unum(unsigned long n) {
    char b[24]; int i = 0;
    do { b[i++] = (char)('0' + (n % 10)); n /= 10; } while (n > 0);
    while (i > 0) grid_putchar(b[--i]);
}

/* Print a signed long to the grid. */
static void grid_put_num(long n) {
    if (n < 0) { grid_putchar('-'); grid_put_unum((unsigned long)-n); }
    else        grid_put_unum((unsigned long)n);
}

/* Print n with a minimum field width, right-aligned with spaces. */
static void grid_put_unum_w(unsigned long n, int width) {
    char b[24]; int i = 0;
    do { b[i++] = (char)('0' + (n % 10)); n /= 10; } while (n > 0);
    for (int sp = width - i; sp > 0; sp--) grid_putchar(' ');
    while (i > 0) grid_putchar(b[--i]);
}

/* Translate a negative errno (from a syscall return) into a short human-readable
 * reason string. Covers the codes userspace is most likely to encounter. */
static const char *errno_str(long e) {
    if (e >= 0)   return "success";
    long v = -e;
    switch (v) {
        case  1: return "operation not permitted";
        case  2: return "no such file or directory";
        case  3: return "no such process";
        case  5: return "I/O error";
        case  9: return "bad file descriptor";
        case 11: return "resource temporarily unavailable";
        case 12: return "out of memory";
        case 13: return "permission denied";
        case 14: return "bad address";
        case 17: return "file exists";
        case 20: return "not a directory";
        case 21: return "is a directory";
        case 22: return "invalid argument";
        case 28: return "no space left on device";
        case 111: return "connection refused";
        case 110: return "connection timed out";
        case 104: return "connection reset";
        default: return "unknown error";
    }
}

/* Fill a clipped rectangle in the ARGB32, stride-addressed window buffer. */
static void fill_rect(u32 *buf, u32 bw, u32 bh, u32 stride_px,
                      i32 x, i32 y, i32 w, i32 h, u32 color) {
    i32 x1 = x < 0 ? 0 : x;
    i32 y1 = y < 0 ? 0 : y;
    i32 x2 = x + w;
    i32 y2 = y + h;
    if (x2 > (i32)bw) x2 = (i32)bw;
    if (y2 > (i32)bh) y2 = (i32)bh;
    if (x1 >= x2 || y1 >= y2) return;
    for (i32 yy = y1; yy < y2; yy++) {
        u32 *row = buf + (u32)yy * stride_px;
        for (i32 xx = x1; xx < x2; xx++) row[xx] = color;
    }
}

/* Render the entire grid + cursor block into the window, then commit. */
static void render(wl_window *win, u32 stride_px) {
    fill_rect(win->pixels, win->w, win->h, stride_px,
              0, 0, (i32)win->w, (i32)win->h, BG_COLOR);

    for (int r = 0; r < g_rows; r++) {
        for (int c = 0; c < g_cols; c++) {
            char ch = grid[r][c];
            if (ch == ' ' || ch == 0) continue;
            font_draw_char(win->pixels, (int)stride_px,
                           (int)win->w, (int)win->h,
                           c * FONT_W, r * FONT_H, ch, grid_color[r][c]);
        }
    }

    fill_rect(win->pixels, win->w, win->h, stride_px,
              cur_col * FONT_W, cur_row * FONT_H, FONT_W, FONT_H, CURSOR_COLOR);

    wl_commit(win);
}

/* =========================================================================
 *  Command history ring buffer
 * ========================================================================= */

#define HIST_MAX   16
#define HIST_ENTRY 256
static char hist_ring[HIST_MAX][HIST_ENTRY];
static int  hist_count;   /* total entries pushed (wraps at HIST_MAX) */

/* Push a non-empty line into the ring. */
static void hist_push(const char *line) {
    if (!line || !line[0]) return;
    int slot = hist_count % HIST_MAX;
    k_strlcpy(hist_ring[slot], line, HIST_ENTRY);
    hist_count++;
}

/*
 * Recall offset from the end: offset=0 → most recent, offset=1 → second-most,
 * etc. Returns NULL if there is no such entry.
 */
static const char *hist_get(int offset) {
    if (offset < 0 || offset >= hist_count || offset >= HIST_MAX) return (void *)0;
    int idx = (hist_count - 1 - offset) % HIST_MAX;
    return hist_ring[idx];
}

/* =========================================================================
 *  In-shell environment variable store
 *  A flat fixed table of NAME/VALUE pairs.  `env` lists, `export NAME=VALUE`
 *  (or plain `set`) assigns, and `$NAME` is expanded in command arguments by
 *  expand_vars() before the line is parsed.  Nothing is shared with the kernel;
 *  this is purely the shell's own scratch state.
 * ========================================================================= */

#define ENV_MAX        32
#define ENV_NAME_MAX   64
#define ENV_VAL_MAX    192
static char env_name[ENV_MAX][ENV_NAME_MAX];
static char env_val[ENV_MAX][ENV_VAL_MAX];
static int  env_count;

/* Find the slot index for `name`, or -1 if it is not set. */
static int env_find(const char *name) {
    for (int i = 0; i < env_count; i++)
        if (k_streq(env_name[i], name)) return i;
    return -1;
}

/* Look up a value by name; returns "" when the variable is unset. */
static const char *env_get(const char *name) {
    int i = env_find(name);
    return (i >= 0) ? env_val[i] : "";
}

/* Set NAME=value, overwriting an existing entry or appending a new one.
 * Silently ignores empty names and a full table. */
static void env_set(const char *name, const char *value) {
    if (!name || !name[0]) return;
    int i = env_find(name);
    if (i < 0) {
        if (env_count >= ENV_MAX) return;
        i = env_count++;
        k_strlcpy(env_name[i], name, ENV_NAME_MAX);
    }
    k_strlcpy(env_val[i], value, ENV_VAL_MAX);
}

/* =========================================================================
 *  Shell: line buffer + command interpreter
 * ========================================================================= */

#define LINE_MAX  256
static char line_buf[LINE_MAX];
static int  line_len;

/* History navigation state: how many steps back we've scrolled (-1 = off). */
static int hist_nav;   /* starts at -1 (not navigating) */

/* Absolute current working directory; always starts with '/', no trailing
 * slash except for the root "/".  Used by the prompt and all fs builtins. */
static char g_cwd[256] = "/";

/* Write a string in a specific color, then restore default. */
static void grid_puts_color(const char *s, u32 color) {
    u32 prev = g_cur_color;
    g_cur_color = color;
    grid_puts(s);
    g_cur_color = prev;
}

/* Colored bash-like prompt:  root@aos:<cwd>$  */
static void shell_prompt(void) {
    grid_puts_color("root", CLR_PROMPT);
    grid_puts_color("@", CLR_PROMPT);
    grid_puts_color("aos", CLR_PROMPT);
    grid_puts_color(":", CLR_DEFAULT);
    grid_puts_color(g_cwd, CLR_PATH);
    grid_puts_color("$ ", CLR_DEFAULT);
}

/* Skip leading spaces. */
static const char *skip_spaces(const char *p) {
    while (*p == ' ' || *p == '\t') p++;
    return p;
}

/* Extract the first whitespace-delimited token from *p into out[cap];
 * advances *p past the token. Returns token length (0 if none). */
static int next_token(const char **p, char *out, int cap) {
    const char *s = skip_spaces(*p);
    int n = 0;
    while (*s && *s != ' ' && *s != '\t' && n < cap - 1) out[n++] = *s++;
    out[n] = '\0';
    *p = s;
    return n;
}

/* ---- built-in command implementations ---- */

static void cmd_help(void) {
    grid_puts("Built-in commands:\n");
    grid_puts("  help              show this list\n");
    grid_puts("  echo <args...>    print arguments\n");
    grid_puts("  clear             clear the screen\n");
    grid_puts("  uptime            time since boot\n");
    grid_puts("  ls [path]         list a directory\n");
    grid_puts("  cat <file>        print file contents\n");
    grid_puts("  cp <src> <dst>    copy a file\n");
    grid_puts("  grep <pat> <file> print lines matching pattern\n");
    grid_puts("  wc <file>         count lines/words/bytes\n");
    grid_puts("  head <file>       print first 10 lines\n");
    grid_puts("  tail <file>       print last 10 lines\n");
    grid_puts("  pwd               print working directory\n");
    grid_puts("  cd [dir]          change directory (no arg or ~ = /)\n");
    grid_puts("  mkdir <dir>       create a directory\n");
    grid_puts("  touch <file>      create an empty file\n");
    grid_puts("  rm <file>         remove a file\n");
    grid_puts("  mv <src> <dst>    rename/move a file\n");
    grid_puts("  write <f> <text>  write text into a file\n");
    grid_puts("  date              ms since boot\n");
    grid_puts("  uname             print OS name\n");
    grid_puts("  whoami            print current user\n");
    grid_puts("  about             OS name, version and credits\n");
    grid_puts("  git <args...>     native git-like VCS\n");
    grid_puts("  run <path>        spawn an app (e.g. run bin/files)\n");
    grid_puts("  launch <path>     alias for run\n");
    grid_puts("  spawn <app>       spawn from sbin/<app>\n");
    grid_puts("  ps                list processes\n");
    grid_puts("  top               detailed process + system stats\n");
    grid_puts("  kill <pid>        SIGKILL a process\n");
    grid_puts("  mem / sysinfo     memory and system info\n");
    grid_puts("  history           show recent commands\n");
    grid_puts("  env / export / set in-shell variables ($NAME expands)\n");
    grid_puts("  stat <file>       size and type of a file\n");
    grid_puts("  cut -d X -f N <f> extract a delimited field\n");
    grid_puts("  tr A B <file>     translate/delete characters\n");
    grid_puts("  sort <file>       sort lines\n");
    grid_puts("  uniq <file>       drop adjacent duplicate lines\n");
    grid_puts("  seq N             print 1..N\n");
    grid_puts("  basename/dirname  split a path\n");
    grid_puts("  test / [ ... ]    evaluate a condition (sets status)\n");
    grid_puts("  sh / source / .   run a script file\n");
    grid_puts("  true / false      no-op success/failure\n");
    grid_puts("  yes [STR]         repeat STR (bounded)\n");
    grid_puts("  ln [-s] <s> <d>   link (falls back to copy)\n");
    grid_puts("  du [dir]          sum file sizes recursively\n");
    grid_puts("  df                memory as a pseudo-filesystem\n");
    grid_puts("Networking:\n");
    grid_puts("  wget URL [-O f]   fetch HTTP (spawns /bin/wget)\n");
    grid_puts("  curl URL          fetch HTTP body (alias for wget)\n");
    grid_puts("  ping HOST [count] ICMP echo (spawns /sbin|/bin/ping)\n");
    grid_puts("  nc HOST PORT [tx] netcat (spawns /bin/nc)\n");
    grid_puts("  ifconfig / ip     show local NIC info (ip/gw/mac)\n");
    grid_puts("  host / nslookup   DNS guidance (use wget to resolve)\n");
    grid_puts("  <cmd>             else spawn /bin/<cmd>\n");
    grid_puts("  (up-arrow)        recall previous command\n");
    grid_puts("Syntax:\n");
    grid_puts("  cmd > file        redirect output (truncate)\n");
    grid_puts("  cmd >> file       redirect output (append)\n");
    grid_puts("  cmd1 ; cmd2       run commands in sequence\n");
    grid_puts("  $(cmd)            substitute a command's output\n");
    grid_puts("  if C; then ..; fi conditional (else optional)\n");
    grid_puts("  for V in a b; do .. done   iterate words\n");
    grid_puts("  while C; do .. done        loop (bounded)\n");
}

static void cmd_echo(const char *args) {
    const char *p = skip_spaces(args);
    grid_puts(p);
    grid_putchar('\n');
}

static void cmd_uptime(void) {
    long ms = sc(SYS_GET_TICKS_MS, 0, 0, 0, 0, 0, 0);
    if (ms < 0) ms = 0;
    unsigned long total = (unsigned long)ms;
    unsigned long secs = total / 1000UL;
    unsigned long rem_ms = total % 1000UL;
    unsigned long mins = secs / 60UL;
    unsigned long s = secs % 60UL;
    unsigned long hrs = mins / 60UL;
    unsigned long m = mins % 60UL;

    grid_puts("up ");
    grid_put_unum(hrs);  grid_puts("h ");
    grid_put_unum(m);    grid_puts("m ");
    grid_put_unum(s);    grid_puts("s (");
    grid_put_unum(total); grid_puts(" ms / ");
    grid_put_unum(rem_ms); grid_puts(" ms frac)\n");
}

/* =========================================================================
 *  Current working directory + path resolution
 *  (g_cwd itself is defined earlier, near shell_prompt())
 * ========================================================================= */

/* Scratch buffer that resolve_path() returns a pointer into. 16-aligned and
 * sized to KPATH_MAX so copy_from_user has enough readable bytes. */
static char g_pathbuf[KPATH_MAX] __attribute__((aligned(16)));

/*
 * Resolve `arg` against g_cwd into g_pathbuf and return a pointer to it.
 *   - empty/NULL arg            -> g_cwd
 *   - arg starting with '/'     -> absolute
 *   - otherwise                 -> g_cwd + "/" + arg
 * The joined string is then normalized in-place: repeated '/' collapse,
 * "." components drop, ".." pops one component (never above "/"). The result
 * always starts with '/' and has no trailing slash (except root "/").
 */
static const char *resolve_path(const char *arg) {
    if (!arg) arg = "";
    while (*arg == ' ' || *arg == '\t') arg++;

    /* zero the whole buffer (kernel may read all of it) */
    for (int i = 0; i < KPATH_MAX; i++) g_pathbuf[i] = '\0';

    if (arg[0] == '\0') {
        k_strlcpy(g_pathbuf, g_cwd, KPATH_MAX);
        return g_pathbuf;
    }

    int n = 0;
    if (arg[0] == '/') {
        /* absolute: just copy arg */
        while (arg[n] && n < KPATH_MAX - 1) { g_pathbuf[n] = arg[n]; n++; }
        g_pathbuf[n] = '\0';
    } else {
        /* relative: g_cwd + "/" + arg */
        int c = 0;
        while (g_cwd[c] && n < KPATH_MAX - 1) g_pathbuf[n++] = g_cwd[c++];
        /* add a separator unless g_cwd already ended in '/' (root case) */
        if (n == 0 || g_pathbuf[n - 1] != '/') {
            if (n < KPATH_MAX - 1) g_pathbuf[n++] = '/';
        }
        int a = 0;
        while (arg[a] && n < KPATH_MAX - 1) g_pathbuf[n++] = arg[a++];
        g_pathbuf[n] = '\0';
    }

    /*
     * Normalize in place. We rebuild a canonical path into a separate static
     * buffer using a component-offset stack so ".." can pop, then copy back.
     */
    static char norm[KPATH_MAX] __attribute__((aligned(16)));
    static int seps[KPATH_MAX / 2];   /* offsets of each component's leading '/' */
    int depth = 0;
    int len = 0;
    norm[len++] = '/';         /* result always starts at root */

    const char *s = g_pathbuf;
    while (*s == '/') s++;     /* skip leading slashes */
    while (*s) {
        /* read one component into [start,end) */
        const char *comp = s;
        int clen = 0;
        while (*s && *s != '/') { s++; clen++; }

        if (clen == 1 && comp[0] == '.') {
            /* "." -> drop */
        } else if (clen == 2 && comp[0] == '.' && comp[1] == '.') {
            /* ".." -> pop one component (never above root) */
            if (depth > 0) {
                depth--;
                len = seps[depth];   /* truncate back to that separator */
                if (len < 1) len = 1;
            }
        } else if (clen > 0) {
            /* real component: append "/comp" */
            if (depth < (int)(sizeof(seps) / sizeof(seps[0]))) {
                seps[depth++] = len;   /* remember where this comp starts */
            }
            if (len > 1) {             /* add separator unless right after root */
                if (len < KPATH_MAX - 1) norm[len++] = '/';
            }
            for (int i = 0; i < clen && len < KPATH_MAX - 1; i++)
                norm[len++] = comp[i];
        }

        while (*s == '/') s++;   /* skip separators between components */
    }
    norm[len] = '\0';

    k_strlcpy(g_pathbuf, norm, KPATH_MAX);
    return g_pathbuf;
}

static void cmd_ls(const char *args) {
    char tok[LINE_MAX];
    const char *p = args;
    next_token(&p, tok, sizeof(tok));   /* empty tok -> resolve_path uses g_cwd */

    /* Resolve relative to the cwd; absolute paths come back normalized. */
    const char *path = resolve_path(tok);

    long dfd = sc(SYS_OPENDIR, (long)path, 0, 0, 0, 0, 0);
    if (dfd < 0) {
        grid_puts("ls: ");
        grid_puts(path);
        grid_puts(": ");
        grid_puts(errno_str(dfd));
        grid_putchar('\n');
        return;
    }

    struct k_dirent de;
    int count = 0;
    for (;;) {
        long r = sc(SYS_READDIR, dfd, (long)&de, 0, 0, 0, 0);
        if (r != 0) break;          /* 0 = entry filled; <0 = end/error */
        de.d_name[NAME_MAX_ - 1] = '\0';
        if (de.d_name[0] == '\0') continue;
        grid_puts(de.d_name);
        if (de.d_type == DT_DIR) grid_putchar('/');
        grid_putchar('\n');
        count++;
    }
    sc(SYS_CLOSEDIR, dfd, 0, 0, 0, 0, 0);

    if (count == 0) grid_puts("(empty)\n");
}

static char cat_buf[512];

static void cmd_cat(const char *args) {
    char tok[LINE_MAX];
    const char *p = args;
    if (!next_token(&p, tok, sizeof(tok))) {
        grid_puts("cat: missing file operand\n");
        return;
    }

    const char *path = resolve_path(tok);

    long fd = sc(SYS_OPEN, (long)path, O_RDONLY, 0, 0, 0, 0);
    if (fd < 0) {
        grid_puts("cat: ");
        grid_puts(path);
        grid_puts(": ");
        grid_puts(errno_str(fd));
        grid_putchar('\n');
        return;
    }

    for (;;) {
        long n = sc(SYS_READ, fd, (long)cat_buf, (long)sizeof(cat_buf), 0, 0, 0);
        if (n <= 0) break;
        for (long i = 0; i < n; i++) {
            char ch = cat_buf[i];
            if (ch == '\r') continue;
            grid_putchar(ch);
        }
    }
    sc(SYS_CLOSE, fd, 0, 0, 0, 0, 0);
    /* Ensure output ends on its own line. */
    if (cur_col != 0) grid_putchar('\n');
}

/* sys_spawn copies 127 bytes; a 128-byte buffer keeps copy_from_user safe. */
static char spawn_path[128];

static void cmd_run(const char *args) {
    char tok[LINE_MAX];
    const char *p = args;
    if (!next_token(&p, tok, sizeof(tok))) {
        grid_puts("run: usage: run <path>   (e.g. run bin/files)\n");
        return;
    }

    for (int i = 0; i < (int)sizeof(spawn_path); i++) spawn_path[i] = '\0';
    k_strlcpy(spawn_path, tok, sizeof(spawn_path));

    long pid = sc(SYS_SPAWN, (long)spawn_path, 0, 0, 0, 0, 0);
    if (pid <= 0) {
        grid_puts("run: cannot execute '");
        grid_puts(spawn_path);
        grid_puts("': ");
        grid_puts(errno_str(pid));
        grid_puts(" (");
        grid_put_num(pid);
        grid_puts(")\n");
        return;
    }
    grid_puts("spawned '");
    grid_puts(spawn_path);
    grid_puts("' as pid ");
    grid_put_unum((unsigned long)pid);
    grid_putchar('\n');
}

/*
 * spawn <app>  -- prepend "sbin/" and call SYS_SPAWN.
 * The path is routed through spawn_path (128 bytes) for kernel safety.
 */
static void cmd_spawn(const char *args) {
    char tok[LINE_MAX];
    const char *p = args;
    if (!next_token(&p, tok, sizeof(tok))) {
        grid_puts("spawn: usage: spawn <app>   (e.g. spawn shell)\n");
        return;
    }

    /* Build "sbin/<app>" in spawn_path. */
    for (int i = 0; i < (int)sizeof(spawn_path); i++) spawn_path[i] = '\0';
    /* prefix */
    const char *pre = "sbin/";
    int pi = 0;
    while (pre[pi] && pi < (int)sizeof(spawn_path) - 1)
        spawn_path[pi] = pre[pi++];
    /* append app name */
    k_strlcpy(spawn_path + pi, tok, (int)sizeof(spawn_path) - pi);

    long pid = sc(SYS_SPAWN, (long)spawn_path, 0, 0, 0, 0, 0);
    if (pid <= 0) {
        grid_puts("spawn: cannot execute '");
        grid_puts(spawn_path);
        grid_puts("': ");
        grid_puts(errno_str(pid));
        grid_puts(" (");
        grid_put_num(pid);
        grid_puts(")\n");
        return;
    }
    grid_puts("spawned '");
    grid_puts(spawn_path);
    grid_puts("' as pid ");
    grid_put_unum((unsigned long)pid);
    grid_putchar('\n');
}

/* -----------------------------------------------------------------------
 * State name helper (matches common kernel process state values).
 * ----------------------------------------------------------------------- */
static const char *state_name(u32 state) {
    switch (state) {
        case 0:  return "READY  ";
        case 1:  return "RUN    ";
        case 2:  return "SLEEP  ";
        case 3:  return "WAIT   ";
        case 4:  return "ZOMBIE ";
        case 5:  return "STOP   ";
        default: return "?      ";
    }
}

/* -----------------------------------------------------------------------
 * ps -- list processes via SYS_PROCLIST=44
 *
 *   sc(44, (long)buf, max_entries, 0, 0, 0, 0) → count filled, or <0 on error
 *
 * procinfo_t is 48 bytes: {u32 pid, parent_pid, state, flags; char name[32]}
 * ----------------------------------------------------------------------- */
#define PS_MAX 64
static procinfo_t ps_buf[PS_MAX];

static void cmd_ps(void) {
    long count = sc(SYS_PROCLIST, (long)ps_buf, PS_MAX, 0, 0, 0, 0);
    if (count < 0) {
        grid_puts("ps: SYS_PROCLIST not available (err ");
        grid_put_num(count);
        grid_puts(")\n");
        return;
    }
    if (count == 0) {
        grid_puts("ps: no processes\n");
        return;
    }

    grid_puts("  PID  PPID  STATE    NAME\n");
    grid_puts("-----  ----  -------  ----------------\n");
    for (long i = 0; i < count; i++) {
        procinfo_t *pi = &ps_buf[i];
        pi->name[31] = '\0';   /* ensure NUL terminated */
        grid_put_unum_w((unsigned long)pi->pid,        5);
        grid_puts("  ");
        grid_put_unum_w((unsigned long)pi->parent_pid, 4);
        grid_puts("  ");
        grid_puts(state_name(pi->state));
        grid_puts("  ");
        grid_puts(pi->name[0] ? pi->name : "(unnamed)");
        grid_putchar('\n');
    }
    grid_puts("(");
    grid_put_unum((unsigned long)count);
    grid_puts(" process(es))\n");
}

/* -----------------------------------------------------------------------
 * top -- detailed view via SYS_PROC_QUERY=60 + SYS_SYSINFO=62;
 *         falls back to plain ps if these syscalls return errors.
 *
 *   SYS_PROC_QUERY: sc(60, pid, (long)&detail, 0, ...) → 0 ok / <0 err
 *   SYS_SYSINFO:    sc(62, (long)&si,  0,       0, ...) → 0 ok / <0 err
 *
 * proc_detail_t: {u32 pid,ppid,state,prio; u64 cpu_ticks; u32 mem_pages,
 *                 vma_count; char name[32]}   (64 bytes)
 * sysinfo_t:     {u64 total_mem,free_mem,uptime_ms; u32 proc_count,_pad}
 * ----------------------------------------------------------------------- */
static sysinfo_t top_si;
static proc_detail_t top_det;

static void cmd_top(void) {
    /* ---- system info ---- */
    long si_ok = sc(SYS_SYSINFO, (long)&top_si, 0, 0, 0, 0, 0);

    /* ---- get process list first ---- */
    long count = sc(SYS_PROCLIST, (long)ps_buf, PS_MAX, 0, 0, 0, 0);
    if (count < 0) {
        grid_puts("top: SYS_PROCLIST not available; err=");
        grid_put_num(count);
        grid_putchar('\n');
        return;
    }

    /* Print system stats header if SYS_SYSINFO worked. */
    if (si_ok == 0) {
        grid_puts("mem: ");
        grid_put_unum(top_si.free_mem / 1024UL);
        grid_puts(" KB free / ");
        grid_put_unum(top_si.total_mem / 1024UL);
        grid_puts(" KB total   procs: ");
        grid_put_unum((unsigned long)top_si.proc_count);
        grid_puts("   up ");
        grid_put_unum(top_si.uptime_ms / 1000UL);
        grid_puts("s\n");
    } else {
        grid_puts("(sysinfo n/a)  ");
        grid_put_unum((unsigned long)count);
        grid_puts(" process(es)\n");
    }

    /* Try to get detailed per-process info. */
    int have_detail = 0;
    /* probe first process to see if SYS_PROC_QUERY works */
    if (count > 0) {
        long r = sc(SYS_PROC_QUERY, (long)ps_buf[0].pid, (long)&top_det, 0, 0, 0, 0);
        have_detail = (r == 0);
    }

    if (have_detail) {
        grid_puts("  PID  PPID PRI  STATE    MEM(pg) VMA  CPUTICKS         NAME\n");
        grid_puts("-----  ---- ---  -------  ------- ---  ----------------\n");
        for (long i = 0; i < count; i++) {
            u32 pid = ps_buf[i].pid;
            long r = sc(SYS_PROC_QUERY, (long)pid, (long)&top_det, 0, 0, 0, 0);
            if (r != 0) {
                /* query failed for this pid: fall back to basic info */
                grid_put_unum_w((unsigned long)pid, 5);
                grid_puts("  (query err ");
                grid_put_num(r);
                grid_puts(")\n");
                continue;
            }
            top_det.name[31] = '\0';
            grid_put_unum_w((unsigned long)top_det.pid,       5);
            grid_puts("  ");
            grid_put_unum_w((unsigned long)top_det.ppid,      4);
            grid_puts(" ");
            grid_put_unum_w((unsigned long)top_det.prio,      3);
            grid_puts("  ");
            grid_puts(state_name(top_det.state));
            grid_puts("  ");
            grid_put_unum_w((unsigned long)top_det.mem_pages, 7);
            grid_puts(" ");
            grid_put_unum_w((unsigned long)top_det.vma_count, 3);
            grid_puts("  ");
            grid_put_unum(top_det.cpu_ticks);
            grid_puts("\t");
            grid_puts(top_det.name[0] ? top_det.name : "(unnamed)");
            grid_putchar('\n');
        }
    } else {
        /* Graceful fallback: just show proclist info (same as ps). */
        grid_puts("(proc_query n/a -- showing ps output)\n");
        grid_puts("  PID  PPID  STATE    NAME\n");
        grid_puts("-----  ----  -------  ----------------\n");
        for (long i = 0; i < count; i++) {
            procinfo_t *pi = &ps_buf[i];
            pi->name[31] = '\0';
            grid_put_unum_w((unsigned long)pi->pid,        5);
            grid_puts("  ");
            grid_put_unum_w((unsigned long)pi->parent_pid, 4);
            grid_puts("  ");
            grid_puts(state_name(pi->state));
            grid_puts("  ");
            grid_puts(pi->name[0] ? pi->name : "(unnamed)");
            grid_putchar('\n');
        }
    }
    grid_puts("(");
    grid_put_unum((unsigned long)count);
    grid_puts(" process(es))\n");
}

/* -----------------------------------------------------------------------
 * kill <pid> -- sc(26, pid, 9, 0, ...) SIGKILL.  Refuse pid 0 and 1.
 * ----------------------------------------------------------------------- */
static void cmd_kill(const char *args) {
    const char *p = skip_spaces(args);
    if (!*p) {
        grid_puts("kill: usage: kill <pid>\n");
        return;
    }
    long pid = k_atoi(p);
    if (pid < 0) {
        grid_puts("kill: invalid pid\n");
        return;
    }
    if (pid == 0 || pid == 1) {
        grid_puts("kill: refusing to kill pid ");
        grid_put_num(pid);
        grid_puts(" (protected)\n");
        return;
    }
    long r = sc(SYS_KILL, pid, 9, 0, 0, 0, 0);
    if (r < 0) {
        grid_puts("kill: failed (err ");
        grid_put_num(r);
        grid_puts(")\n");
    } else {
        grid_puts("killed pid ");
        grid_put_num(pid);
        grid_putchar('\n');
    }
}

/* -----------------------------------------------------------------------
 * mem / sysinfo -- sc(62, &si, 0, 0, ...) → 0 ok / <0 not yet wired.
 * ----------------------------------------------------------------------- */
static sysinfo_t mem_si;

static void cmd_mem(void) {
    long r = sc(SYS_SYSINFO, (long)&mem_si, 0, 0, 0, 0, 0);
    if (r < 0) {
        grid_puts("sysinfo: n/a (err ");
        grid_put_num(r);
        grid_puts(")\n");
        return;
    }
    grid_puts("total mem : ");
    grid_put_unum(mem_si.total_mem / 1024UL);
    grid_puts(" KB\n");
    grid_puts("free  mem : ");
    grid_put_unum(mem_si.free_mem / 1024UL);
    grid_puts(" KB\n");
    grid_puts("used  mem : ");
    grid_put_unum((mem_si.total_mem - mem_si.free_mem) / 1024UL);
    grid_puts(" KB\n");
    grid_puts("uptime    : ");
    grid_put_unum(mem_si.uptime_ms / 1000UL);
    grid_puts("s (");
    grid_put_unum(mem_si.uptime_ms);
    grid_puts(" ms)\n");
    grid_puts("processes : ");
    grid_put_unum((unsigned long)mem_si.proc_count);
    grid_putchar('\n');
}

/* -----------------------------------------------------------------------
 * history -- list the ring buffer most-recent-first.
 * ----------------------------------------------------------------------- */
static void cmd_history(void) {
    int total = hist_count < HIST_MAX ? hist_count : HIST_MAX;
    if (total == 0) {
        grid_puts("(no history)\n");
        return;
    }
    for (int i = 0; i < total; i++) {
        const char *entry = hist_get(i);
        if (!entry) break;
        grid_puts("  ");
        grid_put_unum_w((unsigned long)(hist_count - i), 3);
        grid_puts("  ");
        grid_puts(entry);
        grid_putchar('\n');
    }
}

/* =========================================================================
 *  Filesystem builtins (cwd-aware)
 * ========================================================================= */

/* pwd -- print the current working directory. */
static void cmd_pwd(void) {
    grid_puts(g_cwd);
    grid_putchar('\n');
}

/*
 * cd [dir] -- change the working directory. No arg (or "~") -> root "/".
 * Verifies the target is a real directory by opening it as a dir first.
 */
static void cmd_cd(const char *args) {
    char tok[LINE_MAX];
    const char *p = args;
    int have = next_token(&p, tok, sizeof(tok));

    if (!have || k_streq(tok, "~")) {
        g_cwd[0] = '/';
        g_cwd[1] = '\0';
        return;
    }

    const char *path = resolve_path(tok);
    long fd = sc(SYS_OPENDIR, (long)path, 0, 0, 0, 0, 0);
    if (fd >= 0) {
        sc(SYS_CLOSEDIR, fd, 0, 0, 0, 0, 0);
        k_strlcpy(g_cwd, path, sizeof(g_cwd));
    } else {
        grid_puts("cd: ");
        grid_puts(tok);
        grid_puts(": no such directory\n");
    }
}

/* mkdir <dir> -- create a directory (parents created recursively by kernel). */
static void cmd_mkdir(const char *args) {
    char tok[LINE_MAX];
    const char *p = args;
    if (!next_token(&p, tok, sizeof(tok))) {
        grid_puts("mkdir: missing operand\n");
        return;
    }
    const char *path = resolve_path(tok);
    long r = sc(SYS_MKDIR, (long)path, 0755, 0, 0, 0, 0);
    if (r < 0) {
        grid_puts("mkdir: cannot create '");
        grid_puts(path);
        grid_puts("'\n");
    }
}

/* touch <file> -- create an empty file (or leave existing one untouched). */
static void cmd_touch(const char *args) {
    char tok[LINE_MAX];
    const char *p = args;
    if (!next_token(&p, tok, sizeof(tok))) {
        grid_puts("touch: missing file operand\n");
        return;
    }
    const char *path = resolve_path(tok);
    long fd = sc(SYS_OPEN, (long)path, O_WRONLY | O_CREAT, 0644, 0, 0, 0);
    if (fd >= 0) {
        sc(SYS_CLOSE, fd, 0, 0, 0, 0, 0);
    } else {
        grid_puts("touch: cannot create '");
        grid_puts(path);
        grid_puts("'\n");
    }
}

/* rm <file> -- unlink a file. */
static void cmd_rm(const char *args) {
    char tok[LINE_MAX];
    const char *p = args;
    if (!next_token(&p, tok, sizeof(tok))) {
        grid_puts("rm: missing operand\n");
        return;
    }
    const char *path = resolve_path(tok);
    long r = sc(SYS_UNLINK, (long)path, 0, 0, 0, 0, 0);
    if (r < 0) {
        grid_puts("rm: cannot remove '");
        grid_puts(path);
        grid_puts("'\n");
    }
}

/*
 * mv <src> <dst> -- rename. resolve_path() reuses g_pathbuf, so we resolve
 * src into a dedicated 16-aligned static buffer first, then resolve dst.
 */
static char mv_src[KPATH_MAX] __attribute__((aligned(16)));

static void cmd_mv(const char *args) {
    char srctok[LINE_MAX];
    char dsttok[LINE_MAX];
    const char *p = args;
    if (!next_token(&p, srctok, sizeof(srctok)) ||
        !next_token(&p, dsttok, sizeof(dsttok))) {
        grid_puts("mv: usage: mv <src> <dst>\n");
        return;
    }

    /* Resolve src, copy out of g_pathbuf so the dst resolve can't clobber it. */
    for (int i = 0; i < KPATH_MAX; i++) mv_src[i] = '\0';
    k_strlcpy(mv_src, resolve_path(srctok), KPATH_MAX);

    const char *dst = resolve_path(dsttok);

    long r = sc(SYS_RENAME, (long)mv_src, (long)dst, 0, 0, 0, 0);
    if (r < 0) {
        grid_puts("mv: cannot move '");
        grid_puts(mv_src);
        grid_puts("' to '");
        grid_puts(dst);
        grid_puts("'\n");
    }
}

/*
 * write <file> <text...> -- truncate <file> and write the remaining line text
 * (everything after the filename) followed by a newline.
 */
static void cmd_write(const char *args) {
    char tok[LINE_MAX];
    const char *p = args;
    if (!next_token(&p, tok, sizeof(tok))) {
        grid_puts("write: usage: write <file> <text...>\n");
        return;
    }

    const char *text = skip_spaces(p);   /* remainder of the line is content */
    const char *path = resolve_path(tok);

    long fd = sc(SYS_OPEN, (long)path, O_WRONLY | O_CREAT | O_TRUNC, 0644, 0, 0, 0);
    if (fd < 0) {
        grid_puts("write: ");
        grid_puts(path);
        grid_puts(": ");
        grid_puts(errno_str(fd));
        grid_putchar('\n');
        return;
    }

    long tlen = (long)k_strlen(text);
    if (tlen > 0) {
        long w = sc(SYS_WRITE, fd, (long)text, tlen, 0, 0, 0);
        if (w < 0) {
            grid_puts("write: ");
            grid_puts(path);
            grid_puts(": ");
            grid_puts(errno_str(w));
            grid_putchar('\n');
            sc(SYS_CLOSE, fd, 0, 0, 0, 0, 0);
            return;
        }
    }
    char nl = '\n';
    sc(SYS_WRITE, fd, (long)&nl, 1, 0, 0, 0);
    sc(SYS_CLOSE, fd, 0, 0, 0, 0, 0);
}

/* =========================================================================
 *  Text-utility builtins (cp / grep / wc / head / tail) + info builtins
 * ========================================================================= */

/* Shared 64 KB scratch for reading whole files. Static (stack is tiny). */
#define FILE_BUF_MAX 65536
static char g_filebuf[FILE_BUF_MAX] __attribute__((aligned(16)));

/*
 * Read the whole file at `path` into g_filebuf (up to FILE_BUF_MAX bytes).
 * Returns bytes read (>=0), or -1 if the file could not be opened, or -2 if
 * the file did not fit (it is larger than FILE_BUF_MAX).
 */
static long slurp_file(const char *path) {
    long fd = sc(SYS_OPEN, (long)path, O_RDONLY, 0, 0, 0, 0);
    if (fd < 0) return -1;
    long total = 0;
    for (;;) {
        long room = FILE_BUF_MAX - total;
        if (room <= 0) {
            /* Probe for at least one more byte to detect overflow. */
            char extra;
            long n = sc(SYS_READ, fd, (long)&extra, 1, 0, 0, 0);
            sc(SYS_CLOSE, fd, 0, 0, 0, 0, 0);
            if (n > 0) return -2;   /* file is larger than our buffer */
            return total;
        }
        long n = sc(SYS_READ, fd, (long)(g_filebuf + total), room, 0, 0, 0);
        if (n <= 0) break;
        total += n;
    }
    sc(SYS_CLOSE, fd, 0, 0, 0, 0, 0);
    return total;
}

/*
 * cp <src> <dst> -- copy src into dst. resolve_path() reuses g_pathbuf, so we
 * copy the resolved src into a dedicated static buffer before resolving dst.
 */
static char cp_src[KPATH_MAX] __attribute__((aligned(16)));

static void cmd_cp(const char *args) {
    char srctok[LINE_MAX];
    char dsttok[LINE_MAX];
    const char *p = args;
    if (!next_token(&p, srctok, sizeof(srctok)) ||
        !next_token(&p, dsttok, sizeof(dsttok))) {
        grid_puts("cp: usage: cp <src> <dst>\n");
        return;
    }

    for (int i = 0; i < KPATH_MAX; i++) cp_src[i] = '\0';
    k_strlcpy(cp_src, resolve_path(srctok), KPATH_MAX);

    long n = slurp_file(cp_src);
    if (n == -1) {
        grid_puts("cp: ");
        grid_puts(cp_src);
        grid_puts(": no such file or directory\n");
        return;
    }
    if (n == -2) {
        grid_puts("cp: ");
        grid_puts(cp_src);
        grid_puts(": file too large (exceeds 64 KB buffer)\n");
        return;
    }

    const char *dst = resolve_path(dsttok);
    long fd = sc(SYS_OPEN, (long)dst, O_WRONLY | O_CREAT | O_TRUNC, 0644, 0, 0, 0);
    if (fd < 0) {
        grid_puts("cp: cannot create '");
        grid_puts(dst);
        grid_puts("': ");
        grid_puts(errno_str(fd));
        grid_putchar('\n');
        return;
    }
    if (n > 0) {
        long w = sc(SYS_WRITE, fd, (long)g_filebuf, n, 0, 0, 0);
        if (w < 0) grid_puts("cp: write error\n");
    }
    sc(SYS_CLOSE, fd, 0, 0, 0, 0, 0);
}

/* substring test: does haystack[0..hlen) contain NUL-terminated needle? */
static int contains_sub(const char *hay, long hlen, const char *needle) {
    long nlen = (long)k_strlen(needle);
    if (nlen == 0) return 1;
    if (nlen > hlen) return 0;
    for (long i = 0; i + nlen <= hlen; i++) {
        long j = 0;
        while (j < nlen && hay[i + j] == needle[j]) j++;
        if (j == nlen) return 1;
    }
    return 0;
}

/* grep <pattern> <file> -- print each '\n'-terminated line containing pattern. */
static void cmd_grep(const char *args) {
    char pat[LINE_MAX];
    char ftok[LINE_MAX];
    const char *p = args;
    if (!next_token(&p, pat, sizeof(pat)) ||
        !next_token(&p, ftok, sizeof(ftok))) {
        grid_puts("grep: usage: grep <pattern> <file>\n");
        return;
    }

    const char *path = resolve_path(ftok);
    long n = slurp_file(path);
    if (n == -1) {
        grid_puts("grep: ");
        grid_puts(path);
        grid_puts(": no such file or directory\n");
        return;
    }
    if (n == -2) {
        grid_puts("grep: ");
        grid_puts(path);
        grid_puts(": file too large (exceeds 64 KB buffer)\n");
        return;
    }

    long start = 0;
    for (long i = 0; i <= n; i++) {
        if (i == n || g_filebuf[i] == '\n') {
            long llen = i - start;
            if (i < n || llen > 0) {          /* skip trailing empty line */
                if (contains_sub(g_filebuf + start, llen, pat)) {
                    for (long k = 0; k < llen; k++) {
                        char ch = g_filebuf[start + k];
                        if (ch != '\r') grid_putchar(ch);
                    }
                    grid_putchar('\n');
                }
            }
            start = i + 1;
        }
    }
}

/* wc <file> -- print "<lines> <words> <bytes> <file>". */
static void cmd_wc(const char *args) {
    char ftok[LINE_MAX];
    const char *p = args;
    if (!next_token(&p, ftok, sizeof(ftok))) {
        grid_puts("wc: usage: wc <file>\n");
        return;
    }

    const char *path = resolve_path(ftok);
    long n = slurp_file(path);
    if (n == -1) {
        grid_puts("wc: ");
        grid_puts(path);
        grid_puts(": no such file or directory\n");
        return;
    }
    if (n == -2) {
        grid_puts("wc: ");
        grid_puts(path);
        grid_puts(": file too large (exceeds 64 KB buffer)\n");
        return;
    }

    unsigned long lines = 0, words = 0;
    int in_word = 0;
    for (long i = 0; i < n; i++) {
        char ch = g_filebuf[i];
        if (ch == '\n') lines++;
        if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r') {
            in_word = 0;
        } else if (!in_word) {
            in_word = 1;
            words++;
        }
    }

    grid_put_unum(lines);  grid_putchar(' ');
    grid_put_unum(words);  grid_putchar(' ');
    grid_put_unum((unsigned long)n); grid_putchar(' ');
    grid_puts(ftok);
    grid_putchar('\n');
}

/* head <file> -- print the first 10 lines. */
static void cmd_head(const char *args) {
    char ftok[LINE_MAX];
    const char *p = args;
    if (!next_token(&p, ftok, sizeof(ftok))) {
        grid_puts("head: usage: head <file>\n");
        return;
    }

    const char *path = resolve_path(ftok);
    long n = slurp_file(path);
    if (n == -1) {
        grid_puts("head: ");
        grid_puts(path);
        grid_puts(": no such file or directory\n");
        return;
    }
    if (n == -2) {
        grid_puts("head: ");
        grid_puts(path);
        grid_puts(": file too large (exceeds 64 KB buffer)\n");
        return;
    }

    int lines = 0;
    for (long i = 0; i < n && lines < 10; i++) {
        char ch = g_filebuf[i];
        if (ch == '\r') continue;
        grid_putchar(ch);
        if (ch == '\n') lines++;
    }
    if (cur_col != 0 && !g_cap_on) grid_putchar('\n');
    else if (g_cap_on && n > 0 && g_filebuf[n - 1] != '\n') grid_putchar('\n');
}

/* tail <file> -- print the last 10 lines. */
static void cmd_tail(const char *args) {
    char ftok[LINE_MAX];
    const char *p = args;
    if (!next_token(&p, ftok, sizeof(ftok))) {
        grid_puts("tail: usage: tail <file>\n");
        return;
    }

    const char *path = resolve_path(ftok);
    long n = slurp_file(path);
    if (n == -1) {
        grid_puts("tail: ");
        grid_puts(path);
        grid_puts(": no such file or directory\n");
        return;
    }
    if (n == -2) {
        grid_puts("tail: ");
        grid_puts(path);
        grid_puts(": file too large (exceeds 64 KB buffer)\n");
        return;
    }
    if (n == 0) return;

    /* Walk back from the end counting newlines to find the start of the last
     * 10 lines.  A trailing newline does not count as a separate empty line. */
    long end = n;
    if (g_filebuf[end - 1] == '\n') end--;   /* ignore one trailing newline */
    int nl = 0;
    long start = 0;
    for (long i = end - 1; i >= 0; i--) {
        if (g_filebuf[i] == '\n') {
            nl++;
            if (nl == 10) { start = i + 1; break; }
        }
    }

    for (long i = start; i < n; i++) {
        char ch = g_filebuf[i];
        if (ch != '\r') grid_putchar(ch);
    }
    if (cur_col != 0 && !g_cap_on) grid_putchar('\n');
    else if (g_cap_on && g_filebuf[n - 1] != '\n') grid_putchar('\n');
}

/* date -- no wall-clock syscall; report ms since boot (SYS_GET_TICKS_MS). */
static void cmd_date(void) {
    long ms = sc(SYS_GET_TICKS_MS, 0, 0, 0, 0, 0, 0);
    if (ms < 0) ms = 0;
    grid_puts("uptime ");
    grid_put_unum((unsigned long)ms);
    grid_puts(" ms\n");
}

/* uname -- print the OS name + machine. */
static void cmd_uname(void) {
    grid_puts("AutomationOS x86_64\n");
}

/* whoami -- single-user system. */
static void cmd_whoami(void) {
    grid_puts("user\n");
}

/* about -- OS name, version and credits. */
static void cmd_about(void) {
    grid_puts("AutomationOS v0.1.0\n");
    grid_puts("A from-scratch x86_64 operating system.\n");
    grid_puts("created by fourzerofour & claude\n");
}

/* =========================================================================
 *  New coreutils-style builtins ("real hands")
 * ========================================================================= */

/* ---- env / export / set --------------------------------------------------
 * env             : list every NAME=VALUE pair in the in-shell store.
 * export NAME=VAL : assign (NAME alone clears the value to "").
 * set             : alias for export (with no operand it lists like env).
 * $NAME expansion in arguments is handled separately by expand_vars().
 * ------------------------------------------------------------------------- */
static void cmd_env(void) {
    if (env_count == 0) {
        grid_puts("(no variables set)\n");
        return;
    }
    for (int i = 0; i < env_count; i++) {
        grid_puts(env_name[i]);
        grid_putchar('=');
        grid_puts(env_val[i]);
        grid_putchar('\n');
    }
}

/* Assign from a "NAME=VALUE" / "NAME" token plus the line remainder. The value
 * is everything after '=' on the original line, so `export X=a b c` keeps the
 * spaces.  `verb` is just used for the usage message. */
static void cmd_export(const char *args, const char *verb) {
    const char *p = skip_spaces(args);
    if (*p == '\0') {            /* no operand: behave like env */
        cmd_env();
        return;
    }
    /* Split at the first '='.  Name is the run of chars before it. */
    char name[ENV_NAME_MAX];
    int n = 0;
    while (*p && *p != '=' && *p != ' ' && *p != '\t' && n < ENV_NAME_MAX - 1)
        name[n++] = *p++;
    name[n] = '\0';
    if (n == 0) {
        grid_puts(verb);
        grid_puts(": usage: ");
        grid_puts(verb);
        grid_puts(" NAME=VALUE\n");
        return;
    }
    if (*p == '=') {
        p++;                    /* value is the rest of the line verbatim */
        env_set(name, p);
    } else {
        env_set(name, "");      /* `export NAME` with no '=' -> empty value */
    }
}

/* ---- stat <file> : size + type from SYS_STAT ---------------------------- */
/* Mirrors kernel vfs_stat_t (kernel/include/vfs.h) so copy_to_user lands in a
 * matching layout.  We only read st_mode and st_size. */
typedef struct {
    u64 st_dev;
    u64 st_ino;
    u32 st_mode;
    u32 st_nlink;
    u32 st_uid;
    u32 st_gid;
    u64 st_rdev;
    u64 st_size;
    u64 st_blksize;
    u64 st_blocks;
    u64 st_atime;
    u64 st_mtime;
    u64 st_ctime;
} k_stat_t;

/* POSIX mode type bits (kernel/include/compat/sys/stat.h). */
#define K_S_IFMT  0170000u
#define K_S_IFDIR 0040000u
#define K_S_IFREG 0100000u
#define K_S_IFLNK 0120000u

static k_stat_t g_stat;

static void cmd_stat(const char *args) {
    char tok[LINE_MAX];
    const char *p = args;
    if (!next_token(&p, tok, sizeof(tok))) {
        grid_puts("stat: missing file operand\n");
        return;
    }
    const char *path = resolve_path(tok);
    long r = sc(SYS_STAT, (long)path, (long)&g_stat, 0, 0, 0, 0);
    if (r < 0) {
        grid_puts("stat: cannot stat '");
        grid_puts(path);
        grid_puts("'\n");
        return;
    }
    const char *type;
    switch (g_stat.st_mode & K_S_IFMT) {
        case K_S_IFDIR: type = "directory";    break;
        case K_S_IFREG: type = "regular file"; break;
        case K_S_IFLNK: type = "symlink";      break;
        default:        type = "special";      break;
    }
    grid_puts("  File: ");
    grid_puts(path);
    grid_putchar('\n');
    grid_puts("  Size: ");
    grid_put_unum((unsigned long)g_stat.st_size);
    grid_puts("\n  Type: ");
    grid_puts(type);
    grid_puts("\n  Mode: ");
    grid_put_unum((unsigned long)(g_stat.st_mode & 07777));
    grid_putchar('\n');
}

/* =========================================================================
 *  Small text utilities.  These all operate on a single file argument and,
 *  consistent with grep/wc, slurp it into g_filebuf first.  When invoked
 *  under output redirection the captured g_cap is written to the target file
 *  by dispatch_one, exactly like the existing utilities.
 * ========================================================================= */

/* Emit one line from g_filebuf[start..start+len) (CR-stripped) + newline. */
static void emit_line(long start, long len) {
    for (long i = 0; i < len; i++) {
        char ch = g_filebuf[start + i];
        if (ch != '\r') grid_putchar(ch);
    }
    grid_putchar('\n');
}

/* cut -d X -f N <file> : print field N (1-based) of each line, split on delim.
 * Defaults: delimiter TAB, field 1.  Field out of range -> empty line. */
static void cmd_cut(const char *args) {
    char delim = '\t';
    long field = 1;
    char tok[LINE_MAX];
    char ftok[LINE_MAX];
    ftok[0] = '\0';
    const char *p = args;

    /* Parse flags (-d X / -f N); first non-flag token is the file. */
    for (;;) {
        const char *save = p;
        if (!next_token(&p, tok, sizeof(tok))) break;
        if (k_streq(tok, "-d")) {
            if (next_token(&p, tok, sizeof(tok)) && tok[0]) delim = tok[0];
        } else if (k_streq(tok, "-f")) {
            if (next_token(&p, tok, sizeof(tok))) { long f = k_atoi(tok); if (f > 0) field = f; }
        } else {
            (void)save;
            k_strlcpy(ftok, tok, sizeof(ftok));
            break;
        }
    }
    if (!ftok[0]) { grid_puts("cut: usage: cut -d X -f N <file>\n"); return; }

    const char *path = resolve_path(ftok);
    long n = slurp_file(path);
    if (n == -1) { grid_puts("cut: "); grid_puts(path); grid_puts(": no such file or directory\n"); return; }
    if (n == -2) { grid_puts("cut: file too large\n"); return; }

    long start = 0;
    for (long i = 0; i <= n; i++) {
        if (i == n || g_filebuf[i] == '\n') {
            if (i > start || i < n) {
                /* walk fields within [start,i) */
                long fs = start, cur = 1;
                long j = start;
                for (; j < i; j++) {
                    if (g_filebuf[j] == delim) {
                        if (cur == field) break;
                        cur++;
                        fs = j + 1;
                    }
                }
                long fe = (cur == field) ? j : -1;
                if (fe >= 0) {
                    for (long k = fs; k < fe; k++) {
                        char ch = g_filebuf[k];
                        if (ch != '\r') grid_putchar(ch);
                    }
                }
                grid_putchar('\n');
            }
            start = i + 1;
        }
    }
}

/* tr A B <file> : replace every byte equal to A[i] with B[i] (1:1, by index).
 * If B is shorter than A, characters mapping past B's end are dropped. */
static void cmd_tr(const char *args) {
    char setA[LINE_MAX];
    char setB[LINE_MAX];
    char ftok[LINE_MAX];
    const char *p = args;
    if (!next_token(&p, setA, sizeof(setA)) ||
        !next_token(&p, setB, sizeof(setB)) ||
        !next_token(&p, ftok, sizeof(ftok))) {
        grid_puts("tr: usage: tr SET1 SET2 <file>\n");
        return;
    }
    const char *path = resolve_path(ftok);
    long n = slurp_file(path);
    if (n == -1) { grid_puts("tr: "); grid_puts(path); grid_puts(": no such file or directory\n"); return; }
    if (n == -2) { grid_puts("tr: file too large\n"); return; }

    int alen = (int)k_strlen(setA);
    int blen = (int)k_strlen(setB);
    for (long i = 0; i < n; i++) {
        char ch = g_filebuf[i];
        if (ch == '\r') continue;
        int hit = -1;
        for (int k = 0; k < alen; k++) if (setA[k] == ch) { hit = k; break; }
        if (hit < 0)          grid_putchar(ch);
        else if (hit < blen)  grid_putchar(setB[hit]);
        /* else: no counterpart in SET2 -> drop the byte */
    }
    if (cur_col != 0 && !g_cap_on) grid_putchar('\n');
    else if (g_cap_on && n > 0 && g_filebuf[n - 1] != '\n') grid_putchar('\n');
}

/* Compare two byte ranges in g_filebuf; returns <0,0,>0 like strcmp. */
static int line_cmp(long a, long alen, long b, long blen) {
    long i = 0;
    while (i < alen && i < blen) {
        unsigned char ca = (unsigned char)g_filebuf[a + i];
        unsigned char cb = (unsigned char)g_filebuf[b + i];
        if (ca != cb) return (int)ca - (int)cb;
        i++;
    }
    return (int)(alen - blen);
}

/* Index every line of g_filebuf[0..n) into starts[]/lens[] (CR not stripped
 * here -- emitters strip it).  Returns line count (capped at cap). */
#define SORT_MAX_LINES 1024
static long g_lstart[SORT_MAX_LINES];
static long g_llen[SORT_MAX_LINES];

static long index_lines(long n) {
    long count = 0, start = 0;
    for (long i = 0; i <= n && count < SORT_MAX_LINES; i++) {
        if (i == n || g_filebuf[i] == '\n') {
            long len = i - start;
            /* drop a final empty line (trailing newline) */
            if (!(i == n && len == 0)) {
                g_lstart[count] = start;
                g_llen[count] = len;
                count++;
            }
            start = i + 1;
        }
    }
    return count;
}

/* sort <file> : print lines in ascending byte order (insertion sort). */
static void cmd_sort(const char *args) {
    char ftok[LINE_MAX];
    const char *p = args;
    if (!next_token(&p, ftok, sizeof(ftok))) { grid_puts("sort: usage: sort <file>\n"); return; }
    const char *path = resolve_path(ftok);
    long n = slurp_file(path);
    if (n == -1) { grid_puts("sort: "); grid_puts(path); grid_puts(": no such file or directory\n"); return; }
    if (n == -2) { grid_puts("sort: file too large\n"); return; }

    long c = index_lines(n);
    for (long i = 1; i < c; i++) {
        long ks = g_lstart[i], kl = g_llen[i];
        long j = i - 1;
        while (j >= 0 && line_cmp(g_lstart[j], g_llen[j], ks, kl) > 0) {
            g_lstart[j + 1] = g_lstart[j];
            g_llen[j + 1]   = g_llen[j];
            j--;
        }
        g_lstart[j + 1] = ks;
        g_llen[j + 1]   = kl;
    }
    for (long i = 0; i < c; i++) emit_line(g_lstart[i], g_llen[i]);
}

/* uniq <file> : collapse runs of adjacent identical lines into one. */
static void cmd_uniq(const char *args) {
    char ftok[LINE_MAX];
    const char *p = args;
    if (!next_token(&p, ftok, sizeof(ftok))) { grid_puts("uniq: usage: uniq <file>\n"); return; }
    const char *path = resolve_path(ftok);
    long n = slurp_file(path);
    if (n == -1) { grid_puts("uniq: "); grid_puts(path); grid_puts(": no such file or directory\n"); return; }
    if (n == -2) { grid_puts("uniq: file too large\n"); return; }

    long c = index_lines(n);
    for (long i = 0; i < c; i++) {
        if (i > 0 && line_cmp(g_lstart[i - 1], g_llen[i - 1],
                              g_lstart[i], g_llen[i]) == 0)
            continue;
        emit_line(g_lstart[i], g_llen[i]);
    }
}

/* seq N : print 1..N, one per line (bounded). */
static void cmd_seq(const char *args) {
    const char *p = skip_spaces(args);
    long n = k_atoi(p);
    if (n < 0) { grid_puts("seq: usage: seq N\n"); return; }
    if (n > 100000) n = 100000;     /* bound the output */
    for (long i = 1; i <= n; i++) { grid_put_num(i); grid_putchar('\n'); }
}

/* basename <path> : print the final path component. */
static void cmd_basename(const char *args) {
    char tok[LINE_MAX];
    const char *p = args;
    if (!next_token(&p, tok, sizeof(tok))) { grid_puts("basename: missing operand\n"); return; }
    int len = (int)k_strlen(tok);
    while (len > 1 && tok[len - 1] == '/') tok[--len] = '\0';   /* strip trailing '/' */
    int last = -1;
    for (int i = 0; i < len; i++) if (tok[i] == '/') last = i;
    const char *base = (last >= 0) ? tok + last + 1 : tok;
    grid_puts(base[0] ? base : "/");
    grid_putchar('\n');
}

/* dirname <path> : print the directory portion (everything up to the last '/'). */
static void cmd_dirname(const char *args) {
    char tok[LINE_MAX];
    const char *p = args;
    if (!next_token(&p, tok, sizeof(tok))) { grid_puts("dirname: missing operand\n"); return; }
    int len = (int)k_strlen(tok);
    while (len > 1 && tok[len - 1] == '/') tok[--len] = '\0';
    int last = -1;
    for (int i = 0; i < len; i++) if (tok[i] == '/') last = i;
    if (last < 0)      grid_puts(".");
    else if (last == 0) grid_puts("/");
    else { tok[last] = '\0'; grid_puts(tok); }
    grid_putchar('\n');
}

/* yes [STRING] : repeat STRING (default "y"), bounded so it always terminates. */
static void cmd_yes(const char *args) {
    const char *s = skip_spaces(args);
    if (*s == '\0') s = "y";
    for (int i = 0; i < 1000; i++) { grid_puts(s); grid_putchar('\n'); }
}

/* ---- ln <src> <dst> : no kernel link syscall, so fall back to a copy. ---- */
static char ln_src[KPATH_MAX] __attribute__((aligned(16)));

static void cmd_ln(const char *args) {
    char srctok[LINE_MAX];
    char dsttok[LINE_MAX];
    const char *p = args;
    /* tolerate an ignored -s flag for symbolic-link syntax */
    if (next_token(&p, srctok, sizeof(srctok)) && k_streq(srctok, "-s")) {
        if (!next_token(&p, srctok, sizeof(srctok))) { grid_puts("ln: usage: ln [-s] <src> <dst>\n"); return; }
    }
    if (!srctok[0] || !next_token(&p, dsttok, sizeof(dsttok))) {
        grid_puts("ln: usage: ln [-s] <src> <dst>\n");
        return;
    }

    for (int i = 0; i < KPATH_MAX; i++) ln_src[i] = '\0';
    k_strlcpy(ln_src, resolve_path(srctok), KPATH_MAX);

    long n = slurp_file(ln_src);
    if (n == -1) { grid_puts("ln: "); grid_puts(ln_src); grid_puts(": no such file or directory\n"); return; }
    if (n == -2) { grid_puts("ln: file too large\n"); return; }

    const char *dst = resolve_path(dsttok);
    long fd = sc(SYS_OPEN, (long)dst, O_WRONLY | O_CREAT | O_TRUNC, 0644, 0, 0, 0);
    if (fd < 0) { grid_puts("ln: cannot create '"); grid_puts(dst); grid_puts("'\n"); return; }
    if (n > 0) sc(SYS_WRITE, fd, (long)g_filebuf, n, 0, 0, 0);
    sc(SYS_CLOSE, fd, 0, 0, 0, 0, 0);
    grid_puts("ln: no kernel link support; copied instead\n");
}

/* ---- du <dir> : recursively sum regular-file sizes via opendir + stat. ----
 * Uses one KPATH-safe child-path buffer PER recursion level (recursion shares
 * file-scope statics, so a single shared buffer would be clobbered by deeper
 * calls).  Output is total bytes. */
#define DU_MAX_DEPTH 16
static k_stat_t g_du_stat;
static char g_du_child[DU_MAX_DEPTH + 1][KPATH_MAX] __attribute__((aligned(16)));

static unsigned long du_walk(const char *dir, int depth) {
    if (depth > DU_MAX_DEPTH) return 0;  /* guard against deep/cyclic trees */
    long dfd = sc(SYS_OPENDIR, (long)dir, 0, 0, 0, 0, 0);
    if (dfd < 0) return 0;

    unsigned long total = 0;
    struct k_dirent de;
    char *child = g_du_child[depth];     /* this level's private path buffer */
    for (;;) {
        long r = sc(SYS_READDIR, dfd, (long)&de, 0, 0, 0, 0);
        if (r != 0) break;
        de.d_name[NAME_MAX_ - 1] = '\0';
        if (de.d_name[0] == '\0') continue;
        if (k_streq(de.d_name, ".") || k_streq(de.d_name, "..")) continue;

        /* build "<dir>/<name>" */
        for (int i = 0; i < KPATH_MAX; i++) child[i] = '\0';
        int len = k_strlcpy(child, dir, KPATH_MAX);
        if (len == 0 || child[len - 1] != '/') {
            if (len < KPATH_MAX - 1) { child[len++] = '/'; child[len] = '\0'; }
        }
        k_strlcpy(child + len, de.d_name, KPATH_MAX - len);

        if (de.d_type == DT_DIR) {
            total += du_walk(child, depth + 1);
        } else {
            if (sc(SYS_STAT, (long)child, (long)&g_du_stat, 0, 0, 0, 0) >= 0)
                total += (unsigned long)g_du_stat.st_size;
        }
    }
    sc(SYS_CLOSEDIR, dfd, 0, 0, 0, 0, 0);
    return total;
}

static void cmd_du(const char *args) {
    char tok[LINE_MAX];
    const char *p = args;
    next_token(&p, tok, sizeof(tok));   /* empty -> cwd */
    const char *path = resolve_path(tok);

    /* du_walk reuses g_pathbuf-free buffers, but resolve_path's result lives in
     * g_pathbuf; copy it out first since the recursion never re-resolves. */
    static char du_root[KPATH_MAX] __attribute__((aligned(16)));
    for (int i = 0; i < KPATH_MAX; i++) du_root[i] = '\0';
    k_strlcpy(du_root, path, KPATH_MAX);

    unsigned long total = du_walk(du_root, 0);
    grid_put_unum(total);
    grid_puts("\t");
    grid_puts(du_root);
    grid_putchar('\n');
}

/* ---- df : report memory as a pseudo-filesystem via SYS_SYSINFO=62. -------- */
static sysinfo_t df_si;

static void cmd_df(void) {
    long r = sc(SYS_SYSINFO, (long)&df_si, 0, 0, 0, 0, 0);
    grid_puts("Filesystem     1K-blocks       Used  Available  Mounted on\n");
    if (r < 0) {
        grid_puts("ramfs              (n/a)      (n/a)     (n/a)  /\n");
        return;
    }
    unsigned long total_k = df_si.total_mem / 1024UL;
    unsigned long free_k  = df_si.free_mem / 1024UL;
    unsigned long used_k  = (df_si.total_mem - df_si.free_mem) / 1024UL;
    grid_puts("ramfs      ");
    grid_put_unum_w(total_k, 10);
    grid_puts(" ");
    grid_put_unum_w(used_k, 10);
    grid_puts(" ");
    grid_put_unum_w(free_k, 10);
    grid_puts("  /\n");
}

/* =========================================================================
 *  test / [ ... ] -- evaluate a condition and store the result in g_last_status
 *  Supported forms (all set g_last_status to 0 = true, 1 = false):
 *      test -e FILE        FILE exists                  (SYS_STAT)
 *      test -f FILE        FILE is a regular file
 *      test -d FILE        FILE is a directory
 *      test -z STR         STR has length 0
 *      test -n STR         STR has non-zero length
 *      test S1 = S2        strings equal
 *      test S1 != S2       strings unequal
 *      test N1 -eq N2      integers equal           (-ne -lt -gt also)
 *  When invoked as `[ ... ]` the dispatcher strips the trailing `]`. An empty
 *  test (`test` with no args) is false, like POSIX.
 * ========================================================================= */
static k_stat_t g_test_stat;

/* compare integers; op is one of eq/ne/lt/gt; returns 1 if true */
static int test_int_cmp(long a, const char *op, long b) {
    if (k_streq(op, "-eq")) return a == b;
    if (k_streq(op, "-ne")) return a != b;
    if (k_streq(op, "-lt")) return a <  b;
    if (k_streq(op, "-gt")) return a >  b;
    return 0;
}

/* Copy `args` into `dst`, dropping a trailing standalone `]` token so the
 * `[ ... ]` form and the `test ...` form share one parser. */
static char g_test_args[LINE_MAX];
static const char *strip_test_bracket(const char *args) {
    k_strlcpy(g_test_args, args, sizeof(g_test_args));
    int len = (int)k_strlen(g_test_args);
    /* trim trailing whitespace */
    while (len > 0 && (g_test_args[len - 1] == ' ' || g_test_args[len - 1] == '\t'))
        g_test_args[--len] = '\0';
    /* drop a final ']' that is its own token (preceded by space or at start) */
    if (len > 0 && g_test_args[len - 1] == ']' &&
        (len == 1 || g_test_args[len - 2] == ' ' || g_test_args[len - 2] == '\t')) {
        g_test_args[--len] = '\0';
        while (len > 0 && (g_test_args[len - 1] == ' ' || g_test_args[len - 1] == '\t'))
            g_test_args[--len] = '\0';
    }
    return g_test_args;
}

static void cmd_test(const char *raw) {
    char a1[LINE_MAX], a2[LINE_MAX], a3[LINE_MAX];
    const char *p = strip_test_bracket(raw);
    int n1 = next_token(&p, a1, sizeof(a1));
    int result = 0;   /* default: false */

    if (!n1) {
        g_last_status = 1;   /* `test` with no args is false */
        return;
    }

    /* Unary file / string tests:  -e -f -d -z -n FOLLOWED BY one operand. */
    if ((k_streq(a1, "-e") || k_streq(a1, "-f") || k_streq(a1, "-d") ||
         k_streq(a1, "-z") || k_streq(a1, "-n")) &&
        next_token(&p, a2, sizeof(a2))) {
        if (k_streq(a1, "-z")) {
            result = (a2[0] == '\0');
        } else if (k_streq(a1, "-n")) {
            result = (a2[0] != '\0');
        } else {
            /* file tests need a stat */
            const char *path = resolve_path(a2);
            long r = sc(SYS_STAT, (long)path, (long)&g_test_stat, 0, 0, 0, 0);
            if (r < 0) {
                result = 0;   /* does not exist -> false for -e/-f/-d */
            } else if (k_streq(a1, "-e")) {
                result = 1;
            } else if (k_streq(a1, "-f")) {
                result = ((g_test_stat.st_mode & K_S_IFMT) == K_S_IFREG);
            } else { /* -d */
                result = ((g_test_stat.st_mode & K_S_IFMT) == K_S_IFDIR);
            }
        }
        g_last_status = result ? 0 : 1;
        return;
    }

    /* Binary tests:  S1 <op> S2 */
    if (next_token(&p, a2, sizeof(a2)) && next_token(&p, a3, sizeof(a3))) {
        if (k_streq(a2, "="))        result = k_streq(a1, a3);
        else if (k_streq(a2, "!="))  result = !k_streq(a1, a3);
        else if (k_streq(a2, "-eq") || k_streq(a2, "-ne") ||
                 k_streq(a2, "-lt") || k_streq(a2, "-gt"))
            result = test_int_cmp(k_atoi(a1), a2, k_atoi(a3));
        else
            result = 0;
        g_last_status = result ? 0 : 1;
        return;
    }

    /* Single operand `test STR` -> true if non-empty (POSIX). */
    g_last_status = (a1[0] != '\0') ? 0 : 1;
}

/* =========================================================================
 *  sh SCRIPT  (and aliases `source` / `.`) -- run an automation script.
 *  Slurps the whole file, copies it out of the shared g_filebuf into a private
 *  buffer (so commands the script runs are free to reuse g_filebuf), then hands
 *  the entire text to run_block(), which honours the new control flow. Scripts
 *  bigger than SCRIPT_MAX are rejected.
 * ========================================================================= */
#define SCRIPT_MAX 8192
static char g_script[SCRIPT_MAX] __attribute__((aligned(16)));

static void cmd_sh(const char *args, const char *verb) {
    char tok[LINE_MAX];
    const char *p = args;
    if (!next_token(&p, tok, sizeof(tok))) {
        grid_puts(verb);
        grid_puts(": usage: ");
        grid_puts(verb);
        grid_puts(" <script>\n");
        return;
    }
    const char *path = resolve_path(tok);
    long n = slurp_file(path);
    if (n == -1) {
        grid_puts(verb);
        grid_puts(": ");
        grid_puts(path);
        grid_puts(": no such file or directory\n");
        return;
    }
    if (n == -2 || n >= SCRIPT_MAX) {
        grid_puts(verb);
        grid_puts(": script too large\n");
        return;
    }
    /* Copy out of g_filebuf so the script's own commands may reuse it. */
    long copy = n < SCRIPT_MAX - 1 ? n : SCRIPT_MAX - 1;
    for (long i = 0; i < copy; i++) g_script[i] = g_filebuf[i];
    g_script[copy] = '\0';

    run_block(g_script);
}

/* ---- external-command fallback ------------------------------------------
 * When a verb is not a builtin, spawn "/bin/<cmd>" so standalone tools
 * (sed, awk, make, pkg, tar, ...) run. The rest of the command line is passed
 * as SYS_SPAWN arg2 (a space-separated args string); the kernel builds the
 * child's argv = [/bin/<cmd>, <tokens>...] (see exec.c). Routed through a padded
 * 256-byte buffer so the kernel's copy_from_user can't read past the source.
 * Returns 1 if a process was spawned, 0 if the image was not found. */
static char spawn_args_buf[256];
static int try_external(const char *cmd, const char *args) {
    for (int i = 0; i < (int)sizeof(spawn_path); i++) spawn_path[i] = '\0';
    const char *pre = "/bin/";
    int pi = 0;
    while (pre[pi] && pi < (int)sizeof(spawn_path) - 1) { spawn_path[pi] = pre[pi]; pi++; }
    k_strlcpy(spawn_path + pi, cmd, (int)sizeof(spawn_path) - pi);

    for (int i = 0; i < (int)sizeof(spawn_args_buf); i++) spawn_args_buf[i] = '\0';
    if (args) k_strlcpy(spawn_args_buf, args, sizeof(spawn_args_buf));

    long pid = sc(SYS_SPAWN, (long)spawn_path, (long)spawn_args_buf, 0, 0, 0, 0);
    if (pid <= 0) { g_last_status = 127; return 0; }   /* not found / failed */
    g_last_status = 0;                                  /* launched OK */
    grid_puts("spawned '");
    grid_puts(spawn_path);
    grid_puts("' as pid ");
    grid_put_unum((unsigned long)pid);
    grid_putchar('\n');
    return 1;
}

/*
 * try_spawn_image -- spawn an EXPLICIT image path (e.g. "/sbin/ping"), passing
 * `args` as the SYS_SPAWN arg2 args-string, exactly like try_external but
 * without prefixing "/bin/".  Returns 1 if launched, 0 if not found / failed.
 * Reuses the same padded spawn_path / spawn_args_buf so the kernel's
 * copy_from_user can't read past the source.  Sets g_last_status (0 / 127).
 */
static int try_spawn_image(const char *image, const char *args) {
    for (int i = 0; i < (int)sizeof(spawn_path); i++) spawn_path[i] = '\0';
    k_strlcpy(spawn_path, image, (int)sizeof(spawn_path));

    for (int i = 0; i < (int)sizeof(spawn_args_buf); i++) spawn_args_buf[i] = '\0';
    if (args) k_strlcpy(spawn_args_buf, args, sizeof(spawn_args_buf));

    long pid = sc(SYS_SPAWN, (long)spawn_path, (long)spawn_args_buf, 0, 0, 0, 0);
    if (pid <= 0) { g_last_status = 127; return 0; }
    g_last_status = 0;
    grid_puts("spawned '");
    grid_puts(spawn_path);
    grid_puts("' as pid ");
    grid_put_unum((unsigned long)pid);
    grid_putchar('\n');
    return 1;
}

/* =========================================================================
 *  Networking commands
 *
 *  All of these REUSE the existing spawn plumbing (try_external /
 *  try_spawn_image) to launch the already-built /bin and /sbin tools, EXCEPT
 *  ifconfig/ip which read NIC state directly via the raw SYS_NET_INFO=59
 *  syscall (no spawn, no extra link dependency).
 * ========================================================================= */

/* wget URL [-O file] -> spawn /bin/wget with the verbatim args string. */
static void cmd_wget(const char *args) {
    const char *p = skip_spaces(args);
    if (*p == '\0') {
        grid_puts("wget: usage: wget URL [-O file]\n");
        g_last_status = 1;
        return;
    }
    if (!try_external("wget", p)) {
        grid_puts("wget: /bin/wget not found\n");
    }
}

/* curl URL -> alias that spawns /bin/wget URL (body printed to console). */
static void cmd_curl(const char *args) {
    const char *p = skip_spaces(args);
    if (*p == '\0') {
        grid_puts("curl: usage: curl URL   (alias for wget)\n");
        g_last_status = 1;
        return;
    }
    if (!try_external("wget", p)) {
        grid_puts("curl: /bin/wget not found\n");
    }
}

/* ping HOST [count] -> spawn /sbin/ping, falling back to /bin/ping. */
static void cmd_ping(const char *args) {
    const char *p = skip_spaces(args);
    if (*p == '\0') {
        grid_puts("ping: usage: ping HOST [count]\n");
        g_last_status = 1;
        return;
    }
    if (try_spawn_image("/sbin/ping", p)) return;
    if (try_external("ping", p)) return;
    grid_puts("ping: ping tool not found (/sbin/ping, /bin/ping)\n");
}

/* nc HOST PORT [text] -> spawn /bin/nc with the verbatim args string. */
static void cmd_nc(const char *args) {
    const char *p = skip_spaces(args);
    if (*p == '\0') {
        grid_puts("nc: usage: nc HOST PORT [text]\n");
        g_last_status = 1;
        return;
    }
    if (!try_external("nc", p)) {
        grid_puts("nc: /bin/nc not found\n");
    }
}

/* Print one octet of a host-order IPv4 address; A is the high byte. */
static void grid_put_ip(u32 ip) {
    grid_put_unum((ip >> 24) & 0xFFu); grid_putchar('.');
    grid_put_unum((ip >> 16) & 0xFFu); grid_putchar('.');
    grid_put_unum((ip >>  8) & 0xFFu); grid_putchar('.');
    grid_put_unum( ip        & 0xFFu);
}

/* Print a single byte as two lowercase hex digits. */
static void grid_put_hex2(unsigned char b) {
    static const char hx[] = "0123456789abcdef";
    grid_putchar(hx[(b >> 4) & 0xF]);
    grid_putchar(hx[b & 0xF]);
}

/* Print a 6-byte MAC as xx:xx:xx:xx:xx:xx. */
static void grid_put_mac(const unsigned char mac[6]) {
    for (int i = 0; i < 6; i++) {
        if (i) grid_putchar(':');
        grid_put_hex2(mac[i]);
    }
}

/*
 * ifconfig / ip -- read NIC state directly via SYS_NET_INFO=59 (no spawn).
 *   sc(59, &info, 0, ...) -> 0 fills info, <0 means link down / not wired.
 */
static net_info_t g_net_info;

static void cmd_ifconfig(void) {
    long r = sc(SYS_NET_INFO, (long)&g_net_info, 0, 0, 0, 0, 0);
    if (r < 0) {
        grid_puts("iface: link DOWN\n");
        return;
    }
    grid_puts("iface: link UP  ip ");
    grid_put_ip(g_net_info.ip);
    grid_puts("  gw ");
    grid_put_ip(g_net_info.gateway);
    grid_puts("  mac ");
    grid_put_mac(g_net_info.mac);
    grid_putchar('\n');
}

/*
 * host NAME / nslookup NAME -- there is no standalone DNS tool to spawn, and
 * linking the in-kernel dns_resolve() helper would add a new link dependency
 * (dns.o) to the terminal, which we deliberately avoid.  So we resolve nothing
 * here and instead print actionable guidance: HTTP fetches via wget accept a
 * hostname directly (it resolves internally), so use that.
 */
static void cmd_host(const char *args, const char *verb) {
    char tok[LINE_MAX];
    const char *p = args;
    if (!next_token(&p, tok, sizeof(tok))) {
        grid_puts(verb);
        grid_puts(": usage: ");
        grid_puts(verb);
        grid_puts(" NAME\n");
        g_last_status = 1;
        return;
    }
    grid_puts(verb);
    grid_puts(": no standalone DNS resolver on this system.\n");
    grid_puts("  wget resolves hostnames itself -- e.g.  wget http://");
    grid_puts(tok);
    grid_puts("/\n");
}

/*
 * Match the verb and run the matching builtin. `cmd` is the already-extracted
 * verb; `args` points just past it (skip_spaces handled inside each builtin).
 * Output goes through grid_*; when g_cap_on is set those route into g_cap so
 * the caller (dispatch_one) can redirect a command's output to a file.
 */
static void dispatch_verb(const char *cmd, const char *args) {
    /* Optimistic default: a command "succeeds" unless it explicitly fails.
     * Status-aware verbs (test/[/true/false and the external spawn) overwrite
     * g_last_status below; everything else leaves it at 0 (success). */
    g_last_status = 0;

    if      (k_streq(cmd, "help"))    cmd_help();
    else if (k_streq(cmd, "echo"))    cmd_echo(args);
    else if (k_streq(cmd, "clear"))   { grid_clear(); }
    else if (k_streq(cmd, "uptime"))  cmd_uptime();
    else if (k_streq(cmd, "ls"))      cmd_ls(args);
    else if (k_streq(cmd, "cat"))     cmd_cat(args);
    else if (k_streq(cmd, "cp"))      cmd_cp(args);
    else if (k_streq(cmd, "grep"))    cmd_grep(args);
    else if (k_streq(cmd, "wc"))      cmd_wc(args);
    else if (k_streq(cmd, "head"))    cmd_head(args);
    else if (k_streq(cmd, "tail"))    cmd_tail(args);
    else if (k_streq(cmd, "date"))    cmd_date();
    else if (k_streq(cmd, "uname"))   cmd_uname();
    else if (k_streq(cmd, "whoami"))  cmd_whoami();
    else if (k_streq(cmd, "about"))   cmd_about();
    else if (k_streq(cmd, "run"))     cmd_run(args);
    else if (k_streq(cmd, "launch"))  cmd_run(args);
    else if (k_streq(cmd, "spawn"))   cmd_spawn(args);
    else if (k_streq(cmd, "ps"))      cmd_ps();
    else if (k_streq(cmd, "top"))     cmd_top();
    else if (k_streq(cmd, "kill"))    cmd_kill(args);
    else if (k_streq(cmd, "mem"))     cmd_mem();
    else if (k_streq(cmd, "sysinfo")) cmd_mem();
    else if (k_streq(cmd, "history")) cmd_history();
    else if (k_streq(cmd, "pwd"))     cmd_pwd();
    else if (k_streq(cmd, "cd"))      cmd_cd(args);
    else if (k_streq(cmd, "mkdir"))   cmd_mkdir(args);
    else if (k_streq(cmd, "touch"))   cmd_touch(args);
    else if (k_streq(cmd, "rm"))      cmd_rm(args);
    else if (k_streq(cmd, "mv"))      cmd_mv(args);
    else if (k_streq(cmd, "write"))   cmd_write(args);
    else if (k_streq(cmd, "git"))     git_run(args, g_cwd, grid_puts);
    else if (k_streq(cmd, "env"))     cmd_env();
    else if (k_streq(cmd, "export"))  cmd_export(args, "export");
    else if (k_streq(cmd, "set"))     cmd_export(args, "set");
    else if (k_streq(cmd, "stat"))    cmd_stat(args);
    else if (k_streq(cmd, "cut"))     cmd_cut(args);
    else if (k_streq(cmd, "tr"))      cmd_tr(args);
    else if (k_streq(cmd, "sort"))    cmd_sort(args);
    else if (k_streq(cmd, "uniq"))    cmd_uniq(args);
    else if (k_streq(cmd, "seq"))     cmd_seq(args);
    else if (k_streq(cmd, "basename")) cmd_basename(args);
    else if (k_streq(cmd, "dirname"))  cmd_dirname(args);
    else if (k_streq(cmd, "test"))    cmd_test(args);
    else if (k_streq(cmd, "["))       cmd_test(args);   /* trailing ] stripped by caller */
    else if (k_streq(cmd, "sh"))      cmd_sh(args, "sh");
    else if (k_streq(cmd, "source"))  cmd_sh(args, "source");
    else if (k_streq(cmd, "."))       cmd_sh(args, ".");
    else if (k_streq(cmd, "true"))    { g_last_status = 0; }
    else if (k_streq(cmd, "false"))   { g_last_status = 1; }
    else if (k_streq(cmd, "yes"))     cmd_yes(args);
    else if (k_streq(cmd, "ln"))      cmd_ln(args);
    else if (k_streq(cmd, "du"))      cmd_du(args);
    else if (k_streq(cmd, "df"))      cmd_df();
    /* ---- networking commands ---- */
    else if (k_streq(cmd, "wget"))     cmd_wget(args);
    else if (k_streq(cmd, "curl"))     cmd_curl(args);
    else if (k_streq(cmd, "ping"))     cmd_ping(args);
    else if (k_streq(cmd, "nc"))       cmd_nc(args);
    else if (k_streq(cmd, "ifconfig")) cmd_ifconfig();
    else if (k_streq(cmd, "ip"))       cmd_ifconfig();
    else if (k_streq(cmd, "host"))     cmd_host(args, "host");
    else if (k_streq(cmd, "nslookup")) cmd_host(args, "nslookup");
    else {
        /* Not a builtin: spawn /bin/<cmd>, passing the rest of the line as args. */
        if (!try_external(cmd, args)) {
            grid_puts(cmd);
            grid_puts(": command not found\n");
        }
    }
}

/*
 * Run ONE command segment (no top-level ';').  Handles output redirection:
 * a whitespace-separated `>` or `>>` token splits the command body from the
 * target filename.  The body's grid output is captured into g_cap, then
 * written to the file (truncate for `>`, append for `>>`); on redirect the
 * captured text is NOT echoed to the screen.
 *
 * `seg` is a writable buffer (its `>`/`>>` token is NUL-terminated in place).
 */
static char g_redir_name[LINE_MAX];   /* redirect target token (off g_pathbuf) */

static void dispatch_one(char *seg) {
    const char *p = skip_spaces(seg);
    if (*p == '\0') return;   /* blank segment: nothing to do */

    /*
     * Scan for a top-level `>` / `>>` redirection token.  Only a token that is
     * surrounded by whitespace (or at end-of-line) counts, so a '>' embedded in
     * an argument is left untouched.  `redir` = 0 none, 1 truncate, 2 append.
     */
    int  redir = 0;
    char *body_end = (void *)0;   /* where to cut the command body */
    const char *fname = (void *)0;
    {
        char *q = seg;
        while (*q) {
            if (*q == '>') {
                /* must be at a token boundary (start, or preceded by space) */
                int left_ok = (q == seg) || q[-1] == ' ' || q[-1] == '\t';
                if (left_ok) {
                    char *tok = q;
                    int append = 0;
                    if (q[1] == '>') { append = 1; q++; }
                    /* right side: must be space/end after the '>'/'>>'  */
                    char *after = q + 1;
                    if (*after == '\0' || *after == ' ' || *after == '\t') {
                        redir = append ? 2 : 1;
                        body_end = tok;                 /* cut here */
                        fname = skip_spaces(after);     /* filename follows */
                        break;
                    }
                }
            }
            q++;
        }
    }

    if (redir) {
        /* Copy out the filename token (first whitespace-delimited word). */
        const char *fp = fname;
        next_token(&fp, g_redir_name, sizeof(g_redir_name));
        if (g_redir_name[0] == '\0') {
            grid_puts("syntax error: expected filename after '>'\n");
            return;
        }
        /* Trim the command body in place by cutting at the '>' token. */
        *body_end = '\0';
    }

    /* Re-derive the verb + args from the (possibly trimmed) body. */
    const char *bp = skip_spaces(seg);
    if (*bp == '\0') {
        if (redir) grid_puts("syntax error: missing command before '>'\n");
        return;
    }
    char cmd[LINE_MAX];
    next_token(&bp, cmd, sizeof(cmd));

    /* serial trace of the dispatched command */
    print("[TERM] exec: ");
    print(cmd);
    print("\n");

    if (redir) {
        /* Capture the command body's output instead of drawing it. */
        g_cap_on  = 1;
        g_cap_len = 0;
        dispatch_verb(cmd, bp);
        g_cap_on  = 0;

        /* Resolve the target only now (body's resolve_path calls are done). */
        const char *path = resolve_path(g_redir_name);
        long flags = O_WRONLY | O_CREAT | (redir == 2 ? O_APPEND : O_TRUNC);
        long fd = sc(SYS_OPEN, (long)path, flags, 0644, 0, 0, 0);
        if (fd < 0) {
            grid_puts(path);
            grid_puts(": ");
            grid_puts(errno_str(fd));
            grid_putchar('\n');
            return;
        }
        if (g_cap_len > 0)
            sc(SYS_WRITE, fd, (long)g_cap, (long)g_cap_len, 0, 0, 0);
        sc(SYS_CLOSE, fd, 0, 0, 0, 0, 0);
        return;   /* captured output is NOT echoed to the screen */
    }

    dispatch_verb(cmd, bp);
}

/*
 * Expand $NAME references in `src` into `dst[cap]` using the in-shell env store.
 * A variable name is the run of [A-Za-z0-9_] after a '$'.  Unknown names expand
 * to the empty string; a lone '$' (no name follows) is copied verbatim.  This
 * runs once over the whole line before it is split on ';'.
 */
static int is_name_char(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') || c == '_';
}

static void expand_vars(const char *src, char *dst, int cap) {
    int d = 0;
    for (int i = 0; src[i] && d < cap - 1; ) {
        if (src[i] == '$' && is_name_char(src[i + 1])) {
            char name[ENV_NAME_MAX];
            int n = 0;
            i++;                               /* skip '$' */
            while (src[i] && is_name_char(src[i]) && n < ENV_NAME_MAX - 1)
                name[n++] = src[i++];
            name[n] = '\0';
            const char *val = env_get(name);
            for (int k = 0; val[k] && d < cap - 1; k++) dst[d++] = val[k];
        } else {
            dst[d++] = src[i++];
        }
    }
    dst[d] = '\0';
}

/* =========================================================================
 *  Command substitution  $(cmd)
 *  Run `cmd` with output captured (reusing the existing g_cap machinery), then
 *  splice its TRIMMED output (surrounding whitespace removed, interior newlines
 *  flattened to single spaces) back into the line. The capture scalars are
 *  saved/restored around the inner run. Recursion is bounded by g_subst_depth.
 * ========================================================================= */
#define SUBST_MAX     2048
#define SUBST_MAXDEPTH 8
static char g_subst_buf[SUBST_MAX] __attribute__((aligned(16)));
static int  g_subst_depth;

/* forward decls: command substitution runs a full simple line */
static void run_simple_line(const char *line);

/*
 * Run `cmd` capturing its output into out[cap] (NUL-terminated, trimmed).
 * Returns the trimmed length. Saves/restores the small global capture scalars
 * around the inner run. We deliberately do NOT snapshot g_cap's bytes: all
 * expansion (including this substitution) happens in run_simple_line BEFORE any
 * command produces output, so an outer capture is always empty (len 0) at the
 * moment we recurse -- there is nothing to preserve. The depth guard bounds
 * nested $( ... ) so recursion can never run away.
 */
static int capture_command(const char *cmd, char *out, int cap) {
    if (g_subst_depth >= SUBST_MAXDEPTH) { out[0] = '\0'; return 0; }
    g_subst_depth++;

    /* save outer capture scalars */
    int save_on  = g_cap_on;
    int save_len = g_cap_len;

    g_cap_on  = 1;
    g_cap_len = 0;
    run_simple_line(cmd);
    int produced = g_cap_len;
    if (produced > CAP_MAX) produced = CAP_MAX;

    /* copy captured bytes out, flattening newlines/tabs to spaces */
    int o = 0;
    for (int i = 0; i < produced && o < cap - 1; i++) {
        char ch = g_cap[i];
        if (ch == '\n' || ch == '\r' || ch == '\t') ch = ' ';
        out[o++] = ch;
    }
    out[o] = '\0';

    /* restore outer capture scalars */
    g_cap_on  = save_on;
    g_cap_len = save_len;

    g_subst_depth--;

    /* trim leading + trailing spaces */
    int s = 0;
    while (out[s] == ' ') s++;
    int e = o;
    while (e > s && out[e - 1] == ' ') e--;
    int len = 0;
    for (int i = s; i < e; i++) out[len++] = out[i];
    out[len] = '\0';
    return len;
}

/*
 * expand_line: full expansion pass producing dst[cap]. Handles $(cmd) command
 * substitution first (running the inner command), then $NAME variable
 * expansion, reusing expand_vars for the variable step. A lone '$' or
 * unbalanced '$(' is copied verbatim.
 */
static char g_subst_inner[LINE_MAX];
static char g_subst_stage[LINE_MAX];

static void expand_line(const char *src, char *dst, int cap) {
    /* Pass 1: splice out every $(...) into g_subst_stage. */
    int d = 0;
    for (int i = 0; src[i] && d < (int)sizeof(g_subst_stage) - 1; ) {
        if (src[i] == '$' && src[i + 1] == '(') {
            /* find the matching ')' (track nesting for $( inside ) */
            int j = i + 2, depth = 1, n = 0;
            while (src[j] && depth > 0 && n < (int)sizeof(g_subst_inner) - 1) {
                if (src[j] == '(') depth++;
                else if (src[j] == ')') { depth--; if (depth == 0) break; }
                g_subst_inner[n++] = src[j++];
            }
            if (depth == 0 && src[j] == ')') {
                g_subst_inner[n] = '\0';
                int rlen = capture_command(g_subst_inner, g_subst_buf, sizeof(g_subst_buf));
                for (int k = 0; k < rlen && d < (int)sizeof(g_subst_stage) - 1; k++)
                    g_subst_stage[d++] = g_subst_buf[k];
                i = j + 1;             /* skip past the ')' */
            } else {
                /* unbalanced: copy '$' verbatim and continue */
                g_subst_stage[d++] = src[i++];
            }
        } else {
            g_subst_stage[d++] = src[i++];
        }
    }
    g_subst_stage[d] = '\0';

    /* Pass 2: $NAME variable expansion via the existing helper. */
    expand_vars(g_subst_stage, dst, cap);
}

/*
 * run_simple_line: the leaf executor. Expands $VAR and $(...) into a private
 * mutable copy, then splits on top-level `;` and runs each segment through
 * dispatch_one (so redirection still works per segment). This is what every
 * control-flow body and the interactive prompt ultimately call.
 */
static char g_exec_line[LINE_MAX];

static void run_simple_line(const char *line) {
    const char *q = skip_spaces(line);
    if (*q == '\0') return;   /* blank line: nothing to do */

    expand_line(line, g_exec_line, sizeof(g_exec_line));

    char *seg = g_exec_line;
    char *s = g_exec_line;
    for (;;) {
        if (*s == ';' || *s == '\0') {
            int last = (*s == '\0');
            *s = '\0';               /* terminate this segment */
            dispatch_one(seg);
            if (last) break;
            seg = s + 1;             /* next segment begins after the ';' */
        }
        s++;
    }
}

/* =========================================================================
 *  Shell control flow:  if / for / while  + the `sh SCRIPT` runner
 *
 *  A block of text (one interactive line, or a whole script) is tokenized into
 *  WORDS, with `;` and newlines emitted as the separator token ";". A small
 *  recursive-descent interpreter then walks the token stream:
 *
 *    if   COND ; then BODY [ else BODY ] fi
 *    for  VAR in W1 W2 .. ; do BODY done
 *    while COND ; do BODY done           (iterations capped to avoid hangs)
 *
 *  Leaf statements (runs of words up to a ";") are reassembled into a line and
 *  handed to run_simple_line. COND is the leaf command immediately before
 *  `then`/`do`; its success is g_last_status == 0 (reset before each COND run).
 *  Nesting depth and total tokens are bounded; while-loops are capped at
 *  WHILE_CAP iterations so a runaway condition can never wedge the shell.
 * ========================================================================= */
#define TOK_MAX      512
#define TOK_LEN      LINE_MAX
#define BLOCK_DEPTH  8
#define WHILE_CAP    100000

static char g_tok[TOK_MAX][TOK_LEN];
static int  g_ntok;
static int  g_blk_depth;           /* recursion guard for nested blocks */

/* Is token i a separator (";")? */
static int tok_is_sep(int i) { return g_tok[i][0] == ';' && g_tok[i][1] == '\0'; }

/* Keyword helpers. */
static int tok_is(int i, const char *kw) { return k_streq(g_tok[i], kw); }

/* Tokenize `text` into g_tok[]/g_ntok. Words split on whitespace; `;` and
 * newlines become the standalone separator token ";". Consecutive separators
 * collapse to one (and no empty word tokens are ever produced). */
static void tokenize_block(const char *text) {
    g_ntok = 0;
    int i = 0;
    while (text[i] && g_ntok < TOK_MAX) {
        char ch = text[i];
        if (ch == ' ' || ch == '\t' || ch == '\r') { i++; continue; }
        if (ch == ';' || ch == '\n') {
            /* emit one ";" unless the previous token already was one */
            if (g_ntok > 0 && !tok_is_sep(g_ntok - 1)) {
                g_tok[g_ntok][0] = ';'; g_tok[g_ntok][1] = '\0'; g_ntok++;
            }
            i++;
            continue;
        }
        /* a word: copy until whitespace/';'/newline */
        int n = 0;
        while (text[i] && text[i] != ' ' && text[i] != '\t' && text[i] != '\r' &&
               text[i] != ';' && text[i] != '\n' && n < TOK_LEN - 1) {
            g_tok[g_ntok][n++] = text[i++];
        }
        g_tok[g_ntok][n] = '\0';
        if (n > 0) g_ntok++;
    }
}

/* forward decl: the recursive sequence executor. */
static void exec_seq(int lo, int hi);

/* Within [lo,hi), find the index of keyword `kw` at brace-depth 0 (i.e. not
 * nested inside another if/for/while). Returns -1 if not found. `open`/`close`
 * are the nesting keywords (e.g. for if: opens on "if", closes on "fi").
 * To keep it general we count any of if/for/while as openers and fi/done as
 * closers, and stop at the first `kw` seen at depth 0. */
static int find_kw(int lo, int hi, const char *kw) {
    int depth = 0;
    for (int i = lo; i < hi; i++) {
        if (tok_is(i, "if") || tok_is(i, "for") || tok_is(i, "while")) depth++;
        else if (tok_is(i, "fi") || tok_is(i, "done"))                 depth--;
        else if (depth == 0 && tok_is(i, kw))                          return i;
    }
    return -1;
}

/* Find the matching closer for a block opened at `open_idx` (whose opener is
 * if/for/while). Returns the index of the matching fi/done, or hi if missing. */
static int find_block_end(int open_idx, int hi) {
    int depth = 0;
    for (int i = open_idx; i < hi; i++) {
        if (tok_is(i, "if") || tok_is(i, "for") || tok_is(i, "while")) depth++;
        else if (tok_is(i, "fi") || tok_is(i, "done")) {
            depth--;
            if (depth == 0) return i;
        }
    }
    return hi;
}

/* Reassemble tokens [lo,hi) (a single leaf statement, no separators) into a
 * line and run it. Skips empty ranges. */
static char g_leaf[LINE_MAX];
static void run_leaf(int lo, int hi) {
    int d = 0;
    for (int i = lo; i < hi && d < LINE_MAX - 1; i++) {
        if (tok_is_sep(i)) continue;
        if (d > 0 && d < LINE_MAX - 1) g_leaf[d++] = ' ';
        for (int k = 0; g_tok[i][k] && d < LINE_MAX - 1; k++)
            g_leaf[d++] = g_tok[i][k];
    }
    g_leaf[d] = '\0';
    if (g_leaf[0]) run_simple_line(g_leaf);
}

/* Run the COND tokens [lo,hi) and return 1 if it "succeeded". Resets
 * g_last_status to 0 first so a stale failure never leaks in. */
static int run_cond(int lo, int hi) {
    g_last_status = 0;
    run_leaf(lo, hi);
    return g_last_status == 0;
}

/* Execute an `if` block.  Tokens: if COND ; then BODY [else BODY] fi
 * `s` points at "if"; `end` is the matching "fi". */
static void exec_if(int s, int end) {
    int then_i = find_kw(s + 1, end, "then");
    if (then_i < 0) return;                       /* malformed: bail quietly */
    int else_i = find_kw(then_i + 1, end, "else");
    int body_hi = (else_i >= 0) ? else_i : end;

    int ok = run_cond(s + 1, then_i);
    if (ok) exec_seq(then_i + 1, body_hi);
    else if (else_i >= 0) exec_seq(else_i + 1, end);
}

/* Execute a `for` block. Tokens: for VAR in W1 W2 .. ; do BODY done
 * `s` points at "for"; `end` is the matching "done". */
static void exec_for(int s, int end) {
    if (s + 1 >= end) return;
    char var[ENV_NAME_MAX];
    k_strlcpy(var, g_tok[s + 1], sizeof(var));

    int in_i = -1;
    if (s + 2 < end && tok_is(s + 2, "in")) in_i = s + 2;
    int do_i = find_kw(s + 1, end, "do");
    if (do_i < 0) return;

    /* word list = tokens between "in" and "do" (or empty if no "in") */
    int wlo = (in_i >= 0) ? in_i + 1 : do_i; /* if no `in`, empty list */
    int whi = do_i;

    for (int w = wlo; w < whi; w++) {
        if (tok_is_sep(w)) continue;
        /* Expand $VAR / $(...) in each word so `for f in $DIR` works. The
         * expanded value is used as a single iteration word (no re-splitting). */
        static char fw[LINE_MAX];
        expand_line(g_tok[w], fw, sizeof(fw));
        env_set(var, fw);
        exec_seq(do_i + 1, end);
    }
}

/* Execute a `while` block. Tokens: while COND ; do BODY done
 * `s` points at "while"; `end` is the matching "done". Iterations capped. */
static void exec_while(int s, int end) {
    int do_i = find_kw(s + 1, end, "do");
    if (do_i < 0) return;
    long guard = 0;
    while (guard++ < WHILE_CAP) {
        if (!run_cond(s + 1, do_i)) break;
        exec_seq(do_i + 1, end);
    }
}

/*
 * exec_seq: run the statement sequence in token range [lo,hi). Splits on the
 * separator token ";"; a leaf run of words is executed via run_leaf, while an
 * if/for/while opener delegates to the matching block handler and skips past
 * its closer.
 */
static void exec_seq(int lo, int hi) {
    if (g_blk_depth >= BLOCK_DEPTH) return;   /* guard runaway nesting */
    g_blk_depth++;

    int i = lo;
    while (i < hi) {
        if (tok_is_sep(i)) { i++; continue; }

        if (tok_is(i, "if") || tok_is(i, "for") || tok_is(i, "while")) {
            int end = find_block_end(i, hi);
            if      (tok_is(i, "if"))    exec_if(i, end);
            else if (tok_is(i, "for"))   exec_for(i, end);
            else                         exec_while(i, end);
            i = end + 1;                 /* skip past fi/done */
            continue;
        }

        /* otherwise: a leaf statement -- gather words up to the next ";" */
        int j = i;
        while (j < hi && !tok_is_sep(j) &&
               !tok_is(j, "if") && !tok_is(j, "for") && !tok_is(j, "while"))
            j++;
        run_leaf(i, j);
        i = j;
    }

    g_blk_depth--;
}

/* Run a whole block of text (handles control flow). */
static void run_block(const char *text) {
    tokenize_block(text);
    g_blk_depth = 0;
    exec_seq(0, g_ntok);
}

/* =========================================================================
 *  Interactive line collector + script runner
 *
 *  Interactive lines may open multi-line blocks (typing `if x` then `then ...`
 *  on later lines). We accumulate raw text into g_pending until the if/for/
 *  while nesting balances (depth back to 0), then run the whole block. A single
 *  line containing a complete `if ...; fi` balances immediately and runs at
 *  once. The depth is computed by scanning the words of each new line.
 * ========================================================================= */
#define PENDING_MAX 4096
static char g_pending[PENDING_MAX];
static int  g_pending_len;
static int  g_pending_depth;        /* open if/for/while not yet closed */

/* Count the net change in block nesting contributed by one line's words. */
static int line_block_delta(const char *line) {
    int delta = 0;
    const char *p = line;
    char w[TOK_LEN];
    for (;;) {
        /* split on whitespace AND ';' so "fi" right after ';' still counts */
        while (*p == ' ' || *p == '\t' || *p == ';') p++;
        if (!*p) break;
        int n = 0;
        while (*p && *p != ' ' && *p != '\t' && *p != ';' && n < TOK_LEN - 1)
            w[n++] = *p++;
        w[n] = '\0';
        if (k_streq(w, "if") || k_streq(w, "for") || k_streq(w, "while")) delta++;
        else if (k_streq(w, "fi") || k_streq(w, "done")) delta--;
    }
    return delta;
}

/* Append `line` (+ a newline) to the pending block buffer, bounded. */
static void pending_append(const char *line) {
    int n = (int)k_strlen(line);
    if (g_pending_len + n + 1 >= PENDING_MAX) return;   /* drop on overflow */
    for (int i = 0; i < n; i++) g_pending[g_pending_len++] = line[i];
    g_pending[g_pending_len++] = '\n';
    g_pending[g_pending_len]   = '\0';
}

/*
 * shell_execute: the public entry from the input loop. Implements the multi-line
 * block collector. If a line starts (or continues) an open if/for/while, the
 * line is buffered until nesting balances, then the whole block is executed via
 * run_block. Simple lines run immediately through run_block too (so a one-line
 * `if ...; fi` and plain `;`-chained commands both work).
 */
static void shell_execute(const char *line) {
    const char *q = skip_spaces(line);

    if (g_pending_depth > 0) {
        /* mid-block continuation: always buffer, even blank lines */
        pending_append(line);
        g_pending_depth += line_block_delta(line);
        if (g_pending_depth <= 0) {
            run_block(g_pending);
            g_pending_len = 0; g_pending_depth = 0; g_pending[0] = '\0';
        }
        return;
    }

    if (*q == '\0') return;   /* blank line outside a block: just reprompt */

    int delta = line_block_delta(line);
    if (delta > 0) {
        /* opens a block that isn't closed on this same line: start buffering */
        g_pending_len = 0; g_pending[0] = '\0';
        pending_append(line);
        g_pending_depth = delta;
        return;
    }

    /* Self-contained line (may include a complete if/for/while or none). */
    run_block(line);
}

/*
 * Erase the currently typed line from the display (back to the prompt end).
 * We step backwards over line_len characters and grid_backspace() each one.
 */
static void erase_input_line(void) {
    for (int i = 0; i < line_len; i++)
        grid_backspace();
    line_len = 0;
}

/* =========================================================================
 *  Tab completion
 *
 *  On Tab press, complete the current word:
 *    - If the cursor is on the FIRST word, complete from builtins.
 *    - Otherwise, complete from filenames in the cwd (or the dir prefix).
 *  A single match auto-inserts; multiple matches show all candidates below
 *  the prompt and re-draw the input.
 * ========================================================================= */

/* Builtin names for first-word completion. */
static const char *g_builtins[] = {
    "help", "echo", "clear", "uptime", "ls", "cat", "cp", "grep", "wc",
    "head", "tail", "date", "uname", "whoami", "about", "run", "launch",
    "spawn", "ps", "top", "kill", "mem", "sysinfo", "history", "pwd",
    "cd", "mkdir", "touch", "rm", "mv", "write", "git", "env", "export",
    "set", "stat", "cut", "tr", "sort", "uniq", "seq", "basename",
    "dirname", "test", "sh", "source", "true", "false", "yes", "ln",
    "du", "df", "wget", "curl", "ping", "nc", "ifconfig", "ip", "host",
    "nslookup",
    (void *)0
};

/* prefix match: does s start with prefix[0..plen)? */
static int starts_with(const char *s, const char *prefix, int plen) {
    for (int i = 0; i < plen; i++)
        if (!s[i] || s[i] != prefix[i]) return 0;
    return 1;
}

/* Find the longest common prefix of matches[0..count) (each NUL-terminated).
 * Returns length of the common prefix. */
#define TAB_MATCH_MAX  64
#define TAB_MATCH_LEN  128
static char tab_matches[TAB_MATCH_MAX][TAB_MATCH_LEN];
static int  tab_match_count;

static int common_prefix_len(void) {
    if (tab_match_count == 0) return 0;
    if (tab_match_count == 1) return (int)k_strlen(tab_matches[0]);
    int len = 0;
    for (;;) {
        char ch = tab_matches[0][len];
        if (!ch) break;
        int ok = 1;
        for (int i = 1; i < tab_match_count; i++)
            if (tab_matches[i][len] != ch) { ok = 0; break; }
        if (!ok) break;
        len++;
    }
    return len;
}

/* Collect builtin matches for `word[0..wlen)`. */
static void tab_complete_builtins(const char *word, int wlen) {
    for (int i = 0; g_builtins[i]; i++) {
        if (starts_with(g_builtins[i], word, wlen) && tab_match_count < TAB_MATCH_MAX) {
            k_strlcpy(tab_matches[tab_match_count], g_builtins[i], TAB_MATCH_LEN);
            tab_match_count++;
        }
    }
}

/* Collect filename matches for `word[0..wlen)` from a directory.
 * `word` may contain a directory prefix (e.g. "bin/f").  We split at the
 * last '/' to get the directory to opendir and the partial filename. */
static char tab_dir[KPATH_MAX] __attribute__((aligned(16)));
static char tab_partial[LINE_MAX];

static void tab_complete_files(const char *word, int wlen) {
    /* Find the last '/' in word to split dir/partial. */
    int last_slash = -1;
    for (int i = 0; i < wlen; i++)
        if (word[i] == '/') last_slash = i;

    int dir_len = 0;
    int partial_len = 0;
    if (last_slash >= 0) {
        /* dir = word[0..last_slash], partial = word[last_slash+1..] */
        for (int i = 0; i <= last_slash && i < KPATH_MAX - 1; i++)
            tab_dir[i] = word[i];
        tab_dir[last_slash + 1] = '\0';
        dir_len = last_slash + 1;
        partial_len = wlen - last_slash - 1;
        for (int i = 0; i < partial_len && i < LINE_MAX - 1; i++)
            tab_partial[i] = word[last_slash + 1 + i];
        tab_partial[partial_len] = '\0';
    } else {
        /* No slash: opendir the cwd, partial is the whole word. */
        k_strlcpy(tab_dir, g_cwd, KPATH_MAX);
        dir_len = 0;
        partial_len = wlen;
        for (int i = 0; i < wlen && i < LINE_MAX - 1; i++)
            tab_partial[i] = word[i];
        tab_partial[partial_len] = '\0';
    }

    /* Resolve the directory path. We need to avoid clobbering g_pathbuf's
     * state; resolve_path returns g_pathbuf so copy it out. */
    const char *dir_resolved = resolve_path(tab_dir);
    /* Clear tab_dir fully for kernel safety. */
    for (int i = 0; i < KPATH_MAX; i++) tab_dir[i] = '\0';
    k_strlcpy(tab_dir, dir_resolved, KPATH_MAX);

    long dfd = sc(SYS_OPENDIR, (long)tab_dir, 0, 0, 0, 0, 0);
    if (dfd < 0) return;

    struct k_dirent de;
    for (;;) {
        long r = sc(SYS_READDIR, dfd, (long)&de, 0, 0, 0, 0);
        if (r != 0) break;
        de.d_name[NAME_MAX_ - 1] = '\0';
        if (de.d_name[0] == '\0') continue;
        if (k_streq(de.d_name, ".") || k_streq(de.d_name, "..")) continue;

        if (starts_with(de.d_name, tab_partial, partial_len) &&
            tab_match_count < TAB_MATCH_MAX) {
            /* Build the completion: the dir prefix typed by the user + name. */
            char *m = tab_matches[tab_match_count];
            int pos = 0;
            if (last_slash >= 0) {
                /* Re-add the user's directory prefix. */
                for (int i = 0; i <= last_slash && pos < TAB_MATCH_LEN - 2; i++)
                    m[pos++] = word[i];
            }
            for (int i = 0; de.d_name[i] && pos < TAB_MATCH_LEN - 2; i++)
                m[pos++] = de.d_name[i];
            /* Append '/' for directories. */
            if (de.d_type == DT_DIR && pos < TAB_MATCH_LEN - 1)
                m[pos++] = '/';
            m[pos] = '\0';
            tab_match_count++;
        }
    }
    sc(SYS_CLOSEDIR, dfd, 0, 0, 0, 0, 0);
}

/*
 * tab_complete: called when Tab is pressed. Finds the word under the cursor
 * in line_buf[0..line_len), runs completion, and either auto-inserts a unique
 * match or shows candidates.
 *
 * Returns 1 if the display needs a re-render, 0 otherwise.
 */
static int tab_complete(void) {
    if (line_len == 0) return 0;

    /* Identify the current word (the last whitespace-delimited token). */
    int word_start = line_len;
    while (word_start > 0 && line_buf[word_start - 1] != ' ' &&
           line_buf[word_start - 1] != '\t')
        word_start--;
    int wlen = line_len - word_start;
    const char *word = line_buf + word_start;

    /* Determine if this is the first word (command position). */
    int is_first = 1;
    for (int i = 0; i < word_start; i++)
        if (line_buf[i] != ' ' && line_buf[i] != '\t') { is_first = 0; break; }

    tab_match_count = 0;
    if (is_first) {
        tab_complete_builtins(word, wlen);
        /* Also complete files in case the user is typing a path to run. */
        tab_complete_files(word, wlen);
    } else {
        tab_complete_files(word, wlen);
    }

    if (tab_match_count == 0) return 0;   /* no matches */

    int cplen = common_prefix_len();   /* longest common prefix of all matches */
    if (cplen <= wlen && tab_match_count > 1) {
        /* No additional prefix to insert. Show all candidates. */
        grid_putchar('\n');
        for (int i = 0; i < tab_match_count; i++) {
            grid_puts("  ");
            grid_puts(tab_matches[i]);
            grid_putchar('\n');
        }
        /* Re-draw the prompt + current input. */
        shell_prompt();
        for (int i = 0; i < line_len; i++) grid_putchar(line_buf[i]);
        return 1;
    }

    /* Insert the extension (characters beyond what the user already typed). */
    const char *best = tab_matches[0];   /* all share the common prefix */
    for (int i = wlen; i < cplen && line_len < LINE_MAX - 1; i++) {
        char ch = best[i];
        line_buf[line_len++] = ch;
        grid_putchar(ch);
    }
    /* If unique match and it doesn't end with '/', add a trailing space. */
    if (tab_match_count == 1 && line_len < LINE_MAX - 1) {
        char last_ch = line_buf[line_len - 1];
        if (last_ch != '/') {
            line_buf[line_len++] = ' ';
            grid_putchar(' ');
        }
    }
    return 1;
}

void _start(void) {
    print("[TERM] starting (in-process shell)\n");

    if (wl_connect() != 0) {
        print("[TERM] wl_connect FAILED\n");
        for (;;) sc(SYS_YIELD, 0, 0, 0, 0, 0, 0);
    }

    wl_window *win = wl_create_window(WIN_W, WIN_H, "Terminal");
    if (!win) {
        print("[TERM] wl_create_window FAILED\n");
        for (;;) sc(SYS_YIELD, 0, 0, 0, 0, 0, 0);
    }

    print("[TERM] window ");
    print_num(win->win_id);
    print(" created\n");

    /* Derive the grid from the granted window size (clamp to our buffers). */
    g_cols = (int)(win->w / FONT_W);
    g_rows = (int)(win->h / FONT_H);
    if (g_cols > MAX_COLS) g_cols = MAX_COLS;
    if (g_rows > MAX_ROWS) g_rows = MAX_ROWS;
    if (g_cols < 1) g_cols = 1;
    if (g_rows < 1) g_rows = 1;

    u32 stride_px = win->stride / 4u;   /* font calls want a PIXEL stride */

    /* Banner + first prompt. */
    grid_clear();
    grid_puts_color("AutomationOS", CLR_BANNERHI);
    grid_puts_color(" v0.1.0 -- terminal\n", CLR_BANNER);
    grid_puts_color("Type 'help' for commands, Tab to complete, "
                    "Up/Down for history.\n\n", CLR_BANNER);
    shell_prompt();
    line_len  = 0;
    hist_nav  = -1;   /* not currently navigating history */

    render(win, stride_px);

    long last = sc(SYS_GET_TICKS_MS, 0, 0, 0, 0, 0, 0);
    int shift_down = 0;
    int ctrl_down  = 0;
    /* US-QWERTY layout + modifier state for printable input. We track the shift
     * and ctrl edges ourselves (mirrored into km.shift_l each resolve); km also
     * owns the caps-lock toggle, folded in when a CapsLock key-DOWN reaches
     * keymap_resolve. */
    keymap_state_t km;
    keymap_reset(&km);

    for (;;) {
        int dirty = 0;

        int kind, a, b, c;
        while (wl_poll_event(win, &kind, &a, &b, &c)) {
            if (kind == WL_EVENT_RESIZE) {
                /*
                 * Maximize / snap. The library has ALREADY reallocated the
                 * buffer and updated win->{w,h,stride,pixels}; we only refresh
                 * our cached geometry so every subsequent write is bounded to
                 * the new surface. Recompute the text grid from the new size
                 * (clamped to the static grid[] dimensions) so content reflows,
                 * refresh the cached PIXEL stride, and clamp the cursor into the
                 * new grid. render() reads win->w/win->h fresh and clears the
                 * full surface, so the new margins are painted (no garbage).
                 */
                g_cols = (int)(win->w / FONT_W);
                g_rows = (int)(win->h / FONT_H);
                if (g_cols > MAX_COLS) g_cols = MAX_COLS;
                if (g_rows > MAX_ROWS) g_rows = MAX_ROWS;
                if (g_cols < 1) g_cols = 1;
                if (g_rows < 1) g_rows = 1;
                stride_px = win->stride / 4u;
                if (cur_col >= g_cols) cur_col = g_cols - 1;
                if (cur_row >= g_rows) cur_row = g_rows - 1;
                dirty = 1;
                continue;
            }
            if (kind != WL_EVENT_KEY) continue;   /* ignore pointer for now */
            int keycode = a;
            int pressed = b;

            /* Track modifier state on press AND release. */
            if (keycode == KEY_LEFTSHIFT || keycode == KEY_RIGHTSHIFT) {
                shift_down = pressed ? 1 : 0;
                continue;
            }
            if (keycode == KEY_LEFTCTRL) {
                ctrl_down = pressed ? 1 : 0;
                continue;
            }
            if (!pressed) continue;               /* key-DOWN only          */

            /* Ctrl+C: cancel the current input line and show a fresh prompt. */
            if (ctrl_down && keycode == KEY_C) {
                grid_puts("^C\n");
                line_len = 0;
                hist_nav = -1;
                shell_prompt();
                dirty = 1;
                continue;
            }
            /* Ctrl+L: clear the screen and redraw prompt + current input. */
            if (ctrl_down && keycode == KEY_L) {
                grid_clear();
                shell_prompt();
                for (int i = 0; i < line_len; i++) grid_putchar(line_buf[i]);
                dirty = 1;
                continue;
            }

            if (keycode == KEY_UP) {
                /*
                 * Up-arrow: scroll back through history.
                 * hist_nav starts at -1 (not navigating); first press → 0
                 * (most recent entry).  Clamp at the oldest available entry.
                 */
                int next_nav = hist_nav + 1;
                int available = hist_count < HIST_MAX ? hist_count : HIST_MAX;
                if (next_nav >= available) next_nav = available - 1;
                if (next_nav < 0) { /* nothing in history */ continue; }
                const char *entry = hist_get(next_nav);
                if (!entry) continue;
                /* Erase current input and replace with the recalled entry. */
                erase_input_line();
                int n = k_strlcpy(line_buf, entry, LINE_MAX);
                line_len = n;
                for (int i = 0; i < n; i++) grid_putchar(line_buf[i]);
                hist_nav = next_nav;
                dirty = 1;

            } else if (keycode == KEY_DOWN) {
                /*
                 * Down-arrow: scroll forward (towards current input).
                 * If we reach hist_nav == -1 the line is cleared.
                 */
                if (hist_nav < 0) continue;   /* already at current */
                hist_nav--;
                erase_input_line();
                if (hist_nav >= 0) {
                    const char *entry = hist_get(hist_nav);
                    if (entry) {
                        int n = k_strlcpy(line_buf, entry, LINE_MAX);
                        line_len = n;
                        for (int i = 0; i < n; i++) grid_putchar(line_buf[i]);
                    }
                }
                /* hist_nav == -1 → blank line (already line_len == 0) */
                dirty = 1;

            } else if (keycode == KEY_ENTER) {
                print("[TERM] key 10\n");          /* '\n' == 10 */
                line_buf[line_len] = '\0';
                grid_putchar('\n');                /* finish the input line  */
                hist_push(line_buf);               /* push to history        */
                hist_nav = -1;                     /* reset navigation       */
                shell_execute(line_buf);           /* run it                 */
                shell_prompt();                    /* next prompt            */
                line_len = 0;
                dirty = 1;
            } else if (keycode == KEY_TAB) {
                /* Tab completion. */
                if (tab_complete()) {
                    hist_nav = -1;
                    dirty = 1;
                }
            } else if (keycode == KEY_BACKSPACE) {
                if (line_len > 0) {
                    print("[TERM] key 8\n");        /* BS == 8 */
                    line_len--;
                    grid_backspace();
                    hist_nav = -1;                 /* any edit resets recall  */
                    dirty = 1;
                }
            } else {
                /* Mirror the locally-tracked shift into km, then let the shared
                 * keymap resolve the glyph: this folds caps-lock (a CapsLock
                 * key-DOWN, kc 58, toggles km) and the full Shift+symbol set. */
                km.shift_l = (uint8_t)(shift_down ? 1 : 0);
                km.shift_r = 0;
                char ascii = keymap_resolve((uint8_t)keycode, 1, &km);
                if (ascii && line_len < LINE_MAX - 1) {
                    print("[TERM] key ");
                    print_char(ascii);
                    print("\n");
                    line_buf[line_len++] = ascii;
                    grid_putchar(ascii);           /* echo */
                    hist_nav = -1;                 /* any edit resets recall  */
                    dirty = 1;
                }
                /* Unmapped keycodes / full line are silently ignored. */
            }
        }

        if (dirty) render(win, stride_px);

        /* Light pacing: refresh roughly each frame interval, always yield. */
        long now = sc(SYS_GET_TICKS_MS, 0, 0, 0, 0, 0, 0);
        if (now - last >= 16) last = now;
        sc(SYS_YIELD, 0, 0, 0, 0, 0, 0);
    }
}
