/*
 * virtio_net.c -- VIRTIO-NET-0: legacy virtio-net host driver (poll-driven).
 * ==========================================================================
 * See virtio_net.h for the contract. Legacy ("virtio 0.9.5") interface over
 * BAR0 port I/O -- what QEMU's transitional `-device virtio-net-pci` exposes on
 * the default pc/i440FX machine (PCI 1af4:1000, rev 0). No PCI-capability walk,
 * no MSI-X, no interrupts: RX/TX complete by polling the used ring, exactly like
 * e1000_rx_poll / rtl8139_rx_poll. Identity-mapped RAM (mem.h) means a
 * pmm_alloc_page() pointer IS its physical/DMA address, so ring/buffer physical
 * addresses are the pointers themselves (same contract e1000 relies on).
 *
 * Feature negotiation: only VIRTIO_NET_F_MAC (bit 5) if offered; everything
 * else 0. No MRG_RXBUF -> the fixed 10-byte legacy virtio_net_hdr precedes each
 * frame (zeroed on TX, skipped on RX). Single-descriptor hdr+data framing
 * (QEMU accepts the combined layout).
 *
 * Barrier idiom mirrors e1000.c: sfence + compiler barrier before the queue-
 * notify doorbell; used->idx read through a volatile pointer so the poll loop
 * cannot hoist it.
 */
#include "../../include/virtio_net.h"
#include "../../include/pci.h"
#include "../../include/mem.h"        /* pmm_alloc_page / pmm_alloc_pages */
#include "../../include/x86_64.h"     /* inb/inw/inl/outb/outw/outl      */
#include "../../include/string.h"     /* memcpy / memset                 */
#include "../../include/kernel.h"     /* kprintf                         */

/* ---- PCI identity ---- */
#define VNET_VENDOR   0x1AF4
#define VNET_DEV_LEG  0x1000          /* transitional (legacy-capable)   */
#define VNET_DEV_MOD  0x1041          /* modern-only (we decline)        */
#define PCI_CONFIG_COMMAND  0x04
#define PCI_CMD_IO_SPACE    0x0001
#define PCI_BAR_IO          0x0001    /* BAR bit0 set => I/O space        */

/* ---- Legacy BAR0 register offsets (no MSI-X -> device cfg at 0x14) ---- */
#define VR_HOST_FEATURES  0x00        /* u32 RO */
#define VR_GUEST_FEATURES 0x04        /* u32 RW */
#define VR_QUEUE_PFN      0x08        /* u32 RW (ring phys >> 12)         */
#define VR_QUEUE_SIZE     0x0C        /* u16 RO */
#define VR_QUEUE_SELECT   0x0E        /* u16 RW */
#define VR_QUEUE_NOTIFY   0x10        /* u16 WO */
#define VR_DEVICE_STATUS  0x12        /* u8  RW */
#define VR_ISR_STATUS     0x13        /* u8  RO */
#define VR_NET_MAC        0x14        /* 6 x u8 (device config)          */

/* device status bits */
#define VS_ACK       0x01
#define VS_DRIVER    0x02
#define VS_DRIVER_OK 0x04
#define VS_FAILED    0x80

#define VF_NET_MAC   (1u << 5)        /* VIRTIO_NET_F_MAC                */

/* vring descriptor flags */
#define VRING_F_NEXT   1
#define VRING_F_WRITE  2

/* We post only a handful of buffers into whatever the device's queue size is. */
#define VNET_RX_BUFS   64
#define VNET_TX_BUFS   16
#define VNET_HDR_LEN   10             /* legacy virtio_net_hdr           */
#define VNET_BUF_SZ    2048           /* >= 10 + 1514                    */

typedef struct {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} __attribute__((packed)) vring_desc_t;

typedef struct {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[];                  /* [qsize] (+ used_event tail)     */
} __attribute__((packed)) vring_avail_t;

typedef struct {
    uint32_t id;
    uint32_t len;
} __attribute__((packed)) vring_used_elem_t;

typedef struct {
    uint16_t flags;
    uint16_t idx;
    vring_used_elem_t ring[];
} __attribute__((packed)) vring_used_t;

typedef struct {
    volatile vring_desc_t*  desc;
    volatile vring_avail_t* avail;
    volatile vring_used_t*  used;
    uint16_t qsize;
    uint16_t last_used;               /* free-running u16; compare !=    */
    uint16_t avail_shadow;            /* mirror of avail->idx            */
} vnet_vq_t;

static struct {
    int      present;
    uint16_t iobase;
    uint8_t  mac[6];
    vnet_vq_t rxq, txq;               /* queue 0 = RX, queue 1 = TX      */
    uint8_t* rx_bufs[VNET_RX_BUFS];
    uint8_t* tx_bufs[VNET_TX_BUFS];
    uint16_t tx_next;
} vnet;

static inline void desc_wmb(void) { asm volatile("sfence" ::: "memory"); }

/* ALIGN(x, 4096). */
static inline uint64_t align4k(uint64_t x) { return (x + 0xFFFu) & ~0xFFFull; }

/* Set up one virtqueue: select it, read its size, allocate the contiguous ring
 * (legacy layout), program the PFN. Returns 0 / -1. */
static int vq_setup(vnet_vq_t* q, uint16_t qi) {
    outw(vnet.iobase + VR_QUEUE_SELECT, qi);
    uint16_t sz = inw(vnet.iobase + VR_QUEUE_SIZE);
    if (sz == 0 || sz > 1024) return -1;
    q->qsize = sz;

    /* Legacy ring size: desc[sz] + avail(flags+idx+ring[sz]+used_event),
     * then 4K-aligned used(flags+idx+used_elem[sz]+avail_event). */
    uint64_t part1 = (uint64_t)16 * sz + (uint64_t)2 * (3 + sz);
    uint64_t used_off = align4k(part1);
    uint64_t total = used_off + align4k((uint64_t)6 + (uint64_t)8 * sz);
    uint64_t pages = (total + 0xFFFull) / 0x1000ull;

    void* ring = pmm_alloc_pages((size_t)pages);
    if (!ring) return -1;
    memset(ring, 0, (size_t)pages * 0x1000ull);

    q->desc  = (volatile vring_desc_t*)ring;
    q->avail = (volatile vring_avail_t*)((uint8_t*)ring + (uint64_t)16 * sz);
    q->used  = (volatile vring_used_t*)((uint8_t*)ring + used_off);
    q->last_used = 0;
    q->avail_shadow = 0;

    /* PFN = ring physical >> 12 (identity map: pointer == phys). */
    outl(vnet.iobase + VR_QUEUE_PFN, (uint32_t)(((uint64_t)(uintptr_t)ring) >> 12));
    return 0;
}

int virtio_net_init(void) {
    if (vnet.present) return 0;
    memset(&vnet, 0, sizeof(vnet));

    pci_device_t* dev = pci_find_device(VNET_VENDOR, VNET_DEV_LEG);
    if (!dev) {
        if (pci_find_device(VNET_VENDOR, VNET_DEV_MOD))
            kprintf("[VIRTIO-NET] modern-only device (0x1041) -- legacy driver declines\n");
        else
            kprintf("[VIRTIO-NET] no virtio-net NIC found\n");
        return -1;   /* NO class fallback: side-effect-free when absent */
    }

    /* BAR0 must be an I/O BAR. */
    if (!(dev->bar[0] & PCI_BAR_IO)) {
        kprintf("[VIRTIO-NET] BAR0 is not I/O space -- declining\n");
        return -1;
    }
    vnet.iobase = (uint16_t)(pci_get_bar(dev, 0) & 0xFFFCu);

    /* Enable I/O-space decode + bus master (rtl8139.c idiom). */
    uint16_t cmd = pci_config_read_word(dev->bus, dev->device, dev->function,
                                        PCI_CONFIG_COMMAND);
    cmd |= PCI_CMD_IO_SPACE;
    pci_config_write_word(dev->bus, dev->device, dev->function,
                          PCI_CONFIG_COMMAND, cmd);
    pci_enable_bus_master(dev);

    /* Reset, then ACK + DRIVER. */
    outb(vnet.iobase + VR_DEVICE_STATUS, 0);
    outb(vnet.iobase + VR_DEVICE_STATUS, VS_ACK);
    outb(vnet.iobase + VR_DEVICE_STATUS, VS_ACK | VS_DRIVER);

    /* Negotiate F_MAC only. */
    uint32_t host = inl(vnet.iobase + VR_HOST_FEATURES);
    outl(vnet.iobase + VR_GUEST_FEATURES, host & VF_NET_MAC);
    kprintf("[VIRTIO-NET] features host=%x ack=%x\n", host, (host & VF_NET_MAC));

    /* Read MAC from device config. */
    for (int i = 0; i < 6; i++)
        vnet.mac[i] = inb(vnet.iobase + VR_NET_MAC + i);
    kprintf("VIRTIONET: PROBE ok\n");
    kprintf("[VIRTIO-NET] MAC %x:%x:%x:%x:%x:%x\n",
            vnet.mac[0], vnet.mac[1], vnet.mac[2],
            vnet.mac[3], vnet.mac[4], vnet.mac[5]);

    if (vq_setup(&vnet.rxq, 0) != 0 || vq_setup(&vnet.txq, 1) != 0) {
        outb(vnet.iobase + VR_DEVICE_STATUS, VS_FAILED);
        kprintf("[VIRTIO-NET] vq setup failed\n");
        return -1;
    }

    /* Pre-post RX buffers: desc i owns rx_bufs[i]; publish all 64 in avail. */
    for (int i = 0; i < VNET_RX_BUFS && i < vnet.rxq.qsize; i++) {
        uint8_t* b = (uint8_t*)pmm_alloc_page();
        if (!b) { outb(vnet.iobase + VR_DEVICE_STATUS, VS_FAILED); return -1; }
        vnet.rx_bufs[i] = b;
        vnet.rxq.desc[i].addr  = (uint64_t)(uintptr_t)b;
        vnet.rxq.desc[i].len   = VNET_BUF_SZ;
        vnet.rxq.desc[i].flags = VRING_F_WRITE;   /* device writes into it */
        vnet.rxq.desc[i].next  = 0;
        vnet.rxq.avail->ring[i] = (uint16_t)i;
    }
    desc_wmb();
    vnet.rxq.avail_shadow = (uint16_t)((VNET_RX_BUFS < vnet.rxq.qsize) ? VNET_RX_BUFS : vnet.rxq.qsize);
    vnet.rxq.avail->idx = vnet.rxq.avail_shadow;
    desc_wmb();

    /* TX buffers pre-allocated (no avail entries until a send). */
    for (int i = 0; i < VNET_TX_BUFS && i < vnet.txq.qsize; i++) {
        uint8_t* b = (uint8_t*)pmm_alloc_page();
        if (!b) { outb(vnet.iobase + VR_DEVICE_STATUS, VS_FAILED); return -1; }
        vnet.tx_bufs[i] = b;
    }

    outb(vnet.iobase + VR_DEVICE_STATUS, VS_ACK | VS_DRIVER | VS_DRIVER_OK);
    outw(vnet.iobase + VR_QUEUE_NOTIFY, 0);   /* kick RX so the device takes buffers */

    vnet.present = 1;
    kprintf("[VIRTIO-NET] init complete (rxq=%u txq=%u)\n", vnet.rxq.qsize, vnet.txq.qsize);
    return 0;
}

int virtio_net_present(void) { return vnet.present; }

int virtio_net_get_mac(uint8_t out[ETH_ALEN]) {
    if (!vnet.present) return -1;
    memcpy(out, vnet.mac, ETH_ALEN);
    return 0;
}

int virtio_net_tx(const void* frame, uint16_t len) {
    if (!vnet.present) return -1;
    if (len == 0) return 0;
    if (len > 1514) len = 1514;

    /* NET-ROBUST: cycle TX slots within the buffers we actually allocated
     * (min(VNET_TX_BUFS, qsize)); a device reporting a queue smaller than
     * VNET_TX_BUFS would otherwise index a NULL tx_buf and stamp a descriptor
     * past the real ring. */
    uint16_t txcap = (vnet.txq.qsize < VNET_TX_BUFS) ? vnet.txq.qsize : VNET_TX_BUFS;
    if (txcap == 0) return -1;
    uint16_t slot = (uint16_t)(vnet.tx_next % txcap);
    uint8_t* buf = vnet.tx_bufs[slot];
    if (!buf) return -1;
    memset(buf, 0, VNET_HDR_LEN);                 /* zeroed legacy hdr */
    memcpy(buf + VNET_HDR_LEN, frame, len);
    uint32_t total = VNET_HDR_LEN + (uint32_t)((len < 60) ? 60 : len);  /* pad runt */
    /* NET-ROBUST: zero the runt padding so stale page / prior-frame bytes are
     * never leaked onto the wire (etherleak) -- pmm pages are not pre-zeroed and
     * the 16 TX buffers are reused round-robin. */
    if (len < 60) memset(buf + VNET_HDR_LEN + len, 0, 60u - len);

    /* Use descriptor `slot` in the TX queue. */
    vnet.txq.desc[slot].addr  = (uint64_t)(uintptr_t)buf;
    vnet.txq.desc[slot].len   = total;
    vnet.txq.desc[slot].flags = 0;                /* device reads it */
    vnet.txq.desc[slot].next  = 0;

    vnet.txq.avail->ring[vnet.txq.avail_shadow % vnet.txq.qsize] = slot;
    desc_wmb();
    vnet.txq.avail->idx = ++vnet.txq.avail_shadow;
    desc_wmb();
    outw(vnet.iobase + VR_QUEUE_NOTIFY, 1);       /* doorbell: TX queue */

    vnet.tx_next = (uint16_t)((vnet.tx_next + 1) % txcap);

    /* Bounded wait for this completion so the caller sees a sent frame (the
     * used ring index advances). Timeout still returns len (queued). */
    for (volatile uint32_t i = 0; i < 2000000u; i++) {
        if (vnet.txq.used->idx != vnet.txq.last_used) {
            vnet.txq.last_used = vnet.txq.used->idx;
            break;
        }
        asm volatile("pause");
    }
    return (int)len;
}

int virtio_net_rx_poll(void* out, uint16_t buf_len) {
    if (!vnet.present) return -1;
    if (vnet.rxq.last_used == vnet.rxq.used->idx) return 0;   /* volatile read */
    asm volatile("" ::: "memory");                            /* x86 TSO: no fence needed */

    uint16_t slot = vnet.rxq.last_used % vnet.rxq.qsize;
    vring_used_elem_t e = { vnet.rxq.used->ring[slot].id, vnet.rxq.used->ring[slot].len };
    /* NET-ROBUST: the used-ring id is device-supplied; bound it against the
     * buffers we actually posted (min(VNET_RX_BUFS, qsize)), not the compile-
     * time array size, so a bogus id can't deref an unposted (NULL) rx_buf. */
    uint16_t rxcap = (vnet.rxq.qsize < VNET_RX_BUFS) ? vnet.rxq.qsize : VNET_RX_BUFS;
    int rc;
    if (e.id >= rxcap || e.len < VNET_HDR_LEN) {
        rc = -1;                                              /* malformed; recycle below */
    } else {
        uint32_t flen = e.len - VNET_HDR_LEN;
        if (flen > buf_len) flen = buf_len;
        if (flen > 1514)    flen = 1514;
        memcpy(out, vnet.rx_bufs[e.id] + VNET_HDR_LEN, flen);
        rc = (int)flen;
    }

    /* Recycle this buffer back into the avail ring (repost the same id). */
    uint16_t rid = (uint16_t)(e.id < rxcap ? e.id : 0);
    vnet.rxq.avail->ring[vnet.rxq.avail_shadow % vnet.rxq.qsize] = rid;
    desc_wmb();
    vnet.rxq.avail->idx = ++vnet.rxq.avail_shadow;
    desc_wmb();
    outw(vnet.iobase + VR_QUEUE_NOTIFY, 0);                   /* doorbell: RX queue */

    vnet.rxq.last_used++;
    return rc;
}
