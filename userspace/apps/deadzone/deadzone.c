/*
 * deadzone.c -- "DeadZone" first-person zombie-survival shooter (ring 3).
 * =======================================================================
 *
 * A first-person, endless-wave zombie shooter built by EXTENDING the in-tree
 * g3d software renderer (userspace/lib/g3d, Q16.16 fixed-point, no GPU/FP/libc)
 * and cloning the derby.c game skeleton (fixed-16ms-timestep loop + wl window +
 * static-mesh world + 2D HUD-over-3D). This is the minimal-playable core:
 *
 *   - FPS camera (eye at the player, look from yaw) + WASD strafe in the yaw basis.
 *   - Mouselook via absolute-pointer-X delta, with A/D key-turn fallback.
 *   - One hitscan weapon: left-click / SPACE fires a ray vs each zombie's XZ
 *     circle; nearest hit takes damage. Magazine + reload (R) + recoil kick.
 *   - Zombies (Walker) hunt the player, deal contact damage, flash + die.
 *   - Endless waves: each cleared wave spawns a bigger, tougher one.
 *   - HUD: HP bar + ammo/mag + wave + kills + crosshair, on the OS bitfont.
 *
 * Geometry is hand-built box meshes (like derby's build_carmesh) -- zero asset
 * pipeline. Baked OBJ models, the PSX render pass, inventory/attributes, and the
 * multiplayer server layer onto this green baseline (deadzoned).
 *
 * Build (WSL toolchain; links wl + bitfont + g3d like derby):
 *   cc ... -c userspace/apps/deadzone/deadzone.c -o /tmp/deadzone.o
 *   ld ... /tmp/deadzone.o /tmp/wlc.o /tmp/bf.o /tmp/g3d.o -o /tmp/deadzone.elf
 *
 * Serial: "DEADZONE: ready"  ...  "[DEADZONE] over wave N kills K"
 */

#include "../../lib/wl/wl_client.h"
#include "../../lib/font/bitfont.h"
#include "../../lib/g3d/g3d.h"
/* Multiplayer: the SAME wire protocol the authoritative server (deadzoned)
 * speaks -- shared verbatim so client + server can never drift. */
#include "../deadzoned/dzproto.h"

#pragma GCC optimize ("O2", "no-stack-protector", "no-tree-loop-distribute-patterns")

/* ---- syscall numbers (AOS) ---- */
#define SYS_EXIT          0
#define SYS_WRITE         3
#define SYS_YIELD         15
#define SYS_GET_TICKS_MS  40
#define SYS_AUDIO_STREAM_WRITE 128   /* AUDIO B1: stream 48k/16-bit/stereo PCM */
#define AUD_EAGAIN  (-11)            /* kernel software ring full (EAGAIN)     */

/* In-game audio mixer -- defined below, forward-declared for try_shoot/bite. */
static void audio_trigger(int kind);
static void num_to_str(char *buf, int v);   /* defined later; used by extraction_step */
static void mp_game_connect(void);          /* MP-GUI-1: defined after the mp seam   */
static void mp_game_disconnect(void);       /* used by the connect menu (handle_ui_key) */

/* ---- key scancodes ---- */
#define KEY_ESC     1
#define KEY_R      19
#define KEY_UP    103
#define KEY_DOWN  108
#define KEY_LEFT  105
#define KEY_RIGHT 106
#define KEY_W      17
#define KEY_A      30
#define KEY_S      31
#define KEY_D      32
#define KEY_SPACE  57
#define KEY_TAB    15
#define KEY_C      46
#define KEY_ENTER  28
#define KEY_1       2
#define KEY_2       3
#define KEY_M      50            /* MP-GUI-1: toggle the co-op connect menu  */

typedef unsigned int   u32;
typedef int            i32;
typedef unsigned long  u64;
typedef unsigned char  u8;
typedef unsigned short u16;
typedef short          i16;   /* signed 16-bit PCM sample (in-game audio mixer) */

static inline long sc(long n, long a1, long a2, long a3, long a4, long a5, long a6)
{
    long r;
    register long r10 asm("r10") = a4;
    register long r8  asm("r8")  = a5;
    register long r9  asm("r9")  = a6;
    asm volatile("syscall" : "=a"(r)
                 : "a"(n), "D"(a1), "S"(a2), "d"(a3), "r"(r10), "r"(r8), "r"(r9)
                 : "rcx", "r11", "memory");
    return r;
}
static u64 k_strlen(const char *s) { u64 n = 0; while (s[n]) n++; return n; }
static void print(const char *m) { sc(SYS_WRITE, 1, (long)m, (long)k_strlen(m), 0, 0, 0); }

/* ====================================================================== *
 *  Tunables (Q16.16 world units)
 * ====================================================================== */
#define WIN_W       640
#define WIN_H       480
#define STEP_MS     16
#define MAX_Z       24            /* max concurrent zombies                 */

#define ARENA_HALF_I 14
#define EYE_H        fx_ratio(12,10)
#define MOVE_SPD     fx_ratio(11,100)   /* player units / step              */
#define TURN_KEY     14                 /* brads/step for A/D fallback turn  */
#define MOUSE_SENS   2                  /* brads per pointer-x pixel         */

#define PLAYER_R     fx_ratio(40,100)
#define Z_HX         fx_ratio(33,100)
#define Z_HY         fx_ratio(80,100)
#define Z_HZ         fx_ratio(33,100)
#define Z_R          fx_ratio(45,100)   /* zombie collision radius           */
#define Z_HIT_R      fx_ratio(75,100)   /* hitscan aim forgiveness           */
#define Z_ATTACK_R   fx_ratio(120,100)  /* range at which a zombie bites      */
#define Z_SPEED      fx_ratio(45,1000)  /* walker move / step                */

#define WEAPON_DMG   34
#define MAG_SIZE     12
#define RELOAD_STEPS 55
#define FIRE_COOLDN  6                  /* steps between shots                */
#define Z_BITE_DMG   8
#define Z_BITE_CD    35                 /* steps between a zombie's bites      */

/* ====================================================================== *
 *  State
 * ====================================================================== */
typedef struct {
    fx  x, z;
    i32 head;            /* facing brads */
    i32 health;
    i32 alive;
    i32 hitflash;
    i32 bite_cd;
} Zombie;

static Zombie g_z[MAX_Z];

static fx  g_px, g_pz;        /* player position */
static i32 g_yaw;             /* player view yaw (brads) */
static i32 g_hp;             /* player health 0..100 */
static i32 g_alive;          /* 1 while playing */

static i32 g_mag, g_ammo;    /* in-magazine / reserve */
static i32 g_reload;         /* reload countdown (steps) */
static i32 g_fire_cd;        /* fire cooldown (steps) */
static fx  g_recoil;         /* recoil kick (decays) */
static i32 g_muzzle;         /* muzzle-flash frames */

static i32 g_wave;           /* current wave (1..) */
static i32 g_wave_size;      /* zombies this wave */
static i32 g_spawned;        /* spawned so far this wave */
static i32 g_spawn_timer;    /* steps until next spawn */
static i32 g_kills;
static i32 g_over;           /* 1 = game over */

static u32 g_rng = 0xC0FFEEu;
static u32 rng_next(void) { g_rng = g_rng * 1664525u + 1013904223u; return (g_rng >> 8) & 0x7FFFFFu; }

/* held movement flags */
static i32 g_hold_fwd, g_hold_back, g_hold_left, g_hold_right, g_hold_tl, g_hold_tr;
static i32 g_shoot_held;
static i32 g_have_mouse, g_last_mx;

/* ---- MP-GUI-1: live co-op render state (gated on g_mp_on; default 0 => the
 * single-player game is byte-identical). The render-facing fields live here,
 * ahead of render_scene; the connection object lives by the mp seam below. */
static i32 g_mp_on;                       /* 1 while joined to a server          */
static i32 g_my_slot;                     /* our slot id (learned from DZ_HELLO)  */
static fx  g_rx[DZP_MAX_CLIENTS], g_rz[DZP_MAX_CLIENTS];  /* other players' fx pos */
static i32 g_ron[DZP_MAX_CLIENTS];        /* 1 if that slot is an active peer     */
static int g_demo_cyan_drawn;             /* DZ_MPGUI_DEMO: cyan teammate sprites drawn last frame */

/* ---- Tarkov-lite systems: inventory grid + character attributes ---- */
#define INV_COLS 6
#define INV_ROWS 5
#define INV_SLOTS (INV_COLS * INV_ROWS)
#define ITEM_NONE   0
#define ITEM_AMMO   1
#define ITEM_MEDKIT 2
#define ITEM_ARMOR        3   /* loot: flat bite-damage soak            */
#define ITEM_SCRAP        4   /* loot: currency / repair material       */
#define ITEM_WEAPON_PARTS 5   /* loot: repairs equipped weapon durability */
#define N_ITEM_TYPES      6
typedef struct { u8 type; u16 count; } Slot;
static Slot g_inv[INV_SLOTS];
static i32  g_ui_mode;      /* 0 play, 1 inventory, 2 character */
static i32  g_inv_sel;      /* selected inventory slot          */

/* attributes: 0=STR 1=AGI 2=END 3=PER */
static i32 g_attr[4];
static i32 g_attr_pts;      /* unspent level-up points */
static i32 g_char_sel;      /* selected attribute row  */
static const char *ATTR_NAME[4] = { "STR", "AGI", "END", "PER" };

/* derived stats (recomputed from attributes; read at the gameplay use-sites) */
static i32 g_hp_max;
static fx  g_move_spd;
static i32 g_mag_size;
static fx  g_hit_r;
static i32 g_reload_steps;

static void recompute_derived(void)
{
    g_hp_max       = 100 + g_attr[2] * 20;                       /* END  -> health     */
    g_move_spd     = MOVE_SPD + fx_mul(fx_from_int(g_attr[1]), fx_ratio(1,100)); /* AGI -> speed */
    g_mag_size     = MAG_SIZE + g_attr[0] * 3;                   /* STR  -> magazine   */
    g_hit_r        = Z_HIT_R + fx_mul(fx_from_int(g_attr[3]), fx_ratio(10,100));  /* PER -> aim   */
    g_reload_steps = RELOAD_STEPS - g_attr[1] * 5;              /* AGI  -> faster reload */
    if (g_reload_steps < 20) g_reload_steps = 20;
}

static void inv_add(u8 type, u16 count)
{
    for (int i = 0; i < INV_SLOTS; i++)        /* stack onto an existing slot */
        if (g_inv[i].type == type) { g_inv[i].count += count; return; }
    for (int i = 0; i < INV_SLOTS; i++)        /* else first empty slot       */
        if (g_inv[i].type == ITEM_NONE) { g_inv[i].type = type; g_inv[i].count = count; return; }
}

/* ---- LOOT / EQUIPMENT / DURABILITY ------------------------------------- */
typedef struct { u8 type; u16 cmin, cmax; u16 weight; } LootEntry;
static const LootEntry LOOT_TABLE[] = {
    { ITEM_AMMO,          4, 9, 50 },   /* common                          */
    { ITEM_SCRAP,         1, 3, 25 },
    { ITEM_MEDKIT,        1, 1, 12 },
    { ITEM_WEAPON_PARTS,  1, 2,  8 },
    { ITEM_ARMOR,         1, 1,  5 },   /* rare                            */
};
#define LOOT_TABLE_N (int)(sizeof(LOOT_TABLE)/sizeof(LOOT_TABLE[0]))
static u32 g_loot_wsum;                  /* sum of weights (set in reset)   */

#define MAX_PICKUPS 16
typedef struct { fx x, z; u8 type; u16 count; i32 alive; i32 bob; } Pickup;
static Pickup g_pickup[MAX_PICKUPS];
#define PICKUP_R fx_ratio(80,100)        /* auto-pickup radius (walk over)  */

#define WEAP_DUR_MAX      100
#define WEAP_DUR_PERSHOT  1              /* durability lost per fired round */
#define WEAP_WORN_THRESH  30             /* below this: degraded damage     */
static i32 g_weap_dur;                   /* weapon durability 0..MAX        */
static i32 g_armor;                      /* flat armor points (soaks bites) */
static i32 g_scrap;                      /* repair currency                 */

static void loot_wsum_init(void)
{
    g_loot_wsum = 0;
    for (int i = 0; i < LOOT_TABLE_N; i++) g_loot_wsum += LOOT_TABLE[i].weight;
}

/* Roll the weighted loot table and spawn a world pickup at (x,z). */
static void roll_loot(fx x, fx z)
{
    if (g_loot_wsum == 0) loot_wsum_init();
    u32 r = rng_next() % g_loot_wsum;
    const LootEntry *e = &LOOT_TABLE[0];
    for (int i = 0; i < LOOT_TABLE_N; i++) {
        if (r < LOOT_TABLE[i].weight) { e = &LOOT_TABLE[i]; break; }
        r -= LOOT_TABLE[i].weight;
    }
    u16 cnt = e->cmin;
    if (e->cmax > e->cmin) cnt = (u16)(e->cmin + (rng_next() % (u32)(e->cmax - e->cmin + 1)));
    int slot = -1;
    for (int i = 0; i < MAX_PICKUPS; i++) if (!g_pickup[i].alive) { slot = i; break; }
    if (slot < 0) return;                /* crate cap reached -- drop lost   */
    g_pickup[slot].x = x; g_pickup[slot].z = z;
    g_pickup[slot].type = e->type; g_pickup[slot].count = cnt;
    g_pickup[slot].alive = 1; g_pickup[slot].bob = 0;
}

/* Apply a collected item to the player's loadout. */
/* ====================================================================== *
 *  EXTRACTION / RAID LOOP (RAID-LOOP-0): a run is a timed RAID. Loot
 *  collected this raid is UNSECURED (g_raid_bag); standing in the EXTRACTION
 *  ZONE long enough commits the bag into the PERSISTENT cross-raid stash
 *  (g_stash) and WINS. Dying or the raid timer expiring forfeits the bag.
 *  All userspace, integer/fx only; the kernel + dzproto.h wire are untouched.
 * ====================================================================== */
#define RAID_DURATION_STEPS   11250            /* ~3:00 at STEP_MS=16            */
#define EXTRACT_X             9                 /* extraction-zone center (units)*/
#define EXTRACT_Z             (-9)
#define EXTRACT_R             fx_ratio(200,100) /* zone radius (2.0)             */
#define EXTRACT_CHANNEL_STEPS 120               /* steps held in-zone to extract */
enum { RAID_ACTIVE = 0, RAID_EXTRACTING, RAID_EXTRACTED, RAID_DIED, RAID_TIMEOUT };

static i32 g_raid_timer;                 /* steps remaining (re-init each raid)  */
static i32 g_raid_state;                 /* RAID_* (re-init each raid)           */
static i32 g_extract_hold;               /* consecutive steps held in the zone   */
static u32 g_raid_bag[N_ITEM_TYPES];     /* unsecured loot this raid (forfeitable)*/
static u32 g_stash[N_ITEM_TYPES];        /* PERSISTENT secured loot -- NEVER reset */

static void loot_apply(u8 type, u16 count)
{
    if (type < N_ITEM_TYPES) g_raid_bag[type] += count;   /* RAID: track unsecured gain */
    switch (type) {
        case ITEM_AMMO:         g_ammo  += count;       break;
        case ITEM_ARMOR:        g_armor += 5 * count;   break;
        case ITEM_SCRAP:        g_scrap += count;       break;
        case ITEM_WEAPON_PARTS: g_weap_dur += 40 * count;
                                if (g_weap_dur > WEAP_DUR_MAX) g_weap_dur = WEAP_DUR_MAX;
                                break;
        default:                inv_add(type, count);   break;   /* MEDKIT etc. */
    }
}

/* Walk-over collection: grab any alive pickup within PICKUP_R of the player. */
static void pickups_step(void)
{
    fx r2 = fx_mul(PICKUP_R, PICKUP_R);
    for (int i = 0; i < MAX_PICKUPS; i++) {
        if (!g_pickup[i].alive) continue;
        g_pickup[i].bob++;
        fx dx = g_pickup[i].x - g_px, dz = g_pickup[i].z - g_pz;
        fx d2 = fx_add(fx_mul(dx,dx), fx_mul(dz,dz));
        if (d2 <= r2) { loot_apply(g_pickup[i].type, g_pickup[i].count); g_pickup[i].alive = 0; }
    }
}

/* RAID: extraction-zone channel. Hold the zone EXTRACT_CHANNEL_STEPS continuous
 * sim-steps to commit the unsecured bag into the persistent stash and WIN the
 * raid; leaving the zone (or dying) resets the channel. */
static void extraction_step(void)
{
    if (!g_alive || g_over) return;          /* a corpse / a concluded raid can't extract */
    fx dx = g_px - fx_from_int(EXTRACT_X);
    fx dz = g_pz - fx_from_int(EXTRACT_Z);
    fx d2 = fx_add(fx_mul(dx,dx), fx_mul(dz,dz));
    if (d2 <= fx_mul(EXTRACT_R, EXTRACT_R)) {
        g_raid_state = RAID_EXTRACTING;
        if (++g_extract_hold >= EXTRACT_CHANNEL_STEPS) {
            for (int i = 0; i < N_ITEM_TYPES; i++) g_stash[i] += g_raid_bag[i];   /* secure the haul */
            g_raid_state = RAID_EXTRACTED;
            g_over = 1;                       /* raid concluded as a WIN (g_alive stays 1) */
            char b[24]; int tot = 0;
            for (int i = 0; i < N_ITEM_TYPES; i++) tot += (int)g_stash[i];
            print("[DEADZONE] extracted stash="); num_to_str(b, tot); print(b); print("\n");
        }
    } else {
        g_extract_hold = 0;
        if (g_raid_state == RAID_EXTRACTING) g_raid_state = RAID_ACTIVE;
    }
}

/* Tag a concluding raid as a death-loss -- unless it already concluded as a
 * TIMEOUT (set just before g_alive=0) or an EXTRACTED win. Without this, dying
 * WHILE channeling extraction left g_raid_state stuck at RAID_EXTRACTING, so the
 * green "EXTRACTING..." banner drew on top of the "YOU DIED" screen. Shared by
 * the main loop's loss latch and the headless raid_selftest. */
static void raid_tag_death(void)
{
    if (g_raid_state != RAID_TIMEOUT && g_raid_state != RAID_EXTRACTED)
        g_raid_state = RAID_DIED;
}

static fx fx_clamp(fx v, fx lo, fx hi) { return v < lo ? lo : (v > hi ? hi : v); }
static fx dirx(i32 h) { return fx_sin(h & G3D_ANG_MASK); }
static fx dirz(i32 h) { return fx_cos(h & G3D_ANG_MASK); }

/* ====================================================================== *
 *  Meshes (hand-built boxes; no assets)
 * ====================================================================== */
static g3d_tri g_zmesh[12];
#define MAX_WORLD 320   /* was 220; build_world already pushes ~280 tris (floor+walls+
                         * trees) so the old cap silently clipped geometry -- raised to
                         * fit the new extraction-zone pad AND un-clip the trees. */
static g3d_tri g_world[MAX_WORLD];
static int     g_nworld;

static void box_mesh(g3d_tri *m, fx X, fx Y, fx Z)
{
    vec3 v000=v3(-X,-Y,-Z), v100=v3(X,-Y,-Z), v110=v3(X,Y,-Z), v010=v3(-X,Y,-Z);
    vec3 v001=v3(-X,-Y, Z), v101=v3(X,-Y, Z), v111=v3(X,Y, Z), v011=v3(-X,Y, Z);
    int i=0;
    m[i].v[0]=v001; m[i].v[1]=v101; m[i].v[2]=v111; i++;
    m[i].v[0]=v001; m[i].v[1]=v111; m[i].v[2]=v011; i++;
    m[i].v[0]=v100; m[i].v[1]=v000; m[i].v[2]=v010; i++;
    m[i].v[0]=v100; m[i].v[1]=v010; m[i].v[2]=v110; i++;
    m[i].v[0]=v000; m[i].v[1]=v001; m[i].v[2]=v011; i++;
    m[i].v[0]=v000; m[i].v[1]=v011; m[i].v[2]=v010; i++;
    m[i].v[0]=v101; m[i].v[1]=v100; m[i].v[2]=v110; i++;
    m[i].v[0]=v101; m[i].v[1]=v110; m[i].v[2]=v111; i++;
    m[i].v[0]=v011; m[i].v[1]=v111; m[i].v[2]=v110; i++;
    m[i].v[0]=v011; m[i].v[1]=v110; m[i].v[2]=v010; i++;
    m[i].v[0]=v000; m[i].v[1]=v100; m[i].v[2]=v101; i++;
    m[i].v[0]=v000; m[i].v[1]=v101; m[i].v[2]=v001; i++;
}

static void tri_push(vec3 a, vec3 b, vec3 c, u32 col)
{
    if (g_nworld >= MAX_WORLD) return;
    g_world[g_nworld].v[0]=a; g_world[g_nworld].v[1]=b; g_world[g_nworld].v[2]=c;
    g_world[g_nworld].color=col; g_nworld++;
}
static void quad_push(vec3 a, vec3 b, vec3 c, vec3 d, u32 col, int two)
{
    tri_push(a,b,c,col); tri_push(a,c,d,col);
    if (two) { tri_push(a,c,b,col); tri_push(a,d,c,col); }
}
/* push a colored box into the world mesh at (cx,cz), base on floor, size hx/hy/hz */
static void world_box(fx cx, fx cz, fx hx, fx hy, fx hz, u32 col)
{
    fx y0=0, y1=fx_mul(hy,fx_from_int(2));
    fx x0=cx-hx, x1=cx+hx, z0=cz-hz, z1=cz+hz;
    vec3 a000=v3(x0,y0,z0),a100=v3(x1,y0,z0),a110=v3(x1,y1,z0),a010=v3(x0,y1,z0);
    vec3 a001=v3(x0,y0,z1),a101=v3(x1,y0,z1),a111=v3(x1,y1,z1),a011=v3(x0,y1,z1);
    quad_push(a001,a101,a111,a011,col,0);   /* +Z */
    quad_push(a100,a000,a010,a110,col,0);   /* -Z */
    quad_push(a000,a001,a011,a010,col,0);   /* -X */
    quad_push(a101,a100,a110,a111,col,0);   /* +X */
    quad_push(a011,a111,a110,a010,col,0);   /* +Y top */
}

static void build_world(void)
{
    g_nworld = 0;
    int N = 8;
    fx half = fx_from_int(ARENA_HALF_I);
    fx cell = fx_div(fx_mul(half, fx_from_int(2)), fx_from_int(N));
    for (int gz=0; gz<N; gz++) for (int gx=0; gx<N; gx++) {
        fx x0=fx_add(-half,fx_mul(cell,fx_from_int(gx))), x1=fx_add(x0,cell);
        fx z0=fx_add(-half,fx_mul(cell,fx_from_int(gz))), z1=fx_add(z0,cell);
        u32 col = ((gx+gz)&1) ? 0xFF1E2A1Cu : 0xFF15201Au;   /* dim grass tones */
        quad_push(v3(x0,0,z1),v3(x1,0,z1),v3(x1,0,z0),v3(x0,0,z0), col, 0);
    }
    /* RAID: bright extraction-zone pad on the floor so the player can find it. */
    {
        fx ex=fx_from_int(EXTRACT_X), ez=fx_from_int(EXTRACT_Z), pr=EXTRACT_R, y=fx_ratio(3,100);
        quad_push(v3(ex-pr,y,ez+pr), v3(ex+pr,y,ez+pr), v3(ex+pr,y,ez-pr), v3(ex-pr,y,ez-pr), 0xFF28E048u, 0);
    }
    fx H = fx_from_int(2);
    u32 wcol = 0xFF2B2F36u;
    quad_push(v3(-half,0,half), v3(half,0,half), v3(half,H,half), v3(-half,H,half), wcol,1);
    quad_push(v3(-half,0,-half),v3(half,0,-half),v3(half,H,-half),v3(-half,H,-half),wcol,1);
    quad_push(v3(half,0,-half), v3(half,0,half), v3(half,H,half), v3(half,H,-half), wcol,1);
    quad_push(v3(-half,0,-half),v3(-half,0,half),v3(-half,H,half),v3(-half,H,-half),wcol,1);
    /* a few trees: brown trunk + green canopy boxes (procedural-ish fixed spots) */
    static const int TX[6] = { -9, 7, -4, 10, 3, -11 };
    static const int TZ[6] = {  6, -8, -10, 4, 11, -3 };
    for (int i=0; i<6; i++) {
        fx tx=fx_from_int(TX[i]), tz=fx_from_int(TZ[i]);
        world_box(tx,tz, fx_ratio(18,100), fx_ratio(70,100), fx_ratio(18,100), 0xFF5A3A20u); /* trunk */
        world_box(tx,tz, fx_ratio(70,100), fx_ratio(55,100), fx_ratio(70,100), 0xFF2E6B2Eu); /* canopy lower */
    }
}

/* ====================================================================== *
 *  Reset / waves
 * ====================================================================== */
static void start_wave(int w)
{
    g_wave = w;
    g_wave_size = 4 + (w - 1) * 2;          /* 4, 6, 8, ... */
    if (g_wave_size > MAX_Z) g_wave_size = MAX_Z;
    g_spawned = 0;
    g_spawn_timer = 0;
    if (w > 1) g_attr_pts++;          /* a level-up point per cleared wave */
}

static void deadzone_reset(u64 seed)
{
    g_rng = (u32)(seed ^ 0x9E3779B9u) | 1u;
    for (int i=0;i<MAX_Z;i++) { g_z[i].alive=0; g_z[i].health=0; g_z[i].hitflash=0; g_z[i].bite_cd=0; }
    g_px = 0; g_pz = 0; g_yaw = 0;
    for (int i = 0; i < 4; i++) g_attr[i] = 0;
    g_attr_pts = 0; g_char_sel = 0;
    recompute_derived();
    g_hp = g_hp_max; g_alive = 1; g_over = 0;
    g_mag = g_mag_size; g_ammo = g_mag_size * 6;
    g_reload = 0; g_fire_cd = 0; g_recoil = 0; g_muzzle = 0;
    g_kills = 0;
    g_have_mouse = 0;
    g_ui_mode = 0; g_inv_sel = 0;
    for (int i = 0; i < INV_SLOTS; i++) { g_inv[i].type = ITEM_NONE; g_inv[i].count = 0; }
    inv_add(ITEM_AMMO, g_mag_size * 6);   /* starting kit */
    inv_add(ITEM_MEDKIT, 2);
    /* loot/equipment/durability */
    for (int i = 0; i < MAX_PICKUPS; i++) g_pickup[i].alive = 0;
    g_weap_dur = WEAP_DUR_MAX; g_armor = 0; g_scrap = 0;
    /* RAID: fresh raid clock + state. g_stash is PERSISTENT -- never reset here. */
    g_raid_timer = RAID_DURATION_STEPS;
    g_raid_state = RAID_ACTIVE;
    g_extract_hold = 0;
    for (int i = 0; i < N_ITEM_TYPES; i++) g_raid_bag[i] = 0;
    loot_wsum_init();
    start_wave(1);
}

static int z_alive_count(void)
{
    int n=0; for (int i=0;i<MAX_Z;i++) if (g_z[i].alive) n++; return n;
}

static void spawn_one(void)
{
    int slot=-1; for (int i=0;i<MAX_Z;i++) if (!g_z[i].alive) { slot=i; break; }
    if (slot<0) return;
    i32 ang = rng_next() & G3D_ANG_MASK;
    fx ring = fx_from_int(ARENA_HALF_I) - fx_from_int(1);
    Zombie *z = &g_z[slot];
    z->x = fx_mul(fx_sin(ang), ring);
    z->z = fx_mul(fx_cos(ang), ring);
    z->head = (ang + (G3D_ANG_STEPS/2)) & G3D_ANG_MASK;
    z->health = 30 + (g_wave - 1) * 12;     /* hp scales per wave */
    z->alive = 1;
    z->hitflash = 0;
    z->bite_cd = 0;
    g_spawned++;
}

static void wave_update(void)
{
    if (g_spawned < g_wave_size) {
        if (g_spawn_timer > 0) g_spawn_timer--;
        else { spawn_one(); g_spawn_timer = 18; }   /* one every ~0.3s */
    } else if (z_alive_count() == 0) {
        start_wave(g_wave + 1);                       /* cleared -> next wave */
    }
}

static void hurt_zombie(int i, int dmg)
{
    Zombie *z = &g_z[i];
    if (!z->alive) return;
    z->health -= dmg;
    z->hitflash = 5;
    if (z->health <= 0) {
        z->alive = 0; g_kills++;
        g_ammo += 3;                                   /* scavenge rounds */
        roll_loot(z->x, z->z);                         /* weighted loot drop */
    }
}

/* ====================================================================== *
 *  Input -> movement
 * ====================================================================== */
/* Advance the per-shot cosmetic timers one sim-step: recoil decay, muzzle flash,
 * fire cooldown, and reload completion. Shared by single-player player_step AND
 * the MP loop (where player_step is gated off but these must still advance, or
 * the muzzle flash / fire cooldown freeze after the first co-op shot). */
static void decay_timers(void)
{
    if (g_recoil > 0) g_recoil = fx_mul(g_recoil, fx_ratio(80,100));
    if (g_muzzle > 0) g_muzzle--;
    if (g_fire_cd > 0) g_fire_cd--;
    if (g_reload > 0) { g_reload--; if (g_reload == 0) {
        int need = g_mag_size - g_mag; if (need > g_ammo) need = g_ammo;
        g_mag += need; g_ammo -= need; } }
}

static void player_step(void)
{
    if (!g_alive) return;
    /* A/D fallback turn */
    if (g_hold_tl) g_yaw = (g_yaw - TURN_KEY) & G3D_ANG_MASK;
    if (g_hold_tr) g_yaw = (g_yaw + TURN_KEY) & G3D_ANG_MASK;

    fx fdx = dirx(g_yaw), fdz = dirz(g_yaw);          /* forward */
    fx rdx = dirz(g_yaw), rdz = -dirx(g_yaw);         /* right = forward rotated -90 */
    fx mf = 0, ms = 0;
    if (g_hold_fwd)  mf = fx_add(mf,  g_move_spd);   /* AGI-scaled */
    if (g_hold_back) mf = fx_sub(mf,  g_move_spd);
    if (g_hold_right)ms = fx_add(ms,  g_move_spd);
    if (g_hold_left) ms = fx_sub(ms,  g_move_spd);

    fx nx = fx_add(g_px, fx_add(fx_mul(fdx, mf), fx_mul(rdx, ms)));
    fx nz = fx_add(g_pz, fx_add(fx_mul(fdz, mf), fx_mul(rdz, ms)));
    /* keep the player inside the arena */
    fx lim = fx_from_int(ARENA_HALF_I) - PLAYER_R - fx_ratio(20,100);
    g_px = fx_clamp(nx, -lim, lim);
    g_pz = fx_clamp(nz, -lim, lim);

    decay_timers();
}

static void try_shoot(void)
{
    if (!g_alive || g_over || g_fire_cd > 0 || g_reload > 0) return;   /* MP-WIN-FIX: no fire after a concluded raid */
    if (g_mag <= 0) { if (g_ammo > 0) g_reload = g_reload_steps; return; }
    g_mag--; g_fire_cd = FIRE_COOLDN; g_recoil = fx_ratio(6,100); g_muzzle = 3;
    audio_trigger(0);                                    /* gunshot SFX */
    if (g_weap_dur > 0) g_weap_dur -= WEAP_DUR_PERSHOT;   /* weapon wears with use */

    fx fdx = dirx(g_yaw), fdz = dirz(g_yaw);
    int best=-1; fx bestt=fx_from_int(9999);
    for (int i=0;i<MAX_Z;i++) {
        if (!g_z[i].alive) continue;
        fx dx = g_z[i].x - g_px, dz = g_z[i].z - g_pz;
        fx t = fx_add(fx_mul(dx, fdx), fx_mul(dz, fdz));     /* forward distance */
        if (t <= 0) continue;
        fx perp = fx_sub(fx_mul(dx, fdz), fx_mul(dz, fdx));   /* lookdir unit -> |perp| = miss dist */
        if (perp < 0) perp = -perp;
        if (perp <= g_hit_r && t < bestt) { best = i; bestt = t; }   /* PER-scaled aim */
    }
    if (best >= 0) {
        /* a worn weapon (durability < threshold) does proportionally less damage */
        int dmg = (g_weap_dur < WEAP_WORN_THRESH)
                  ? (WEAPON_DMG * g_weap_dur) / WEAP_DUR_MAX + 1
                  : WEAPON_DMG;
        hurt_zombie(best, dmg);
    }
}

/* ====================================================================== *
 *  Zombie AI: hunt the player; bite on contact
 * ====================================================================== */
static void zombies_step(void)
{
    for (int i=0;i<MAX_Z;i++) {
        Zombie *z = &g_z[i];
        if (z->hitflash > 0) z->hitflash--;
        if (z->bite_cd > 0) z->bite_cd--;
        if (!z->alive) continue;

        fx dx = g_px - z->x, dz = g_pz - z->z;
        fx d2 = fx_add(fx_mul(dx,dx), fx_mul(dz,dz));
        /* face the player */
        fx zdx = dirx(z->head), zdz = dirz(z->head);
        fx cross = fx_sub(fx_mul(zdx, dz), fx_mul(zdz, dx));
        if (cross > fx_ratio(3,100)) z->head = (z->head + 12) & G3D_ANG_MASK;
        else if (cross < -fx_ratio(3,100)) z->head = (z->head - 12) & G3D_ANG_MASK;

        fx attack2 = fx_mul(Z_ATTACK_R, Z_ATTACK_R);
        if (d2 > attack2) {
            /* walk toward the player along its heading */
            z->x = fx_add(z->x, fx_mul(dirx(z->head), Z_SPEED));
            z->z = fx_add(z->z, fx_mul(dirz(z->head), Z_SPEED));
        } else {
            /* in range: bite on cooldown */
            if (g_alive && z->bite_cd == 0) {
                int bd = Z_BITE_DMG - g_armor / 4;     /* armor soaks bite dmg */
                if (bd < 1) bd = 1;                     /* always at least 1    */
                g_hp -= bd; z->bite_cd = Z_BITE_CD;
                audio_trigger(1);                       /* zombie bite SFX */
                if (g_hp <= 0) { g_hp = 0; g_alive = 0; }
            }
        }
        /* light separation so they don't fully stack */
        for (int j=i+1;j<MAX_Z;j++) {
            if (!g_z[j].alive) continue;
            fx sx = g_z[j].x - z->x, sz = g_z[j].z - z->z;
            fx s2 = fx_add(fx_mul(sx,sx), fx_mul(sz,sz));
            fx mind = Z_R + Z_R;
            if (s2 > 0 && s2 < fx_mul(mind,mind)) {
                fx d = fx_sqrt(s2); if (d < fx_ratio(1,100)) d = fx_ratio(1,100);
                fx nx = fx_div(sx,d), nz = fx_div(sz,d);
                fx push = fx_ratio(2,100);
                z->x = fx_sub(z->x, fx_mul(nx,push)); z->z = fx_sub(z->z, fx_mul(nz,push));
                g_z[j].x = fx_add(g_z[j].x, fx_mul(nx,push)); g_z[j].z = fx_add(g_z[j].z, fx_mul(nz,push));
            }
        }
    }
}

/* ====================================================================== *
 *  Render
 * ====================================================================== */
/* PSX look: render the 3D scene into a small low-res target, then nearest-
 * upscale-blit it to the window. Gives chunky PSX pixels + ~4x faster software
 * raster. The HUD is drawn at full window res on top. */
#define LOWW 320
#define LOWH 240
static u32      g_lowfb[LOWW * LOWH];
static g3d_i32  g_lowz[LOWW * LOWH];
#define BG_COLOR 0xFF0A0F14u

static void upscale_blit(wl_window *win)
{
    u32 stride = win->stride / 4u;
    int ww = (int)win->w, wh = (int)win->h;
    for (int y = 0; y < wh; y++) {
        int ly = (y * LOWH) / wh; if (ly >= LOWH) ly = LOWH - 1;
        const u32 *src = g_lowfb + (u64)ly * LOWW;
        u32 *dst = win->pixels + (u64)y * stride;
        for (int x = 0; x < ww; x++) {
            int lx = (x * LOWW) / ww; if (lx >= LOWW) lx = LOWW - 1;
            dst[x] = src[lx];
        }
    }
}

static u32 tint_white(u32 c, int amt)
{
    int r=(c>>16)&0xFF, g=(c>>8)&0xFF, b=c&0xFF;
    r += ((255-r)*amt)>>8; g += ((255-g)*amt)>>8; b += ((255-b)*amt)>>8;
    return 0xFF000000u | (r<<16)|(g<<8)|b;
}

static mat4 camera_view(void)
{
    vec3 up = v3(0, FX_ONE, 0);
    fx lx = dirx(g_yaw), lz = dirz(g_yaw);
    vec3 eye = v3(g_px, EYE_H, g_pz);
    vec3 tgt = v3(fx_add(g_px, lx), fx_sub(EYE_H, fx_ratio(15,100)), fx_add(g_pz, lz));
    return mat4_lookat(eye, tgt, up);
}

/* ====================================================================== *
 *  VISUAL OVERHAUL: generated pixel-art assets + billboard sprite rendering.
 *  No GPU, no texture-mapping in g3d -- so enemies/loot are drawn as billboarded
 *  image sprites blitted into the low-res framebuffer (DOOM-style), painter-
 *  sorted far->near with distance fog. Assets are generated procedurally at
 *  launch (gen_assets) into static ARGB buffers; alpha 0 == transparent.
 * ====================================================================== */
#define ZSPR_W 16
#define ZSPR_H 28
static u32 g_spr_zombie[ZSPR_W * ZSPR_H];
#define LSPR_W 10
#define LSPR_H 10
static u32 g_spr_loot[LSPR_W * LSPR_H];   /* white base, multiplied by a per-type tint */

static void spr_rect(u32 *s, int w, int h, int x, int y, int rw, int rh, u32 c) {
    for (int yy = y; yy < y + rh; yy++) { if (yy < 0 || yy >= h) continue;
        for (int xx = x; xx < x + rw; xx++) { if (xx < 0 || xx >= w) continue; s[yy*w + xx] = c; } }
}

static void gen_assets(void) {
    for (int i = 0; i < ZSPR_W*ZSPR_H; i++) g_spr_zombie[i] = 0;       /* transparent */
    u32 skin = 0xFF6E8E4Eu, skinD = 0xFF566E3Cu, shirt = 0xFF34402Fu,
        pants = 0xFF24242Au, blood = 0xFF7A1E1Eu;
    /* head */
    spr_rect(g_spr_zombie, ZSPR_W, ZSPR_H, 5, 2, 6, 6, skin);
    spr_rect(g_spr_zombie, ZSPR_W, ZSPR_H, 5, 2, 2, 6, skinD);        /* left shade */
    g_spr_zombie[3*ZSPR_W + 6] = 0xFFB01010u;                         /* eyes (glow) */
    g_spr_zombie[3*ZSPR_W + 9] = 0xFFB01010u;
    g_spr_zombie[6*ZSPR_W + 7] = blood;                              /* mouth */
    /* torso: torn shirt + exposed wound */
    spr_rect(g_spr_zombie, ZSPR_W, ZSPR_H, 4, 8, 8, 11, shirt);
    spr_rect(g_spr_zombie, ZSPR_W, ZSPR_H, 4, 8, 2, 11, skinD);
    spr_rect(g_spr_zombie, ZSPR_W, ZSPR_H, 7, 12, 3, 4, skin);
    g_spr_zombie[13*ZSPR_W + 8] = blood;
    /* arms reaching (down the sides) */
    spr_rect(g_spr_zombie, ZSPR_W, ZSPR_H, 1, 9, 3, 8, skin);
    spr_rect(g_spr_zombie, ZSPR_W, ZSPR_H, 12, 9, 3, 8, skinD);
    /* legs */
    spr_rect(g_spr_zombie, ZSPR_W, ZSPR_H, 5, 19, 3, 9, pants);
    spr_rect(g_spr_zombie, ZSPR_W, ZSPR_H, 8, 19, 3, 9, pants);
    /* loot crate: white base (tinted per type at blit) + dark border */
    for (int i = 0; i < LSPR_W*LSPR_H; i++) g_spr_loot[i] = 0xFFFFFFFFu;
    spr_rect(g_spr_loot, LSPR_W, LSPR_H, 0, 0, LSPR_W, 1, 0xFF606060u);
    spr_rect(g_spr_loot, LSPR_W, LSPR_H, 0, LSPR_H-1, LSPR_W, 1, 0xFF303030u);
    spr_rect(g_spr_loot, LSPR_W, LSPR_H, 0, 0, 1, LSPR_H, 0xFF505050u);
    spr_rect(g_spr_loot, LSPR_W, LSPR_H, LSPR_W-1, 0, 1, LSPR_H, 0xFF303030u);
    spr_rect(g_spr_loot, LSPR_W, LSPR_H, 4, 0, 2, LSPR_H, 0xFF707070u); /* strap */
}

/* Nearest-scaled sprite blit into the low-res framebuffer, multiplied by `mul`
 * (0xFFRRGGBB; use 0xFFFFFFFF for unchanged). Strictly bounds-clipped -> never
 * writes outside g_lowfb (a wrong projection looks wrong but cannot crash). */
static void blit_spr(const u32 *spr, int sw, int sh, int dx, int dy, int dw, int dh, u32 mul) {
    if (dw <= 0 || dh <= 0) return;
    int mr = (mul>>16)&0xFF, mg = (mul>>8)&0xFF, mb = mul&0xFF;
    for (int py = dy; py < dy + dh; py++) {
        if (py < 0 || py >= LOWH) continue;
        int sy = (py - dy) * sh / dh; if (sy < 0) sy = 0; if (sy >= sh) sy = sh - 1;
        u32 *row = g_lowfb + (u64)py * LOWW;
        for (int px = dx; px < dx + dw; px++) {
            if (px < 0 || px >= LOWW) continue;
            int sx = (px - dx) * sw / dw; if (sx < 0) sx = 0; if (sx >= sw) sx = sw - 1;
            u32 c = spr[sy*sw + sx];
            if (!(c >> 24)) continue;                                /* transparent */
            int cr = (c>>16)&0xFF, cg = (c>>8)&0xFF, cb = c&0xFF;
            row[px] = 0xFF000000u | (((cr*mr)/255)<<16) | (((cg*mg)/255)<<8) | ((cb*mb)/255);
        }
    }
}

/* Apocalyptic gradient sky filled into the low-res framebuffer (replaces the
 * flat clear). The ground mesh paints over the lower half. */
static void sky_fill(g3d_target *t) {
    int half = LOWH/2; if (half < 1) half = 1;
    for (int y = 0; y < LOWH; y++) {
        int yy = y > half ? half : y;
        int r = 10 + (62-10)*yy/half, g = 12 + (58-12)*yy/half, b = 20 + (40-20)*yy/half;
        u32 c = 0xFF000000u | ((u32)r<<16) | ((u32)g<<8) | (u32)b;
        u32 *row = t->pixels + (g3d_i32)y * t->stride;
        for (int x = 0; x < t->w; x++) row[x] = c;
    }
}

static u32 loot_color(u8 type) {
    switch (type) {
        case ITEM_AMMO:         return 0xFFE9C46Au;   /* amber  */
        case ITEM_MEDKIT:       return 0xFFE63946u;   /* red    */
        case ITEM_ARMOR:        return 0xFF5A8AE6u;   /* blue   */
        case ITEM_WEAPON_PARTS: return 0xFF4FD0C0u;   /* cyan   */
        default:                return 0xFFB0B0B0u;   /* scrap/grey */
    }
}

/* Project a world point through the view matrix into screen space (pinhole;
 * camera looks down -Z). Returns 1 + fills sx/syb/depth if in front. */
static int project_pt(mat4 view, fx wx, fx wy, fx wz, fx focal, int *sx, int *syb, fx *depth) {
    vec3 e = mat4_mul_point(view, v3(wx, wy, wz));
    fx d = -e.z;
    if (d <= fx_ratio(2,10)) return 0;                /* behind / too near */
    *sx  = LOWW/2 + fx_to_int(fx_mul(fx_div(e.x, d), focal));
    *syb = LOWH/2 - fx_to_int(fx_mul(fx_div(e.y, d), focal));
    *depth = d;
    return 1;
}

static void render_scene(g3d_target *tgt, mat4 proj, vec3 light)
{
    mat4 view = camera_view();
    mat4 vp = mat4_mul(proj, view);
    sky_fill(tgt);                                    /* gradient sky */
    g3d_clear_depth(tgt);
    g3d_draw_mesh(tgt, g_world, g_nworld, mat4_identity(), vp, light);

    /* focal length for billboard projection (matches the 70-deg fov in _start) */
    fx tan35 = fx_div(fx_sin(g3d_deg(35)), fx_cos(g3d_deg(35)));
    fx focal = fx_div(fx_from_int(LOWW/2), tan35);

    /* collect alive zombies + painter-sort far->near (MAX_Z small) */
    int order[MAX_Z]; fx depth[MAX_Z]; int nz = 0;
    for (int i = 0; i < MAX_Z; i++) {
        if (!g_z[i].alive) continue;
        int sx, syb; fx d;
        if (!project_pt(view, g_z[i].x, 0, g_z[i].z, focal, &sx, &syb, &d)) continue;
        order[nz] = i; depth[nz] = d; nz++;
    }
    for (int a = 1; a < nz; a++) {
        int oi = order[a]; fx od = depth[a]; int b = a - 1;
        while (b >= 0 && depth[b] < od) { order[b+1]=order[b]; depth[b+1]=depth[b]; b--; }
        order[b+1] = oi; depth[b+1] = od;
    }
    for (int q = 0; q < nz; q++) {
        int i = order[q]; int sx, syb; fx d;
        if (!project_pt(view, g_z[i].x, 0, g_z[i].z, focal, &sx, &syb, &d)) continue;
        int h = fx_to_int(fx_div(fx_mul(fx_ratio(16,10), focal), d));
        if (h < 6) h = 6; if (h > 420) h = 420;
        int w = h * ZSPR_W / ZSPR_H;
        int fog = 255 - fx_to_int(d) * 13; if (fog < 70) fog = 70; if (fog > 255) fog = 255;
        u32 mul = g_z[i].hitflash ? 0xFFFFFFFFu
                                  : (0xFF000000u | ((u32)fog<<16) | ((u32)fog<<8) | (u32)fog);
        blit_spr(g_spr_zombie, ZSPR_W, ZSPR_H, sx - w/2, syb - h, w, h, mul);
    }

    /* MP-GUI-1: other co-op players from the latest snapshot, as billboards.
     * Reuse the zombie sprite tinted cyan so teammates read as friendly. Gated
     * on g_mp_on => single-player draws nothing here (g_ron is all-zero). */
    g_demo_cyan_drawn = 0;
    for (int i = 0; g_mp_on && i < DZP_MAX_CLIENTS; i++) {
        if (!g_ron[i]) continue;
        int sx, syb; fx d;
        if (!project_pt(view, g_rx[i], 0, g_rz[i], focal, &sx, &syb, &d)) continue;
        int h = fx_to_int(fx_div(fx_mul(fx_ratio(16,10), focal), d));
        if (h < 6) h = 6; if (h > 420) h = 420;
        int w = h * ZSPR_W / ZSPR_H;
        blit_spr(g_spr_zombie, ZSPR_W, ZSPR_H, sx - w/2, syb - h, w, h, 0xFF40C0FFu);
        g_demo_cyan_drawn++;                /* DZ_MPGUI_DEMO render proof */
    }

    /* loot pickups as billboarded crates on the ground (bob up/down) */
    for (int i = 0; i < MAX_PICKUPS; i++) {
        if (!g_pickup[i].alive) continue;
        int sx, syb; fx d;
        if (!project_pt(view, g_pickup[i].x, fx_ratio(12,100), g_pickup[i].z, focal, &sx, &syb, &d)) continue;
        int h = fx_to_int(fx_div(fx_mul(fx_ratio(5,10), focal), d));
        if (h < 4) h = 4; if (h > 140) h = 140;
        int bob = (g_pickup[i].bob / 6) % 5; if (bob > 2) bob = 4 - bob;
        blit_spr(g_spr_loot, LSPR_W, LSPR_H, sx - h/2, syb - h - bob, h, h, loot_color(g_pickup[i].type));
    }
}

/* ====================================================================== *
 *  HUD
 * ====================================================================== */
static void fill_rect(u32 *px,int stride,int w,int h,int x,int y,int rw,int rh,u32 col)
{
    if (x<0){rw+=x;x=0;} if (y<0){rh+=y;y=0;}
    if (x+rw>w) rw=w-x; if (y+rh>h) rh=h-y;
    for (int yy=0;yy<rh;yy++){ u32 *row=px+(u64)(y+yy)*stride+x; for(int xx=0;xx<rw;xx++) row[xx]=col; }
}
static void num_to_str(char *buf, int v) {
    int n=0; char tmp[12]; if (v<0) v=0;
    if (v==0) tmp[n++]='0'; else while (v>0){ tmp[n++]=(char)('0'+v%10); v/=10; }
    int k=0; while (n>0) buf[k++]=tmp[--n]; buf[k]=0;
}

static void draw_hud(wl_window *win, int tw, int th)
{
    u32 *px = win->pixels; int stride=(int)(win->stride/4u);
    char buf[24], lbl[40]; int n;

    /* crosshair */
    int cx=tw/2, cy=th/2;
    fill_rect(px,stride,tw,th, cx-7,cy-1,14,2, 0xFFE0E0E0u);
    fill_rect(px,stride,tw,th, cx-1,cy-7,2,14, 0xFFE0E0E0u);
    if (g_muzzle>0) fill_rect(px,stride,tw,th, cx-3,cy-3,6,6, 0xFFFFE060u);

    /* health bar top-left */
    int bx=12, by=12, bw=200, bh=16;
    fill_rect(px,stride,tw,th, bx-2,by-2,bw+4,bh+4, 0xFF000000u);
    fill_rect(px,stride,tw,th, bx,by,bw,bh, 0xFF333A44u);
    int hpw=(bw*(g_hp<0?0:g_hp))/(g_hp_max>0?g_hp_max:1);
    u32 hpc = g_hp>50?0xFF3CCB5Au:(g_hp>20?0xFFE9C46Au:0xFFE63946u);
    fill_rect(px,stride,tw,th, bx,by,hpw,bh, hpc);
    font_draw_string(px,stride,tw,th, bx+4,by, g_alive?"HP":"DEAD", 0xFFFFFFFFu);

    /* weapon durability bar + armor/scrap readout. SP raid-economy values; in
     * server-authoritative co-op they're frozen/meaningless, so hide them in MP
     * (matches the RAID-timer !g_mp_on gate below). */
    if (!g_mp_on) {
    int dby=by+bh+6, dbw=140, dbh=8;
    fill_rect(px,stride,tw,th, bx-2,dby-2,dbw+4,dbh+4, 0xFF000000u);
    fill_rect(px,stride,tw,th, bx,dby,dbw,dbh, 0xFF333A44u);
    int durw=(dbw*(g_weap_dur<0?0:g_weap_dur))/WEAP_DUR_MAX;
    u32 durc=g_weap_dur>WEAP_WORN_THRESH?0xFF8AB4F8u:0xFFE9C46Au;
    fill_rect(px,stride,tw,th, bx,dby,durw,dbh, durc);
    font_draw_string(px,stride,tw,th, bx+dbw+8,dby-3, "DUR", 0xFFAAB2BFu);
    /* armor + scrap readout */
    { int p=0; const char *ar="ARMOR "; while(ar[p]){lbl[p]=ar[p];p++;}
      char b2[24]; num_to_str(b2,g_armor); int kk=0; while(b2[kk]){lbl[p++]=b2[kk++];}
      const char *sl="  SCRAP "; int mm=0; while(sl[mm]){lbl[p++]=sl[mm++];}
      num_to_str(b2,g_scrap); kk=0; while(b2[kk]){lbl[p++]=b2[kk++];} lbl[p]=0;
      font_draw_string(px,stride,tw,th, bx, dby+dbh+6, lbl, 0xFFBFC7D2u); }
    }

    /* ammo bottom-right */
    n=0; const char *al="AMMO "; while(al[n]){lbl[n]=al[n];n++;}
    num_to_str(buf, g_mag); int k=0; while(buf[k]){lbl[n++]=buf[k++];}
    lbl[n++]='/'; num_to_str(buf, g_ammo); k=0; while(buf[k]){lbl[n++]=buf[k++];} lbl[n]=0;
    font_draw_string(px,stride,tw,th, tw-12-(int)k_strlen(lbl)*FONT_W, th-FONT_H-10, lbl,
                     g_reload?0xFFE9C46Au:0xFFFFFFFFu);
    if (g_reload) font_draw_string(px,stride,tw,th, tw/2-32, th/2+24, "RELOADING", 0xFFE9C46Au);

    /* wave + kills top-right */
    n=0; const char *wl_="WAVE "; while(wl_[n]){lbl[n]=wl_[n];n++;}
    num_to_str(buf, g_wave); k=0; while(buf[k]){lbl[n++]=buf[k++];}
    const char *kl="  KILLS "; int m=0; while(kl[m]){lbl[n++]=kl[m++];}
    num_to_str(buf, g_kills); k=0; while(buf[k]){lbl[n++]=buf[k++];} lbl[n]=0;
    font_draw_string(px,stride,tw,th, tw-12-(int)k_strlen(lbl)*FONT_W, 14, lbl, 0xFFFFFFFFu);

    /* RAID timer (mm:ss) + SECURED(stash) / RISK(unsecured bag), top-center.
     * MP-OVER-FIX (part 2): SP-only -- in server-authoritative co-op the local
     * raid clock is frozen + the stash/bag are meaningless, so hide them (matches
     * the game-over overlay's !g_mp_on gate below). */
    if (!g_mp_on) {
        int secs = (g_raid_timer * STEP_MS) / 1000, mins = secs / 60; secs %= 60;
        n=0; const char *rl="RAID "; while(rl[n]){lbl[n]=rl[n];n++;}
        num_to_str(buf, mins); k=0; while(buf[k]){lbl[n++]=buf[k++];}
        lbl[n++]=':'; if (secs<10) lbl[n++]='0';
        num_to_str(buf, secs); k=0; while(buf[k]){lbl[n++]=buf[k++];} lbl[n]=0;
        u32 tc = (g_raid_timer < (20*1000)/STEP_MS) ? 0xFFE63946u : 0xFFFFFFFFu;   /* red under ~20s */
        font_draw_string(px,stride,tw,th, tw/2-(int)k_strlen(lbl)*FONT_W/2, 14, lbl, tc);
        int sec=0, risk=0;
        for (int i=0;i<N_ITEM_TYPES;i++){ sec += (int)g_stash[i]; risk += (int)g_raid_bag[i]; }
        n=0; const char *cl="SECURED "; while(cl[n]){lbl[n]=cl[n];n++;}
        num_to_str(buf, sec); k=0; while(buf[k]){lbl[n++]=buf[k++];}
        const char *rk="  RISK "; int mq=0; while(rk[mq]){lbl[n++]=rk[mq++];}
        num_to_str(buf, risk); k=0; while(buf[k]){lbl[n++]=buf[k++];} lbl[n]=0;
        font_draw_string(px,stride,tw,th, tw/2-(int)k_strlen(lbl)*FONT_W/2, 26, lbl, 0xFFBFC7D2u);
        if (g_raid_state == RAID_EXTRACTING)
            font_draw_string(px,stride,tw,th, tw/2-44, th/2+40, "EXTRACTING...", 0xFF3CCB5Au);
    }

    /* weapon viewmodel (bottom-center) with a recoil kick + muzzle flash */
    int kick = g_muzzle>0 ? 7 : (g_recoil>0 ? 3 : 0);
    int gx=tw/2-14, gy=th-82+kick;
    fill_rect(px,stride,tw,th, gx-10,gy+46,46,26, 0xFF222A33u);   /* stock/grip   */
    fill_rect(px,stride,tw,th, gx,gy,28,60, 0xFF1A1F26u);        /* receiver     */
    fill_rect(px,stride,tw,th, gx+3,gy+2,4,56, 0xFF2A323Cu);     /* highlight     */
    fill_rect(px,stride,tw,th, gx+9,gy-26,9,30, 0xFF12161Cu);    /* barrel        */
    fill_rect(px,stride,tw,th, gx+20,gy+12,11,18, 0xFF2A333Du);  /* magazine      */
    if (g_muzzle>0) {                                            /* muzzle flash  */
        int mfx=gx+9, mfy=gy-30;
        fill_rect(px,stride,tw,th, mfx-5,mfy-5,19,14, 0xFFFFE060u);
        fill_rect(px,stride,tw,th, mfx,mfy-11,9,24, 0xFFFFF4B0u);
    }

    font_draw_string(px,stride,tw,th, 12, th-FONT_H-8,
                     "WASD move  MOUSE/A-D look(arrows)  LMB/SPACE fire  R reload  ESC quit",
                     0xFFAAB2BFu);

    if (g_over && !g_mp_on) {   /* MP-OVER-FIX: the SP conclusion screen never bleeds into co-op */
        const char *msg; u32 mc;
        if      (g_raid_state == RAID_EXTRACTED) { msg="EXTRACTED -  Press R"; mc=0xFF3CCB5Au; } /* green */
        else if (g_raid_state == RAID_TIMEOUT)   { msg="TIME UP -  Press R";   mc=0xFFE9C46Au; } /* amber */
        else                                     { msg="YOU DIED -  Press R";  mc=0xFFE63946u; } /* red   */
        int mw=(int)k_strlen(msg)*FONT_W*2, mx=(tw-mw)/2, my=th/2-16;
        fill_rect(px,stride,tw,th, mx-16,my-12,mw+32,FONT_H+24, 0xCC000000u);
        for (int s=0;msg[s];s++){ int xx=mx+s*FONT_W*2;
            font_draw_char(px,stride,tw,th, xx,my,msg[s],mc);
            font_draw_char(px,stride,tw,th, xx+1,my,msg[s],mc); }
    }
}

/* ====================================================================== *
 *  Tarkov-lite UI: inventory + character/attribute menus
 * ====================================================================== */
static void use_item(int slot)
{
    if (slot < 0 || slot >= INV_SLOTS) return;
    Slot *s = &g_inv[slot];
    if (s->type == ITEM_AMMO && s->count > 0) {
        g_ammo += s->count; s->type = ITEM_NONE; s->count = 0;
    } else if (s->type == ITEM_MEDKIT && s->count > 0) {
        g_hp += 30; if (g_hp > g_hp_max) g_hp = g_hp_max;
        s->count--; if (s->count == 0) s->type = ITEM_NONE;
    }
}
static void spend_point(int idx)
{
    if (g_attr_pts <= 0 || idx < 0 || idx > 3) return;
    g_attr[idx]++; g_attr_pts--;
    recompute_derived();
    if (idx == 2) { g_hp += 20; if (g_hp > g_hp_max) g_hp = g_hp_max; }  /* END heal */
}
static void handle_ui_key(int a)
{
    if (a == KEY_ESC) { g_ui_mode = 0; return; }
    if (g_ui_mode == 1) {            /* inventory grid */
        if      (a == KEY_LEFT  && (g_inv_sel % INV_COLS))               g_inv_sel--;
        else if (a == KEY_RIGHT && (g_inv_sel % INV_COLS) != INV_COLS-1) g_inv_sel++;
        else if (a == KEY_UP    && g_inv_sel >= INV_COLS)                g_inv_sel -= INV_COLS;
        else if (a == KEY_DOWN  && g_inv_sel + INV_COLS < INV_SLOTS)     g_inv_sel += INV_COLS;
        else if (a == KEY_ENTER)                                         use_item(g_inv_sel);
    } else if (g_ui_mode == 2) {     /* character */
        if      (a == KEY_UP   && g_char_sel > 0) g_char_sel--;
        else if (a == KEY_DOWN && g_char_sel < 3) g_char_sel++;
        else if (a == KEY_ENTER)                  spend_point(g_char_sel);
    } else if (g_ui_mode == 3) {     /* MP-GUI-1: co-op connect menu */
        if (a == KEY_ENTER) { if (g_mp_on) mp_game_disconnect(); else mp_game_connect(); }
    }
}
static const char *item_label(u8 t)
{
    if (t == ITEM_AMMO)   return "AM";
    if (t == ITEM_MEDKIT) return "MD";
    return "..";
}
static void draw_ui_panel(wl_window *win)
{
    if (g_ui_mode == 0) return;
    u32 *px = win->pixels; int stride = (int)(win->stride/4u);
    int tw = (int)win->w, th = (int)win->h;
    fill_rect(px,stride,tw,th, 0,0,tw,th, 0x99000000u);     /* dim the scene */
    int pw = 372, ph = 250, pxx = (tw-pw)/2, pyy = (th-ph)/2;
    fill_rect(px,stride,tw,th, pxx-2,pyy-2,pw+4,ph+4, 0xFF000000u);
    fill_rect(px,stride,tw,th, pxx,pyy,pw,ph, 0xFF1A2230u);

    if (g_ui_mode == 1) {
        font_draw_string(px,stride,tw,th, pxx+12,pyy+8,
                         "INVENTORY  arrows=move ENTER=use TAB=close", 0xFFFFFFFFu);
        int gx0 = pxx+16, gy0 = pyy+34, cell = 56;
        for (int r=0;r<INV_ROWS;r++) for (int c=0;c<INV_COLS;c++) {
            int idx = r*INV_COLS+c, cxp = gx0+c*cell, cyp = gy0+r*cell;
            u32 bg = (idx==g_inv_sel) ? 0xFF3A5A8Au : 0xFF2A3340u;
            fill_rect(px,stride,tw,th, cxp,cyp,cell-6,cell-6, bg);
            if (g_inv[idx].type != ITEM_NONE) {
                char nb[12]; num_to_str(nb, g_inv[idx].count);
                font_draw_string(px,stride,tw,th, cxp+4,cyp+4,  item_label(g_inv[idx].type), 0xFFFFE0A0u);
                font_draw_string(px,stride,tw,th, cxp+4,cyp+22, nb, 0xFFCFE8FFu);
            }
        }
    } else if (g_ui_mode == 3) {
        font_draw_string(px,stride,tw,th, pxx+12,pyy+8,
                         "CO-OP  (loopback 127.0.0.1:27015)", 0xFFFFFFFFu);
        font_draw_string(px,stride,tw,th, pxx+12,pyy+44,
                         g_mp_on ? "Status: CONNECTED" : "Status: offline",
                         g_mp_on ? 0xFF3CCB5Au : 0xFFAAB2BFu);
        if (g_mp_on) {
            char nb[12]; num_to_str(nb, g_my_slot);
            font_draw_string(px,stride,tw,th, pxx+12,pyy+72,  "Your slot:", 0xFFAAB2BFu);
            font_draw_string(px,stride,tw,th, pxx+108,pyy+72, nb, 0xFFCFE8FFu);
            font_draw_string(px,stride,tw,th, pxx+12,pyy+120,
                             "ENTER = disconnect      M / ESC = close", 0xFFFFE060u);
        } else {
            font_draw_string(px,stride,tw,th, pxx+12,pyy+72,
                             "Spawn 'deadzoned' first, then connect.", 0xFFAAB2BFu);
            font_draw_string(px,stride,tw,th, pxx+12,pyy+120,
                             "ENTER = connect         M / ESC = close", 0xFFFFE060u);
        }
    } else {
        char nb[12];
        font_draw_string(px,stride,tw,th, pxx+12,pyy+8,
                         "CHARACTER  up/down ENTER=spend C=close", 0xFFFFFFFFu);
        font_draw_string(px,stride,tw,th, pxx+12,pyy+30, "Points:", 0xFFAAB2BFu);
        num_to_str(nb, g_attr_pts);
        font_draw_string(px,stride,tw,th, pxx+84,pyy+30, nb, 0xFF3CCB5Au);
        for (int i=0;i<4;i++) {
            int ry = pyy+58 + i*22;
            u32 col = (i==g_char_sel) ? 0xFFFFE060u : 0xFFCFE8FFu;
            font_draw_string(px,stride,tw,th, pxx+16,ry, ATTR_NAME[i], col);
            num_to_str(nb, g_attr[i]);
            font_draw_string(px,stride,tw,th, pxx+74,ry, nb, col);
        }
        int dy = pyy+58+4*22+12;
        font_draw_string(px,stride,tw,th, pxx+16,dy,    "HPmax", 0xFFAAB2BFu);
        num_to_str(nb, g_hp_max);   font_draw_string(px,stride,tw,th, pxx+84,dy,    nb, 0xFFFFFFFFu);
        font_draw_string(px,stride,tw,th, pxx+16,dy+18, "Mag",   0xFFAAB2BFu);
        num_to_str(nb, g_mag_size); font_draw_string(px,stride,tw,th, pxx+84,dy+18, nb, 0xFFFFFFFFu);
    }
}

/* Headless proof: attributes deterministically drive the derived gameplay stats. */
/* ====================================================================== *
 *  In-game audio: a tiny NON-BLOCKING SFX mixer over AUDIO B1's
 *  SYS_AUDIO_STREAM_WRITE (48 kHz / 16-bit / stereo). Procedural gunshot +
 *  bite voices; each frame we fill the kernel ring as far as it will take
 *  (EAGAIN-throttled) so playback stays gapless and the game loop never
 *  blocks. Gracefully no-ops on a kernel with no HDA device (the syscall
 *  returns a negative rc -> we disable). No art/audio assets: all synthesized.
 * ====================================================================== */
#define AUD_RATE      48000u
#define AUD_CHUNK     480u           /* frames per write (10 ms; 1920 B stereo) */
#define AUD_VOICES    6
#define AUD_PER_FRAME 24             /* cap writes/frame so a frame can't stall  */

typedef struct {
    int  active;
    int  kind;      /* 0 = gunshot, 1 = bite */
    u32  pos;       /* frame within the sound */
    u32  dur;       /* total frames */
    u32  phase;     /* tone phase (16-bit brad) */
    u32  inc;       /* tone increment per frame */
    u32  seed;      /* noise LCG state */
} aud_voice;

static aud_voice g_av[AUD_VOICES];
static i16 g_aud_buf[AUD_CHUNK * 2];   /* interleaved L,R scratch */
static int g_audio_off = 0;            /* set once the syscall reports no device */
static u32 g_aud_seed  = 0x1234567u;
static int g_aud_streamed = 0;         /* selftest counter: writes accepted */

/* integer sine, brad 0..65535 (Bhaskara I; no libm) */
static i32 aud_sin(u32 brad){
    brad &= 0xFFFF; int neg=0;
    if (brad >= 32768){ brad -= 32768; neg=1; }
    u32 deg = (brad * 180u) / 32768u;
    u32 t = deg * (180u - deg);
    i32 num = (i32)(4u*t) * 32767;
    i32 den = (i32)(40500u - t);
    i32 v = den ? (num/den) : 0;
    if (v > 32767) v = 32767;
    return neg ? -v : v;
}
static u32 aud_noise(u32* s){ *s = (*s)*1103515245u + 12345u; return *s; }

/* start a voice in a free slot (steal slot 0 if all busy). */
static void audio_trigger(int kind){
    if (g_audio_off) return;
    int slot = -1;
    for (int i=0;i<AUD_VOICES;i++) if (!g_av[i].active){ slot=i; break; }
    if (slot < 0) slot = 0;
    aud_voice* v = &g_av[slot];
    v->active = 1; v->kind = kind; v->pos = 0; v->phase = 0;
    v->seed = (g_aud_seed += 0x9E3779B9u);
    if (kind == 0){ v->dur = (AUD_RATE*90u)/1000u; v->inc = (70u *65536u)/AUD_RATE; }  /* gunshot ~90ms / 70Hz boom */
    else          { v->dur = (AUD_RATE*55u)/1000u; v->inc = (140u*65536u)/AUD_RATE; }  /* bite ~55ms / 140Hz thud   */
}

/* one mixed mono sample (advances the voice). */
static i32 aud_voice_sample(aud_voice* v){
    if (!v->active) return 0;
    u32 rem = v->dur - v->pos;
    i32 env = (i32)((rem * 4096u) / v->dur);   /* 4096 = unity */
    env = (env * env) >> 12;                    /* square -> percussive decay */
    i32 tone  = aud_sin(v->phase);
    i32 noise = (i32)((aud_noise(&v->seed) >> 16) & 0x7FFFu) - 16384;
    i32 s = (v->kind == 0) ? (tone/3 + noise)        /* gunshot: noise + low boom */
                           : (tone*3/4 + noise/3);   /* bite: tonal thud + crunch */
    s = (s * env) >> 12;
    v->phase += v->inc;
    if (++v->pos >= v->dur) v->active = 0;
    return s;
}

/* mix AUD_CHUNK frames of all active voices into g_aud_buf (stereo, clamped). */
static void audio_mix(void){
    for (u32 f=0; f<AUD_CHUNK; f++){
        i32 m = 0;
        for (int i=0;i<AUD_VOICES;i++) if (g_av[i].active) m += aud_voice_sample(&g_av[i]);
        if (m >  32767) m =  32767;
        if (m < -32768) m = -32768;
        g_aud_buf[f*2+0] = (i16)m;
        g_aud_buf[f*2+1] = (i16)m;
    }
}

/* per game frame: fill the kernel ring as far as it will take, non-blocking.
 * On EAGAIN we REWIND the voices so the chunk is regenerated next frame (no
 * waveform gap); a negative rc means no audio device -> disable for good. */
static void audio_step(void){
    if (g_audio_off) return;
    for (int w=0; w<AUD_PER_FRAME; w++){
        aud_voice saved[AUD_VOICES];
        for (int i=0;i<AUD_VOICES;i++) saved[i] = g_av[i];
        audio_mix();
        /* AUDIO-PARTIAL-FIX: SYS_AUDIO_STREAM_WRITE is byte-granular + (now) frame-
         * aligned, so a SHORT accept means the ring filled mid-chunk. Advance an
         * offset by what was accepted (never re-send the accepted prefix), and only
         * REWIND the voices when NOTHING was accepted (ring full at the chunk start)
         * so the common case has no waveform gap. */
        long want = (long)sizeof(g_aud_buf), off = 0;
        for (;;) {
            long n = sc(SYS_AUDIO_STREAM_WRITE, (long)((char*)g_aud_buf + off),
                        want - off, 0,0,0,0);
            if (n < 0 && n != AUD_EAGAIN){ g_audio_off = 1; return; }   /* no device */
            if (n > 0) off += n;                 /* kernel returns a frame-aligned count */
            if (off >= want) break;              /* whole chunk emitted */
            if (off == 0)                        /* ring full at start -> rewind, retry next frame */
                for (int i=0;i<AUD_VOICES;i++) g_av[i] = saved[i];
            return;                              /* ring is full; stop feeding this frame */
        }
        g_aud_streamed++;
    }
}

/* Headless audio proof: trigger a gunshot, pump the mixer a few frames. With
 * HDA the stream accepts bytes (PASS); without a device the first write returns
 * a negative rc and we disable (honest SKIP). Either way the game keeps running. */
static void audio_selftest(void){
    char nb[12];
    for (int i=0;i<AUD_VOICES;i++) g_av[i].active = 0;
    g_audio_off = 0; g_aud_streamed = 0;
    audio_trigger(0);                       /* a gunshot */
    for (int f=0; f<8 && !g_audio_off; f++) audio_step();
    if (g_audio_off) {
        print("DEADZONE: audio SKIP (no HDA device)\n");
    } else {
        print("DEADZONE: audio PASS streamed="); num_to_str(nb, g_aud_streamed); print(nb); print("\n");
    }
    for (int i=0;i<AUD_VOICES;i++) g_av[i].active = 0;   /* clean for gameplay */
}

/* Headless raid-lifecycle proof: drive a REAL pickups_step collect -> extract
 * (stash grows) and a collect -> die (stash unchanged) through the actual game
 * code. Saves/restores the PERSISTENT stash + rng and ends with a clean reset,
 * so it must run LAST and leaves live play untouched. */
static void raid_selftest(void)
{
    char nb[12];
    u32 save_stash[N_ITEM_TYPES]; for (int i=0;i<N_ITEM_TYPES;i++) save_stash[i]=g_stash[i];
    u32 save_rng = g_rng;
    u64 now = (u64)sc(SYS_GET_TICKS_MS,0,0,0,0,0,0);

    /* Scenario A: collect 7 SCRAP via pickups_step, then EXTRACT -> stash kept. */
    deadzone_reset(now);
    for (int i=0;i<N_ITEM_TYPES;i++) g_stash[i]=0;          /* clean stash baseline */
    g_pickup[0].x=g_px; g_pickup[0].z=g_pz; g_pickup[0].type=ITEM_SCRAP;
    g_pickup[0].count=7; g_pickup[0].alive=1; g_pickup[0].bob=0;
    pickups_step();                                          /* -> loot_apply -> bag[SCRAP]=7 */
    int bagged = (g_raid_bag[ITEM_SCRAP]==7) ? 1 : 0;
    g_px=fx_from_int(EXTRACT_X); g_pz=fx_from_int(EXTRACT_Z);
    for (int s=0; s<EXTRACT_CHANNEL_STEPS+2 && g_raid_state!=RAID_EXTRACTED; s++) extraction_step();
    int extracted = (g_raid_state==RAID_EXTRACTED && g_over==1) ? 1 : 0;
    int kept = (g_stash[ITEM_SCRAP]==7) ? 1 : 0;

    /* Scenario B: collect 5 SCRAP, then DIE before extracting -> stash unchanged. */
    deadzone_reset(now ^ 0x55u);                            /* fresh raid; stash persists */
    u32 stash_before = g_stash[ITEM_SCRAP];                 /* == 7 from scenario A */
    g_pickup[0].x=g_px; g_pickup[0].z=g_pz; g_pickup[0].type=ITEM_SCRAP;
    g_pickup[0].count=5; g_pickup[0].alive=1; g_pickup[0].bob=0;
    pickups_step();
    g_hp=0; g_alive=0;
    /* RAID-FIX proof: enter the extraction channel FIRST, so this proves a death
     * MID-EXTRACTION is tagged DIED (not left stuck at RAID_EXTRACTING -> the stale
     * "EXTRACTING..." banner bug). raid_tag_death() is the REAL loss-latch logic. */
    g_raid_state = RAID_EXTRACTING;
    raid_tag_death();
    int died = (g_raid_state==RAID_DIED) ? 1 : 0;
    int lost = (g_stash[ITEM_SCRAP]==stash_before) ? 1 : 0; /* bag (5) NOT secured */

    /* raid_tag_death must PRESERVE an already-terminal TIMEOUT/EXTRACTED tag (only a
     * non-terminal state becomes DIED) -- else a win/timeout would relabel a death. */
    g_raid_state = RAID_TIMEOUT;   raid_tag_death(); int keepT = (g_raid_state==RAID_TIMEOUT)   ? 1 : 0;
    g_raid_state = RAID_EXTRACTED; raid_tag_death(); int keepX = (g_raid_state==RAID_EXTRACTED) ? 1 : 0;

    int ok = bagged && extracted && kept && died && lost && keepT && keepX;
    print("DEADZONE: raid "); print(ok?"PASS":"FAIL");
    print(" bagged=");    num_to_str(nb,bagged);    print(nb);
    print(" extracted="); num_to_str(nb,extracted); print(nb);
    print(" kept=");      num_to_str(nb,kept);      print(nb);
    print(" died=");      num_to_str(nb,died);      print(nb);
    print(" lost=");      num_to_str(nb,lost);      print(nb);
    print(" keepT=");     num_to_str(nb,keepT);     print(nb);
    print(" keepX=");     num_to_str(nb,keepX);     print(nb);
    print("\n");

    for (int i=0;i<N_ITEM_TYPES;i++) g_stash[i]=save_stash[i];   /* restore persistent stash */
    g_rng = save_rng;
    deadzone_reset((u64)sc(SYS_GET_TICKS_MS,0,0,0,0,0,0));        /* clean raid for live play */
}

static void systems_selftest(void)
{
    char nb[12];
    for (int i=0;i<4;i++) g_attr[i]=0; recompute_derived();
    int hp0=g_hp_max, mag0=g_mag_size;
    g_attr[2]=1; g_attr[0]=1; recompute_derived();      /* +1 END, +1 STR */
    int hp1=g_hp_max, mag1=g_mag_size;
    int ok = (hp0==100 && mag0==MAG_SIZE && hp1==120 && mag1==MAG_SIZE+3);
    for (int i=0;i<4;i++) g_attr[i]=0; recompute_derived();   /* reset to clean */
    print("DEADZONE: systems "); print(ok ? "PASS" : "FAIL");
    print(" inv=");  num_to_str(nb, INV_SLOTS); print(nb);
    print(" hp0="); num_to_str(nb, hp0); print(nb);
    print(" hp1="); num_to_str(nb, hp1); print(nb);
    print(" mag0="); num_to_str(nb, mag0); print(nb);
    print(" mag1="); num_to_str(nb, mag1); print(nb);
    print("\n");
}

/* Headless proof of loot/equipment/durability: deterministic (fixed RNG seed),
 * no window/peer needed. Saves+restores g_rng so gameplay randomness is intact. */
static void loot_selftest(void)
{
    char nb[12];
    u32 saved = g_rng;
    loot_wsum_init();
    g_rng = 0x1234567u | 1u;                          /* fixed seed             */
    for (int i=0;i<MAX_PICKUPS;i++) g_pickup[i].alive = 0;

    int drops = 0;
    for (int k=0;k<8;k++) roll_loot(0,0);             /* 8 kills' worth of loot */
    for (int i=0;i<MAX_PICKUPS;i++) if (g_pickup[i].alive) drops++;

    g_weap_dur = WEAP_DUR_MAX;                         /* 50 shots wear exactly 50 */
    for (int s=0;s<50;s++) if (g_weap_dur>0) g_weap_dur -= WEAP_DUR_PERSHOT;

    g_armor = 10; int soak = Z_BITE_DMG - g_armor/4;  /* 8 - 10/4 = 6           */
    if (soak < 1) soak = 1;

    int ok = (drops == 8)
          && (g_weap_dur == WEAP_DUR_MAX - 50)
          && (soak == 6)
          && (g_loot_wsum == 100);
    print("DEADZONE: loot "); print(ok ? "PASS" : "FAIL");
    print(" drops="); num_to_str(nb, drops);             print(nb);
    print(" dur=");   num_to_str(nb, g_weap_dur);        print(nb);
    print(" soak=");  num_to_str(nb, soak);              print(nb);
    print(" wsum=");  num_to_str(nb, (int)g_loot_wsum);  print(nb);
    print("\n");

    /* restore clean gameplay state */
    for (int i=0;i<MAX_PICKUPS;i++) g_pickup[i].alive = 0;
    g_armor = 0; g_weap_dur = WEAP_DUR_MAX; g_rng = saved;
}

/* ====================================================================== *
 *  Entry
 * ====================================================================== */
/* ===================================================================== */
/* Multiplayer client -- connect to an authoritative deadzoned server and  */
/* exchange the shared dzproto.h packets over TCP. The single-player game   */
/* runs unchanged; these are the seam a co-op session uses. Live two-       */
/* instance play is opt-in (no NIC/peer needed for single-player).          */
/* ===================================================================== */
#define SYS_SOCKET     51
#define SYS_CONNECT    52
#define SYS_SEND       53
#define SYS_RECV       54
#define SYS_CLOSE_SK   55
#define SYS_SOCK_POLL  58
#define MP_SOCK_STREAM  1
#define MP_EAGAIN     (-11)
#define DZ_GAME_PORT   27015

typedef struct { long fd; dz_u32 seq; dz_u8 rx[2048]; dz_u32 rxn; dz_u32 slot; } dz_client;

/* Connect to a server. ip_be = packed big-endian a.b.c.d, port = bare number.
 * Returns 0 on success, negative on a hard error. (NOTE: deadzone.c sc() is the
 * 7-arg form -- do NOT paste the 6-arg client/server call shape here.) */
static int mp_connect(dz_client *c, dz_u32 ip_be, long port)
{
    c->fd = sc(SYS_SOCKET, MP_SOCK_STREAM, 0, 0, 0, 0, 0);
    if (c->fd < 0) return -1;
    long r = sc(SYS_CONNECT, c->fd, (long)ip_be, port, 0, 0, 0);
    if (r < 0) { sc(SYS_CLOSE_SK, c->fd, 0, 0, 0, 0, 0); return -2; }  /* synchronous connect: any neg = hard fail */
    c->seq = 0; c->rxn = 0; c->slot = 0;
    /* MP-HELLO-0: read the server's 16B DZ_HELLO join-ack FIRST (bounded recv ->
     * trailing snapshot bytes stay in the kernel for mp_poll_snapshot). */
    {
        dz_u8 hb[DZ_HELLO_BYTES]; long off = 0; int guard = 0;
        while (off < DZ_HELLO_BYTES) {
            sc(SYS_SOCK_POLL, 0, 0, 0, 0, 0, 0);
            long n = sc(SYS_RECV, c->fd, (long)(hb + off), DZ_HELLO_BYTES - off, 0, 0, 0);
            if (n > 0) { off += n; continue; }
            if (n == MP_EAGAIN) { sc(SYS_YIELD,0,0,0,0,0,0); if (++guard > 200000) { sc(SYS_CLOSE_SK,c->fd,0,0,0,0,0); return -3; } continue; }
            sc(SYS_CLOSE_SK, c->fd, 0, 0, 0, 0, 0); return -4;   /* closed before hello */
        }
        dz_hello_t hel;
        if (!dz_hello_decode(hb, &hel) || hel.proto_ver != DZ_PROTO_VER) { sc(SYS_CLOSE_SK,c->fd,0,0,0,0,0); return -5; }
        c->slot = hel.slot;
    }
    return 0;
}

/* Send this frame's input to the server (bounded). Returns 0 / negative. */
static int mp_send_input(dz_client *c, dz_i32 mx, dz_i32 my, dz_u32 yaw, dz_u32 buttons)
{
    dz_input_t in; in.seq = ++c->seq; in.move_x = mx; in.move_y = my;
    in.yaw = yaw; in.buttons = buttons;
    dz_u8 b[DZ_INPUT_BYTES]; dz_input_encode(b, &in);
    long off = 0; int guard = 0;
    while (off < DZ_INPUT_BYTES) {
        long n = sc(SYS_SEND, c->fd, (long)(b + off), DZ_INPUT_BYTES - off, 0, 0, 0);
        if (n > 0) { off += n; continue; }
        if (n == MP_EAGAIN) { sc(SYS_YIELD,0,0,0,0,0,0); if(++guard>100000) return -1; continue; }
        return -1;
    }
    return 0;
}

/* Drain pending bytes, decode complete snapshots, keep the latest in `out`.
 * Returns 1 if at least one fresh snapshot was decoded this call. */
static int mp_poll_snapshot(dz_client *c, dz_snapshot_t *out)
{
    int got = 0;
    sc(SYS_SOCK_POLL, 0, 0, 0, 0, 0, 0);
    for (;;) {
        if (c->rxn >= sizeof(c->rx)) c->rxn = 0;            /* defensive resync */
        long n = sc(SYS_RECV, c->fd, (long)(c->rx + c->rxn),
                    (long)(sizeof(c->rx) - c->rxn), 0, 0, 0);
        if (n <= 0) break;                                  /* EAGAIN / closed  */
        c->rxn += (dz_u32)n;
        for (;;) {
            if (c->rxn < 4u * DZ_SNAP_HDR_U32) break;
            /* Validate magic before trusting the header counts (rx+12/rx+16); on a
             * bad/garbage frame resync (rxn=0) rather than consuming a possibly-
             * wrapped length, which would desync the stream forever. */
            if (dz_get_u32(c->rx) != DZ_SNAP_MAGIC) { c->rxn = 0; break; }
            dz_u32 need = dz_snap_bytes(dz_get_u32(c->rx + 12), dz_get_u32(c->rx + 16));
            if (need == 0 || need > sizeof(c->rx)) { c->rxn = 0; break; }
            if (c->rxn < need) break;
            if (!dz_snap_decode(c->rx, need, out)) { c->rxn = 0; break; }
            got = 1;
            dz_u32 rem = c->rxn - need;
            for (dz_u32 k = 0; k < rem; k++) c->rx[k] = c->rx[need + k];
            c->rxn = rem;
        }
    }
    return got;
}

/* ====================================================================== *
 *  MP-GUI-1: live co-op wiring. All of it is gated on g_mp_on (default 0),
 *  so with no connection the single-player game is byte-for-byte identical.
 *  Connection object + the wire<->fx coordinate bridge + per-frame
 *  send-input / apply-snapshot + connect/disconnect.
 * ====================================================================== */
static dz_client     g_mpc;          /* the live connection                   */
static dz_snapshot_t g_snap;         /* latest authoritative snapshot         */
static u64           g_mp_last_send; /* ms of last input send (send cadence)  */
static int           g_mp_send_steps;/* sim-steps elapsed since last send (MP-SPEED-FIX) */

/* The server world is [0..DZ_SRV_ARENA] (centre DZ_SRV_ARENA/2) on both axes;
 * the game uses fx centred at 0 spanning +-ARENA_HALF_I. i64 intermediates =>
 * no overflow even at the arena edge (|w-centre|*fx(28) ~ 9.2e9 > i32). */
#define DZ_SRV_ARENA  10000
static fx  wire_to_fx(dz_i32 w) {
    /* COORD-FIX: clamp the untrusted server coord to the arena BEFORE converting --
     * a corrupt/malicious snapshot can't teleport us out of bounds, and the centring
     * is done in 64-bit so a pathological w (near INT_MIN) can't signed-overflow.
     * Legit server coords are always in [0,DZ_SRV_ARENA], so this is a no-op for
     * normal play. */
    if (w < 0) w = 0; else if (w > DZ_SRV_ARENA) w = DZ_SRV_ARENA;
    g3d_i64 num = ((g3d_i64)w - DZ_SRV_ARENA/2) * (g3d_i64)fx_from_int(2*ARENA_HALF_I);
    return (fx)(num / DZ_SRV_ARENA);
}
static dz_i32 fx_to_wire(fx d) {     /* a per-frame DELTA (no centring)        */
    g3d_i64 num = (g3d_i64)d * (g3d_i64)DZ_SRV_ARENA;
    return (dz_i32)(num / (g3d_i64)fx_from_int(2*ARENA_HALF_I));
}

/* Apply the freshest authoritative snapshot to local render/HUD state. In MP
 * the server owns everything: our position/hp/score/wave, the zombie set, AND
 * the other players. Everything is capped to the local array sizes. */
static void mp_apply_snapshot(void)
{
    for (int i = 0; i < DZP_MAX_CLIENTS; i++) g_ron[i] = 0;
    int np = (int)g_snap.n_players; if (np > DZP_MAX_CLIENTS) np = DZP_MAX_CLIENTS;
    for (int i = 0; i < np; i++) {
        dz_u32 id = g_snap.p[i].id;
        if (id == DZ_SLOT_EMPTY) continue;                  /* phantom slot */
        fx wx = wire_to_fx(g_snap.p[i].x), wz = wire_to_fx(g_snap.p[i].y);
        if ((int)id == g_my_slot) {                         /* that's us */
            g_px = wx; g_pz = wz;
            g_hp = g_snap.p[i].hp; g_alive = (g_hp > 0);
            g_kills = (i32)g_snap.p[i].score;
        } else if (id < (dz_u32)DZP_MAX_CLIENTS) {          /* a teammate */
            g_rx[id] = wx; g_rz[id] = wz; g_ron[id] = 1;
        }
    }
    g_wave = (i32)g_snap.wave;
    int nz = (int)g_snap.n_zombies; if (nz > MAX_Z) nz = MAX_Z;   /* 64 -> cap 24 */
    for (int i = 0; i < MAX_Z; i++) g_z[i].alive = 0;
    for (int i = 0; i < nz; i++) {
        g_z[i].x = wire_to_fx(g_snap.z[i].x);
        g_z[i].z = wire_to_fx(g_snap.z[i].y);
        g_z[i].alive = 1; g_z[i].health = 1; g_z[i].hitflash = 0;
    }
}

/* Compute the wire-space move intent for `nsteps` sim-steps from the held inputs
 * (no send). Mirrors player_step's forward/right basis. MP-SPEED-FIX: scale the
 * per-step displacement by the sim-steps elapsed since the last send, so movement
 * is frame-rate-INDEPENDENT and approximates SP speed (the server clamps each axis
 * to PLAYER_SPEED; at very high AGI / a multi-step stall a diagonal step may clip
 * slightly, bounded). Factored out of mp_pump_input so the nsteps scaling is
 * headlessly testable (mp_selftest). */
static void mp_move_wire(int nsteps, dz_i32 *out_wx, dz_i32 *out_wz)
{
    if (nsteps < 1) nsteps = 1;
    fx fdx = dirx(g_yaw), fdz = dirz(g_yaw);          /* forward */
    fx rdx = dirz(g_yaw), rdz = -dirx(g_yaw);         /* right   */
    fx mf = 0, ms = 0;
    if (g_hold_fwd)  mf = fx_add(mf,  g_move_spd);
    if (g_hold_back) mf = fx_sub(mf,  g_move_spd);
    if (g_hold_right)ms = fx_add(ms,  g_move_spd);
    if (g_hold_left) ms = fx_sub(ms,  g_move_spd);
    fx wdx = fx_add(fx_mul(fdx, mf), fx_mul(rdx, ms));
    fx wdz = fx_add(fx_mul(fdz, mf), fx_mul(rdz, ms));
    *out_wx = fx_to_wire(wdx) * nsteps;
    *out_wz = fx_to_wire(wdz) * nsteps;
}

/* Translate the held inputs into a server-wire move + fire bit and send it.
 * Returns 0 / negative. */
static int mp_pump_input(int nsteps)
{
    dz_i32 wx, wz; mp_move_wire(nsteps, &wx, &wz);
    dz_u32 btn = g_shoot_held ? DZ_BTN_FIRE : 0u;
    return mp_send_input(&g_mpc, wx, wz, (dz_u32)g_yaw, btn);
}

/* Connect to the local server (loopback 127.0.0.1, host-ordered like dzclient).
 * Learns our slot from the DZ_HELLO join-ack mp_connect already parsed. */
static void mp_game_connect(void)
{
    if (g_mp_on) return;
    g_ui_mode = 0;                                   /* drop the menu, show play */
    if (mp_connect(&g_mpc, 0x7F000001u, DZ_GAME_PORT) != 0) { g_mp_on = 0; return; }
    g_my_slot = (int)g_mpc.slot;
    g_mp_last_send = 0; g_mp_send_steps = 0; g_mp_on = 1;
    /* MP-OVER-FIX: clear any single-player conclusion latch so a prior SP death/
     * timeout/extraction can't bleed its game-over overlay over the live session. */
    g_over = 0; g_alive = 1; g_raid_state = RAID_ACTIVE;
    /* MP-JOIN-FIX: also blank the local render world so the frame(s) before the
     * first server snapshot don't show the stale single-player world (old position/
     * zombies/kills, or an empty bar still labelled "HP" after joining from an SP
     * death where g_hp==0). mp_apply_snapshot overwrites these once a snap arrives. */
    g_hp = g_hp_max; g_kills = 0; g_wave = 0; g_px = 0; g_pz = 0;
    for (int i = 0; i < MAX_Z; i++) g_z[i].alive = 0;
    for (int i = 0; i < MAX_PICKUPS; i++) g_pickup[i].alive = 0;   /* clear stale SP ground loot (drawn ungated) */
    for (int i = 0; i < N_ITEM_TYPES; i++) g_raid_bag[i] = 0;      /* + the stale unsecured bag */
    for (int i = 0; i < DZP_MAX_CLIENTS; i++) g_ron[i] = 0;
    print("DEADZONE: mp connected\n");
}
/* Graceful disconnect: drop the socket and fall back to a fresh single-player raid. */
static void mp_game_disconnect(void)
{
    if (!g_mp_on) return;
    sc(SYS_CLOSE_SK, g_mpc.fd, 0, 0, 0, 0, 0);
    g_mp_on = 0;
    for (int i = 0; i < DZP_MAX_CLIENTS; i++) g_ron[i] = 0;
    deadzone_reset((u64)sc(SYS_GET_TICKS_MS,0,0,0,0,0,0));
    print("DEADZONE: mp disconnected\n");
}

/* Headless proof that THIS binary speaks the shared protocol the server uses:
 * encode/decode both packet families through dzproto.h and assert round-trip.
 * Runs at launch like systems_selftest(); no network/peer required. */
static void mp_selftest(void)
{
    (void)mp_connect; (void)mp_send_input; (void)mp_poll_snapshot;   /* link-keep */

    dz_input_t in, in2;
    in.seq = 42; in.move_x = -9; in.move_y = 12; in.yaw = 300; in.buttons = DZ_BTN_FIRE;
    dz_u8 ib[DZ_INPUT_BYTES]; dz_input_encode(ib, &in);
    if (!dz_input_decode(ib, &in2) || in2.seq != in.seq || in2.move_x != in.move_x
        || in2.buttons != in.buttons) { print("DEADZONE: mp FAIL input\n"); return; }

    static dz_snapshot_t s, s2;
    s.tick = 5; s.wave = 2; s.n_players = 2; s.n_zombies = 5;
    for (dz_u32 i = 0; i < s.n_players; i++) {
        s.p[i].id = i; s.p[i].x = 10 + (dz_i32)i; s.p[i].y = 20; s.p[i].hp = 88;
        s.p[i].yaw = 1; s.p[i].score = i;
    }
    for (dz_u32 i = 0; i < s.n_zombies; i++) { s.z[i].x = (dz_i32)i*3; s.z[i].y = (dz_i32)i; s.z[i].state = 1; }
    static dz_u8 sb[DZ_SNAP_MAX_BYTES];
    dz_u32 n = dz_snap_encode(&s, sb, sizeof(sb));
    if (!n || !dz_snap_decode(sb, n, &s2) || s2.n_zombies != s.n_zombies
        || s2.p[1].x != s.p[1].x) { print("DEADZONE: mp FAIL snap\n"); return; }

    /* MP-HELLO-0: the DZ_HELLO join-ack round-trips on the game side too. */
    dz_hello_t h, h2; h.slot = 1; h.max_players = 8; h.proto_ver = DZ_PROTO_VER;
    dz_u8 hbb[DZ_HELLO_BYTES]; dz_hello_encode(hbb, &h);
    if (!dz_hello_decode(hbb, &h2) || h2.slot != h.slot || h2.proto_ver != h.proto_ver) {
        print("DEADZONE: mp FAIL hello\n"); return;
    }

    /* MP-GUI-1: the wire<->fx coordinate bridge is sane -- the arena centre maps
     * to the world origin, the edges straddle it, and the delta map keeps sign. */
    if (wire_to_fx(DZ_SRV_ARENA/2) != 0 || wire_to_fx(0) >= 0
        || wire_to_fx(DZ_SRV_ARENA) <= 0 || fx_to_wire(fx_from_int(1)) <= 0) {
        print("DEADZONE: mp FAIL coord\n"); return;
    }

    /* COORD-FIX: an out-of-arena (corrupt/hostile) server coord must CLAMP to the
     * arena edge, not signed-overflow into a garbage world position. */
    if (wire_to_fx(99999) != wire_to_fx(DZ_SRV_ARENA)
        || wire_to_fx(-99999) != wire_to_fx(0)) {
        print("DEADZONE: mp FAIL coord_clamp\n"); return;
    }

    /* MP-SPEED-FIX: the wire move intent scales LINEARLY with sim-steps (frame-rate
     * independence). Use a ~45-degree yaw so BOTH axes are non-zero (a per-axis
     * scaling regression can't hide on a zero axis) and check the nsteps<1 clamp.
     * Exercises the real mp_move_wire path (no harness drives the live GUI send). */
    {
        int s_fwd = g_hold_fwd; i32 s_yaw = g_yaw;
        g_hold_fwd = 1; g_yaw = G3D_ANG_STEPS / 8;        /* dirx,dirz both != 0 */
        dz_i32 wx0, wz0, wx1, wz1, wx2, wz2;
        mp_move_wire(0, &wx0, &wz0);                       /* nsteps<1 -> clamps to 1 */
        mp_move_wire(1, &wx1, &wz1);
        mp_move_wire(2, &wx2, &wz2);
        int ok = (wx1 != 0) && (wz1 != 0)                 /* both axes move */
              && (wx0 == wx1) && (wz0 == wz1)             /* nsteps<1 clamp */
              && (wx2 == 2 * wx1) && (wz2 == 2 * wz1);    /* linear in nsteps */
        g_hold_fwd = s_fwd; g_yaw = s_yaw;
        if (!ok) { print("DEADZONE: mp FAIL speed_scale\n"); return; }
    }

    print("DEADZONE: mp PASS (dzproto client wire ok)\n");
}

void _start(void)
{
    if (wl_connect() != 0) { print("DEADZONE: wl_connect FAILED\n"); for(;;) sc(SYS_YIELD,0,0,0,0,0,0); }
    wl_window *win = wl_create_window(WIN_W, WIN_H, "DeadZone");
    if (!win) { print("DEADZONE: wl_create_window FAILED\n"); for(;;) sc(SYS_YIELD,0,0,0,0,0,0); }

    build_world();
    box_mesh(g_zmesh, Z_HX, Z_HY, Z_HZ);
    gen_assets();                       /* generate pixel-art sprite assets */

    /* PSX low-res render target (static; upscaled to the window each frame). */
    g3d_target tgt;
    tgt.pixels=g_lowfb; tgt.zbuf=g_lowz; tgt.stride=LOWW; tgt.w=LOWW; tgt.h=LOWH;

    fx aspect = fx_ratio(WIN_W, WIN_H);
    mat4 proj = mat4_perspective(g3d_deg(70), aspect, fx_ratio(1,10), fx_from_int(120));
    vec3 light = v3(fx_ratio(-3,10), fx_ratio(8,10), fx_ratio(4,10));

    u64 now=(u64)sc(SYS_GET_TICKS_MS,0,0,0,0,0,0);
    deadzone_reset(now);
    u64 last=now, acc=0;

    systems_selftest();          /* headless proof: attributes -> derived stats */
    mp_selftest();               /* headless proof: client speaks dzproto wire  */
    loot_selftest();             /* headless proof: loot/equip/durability       */
    audio_selftest();            /* headless proof: in-game SFX stream (or SKIP) */
    raid_selftest();             /* headless proof: extraction/raid lifecycle    */
    print("DEADZONE: ready\n");
#ifdef DZ_MPGUI_DEMO
    mp_game_connect();   /* 2-window co-op demo: auto-join the local deadzoned */
#endif

    for (;;) {
        int kind,a,b,cev;
        while (wl_poll_event(win,&kind,&a,&b,&cev)) {
            if (kind==WL_EVENT_RESIZE) { continue; }  /* low-res tgt static; blit adapts to win */
            if (kind==WL_EVENT_POINTER) {
                if (g_ui_mode == 0) {
                    if (g_have_mouse) {
                        int dx = a - g_last_mx;
                        g_yaw = (g_yaw + dx * MOUSE_SENS) & G3D_ANG_MASK;
                    }
                    g_shoot_held = (cev & 1);
                    if (g_shoot_held) try_shoot();
                } else {
                    g_shoot_held = 0;
                }
                g_last_mx = a; g_have_mouse = 1;
                continue;
            }
            if (kind!=WL_EVENT_KEY) continue;
            i32 down=(b==1);
            /* panel toggles work in any mode */
            if (down && a==KEY_TAB) { g_ui_mode=(g_ui_mode==1)?0:1; continue; }
            if (down && a==KEY_C)   { g_ui_mode=(g_ui_mode==2)?0:2; continue; }
            if (down && a==KEY_M)   { g_ui_mode=(g_ui_mode==3)?0:3; continue; }  /* MP-GUI-1 */
            if (g_ui_mode != 0) { if (down) handle_ui_key(a); continue; }
            switch (a) {
            case KEY_W: case KEY_UP:    g_hold_fwd=down; break;
            case KEY_S: case KEY_DOWN:  g_hold_back=down; break;
            case KEY_A:                 g_hold_left=down; break;   /* strafe */
            case KEY_D:                 g_hold_right=down; break;
            case KEY_LEFT:              g_hold_tl=down; break;     /* turn */
            case KEY_RIGHT:             g_hold_tr=down; break;
            case KEY_SPACE: if (down) try_shoot(); break;
            case KEY_R: if (down && g_mag<g_mag_size && g_ammo>0 && g_reload==0) g_reload=g_reload_steps; break;
            case KEY_ESC: if (down) sc(SYS_EXIT,0,0,0,0,0,0); break;
            }
            if (a==KEY_R && down && g_over) deadzone_reset((u64)sc(SYS_GET_TICKS_MS,0,0,0,0,0,0));
        }

        now=(u64)sc(SYS_GET_TICKS_MS,0,0,0,0,0,0);
        acc += (now-last); last=now;
        if (acc > (u64)(STEP_MS*5)) acc=STEP_MS*5;
        int steps=0;
        while (acc>=STEP_MS && steps<5) {
            acc-=STEP_MS; steps++;
            if (!g_mp_on && !g_over && g_ui_mode==0) {   /* local sim; MP => server-authoritative, panel => frozen */
                player_step();
                if (g_shoot_held) try_shoot();
                zombies_step();
                pickups_step();          /* walk-over loot collection */
                extraction_step();       /* RAID: extraction-zone channel -> secure + win */
                wave_update();
                /* RAID timer: expiry = MIA (loss). Set the result BEFORE g_alive=0 so the
                 * latch below tags it TIMEOUT, not DIED. */
                if (g_alive && !g_over && g_raid_timer > 0 && --g_raid_timer == 0) {
                    g_raid_state = RAID_TIMEOUT; g_alive = 0;
                }
                if (!g_alive && !g_over) {
                    raid_tag_death();   /* RAID-FIX: DIED unless already TIMEOUT/EXTRACTED */
                    g_over = 1;
                    char b1[24], b2[24]; num_to_str(b1,g_wave); num_to_str(b2,g_kills);
                    print("[DEADZONE] over wave "); print(b1); print(" kills "); print(b2); print("\n");
                }
            }
        }

        /* MP-GUI-1: when joined, the server drives the world. Send our held
         * input at ~30 Hz, then apply the freshest authoritative snapshot.
         * A send failure (peer gone) drops us cleanly back to single-player. */
        if (g_mp_on) {
#ifdef DZ_MPGUI_DEMO
            /* Self-driving demo (no keyboard input): FACE the other player -- they
             * spawn on the x-axis, so slot 0 looks +x toward slot 1 and slot 1 looks
             * -x toward slot 0 -- and slowly strafe so the cyan teammate stays
             * dead-ahead AND visibly slides side to side. */
            g_yaw = (g_my_slot == 1) ? (3 * G3D_ANG_STEPS / 4) : (G3D_ANG_STEPS / 4);
            { static int dt = 0; int ph = ((dt++) / 45) & 1;
              g_hold_left = ph; g_hold_right = !ph; g_hold_fwd = 0; g_hold_back = 0; }
#endif
            g_mp_send_steps += steps;                 /* accumulate elapsed sim-steps */
            /* MP-FEEDBACK-FIX: the local sim (which decays these) is gated off in
             * MP, so advance the cosmetic shoot timers here -- else the muzzle flash
             * sticks ON and the fire cooldown freezes after the first co-op shot. */
            for (int s = 0; s < steps; s++) decay_timers();
            if (now - g_mp_last_send >= 33) {
                int ns = g_mp_send_steps > 0 ? g_mp_send_steps : 1;
                if (mp_pump_input(ns) != 0) mp_game_disconnect();
                else { g_mp_last_send = now; g_mp_send_steps = 0; }
            }
            if (g_mp_on && mp_poll_snapshot(&g_mpc, &g_snap)) mp_apply_snapshot();
        }

        render_scene(&tgt, proj, light);   /* into the low-res PSX target */
#ifdef DZ_MPGUI_DEMO
        /* Headless render proof: how many cyan teammate sprites this window actually
         * DREW this frame (passed projection). >=1 => the co-op render is visible. */
        { static int rprt = 0; if (g_mp_on && ++rprt >= 30) { rprt = 0; char nb[12];
            print("DEADZONE: coop slot="); num_to_str(nb, g_my_slot); print(nb);
            print(" drew_cyan="); num_to_str(nb, g_demo_cyan_drawn); print(nb); print("\n"); } }
#endif
        upscale_blit(win);                  /* nearest-upscale to the window */
        draw_hud(win, (int)win->w, (int)win->h);  /* HUD at full res on top */
        draw_ui_panel(win);                 /* inventory / character overlay */
        wl_commit(win);
        audio_step();                       /* feed gunshot/bite SFX to the HDA stream */
        sc(SYS_YIELD,0,0,0,0,0,0);
    }
}
