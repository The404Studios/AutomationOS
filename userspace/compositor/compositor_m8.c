/*
 * compositor_m6.c -- Milestone M6 WINDOW MANAGER (ring 3): m5 + WM features.
 *
 * Copied verbatim from compositor_m5.c (verified working animated desktop
 * shell). m5 is left untouched; m6 ADDS true window-manager behavior on top.
 * Keeps ALL m5 behavior: top panel (clock + focused title), bottom dock
 * (launcher spawns sbin/terminal + per-window taskbar), zero-copy SHM client
 * compositing, animation engine (open/close/minimize/restore, fixed-point),
 * rounded corners + soft shadows + wallpaper, font titlebars, mouse window-
 * management (drag/focus/raise/close/minimize), keyboard + pointer forwarding
 * to the focused window, chrome reservation, unconditional SYS_YIELD per frame.
 *
 * NEW IN M6 (window-manager layer)
 * ================================
 *  1. WINDOW SNAPPING. While dragging a window by its titlebar, if the cursor
 *     reaches a screen edge we show a translucent snap PREVIEW of where the
 *     window will land; on release the window SNAPS and animates (geometry
 *     tween, reusing the animation clock) to:
 *       - LEFT  edge  -> left half  of the work area (panel..dock).
 *       - RIGHT edge  -> right half of the work area.
 *       - TOP   edge  -> MAXIMIZED  (fills the whole work area).
 *     Each window stores its pre-snap geometry (saved_x/y/w/h). Dragging a
 *     snapped window away RESTORES that geometry (also animated) before the
 *     normal move resumes, so snapping is fully reversible.
 *
 *  2. ALT+TAB window cycling. We track Left-Alt (KEY_LEFTALT) press/release
 *     and Tab (KEY_TAB) from the forwarded keyboard stream. While Alt is held,
 *     each Tab press cycles focus/raise to the next window in a MOST-RECENTLY-
 *     USED ring (an MRU list maintained on every focus). Alt+Tab is intercepted
 *     in the compositor and is NOT forwarded to the focused client.
 *
 *  3. KEYBOARD SHORTCUTS (intercepted, not forwarded):
 *       - Alt+Q  or  Alt+F4  -> close the focused window (close animation).
 *       - Alt+M              -> minimize the focused window.
 *       - Alt+D              -> show desktop (minimize all windows).
 *     Everything else (and every key while Alt is NOT held) is forwarded to
 *     the focused client exactly as in m5.
 *
 *  4. NOTIFICATION TOAST. A transient top-right toast with fade-in/hold/fade-
 *     out, driven by SYS_GET_TICKS_MS. Demoed at startup with
 *     "Welcome to AutomationOS" for a few seconds.
 *
 *  KEY INTERCEPTION CONTRACT: keyboard events are pumped by the compositor
 *  (pump_input, keyboard==1). m6 routes each EV_KEY through wm_handle_key()
 *  FIRST; that function updates Alt state, consumes Alt+Tab / Alt+Q / Alt+F4 /
 *  Alt+M, and returns 1 ("consumed") for those. Only un-consumed keys are
 *  forwarded to the focused client via send_key_to_focus(), so normal client
 *  typing is unaffected.
 *
 * NEW IN M5
 * =========
 *  1. ANIMATION ENGINE (frame-clock driven via SYS_GET_TICKS_MS). Each window
 *     slot carries an animation phase (NONE/OPENING/CLOSING/MINIMIZING/
 *     RESTORING), anim_start_ms, anim_dur_ms, and a per-frame computed scale
 *     (FIXED-POINT, /256) + alpha (0..256).
 *       - OPEN  (~180ms): scale 0.90 -> 1.00 (ease-out-cubic), alpha 0 -> 256.
 *       - CLOSE (~150ms): scale 1.00 -> 0.90 (ease-in-cubic), alpha 256 -> 0;
 *         the slot is destroyed (shmdt + free) only when t>=1.
 *       - MINIMIZE (~220ms): the drawn rect shrinks + slides toward the
 *         window's taskbar button, alpha -> 0; the slot stays alive + hidden
 *         and keeps its taskbar entry.
 *       - RESTORE  (~220ms): reverse of minimize (taskbar rect -> geometry).
 *     A window whose phase != NONE is drawn via blit_surface_scaled_alpha()
 *     (nearest-neighbor scale about a center point + alpha blend); otherwise
 *     the normal 1:1 opaque blit is used.
 *
 *     FIXED-POINT, NOT FLOAT: scale is an int in 1/256 units (Q8). Easing is
 *     evaluated on integer t in [0,256]. This deliberately avoids gcc emitting
 *     libgcc soft-float helpers (__mulsf3 etc.) that won't link under
 *     -nostdlib.
 *
 *  2. VISUAL POLISH
 *       - Rounded window OUTER corners (radius ~8px): corner pixels of the
 *         frame/border/titlebar are clipped so windows read as rounded.
 *       - Soft layered drop shadow (4 translucent rects, growing + fading,
 *         offset down-right) so windows lift off the wallpaper.
 *       - Wallpaper: tasteful full-screen vertical gradient
 *         (0xFF101826 navy top -> 0xFF1B2A3A bottom), per-scanline.
 *       - Dock: rounded top corners + rounded launcher button + subtle hover
 *         highlight on dock/taskbar items under the cursor.
 *
 * Aether Dark palette: desktop 0xFF1C1C1E, panel/dock 0xFF2C2C2E,
 *   hover 0xFF3A3A3C, text 0xFFFFFFFF, text-dim 0xFFAEAEB2, accent 0xFF0A84FF,
 *   border 0xFF38383A, close 0xFFFF5F57, min 0xFFFFBD47.
 *
 * Build (EXACT -- flags DIRECT on the cmdline, never via an unquoted shell
 * var, or -fno-stack-protector is dropped and it faults at CR2=0x28):
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -O2 \
 *       -c userspace/compositor/compositor_m6.c -o /tmp/cm6.o
 *   gcc <same flags> -c userspace/lib/font/bitfont.c -o /tmp/bf.o
 *   ld -nostdlib -static -n -no-pie -e _start -T userspace/userspace.ld \
 *       /tmp/cm6.o /tmp/bf.o -o /tmp/cm6.elf
 *   objdump -d /tmp/cm6.elf | grep "fs:0x28"   # MUST be empty
 */

typedef unsigned char       uint8_t;
typedef unsigned short      uint16_t;
typedef unsigned int        uint32_t;
typedef unsigned long       size_t;
typedef unsigned long       uint64_t;   /* match <stdint.h> __UINT64_TYPE__ (LP64) */
typedef int                 int32_t;
typedef long                int64_t;    /* match <stdint.h> __INT64_TYPE__  (LP64) */

#include "../lib/icon/icon.h"

/* ---- syscall numbers (from kernel/include/syscall.h) ---- */
#define SYS_READ          2
#define SYS_WRITE         3
#define SYS_OPEN          4
#define SYS_YIELD         15
#define SYS_SLEEP          9      /* blocking ms sleep (frees the CPU for clients) */
#define SYS_SPAWN         16
#define SYS_SPAWN_EX_ARGV 106   /* spawn with a NUL-split argv vector (spaces survive) */
#define SYS_SHMAT         19
#define SYS_SHMDT         20
#define SYS_SHMCTL        21
#define IPC_STAT      0x2000     /* shmctl: get segment status (size) */
#define SYS_MSGGET        22
#define SYS_MSGSND        23
#define SYS_MSGRCV        24
#define SYS_KILL          26
#define SYS_OPENDIR       30
#define SYS_READDIR       31
#define SYS_CLOSEDIR      32
#define SYS_MMAP          37
#define SYS_FB_ACQUIRE    39
#define SYS_GET_TICKS_MS  40
#define SYS_TIME          41     /* RTC: Unix epoch seconds          */
#define SYS_GETTIME       42     /* RTC: fill rtc_time_t* with time  */
#define SYS_MKDIR         67
#define SYS_SYSINFO       62     /* procapi: system memory/uptime     */
#define SYS_BATTERY       93     /* EC battery: {present,state,%,ac}  */
#define SYS_NET_INFO      59     /* query IP/MAC/link state           */

/* net_info_t -- mirrors kernel uapi_net_info_t (kernel/include/uapi/net.h).
 * Used by the panel network indicator (queried once per second). */
typedef struct {
    char     ifname[16];
    uint8_t  mac[6];
    uint8_t  _pad[2];
    uint32_t ip;            /* host byte order */
    uint32_t netmask;
    uint32_t gateway;
    uint32_t dns;
    uint8_t  up;
    uint8_t  dhcp;
    uint8_t  _reserved[6];
    uint64_t tx_packets;
    uint64_t rx_packets;
    uint64_t tx_bytes;
    uint64_t rx_bytes;
} comp_net_info_t;

/* Broken-down calendar time from the CMOS RTC (mirrors kernel rtc_time_t). */
typedef struct {
    uint16_t year;    /* full 4-digit year, e.g. 2026 */
    uint8_t  month;   /* 1..12 */
    uint8_t  day;     /* 1..31 */
    uint8_t  hour;    /* 0..23 */
    uint8_t  min;     /* 0..59 */
    uint8_t  sec;     /* 0..59 */
} comp_rtc_time_t;

/* sysinfo_t -- mirrors kernel procapi.h (first 32 bytes).  Extra battery
 * fields extend the struct for future kernel battery support; the kernel
 * only copies 32 bytes today, so the extras stay zeroed (bat_present=0). */
typedef struct {
    uint64_t total_mem;     /* total physical memory in bytes          */
    uint64_t free_mem;      /* free physical memory in bytes           */
    uint64_t uptime_ms;     /* milliseconds since boot                 */
    uint32_t proc_count;    /* number of live processes                */
    uint32_t heap_used_kb;  /* kernel heap bytes in use / 1024         */
    /* --- extended fields (zeroed until kernel fills them) --- */
    uint32_t bat_present;   /* 1 if battery detected                   */
    uint32_t bat_state;     /* 0=idle, 1=discharging, 2=charging       */
    uint32_t bat_percent;   /* 0-100                                   */
    uint32_t bat_ac;        /* 1 if AC adapter connected               */
} comp_sysinfo_t;

/* ---- directory entry (mirror of kernel struct dirent, kernel/include/vfs.h) ----
 * The kernel copies sizeof(struct dirent) bytes into the user buffer, so the
 * field layout MUST match exactly: d_ino(8) d_off(8) d_reclen(2) d_type(1)
 * d_name[256]. */
#define DESK_NAME_MAX  256
#define DT_DIR         4
struct dirent {
    uint64_t d_ino;
    int64_t  d_off;
    uint16_t d_reclen;
    uint8_t  d_type;
    char     d_name[DESK_NAME_MAX];
};

/* signals for SYS_KILL */
#define SIGKILL           9
#define SIGTERM           15

/* mmap prot bits */
#define VMM_PROT_READ   0x01
#define VMM_PROT_WRITE  0x02

/* open() flags (from kernel/include/vfs.h + evdev O_NONBLOCK) */
#define O_RDONLY    0x0000
#define O_NONBLOCK  0x0800

/* SysV IPC flags (from kernel/include/ipc.h) */
#define IPC_CREAT   0x0200
#define IPC_NOWAIT  0x0800

/* IPC error codes we care about (kernel returns these) */
#define IPC_ENOMSG  -42   /* no message of desired type (NOWAIT, queue empty) */

/* ---- 3-arg and 6-arg inline syscalls ---- */
static inline long syscall(long n, long a1, long a2, long a3) {
    long ret;
    asm volatile("syscall" : "=a"(ret) : "a"(n), "D"(a1), "S"(a2), "d"(a3)
                 : "rcx", "r11", "memory");
    return ret;
}

static inline long sc6(long n, long a1, long a2, long a3, long a4, long a5, long a6) {
    long r;
    register long r10 asm("r10") = a4, r8 asm("r8") = a5, r9 asm("r9") = a6;
    asm volatile("syscall" : "=a"(r)
                 : "a"(n), "D"(a1), "S"(a2), "d"(a3), "r"(r10), "r"(r8), "r"(r9)
                 : "rcx", "r11", "memory");
    return r;
}

/* ====================================================================== *
 *  PERF: frame-dirty gating + on-screen stats overlay                    *
 * ---------------------------------------------------------------------- *
 *  composite() re-blits every window into the back buffer each frame, so *
 *  its cost scales with the number of open windows. On a slow framebuffer *
 *  (e.g. the ThinkPad T410) the desktop therefore lags more with each app *
 *  opened. The fix is a frame-dirty flag: when GENUINELY nothing changed  *
 *  this frame, we skip composite()+present entirely and burn no CPU.      *
 *                                                                         *
 *  g_dirty is the gate. EVERY change path sets it (input, window lifecycle, *
 *  client surface commits, animations, dock hover). When in any doubt the *
 *  code marks dirty -- a wrongly-SKIPPED frame (stale display) is a worse  *
 *  bug than a wrongly-DRAWN one, so the bias is hard toward marking dirty. *
 *                                                                         *
 *  COMPOSITOR_STATS gates a tiny corner overlay (FPS / frame-time ms /     *
 *  window count / pixels presented) so the owner can measure "1 app vs 5   *
 *  apps" live on the T410. It is cheap and toggleable at runtime with     *
 *  Alt+S (F11/F12 are NOT wired through the kernel PS/2 keymap, so Alt+S    *
 *  -- which the WM already intercepts as a modifier chord -- is the hook).  *
 * ---------------------------------------------------------------------- */
#ifndef COMPOSITOR_STATS
#define COMPOSITOR_STATS 0        /* overlay OFF by default (on-by-default forced the heavy path every frame -> froze the mouse); Alt+S toggles it on to spot-check */
#endif

/* Frame-dirty flag. Seeded to 1 so the very first frame (boot fade + initial
 * desktop) always composites + presents. Set by mark_dirty() on every change
 * path; cleared in the frame loop after a composite+present. */
static int g_dirty = 1;
static inline void mark_dirty(void) { g_dirty = 1; }

/* ====================================================================== *
 *  DAMAGE SCISSOR -- scene-damage accumulator + global scissor rect       *
 * ---------------------------------------------------------------------- *
 *  See docs/COMPOSITOR_DAMAGE_SCISSOR_DESIGN.md for the full rationale.   *
 *  The accumulator (g_dmg_*) collects per-frame damage from client        *
 *  commits, WM actions, and chrome changes.  At the top of the dirty      *
 *  block the ACTIVE SCISSOR (g_scis_*) is set from the accumulator so     *
 *  composite() rasterizers only touch the dirty rect.  present_diff() is  *
 *  unchanged -- it still bounds the FB write by scanning back vs prev.    *
 *                                                                          *
 *  GATED: with COMP_DAMAGE_SCISSOR=0 the scissor stays full-screen every  *
 *  frame and the behavior is byte-identical to the pre-scissor codepath.  *
 * ---------------------------------------------------------------------- */
#ifndef COMP_DAMAGE_SCISSOR
#define COMP_DAMAGE_SCISSOR 1     /* ON: rasterizers clamp to damage scissor  */
#endif

/* Active scissor rect.  Every rasterizer (fill_rect, blend_rect, blit_*)
 * clamps to this in addition to the buffer bounds.  Declared above fill_rect
 * so all rasterizers below see them.  Initialised to full-screen in _start
 * after g_fb_w/g_fb_h are known. */
static int32_t g_scis_x0 = 0, g_scis_y0 = 0;
static int32_t g_scis_x1 = 0x7FFF, g_scis_y1 = 0x7FFF;   /* safe pre-init: huge => no-op clamp */

/* Damage accumulator (union of dirty rects this frame). */
static int32_t g_dmg_x0, g_dmg_y0, g_dmg_x1, g_dmg_y1;
static int     g_dmg_any = 0;       /* 1 = at least one damage_add this frame */

/* Full-damage cooldown: CREATE/DESTROY/RESIZE force N frames of full-screen
 * recomposite so z-order changes, shadow reveals, wallpaper exposure etc.
 * settle without partial-rect artifacts. */
#define FULL_DAMAGE_COOLDOWN_FRAMES 3
static int g_full_damage_cooldown = 0;

/* Shadow extent pad: worst-case shadow reach is ~23px bottom; 24 covers it. */
#define SHADOW_PAD 24

/* -- helpers (called after g_fb_w/g_fb_h are set, so they read the live values) -- */
static inline void scissor_reset_full(void) {
    g_scis_x0 = 0;          g_scis_y0 = 0;
    g_scis_x1 = 0x7FFF;     g_scis_y1 = 0x7FFF;  /* effectively full-screen */
}
static inline void damage_reset(void) {
    g_dmg_any = 0;
    g_dmg_x0 = 0; g_dmg_y0 = 0; g_dmg_x1 = 0; g_dmg_y1 = 0;
}
static inline void damage_add(int32_t x0, int32_t y0, int32_t x1, int32_t y1) {
    /* clamp to screen (g_fb_w/g_fb_h may be 0 before init; harmless) */
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    /* union */
    if (!g_dmg_any) {
        g_dmg_x0 = x0; g_dmg_y0 = y0; g_dmg_x1 = x1; g_dmg_y1 = y1;
        g_dmg_any = 1;
    } else {
        if (x0 < g_dmg_x0) g_dmg_x0 = x0;
        if (y0 < g_dmg_y0) g_dmg_y0 = y0;
        if (x1 > g_dmg_x1) g_dmg_x1 = x1;
        if (y1 > g_dmg_y1) g_dmg_y1 = y1;
    }
}
static inline void damage_add_full(void) {
    damage_add(0, 0, 0x7FFF, 0x7FFF);
}

/* Stats overlay runtime state. g_stats_on toggles the overlay (Alt+S). The
 * other fields are sampled each presented frame so the overlay shows live nums. */
static int      g_stats_on        = COMPOSITOR_STATS;
static long     g_last_frame_ms   = 0;   /* tick at the last PRESENTED frame    */
static long     g_frame_dt_ms     = 0;   /* ms since the previous presented frame */
static long     g_fps_x10         = 0;   /* FPS * 10 (one decimal), smoothed    */
static uint32_t g_present_px      = 0;   /* pixels written to the FB last present */
static int      g_present_did     = 0;   /* 1 = present_diff actually wrote      */

/* Forward decl: the toast's remaining-visible duration is defined further down
 * (next to render_toast), but anim_tick() -- which is above it -- reads it to
 * keep the frame dirty while a toast is fading in/out. */
static int32_t g_toast_dur_ms;

/* ---- tiny serial diagnostics ---- */
static size_t k_strlen(const char *s) { size_t l = 0; while (s[l]) l++; return l; }
static void print(const char *m) { syscall(SYS_WRITE, 1, (long)m, (long)k_strlen(m)); }
static void print_num(long n) {
    char b[24]; int i = 0;
    if (n < 0) { print("-"); n = -n; }
    do { b[i++] = (char)('0' + (n % 10)); n /= 10; } while (n > 0);
    while (i > 0) { char c = b[--i]; syscall(SYS_WRITE, 1, (long)&c, 1); }
}

#ifdef SELFHEAL
/* ====================================================================== *
 *  SELFHEAL — per-frame liveness heartbeat (gated; compiled out by default) *
 * ---------------------------------------------------------------------- *
 *  Publish a monotonic frame counter + timestamp into a SysV SHM page that
 *  init (PID 1) creates+owns, so sbin/cwatchdog can tell a LIVE desktop from
 *  a frozen one and trigger recovery.  See userspace/include/selfheal.h for
 *  the ownership + one-shot-latch reasoning.  selfheal.h uses only primitive
 *  types, so this -ffreestanding TU (with its own int typedefs) includes it
 *  directly with no collision and zero field drift.                         *
 * ---------------------------------------------------------------------- */
#include "../include/selfheal.h"
#define SYS_GETPID   8
#define SYS_SHMGET  18
/* SYS_SHMAT(19) + SYS_GET_TICKS_MS(40) are already #defined above. */

static volatile sh_heartbeat_t* g_hb = (volatile sh_heartbeat_t*)0;
/* SELFHEAL v2: the window registry lives in the SAME init-owned page (offset
 * SH_WINREG_OFFSET) so a respawned compositor can RESTORE the desktop instead
 * of coming back empty. Mirrored by selfheal_reg_sync (definitions live after
 * the window table/protocol code they need); read once by
 * selfheal_restore_windows at respawn. */
static volatile sh_winreg_ent_t* g_wreg = (volatile sh_winreg_ent_t*)0;
static int g_sh_respawn = 0;            /* selfheal_init: was_init latch        */
static void selfheal_reg_sync(void);
static void selfheal_restore_windows(uint32_t fb_w, uint32_t fb_h);
#ifdef SELFHEAL_FREEZE
#ifndef FREEZE_AT_FRAME
#define FREEZE_AT_FRAME 240        /* ~4s into the loop @ ~60fps */
#endif
#ifndef FREEZE_MODE
#define FREEZE_MODE 0              /* 0=blocking (recoverable on default), 1=tight-loop (needs PREEMPT) */
#endif
static uint64_t g_freeze_at = 0;   /* 0 = never freeze; set only for the FIRST instance */
#endif

/* Attach the heartbeat page LOOKUP-ONLY (init owns it; never IPC_CREAT here, so
 * the compositor can never become the owner — see selfheal.h).  `magic` doubles
 * as a "a prior instance already ran" latch: a fresh page (init zeroed it) reads
 * magic==0 -> first instance; a value that survived a kill reads magic==MAGIC ->
 * this is a respawn. */
static void selfheal_init(void) {
    /* Lookup-only: pass the real size + NO IPC_CREAT. This kernel's sys_shmget
     * rejects size==0 (shm.c:275), but with a valid size and the key already
     * present it returns the existing segment (shm.c:289-299) WITHOUT creating —
     * so we attach init's page and never become the owner. */
    long id = sc6(SYS_SHMGET, (long)SELFHEAL_SHM_KEY, (long)SELFHEAL_SHM_SIZE, 0, 0, 0, 0);
    if (id < 0) { print("[SHELL] SELFHEAL: heartbeat segment missing\n"); return; }
    long p = sc6(SYS_SHMAT, id, 0, 0, 0, 0, 0);                        /* RW attach */
    if (p <= 0) { print("[SHELL] SELFHEAL: heartbeat shmat FAILED\n"); return; }
    g_hb   = (volatile sh_heartbeat_t*)p;
    g_wreg = (volatile sh_winreg_ent_t*)((char*)p + SH_WINREG_OFFSET);
    unsigned int was_init = (g_hb->magic == SELFHEAL_MAGIC);           /* respawn? */
    g_sh_respawn = (int)was_init;       /* _start runs the window restore on respawn */
    g_hb->version        = SELFHEAL_VERSION;
    g_hb->compositor_pid = (unsigned int)syscall(SYS_GETPID, 0, 0, 0);
    if (!was_init) g_hb->frame_counter = 0;
    g_hb->last_frame_ms  = (unsigned long long)syscall(SYS_GET_TICKS_MS, 0, 0, 0);
    g_hb->state          = SH_STATE_RUNNING;
    g_hb->magic          = SELFHEAL_MAGIC;                             /* publish LAST */
#ifdef SELFHEAL_FREEZE
    g_freeze_at = was_init ? 0 : FREEZE_AT_FRAME;   /* only the first instance freezes */
#endif
    print("[SHELL] SELFHEAL: heartbeat published\n");
}
#endif /* SELFHEAL */

/* ---- framebuffer geometry returned by SYS_FB_ACQUIRE ---- */
typedef struct { uint64_t vaddr; uint32_t width, height, pitch, bpp; } fb_acquire_t;

/* ====================================================================== *
 *  WAYLAND-LIKE PROTOCOL (local mirror of wl_proto.h)                     *
 * ====================================================================== */
#define WL_COMP_INBOX_KEY   0x434F4D50          /* "COMP" -- server inbox  */
#define WL_REPLY_KEY(pid)   (0x52000000 + (pid))/* per-client event queue  */

/* client -> server message types */
#define WL_REQ_CREATE   1
#define WL_REQ_COMMIT   2
#define WL_REQ_DESTROY  3
#define WL_REQ_RESIZE   4   /* client reallocated its buffer (new shm_id)         */

/* server -> client message types */
#define WL_EVT_CREATED  1
#define WL_EVT_POINTER  2
#define WL_EVT_KEY      3
#define WL_EVT_CONFIGURE 4  /* ask client to resize to w x h                      */

#define WL_TITLE_MAX    48

/* mtype is the SysV message type; the kernel treats it as 8 bytes (int64_t)
 * and the payload follows it. msgsz passed to msgsnd/msgrcv is the size of
 * the payload AFTER mtype (verified against kernel/ipc/msgqueue.c). */
typedef struct {
    int64_t  mtype;
    int32_t  pid;
    int32_t  shm_id;
    uint32_t w, h, stride;        /* stride in PIXELS (client pixel pitch)  */
    char     title[WL_TITLE_MAX];
} wl_req_create_t;

typedef struct {
    int64_t  mtype;
    int32_t  win_id;
    uint32_t x, y, w, h;          /* committed damage rect (we mark dirty)  */
} wl_req_commit_t;

typedef struct {
    int64_t  mtype;
    int32_t  win_id;
} wl_req_destroy_t;

/* client -> server: new buffer after a WL_EVT_CONFIGURE (byte-compatible with
 * wl_resize_req in wl_proto.h: long==int64_t, int==int32_t, uint==uint32_t). */
typedef struct {
    int64_t  mtype;
    int32_t  win_id;
    int32_t  shm_id;
    uint32_t w, h, stride;
} wl_req_resize_t;

typedef struct {
    int64_t  mtype;
    int32_t  win_id;
} wl_evt_created_t;

/* server -> client: ask the client to resize to w x h. */
typedef struct {
    int64_t  mtype;
    int32_t  win_id;
    uint32_t w, h;
} wl_evt_configure_t;

typedef struct {
    int64_t  mtype;
    int32_t  x, y, buttons;
    int32_t  wheel;  /* mouse wheel delta (positive=scroll up/away, negative=scroll down/toward) */
} wl_evt_pointer_t;

typedef struct {
    int64_t  mtype;
    int32_t  keycode, pressed;
} wl_evt_key_t;

/* A receive buffer large enough for the biggest client->server message
 * (WL_REQ_CREATE). We msgrcv() with type 0 (any) into this union. */
typedef union {
    int64_t          mtype;       /* common first field */
    wl_req_create_t  create;
    wl_req_commit_t  commit;
    wl_req_destroy_t destroy;
    wl_req_resize_t  resize;
    char             raw[128];
} wl_inbox_msg_t;

/* ====================================================================== *
 *  Pixel math (self-contained, from compositor_m2/m3.c)                   *
 * ====================================================================== */
static inline uint32_t blend_pixel(uint32_t src, uint32_t dst) {
    uint32_t a = (src >> 24) & 0xFF;
    if (a == 0xFF) return src;
    if (a == 0)    return dst;
    uint32_t sr = (src >> 16) & 0xFF, sg = (src >> 8) & 0xFF, sb = src & 0xFF;
    uint32_t dr = (dst >> 16) & 0xFF, dg = (dst >> 8) & 0xFF, db = dst & 0xFF;
    uint32_t ia = 255 - a;
    uint32_t or_ = (sr * a + dr * ia) / 255;
    uint32_t og  = (sg * a + dg * ia) / 255;
    uint32_t ob  = (sb * a + db * ia) / 255;
    return 0xFF000000u | (or_ << 16) | (og << 8) | ob;
}

/* Blend an opaque-RGB src over dst with an explicit 0..256 alpha (Q8). This is
 * the workhorse for animated (scaled) window content where each pixel inherits
 * the window's animation alpha rather than its own A channel. */
static inline uint32_t blend_pixel_a256(uint32_t src, uint32_t dst, uint32_t a256) {
    if (a256 >= 256) return 0xFF000000u | (src & 0x00FFFFFFu);
    if (a256 == 0)   return dst;
    uint32_t sr = (src >> 16) & 0xFF, sg = (src >> 8) & 0xFF, sb = src & 0xFF;
    uint32_t dr = (dst >> 16) & 0xFF, dg = (dst >> 8) & 0xFF, db = dst & 0xFF;
    uint32_t ia = 256 - a256;
    uint32_t r = (sr * a256 + dr * ia) >> 8;
    uint32_t g = (sg * a256 + dg * ia) >> 8;
    uint32_t b = (sb * a256 + db * ia) >> 8;
    return 0xFF000000u | (r << 16) | (g << 8) | b;
}

static void fill_rect(uint32_t *buf, uint32_t bw, uint32_t bh, uint32_t stride,
                      int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color) {
    int32_t x1 = x < 0 ? 0 : x;
    int32_t y1 = y < 0 ? 0 : y;
    int32_t x2 = x + w;
    int32_t y2 = y + h;
    if (x2 > (int32_t)bw) x2 = (int32_t)bw;
    if (y2 > (int32_t)bh) y2 = (int32_t)bh;
#if COMP_DAMAGE_SCISSOR
    if (x1 < g_scis_x0) x1 = g_scis_x0;
    if (y1 < g_scis_y0) y1 = g_scis_y0;
    if (x2 > g_scis_x1) x2 = g_scis_x1;
    if (y2 > g_scis_y1) y2 = g_scis_y1;
#endif
    if (x1 >= x2 || y1 >= y2) return;
    /* 64-bit fill: pack two copies of `color` into a uint64_t and write pairs.
     * On cached RAM this halves the store count; on UC/WC it halves PCIe txns. */
    uint64_t color64 = ((uint64_t)color << 32) | (uint64_t)color;
    for (int32_t yy = y1; yy < y2; yy++) {
        uint32_t *row = buf + (uint32_t)yy * stride;
        uint32_t span = (uint32_t)(x2 - x1);
        uint64_t *d64 = (uint64_t *)&row[x1];
        uint32_t pairs = span >> 1;
        uint32_t tail  = span & 1u;
        for (uint32_t i = 0; i < pairs; i++) d64[i] = color64;
        if (tail) row[x2 - 1] = color;
    }
}

static void blend_rect(uint32_t *buf, uint32_t bw, uint32_t bh, uint32_t stride,
                       int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color) {
    int32_t x1 = x < 0 ? 0 : x;
    int32_t y1 = y < 0 ? 0 : y;
    int32_t x2 = x + w;
    int32_t y2 = y + h;
    if (x2 > (int32_t)bw) x2 = (int32_t)bw;
    if (y2 > (int32_t)bh) y2 = (int32_t)bh;
#if COMP_DAMAGE_SCISSOR
    if (x1 < g_scis_x0) x1 = g_scis_x0;
    if (y1 < g_scis_y0) y1 = g_scis_y0;
    if (x2 > g_scis_x1) x2 = g_scis_x1;
    if (y2 > g_scis_y1) y2 = g_scis_y1;
#endif
    if (x1 >= x2 || y1 >= y2) return;
    for (int32_t yy = y1; yy < y2; yy++) {
        uint32_t *row = buf + (uint32_t)yy * stride;
        for (int32_t xx = x1; xx < x2; xx++) row[xx] = blend_pixel(color, row[xx]);
    }
}

static void stroke_rect(uint32_t *buf, uint32_t bw, uint32_t bh, uint32_t stride,
                        int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color) {
    fill_rect(buf, bw, bh, stride, x, y, w, 1, color);
    fill_rect(buf, bw, bh, stride, x, y + h - 1, w, 1, color);
    fill_rect(buf, bw, bh, stride, x, y, 1, h, color);
    fill_rect(buf, bw, bh, stride, x + w - 1, y, 1, h, color);
}

/* Filled rounded rectangle: a fill_rect with the four corner pixels clipped
 * (cheap, just enough to read as "rounded" at small sizes). */
static void fill_round_rect(uint32_t *buf, uint32_t bw, uint32_t bh, uint32_t stride,
                            int32_t x, int32_t y, int32_t w, int32_t h,
                            int32_t r, uint32_t color) {
    if (r < 1) { fill_rect(buf, bw, bh, stride, x, y, w, h, color); return; }
    /* middle band (full width) */
    fill_rect(buf, bw, bh, stride, x, y + r, w, h - 2 * r, color);
    /* top + bottom bands inset by the corner radius */
    fill_rect(buf, bw, bh, stride, x + r, y, w - 2 * r, r, color);
    fill_rect(buf, bw, bh, stride, x + r, y + h - r, w - 2 * r, r, color);
    /* fill the corner quarter-discs */
    for (int32_t dy = 0; dy < r; dy++) {
        for (int32_t dx = 0; dx < r; dx++) {
            int32_t off = (r - 1 - dx) * (r - 1 - dx) + (r - 1 - dy) * (r - 1 - dy);
            if (off > (r - 1) * (r - 1)) continue;   /* outside the disc */
            /* top-left */
            fill_rect(buf, bw, bh, stride, x + dx, y + dy, 1, 1, color);
            /* top-right */
            fill_rect(buf, bw, bh, stride, x + w - 1 - dx, y + dy, 1, 1, color);
            /* bottom-left */
            fill_rect(buf, bw, bh, stride, x + dx, y + h - 1 - dy, 1, 1, color);
            /* bottom-right */
            fill_rect(buf, bw, bh, stride, x + w - 1 - dx, y + h - 1 - dy, 1, 1, color);
        }
    }
}

/* Rounded-rect TOP corners only (used for the dock: square bottom, soft top). */
static void fill_round_top_rect(uint32_t *buf, uint32_t bw, uint32_t bh, uint32_t stride,
                                int32_t x, int32_t y, int32_t w, int32_t h,
                                int32_t r, uint32_t color) {
    if (r < 1) { fill_rect(buf, bw, bh, stride, x, y, w, h, color); return; }
    fill_rect(buf, bw, bh, stride, x, y + r, w, h - r, color);   /* everything below the top corner band */
    fill_rect(buf, bw, bh, stride, x + r, y, w - 2 * r, r, color); /* top band between corners */
    for (int32_t dy = 0; dy < r; dy++) {
        for (int32_t dx = 0; dx < r; dx++) {
            int32_t off = (r - 1 - dx) * (r - 1 - dx) + (r - 1 - dy) * (r - 1 - dy);
            if (off > (r - 1) * (r - 1)) continue;
            fill_rect(buf, bw, bh, stride, x + dx, y + dy, 1, 1, color);            /* top-left */
            fill_rect(buf, bw, bh, stride, x + w - 1 - dx, y + dy, 1, 1, color);    /* top-right */
        }
    }
}

/* Is (lx,ly) -- coords RELATIVE to a rounded WxH rect with corner radius r --
 * outside the rounded shape? Used to punch the 4 corners out of a window so it
 * reads as rounded. */
static inline int round_corner_clipped(int32_t lx, int32_t ly, int32_t w, int32_t h, int32_t r) {
    if (r < 1) return 0;
    int32_t cx, cy;          /* nearest corner-disc center */
    int inx = 0, iny = 0;
    if (lx < r)            { cx = r;         inx = 1; }
    else if (lx >= w - r)  { cx = w - r - 1; inx = 1; }
    else                     cx = lx;
    if (ly < r)            { cy = r;         iny = 1; }
    else if (ly >= h - r)  { cy = h - r - 1; iny = 1; }
    else                     cy = ly;
    if (!inx || !iny) return 0;             /* only the 4 corner squares matter */
    int32_t ddx = lx - cx, ddy = ly - cy;
    return (ddx * ddx + ddy * ddy) > (r * r);
}

/*
 * Blit a client's shared-memory pixel buffer (ARGB32, row stride in pixels)
 * into the back buffer at (dx,dy), clipped to the screen and (optionally) to
 * a vertical band [clip_y0, clip_y1) so windows never overdraw the chrome.
 * Pixels are copied opaque (clients own their content).
 */
static void blit_surface_clip(uint32_t *buf, uint32_t bw, uint32_t bh, uint32_t stride,
                              const uint32_t *src, uint32_t sw, uint32_t sh, uint32_t sstride,
                              int32_t dx, int32_t dy, int32_t clip_y0, int32_t clip_y1) {
    if (!src || sw == 0 || sh == 0) return;
    /* Pre-compute the effective horizontal pixel span once (screen + scissor),
     * eliminating the per-pixel x-axis bounds + scissor checks from the inner
     * loop.  This is the blit_surface_clip fast-span optimisation. */
    int32_t hx0 = dx < 0 ? -dx : 0;                                   /* first src col visible */
    int32_t hx1 = (dx + (int32_t)sw > (int32_t)bw) ? (int32_t)bw - dx : (int32_t)sw;
#if COMP_DAMAGE_SCISSOR
    if (dx + hx0 < g_scis_x0) hx0 = g_scis_x0 - dx;
    if (dx + hx1 > g_scis_x1) hx1 = g_scis_x1 - dx;
#endif
    if (hx0 >= hx1) return;                                            /* fully clipped horizontally */
    for (uint32_t sy = 0; sy < sh; sy++) {
        int32_t py = dy + (int32_t)sy;
        if (py < 0 || py >= (int32_t)bh) continue;
        if (py < clip_y0 || py >= clip_y1) continue;   /* keep out of chrome */
#if COMP_DAMAGE_SCISSOR
        if (py < g_scis_y0 || py >= g_scis_y1) continue;
#endif
        const uint32_t *srow = src + (uint64_t)sy * sstride;
        uint32_t *drow = buf + (uint32_t)py * stride;
        for (int32_t sx = hx0; sx < hx1; sx++)
            drow[dx + sx] = srow[sx] | 0xFF000000u;   /* force opaque */
    }
}

/*
 * ANIMATION BLIT: nearest-neighbor scale a client surface and alpha-blend it.
 *
 * The source (sw x sh) is scaled by (scale_num/scale_den) ABOUT the rect's
 * center (the natural 1:1 destination is dst_x..dst_x+sw, dst_y..dst_y+sh).
 * Each emitted pixel is blended with the constant alpha a256 (0..256). Output
 * is clipped to the screen and to the chrome band [clip_y0, clip_y1).
 *
 * Fixed-point only: scale is num/den integers, alpha is a 0..256 integer. No
 * floats -> no libgcc soft-float helpers.
 */
static void blit_surface_scaled_alpha(uint32_t *buf, uint32_t bw, uint32_t bh, uint32_t stride,
                                       const uint32_t *src, uint32_t sw, uint32_t sh, uint32_t sstride,
                                       int32_t dst_x, int32_t dst_y,
                                       int32_t scale_num, int32_t scale_den, uint32_t a256,
                                       int32_t clip_y0, int32_t clip_y1) {
    if (!src || sw == 0 || sh == 0 || scale_den <= 0 || a256 == 0) return;
    if (scale_num <= 0) return;

    /* scaled destination size */
    int32_t dw = (int32_t)((uint64_t)sw * (uint32_t)scale_num / (uint32_t)scale_den);
    int32_t dh = (int32_t)((uint64_t)sh * (uint32_t)scale_num / (uint32_t)scale_den);
    if (dw <= 0 || dh <= 0) return;

    /* keep the rect centered on the natural (unscaled) center */
    int32_t cx = dst_x + (int32_t)sw / 2;
    int32_t cy = dst_y + (int32_t)sh / 2;
    int32_t ox = cx - dw / 2;
    int32_t oy = cy - dh / 2;

    for (int32_t py = 0; py < dh; py++) {
        int32_t sy = oy + py;
        if (sy < 0 || sy >= (int32_t)bh) continue;
        if (sy < clip_y0 || sy >= clip_y1) continue;
#if COMP_DAMAGE_SCISSOR
        if (sy < g_scis_y0 || sy >= g_scis_y1) continue;
#endif
        /* map destination row -> source row (nearest) */
        uint32_t srcy = (uint32_t)((uint64_t)py * (uint32_t)scale_den / (uint32_t)scale_num);
        if (srcy >= sh) srcy = sh - 1;
        const uint32_t *srow = src + (uint64_t)srcy * sstride;
        uint32_t *drow = buf + (uint32_t)sy * stride;
        for (int32_t px = 0; px < dw; px++) {
            int32_t sx = ox + px;
            if (sx < 0 || sx >= (int32_t)bw) continue;
#if COMP_DAMAGE_SCISSOR
            if (sx < g_scis_x0 || sx >= g_scis_x1) continue;
#endif
            uint32_t srcx = (uint32_t)((uint64_t)px * (uint32_t)scale_den / (uint32_t)scale_num);
            if (srcx >= sw) srcx = sw - 1;
            drow[sx] = blend_pixel_a256(srow[srcx], drow[sx], a256);
        }
    }
}

/* ====================================================================== *
 *  Theme -- Aether Dark                                                    *
 * ====================================================================== */
#define COL_DESKTOP   0xFF1C1C1Eu
#define COL_PANEL     0xFF2C2C2Eu
#define COL_HOVER     0xFF3A3A3Cu
#define COL_TEXT      0xFFFFFFFFu
#define COL_TEXT_DIM  0xFFAEAEB2u
#define COL_ACCENT    0xFF0A84FFu
#define COL_BORDER    0xFF38383Au

/* Wallpaper gradient endpoints (deep blue-navy). */
#define WALL_TOP        0xFF101826u
#define WALL_BOT        0xFF1B2A3Au

#define TITLEBAR_FOCUS  0xFF3A3A3Cu   /* focused window titlebar             */
#define TITLEBAR_UNFOC  0xFF2C2C2Eu   /* unfocused titlebar                  */
#define BORDER_FOCUS    0xFF0A84FFu   /* focused border (accent)             */
#define BORDER_UNFOC    0xFF38383Au   /* unfocused border                    */
#define CURSOR_FILL     0xFFFFFFFFu
#define CURSOR_EDGE     0xFF000000u
#define BTN_CLOSE       0xFFFF5F57u   /* close box (red)                     */
#define BTN_MIN         0xFFFFBD47u   /* minimize box (amber)                */
#define WIN_PLACEHOLDER 0xFF1C1C1Eu   /* shown if a client has no shm yet    */

/* GUI SCALE: text-bearing chrome bars DERIVE from the runtime font cell (FONT_H,
 * defined below) so the WHOLE desktop -- not just the IDE -- grows/shrinks when the
 * user zooms (Alt+wheel). These expand at use (after FONT_H is defined), and are
 * used only in functions further down, so the macro-order is fine. */
#define TITLEBAR_H  (FONT_H + 12)
#define BORDER_W    1
#define WIN_RADIUS  8                 /* rounded window outer-corner radius (matches dock/menu) */

/* chrome geometry (font-cell-derived; track the global UI zoom) */
#define PANEL_H     (FONT_H + 12)
#define DOCK_H      (FONT_H + 18)
#define LAUNCH_SZ   (FONT_H + 16)      /* launcher button                     */
#define TASK_W_MAX  (FONT_W * 12)      /* taskbar button max width (~12 chars) */
#define TASK_W_MIN  (FONT_W * 6)       /* minimum before we stop shrinking    */
#define TASK_H      (FONT_H + 12)      /* taskbar button height               */
#define TASK_GAP    8                  /* gap between taskbar buttons          */

/* Effective taskbar button width.  Recalculated each frame by
 * task_reflow() so buttons shrink to fit when many windows are open
 * instead of silently hiding the overflow. */
static int32_t g_task_w = 0;   /* 0 = not yet computed (set before first use) */
#define TASK_W g_task_w
#define CLOSE_SZ    (FONT_H - 4)       /* titlebar close box hit/visual size  */
#define MIN_SZ      (FONT_H - 4)       /* titlebar minimize box hit/visual    */

/* animation tunables (ms) */
#define ANIM_OPEN_MS    180
#define ANIM_CLOSE_MS   150
#define ANIM_MIN_MS     220
#define ANIM_RESTORE_MS 220

/* M8: simple per-window fade-in duration (ms).  Separate from PH_OPENING
 * so it works for any window regardless of the scale-anim state. */
#define FADE_IN_MS      150

/* animation phases */
#define PH_NONE       0
#define PH_OPENING    1
#define PH_CLOSING    2
#define PH_MINIMIZING 3
#define PH_RESTORING  4
#define PH_SNAPPING   5          /* M6: geometry tween (snap or un-snap)        */

/* M6: snap geometry tween duration (ms) */
#define ANIM_SNAP_MS  170

/* ---------------------------------------------------------------------- *
 *  M6: keyboard scancodes (mirror of kernel/include/input.h)              *
 * ---------------------------------------------------------------------- */
#define KEY_TAB       15
#define KEY_Q         16
#define KEY_D         32      /* Alt+D: show desktop (minimize all)    */
#define KEY_S         31      /* PERF: Alt+S toggles the stats overlay */
#define KEY_K         37
#define KEY_M         50
#define KEY_LEFTALT   56
#define KEY_F4        62
#define KEY_ENTER     28      /* Alt+Enter toggles maximize (fullscreen-to-work-area) */

/* M6: snap target kinds (also used as the "currently snapped" tag) */
#define SNAP_NONE     0
#define SNAP_LEFT     1
#define SNAP_RIGHT    2
#define SNAP_MAX      3

/* M6: how close (px) the cursor must get to a screen edge to arm a snap. */
#define SNAP_EDGE_PX  12

/* ======================================================================
 * M8: RIGHT-SIDE MACOS-STYLE VERTICAL DOCK
 * ====================================================================== */
#define RDOCK_W          44    /* width of the right dock strip (px) — T410 fit */
#define RDOCK_ICON_BASE  32    /* base (non-magnified) icon tile size (px)    */
#define RDOCK_PAD         3    /* gap between icon tiles (px)                 */
#define RDOCK_CORNER      8    /* rounded corner radius for icon tiles        */
#define RDOCK_MARGIN_TOP 28    /* top margin inside the dock strip            */
/* DESKTOP-REVAMP-0: icon 36->32 / pad 4->3 / margin 40->28 tightens the strip so
 * 17 app icons + 2 folders (19 slots, stride 35) fit above the taskbar on the
 * 800px-tall screen -- room for the new Claude + Anthropic dock icons. */

/* Magnification parameters (fixed-point, all in Q8 / integers):
 *   scale = 1 + (MAX_EXTRA/256) * max(0, 1 - dy/INFLUENCE)
 *   MAX_EXTRA/256 ~ 0.9  => max scale ~ 1.9
 *   INFLUENCE ~ 110 px  */
#define RDOCK_MAG_MAX_EXTRA  230   /* (MAX-1)*256 => (1.9-1)*256 = 230        */
#define RDOCK_MAG_INFLUENCE  110   /* pixel radius of magnification field     */
#define RDOCK_SMOOTH_SHIFT     3   /* smooth toward target: >>3 per frame     */

/* Bounce animation on launch click: horizontal (leftward) swing */
#define RDOCK_BOUNCE_MS   420
#define RDOCK_BOUNCE_AMP   18   /* max pixel displacement (left)             */

/* Folder popover (legacy rect popover replaced by the rainbow fan-out below;
 * the ANIM_MS timing is reused to drive the fan open/close animation). */
#define RDOCK_POPOVER_W   160
#define RDOCK_POPOVER_H   200
#define RDOCK_POPOVER_ANIM_MS 180

/* Folder rainbow fan-out (open state). Member app icons sweep OUT along a
 * ~160-degree semicircle into the workspace, floating and twinkling. */
#define RDOCK_FAN_ARC_DEG   160   /* total angular spread of the rainbow      */
#define RDOCK_FAN_RADIUS    140   /* arc radius at full open (px)             */
#define RDOCK_FAN_TILE       44   /* fanned-out member icon tile size (px)    */
#define RDOCK_FAN_BOB_AMP     4   /* vertical floating bob amplitude (px)     */
#define RDOCK_FAN_SPARKLES    8   /* sparkle dots drawn around a hovered icon */

/* Number of app entries and folders */
#define RDOCK_NICONS  20   /* DESKTOP-REVAMP-0: +Claude chat +Anthropic panel +Cockpit +Sound +DeadZone */
#define RDOCK_NFOLDERS 2

/* App descriptor */
typedef struct {
    const char *label;   /* 2-char label shown on tile           */
    const char *path;    /* relative spawn path                  */
    uint32_t    color;   /* tile background color (ARGB)         */
} rdock_app_t;

/* Folder descriptor */
typedef struct {
    const char     *label;
    uint32_t        color;
    int             members[4];  /* indices into rdock_apps[], -1=unused */
    int             nmembers;
} rdock_folder_t;

/* ---- App table ---- */
/* Curated dock: the core apps as loose icons + a Games and a Tools folder for
 * the rest. The long-tail demo/game apps (notes, paint, chess, asteroids,
 * sudoku, controlcenter, photos, bubbletd, zombietd, dateapp) are still built
 * and launchable from the file manager / terminal -- they're just no longer
 * cluttering the dock strip. Folder members[] index into THIS array. */
static const rdock_app_t rdock_apps[RDOCK_NICONS] = {
    { "Te", "sbin/terminal",    0xFF1E6FB5u },  /* 0  Terminal   */
    { "Fi", "sbin/filemanager", 0xFF2E8B57u },  /* 1  Files      */
    { "Id", "sbin/ide",         0xFF2D6A8Bu },  /* 2  IDE        */
    { "Wb", "sbin/browser2",    0xFF1565C0u },  /* 3  Browser    */
    { "Nm", "sbin/netman",      0xFF00897Bu },  /* 4  NetManager */
    { "St", "sbin/settings",    0xFF444466u },  /* 5  Settings   */
    { "Ca", "sbin/calculator",  0xFF555577u },  /* 6  Calculator */
    { "Cl", "sbin/clock",       0xFF336699u },  /* 7  Clock      */
    { "Ed", "sbin/editor",      0xFF8B6914u },  /* 8  Editor     */
    { "Sn", "sbin/snake",       0xFF2D6A2Du },  /* 9  Snake      */
    { "Tt", "sbin/tetris",      0xFF6B2D8Bu },  /* 10 Tetris     */
    { "20", "sbin/game2048",    0xFF8B4513u },  /* 11 2048       */
    { "Pm", "sbin/pacman",      0xFFFFD60Au },  /* 12 Pac-Man    */
    { "C+", "sbin/clockapp",    0xFF0067C0u },  /* 13 Clock+     */
    { "Db", "sbin/derby",       0xFFD35400u },  /* 14 Derby 3D   */
    { "Cc", "sbin/claudechat",  0xFFD97757u },  /* 15 Claude     */
    { "An", "sbin/anthropic",   0xFFB8865Au },  /* 16 Anthropic  */
    { "Ck", "sbin/cockpit",     0xFF6B5B95u },  /* 17 Cockpit    -- the agent console */
    { "Au", "sbin/soundman",    0xFF1DB954u },  /* 18 Sound      -- HDA volume/mute/test */
    { "Dz", "sbin/deadzone",    0xFF8B0000u },  /* 19 DeadZone   -- FPS zombie survival  */
};

/* ---- Folder table (Games + Tools); members[] index into rdock_apps[] ---- */
static const rdock_folder_t rdock_folders[RDOCK_NFOLDERS] = {
    { "Gm", 0xFF3A3A5Cu, { 9, 10, 11, 12 }, 4 },   /* Games: snake/tetris/2048/pacman */
    { "Tl", 0xFF3A5C3Au, { 6, 7, 8, 13 }, 4 },     /* Tools: calc/clock/editor/clock+ */
};

/* ---- Per-icon animation state ---- */
typedef struct {
    int32_t scale_q8;       /* current magnified scale (Q8, 256=1.0)         */
    int32_t scale_target;   /* target scale this frame (Q8)                  */
    long    bounce_start;   /* SYS_GET_TICKS_MS when bounce started, 0=off   */
    int     bounce_active;
} rdock_icon_state_t;

/* ---- Per-folder state ---- */
typedef struct {
    int     open;           /* 1 = popover expanded                           */
    int32_t anim_t;         /* Q8 progress 0..256 for open/close anim         */
    long    anim_start;
    int     anim_closing;   /* 1 = animating closed                           */
} rdock_folder_state_t;

/* TOTAL items in the dock strip = apps + folders.
 * Folders are inserted after their last member slot conceptually, but we
 * just append them at the bottom of the icon list for simplicity. */
#define RDOCK_TOTAL  (RDOCK_NICONS + RDOCK_NFOLDERS)

/* ====================== Popup menus (start + context) ================== *
 *  Start menu opens above the launcher; right-click opens a context menu  *
 *  on the desktop. Both share one popup with a header + clickable rows.   *
 * ---------------------------------------------------------------------- */
#define MENU_W            190
#define MENU_ROW_H        26
#define MENU_HDR_H        28
#define MENU_MAX          16
#define MACT_NONE          0   /* row spawns g_menu_path */
#define MACT_MINIMIZE_ALL  1
#define MACT_CLOSE_ALL     2
#define MACT_ABOUT         3
#define MACT_NEW_FOLDER    4   /* create /Desktop/NewFolder[N] then rescan */
#define MACT_WIN_MINIMIZE  5   /* minimize g_menu_target_slot (title-bar path) */
#define MACT_WIN_MAXIMIZE  6   /* toggle SNAP_MAX on g_menu_target_slot         */
#define MACT_WIN_CLOSE     7   /* close g_menu_target_slot                      */
#define MACT_REFRESH       8   /* force a full desktop repaint (best-effort)    */
#define MACT_DISPLAY_SETTINGS 9 /* spawn sbin/settings                          */
#define MACT_WIN_SNAP_LEFT  10  /* snap g_menu_target_slot to the left half      */
#define MACT_WIN_SNAP_RIGHT 11  /* snap g_menu_target_slot to the right half     */
static int         g_menu_open   = 0;
static int         g_about_open  = 0;   /* modal About dialog (centered panel) */
static int         g_menu_is_ctx = 0;   /* 0=start menu, 1=context menu */
static int32_t     g_menu_x = 0, g_menu_y = 0;
static int         g_menu_n = 0;
static int         g_menu_hover = -1;
static int         g_menu_target_slot = -1;  /* window the ctx menu acts on, -1=desktop */
static const char *g_menu_label[MENU_MAX];
static const char *g_menu_path[MENU_MAX];
static int         g_menu_action[MENU_MAX];
static int32_t menu_height(void) { return MENU_HDR_H + g_menu_n * MENU_ROW_H + 6; }

/* ---- Drag-and-drop dock (DOCK-DND-0): the const rdock_apps[]/rdock_folders[]
 * are the SEED; the live dock is mutable so the user can drag icons, combine
 * them into new boxes, and select them. Folders become runtime-mutable and the
 * per-icon STATE arrays are oversized to a capacity so new boxes fit. ---- */
#define DOCK_FOLDERS_MAX  8                          /* incl. the 2 seed folders */
#define DOCK_MEMBERS_MAX  12                         /* apps a box can hold      */
#define RDOCK_CAP  (RDOCK_NICONS + DOCK_FOLDERS_MAX) /* per-icon state capacity  */

/* Mutable folder (seeded from the const rdock_folder_t at init). A member is
 * either a catalog app (members[m] = index into rdock_apps[]) or, for an app
 * dragged in from the Start menu, a free path (members[m] = -1, mpath[m] set). */
typedef struct {
    char     label[4];
    uint32_t color;
    int      members[DOCK_MEMBERS_MAX];          /* catalog index, or -1        */
    char     mpath[DOCK_MEMBERS_MAX][64];        /* spawn path for members[m]==-1 */
    int      nmembers;
} dock_folder_t;

static dock_folder_t g_folders[DOCK_FOLDERS_MAX];
static int           g_nfolders = 0;                 /* live folder count        */
static int           g_app_hidden[RDOCK_NICONS];     /* 1 = filed inside a box   */

static rdock_icon_state_t   g_rdock_icons[RDOCK_CAP];
static rdock_folder_state_t g_rdock_folders[DOCK_FOLDERS_MAX];
static int32_t              g_rdock_hovered = -1;  /* -1 or index 0..TOTAL-1 */

/* Which folder popover is open (-1 = none). Only one at a time. */
static int32_t g_rdock_open_folder = -1;

/* Live iteration bound over the idx space: apps 0..RDOCK_NICONS-1, then folders
 * RDOCK_NICONS..RDOCK_NICONS+g_nfolders-1. Hidden apps are skipped by callers. */
static int rdock_idx_end(void) { return RDOCK_NICONS + g_nfolders; }

/* ---- DOCK-DND-0 drag + selection state (all inert until a drag begins) ---- */
static int     g_dock_drag      = -1;   /* idx currently being dragged, or -1   */
static int     g_dock_press_idx = -1;   /* idx pressed (pending click-or-drag)  */
static int32_t g_dock_press_x = 0, g_dock_press_y = 0;  /* press position       */
static int32_t g_dock_drag_x  = 0, g_dock_drag_y  = 0;  /* live ghost CENTER     */
static int     g_dock_drop_tgt   = -1;  /* idx the ghost hovers as a drop target */
static long    g_dock_drop_anim  = 0;   /* SYS_GET_TICKS_MS of the last drop pop */
static int     g_dock_selected[RDOCK_CAP];   /* 1 = selected (highlighted)       */

/* DOCK-DND-1: Start-menu -> dock drag handoff over a well-known SHM page. */
#include "../include/dockdnd.h"
/* SYNTHINPUT-0: agent -> compositor synthetic mouse/keyboard injection (mirrors the
 * dockdnd SHM seam). The compositor creates+owns the page; agent tools attach
 * lookup-only and enqueue events drained each frame by pump_synth_input(). */
#include "../include/synthinput.h"
static volatile synthinput_shm_t *g_synth = (volatile synthinput_shm_t *)0;
#ifndef SYS_SHMGET
#define SYS_SHMGET  18
#endif
static volatile dockdnd_shm_t *g_dnd = (volatile dockdnd_shm_t *)0;
static int g_pinned_folder = -1;   /* idx of the auto-created "Pinned" box, or -1 */
static int dock_path_ok(const char *p);   /* validate an SHM-sourced spawn path */

/* The spawn path of folder [fidx] member [m]: a catalog app, or a Start-menu
 * app dragged in (free path). Returns NULL if the slot is empty. */
static const char *folder_member_path(int fidx, int m) {
    dock_folder_t *f = &g_folders[fidx];
    if (m < 0 || m >= f->nmembers) return (const char *)0;
    int mi = f->members[m];
    if (mi >= 0 && mi < RDOCK_NICONS) return rdock_apps[mi].path;
    if (f->mpath[m][0]) return f->mpath[m];
    return (const char *)0;
}

/* ====================================================================== *
 *  M8: DESKTOP ICONS  ("/Desktop" contents as labeled wallpaper icons)   *
 *                                                                         *
 *  The IDE compiles programs to "/Desktop". We enumerate that directory  *
 *  via SYS_OPENDIR/READDIR/CLOSEDIR (same pattern the file/folder code    *
 *  uses), lay the entries out in a grid at the top-left of the desktop,   *
 *  and CLAMP every tile fully on-screen (icon_for_app / icon_* write      *
 *  UNCLIPPED, so a partially off-screen tile would scribble past the back *
 *  buffer). Double-click runs them; right-click empty space can make new  *
 *  folders.                                                               *
 * ---------------------------------------------------------------------- */
#define DESK_MAX_ICONS   64    /* cap entries we track / draw                 */
#define DESK_NAME_DISP   32    /* truncated name length we store for label    */
#define DESK_TILE        56    /* icon art size (px)                          */
#define DESK_CELL_W      96    /* grid cell width  (tile + label margin)      */
#define DESK_CELL_H      88    /* grid cell height (tile + label + gap)       */
#define DESK_ORIGIN_X    16    /* grid left margin                            */
#define DESK_LABEL_GAP    4    /* gap between tile bottom and label           */
#define DESK_DBLCLICK_MS 400   /* two clicks within this window = double      */
#define DESK_RESCAN_FRAMES 120 /* periodic rescan cadence (frames ~ 2s)       */
#define DESK_PATH        256   /* full VFS path stored per icon (identity)    */

/* Icon kind drives the double-click action. The FULL path is identity; the
 * (truncated) name is only the grid label. */
typedef enum { DI_FOLDER = 0, DI_APP, DI_FILE, DI_PROJECT } desk_kind_t;

typedef struct {
    char        name[DESK_NAME_DISP + 1];  /* truncated display label ONLY         */
    char        path[DESK_PATH];           /* full VFS path = identity (spawn/open) */
    desk_kind_t kind;
    int         is_dir;                    /* 1 = directory (quick check)          */
} desk_icon_t;

static desk_icon_t g_desk_icons[DESK_MAX_ICONS];
static int         g_desk_count = 0;
/* double-click tracking: last clicked icon index + timestamp */
static int  g_desk_last_idx  = -1;
static long g_desk_last_ms   = 0;
static int  g_desk_hover     = -1;   /* icon idx the cursor is over (-1 = none).
                                      * A change forces a full recomposite so the
                                      * hover glow (wallpaper layer, outside any
                                      * window commit rect) never ghosts. */

/* Titlebar double-click tracking: double-click toggles maximize/restore. */
#define TB_DBLCLICK_MS 400
static int  g_tb_last_slot = -1;
static long g_tb_last_ms   = 0;

/* M6: snap-armed-during-drag preview. g_snap_armed is SNAP_* (the zone the
 * window will land in if released now), or SNAP_NONE if the cursor is not at an
 * edge. The compositor draws a translucent preview for whatever is armed. */
static int32_t g_snap_armed = SNAP_NONE;

static uint32_t lerp_color(uint32_t a, uint32_t b, uint32_t num, uint32_t den) {
    if (den == 0) den = 1;
    int32_t ar = (int32_t)((a >> 16) & 0xFF), ag = (int32_t)((a >> 8) & 0xFF), ab = (int32_t)(a & 0xFF);
    int32_t br = (int32_t)((b >> 16) & 0xFF), bg = (int32_t)((b >> 8) & 0xFF), bb = (int32_t)(b & 0xFF);
    /* Use signed deltas: when a channel of `a` exceeds `b`, (br-ar) is negative.
     * Computed in unsigned it wraps to ~4 billion and the result explodes. */
    int32_t r  = ar + (br - ar) * (int32_t)num / (int32_t)den;
    int32_t g  = ag + (bg - ag) * (int32_t)num / (int32_t)den;
    int32_t bl = ab + (bb - ab) * (int32_t)num / (int32_t)den;
    return 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)bl;
}

/* ---------------------------------------------------------------------- *
 *  Easing -- fixed-point: input + output are Q8 in [0,256].               *
 * ---------------------------------------------------------------------- */
static inline int32_t clamp256(int32_t t) { return t < 0 ? 0 : (t > 256 ? 256 : t); }

/* ease_out_cubic(t) = 1 - (1-t)^3   (Q8 in, Q8 out) */
static int32_t ease_out_cubic(int32_t t) {
    t = clamp256(t);
    int32_t u = 256 - t;                       /* (1-t) in Q8 */
    int32_t u3 = (int32_t)(((int64_t)u * u * u) >> 16);  /* u^3 / 256^2 -> Q8 */
    return clamp256(256 - u3);
}

/* ease_in_cubic(t) = t^3   (Q8 in, Q8 out) */
static int32_t ease_in_cubic(int32_t t) {
    t = clamp256(t);
    return clamp256((int32_t)(((int64_t)t * t * t) >> 16));
}

/* ease_in_out_cubic: <0.5 -> 4t^3, else 1 - (-2t+2)^3 / 2  (Q8). */
static int32_t ease_in_out_cubic(int32_t t) {
    t = clamp256(t);
    if (t < 128) {
        /* 4*t^3 = (t^3) << 2 ; t^3 in Q8 = (t*t*t)>>16 */
        int32_t t3 = (int32_t)(((int64_t)t * t * t) >> 16);
        return clamp256(t3 << 2);
    } else {
        int32_t u = 2 * (256 - t);             /* (-2t+2) in Q8 == 2*(1-t) */
        int32_t u3 = (int32_t)(((int64_t)u * u * u) >> 16);
        return clamp256(256 - (u3 >> 1));
    }
}

/* ---------------------------------------------------------------------- *
 *  Fixed-point sine, NO FLOAT / NO LIBM.                                  *
 *  sin_q(deg) returns sin(deg) in Q8 (signed, -256..256). Uses a small    *
 *  quarter-wave table (0..90 deg, step 5 deg) with linear interpolation   *
 *  and the usual quadrant symmetry. Degrees are wrapped to [0,360).       *
 *  Reused by the folder fan-out (arc layout), icon floating bob, and the  *
 *  sparkle particle orbits.                                               *
 * ---------------------------------------------------------------------- */
static const int32_t SINQ_TBL[19] = {  /* sin(0..90 by 5 deg) * 256 */
      0,  22,  44,  66,  88, 109, 128, 147, 165, 181,
    196, 209, 221, 231, 240, 247, 252, 255, 256
};
static int32_t sin_q(int32_t deg) {
    /* wrap into [0,360) without %, robust for large/negative inputs */
    deg %= 360;
    if (deg < 0) deg += 360;
    int sign = 1;
    if (deg >= 180) { deg -= 180; sign = -1; }   /* lower half is negated */
    if (deg > 90) deg = 180 - deg;               /* fold 90..180 onto 0..90 */
    /* table lookup with linear interp; index by 5-degree buckets */
    int32_t i = deg / 5;
    int32_t frac = deg - i * 5;                  /* 0..4 */
    int32_t a = SINQ_TBL[i];
    int32_t b = SINQ_TBL[i + 1];
    int32_t v = a + (b - a) * frac / 5;
    return sign * v;
}
/* cos via phase shift */
static int32_t cos_q(int32_t deg) { return sin_q(deg + 90); }

/* font (linked from userspace/lib/font/bitfont.c) */
extern int font_draw_string(unsigned int *fbuf, int fstride, int fbw, int fbh,
                            int tx, int ty, const char *s, unsigned int color);
/* Scalable text renderer (userspace/lib/font2) -- bounds-safe fractional cell. */
extern void font2_draw_cell_clip(unsigned int *px, int stride, int maxw, int maxh,
                                 int clip_x0, int clip_x1, int x, int y,
                                 const char *str, int cell_w, int cell_h,
                                 unsigned int argb);

/* RUNTIME GLOBAL UI SCALE. The whole desktop -- chrome AND every text draw routed
 * through cz_text() -- renders at a glyph cell of (8,16) * g_ui_pct/100. Alt+wheel
 * zooms it live. FONT_W/FONT_H alias the runtime cell so all centering/truncation
 * math tracks it; the chrome bar heights derive from FONT_H so they grow with it. */
static int g_ui_pct = 130;     /* 50..200; overridden in _start per resolution    */
static int g_cell_w = 10;
static int g_cell_h = 20;
#define FONT_W g_cell_w
#define FONT_H g_cell_h
static void cz_set_scale(int pct) {
    if (pct < 100) pct = 100;   /* UI-CRISP-0: integer-only (100/200); no fractional blur */
    if (pct > 200) pct = 200;
    g_ui_pct = pct;
    g_cell_w = 8  * pct / 100;
    g_cell_h = 16 * pct / 100;
}
/* Drop-in scaled replacement for font_draw_string() (identical signature), so
 * every chrome text draw routes through the scalable renderer. */
static void cz_text(unsigned int *buf, int stride, int bw, int bh,
                    int x, int y, const char *s, unsigned int color) {
#if COMP_DAMAGE_SCISSOR
    int cx0 = g_scis_x0 > 0  ? g_scis_x0 : 0;
    int cx1 = g_scis_x1 < bw ? g_scis_x1 : bw;
    font2_draw_cell_clip(buf, stride, bw, bh, cx0, cx1, x, y, s, g_cell_w, g_cell_h, color);
#else
    font2_draw_cell_clip(buf, stride, bw, bh, 0, bw, x, y, s, g_cell_w, g_cell_h, color);
#endif
}

/* ---- cursor arrow bitmap (from compositor_m2/m3.c) ----
 * '#' = black edge, '.' = white fill.
 * draw_cursor() auto-generates a 1px white outline around the black edge so
 * the cursor stays visible against ALL backgrounds (T410 TN panel). */
#define CUR_W 12
#define CUR_H 19
static const char *CURSOR[CUR_H] = {
    "#           ", "##          ", "#.#         ", "#..#        ",
    "#...#       ", "#....#      ", "#.....#     ", "#......#    ",
    "#.......#   ", "#........#  ", "#.........# ", "#......#####",
    "#...#..#    ", "#..# #..#   ", "#.#  #..#   ", "##    #..#  ",
    "#     #..#  ", "       #..# ", "       ####  ",
};

/* Is bitmap position (r,c) a cursor pixel? */
static inline int cur_opaque(int32_t r, int32_t c) {
    if (r < 0 || r >= CUR_H || c < 0 || c >= CUR_W) return 0;
    char m = CURSOR[r][c];
    return (m == '#' || m == '.');
}

static void draw_cursor(uint32_t *buf, uint32_t bw, uint32_t bh, uint32_t stride,
                        int32_t cx, int32_t cy) {
    /* 1px white outline: the drawn footprint extends 1px beyond the bitmap in
     * every direction.  For each pixel in the extended rect, if it is NOT a
     * bitmap pixel but is 4-adjacent to one, it gets the white outline color.
     * Actual bitmap pixels overwrite: '#'->black edge, '.'->white fill.
     * Uses the same UC-FB staging-line optimisation as the original. */
    int32_t x0 = cx - 1, y0 = cy - 1;
    int32_t ew = CUR_W + 2, eh = CUR_H + 2;
    for (int32_t r = 0; r < eh; r++) {
        int32_t py = y0 + r;
        if (py < 0 || py >= (int32_t)bh) continue;
        int32_t vis_x0 = x0 < 0 ? 0 : x0;
        int32_t vis_x1 = x0 + ew;
        if (vis_x1 > (int32_t)bw) vis_x1 = (int32_t)bw;
        if (vis_x0 >= vis_x1) continue;
        uint32_t span = (uint32_t)(vis_x1 - vis_x0);
        uint32_t line[CUR_W + 2];
        uint32_t *fb_row = buf + (uint32_t)py * stride + (uint32_t)vis_x0;
        for (uint32_t i = 0; i < span; i++) line[i] = fb_row[i];
        int any = 0;
        for (int32_t c = 0; c < ew; c++) {
            int32_t px = x0 + c;
            if (px < vis_x0 || px >= vis_x1) continue;
            int32_t br = r - 1, bc = c - 1;   /* bitmap coords */
            if (cur_opaque(br, bc)) {
                char m = CURSOR[br][bc];
                line[px - vis_x0] = (m == '#') ? CURSOR_EDGE : CURSOR_FILL;
                any = 1;
            } else if (cur_opaque(br-1,bc) || cur_opaque(br+1,bc) ||
                       cur_opaque(br,bc-1) || cur_opaque(br,bc+1)) {
                line[px - vis_x0] = CURSOR_FILL;
                any = 1;
            }
        }
        if (!any) continue;
        uint32_t pairs = span >> 1;
        uint32_t tail  = span & 1u;
        uint64_t *d64 = (uint64_t *)fb_row;
        uint64_t *s64 = (uint64_t *)line;
        for (uint32_t i = 0; i < pairs; i++) d64[i] = s64[i];
        if (tail) fb_row[span - 1] = line[span - 1];
    }
}

/* ====================================================================== *
 *  Window registry                                                        *
 * ====================================================================== */
#define MAX_WINDOWS 16          /* >= 8 concurrent windows required        */

typedef struct {
    int       used;
    int32_t   win_id;           /* server-assigned id (>0)                 */
    int32_t   client_pid;       /* owner pid (for reply queue)             */
    int32_t   reply_qid;        /* cached WL_REPLY_KEY(pid) queue id, or -1 */
    int32_t   shm_id;           /* client's shm segment id                 */
    uint32_t *pixels;           /* shmat() base of client's pixel buffer   */
    uint64_t  shm_vaddr;        /* attach addr (for shmdt)                 */
    uint32_t  w, h, stride;     /* surface dims; stride in PIXELS          */
    /* IMMUTABLE client buffer extent, captured at create time and NEVER
     * overwritten by snap/maximize (which only rewrite w/h to the maximized
     * drawable rect). The client's SHM is buf_w*buf_h*4 bytes, so any blit
     * MUST clamp its SOURCE read to (buf_w,buf_h) or it walks past the mapped
     * segment and page-faults the compositor. (stride stays == buf_w, so
     * buf_w is redundant with stride but kept explicit for clarity.)        */
    uint32_t  buf_w, buf_h;     /* real SHM pixel extent (clamp source read) */
    int32_t   x, y;             /* placement of window FRAME (titlebar top)*/
    char      title[WL_TITLE_MAX];
    int       dirty;            /* set on commit (informational)           */

    /* ---- M5 animation state ---- */
    int32_t   phase;            /* PH_NONE / OPENING / CLOSING / MINIMIZING / RESTORING / SNAPPING */
    long      anim_start_ms;    /* SYS_GET_TICKS_MS when the phase began    */
    int32_t   anim_dur_ms;      /* phase duration in ms                     */
    int       minimized;        /* sticky: window is parked in the taskbar  */
    int32_t   tb_idx;           /* taskbar slot index captured at minimize  */

    /* ---- M6 window-manager state ---- */
    int32_t   snap_state;       /* SNAP_NONE / LEFT / RIGHT / MAX (current)  */
    int32_t   saved_x, saved_y; /* pre-snap geometry to restore to           */
    uint32_t  saved_w, saved_h; /* pre-snap surface size                     */
    /* PH_SNAPPING geometry tween: animate (from_*) -> (to_*) over the phase. */
    int32_t   from_x, from_y;
    uint32_t  from_w, from_h;
    int32_t   to_x, to_y;
    uint32_t  to_w, to_h;

    /* ---- M8 cheap per-window FADE-IN (compositor-internal) ---- *
     * fade_alpha ramps 0..255 over ~150ms after creation. When it   *
     * reaches 255 it stays there; the fast opaque path is used then.*
     * This is independent of (and additive to) the PH_OPENING       *
     * scale animation so windows smoothly appear from both effects. */
    uint32_t  fade_alpha;       /* 0..255; 255 = fully opaque, use fast path */
    long      fade_start_ms;    /* SYS_GET_TICKS_MS when fade began          */
} window_t;

static window_t g_windows[MAX_WINDOWS];
static int32_t  g_next_win_id = 1;

/* z-order: index list, back (0) to front (count-1). focus = topmost/front. */
static int32_t g_zorder[MAX_WINDOWS];
static int32_t g_zcount = 0;

/* M6: most-recently-used focus ring (front = most recent). Alt+Tab cycles it.
 * Kept distinct from z-order so that Alt-Tabbing through windows during a hold
 * doesn't reshuffle the MRU order until Alt is released. */
static int32_t g_mru[MAX_WINDOWS];
static int32_t g_mru_count = 0;

/* monotonic spawn counter, for staggering new window placement */
static int32_t g_spawn_seq = 0;

static int find_free_slot(void) {
    for (int i = 0; i < MAX_WINDOWS; i++) if (!g_windows[i].used) return i;
    return -1;
}

static int slot_by_win_id(int32_t win_id) {
    for (int i = 0; i < MAX_WINDOWS; i++)
        if (g_windows[i].used && g_windows[i].win_id == win_id) return i;
    return -1;
}

static void z_push_front(int slot) {
    int32_t w = 0;
    for (int32_t i = 0; i < g_zcount; i++)
        if (g_zorder[i] != slot) g_zorder[w++] = g_zorder[i];
    g_zcount = w;
    g_zorder[g_zcount++] = (int32_t)slot;
    mark_dirty();    /* PERF: raising/focusing reorders the window stack = repaint */
}

static void z_remove(int slot) {
    int32_t w = 0;
    for (int32_t i = 0; i < g_zcount; i++)
        if (g_zorder[i] != slot) g_zorder[w++] = g_zorder[i];
    g_zcount = w;
    mark_dirty();    /* PERF: removing a window from the stack = repaint */
}

/* M6: MRU ring helpers (front = index 0 = most-recently focused). */
static void mru_promote(int slot) {
    int32_t w = 0;
    for (int32_t i = 0; i < g_mru_count; i++)
        if (g_mru[i] != slot) g_mru[w++] = g_mru[i];
    g_mru_count = w;
    /* Clamp before the shift: if `slot` was not already in a full ring, the
     * shift's first write (g_mru[g_mru_count]) would land at g_mru[MAX_WINDOWS]
     * — one past the array. Drop the least-recently-used entry to make room. */
    if (g_mru_count >= MAX_WINDOWS) g_mru_count = MAX_WINDOWS - 1;
    /* shift down + insert at front */
    for (int32_t i = g_mru_count; i > 0; i--) g_mru[i] = g_mru[i - 1];
    g_mru[0] = (int32_t)slot;
    g_mru_count++;
}

static void mru_remove(int slot) {
    int32_t w = 0;
    for (int32_t i = 0; i < g_mru_count; i++)
        if (g_mru[i] != slot) g_mru[w++] = g_mru[i];
    g_mru_count = w;
}

/* Topmost focusable window slot (skips minimized/closing), or -1. Used for the
 * panel title and input forwarding so a parked/closing window never "owns" the
 * keyboard. */
static int focused_slot(void) {
    for (int32_t i = g_zcount - 1; i >= 0; i--) {
        int s = (int)g_zorder[i];
        if (s < 0 || s >= MAX_WINDOWS || !g_windows[s].used) continue;
        if (g_windows[s].minimized) continue;
        if (g_windows[s].phase == PH_CLOSING || g_windows[s].phase == PH_MINIMIZING) continue;
        return s;
    }
    return -1;
}

/* Resolve (and cache) a client's reply queue. */
static int32_t client_reply_qid(window_t *win) {
    if (win->reply_qid >= 0) return win->reply_qid;
    long qid = sc6(SYS_MSGGET, (long)WL_REPLY_KEY(win->client_pid),
                   (long)(IPC_CREAT | 0666), 0, 0, 0, 0);
    if (qid >= 0) win->reply_qid = (int32_t)qid;
    return (int32_t)qid;
}

/* Ask a window's client to resize its surface to w x h (WL_EVT_CONFIGURE). The
 * client reallocates its shm buffer and replies WL_REQ_RESIZE; until then the
 * blit source-clamps to the old buf_w/buf_h keep rendering safe. Used on
 * maximize/restore/snap so the frame can fill crisply instead of letterboxing. */
static void send_configure(window_t *win, uint32_t w, uint32_t h) {
    if (!win) return;
    int32_t qid = client_reply_qid(win);
    if (qid < 0) return;
    wl_evt_configure_t ev;
    ev.mtype  = WL_EVT_CONFIGURE;
    ev.win_id = win->win_id;
    ev.w = w;
    ev.h = h;
    sc6(SYS_MSGSND, qid, (long)&ev, (long)(sizeof(ev) - sizeof(int64_t)), 0, 0, 0);
}

/* ====================================================================== *
 *  Dock / taskbar layout (forward decls -- the animation engine needs the *
 *  taskbar button geometry to compute the minimize/restore target rect).  *
 * ====================================================================== */
static uint32_t g_fb_w = 0, g_fb_h = 0;   /* cached for placement clamping */

static int32_t dock_top(uint32_t h)        { return (int32_t)h - DOCK_H; }
static int32_t launcher_x(void)            { return 8; }
static int32_t launcher_y(uint32_t h)      { return dock_top(h) + (DOCK_H - LAUNCH_SZ) / 2; }
static int32_t taskbtn_x(int idx)          { return launcher_x() + LAUNCH_SZ + 12 + idx * (TASK_W + TASK_GAP); }
static int32_t taskbtn_y(uint32_t h)       { return dock_top(h) + (DOCK_H - TASK_H) / 2; }

/* Recompute g_task_w so all open windows fit in the bottom dock.  Called
 * once per frame before any taskbar layout / hit-test.  Shrinks buttons
 * proportionally instead of silently hiding the overflow; clamps to
 * TASK_W_MIN so labels stay at least partially readable. */
static void task_reflow(uint32_t scr_w) {
    int nwin = 0;
    for (int s = 0; s < MAX_WINDOWS; s++)
        if (g_windows[s].used) nwin++;
    if (nwin < 1) nwin = 1;
    int32_t avail = (int32_t)scr_w - (launcher_x() + LAUNCH_SZ + 12) - 8;
    int32_t btn   = (avail / nwin) - TASK_GAP;
    if (btn > TASK_W_MAX) btn = TASK_W_MAX;
    if (btn < TASK_W_MIN) btn = TASK_W_MIN;
    g_task_w = btn;
}

/* ---------------------------------------------------------------------- *
 *  M6: work area (the chrome-free region between panel and dock) + the     *
 *  snap target rectangles. All of these are in FRAME coordinates: x/y is   *
 *  the top-left of the titlebar; w/h is the CLIENT surface size (the frame  *
 *  adds TITLEBAR_H + borders), matching window_t and render_window_static.  *
 * ---------------------------------------------------------------------- */
#define WORK_MARGIN  4           /* small gap so snapped windows aren't flush */

static int32_t work_x0(void)        { return WORK_MARGIN; }
static int32_t work_y0(void)        { return PANEL_H + WORK_MARGIN; }
/* M8: subtract right dock width so snapping and work area respect the dock */
static int32_t work_x1(void)        { return (int32_t)g_fb_w - RDOCK_W - WORK_MARGIN; }
static int32_t work_y1(void)        { return (int32_t)g_fb_h - DOCK_H - WORK_MARGIN; }
static int32_t work_w(void)         { return work_x1() - work_x0(); }
/* full frame height available (titlebar + client + 2 borders) */
static int32_t work_frame_h(void)   { return work_y1() - work_y0(); }
/* client height for a frame that fills the work area vertically */
static int32_t work_client_h(void)  { int32_t v = work_frame_h() - TITLEBAR_H - 2 * BORDER_W;
                                      return v < 1 ? 1 : v; }

/* Compute the FRAME x/y + CLIENT w/h for a snap target. Returns 0 on success. */
static int snap_target_rect(int32_t kind, int32_t *fx, int32_t *fy,
                            uint32_t *cw, uint32_t *ch) {
    int32_t half = work_w() / 2;
    int32_t cwid = half - BORDER_W;                /* client w fitting one half */
    if (cwid < 1) cwid = 1;
    int32_t chgt = work_client_h();
    switch (kind) {
        case SNAP_LEFT:
            *fx = work_x0() + BORDER_W;
            *fy = work_y0() + BORDER_W;
            *cw = (uint32_t)cwid; *ch = (uint32_t)chgt;
            return 0;
        case SNAP_RIGHT:
            *fx = work_x0() + half + BORDER_W;
            *fy = work_y0() + BORDER_W;
            *cw = (uint32_t)cwid; *ch = (uint32_t)chgt;
            return 0;
        case SNAP_MAX: {
            int32_t fullw = work_w() - 2 * BORDER_W;
            if (fullw < 1) fullw = 1;
            *fx = work_x0() + BORDER_W;
            *fy = work_y0() + BORDER_W;
            *cw = (uint32_t)fullw; *ch = (uint32_t)chgt;
            return 0;
        }
        default:
            return -1;
    }
}

/* Visible taskbar index for a slot (matches render_dock's slot-order layout). */
static int taskbar_index_of(int slot) {
    int idx = 0;
    for (int s = 0; s < MAX_WINDOWS; s++) {
        if (!g_windows[s].used) continue;
        if (s == slot) return idx;
        idx++;
    }
    return -1;
}

/* ====================================================================== *
 *  Animation engine                                                       *
 * ====================================================================== */

/* Compute raw linear t (Q8 in [0,256]) for a slot's current phase at `now`. */
static int32_t anim_linear_t(window_t *win, long now) {
    if (win->anim_dur_ms <= 0) return 256;
    long dt = now - win->anim_start_ms;
    if (dt <= 0) return 0;
    if (dt >= win->anim_dur_ms) return 256;
    return (int32_t)((dt * 256) / win->anim_dur_ms);
}

/* Eased progress for the VISUAL window animations (open/close/minimize scale).
 * ease-out-cubic: 1 - (1-t)^3, Q8 in [0,256]. Smooth deceleration instead of a
 * linear ramp -> a more satisfying, Windows-like feel. The timing/completion
 * logic keeps using the raw linear t (it must hit exactly 256 at the end). */
static int32_t anim_eased_t(window_t *win, long now) {
    int32_t t = anim_linear_t(win, now);
    if (t <= 0)   return 0;
    if (t >= 256) return 256;
    int32_t inv = 256 - t;
    int64_t i2  = ((int64_t)inv * inv) >> 8;
    int64_t i3  = (i2 * inv) >> 8;
    int64_t f   = 256 - i3;
    if (f < 0)   f = 0;
    if (f > 256) f = 256;
    return (int32_t)f;
}

static void anim_begin(window_t *win, int32_t phase, int32_t dur_ms, long now) {
    win->phase = phase;
    win->anim_dur_ms = dur_ms;
    win->anim_start_ms = now;
    /* PERF: every window animation (open/close/minimize/restore/snap) starts
     * here. Mark dirty so the first animated frame renders even if it began on
     * an otherwise-idle frame; anim_tick keeps it dirty for the rest of the run. */
    mark_dirty();
}

/*
 * Per-frame animation advancement. Resolves completed phases:
 *   OPENING    -> NONE (settled full-size opaque)
 *   CLOSING    -> destroy the slot (shmdt + free)
 *   MINIMIZING -> NONE + minimized=1 (parked, hidden, taskbar entry kept)
 *   RESTORING  -> NONE + minimized=0
 * Called once per frame BEFORE composite().
 */
static void destroy_slot(int slot);    /* fwd */

/* M8: Advance each live window's cheap fade-in alpha toward 255.
 * Called once per frame from anim_tick().  Uses only integer math;
 * no floats, no libgcc helpers. */
static void advance_fade_in(long now) {
    for (int s = 0; s < MAX_WINDOWS; s++) {
        window_t *win = &g_windows[s];
        if (!win->used || win->fade_alpha >= 255) continue;
        long dt = now - win->fade_start_ms;
        if (dt <= 0) { win->fade_alpha = 0; continue; }
        if (dt >= FADE_IN_MS) { win->fade_alpha = 255; continue; }
        /* linear ramp: alpha = 255 * dt / FADE_IN_MS */
        win->fade_alpha = (uint32_t)((long)255 * dt / FADE_IN_MS);
        if (win->fade_alpha > 255) win->fade_alpha = 255;
    }
}

static void anim_tick(long now) {
    advance_fade_in(now);
    for (int s = 0; s < MAX_WINDOWS; s++) {
        window_t *win = &g_windows[s];
        if (!win->used || win->phase == PH_NONE) continue;
        int32_t t = anim_linear_t(win, now);
        if (t < 256) continue;                 /* still animating */
        /* phase finished THIS frame: mark dirty so the FINAL settled position
         * renders. Critical for PH_SNAPPING, whose settle step below snaps the
         * geometry to its destination -- without this the gate could skip the
         * last frame and leave the window one tween-step short. Also covers the
         * post-close/minimize/restore repaint (a window vanished/parked). */
        mark_dirty();
        /* phase finished */
        switch (win->phase) {
            case PH_OPENING:
                win->phase = PH_NONE;
                break;
            case PH_CLOSING:
                destroy_slot(s);                /* tears down + clears phase */
                break;
            case PH_MINIMIZING:
                win->phase = PH_NONE;
                win->minimized = 1;             /* park in taskbar */
                break;
            case PH_RESTORING:
                win->phase = PH_NONE;
                win->minimized = 0;
                break;
            case PH_SNAPPING:
                /* settle the geometry at the tween's destination */
                win->x = win->to_x;
                win->y = win->to_y;
                win->w = win->to_w;
                win->h = win->to_h;
                win->phase = PH_NONE;
                break;
            default:
                win->phase = PH_NONE;
                break;
        }
    }

    /* PERF: keep the frame dirty for as long as ANYTHING is animating, so the
     * dirty-gate never skips a frame that would have advanced an animation.
     * This is the single place that covers every per-frame visual motion:
     *   - window open/close/min/restore/snap phases (PH_* != PH_NONE)
     *   - the cheap per-window fade-in (fade_alpha < 255)
     *   - a visible toast (g_toast_dur_ms > 0)
     *   - right-dock folder open/close tween (anim_t in motion)
     *   - right-dock icon bounce
     *   - right-dock magnify scale still settling toward its target
     * If any is live we mark dirty; the gate then composites+presents this
     * frame and re-checks next frame, so motion stays smooth.               */
    for (int s = 0; s < MAX_WINDOWS; s++) {
        window_t *win = &g_windows[s];
        if (!win->used) continue;
        if (win->phase != PH_NONE || win->fade_alpha < 255) { mark_dirty(); break; }
    }
    if (g_toast_dur_ms > 0) mark_dirty();
    for (int fi = 0; fi < g_nfolders; fi++) {
        rdock_folder_state_t *fs = &g_rdock_folders[fi];
        if (fs->open || fs->anim_closing || fs->anim_t > 0) {
            mark_dirty();
            /* GHOST FIX: the rainbow fan-out sweeps member icons up to
             * RDOCK_FAN_RADIUS (140px) LEFT of the dock strip -- well outside any
             * window's commit rect. If a window (e.g. the IDE) commits this frame
             * the damage scissor narrows to that window and clips the fan repaint,
             * leaving stale fanned-icon pixels. Force a full recomposite so the
             * fan's old AND new footprints both repaint. */
            g_full_damage_cooldown = FULL_DAMAGE_COOLDOWN_FRAMES;
            break;
        }
    }
    for (int i = 0; i < rdock_idx_end(); i++) {
        /* magnify scale still easing toward its target == visible motion */
        if (g_rdock_icons[i].bounce_active ||
            g_rdock_icons[i].scale_q8 != g_rdock_icons[i].scale_target) {
            mark_dirty();
            /* GHOST FIX: hover-magnify grows an icon up to ~1.9x (68px) plus a
             * tooltip plate, both LEFT of the 44px strip and OUTSIDE any window's
             * commit rect. A committing window (e.g. the IDE caret blink) would
             * otherwise narrow the damage scissor and clip the dock repaint,
             * leaving stale enlarged-icon / tooltip pixels -- the reported dock
             * hover/animation ghosting. Force a full recomposite so old AND new
             * footprints repaint. Self-limiting: only fires while the scale is
             * actually easing (settled icons emit no motion, so no extra cost). */
            g_full_damage_cooldown = FULL_DAMAGE_COOLDOWN_FRAMES;
            break;
        }
    }
}

/* ====================================================================== *
 *  Compositing                                                            *
 * ====================================================================== */

/* Wallpaper: full-screen vertical navy gradient + subtle dot grid + centered
 * "AutomationOS" branding.  PERF: the per-scanline gradient color is CACHED
 * and recomputed only when the height changes.  The fill itself still runs
 * each frame (it's the background).
 *
 * DOT GRID: every 32px a single pixel is brightened by +0x0C per channel,
 * giving a barely-visible regularity that reads as texture on the T410's TN
 * panel without looking busy. */
static inline uint32_t wall_dot(uint32_t base) {
    uint32_t r = ((base >> 16) & 0xFF) + 0x0Cu;
    uint32_t g = ((base >>  8) & 0xFF) + 0x0Cu;
    uint32_t b = ( base        & 0xFF) + 0x0Cu;
    if (r > 0xFF) r = 0xFF;
    if (g > 0xFF) g = 0xFF;
    if (b > 0xFF) b = 0xFF;
    return 0xFF000000u | (r << 16) | (g << 8) | b;
}
#define WALL_GRID_STEP 32
static void render_desktop(uint32_t *buf, uint32_t w, uint32_t h, uint32_t stride) {
    static uint32_t grad[2160];          /* cached column gradient (>= 4K tall)  */
    static uint32_t grad_h = 0;
    if (h <= 2160) {
        if (h != grad_h) {
            for (uint32_t y = 0; y < h; y++)
                grad[y] = lerp_color(WALL_TOP, WALL_BOT, y, h ? h - 1 : 1);
            grad_h = h;
        }
#if COMP_DAMAGE_SCISSOR
        {
            uint32_t sy0 = (uint32_t)(g_scis_y0 > 0          ? g_scis_y0 : 0);
            uint32_t sy1 = (uint32_t)(g_scis_y1 < (int32_t)h ? g_scis_y1 : h);
            uint32_t sx0 = (uint32_t)(g_scis_x0 > 0          ? g_scis_x0 : 0);
            uint32_t sx1 = (uint32_t)(g_scis_x1 < (int32_t)w ? g_scis_x1 : w);
            if (sy0 < sy1 && sx0 < sx1) {
                for (uint32_t y = sy0; y < sy1; y++) {
                    uint32_t c = grad[y];
                    uint32_t *r = buf + y * stride;
                    if ((y % WALL_GRID_STEP) == 0) {
                        /* Grid row: sparse dots every 32px, rest = gradient. */
                        uint32_t cdot = wall_dot(c);
                        for (uint32_t x = sx0; x < sx1; x++)
                            r[x] = ((x % WALL_GRID_STEP) == 0) ? cdot : c;
                    } else {
                        /* Non-grid row (97%): uniform color -> 64-bit bulk fill. */
                        uint32_t span = sx1 - sx0;
                        uint64_t c64 = ((uint64_t)c << 32) | (uint64_t)c;
                        uint64_t *d64 = (uint64_t *)&r[sx0];
                        uint32_t pairs = span >> 1;
                        uint32_t tail  = span & 1u;
                        for (uint32_t i = 0; i < pairs; i++) d64[i] = c64;
                        if (tail) r[sx1 - 1] = c;
                    }
                }
            }
        }
#else
        for (uint32_t y = 0; y < h; y++) {
            uint32_t c = grad[y];
            uint32_t *r = buf + y * stride;
            if ((y % WALL_GRID_STEP) == 0) {
                uint32_t cdot = wall_dot(c);
                for (uint32_t x = 0; x < w; x++)
                    r[x] = ((x % WALL_GRID_STEP) == 0) ? cdot : c;
            } else {
                uint64_t c64 = ((uint64_t)c << 32) | (uint64_t)c;
                uint64_t *d64 = (uint64_t *)r;
                uint32_t pairs = w >> 1;
                uint32_t tail  = w & 1u;
                for (uint32_t i = 0; i < pairs; i++) d64[i] = c64;
                if (tail) r[w - 1] = c;
            }
        }
#endif
    } else {
        for (uint32_t y = 0; y < h; y++) {
            uint32_t c = lerp_color(WALL_TOP, WALL_BOT, y, h ? h - 1 : 1);
            uint32_t *r = buf + y * stride;
            for (uint32_t x = 0; x < w; x++) r[x] = c;
        }
    }

    /* Centered "AutomationOS" branding in the lower-center of the work area,
     * drawn as very subtle dim text (~25% brighter than the gradient). */
    {
        static const char *brand = "AutomationOS";
        int blen = 12;
        int32_t bx = ((int32_t)w - blen * FONT_W) / 2;
        int32_t by = (int32_t)h / 2 + (int32_t)h / 6;
        cz_text(buf, (int)stride, (int)w, (int)h, bx, by, brand, 0xFF2A3A4Au);
    }
}

/* ====================================================================== *
 *  M8: DESKTOP-ICON scan / layout / render                               *
 * ====================================================================== */

/* (Re)enumerate "/Desktop" into g_desk_icons[]. Mirrors how the dock folder
 * code reads directories: opendir -> readdir loop -> closedir. Skips "." and
 * "..". Robust: bails silently if /Desktop can't be opened (e.g. not mounted
 * yet) and just leaves the previous list. */
/* Does the name end in ".elf"? (a runnable app icon). */
static int desk_name_is_elf(const char* s) {
    int n = 0; while (s[n]) n++;
    return n >= 4 && s[n-4] == '.' && s[n-3] == 'e' && s[n-2] == 'l' && s[n-1] == 'f';
}
static int desk_streq(const char* a, const char* b) {
    int i = 0; while (a[i] && b[i]) { if (a[i] != b[i]) return 0; i++; } return a[i] == b[i];
}

static void desk_scan(void) {
    int prev_count = g_desk_count;
    long dfd = syscall(SYS_OPENDIR, (long)"/Desktop", 0, 0);
    if (dfd < 0) {
        if (g_desk_count != 0) {                 /* icons vanished -> repaint */
            mark_dirty();
            g_full_damage_cooldown = FULL_DAMAGE_COOLDOWN_FRAMES;  /* layout change: avoid clipping */
        }
        g_desk_count = 0;
        return;                                  /* no /Desktop -> nothing */
    }

    int n = 0;
    struct dirent ent;
    while (n < DESK_MAX_ICONS) {
        long rr = syscall(SYS_READDIR, dfd, (long)&ent, 0);
        if (rr < 0) break;                        /* end of dir / error */
        /* skip "." and ".." */
        if (ent.d_name[0] == '.' &&
            (ent.d_name[1] == '\0' ||
             (ent.d_name[1] == '.' && ent.d_name[2] == '\0')))
            continue;
        if (ent.d_name[0] == '\0') continue;      /* defensive */
        /* The projects container is not shown as a plain folder -- its children
         * are surfaced individually as PROJECT icons below. */
        if (ent.d_type == DT_DIR && desk_streq(ent.d_name, "Projects")) continue;

        desk_icon_t *di = &g_desk_icons[n];
        /* display label: truncated for the grid. */
        int i = 0;
        while (ent.d_name[i] && i < DESK_NAME_DISP) { di->name[i] = ent.d_name[i]; i++; }
        di->name[i] = '\0';
        /* identity: the FULL VFS path, built from the UNtruncated name -- never
         * reconstructed from the label (that was the open-the-wrong-folder bug). */
        { const char* pre = "/Desktop/"; int p = 0;
          while (pre[p] && p < (int)sizeof(di->path) - 1) { di->path[p] = pre[p]; p++; }
          int q = 0;
          while (ent.d_name[q] && p < (int)sizeof(di->path) - 1) di->path[p++] = ent.d_name[q++];
          di->path[p] = '\0'; }
        di->is_dir = (ent.d_type == DT_DIR);
        di->kind   = di->is_dir ? DI_FOLDER
                   : (desk_name_is_elf(ent.d_name) ? DI_APP : DI_FILE);
        n++;
    }
    syscall(SYS_CLOSEDIR, dfd, 0, 0);

    /* Surface each project under /Desktop/Projects as a PROJECT icon on the
     * desktop -- the project folder IS the desktop icon. Bails silently if the
     * directory doesn't exist yet (no projects created). */
    long pfd = syscall(SYS_OPENDIR, (long)"/Desktop/Projects", 0, 0);
    if (pfd >= 0) {
        struct dirent pent;
        while (n < DESK_MAX_ICONS) {
            long rr = syscall(SYS_READDIR, pfd, (long)&pent, 0);
            if (rr < 0) break;
            if (pent.d_name[0] == '.' &&
                (pent.d_name[1] == '\0' ||
                 (pent.d_name[1] == '.' && pent.d_name[2] == '\0'))) continue;
            if (pent.d_name[0] == '\0') continue;
            if (pent.d_type != DT_DIR) continue;      /* projects are directories */

            desk_icon_t *di = &g_desk_icons[n];
            int i = 0;
            while (pent.d_name[i] && i < DESK_NAME_DISP) { di->name[i] = pent.d_name[i]; i++; }
            di->name[i] = '\0';
            { const char* pre = "/Desktop/Projects/"; int p = 0;
              while (pre[p] && p < (int)sizeof(di->path) - 1) { di->path[p] = pre[p]; p++; }
              int q = 0;
              while (pent.d_name[q] && p < (int)sizeof(di->path) - 1) di->path[p++] = pent.d_name[q++];
              di->path[p] = '\0'; }
            di->is_dir = 1;
            di->kind   = DI_PROJECT;
            n++;
        }
        syscall(SYS_CLOSEDIR, pfd, 0, 0);
    }

    /* PERF: the periodic /Desktop rescan only changes the screen when the icon
     * set changed. Use the count as a cheap change proxy and mark dirty so a
     * newly-created/removed icon repaints (the common case: IDE output, New
     * Folder). A rename that keeps the same count is rare on /Desktop and is
     * picked up by the next genuine dirty frame; biasing here would force a full
     * recomposite every rescan and defeat the gate. */
    if (n != prev_count) {
        mark_dirty();
        /* An icon appeared/disappeared (e.g. an IDE build dropped a new ELF on
         * /Desktop) -> the grid relayouts. Force a full recomposite so a window
         * committing this same frame can't clip the relayout into a ghost. */
        g_full_damage_cooldown = FULL_DAMAGE_COOLDOWN_FRAMES;
    }
    g_desk_count = n;
}

/* Compute the grid cell origin (top-left of the TILE) for desktop icon [idx],
 * laid out in a top-left grid and CLAMPED so the whole DESK_CELL fits between
 * the panel and the dock and left of the right dock. Returns 1 if a valid,
 * fully-on-screen slot exists for idx, else 0 (caller skips it). */
static int desk_icon_origin(int idx, uint32_t W, uint32_t H,
                            int32_t *otx, int32_t *oty) {
    int32_t area_x0 = DESK_ORIGIN_X;
    int32_t area_y0 = PANEL_H + 8;
    int32_t area_x1 = (int32_t)W - RDOCK_W - 4;     /* keep clear of right dock */
    int32_t area_y1 = (int32_t)H - DOCK_H - 4;      /* keep clear of bottom dock */

    int32_t avail_w = area_x1 - area_x0;
    int32_t avail_h = area_y1 - area_y0;
    if (avail_w < DESK_CELL_W || avail_h < DESK_CELL_H) return 0;

    int cols = avail_w / DESK_CELL_W;
    int rows = avail_h / DESK_CELL_H;
    if (cols < 1) cols = 1;
    if (rows < 1) rows = 1;
    if (idx >= cols * rows) return 0;               /* no room: skip */

    /* column-major: fill top-to-bottom then left-to-right (standard desktop) */
    int col = idx / rows;
    int row = idx % rows;

    int32_t cell_x = area_x0 + col * DESK_CELL_W;
    int32_t cell_y = area_y0 + row * DESK_CELL_H;

    /* center the tile horizontally inside its cell */
    int32_t tx = cell_x + (DESK_CELL_W - DESK_TILE) / 2;
    int32_t ty = cell_y;

    /* Final hard clamp so neither the tile NOR its label can leave the buffer.
     * (icon_* and font_draw both clip-by-cell only; tile must be fully inside.) */
    if (tx < 2) tx = 2;
    if (ty < PANEL_H + 2) ty = PANEL_H + 2;
    if (tx + DESK_TILE > area_x1) return 0;
    if (ty + DESK_TILE + DESK_LABEL_GAP + FONT_H > area_y1) return 0;

    *otx = tx; *oty = ty;
    return 1;
}

/* Draw all desktop icons onto the wallpaper. Called from composite() right
 * after render_desktop(), so they sit beneath windows/chrome. */
static void render_desktop_icons(uint32_t *buf, uint32_t w, uint32_t h, uint32_t stride,
                                 int32_t cur_x, int32_t cur_y) {
    for (int i = 0; i < g_desk_count; i++) {
        int32_t tx, ty;
        if (!desk_icon_origin(i, w, h, &tx, &ty)) continue;

        desk_icon_t *di = &g_desk_icons[i];

        /* hover highlight rectangle behind the whole cell */
        int hovered = (cur_x >= tx - 6 && cur_x < tx + DESK_TILE + 6 &&
                       cur_y >= ty - 2 && cur_y < ty + DESK_TILE + DESK_LABEL_GAP + FONT_H + 2);
        if (hovered)
            fill_round_rect(buf, w, h, stride, tx - 6, ty - 2,
                            DESK_TILE + 12, DESK_TILE + DESK_LABEL_GAP + FONT_H + 4,
                            8, 0x550A84FFu);   /* accent-blue hover glow (was flat white) */

        /* the icon art (drawn FULLY inside the clamped tile) */
        if (di->is_dir) {
            icon_folder(buf, (int)stride, tx, ty, DESK_TILE, 0xFFE0B040u);
        } else {
            /* recognizable icon when the name matches a known app, else a
             * generic text-file tile. icon_for_app falls back to initials. */
            icon_for_app(buf, (int)stride, tx, ty, DESK_TILE, di->name);
        }

        /* centered name label below the tile (truncated to fit the cell) */
        int nlen = 0; while (di->name[nlen]) nlen++;
        int maxch = DESK_CELL_W / FONT_W;
        if (maxch < 1) maxch = 1;
        char lbl[DESK_NAME_DISP + 1];
        int show = nlen > maxch ? maxch : nlen;
        int li;
        for (li = 0; li < show; li++) lbl[li] = di->name[li];
        lbl[show] = '\0';
        int32_t lbl_w = show * FONT_W;
        int32_t lbl_x = tx + DESK_TILE / 2 - lbl_w / 2;
        int32_t lbl_y = ty + DESK_TILE + DESK_LABEL_GAP;
        if (lbl_x < 2) lbl_x = 2;
        cz_text(buf, (int)stride, (int)w, (int)h, lbl_x, lbl_y, lbl, COL_TEXT);
    }
}

/* Soft layered drop shadow: 4 translucent rounded rects, growing outward and
 * fading, offset down-right, so the window appears to float. */
static void draw_soft_shadow(uint32_t *buf, uint32_t w, uint32_t h, uint32_t stride,
                             int32_t fx, int32_t fy, int32_t fw, int32_t fh) {
    static const int32_t grow[4]  = { 2, 5, 9, 14 };
    /* T410 TN-panel boost: ~40% more opaque so shadows read on low-contrast TN */
    static const uint32_t a[4]    = { 0x70000000u, 0x48000000u, 0x2C000000u, 0x18000000u };
    static const int32_t offy[4]  = { 2, 4, 6, 9 };
    for (int i = 3; i >= 0; i--) {
        int32_t g = grow[i];
        int32_t sx = fx - g + 2;            /* slight right bias */
        int32_t sy = fy - g + offy[i];      /* down bias */
        int32_t sw = fw + 2 * g;
        int32_t sh = fh + 2 * g;
        /* blended rounded rect (manual: blend the round-rect span set) */
        int32_t r = WIN_RADIUS + g;
        /* middle band */
        blend_rect(buf, w, h, stride, sx, sy + r, sw, sh - 2 * r, a[i]);
        blend_rect(buf, w, h, stride, sx + r, sy, sw - 2 * r, r, a[i]);
        blend_rect(buf, w, h, stride, sx + r, sy + sh - r, sw - 2 * r, r, a[i]);
        /* corner discs */
        for (int32_t dy = 0; dy < r; dy++) {
            for (int32_t dx = 0; dx < r; dx++) {
                int32_t off = (r - 1 - dx) * (r - 1 - dx) + (r - 1 - dy) * (r - 1 - dy);
                if (off > (r - 1) * (r - 1)) continue;
                blend_rect(buf, w, h, stride, sx + dx, sy + dy, 1, 1, a[i]);
                blend_rect(buf, w, h, stride, sx + sw - 1 - dx, sy + dy, 1, 1, a[i]);
                blend_rect(buf, w, h, stride, sx + dx, sy + sh - 1 - dy, 1, 1, a[i]);
                blend_rect(buf, w, h, stride, sx + sw - 1 - dx, sy + sh - 1 - dy, 1, 1, a[i]);
            }
        }
    }
}

/*
 * Draw a settled (phase==NONE, non-minimized) window with full decorations,
 * rounded outer corners + soft shadow. frame_x/frame_y is the top-left of the
 * titlebar; the client area sits below it. Content is clipped to the chrome-
 * free band [PANEL_H, H-DOCK_H).
 *
 * M8 fade-in: if win->fade_alpha < 255 the window is still fading in.
 * The chrome rectangles are drawn as translucent blend_rects, and the
 * client surface is blitted via blit_surface_scaled_alpha at scale 1:1
 * with the fade alpha.  Once fade_alpha == 255 (the fast path), all
 * drawing is the same opaque code as before.
 */
/* Compute the outer screen-space bounding box of a window INCLUDING its chrome
 * (titlebar, 1px border) and the 4-layer soft shadow.  The result is clamped to
 * [0, g_fb_w) x [0, g_fb_h) so callers can union it directly into the damage
 * accumulator.  This is the single source of truth for every footprint consumer
 * (damage_add, present hinting, etc.) -- no caller has to remember SHADOW_PAD. */
static void win_footprint(const window_t *win,
                          int32_t *ox0, int32_t *oy0, int32_t *ox1, int32_t *oy1) {
    int32_t outer_x = win->x - BORDER_W;
    int32_t outer_y = win->y - BORDER_W;
    int32_t full_w  = (int32_t)win->w + 2 * BORDER_W;
    int32_t full_h  = (int32_t)win->h + TITLEBAR_H + 2 * BORDER_W;
    int32_t x0 = outer_x - SHADOW_PAD;
    int32_t y0 = outer_y - SHADOW_PAD;
    int32_t x1 = outer_x + full_w + SHADOW_PAD;
    int32_t y1 = outer_y + full_h + SHADOW_PAD;
    /* clamp to screen */
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > (int32_t)g_fb_w) x1 = (int32_t)g_fb_w;
    if (y1 > (int32_t)g_fb_h) y1 = (int32_t)g_fb_h;
    *ox0 = x0; *oy0 = y0; *ox1 = x1; *oy1 = y1;
}

static void render_window_static(uint32_t *buf, uint32_t w, uint32_t h, uint32_t stride,
                                 window_t *win, int focused) {
    int32_t cw = (int32_t)win->w;
    int32_t ch = (int32_t)win->h;
    int32_t fx = win->x;
    int32_t fy = win->y;
    int32_t client_x = fx;
    int32_t client_y = fy + TITLEBAR_H;

    int32_t clip_y0 = PANEL_H;
    int32_t clip_y1 = (int32_t)h - DOCK_H;

    uint32_t tb_col  = focused ? TITLEBAR_FOCUS : TITLEBAR_UNFOC;
    uint32_t bd_col  = focused ? BORDER_FOCUS   : BORDER_UNFOC;

    int32_t full_w = cw + 2 * BORDER_W;
    int32_t full_h = ch + TITLEBAR_H + 2 * BORDER_W;
    int32_t outer_x = fx - BORDER_W;
    int32_t outer_y = fy - BORDER_W;

    /* M8 fade-in: convert 0..255 alpha to 0..256 Q8 for blend_pixel_a256. */
    uint32_t fa256 = win->fade_alpha;               /* 0..255 */
    if (fa256 >= 255) fa256 = 256;                  /* 255 snaps to fully opaque */
    int fading = (fa256 < 256);

    /* soft drop shadow (always draw, fading shadow is a nice touch) */
    if (!fading) {
        draw_soft_shadow(buf, w, h, stride, outer_x, outer_y, full_w, full_h);
    } else {
        /* Lighter shadow during fade so it doesn't look baked-in while transparent */
        static const uint32_t sha[4] = { 0x22000000u, 0x18000000u, 0x0E000000u, 0x08000000u };
        static const int32_t  sgrow[4] = { 2, 5, 9, 14 };
        static const int32_t  soffy[4] = { 2, 4, 6,  9 };
        for (int si = 3; si >= 0; si--) {
            int32_t sg = sgrow[si];
            int32_t sx2 = outer_x - sg + 2;
            int32_t sy2 = outer_y - sg + soffy[si];
            int32_t sw2 = full_w + 2 * sg;
            int32_t sh2 = full_h + 2 * sg;
            /* alpha-scale by fade fraction */
            uint32_t sa = (((sha[si] >> 24) * fa256) >> 8) << 24;
            uint32_t sc = sa | (sha[si] & 0x00FFFFFFu);
            blend_rect(buf, w, h, stride, sx2, sy2 + WIN_RADIUS, sw2, sh2 - 2 * WIN_RADIUS, sc);
            blend_rect(buf, w, h, stride, sx2 + WIN_RADIUS, sy2, sw2 - 2 * WIN_RADIUS, WIN_RADIUS, sc);
            blend_rect(buf, w, h, stride, sx2 + WIN_RADIUS, sy2 + sh2 - WIN_RADIUS,
                       sw2 - 2 * WIN_RADIUS, WIN_RADIUS, sc);
        }
    }

    if (!fading) {
        /* ---- FAST PATH: fully faded in, use opaque drawing ---- */

        /* rounded border + titlebar */
        fill_round_top_rect(buf, w, h, stride, fx, fy, cw, TITLEBAR_H, WIN_RADIUS, tb_col);

        /* close + minimize boxes */
        int32_t close_x = fx + cw - CLOSE_SZ - 8;
        int32_t close_y = fy + (TITLEBAR_H - CLOSE_SZ) / 2;
        int32_t min_x   = close_x - MIN_SZ - 6;
        int32_t min_y   = fy + (TITLEBAR_H - MIN_SZ) / 2;
        fill_round_rect(buf, w, h, stride, close_x, close_y, CLOSE_SZ, CLOSE_SZ, 3, BTN_CLOSE);
        fill_round_rect(buf, w, h, stride, min_x,   min_y,   MIN_SZ,   MIN_SZ,   3, BTN_MIN);
        cz_text(buf, (int)stride, (int)w, (int)h,
                         close_x + (CLOSE_SZ - FONT_W) / 2,
                         close_y + (CLOSE_SZ - FONT_H) / 2, "x", COL_TEXT);
        cz_text(buf, (int)stride, (int)w, (int)h,
                         min_x + (MIN_SZ - FONT_W) / 2,
                         min_y + (MIN_SZ - FONT_H) / 2, "-", 0xFF000000u);

        /* maximize/restore box (left of minimize): a square-outline glyph when
         * un-maximized, overlapping-rectangles glyph when maximized (restore).
         * Font-independent. Clicking it toggles SNAP_MAX (see handle_mouse). */
        int32_t max_x = min_x - MIN_SZ - 6;
        int32_t max_y = fy + (TITLEBAR_H - MIN_SZ) / 2;
        {
            uint32_t max_bg = (win->snap_state == SNAP_MAX) ? 0xFF5BAF5Bu : BTN_MIN;
            fill_round_rect(buf, w, h, stride, max_x, max_y, MIN_SZ, MIN_SZ, 3, max_bg);
            int32_t gs = (MIN_SZ > 10) ? 8 : ((int32_t)MIN_SZ - 4);
            int32_t gx = max_x + ((int32_t)MIN_SZ - gs) / 2;
            int32_t gy = max_y + ((int32_t)MIN_SZ - gs) / 2;
            if (win->snap_state == SNAP_MAX) {
                /* restore glyph: two overlapping small squares */
                int32_t rs = gs * 3 / 4;       /* sub-rect size */
                int32_t off = gs - rs;          /* offset for second rect */
                /* back rect (top-right) */
                fill_round_rect(buf, w, h, stride, gx + off, gy, rs, rs, 1, 0xFF202020u);
                fill_round_rect(buf, w, h, stride, gx + off + 1, gy + 1, rs - 2, rs - 2, 0, max_bg);
                /* front rect (bottom-left) */
                fill_round_rect(buf, w, h, stride, gx, gy + off, rs, rs, 1, 0xFF202020u);
                fill_round_rect(buf, w, h, stride, gx + 1, gy + off + 1, rs - 2, rs - 2, 0, max_bg);
            } else {
                /* maximize glyph: single square outline */
                fill_round_rect(buf, w, h, stride, gx, gy, gs, gs, 1, 0xFF202020u);
                fill_round_rect(buf, w, h, stride, gx + 1, gy + 2, gs - 2, gs - 3, 1, BTN_MIN);
            }
        }

        /* window title text. IDE-REPAIR-0 I0: sanitize a path-like title to its
         * last component so a stray raw VFS path never shows in a titlebar. */
        const char *tt = win->title[0] ? win->title : "window", *tb = tt;
        for (const char *p = tt; *p; p++) if (*p == '/') tb = p + 1;
        if (!*tb) tb = "window";
        cz_text(buf, (int)stride, (int)w, (int)h,
                         fx + 8, fy + (TITLEBAR_H - FONT_H) / 2, tb,
                         focused ? COL_TEXT : COL_TEXT_DIM);

        /* client surface. Clamp the SOURCE read to the client's REAL SHM extent
         * (buf_w x buf_h): when a window is maximized, win->w/win->h grow to the
         * maximized rect but the client's buffer stays buf_w x buf_h, so reading
         * win->w x win->h rows here would walk past the mapped segment and fault
         * the compositor. Clamping letterboxes the smaller content instead. */
        if (win->pixels) {
            uint32_t sw = win->w < win->buf_w ? win->w : win->buf_w;
            uint32_t sh = win->h < win->buf_h ? win->h : win->buf_h;
            blit_surface_clip(buf, w, h, stride,
                              win->pixels, sw, sh, win->stride,
                              client_x, client_y, clip_y0, clip_y1);
        } else {
            int32_t py0 = client_y < clip_y0 ? clip_y0 : client_y;
            int32_t py1 = client_y + ch;
            if (py1 > clip_y1) py1 = clip_y1;
            if (py1 > py0)
                fill_rect(buf, w, h, stride, client_x, py0, cw, py1 - py0, WIN_PLACEHOLDER);
        }

        /* 1px border around the whole frame */
        stroke_rect(buf, w, h, stride, outer_x, outer_y, full_w, full_h, bd_col);

    } else {
        /* ---- FADE-IN PATH: blend everything at fa256 alpha ---- */

        /* Titlebar: alpha-blended filled rounded-top rect. We approximate by
         * drawing a plain blended rect (the full titlebar band) rather than the
         * rounded variant to keep this path simple and cheap. */
        {
            int32_t ty1 = fy, ty2 = fy + TITLEBAR_H;
            if (ty1 < clip_y0) ty1 = clip_y0;
            if (ty2 > clip_y1) ty2 = clip_y1;
            if (ty2 > ty1) {
                int32_t x1 = fx < 0 ? 0 : fx;
                int32_t x2 = fx + cw; if (x2 > (int32_t)w) x2 = (int32_t)w;
                uint32_t tbr = (tb_col >> 16) & 0xFF;
                uint32_t tbg = (tb_col >>  8) & 0xFF;
                uint32_t tbb = (tb_col      ) & 0xFF;
                for (int32_t yy = ty1; yy < ty2; yy++) {
#if COMP_DAMAGE_SCISSOR
                    if (yy < g_scis_y0 || yy >= g_scis_y1) continue;
#endif
                    uint32_t *drow = buf + (uint32_t)yy * stride;
                    for (int32_t xx = x1; xx < x2; xx++) {
#if COMP_DAMAGE_SCISSOR
                        if (xx < g_scis_x0 || xx >= g_scis_x1) continue;
#endif
                        uint32_t d = drow[xx];
                        uint32_t dr = (d >> 16) & 0xFF, dg = (d >> 8) & 0xFF, db = d & 0xFF;
                        uint32_t ia = 256 - fa256;
                        uint32_t or_ = (tbr * fa256 + dr * ia) >> 8;
                        uint32_t og  = (tbg * fa256 + dg * ia) >> 8;
                        uint32_t ob  = (tbb * fa256 + db * ia) >> 8;
                        drow[xx] = 0xFF000000u | (or_ << 16) | (og << 8) | ob;
                    }
                }
            }
        }

        /* Client surface: 1:1 scale blit via blit_surface_scaled_alpha */
        if (win->pixels) {
            /* clamp source read to the real SHM extent (see fast-path note) */
            uint32_t sw = win->w < win->buf_w ? win->w : win->buf_w;
            uint32_t sh = win->h < win->buf_h ? win->h : win->buf_h;
            blit_surface_scaled_alpha(buf, w, h, stride,
                                      win->pixels, sw, sh, win->stride,
                                      client_x, client_y,
                                      256, 256, fa256,
                                      clip_y0, clip_y1);
        } else {
            /* placeholder: blended solid rect */
            int32_t py0 = client_y < clip_y0 ? clip_y0 : client_y;
            int32_t py1 = client_y + ch;
            if (py1 > clip_y1) py1 = clip_y1;
            if (py1 > py0) {
                uint32_t phcol = (fa256 << 24) | (WIN_PLACEHOLDER & 0x00FFFFFFu);
                blend_rect(buf, w, h, stride, client_x, py0, cw, py1 - py0, phcol);
            }
        }

        /* 1px border (alpha-blended) */
        {
            uint32_t ba = (fa256 << 24) | (bd_col & 0x00FFFFFFu);
            blend_rect(buf, w, h, stride, outer_x, outer_y, full_w, 1, ba);
            blend_rect(buf, w, h, stride, outer_x, outer_y + full_h - 1, full_w, 1, ba);
            blend_rect(buf, w, h, stride, outer_x, outer_y, 1, full_h, ba);
            blend_rect(buf, w, h, stride, outer_x + full_w - 1, outer_y, 1, full_h, ba);
        }

        /* No rounded-corner punch during fade: corners remain as-drawn, which is
         * fine since the overall alpha is low and the effect is brief (~150ms). */
        return;
    }  /* end fade-in path */

    /* punch rounded OUTER corners: overwrite the corner pixels of the whole
     * frame rect with the wallpaper color underneath. We recompute the
     * wallpaper per scanline so the corners blend into the gradient. */
    for (int32_t ly = 0; ly < full_h; ly++) {
        int32_t py = outer_y + ly;
        if (py < 0 || py >= (int32_t)h) continue;
#if COMP_DAMAGE_SCISSOR
        if (py < g_scis_y0 || py >= g_scis_y1) continue;
#endif
        uint32_t wall = lerp_color(WALL_TOP, WALL_BOT, (uint32_t)py, h ? h - 1 : 1);
        for (int32_t lx = 0; lx < full_w; lx++) {
            if (!round_corner_clipped(lx, ly, full_w, full_h, WIN_RADIUS)) continue;
            int32_t px = outer_x + lx;
            if (px < 0 || px >= (int32_t)w) continue;
#if COMP_DAMAGE_SCISSOR
            if (px < g_scis_x0 || px >= g_scis_x1) continue;
#endif
            if (py < PANEL_H || py >= (int32_t)h - DOCK_H) continue;  /* respect chrome band */
            buf[(uint32_t)py * stride + (uint32_t)px] = wall;
        }
    }
}

/*
 * Draw an ANIMATING window (phase != NONE). We compute an eased progress, a
 * scale (Q8) and an alpha (0..256), and a draw origin. The whole window
 * (titlebar + client) is captured conceptually as the client surface plus
 * decorations; for simplicity + speed we animate the CLIENT pixel buffer
 * scaled/alpha-blended, and draw the titlebar chrome at matching alpha at the
 * scaled position. This keeps the effect crisp without an offscreen capture.
 */
static void render_window_anim(uint32_t *buf, uint32_t w, uint32_t h, uint32_t stride,
                               window_t *win, int focused, long now) {
    int32_t cw = (int32_t)win->w;
    int32_t ch = (int32_t)win->h;
    int32_t clip_y0 = PANEL_H;
    int32_t clip_y1 = (int32_t)h - DOCK_H;

    int32_t lin = anim_eased_t(win, now);

    /* geometry: settled frame top-left */
    int32_t fx = win->x, fy = win->y;

    int32_t scale_num = 256, scale_den = 256;     /* Q8 scale = 1.0 */
    uint32_t alpha = 256;
    int32_t draw_fx = fx, draw_fy = fy;

    if (win->phase == PH_OPENING) {
        int32_t e = ease_out_cubic(lin);
        /* scale 0.90 -> 1.00 : 230/256 .. 256/256 */
        scale_num = 230 + (256 - 230) * e / 256;
        alpha = (uint32_t)e;                       /* 0 -> 256 */
    } else if (win->phase == PH_CLOSING) {
        int32_t e = ease_in_cubic(lin);
        /* scale 1.00 -> 0.90 */
        scale_num = 256 - (256 - 230) * e / 256;
        alpha = (uint32_t)(256 - e);               /* 256 -> 0 */
    } else if (win->phase == PH_MINIMIZING || win->phase == PH_RESTORING) {
        /* shrink + slide toward (or out of) the taskbar button. */
        int32_t e;
        int32_t idx = (win->tb_idx >= 0) ? win->tb_idx : taskbar_index_of((int)(win - g_windows));
        int32_t tb_x = taskbtn_x(idx >= 0 ? idx : 0);
        int32_t tb_y = taskbtn_y(g_fb_h);
        /* target: small window centered on the taskbar button */
        int32_t tgt_fx = tb_x + (TASK_W - cw / 6) / 2;     /* shrink to ~1/6 then alpha-out */
        int32_t tgt_fy = tb_y;
        int32_t min_num = 96;                              /* shrink to ~0.375 */
        if (win->phase == PH_MINIMIZING) {
            e = ease_in_out_cubic(lin);
            scale_num = 256 - (256 - min_num) * e / 256;
            draw_fx = fx + (tgt_fx - fx) * e / 256;
            draw_fy = fy + (tgt_fy - fy) * e / 256;
            alpha = (uint32_t)(256 - e * 200 / 256);       /* fade toward ~22% near end */
            if (alpha > 256) alpha = 256;
        } else { /* RESTORING: reverse */
            e = ease_in_out_cubic(lin);
            int32_t r = 256 - e;                            /* 256 -> 0 */
            scale_num = 256 - (256 - min_num) * r / 256;
            draw_fx = fx + (tgt_fx - fx) * r / 256;
            draw_fy = fy + (tgt_fy - fy) * r / 256;
            alpha = (uint32_t)(256 - r * 200 / 256);
            if (alpha > 256) alpha = 256;
        }
        if (idx < 0) { /* no taskbar slot (shouldn't happen) -> just fade */
            draw_fx = fx; draw_fy = fy;
        }
    }

    if (alpha == 0) return;

    int32_t draw_client_x = draw_fx;
    int32_t draw_client_y = draw_fy + TITLEBAR_H;

    /* Animated titlebar chrome: scale the titlebar fill the same as content by
     * drawing a scaled rounded rect. For simplicity (and to avoid a second
     * scaler) we draw the titlebar at the *scaled width* anchored at the scaled
     * origin, then alpha-blend with the chrome color. */
    int32_t sw = (int32_t)((uint64_t)cw * (uint32_t)scale_num / (uint32_t)scale_den);
    int32_t sh_tb = (int32_t)((uint64_t)TITLEBAR_H * (uint32_t)scale_num / (uint32_t)scale_den);
    if (sw < 1) sw = 1;
    if (sh_tb < 1) sh_tb = 1;
    /* center the scaled titlebar over the natural titlebar center */
    int32_t tb_cx = draw_fx + cw / 2;
    int32_t tb_cy = draw_fy + TITLEBAR_H / 2;
    int32_t tb_x = tb_cx - sw / 2;
    int32_t tb_y = tb_cy - sh_tb / 2;
    uint32_t tb_col = focused ? TITLEBAR_FOCUS : TITLEBAR_UNFOC;
    /* clamp the titlebar draw into the chrome band */
    {
        int32_t yy0 = tb_y, yy1 = tb_y + sh_tb;
        if (yy0 < clip_y0) yy0 = clip_y0;
        if (yy1 > clip_y1) yy1 = clip_y1;
        if (yy1 > yy0) {
            /* alpha-blend the titlebar fill */
            int32_t x1 = tb_x < 0 ? 0 : tb_x;
            int32_t x2 = tb_x + sw;
            if (x2 > (int32_t)w) x2 = (int32_t)w;
            for (int32_t yy = yy0; yy < yy1; yy++) {
                uint32_t *drow = buf + (uint32_t)yy * stride;
                for (int32_t xx = x1; xx < x2; xx++)
                    drow[xx] = blend_pixel_a256(tb_col, drow[xx], alpha);
            }
        }
    }

    /* Animated client content: scaled + alpha-blended about its center.
     * Clamp the source read to the real SHM extent (see fast-path note). */
    if (win->pixels) {
        uint32_t sw = win->w < win->buf_w ? win->w : win->buf_w;
        uint32_t sh = win->h < win->buf_h ? win->h : win->buf_h;
        blit_surface_scaled_alpha(buf, w, h, stride,
                                  win->pixels, sw, sh, win->stride,
                                  draw_client_x, draw_client_y,
                                  scale_num, scale_den, alpha,
                                  clip_y0, clip_y1);
    }
}

/*
 * M6: SNAP geometry tween. Interpolate (from_*) -> (to_*) with ease_in_out_cubic
 * and draw a normal, fully-opaque, decorated window at the interpolated rect.
 * We do this by temporarily overriding win->x/y/w/h with the tween values and
 * reusing render_window_static (so corners/shadow/titlebar all match), then
 * restoring the settled fields. The blit clips the client surface to the new
 * width/height; if the client buffer is smaller, the placeholder fills the rest.
 */
static void render_window_snapping(uint32_t *buf, uint32_t w, uint32_t h, uint32_t stride,
                                   window_t *win, int focused, long now) {
    int32_t lin = anim_eased_t(win, now);
    int32_t e   = ease_in_out_cubic(lin);          /* Q8 progress 0..256 */

    int32_t ix = win->from_x + (win->to_x - win->from_x) * e / 256;
    int32_t iy = win->from_y + (win->to_y - win->from_y) * e / 256;
    int32_t iw = (int32_t)win->from_w + ((int32_t)win->to_w - (int32_t)win->from_w) * e / 256;
    int32_t ih = (int32_t)win->from_h + ((int32_t)win->to_h - (int32_t)win->from_h) * e / 256;
    if (iw < 1) iw = 1;
    if (ih < 1) ih = 1;

    /* override -> draw -> restore */
    int32_t  ox = win->x, oy = win->y;
    uint32_t ow = win->w, oh = win->h;
    win->x = ix; win->y = iy; win->w = (uint32_t)iw; win->h = (uint32_t)ih;
    render_window_static(buf, w, h, stride, win, focused);
    win->x = ox; win->y = oy; win->w = ow; win->h = oh;
}

/* ---------------------------------------------------------------------- *
 *  Chrome: top panel + bottom dock (always drawn on top of windows)      *
 * ---------------------------------------------------------------------- */

/* g_rtc_ok: set once at boot after probing SYS_GETTIME.  When 0 we
 * fall back to formatting uptime (SYS_GET_TICKS_MS) so the clock
 * never shows garbage. */
static int  g_rtc_ok;
static comp_rtc_time_t g_rtc;   /* latest RTC snapshot (refreshed once/sec) */

/* --- Network status indicator (queried once/sec alongside the clock) --- */
static char     g_net_label[20] = "No Net";  /* rendered text: IP or "No Net" */
static uint32_t g_net_color     = 0xFF888888u;  /* gray=down, green=up */
static int      g_net_up        = 0;

/* --- Battery indicator (queried once/sec via SYS_BATTERY) --- */
static int      g_bat_present   = 0;          /* 0 on QEMU/desktop, 1 on laptop */
static int      g_bat_state     = 0;          /* 0=idle, 1=discharging, 2=charging */
static int      g_bat_percent   = 0;          /* 0-100 */
static int      g_bat_ac        = 0;          /* 1 if AC connected */
static char     g_bat_label[12] = "";         /* "BAT: 85%" or "CHG: 85%" */
static uint32_t g_bat_color     = 0xFF44DD44u; /* green >50, yellow 20-50, red <20 */

/* Format a host-order IPv4 into dst (>=16 bytes). */
static void format_ipv4(uint32_t ip, char *dst) {
    int pos = 0;
    for (int octet = 3; octet >= 0; octet--) {
        uint32_t v = (ip >> (octet * 8)) & 0xFF;
        if (v >= 100) dst[pos++] = (char)('0' + v / 100);
        if (v >= 10)  dst[pos++] = (char)('0' + (v / 10) % 10);
        dst[pos++] = (char)('0' + v % 10);
        if (octet > 0) dst[pos++] = '.';
    }
    dst[pos] = '\0';
}

/* Query SYS_NET_INFO and update g_net_label / g_net_color.
 * Called once per second from the frame loop (piggybacked on clock refresh). */
static void refresh_net_status(void) {
    comp_net_info_t info;
    /* zero the struct so a partial kernel fill doesn't leave garbage */
    for (unsigned i = 0; i < sizeof(info); i++) ((uint8_t *)&info)[i] = 0;
    long rc = syscall(SYS_NET_INFO, (long)&info, 0, 0);
    if (rc == 0 && info.up && info.ip != 0) {
        g_net_up    = 1;
        g_net_color = 0xFF44DD44u;   /* green */
        format_ipv4(info.ip, g_net_label);
    } else {
        g_net_up    = 0;
        g_net_color = 0xFF888888u;   /* gray */
        g_net_label[0] = 'N'; g_net_label[1] = 'o'; g_net_label[2] = ' ';
        g_net_label[3] = 'N'; g_net_label[4] = 'e'; g_net_label[5] = 't';
        g_net_label[6] = '\0';
    }
}

/* Format uptime-ms as HH:MM:SS into out (>=9 bytes). Fallback path. */
static void format_clock_uptime(long ms, char *out) {
    long total = ms / 1000;
    long ss = total % 60;
    long mm = (total / 60) % 60;
    long hh = (total / 3600) % 100;   /* wrap hours at 100 to keep 2 digits */
    out[0] = (char)('0' + (hh / 10)); out[1] = (char)('0' + (hh % 10));
    out[2] = ':';
    out[3] = (char)('0' + (mm / 10)); out[4] = (char)('0' + (mm % 10));
    out[5] = ':';
    out[6] = (char)('0' + (ss / 10)); out[7] = (char)('0' + (ss % 10));
    out[8] = '\0';
}

/* Format wall-clock RTC time as HH:MM:SS into out (>=9 bytes). */
static void format_clock_rtc(const comp_rtc_time_t *r, char *out) {
    out[0] = (char)('0' + (r->hour / 10)); out[1] = (char)('0' + (r->hour % 10));
    out[2] = ':';
    out[3] = (char)('0' + (r->min / 10));  out[4] = (char)('0' + (r->min % 10));
    out[5] = ':';
    out[6] = (char)('0' + (r->sec / 10));  out[7] = (char)('0' + (r->sec % 10));
    out[8] = '\0';
}

/* Refresh g_rtc from the CMOS RTC via SYS_GETTIME.  Called at most
 * once per second from the frame loop so the syscall cost is trivial. */
static void refresh_rtc(void) {
    if (!g_rtc_ok) return;
    comp_rtc_time_t tmp;
    long r = syscall(SYS_GETTIME, (long)&tmp, 0, 0);
    if (r == 0) g_rtc = tmp;
}

/* Query battery status via the dedicated SYS_BATTERY syscall (piggybacked on
 * the once/sec clock refresh).  Updates g_bat_* globals.  On QEMU/desktop the
 * kernel returns present=0 and g_bat_present stays 0 so the panel hides the
 * indicator entirely. */
static void refresh_battery(void) {
    struct { uint8_t present; uint8_t state; uint8_t percent; uint8_t ac; } bi = {0};
    long rc = syscall(SYS_BATTERY, (long)&bi, 0, 0);
    if (rc != 0) { g_bat_present = 0; return; }
    g_bat_present = bi.present;
    if (!bi.present) return;
    g_bat_state   = bi.state;
    g_bat_percent = bi.percent;
    g_bat_ac      = bi.ac;

    /* Build label: "CHG: XX%" (charging/AC) or "BAT: XX%" (discharging) */
    const char *pfx = (bi.state == 2 || bi.ac) ? "CHG: " : "BAT: ";
    int p = 0;
    while (pfx[p]) { g_bat_label[p] = pfx[p]; p++; }
    int pct = bi.percent;
    if (pct > 99) { g_bat_label[p++] = '1'; g_bat_label[p++] = '0'; g_bat_label[p++] = '0'; }
    else if (pct >= 10) { g_bat_label[p++] = (char)('0' + pct / 10); g_bat_label[p++] = (char)('0' + pct % 10); }
    else { g_bat_label[p++] = (char)('0' + pct); }
    g_bat_label[p++] = '%';
    g_bat_label[p]   = '\0';

    /* Color: green >50%, yellow 20-50%, red <20% */
    if (pct > 50)      g_bat_color = 0xFF44DD44u;  /* green  */
    else if (pct >= 20) g_bat_color = 0xFFDDCC22u;  /* yellow */
    else                g_bat_color = 0xFFDD4444u;  /* red    */
}

/* Copy a (possibly long) title into a fixed buffer, truncating with an
 * ellipsis-ish ".." if it would overflow `maxchars` glyphs. */
static void truncate_title(const char *src, char *dst, int maxchars) {
    int i = 0;
    while (src[i] && i < maxchars) { dst[i] = src[i]; i++; }
    if (src[i] && maxchars >= 2) { dst[maxchars - 1] = '.'; dst[maxchars - 2] = '.'; }
    dst[i] = '\0';
    if (i >= maxchars) dst[maxchars] = '\0';
}

static void render_panel(uint32_t *buf, uint32_t w, uint32_t h, uint32_t stride, long now) {
    /* panel background + 1px bottom border */
    fill_rect(buf, w, h, stride, 0, 0, (int32_t)w, PANEL_H, COL_PANEL);
    fill_rect(buf, w, h, stride, 0, PANEL_H - 1, (int32_t)w, 1, COL_BORDER);

    /* left: focused window title (or product name).
     * IDE-REPAIR-0 I0 / DESKTOP-PROJECT-REGRESSION-0: a focused window's title
     * must be an app NAME, never a raw VFS path. If it contains a '/', ignore
     * it and keep the product name so "sbin/..." can never render up here. */
    const char *title = "AutomationOS";
    int slot = focused_slot();
    if (slot >= 0 && g_windows[slot].used && g_windows[slot].title[0]) {
        const char *wt = g_windows[slot].title;
        int pathlike = 0;
        for (const char *p = wt; *p; p++) if (*p == '/') { pathlike = 1; break; }
        if (!pathlike) title = wt;
    }
    cz_text(buf, (int)stride, (int)w, (int)h,
                     12, (PANEL_H - FONT_H) / 2, title, COL_TEXT);

    /* right: clock HH:MM:SS -- wall-clock from the CMOS RTC when available,
     * otherwise uptime (SYS_GET_TICKS_MS) as a fallback.  g_rtc is refreshed
     * once per second in the frame loop (see refresh_rtc call site). */
    char clk[9];
    if (g_rtc_ok)
        format_clock_rtc(&g_rtc, clk);
    else
        format_clock_uptime(now, clk);
    int clk_w = (int)k_strlen(clk) * FONT_W;
    /* Legibility: seat the time on a dark rounded pill so it pops off the panel
     * and is visually separated from the (crowded) network indicator to its left
     * -- the user reported "can't see the time very well" on the T410 TN panel. */
    {
        int32_t clk_x = (int)w - clk_w - 12;
        fill_round_rect(buf, w, h, stride, clk_x - 5, (PANEL_H - FONT_H) / 2 - 2,
                        clk_w + 10, FONT_H + 4, 4, 0xFF1C1C1Eu);
    }
    cz_text(buf, (int)stride, (int)w, (int)h,
                     (int)w - clk_w - 12, (PANEL_H - FONT_H) / 2, clk, COL_TEXT);
    /* right: network signal bars (colored by live SYS_NET_INFO status) +
     * IP/status text label, all just left of the clock. */
    {
        int32_t nx     = (int32_t)w - clk_w - 12 - 26;
        int32_t base_y = PANEL_H / 2 + 5;
        uint32_t bar_col = g_net_up ? 0xFF44DD44u : COL_TEXT_DIM;
        for (int b = 0; b < 4; b++) {
            int32_t bh = 3 + b * 3;
            fill_rect(buf, w, h, stride, nx + b * 5, base_y - bh, 3, bh, bar_col);
        }
        /* network status text (IP or "No Net") to the left of the bars */
        int net_w = (int)k_strlen(g_net_label) * FONT_W;
        cz_text(buf, (int)stride, (int)w, (int)h,
                         nx - net_w - 8, (PANEL_H - FONT_H) / 2,
                         g_net_label, g_net_color);

        /* battery percentage left of the network text (only on laptops).
         * Hidden on QEMU/desktop where ec_battery_available() returns false. */
        if (g_bat_present && g_bat_label[0]) {
            int bat_w = (int)k_strlen(g_bat_label) * FONT_W;
            int bat_x = nx - net_w - 8 - bat_w - 12;
            cz_text(buf, (int)stride, (int)w, (int)h,
                             bat_x, (PANEL_H - FONT_H) / 2, g_bat_label, g_bat_color);
        }
    }
}

static void render_dock(uint32_t *buf, uint32_t w, uint32_t h, uint32_t stride,
                        int32_t cur_x, int32_t cur_y) {
    int32_t dy = dock_top(h);
    /* dock background: rounded TOP corners + 1px top border */
    fill_round_top_rect(buf, w, h, stride, 0, dy, (int32_t)w, DOCK_H, 10, COL_PANEL);
    fill_rect(buf, w, h, stride, 0, dy, (int32_t)w, 1, COL_BORDER);

    /* launcher button: rounded accent square labeled "T"; subtle hover lift */
    int32_t lx = launcher_x(), ly = launcher_y(h);
    int launch_hover = (cur_x >= lx && cur_x < lx + LAUNCH_SZ &&
                        cur_y >= ly && cur_y < ly + LAUNCH_SZ);
    if (launch_hover)
        fill_round_rect(buf, w, h, stride, lx - 1, ly - 1, LAUNCH_SZ + 2, LAUNCH_SZ + 2, 9, COL_HOVER);
    fill_round_rect(buf, w, h, stride, lx, ly, LAUNCH_SZ, LAUNCH_SZ, 8, COL_ACCENT);
    cz_text(buf, (int)stride, (int)w, (int)h,
                     lx + (LAUNCH_SZ - FONT_W) / 2, ly + (LAUNCH_SZ - FONT_H) / 2,
                     "T", COL_TEXT);
    /* Tooltip for the launcher button */
    if (launch_hover) {
        int32_t lt_w = 8 * FONT_W + 16;  /* "Terminal" */
        int32_t lt_h = FONT_H + 8;
        int32_t lt_x = lx + LAUNCH_SZ / 2 - lt_w / 2;
        int32_t lt_y = dy - lt_h - 6;
        if (lt_x < 4) lt_x = 4;
        fill_round_rect(buf, w, h, stride, lt_x, lt_y, lt_w, lt_h, 5, 0xF0111111u);
        stroke_rect(buf, w, h, stride, lt_x, lt_y, lt_w, lt_h, COL_BORDER);
        cz_text(buf, (int)stride, (int)w, (int)h,
                         lt_x + 8, lt_y + 4, "Terminal", COL_TEXT);
    }

    /* taskbar: one button per window (minimized windows keep their button). */
    int focused = focused_slot();
    int idx = 0;
    for (int s = 0; s < MAX_WINDOWS; s++) {
        if (!g_windows[s].used) continue;
        int32_t bx = taskbtn_x(idx);
        int32_t by = taskbtn_y(h);
        if (bx + TASK_W > (int32_t)w) break;        /* ran out of dock space */
        int hover = (cur_x >= bx && cur_x < bx + TASK_W &&
                     cur_y >= by && cur_y < by + TASK_H);
        /* Distinct focus vs hover: the active window is ACCENT-filled with a
         * bottom accent bar + white label; a hovered (non-active) button gets a
         * subtle lift (a slightly larger rounded backing) for a fluid feel. */
        if (hover && s != focused)
            fill_round_rect(buf, w, h, stride, bx - 1, by - 1, TASK_W + 2, TASK_H + 2, 5, COL_HOVER);
        uint32_t bg = (s == focused) ? COL_ACCENT : (hover ? COL_HOVER : COL_PANEL);
        fill_round_rect(buf, w, h, stride, bx, by, TASK_W, TASK_H, 4, bg);
        stroke_rect(buf, w, h, stride, bx, by, TASK_W, TASK_H,
                    (s == focused) ? COL_ACCENT : COL_BORDER);
        if (s == focused)                              /* active-window accent bar */
            fill_round_rect(buf, w, h, stride, bx + 3, by + TASK_H - 3, TASK_W - 6, 2, 1, COL_TEXT);
        /* minimized windows get a dim accent dot to show they're parked */
        if (g_windows[s].minimized)
            fill_round_rect(buf, w, h, stride, bx + 4, by + TASK_H / 2 - 2, 4, 4, 2,
                            (s == focused) ? COL_TEXT : COL_ACCENT);
        char t[20];
        truncate_title(g_windows[s].title[0] ? g_windows[s].title : "window",
                       t, (TASK_W - 12) / FONT_W);
        cz_text(buf, (int)stride, (int)w, (int)h,
                         bx + (g_windows[s].minimized ? 12 : 6),
                         by + (TASK_H - FONT_H) / 2, t,
                         (s == focused) ? 0xFFFFFFFFu : COL_TEXT_DIM);

        /* Tooltip: show full window title ABOVE the dock when hovered */
        if (hover) {
            const char *full = g_windows[s].title[0] ? g_windows[s].title : "window";
            int tlen = 0;
            while (full[tlen]) tlen++;
            if (tlen > 40) tlen = 40;   /* cap so the tooltip fits on screen */
            int32_t tip_w = tlen * FONT_W + 16;
            int32_t tip_h = FONT_H + 8;
            int32_t tip_x = bx + TASK_W / 2 - tip_w / 2;
            int32_t tip_y = dy - tip_h - 6;
            if (tip_x < 4) tip_x = 4;
            if (tip_x + tip_w > (int32_t)w - 4) tip_x = (int32_t)w - 4 - tip_w;
            if (tip_y < PANEL_H + 2) tip_y = PANEL_H + 2;
            fill_round_rect(buf, w, h, stride, tip_x, tip_y, tip_w, tip_h, 5, 0xF0111111u);
            stroke_rect(buf, w, h, stride, tip_x, tip_y, tip_w, tip_h, COL_BORDER);
            /* draw the full title (truncated copy to add NUL at tlen) */
            char tipbuf[44];
            int ti;
            for (ti = 0; ti < tlen && full[ti]; ti++) tipbuf[ti] = full[ti];
            tipbuf[ti] = '\0';
            cz_text(buf, (int)stride, (int)w, (int)h,
                             tip_x + 8, tip_y + 4, tipbuf, COL_TEXT);
        }

        idx++;
    }
}

/* ---------------------------------------------------------------------- *
 *  M6: snap preview overlay. While a drag has armed a snap zone, draw a    *
 *  translucent accent rect over the area the window will occupy on release.*
 *  (snap_target_rect / g_snap_armed are defined earlier in the file.)      *
 * ---------------------------------------------------------------------- */
static void render_snap_preview(uint32_t *buf, uint32_t w, uint32_t h, uint32_t stride) {
    if (g_snap_armed == SNAP_NONE) return;
    int32_t fx, fy; uint32_t cw, ch;
    if (snap_target_rect(g_snap_armed, &fx, &fy, &cw, &ch) != 0) return;
    /* whole-frame rect (titlebar + client + borders), matching a real window */
    int32_t ox = fx - BORDER_W;
    int32_t oy = fy - BORDER_W;
    int32_t fw = (int32_t)cw + 2 * BORDER_W;
    int32_t fh = (int32_t)ch + TITLEBAR_H + 2 * BORDER_W;
    /* translucent accent fill + a brighter 2px border */
    fill_round_rect(buf, w, h, stride, ox, oy, fw, fh, WIN_RADIUS, 0x500A84FFu);
    stroke_rect(buf, w, h, stride, ox, oy, fw, fh, 0xCC0A84FFu);
    stroke_rect(buf, w, h, stride, ox + 1, oy + 1, fw - 2, fh - 2, 0x880A84FFu);
}

/* ---------------------------------------------------------------------- *
 *  M6: notification toast. A single transient top-right message with      *
 *  fade-in / hold / fade-out, all driven by SYS_GET_TICKS_MS.             *
 * ---------------------------------------------------------------------- */
#define TOAST_MAX_LEN   48
#define TOAST_FADE_MS   220
#define TOAST_PAD_X     14
#define TOAST_PAD_Y     10
#define TOAST_MARGIN    12

static char g_toast_text[TOAST_MAX_LEN + 1] = {0};
static long g_toast_start_ms = 0;
static int32_t g_toast_dur_ms = 0;     /* total visible time incl. fades; 0=off */

/* Show a toast for `dur_ms` total (fade-in + hold + fade-out). */
static void toast_show(const char *msg, int32_t dur_ms) {
    int i = 0;
    while (msg[i] && i < TOAST_MAX_LEN) { g_toast_text[i] = msg[i]; i++; }
    g_toast_text[i] = '\0';
    g_toast_start_ms = syscall(SYS_GET_TICKS_MS, 0, 0, 0);
    g_toast_dur_ms   = dur_ms;
    mark_dirty();    /* PERF: a toast appearing is a visible change; anim_tick
                      * then keeps the frame dirty while it fades in/out.        */
}

/* Draw the toast (if active) with its current fade alpha. */
static void render_toast(uint32_t *buf, uint32_t w, uint32_t h, uint32_t stride, long now) {
    if (g_toast_dur_ms <= 0 || g_toast_text[0] == '\0') return;
    long dt = now - g_toast_start_ms;
    if (dt < 0) dt = 0;
    if (dt >= g_toast_dur_ms) { g_toast_dur_ms = 0; g_toast_text[0] = '\0'; return; }

    /* fade-in over the first TOAST_FADE_MS, fade-out over the last TOAST_FADE_MS */
    uint32_t alpha = 256;
    if (dt < TOAST_FADE_MS) {
        alpha = (uint32_t)((dt * 256) / TOAST_FADE_MS);
    } else if (dt > g_toast_dur_ms - TOAST_FADE_MS) {
        long rem = g_toast_dur_ms - dt;
        alpha = (uint32_t)((rem * 256) / TOAST_FADE_MS);
    }
    if (alpha > 256) alpha = 256;
    if (alpha == 0) return;

    int len = (int)k_strlen(g_toast_text);
    int32_t tw = len * FONT_W + 2 * TOAST_PAD_X;
    int32_t th = FONT_H + 2 * TOAST_PAD_Y;
    int32_t tx = (int32_t)w - tw - TOAST_MARGIN;
    /* (M8) clamp tx so a very long toast can't produce a negative x and
     * cause the background rect to be drawn off the left edge of the screen. */
    if (tx < 4) tx = 4;
    int32_t ty = PANEL_H + TOAST_MARGIN;            /* just below the panel */

    /* panel background (rounded) blended at the fade alpha */
    {
        /* approximate alpha-fill by blending COL_PANEL with alpha into a rounded
         * rect: do a rounded shadow then a translucent fill + accent border. */
        blend_rect(buf, w, h, stride, tx + 3, ty + 4, tw, th, 0x40000000u);  /* soft shadow */
        /* rounded translucent body */
        uint32_t body = 0xFF000000u | (COL_PANEL & 0x00FFFFFFu);
        /* manual rounded fill with alpha: reuse fill_round_rect by pre-blending
         * not trivial; instead blend a plain rounded rect span set. */
        int32_t r = 8;
        uint32_t ba = (alpha * 0xE0u) >> 8;          /* body ~88% * fade */
        uint32_t bcol = (ba << 24) | (COL_PANEL & 0x00FFFFFFu);
        (void)body;
        blend_rect(buf, w, h, stride, tx, ty + r, tw, th - 2 * r, bcol);
        blend_rect(buf, w, h, stride, tx + r, ty, tw - 2 * r, r, bcol);
        blend_rect(buf, w, h, stride, tx + r, ty + th - r, tw - 2 * r, r, bcol);
        for (int32_t dy = 0; dy < r; dy++)
            for (int32_t dx = 0; dx < r; dx++) {
                int32_t off = (r - 1 - dx) * (r - 1 - dx) + (r - 1 - dy) * (r - 1 - dy);
                if (off > (r - 1) * (r - 1)) continue;
                blend_rect(buf, w, h, stride, tx + dx, ty + dy, 1, 1, bcol);
                blend_rect(buf, w, h, stride, tx + tw - 1 - dx, ty + dy, 1, 1, bcol);
                blend_rect(buf, w, h, stride, tx + dx, ty + th - 1 - dy, 1, 1, bcol);
                blend_rect(buf, w, h, stride, tx + tw - 1 - dx, ty + th - 1 - dy, 1, 1, bcol);
            }
        /* accent left bar */
        uint32_t acol = (alpha << 24) | (COL_ACCENT & 0x00FFFFFFu);
        blend_rect(buf, w, h, stride, tx, ty + r, 3, th - 2 * r, acol);
    }

    /* text: scale the text color's apparent brightness by alpha (blend toward
     * the body) so it fades together. We approximate by drawing at COL_TEXT --
     * the font has no alpha param, so for partial fades we draw only when the
     * toast is mostly visible to avoid harsh popping. */
    if (alpha >= 64) {
        cz_text(buf, (int)stride, (int)w, (int)h,
                         tx + TOAST_PAD_X, ty + TOAST_PAD_Y, g_toast_text, COL_TEXT);
    }
}

/* forward decl for point_in (defined later in the mouse-interaction section) */
static int point_in(int32_t px, int32_t py, int32_t x, int32_t y, int32_t w, int32_t h);

/* ======================================================================
 * M8: RIGHT DOCK HELPERS
 * ====================================================================== */

/* M8 dock-layout fix: the two FOLDERS (data indices RDOCK_NICONS .. TOTAL-1)
 * must ALWAYS be visible/clickable, otherwise the rainbow fan-out is
 * unreachable. We map every data index to a VISUAL slot so the folders render
 * at the TOP of the strip (visual slots 0..NFOLDERS-1) and the apps render
 * below (visual slots NFOLDERS..). The off-screen cull then only drops
 * overflow APPS at the bottom, never the folders. Only the layout/draw/hit-test
 * ORDER changes; the rdock_apps[]/rdock_folders[] data is untouched and every
 * site keys off rdock_ref_cy(), so draw and hit-test stay in lockstep. */
static int rdock_visual_slot(int idx) {
    if (idx >= RDOCK_NICONS)               /* folder -> top */
        return idx - RDOCK_NICONS;
    /* app -> below all live folders; count only VISIBLE (un-filed) apps before
     * it so the strip closes the gap when an app is filed into a box. */
    int slot = g_nfolders;
    for (int i = 0; i < idx; i++) if (!g_app_hidden[i]) slot++;
    return slot;
}

/* Return the un-magnified Y center of icon slot [idx] in the dock strip.
 * The strip starts at y = PANEL_H + RDOCK_MARGIN_TOP and stacks downward,
 * indexed by the VISUAL slot (folders first). We don't pre-compute because
 * magnification shifts everything dynamically; the reference layout is the
 * settled (no magnification) positions. */
static int32_t rdock_ref_cy(int idx) {
    int slot = rdock_visual_slot(idx);
    return PANEL_H + RDOCK_MARGIN_TOP + slot * (RDOCK_ICON_BASE + RDOCK_PAD)
           + RDOCK_ICON_BASE / 2;
}

/* Compute magnification scale (Q8) for a slot given cursor Y. Uses integer
 * fixed-point only:
 *   dy = |cursor_y - ref_cy|
 *   extra = RDOCK_MAG_MAX_EXTRA * max(0, INFLUENCE - dy) / INFLUENCE
 *   scale = 256 + extra                 (256 = 1.0 in Q8)
 */
static int32_t rdock_mag_scale(int idx, int32_t cursor_y) {
    int32_t cy   = rdock_ref_cy(idx);
    int32_t dy   = cursor_y - cy;
    if (dy < 0) dy = -dy;
    if (dy >= RDOCK_MAG_INFLUENCE) return 256;
    int32_t extra = (int32_t)((long)RDOCK_MAG_MAX_EXTRA * (RDOCK_MAG_INFLUENCE - dy)
                              / RDOCK_MAG_INFLUENCE);
    return 256 + extra;
}

/* Given the magnified scale (Q8) and the base size, return the actual pixel size. */
static int32_t rdock_scaled_sz(int32_t scale_q8) {
    return (int32_t)((long)RDOCK_ICON_BASE * scale_q8 / 256);
}

/* Compute the TOP-Y of icon slot [idx] given current scale array.
 * Stacks magnified icons from RDOCK_MARGIN_TOP below the panel; each icon
 * takes max(base, scaled) height + RDOCK_PAD spacing. We keep icons centered
 * on their reference position: top = ref_cy - sz/2. */
static int32_t rdock_icon_top(int idx) {
    int32_t sz = rdock_scaled_sz(g_rdock_icons[idx].scale_q8);
    return rdock_ref_cy(idx) - sz / 2;
}

/* X of the left edge of the dock strip (icon tiles are centered in the strip).*/
static int32_t rdock_strip_x(uint32_t W) {
    return (int32_t)W - RDOCK_W;
}

/* X of the left edge of an icon tile (centered in the strip). */
static int32_t rdock_icon_x(uint32_t W, int32_t sz) {
    int32_t strip_x = rdock_strip_x(W);
    return strip_x + (RDOCK_W - sz) / 2;
}

/* ---- Bounce offset (horizontal, leftward) for a slot.
 * Uses a decaying sine approximation in fixed-point.
 * bounce_phase in [0,256] maps to one full cycle.
 * Offset = AMP * sin(phase * pi) * decay
 * We approximate sin(t*pi) where t in [0,1] as:
 *   4*t*(1-t)  (a parabola, peaks at 0.5)
 * The result is positive (leftward displacement). */
static int32_t rdock_bounce_offset(int slot, long now) {
    rdock_icon_state_t *st = &g_rdock_icons[slot];
    if (!st->bounce_active) return 0;
    long dt = now - st->bounce_start;
    if (dt < 0) dt = 0;
    if (dt >= RDOCK_BOUNCE_MS) { st->bounce_active = 0; return 0; }
    /* linear phase 0..256 */
    int32_t phase = (int32_t)(dt * 256 / RDOCK_BOUNCE_MS);
    /* two half-bounces: t in [0,128] and [128,256] with decaying amplitude */
    int32_t t, amp;
    if (phase < 128) {
        t = phase * 2;               /* 0..256 for first half */
        amp = RDOCK_BOUNCE_AMP;
    } else {
        t = (phase - 128) * 2;
        amp = RDOCK_BOUNCE_AMP / 2;  /* second bounce is smaller */
    }
    /* sin-approx: 4*t*(256-t)/256^2 * amp */
    int32_t s = (int32_t)((long)4 * t * (256 - t) / 65536);  /* 0..256 */
    return (int32_t)((long)amp * s / 256);
}

/* ---- Update magnification scales each frame ---- */
static void rdock_update_scales(int32_t cursor_x, int32_t cursor_y, uint32_t W) {
    int32_t strip_x = rdock_strip_x(W);
    int on_dock = (cursor_x >= strip_x);
    for (int i = 0; i < rdock_idx_end(); i++) {
        int32_t target;
        if (on_dock) {
            target = rdock_mag_scale(i, cursor_y);
        } else {
            target = 256;  /* no magnification when cursor off dock */
        }
        g_rdock_icons[i].scale_target = target;
        /* smooth toward target */
        int32_t cur = g_rdock_icons[i].scale_q8;
        int32_t diff = target - cur;
        cur += diff - (diff >> RDOCK_SMOOTH_SHIFT);
        /* clamp */
        if (cur < 256) cur = 256;
        if (cur > 256 + RDOCK_MAG_MAX_EXTRA) cur = 256 + RDOCK_MAG_MAX_EXTRA;
        g_rdock_icons[i].scale_q8 = cur;
    }
}

/* ---- Draw a 2x2 mini-grid inside a tile for folder icons ---- */
static void draw_folder_grid(uint32_t *buf, uint32_t bw, uint32_t bh, uint32_t stride,
                             int32_t tx, int32_t ty, int32_t tsz, int fidx) {
    dock_folder_t *f = &g_folders[fidx];
    int32_t inner = tsz / 2 - 4;
    if (inner < 4) inner = 4;
    /* 2x2 sub-tiles */
    static const int32_t gox[4] = {0, 1, 0, 1};
    static const int32_t goy[4] = {0, 0, 1, 1};
    static const uint32_t gcols[4] = {
        0xFF5588BBu, 0xFF88BB55u, 0xFFBB5588u, 0xFF55BBBBu
    };
    for (int m = 0; m < (f->nmembers < 4 ? f->nmembers : 4); m++) {
        int32_t gx = tx + 3 + gox[m] * (inner + 2);
        int32_t gy = ty + 3 + goy[m] * (inner + 2);
        /* member procedural icon (draws its own tile background) */
        const char *mp = folder_member_path(fidx, m);
        if (mp) {
            icon_for_app(buf, (int)stride, gx, gy, inner, mp + 5);  /* skip "sbin/" */
        } else {
            fill_round_rect(buf, bw, bh, stride, gx, gy, inner, inner, 3, gcols[m]);
        }
    }
}

/* ====================================================================== *
 *  M8+: RAINBOW FAN-OUT  (replaces the rectangular folder popover)        *
 *                                                                         *
 *  When a folder is open, its member app icons sweep OUT from the folder  *
 *  tile along a ~160-degree semicircle (a "rainbow") into the workspace.  *
 *  Each icon floats with a gentle vertical bob and twinkles with sparkle  *
 *  particles while the cursor hovers it.                                  *
 *                                                                         *
 *  CRITICAL CLAMP: icon_for_app() clips only against the cell it is told  *
 *  to draw, NOT the screen buffer, so an off-screen cell writes OOB. We   *
 *  compute the arc position then clamp the FULL tile inside the buffer:   *
 *     x in [4, w - RDOCK_W - tile - 4]                                    *
 *     y in [PANEL_H + 4, h - DOCK_H - tile - 4]                           *
 *  Sparkles are drawn with blend_rect (already buffer-clipped) and are    *
 *  additionally guarded so they never start a write off-screen.          *
 * ---------------------------------------------------------------------- */

/* Compute the clamped, on-screen top-left rect of fanned-out member slot [m]
 * of folder [fidx]. anim_t256 is the open progress (0..256). `now` drives the
 * floating bob. Writes the clamped tile origin to *ox/*oy and size to *otile.
 * Returns 1 if the member is valid (and *omi = app index), else 0. */
static int rdock_fan_member_rect(int fidx, int m, int32_t anim_t256, long now,
                                 uint32_t W, uint32_t H,
                                 int32_t *ox, int32_t *oy, int32_t *otile, int *omi) {
    dock_folder_t *f = &g_folders[fidx];
    if (m < 0 || m >= f->nmembers) return 0;
    int mi = f->members[m];
    if (!folder_member_path(fidx, m)) return 0;   /* catalog OR free-path member */

    int32_t tile = RDOCK_FAN_TILE;
    int dock_idx = RDOCK_NICONS + fidx;
    int32_t sz   = rdock_scaled_sz(g_rdock_icons[dock_idx].scale_q8);

    /* anchor the arc on the folder tile center */
    int32_t cx0 = rdock_strip_x(W) + RDOCK_W / 2;
    int32_t cy0 = rdock_icon_top(dock_idx) + sz / 2;

    /* radius grows with eased open progress */
    int32_t prog = ease_out_cubic(clamp256(anim_t256));
    int32_t radius = (int32_t)((long)RDOCK_FAN_RADIUS * prog / 256);

    /* angle: arc centered on 180 deg (straight LEFT into the workspace),
     * spread over RDOCK_FAN_ARC_DEG. For a single member, sit dead-center. */
    int n = f->nmembers;
    int32_t ang;
    if (n <= 1) {
        ang = 180;
    } else {
        int32_t start = 180 - RDOCK_FAN_ARC_DEG / 2;          /* top of rainbow */
        int32_t step  = RDOCK_FAN_ARC_DEG / (n - 1);
        ang = start + m * step;
    }

    /* arc center-of-tile position (cos negative => leftward) */
    int32_t mx = cx0 + (int32_t)((long)cos_q(ang) * radius / 256);
    int32_t my = cy0 + (int32_t)((long)sin_q(ang) * radius / 256);

    /* floating bob: offset = sin(now/8 + m*40) * AMP   (degrees in sin_q) */
    int32_t bob = (int32_t)((long)sin_q((int32_t)(now / 8) + m * 40)
                            * RDOCK_FAN_BOB_AMP / 256);
    my += bob;

    /* convert center -> top-left */
    int32_t x = mx - tile / 2;
    int32_t y = my - tile / 2;

    /* CRITICAL clamp: keep the whole tile inside the back buffer so the
     * unclipped icon library never scribbles past it. */
    int32_t xmin = 4;
    int32_t xmax = (int32_t)W - RDOCK_W - tile - 4;
    int32_t ymin = PANEL_H + 4;
    int32_t ymax = (int32_t)H - DOCK_H - tile - 4;
    if (xmax < xmin) xmax = xmin;     /* degenerate tiny screen: pin to xmin */
    if (ymax < ymin) ymax = ymin;
    if (x < xmin) x = xmin;
    if (x > xmax) x = xmax;
    if (y < ymin) y = ymin;
    if (y > ymax) y = ymax;

    *ox = x; *oy = y; *otile = tile; *omi = mi;
    return 1;
}

/* Draw ~RDOCK_FAN_SPARKLES twinkling sparkle dots orbiting a hovered tile.
 * Cheap: tiny blend_rects (buffer-clipped) at sin/cos-animated offsets with a
 * twinkling alpha. Each sparkle is additionally skipped if it would begin a
 * write off-screen. */
static void rdock_draw_sparkles(uint32_t *buf, uint32_t bw, uint32_t bh, uint32_t stride,
                                int32_t tx, int32_t ty, int32_t tile, long now) {
    int32_t cx = tx + tile / 2;
    int32_t cy = ty + tile / 2;
    int32_t orbit = tile / 2 + 6;
    for (int s = 0; s < RDOCK_FAN_SPARKLES; s++) {
        int32_t phase = s * (360 / RDOCK_FAN_SPARKLES);
        /* orbit angle advances with time; radius pulses a little */
        int32_t ang = (int32_t)(now / 4) + phase;
        int32_t rr  = orbit + (int32_t)((long)sin_q((int32_t)(now / 3) + phase) * 4 / 256);
        int32_t sx = cx + (int32_t)((long)cos_q(ang) * rr / 256);
        int32_t sy = cy + (int32_t)((long)sin_q(ang) * rr / 256);
        /* twinkle alpha 0x40..0xF0 from a sine of time */
        int32_t tw = sin_q((int32_t)(now / 2) + s * 67);   /* -256..256 */
        int32_t a  = 0x90 + tw * 0x60 / 256;               /* 0x30..0xF0 */
        if (a < 0x20) a = 0x20;
        uint32_t col = ((uint32_t)a << 24) | 0x00FFF4C0u;  /* warm white spark */
        /* dot size pulses 1..2 px; only draw fully on-screen */
        int32_t dsz = 1 + ((tw > 0) ? 1 : 0);
        if (sx < 0 || sy < 0 ||
            sx + dsz > (int32_t)bw || sy + dsz > (int32_t)bh) continue;
        blend_rect(buf, bw, bh, stride, sx, sy, dsz, dsz, col);
        /* tiny cross arms for a "star" feel (each still buffer-clipped) */
        blend_rect(buf, bw, bh, stride, sx - 1, sy, 1, 1, (col & 0x00FFFFFFu) | 0x50000000u);
        blend_rect(buf, bw, bh, stride, sx + dsz, sy, 1, 1, (col & 0x00FFFFFFu) | 0x50000000u);
    }
}

/* ---- Draw a folder's rainbow fan-out (open state) ---- */
static void draw_folder_fanout(uint32_t *buf, uint32_t bw, uint32_t bh, uint32_t stride,
                               int fidx, uint32_t W, uint32_t H,
                               int32_t anim_t256, int32_t cur_x, int32_t cur_y, long now) {
    dock_folder_t *f = &g_folders[fidx];
    if (anim_t256 <= 0) return;

    for (int m = 0; m < f->nmembers; m++) {
        int32_t x, y, tile; int mi;
        if (!rdock_fan_member_rect(fidx, m, anim_t256, now, W, H, &x, &y, &tile, &mi))
            continue;

        int hovered = (cur_x >= x && cur_x < x + tile &&
                       cur_y >= y && cur_y < y + tile);

        /* sparkles UNDER the icon while hovered */
        if (hovered) rdock_draw_sparkles(buf, bw, bh, stride, x, y, tile, now);

        /* floating member icon (draws its own tile background, now safely
         * clamped fully inside the buffer). Member may be a catalog app or a
         * Start-menu app dragged in (free path). */
        (void)mi;
        const char *mp = folder_member_path(fidx, m);
        icon_for_app(buf, (int)stride, x, y, tile, (mp ? mp : "sbin/x") + 5);

        /* hover ring to make the focused member pop */
        if (hovered)
            stroke_rect(buf, bw, bh, stride, x - 1, y - 1, tile + 2, tile + 2, COL_ACCENT);
    }
}

/* ---- Main right-dock render function ---- */
static void render_right_dock(uint32_t *buf, uint32_t w, uint32_t h, uint32_t stride,
                               int32_t cur_x, int32_t cur_y, long now) {
    int32_t strip_x = rdock_strip_x(w);

    /* dock strip background */
    fill_rect(buf, w, h, stride, strip_x, PANEL_H, RDOCK_W,
              (int32_t)h - PANEL_H - DOCK_H, 0xE02C2C2Eu);
    /* left border */
    fill_rect(buf, w, h, stride, strip_x, PANEL_H, 1,
              (int32_t)h - PANEL_H - DOCK_H, COL_BORDER);

    g_rdock_hovered = -1;

    for (int i = 0; i < rdock_idx_end(); i++) {
        if (i < RDOCK_NICONS && g_app_hidden[i]) continue;  /* filed into a box */
        if (i == g_dock_drag) continue;                     /* drawn as the ghost */
        int32_t sz   = rdock_scaled_sz(g_rdock_icons[i].scale_q8);
        int32_t ty   = rdock_icon_top(i);
        int32_t tx   = rdock_icon_x(w, sz);
        /* bounce: shift left */
        int32_t boff = rdock_bounce_offset(i, now);
        tx -= boff;

        /* Keep the (possibly hover-magnified) tile fully inside the buffer WIDTH.
         * icon_for_app / ic_fill_rect draw their rows UNCLIPPED, and a real-HW
         * framebuffer whose pitch == width*4 (e.g. the T410: 1024 wide, pitch 4096)
         * has NO stride slack -- so a tile overflowing the RIGHT edge wraps onto the
         * next row's LEFT edge: the "left bleed" (#77). Anchor the magnified tile to
         * the right edge so it grows LEFTWARD (into the screen) instead of off-screen.
         * Clamp BEFORE the hover test so detection matches the drawn position. QEMU
         * hides this only because its VBE reports a padded pitch > width*4. */
        if (tx + sz > (int32_t)w) tx = (int32_t)w - sz;
        if (tx < 0) tx = 0;

        /* Skip any icon whose (possibly hover-magnified) tile is not FULLY
         * inside the drawable strip. icon_for_app / ic_fill_rect write their
         * tile rows UNCLIPPED, so a partially-off-screen tile would scribble
         * past the back buffer and fault the compositor. With more dock items
         * than fit vertically, the overflow ones are simply not drawn here
         * (they remain reachable via the start menu). */
        if (ty < PANEL_H || ty + sz > (int32_t)h - DOCK_H) continue;

        /* hover detection (magnified rect) */
        int hovered = (cur_x >= tx && cur_x < tx + sz &&
                       cur_y >= ty && cur_y < ty + sz);
        if (hovered) g_rdock_hovered = i;

        uint32_t tile_col;
        int is_folder = (i >= RDOCK_NICONS);

        if (is_folder) {
            int fi = i - RDOCK_NICONS;
            tile_col = g_folders[fi].color;
        } else {
            tile_col = rdock_apps[i].color;
        }

        /* hover highlight: lighten the tile */
        if (hovered) {
            uint32_t hr = ((tile_col >> 16) & 0xFF) + 0x30;
            uint32_t hg = ((tile_col >>  8) & 0xFF) + 0x30;
            uint32_t hb = ( tile_col        & 0xFF) + 0x30;
            if (hr > 0xFF) hr = 0xFF;
            if (hg > 0xFF) hg = 0xFF;
            if (hb > 0xFF) hb = 0xFF;
            tile_col = 0xFF000000u | (hr << 16) | (hg << 8) | hb;
        }

        if (is_folder) {
            /* tile background + 2x2 mini-grid */
            fill_round_rect(buf, w, h, stride, tx, ty, sz, sz, RDOCK_CORNER, tile_col);
            draw_folder_grid(buf, w, h, stride, tx, ty, sz, i - RDOCK_NICONS);
        } else {
            /* recognizable procedural icon (draws its own tile background) */
            icon_for_app(buf, (int)stride, tx, ty, sz, rdock_apps[i].path + 5);
        }

        /* DOCK-DND-0: selection ring (toggled with Alt+click) */
        if (i < RDOCK_NICONS && g_dock_selected[i]) {
            stroke_rect(buf, w, h, stride, tx - 2, ty - 2, sz + 4, sz + 4, COL_ACCENT);
            stroke_rect(buf, w, h, stride, tx - 3, ty - 3, sz + 6, sz + 6, COL_ACCENT);
        }

        /* tooltip: show full name to the LEFT when hovered */
        if (hovered && !is_folder) {
            const char *tip = rdock_apps[i].path + 5;  /* skip "sbin/" */
            int tip_len = 0;
            while (tip[tip_len]) tip_len++;
            int32_t tip_w = tip_len * FONT_W + 16;
            int32_t tip_h = FONT_H + 8;
            int32_t tip_x = strip_x - tip_w - 6;
            int32_t tip_y = ty + sz / 2 - tip_h / 2;
            if (tip_x < 4) tip_x = 4;
            fill_round_rect(buf, w, h, stride, tip_x, tip_y, tip_w, tip_h, 5, 0xF0111111u);
            stroke_rect(buf, w, h, stride, tip_x, tip_y, tip_w, tip_h, COL_BORDER);
            cz_text(buf, (int)stride, (int)w, (int)h,
                             tip_x + 8, tip_y + 4, tip, COL_TEXT);
        }
    }

    /* ---- draw open folders as rainbow fan-outs ---- */
    for (int fi = 0; fi < g_nfolders; fi++) {
        rdock_folder_state_t *fs = &g_rdock_folders[fi];
        if (!fs->open && fs->anim_t <= 0) continue;

        /* advance fan open/close animation (anim_t Q8 0..256) */
        long dt = now - fs->anim_start;
        int32_t target_t = fs->anim_closing ? 0 : 256;
        int32_t speed = (int32_t)(dt * 256 / RDOCK_POPOVER_ANIM_MS);
        if (speed > 256) speed = 256;
        if (fs->anim_closing) {
            fs->anim_t = 256 - speed;
            if (fs->anim_t <= 0) { fs->anim_t = 0; fs->open = 0; }
        } else {
            fs->anim_t = speed;
            if (fs->anim_t > 256) fs->anim_t = 256;
        }
        (void)target_t;

        draw_folder_fanout(buf, w, h, stride, fi, w, h, fs->anim_t,
                           cur_x, cur_y, now);
    }

    /* ---- DOCK-DND-0: drop-target ring + the dragged ghost (drawn on top) ---- */
    if (g_dock_drag >= 0 && g_dock_drag < RDOCK_NICONS) {
        if (g_dock_drop_tgt >= 0) {
            int32_t tsz = rdock_scaled_sz(g_rdock_icons[g_dock_drop_tgt].scale_q8);
            int32_t tty = rdock_icon_top(g_dock_drop_tgt);
            int32_t ttx = rdock_icon_x(w, tsz) - rdock_bounce_offset(g_dock_drop_tgt, now);
            if (ttx + tsz > (int32_t)w) ttx = (int32_t)w - tsz;
            if (ttx < 0) ttx = 0;
            /* a glowing rounded ring to say "drop here to make/extend a box" */
            blend_rect(buf, w, h, stride, ttx - 4, tty - 4, tsz + 8, tsz + 8, 0x6047A0FFu);
            stroke_rect(buf, w, h, stride, ttx - 4, tty - 4, tsz + 8, tsz + 8, COL_ACCENT);
        }
        /* the dragged icon, enlarged, following (and centered on) the cursor */
        int32_t gsz = RDOCK_ICON_BASE + 12;
        int32_t gx  = g_dock_drag_x - gsz / 2;
        int32_t gy  = g_dock_drag_y - gsz / 2;
        if (gx + gsz > (int32_t)w) gx = (int32_t)w - gsz;
        if (gy + gsz > (int32_t)h) gy = (int32_t)h - gsz;
        if (gx < 0) gx = 0;
        if (gy < 0) gy = 0;
        blend_rect(buf, w, h, stride, gx + 3, gy + 4, gsz, gsz, 0x50000000u);  /* shadow */
        icon_for_app(buf, (int)stride, gx, gy, gsz, rdock_apps[g_dock_drag].path + 5);
    }

    /* DOCK-DND-1: ghost for a Start-menu app being dragged in (handed off via
     * SHM). Highlight the dock strip as the drop zone. */
    if (g_dnd && g_dnd->magic == DOCKDND_MAGIC && g_dnd->active && !g_dnd->claimed) {
        if (cur_x >= rdock_strip_x(w))
            blend_rect(buf, w, h, stride, rdock_strip_x(w), PANEL_H, RDOCK_W,
                       (int32_t)h - PANEL_H - DOCK_H, 0x4047A0FFu);
        int32_t gsz = RDOCK_ICON_BASE + 12;
        int32_t gx = cur_x - gsz / 2, gy = cur_y - gsz / 2;
        if (gx + gsz > (int32_t)w) gx = (int32_t)w - gsz;
        if (gy + gsz > (int32_t)h) gy = (int32_t)h - gsz;
        if (gx < 0) gx = 0;
        if (gy < 0) gy = 0;
        blend_rect(buf, w, h, stride, gx + 3, gy + 4, gsz, gsz, 0x50000000u);
        char gp[64]; int gi = 0;
        for (; gi < 63 && g_dnd->path[gi]; gi++) gp[gi] = g_dnd->path[gi];
        gp[gi] = 0;
        /* only deref past the "sbin/"/"bin/" prefix for a validated path */
        icon_for_app(buf, (int)stride, gx, gy, gsz, dock_path_ok(gp) ? gp + 5 : "x");
    }
}

/* ---- Hit-test + spawn for the right dock on left-click ---- */
/* Returns 1 if the click was consumed by the right dock, else 0. */
/* Return the dock STRIP icon idx under (cx,cy), or -1. Mirrors the render
 * geometry so hit-test == draw. Skips filed + currently-dragged icons. */
static int rdock_icon_at(int32_t cx, int32_t cy, uint32_t W, long now) {
    if (cx < rdock_strip_x(W)) return -1;
    for (int i = 0; i < rdock_idx_end(); i++) {
        if (i < RDOCK_NICONS && g_app_hidden[i]) continue;
        if (i == g_dock_drag) continue;
        int32_t sz = rdock_scaled_sz(g_rdock_icons[i].scale_q8);
        int32_t ty = rdock_icon_top(i);
        int32_t tx = rdock_icon_x(W, sz) - rdock_bounce_offset(i, now);
        if (point_in(cx, cy, tx, ty, sz, sz)) return i;
    }
    return -1;
}

/* The click action for strip icon [i]: toggle a folder open/closed, or spawn an
 * app. Called on release when the press was NOT promoted to a drag. */
static void dock_activate(int i, long now) {
    if (i >= RDOCK_NICONS) {                       /* folder: toggle the fan-out */
        int fi = i - RDOCK_NICONS;
        rdock_folder_state_t *fs = &g_rdock_folders[fi];
        if (fs->open || (!fs->anim_closing && fs->anim_t > 0)) {
            fs->anim_closing = 1; fs->anim_start = now; g_rdock_open_folder = -1;
        } else {
            if (g_rdock_open_folder >= 0 && g_rdock_open_folder != fi) {
                rdock_folder_state_t *ofs = &g_rdock_folders[g_rdock_open_folder];
                ofs->anim_closing = 1; ofs->anim_start = now;
            }
            fs->open = 1; fs->anim_closing = 0; fs->anim_start = now; fs->anim_t = 0;
            g_rdock_open_folder = fi;
        }
    } else {
        syscall(SYS_SPAWN, (long)rdock_apps[i].path, 0, 0);
    }
    g_rdock_icons[i].bounce_active = 1;
    g_rdock_icons[i].bounce_start  = now;
}

/* File app[app] (plus any selected apps) into existing folder [fi]. */
static void dock_file_into_folder(int app, int fi, long now) {
    dock_folder_t *f = &g_folders[fi];
    int add[DOCK_MEMBERS_MAX], na = 0;
    add[na++] = app;
    for (int i = 0; i < RDOCK_NICONS; i++)
        if (g_dock_selected[i] && i != app && !g_app_hidden[i] && na < DOCK_MEMBERS_MAX)
            add[na++] = i;
    for (int k = 0; k < na && f->nmembers < DOCK_MEMBERS_MAX; k++) {
        int dup = 0;
        for (int m = 0; m < f->nmembers; m++) if (f->members[m] == add[k]) dup = 1;
        if (dup) continue;
        f->members[f->nmembers++] = add[k];
        g_app_hidden[add[k]]   = 1;
        g_dock_selected[add[k]] = 0;
    }
    int idx = RDOCK_NICONS + fi;
    g_rdock_icons[idx].bounce_active = 1;
    g_rdock_icons[idx].bounce_start  = now;
    g_dock_drop_anim = now;
}

/* Combine app[a] + app[b] (plus any selected apps) into a NEW box/folder. */
static void dock_combine_into_box(int a, int b, long now) {
    if (g_nfolders >= DOCK_FOLDERS_MAX) { dock_file_into_folder(a, 0, now); return; }
    int fi = g_nfolders;
    dock_folder_t *f = &g_folders[fi];
    f->label[0] = 'B'; f->label[1] = 'x'; f->label[2] = 0; f->label[3] = 0;
    f->color    = 0xFF3A4A6Cu;
    f->nmembers = 0;
    int add[DOCK_MEMBERS_MAX], na = 0;
    add[na++] = b;
    add[na++] = a;
    for (int i = 0; i < RDOCK_NICONS; i++)
        if (g_dock_selected[i] && i != a && i != b && !g_app_hidden[i] && na < DOCK_MEMBERS_MAX)
            add[na++] = i;
    for (int k = 0; k < na && f->nmembers < DOCK_MEMBERS_MAX; k++) {
        f->members[f->nmembers++] = add[k];
        g_app_hidden[add[k]]   = 1;
        g_dock_selected[add[k]] = 0;
    }
    int idx = RDOCK_NICONS + fi;
    g_rdock_icons[idx].scale_q8 = 256; g_rdock_icons[idx].scale_target = 256;
    g_rdock_icons[idx].bounce_active = 1; g_rdock_icons[idx].bounce_start = now;
    g_rdock_folders[fi].open = 0; g_rdock_folders[fi].anim_t = 0;
    g_rdock_folders[fi].anim_closing = 0; g_rdock_folders[fi].anim_start = 0;
    g_nfolders++;
    g_dock_drop_anim = now;
}

/* Resolve a drag drop: onto a folder = file it; onto another app = make a box;
 * elsewhere = snap back (no-op). */
static void dock_drop(int drag, int tgt, uint32_t W, long now) {
    (void)W;
    if (drag < 0 || drag >= RDOCK_NICONS) return;   /* only apps are draggable   */
    if (tgt < 0 || tgt == drag) return;             /* dropped on nothing        */
    if (tgt >= RDOCK_NICONS) dock_file_into_folder(drag, tgt - RDOCK_NICONS, now);
    else                     dock_combine_into_box(drag, tgt, now);
}

/* Accept only "sbin/<name>" / "bin/<name>" with no traversal or non-printables,
 * so a write to the (attacker-influenceable) handoff SHM cannot make the dock
 * spawn an arbitrary binary. Requires NUL-termination within 64 bytes. */
static int dock_path_ok(const char *p) {
    if (!p) return 0;
    int sbin = (p[0]=='s' && p[1]=='b' && p[2]=='i' && p[3]=='n' && p[4]=='/');
    int bin  = (p[0]=='b' && p[1]=='i' && p[2]=='n' && p[3]=='/');
    if (!sbin && !bin) return 0;
    int n = 0;
    for (; p[n]; n++) {
        if (n >= 63) return 0;                        /* must NUL-terminate < 64 */
        if (p[n] < 0x20 || p[n] > 0x7e) return 0;     /* printable ASCII only    */
        if (p[n] == '.' && p[n + 1] == '.') return 0; /* no path traversal       */
        if (p[n] == '/' && p[n + 1] == '/') return 0;
    }
    return (n >= 5);
}

/* DOCK-DND-1: pin a Start-menu app (free path) into the dock's "Pinned" box,
 * creating the box on first use. Dedups by path; animated bounce on add. The
 * path is validated (allowlist) because it originates from the handoff SHM. */
static void dock_pin_path(const char *path, uint32_t color, long now) {
    if (!dock_path_ok(path)) { print("[DOCK] reject pin (bad path)\n"); return; }
    int fi;
    if (g_pinned_folder >= 0 && g_pinned_folder < g_nfolders) {
        fi = g_pinned_folder;
    } else {
        if (g_nfolders >= DOCK_FOLDERS_MAX) return;
        fi = g_nfolders++;
        g_pinned_folder = fi;
        dock_folder_t *nf = &g_folders[fi];
        nf->label[0] = 'P'; nf->label[1] = 'n'; nf->label[2] = 0; nf->label[3] = 0;
        nf->color    = color ? color : 0xFF2E6B4Fu;
        nf->nmembers = 0;
        int idx = RDOCK_NICONS + fi;
        g_rdock_icons[idx].scale_q8 = 256; g_rdock_icons[idx].scale_target = 256;
        g_rdock_folders[fi].open = 0; g_rdock_folders[fi].anim_t = 0;
        g_rdock_folders[fi].anim_closing = 0; g_rdock_folders[fi].anim_start = 0;
    }
    dock_folder_t *f = &g_folders[fi];
    for (int k = 0; k < f->nmembers; k++) {           /* dedup by path */
        const char *p = folder_member_path(fi, k);
        int same = (p != 0);
        for (int j = 0; same && (path[j] || p[j]); j++) if (path[j] != p[j]) same = 0;
        if (same) {
            g_rdock_icons[RDOCK_NICONS + fi].bounce_active = 1;
            g_rdock_icons[RDOCK_NICONS + fi].bounce_start  = now;
            return;
        }
    }
    if (f->nmembers >= DOCK_MEMBERS_MAX) return;
    int m = f->nmembers;
    f->members[m] = -1;
    int i = 0; while (path[i] && i < 63) { f->mpath[m][i] = path[i]; i++; } f->mpath[m][i] = 0;
    f->nmembers++;
    g_rdock_icons[RDOCK_NICONS + fi].bounce_active = 1;
    g_rdock_icons[RDOCK_NICONS + fi].bounce_start  = now;
    g_dock_drop_anim = now;
    print("[DOCK] pinned "); print(path); print(" -> box\n");
}

static int rdock_handle_click(int32_t cx, int32_t cy, uint32_t W, long now) {
    int32_t strip_x = rdock_strip_x(W);

    /* Click outside the dock strip entirely? The rainbow fan-out lives out in
     * the workspace, so member icons are hit-tested here. */
    if (cx < strip_x) {
        int any_open = 0;

        /* 1) Did the click land on a fanned-out member icon? Use the IDENTICAL
         * clamped geometry as the renderer so hit-test == what is drawn. */
        for (int fi = 0; fi < g_nfolders; fi++) {
            rdock_folder_state_t *fs = &g_rdock_folders[fi];
            if (!fs->open && fs->anim_t <= 0) continue;
            any_open = 1;
            dock_folder_t *f = &g_folders[fi];
            for (int m = 0; m < f->nmembers; m++) {
                int32_t x, y, tile; int mi;
                if (!rdock_fan_member_rect(fi, m, fs->anim_t, now,
                                           g_fb_w, g_fb_h, &x, &y, &tile, &mi))
                    continue;
                if (point_in(cx, cy, x, y, tile, tile)) {
                    (void)mi;
                    const char *mp = folder_member_path(fi, m);
                    if (mp) {
                        print("[SHELL] dock folder spawn: "); print(mp); print("\n");
                        syscall(SYS_SPAWN, (long)mp, 0, 0);
                    }
                    /* bounce the folder tile to acknowledge the launch */
                    g_rdock_icons[RDOCK_NICONS + fi].bounce_active = 1;
                    g_rdock_icons[RDOCK_NICONS + fi].bounce_start  = now;
                    return 1;
                }
            }
        }

        /* 2) Click elsewhere while a fan is open: close it (reverse anim). */
        if (any_open) {
            for (int fi = 0; fi < g_nfolders; fi++) {
                rdock_folder_state_t *fs = &g_rdock_folders[fi];
                if (fs->open || fs->anim_t > 0) {
                    fs->anim_closing = 1;
                    fs->anim_start   = now;
                }
            }
            g_rdock_open_folder = -1;
            return 1;   /* consume: dismissed the fan rather than hitting desktop */
        }
        return 0;
    }

    /* Click on the dock strip: ARM a press on the hit icon. The actual action
     * (launch / folder toggle, an Alt+click select, or a drag-to-combine) is
     * resolved on RELEASE in handle_mouse's dock-drag block. */
    for (int i = 0; i < rdock_idx_end(); i++) {
        if (i < RDOCK_NICONS && g_app_hidden[i]) continue;  /* filed into a box */
        int32_t sz   = rdock_scaled_sz(g_rdock_icons[i].scale_q8);
        int32_t ty   = rdock_icon_top(i);
        int32_t tx   = rdock_icon_x(W, sz) - rdock_bounce_offset(i, now);
        if (!point_in(cx, cy, tx, ty, sz, sz)) continue;
        g_dock_press_idx = i;
        g_dock_press_x   = cx;
        g_dock_press_y   = cy;
        g_dock_drag      = -1;
        return 1;
    }
    return 1;  /* consumed: any click on the strip is dock territory */
}

/* Render the popup menu (start / context) on top of the scene, under cursor. */
static void draw_menu(uint32_t *buf, uint32_t w, uint32_t h, uint32_t stride) {
    if (!g_menu_open) return;
    int32_t mh = menu_height();
    blend_rect(buf, w, h, stride, g_menu_x + 5, g_menu_y + 6, MENU_W, mh, 0x60000000u); /* shadow */
    fill_round_rect(buf, w, h, stride, g_menu_x, g_menu_y, MENU_W, mh, 8, 0xFF1C2230u); /* panel */
    const char *title = g_menu_is_ctx ? "Actions" : "AutomationOS";
    cz_text(buf, (int)stride, (int)w, (int)h,
                     g_menu_x + 12, g_menu_y + (MENU_HDR_H - FONT_H) / 2, title, COL_ACCENT);
    blend_rect(buf, w, h, stride, g_menu_x + 8, g_menu_y + MENU_HDR_H - 1,
               MENU_W - 16, 1, 0x40FFFFFFu); /* divider */
    for (int i = 0; i < g_menu_n; i++) {
        int32_t ry = g_menu_y + MENU_HDR_H + i * MENU_ROW_H;
        int hov = (i == g_menu_hover);
        if (hov)
            fill_round_rect(buf, w, h, stride, g_menu_x + 4, ry + 1,
                            MENU_W - 8, MENU_ROW_H - 2, 4, COL_ACCENT);
        /* hovered row gets white text on the accent fill for proper contrast */
        cz_text(buf, (int)stride, (int)w, (int)h,
                         g_menu_x + 14, ry + (MENU_ROW_H - FONT_H) / 2,
                         g_menu_label[i], hov ? 0xFFFFFFFFu : COL_TEXT);
    }
}

static int about_slen(const char *s) { int n = 0; while (s[n]) n++; return n; }
static void about_cat(char *dst, int *pos, int cap, const char *s) {
    while (*s && *pos < cap - 1) dst[(*pos)++] = *s++;
    dst[*pos] = '\0';
}
static void about_catu(char *dst, int *pos, int cap, unsigned long long v) {
    char tmp[20]; int n = 0;
    do { tmp[n++] = (char)('0' + (int)(v % 10)); v /= 10; } while (v > 0 && n < 20);
    while (n > 0 && *pos < cap - 1) dst[(*pos)++] = tmp[--n];
    dst[*pos] = '\0';
}
static void about_get_cpu_brand(char *brand) {
    uint32_t regs[12];
    for (int i = 0; i < 3; i++) {
        uint32_t leaf = 0x80000002u + (uint32_t)i;
        uint32_t a, b, c, d;
        asm volatile("cpuid" : "=a"(a),"=b"(b),"=c"(c),"=d"(d) : "a"(leaf),"c"(0));
        regs[i*4+0]=a; regs[i*4+1]=b; regs[i*4+2]=c; regs[i*4+3]=d;
    }
    const char *raw = (const char *)regs;
    int start = 0;
    while (start < 48 && raw[start] == ' ') start++;
    int j = 0;
    while (start < 48 && raw[start] && j < 48) brand[j++] = raw[start++];
    brand[j] = '\0';
}

/* Modal "About" panel: a centered card with live system information.
 * Dismissed by any click (handled in handle_mouse). */
static void draw_about(uint32_t *buf, uint32_t w, uint32_t h, uint32_t stride) {
    if (!g_about_open) return;

    blend_rect(buf, w, h, stride, 0, 0, (int32_t)w, (int32_t)h, 0x88000000u);

    /* -- gather system info -- */
    char cpu_brand[49]; about_get_cpu_brand(cpu_brand);
    comp_sysinfo_t si;
    for (unsigned i = 0; i < sizeof(si); i++) ((uint8_t *)&si)[i] = 0;
    long si_ok = syscall(SYS_SYSINFO, (long)&si, 0, 0);

    enum { MAXL = 100, NLINES = 10 };
    char lines[NLINES][MAXL];
    uint32_t colors[NLINES];
    int nlines = 0;
    int p;

    p = 0; about_cat(lines[nlines], &p, MAXL, "AutomationOS v0.1.0");
    colors[nlines] = COL_ACCENT; nlines++;

    p = 0; about_cat(lines[nlines], &p, MAXL, "from-scratch x86_64 operating system");
    colors[nlines] = COL_TEXT_DIM; nlines++;

    p = 0; about_cat(lines[nlines], &p, MAXL, "CPU:  ");
    about_cat(lines[nlines], &p, MAXL, cpu_brand[0] ? cpu_brand : "Unknown");
    colors[nlines] = COL_TEXT; nlines++;

    p = 0; about_cat(lines[nlines], &p, MAXL, "RAM:  ");
    if (si_ok == 0) {
        unsigned long long used_mb = (si.total_mem - si.free_mem) / (1024*1024);
        unsigned long long total_mb = si.total_mem / (1024*1024);
        about_catu(lines[nlines], &p, MAXL, used_mb);
        about_cat(lines[nlines], &p, MAXL, " / ");
        about_catu(lines[nlines], &p, MAXL, total_mb);
        about_cat(lines[nlines], &p, MAXL, " MB used");
    } else {
        about_cat(lines[nlines], &p, MAXL, "unavailable");
    }
    colors[nlines] = COL_TEXT; nlines++;

    p = 0; about_cat(lines[nlines], &p, MAXL, "Display:  ");
    about_catu(lines[nlines], &p, MAXL, g_fb_w);
    about_cat(lines[nlines], &p, MAXL, " x ");
    about_catu(lines[nlines], &p, MAXL, g_fb_h);
    colors[nlines] = COL_TEXT; nlines++;

    p = 0; about_cat(lines[nlines], &p, MAXL, "Network:  ");
    about_cat(lines[nlines], &p, MAXL, g_net_up ? g_net_label : "No network");
    colors[nlines] = COL_TEXT; nlines++;

    p = 0; about_cat(lines[nlines], &p, MAXL, "Uptime:  ");
    if (si_ok == 0) {
        unsigned long long secs = si.uptime_ms / 1000;
        unsigned long long days = secs / 86400;
        unsigned long long hrs  = (secs % 86400) / 3600;
        unsigned long long mins = (secs % 3600) / 60;
        unsigned long long ss   = secs % 60;
        if (days > 0) { about_catu(lines[nlines], &p, MAXL, days); about_cat(lines[nlines], &p, MAXL, "d "); }
        about_catu(lines[nlines], &p, MAXL, hrs);  about_cat(lines[nlines], &p, MAXL, "h ");
        about_catu(lines[nlines], &p, MAXL, mins); about_cat(lines[nlines], &p, MAXL, "m ");
        about_catu(lines[nlines], &p, MAXL, ss);   about_cat(lines[nlines], &p, MAXL, "s");
    } else {
        about_cat(lines[nlines], &p, MAXL, "unavailable");
    }
    colors[nlines] = COL_TEXT; nlines++;

    p = 0; about_cat(lines[nlines], &p, MAXL, "Built:  ");
    about_cat(lines[nlines], &p, MAXL, __DATE__);
    about_cat(lines[nlines], &p, MAXL, "  ");
    about_cat(lines[nlines], &p, MAXL, __TIME__);
    colors[nlines] = COL_TEXT_DIM; nlines++;

    p = 0; about_cat(lines[nlines], &p, MAXL, "created by fourzerofour & claude");
    colors[nlines] = COL_TEXT; nlines++;

    p = 0; about_cat(lines[nlines], &p, MAXL, "(click anywhere to close)");
    colors[nlines] = COL_TEXT_DIM; nlines++;

    /* -- layout -- */
    int header_h = 44;
    int line_h   = FONT_H + 4;
    int body_top = header_h + 12;
    int body_h   = (nlines - 1) * line_h + 8;
    int32_t ph   = body_top + body_h;
    int maxlen = 0;
    for (int i = 0; i < nlines; i++) { int len = about_slen(lines[i]); if (len > maxlen) maxlen = len; }
    int32_t pw = maxlen * FONT_W + 48;
    if (pw < 440) pw = 440;
    int32_t px = ((int32_t)w - pw) / 2;
    int32_t py = ((int32_t)h - ph) / 2;

    blend_rect(buf, w, h, stride, px + 6, py + 10, pw, ph, 0x60000000u);
    fill_round_rect(buf, w, h, stride, px, py, pw, ph, 12, 0xFF1C2230u);
    fill_round_rect(buf, w, h, stride, px, py, pw, header_h, 12, 0xFF2A3552u);

    int32_t tx0 = px + (pw - about_slen(lines[0]) * FONT_W) / 2;
    cz_text(buf, (int)stride, (int)w, (int)h, tx0, py + (header_h - FONT_H) / 2, lines[0], colors[0]);

    int32_t lx = px + 24;
    for (int i = 1; i < nlines; i++) {
        int32_t ly = py + body_top + (i - 1) * line_h;
        int32_t x = lx;
        if (i >= nlines - 2)
            x = px + (pw - about_slen(lines[i]) * FONT_W) / 2;
        cz_text(buf, (int)stride, (int)w, (int)h, x, ly, lines[i], colors[i]);
    }
}

/* ====================================================================== *
 *  PERF: on-screen stats overlay (the "measure first" piece)              *
 * ---------------------------------------------------------------------- *
 *  A tiny top-left box drawn each presented frame showing live FPS, frame *
 *  time (ms), composited window count, and pixels pushed to the FB by the *
 *  last present_diff. This is what lets the owner watch "1 app vs 5 apps"  *
 *  on the T410 and see where the time goes. Kept cheap: a handful of small *
 *  fill_rects + ~4 short strings, so measuring never dominates the frame.  *
 *  Toggle at runtime with Alt+S (see wm_handle_key); default-on via        *
 *  COMPOSITOR_STATS.                                                       *
 * ---------------------------------------------------------------------- */

/* Append unsigned `v` (base 10) to dst[] at *pos, bounded by cap. */
static void stat_putu(char *dst, int *pos, int cap, long v) {
    char tmp[16]; int n = 0;
    if (v < 0) v = 0;
    do { tmp[n++] = (char)('0' + (v % 10)); v /= 10; } while (v > 0 && n < 16);
    while (n > 0 && *pos < cap - 1) dst[(*pos)++] = tmp[--n];
}
/* Append a literal string. */
static void stat_puts(char *dst, int *pos, int cap, const char *s) {
    while (*s && *pos < cap - 1) dst[(*pos)++] = *s++;
}

/* Count windows that composite() would actually blit this frame (live, not
 * minimized-and-parked). Mirrors the visibility test in the window loop so the
 * overlay's "win" number matches the real per-frame compositing cost. */
static int stats_visible_windows(void) {
    int n = 0;
    for (int32_t i = 0; i < g_zcount; i++) {
        int slot = (int)g_zorder[i];
        if (slot < 0 || slot >= MAX_WINDOWS || !g_windows[slot].used) continue;
        window_t *win = &g_windows[slot];
        if (win->phase == PH_NONE && win->minimized) continue;  /* parked: skipped */
        n++;
    }
    return n;
}

/* Draw the overlay into the back buffer (so present_diff picks it up). Reads the
 * sampled g_fps_x10 / g_frame_dt_ms / g_present_px globals (updated once per
 * presented frame in the frame loop). */
static void render_stats_overlay(uint32_t *buf, uint32_t w, uint32_t h, uint32_t stride) {
    if (!g_stats_on) return;

    /* Build two short lines so the box stays small.
     *   line1: "FPS 59.4  3.2ms"
     *   line2: "win 5  px 18240"                                            */
    char l1[40]; int p1 = 0;
    stat_puts(l1, &p1, (int)sizeof(l1), "FPS ");
    stat_putu(l1, &p1, (int)sizeof(l1), g_fps_x10 / 10);
    stat_puts(l1, &p1, (int)sizeof(l1), ".");
    stat_putu(l1, &p1, (int)sizeof(l1), g_fps_x10 % 10);
    stat_puts(l1, &p1, (int)sizeof(l1), "  ");
    stat_putu(l1, &p1, (int)sizeof(l1), g_frame_dt_ms);
    stat_puts(l1, &p1, (int)sizeof(l1), "ms");
    l1[p1] = '\0';

    char l2[40]; int p2 = 0;
    stat_puts(l2, &p2, (int)sizeof(l2), "win ");
    stat_putu(l2, &p2, (int)sizeof(l2), stats_visible_windows());
    stat_puts(l2, &p2, (int)sizeof(l2), "  px ");
    stat_putu(l2, &p2, (int)sizeof(l2), (long)g_present_px);
    l2[p2] = '\0';

    /* line3: the CPU split. The compositor runs ENTIRELY on CPU0; CPU1 does NO
     * rendering yet (it's the brick-3.5 heartbeat / soon the brick-5 managed
     * idle), so it reads "idle". This is the line that shows the second core is
     * not helping render until bricks 6-8 put worker jobs on it. A real per-CPU
     * load % is a Batch-3 worker-stats item (once CPU1 actually runs jobs). */
    char l3[40]; int p3 = 0;
    stat_puts(l3, &p3, (int)sizeof(l3), "CPU0 render  CPU1 idle");
    l3[p3] = '\0';

    /* Box geometry: top-left, just below the panel so it never fights the panel
     * title. Width sized to the longest line. */
    int len = p1 > p2 ? p1 : p2; if (p3 > len) len = p3;
    int32_t bx = 8;
    int32_t by = PANEL_H + 6;
    int32_t bw = len * FONT_W + 12;
    int32_t bh = 3 * FONT_H + 10;

    /* translucent dark plate + thin accent border (cheap, readable) */
    blend_rect(buf, w, h, stride, bx, by, bw, bh, 0xC0101418u);
    stroke_rect(buf, w, h, stride, bx, by, bw, bh, 0x800A84FFu);

    cz_text(buf, (int)stride, (int)w, (int)h,
                     bx + 6, by + 5, l1, 0xFF6FE26Fu);            /* green-ish */
    cz_text(buf, (int)stride, (int)w, (int)h,
                     bx + 6, by + 5 + FONT_H, l2, 0xFFE2E27Au);   /* amber-ish */
    cz_text(buf, (int)stride, (int)w, (int)h,
                     bx + 6, by + 5 + 2 * FONT_H, l3, 0xFF7AC8FFu); /* blue-ish */
}

/* Alt+Tab selection overlay (body defined after the Alt+Tab state in the WM
 * section; forward-declared here so composite() can call it). */
static void render_alttab_overlay(uint32_t *buf, uint32_t w, uint32_t h, uint32_t stride);

/* ====================================================================== */
static void composite(uint32_t *buf, uint32_t w, uint32_t h, uint32_t stride,
                      int32_t cursor_x, int32_t cursor_y, long now) {
    render_desktop(buf, w, h, stride);

    /* M8: /Desktop contents as labeled icons (beneath windows + chrome) */
    render_desktop_icons(buf, w, h, stride, cursor_x, cursor_y);

    /* windows, back to front */
    int top = focused_slot();
    for (int32_t i = 0; i < g_zcount; i++) {
        int slot = (int)g_zorder[i];
        if (slot < 0 || slot >= MAX_WINDOWS || !g_windows[slot].used) continue;
        window_t *win = &g_windows[slot];
        if (win->phase == PH_SNAPPING) {
            render_window_snapping(buf, w, h, stride, win, slot == top, now);
        } else if (win->phase != PH_NONE) {
            render_window_anim(buf, w, h, stride, win, slot == top, now);
        } else if (win->minimized) {
            continue;                       /* parked: only its taskbar button shows */
        } else {
            render_window_static(buf, w, h, stride, win, slot == top);
        }
    }

    /* M6: snap preview over windows (below chrome) */
    render_snap_preview(buf, w, h, stride);

    /* Alt+Tab selection highlight over the windows (below chrome) */
    render_alttab_overlay(buf, w, h, stride);

    /* always-on-top chrome (drawn AFTER windows) */
    render_panel(buf, w, h, stride, now);
    render_dock(buf, w, h, stride, cursor_x, cursor_y);

    /* M8: right vertical dock (drawn after bottom dock so it paints over the
     * bottom-right corner where they meet) */
    rdock_update_scales(cursor_x, cursor_y, w);
    render_right_dock(buf, w, h, stride, cursor_x, cursor_y, now);

    /* M6: notification toast above the chrome */
    render_toast(buf, w, h, stride, now);

    /* popup menu (start / context) above the chrome, beneath the cursor */
    draw_menu(buf, w, h, stride);

    /* modal About dialog above everything except the cursor */
    draw_about(buf, w, h, stride);

    /* PERF: stats overlay above the chrome, beneath the cursor (so the cursor is
     * never occluded). Guard the CALL on g_stats_on so the default (overlay off)
     * pays zero function-call / string-format cost every composite. */
    if (g_stats_on) render_stats_overlay(buf, w, h, stride);

    /* NOTE: the cursor is NOT drawn here. `back` is the cursor-less SCENE so the
     * frame loop can move the cursor (overlay it on the framebuffer at present
     * time) WITHOUT recompositing the whole scene -- the smooth-mouse fast path.
     * present_diff(back, prev) detects any real scene change (incl. hover FX);
     * the cursor is overlaid separately by present_cursor(). */
    (void)cursor_x; (void)cursor_y;
}

static void present(uint32_t *fb, uint32_t *back, uint32_t h, uint32_t stride) {
    /* 64-bit bulk copy: halves store count to the (likely UC) framebuffer. */
    uint32_t total  = h * stride;
    uint32_t pairs  = total >> 1;
    uint32_t tail   = total & 1u;
    uint64_t *d64 = (uint64_t *)fb;
    uint64_t *s64 = (uint64_t *)back;
    for (uint32_t i = 0; i < pairs; i++)
        d64[i] = s64[i];
    if (tail)
        fb[total - 1] = back[total - 1];
}

/* Dirty-rectangle present: copy only the bounding box of pixels that changed
 * since the previous frame (back vs prev) into the hardware framebuffer, then
 * update prev. The back-vs-prev scan is normal CACHED RAM (fast); only the
 * changed rectangle is written to the SLOW linear framebuffer. On a static or
 * lightly-animated desktop this turns a full ~3MB MMIO write per frame into a
 * tiny one -- the dominant cost on a slow framebuffer like the ThinkPad T410's.
 * Worst case (everything changed) it degrades to a full copy == present().
 * `w` is the visible width (<= stride); columns in [w,stride) are padding and
 * are left untouched (they're never displayed). */
static uint32_t present_diff(uint32_t *fb, uint32_t *back, uint32_t *prev,
                             uint32_t w, uint32_t h, uint32_t stride) {
    /* PHASE 1: scan back vs prev (both CACHED RAM -- fast) to find the
     * dirty bounding box.  This is identical to the old scan. */
    uint32_t minx = w, miny = h, maxx = 0, maxy = 0;
    int any = 0;
    for (uint32_t y = 0; y < h; y++) {
        uint32_t off = y * stride;
        for (uint32_t x = 0; x < w; x++) {
            if (back[off + x] != prev[off + x]) {
                if (x < minx) minx = x;
                if (x > maxx) maxx = x;
                if (y < miny) miny = y;
                if (y > maxy) maxy = y;
                any = 1;
            }
        }
    }
    if (!any) return 0;               /* nothing changed: skip the fb write    */

    /* PHASE 2: per-row SPAN copy within the bounding box.
     *
     * Instead of writing every pixel in the bbox to the SLOW framebuffer,
     * scan each row to find the leftmost and rightmost actually-changed pixels,
     * then copy that contiguous span with a single memcpy-width burst.  On UC
     * memory each store is a full PCIe transaction; on WC the CPU coalesces
     * the burst.  Either way, copying only the truly-dirty span per row (and
     * skipping unchanged rows entirely) is a big win over the old full-bbox
     * rectangle copy.
     *
     * The prev buffer is updated by an inner 64-bit loop (both src and dst are
     * cached RAM -> trivially fast). */
    uint32_t total_px = 0;
    for (uint32_t y = miny; y <= maxy; y++) {
        uint32_t off = y * stride;
        /* Find first and last changed pixel on this row within the bbox. */
        uint32_t rx0 = maxx + 1, rx1 = minx;
        for (uint32_t x = minx; x <= maxx; x++) {
            if (back[off + x] != prev[off + x]) {
                if (x < rx0) rx0 = x;
                rx1 = x;
            }
        }
        if (rx0 > rx1) continue;   /* this row is clean within the bbox */
        uint32_t span = rx1 - rx0 + 1;
        total_px += span;

        /* Copy span to framebuffer: use 64-bit stores for 2 pixels/write.
         * On UC memory this halves the PCIe transaction count. */
        uint32_t *fb_row   = &fb[off + rx0];
        uint32_t *back_row = &back[off + rx0];
        uint32_t *prev_row = &prev[off + rx0];
        uint32_t pairs = span >> 1;
        uint32_t tail  = span & 1u;
        uint64_t *d64 = (uint64_t *)fb_row;
        uint64_t *s64 = (uint64_t *)back_row;
        for (uint32_t i = 0; i < pairs; i++)
            d64[i] = s64[i];
        if (tail)
            fb_row[span - 1] = back_row[span - 1];

        /* Update prev (cached RAM -> fast byte-width is fine, but we use
         * 64-bit for consistency). */
        uint64_t *p64 = (uint64_t *)prev_row;
        for (uint32_t i = 0; i < pairs; i++)
            p64[i] = s64[i];
        if (tail)
            prev_row[span - 1] = back_row[span - 1];
    }
    return total_px;
}

/* SMOOTH-MOUSE FAST PATH state: where the cursor sprite is currently painted on
 * the framebuffer (so we can erase it from there), and whether the pointer moved
 * this frame. `back` is the cursor-less scene; `prev` mirrors it. */
static int      g_cursor_moved = 0;
static int32_t  g_cur_drawn_x  = -1000, g_cur_drawn_y = -1000;

/* Copy the scene rect [x,x+w) x [y,y+h) from `back` to BOTH the framebuffer and
 * `prev` (prev tracks the cursor-less scene, so present_diff stays coherent).
 * Clipped to the screen. Used to erase the old cursor / paint the new one's
 * background before the sprite goes on top. Returns pixels written. */
static uint32_t blit_back_rect(uint32_t *fb, uint32_t *back, uint32_t *prev,
                               int32_t x, int32_t y, int32_t w, int32_t h,
                               uint32_t scr_w, uint32_t scr_h, uint32_t stride) {
    int32_t x1 = x < 0 ? 0 : x, y1 = y < 0 ? 0 : y;
    int32_t x2 = x + w, y2 = y + h;
    if (x2 > (int32_t)scr_w) x2 = (int32_t)scr_w;
    if (y2 > (int32_t)scr_h) y2 = (int32_t)scr_h;
    if (x1 >= x2 || y1 >= y2) return 0;
    for (int32_t yy = y1; yy < y2; yy++) {
        uint32_t off = (uint32_t)yy * stride + (uint32_t)x1;
        uint32_t span = (uint32_t)(x2 - x1);

        /* 64-bit bulk copy to framebuffer (halves PCIe transactions on UC/WC). */
        uint64_t *d64 = (uint64_t *)&fb[off];
        uint64_t *s64 = (uint64_t *)&back[off];
        uint32_t pairs = span >> 1;
        uint32_t tail  = span & 1u;
        for (uint32_t i = 0; i < pairs; i++)
            d64[i] = s64[i];
        if (tail)
            fb[off + span - 1] = back[off + span - 1];

        /* Update prev (cached RAM). */
        if (prev) {
            uint64_t *p64 = (uint64_t *)&prev[off];
            for (uint32_t i = 0; i < pairs; i++)
                p64[i] = s64[i];
            if (tail)
                prev[off + span - 1] = back[off + span - 1];
        }
    }
    return (uint32_t)((x2 - x1) * (y2 - y1));
}

/* Move the cursor sprite from (ox,oy) to (nx,ny) on the framebuffer WITHOUT
 * recompositing the scene: restore the scene under the OLD sprite, restore the
 * scene under the NEW spot, then overlay the arrow at the new spot. Tiny: two
 * 14x21 rects + the sprite. This is what makes mouse movement smooth + lag-free
 * across the whole screen (the old code re-rendered + re-scanned the FULL frame
 * on every pointer event -> the bottom-of-screen lag the user reported). */
static uint32_t present_cursor(uint32_t *fb, uint32_t *back, uint32_t *prev,
                               int32_t ox, int32_t oy, int32_t nx, int32_t ny,
                               uint32_t scr_w, uint32_t scr_h, uint32_t stride) {
    uint32_t px = 0;
    /* +2/-1: the 1px white outline extends beyond the bitmap in every direction */
    px += blit_back_rect(fb, back, prev, ox - 1, oy - 1, CUR_W + 2, CUR_H + 2, scr_w, scr_h, stride);
    if (nx != ox || ny != oy)
        px += blit_back_rect(fb, back, prev, nx - 1, ny - 1, CUR_W + 2, CUR_H + 2, scr_w, scr_h, stride);
    draw_cursor(fb, scr_w, scr_h, stride, nx, ny);   /* arrow on very top */
    return px;
}

/* Boot transition: how long the kernel splash fluidly cross-fades into the
 * desktop, in ms.  The iris writes the FULL screen every frame -- on a UC
 * framebuffer (T410 Ironlake, ~3.9 MB per frame at 1280x800) that is extremely
 * expensive.  400 ms is long enough to look intentional + fluid but short enough
 * to avoid a noticeable boot-to-desktop lag on real hardware. */
#define BOOT_FADE_MS  400

/* Present a per-channel cross-fade of `splash` -> `back` into `fb`.
 * `t` is the blend amount in [0,256] (0 = all splash, 256 = all desktop).
 * Kept as a fallback; the boot transition uses present_circle_iris() below. */
static void present_crossfade(uint32_t *fb, uint32_t *back, uint32_t *splash,
                              uint32_t total, uint32_t t) {
    if (t > 256u) t = 256u;
    uint32_t it = 256u - t;
    for (uint32_t i = 0; i < total; i++) {
        uint32_t a = splash[i], b = back[i];
        uint32_t r = ((((a >> 16) & 0xFFu) * it) + (((b >> 16) & 0xFFu) * t)) >> 8;
        uint32_t g = ((((a >>  8) & 0xFFu) * it) + (((b >>  8) & 0xFFu) * t)) >> 8;
        uint32_t bl= (((( a       & 0xFFu) * it) + ((  b       & 0xFFu) * t))) >> 8;
        fb[i] = (r << 16) | (g << 8) | bl;
    }
}

/* Integer sqrt (Newton) -- used once per frame for the iris radius. */
static uint32_t isqrt32(uint32_t n) {
    if (n == 0) return 0;
    uint32_t x = n, y = (x + 1u) / 2u;
    while (y < x) { x = y; y = (x + n / x) / 2u; }
    return x;
}

/* Circular IRIS reveal: the desktop (`back`) opens through a growing, eased,
 * soft-edged circle centered on screen, over the captured boot splash. `t` in
 * [0,256] is transition progress; `max_radius` = center-to-corner distance so
 * the circle fully covers the screen at t=256. This is the fluid "Welcome to
 * AutomationOS" -> desktop boot transition.
 *
 * UC-FB OPTIMISATION: the circle only GROWS, so each frame only needs to write
 * the annular band between the previous inner radius and the current outer
 * radius.  Pixels already fully revealed (inside prev inner) or still-splash
 * (outside curr outer) are unchanged from last frame -> skip them.  On the T410
 * (1280x800) this typically cuts the per-frame UC writes from ~1M px to ~50-100K.
 * Row-level clipping skips entire scanlines outside the band vertically. */
static int64_t g_iris_prev_inner2 = 0;
static int64_t g_iris_prev_outer2 = 0;
static int     g_iris_frame0      = 1;   /* first frame must write everything */

static void present_circle_iris(uint32_t *fb, uint32_t *back, uint32_t *splash,
                                uint32_t W, uint32_t H, uint32_t stride,
                                uint32_t max_radius, uint32_t t) {
    if (t > 256u) t = 256u;
    /* smoothstep easing 3t^2 - 2t^3, kept in [0,256] fixed-point. */
    uint32_t eased  = (uint32_t)(((uint64_t)t * t * (768u - 2u * t)) >> 16);
    uint32_t radius = (max_radius * eased) >> 8;          /* px */
    int32_t  cx = (int32_t)W / 2, cy = (int32_t)H / 2;
    const int32_t FEATHER = 28;                           /* soft-edge band */
    int64_t inner  = (int64_t)radius - FEATHER; if (inner < 0) inner = 0;
    int64_t outer  = (int64_t)radius + FEATHER;
    int64_t inner2 = inner * inner, outer2 = outer * outer;
    int64_t band   = outer2 - inner2; if (band < 1) band = 1;

    /* Delta bounds: only write pixels in the annulus [prev_inner .. curr_outer].
     * First frame (g_iris_frame0) writes everything since the FB starts with the
     * raw splash and we need to establish the initial state. */
    int64_t skip_inner2 = g_iris_frame0 ? -1 : g_iris_prev_inner2;
    int64_t skip_outer2 = outer2;

    for (uint32_t y = 0; y < H; y++) {
        int32_t  dy  = (int32_t)y - cy;
        int64_t  dy2 = (int64_t)dy * dy;
        /* Row-level skip: if dy^2 alone exceeds the write outer bound, every pixel
         * on this row is outside the changed annulus -> skip the entire row. */
        if (!g_iris_frame0 && dy2 > skip_outer2) continue;
        uint32_t off = y * stride;
        for (uint32_t x = 0; x < W; x++) {
            int32_t dx = (int32_t)x - cx;
            int64_t dist2 = (int64_t)dx * dx + dy2;
            /* Skip pixels inside the already-written inner zone. */
            if (!g_iris_frame0 && dist2 < skip_inner2) continue;
            /* Skip pixels outside the current effect boundary. */
            if (!g_iris_frame0 && dist2 > skip_outer2) continue;
            uint32_t px;
            if (dist2 <= inner2) {
                px = back[off + x];                        /* fully revealed */
            } else if (dist2 >= outer2) {
                px = splash[off + x];                      /* still splash    */
            } else {
                uint32_t bl = (uint32_t)(((dist2 - inner2) * 256) / band); /* back->splash */
                uint32_t a = back[off + x], b = splash[off + x];
                uint32_t it = 256u - bl;
                uint32_t r = ((((a >> 16) & 0xFFu) * it) + (((b >> 16) & 0xFFu) * bl)) >> 8;
                uint32_t g = ((((a >>  8) & 0xFFu) * it) + (((b >>  8) & 0xFFu) * bl)) >> 8;
                uint32_t c = (((  a        & 0xFFu) * it) + ((  b        & 0xFFu) * bl)) >> 8;
                px = (r << 16) | (g << 8) | c;
            }
            fb[off + x] = px;
        }
    }
    /* Remember this frame's radii for next-frame delta. */
    g_iris_prev_inner2 = inner2;
    g_iris_prev_outer2 = outer2;
    g_iris_frame0 = 0;
}

/* ====================================================================== *
 *  Client requests                                                        *
 * ====================================================================== */

/* Clamp a window frame so its titlebar stays inside the chrome-free region. */
static void clamp_window(window_t *win) {
    int32_t min_y = PANEL_H + 4;
    int32_t max_y = (int32_t)g_fb_h - DOCK_H - TITLEBAR_H - 8;
    /* M8: right edge is screen_w - RDOCK_W so windows don't slide under dock */
    int32_t max_x = (int32_t)g_fb_w - RDOCK_W - (int32_t)win->w - 4;
    if (win->y < min_y) win->y = min_y;
    if (max_y >= min_y && win->y > max_y) win->y = max_y;
    if (win->x < 4) win->x = 4;
    if (max_x >= 4 && win->x > max_x) win->x = max_x;
}

/* Query an shm segment's byte size via SHMCTL IPC_STAT. Returns the size, or 0
 * if it can't be determined (caller treats 0 as "reject the buffer"). The struct
 * layout mirrors the kernel's IPC_STAT fill (= userspace struct shmid_ds). */
static uint64_t shm_segment_size(int shm_id) {
    struct {
        unsigned int  shm_perm_uid, shm_perm_gid, shm_perm_mode;
        unsigned long shm_segsz, shm_atime, shm_dtime, shm_ctime;
        unsigned int  shm_cpid, shm_lpid, shm_nattch;
    } ds;
    ds.shm_segsz = 0;
    long r = sc6(SYS_SHMCTL, (long)shm_id, IPC_STAT, (long)&ds, 0, 0, 0);
    if (r != 0) return 0;
    return (uint64_t)ds.shm_segsz;
}

static void handle_create(const wl_req_create_t *req) {
    int slot = find_free_slot();
    if (slot < 0) {
        print("[COMP] window limit reached, rejecting create from pid=");
        print_num(req->pid); print("\n");
        return;
    }

    /* Validate client-supplied geometry BEFORE trusting it for SHM blits. A zero
     * dimension is fatal (no buffer). A request LARGER than the display is NOT
     * rejected -- an app may legitimately ask to open "maximized" without knowing
     * the screen size -- instead we CLAMP the DISPLAY size (win->w/h) to the
     * framebuffer while keeping the real buffer stride + SHM extent (buf_w/buf_h)
     * so every blit still source-clamps within the mapped segment (no OOB read).
     * The client re-reads its own win->w/h and reflows; on a screen at least as
     * large as the request (the common case) nothing is clamped. */
    if (req->w == 0 || req->h == 0) {
        print("[COMP] rejecting create: zero geometry w="); print_num((long)req->w);
        print(" h="); print_num((long)req->h); print(" pid="); print_num(req->pid);
        print("\n");
        return;
    }
    uint32_t disp_w = req->w > g_fb_w ? g_fb_w : req->w;
    uint32_t disp_h = req->h > g_fb_h ? g_fb_h : req->h;

    window_t *win = &g_windows[slot];
    for (size_t i = 0; i < sizeof(*win); i++) ((char *)win)[i] = 0;
    win->used       = 1;
    win->win_id     = g_next_win_id++;
    /* Guard against overflow: win_id must stay positive (>0). After 2^31-1
     * windows (theoretical; the 16-slot MAX_WINDOWS makes it ~impossible) the
     * signed int wraps to 0 or negative, breaking slot_by_win_id lookups. Reset
     * to 1 on wrap so IDs are always valid positive integers. */
    if (g_next_win_id <= 0) g_next_win_id = 1;
    win->client_pid = req->pid;
    win->reply_qid  = -1;
    win->shm_id     = req->shm_id;
    win->w          = disp_w;   /* DISPLAY size: clamped to the framebuffer    */
    win->h          = disp_h;
    /* Force stride to the REAL buffer width (in pixels). All clients allocate a
     * tightly-packed req->w * req->h * 4 buffer, so stride == req->w; the blit
     * source-clamps cols to min(win->w, buf_w) and rows to win->h, both within
     * the segment. Deriving stride from the client-supplied (unvalidated)
     * req->stride was an OOB-read vector; pin it to the real width instead. */
    win->stride     = req->w;
    /* Record the IMMUTABLE SHM extent. snap/maximize rewrite win->w/win->h to
     * the maximized drawable rect, but the client's SHM stays this size (wl-lite
     * has no resize event), so every blit clamps its SOURCE read to buf_w/buf_h
     * to avoid reading past the mapped segment. NEVER overwrite these. */
    win->buf_w      = req->w;
    win->buf_h      = req->h;
    win->dirty      = 1;
    win->phase      = PH_NONE;
    win->minimized  = 0;
    win->tb_idx     = -1;
    win->snap_state = SNAP_NONE;                    /* M6: not snapped yet      */
    /* M8: start fade-in from fully transparent. */
    win->fade_alpha    = 0;
    win->fade_start_ms = syscall(SYS_GET_TICKS_MS, 0, 0, 0);
    for (int i = 0; i < WL_TITLE_MAX; i++) win->title[i] = req->title[i];
    win->title[WL_TITLE_MAX - 1] = '\0';

    /* zero-copy attach: map the client's shm segment into THIS process.
     * If the attach or validation fails, ABORT the entire create: undo the slot
     * setup (z-order, MRU, used flag) and return.  Before this fix, a failed
     * shmat left a phantom window (used=1, pixels=NULL) that permanently leaked
     * a slot -- the reaper never cleaned it because the client PID was alive. */
    long addr = sc6(SYS_SHMAT, (long)req->shm_id, 0, 0, 0, 0, 0);
    if (addr > 0) {
        /* Validate the client's claimed buffer extent (w*h*4, stride pinned to
         * w) fits the ACTUAL shm segment. A malicious client can shmget a tiny
         * segment yet claim a huge w*h, making the compositor blit read past the
         * mapping (fault = desktop-wide DoS, or neighbour-memory info leak). */
        uint64_t need = (uint64_t)req->w * (uint64_t)req->h * 4u;
        uint64_t have = shm_segment_size(req->shm_id);
        if (have == 0 || need > have) {
            print("[COMP] rejecting create: buf "); print_num((long)need);
            print(" > shm "); print_num((long)have);
            print(" shm_id="); print_num(req->shm_id); print("\n");
            sc6(SYS_SHMDT, addr, 0, 0, 0, 0, 0);
            /* ABORT: the slot was never added to z-order or MRU (those happen
             * below), so just clear used to reclaim it. */
            win->used = 0;
            return;
        }
        win->shm_vaddr = (uint64_t)addr;
        win->pixels    = (uint32_t *)addr;
    } else {
        print("[COMP] shmat FAILED shm_id="); print_num(req->shm_id);
        print(" r="); print_num(addr); print("\n");
        /* ABORT: the slot was never added to z-order or MRU (those happen
         * below), so just clear used to reclaim it. */
        win->used = 0;
        return;
    }

    /* staggered placement inside the chrome-free region */
    int32_t step = g_spawn_seq++ % 6;
    win->x = 80 + step * 40;
    win->y = PANEL_H + 24 + step * 36;
    clamp_window(win);

    z_push_front(slot);            /* new window becomes focused (topmost)  */
    mru_promote(slot);             /* M6: front of the MRU ring             */

    /* M5: begin the OPEN animation (scale 0.90->1.00, alpha 0->256). */
    anim_begin(win, PH_OPENING, ANIM_OPEN_MS, syscall(SYS_GET_TICKS_MS, 0, 0, 0));

    /* Damage-scissor: a new window changes z-order + reveals shadows globally;
     * force full-screen recomposite for a few frames so nothing is clipped. */
    g_full_damage_cooldown = FULL_DAMAGE_COOLDOWN_FRAMES;
    damage_add_full();

    int32_t qid = client_reply_qid(win);
    if (qid >= 0) {
        wl_evt_created_t ev;
        ev.mtype  = WL_EVT_CREATED;
        ev.win_id = win->win_id;
        long r = sc6(SYS_MSGSND, qid, (long)&ev,
                     (long)(sizeof(ev) - sizeof(int64_t)), 0, 0, 0);
        if (r < 0) { print("[COMP] msgsnd CREATED failed r="); print_num(r); print("\n"); }
    } else {
        print("[COMP] no reply queue for pid="); print_num(req->pid); print("\n");
    }

    print("[COMP] client connected win="); print_num(win->win_id);
    print(" "); print_num((long)win->w); print("x"); print_num((long)win->h);
    print(" pid="); print_num(win->client_pid); print("\n");

#ifdef SELFHEAL
    selfheal_reg_sync();                /* mirror the new window for recovery */
#endif
}

static void handle_commit(const wl_req_commit_t *req) {
    int slot = slot_by_win_id(req->win_id);
    if (slot < 0) return;
    window_t *win = &g_windows[slot];
    win->dirty = 1;
    if (!win->pixels && win->shm_id > 0) {
        long addr = sc6(SYS_SHMAT, (long)win->shm_id, 0, 0, 0, 0, 0);
        if (addr > 0) {
            /* Re-validate the buffer extent against the segment on this
             * (re)attach too -- a rejected/oversized create must not sneak its
             * too-small segment in via a later commit (defense-in-depth on top
             * of clearing shm_id at reject time). buf_w/buf_h = claimed extent. */
            uint64_t need = (uint64_t)win->buf_w * (uint64_t)win->buf_h * 4u;
            uint64_t have = shm_segment_size(win->shm_id);
            if (have == 0 || need > have) {
                sc6(SYS_SHMDT, addr, 0, 0, 0, 0, 0);
            } else {
                win->shm_vaddr = (uint64_t)addr;
                win->pixels    = (uint32_t *)addr;
            }
        }
    }

    /* DAMAGE SCISSOR: translate the client's commit damage rect into screen
     * coords and union it into the per-frame damage accumulator.  The client
     * sends x/y/w/h in its buffer space (wl_req_commit_t.x/y/w/h); clamp to
     * buf_w/buf_h so a malicious client can't bloat the damage rect past the
     * real buffer and force wasteful full-screen recomposites. */
    {
        uint32_t cdx = req->x, cdy = req->y, cdw = req->w, cdh = req->h;
        /* Clamp to the client's actual buffer extent. */
        if (cdx > win->buf_w) cdx = win->buf_w;
        if (cdy > win->buf_h) cdy = win->buf_h;
        if (cdx + cdw > win->buf_w) cdw = win->buf_w - cdx;
        if (cdy + cdh > win->buf_h) cdh = win->buf_h - cdy;
        if (cdw > 0 && cdh > 0) {
            /* Translate buffer coords to screen coords (client area starts at
             * win->x, win->y + TITLEBAR_H). */
            int32_t sx0 = win->x + (int32_t)cdx;
            int32_t sy0 = win->y + TITLEBAR_H + (int32_t)cdy;
            int32_t sx1 = sx0 + (int32_t)cdw;
            int32_t sy1 = sy0 + (int32_t)cdh;
            /* Pad to the window footprint when the damage touches an edge
             * (shadow/border pixels extend past the client area). */
            if (cdx == 0 || cdy == 0 || cdx + cdw >= win->buf_w || cdy + cdh >= win->buf_h) {
                int32_t fx0, fy0, fx1, fy1;
                win_footprint(win, &fx0, &fy0, &fx1, &fy1);
                damage_add(fx0, fy0, fx1, fy1);
            } else {
                damage_add(sx0, sy0, sx1, sy1);
            }
        } else {
            /* Zero-area damage or missing rect: fall back to the full footprint. */
            int32_t fx0, fy0, fx1, fy1;
            win_footprint(win, &fx0, &fy0, &fx1, &fy1);
            damage_add(fx0, fy0, fx1, fy1);
        }
    }
}

/* Tear down a window slot: detach shm, remove from z-order, free the slot.
 * Also SIGTERM the owning process so it exits cleanly -- without this, closing
 * a window via the titlebar close button / Alt+Q / Alt+F4 / context menu leaves
 * the client running headlessly, leaking a PID slot. reap_dead_windows() only
 * fires for ALREADY-dead processes; this is the missing half that CAUSES them to
 * exit. SIGTERM (not SIGKILL) gives the client a chance to clean up; Alt+K
 * remains the hard-kill path for hung apps. A client that sent WL_REQ_DESTROY
 * (voluntary close) is already on its way out, so the redundant signal is a
 * harmless no-op. */
static void destroy_slot(int slot) {
    if (slot < 0 || slot >= MAX_WINDOWS || !g_windows[slot].used) return;
    window_t *win = &g_windows[slot];
    int32_t pid = win->client_pid;
    if (win->shm_vaddr) {
        sc6(SYS_SHMDT, (long)win->shm_vaddr, 0, 0, 0, 0, 0);
    }
    z_remove(slot);
    mru_remove(slot);                              /* M6: drop from MRU ring   */
    /* Zero the ENTIRE slot to reclaim all state (shm_vaddr, pixels, reply_qid,
     * win_id, shm_id, etc.).  Before this fix, stale fields like reply_qid
     * survived and pointed at a dead queue after the client exited. */
    for (size_t i = 0; i < sizeof(*win); i++) ((char *)win)[i] = 0;
    /* Signal the client AFTER clearing the slot so the compositor never renders
     * a window whose process is mid-teardown. pid <= 1 guards against killing
     * init or the compositor itself. */
    if (pid > 1) {
        syscall(SYS_KILL, pid, SIGTERM, 0);
    }
#ifdef SELFHEAL
    selfheal_reg_sync();                /* drop the window from the recovery mirror */
#endif
}

/* Begin the CLOSE animation; the slot is freed when the animation completes
 * (in anim_tick). If a client sends DESTROY, we honor it the same way so the
 * exit is always animated. */
static void begin_close(int slot) {
    if (slot < 0 || slot >= MAX_WINDOWS || !g_windows[slot].used) return;
    window_t *win = &g_windows[slot];
    if (win->phase == PH_CLOSING) return;          /* already closing */
    win->minimized = 0;                            /* un-park so it animates from where it is */
    anim_begin(win, PH_CLOSING, ANIM_CLOSE_MS, syscall(SYS_GET_TICKS_MS, 0, 0, 0));
    /* Damage-scissor: closing reveals wallpaper/underlying windows + shadows. */
    g_full_damage_cooldown = FULL_DAMAGE_COOLDOWN_FRAMES;
    damage_add_full();
}

static void handle_destroy(const wl_req_destroy_t *req) {
    int slot = slot_by_win_id(req->win_id);
    if (slot < 0) return;
    int32_t id = g_windows[slot].win_id;
    begin_close(slot);
    print("[COMP] client disconnected win="); print_num(id); print("\n");
}

/* WL_REQ_RESIZE: the client reallocated its pixel buffer to a new size + shm
 * segment (responding to a WL_EVT_CONFIGURE). Re-attach the new segment and
 * adopt the new buffer extent. This is the ONLY place buf_w/buf_h/shm change
 * after create. Geometry is validated against the framebuffer (like create) so
 * a bogus size can't make later blits read past the mapped segment. The new
 * segment is mapped BEFORE the old is detached, so a failed shmat keeps the
 * window rendering with its existing buffer rather than going blank. */
static void handle_resize(const wl_req_resize_t *req) {
    int slot = slot_by_win_id(req->win_id);
    if (slot < 0) return;
    if (req->w == 0 || req->h == 0 || req->w > g_fb_w || req->h > g_fb_h) {
        print("[COMP] rejecting resize: bad geometry win="); print_num(req->win_id);
        print("\n");
        return;
    }
    window_t *win = &g_windows[slot];
    long addr = sc6(SYS_SHMAT, (long)req->shm_id, 0, 0, 0, 0, 0);
    if (addr <= 0) {
        print("[COMP] resize shmat FAILED shm_id="); print_num(req->shm_id);
        print(" r="); print_num(addr); print("\n");
        return;                                  /* keep the old buffer */
    }
    /* Validate the new buffer extent fits the new segment before adopting it
     * (same OOB-blit protection as handle_create). On failure, detach the new
     * mapping and keep the old buffer. */
    {
        uint64_t need = (uint64_t)req->w * (uint64_t)req->h * 4u;
        uint64_t have = shm_segment_size(req->shm_id);
        if (have == 0 || need > have) {
            print("[COMP] rejecting resize: buf "); print_num((long)need);
            print(" > shm "); print_num((long)have); print("\n");
            sc6(SYS_SHMDT, addr, 0, 0, 0, 0, 0);
            return;                              /* keep the old buffer */
        }
    }
    uint64_t old_vaddr = win->shm_vaddr;
    win->shm_id    = req->shm_id;
    win->shm_vaddr = (uint64_t)addr;
    win->pixels    = (uint32_t *)addr;
    win->buf_w     = req->w;                      /* the immutable extent moves here */
    win->buf_h     = req->h;
    win->stride    = req->w;                      /* tightly packed, pinned to w */
    win->dirty     = 1;
    mark_dirty();                                 /* force a recomposite so the new buffer shows */
    /* Damage-scissor: resize changes the window footprint globally. */
    g_full_damage_cooldown = FULL_DAMAGE_COOLDOWN_FRAMES;
    damage_add_full();
    if (old_vaddr) sc6(SYS_SHMDT, (long)old_vaddr, 0, 0, 0, 0, 0);
    print("[COMP] resize win="); print_num(req->win_id);
    print(" to "); print_num((long)req->w); print("x"); print_num((long)req->h);
    print("\n");
}

#ifdef SELFHEAL
/* SELFHEAL v2: mirror the live window table into the registry page (full
 * re-mirror; 16 entries of plain stores -- cheap enough to run per second).
 * Windows mid-close are skipped so a recovery never resurrects one. */
static void selfheal_reg_sync(void) {
    if (!g_wreg) return;
    for (int s = 0; s < MAX_WINDOWS && s < (int)SH_WINREG_MAX; s++) {
        window_t *win = &g_windows[s];
        volatile sh_winreg_ent_t *e = &g_wreg[s];
        if (!win->used || win->phase == PH_CLOSING) { e->used = 0; continue; }
        e->win_id     = win->win_id;
        e->client_pid = win->client_pid;
        e->shm_id     = win->shm_id;
        e->buf_w      = win->buf_w;
        e->buf_h      = win->buf_h;
        e->x          = win->x;
        e->y          = win->y;
        for (int i = 0; i < WL_TITLE_MAX && i < (int)SH_WINREG_TITLE; i++)
            e->title[i] = win->title[i];
        e->title[SH_WINREG_TITLE - 1] = '\0';
        e->used = 1;                                /* publish LAST */
    }
}

/* SELFHEAL v2: a RESPAWNED compositor rebuilds its window table from the
 * registry the previous instance mirrored. Re-attach each client's pixel
 * buffer by shm_id: the CLIENT owns that segment, so if the client died the
 * segment died with it and shmat fails => the failed attach IS the liveness
 * test; stale entries are cleared. Windows come back under their ORIGINAL
 * win_id so the clients' handles (and their commit/destroy requests) stay
 * valid; reply queues re-resolve lazily by pid. */
static void selfheal_restore_windows(uint32_t fb_w, uint32_t fb_h) {
    if (!g_wreg) return;
    int restored = 0;
    for (int s = 0; s < (int)SH_WINREG_MAX; s++) {
        volatile sh_winreg_ent_t *e = &g_wreg[s];
        if (!e->used) continue;
        if (e->win_id <= 0 || e->shm_id < 0 ||
            e->buf_w == 0 || e->buf_h == 0) { e->used = 0; continue; }

        long addr = sc6(SYS_SHMAT, (long)e->shm_id, 0, 0, 0, 0, 0);
        if (addr <= 0) {                            /* client died with its buffer */
            print("[COMP] SELFHEAL: skip win="); print_num(e->win_id);
            print(" (client gone)\n");
            e->used = 0;
            continue;
        }
        /* Same OOB-blit guard as handle_create: the claimed extent must fit
         * the actual segment (the registry could be stale across a resize). */
        uint64_t need = (uint64_t)e->buf_w * (uint64_t)e->buf_h * 4u;
        uint64_t have = shm_segment_size(e->shm_id);
        if (have == 0 || need > have) {
            sc6(SYS_SHMDT, addr, 0, 0, 0, 0, 0);
            e->used = 0;
            continue;
        }

        int slot = find_free_slot();
        if (slot < 0) { sc6(SYS_SHMDT, addr, 0, 0, 0, 0, 0); break; }
        window_t *win = &g_windows[slot];
        for (size_t i = 0; i < sizeof(*win); i++) ((char *)win)[i] = 0;
        win->used       = 1;
        win->win_id     = e->win_id;                /* ORIGINAL id: client handles live */
        win->client_pid = e->client_pid;
        win->reply_qid  = -1;                       /* re-resolve lazily by pid */
        win->shm_id     = e->shm_id;
        win->shm_vaddr  = (uint64_t)addr;
        win->pixels     = (uint32_t *)addr;
        win->buf_w      = e->buf_w;
        win->buf_h      = e->buf_h;
        win->stride     = e->buf_w;                 /* tightly packed, pinned (create rule) */
        win->w          = e->buf_w > fb_w ? fb_w : e->buf_w;
        win->h          = e->buf_h > fb_h ? fb_h : e->buf_h;
        win->x          = e->x;
        win->y          = e->y;
        win->dirty      = 1;
        win->phase      = PH_NONE;
        win->minimized  = 0;                        /* recovered windows come back visible */
        win->tb_idx     = -1;
        win->snap_state = SNAP_NONE;
        win->fade_alpha    = 0;                     /* fade back in: visible "I'm back" cue */
        win->fade_start_ms = syscall(SYS_GET_TICKS_MS, 0, 0, 0);
        for (int i = 0; i < WL_TITLE_MAX && i < (int)SH_WINREG_TITLE; i++)
            win->title[i] = e->title[i];
        win->title[WL_TITLE_MAX - 1] = '\0';
        clamp_window(win);
        if (win->win_id >= g_next_win_id) g_next_win_id = win->win_id + 1;
        z_push_front(slot);
        mru_promote(slot);
        restored++;
        print("[COMP] SELFHEAL: restored win="); print_num(win->win_id);
        print(" pid="); print_num(win->client_pid);
        print(" "); print_num((long)win->buf_w); print("x"); print_num((long)win->buf_h);
        print("\n");
    }
    if (restored > 0) {
        g_full_damage_cooldown = FULL_DAMAGE_COOLDOWN_FRAMES;
        damage_add_full();
        mark_dirty();
    }
    print("[COMP] SELFHEAL: restore done, windows="); print_num(restored); print("\n");
}
#endif /* SELFHEAL */

/* Maximum client messages to service per frame.  A malicious/chatty client that
 * floods the inbox could otherwise stall the compositor in this loop forever,
 * starving input + present.  64 is generous (one per window per frame is
 * typical; 64 covers a burst of rapid commits without blocking the frame). */
#define DRAIN_BUDGET 64

static void drain_inbox(int32_t inbox_qid) {
    wl_inbox_msg_t msg;
    int budget = DRAIN_BUDGET;
    while (budget-- > 0) {
        /* Zero before each receive: the kernel msgrcv copies only the bytes the
         * client actually sent, so a deliberately short message would otherwise
         * leave stale fields from a PRIOR message (or initial stack garbage) for
         * the handler to trust -- a lever for a malicious client to place chosen
         * values into fields it "didn't send". Zeroing makes un-sent fields read
         * as 0, which the handlers' w==0/h==0 + shm_id validation then rejects. */
        __builtin_memset(&msg, 0, sizeof(msg));
        long r = sc6(SYS_MSGRCV, inbox_qid, (long)&msg,
                     (long)(sizeof(msg) - sizeof(int64_t)), 0, (long)IPC_NOWAIT, 0);
        if (r < 0) break;
        /* PERF: ANY client request is a visible change -- a new window (CREATE),
         * a fresh surface frame the client drew (COMMIT), or a teardown
         * (DESTROY). Mark dirty unconditionally so the gate recomposites. This
         * is the choke-point for "a client drew a new frame". */
        mark_dirty();
        switch (msg.mtype) {
            case WL_REQ_CREATE:  handle_create(&msg.create);   break;
            case WL_REQ_COMMIT:  handle_commit(&msg.commit);   break;
            case WL_REQ_DESTROY: handle_destroy(&msg.destroy); break;
            case WL_REQ_RESIZE:  handle_resize(&msg.resize);   break;
            default: break;
        }
    }
}

/* ====================================================================== *
 *  Input: /dev/input/event0 (kbd) + event1 (mouse), polled non-blocking  *
 * ====================================================================== */
typedef struct {
    uint64_t timestamp;
    uint16_t type;
    uint16_t code;
    int32_t  value;
} input_event_t;

#define EV_KEY  0
#define EV_REL  1
#define REL_X   0
#define REL_Y   1
#define REL_WHEEL 8
#define BTN_LEFT_CODE    0x110
#define BTN_RIGHT_CODE   0x111
#define BTN_MIDDLE_CODE  0x112
#define EVENTS_PER_READ  32

static int32_t g_kbd_fd   = -1;
static int32_t g_mouse_fd = -1;

/* live pointer state */
static int32_t g_cursor_x = 0;
static int32_t g_cursor_y = 0;
static int32_t g_buttons  = 0;   /* bit0=left bit1=right bit2=middle */
static int32_t g_wheel_delta = 0; /* accumulated wheel scroll (reset per frame) */
/* Per-event click latches. A quick click delivers press+release in the SAME
 * pump_input batch, so the once-per-frame g_buttons edge check misses it. We
 * latch the rising edge as it happens (capturing the cursor position) and
 * consume it in handle_mouse, so every click registers regardless of batching. */
static int32_t g_click_latch  = 0, g_click_cx  = 0, g_click_cy  = 0;  /* left  */
static int32_t g_rclick_latch = 0, g_rclick_cx = 0, g_rclick_cy = 0;  /* right */

static int g_alt_held;   /* fwd decl (defined below); used by the Alt+wheel zoom */
static void send_pointer_to_focus(void) {
    /* Alt + mouse-wheel = GLOBAL DESKTOP ZOOM. Consume the wheel here (don't
     * forward it to the app) and step the whole-desktop UI scale by 10% per
     * notch; the next composite re-renders all chrome at the new cell. Works even
     * with no focused window (zoom from the bare desktop). */
    if (g_alt_held && g_wheel_delta != 0) {
        cz_set_scale(g_ui_pct + (g_wheel_delta > 0 ? 100 : -100));  /* UI-CRISP-0: integer 100<->200 */
        g_wheel_delta = 0;
        mark_dirty();
    }
    int slot = focused_slot();
    if (slot < 0) return;
    window_t *win = &g_windows[slot];
    int32_t qid = client_reply_qid(win);
    if (qid < 0) return;
    wl_evt_pointer_t ev;
    ev.mtype   = WL_EVT_POINTER;
    ev.x       = g_cursor_x - win->x;
    ev.y       = g_cursor_y - (win->y + TITLEBAR_H);
    ev.buttons = g_buttons;
    ev.wheel   = g_wheel_delta;
    sc6(SYS_MSGSND, qid, (long)&ev, (long)(sizeof(ev) - sizeof(int64_t)), 0, 0, 0);
}

static void send_key_to_focus(int32_t keycode, int32_t pressed) {
    int slot = focused_slot();
    if (slot < 0) return;
    window_t *win = &g_windows[slot];
    int32_t qid = client_reply_qid(win);
    if (qid < 0) return;
    wl_evt_key_t ev;
    ev.mtype   = WL_EVT_KEY;
    ev.keycode = keycode;
    ev.pressed = pressed;
    sc6(SYS_MSGSND, qid, (long)&ev, (long)(sizeof(ev) - sizeof(int64_t)), 0, 0, 0);
}

/* ---------------------------------------------------------------------- *
 *  M6: keyboard shortcut + Alt+Tab interception.                          *
 *                                                                          *
 *  wm_handle_key() is called for every EV_KEY BEFORE forwarding. It tracks *
 *  Left-Alt state and consumes the WM shortcuts so they never reach the    *
 *  client. Returns 1 if the key was consumed (do NOT forward), else 0.     *
 * ---------------------------------------------------------------------- */
static int     g_alt_held    = 0;      /* Left-Alt currently down?          */
static int     g_alttab_live = 0;      /* an Alt+Tab cycle is in progress    */
static int32_t g_alttab_pos  = 0;      /* index into the MRU ring this cycle */

/* fwd decls for WM actions defined later in the file */
static void focus_window(int slot);
static void begin_minimize(int slot);

/* Raise + focus a slot WITHOUT reordering the MRU ring (so repeated Alt+Tab
 * presses keep cycling predictably; the MRU is committed when Alt is released). */
static void alttab_raise(int slot) {
    if (slot < 0 || slot >= MAX_WINDOWS || !g_windows[slot].used) return;
    z_push_front(slot);                            /* raise visually + focus   */
    print("[SHELL] alt-tab focus win "); print_num(g_windows[slot].win_id); print("\n");
}

/* Alt+Tab selection overlay: while a cycle is live, glow an accent border around
 * the window that WILL be focused on Alt-release + show its title centered, so
 * the user can see what they're switching to. */
static void render_alttab_overlay(uint32_t *buf, uint32_t w, uint32_t h, uint32_t stride) {
    if (!g_alttab_live || g_mru_count == 0) return;
    if (g_alttab_pos < 0 || g_alttab_pos >= g_mru_count) return;
    int slot = (int)g_mru[g_alttab_pos];
    if (slot < 0 || slot >= MAX_WINDOWS || !g_windows[slot].used) return;
    window_t *win = &g_windows[slot];
    /* enclose the FULL frame incl. its BORDER_W borders, with a small margin */
    int32_t fx = win->x - BORDER_W - 3, fy = win->y - BORDER_W - 3;
    int32_t fw = (int32_t)win->w + 2 * BORDER_W + 6;
    int32_t fh = (int32_t)(TITLEBAR_H + win->h) + 2 * BORDER_W + 6;
    stroke_rect(buf, w, h, stride, fx,     fy,     fw,     fh,     0xFF0A84FFu);
    stroke_rect(buf, w, h, stride, fx - 1, fy - 1, fw + 2, fh + 2, 0x900A84FFu);
    stroke_rect(buf, w, h, stride, fx - 2, fy - 2, fw + 4, fh + 4, 0x500A84FFu);
    const char *t = win->title[0] ? win->title : "window";
    int tl = 0; while (t[tl] && tl < WL_TITLE_MAX) tl++;
    int pill_w = tl * FONT_W + 24;
    int pill_h = FONT_H + 14;
    int32_t pxc = (int32_t)w / 2 - pill_w / 2;
    int32_t pyc = (int32_t)h / 2 - pill_h / 2;
    blend_rect (buf, w, h, stride, pxc, pyc, pill_w, pill_h, 0xE0101418u);
    stroke_rect(buf, w, h, stride, pxc, pyc, pill_w, pill_h, 0xFF0A84FFu);
    cz_text(buf, (int)stride, (int)w, (int)h, pxc + 12, pyc + 7, t, 0xFFFFFFFFu);
}

/* Pick the next focusable (used, not minimized, not closing) slot when Alt+Tab
 * advances. We walk the MRU ring starting after the current position. */
static void alttab_advance(void) {
    if (g_mru_count == 0) return;
    /* On the first Tab of a cycle, start from the 2nd MRU entry so one tap
     * swaps to the previously-focused window (classic Alt+Tab behavior). */
    int start = g_alttab_live ? (g_alttab_pos + 1) : 1;
    for (int n = 0; n < g_mru_count; n++) {
        int idx  = (start + n) % g_mru_count;
        int slot = (int)g_mru[idx];
        if (slot < 0 || slot >= MAX_WINDOWS || !g_windows[slot].used) continue;
        if (g_windows[slot].minimized) continue;
        if (g_windows[slot].phase == PH_CLOSING || g_windows[slot].phase == PH_MINIMIZING)
            continue;
        g_alttab_pos  = idx;
        g_alttab_live = 1;
        alttab_raise(slot);
        return;
    }
}

/* Forward decls: the Alt+Enter maximize chord below reuses the snap helpers that
 * are defined later in the file (window-management section). */
static void begin_snap_to(int slot, int32_t kind);
static void start_geom_tween(window_t *win, int32_t to_x, int32_t to_y,
                             uint32_t to_w, uint32_t to_h);

/* Returns 1 if the key was consumed by the WM (do not forward to client). */
static int wm_handle_key(int32_t keycode, int32_t pressed) {
    /* Track the Alt modifier. */
    if (keycode == KEY_LEFTALT) {
        g_alt_held = pressed ? 1 : 0;
        if (!pressed && g_alttab_live) {
            /* Alt released: commit the focused window to the front of the MRU. */
            int f = focused_slot();
            if (f >= 0) mru_promote(f);
            g_alttab_live = 0;
        }
        return 1;                                  /* never forward the Alt key */
    }

    if (!g_alt_held) return 0;                     /* no modifier -> forward    */

    /* Alt is held: intercept the WM chords on key DOWN only. */
    if (pressed) {
        if (keycode == KEY_TAB) {
            alttab_advance();
            return 1;
        }
        if (keycode == KEY_Q || keycode == KEY_F4) {
            int f = focused_slot();
            if (f >= 0) { int32_t id = g_windows[f].win_id; begin_close(f);
                          print("[SHELL] shortcut close win "); print_num(id); print("\n"); }
            return 1;
        }
        if (keycode == KEY_M) {
            int f = focused_slot();
            if (f >= 0) begin_minimize(f);
            return 1;
        }
        if (keycode == KEY_ENTER) {
            /* Alt+Enter: toggle maximize on the focused window (same path as the
             * titlebar maximize box). With a full-work-area client buffer the
             * window fills the screen; smaller fixed buffers fill up to their
             * own surface until the resize/configure protocol lands. */
            int f = focused_slot();
            if (f >= 0) {
                window_t* win = &g_windows[f];
                if (win->snap_state == SNAP_MAX) {
                    send_configure(win, win->saved_w, win->saved_h);  /* shrink buffer back */
                    start_geom_tween(win, win->saved_x, win->saved_y,
                                     win->saved_w, win->saved_h);
                    win->snap_state = SNAP_NONE;
                } else {
                    begin_snap_to(f, SNAP_MAX);
                }
            }
            return 1;
        }
        if (keycode == KEY_S) {
            /* PERF: toggle the on-screen stats overlay (FPS/frame-time/windows/
             * pixels). Mark dirty so it appears/disappears on the next frame. */
            g_stats_on = !g_stats_on;
            mark_dirty();
            print("[SHELL] stats overlay ");
            print(g_stats_on ? "ON\n" : "OFF\n");
            return 1;
        }
        if (keycode == KEY_K) {
            /* FORCE QUIT: SIGKILL the focused window's client process, even if
             * it is hung and not draining its event queue. The per-frame liveness
             * sweep (reap_dead_windows) removes the window once the pid is gone;
             * we also start the close animation now for instant feedback. */
            int f = focused_slot();
            if (f >= 0) {
                int32_t pid = g_windows[f].client_pid;
                int32_t id  = g_windows[f].win_id;
                long rc = syscall(SYS_KILL, pid, SIGKILL, 0);
                print("[SHELL] FORCE QUIT win "); print_num(id);
                print(" pid "); print_num(pid);
                print(" rc "); print_num(rc); print("\n");
                begin_close(f);
            }
            return 1;
        }
        if (keycode == KEY_D) {
            /* SHOW DESKTOP: minimize every visible window so the desktop is
             * accessible. Mirrors the MACT_MINIMIZE_ALL start-menu action. */
            for (int s = 0; s < MAX_WINDOWS; s++)
                if (g_windows[s].used && !g_windows[s].minimized &&
                    g_windows[s].phase != PH_CLOSING)
                    begin_minimize(s);
            print("[SHELL] show desktop (Alt+D)\n");
            return 1;
        }
    }
    /* Consume the key-UP of an intercepted chord too, so the client never sees
     * a dangling release for a press it never got. (Tab/Q/F4/M/K/S/D while Alt held.) */
    if (keycode == KEY_TAB || keycode == KEY_Q || keycode == KEY_F4 ||
        keycode == KEY_M   || keycode == KEY_K || keycode == KEY_S ||
        keycode == KEY_D   || keycode == KEY_ENTER)
        return 1;

    return 0;                                       /* other Alt+<key>: forward  */
}

/* M6 liveness sweep: a client may exit normally OR be force-killed (Alt+K or
 * the Task Manager) without ever sending WL_REQ_DESTROY. Probe each window's
 * owner pid with kill(pid, 0) (signal 0 = existence check); if it returns
 * ESRCH the process is gone, so animate the orphaned window away. Throttled by
 * the caller to a few times per second. */
static void reap_dead_windows(void) {
    for (int s = 0; s < MAX_WINDOWS; s++) {
        if (!g_windows[s].used) continue;
        if (g_windows[s].phase == PH_CLOSING) continue;   /* already leaving */
        int32_t pid = g_windows[s].client_pid;
        if (pid <= 0) continue;
        long rc = syscall(SYS_KILL, pid, 0, 0);
        /* kill(pid,0) returns 0 for a live process. Any negative return means
         * the process is gone/unsignalable (this single-user kernel has no
         * signal permission check, so the only error is ESRCH). Treat any
         * error as "dead" rather than hardcoding a specific errno value. */
        if (rc < 0) {
            print("[SHELL] reaping dead win "); print_num(g_windows[s].win_id);
            print(" (pid "); print_num(pid); print(" gone)\n");
            begin_close(s);
        }
    }
}

/* Read + process all currently-buffered events from one fd. keyboard==1
 * routes EV_KEY presses to the focused window; else the fd is the mouse. */
static void pump_input(int32_t fd, int keyboard, uint32_t W, uint32_t H) {
    if (fd < 0) return;
    input_event_t evs[EVENTS_PER_READ];
    long n = syscall(SYS_READ, fd, (long)evs, (long)sizeof(evs));
    if (n <= 0) return;
    long count = n / (long)sizeof(input_event_t);
    int pointer_changed = 0;

    for (long i = 0; i < count; i++) {
        input_event_t *e = &evs[i];
        if (keyboard) {
            if (e->type == EV_KEY) {
                int32_t code    = (int32_t)e->code;
                int32_t pressed = e->value != 0 ? 1 : 0;
                /* PERF: any key event may change the display -- a WM chord
                 * (Alt+Tab raise, Alt+Q close, Alt+M minimize) or a key the
                 * focused client will redraw in response to (e.g. typing in an
                 * editor/terminal). Mark dirty for every key event; biasing
                 * toward an extra recomposite is cheaper than a stale screen. */
                mark_dirty();
                /* M6: WM intercepts Alt / Alt+Tab / Alt+Q / Alt+F4 / Alt+M
                 * BEFORE the client ever sees them. Everything else forwards. */
                if (!wm_handle_key(code, pressed))
                    send_key_to_focus(code, pressed);
            }
            continue;
        }
        if (e->type == EV_REL) {
            if (e->code == REL_X) { g_cursor_x += e->value; pointer_changed = 1; }
            else if (e->code == REL_Y) { g_cursor_y += e->value; pointer_changed = 1; }
            else if (e->code == REL_WHEEL) { g_wheel_delta += e->value; }
        } else if (e->type == EV_KEY) {
            int32_t bit = -1;
            if (e->code == BTN_LEFT_CODE)   bit = 0;
            else if (e->code == BTN_RIGHT_CODE)  bit = 1;
            else if (e->code == BTN_MIDDLE_CODE) bit = 2;
            if (bit >= 0) {
                if (e->value) {
                    /* Latch the rising edge here so a press+release in one batch
                     * still registers as a click (the frame-level edge would be
                     * lost). Capture the cursor position at press time. */
                    if (bit == 0 && !(g_buttons & 1)) {
                        g_click_latch = 1; g_click_cx = g_cursor_x; g_click_cy = g_cursor_y;
                    } else if (bit == 1 && !(g_buttons & 2)) {
                        g_rclick_latch = 1; g_rclick_cx = g_cursor_x; g_rclick_cy = g_cursor_y;
                    }
                    g_buttons |= (1 << bit);
                } else {
                    g_buttons &= ~(1 << bit);
                }
                pointer_changed = 1;
            }
        }
    }

    if (!keyboard && pointer_changed) {
        if (g_cursor_x < 0) g_cursor_x = 0;
        if (g_cursor_y < 0) g_cursor_y = 0;
        if (g_cursor_x >= (int32_t)W) g_cursor_x = (int32_t)W - 1;
        if (g_cursor_y >= (int32_t)H) g_cursor_y = (int32_t)H - 1;
        /* SMOOTH-MOUSE FAST PATH: a pure pointer move only needs the cursor
         * SPRITE moved (present_cursor), not a full scene recomposite+rescan.
         * We force a real scene recomposite (mark_dirty) ONLY when the move can
         * change non-cursor pixels: near hover-reactive chrome (top panel, bottom
         * dock, right-dock magnify field, a window titlebar), while a menu/dialog
         * is open, a snap preview or toast is live, or a window is animating --
         * plus a periodic safety net so a missed hover can never linger. Button
         * changes always recomposite (handled by the click latch + handle_mouse).
         * Otherwise it's a pure move and the fast path handles it downstream. */
        static uint32_t g_cursor_safety = 0;
        g_cursor_moved = 1;
        int hover = (g_cursor_y < PANEL_H) ||
                    (g_cursor_y > (int32_t)H - DOCK_H) ||
                    (g_cursor_x > (int32_t)W - RDOCK_W - 8) ||  /* GUI-LAT-1: recomposite only over the 48px dock strip + 8px hysteresis, not the full 110px magnify field */
                    g_menu_open || g_about_open ||
                    g_snap_armed != SNAP_NONE || g_toast_dur_ms > 0 ||
                    (g_buttons != 0);                /* dragging / press-drag */
        if (!hover) {
            for (int wi = 0; wi < MAX_WINDOWS; wi++) {
                if (!g_windows[wi].used) continue;
                if (g_windows[wi].phase != PH_NONE) { hover = 1; break; }
                int32_t wx = g_windows[wi].x, wy = g_windows[wi].y;
                if (g_cursor_x >= wx && g_cursor_x < wx + (int32_t)g_windows[wi].w &&
                    g_cursor_y >= wy && g_cursor_y < wy + TITLEBAR_H) { hover = 1; break; }
            }
        }
        if (hover || (g_cursor_safety++ & 255) == 0) mark_dirty();  /* GUI-LAT-1: bare-canvas moves take present_cursor (2 sprite rects); full-recomposite safety net every 256th packet, not 16th */
        send_pointer_to_focus();
    }
}

/* SYNTHINPUT-0: drain the agent injection ring and apply each event exactly as the
 * real PS/2 path would -- update the cursor/buttons + reuse send_pointer_to_focus /
 * wm_handle_key / send_key_to_focus. Single-producer (agent tool) / single-consumer
 * (here), so no locking; head/tail are accessed through the volatile SHM pointer.
 * Drained BEFORE the real pump_input() each frame so human + agent input mix freely. */
static void pump_synth_input(uint32_t W, uint32_t H) {
    volatile synthinput_shm_t *si = g_synth;
    if (!si || si->magic != SYNTHINPUT_MAGIC || !si->active) return;
    int moved = 0;
    int guard = 0;
    while (si->tail != si->head && guard++ < SYNTHINPUT_QMAX) {
        unsigned idx = si->tail & (SYNTHINPUT_QMAX - 1);
        unsigned short type = si->q[idx].type, code = si->q[idx].code;
        int value = si->q[idx].value;
        /* Keep tail in the SAME [0,QMAX) space the producers keep head in (they store
         * (head+1)%QMAX), else tail!=head stays true forever after the first wrap and
         * we replay stale slots every frame. QMAX is a power of two so & works. */
        si->tail = (si->tail + 1) & (SYNTHINPUT_QMAX - 1);
        { static int g_synth_first = 0;
          if (!g_synth_first) { g_synth_first = 1;
              print("[SHELL] SYNTHINPUT: input applied (agent is driving)\n"); } }
        /* SYNTHINPUT-WRAP-0: prove the >64-event tail wrap. Cumulative drained count
         * across frames; the FIRST time it exceeds QMAX (tail has wrapped past slot 63)
         * print the count + settled cursor_x ONCE. A stale-replay regression would
         * re-trip this every frame and keep moving the cursor. */
        { static unsigned long g_synth_drained = 0; static int g_synth_wrap = 0;
          g_synth_drained++;
          if (!g_synth_wrap && g_synth_drained > SYNTHINPUT_QMAX) { g_synth_wrap = 1;
              print("[SHELL] SYNTHINPUT: drained "); print_num((long)g_synth_drained);
              print(" events cursor_x="); print_num((long)g_cursor_x); print("\n"); } }
        if (type == SI_EV_REL) {
            if (code == SI_REL_X)      { g_cursor_x += value; moved = 1; }
            else if (code == SI_REL_Y) { g_cursor_y += value; moved = 1; }
            else if (code == SI_REL_WHEEL) { g_wheel_delta += value; }
        } else if (type == SI_EV_KEY) {
            int pressed = value != 0 ? 1 : 0;
            int bit = (code == SI_BTN_LEFT)   ? 0 :
                      (code == SI_BTN_RIGHT)  ? 1 :
                      (code == SI_BTN_MIDDLE) ? 2 : -1;
            if (bit >= 0) {
                if (pressed) {
                    if (bit == 0 && !(g_buttons & 1)) { g_click_latch = 1; g_click_cx = g_cursor_x; g_click_cy = g_cursor_y; }
                    else if (bit == 1 && !(g_buttons & 2)) { g_rclick_latch = 1; g_rclick_cx = g_cursor_x; g_rclick_cy = g_cursor_y; }
                    g_buttons |= (1 << bit);
                } else {
                    g_buttons &= ~(1 << bit);
                }
                moved = 1;
            } else {
                mark_dirty();
                if (!wm_handle_key((int32_t)code, pressed))
                    send_key_to_focus((int32_t)code, pressed);
            }
        }
    }
    if (moved) {
        if (g_cursor_x < 0) g_cursor_x = 0;
        if (g_cursor_y < 0) g_cursor_y = 0;
        if (g_cursor_x >= (int32_t)W) g_cursor_x = (int32_t)W - 1;
        if (g_cursor_y >= (int32_t)H) g_cursor_y = (int32_t)H - 1;
        g_cursor_moved = 1;
        mark_dirty();
        send_pointer_to_focus();
    }
}

/* ---------------------------------------------------------------------- *
 *  Desktop-shell mouse interaction (hit-testing + drag-move)             *
 * ---------------------------------------------------------------------- */
static int32_t g_prev_buttons = 0;     /* for left-click edge detection    */
static int     g_drag_slot   = -1;     /* slot being drag-moved, or -1     */
static int32_t g_drag_dx = 0, g_drag_dy = 0;  /* cursor-to-frame offset    */

static int point_in(int32_t px, int32_t py, int32_t x, int32_t y, int32_t w, int32_t h) {
    return px >= x && px < x + w && py >= y && py < y + h;
}

/* M6: which snap zone (if any) does the cursor currently arm? Top edge wins
 * (maximize), then left/right. Returns SNAP_NONE if not near any edge. */
static int32_t snap_zone_for_cursor(int32_t cx, int32_t cy, uint32_t W, uint32_t H) {
    (void)H;
    if (cy <= PANEL_H + SNAP_EDGE_PX)               return SNAP_MAX;
    if (cx <= SNAP_EDGE_PX)                          return SNAP_LEFT;
    /* M8: right snap arm fires at the work-area right edge (before the dock) */
    if (cx >= (int32_t)W - RDOCK_W - SNAP_EDGE_PX)  return SNAP_RIGHT;
    return SNAP_NONE;
}

static void spawn_terminal(void) {
    print("[SHELL] launch terminal\n");
    long r = syscall(SYS_SPAWN, (long)"sbin/terminal", 0, 0);
    if (r < 0) { print("[SHELL] spawn failed r="); print_num(r); print("\n"); }
}

/* Focus + raise a window slot. */
static void focus_window(int slot) {
    if (slot < 0 || slot >= MAX_WINDOWS || !g_windows[slot].used) return;
    z_push_front(slot);
    mru_promote(slot);                             /* M6: keep MRU ring current */
    print("[SHELL] focus win "); print_num(g_windows[slot].win_id); print("\n");
}

/* Begin minimize (park toward taskbar). */
static void begin_minimize(int slot) {
    if (slot < 0 || slot >= MAX_WINDOWS || !g_windows[slot].used) return;
    window_t *win = &g_windows[slot];
    if (win->minimized || win->phase == PH_MINIMIZING || win->phase == PH_CLOSING) return;
    win->tb_idx = taskbar_index_of(slot);          /* capture target taskbar slot */
    anim_begin(win, PH_MINIMIZING, ANIM_MIN_MS, syscall(SYS_GET_TICKS_MS, 0, 0, 0));
    print("[SHELL] minimize win "); print_num(win->win_id); print("\n");
}

/* Begin restore (un-park from taskbar). */
static void begin_restore(int slot) {
    if (slot < 0 || slot >= MAX_WINDOWS || !g_windows[slot].used) return;
    window_t *win = &g_windows[slot];
    if (win->phase == PH_RESTORING || win->phase == PH_CLOSING) return;
    win->tb_idx = taskbar_index_of(slot);
    win->minimized = 0;                            /* visible again, animating in */
    anim_begin(win, PH_RESTORING, ANIM_RESTORE_MS, syscall(SYS_GET_TICKS_MS, 0, 0, 0));
    z_push_front(slot);                            /* raise + focus on restore */
    print("[SHELL] restore win "); print_num(win->win_id); print("\n");
}

/* ---------------------------------------------------------------------- *
 *  M6: window snapping. begin_snap_to() animates a window into a snap zone *
 *  (left/right half or maximized); begin_unsnap() animates it back to the  *
 *  geometry that was saved when it first snapped.                          *
 * ---------------------------------------------------------------------- */

/* Kick off the PH_SNAPPING geometry tween from the window's current frame to
 * (to_x,to_y) + client (to_w,to_h). */
static void start_geom_tween(window_t *win, int32_t to_x, int32_t to_y,
                             uint32_t to_w, uint32_t to_h) {
    win->from_x = win->x;  win->from_y = win->y;
    win->from_w = win->w;  win->from_h = win->h;
    win->to_x   = to_x;    win->to_y   = to_y;
    win->to_w   = to_w;    win->to_h   = to_h;
    anim_begin(win, PH_SNAPPING, ANIM_SNAP_MS, syscall(SYS_GET_TICKS_MS, 0, 0, 0));
}

/* Snap a window to the given zone. Saves the pre-snap geometry the FIRST time
 * (so repeated snaps don't overwrite the real restore target). */
static void begin_snap_to(int slot, int32_t kind) {
    if (slot < 0 || slot >= MAX_WINDOWS || !g_windows[slot].used) return;
    window_t *win = &g_windows[slot];
    if (win->phase == PH_CLOSING || win->phase == PH_MINIMIZING) return;
    int32_t fx, fy; uint32_t cw, ch;
    if (snap_target_rect(kind, &fx, &fy, &cw, &ch) != 0) return;
    if (win->snap_state == SNAP_NONE) {            /* remember where we came from */
        win->saved_x = win->x; win->saved_y = win->y;
        win->saved_w = win->w; win->saved_h = win->h;
    }
    win->snap_state = kind;
    /* GROW-TO-FILL (resize/configure protocol): ask the client to reallocate its
     * pixel buffer to the snap target so the window fills the frame crisply for
     * ANY app (terminal, games), instead of clamping the frame down to the app's
     * fixed buffer. The blit source-clamps (buf_w/buf_h) keep rendering safe until
     * the client's WL_REQ_RESIZE arrives and updates buf_w/buf_h. */
    if (cw != win->buf_w || ch != win->buf_h)
        send_configure(win, cw, ch);
    start_geom_tween(win, fx, fy, cw, ch);
    print("[SHELL] snap win "); print_num(win->win_id);
    print(" kind="); print_num(kind); print("\n");
}

/*
 * Resolve all left-mouse interaction. Called once per frame AFTER pump_input
 * has updated g_cursor_x/y + g_buttons. Uses edge detection (prev vs current
 * left-button bit) so a click fires exactly once on press.
 */
/* ---------------------------- popup-menu logic ------------------------- */
static void menu_add(const char *label, const char *path, int action) {
    if (g_menu_n >= MENU_MAX) return;
    g_menu_label[g_menu_n]  = label;
    g_menu_path[g_menu_n]   = path;
    g_menu_action[g_menu_n] = action;
    g_menu_n++;
}
static void open_start_menu(uint32_t W, uint32_t H) {
    (void)W;
    g_menu_n = 0; g_menu_is_ctx = 0;
    for (int i = 0; i < RDOCK_NICONS; i++)
        menu_add(rdock_apps[i].path + 5, rdock_apps[i].path, MACT_NONE);  /* skip "sbin/" */
    int32_t mh = menu_height();
    g_menu_x = launcher_x();
    g_menu_y = dock_top(H) - mh - 6;
    if (g_menu_y < PANEL_H + 4) g_menu_y = PANEL_H + 4;
    g_menu_open = 1;
}
/* Open the right-click context menu. `target_slot` is the window under the
 * cursor (so the menu acts on THAT window), or -1 for the bare desktop. The
 * item set is chosen per target so the menu is appropriate to what was clicked. */
static void open_context_menu(int32_t cx, int32_t cy, int target_slot,
                              uint32_t W, uint32_t H) {
    g_menu_n = 0; g_menu_is_ctx = 1;
    g_menu_target_slot = target_slot;
    if (target_slot >= 0 && target_slot < MAX_WINDOWS && g_windows[target_slot].used) {
        /* window context: actions on that specific window (same code paths as
         * the title-bar minimize/maximize/close buttons). */
        menu_add("Minimize", (const char *)0, MACT_WIN_MINIMIZE);
        if (g_windows[target_slot].snap_state == SNAP_MAX)
            menu_add("Restore",  (const char *)0, MACT_WIN_MAXIMIZE);
        else
            menu_add("Maximize", (const char *)0, MACT_WIN_MAXIMIZE);
        menu_add("Snap Left",  (const char *)0, MACT_WIN_SNAP_LEFT);
        menu_add("Snap Right", (const char *)0, MACT_WIN_SNAP_RIGHT);
        menu_add("Close",    (const char *)0, MACT_WIN_CLOSE);
    } else {
        /* desktop context: actions on the workspace. */
        menu_add("New Folder",       (const char *)0, MACT_NEW_FOLDER);
        menu_add("Display Settings", (const char *)0, MACT_DISPLAY_SETTINGS);
        menu_add("Refresh",          (const char *)0, MACT_REFRESH);
        menu_add("About",            (const char *)0, MACT_ABOUT);
    }
    int32_t mh = menu_height();
    g_menu_x = cx; g_menu_y = cy;
    if (g_menu_x + MENU_W > (int32_t)W - RDOCK_W) g_menu_x = (int32_t)W - RDOCK_W - MENU_W - 4;
    if (g_menu_x < 4) g_menu_x = 4;
    if (g_menu_y + mh > (int32_t)H - DOCK_H) g_menu_y = (int32_t)H - DOCK_H - mh - 4;
    if (g_menu_y < PANEL_H + 4) g_menu_y = PANEL_H + 4;
    g_menu_open = 1;
}
/* Append the NUL-terminated `src` to `dst` (which already holds `*plen` chars,
 * room for `cap` total incl NUL). Updates *plen. Bounded. */
static void desk_strcat(char *dst, int *plen, int cap, const char *src) {
    int n = *plen;
    for (int i = 0; src[i] && n < cap - 1; i++) dst[n++] = src[i];
    dst[n] = '\0';
    *plen = n;
}
/* Append a small positive integer as decimal text. */
static void desk_cat_num(char *dst, int *plen, int cap, int v) {
    char tmp[12]; int t = 0;
    if (v == 0) tmp[t++] = '0';
    while (v > 0 && t < (int)sizeof(tmp)) { tmp[t++] = (char)('0' + v % 10); v /= 10; }
    while (t > 0) { int n = *plen; if (n < cap - 1) { dst[n++] = tmp[--t]; dst[n] = '\0'; *plen = n; } else break; }
}

/* Create "/Desktop/NewFolder" (or NewFolder2, NewFolder3, ... if taken) via
 * SYS_MKDIR, then rescan so the new tile appears. Existence is probed with
 * SYS_OPENDIR (succeeds only on an existing dir) so we pick a fresh name. */
static void desk_new_folder(void) {
    char path[64];
    for (int n = 1; n <= 99; n++) {
        int len = 0;
        path[0] = '\0';
        desk_strcat(path, &len, (int)sizeof(path), "/Desktop/NewFolder");
        if (n > 1) desk_cat_num(path, &len, (int)sizeof(path), n);

        /* does it already exist? opendir returns >=0 only for an existing dir */
        long probe = syscall(SYS_OPENDIR, (long)path, 0, 0);
        if (probe >= 0) { syscall(SYS_CLOSEDIR, probe, 0, 0); continue; }

        long r = syscall(SYS_MKDIR, (long)path, 0755, 0);
        print("[SHELL] new folder "); print(path);
        if (r < 0) { print(" (failed r="); print_num(r); print(")"); }
        print("\n");
        break;
    }
    desk_scan();   /* refresh the desktop icon list */
}

static void menu_run(int idx) {
    if (idx < 0 || idx >= g_menu_n) return;
    if (g_menu_path[idx]) {
        long r = syscall(SYS_SPAWN, (long)g_menu_path[idx], 0, 0);
        print("[SHELL] menu launch "); print(g_menu_path[idx]);
        if (r < 0) print(" (failed)");
        print("\n");
    } else if (g_menu_action[idx] == MACT_CLOSE_ALL) {
        for (int s = 0; s < MAX_WINDOWS; s++)
            if (g_windows[s].used && g_windows[s].phase != PH_CLOSING) begin_close(s);
    } else if (g_menu_action[idx] == MACT_MINIMIZE_ALL) {
        for (int s = 0; s < MAX_WINDOWS; s++)
            if (g_windows[s].used && !g_windows[s].minimized &&
                g_windows[s].phase != PH_CLOSING) begin_minimize(s);
    } else if (g_menu_action[idx] == MACT_ABOUT) {
        g_about_open = 1;   /* show the modal About dialog (not a toast) */
    } else if (g_menu_action[idx] == MACT_NEW_FOLDER) {
        desk_new_folder();
    } else if (g_menu_action[idx] == MACT_DISPLAY_SETTINGS) {
        syscall(SYS_SPAWN, (long)"sbin/settings", 0, 0);
    } else if (g_menu_action[idx] == MACT_REFRESH) {
        desk_scan();        /* re-read desktop icons; scene repaints every frame */
    } else if (g_menu_action[idx] == MACT_WIN_MINIMIZE ||
               g_menu_action[idx] == MACT_WIN_MAXIMIZE ||
               g_menu_action[idx] == MACT_WIN_SNAP_LEFT ||
               g_menu_action[idx] == MACT_WIN_SNAP_RIGHT ||
               g_menu_action[idx] == MACT_WIN_CLOSE) {
        /* Act on the window the menu was opened over, mirroring the title-bar
         * button handlers exactly. Re-validate the slot in case the window was
         * closed/reaped while the menu was open. */
        int slot = g_menu_target_slot;
        if (slot >= 0 && slot < MAX_WINDOWS && g_windows[slot].used &&
            g_windows[slot].phase != PH_CLOSING &&
            g_windows[slot].phase != PH_MINIMIZING) {
            window_t *win = &g_windows[slot];
            if (g_menu_action[idx] == MACT_WIN_CLOSE) {
                int32_t id = win->win_id;
                begin_close(slot);
                print("[SHELL] close win "); print_num(id); print("\n");
            } else if (g_menu_action[idx] == MACT_WIN_MINIMIZE) {
                begin_minimize(slot);
            } else if (g_menu_action[idx] == MACT_WIN_SNAP_LEFT) {
                begin_snap_to(slot, SNAP_LEFT);
            } else if (g_menu_action[idx] == MACT_WIN_SNAP_RIGHT) {
                begin_snap_to(slot, SNAP_RIGHT);
            } else { /* MACT_WIN_MAXIMIZE: toggle SNAP_MAX (same as title button) */
                if (win->snap_state == SNAP_MAX) {
                    send_configure(win, win->saved_w, win->saved_h);
                    start_geom_tween(win, win->saved_x, win->saved_y,
                                     win->saved_w, win->saved_h);
                    win->snap_state = SNAP_NONE;
                } else {
                    begin_snap_to(slot, SNAP_MAX);
                }
            }
        }
    }
}
/* Returns 1 if the click was consumed (selecting a row, or dismissing). */
static int menu_handle_click(int32_t cx, int32_t cy) {
    if (!g_menu_open) return 0;
    int32_t mh = menu_height();
    if (point_in(cx, cy, g_menu_x, g_menu_y, MENU_W, mh)) {
        int32_t ry = cy - (g_menu_y + MENU_HDR_H);
        if (ry >= 0) {
            int row = ry / MENU_ROW_H;
            if (row >= 0 && row < g_menu_n) menu_run(row);
        }
    }
    g_menu_open = 0;   /* any click dismisses the menu */
    return 1;
}

/* Hit-test the desktop-icon grid at (cx,cy). Returns the icon index, or -1. */
static int desk_icon_at(int32_t cx, int32_t cy, uint32_t W, uint32_t H) {
    for (int i = 0; i < g_desk_count; i++) {
        int32_t tx, ty;
        if (!desk_icon_origin(i, W, H, &tx, &ty)) continue;
        /* hit area = the tile plus its label row (matches the hover rect) */
        if (point_in(cx, cy, tx - 6, ty - 2,
                     DESK_TILE + 12, DESK_TILE + DESK_LABEL_GAP + FONT_H + 4))
            return i;
    }
    return -1;
}

/* Handle a left click on a desktop icon: single click selects (arms double),
 * a second click on the SAME icon within DESK_DBLCLICK_MS launches it:
 *   directory  -> spawn sbin/filemanager
 *   regular fl -> SYS_SPAWN "/Desktop/<name>" (runs IDE-compiled programs)
 * Returns 1 if a desktop icon was clicked (consume), else 0. */
static int desk_handle_click(int32_t cx, int32_t cy, uint32_t W, uint32_t H, long now) {
    int idx = desk_icon_at(cx, cy, W, H);
    if (idx < 0) return 0;

    int is_double = (idx == g_desk_last_idx &&
                     (now - g_desk_last_ms) <= DESK_DBLCLICK_MS &&
                     (now - g_desk_last_ms) >= 0);
    g_desk_last_idx = idx;
    g_desk_last_ms  = now;
    if (!is_double) return 1;        /* first click: select only */

    g_desk_last_idx = -1;            /* reset so a 3rd click starts fresh */
    desk_icon_t *di = &g_desk_icons[idx];

    /* Identity is the full stored path -- never reconstruct it from the
     * (truncated) display label. Dispatch by kind: folders/projects open in the
     * file manager at their real directory; apps/files are spawned directly. */
    if (di->kind == DI_PROJECT) {
        /* AUDIT-9 + IDE-PATH-FIX: open a project folder IN the IDE with its root as
         * argv[1] via SYS_SPAWN_EX_ARGV (NUL-separated argv buffer + byte length +
         * sc6 stdio=0) so a name with a space survives exec's NUL-only split. */
        char ide_av[256]; int ide_n = 0;
        for (const char *pp = di->path; *pp && ide_n < (int)sizeof(ide_av) - 1; ) ide_av[ide_n++] = *pp++;
        ide_av[ide_n++] = '\0';                 /* one NUL-terminated argv[1] entry */
        long r = sc6(SYS_SPAWN_EX_ARGV, (long)"sbin/ide", (long)ide_av, (long)ide_n, 0, 0, 0);
        print("[SHELL] desktop ide "); print(di->path);
        if (r < 0) {
            print(" (ide spawn fail r="); print_num(r); print(" -> filemanager)");
            syscall(SYS_SPAWN, (long)"sbin/filemanager", (long)di->path, 0);
        }
        print("\n");
    } else if (di->kind == DI_FOLDER) {
        long r = syscall(SYS_SPAWN, (long)"sbin/filemanager", (long)di->path, 0);
        print("[SHELL] desktop open "); print(di->path);
        if (r < 0) { print(" (spawn fail r="); print_num(r); print(")"); }
        print("\n");
    } else {
        long r = syscall(SYS_SPAWN, (long)di->path, 0, 0);
        print("[SHELL] desktop run "); print(di->path);
        if (r < 0) { print(" (spawn fail r="); print_num(r); print(")"); }
        print("\n");
    }
    return 1;
}

static void handle_mouse(uint32_t W, uint32_t H) {
    int32_t left      = g_buttons & 1;
    int32_t prev_left = g_prev_buttons & 1;
    int32_t release   = !left && prev_left;     /* falling edge = end drag  */
    int32_t cx = g_cursor_x, cy = g_cursor_y;   /* LIVE cursor (drag track) */

    /* PERF: handle_mouse has ~20 state-changing exit paths (focus/raise, drag-
     * move, snap, minimize, maximize, close, menu open/close, taskbar + dock +
     * icon clicks, app launch). Rather than instrument each one, mark dirty once
     * here whenever there is ANY interaction in flight this frame:
     *   - a pending left-click latch (g_click_latch)  -> a click will dispatch
     *   - a pending right-click latch (g_rclick_latch) -> a context menu may open
     *   - an active drag (g_drag_slot >= 0)            -> the window is moving
     *   - an open popup menu (g_menu_open)             -> hover row may change
     *   - a held/just-released button                  -> drag end / re-focus
     * This deliberately over-marks (a no-op click still repaints one frame),
     * honoring "bias hard toward dirty". Cursor-move-only frames are already
     * marked in pump_input. */
    if (g_click_latch || g_rclick_latch || g_drag_slot >= 0 ||
        g_menu_open  || g_about_open    || left || prev_left)
        mark_dirty();

    /* GHOST FIX (desktop folder icons): the hover glow is painted into the
     * wallpaper layer, OUTSIDE any window's commit rect. A pure cursor move does
     * not recomposite the scene (double-buffered fast path), so on its own the
     * glow neither appears nor is erased; and when a window IS committing, the
     * narrow damage scissor clips the glow region, leaving a stale glow = the
     * reported "folder ghosts behind the IDE". Track the hovered icon; on any
     * transition, mark dirty AND force a few full-screen frames so the old glow
     * is erased and the new one drawn. */
    {
        int dh = desk_icon_at(cx, cy, W, H);
        if (dh != g_desk_hover) {
            g_desk_hover = dh;
            mark_dirty();
            g_full_damage_cooldown = FULL_DAMAGE_COOLDOWN_FRAMES;
        }
    }

    /* Track which menu row the cursor is over (for highlight), every frame. */
    if (g_menu_open) {
        g_menu_hover = -1;
        if (point_in(g_cursor_x, g_cursor_y, g_menu_x, g_menu_y + MENU_HDR_H,
                     MENU_W, g_menu_n * MENU_ROW_H))
            g_menu_hover = (g_cursor_y - (g_menu_y + MENU_HDR_H)) / MENU_ROW_H;
    }

    /* Right-click opens a CONTEXT-AWARE menu. If a live window is under the
     * cursor, the menu acts on THAT window (minimize/maximize/close). Otherwise,
     * on the bare desktop (not over the dock and not over a desktop icon), it
     * offers desktop actions. Right-clicks on the dock/icons fall through. */
    if (g_rclick_latch) {
        g_rclick_latch = 0;
        int in_region = (g_rclick_cx < (int32_t)W - RDOCK_W &&
                         g_rclick_cy > PANEL_H && g_rclick_cy < (int32_t)H - DOCK_H);
        int on_icon = (desk_icon_at(g_rclick_cx, g_rclick_cy, W, H) >= 0);
        int win_slot = -1;   /* topmost live window under the cursor, or -1 */
        for (int32_t i = g_zcount - 1; i >= 0 && win_slot < 0; i--) {
            int slot = (int)g_zorder[i];
            if (slot < 0 || slot >= MAX_WINDOWS || !g_windows[slot].used) continue;
            window_t *win = &g_windows[slot];
            if (win->minimized) continue;
            if (win->phase == PH_CLOSING || win->phase == PH_MINIMIZING) continue;
            if (point_in(g_rclick_cx, g_rclick_cy, win->x, win->y,
                         (int32_t)win->w, (int32_t)win->h + TITLEBAR_H))
                win_slot = slot;
        }
        if (win_slot >= 0) {
            /* over a window: menu acts on that window (raise it first to match
             * the title-bar buttons, which operate on the focused window). */
            focus_window(win_slot);
            open_context_menu(g_rclick_cx, g_rclick_cy, win_slot, W, H);
            g_prev_buttons = g_buttons;
            return;
        }
        /* Right-click on a taskbar button -> context menu for that window. */
        if (g_rclick_cy >= (int32_t)H - DOCK_H) {
            int tb_idx = 0;
            for (int s = 0; s < MAX_WINDOWS; s++) {
                if (!g_windows[s].used) continue;
                if (g_windows[s].phase == PH_CLOSING) { tb_idx++; continue; }
                int32_t bx = taskbtn_x(tb_idx), by = taskbtn_y(H);
                if (bx + TASK_W > (int32_t)W) break;
                if (point_in(g_rclick_cx, g_rclick_cy, bx, by, TASK_W, TASK_H)) {
                    open_context_menu(g_rclick_cx, g_rclick_cy, s, W, H);
                    g_prev_buttons = g_buttons;
                    return;
                }
                tb_idx++;
            }
        }
        if (in_region && !on_icon) {
            open_context_menu(g_rclick_cx, g_rclick_cy, -1, W, H);
            g_prev_buttons = g_buttons;
            return;
        }
    }

    /* ---- active drag-move: track cursor while left held ---- */
    if (g_drag_slot >= 0) {
        window_t *dw = &g_windows[g_drag_slot];
        /* The dragged window may have been closed/force-quit (Alt+K) or reaped
         * mid-drag. If it is no longer a live, non-closing window, abandon the
         * drag so we don't ghost-move a dead slot (or a reused one). */
        if (!dw->used || dw->phase == PH_CLOSING || dw->phase == PH_MINIMIZING) {
            g_drag_slot  = -1;
            g_snap_armed = SNAP_NONE;
            g_prev_buttons = g_buttons;
            return;
        }
        if (left && dw->used) {
            /* M6: dragging a snapped window away un-snaps it back to its saved
             * floating SIZE immediately (no tween mid-drag) and re-anchors the
             * grab so the cursor stays on the titlebar as it follows the mouse. */
            if (dw->snap_state != SNAP_NONE) {
                /* Tell the client to resize its buffer back to the pre-snap
                 * size, matching every other un-snap path (maximize button,
                 * double-click, Alt+Enter, context menu). Without this the
                 * client keeps the oversized snap buffer allocated. */
                send_configure(dw, dw->saved_w, dw->saved_h);
                dw->w = dw->saved_w;               /* restore floating size     */
                dw->h = dw->saved_h;
                dw->snap_state = SNAP_NONE;
                if (dw->phase == PH_SNAPPING) dw->phase = PH_NONE;
                /* re-anchor: keep grab proportional but inside the titlebar */
                g_drag_dx = (int32_t)dw->w / 2;
                g_drag_dy = TITLEBAR_H / 2;
            }
            dw->x = cx - g_drag_dx;
            dw->y = cy - g_drag_dy;
            clamp_window(dw);
            /* M6: arm a snap preview if the cursor reached a screen edge */
            g_snap_armed = snap_zone_for_cursor(cx, cy, W, H);
        }
        if (release) {
            if (g_snap_armed != SNAP_NONE && dw->used) {
                begin_snap_to(g_drag_slot, g_snap_armed);
            } else {
                print("[SHELL] move win ");
                print_num(dw->used ? dw->win_id : -1);
                print("\n");
            }
            g_snap_armed = SNAP_NONE;
            g_drag_slot  = -1;
        }
        g_prev_buttons = g_buttons;
        return;                                  /* drag owns the pointer   */
    }

    /* ---- DOCK-DND-1: a drag handed off from the Start menu via SHM. The
     * compositor owns the cursor + the dock, so it renders the ghost (in
     * render_right_dock) and resolves the drop here: over the dock = pin into
     * the "Pinned" box; elsewhere = cancel. ---- */
    if (g_dnd && g_dnd->magic == DOCKDND_MAGIC && g_dnd->active && !g_dnd->claimed) {
        long dnow = syscall(SYS_GET_TICKS_MS, 0, 0, 0);
        if (left) { mark_dirty(); g_prev_buttons = g_buttons; return; }
        if (cx >= rdock_strip_x(W)) {
            char p[64]; int i = 0;
            for (; i < 63 && g_dnd->path[i]; i++) p[i] = g_dnd->path[i];
            p[i] = 0;
            dock_pin_path(p, g_dnd->color, dnow);
        } else {
            print("[DOCK] start-menu drag cancelled (off dock)\n");
        }
        g_dnd->active  = 0;
        g_dnd->claimed = 1;
        mark_dirty();
        g_prev_buttons = g_buttons;
        return;
    }

    /* ---- DOCK-DND-0: dock icon drag. Armed by rdock_handle_click on a strip
     * press; here we track the held drag and resolve on release: a drag onto
     * another app makes a new box, onto a folder files it; a non-drag press is a
     * launch / folder-toggle, or an Alt+click selection toggle. ---- */
    if (g_dock_press_idx >= 0 || g_dock_drag >= 0) {
        long dnow = syscall(SYS_GET_TICKS_MS, 0, 0, 0);
        if (left) {
            int32_t ddx = cx - g_dock_press_x, ddy = cy - g_dock_press_y;
            if (g_dock_drag < 0 && (ddx * ddx + ddy * ddy) > 36) {  /* >6px = drag */
                g_dock_drag = g_dock_press_idx;
                print("[DOCK] drag start idx="); print_num(g_dock_drag); print("\n");
            }
            if (g_dock_drag >= 0) {
                g_dock_drag_x = cx; g_dock_drag_y = cy;
                int t = rdock_icon_at(cx, cy, W, dnow);
                g_dock_drop_tgt = (t == g_dock_drag) ? -1 : t;
            }
            mark_dirty();
            g_prev_buttons = g_buttons;
            return;
        }
        if (g_dock_drag >= 0) {
            print("[DOCK] drop drag="); print_num(g_dock_drag);
            print(" tgt="); print_num(g_dock_drop_tgt); print("\n");
            dock_drop(g_dock_drag, g_dock_drop_tgt, W, dnow);
        } else if (g_dock_press_idx >= 0) {
            if (g_alt_held && g_dock_press_idx < RDOCK_NICONS)
                g_dock_selected[g_dock_press_idx] = !g_dock_selected[g_dock_press_idx];
            else
                dock_activate(g_dock_press_idx, dnow);
        }
        g_dock_press_idx = -1; g_dock_drag = -1; g_dock_drop_tgt = -1;
        mark_dirty();
        g_prev_buttons = g_buttons;
        return;
    }

    /* Click dispatch is driven by the per-event latch (set in pump_input), not a
     * frame-level button edge — a quick click whose press+release arrive in one
     * input batch would otherwise be lost. Hit-test at the latched press pos. */
    if (!g_click_latch) { g_prev_buttons = g_buttons; return; }
    g_click_latch = 0;
    cx = g_click_cx; cy = g_click_cy;

    /* ---------- click hit-testing (priority order) ---------- */

    /* modal About dialog is topmost: any click dismisses it and is consumed. */
    if (g_about_open) { g_about_open = 0; g_prev_buttons = g_buttons; return; }

    /* 0a) an open popup menu owns the next click (select a row or dismiss). */
    if (menu_handle_click(cx, cy)) { g_prev_buttons = g_buttons; return; }

    /* 0b) top-panel NETWORK INDICATOR: clicking the IP/status label or signal
     *     bars opens the Network Manager so the user can connect to a router /
     *     view local connections. Geometry mirrors render_panel(): the clock is
     *     always "HH:MM:SS" (8 chars), the bars sit clk_w+12+26 from the right,
     *     and the status label is net_w+8 left of the bars. */
    {
        int32_t clk_w = 8 * FONT_W;
        int32_t net_w = (int32_t)k_strlen(g_net_label) * FONT_W;
        int32_t nx    = (int32_t)W - clk_w - 12 - 26;
        int32_t hit_x = nx - net_w - 8;
        int32_t hit_w = (nx + 20) - hit_x;       /* label + 4 signal bars + pad */
        if (hit_x < 0) hit_x = 0;
        if (point_in(cx, cy, hit_x, 0, hit_w, PANEL_H)) {
            print("[SHELL] launch network manager\n");
            syscall(SYS_SPAWN, (long)"sbin/netman", 0, 0);
            g_prev_buttons = g_buttons;
            return;
        }
    }

    /* 0) M8: right dock -- highest priority for clicks in its x-range OR in
     *    an open popover that extends into the workspace. */
    {
        long now_click = syscall(SYS_GET_TICKS_MS, 0, 0, 0);
        /* rdock_handle_click also handles "click outside popover" collapse,
         * so we call it for every click; it returns 1 only if consumed. */
        if (rdock_handle_click(cx, cy, W, now_click)) {
            g_prev_buttons = g_buttons;
            return;
        }
    }

    /* 1) dock launcher button */
    {
        int32_t lx = launcher_x(), ly = launcher_y(H);
        if (point_in(cx, cy, lx, ly, LAUNCH_SZ, LAUNCH_SZ)) {
            /* Launch the Windows-11-style start-menu app (replaces the old
             * built-in popup). It renders as its own window and closes itself
             * after launching an app. (void) the old helper to avoid -Wunused. */
            (void)open_start_menu;
            syscall(SYS_SPAWN, (long)"sbin/startmenu", 0, 0);
            g_prev_buttons = g_buttons;
            return;
        }
    }

    /* 2) taskbar buttons: focus+raise a normal window, or RESTORE a minimized
     *    one. Iterate visible windows in slot order to match render_dock.
     *    Skip windows in the close animation: clicking their (still-visible)
     *    taskbar button during the fade-out must NOT refocus or re-raise them. */
    {
        int idx = 0;
        for (int s = 0; s < MAX_WINDOWS; s++) {
            if (!g_windows[s].used) continue;
            if (g_windows[s].phase == PH_CLOSING) { idx++; continue; }
            int32_t bx = taskbtn_x(idx), by = taskbtn_y(H);
            if (bx + TASK_W > (int32_t)W) break;
            if (point_in(cx, cy, bx, by, TASK_W, TASK_H)) {
                if (g_windows[s].minimized || g_windows[s].phase == PH_MINIMIZING)
                    begin_restore(s);
                else if (s == focused_slot())
                    begin_minimize(s);   /* toggle: click active app -> minimize */
                else
                    focus_window(s);
                g_prev_buttons = g_buttons;
                return;
            }
            idx++;
        }
    }

    /* 3) windows, FRONT to back (topmost first). Skip parked/closing windows. */
    for (int32_t i = g_zcount - 1; i >= 0; i--) {
        int slot = (int)g_zorder[i];
        if (slot < 0 || slot >= MAX_WINDOWS || !g_windows[slot].used) continue;
        window_t *win = &g_windows[slot];
        if (win->minimized) continue;
        if (win->phase == PH_CLOSING || win->phase == PH_MINIMIZING) continue;
        int32_t fx = win->x, fy = win->y;
        int32_t cw = (int32_t)win->w;
        int32_t full_h = (int32_t)win->h + TITLEBAR_H;

        /* outside this window's whole frame? keep searching lower windows */
        if (!point_in(cx, cy, fx, fy, cw, full_h)) continue;

        /* 3a) titlebar close box */
        int32_t close_x = fx + cw - CLOSE_SZ - 8;
        int32_t close_y = fy + (TITLEBAR_H - CLOSE_SZ) / 2;
        if (point_in(cx, cy, close_x, close_y, CLOSE_SZ, CLOSE_SZ)) {
            int32_t id = win->win_id;
            begin_close(slot);
            print("[SHELL] close win "); print_num(id); print("\n");
            g_prev_buttons = g_buttons;
            return;
        }

        /* 3a2) titlebar minimize box (left of the close box) */
        int32_t min_x = close_x - MIN_SZ - 6;
        int32_t min_y = fy + (TITLEBAR_H - MIN_SZ) / 2;
        if (point_in(cx, cy, min_x, min_y, MIN_SZ, MIN_SZ)) {
            begin_minimize(slot);
            g_prev_buttons = g_buttons;
            return;
        }

        /* 3a3) titlebar maximize/restore box (left of minimize) -> toggle SNAP_MAX */
        int32_t max_x = min_x - MIN_SZ - 6;
        int32_t max_y = fy + (TITLEBAR_H - MIN_SZ) / 2;
        if (point_in(cx, cy, max_x, max_y, MIN_SZ, MIN_SZ)) {
            if (win->snap_state == SNAP_MAX) {
                /* restore: shrink the client buffer back, then tween to the
                 * saved pre-maximize geometry */
                send_configure(win, win->saved_w, win->saved_h);
                start_geom_tween(win, win->saved_x, win->saved_y,
                                 win->saved_w, win->saved_h);
                win->snap_state = SNAP_NONE;
            } else {
                begin_snap_to(slot, SNAP_MAX);   /* maximize to the work area */
            }
            g_prev_buttons = g_buttons;
            return;
        }

        /* 3b) titlebar (not a button) -> focus + double-click maximize toggle + drag */
        if (point_in(cx, cy, fx, fy, cw, TITLEBAR_H)) {
            focus_window(slot);
            /* Double-click detection: toggle maximize on the same window. */
            {
                long tb_now = syscall(SYS_GET_TICKS_MS, 0, 0, 0);
                int is_dbl = (slot == g_tb_last_slot &&
                              (tb_now - g_tb_last_ms) <= TB_DBLCLICK_MS &&
                              (tb_now - g_tb_last_ms) >= 0);
                g_tb_last_slot = slot;
                g_tb_last_ms   = tb_now;
                if (is_dbl) {
                    g_tb_last_slot = -1;   /* reset so a 3rd click starts fresh */
                    if (win->snap_state == SNAP_MAX) {
                        send_configure(win, win->saved_w, win->saved_h);
                        start_geom_tween(win, win->saved_x, win->saved_y,
                                         win->saved_w, win->saved_h);
                        win->snap_state = SNAP_NONE;
                    } else {
                        begin_snap_to(slot, SNAP_MAX);
                    }
                    g_prev_buttons = g_buttons;
                    return;
                }
            }
            if (left) {   /* button still held -> begin drag; quick click = focus only */
                g_drag_slot = slot;
                g_drag_dx = cx - win->x;
                g_drag_dy = cy - win->y;
            }
            g_prev_buttons = g_buttons;
            return;
        }

        /* 3c) window body -> focus + raise (let the client handle the click) */
        focus_window(slot);
        send_pointer_to_focus();
        g_prev_buttons = g_buttons;
        return;
    }

    /* 3.5) desktop icons on the bare wallpaper (no window was hit above).
     *      Double-click launches (dir -> filemanager, file -> /Desktop/name). */
    {
        long now_dc = syscall(SYS_GET_TICKS_MS, 0, 0, 0);
        if (desk_handle_click(cx, cy, W, H, now_dc)) {
            g_prev_buttons = g_buttons;
            return;
        }
    }

    /* 4) clicked empty desktop / chrome with nothing actionable: no-op */
    g_prev_buttons = g_buttons;
}

/* ====================================================================== */
void _start(void) {
    print("[SHELL] M8 right-dock: 12 icons, 2 folders\n");

    for (int i = 0; i < MAX_WINDOWS; i++) { g_windows[i].used = 0; g_windows[i].phase = PH_NONE; }
    g_zcount = 0;
    g_mru_count = 0;                                /* M6: empty MRU ring        */

    /* M8: initialise right-dock state */
    for (int i = 0; i < RDOCK_CAP; i++) {
        g_rdock_icons[i].scale_q8      = 256;
        g_rdock_icons[i].scale_target  = 256;
        g_rdock_icons[i].bounce_active = 0;
        g_rdock_icons[i].bounce_start  = 0;
    }
    for (int fi = 0; fi < DOCK_FOLDERS_MAX; fi++) {
        g_rdock_folders[fi].open         = 0;
        g_rdock_folders[fi].anim_t       = 0;
        g_rdock_folders[fi].anim_start   = 0;
        g_rdock_folders[fi].anim_closing = 0;
    }
    /* DOCK-DND-0: seed the mutable folder table from the const rdock_folders[];
     * g_app_hidden stays all-0 so the initial dock is byte-identical to before. */
    g_nfolders = RDOCK_NFOLDERS;
    for (int fi = 0; fi < g_nfolders; fi++) {
        const rdock_folder_t *s = &rdock_folders[fi];
        int li = 0;
        while (s->label && s->label[li] && li < 3) { g_folders[fi].label[li] = s->label[li]; li++; }
        g_folders[fi].label[li] = 0;
        g_folders[fi].color    = s->color;
        g_folders[fi].nmembers = s->nmembers;
        for (int m = 0; m < s->nmembers && m < DOCK_MEMBERS_MAX; m++)
            g_folders[fi].members[m] = s->members[m];
    }
    for (int i = 0; i < RDOCK_NICONS; i++) g_app_hidden[i] = 0;
    g_rdock_hovered     = -1;
    g_rdock_open_folder = -1;

    /* DOCK-DND-1: create + own the Start-menu -> dock drag handoff page (the
     * Start menu attaches lookup-only and writes a pending drag here). */
    {
        /* 0600: owner-only. The producer (Start menu) is a same-UID init child,
         * so it still attaches; a different non-root UID cannot inject a drag.
         * Defense-in-depth only -- the real arbitrary-spawn guard is the path
         * allowlist in dock_pin_path (the SHM path is attacker-influenceable). */
        long id = sc6(SYS_SHMGET, (long)DOCKDND_SHM_KEY, (long)DOCKDND_SHM_SIZE,
                      (long)(IPC_CREAT | 0600), 0, 0, 0);
        if (id >= 0) {
            long p = sc6(SYS_SHMAT, id, 0, 0, 0, 0, 0);
            if (p > 0) {
                g_dnd = (volatile dockdnd_shm_t *)p;
                volatile char *z = (volatile char *)p;      /* sys_shmget won't zero */
                for (int i = 0; i < (int)sizeof(dockdnd_shm_t); i++) z[i] = 0;
                g_dnd->magic = DOCKDND_MAGIC;
                print("[SHELL] DOCK-DND: handoff page ready\n");
            }
        }
    }

    /* SYNTHINPUT-0: create + own the synthetic-input injection page. Agent tools
     * (tool_mouse/tool_key) attach lookup-only and enqueue events; pump_synth_input()
     * drains them each frame. 0600 owner-only (same defense-in-depth as dockdnd; the
     * real authority is the agent gate that whitelists mouse/key). active=1 means
     * injection is authorised; a future cockpit STOP sets active=0 to kill it. */
    {
        long id = sc6(SYS_SHMGET, (long)SYNTHINPUT_SHM_KEY, (long)SYNTHINPUT_SHM_SIZE,
                      (long)(IPC_CREAT | 0600), 0, 0, 0);
        if (id >= 0) {
            long p = sc6(SYS_SHMAT, id, 0, 0, 0, 0, 0);
            if (p > 0) {
                g_synth = (volatile synthinput_shm_t *)p;
                volatile char *z = (volatile char *)p;      /* sys_shmget won't zero */
                for (int i = 0; i < (int)sizeof(synthinput_shm_t); i++) z[i] = 0;
                g_synth->active = 1;
                g_synth->magic  = SYNTHINPUT_MAGIC;          /* publish LAST */
                print("[SHELL] SYNTHINPUT: injection page ready\n");
            }
        }
    }

    /* 1. Acquire the framebuffer. */
    fb_acquire_t fb;
    long r = syscall(SYS_FB_ACQUIRE, (long)&fb, 0, 0);
    if (r != 0) {
        print("[SHELL] fb_acquire FAILED r="); print_num(r); print("\n");
        for (;;) syscall(SYS_YIELD, 0, 0, 0);
    }
    uint32_t W = fb.width, H = fb.height;
    uint32_t stride = fb.pitch / 4;
    g_fb_w = W; g_fb_h = H;
    print("[SHELL] fb "); print_num(W); print("x"); print_num(H);
    print(" pitch="); print_num(fb.pitch);
    print(" bpp="); print_num(fb.bpp); print("\n");

    /* Resolution-adaptive UI scale. 65% (5x10 cell) on 1280x800 and below: the
     * old 50% (4x8) dropped every other source column/row of the 8x16 glyphs,
     * collapsing strokes into an UNREADABLE smear on the T410 panel. 65% keeps
     * 5 of 8 columns and 10 of 16 rows -- materially more legible while still
     * giving ~256 cols x 80 rows. font2_draw_cell_clip nearest-neighbor down-
     * samples from the 8x16 source so sub-100% works (mild per-glyph unevenness
     * at fractional scale is expected; 100% is the artifact-free fallback if 65%
     * is still too small). Alt+wheel zooms live (floor 50%, ceiling 200%). All
     * chrome bar heights derive from FONT_H so they track automatically. Larger
     * displays keep 130% (10x20). */
    /* UI-CRISP-0: INTEGER native scale at every resolution. The old 65%/130%
     * fractional scales made font2 nearest-neighbor-replicate the 8x16 glyph
     * unevenly = blur. 100% (8x16) is artifact-free; Alt+wheel zooms to 200%
     * (also integer/crisp). Chrome bar heights derive from FONT_H and re-track. */
    (void)W; (void)H;
    cz_set_scale(100);
    print("[SHELL] UI scale "); print_num(g_ui_pct); print("% cell ");
    print_num(g_cell_w); print("x"); print_num(g_cell_h); print("\n");

    /* Initial taskbar button width (before any windows are open). */
    g_task_w = TASK_W_MAX;

    /* 2. Allocate the back buffer. */
    size_t bb_bytes = (size_t)fb.pitch * H;
    long bbp = syscall(SYS_MMAP, 0, (long)bb_bytes, VMM_PROT_READ | VMM_PROT_WRITE);
    uint32_t *hw   = (uint32_t *)fb.vaddr;
    uint32_t *back;
    if (bbp > 0) {
        back = (uint32_t *)bbp;
        print("[SHELL] back buffer OK bytes="); print_num((long)bb_bytes); print("\n");
    } else {
        back = hw;
        print("[SHELL] back buffer mmap FAILED ("); print_num(bbp);
        print(") -- rendering direct to fb\n");
    }

    /* Boot cross-fade DISABLED. It used to snapshot the kernel splash by reading
     * the entire hardware framebuffer (`for i in W*H: splash[i] = hw[i]`). On real
     * hardware (e.g. the ThinkPad T410) the framebuffer is UNCACHED MMIO, so each
     * of those ~1,000,000 reads is a slow uncached bus transaction -- the whole
     * capture takes seconds to MINUTES, during which the compositor is wedged here
     * BEFORE its first composite. The result: the kernel boot screen stays on
     * screen, the desktop never appears, and the (cooperative) system looks frozen.
     * On QEMU the FB is RAM-backed so the read was instant and the bug was hidden.
     *
     * Reading an uncached framebuffer is never acceptable. Leave `splash` NULL:
     * the frame loop's `boot_fading` guard (back != hw && splash && ...) then stays
     * false, so the compositor goes straight to a normal full present -- the desktop
     * just appears (one bounded full-screen WRITE, then fast dirty-rect updates)
     * instead of cross-fading. Cosmetic loss only. (NEVER read hw[]/the FB.) */
    uint32_t *splash = (uint32_t *)0;

    /* Previous-frame buffer for dirty-rectangle present (perf). present_diff()
     * writes only the changed bounding box to the slow framebuffer each frame.
     * Zero-filled by mmap, so the first present_diff sees an all-different frame
     * and full-copies once, then tracks deltas. Only used when double-buffered. */
    uint32_t *prev = (uint32_t *)0;
    if (back != hw) {
        long pvp = syscall(SYS_MMAP, 0, (long)bb_bytes,
                           VMM_PROT_READ | VMM_PROT_WRITE);
        if (pvp > 0) {
            prev = (uint32_t *)pvp;
            print("[SHELL] dirty-rect present enabled\n");
        }
    }

    /* Center-to-corner distance: the radius the circular boot iris grows to so
     * it fully covers the screen by the end of the transition. */
    uint32_t _half_w = W / 2, _half_h = H / 2;
    uint32_t max_radius = isqrt32(_half_w * _half_w + _half_h * _half_h);

    long boot_ms = syscall(SYS_GET_TICKS_MS, 0, 0, 0);

    /* 3. Create the server command inbox (SysV message queue). */
    long inbox = sc6(SYS_MSGGET, (long)WL_COMP_INBOX_KEY,
                     (long)(IPC_CREAT | 0666), 0, 0, 0, 0);
    if (inbox < 0) {
        print("[SHELL] msgget(inbox) FAILED r="); print_num(inbox);
        print(" -- continuing without IPC (no clients)\n");
    } else {
        print("[SHELL] inbox queue id="); print_num(inbox);
        print(" key=0x434F4D50\n");
    }
    int32_t inbox_qid = (int32_t)inbox;

    /* 4. Open input devices non-blocking. */
    g_kbd_fd   = (int32_t)syscall(SYS_OPEN, (long)"/dev/input/event0",
                                  (long)(O_RDONLY | O_NONBLOCK), 0);
    g_mouse_fd = (int32_t)syscall(SYS_OPEN, (long)"/dev/input/event1",
                                  (long)(O_RDONLY | O_NONBLOCK), 0);
    if (g_kbd_fd < 0)
        print("[SHELL] WARN: /dev/input/event0 (kbd) unavailable -- degraded\n");
    else { print("[SHELL] keyboard fd="); print_num(g_kbd_fd); print("\n"); }
    if (g_mouse_fd < 0)
        print("[SHELL] WARN: /dev/input/event1 (mouse) unavailable -- degraded\n");
    else { print("[SHELL] mouse fd="); print_num(g_mouse_fd); print("\n"); }

    /* cursor starts at screen center */
    g_cursor_x = (int32_t)(W / 2);
    g_cursor_y = (int32_t)(H / 2);

    /* M8: initial scan of /Desktop so compiled programs show as icons */
    desk_scan();

    /* M8: the welcome toast is REMOVED (was: toast_show("AutomationOS M8 —
     * right dock ready", 3500)). On the T410 it presented as the top-right
     * "flashing square with changing text": the box never left the screen
     * (expiry only takes effect when a post-expiry recomposite is actually
     * PRESENTED -- an idle desktop never presents that frame, so the last
     * presented frame keeps the toast forever) and per-second clock repaints
     * made its fade alpha jump around. The em-dash also rendered as garbage
     * glyphs (multi-byte UTF-8 through the 8x16 ASCII font). The toast
     * machinery (toast_show/render_toast) stays for future notifications;
     * any future caller must also fix the present-on-expiry gap. */

#ifdef SELFHEAL
    /* Attach + publish the liveness heartbeat before the loop starts beating it. */
    selfheal_init();
    /* v2: on a RESPAWN (cwatchdog recovery or crash), rebuild the window table
     * from the registry the previous instance mirrored -- recovery that loses
     * every open window reads as "self heal is not working". Runs before the
     * frame loop, so queued client commits find their windows again. */
    if (g_sh_respawn) selfheal_restore_windows(W, H);
#endif

    /* 4b. Probe the CMOS RTC via SYS_GETTIME so the panel clock can show
     * real wall-clock time instead of uptime.  If the syscall is not wired
     * (returns non-zero) we silently fall back to uptime formatting. */
    {
        comp_rtc_time_t probe;
        long rr = syscall(SYS_GETTIME, (long)&probe, 0, 0);
        if (rr == 0 && probe.year >= 2000 && probe.year <= 2099) {
            g_rtc_ok = 1;
            g_rtc    = probe;
            print("[SHELL] RTC wall-clock available\n");
        } else {
            g_rtc_ok = 0;
            print("[SHELL] RTC unavailable, clock shows uptime\n");
        }
    }

    /* 5. Frame loop. */
    long t0 = syscall(SYS_GET_TICKS_MS, 0, 0, 0);
    long next = t0;
    uint64_t frame = 0;
    long last_clock_sec = -1;          /* PERF: last second the panel clock showed */
    g_last_frame_ms = t0;              /* seed frame-time measurement              */

    print("[SHELL] entering frame loop\n");
    for (;;) {
        long now = syscall(SYS_GET_TICKS_MS, 0, 0, 0);

        /* a) drain client requests */
        if (inbox_qid >= 0) drain_inbox(inbox_qid);

        /* a2) reflow taskbar button widths to fit the current window count */
        task_reflow(W);

        /* b) pump input devices (updates cursor + buttons, forwards kbd/ptr) */
        pump_synth_input(W, H);            /* SYNTHINPUT-0: drain agent injection FIRST */
        pump_input(g_kbd_fd,   1, W, H);
        pump_input(g_mouse_fd, 0, W, H);

        /* c) resolve desktop-shell mouse interaction (click/drag/close/launch) */
        handle_mouse(W, H);

        /* d) advance animations (may destroy slots whose CLOSE finished) */
        anim_tick(now);

        /* d2) liveness sweep (~2x/sec): drop windows whose client exited or was
         * force-quit (Alt+K / Task Manager) without a clean WL_REQ_DESTROY. */
        if ((frame % 30) == 0) reap_dead_windows();

        /* d3) periodically rescan /Desktop so newly compiled/created items
         * (e.g. IDE output, New Folder) appear without a restart. */
        if ((frame % DESK_RESCAN_FRAMES) == 0) desk_scan();

        /* d4) PERF: the panel clock shows HH:MM:SS, so it must repaint once per
         * second even on an otherwise-idle desktop. Pulse dirty when the second
         * rolls over; present_diff then pushes only the tiny clock rect. This is
         * the one perpetual "animation" the gate must never starve.
         * When the RTC is available we also refresh g_rtc here (one
         * SYS_GETTIME per second -- negligible cost). */
        {
            long sec = now / 1000;
            if (sec != last_clock_sec) {
                last_clock_sec = sec;
                refresh_rtc();          /* update wall-clock snapshot */
                refresh_net_status();   /* update network indicator   */
                refresh_battery();      /* update battery indicator   */
                mark_dirty();
#ifdef SELFHEAL
                /* v2: re-mirror the window table once per second so the
                 * recovery registry tracks moves/snaps/titles without
                 * hooking every mutation site. 16 entries of plain stores. */
                selfheal_reg_sync();
#endif
            }
        }

        /* d5) PERF: while the boot iris cross-fade is still running it changes
         * every frame -- force dirty so the gate can't skip it. */
        int boot_fading = (back != hw && splash && (now - boot_ms) >= 0 &&
                           (now - boot_ms) < BOOT_FADE_MS);
        if (boot_fading) mark_dirty();

        /* e) composite + present -- BUT ONLY WHEN DIRTY. composite() re-blits all
         * windows (cost scales with window count), and present writes the FB, so
         * skipping both on a genuinely-unchanged frame is the main perf win for
         * "many apps open but mostly idle". Every change path sets g_dirty (see
         * mark_dirty() call sites: input, window lifecycle, client commits,
         * animations, dock hover, the per-second clock pulse above). When NOT
         * dirty we present nothing and just yield. For the first BOOT_FADE_MS we
         * fluidly cross-fade the kernel boot splash into the desktop. */
        if (g_dirty || g_cursor_moved) {
            /* No double buffer (mmap failed, back==hw) -> no cursor fast path is
             * possible; recomposite the whole scene on any change and draw the
             * cursor directly. The fast path below needs back!=hw + prev. */
            int scene = g_dirty || (back == hw && g_cursor_moved);
            g_dirty = 0;                        /* consume before drawing so a
                                                * change DURING this frame's draw
                                                * (e.g. a late inbox msg next
                                                * iteration) re-arms cleanly.    */

            /* DAMAGE SCISSOR: narrow the active scissor to the accumulated damage
             * rect when all conditions are met, so composite() only rasterizes the
             * dirty region.  Otherwise keep the full-screen scissor (safe fallback).
             * The cooldown ensures CREATE/DESTROY/RESIZE get N full-screen frames
             * so z-order / shadow / wallpaper-reveal changes settle cleanly. */
#if COMP_DAMAGE_SCISSOR
            if (scene && g_dmg_any && !boot_fading && g_full_damage_cooldown <= 0) {
                g_scis_x0 = g_dmg_x0; g_scis_y0 = g_dmg_y0;
                g_scis_x1 = g_dmg_x1; g_scis_y1 = g_dmg_y1;
            } else {
                scissor_reset_full();
                if (g_full_damage_cooldown > 0) g_full_damage_cooldown--;
            }
#else
            scissor_reset_full();
            if (g_full_damage_cooldown > 0) g_full_damage_cooldown--;
#endif
            /* Recomposite the cursor-less SCENE only when something other than the
             * pointer changed. A pure pointer move skips this (the expensive bit:
             * re-blitting every window + the full back-vs-prev scan) and just
             * slides the cursor sprite below. */
            if (scene) composite(back, W, H, stride, g_cursor_x, g_cursor_y, now);
            /* Reset scissor + damage after composite so stray draws between frames
             * default to full-screen (safe) and the next frame starts clean. */
            scissor_reset_full();
            damage_reset();
            if (back != hw) {
                if (boot_fading) {
                    uint32_t t = (uint32_t)(((now - boot_ms) * 256) / BOOT_FADE_MS);
                    present_circle_iris(hw, back, splash, W, H, stride, max_radius, t);
                    present_cursor(hw, back, prev, g_cur_drawn_x, g_cur_drawn_y,
                                   g_cursor_x, g_cursor_y, W, H, stride);
                    g_cur_drawn_x = g_cursor_x; g_cur_drawn_y = g_cursor_y;
                    g_present_px = W * H;        /* iris touches the whole screen */
                    g_present_did = 1;
                } else {
                    uint32_t px = 0;
                    if (scene) {
                        if (prev) px = present_diff(hw, back, prev, W, H, stride);
                        else      { present(hw, back, H, stride); px = W * H; }
                        splash = (uint32_t *)0;  /* fade complete: stop blending */
                    }
                    /* Slide/overlay the cursor sprite (tiny: 2 small rects). Runs
                     * every frame the pointer moved -- this is the smooth-mouse,
                     * no-bottom-lag path -- and also re-asserts the sprite over a
                     * fresh scene present. */
                    px += present_cursor(hw, back, prev, g_cur_drawn_x, g_cur_drawn_y,
                                         g_cursor_x, g_cursor_y, W, H, stride);
                    g_cur_drawn_x = g_cursor_x; g_cur_drawn_y = g_cursor_y;
                    g_present_px = px;
                    g_present_did = (px != 0);
                }
            } else {
                /* back==hw: composite() wrote the cursor-less scene straight to the
                 * framebuffer; drop the cursor sprite on top. */
                draw_cursor(hw, W, H, stride, g_cursor_x, g_cursor_y);
                g_present_px = W * H;
                g_present_did = 1;
            }
            g_cursor_moved = 0;

            /* PERF: sample frame-time + FPS at each PRESENTED frame (idle frames
             * that skip the draw don't count, so FPS reflects real work). Smooth
             * FPS with a simple IIR so the overlay number is readable. */
            g_frame_dt_ms = now - g_last_frame_ms;
            if (g_frame_dt_ms < 0) g_frame_dt_ms = 0;
            g_last_frame_ms = now;
            if (g_frame_dt_ms > 0) {
                long inst_fps_x10 = 10000 / g_frame_dt_ms;   /* (1000/dt)*10 */
                if (g_fps_x10 == 0) g_fps_x10 = inst_fps_x10;
                else g_fps_x10 += (inst_fps_x10 - g_fps_x10) / 4;  /* IIR a=1/4 */
            }

            /* DESKTOP-SPLIT-0 evidence: surface the already-computed FPS on
             * serial every ~30s so a smoke can compare a split boot against
             * a baseline boot ("fps_ge_baseline"). One line per window,
             * counted on PRESENTED frames only (idle frames don't present,
             * so the number reflects real compositing work). Negligible
             * steady-state cost (a counter + one print/30s). */
            {
                static long fpswin_start = 0;
                static long fpswin_frames = 0;
                fpswin_frames++;
                if (fpswin_start == 0) fpswin_start = now;
                if (now - fpswin_start >= 30000) {
                    print("[COMP] fps window: frames=");
                    print_num(fpswin_frames);
                    print(" fps_x10=");
                    print_num(g_fps_x10);
                    print(" over_ms=");
                    print_num(now - fpswin_start);
                    print("\n");
                    fpswin_start = now;
                    fpswin_frames = 0;
                }
            }
        }

        /* GUI-PERF mouse fix: a heavy composite (e.g. the IDE open -> ~50ms/frame)
         * leaves the cursor FROZEN at its loop-top position for the whole frame,
         * because input is pumped only once at the top (above). Re-pump the pointer
         * and slide the sprite to its FRESH position at frame END so it tracks the
         * mouse at frame rate instead of lagging a whole composite behind. Cheap:
         * present_cursor is 2 sprite rects copied from back/prev (the cursor-less
         * scene) so it can never source/leave stale pixels; and it never enters
         * handle_mouse/drag (those run only at the loop top), so no re-entrancy. */
        if (back != hw && prev) {
            pump_input(g_mouse_fd, 0, W, H);
            if (g_cursor_moved) {
                present_cursor(hw, back, prev, g_cur_drawn_x, g_cur_drawn_y,
                               g_cursor_x, g_cursor_y, W, H, stride);
                g_cur_drawn_x = g_cursor_x; g_cur_drawn_y = g_cursor_y;
                g_cursor_moved = 0;
            }
        }

        frame++;
        g_wheel_delta = 0;  /* reset wheel accumulator for next frame */
        /* Per-frame serial logging REMOVED. It wrote "[SHELL] frame N" to fd 1
         * (=serial) every 60 frames; on real hardware where COM1 exists but is
         * undrained (the T410), each such write spun the kernel UART driver with
         * interrupts disabled for ~hundreds of ms, freezing the timer and the
         * whole cooperative desktop ~1s after boot. The kernel now auto-disables a
         * wedged UART, but the compositor should not spam serial every frame in
         * the steady state regardless. (Was: print frame/window count.) */

        /* f) Frame pacing. On the COOPERATIVE scheduler the old code busy-YIELDED
         * until the 16ms budget elapsed -- but a yielding process stays RUNNABLE,
         * so the compositor only re-checked the clock when the round-robin cycled
         * back to it, massively OVERSHOOTING the budget (frames ~100ms+ => ~9 FPS
         * with several apps open). Instead BLOCK-SLEEP the remainder: the kernel
         * drops us from the runqueue, runs the clients, and wakes us right at the
         * deadline -- so we present on a steady ~60Hz cadence and clients still get
         * their full share of CPU. If we overran the budget, just yield once. */
        now = syscall(SYS_GET_TICKS_MS, 0, 0, 0);
        next += 16;
        long sleep_ms = next - now;
        if (sleep_ms > 16) sleep_ms = 16;              /* clamp after a stall */
        if (sleep_ms >= 1) syscall(SYS_SLEEP, sleep_ms, 0, 0);
        else               syscall(SYS_YIELD, 0, 0, 0);
        now = syscall(SYS_GET_TICKS_MS, 0, 0, 0);
        if (next < now - 64) next = now;     /* resync after a clock jump */

#ifdef SELFHEAL
        /* Heartbeat: stamp the loop's liveness ONCE PER ITERATION (incl. idle,
         * present-skipped frames) so a freeze of EITHER kind stalls the counter.
         * Two volatile stores; zero syscalls on this hot path. */
        if (g_hb) { g_hb->frame_counter = frame; g_hb->last_frame_ms = (unsigned long long)now; }
#endif
#ifdef SELFHEAL_FREEZE
        /* Forced-freeze PROOF (one-shot via the magic latch set in selfheal_init):
         * only the FIRST compositor instance freezes; the respawn runs normally,
         * so cwatchdog proves recovery with exactly ONE freeze (no breaker storm). */
        if (g_freeze_at && frame >= g_freeze_at) {
            print("[SHELL] FREEZE_TEST: entering freeze mode ");
            print_num(FREEZE_MODE); print("\n");
#if FREEZE_MODE == 0
            /* blocking freeze: stuck off the frame loop but YIELDING the CPU, so
             * the watchdog runs on the cooperative kernel. Short repeating sleep
             * (not one 1000s sleep) so a pending SIGKILL is delivered within a
             * poll and no long-lived timer entry is stranded after teardown. */
            for (;;) syscall(SYS_SLEEP, 250, 0, 0);
#else
            for (;;) { __asm__ volatile("" ::: "memory"); } /* tight loop: never yields -> needs PREEMPT */
#endif
        }
#endif
    }
}
