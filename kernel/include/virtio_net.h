#ifndef VIRTIO_NET_H
#define VIRTIO_NET_H
/*
 * virtio_net.h -- VIRTIO-NET-0: legacy virtio-net NIC (poll-driven).
 * =================================================================
 * A third NIC backend behind the same 5-call contract e1000/rtl8139 use, so
 * kernel/net/net.c can select it with zero changes above the seam. Targets
 * QEMU's transitional `-device virtio-net-pci` (PCI 1af4:1000, legacy register
 * file on BAR0 I/O space) on the default pc/i440FX machine. Driven entirely by
 * polling the used ring -- no interrupts, matching the poll-mode stack.
 *
 * Probe is EXACT-ID only (no class fallback), so on any machine without a
 * virtio NIC (the T410) it is a side-effect-free table miss. net_init() must
 * probe this BEFORE e1000_init() (e1000's class-scan fallback would otherwise
 * claim the virtio device and treat its I/O-port BAR as an MMIO pointer).
 */
#include "types.h"
#include "net.h"                 /* ETH_ALEN */

/* 0 on success (device present + rings up), <0 on absent/failed. Idempotent;
 * never hangs (all waits bounded); side-effect-free when no device is present. */
int virtio_net_init(void);
int virtio_net_present(void);
int virtio_net_get_mac(uint8_t out[ETH_ALEN]);
int virtio_net_tx(const void* frame, uint16_t len);      /* bytes queued or <0 */
int virtio_net_rx_poll(void* buf, uint16_t buf_len);     /* >0 len, 0 empty, <0 */

#endif /* VIRTIO_NET_H */
