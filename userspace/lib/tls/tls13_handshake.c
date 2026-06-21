/*
 * tls13_handshake.c -- TLS 1.3 handshake wire parsing. See tls13_handshake.h.
 * Freestanding.
 */
#include "tls13_handshake.h"

static unsigned int rd16(const unsigned char *p) { return ((unsigned)p[0] << 8) | p[1]; }
static unsigned long rd24(const unsigned char *p) {
    return ((unsigned long)p[0] << 16) | ((unsigned long)p[1] << 8) | p[2];
}

int tls13_parse_server_hello(const unsigned char *sh, unsigned long shlen,
                             unsigned short *cipher, int *is_tls13,
                             unsigned short *group,
                             unsigned char *peer_pub, unsigned long peer_pub_cap,
                             unsigned long *peer_pub_len)
{
    *is_tls13 = 0; *peer_pub_len = 0; *group = 0; *cipher = 0;
    if (shlen < 4 || sh[0] != 0x02) return -1;            /* server_hello */
    unsigned long blen = rd24(sh + 1);
    const unsigned char *p = sh + 4, *end = sh + 4 + blen;
    if (4 + blen > shlen) return -1;

    if (end - p < 2 + 32 + 1) return -1;
    p += 2;                                               /* legacy_version */
    /* HelloRetryRequest is a ServerHello whose random is the fixed SHA-256 of
     * "HelloRetryRequest" (RFC 8446 4.1.3); we do not support it (would need to
     * resend the ClientHello with the server-chosen group) -> reject with -2. */
    {
        static const unsigned char HRR[32] = {
            0xcf,0x21,0xad,0x74,0xe5,0x9a,0x61,0x11,0xbe,0x1d,0x8c,0x02,0x1e,0x65,0xb8,0x91,
            0xc2,0xa2,0x11,0x16,0x7a,0xbb,0x8c,0x5e,0x07,0x9e,0x09,0xe2,0xc8,0xa8,0x33,0x9c };
        int hrr = 1;
        for (int i = 0; i < 32; i++) if (p[i] != HRR[i]) { hrr = 0; break; }
        if (hrr) return -2;
    }
    p += 32;                                              /* random         */
    unsigned int sidlen = *p++;                           /* session_id echo */
    if (p + sidlen + 2 + 1 > end) return -1;
    p += sidlen;
    *cipher = (unsigned short)rd16(p); p += 2;            /* cipher_suite   */
    p += 1;                                               /* compression    */
    if (p + 2 > end) return -1;
    unsigned int extlen = rd16(p); p += 2;
    if (p + extlen > end) return -1;
    const unsigned char *eend = p + extlen;

    while (p + 4 <= eend) {
        unsigned int et = rd16(p); unsigned int el = rd16(p + 2);
        const unsigned char *ed = p + 4;
        if (ed + el > eend) return -1;
        if (et == 0x002b) {                               /* supported_versions */
            if (el == 2 && rd16(ed) == 0x0304) *is_tls13 = 1;
        } else if (et == 0x0033) {                        /* key_share */
            if (el >= 4) {
                *group = (unsigned short)rd16(ed);
                unsigned int kl = rd16(ed + 2);
                if (4u + kl <= el && kl <= peer_pub_cap) {
                    for (unsigned int i = 0; i < kl; i++) peer_pub[i] = ed[4 + i];
                    *peer_pub_len = kl;
                }
            }
        }
        p = ed + el;
    }
    return 0;
}

int tls13_next_handshake_msg(const unsigned char *buf, unsigned long buflen,
                             unsigned long *off,
                             const unsigned char **body, unsigned long *body_len)
{
    unsigned long o = *off;
    if (o + 4 > buflen) return 0;                         /* no complete header */
    int type = buf[o];
    unsigned long len = rd24(buf + o + 1);
    if (o + 4 + len > buflen) return -1;                  /* truncated body */
    *body = buf + o + 4;
    *body_len = len;
    *off = o + 4 + len;
    return type;
}
