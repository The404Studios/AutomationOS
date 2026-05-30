/*
 * kernel/include/rtl8139.h -- Realtek RTL8139 (RTL8139C/C+) NIC driver API
 * ========================================================================
 *
 * This header is the canonical interface for kernel/drivers/net/rtl8139.c.
 * It MIRRORS the shape of kernel/include/e1000.h on purpose, so the network
 * layer can drive EITHER NIC through an identical 5-call contract:
 *
 *      rtl8139_init / _present / _get_mac / _tx / _rx_poll
 *      e1000_init   / _present / _get_mac / _tx / _rx_poll
 *
 * -----------------------------------------------------------------------
 * Hardware target
 * -----------------------------------------------------------------------
 *  QEMU flag:  -device rtl8139,netdev=n0 -netdev user,id=n0
 *  PCI id:     vendor 0x10EC, device 0x8139 (Realtek RTL8139)
 *  Also found on countless real legacy PCI/CardBus Fast-Ethernet cards.
 *
 *  BAR layout (RTL8139 spec):
 *    BAR0 = IOAR  -- I/O-port register window  (this driver's PRIMARY path)
 *    BAR1 = MEMAR -- MMIO register window (mirror of the same registers)
 *  The classic RTL8139 bring-up is done over PORT I/O, and this kernel
 *  already provides inb/outb/inw/outw/inl/outl (kernel/include/x86_64.h),
 *  exactly as the UHCI driver uses for its I/O-BAR device.  We therefore
 *  use BAR0 (port I/O) by default and only fall back to BAR1 (MMIO) if
 *  BAR0 is not an I/O-space BAR.  See "BAR/IO approach" in rtl8139.c.
 *
 * -----------------------------------------------------------------------
 * Phys-address / DMA contract  (identical to e1000.c)
 * -----------------------------------------------------------------------
 *  pmm_alloc_page() / pmm_alloc_pages() return a void* that is BOTH the CPU
 *  virtual address and the physical / DMA address, because the kernel maps
 *  all physical RAM 1:1 (identity map).  The RTL8139 is a 32-bit bus master:
 *  the RX ring base (RBSTART) and the 4 TX buffer addresses (TSAD0..3) are
 *  32-bit physical addresses written straight from those pointers.  The
 *  driver verifies every DMA buffer lives below 4 GiB (the chip cannot
 *  address higher) and is .bss-safe: NO large static arrays -- every DMA
 *  region comes from the PMM, exactly like e1000.c.
 *
 * -----------------------------------------------------------------------
 * RX mode: poll-driven (no IRQs), same as e1000.c
 * -----------------------------------------------------------------------
 *  The driver masks all NIC interrupts (IMR=0) and drives receive purely by
 *  polling the RX ring (CAPR/CBR cursor + the per-packet 4-byte RX header).
 *  The integrator pumps rtl8139_rx_poll() (via net_recv) in a loop.
 *
 * -----------------------------------------------------------------------
 * INTEGRATOR NOTE -- wiring this as a fallback NIC
 * -----------------------------------------------------------------------
 *  Wiring this driver into the net layer is the INTEGRATOR'S job and is NOT
 *  done here (this file does not touch kernel/net/net.c or e1000.c).  The
 *  intended pattern in kernel/net/net.c is:
 *
 *      if (e1000_init() == 0) {
 *          // route net_send -> e1000_tx, net_recv -> e1000_rx_poll
 *      } else if (rtl8139_init() == 0) {
 *          // route net_send -> rtl8139_tx, net_recv -> rtl8139_rx_poll
 *      }
 *      // net_get_mac() returns whichever NIC's _get_mac() succeeded
 *
 *  Because the call signatures match e1000's exactly, the net layer can hold
 *  a tiny function-pointer table (tx/rx_poll/get_mac) selected at init time.
 *
 * -----------------------------------------------------------------------
 * Init order
 * -----------------------------------------------------------------------
 *  1. pci_init()         -- enumerate PCI bus
 *  2. rtl8139_init()     -- detect 0x10EC:0x8139, map BAR, reset, bring up
 *
 *  rtl8139_init() is idempotent, returns 0 on success and negative if no
 *  RTL8139 is present (it never hangs), so it is safe to try after e1000.
 */

#ifndef RTL8139_H
#define RTL8139_H

#include "types.h"

/* Number of MAC address bytes (matches ETH_ALEN in net.h / e1000.h). */
#ifndef ETH_ALEN
#define ETH_ALEN 6
#endif

/* ------------------------------------------------------------------ */
/* Driver lifecycle                                                    */
/* ------------------------------------------------------------------ */

/*
 * rtl8139_init() -- detect, reset, and bring up the Realtek RTL8139 NIC.
 *
 * Performs the standard RTL8139 bring-up:
 *   - pci_find_device(0x10EC, 0x8139); fall back to a generic ethernet
 *     class scan (class 0x02 / subclass 0x00) only if the exact ID is
 *     absent.
 *   - Enable I/O (or memory) space + bus-master in the PCI COMMAND register.
 *   - Map BAR0 (I/O port window; preferred) or BAR1 (MMIO) for register access.
 *   - Power on the chip   (CONFIG1 = 0x00).
 *   - Software reset      (CMD = RST; spin until the RST bit self-clears).
 *   - Allocate the RX ring buffer (8 KiB + 16-byte header slack + 1500 wrap
 *     slack) from the PMM and program RBSTART.
 *   - RCR = accept broadcast + physical-match + multicast, WRAP set, max DMA
 *     burst, 8K(+16) ring size.
 *   - TCR = sane defaults (max DMA burst, normal IFG).
 *   - IMR = 0, ISR cleared (poll mode; no IRQs).
 *   - Enable receiver + transmitter (CMD = RE | TE).
 *   - Read the 6-byte MAC from IDR0..IDR5.
 *
 * Returns:
 *    0   -- RTL8139 found, configured, and ready.
 *   <0   -- NIC not present, BAR unusable, or DMA allocation failed.
 *           Logs via kprintf("[RTL8139] ...") and is safe to ignore; all
 *           subsequent rtl8139_tx / rtl8139_rx_poll calls then return <0.
 *
 * Idempotent (re-calling after success is a no-op returning 0).  Not
 * re-entrant; call once from kernel init.
 */
int  rtl8139_init(void);

/*
 * rtl8139_present() -- 1 once rtl8139_init() succeeded, else 0.
 * Cheap predicate; no side effects.
 */
int  rtl8139_present(void);

/* ------------------------------------------------------------------ */
/* MAC address                                                         */
/* ------------------------------------------------------------------ */

/*
 * rtl8139_get_mac() -- copy the 6-byte MAC (read from IDR0..5 during init)
 * into out[6].  Returns 0 on success, <0 if rtl8139_init() has not succeeded.
 */
int  rtl8139_get_mac(unsigned char out[6]);

/* ------------------------------------------------------------------ */
/* Transmit                                                            */
/* ------------------------------------------------------------------ */

/*
 * rtl8139_tx() -- transmit one raw Ethernet frame via the 4 TX descriptors.
 *
 * `frame` is a fully-built Ethernet II frame (dst, src, ethertype, payload --
 * no FCS; the chip appends it).  `len` is the frame length in bytes; runt
 * frames are zero-padded to the 60-byte Ethernet minimum.  The driver copies
 * the frame into the next of the 4 round-robin TX buffers, writes TSAD<n> with
 * its physical address and TSD<n> with the length (which kicks the DMA), then
 * spins briefly for the TOK (Transmit OK) completion bit.
 *
 * Returns: bytes queued (== original `len`) on success, <0 on error.
 */
int  rtl8139_tx(const void* frame, unsigned short len);

/* ------------------------------------------------------------------ */
/* Receive                                                             */
/* ------------------------------------------------------------------ */

/*
 * rtl8139_rx_poll() -- drain one received frame from the RX ring buffer.
 *
 * The RTL8139 RX path is a single linear ring buffer (RBSTART), NOT a
 * descriptor array.  Each received packet is prefixed by a 4-byte header
 * (16-bit receive status + 16-bit length, length INCLUDES the 4-byte CRC).
 * This call checks the BUFE (buffer-empty) bit in CMD; if data is present it
 * reads the header at the CAPR cursor, copies the frame (minus the 4-byte
 * header and the trailing 4-byte CRC) into `buf` (up to `buf_len`), advances
 * CAPR past the packet (4-byte aligned, wrapping the ring), and returns.
 *
 * Returns:
 *   >0  -- frame length in bytes; one frame written to `buf`.
 *    0  -- ring empty (nothing pending); caller should back off / yield.
 *   <0  -- driver not initialized, bad args, or a corrupt RX header.
 */
int  rtl8139_rx_poll(void* buf, unsigned short buf_len);

#endif /* RTL8139_H */
