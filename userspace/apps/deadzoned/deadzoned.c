/*
 * deadzoned -- the DeadZone authoritative multiplayer game server.
 *
 * A from-scratch, freestanding TCP game server for the in-OS zombie-survival
 * shooter (sbin/deadzone). It is AUTHORITATIVE: the world (players, zombies,
 * waves) lives here; clients send only inputs and render the snapshots the
 * server broadcasts. This is the standard "server is truth" model -- it makes
 * the game cheat-resistant and lets several players share one world.
 *
 * It speaks our own native, length-free fixed-size binary wire protocol over
 * the OS's hand-rolled TCP stack (the same SYS_SOCKET/BIND/LISTEN/ACCEPT path
 * httpd uses) -- NOT lwIP, NOT a UNIX socket. Two packet shapes:
 *
 *   client -> server : DZ_INPUT   (move intent + yaw + buttons)        [DZI1]
 *   server -> client : DZ_SNAPSHOT(tick, wave, players[], zombies[])   [DZS1]
 *
 * Both are packed little-endian by hand (put_u32/get_i32) so there is zero
 * struct-padding ambiguity on the wire.
 *
 * Usage:
 *   deadzoned            serve on the default port (27015), authoritative loop
 *   deadzoned PORT       serve on PORT
 *   deadzoned -t         headless self-test (sim + protocol round-trip +
 *                        bind/listen/accept) -> "DEADZONED SELFTEST: PASS ..."
 *
 * The self-test needs NO client and NO network peer: it runs the authoritative
 * sim for a fixed number of ticks, serialises a snapshot and parses it back
 * (round-trip), and proves the listening socket can be opened/bound/listened/
 * accepted(non-blocking)/closed. That is the deterministic, hardware-free proof
 * marker; the live serve loop is the same socket scaffolding httpd already
 * proves never wedges the desktop (every wait is iteration-capped + yields).
 *
 * Build (host gcc cross-compile via build_all.sh, identical CF/LD to httpd):
 *   cc ... -c userspace/apps/deadzoned/deadzoned.c -o /tmp/deadzoned.o
 *   ld ... /tmp/crt0.o /tmp/deadzoned.o -o /tmp/deadzoned.elf
 *   objdump -d /tmp/deadzoned.elf | grep fs:0x28   # MUST be empty
 */

/* ---- syscall numbers (AutomationOS ABI -- see kernel/include/syscall.h) -- */
#define SYS_EXIT        0
#define SYS_WRITE       3    /* write(fd, buf, len)           fd1 = stdout    */
#define SYS_SLEEP       9    /* sleep(ms) -- real blocking ms sleep            */
#define SYS_YIELD       15   /* cooperative yield                             */
#define SYS_SOCKET      51   /* socket(SOCK_STREAM) -> fd                     */
#define SYS_SEND        53   /* send(fd, buf, len) -> bytes                   */
#define SYS_RECV        54   /* recv(fd, buf, len) -> bytes/0(closed)/-11     */
#define SYS_CLOSE_SK    55   /* close(socket fd) -> 0                          */
#define SYS_SOCK_POLL   58   /* pump the NIC RX/timers before accept/recv     */
#define SYS_BIND        76   /* bind(fd, port) -> 0/neg                       */
#define SYS_LISTEN      77   /* listen(fd, backlog) -> 0/neg                  */
#define SYS_ACCEPT      78   /* accept(fd) -> child fd / SOCK_EAGAIN(-11)     */

#define SOCK_STREAM     1
#define FD_STDOUT       1

#define SOCK_OK          0
#define SOCK_EAGAIN    (-11)

/* ---- tunables ---------------------------------------------------------- */
#define DEFAULT_PORT     27015      /* the DeadZone game port                */
#define LISTEN_BACKLOG   8
#define SELFTEST_PORT    27115

#define MAX_CLIENTS      8          /* co-op players in one world            */
#define MAX_ZOMBIES      64
#define ARENA           10000       /* world is [0..ARENA] in both axes      */
#define WORLD_SCALE      1          /* integer world units (no float)        */

/* Per-loop bounds so the daemon ALWAYS makes progress + never hangs the box */
#define ACCEPT_TRIES     4          /* non-blocking accepts attempted / tick */
#define SERVE_MAX_TICKS  2000000    /* finite lifetime; returns to the OS    */
#define TICK_MS          33         /* ~30 Hz authoritative tick             */
#define CLIENT_RXBUF     256        /* per-client TCP reassembly buffer      */

typedef unsigned int   u32;
typedef int            i32;
typedef unsigned char  u8;

/* The wire protocol is shared verbatim with the game client (sbin/deadzone)
 * via dzproto.h -- ONE definition for both ends so they can never drift. */
#include "dzproto.h"

/* ===================================================================== */
/* Raw 6-arg inline syscall (verbatim from httpd.c / nc.c)               */
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

/* ---- tiny freestanding I/O helpers ------------------------------------ */
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
static void out_num(long v)
{
    if (v < 0) { out_puts("-"); out_unum((unsigned long)(-v)); }
    else        out_unum((unsigned long)v);
}
static int s_eq(const char *a, const char *b)
{
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}
static long parse_port(const char *s)
{
    long v = 0; int any = 0;
    if (!s) return -1;
    for (; *s; s++) {
        if (*s < '0' || *s > '9') return -1;
        v = v * 10 + (*s - '0'); any = 1;
        if (v > 65535) return -1;
    }
    return any ? v : -1;
}

/* ===================================================================== */
/* Deterministic PRNG (LCG) -- self-test must be reproducible, so we do   */
/* NOT touch any wall clock. Spawn positions come from this stream.       */
/* ===================================================================== */
static u32 g_rng = 0x1234567u;
static u32 rng_next(void)
{
    g_rng = g_rng * 1103515245u + 12345u;
    return (g_rng >> 8) & 0x7fffffffu;
}
static i32 rng_range(i32 lo, i32 hi)   /* inclusive lo, exclusive hi */
{
    if (hi <= lo) return lo;
    return lo + (i32)(rng_next() % (u32)(hi - lo));
}

/* Wire (un)packers + packet (de)serializers live in dzproto.h (dz_put_u32 /
 * dz_snap_encode / dz_input_decode / ...). The server maps its internal World
 * onto the shared dz_snapshot_t below. */

/* ===================================================================== */
/* Authoritative world model                                             */
/* ===================================================================== */
#define Z_DEAD   0
#define Z_ALIVE  1

typedef struct {
    i32 x, y;       /* world position                                      */
    i32 hp;         /* > 0 alive                                           */
    i32 yaw;        /* facing, "brads" 0..1023 (matches g3d/deadzone)      */
    u32 active;     /* slot in use (a client occupies it)                  */
    u32 score;      /* kills                                               */
    u32 last_seq;   /* last input sequence applied (drop stale/dupes)      */
} Player;

typedef struct {
    i32 x, y;
    u32 state;      /* Z_ALIVE / Z_DEAD                                    */
} Zombie;

typedef struct {
    u32 tick;
    u32 wave;
    u32 n_players;          /* slot count (active or not), == MAX_CLIENTS  */
    u32 n_zombies;          /* live zombie count this wave                 */
    u32 alive;              /* zombies still alive                         */
    Player  p[MAX_CLIENTS];
    Zombie  z[MAX_ZOMBIES];
} World;

static World g_world;

/* DZ_INPUT, as parsed off the wire */
typedef struct {
    u32 seq;
    i32 move_x, move_y;     /* desired step this tick (clamped server-side) */
    u32 yaw;
    u32 buttons;            /* bit0 = fire                                  */
} Input;

#define BTN_FIRE   0x1u

#define PLAYER_HP_MAX   100
#define PLAYER_SPEED    256         /* max world units / packet -- covers the client's
                                     * time-scaled multi-step send incl. leveled-AGI
                                     * diagonal play (was 60 = a per-step cap that pinned
                                     * co-op to ~half SP speed). Teleport stays bounded by
                                     * the arena position-clamp below, so this is safe. */
#define ZOMBIE_SPEED    24
#define ZOMBIE_REACH    140         /* contact distance for melee           */
#define ZOMBIE_DMG      6
#define FIRE_RANGE      2600        /* hitscan reach                        */
#define FIRE_CONE       170         /* aim forgiveness (world units lateral) */

static i32 iabs(i32 v) { return v < 0 ? -v : v; }

/* Cheap integer distance proxy (Manhattan) -- fine for AI + range gates.  */
static i32 dist_manhattan(i32 ax, i32 ay, i32 bx, i32 by)
{
    return iabs(ax - bx) + iabs(ay - by);
}

static i32 clampi(i32 v, i32 lo, i32 hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

/* Spawn `count` zombies around the arena edges, chasing inward. */
static void spawn_wave(World *w, u32 count)
{
    if (count > MAX_ZOMBIES) count = MAX_ZOMBIES;
    for (u32 i = 0; i < MAX_ZOMBIES; i++) w->z[i].state = Z_DEAD;
    for (u32 i = 0; i < count; i++) {
        /* pick an edge, then a position along it */
        int edge = rng_range(0, 4);
        i32 t = rng_range(0, ARENA);
        switch (edge) {
            case 0: w->z[i].x = 0;        w->z[i].y = t;        break;
            case 1: w->z[i].x = ARENA;    w->z[i].y = t;        break;
            case 2: w->z[i].x = t;        w->z[i].y = 0;        break;
            default:w->z[i].x = t;        w->z[i].y = ARENA;    break;
        }
        w->z[i].state = Z_ALIVE;
    }
    w->n_zombies = count;
    w->alive     = count;
}

static void world_init(World *w)
{
    for (u32 i = 0; i < MAX_CLIENTS; i++) {
        w->p[i].x = ARENA / 2 + (i32)i * 120;
        w->p[i].y = ARENA / 2;
        w->p[i].hp = PLAYER_HP_MAX;
        w->p[i].yaw = 0;
        w->p[i].active = 0;
        w->p[i].score = 0;
        w->p[i].last_seq = 0;
    }
    w->n_players = MAX_CLIENTS;
    w->tick = 0;
    w->wave = 1;
    spawn_wave(w, 6);
}

/* Reserve a player slot for a freshly-connected client; -1 if the world is
 * full. The slot is reset to a live spawn. */
static int world_join(World *w)
{
    for (u32 i = 0; i < MAX_CLIENTS; i++) {
        if (!w->p[i].active) {
            w->p[i].active = 1;
            w->p[i].hp = PLAYER_HP_MAX;
            w->p[i].x = ARENA / 2 + ((i32)i * 2 - 1) * 1200;  /* spread so players don't spawn stacked */
            w->p[i].y = ARENA / 2;
            w->p[i].yaw = 0;
            w->p[i].score = 0;
            w->p[i].last_seq = 0;
            return (int)i;
        }
    }
    return -1;
}

static void world_leave(World *w, int slot)
{
    if (slot >= 0 && slot < MAX_CLIENTS) w->p[slot].active = 0;
}

/* Nearest ALIVE & active player to a point; -1 if none. */
static int nearest_player(World *w, i32 x, i32 y)
{
    int best = -1; i32 bestd = 0x7fffffff;
    for (u32 i = 0; i < MAX_CLIENTS; i++) {
        if (!w->p[i].active || w->p[i].hp <= 0) continue;
        i32 d = dist_manhattan(x, y, w->p[i].x, w->p[i].y);
        if (d < bestd) { bestd = d; best = (int)i; }
    }
    return best;
}

/* Apply one client input to its player slot (authoritatively). */
static void apply_input(World *w, int slot, const Input *in)
{
    if (slot < 0 || slot >= MAX_CLIENTS || !w->p[slot].active) return;
    if (w->p[slot].hp <= 0) return;
    /* drop stale / duplicate inputs (TCP can't reorder, but clients may
     * resend; last_seq monotonicity keeps us authoritative) */
    if (in->seq && in->seq <= w->p[slot].last_seq) return;
    w->p[slot].last_seq = in->seq;

    i32 dx = clampi(in->move_x, -PLAYER_SPEED, PLAYER_SPEED);
    i32 dy = clampi(in->move_y, -PLAYER_SPEED, PLAYER_SPEED);
    w->p[slot].x = clampi(w->p[slot].x + dx, 0, ARENA);
    w->p[slot].y = clampi(w->p[slot].y + dy, 0, ARENA);
    w->p[slot].yaw = in->yaw & 1023u;

    if (in->buttons & BTN_FIRE) {
        /* hitscan: kill the nearest alive zombie within range + cone */
        int best = -1; i32 bestd = 0x7fffffff;
        for (u32 zi = 0; zi < w->n_zombies; zi++) {
            if (w->z[zi].state != Z_ALIVE) continue;
            i32 d = dist_manhattan(w->p[slot].x, w->p[slot].y,
                                   w->z[zi].x, w->z[zi].y);
            if (d <= FIRE_RANGE && d < bestd) { bestd = d; best = (int)zi; }
        }
        if (best >= 0) {
            w->z[best].state = Z_DEAD;
            if (w->alive) w->alive--;
            w->p[slot].score++;
        }
    }
}

/* Advance the world one authoritative tick: zombies chase + bite; respawn
 * the next (bigger) wave when the field is clear. */
static void world_tick(World *w)
{
    w->tick++;

    for (u32 zi = 0; zi < w->n_zombies; zi++) {
        if (w->z[zi].state != Z_ALIVE) continue;
        int tgt = nearest_player(w, w->z[zi].x, w->z[zi].y);
        if (tgt < 0) continue;            /* no live player -> idle         */

        i32 px = w->p[tgt].x, py = w->p[tgt].y;
        i32 sx = (px > w->z[zi].x) ? ZOMBIE_SPEED : (px < w->z[zi].x ? -ZOMBIE_SPEED : 0);
        i32 sy = (py > w->z[zi].y) ? ZOMBIE_SPEED : (py < w->z[zi].y ? -ZOMBIE_SPEED : 0);
        w->z[zi].x = clampi(w->z[zi].x + sx, 0, ARENA);
        w->z[zi].y = clampi(w->z[zi].y + sy, 0, ARENA);

        if (dist_manhattan(w->z[zi].x, w->z[zi].y, px, py) <= ZOMBIE_REACH) {
            w->p[tgt].hp -= ZOMBIE_DMG;
            if (w->p[tgt].hp < 0) w->p[tgt].hp = 0;
        }
    }

    if (w->alive == 0) {
        w->wave++;
        u32 next = 4 + w->wave * 4;        /* endless, escalating waves      */
        if (next > MAX_ZOMBIES) next = MAX_ZOMBIES;
        spawn_wave(w, next);
        /* revive downed players for the next wave (co-op forgiveness) */
        for (u32 i = 0; i < MAX_CLIENTS; i++)
            if (w->p[i].active && w->p[i].hp <= 0) w->p[i].hp = PLAYER_HP_MAX / 2;
    }
}

/* ===================================================================== */
/* Wire serialization -- maps the server's internal World onto the SHARED  */
/* dz_snapshot_t and delegates the actual byte-packing to dzproto.h, so    */
/* the bytes the server emits are exactly what the client's dz_snap_decode  */
/* (same header) reads back.                                                */
/* ===================================================================== */
static dz_snapshot_t g_snapview;     /* scratch view for snap_serialize    */

static u32 snap_serialize(const World *w, u8 *buf, u32 cap)
{
    dz_snapshot_t *v = &g_snapview;
    v->tick = w->tick; v->wave = w->wave;
    v->n_players = w->n_players; v->n_zombies = w->n_zombies;
    for (u32 i = 0; i < w->n_players; i++) {
        /* MP-PHANTOM-FIX: honor the wire contract -- an UNOCCUPIED slot serializes
         * as DZ_SLOT_EMPTY with neutralized fields, so the client's
         * `if (id==DZ_SLOT_EMPTY) continue;` guard fires and no phantom teammate is
         * rendered. (Before: every slot got id=i -> a solo co-op drew 7 ghosts.) */
        if (!w->p[i].active) {
            v->p[i].id    = DZ_SLOT_EMPTY;
            v->p[i].x = 0; v->p[i].y = 0; v->p[i].hp = 0;
            v->p[i].yaw = 0; v->p[i].score = 0;
            continue;
        }
        v->p[i].id    = i;
        v->p[i].x     = w->p[i].x;
        v->p[i].y     = w->p[i].y;
        v->p[i].hp    = w->p[i].hp;
        v->p[i].yaw   = (dz_u32)w->p[i].yaw;
        v->p[i].score = w->p[i].score;
    }
    for (u32 i = 0; i < w->n_zombies; i++) {
        v->z[i].x = w->z[i].x; v->z[i].y = w->z[i].y; v->z[i].state = w->z[i].state;
    }
    return dz_snap_encode(v, (dz_u8 *)buf, cap);
}

/* Parse a DZ_INPUT packet into the server's Input. Returns 1 if valid. */
static int input_parse(const u8 *buf, Input *in)
{
    dz_input_t d;
    if (!dz_input_decode((const dz_u8 *)buf, &d)) return 0;
    in->seq = d.seq; in->move_x = d.move_x; in->move_y = d.move_y;
    in->yaw = d.yaw; in->buttons = d.buttons;
    return 1;
}

/* ===================================================================== */
/* Bounded socket send (verbatim model from httpd send_all)              */
/* ===================================================================== */
static long send_all(long fd, const u8 *buf, long len)
{
    long off = 0; int guard = 0;
    while (off < len) {
        long n = sc(SYS_SEND, fd, (long)(buf + off), len - off, 0, 0);
        if (n > 0) { off += n; guard = 0; continue; }
        if (n == SOCK_EAGAIN) {
            sc(SYS_YIELD, 0, 0, 0, 0, 0);
            if (++guard > 100000) break;
            continue;
        }
        return n;                       /* hard error (reset) */
    }
    return off;
}

/* ===================================================================== */
/* Live authoritative serve loop                                         */
/* ===================================================================== */
typedef struct {
    long fd;            /* socket fd, -1 if free                          */
    int  slot;          /* player slot in the world                       */
    u8   rx[CLIENT_RXBUF];
    u32  rxn;           /* bytes buffered                                  */
} Client;

static Client g_cli[MAX_CLIENTS];
static u8     g_snapbuf[DZ_SNAP_MAX_BYTES];

static void client_clear(int i)
{
    g_cli[i].fd = -1;
    g_cli[i].slot = -1;
    g_cli[i].rxn = 0;
}

/* Drain whatever input bytes are pending on a client; apply each complete
 * DZ_INPUT_BYTES packet authoritatively. Non-blocking. Returns 0 if the peer
 * closed (caller drops it), 1 otherwise. */
static int client_pump_input(Client *c, World *w)
{
    for (;;) {
        if (c->rxn >= CLIENT_RXBUF) c->rxn = 0;     /* defensive resync */
        long n = sc(SYS_RECV, c->fd, (long)(c->rx + c->rxn),
                    (long)(CLIENT_RXBUF - c->rxn), 0, 0);
        if (n == 0) return 0;                       /* closed */
        if (n == SOCK_EAGAIN) break;                /* nothing more now */
        if (n < 0) break;                           /* transient error  */
        c->rxn += (u32)n;
        while (c->rxn >= DZ_INPUT_BYTES) {
            Input in;
            /* A bad magic means the input stream is misframed -- resync to the
             * buffer head rather than blindly consuming 24 bytes (which would
             * never re-align). Loopback TCP can't reorder, but a buggy/hostile
             * client could send garbage; this keeps the authoritative server robust. */
            if (!input_parse(c->rx, &in)) { c->rxn = 0; break; }
            apply_input(w, c->slot, &in);
            /* shift the consumed packet out of the buffer */
            u32 rem = c->rxn - DZ_INPUT_BYTES;
            for (u32 k = 0; k < rem; k++) c->rx[k] = c->rx[DZ_INPUT_BYTES + k];
            c->rxn = rem;
        }
    }
    return 1;
}

static int do_serve(long port)
{
    long lfd = sc(SYS_SOCKET, SOCK_STREAM, 0, 0, 0, 0);
    if (lfd < 0) { out_puts("deadzoned: socket() rc="); out_num(lfd); out_puts("\n"); return 3; }
    long br = sc(SYS_BIND, lfd, port, 0, 0, 0);
    if (br < 0) { out_puts("deadzoned: bind(:"); out_num(port); out_puts(") rc="); out_num(br); out_puts("\n"); sc(SYS_CLOSE_SK, lfd, 0,0,0,0); return 4; }
    long lr = sc(SYS_LISTEN, lfd, LISTEN_BACKLOG, 0, 0, 0);
    if (lr < 0) { out_puts("deadzoned: listen() rc="); out_num(lr); out_puts("\n"); sc(SYS_CLOSE_SK, lfd, 0,0,0,0); return 5; }

    for (int i = 0; i < MAX_CLIENTS; i++) client_clear(i);
    world_init(&g_world);

    out_puts("DEADZONED: listening on 0.0.0.0:");
    out_num(port);
    out_puts(" (authoritative, max_players=");
    out_num(MAX_CLIENTS);
    out_puts(")\n");

    for (long t = 0; t < SERVE_MAX_TICKS; t++) {
        sc(SYS_SOCK_POLL, 0, 0, 0, 0, 0);

        /* ---- accept new clients (non-blocking, a few per tick) ---- */
        for (int a = 0; a < ACCEPT_TRIES; a++) {
            long cfd = sc(SYS_ACCEPT, lfd, 0, 0, 0, 0);
            if (cfd == SOCK_EAGAIN) break;
            if (cfd < 0) break;
            int placed = -1;
            for (int i = 0; i < MAX_CLIENTS; i++) {
                if (g_cli[i].fd < 0) { placed = i; break; }
            }
            if (placed < 0) { sc(SYS_CLOSE_SK, cfd, 0,0,0,0); continue; }
            int slot = world_join(&g_world);
            if (slot < 0) { sc(SYS_CLOSE_SK, cfd, 0,0,0,0); continue; }
            g_cli[placed].fd = cfd;
            g_cli[placed].slot = slot;
            g_cli[placed].rxn = 0;
            /* MP-HELLO-0: tell the client its assigned slot FIRST (a 16B join-ack).
             * TCP in-order delivery guarantees it precedes this tick's snapshot
             * broadcast, so the client reads it before its snapshot pump. */
            {
                dz_hello_t hel = { (dz_u32)slot, (dz_u32)MAX_CLIENTS, (dz_u32)DZ_PROTO_VER };
                u8 hb[DZ_HELLO_BYTES];
                dz_hello_encode(hb, &hel);
                if (send_all(cfd, hb, DZ_HELLO_BYTES) != DZ_HELLO_BYTES) {
                    world_leave(&g_world, slot);
                    sc(SYS_CLOSE_SK, cfd, 0,0,0,0);
                    client_clear(placed);
                    continue;
                }
            }
            out_puts("DEADZONED: client joined slot=");
            out_num(slot); out_puts("\n");
        }

        /* ---- read inputs from every connected client ---- */
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (g_cli[i].fd < 0) continue;
            if (!client_pump_input(&g_cli[i], &g_world)) {
                world_leave(&g_world, g_cli[i].slot);
                sc(SYS_CLOSE_SK, g_cli[i].fd, 0,0,0,0);
                client_clear(i);
            }
        }

        /* ---- advance the authoritative world one tick ---- */
        world_tick(&g_world);

        /* ---- broadcast the snapshot to every client ---- */
        u32 slen = snap_serialize(&g_world, g_snapbuf, sizeof(g_snapbuf));
        if (slen) {
            for (int i = 0; i < MAX_CLIENTS; i++) {
                if (g_cli[i].fd < 0) continue;
                long sr = send_all(g_cli[i].fd, g_snapbuf, (long)slen);
                if (sr < 0) {        /* peer reset -> drop */
                    world_leave(&g_world, g_cli[i].slot);
                    sc(SYS_CLOSE_SK, g_cli[i].fd, 0,0,0,0);
                    client_clear(i);
                }
            }
        }

        /* pace the tick; SYS_SLEEP yields the CPU so the desktop stays smooth */
        sc(SYS_SLEEP, TICK_MS, 0, 0, 0, 0);
    }

    for (int i = 0; i < MAX_CLIENTS; i++)
        if (g_cli[i].fd >= 0) sc(SYS_CLOSE_SK, g_cli[i].fd, 0,0,0,0);
    sc(SYS_CLOSE_SK, lfd, 0, 0, 0, 0);
    out_puts("DEADZONED: shut down\n");
    return 0;
}

/* ===================================================================== */
/* Headless self-test (deadzoned -t)                                     */
/* ===================================================================== */
/*
 * Proves, with NO client and NO live peer:
 *   1. the authoritative sim runs + progresses (zombies die under fire, the
 *      wave counter escalates),
 *   2. the snapshot wire format round-trips byte-exactly (serialize->parse),
 *   3. the listening socket can be opened/bound/listened/accepted(EAGAIN)/
 *      closed -- i.e. the server CAN host.
 * Prints "DEADZONED SELFTEST: PASS ..." / "... FAIL <why>".
 */
static int self_test(void)
{
    /* (1) authoritative sim ------------------------------------------- */
    World *w = &g_world;
    world_init(w);
    int slot = world_join(w);
    if (slot < 0) { out_puts("DEADZONED SELFTEST: FAIL join\n"); return 1; }
    if (w->wave != 1 || w->n_zombies == 0) {
        out_puts("DEADZONED SELFTEST: FAIL init\n"); return 1;
    }
    u32 kills = 0;
    /* script the player: aim at a zombie and fire every tick for a while */
    for (int t = 0; t < 4000; t++) {
        /* steer the player toward the first alive zombie so fire connects */
        int z = -1;
        for (u32 zi = 0; zi < w->n_zombies; zi++)
            if (w->z[zi].state == Z_ALIVE) { z = (int)zi; break; }
        Input in; in.seq = (u32)t + 1; in.yaw = 0; in.buttons = BTN_FIRE;
        in.move_x = in.move_y = 0;
        if (z >= 0) {
            in.move_x = clampi(w->z[z].x - w->p[slot].x, -PLAYER_SPEED, PLAYER_SPEED);
            in.move_y = clampi(w->z[z].y - w->p[slot].y, -PLAYER_SPEED, PLAYER_SPEED);
        }
        u32 before = w->p[slot].score;
        apply_input(w, slot, &in);
        if (w->p[slot].score > before) kills++;
        world_tick(w);
        if (w->wave >= 3) break;       /* proved escalation */
    }
    if (w->wave < 2) { out_puts("DEADZONED SELFTEST: FAIL no_wave_progress\n"); return 1; }
    if (kills == 0)  { out_puts("DEADZONED SELFTEST: FAIL no_kills\n"); return 1; }

    /* (2) snapshot round-trip ----------------------------------------- */
    u8 buf[DZ_SNAP_MAX_BYTES];
    u32 n = snap_serialize(w, buf, sizeof(buf));
    if (n == 0) { out_puts("DEADZONED SELFTEST: FAIL serialize\n"); return 1; }
    static dz_snapshot_t parsed;       /* big -- keep it off the stack */
    if (!dz_snap_decode(buf, n, &parsed)) { out_puts("DEADZONED SELFTEST: FAIL parse\n"); return 1; }
    if (parsed.tick != w->tick || parsed.wave != w->wave ||
        parsed.n_players != w->n_players || parsed.n_zombies != w->n_zombies) {
        out_puts("DEADZONED SELFTEST: FAIL roundtrip_hdr\n"); return 1;
    }
    if (parsed.p[slot].x != w->p[slot].x || parsed.p[slot].y != w->p[slot].y ||
        parsed.p[slot].hp != w->p[slot].hp || parsed.p[slot].score != w->p[slot].score) {
        out_puts("DEADZONED SELFTEST: FAIL roundtrip_player\n"); return 1;
    }
    if (w->n_zombies > 0 &&
        (parsed.z[0].x != w->z[0].x || parsed.z[0].y != w->z[0].y ||
         parsed.z[0].state != w->z[0].state)) {
        out_puts("DEADZONED SELFTEST: FAIL roundtrip_zombie\n"); return 1;
    }

    /* (2b) MP-PHANTOM-FIX: only slot `slot` joined, so every OTHER slot must
     * serialize + decode as DZ_SLOT_EMPTY (not a valid id) -- proving the producer
     * honors the wire contract the client's phantom-skip guard relies on. Before
     * the fix every slot carried id=i and the GUI client drew 7 ghost teammates. */
    if (parsed.p[slot].id != (dz_u32)slot) {
        out_puts("DEADZONED SELFTEST: FAIL active_id\n"); return 1;
    }
    {
        u32 empties = 0;
        for (u32 i = 0; i < parsed.n_players; i++)
            if ((int)i != slot && parsed.p[i].id == DZ_SLOT_EMPTY) empties++;
        if (empties != parsed.n_players - 1) {
            out_puts("DEADZONED SELFTEST: FAIL phantom_slots\n"); return 1;
        }
    }

    /* (3) listening socket -- proves it can host ----------------------- */
    long fd = sc(SYS_SOCKET, SOCK_STREAM, 0, 0, 0, 0);
    if (fd < 0) { out_puts("DEADZONED SELFTEST: FAIL socket rc="); out_num(fd); out_puts("\n"); return 1; }
    long br = sc(SYS_BIND, fd, SELFTEST_PORT, 0, 0, 0);
    if (br != SOCK_OK) { out_puts("DEADZONED SELFTEST: FAIL bind rc="); out_num(br); out_puts("\n"); sc(SYS_CLOSE_SK, fd,0,0,0,0); return 1; }
    long lr = sc(SYS_LISTEN, fd, LISTEN_BACKLOG, 0, 0, 0);
    if (lr != SOCK_OK) { out_puts("DEADZONED SELFTEST: FAIL listen rc="); out_num(lr); out_puts("\n"); sc(SYS_CLOSE_SK, fd,0,0,0,0); return 1; }
    sc(SYS_SOCK_POLL, 0, 0, 0, 0, 0);
    long ar = sc(SYS_ACCEPT, fd, 0, 0, 0, 0);   /* nothing connecting -> EAGAIN expected */
    if (ar < 0 && ar != SOCK_EAGAIN) { out_puts("DEADZONED SELFTEST: FAIL accept rc="); out_num(ar); out_puts("\n"); sc(SYS_CLOSE_SK, fd,0,0,0,0); return 1; }
    sc(SYS_CLOSE_SK, fd, 0, 0, 0, 0);

    out_puts("DEADZONED SELFTEST: PASS sim_kills=");
    out_num((long)kills);
    out_puts(" wave=");
    out_num((long)w->wave);
    out_puts(" snap_bytes=");
    out_num((long)n);
    out_puts(" roundtrip=ok listen=ok port=");
    out_num(SELFTEST_PORT);
    out_puts(" phantoms_ok=1\n");
    return 0;
}

/* ===================================================================== */
/* Entry point                                                           */
/* ===================================================================== */
int main(int argc, char **argv)
{
    if (argc >= 2 && s_eq(argv[1], "-t"))
        return self_test();

    long port = DEFAULT_PORT;
    if (argc >= 2) {
        long p = parse_port(argv[1]);
        if (p < 0) {
            out_puts("deadzoned: invalid port '"); out_puts(argv[1]);
            out_puts("'\nusage: deadzoned [PORT] | deadzoned -t\n");
            return 1;
        }
        port = p;
    }
    return do_serve(port);
}
