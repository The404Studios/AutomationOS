/*
 * sae.h -- freestanding WPA3 SAE ("dragonfly") handshake, group 19 (P-256).
 * =========================================================================
 *
 * Simultaneous Authentication of Equals (IEEE 802.11-2020 sec. 12.4) is the
 * password-authenticated key exchange that replaces WPA2-PSK's 4-way-handshake
 * PMK derivation in WPA3-Personal.  Both peers prove knowledge of a shared
 * password without ever transmitting it, and derive a fresh PMK resistant to
 * offline dictionary attack.
 *
 * This implementation covers the original "hunting and pecking" PWE
 * derivation (IEEE 802.11-2020 sec. 12.4.4.2.2 / RFC 7664 sec. 3.2.1) for
 * finite-cyclic group 19 (ECP NIST P-256).  It does NOT implement the newer
 * Hash-to-Element (H2E, group 19 with a different PWE method) -- only the
 * classic dragonfly hunt-and-peck path, which is the one IEEE Annex J.10
 * provides worked test vectors for.
 *
 * Pure computation: no libc, no syscalls, no malloc, no standard headers,
 * fixed stack buffers only.  Depends on the freestanding crypto lib:
 *   p256.c / p256_internal.h  -- the curve field/group arithmetic
 *   sha256.c, hmac.c          -- HMAC-SHA256 + the IEEE 802.11 KDF
 *
 * Build flags (matching the rest of the crypto lib):
 *   -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector
 *   -fno-pic -fno-pie -mno-red-zone -mstackrealign -O2
 *
 * -------------------------------------------------------------------------
 * Protocol summary (group 19)
 * -------------------------------------------------------------------------
 *   PWE   : the password element, a curve point derived from the password
 *           and the two peer MAC addresses by hunting-and-pecking.
 *   rand  : a random scalar in [1, q-1]   (q = group order n)
 *   mask  : a random scalar in [1, q-1]
 *   commit-scalar  s = (rand + mask) mod q
 *   commit-element E = inverse( mask * PWE )      (point negation of mask*PWE)
 *
 *   On receiving the peer's (peer_s, peer_E):
 *     K = rand * ( peer_E + peer_s * PWE )
 *     k = X-coordinate(K)
 *     keyseed = HMAC-SHA256( <32 zero bytes>, k )
 *     context = (s + peer_s) mod q
 *     KCK || PMK = KDF-512( keyseed, "SAE KCK and PMK", context )
 *       KCK = first  32 bytes   (confirm key)
 *       PMK = second 32 bytes   (delivered to the 4-way handshake)
 *
 *   Confirm exchange (sec. 12.4.5.5):
 *     confirm = CN( KCK, send-confirm,
 *                   own_scalar,  own_element,
 *                   peer_scalar, peer_element )
 *     where
 *       CN(K, sc, s1, E1, s2, E2) =
 *           HMAC-SHA256( K, sc_LE16 || s1 || E1 || s2 || E2 )
 *       send-confirm is a 16-bit anti-replay counter (little-endian), 0 for the
 *       first confirm.  The VERIFIER swaps the (scalar,element) pairs: it checks
 *       CN(KCK, peer_sc, peer_scalar, peer_element, own_scalar, own_element).
 *       Scalars are 32-byte big-endian; elements are 64-byte X||Y.
 */

#ifndef CRYPTO_SAE_H
#define CRYPTO_SAE_H

/* Freestanding fixed-width types, guarded to coexist with the other crypto
 * headers (pbkdf2.h, ccm.h, ...) that share CRYPTO_STDINT_DEFINED. */
#ifndef CRYPTO_STDINT_DEFINED
#define CRYPTO_STDINT_DEFINED
typedef unsigned char  uint8_t;
typedef unsigned int   uint32_t;
#endif

/* SAE element/scalar sizes for group 19 (P-256): 32-byte field/scalar values. */
#define SAE_SCALAR_LEN  32   /* commit-scalar, k, KCK, PMK            */
#define SAE_ELEMENT_LEN 64   /* commit-element = X(32) || Y(32)       */
#define SAE_PMK_LEN     32
#define SAE_KCK_LEN     32

/* -------------------------------------------------------------------------
 * Per-peer SAE state.  A caller drives one of these per authentication.
 * All buffers are big-endian wire encodings (X || Y for points).
 * ---------------------------------------------------------------------- */
typedef struct {
    int          valid;                     /* 1 once PWE has been derived  */
    unsigned char pwe_x[SAE_SCALAR_LEN];    /* PWE X coordinate             */
    unsigned char pwe_y[SAE_SCALAR_LEN];    /* PWE Y coordinate             */

    unsigned char rand[SAE_SCALAR_LEN];     /* our secret scalar rand       */
    unsigned char mask[SAE_SCALAR_LEN];     /* our secret scalar mask       */

    unsigned char commit_scalar[SAE_SCALAR_LEN];   /* s = (rand+mask) mod q */
    unsigned char commit_element[SAE_ELEMENT_LEN]; /* E = -(mask*PWE)       */
} sae_state;

/*
 * sae_derive_pwe -- hunting-and-pecking PWE derivation (sec. 12.4.4.2.2).
 *
 *   password   : the SAE password (octets; length passwordlen)
 *   mac_a/mac_b: the two peer MAC addresses (6 bytes each, any order -- the
 *                algorithm canonicalises to MAX||MIN internally)
 *   st         : receives pwe_x/pwe_y and is marked valid on success
 *
 * Runs a FIXED number of iterations (>= 40) regardless of when the PWE is
 * found, then keeps iterating without updating PWE, to blunt the timing
 * side-channel that the classic dragonfly hunt-and-peck is known for.
 *
 * Returns 0 on success, -1 if no PWE was found in the iteration budget
 * (cryptographically negligible) or on bad arguments.
 */
int sae_derive_pwe(sae_state *st,
                   const unsigned char *password, unsigned long passwordlen,
                   const unsigned char mac_a[6], const unsigned char mac_b[6]);

/*
 * sae_build_commit -- produce our commit message (sec. 12.4.5.3).
 *
 * Given rand and mask scalars (each a 32-byte big-endian value in [1,q-1]),
 * computes:
 *   commit_scalar  = (rand + mask) mod q
 *   commit_element = inverse(mask * PWE)
 * and stores them (plus rand/mask) in st.  st must already hold a valid PWE.
 *
 * Real supplicants supply rand/mask from a CSPRNG; exposing them as inputs
 * lets the self-test drive both sides deterministically.
 *
 * Returns 0 on success, -1 on bad state / out-of-range scalar / point error.
 */
int sae_build_commit(sae_state *st,
                     const unsigned char rand[32],
                     const unsigned char mask[32]);

/*
 * sae_process_commit -- consume the peer commit and derive KCK/PMK
 * (sec. 12.4.5.4 + 12.4.4.3).
 *
 *   st            : our state, after a successful sae_build_commit
 *   peer_scalar   : peer commit-scalar (32-byte big-endian, must be in [1,q-1])
 *   peer_element  : peer commit-element X||Y (64 bytes), must be on the curve
 *   kck           : output confirm key (32 bytes), may be NULL if not needed
 *   pmk           : output PMK (32 bytes)
 *
 * Returns 0 on success, -1 if the peer values are invalid (scalar out of
 * range, element not on curve, K is the identity, etc.).
 */
int sae_process_commit(const sae_state *st,
                       const unsigned char peer_scalar[32],
                       const unsigned char peer_element[64],
                       unsigned char kck[32],
                       unsigned char pmk[32]);

/*
 * sae_build_confirm -- produce OUR confirm value (sec. 12.4.5.5).
 *
 *   confirm = HMAC-SHA256( KCK,
 *                          send_confirm_LE16 ||
 *                          own_commit_scalar  || own_commit_element  ||
 *                          peer_commit_scalar || peer_commit_element )
 *
 *   st            : our state, after a successful sae_build_commit (supplies
 *                   own commit_scalar / commit_element)
 *   kck           : the confirm key from sae_process_commit (32 bytes)
 *   send_confirm  : the 16-bit anti-replay counter (Sync/sc); 0 for the first
 *                   confirm.  Encoded little-endian into the hash.
 *   peer_scalar   : peer commit-scalar (32-byte big-endian)
 *   peer_element  : peer commit-element X||Y (64 bytes)
 *   confirm_out   : receives the 32-byte confirm value
 *
 * Returns 0 on success, -1 on bad arguments / state.
 */
int sae_build_confirm(const sae_state *st,
                      const unsigned char kck[32],
                      unsigned short send_confirm,
                      const unsigned char peer_scalar[32],
                      const unsigned char peer_element[64],
                      unsigned char confirm_out[32]);

/*
 * sae_check_confirm -- verify the PEER's confirm value (sec. 12.4.5.5).
 *
 * The verifier swaps the (scalar,element) pairs relative to the builder:
 *   expect = HMAC-SHA256( KCK,
 *                         peer_send_confirm_LE16 ||
 *                         peer_commit_scalar || peer_commit_element ||
 *                         own_commit_scalar  || own_commit_element )
 * and compares it (constant-time-ish) against the received peer_confirm.
 *
 *   st               : our state (supplies own commit_scalar / commit_element)
 *   kck              : the confirm key (32 bytes)
 *   peer_send_confirm: the peer's 16-bit send-confirm counter
 *   peer_scalar      : peer commit-scalar (32-byte big-endian)
 *   peer_element     : peer commit-element X||Y (64 bytes)
 *   peer_confirm     : the 32-byte confirm value the peer sent
 *
 * Returns 0 if the peer confirm verifies, -1 otherwise.
 */
int sae_check_confirm(const sae_state *st,
                      const unsigned char kck[32],
                      unsigned short peer_send_confirm,
                      const unsigned char peer_scalar[32],
                      const unsigned char peer_element[64],
                      const unsigned char peer_confirm[32]);

/*
 * sae_derive_pmk -- one-shot convenience wrapper that drives a full SAE
 * exchange for ONE side.  Given the password, both MACs, and this side's
 * rand/mask plus the PEER's commit (peer_scalar/peer_element), produces the
 * PMK.  Used by the self-test to run both parties.
 *
 * Returns 0 on success, -1 on any failure.
 */
int sae_derive_pmk(const unsigned char *password, unsigned long passwordlen,
                   const unsigned char mac_a[6], const unsigned char mac_b[6],
                   const unsigned char rand[32], const unsigned char mask[32],
                   const unsigned char peer_scalar[32],
                   const unsigned char peer_element[64],
                   unsigned char pmk[32]);

/*
 * sae_selftest -- known-answer + self-consistency tests.  Returns 0 on PASS.
 */
int sae_selftest(void);

#endif /* CRYPTO_SAE_H */
