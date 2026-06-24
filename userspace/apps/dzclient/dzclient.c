/*
 * dzclient -- the DeadZone LIVE loopback co-op proof (DEADZONE-MP-LIVE-0).
 *
 * A headless, pure-syscall TCP client that connects to a running authoritative
 * deadzoned (sbin/deadzoned) over the kernel's loopback datapath -- 127.0.0.1,
 * the ip_tx 127/8 short-circuit (kernel/net/socket.c) -- which delivers a full
 * SYN/SYN-ACK/ACK handshake + bidirectional TCP locally with NO NIC, no ARP and
 * no -netdev. It then plays a scripted authoritative session:
 *
 *   each tick:  steer our player toward the nearest alive zombie (from the last
 *               server snapshot) + hold FIRE, send DZ_INPUT, poll DZ_SNAPSHOT.
 *
 * and verifies the server applied OUR inputs authoritatively -- our player moved
 * from the spawn point (server consumed our move) AND scored kills (server
 * consumed our fire via its own hitscan) -- and that we decoded the broadcast
 * snapshots. This is the LIVE end-to-end proof that the (until now dormant)
 * multiplayer seam in sbin/deadzone actually speaks to sbin/deadzoned over real
 * TCP. It is the first userspace two-process loopback-TCP exchange in the OS.
 *
 * Prints (deterministic, server uses an LCG so the world is reproducible):
 *   "DEADZONE: mp LIVE PASS frames=.. snaps=.. wave=.. kills=.. moved=1 zalive=.."
 *   "DEADZONE: mp LIVE FAIL <why>"
 *
 * Shares the wire protocol VERBATIM with the server + game via dzproto.h, so the
 * bytes it sends/parses are exactly what the server reads/emits (they cannot
 * drift). Spawned ONLY by an init built with -DDZ_MPLIVE (build with DZ_MPLIVE=1)
 * -- the default boot never runs it, so it is byte-behaviorally invisible.
 *
 * Build (host gcc cross-compile via build_all.sh, identical CF/LD to deadzoned):
 *   cc ... -c userspace/apps/dzclient/dzclient.c -o /tmp/dzclient.o
 *   ld ... /tmp/crt0.o /tmp/dzclient.o -o /tmp/dzclient.elf
 */

/* ---- syscall numbers (AutomationOS ABI -- see kernel/include/syscall.h) -- */
#define SYS_EXIT        0
#define SYS_WRITE       3    /* write(fd, buf, len)           fd1 = stdout    */
#define SYS_SLEEP       9    /* sleep(ms) -- real blocking ms sleep            */
#define SYS_YIELD       15   /* cooperative yield                             */
#define SYS_SOCKET      51   /* socket(SOCK_STREAM) -> fd                     */
#define SYS_CONNECT     52   /* connect(fd, ip_host_order, port) -> 0/neg     */
#define SYS_SEND        53   /* send(fd, buf, len) -> bytes                   */
#define SYS_RECV        54   /* recv(fd, buf, len) -> bytes/0(closed)/-11     */
#define SYS_CLOSE_SK    55   /* close(socket fd) -> 0                          */
#define SYS_SOCK_POLL   58   /* pump the stack before recv                    */

#define SOCK_STREAM     1
#define FD_STDOUT       1

#define SOCK_OK          0
#define SOCK_EAGAIN    (-11)

/* 127.0.0.1 in HOST byte order. SYS_CONNECT takes the ip host-ordered, and the
 * loopback short-circuit gates on (ip >> 24) == 127 -- so this is 0x7F000001,
 * NOT a byte-swapped big-endian form. */
#define LOOPBACK_IP     0x7F000001u
#define DZ_GAME_PORT    27015     /* == deadzoned DEFAULT_PORT (no -p given)    */

#define CONNECT_TRIES   40        /* retry: the server may still be binding    */
#define CONNECT_WAIT_MS 100
#define PLAY_FRAMES     220       /* ~7.3s at 33ms -- well within smoke timeout */
#define TICK_MS         33        /* match the server's ~30 Hz tick            */
#define MOVE_MAX        60        /* == server PLAYER_SPEED (input is clamped)  */
#define MOVED_EPS       200       /* world units of net displacement = "moved"  */
#define Z_ALIVE_WIRE    1         /* dz_zombie_t.state == 1 (server's Z_ALIVE)  */

typedef unsigned int   u32;
typedef int            i32;
typedef unsigned char  u8;

/* ONE protocol definition shared with sbin/deadzoned + sbin/deadzone. */
#include "../deadzoned/dzproto.h"

/* ===================================================================== */
/* Raw 6-arg inline syscall (verbatim from deadzoned.c / httpd.c)         */
/* ===================================================================== */
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

/* ---- tiny freestanding I/O helpers (verbatim from deadzoned.c) -------- */
static unsigned int s_len(const char *s)
{
    unsigned int n = 0;
    if (!s) return 0;
    while (s[n]) n++;
    return n;
}
static void out_write(const char *s, unsigned int n)
{
    sc(SYS_WRITE, FD_STDOUT, (long)s, (long)n, 0, 0);
}
static void out_puts(const char *s) { out_write(s, s_len(s)); }
static void out_unum(unsigned long v)
{
    char b[24]; int i = 0;
    do { b[i++] = (char)('0' + (v % 10)); v /= 10; } while (v > 0);
    char rev[24]; int j = 0;
    while (i > 0) rev[j++] = b[--i];
    out_write(rev, (unsigned int)j);
}

static i32 iabs(i32 v) { return v < 0 ? -v : v; }
static i32 clamp_move(i32 v)
{
    if (v >  MOVE_MAX) return  MOVE_MAX;
    if (v < -MOVE_MAX) return -MOVE_MAX;
    return v;
}

/* ===================================================================== */
/* Client seam -- the same three calls the dormant deadzone.c seam uses,   */
/* re-expressed with deadzoned's syscall wrapper + bounded waits.          */
/* ===================================================================== */
typedef struct { long fd; dz_u32 seq; dz_u8 rx[2048]; dz_u32 rxn; } dz_client;

/* Connect to ip:port (ip = host byte order). The kernel TCP connect is fully
 * synchronous -- it never returns SOCK_EAGAIN (there is no non-blocking socket),
 * and with the frozen-tick backstop it returns SOCK_ETIMEDOUT/ECONN (a negative
 * rc) when the peer is not listening yet. So treat ANY negative rc as a hard
 * failure: close the fd (no leak) and return it; the caller retries with a fresh
 * socket. Returns 0 on success, or a negative rc (or -1000 if socket() failed). */
static long mp_connect(dz_client *c, dz_u32 ip, long port)
{
    c->fd = sc(SYS_SOCKET, SOCK_STREAM, 0, 0, 0, 0);
    if (c->fd < 0) return -1000;
    long r = sc(SYS_CONNECT, c->fd, (long)ip, port, 0, 0);
    if (r < 0) { sc(SYS_CLOSE_SK, c->fd, 0, 0, 0, 0); return r; }
    c->seq = 0; c->rxn = 0;
    return r;
}

/* Send this frame's input (bounded; yields on EAGAIN). Returns 0 / negative. */
static int mp_send_input(dz_client *c, dz_i32 mx, dz_i32 my, dz_u32 yaw, dz_u32 buttons)
{
    dz_input_t in; in.seq = ++c->seq; in.move_x = mx; in.move_y = my;
    in.yaw = yaw; in.buttons = buttons;
    dz_u8 b[DZ_INPUT_BYTES]; dz_input_encode(b, &in);
    long off = 0; int guard = 0;
    while (off < DZ_INPUT_BYTES) {
        long n = sc(SYS_SEND, c->fd, (long)(b + off), DZ_INPUT_BYTES - off, 0, 0);
        if (n > 0) { off += n; continue; }
        if (n == SOCK_EAGAIN) { sc(SYS_YIELD, 0, 0, 0, 0, 0); if (++guard > 100000) return -1; continue; }
        return -1;
    }
    return 0;
}

/* Drain pending bytes, decode complete snapshots, keep the latest in `out`.
 * Non-blocking + reassembling. Returns 1 if a fresh snapshot was decoded. */
static int mp_poll_snapshot(dz_client *c, dz_snapshot_t *out)
{
    int got = 0;
    sc(SYS_SOCK_POLL, 0, 0, 0, 0, 0);
    for (;;) {
        if (c->rxn >= sizeof(c->rx)) c->rxn = 0;            /* defensive resync */
        long n = sc(SYS_RECV, c->fd, (long)(c->rx + c->rxn),
                    (long)(sizeof(c->rx) - c->rxn), 0, 0);
        if (n <= 0) break;                                  /* EAGAIN / closed  */
        c->rxn += (dz_u32)n;
        for (;;) {
            if (c->rxn < 4u * DZ_SNAP_HDR_U32) break;
            /* Validate the magic BEFORE trusting the header's count fields (rx+12/
             * rx+16); a bad magic means the stream is misframed -> full resync. */
            if (dz_get_u32(c->rx) != DZ_SNAP_MAGIC) { c->rxn = 0; break; }
            dz_u32 need = dz_snap_bytes(dz_get_u32(c->rx + 12), dz_get_u32(c->rx + 16));
            if (need == 0 || need > sizeof(c->rx)) { c->rxn = 0; break; }
            if (c->rxn < need) break;
            /* On a structurally-invalid frame, resync rather than consuming `need`
             * (a wrapped/garbage length would otherwise desync the stream forever). */
            if (!dz_snap_decode(c->rx, need, out)) { c->rxn = 0; break; }
            got = 1;
            dz_u32 rem = c->rxn - need;
            for (dz_u32 k = 0; k < rem; k++) c->rx[k] = c->rx[need + k];
            c->rxn = rem;
        }
    }
    return got;
}

/* ===================================================================== */
/* Live session                                                          */
/* ===================================================================== */
static dz_client     g_c;
static dz_snapshot_t g_snap;      /* big -- keep it off the stack          */

/* We are the sole client -> world_join() hands us slot 0 deterministically,
 * so g_snap.p[MY_SLOT] is OUR authoritative player. */
#define MY_SLOT 0

int main(void)
{
    /* ---- connect (retry while the server is still binding/listening) -- */
    int connected = 0;
    for (int a = 0; a < CONNECT_TRIES && !connected; a++) {
        if (mp_connect(&g_c, LOOPBACK_IP, DZ_GAME_PORT) >= 0) connected = 1;
        else sc(SYS_SLEEP, CONNECT_WAIT_MS, 0, 0, 0, 0);
    }
    if (!connected) { out_puts("DEADZONE: mp LIVE FAIL connect\n"); sc(SYS_EXIT, 1, 0, 0, 0, 0); }

    i32  start_x = 0, start_y = 0; int have_start = 0;
    u32  snaps = 0, kills = 0, wave = 0, zalive_seen = 0;
    int  moved = 0;

    for (int f = 0; f < PLAY_FRAMES; f++) {
        /* ---- decide this frame's input from the latest snapshot ------- */
        i32 mx = 0, my = 0;
        if (have_start) {
            i32 px = g_snap.p[MY_SLOT].x, py = g_snap.p[MY_SLOT].y;
            i32 bestd = 0x7fffffff; int zbest = -1;
            for (dz_u32 zi = 0; zi < g_snap.n_zombies; zi++) {
                if (g_snap.z[zi].state != Z_ALIVE_WIRE) continue;
                i32 d = iabs(g_snap.z[zi].x - px) + iabs(g_snap.z[zi].y - py);
                if (d < bestd) { bestd = d; zbest = (int)zi; }
            }
            if (zbest >= 0) {
                mx = clamp_move(g_snap.z[zbest].x - px);
                my = clamp_move(g_snap.z[zbest].y - py);
            }
        }
        /* hold FIRE every frame; the server's authoritative hitscan only
         * connects once we have closed to within its range. */
        if (mp_send_input(&g_c, mx, my, 0, DZ_BTN_FIRE) < 0) {
            out_puts("DEADZONE: mp LIVE FAIL send (server dropped)\n");
            sc(SYS_CLOSE_SK, g_c.fd, 0, 0, 0, 0);
            sc(SYS_EXIT, 1, 0, 0, 0, 0);
        }

        /* ---- apply whatever the server has broadcast ------------------ */
        if (mp_poll_snapshot(&g_c, &g_snap)) {
            snaps++;
            if (!have_start) { start_x = g_snap.p[MY_SLOT].x; start_y = g_snap.p[MY_SLOT].y; have_start = 1; }
            if (g_snap.wave > wave) wave = g_snap.wave;
            if (g_snap.p[MY_SLOT].score > kills) kills = g_snap.p[MY_SLOT].score;
            u32 za = 0;
            for (dz_u32 zi = 0; zi < g_snap.n_zombies; zi++)
                if (g_snap.z[zi].state == Z_ALIVE_WIRE) za++;
            if (za > zalive_seen) zalive_seen = za;
            if (iabs(g_snap.p[MY_SLOT].x - start_x) + iabs(g_snap.p[MY_SLOT].y - start_y) > MOVED_EPS)
                moved = 1;
        }
        sc(SYS_SLEEP, TICK_MS, 0, 0, 0, 0);
    }

    sc(SYS_CLOSE_SK, g_c.fd, 0, 0, 0, 0);

    /* ---- verdict ------------------------------------------------------ */
    if (snaps == 0) { out_puts("DEADZONE: mp LIVE FAIL no_snapshot\n"); sc(SYS_EXIT, 1, 0, 0, 0, 0); }
    if (!moved)     { out_puts("DEADZONE: mp LIVE FAIL no_move\n");     sc(SYS_EXIT, 1, 0, 0, 0, 0); }
    if (kills == 0) { out_puts("DEADZONE: mp LIVE FAIL no_kills\n");    sc(SYS_EXIT, 1, 0, 0, 0, 0); }

    out_puts("DEADZONE: mp LIVE PASS frames=");
    out_unum(PLAY_FRAMES);
    out_puts(" snaps=");  out_unum(snaps);
    out_puts(" wave=");   out_unum(wave);
    out_puts(" kills=");  out_unum(kills);
    out_puts(" moved=1 zalive="); out_unum(zalive_seen);
    out_puts("\n");

    sc(SYS_EXIT, 0, 0, 0, 0, 0);
    return 0;
}
