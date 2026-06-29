/*
 * dzproto_test -- headless cross-check of the DeadZone multiplayer wire
 * protocol (dzproto.h), the SAME header the server (deadzoned) and the game
 * client (deadzone) both include. Proves the encode/decode pair round-trips
 * byte-exactly for both packet families, so the two ends are wire-compatible.
 *
 * Prints "DZPROTO: PASS ..." / "DZPROTO: FAIL <why>". Pure crt0 console binary.
 */
#define SYS_WRITE   3
#define FD_STDOUT   1

#include "dzproto.h"

static long sc(long n, long a1, long a2, long a3)
{
    long r;
    asm volatile("syscall" : "=a"(r)
                 : "a"(n), "D"(a1), "S"(a2), "d"(a3)
                 : "rcx", "r11", "memory");
    return r;
}
static unsigned slen(const char *s){unsigned n=0;while(s[n])n++;return n;}
static void puts_(const char *s){ sc(SYS_WRITE, FD_STDOUT, (long)s, slen(s)); }
static void unum(unsigned long v){
    char b[24]; int i=0; do{b[i++]=(char)('0'+v%10);v/=10;}while(v);
    char r[24]; int j=0; while(i)r[j++]=b[--i];
    sc(SYS_WRITE, FD_STDOUT, (long)r, j);
}

#define FAIL(why) do{ puts_("DZPROTO: FAIL " why "\n"); return 1; }while(0)

int main(void)
{
    /* (1) input packet round-trip ------------------------------------- */
    dz_input_t in = { .seq = 0xCAFE1234u, .move_x = -57, .move_y = 60,
                      .yaw = 777, .buttons = DZ_BTN_FIRE };
    dz_u8 ibuf[DZ_INPUT_BYTES];
    dz_input_encode(ibuf, &in);
    /* the magic must be exactly the ASCII "DZI1" little-endian */
    if (!(ibuf[0]=='D' && ibuf[1]=='Z' && ibuf[2]=='I' && ibuf[3]=='1'))
        FAIL("input_magic_bytes");
    dz_input_t in2;
    if (!dz_input_decode(ibuf, &in2)) FAIL("input_decode");
    if (in2.seq != in.seq || in2.move_x != in.move_x || in2.move_y != in.move_y
        || in2.yaw != in.yaw || in2.buttons != in.buttons) FAIL("input_roundtrip");

    /* (2) snapshot round-trip with several players + zombies ---------- */
    static dz_snapshot_t s;
    s.tick = 9001; s.wave = 4; s.n_players = 3; s.n_zombies = 17;
    for (dz_u32 i = 0; i < s.n_players; i++) {
        s.p[i].id = i; s.p[i].x = 100 + (dz_i32)i*7; s.p[i].y = -200 - (dz_i32)i*3;
        s.p[i].hp = 100 - (dz_i32)i*11; s.p[i].yaw = (i*113) & 1023u;
        s.p[i].score = i*5;
    }
    for (dz_u32 i = 0; i < s.n_zombies; i++) {
        s.z[i].x = (dz_i32)i*131 - 500; s.z[i].y = (dz_i32)i*29 + 7;
        s.z[i].state = (i & 3) ? 1u : 0u;
    }
    static dz_u8 sbuf[DZ_SNAP_MAX_BYTES];
    dz_u32 n = dz_snap_encode(&s, sbuf, sizeof(sbuf));
    if (n == 0) FAIL("snap_encode");
    if (n != dz_snap_bytes(s.n_players, s.n_zombies)) FAIL("snap_bytes_mismatch");
    if (!(sbuf[0]=='D' && sbuf[1]=='Z' && sbuf[2]=='S' && sbuf[3]=='1'))
        FAIL("snap_magic_bytes");
    static dz_snapshot_t s2;
    if (!dz_snap_decode(sbuf, n, &s2)) FAIL("snap_decode");
    if (s2.tick != s.tick || s2.wave != s.wave
        || s2.n_players != s.n_players || s2.n_zombies != s.n_zombies)
        FAIL("snap_hdr_roundtrip");
    for (dz_u32 i = 0; i < s.n_players; i++)
        if (s2.p[i].id != s.p[i].id || s2.p[i].x != s.p[i].x || s2.p[i].y != s.p[i].y
            || s2.p[i].hp != s.p[i].hp || s2.p[i].yaw != s.p[i].yaw
            || s2.p[i].score != s.p[i].score) FAIL("snap_player_roundtrip");
    for (dz_u32 i = 0; i < s.n_zombies; i++)
        if (s2.z[i].x != s.z[i].x || s2.z[i].y != s.z[i].y
            || s2.z[i].state != s.z[i].state) FAIL("snap_zombie_roundtrip");

    /* (3) a truncated snapshot must be rejected (bounds safety) -------- */
    if (dz_snap_decode(sbuf, n - 1, &s2)) FAIL("truncation_not_rejected");

    puts_("DZPROTO: PASS players="); unum(s.n_players);
    puts_(" zombies=");             unum(s.n_zombies);
    puts_(" snap_bytes=");          unum(n);
    puts_(" input_bytes=");         unum(DZ_INPUT_BYTES);
    puts_("\n");
    return 0;
}
