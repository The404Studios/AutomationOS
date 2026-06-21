/*
 * tls13_handshake.h -- TLS 1.3 handshake wire parsing (RFC 8446 4.1.3, 4.3+).
 * ==========================================================================
 * Freestanding helpers for the TLS 1.3 client handshake: parse the ServerHello
 * (detect 1.3 + extract the negotiated suite and key_share), and iterate the
 * handshake messages inside the decrypted server flight. The cryptographic
 * steps live in tls13_keysched / tls13_record / tls13_certverify.
 */
#ifndef TLS13_HANDSHAKE_H
#define TLS13_HANDSHAKE_H

/* TLS 1.3 cipher suites. */
#define TLS13_AES_128_GCM_SHA256  0x1301
#define TLS13_AES_256_GCM_SHA384  0x1302
#define TLS13_CHACHA20_SHA256     0x1303

/* Named groups. */
#define TLS13_GROUP_X25519  0x001d
#define TLS13_GROUP_P256    0x0017

/*
 * Parse a ServerHello handshake message (sh = type 0x02 || len(3) || body).
 * On success sets *cipher, and if the supported_versions extension selects
 * 0x0304, *is_tls13=1 and the key_share is extracted into *group + peer_pub
 * (peer_pub_cap bytes, *peer_pub_len set). Returns 0 on success, negative on
 * malformed input. is_tls13=0 means a TLS 1.2 (or earlier) ServerHello.
 */
int tls13_parse_server_hello(const unsigned char *sh, unsigned long shlen,
                             unsigned short *cipher, int *is_tls13,
                             unsigned short *group,
                             unsigned char *peer_pub, unsigned long peer_pub_cap,
                             unsigned long *peer_pub_len);

/*
 * Iterate handshake messages in a buffer (e.g. the decrypted server flight).
 * On the i-th call with *off the running offset, returns the message type and
 * sets *body/*body_len (the message body, after the 4-byte header) and advances
 * *off past the whole message. Returns the msg type (>0), or 0 when no complete
 * message remains, or -1 on malformed framing.
 */
int tls13_next_handshake_msg(const unsigned char *buf, unsigned long buflen,
                             unsigned long *off,
                             const unsigned char **body, unsigned long *body_len);

#endif /* TLS13_HANDSHAKE_H */
