/*
 * streamtest -- the AUDIO B1 part-2 userspace streaming proof.
 *
 * A ring-3 app streams continuous PCM to the HDA device via the new
 * SYS_AUDIO_STREAM_WRITE syscall (slot 128). The kernel pushes it into a
 * software ring; the on_bcis IRQ refill drains the ring into the DMA buffer
 * (gapless). We generate a 48 kHz / 16-bit / stereo sine (integer math, no libm)
 * and write it in a back-pressure loop (EAGAIN -> sleep so the DMA drains), then
 * prove the end-to-end path:
 *
 *   PASS iff the kernel ACCEPTED more bytes than the ring can hold (>2x ring) --
 *   which is only possible if the on_bcis consumer drained the ring repeatedly,
 *   i.e. real DMA streaming of OUR PCM. A negative return (ENOTSUP on a non-HDA
 *   kernel / ENODEV with no codec) -> honest SKIP.
 *
 * Marker: "STREAMTEST: PASS writes=N bytes=M" / "... FAIL ..." / "... SKIP ...".
 * Spawned only by an init built -DAUDIO_STREAMTEST. Pure-syscall, links crt0.
 */

#define SYS_EXIT               0
#define SYS_WRITE              3    /* write(fd,buf,len)  fd1 = serial/stdout    */
#define SYS_SLEEP              9    /* sleep(ms) real blocking ms                */
#define SYS_YIELD             15
#define SYS_AUDIO_STREAM_WRITE 128  /* sc(128, pcm, bytes, 0) -> bytes/neg       */
#define FD_STDOUT              1
#define SOCK_EAGAIN          (-11)  /* ring full (kernel returns EAGAIN)         */

typedef unsigned int   u32;
typedef int            i32;
typedef unsigned char  u8;
typedef short          i16;

/* ---- raw 6-arg syscall (verbatim from deadzoned.c) -------------------- */
static long sc(long n, long a1, long a2, long a3, long a4, long a5) {
    long r;
    register long r10 asm("r10") = a4;
    register long r8  asm("r8")  = a5;
    asm volatile("syscall" : "=a"(r)
                 : "a"(n), "D"(a1), "S"(a2), "d"(a3), "r"(r10), "r"(r8)
                 : "rcx", "r11", "memory");
    return r;
}
static unsigned s_len(const char* s){ unsigned n=0; if(!s)return 0; while(s[n])n++; return n; }
static void out_write(const char* s, unsigned n){ sc(SYS_WRITE, FD_STDOUT, (long)s, (long)n, 0, 0); }
static void out_puts(const char* s){ out_write(s, s_len(s)); }
static void out_unum(unsigned long v){
    char b[24]; int i=0;
    do { b[i++] = (char)('0' + (v % 10)); v /= 10; } while (v > 0);
    char r[24]; int j=0; while (i>0) r[j++] = b[--i];
    out_write(r, (unsigned)j);
}

/* ---- integer sine (Bhaskara I), brad input 0..65535 (verbatim from audio_tone.c) */
static i32 isin(u32 brad){
    brad &= 0xFFFF; int neg = 0;
    if (brad >= 32768){ brad -= 32768; neg = 1; }
    u32 deg = (brad * 180u) / 32768u;
    u32 t = deg * (180u - deg);
    i32 num = (i32)(4u * t) * 32767;
    i32 den = (i32)(40500u - t);
    i32 v = den ? (num / den) : 0;
    if (v > 32767) v = 32767;
    if (neg) v = -v;
    return v;
}

#define RATE      48000u
#define CHUNK_FR  512u                 /* frames per write; 4 B/frame stereo s16 */
#define RING_SIZE (64u * 1024u)        /* must match kernel HDA_RING_SIZE        */
#define TARGET    (4u * RING_SIZE)      /* prove >2x ring drained (256 KB)        */
#define MAX_ITERS 1200                  /* safety cap (~ a few s wall w/ sleeps)  */

static i16 g_pcm[CHUNK_FR * 2];        /* interleaved L,R                        */
static u32 g_phase = 0;
static u32 g_inc   = (u32)(((unsigned long long)440 * 65536ULL) / RATE);

static void fill_chunk(void){
    for (u32 f = 0; f < CHUNK_FR; f++){
        i32 s = isin(g_phase) / 2;     /* ~50% amplitude */
        g_phase += g_inc;
        g_pcm[f*2+0] = (i16)s;
        g_pcm[f*2+1] = (i16)s;
    }
}

int main(int argc, char** argv){
    (void)argc; (void)argv;
    unsigned long bytes = 0; u32 writes = 0, eagain = 0; int iters = 0;

    while (bytes < TARGET && iters < MAX_ITERS){
        iters++;
        fill_chunk();
        long n = sc(SYS_AUDIO_STREAM_WRITE, (long)g_pcm, (long)sizeof(g_pcm), 0, 0, 0);
        if (n > 0){
            bytes += (unsigned long)n; writes++;
            /* brief pace so the on_bcis DMA can drain the ring between bursts */
            if ((writes & 3u) == 0) sc(SYS_SLEEP, 4, 0, 0, 0, 0);
        } else if (n == SOCK_EAGAIN){
            eagain++;
            if (eagain > 4000) break;          /* DMA not draining -> give up */
            sc(SYS_SLEEP, 6, 0, 0, 0, 0);      /* let the ring drain */
        } else {
            /* negative != EAGAIN: ENOTSUP (non-HDA kernel) / ENODEV (no codec) */
            out_puts("STREAMTEST: SKIP (SYS_AUDIO_STREAM_WRITE rc=");
            { long v = -n; out_puts("-"); out_unum((unsigned long)v); }
            out_puts(" -- no audio device / unhandled)\n");
            sc(SYS_EXIT, 0, 0, 0, 0, 0);
        }
    }

    out_puts("STREAMTEST: streamed writes="); out_unum(writes);
    out_puts(" bytes=");  out_unum(bytes);
    out_puts(" eagain="); out_unum(eagain);
    out_puts("\n");
    /* PASS = the kernel accepted > 2x the ring (only possible if the on_bcis
     * consumer drained it repeatedly = real DMA streaming of our PCM). */
    if (writes > 8 && bytes > (2u * RING_SIZE))
        out_puts("STREAMTEST: PASS (userspace PCM streamed to HDA DMA)\n");
    else
        out_puts("STREAMTEST: FAIL\n");

    sc(SYS_EXIT, 0, 0, 0, 0, 0);
    return 0;
}
