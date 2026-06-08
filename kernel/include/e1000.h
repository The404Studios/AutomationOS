/*
 * kernel/include/e1000.h -- Intel 82540EM (QEMU e1000) NIC driver API
 * =====================================================================
 *
 * This header is the canonical interface for kernel/drivers/net/e1000.c.
 * It is included by kernel/drivers/net/e1000.c itself, and may be included
 * by any kernel code that needs to call the low-level NIC functions directly
 * without pulling in the full net stack header (kernel/include/net.h).
 *
 * Note: kernel/include/net.h also declares these functions (in its
 * "Low-level NIC driver API" section) because the net stack calls them
 * from kernel/net/net.c.  Both sets of declarations are identical; there
 * is no conflict.  The integrator does NOT need to change either header.
 *
 * -----------------------------------------------------------------------
 * Hardware target
 * -----------------------------------------------------------------------
 *  QEMU flag:  -device e1000,netdev=n0 -netdev user,id=n0
 *  PCI id:     vendor 0x8086, device 0x100E (Intel 82540EM)
 *  MMIO:       BAR0, accessed via volatile 32-bit reads/writes
 *  DMA:        bus-master; descriptor rings + per-descriptor buffers are
 *              allocated with pmm_alloc_page() which returns a physical page
 *              that is identity-mapped (virt == phys) in this kernel.
 *
 * -----------------------------------------------------------------------
 * Phys-address / DMA contract
 * -----------------------------------------------------------------------
 *  pmm_alloc_page() returns a void* that is simultaneously:
 *    - the CPU virtual address  (for memcpy, struct overlays)
 *    - the physical / DMA address (what the e1000 DMA engine sees)
 *  This works because the kernel maps all physical RAM 1:1 (identity map).
 *  No IOMMU translation is needed.  The driver casts the pointer to
 *  uint64_t and writes it straight into RDBAL/RDBAH / TDBAL/TDBAH and
 *  into each descriptor's addr field.
 *
 * -----------------------------------------------------------------------
 * RX mode: poll-driven (no IRQs)
 * -----------------------------------------------------------------------
 *  The driver leaves the e1000's interrupt mask fully cleared (IMC=0xFFFF).
 *  The net stack (kernel/net/net.c) drives receive by calling net_recv(),
 *  which calls e1000_rx_poll() directly.  The integrator must pump
 *  net_recv() (or e1000_rx_poll()) in a loop -- either in the kernel main
 *  loop, in a kernel thread, or from a scheduler tick -- to drain received
 *  frames.  Frames are NOT delivered automatically via an IRQ path.
 *
 * -----------------------------------------------------------------------
 * TX/RX hook names (what kernel/net/net.c calls)
 * -----------------------------------------------------------------------
 *  TX:  int e1000_tx(const void* frame, uint16_t len)
 *         Called by net_send().  Copies the frame into a TX descriptor
 *         buffer, bumps TDT, and waits for the DD writeback bit.
 *
 *  RX:  int e1000_rx_poll(void* buf, uint16_t buf_len)
 *         Called by net_recv().  Returns >0 with one frame copied into
 *         buf, 0 when the ring is empty, negative on error.
 *         The caller (net_recv) feeds the raw frame into the stack demux.
 *
 * -----------------------------------------------------------------------
 * Init order
 * -----------------------------------------------------------------------
 *  1. pci_init()        -- enumerate PCI bus
 *  2. net_init()        -- calls e1000_init() internally, then configures
 *                          the static IP / ARP state
 *  (do NOT call e1000_init() again; net_init() is the entry point)
 *
 *  If you need to call e1000_init() standalone (e.g. for testing without
 *  the full stack) it is safe: it is idempotent and returns 0 on success,
 *  negative if no NIC was found (never hangs).
 */

#ifndef E1000_H
#define E1000_H

#include "types.h"

/* Number of MAC address bytes (matches ETH_ALEN in net.h). */
#ifndef ETH_ALEN
#define ETH_ALEN 6
#endif

/* ------------------------------------------------------------------ */
/* Driver lifecycle                                                    */
/* ------------------------------------------------------------------ */

/*
 * e1000_init() -- detect, reset, and bring up the Intel e1000 NIC.
 *
 * Performs:
 *   - Walks a table of candidate Intel device IDs (vendor 0x8086) to locate
 *     the NIC: the classic/verified QEMU parts (82540EM 0x100E, 82545 0x100F,
 *     82574L 0x10D3) plus real-hardware e1000e-class parts such as the
 *     ThinkPad T410's 82577LM (0x10EA), 82578/82579, and i217/i218.  Falls
 *     back to a generic PCI class 0x02:0x00 (ethernet) scan if none match.
 *     NOTE: the e1000e-class parts are detected and brought up best-effort;
 *     their PHY/MAC quirks may need e1000e-specific follow-up.  The QEMU
 *     82540/82574L path is the verified one.
 *   - pci_enable_memory_space() + pci_enable_bus_master() on BAR0.
 *   - Full CTRL_RST device reset with post-reset CTRL_SLU link-up.
 *   - Multicast table array cleared (128 dwords zeroed).
 *   - MAC read from RAL0/RAH0 (QEMU pre-loads from the EEPROM image).
 *   - RX ring: NUM_RX_DESC=32 descriptors + 2048-byte buffers, RCTL enabled.
 *   - TX ring: NUM_TX_DESC=8  descriptors + 2048-byte buffers, TCTL enabled.
 *   - IRQ mask fully cleared (poll-mode; no IRQ handler registered).
 *
 * Returns:
 *   0   -- NIC found, configured, and ready.
 *  -1   -- NIC not present or memory allocation failed.
 *          Logs a message via kprintf() and is safe to ignore; all
 *          subsequent calls to e1000_tx/e1000_rx_poll will return -1.
 *
 * Thread safety: call once from kernel init; not re-entrant.
 */
int  e1000_init(void);

/*
 * e1000_present() -- true once e1000_init() succeeded.
 *
 * Cheap predicate; no side effects.
 */
bool e1000_present(void);

/* ------------------------------------------------------------------ */
/* MAC address                                                         */
/* ------------------------------------------------------------------ */

/*
 * e1000_get_mac() -- copy the 6-byte burned-in MAC address into out[6].
 *
 * The MAC is read from the Receive Address Low/High registers (RAL0/RAH0)
 * during e1000_init(), so this is a simple memory copy after init.
 *
 * Returns 0 on success, -1 if e1000_init() has not yet succeeded.
 *
 * Equivalent public wrapper: net_get_mac() in kernel/net/net.c.
 * Direct callers (e.g. an e1000 standalone test) may call this instead.
 */
int  e1000_get_mac(uint8_t out[ETH_ALEN]);

/* ------------------------------------------------------------------ */
/* Transmit                                                            */
/* ------------------------------------------------------------------ */

/*
 * e1000_tx() -- transmit one raw Ethernet frame.
 *
 * `frame` must point to a fully-built Ethernet II frame (dest MAC, src MAC,
 * EtherType, payload -- no FCS, the hardware appends it).  `len` is the
 * frame length in bytes (excluding FCS); frames shorter than 60 bytes are
 * zero-padded to meet the Ethernet minimum (TCTL_PSP also pads).
 *
 * The call spins briefly waiting for a free TX descriptor (up to ~100k
 * iterations) and for the descriptor-done writeback bit after kicking TDT.
 * This is appropriate for single-threaded poll-mode; in a preemptive
 * environment the integrator should add a scheduler yield in the spin loops.
 *
 * Returns: bytes queued (== original `len`) on success, -1 on error.
 *
 * Called by: net_send() in kernel/net/net.c (the TX hook name).
 */
int  e1000_tx(const void* frame, uint16_t len);

/* ------------------------------------------------------------------ */
/* Receive                                                             */
/* ------------------------------------------------------------------ */

/*
 * e1000_rx_poll() -- drain one received frame from the RX ring.
 *
 * Checks the next descriptor at e1000.rx_cur.  If the DD (Descriptor Done)
 * status bit is set the frame is copied into `buf` (up to `buf_len` bytes),
 * the descriptor is recycled (status cleared, RDT advanced), and the
 * software head advances.
 *
 * Returns:
 *  >0  -- frame length in bytes; one frame has been written to `buf`.
 *   0  -- ring empty (no frame pending); caller should back off or yield.
 *  -1  -- driver not initialized or bad arguments.
 *
 * Called by: net_recv() in kernel/net/net.c (the RX hook name).
 *
 * The integrator MUST pump net_recv() (or this function directly) in a
 * loop; there is no IRQ path.  Typical patterns:
 *
 *   Kernel main loop (simplest):
 *     while (1) { net_recv(frame_buf, sizeof(frame_buf)); schedule(); }
 *
 *   Dedicated kernel thread:
 *     void net_poll_thread(void) {
 *         while (1) { net_recv(frame_buf, sizeof(frame_buf)); yield(); }
 *     }
 */
int  e1000_rx_poll(void* buf, uint16_t buf_len);

/*
 * e1000_transmit_batch() -- send multiple frames with a single doorbell.
 *
 * `frames` is an array of pointers to raw Ethernet frames, `lengths` holds
 * the corresponding byte counts, and `count` is the number of frames.
 * A single MMIO TDT write kicks the entire batch, amortising the PCIe
 * round-trip.  Returns the number of frames actually queued (may be less
 * than `count` if the TX ring is full).
 */
int  e1000_transmit_batch(const void* const* frames, const uint16_t* lengths,
                          uint32_t count);

#endif /* E1000_H */
