# Drivers & I/O

AutomationOS drives hardware through a layered stack: platform drivers sit directly on I/O ports and MMIO, a block/VFS layer abstracts storage, and a TCP/IP stack plus userspace TLS handles networking. Everything targets x86_64 under QEMU (`-machine q35`, `-device e1000`) and boots natively on a ThinkPad T410. Real-hardware experience shaped several hard constraints documented below.

---

## Frozen-tick iteration-cap discipline

Early boot runs with interrupts off (`sti()` is called last in `kernel.c`). The PIT divisor is programmed, so `timer_get_frequency()` returns a valid frequency, but `IRQ0` is masked: `timer_get_ticks()` never advances. Any loop that waits on a hardware bit using only a wall-clock timeout will spin forever on real hardware.

**The rule**: every hardware spin in the early-boot path carries a dual bound:

1. An **iteration cap** (`AHCI_SPIN_ITERS_PER_MS * timeout_ms`, capped at `AHCI_SPIN_ITERS_MAX`) — always fires, even when ticks are frozen.
2. A **wall-clock bound** (`timer_get_ticks()` delta) — only effective after `sti()`, provides accurate timeouts during runtime.

This pattern appears in three drivers where a hang was confirmed or anticipated on the T410:

| Driver | Constant | Source |
|---|---|---|
| AHCI | `AHCI_SPIN_ITERS_PER_MS 200000`, `AHCI_SPIN_ITERS_MAX = 2000 * per_ms` | `kernel/drivers/storage/ahci.c` |
| PS/2 | `PS2_SPIN_PER_MS 100000`, `PS2_TIMEOUT_MS 200` | `kernel/drivers/ps2.c` |
| RTC | `RTC_UIP_SPIN_MAX 10000` | `kernel/drivers/rtc.c` |

---

## Platform drivers

### Framebuffer (`kernel/drivers/framebuffer.c`)

Drives a 32-bit VESA/VBE linear framebuffer passed by Multiboot. The driver records `phys_base`, `width`, `height`, `pitch`, and derives `bpp` from `pitch / width`.

Key routines:

| Function | Description |
|---|---|
| `framebuffer_init(fb_addr, w, h, pitch)` | Store geometry, mark initialized |
| `framebuffer_clear(color)` | Fill using 64-bit stores (2 px/write) via `fill_row_u64` |
| `framebuffer_draw_rect(x, y, w, h, color)` | Clipped rect fill, one `fill_row_u64` per scanline |
| `framebuffer_putchar(c, x, y, color)` | Render from embedded 8×8 bitmap font (ASCII 32–127) |
| `framebuffer_puts_scaled(s, x, y, color, scale)` | Scaled text used for the boot splash |
| `framebuffer_get_info(out)` | Export geometry to userspace (`fb_info_t`) |

`fill_row_u64` packs two `uint32_t` pixels into one 64-bit store, halving store instructions versus per-pixel calls. `pitch` is respected, so framebuffers where `pitch != width * 4` are handled correctly.

### PS/2 keyboard and mouse (`kernel/drivers/ps2.c`)

Drives the 8042 PS/2 controller at ports `0x60`/`0x64`. Every status-register wait goes through `ps2_wait_status(mask, want)`, which applies the frozen-tick dual-bound discipline above (`PS2_SPIN_PER_MS * PS2_TIMEOUT_MS` iterations, plus an optional wall-clock check once IRQ0 is live). The only data-read primitive is `ps2_read_data_timeout(bool* ok)`, which sets `*ok = false` and returns `PS2_NO_DATA (0xFF)` on timeout.

**Keyboard**: scancode set 1, US QWERTY. Two lookup tables (`scancode_to_ascii`, `scancode_to_ascii_shift`). Modifier state (`shift_pressed`, `ctrl_pressed`, `alt_pressed`, `caps_lock`) is tracked. Scancodes are also reported to the input subsystem as Linux-style keycodes via `input_report_key()`. A 256-byte ring buffer (`keyboard_buffer`) serves `ps2_getchar()` and `ps2_getchar_blocking()`. Buffer access uses `save_flags_cli()` / `restore_flags()` (not a blind `sti()`) to avoid re-enabling interrupts mid-handler.

**Mouse**: standard 3-byte PS/2 mouse protocol. A byte-0 sync guard (bit 3 must be set in any valid packet header) prevents a permanently offset 3-byte boundary if the stream is entered mid-packet. Relative motion and button events are reported via `input_report_rel()` / `input_report_key()`. Mouse bring-up (`ps2_mouse_init`) returns `-1` and skips registration gracefully if the second PS/2 port test times out — an absent or unresponsive Synaptics/TrackPoint on the T410 does not block boot.

IRQ handlers: `ps2_irq_handler` (IRQ1, keyboard), `ps2_mouse_irq_wrapper` → `ps2_mouse_irq_handler` (IRQ12).

### PCI enumeration (`kernel/drivers/pci.c`)

Uses PCI Configuration Mechanism #1: address register `0xCF8`, data window `0xCFC`. `pci_init()` brute-force scans all 256 buses × 32 devices × 8 functions, recording up to `PCI_MAX_DEVICES (64)` entries in a static table. Drivers locate their controller via `pci_find_class(class, subclass, ...)` or `pci_find_device(vendor, device, ...)`, decode BARs with `pci_get_bar()`, and set COMMAND-register bits with `pci_enable_bus_master()` / `pci_enable_memory_space()`. 64-bit BAR pairs (type `0x04`) are decoded correctly by combining the low and high dword.

### RTC (`kernel/drivers/rtc.c`)

CMOS RTC via I/O ports `0x70` (index) and `0x71` (data). Handles BCD vs binary mode and 12/24-hour mode from Status Register B. The Update-In-Progress (UIP) spin in `rtc_read()` is capped at `RTC_UIP_SPIN_MAX 10000` iterations (covers the ≤244 µs UIP assertion with margin); an unresponsive RTC never hangs the kernel. Exports: `rtc_init()`, `rtc_read()` (fills `rtc_time_t`), `rtc_unix_time()` (returns `int64_t` epoch seconds).

### RNG (`kernel/drivers/rng.c`)

Two-tier strategy:

1. **RDRAND** — detected via CPUID leaf 1 ECX[30], used with a carry-flag retry loop (up to `RNG_RDRAND_RETRIES` per word).
2. **Fallback: xorshift128+** — seeded from two `rdtsc` samples (before/after a short spin), CMOS RTC seconds/minutes (ports `0x70`/`0x71`), and a compile-time constant, so two boots on the same TSC tick are distinguishable. If RDRAND fails all retries, the PRNG result is mixed in to avoid returning zero.

### Programmable Interval Timer (`kernel/drivers/pit.c`)

Channel 0 in rate-generator mode. `pit_init(frequency)` computes the divisor with nearest-integer rounding (`(PIT_BASE_FREQUENCY + frequency/2) / frequency`) and derives `timer_frequency` from the actual divisor, keeping `timer_get_ticks_ms()` and `timer_sleep()` drift-free even when the requested frequency does not divide `1193182 Hz` evenly. `timer_handler()` (IRQ0) increments `timer_ticks` and calls `scheduler_tick()` for SMP load balancing. `timer_sleep()` checks `RFLAGS.IF` before issuing `hlt` to avoid hanging with interrupts off.

### GPU / display (`kernel/drivers/gpu/nvidia.c`)

**Detection-only.** Targets the T410's NVIDIA NVS 3100M (PCI `0x10DE:0x0A6C`, GT218/Tesla). The driver:

1. Scans the PCI bus for vendor `0x10DE`, class `0x03` (display controller).
2. Maps BAR0 (MMIO register aperture, ≤16 MiB on Tesla) and records BAR1 (VRAM aperture).
3. Performs one read-only access: `PMC_BOOT_0` at MMIO offset `0x000000`, which encodes the chip ID.
4. **Adopts** the firmware-configured VESA/VBIOS linear framebuffer. It does not program the display — native mode-setting on Tesla (CRTC + PLL + DCB) requires on-hardware iteration that would blank the panel if done blind.

On QEMU (no `0x10DE` device present) `nvidia_init()` is a clean no-op. Native mode-setting scaffolding (VBIOS ROM read, DCB parse, CRTC/PLL programming) is present in the source but explicitly not called (marked `TODO`).

---

## Storage

### AHCI (`kernel/drivers/storage/ahci.c`, `ahci_block.c`, `ahci_dma_pool.c`)

Brings up an AHCI HBA (PCI class `0x01`, subclass `0x06`, prog-if `0x01`) via BAR5 (ABAR). All DMA structures (command list, received FIS, command tables, PRDT data buffers) are allocated from the PMM (physical memory manager) because the identity map makes `pmm_alloc_page()` pointers valid as DMA addresses. The kernel heap (`kmalloc`) lives in the high-half virtual window and cannot be used for DMA.

The driver implements:
- AHCI HBA reset and port detection (first implemented port with a connected SATA device)
- ATA `IDENTIFY DEVICE`
- ATA `READ DMA EXT` / `WRITE DMA EXT` (512-byte sectors)

Every register-bit wait goes through `ahci_wait_until(reg, mask, value, timeout_ms)`, which applies the dual-bound iteration-cap discipline. The T410-class hang (boot reached AHCI probe then froze) was caused by a pure wall-clock timeout that never fired with `IRQ0` masked; `AHCI_SPIN_ITERS_PER_MS 200000` eliminates this.

Memory barriers (`mfence`/`sfence`/`lfence`) separate descriptor writes from HBA doorbell kicks.

### Block device layer (`kernel/drivers/storage/block.c`)

A thin registration table (up to `MAX_BLOCK_DEVICES 16`) with a common `block_read` / `block_write` interface. AHCI registers itself here via `ahci_register_block_device()`. ext2 and FAT32 call `block_read()` through this layer.

### DiskFS (`kernel/fs/diskfs.c`)

The durable persistence layer on top of AHCI. Without it the OS is entirely RAM-backed and loses state at power-off.

**Layer 1 — superblock** (version 2): one 512-byte record at LBA 64 carrying a `DISKFS_MAGIC (0x4B534644)`, format version, a monotonically-increasing boot counter, a checksum, and the filesystem region layout. On boot: read → validate → bump counter → write → read-back verify. A counter progression `2,3,4,...` across reboots proves durable persistence (`scripts/smoke_persist.sh` is a 2-boot harness).

**Layer 2 — simple filesystem**:

| Region | LBAs | Size |
|---|---|---|
| Superblock | 64 | 1 sector |
| Allocation bitmap | 96 | 1 sector (4096 tracked blocks) |
| Inode table (64 inodes) | 128–143 | 16 sectors, 128 B/inode |
| Data region (4 KiB blocks) | 4096+ | |

Inodes are 128-byte records with `name[56]`, 12 direct block pointers, and 1 single-indirect pointer (1024 × 4 KiB = ~4.05 MiB max file size). There are no directory blocks; the root directory is the set of all used, named inodes. All metadata is written through to disk on every mutation (no write-back cache, no journal).

Public API: `diskfs_create`, `diskfs_open`, `diskfs_read`, `diskfs_write`, `diskfs_size`, `diskfs_unlink`, `diskfs_list`. Gate: if `ahci_present()` returns false, `diskfs_init()` is a clean no-op.

---

## Filesystems

### VFS (`kernel/fs/vfs.c`)

The hub of all file access. Maintains a mount table (`vfs_mount_t` list), a per-process file-descriptor table (via `process_get_current()`), an inode cache, and a dentry cache. Provides a unified `open`/`read`/`write`/`close`/`lseek` interface dispatched through per-filesystem `vfs_file_ops_t` and `vfs_inode_ops_t` vtables. Device nodes are handled via `vfs_devnode_t` (magic checked on open).

The VFS also hosts the built-in **ramfs**, which serves as the root (`/`) filesystem. ramfs inodes are heap-allocated, grow on write, and disappear at power-off. It is the default backing for all paths not covered by a disk-backed mount.

### initrd (`kernel/init/initrd.c`)

Parses a POSIX ustar TAR image passed by the bootloader at a physical address. `initrd_init(addr, size)` records the mapping; `initrd_mount()` walks the TAR headers and registers each file into the VFS. `initrd_get_file(path, &size)` returns a direct pointer into the in-memory image — zero-copy. The ELF loader and `exec` syscall use this to load `/init` and userspace binaries.

### ext2 (`kernel/fs/ext2.c`)

Read-only ext2 implementation registered with the filesystem type registry (`kernel/fs/fs_registry.c`). Reads blocks via `block_read()` through the block device layer. Write operations (`create`, `mkdir`, `unlink`, `rmdir`) are stubbed (`NULL` in the inode ops vtable). Sector-to-block mapping uses `block_size / 512` sectors per block.

### FAT32 (`kernel/fs/fat32.c`)

Read-only FAT32 implementation, same structure as ext2: registered with the fs registry, reads through the block layer, writes stubbed.

### ELF loader and exec (`kernel/fs/elf_loader.c`, `kernel/fs/exec.c`)

`elf_validate_header()` checks the ELF magic, confirms 64-bit class (`ELFCLASS64`) and little-endian encoding (`ELFDATA2LSB`). `exec.c` is the bridge between the ELF loader and process management: it allocates a user stack (8 MiB, top at `0x00007FFFFFFFE000`), loads PT_LOAD segments, sets up a VMA (`kernel/include/vma.h`), and performs the ring-3 transition via `jump_to_usermode(entry, stack)` (an `IRET` sequence with `CS=0x1B`, `SS=0x23`). The `EXEC_QUIET` build flag silences per-spawn log output to avoid serial I/O stalls under QEMU.

---

## Networking

### e1000 NIC driver (`kernel/drivers/net/e1000.c`)

Targets QEMU's emulated Intel 82540EM (`-device e1000`, PCI `0x8086:0x100E`). Also detects a range of e1000e-class parts including the **ThinkPad T410's Intel 82577LM** (PCI `0x8086:0x10EA`).

**T410 / PCH runtime gate**: The 82577LM is a PCH-integrated LAN part. Its MMIO/MDIO/SW-FW-semaphore bring-up path was confirmed to stall the bus on the real T410 in a way that iteration caps cannot unwedge — a bus stall, not a software spin. The driver detects PCH parts via `e1000_is_pch(device_id)` and returns `-1` immediately, before touching any MMIO:

```c
if (e1000.is_pch) {
    kprintf("[E1000] PCH NIC 0x%04x detected -- bring-up DISABLED ...\n");
    return -1;
}
```

PCI config-space enumeration (safe) still identifies the device; the gate fires before `pci_get_bar()` or any register access. The T410 therefore boots to the desktop with the NIC detected but link-down. The PCH bring-up code (SWFLAG acquisition, MDIC PHY power-up) is complete and intact in the source; the gate can be removed once validated on real hardware.

**Verified QEMU path** (82540EM / 82545 / 82574L):
- Allocates RX ring (32 descriptors, 2 KiB buffers) and TX ring (8 descriptors) from the PMM.
- MAC read from Receive Address registers (RAL0/RAH0), seeded by QEMU from the EEPROM image.
- RCTL/TCTL programmed to enable receiver and transmitter.
- Poll-mode RX/TX — `net_recv()` must be called to drain the NIC.

### Kernel network stack (`kernel/net/`)

| File | Responsibility |
|---|---|
| `net.c` | Ethernet framing, ARP (request + reply + 16-entry cache), IPv4 RX/TX, ICMP echo reply, IPv4 fragment reassembly, RTL8139 fallback |
| `route.c` | IPv4 routing table, up to 16 entries, longest-prefix match, `route_add`/`route_lookup`/`route_print` |
| `socket.c` | BSD-style socket table (heap-allocated, up to `SOCK_MAX` entries), `sock_poll()` demux, shared IPv4 TX, TCP timers |
| `udp.c` | UDP TX (with IPv4 pseudo-header checksum) and inbound demux into socket receive rings |
| `tcp.c` | RFC 793 active-open TCP: 3-way handshake, segmentation (`TCP_MSS`), multi-retransmit with exponential back-off, out-of-order side-table (`tcp_ooo[]`), dynamic receive window, FIN/RST handling |
| `netsyscall.c` | Syscall wiring for `SYS_SOCKET`, `SYS_CONNECT`, `SYS_SEND`, `SYS_RECV`, `SYS_SENDTO`, `SYS_RECVFROM`, `SYS_CLOSE_SK`, `SYS_SOCK_POLL`, `SYS_BIND` |

The stack is poll-mode and single-threaded. In QEMU user-net (slirp) the guest address is `10.0.2.15` and the gateway is `10.0.2.2`.

### Userspace networking (`userspace/lib/net/`, `userspace/lib/tls/`)

A freestanding (no libc, no stdio, no malloc) networking library compiled for ring 3:

| Module | File | Description |
|---|---|---|
| DNS resolver | `userspace/lib/net/dns.c` | A-record query over UDP to `10.0.2.3:53` (QEMU slirp DNS); dotted-quad shortcut bypasses network |
| HTTP/1.1 client | `userspace/lib/net/http.c` | GET with redirect following (up to cap), chunked transfer decoding, Content-Length, gzip/deflate inflate, 1-slot keep-alive cache, both `http://` and `https://` |
| TLS 1.2 client | `userspace/lib/tls/tls.c` | Handshake over an already-connected TCP fd. Advertises ECDHE+AEAD suites: `ECDHE_ECDSA_WITH_AES_128_GCM_SHA256`, `ECDHE_RSA_WITH_AES_128_GCM_SHA256`, `ECDHE_RSA_WITH_AES_256_GCM_SHA384`, `ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256`, `ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256`, plus legacy `TLS_RSA_WITH_AES_128_CBC_SHA`. x25519 and secp256r1 key exchange. ServerKeyExchange signature is verified. Certificate chain validation via `x509_verify_chain()` when linked; without it the connection is flagged encrypted-but-unauthenticated. No session resumption. |
| X.509 / ASN.1 | `userspace/lib/tls/x509.c`, `asn1.c` | DER parsing, SubjectPublicKeyInfo decode, basic constraints |
| TLS connection helper | `userspace/lib/tls/tlsconn.c` | Wraps DNS resolve + TCP connect + TLS handshake into a single `netconn` object used by the HTTP client |
| HKDF | `userspace/lib/crypto/hkdf.c` | Used internally by the TLS 1.2 PRF machinery |

Userspace apps using this stack: `userspace/apps/tlsprobe`, `userspace/apps/netman`, `userspace/apps/livenet`, `userspace/apps/nettool`.

---

## See also

- [Home](Home.md)
- [Architecture](Architecture.md)
- [Kernel Internals](Kernel-Internals.md)
- [Desktop & Apps](Desktop-and-Apps.md)
- [Building & Running](Building-and-Running.md)
- [Roadmap](../ROADMAP.md)
