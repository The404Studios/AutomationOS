/*
 * reader.c -- Markdown/Doc Reader app (freestanding, ring 3).
 * ============================================================
 *
 * 720x520 window.  Left sidebar (~160px) is a table-of-contents listing
 * built-in manual pages.  Right panel renders the current page with a
 * lightweight Markdown renderer supporting:
 *
 *   # H1 / ## H2 / ### H3  -- headings (scaled via font2 if available)
 *   **bold**               -- inline bold (brighter colour)
 *   - / * bullet lists     -- indented bullets
 *   1. numbered lists      -- numbered items
 *   ``` fenced code ```    -- monospace box with tinted background
 *   > blockquote           -- indented, coloured left-bar
 *   ---                    -- horizontal rule
 *   paragraphs             -- word-wrapped to content width
 *
 * Scrolling: Up/Down arrows (+10px), Page Up/Down (+page), mouse wheel
 * (WL_EVENT_POINTER with b == scroll delta if c & 4).
 *
 * Clicking a TOC entry shows that page.
 *
 * Build (WSL Arch; exact flags; NEVER use a variable):
 *
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin \
 *       -fno-stack-protector -fno-pic -fno-pie -mno-red-zone -O2 \
 *       -c userspace/apps/reader/reader.c -o /tmp/reader.o
 *
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin \
 *       -fno-stack-protector -fno-pic -fno-pie -mno-red-zone -O2 \
 *       -c userspace/lib/wl/wl_client.c -o /tmp/wlc.o
 *
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin \
 *       -fno-stack-protector -fno-pic -fno-pie -mno-red-zone -O2 \
 *       -c userspace/lib/font/bitfont.c -o /tmp/bf.o
 *
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin \
 *       -fno-stack-protector -fno-pic -fno-pie -mno-red-zone -O2 \
 *       -c userspace/lib/font2/font2.c -o /tmp/font2.o
 *
 *   ld -nostdlib -static -n -no-pie -e _start \
 *       -T userspace/userspace.ld \
 *       /tmp/reader.o /tmp/wlc.o /tmp/bf.o /tmp/font2.o \
 *       -o /tmp/reader.elf
 *
 *   objdump -d /tmp/reader.elf | grep 'fs:0x28'   # MUST be empty
 *
 * Syscalls used (inline; no libc):
 *   SYS_WRITE = 3   serial diagnostics
 *   SYS_YIELD = 15  cooperative multitasking
 *   SYS_GET_TICKS_MS = 40
 */

#include "../../lib/wl/wl_client.h"
#include "../../lib/font/bitfont.h"
#include "../../lib/font2/font2.h"

/* ---- syscall numbers ---- */
#define SYS_WRITE         3
#define SYS_YIELD         15
#define SYS_GET_TICKS_MS  40

/* ---- types ---- */
typedef unsigned int   u32;
typedef int            i32;
typedef unsigned long  u64;
typedef long           i64;
typedef unsigned short u16;
typedef unsigned char  u8;

/* ---- inline syscall ---- */
static inline i64 sc(i64 n, i64 a1, i64 a2, i64 a3, i64 a4, i64 a5, i64 a6)
{
    i64 r;
    register i64 r10 asm("r10") = a4;
    register i64 r8  asm("r8")  = a5;
    register i64 r9  asm("r9")  = a6;
    asm volatile("syscall"
                 : "=a"(r)
                 : "a"(n), "D"(a1), "S"(a2), "d"(a3),
                   "r"(r10), "r"(r8), "r"(r9)
                 : "rcx", "r11", "memory");
    return r;
}

/* ---- serial diagnostics ---- */
static u64 k_strlen(const char *s) { u64 n = 0; while (s[n]) n++; return n; }
static void serial_print(const char *m)
{
    sc(SYS_WRITE, 1, (i64)m, (i64)k_strlen(m), 0, 0, 0);
}

/* ---- string helpers ---- */
static void k_memset(void *dst, u8 v, u64 n)
{
    u8 *d = (u8*)dst;
    for (u64 i = 0; i < n; i++) d[i] = v;
}

static void k_memcpy(void *dst, const void *src, u64 n)
{
    u8 *d = (u8*)dst;
    const u8 *s = (const u8*)src;
    for (u64 i = 0; i < n; i++) d[i] = s[i];
}

static int k_strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

static int k_strncmp(const char *a, const char *b, int n)
{
    for (int i = 0; i < n; i++) {
        if (a[i] != b[i]) return (unsigned char)a[i] - (unsigned char)b[i];
        if (!a[i]) return 0;
    }
    return 0;
}

static void k_strlcpy(char *dst, const char *src, int n)
{
    int i = 0;
    while (i < n - 1 && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

/* ---- drawing helpers ---- */
static void fill_rect(u32 *pixels, u32 stride_px, u32 bw, u32 bh,
                      i32 x, i32 y, i32 w, i32 h, u32 color)
{
    i32 x1 = x < 0 ? 0 : x;
    i32 y1 = y < 0 ? 0 : y;
    i32 x2 = x + w; if (x2 > (i32)bw) x2 = (i32)bw;
    i32 y2 = y + h; if (y2 > (i32)bh) y2 = (i32)bh;
    if (x1 >= x2 || y1 >= y2) return;
    for (i32 yy = y1; yy < y2; yy++) {
        u32 *row = pixels + (u32)yy * stride_px;
        for (i32 xx = x1; xx < x2; xx++) row[xx] = color;
    }
}

static void draw_hline(u32 *pixels, u32 stride_px, u32 bw, u32 bh,
                       i32 x, i32 y, i32 w, u32 color)
{
    fill_rect(pixels, stride_px, bw, bh, x, y, w, 1, color);
}

static void draw_vline(u32 *pixels, u32 stride_px, u32 bw, u32 bh,
                       i32 x, i32 y, i32 h, u32 color)
{
    fill_rect(pixels, stride_px, bw, bh, x, y, 1, h, color);
}

/* ---- keycodes ---- */
#define KEY_ESC         1
#define KEY_PAGEUP      104
#define KEY_PAGEDOWN    109
#define KEY_UP          103
#define KEY_DOWN        108
#define KEY_LEFT        105
#define KEY_RIGHT       106

/* ---- window / layout geometry ---- */
#define WIN_W       720
#define WIN_H       520
#define TOC_W       160
#define DIVIDER_W   1
#define TITLEBAR_H  34
#define STATUS_H    22

#define CONTENT_X    (TOC_W + DIVIDER_W)
#define CONTENT_W    (WIN_W - CONTENT_X)
#define CONTENT_TOP  TITLEBAR_H
#define CONTENT_H    (WIN_H - TITLEBAR_H - STATUS_H)
#define CONTENT_BOT  (CONTENT_TOP + CONTENT_H)

/* Content text area inset */
#define TEXT_MARGIN_L  12
#define TEXT_MARGIN_R  16
#define TEXT_X         (CONTENT_X + TEXT_MARGIN_L)
#define TEXT_W         (CONTENT_W - TEXT_MARGIN_L - TEXT_MARGIN_R)

/* ---- color palette (dark theme) ---- */
#define COL_BG            0xFF1E1E2Eu  /* main background               */
#define COL_TOC_BG        0xFF181825u  /* sidebar background            */
#define COL_TOC_SEL       0xFF313244u  /* selected TOC entry            */
#define COL_TOC_HOV       0xFF252538u  /* hovered TOC entry             */
#define COL_TITLEBAR      0xFF11111Bu  /* title bar                     */
#define COL_STATUS_BG     0xFF11111Bu  /* status bar                    */
#define COL_DIVIDER       0xFF45475Au  /* sidebar divider               */
#define COL_TEXT          0xFFCDD6F4u  /* body text (catppuccin lavender)*/
#define COL_TEXT_DIM      0xFF6C7086u  /* dimmed / secondary text       */
#define COL_H1            0xFF89B4FAu  /* H1 colour (blue)              */
#define COL_H2            0xFFA6E3A1u  /* H2 colour (green)             */
#define COL_H3            0xFFF9E2AFu  /* H3 colour (yellow)            */
#define COL_BOLD          0xFFCBA6F7u  /* bold inline text (mauve)      */
#define COL_CODE_BG       0xFF181825u  /* code block tinted background  */
#define COL_CODE_FG       0xFFA6E3A1u  /* code block foreground         */
#define COL_CODE_BORDER   0xFF45475Au  /* code block border             */
#define COL_QUOTE_BAR     0xFF89DCEB u /* blockquote left bar (sky)     */
#define COL_QUOTE_TEXT    0xFF9399B2u  /* blockquote text               */
#define COL_BULLET        0xFFF38BA8u  /* bullet / list marker (red)    */
#define COL_RULE          0xFF45475Au  /* horizontal rule               */
#define COL_TOC_TXT       0xFFBAC2DEu  /* TOC entry text                */
#define COL_TOC_SEL_TXT   0xFFCDD6F4u  /* selected TOC text             */
#define COL_TOC_HEADER    0xFF89B4FAu  /* TOC section header            */
#define COL_ACCENT        0xFF89B4FAu  /* accent / app name             */
#define COL_SCROLLBAR_BG  0xFF181825u  /* scrollbar track               */
#define COL_SCROLLBAR_FG  0xFF45475Au  /* scrollbar thumb               */

/* ---- rendered line types ---- */
#define LT_PARA      0   /* normal paragraph text (word-wrapped) */
#define LT_H1        1   /* # Heading 1 */
#define LT_H2        2   /* ## Heading 2 */
#define LT_H3        3   /* ### Heading 3 */
#define LT_BULLET    4   /* - / * bullet item */
#define LT_NUMBERED  5   /* 1. numbered item */
#define LT_CODE      6   /* inside ``` fence */
#define LT_BLOCKQUOTE 7  /* > blockquote */
#define LT_RULE      8   /* --- horizontal rule */
#define LT_BLANK     9   /* blank / spacer */

/* Maximum characters in a single rendered text segment. */
#define SEG_MAX  256

/*
 * A single visual "rendered line" after layout.
 * The renderer pre-processes each markdown source line into one or more
 * RenderedLine entries that the draw loop can handle directly.
 */
typedef struct {
    u8   type;              /* LT_* */
    char text[SEG_MAX];     /* displayable text, stripped of markdown syntax */
    int  num;               /* for LT_NUMBERED: the item number */
    int  indent;            /* extra left indent in pixels */
    int  has_bold_start;    /* index in text[] where bold region begins (-1 if none) */
    int  has_bold_end;      /* index in text[] where bold region ends */
} RenderedLine;

/* Max rendered lines per page. */
#define MAX_RLINES  1024

static RenderedLine g_rlines[MAX_RLINES];
static int          g_rline_count;

/* Scroll state (pixel offset into the rendered content). */
static int g_scroll_y;
static int g_max_scroll;   /* updated after layout */

/* ================================================================
 * MANUAL PAGE CONTENT
 * Four hard-coded pages of the AutomationOS Manual.
 * ================================================================ */

/* Each page is a NUL-terminated multi-line C string.
 * Lines are separated by '\n'.  Markdown syntax as described in header. */

static const char *g_page_welcome =
"# Welcome to AutomationOS\n"
"\n"
"AutomationOS is a from-scratch x86_64 operating system designed for clarity,\n"
"performance, and a pleasant desktop experience.  It runs entirely in protected\n"
"mode, boots via GRUB2, and supports multiple ring-3 userspace processes with\n"
"full memory isolation.\n"
"\n"
"## What Makes AutomationOS Different\n"
"\n"
"- **Pure freestanding userspace** -- every app is an ELF with no libc, no\n"
"  dynamic linker, and no kernel-provided runtime.  Apps talk to the kernel\n"
"  through a small, stable syscall ABI.\n"
"- **Wayland-lite compositor** -- a lightweight IPC-based compositor (wl) lets\n"
"  apps create pixel-buffer windows without needing a display server protocol.\n"
"- **Cooperative scheduling** -- the kernel provides SYS_YIELD so apps can\n"
"  voluntarily hand back the CPU; a simple round-robin scheduler keeps the\n"
"  desktop responsive.\n"
"- **Freestanding fonts** -- the bitfont (8x16 IBM VGA) and font2 (scaled,\n"
"  anti-aliased, gradient) libraries render text without any external font\n"
"  resources.\n"
"\n"
"## System Architecture\n"
"\n"
"```\n"
"  +-----------------------------+\n"
"  |   Userspace Applications    | ring 3\n"
"  |  (wl_client + bitfont/font2)|\n"
"  +-----------------------------+\n"
"  |   Compositor  (wl server)   | ring 3\n"
"  +-----------------------------+\n"
"  |   Kernel (scheduler, VFS,   | ring 0\n"
"  |   PS/2, framebuffer, GDT)   |\n"
"  +-----------------------------+\n"
"  |   GRUB2 boot + loader.c     | real/prot mode\n"
"  +-----------------------------+\n"
"```\n"
"\n"
"## Getting Started\n"
"\n"
"The desktop shell starts automatically after boot.  You can launch apps from\n"
"the taskbar or by pressing **Alt+Space** to open the app launcher.\n"
"\n"
"Use the left panel to navigate to other sections of this manual.\n"
"\n"
"---\n"
"\n"
"## Core Subsystems\n"
"\n"
"### Memory\n"
"\n"
"Each process gets its own page table (CR3).  The kernel uses a simple\n"
"first-fit physical allocator.  Stack guard pages are set to not-present\n"
"to catch overflows.\n"
"\n"
"### File System\n"
"\n"
"A RAM-backed VFS exposes a POSIX-like open/read/write/close interface.\n"
"Files are stored in memory and persist across the session.  /tmp is\n"
"always available.\n"
"\n"
"### PS/2 Keyboard\n"
"\n"
"The PS/2 driver reads scan codes from port 0x60 and translates them to\n"
"Linux-compatible keycodes via an internal table.  Events are delivered to\n"
"the focused window through the compositor event queue.\n";

static const char *g_page_shortcuts =
"# Keyboard Shortcuts\n"
"\n"
"AutomationOS uses an **Alt-key** convention for window management, keeping\n"
"the Ctrl key free for application use.\n"
"\n"
"## Window Management\n"
"\n"
"| Action              | Shortcut          |\n"
"| ------------------- | ----------------- |\n"
"| Switch window       | Alt + Tab         |\n"
"| Close window        | Alt + Q  or  F4   |\n"
"| Minimize window     | Alt + M           |\n"
"| Force-quit process  | Alt + K           |\n"
"| Move window         | Alt + drag        |\n"
"| Snap to edge        | drag to screen edge|\n"
"\n"
"---\n"
"\n"
"## Snap Zones\n"
"\n"
"Drag a window to the left or right edge of the screen and release to snap\n"
"it to that half.  Drag to the top edge to maximise.  Drag away from an\n"
"edge to restore.\n"
"\n"
"```\n"
"  +--------+--------+\n"
"  |        |        |\n"
"  | snap L | snap R |\n"
"  |        |        |\n"
"  +--------+--------+\n"
"     drag to edge\n"
"```\n"
"\n"
"## Desktop Shortcuts\n"
"\n"
"- **Alt+Space** -- open the app launcher\n"
"- **Alt+Tab** -- cycle through open windows\n"
"- **Alt+Q** -- close the focused window\n"
"- **Alt+M** -- minimise to taskbar\n"
"- **Alt+K** -- force-quit (sends SIGKILL equivalent)\n"
"- **Alt+F4** -- close focused window (Windows-style alias)\n"
"\n"
"## Application Shortcuts\n"
"\n"
"These are standard across built-in apps:\n"
"\n"
"- **F2** -- save (Notes, editor apps)\n"
"- **F5** -- new / refresh\n"
"- **Ctrl+C** -- copy (where supported)\n"
"- **Ctrl+V** -- paste (where supported)\n"
"- **Escape** -- cancel / close dialog\n"
"- **Tab** -- move focus to next widget\n"
"\n"
"## Reader (this app)\n"
"\n"
"- **Up / Down arrows** -- scroll 10px\n"
"- **Page Up / Page Down** -- scroll one page\n"
"- **Click TOC entry** -- jump to that manual page\n"
"\n"
"---\n"
"\n"
"> Tip: All shortcuts work when a window has keyboard focus. Click a window\n"
"> to give it focus.\n";

static const char *g_page_apps =
"# Apps & Games\n"
"\n"
"AutomationOS ships a growing catalog of built-in applications and games,\n"
"all written as freestanding ring-3 ELFs.\n"
"\n"
"## Productivity\n"
"\n"
"### Notes\n"
"\n"
"A multi-line text editor with a file sidebar.  Notes are saved to /tmp/notes\n"
"as plain .txt files.  Supports keyboard editing, mouse cursor placement,\n"
"and F2-save / F5-new.\n"
"\n"
"### Reader (this app)\n"
"\n"
"A Markdown document reader with a built-in manual.  Renders headings, bold,\n"
"code blocks, blockquotes, bullet lists, numbered lists, and horizontal rules.\n"
"\n"
"### Sheet\n"
"\n"
"A simple spreadsheet with formula evaluation for basic arithmetic.\n"
"\n"
"### Calculator\n"
"\n"
"A desktop calculator supporting +, -, *, / with keyboard and mouse input.\n"
"\n"
"## System Tools\n"
"\n"
"### Terminal\n"
"\n"
"A VT100-compatible terminal emulator running the kernel shell.  Supports\n"
"colour escape sequences, scrollback, and tab completion.\n"
"\n"
"### File Explorer\n"
"\n"
"Browse the in-memory VFS.  Supports drag-and-drop, copy, paste, delete,\n"
"rename, and a live search bar.\n"
"\n"
"### Task Manager\n"
"\n"
"Shows running processes, CPU and memory usage.  Allows sending force-quit\n"
"to any process.\n"
"\n"
"### Settings\n"
"\n"
"Configure display brightness, keyboard repeat rate, theme accent colours,\n"
"and accessibility options (high contrast, large text).\n"
"\n"
"---\n"
"\n"
"## Games\n"
"\n"
"### Snake\n"
"\n"
"Classic snake game with arrow-key controls.  Grows on each food pellet;\n"
"level increases every 5 pellets.\n"
"\n"
"### Tetris\n"
"\n"
"Falling-block puzzle.  Arrow keys move/rotate; Down arrow soft-drops;\n"
"Space hard-drops.\n"
"\n"
"### 2048\n"
"\n"
"Slide tiles with arrow keys to merge matching numbers.  Reach 2048 to win.\n"
"\n"
"---\n"
"\n"
"## Media\n"
"\n"
"### Paint\n"
"\n"
"A pixel-art canvas with brush, fill, and a 16-colour palette.  Draw with\n"
"the mouse; right-click to pick colour.\n"
"\n"
"### Image Viewer\n"
"\n"
"Displays images loaded from the VFS.  Supports zoom in/out and pan.\n"
"\n"
"### Synth\n"
"\n"
"A software synthesizer with 4 oscillators, ADSR envelope, and a virtual\n"
"keyboard rendered on screen.\n";

static const char *g_page_dev =
"# For Developers\n"
"\n"
"AutomationOS is designed to be hackable.  Every component is a C source\n"
"file you can read, modify, and recompile.  This page explains how to build\n"
"and run your own app.\n"
"\n"
"## App Anatomy\n"
"\n"
"An AutomationOS app is a **freestanding ELF64** binary:\n"
"\n"
"- No libc, no dynamic linker, no C runtime startup.\n"
"- Entry point is `_start` (not `main`).\n"
"- All syscalls are inlined assembly (`syscall` instruction).\n"
"- Links against `wl_client.o` (compositor) and `bitfont.o` (font).\n"
"- Optionally links `font2.o` for scaled/gradient headings.\n"
"\n"
"## Build Flags\n"
"\n"
"```\n"
"gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin \\\n"
"    -fno-stack-protector -fno-pic -fno-pie -mno-red-zone -O2 \\\n"
"    -c myapp.c -o myapp.o\n"
"```\n"
"\n"
"> **-fno-stack-protector** is mandatory.  Without it, gcc emits a\n"
"> `mov %fs:0x28, %rax` canary check that faults immediately because\n"
"> the kernel does not set up an FS base for userspace.\n"
"\n"
"## Link Command\n"
"\n"
"```\n"
"ld -nostdlib -static -n -no-pie -e _start \\\n"
"   -T userspace/userspace.ld \\\n"
"   myapp.o wlc.o bf.o \\\n"
"   -o myapp.elf\n"
"```\n"
"\n"
"Verify no canary:\n"
"\n"
"```\n"
"objdump -d myapp.elf | grep 'fs:0x28'  # must print nothing\n"
"```\n"
"\n"
"## Syscall ABI\n"
"\n"
"Inline syscalls use the Linux x86-64 convention:\n"
"\n"
"```\n"
"rax = syscall number\n"
"rdi, rsi, rdx, r10, r8, r9 = arguments 1-6\n"
"rax = return value (negative = error)\n"
"```\n"
"\n"
"### Key Syscall Numbers\n"
"\n"
"- **SYS_READ = 2** -- read(fd, buf, len)\n"
"- **SYS_WRITE = 3** -- write(fd, buf, len)\n"
"- **SYS_OPEN = 4** -- open(path, flags, mode)\n"
"- **SYS_CLOSE = 5** -- close(fd)\n"
"- **SYS_YIELD = 15** -- yield CPU to scheduler\n"
"- **SYS_GETPID = 20** -- getpid()\n"
"- **SYS_OPENDIR = 30** -- opendir(path)\n"
"- **SYS_READDIR = 31** -- readdir(dfd, dirent*)\n"
"- **SYS_CLOSEDIR = 32** -- closedir(dfd)\n"
"- **SYS_GET_TICKS_MS = 40** -- milliseconds since boot\n"
"\n"
"## The wl Client Library\n"
"\n"
"Include `userspace/lib/wl/wl_client.h` and link `wl_client.o`.\n"
"\n"
"```\n"
"wl_connect();                        // connect to compositor\n"
"wl_window *w = wl_create_window(\n"
"    width, height, \"Title\");         // allocate shm pixel buffer\n"
"// draw into w->pixels (ARGB32)\n"
"wl_commit(w);                        // send frame to compositor\n"
"wl_poll_event(w, &kind, &a,&b,&c);  // non-blocking event poll\n"
"```\n"
"\n"
"Events:\n"
"\n"
"- **WL_EVENT_KEY (3)** -- a=keycode, b=pressed, c=0\n"
"- **WL_EVENT_POINTER (2)** -- a=x, b=y, c=buttons\n"
"\n"
"## The Font Libraries\n"
"\n"
"### bitfont (8x16 monospace)\n"
"\n"
"```\n"
"font_draw_char(pixels, stride, w, h, x, y, ch, 0xFFCDD6F4);\n"
"font_draw_string(pixels, stride, w, h, x, y, \"hello\", color);\n"
"```\n"
"\n"
"### font2 (scaled + anti-aliased)\n"
"\n"
"```\n"
"font2_draw_scaled(pixels, stride, x, y, \"H1 Title\", 3, 0xFF89B4FA);\n"
"font2_draw_gradient(pixels, stride, x, y, \"Fancy\", 2,\n"
"                    0xFF89B4FA, 0xFF45475A);\n"
"font2_draw_aa(pixels, stride, x, y, \"smooth\", 0xFFCDD6F4, 0xFF1E1E2E);\n"
"```\n"
"\n"
"## Game Library\n"
"\n"
"Include `userspace/lib/game/game.h` for sprite rendering, collision\n"
"helpers, and a simple state-machine framework useful for game loops.\n"
"\n"
"## UI Library\n"
"\n"
"Include `userspace/lib/ui/ui.h` for buttons, checkboxes, sliders, and\n"
"text inputs with mouse/keyboard handling built in.\n"
"\n"
"---\n"
"\n"
"## Tips\n"
"\n"
"1. Start from `userspace/apps/notes/notes.c` -- it is well-commented.\n"
"2. Static global arrays are your friend (no heap allocator needed).\n"
"3. Always route path strings through a zeroed 256-byte buffer before\n"
"   passing to the kernel to avoid page-fault on short strings.\n"
"4. Call `SYS_YIELD` at the end of every frame loop iteration.\n"
"5. Use `serial_print` (SYS_WRITE fd=1) for debug output visible in QEMU\n"
"   serial console.\n";

/* ---- TOC entries ---- */
#define MAX_PAGES  8

typedef struct {
    const char *title;
    const char *content;
} Page;

static Page g_pages[MAX_PAGES];
static int  g_page_count;
static int  g_cur_page;

/* Mouse state */
static int g_mouse_x;
static int g_mouse_y;
static int g_prev_btn;

/* ================================================================
 * MARKDOWN LAYOUT ENGINE
 * Converts a markdown source string into a flat array of RenderedLine.
 * ================================================================ */

/* Check if character is whitespace */
static int is_space(char c) { return c == ' ' || c == '\t'; }

/* Strip leading/trailing spaces, return new length */
static int trim_right(char *buf, int len)
{
    while (len > 0 && is_space(buf[len-1])) len--;
    buf[len] = '\0';
    return len;
}

/* Parse inline **bold** markers.
 * Copies text from src to dst, stripping ** markers.
 * Sets bold_start / bold_end to the indices in dst where bold region is.
 * Only handles the first bold span for simplicity. */
static void parse_inline(const char *src, char *dst, int max_dst,
                         int *bold_start, int *bold_end)
{
    *bold_start = -1;
    *bold_end   = -1;
    int di = 0;
    int si = 0;
    int in_bold = 0;
    int src_len = (int)k_strlen(src);

    while (si < src_len && di < max_dst - 1) {
        /* Check for ** */
        if (src[si] == '*' && src[si+1] == '*') {
            if (!in_bold) {
                if (*bold_start == -1) *bold_start = di;
                in_bold = 1;
            } else {
                if (*bold_end == -1) *bold_end = di;
                in_bold = 0;
            }
            si += 2;
            continue;
        }
        dst[di++] = src[si++];
    }
    dst[di] = '\0';
    /* If still in bold, close it */
    if (in_bold && *bold_end == -1) *bold_end = di;
}

/*
 * Word-wrap helper: emit wrapped lines from src_text into g_rlines[].
 * type, indent, bold region are preserved on each wrapped line.
 * max_chars: approximate max characters per line based on pixel width.
 */
static void emit_wrapped(int type, const char *text, int indent,
                         int bold_start, int bold_end, int max_chars)
{
    if (max_chars < 1) max_chars = 1;
    int len = (int)k_strlen(text);
    int pos = 0;

    /* Handle empty line */
    if (len == 0) {
        if (g_rline_count < MAX_RLINES) {
            RenderedLine *rl = &g_rlines[g_rline_count++];
            k_memset(rl, 0, sizeof(*rl));
            rl->type = (u8)type;
            rl->indent = indent;
            rl->has_bold_start = -1;
            rl->has_bold_end   = -1;
        }
        return;
    }

    while (pos < len && g_rline_count < MAX_RLINES) {
        /* Find how many chars fit */
        int avail = len - pos;
        int take = avail < max_chars ? avail : max_chars;

        /* If not at end, try to break at word boundary */
        if (pos + take < len) {
            int bp = take;
            while (bp > 0 && !is_space(text[pos + bp])) bp--;
            if (bp > 0) take = bp + 1; /* include the space */
        }

        /* Skip leading space on continuation lines */
        int start = pos;
        if (pos > 0) {
            while (start < pos + take && is_space(text[start])) start++;
        }

        RenderedLine *rl = &g_rlines[g_rline_count++];
        k_memset(rl, 0, sizeof(*rl));
        rl->type   = (u8)type;
        rl->indent = indent;

        int copy = pos + take - start;
        if (copy < 0) copy = 0;
        if (copy > SEG_MAX - 1) copy = SEG_MAX - 1;
        k_memcpy(rl->text, text + start, (u64)copy);
        rl->text[copy] = '\0';
        trim_right(rl->text, copy);

        /* Adjust bold range for this chunk */
        if (bold_start >= 0) {
            int bs = bold_start - start;
            int be = bold_end   - start;
            int chunk_len = (int)k_strlen(rl->text);
            if (be > 0 && bs < chunk_len) {
                rl->has_bold_start = bs < 0 ? 0 : bs;
                rl->has_bold_end   = be > chunk_len ? chunk_len : be;
            } else {
                rl->has_bold_start = -1;
                rl->has_bold_end   = -1;
            }
        } else {
            rl->has_bold_start = -1;
            rl->has_bold_end   = -1;
        }

        pos += take;
    }
}

/* Add a simple (no-wrap) line. */
static void emit_line(int type, const char *text, int indent,
                      int bold_start, int bold_end, int num)
{
    if (g_rline_count >= MAX_RLINES) return;
    RenderedLine *rl = &g_rlines[g_rline_count++];
    k_memset(rl, 0, sizeof(*rl));
    rl->type  = (u8)type;
    rl->indent = indent;
    rl->num   = num;
    rl->has_bold_start = bold_start;
    rl->has_bold_end   = bold_end;
    k_strlcpy(rl->text, text, SEG_MAX);
}

/*
 * Layout the markdown content into g_rlines[].
 * max_chars: characters per line for word-wrap.
 */
static void layout_markdown(const char *md, int max_chars)
{
    g_rline_count = 0;

    /* We'll walk line by line through the source. */
    const char *p = md;
    int in_code_fence = 0;
    int numbered_index = 1; /* current list number */

    while (*p) {
        /* Extract one source line */
        const char *line_start = p;
        while (*p && *p != '\n') p++;
        int line_len = (int)(p - line_start);
        if (*p == '\n') p++;

        /* Copy to a mutable buffer */
        char line[SEG_MAX];
        int ll = line_len < SEG_MAX - 1 ? line_len : SEG_MAX - 1;
        k_memcpy(line, line_start, (u64)ll);
        line[ll] = '\0';

        /* ---- Code fence toggle ---- */
        if (ll >= 3 && line[0] == '`' && line[1] == '`' && line[2] == '`') {
            if (!in_code_fence) {
                /* opening fence; show any language tag as comment */
                in_code_fence = 1;
                /* blank line above the code block for spacing */
                emit_line(LT_BLANK, "", 0, -1, -1, 0);
            } else {
                in_code_fence = 0;
                emit_line(LT_BLANK, "", 0, -1, -1, 0);
            }
            continue;
        }

        if (in_code_fence) {
            emit_line(LT_CODE, line, 0, -1, -1, 0);
            continue;
        }

        /* ---- Blank line ---- */
        {
            int blank = 1;
            for (int i = 0; i < ll; i++) if (!is_space(line[i])) { blank = 0; break; }
            if (blank) {
                emit_line(LT_BLANK, "", 0, -1, -1, 0);
                numbered_index = 1;
                continue;
            }
        }

        /* ---- Horizontal rule --- */
        if (ll >= 3 &&
            (k_strncmp(line, "---", 3) == 0 ||
             k_strncmp(line, "===", 3) == 0 ||
             k_strncmp(line, "***", 3) == 0)) {
            int all_dash = 1;
            for (int i = 0; i < ll; i++) {
                if (line[i] != '-' && line[i] != '=' && line[i] != '*'
                    && !is_space(line[i])) { all_dash = 0; break; }
            }
            if (all_dash) {
                emit_line(LT_RULE, "", 0, -1, -1, 0);
                continue;
            }
        }

        /* ---- Headings ---- */
        if (line[0] == '#') {
            int level = 0;
            while (level < ll && line[level] == '#') level++;
            /* Skip space after hashes */
            int text_start = level;
            while (text_start < ll && is_space(line[text_start])) text_start++;
            const char *htext = line + text_start;
            int type = (level == 1) ? LT_H1 : (level == 2) ? LT_H2 : LT_H3;
            /* Headings: parse bold inline */
            char plain[SEG_MAX];
            int bs, be;
            parse_inline(htext, plain, SEG_MAX, &bs, &be);
            /* Add blank line before H1/H2 */
            if ((type == LT_H1 || type == LT_H2) && g_rline_count > 0) {
                emit_line(LT_BLANK, "", 0, -1, -1, 0);
            }
            emit_line(type, plain, 0, bs, be, 0);
            emit_line(LT_BLANK, "", 0, -1, -1, 0);
            continue;
        }

        /* ---- Blockquote ---- */
        if (line[0] == '>') {
            int text_start = 1;
            while (text_start < ll && is_space(line[text_start])) text_start++;
            char plain[SEG_MAX];
            int bs, be;
            parse_inline(line + text_start, plain, SEG_MAX, &bs, &be);
            emit_wrapped(LT_BLOCKQUOTE, plain, 16, bs, be, max_chars - 4);
            continue;
        }

        /* ---- Bullet list (- or * or + ) ---- */
        if ((line[0] == '-' || line[0] == '*' || line[0] == '+') &&
            ll > 1 && is_space(line[1])) {
            int text_start = 2;
            while (text_start < ll && is_space(line[text_start])) text_start++;
            char plain[SEG_MAX];
            int bs, be;
            parse_inline(line + text_start, plain, SEG_MAX, &bs, &be);
            emit_wrapped(LT_BULLET, plain, 24, bs, be, max_chars - 4);
            continue;
        }

        /* ---- Numbered list (1. 2. etc) ---- */
        if (ll > 2 && line[0] >= '1' && line[0] <= '9') {
            int di = 0;
            int num = 0;
            while (di < ll && line[di] >= '0' && line[di] <= '9') {
                num = num * 10 + (line[di] - '0');
                di++;
            }
            if (di < ll && line[di] == '.' && di < ll - 1 && is_space(line[di+1])) {
                int text_start = di + 2;
                while (text_start < ll && is_space(line[text_start])) text_start++;
                char plain[SEG_MAX];
                int bs, be;
                parse_inline(line + text_start, plain, SEG_MAX, &bs, &be);
                /* First wrapped line gets the number; continuation gets blank */
                if (g_rline_count < MAX_RLINES) {
                    emit_wrapped(LT_NUMBERED, plain, 24, bs, be, max_chars - 4);
                    /* Stamp the number onto the first line of this item */
                    /* Find the first wrapped line we just added */
                    int first_nl = g_rline_count - 1;
                    /* Walk back to find first line of this wrapped block */
                    int start_nl = first_nl;
                    /* Since emit_wrapped adds lines sequentially, find them */
                    /* Simple approach: the last N lines are this item; stamp num */
                    /* We need to tag only the first -- use a small trick: */
                    /* We can't easily find the first, so stamp the one BEFORE
                     * the call was at rline_count. Track it manually. */
                    (void)num;
                    /* Stamp num on all numbered lines; renderer will show it
                     * only on the first (non-zero num detection). */
                    for (int ni = start_nl; ni <= first_nl; ni++) {
                        g_rlines[ni].num = numbered_index;
                    }
                }
                numbered_index++;
                continue;
            }
        }

        /* ---- Skip table rows (| ... |) -- render as code for readability ---- */
        if (line[0] == '|') {
            emit_line(LT_CODE, line, 0, -1, -1, 0);
            continue;
        }

        /* ---- Normal paragraph text ---- */
        {
            char plain[SEG_MAX];
            int bs, be;
            parse_inline(line, plain, SEG_MAX, &bs, &be);
            emit_wrapped(LT_PARA, plain, 0, bs, be, max_chars);
        }
    }
}

/* ================================================================
 * RENDERING
 * ================================================================ */

/* pt_in_rect */
static int pt_in_rect(int px, int py, int rx, int ry, int rw, int rh)
{
    return (px >= rx && px < rx + rw && py >= ry && py < ry + rh);
}

/*
 * Draw text with optional bold region.
 * Bold region [bold_start, bold_end) is rendered in COL_BOLD.
 * Returns x after the entire string.
 */
static int draw_inline_text(u32 *pix, int spx, int bw, int bh,
                            int x, int y, const char *text,
                            u32 normal_col, u32 bold_col,
                            int bold_start, int bold_end)
{
    int len = (int)k_strlen(text);
    if (bold_start < 0 || bold_start >= len) {
        /* No bold: draw whole string */
        return font_draw_string(pix, spx, bw, bh, x, y, text, normal_col) + x;
    }

    /* Pre-bold segment */
    if (bold_start > 0) {
        char seg[SEG_MAX];
        int slen = bold_start < SEG_MAX - 1 ? bold_start : SEG_MAX - 1;
        k_memcpy(seg, text, (u64)slen);
        seg[slen] = '\0';
        x += font_draw_string(pix, spx, bw, bh, x, y, seg, normal_col);
    }
    /* Bold segment */
    {
        int blen = bold_end - bold_start;
        if (blen > SEG_MAX - 1) blen = SEG_MAX - 1;
        char seg[SEG_MAX];
        k_memcpy(seg, text + bold_start, (u64)blen);
        seg[blen] = '\0';
        x += font_draw_string(pix, spx, bw, bh, x, y, seg, bold_col);
    }
    /* Post-bold segment */
    if (bold_end < len) {
        x += font_draw_string(pix, spx, bw, bh, x, y, text + bold_end, normal_col);
    }
    return x;
}

/* Height of a single rendered line in pixels, by type. */
static int line_height(int type)
{
    switch (type) {
    case LT_H1:    return FONT_H * 3 + 6;   /* font2 scale 3 */
    case LT_H2:    return FONT_H * 2 + 4;   /* font2 scale 2 */
    case LT_H3:    return FONT_H * 1 + 4;   /* font2 scale 1 but styled */
    case LT_BLANK: return FONT_H / 2;       /* half-line gap */
    case LT_RULE:  return 10;
    case LT_CODE:  return FONT_H + 2;
    default:       return FONT_H + 3;       /* para, bullet, etc */
    }
}

/* Compute total content height. */
static int total_content_height(void)
{
    int h = 0;
    for (int i = 0; i < g_rline_count; i++)
        h += line_height(g_rlines[i].type);
    return h;
}

/* Draw one number string into pix; returns pixel width used. */
static void draw_num(u32 *pix, int spx, int bw, int bh,
                     int x, int y, int num, u32 col)
{
    char buf[16];
    int n = num; int i = 0;
    if (n == 0) { buf[i++] = '0'; }
    else { char tmp[12]; int j=0; while(n>0){tmp[j++]=(char)('0'+n%10);n/=10;}
           while(j>0) buf[i++]=tmp[--j]; }
    buf[i++] = '.'; buf[i++] = ' '; buf[i] = '\0';
    font_draw_string(pix, spx, bw, bh, x, y, buf, col);
}

/*
 * Render the content panel.  All y positions are in window-space;
 * g_scroll_y is subtracted to get the visual offset.
 */
static void render_content(u32 *pix, u32 spx, u32 bw, u32 bh)
{
    /* Content area background */
    fill_rect(pix, (u32)spx, bw, bh, CONTENT_X, CONTENT_TOP,
              CONTENT_W, CONTENT_H, COL_BG);

    /* Scrollbar */
    {
        int track_x = WIN_W - 8;
        int track_y = CONTENT_TOP;
        int track_h = CONTENT_H;
        fill_rect(pix, (u32)spx, bw, bh, track_x, track_y, 6, track_h, COL_SCROLLBAR_BG);

        int total_h = total_content_height();
        if (total_h > CONTENT_H) {
            int thumb_h = CONTENT_H * CONTENT_H / total_h;
            if (thumb_h < 20) thumb_h = 20;
            int thumb_y = track_y + g_scroll_y * (track_h - thumb_h) /
                          (total_h - CONTENT_H);
            fill_rect(pix, (u32)spx, bw, bh, track_x, thumb_y, 6, thumb_h,
                      COL_SCROLLBAR_FG);
        }
    }

    /* Clip drawing to the content region */
    int cy   = CONTENT_TOP;
    int clip_top = CONTENT_TOP;
    int clip_bot = CONTENT_BOT;

    /* Accumulate virtual Y (document space) */
    int doc_y = 0;

    for (int i = 0; i < g_rline_count; i++) {
        RenderedLine *rl = &g_rlines[i];
        int lh = line_height(rl->type);
        int win_y = cy + doc_y - g_scroll_y;  /* window-space y */
        doc_y += lh;

        /* Skip lines above clip */
        if (win_y + lh < clip_top) continue;
        /* Stop lines below clip */
        if (win_y > clip_bot) break;

        int tx = TEXT_X + rl->indent;
        int ty = win_y;

        switch (rl->type) {

        case LT_BLANK:
            break;

        case LT_RULE:
            /* Horizontal rule with accent tints */
            draw_hline(pix, (u32)spx, bw, bh,
                       TEXT_X, ty + 4, TEXT_W - 8, COL_RULE);
            break;

        case LT_H1:
            /* Scale 3 gradient heading */
            font2_draw_gradient(pix, (int)spx, tx, ty,
                                rl->text, 3,
                                COL_H1, 0xFF6699CCu);
            break;

        case LT_H2:
            /* Scale 2 solid heading */
            font2_draw_scaled(pix, (int)spx, tx, ty,
                              rl->text, 2, COL_H2);
            break;

        case LT_H3:
            /* 1x but bold colour + underline */
            font_draw_string(pix, (int)spx, (int)bw, (int)bh,
                             tx, ty, rl->text, COL_H3);
            /* underline */
            {
                int tw = font_text_width(rl->text);
                draw_hline(pix, (u32)spx, bw, bh, tx, ty + FONT_H + 1, tw, COL_H3);
            }
            break;

        case LT_CODE:
            /* Tinted background for the code line */
            fill_rect(pix, (u32)spx, bw, bh,
                      TEXT_X - 4, ty, TEXT_W, lh, COL_CODE_BG);
            font_draw_string(pix, (int)spx, (int)bw, (int)bh,
                             tx, ty + 1, rl->text, COL_CODE_FG);
            break;

        case LT_BLOCKQUOTE:
            /* Left colour bar */
            draw_vline(pix, (u32)spx, bw, bh,
                       TEXT_X + 2, ty, lh - 2, COL_QUOTE_BAR);
            font_draw_string(pix, (int)spx, (int)bw, (int)bh,
                             tx + 8, ty, rl->text, COL_QUOTE_TEXT);
            break;

        case LT_BULLET:
            /* Bullet dot */
            fill_rect(pix, (u32)spx, bw, bh,
                      TEXT_X + 8, ty + FONT_H/2 - 2, 4, 4, COL_BULLET);
            draw_inline_text(pix, (int)spx, (int)bw, (int)bh,
                             tx, ty, rl->text,
                             COL_TEXT, COL_BOLD,
                             rl->has_bold_start, rl->has_bold_end);
            break;

        case LT_NUMBERED:
            /* Number */
            draw_num(pix, (int)spx, (int)bw, (int)bh,
                     TEXT_X, ty, rl->num, COL_BULLET);
            draw_inline_text(pix, (int)spx, (int)bw, (int)bh,
                             tx, ty, rl->text,
                             COL_TEXT, COL_BOLD,
                             rl->has_bold_start, rl->has_bold_end);
            break;

        case LT_PARA:
        default:
            draw_inline_text(pix, (int)spx, (int)bw, (int)bh,
                             tx, ty, rl->text,
                             COL_TEXT, COL_BOLD,
                             rl->has_bold_start, rl->has_bold_end);
            break;
        }
    }

    /* Draw a subtle border between first code block lines and surroundings */
    /* (done inline above; no extra pass needed) */
}

/* Render the TOC sidebar. */
static void render_toc(u32 *pix, u32 spx, u32 bw, u32 bh)
{
    fill_rect(pix, (u32)spx, bw, bh, 0, TITLEBAR_H, TOC_W, CONTENT_H, COL_TOC_BG);

    /* "Contents" header */
    fill_rect(pix, (u32)spx, bw, bh, 0, TITLEBAR_H, TOC_W, 24, COL_TITLEBAR);
    font_draw_string(pix, (int)spx, (int)bw, (int)bh,
                     6, TITLEBAR_H + 4, "Contents", COL_TOC_HEADER);
    draw_hline(pix, (u32)spx, bw, bh, 0, TITLEBAR_H + 24, TOC_W, COL_DIVIDER);

    int entry_h = 28;
    int toc_top = TITLEBAR_H + 26;

    for (int i = 0; i < g_page_count; i++) {
        int ey = toc_top + i * entry_h;
        int hover = pt_in_rect(g_mouse_x, g_mouse_y, 0, ey, TOC_W, entry_h);

        u32 bg, fg;
        if (i == g_cur_page) {
            bg = COL_TOC_SEL;
            fg = COL_TOC_SEL_TXT;
        } else if (hover) {
            bg = COL_TOC_HOV;
            fg = COL_TOC_TXT;
        } else {
            bg = COL_TOC_BG;
            fg = COL_TEXT_DIM;
        }
        fill_rect(pix, (u32)spx, bw, bh, 0, ey, TOC_W, entry_h, bg);

        /* Selection bar on left edge */
        if (i == g_cur_page) {
            fill_rect(pix, (u32)spx, bw, bh, 0, ey, 3, entry_h, COL_ACCENT);
        }

        /* Truncate title to fit */
        char disp[24];
        k_strlcpy(disp, g_pages[i].title, sizeof(disp));
        font_draw_string(pix, (int)spx, (int)bw, (int)bh,
                         8, ey + (entry_h - FONT_H) / 2, disp, fg);

        draw_hline(pix, (u32)spx, bw, bh, 0, ey + entry_h - 1, TOC_W, COL_DIVIDER);
    }
}

/* Render the title bar. */
static void render_titlebar(u32 *pix, u32 spx, u32 bw, u32 bh)
{
    fill_rect(pix, (u32)spx, bw, bh, 0, 0, WIN_W, TITLEBAR_H, COL_TITLEBAR);
    draw_hline(pix, (u32)spx, bw, bh, 0, TITLEBAR_H - 1, WIN_W, COL_DIVIDER);

    /* App title (font2 scale 2) */
    font2_draw_scaled(pix, (int)spx, 10, (TITLEBAR_H - FONT_H * 2) / 2,
                      "Reader", 2, COL_ACCENT);

    /* Current page title */
    if (g_cur_page < g_page_count) {
        const char *ptitle = g_pages[g_cur_page].title;
        int tx = 10 + font2_text_width("Reader", 2) + 16;
        /* Separator dot */
        font_draw_string(pix, (int)spx, (int)bw, (int)bh,
                         tx, (TITLEBAR_H - FONT_H) / 2 + 2, "|", COL_DIVIDER);
        font_draw_string(pix, (int)spx, (int)bw, (int)bh,
                         tx + 12, (TITLEBAR_H - FONT_H) / 2 + 2, ptitle, COL_TEXT_DIM);
    }
}

/* Render the status bar. */
static void render_status(u32 *pix, u32 spx, u32 bw, u32 bh)
{
    int sy = WIN_H - STATUS_H;
    fill_rect(pix, (u32)spx, bw, bh, 0, sy, WIN_W, STATUS_H, COL_STATUS_BG);
    draw_hline(pix, (u32)spx, bw, bh, 0, sy, WIN_W, COL_DIVIDER);

    font_draw_string(pix, (int)spx, (int)bw, (int)bh,
                     CONTENT_X + 4, sy + (STATUS_H - FONT_H) / 2,
                     "Up/Down: scroll  |  PgUp/PgDn: page  |  Click TOC: navigate",
                     COL_TEXT_DIM);
}

/* Full frame render. */
static void render(wl_window *win)
{
    u32 spx = win->stride / 4u;
    u32 bw  = win->w;
    u32 bh  = win->h;
    u32 *pix = win->pixels;

    render_titlebar(pix, spx, bw, bh);
    render_toc(pix, spx, bw, bh);

    /* Divider between TOC and content */
    draw_vline(pix, (u32)spx, bw, bh, TOC_W, TITLEBAR_H, CONTENT_H, COL_DIVIDER);

    render_content(pix, spx, bw, bh);
    render_status(pix, spx, bw, bh);
}

/* ================================================================
 * PAGE SWITCHING
 * ================================================================ */

/* Approximate chars per line for word-wrap given content pixel width. */
#define WRAP_CHARS  ((TEXT_W - 8) / FONT_W)

static void switch_page(int idx)
{
    if (idx < 0 || idx >= g_page_count) return;
    g_cur_page = idx;
    g_scroll_y = 0;

    /* Layout the new page */
    layout_markdown(g_pages[idx].content, WRAP_CHARS);

    /* Compute max scroll */
    int total = total_content_height();
    g_max_scroll = total - CONTENT_H;
    if (g_max_scroll < 0) g_max_scroll = 0;

    serial_print("[READER] page ");
    {
        char buf[4]; buf[0] = (char)('0' + idx); buf[1] = '\0';
        serial_print(buf);
    }
    serial_print(": ");
    serial_print(g_pages[idx].title);
    serial_print("\n");
}

/* ================================================================
 * INPUT
 * ================================================================ */

static void scroll_by(int delta)
{
    g_scroll_y += delta;
    if (g_scroll_y < 0) g_scroll_y = 0;
    if (g_scroll_y > g_max_scroll) g_scroll_y = g_max_scroll;
}

static void handle_key(int keycode, int pressed)
{
    if (!pressed) return;
    switch (keycode) {
    case KEY_UP:       scroll_by(-10); break;
    case KEY_DOWN:     scroll_by(+10); break;
    case KEY_PAGEUP:   scroll_by(-CONTENT_H + FONT_H); break;
    case KEY_PAGEDOWN: scroll_by(+CONTENT_H - FONT_H); break;
    case KEY_LEFT:
        if (g_cur_page > 0) switch_page(g_cur_page - 1);
        break;
    case KEY_RIGHT:
        if (g_cur_page < g_page_count - 1) switch_page(g_cur_page + 1);
        break;
    default: break;
    }
}

static void handle_click(int mx, int my)
{
    /* TOC click */
    if (mx < TOC_W && my >= TITLEBAR_H + 26 && my < CONTENT_BOT) {
        int entry_h = 28;
        int idx = (my - (TITLEBAR_H + 26)) / entry_h;
        if (idx >= 0 && idx < g_page_count) {
            switch_page(idx);
        }
    }
}

/* ================================================================
 * _start
 * ================================================================ */
void _start(void)
{
    serial_print("[READER] starting\n");

    /* ---- Register manual pages ---- */
    g_page_count = 0;

    g_pages[g_page_count].title   = "Welcome";
    g_pages[g_page_count].content = g_page_welcome;
    g_page_count++;

    g_pages[g_page_count].title   = "Shortcuts";
    g_pages[g_page_count].content = g_page_shortcuts;
    g_page_count++;

    g_pages[g_page_count].title   = "Apps & Games";
    g_pages[g_page_count].content = g_page_apps;
    g_page_count++;

    g_pages[g_page_count].title   = "For Developers";
    g_pages[g_page_count].content = g_page_dev;
    g_page_count++;

    /* Initialise state */
    g_cur_page = 0;
    g_scroll_y = 0;
    g_max_scroll = 0;
    g_mouse_x = 0;
    g_mouse_y = 0;
    g_prev_btn = 0;

    /* Layout first page */
    switch_page(0);

    /* ---- Connect to compositor ---- */
    if (wl_connect() != 0) {
        serial_print("[READER] wl_connect FAILED\n");
        for (;;) sc(SYS_YIELD, 0, 0, 0, 0, 0, 0);
    }

    wl_window *win = wl_create_window(WIN_W, WIN_H, "Reader");
    if (!win) {
        serial_print("[READER] wl_create_window FAILED\n");
        for (;;) sc(SYS_YIELD, 0, 0, 0, 0, 0, 0);
    }

    serial_print("[READER] window created\n");

    /* ---- Event loop ---- */
    for (;;) {
        int kind, a, b, c;
        while (wl_poll_event(win, &kind, &a, &b, &c)) {
            if (kind == WL_EVENT_KEY) {
                handle_key(a, b);
            } else if (kind == WL_EVENT_POINTER) {
                g_mouse_x = a;
                g_mouse_y = b;
                int cur_btn = c;
                if (cur_btn && !g_prev_btn) {
                    handle_click(a, b);
                }
                /* Mouse wheel: some compositors encode scroll in b when c&4 */
                if (c & 4) {
                    scroll_by(b * 3);
                }
                g_prev_btn = cur_btn;
            }
        }

        render(win);
        wl_commit(win);
        sc(SYS_YIELD, 0, 0, 0, 0, 0, 0);
    }
}
