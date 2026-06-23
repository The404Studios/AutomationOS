/*
 * dzproto.h -- the DeadZone multiplayer wire protocol, shared verbatim by the
 * authoritative server (sbin/deadzoned) and the game client (sbin/deadzone).
 *
 * ONE definition for both ends -> they cannot drift. Everything is packed
 * little-endian by hand (no struct-on-the-wire, no padding ambiguity). Two
 * fixed-shape packet families:
 *
 *   client -> server : DZ_INPUT    [DZI1]  fixed 24 bytes
 *       u32 magic, u32 seq, i32 move_x, i32 move_y, u32 yaw, u32 buttons
 *
 *   server -> client : DZ_SNAPSHOT [DZS1]  variable
 *       u32 magic, u32 tick, u32 wave, u32 n_players, u32 n_zombies,
 *       players[n_players]: { u32 id, i32 x, i32 y, i32 hp, u32 yaw, u32 score }
 *       zombies[n_zombies]: { i32 x, i32 y, u32 state }
 *
 * Header-only, freestanding, no libc. Safe to include from a -ffreestanding
 * translation unit on both sides.
 */
#ifndef DZPROTO_H
#define DZPROTO_H

#ifndef DZPROTO_TYPES
#define DZPROTO_TYPES
typedef unsigned int   dz_u32;
typedef int            dz_i32;
typedef unsigned char  dz_u8;
#endif

/* ---- world bounds (shared so the client can clamp/render consistently) -- */
#define DZP_MAX_CLIENTS   8
#define DZP_MAX_ZOMBIES   64
#define DZP_ARENA         10000

/* ---- packet magics ("DZI1"/"DZS1" little-endian) ----------------------- */
#define DZ_INPUT_MAGIC    0x31495A44u
#define DZ_SNAP_MAGIC     0x31535A44u

/* ---- input buttons ----------------------------------------------------- */
#define DZ_BTN_FIRE       0x1u

/* ---- fixed sizes ------------------------------------------------------- */
#define DZ_INPUT_BYTES    24
#define DZ_SNAP_HDR_U32   5
#define DZ_SNAP_PLAYER_U32 6
#define DZ_SNAP_ZOMBIE_U32 3
#define DZ_SNAP_MAX_BYTES  (4 * (DZ_SNAP_HDR_U32 \
                               + DZP_MAX_CLIENTS * DZ_SNAP_PLAYER_U32 \
                               + DZP_MAX_ZOMBIES * DZ_SNAP_ZOMBIE_U32))

/* ---- little-endian byte (un)packers ------------------------------------ */
static inline void dz_put_u32(dz_u8 *p, dz_u32 v)
{
    p[0] = (dz_u8)(v); p[1] = (dz_u8)(v >> 8);
    p[2] = (dz_u8)(v >> 16); p[3] = (dz_u8)(v >> 24);
}
static inline dz_u32 dz_get_u32(const dz_u8 *p)
{
    return (dz_u32)p[0] | ((dz_u32)p[1] << 8)
         | ((dz_u32)p[2] << 16) | ((dz_u32)p[3] << 24);
}
static inline void dz_put_i32(dz_u8 *p, dz_i32 v) { dz_put_u32(p, (dz_u32)v); }
static inline dz_i32 dz_get_i32(const dz_u8 *p)   { return (dz_i32)dz_get_u32(p); }

/* ---- input packet ------------------------------------------------------ */
typedef struct {
    dz_u32 seq;
    dz_i32 move_x, move_y;
    dz_u32 yaw;
    dz_u32 buttons;
} dz_input_t;

/* Encode an input into exactly DZ_INPUT_BYTES bytes. */
static inline void dz_input_encode(dz_u8 *buf, const dz_input_t *in)
{
    dz_put_u32(buf,      DZ_INPUT_MAGIC);
    dz_put_u32(buf + 4,  in->seq);
    dz_put_i32(buf + 8,  in->move_x);
    dz_put_i32(buf + 12, in->move_y);
    dz_put_u32(buf + 16, in->yaw);
    dz_put_u32(buf + 20, in->buttons);
}
/* Decode an input packet; returns 1 if the magic matches. */
static inline int dz_input_decode(const dz_u8 *buf, dz_input_t *in)
{
    if (dz_get_u32(buf) != DZ_INPUT_MAGIC) return 0;
    in->seq     = dz_get_u32(buf + 4);
    in->move_x  = dz_get_i32(buf + 8);
    in->move_y  = dz_get_i32(buf + 12);
    in->yaw     = dz_get_u32(buf + 16);
    in->buttons = dz_get_u32(buf + 20);
    return 1;
}

/* ---- snapshot (client-side view) --------------------------------------- */
typedef struct { dz_u32 id; dz_i32 x, y, hp; dz_u32 yaw, score; } dz_player_t;
typedef struct { dz_i32 x, y; dz_u32 state; } dz_zombie_t;

typedef struct {
    dz_u32 tick, wave, n_players, n_zombies;
    dz_player_t p[DZP_MAX_CLIENTS];
    dz_zombie_t z[DZP_MAX_ZOMBIES];
} dz_snapshot_t;

/* How many bytes a snapshot with these counts occupies on the wire. */
static inline dz_u32 dz_snap_bytes(dz_u32 n_players, dz_u32 n_zombies)
{
    return 4 * (DZ_SNAP_HDR_U32 + n_players * DZ_SNAP_PLAYER_U32
                                + n_zombies * DZ_SNAP_ZOMBIE_U32);
}

/* Serialize a snapshot view into buf (<=cap). Returns byte length or 0. Used
 * by the server; the client only ever decodes, but sharing it keeps the two
 * sides provably symmetric (the round-trip self-test exercises both). */
static inline dz_u32 dz_snap_encode(const dz_snapshot_t *s, dz_u8 *buf, dz_u32 cap)
{
    dz_u32 need = dz_snap_bytes(s->n_players, s->n_zombies);
    if (need > cap) return 0;
    dz_u8 *p = buf;
    dz_put_u32(p, DZ_SNAP_MAGIC);  p += 4;
    dz_put_u32(p, s->tick);        p += 4;
    dz_put_u32(p, s->wave);        p += 4;
    dz_put_u32(p, s->n_players);   p += 4;
    dz_put_u32(p, s->n_zombies);   p += 4;
    for (dz_u32 i = 0; i < s->n_players; i++) {
        dz_put_u32(p, s->p[i].id);    p += 4;
        dz_put_i32(p, s->p[i].x);     p += 4;
        dz_put_i32(p, s->p[i].y);     p += 4;
        dz_put_i32(p, s->p[i].hp);    p += 4;
        dz_put_u32(p, s->p[i].yaw);   p += 4;
        dz_put_u32(p, s->p[i].score); p += 4;
    }
    for (dz_u32 i = 0; i < s->n_zombies; i++) {
        dz_put_i32(p, s->z[i].x);     p += 4;
        dz_put_i32(p, s->z[i].y);     p += 4;
        dz_put_u32(p, s->z[i].state); p += 4;
    }
    return need;
}

/* Parse a snapshot into a client-side view. Returns 1 on a structurally valid
 * packet, 0 otherwise. Bounds-checks counts + length against the buffer. */
static inline int dz_snap_decode(const dz_u8 *buf, dz_u32 len, dz_snapshot_t *out)
{
    if (len < 4 * DZ_SNAP_HDR_U32) return 0;
    const dz_u8 *p = buf;
    if (dz_get_u32(p) != DZ_SNAP_MAGIC) return 0;
    p += 4;
    out->tick      = dz_get_u32(p); p += 4;
    out->wave      = dz_get_u32(p); p += 4;
    out->n_players = dz_get_u32(p); p += 4;
    out->n_zombies = dz_get_u32(p); p += 4;
    if (out->n_players > DZP_MAX_CLIENTS || out->n_zombies > DZP_MAX_ZOMBIES) return 0;
    if (len < dz_snap_bytes(out->n_players, out->n_zombies)) return 0;
    for (dz_u32 i = 0; i < out->n_players; i++) {
        out->p[i].id    = dz_get_u32(p); p += 4;
        out->p[i].x     = dz_get_i32(p); p += 4;
        out->p[i].y     = dz_get_i32(p); p += 4;
        out->p[i].hp    = dz_get_i32(p); p += 4;
        out->p[i].yaw   = dz_get_u32(p); p += 4;
        out->p[i].score = dz_get_u32(p); p += 4;
    }
    for (dz_u32 i = 0; i < out->n_zombies; i++) {
        out->z[i].x     = dz_get_i32(p); p += 4;
        out->z[i].y     = dz_get_i32(p); p += 4;
        out->z[i].state = dz_get_u32(p); p += 4;
    }
    return 1;
}

#endif /* DZPROTO_H */
