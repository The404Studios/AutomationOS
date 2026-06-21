/*
 * tls.h -- minimal TLS 1.2 client (RFC 5246), freestanding ring-3 userspace.
 * ==========================================================================
 *
 * A small, deliberately-minimal TLS 1.2 *client*. It runs on top of an
 * ALREADY-CONNECTED TCP socket (the caller does net_socket()/net_connect()
 * first and hands us the fd). We do the handshake, then encrypt/decrypt
 * application data on tls_write()/tls_read().
 *
 * No libc / stdio / malloc / standard headers. Everything is inline syscalls
 * + fixed/static buffers + our own helpers. The cryptographic primitives are
 * provided by sibling libraries (crypto/*.h, tls/x509.h) compiled separately;
 * we only orchestrate them.
 *
 * CIPHER SUITES
 * -------------
 *   Modern ECDHE + AEAD suites (advertised in this preference order, and any of
 *   them is negotiated and driven end-to-end):
 *       0xC02B  ECDHE_ECDSA_WITH_AES_128_GCM_SHA256
 *       0xC02F  ECDHE_RSA_WITH_AES_128_GCM_SHA256     (broadest compatibility)
 *       0xC030  ECDHE_RSA_WITH_AES_256_GCM_SHA384
 *       0xCCA8  ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256
 *       0xCCA9  ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256
 *   Legacy last-resort fallback (no forward secrecy, kept for old servers):
 *       0x002F  TLS_RSA_WITH_AES_128_CBC_SHA
 *
 *   ECDHE key exchange supports the x25519 (0x001D) and secp256r1 (0x0017)
 *   named groups. The ServerKeyExchange signature is verified (RSA-PKCS1 or
 *   ECDSA-P256) against the server certificate's public key over
 *   client_random || server_random || ECDHE params. The PRF / Finished hash is
 *   selected per suite (P_SHA256 vs P_SHA384). Record protection for the AEAD
 *   suites follows RFC 5288 (AES-GCM, explicit 8-byte nonce on the wire) and
 *   RFC 7905 (ChaCha20-Poly1305, 12-byte nonce = write IV XOR padded seq).
 *
 * SECURITY CAVEATS (READ THIS)
 * ----------------------------
 *   *** Certificate CHAIN validation is performed ONLY if a separate
 *       x509_verify module is linked in (x509_verify_chain()). This TLS layer
 *       always verifies the ServerKeyExchange SIGNATURE with the leaf cert's
 *       public key (so an attacker cannot forge ECDHE params for that cert),
 *       but proving the leaf cert actually BELONGS to `server_name` and chains
 *       to a trusted root is the chain validator's job. If x509_verify_chain()
 *       is absent or returns nonzero, the handshake still completes but the
 *       connection is flagged encrypted-but-UNAUTHENTICATED: tls_cert_trusted()
 *       returns 0 and callers MUST treat the peer identity as unproven (an
 *       active man-in-the-middle presenting any valid-looking cert defeats it).
 *       When it returns 0 (trusted) tls_cert_trusted() returns 1. ***
 *
 *   Other honest limitations:
 *     - The legacy 0x002F path uses RSA key transport (NO forward secrecy) and
 *       HMAC-SHA1 (weak); it exists only as a last-resort fallback.
 *     - No constant-time discipline (documented Lucky-13-class caveat on the
 *       CBC fallback; AEAD tag checks rely on the sibling AEAD primitives).
 *     - No session resumption / renegotiation / client certificates.
 *     - For ECDSA server certs we parse the leaf SPKI EC point directly here
 *       (uncompressed secp256r1 point); other curves in the cert are rejected.
 *
 * BUILD (flags passed DIRECTLY on the command line; do NOT add fs:0x28 canary):
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -mstackrealign -O2 \
 *       -c userspace/lib/tls/tls.c -o tls.o
 *   # link with the crypto sibling objects (sha256.o sha384/sha512.o sha1.o
 *   #  hmac.o aes.o chacha20poly1305.o x25519.o p256.o rsa.o x509.o, and
 *   #  optionally x509_verify.o) and your app, via userspace/userspace.ld.
 *   objdump -d tls.o | grep fs:0x28   # MUST be empty
 */

#ifndef TLS_H
#define TLS_H

/* ---- error codes (negative) ---- */
#define TLS_OK              0
#define TLS_ERR            (-1)   /* generic failure                         */
#define TLS_ERR_IO         (-2)   /* socket send/recv error or peer closed   */
#define TLS_ERR_PROTO      (-3)   /* malformed / unexpected TLS message      */
#define TLS_ERR_ALERT      (-4)   /* peer sent a fatal alert                 */
#define TLS_ERR_SUITE      (-5)   /* server picked a suite we do not support */
#define TLS_ERR_CERT       (-6)   /* could not extract server pubkey         */
#define TLS_ERR_CRYPTO     (-7)   /* a crypto primitive failed               */
#define TLS_ERR_BUF        (-8)   /* a fixed buffer was too small            */
#define TLS_ERR_VERIFY     (-9)   /* Finished verify_data mismatch           */
#define TLS_ERR_CLOSED     (-10)  /* connection is closed                    */
#define TLS_ERR_SIG        (-11)  /* ServerKeyExchange signature is invalid  */
#define TLS_ERR_CURVE      (-12)  /* unsupported ECDHE named group / point   */

/* ---- sizing constants ---- */
/* TLS record payload (TLSPlaintext.length) max is 2^14 = 16384. Add room for
 * the 5-byte record header and CBC overhead (explicit IV + MAC + padding). */
#define TLS_MAX_PLAINTEXT  16384
#define TLS_RECORD_OVERHEAD 256          /* hdr + IV(16) + MAC(20) + pad      */
#define TLS_REC_BUF        (TLS_MAX_PLAINTEXT + TLS_RECORD_OVERHEAD)

/* Key material sizes. The legacy CBC suite uses a 20-byte HMAC-SHA1 MAC key
 * and a 16-byte AES-128 key; the modern AEAD suites have NO MAC key and use up
 * to a 32-byte (AES-256 / ChaCha20) bulk key plus a 4-byte implicit nonce salt.
 * The buffers below are sized for the largest of each so one struct serves all
 * negotiated suites. */
#define TLS_MAC_KEY_LEN    20            /* HMAC-SHA1 key (legacy CBC only)   */
#define TLS_ENC_KEY_LEN    32            /* widest bulk key (AES-256/ChaCha20)*/
#define TLS_FIXED_IV_LEN   16            /* widest write IV / explicit IV     */
#define TLS_AEAD_SALT_LEN  4             /* GCM/ChaCha implicit nonce salt    */
#define TLS_AEAD_TAG_LEN   16            /* GCM / Poly1305 auth tag           */
#define TLS_RANDOM_LEN     32
#define TLS_MASTER_LEN     48

/* ECDHE key-exchange sizes. */
#define TLS_ECDHE_MAX_PUB  65            /* uncompressed secp256r1 point      */
#define TLS_ECDHE_SECRET   32            /* x25519 / p256 shared secret bytes */

/*
 * tls_conn_t -- full connection state. Defined here (not opaque) so the caller
 * can simply declare `static tls_conn_t c;` -- no allocator required. It is
 * large (~40 KB, dominated by the two record buffers); place it in static
 * storage, not on a small stack.
 */
typedef struct tls_conn {
    int  fd;                 /* the already-connected TCP socket           */
    int  established;        /* 1 once the handshake completed             */
    int  closed;            /* 1 once close_notify sent or peer gone      */
    int  cert_trusted;      /* 1 if x509_verify_chain() trusted the leaf  */

    /* negotiated suite (0xC02B/0xC02F/0xC030/0xCCA8/0xCCA9/0x002F) */
    unsigned short cipher;

    /* --- decoded properties of the negotiated suite (set after ServerHello) */
    int  kx_ecdhe;          /* 1 = ECDHE, 0 = static RSA key transport    */
    int  auth_ecdsa;        /* 1 = server cert/sig is ECDSA, 0 = RSA       */
    int  aead;              /* 1 = AEAD record protection (GCM/ChaCha)     */
    int  aead_chacha;       /* 1 = ChaCha20-Poly1305, 0 = AES-GCM         */
    int  hash_sha384;       /* 1 = PRF/Finished use SHA-384, 0 = SHA-256   */
    unsigned int enc_key_len;  /* bulk key length in bytes (16 or 32)     */

    /* handshake randoms */
    unsigned char client_random[TLS_RANDOM_LEN];
    unsigned char server_random[TLS_RANDOM_LEN];

    /* derived secrets / key material */
    unsigned char master_secret[TLS_MASTER_LEN];
    unsigned char client_mac_key[TLS_MAC_KEY_LEN];   /* legacy CBC suite only */
    unsigned char server_mac_key[TLS_MAC_KEY_LEN];   /* legacy CBC suite only */
    unsigned char client_enc_key[TLS_ENC_KEY_LEN];
    unsigned char server_enc_key[TLS_ENC_KEY_LEN];
    unsigned char client_iv[TLS_FIXED_IV_LEN];   /* CBC: unused; AEAD: salt    */
    unsigned char server_iv[TLS_FIXED_IV_LEN];   /* CBC: unused; AEAD: salt    */

    /* --- ECDHE ephemeral state (set during the handshake) --- */
    unsigned short named_group;                  /* 0x001D x25519 / 0x0017 p256 */
    unsigned char  ecdhe_priv[32];               /* our ephemeral scalar       */
    unsigned char  ecdhe_pub[TLS_ECDHE_MAX_PUB]; /* our ephemeral public point */
    unsigned int   ecdhe_pub_len;                /* 32 (x25519) or 65 (p256)   */
    unsigned char  peer_pub[TLS_ECDHE_MAX_PUB];  /* server's ECDHE public point */
    unsigned int   peer_pub_len;                 /* 32 (x25519) or 65 (p256)   */

    /* record-layer sequence numbers (per RFC 5246 6.2.3.1, big-endian u64) */
    unsigned long long write_seq;
    unsigned long long read_seq;

    /* receive reassembly buffer for whole TLS records arriving in pieces */
    unsigned char rbuf[TLS_REC_BUF];
    unsigned long rbuf_len;            /* valid bytes currently in rbuf      */

    /* scratch for a single decrypted application-data record + read cursor  */
    unsigned char app_buf[TLS_REC_BUF];
    unsigned long app_len;            /* decrypted plaintext bytes available */
    unsigned long app_off;            /* consumed so far by tls_read()       */

    /* outgoing record scratch */
    unsigned char wbuf[TLS_REC_BUF];

    /* server RSA public key (extracted from leaf cert; chain trust is a
     * SEPARATE concern -- see cert_trusted / x509_verify_chain). */
    unsigned char srv_mod[512];
    unsigned long srv_mod_len;
    unsigned char srv_exp[16];
    unsigned long srv_exp_len;

    /* server ECDSA (secp256r1) public key, uncompressed point, for ECDSA SKE
     * signature verification when the leaf cert is an EC cert. */
    unsigned char srv_ec_pub[65];
    unsigned long srv_ec_pub_len;       /* 65 when present, else 0            */

    /* leaf certificate DER kept for the chain validator (if linked). This
     * aliases chain_der[0]/chain_len[0] below for any code that reads it. */
    const unsigned char *leaf_der;
    unsigned long        leaf_der_len;

    /* FULL server-sent certificate chain (leaf first, then intermediates), as
     * parsed out of the Certificate handshake message. Pointers alias the
     * handshake accumulator (valid only during the handshake). Passed to
     * x509_verify_chain() so leaf->intermediate->root validation can succeed
     * for real sites; chain_count is the number of valid entries. */
#define TLS_MAX_CHAIN 8
    const unsigned char *chain_der[TLS_MAX_CHAIN];
    unsigned long        chain_len[TLS_MAX_CHAIN];
    int                  chain_count;

    /* --- TLS 1.3 state (RFC 8446); set only when a 1.3 ServerHello is
     * negotiated. The 1.3 handshake + record protection reuse the proven
     * tls13_* modules; these fields carry the per-connection key material. */
    int                is_tls13;             /* 1 once 1.3 negotiated          */
    unsigned char      t13_cli_priv[32];     /* our x25519 key_share private   */
    unsigned char      t13_peer_pub[32];     /* server key_share public        */
    unsigned char      t13_chs[32];          /* client handshake traffic secret */
    unsigned char      t13_shs[32];          /* server handshake traffic secret */
    int                t13_aead;             /* TLS13_AEAD_* id (record AEAD)   */
    unsigned int       t13_keylen;           /* bulk key length (16 or 32)      */
    unsigned char      t13_rkey[32], t13_riv[12];  /* read (server) app key/iv  */
    unsigned char      t13_wkey[32], t13_wiv[12];  /* write (client) app key/iv */
    unsigned long long t13_rseq, t13_wseq;   /* app record sequence numbers     */
} tls_conn_t;

/*
 * tls_client_connect -- perform the full TLS 1.2 handshake over `tcp_fd`.
 *   c           : caller-provided, zero-initialized-or-not state (we init it).
 *   tcp_fd      : an already-connected TCP socket fd.
 *   server_name : NUL-terminated hostname, sent in the SNI extension and passed
 *                 to the optional x509_verify_chain() validator. Whether the
 *                 cert was actually proven to belong to this host is reported by
 *                 tls_cert_trusted() (see the security caveat above).
 * Returns 0 on success, a negative TLS_ERR_* on failure.
 */
int  tls_client_connect(tls_conn_t *c, int tcp_fd, const char *server_name);

/*
 * tls_write -- encrypt and send `len` application-data bytes.
 * Returns bytes accepted (== len on success) or a negative TLS_ERR_*.
 */
long tls_write(tls_conn_t *c, const void *buf, unsigned long len);

/*
 * tls_read -- receive and decrypt application data into `buf` (cap bytes).
 * Returns bytes read (>0), 0 if the peer closed cleanly (close_notify),
 * or -1 on error.
 */
long tls_read(tls_conn_t *c, void *buf, unsigned long cap);

/*
 * tls_close -- send close_notify (best effort) and mark the connection closed.
 * Does NOT close the underlying TCP fd (the caller owns it).
 */
void tls_close(tls_conn_t *c);

/*
 * tls_selftest -- offline (no network) sanity check of the key-derivation
 * math. Verifies the TLS 1.2 PRF (P_SHA256) against the well-known published
 * TLS 1.2 PRF test vector. Returns 0 on pass, negative on failure. Call this
 * to gate the handshake's correctness without a live server.
 */
int  tls_selftest(void);

/*
 * tls_cert_trusted -- report whether the server certificate chain was validated
 * to a trusted root for `server_name`. Returns 1 if the optional
 * x509_verify_chain() module was linked AND accepted the chain, 0 otherwise
 * (no validator linked, or it rejected the chain). When this returns 0 the
 * traffic is encrypted but the PEER IDENTITY IS UNAUTHENTICATED -- callers that
 * display a "lock" indicator MUST gate it on this returning 1.
 */
int  tls_cert_trusted(const tls_conn_t *c);

#endif /* TLS_H */
