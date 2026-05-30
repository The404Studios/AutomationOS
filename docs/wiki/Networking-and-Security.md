# Networking & Security

AutomationOS speaks to the network through an entirely from-scratch stack: a
poll-mode NIC driver in the kernel, a minimal Ethernet/ARP/IPv4/ICMP/UDP/TCP
implementation, a BSD-style socket layer exposed to ring 3 via syscalls, a
freestanding userspace networking library (DNS, HTTP/1.1, DHCP), and a
hand-rolled cryptography suite — hashes, AEAD ciphers, public-key, and
ASN.1/X.509 — that powers a real **TLS 1.2** client and `https://` fetching.
Nothing here links a third-party library; every byte of crypto and protocol is
original code compiled `-ffreestanding -nostdlib`. Everything is single-threaded
and poll-mode, matching the rest of the OS.

This page is the network counterpart to [Drivers & I/O](Drivers-and-IO.md),
which covers the storage/VFS side; the NIC driver and kernel stack summarized
there are documented in full below.

---

## The NIC driver (`kernel/drivers/net/e1000.c`)

The supported NIC is Intel's e1000 family. The **verified** path targets QEMU's
emulated 82540EM (`-device e1000`, PCI `0x8086:0x100E`) and the closely related
82545; QEMU's `-device e1000e` (82574L, `0x10D3`) is treated as part of the same
classic bring-up. Detection also recognizes a range of e1000e/PCH parts
including the **ThinkPad T410's Intel 82577LM** (`0x8086:0x10EA`).

**Classic bring-up** (the working path) is deliberately simple: scan PCI for the
device, enable bus-master + memory space, map BAR0 (the MMIO register file),
read the MAC from the Receive Address registers (RAL0/RAH0, seeded by QEMU from
the EEPROM image as `52:54:00:12:34:56`), allocate RX (32 descriptors, 2 KiB
buffers) and TX (8 descriptors) rings from the PMM, then program
RDBAL/RDBAH/RDLEN/RDH/RDT and the TX equivalents and set RCTL/TCTL to enable the
receiver and transmitter. Because the kernel identity-maps physical RAM 1:1, a
`pmm_alloc_page()` pointer **is** its DMA address. RX/TX is poll-mode (device
IRQs are left masked); the caller drains the NIC by calling into the stack.

### The T410 / PCH runtime gate (honest scope)

The PCH-integrated parts (82577LM and friends: 82578/82579/i217/i218) are not
the classic e1000 — the MAC lives in the chipset and the PHY hangs off an
internal MDIO link **shared with the Management-Engine firmware**. Driving it
requires taking the SW/FW MDIO-ownership flag (`EXTCNF_CTRL.SWFLAG`), then
powering the PHY up and restarting auto-negotiation over MDIC. That full
bring-up (`e1000_pch_phy_bringup()`) is written and intact in the source — but it
is **gated off at runtime**. On the real T410, re-enabling it made the boot hang
inside the 82577LM bring-up: a *bus stall*, not a software spin, so the
iteration-cap discipline used elsewhere in the driver cannot unwedge it. The
gate fires the instant a PCH part is recognized, *before any MMIO access*:

```c
if (e1000.is_pch) {
    kprintf("[E1000] PCH NIC 0x%04x detected -- bring-up DISABLED (hangs real "
            "hardware); declining to keep the boot alive\n", dev->device_id);
    return -1;
}
```

PCI config-space enumeration (safe) still identifies the device, so the T410
boots to the desktop with the NIC **detected but link-down**. The gate can be
removed once the PCH path is validated on real hardware. (An RTL8139 fallback
path also exists in `kernel/net/net.c` for the case where no e1000 is present.)

---

## The kernel protocol stack (`kernel/net/`)

A "just enough" TCP/IP stack sitting directly on the NIC. It is poll-mode and
single-threaded: the caller drives `net_recv()` in a loop, and inbound frames
are run through ARP-learning, ARP-reply, and ICMP-echo-reply on the way in. All
on-wire multi-byte fields are big-endian; internal IPs are kept host-order. In
QEMU user-net (slirp) the guest is `10.0.2.15`, the gateway `10.0.2.2`, and DNS
`10.0.2.3`.

| File | Responsibility |
|---|---|
| `net.c` | Ethernet framing; ARP (request + reply + 16-entry cache); IPv4 RX/TX; ICMP echo reply; IPv4 fragment reassembly (up to 16 concurrent, 30 s timeout); NIC backend selection (e1000 / RTL8139) |
| `route.c` | IPv4 routing table (up to 16 entries), longest-prefix match: `route_add` / `route_lookup` / `route_print` |
| `socket.c` | BSD-style socket table (heap-allocated, up to `SOCK_MAX`), `sock_poll()` demux, shared IPv4 TX, TCP timers |
| `udp.c` | UDP TX (with IPv4 pseudo-header checksum) and inbound demux into per-socket receive rings |
| `tcp.c` | RFC 793 active-open TCP: 3-way handshake, segmentation (`TCP_MSS`), multi-retransmit with exponential back-off, out-of-order side-table, dynamic receive window, FIN/RST handling |
| `netsyscall.c` | Syscall wiring (see below) |

### The socket layer & syscalls (`kernel/net/netsyscall.c`)

The socket table is exposed to ring 3 as a BSD-ish API. The wired syscalls are
`SYS_SOCKET`, `SYS_CONNECT`, `SYS_SEND`, `SYS_RECV`, `SYS_SENDTO`,
`SYS_RECVFROM`, `SYS_CLOSE_SK`, `SYS_SOCK_POLL`, and `SYS_BIND`; there is also a
raw-frame path (`SYS_NET_SEND` / `SYS_NET_RECV`, numbers 68/69) and
`SYS_NET_INFO` (59) which returns the NIC MAC + assigned IPv4 + gateway. Note
that the active-open model is the supported one — `SYS_BIND` exists but DHCP, for
example, works *without* claiming a fixed port (see below).

---

## The userspace networking library (`userspace/lib/net/`)

A freestanding ring-3 library (no libc, no stdio, no malloc) built directly on
the socket syscalls. Every module is fully **bounded** — no call can hang the OS
— and **gracefully degrades**: `net_available()` probes `SYS_NET_INFO` once and
caches the result, and when the driver is unwired every call returns
`NET_ERR_UNAVAIL` (`-ENOSYS`) so callers print "networking not enabled" instead
of crashing.

| Module | File | What it does |
|---|---|---|
| Socket wrapper | `net.c` / `net.h` | `net_socket` / `connect` / `send` / `recv` / `sendto` / `recvfrom` / `close` / `poll` / `info`, plus `htons`/`htonl`, dotted-quad parse, and IP/MAC formatting |
| DNS resolver | `dns.c` / `dns.h` | A-record query over UDP to `10.0.2.3:53` (slirp DNS); dotted-quad shortcut bypasses the network; CNAME-chain following, name-compression handling, in-memory cache, also `dns_resolve_all` and `dns_reverse` (PTR) |
| HTTP/1.1 client | `http.c` / `http.h` | GET with redirect following (3xx, capped at 5), `Transfer-Encoding: chunked` decode, `Content-Length`, gzip/deflate inflate, a 1-slot keep-alive cache (30 s idle), Range requests, an ~8 s total wall-clock budget, and both `http://` and `https://` |
| DHCP client | `dhcp.c` / `dhcp.h` | Full DISCOVER → OFFER → REQUEST → ACK over UDP. Works without `SYS_BIND` by using an ephemeral port + the BOOTP broadcast flag. **Obtains** a lease only — applying it to the live kernel config would need a `SYS_NET_SET_CONFIG` hook that doesn't exist yet (documented honestly in the source) |
| TLS connection helper | `tlsconn.c` / `tlsconn.h` | Wraps DNS resolve + TCP connect + TLS handshake into one `netconn` object that the HTTP client uses for `https://` |

### Userspace network tools

Each tool runs at boot in a self-test mode (no arguments) printing
`PASS`/`SKIP`/`FAIL`, and accepts live arguments interactively. They are
freestanding ELF binaries linked with `crt0`.

| Tool | Path | What it is |
|---|---|---|
| `ping` | `apps/ping` | ICMP echo: `ping HOST [count]`; bare `ping` pings the gateway as a self-test |
| `nc` | `apps/nc` | netcat-lite TCP client: `nc HOST PORT`, plus `-e "TEXT"` send-and-read forms |
| `wget` | `apps/wget` | HTTP/HTTPS downloader: `wget URL`, `-O FILE`, `-k`/`--insecure` to silence the CA-not-validated warning |
| `dhcpc` | `apps/dhcpc` | DHCP client: runs `dhcp_acquire()` and prints the lease |
| `nettest` | `apps/nettest` | Kernel-stack smoke test: reads `SYS_NET_INFO`, builds + sends a broadcast ARP request, polls for the reply |
| `nettool` / `netman` / `livenet` / `tlsprobe` | `apps/*` | network diagnostics, the GUI network manager, a live-net probe, and the TLS interop prober |

---

## The cryptography suite (`userspace/lib/crypto/`)

A complete, self-contained crypto library — pure computation, no libc, no
syscalls, no malloc, all caller-supplied fixed buffers and its own
`memset`/`memcpy`. It is built for ring 3 with the `-ffreestanding -nostdlib`
discipline used everywhere in the userspace (and the headers insist `objdump`
show **no** `fs:0x28` stack canary).

| Category | Files | Primitives |
|---|---|---|
| Hashes | `sha256.c`, `sha512.c` (SHA-384/512), `sha1.c`, `md5.c` | SHA-256, SHA-384, SHA-512, SHA-1, MD5 |
| MAC / KDF | `hmac.c`, `hkdf.c` | HMAC-SHA256/SHA1; HKDF (RFC 5869) over SHA-256 **and** SHA-384, plus a TLS 1.3 `HKDF-Expand-Label` (RFC 8446 §7.1) |
| Block cipher | `aes.c` | AES-128/256, single-block, CBC, and GCM (AEAD) |
| Stream AEAD | `chacha20poly1305.c` | ChaCha20-Poly1305 (RFC 8439), with constant-time tag verify on decrypt |
| ECDH | `x25519.c` | X25519 / Curve25519 (RFC 7748), constant-time Montgomery ladder |
| EC | `p256.c` | NIST P-256 (secp256r1) ECDH + ECDSA **verification** (FIPS 186-4) |
| Public-key | `rsa.c`, `bignum.c` | RSA PKCS#1 v1.5 encrypt + signature **verify** (public-key half only, Montgomery `mod_exp`); arbitrary-precision integers |
| Encoding | `base64.c` | Base64 encode/decode |

A few honest notes the source makes itself: RSA here is the *public-key* half
only (no private key / CRT / blinding), because a TLS *client* only ever needs
to encrypt to and verify the server. p256 implements ECDSA *verify*, not sign.
X25519 and the AEAD decrypts are written constant-time; the legacy CBC path is
not (a documented Lucky-13-class caveat).

### Known-answer self-tests (`cryptotest.c`)

Every primitive is checked against published test vectors at boot, and each test
returns its own failure id so a broken primitive is identifiable:

- SHA-256 / SHA-1 / MD5 of `"abc"`
- HMAC-SHA256 (RFC 4231 case 1) and HMAC-SHA1 (RFC 2202 case 1)
- AES-128 and AES-256 single-block encrypt (FIPS-197), with an AES-128 decrypt
  round-trip
- AES-128-CBC against NIST SP 800-38A F.2.1
- AES-128-GCM against the NIST GCM Case 4 vector — encrypt (ct + tag), decrypt
  with tag verify, **and** a negative test where a corrupted tag *must* fail

ChaCha20-Poly1305 (RFC 8439 vectors), X25519 (RFC 7748), p256 (NIST CAVP), RSA,
and HKDF (RFC 5869) each ship their own `*_selftest()` too. The boot smoke test
gates on the combined marker
`crypto/HTTPS KATs verified (SHA/AES/HMAC/RSA/X.509/TLS-PRF)`.

---

## TLS & HTTPS (`userspace/lib/tls/`)

The TLS layer is a deliberately-minimal **TLS 1.2 client** (RFC 5246) that runs
over an already-connected TCP fd: the caller does the socket connect and hands
`tls_client_connect()` the fd; the layer drives the handshake and then
encrypts/decrypts application data on `tls_write()` / `tls_read()`. The whole
connection state lives in one (~40 KB) `tls_conn_t` struct — no allocator
required.

**Cipher suites** advertised (and any negotiated end-to-end), in preference
order:

| Suite | Notes |
|---|---|
| `0xC02B` ECDHE_ECDSA_WITH_AES_128_GCM_SHA256 | ECDHE + AEAD |
| `0xC02F` ECDHE_RSA_WITH_AES_128_GCM_SHA256 | broadest compatibility |
| `0xC030` ECDHE_RSA_WITH_AES_256_GCM_SHA384 | SHA-384 PRF |
| `0xCCA8` ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256 | |
| `0xCCA9` ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256 | |
| `0x002F` TLS_RSA_WITH_AES_128_CBC_SHA | legacy last-resort fallback only |

ECDHE key exchange supports the **x25519** (`0x001D`) and **secp256r1**
(`0x0017`) named groups. The ServerKeyExchange signature is verified
(RSA-PKCS1 or ECDSA-P256) against the leaf certificate's public key over
`client_random || server_random || ECDHE params`. The PRF / Finished hash is
selected per suite (P_SHA256 vs P_SHA384). AEAD record protection follows
RFC 5288 (AES-GCM, explicit 8-byte nonce) and RFC 7905 (ChaCha20-Poly1305).
`tls_selftest()` checks the TLS 1.2 PRF against the published vector offline.

> Scope note: the handshake state machine in `tls.c` is **TLS 1.2 only** — there
> is no 1.3 handshake. The crypto library *does* contain the TLS 1.3 key-schedule
> building block (`HKDF-Expand-Label`), but the client does not negotiate 1.3.
> There is no session resumption, renegotiation, or client-certificate support.

### Certificate chain validation (`x509.c`, `x509_verify.c`, `asn1.c`)

Extracting the server's public key (`x509.c` + the bounds-checked `asn1.c` DER
reader) gives *encryption* but not *authentication*. `x509_verify_chain()`
closes that gap with the classic four checks against the embedded CA bundle
(`ca_bundle.c` / `ca_roots_data.h`):

1. **Chain linkage + signatures** — each cert is verified against the next
   cert's key, and issuer-DN must equal the next subject-DN.
2. **Trust anchor** — the top of the chain must be issued by a root in the CA
   store.
3. **Validity window** — `notBefore <= now <= notAfter` for every cert.
4. **Hostname** — the leaf's subjectAltName dNSNames (or CN fallback) must match
   the requested host, with single-label `*.` wildcard support.

The source is candid about the limits: **no revocation** (no CRL, no OCSP); only
RSA-SHA256/384/512 and ECDSA-P256-SHA256 signature algorithms are accepted (SHA-1
and RSA-PSS are *rejected*, not silently trusted); and `basicConstraints`
CA:TRUE / pathLen / key-usage are not enforced. If the verifier is **not**
linked, the TLS handshake still completes but the connection is flagged
**encrypted-but-unauthenticated** — `tls_cert_trusted()` returns 0, and the
HTTP layer surfaces `nc->trusted == 0` so a UI "lock" indicator must be gated on
it (this is exactly what `wget -k` acknowledges). The same caveat is why
`browser2` can show pages over HTTPS while being honest about trust.

How it all stacks up for an `https://` GET: `https_get()` → `netconn` (DNS
resolve + TCP connect + `tls_client_connect`) → TLS records → the same HTTP/1.1
response parsing used for plain HTTP.

---

## See also

- [Home](Home.md)
- [Architecture](Architecture.md)
- [Kernel Internals](Kernel-Internals.md)
- [Drivers & I/O](Drivers-and-IO.md)
- [Browser & Web Engine](Browser-and-Web-Engine.md)
- [Desktop & Apps](Desktop-and-Apps.md)
- [Building & Running](Building-and-Running.md)
- [Roadmap](../ROADMAP.md)
