# AutomationOS Driver Writing Guide

**Version:** 2.0  
**Target Audience:** Kernel developers  
**Last Updated:** 2026-05-26

## Table of Contents

1. [Introduction](#introduction)
2. [Driver Structure](#driver-structure)
3. [PCI Driver Example](#pci-driver-example)
4. [USB Driver Example](#usb-driver-example)
5. [Platform Driver Example](#platform-driver-example)
6. [DMA Usage](#dma-usage)
7. [IRQ Handling](#irq-handling)
8. [Power Management](#power-management)
9. [Testing](#testing)
10. [Debugging](#debugging)
11. [Common Pitfalls](#common-pitfalls)
12. [Checklist](#checklist)

---

## Introduction

This guide provides practical examples for writing device drivers in AutomationOS. It follows the driver framework documented in `DRIVER_FRAMEWORK.md`.

### Prerequisites

- Understanding of C programming
- Basic hardware knowledge (registers, DMA, interrupts)
- Familiarity with the specific hardware you're writing a driver for

### File Organization

```
kernel/drivers/
├── core/           # Driver framework (don't modify)
├── storage/        # Storage drivers (NVMe, AHCI, etc.)
├── net/            # Network drivers
├── usb/            # USB infrastructure and class drivers
├── gpu/            # Graphics drivers
├── audio/          # Audio drivers
└── platform/       # Platform-specific devices
```

---

## Driver Structure

### Minimal Driver Template

```c
/*
 * My Device Driver
 * Description: Brief description of what this driver does
 */

#include "../../include/device.h"
#include "../../include/drivers.h"
#include "../../include/dma.h"
#include "../../include/irq.h"
#include "../../include/kernel.h"
#include "../../include/mem.h"
#include "../../include/pci.h"  // Or usb.h, platform.h, etc.

// Device private data
typedef struct {
    device_t* dev;
    void* mmio_base;
    uint32_t irq;
    dma_pool_t* dma_pool;
    
    // Add device-specific fields here
    uint32_t state;
    spinlock_t lock;
} my_device_data_t;

/**
 * Probe - Called when device is detected
 */
static int my_probe(device_t* dev) {
    kprintf("[MYDRV] Probing device: %s\n", dev->name);
    
    // Allocate driver private data
    my_device_data_t* priv = kmalloc(sizeof(my_device_data_t));
    if (!priv) {
        return -ENOMEM;
    }
    
    memset(priv, 0, sizeof(my_device_data_t));
    priv->dev = dev;
    device_set_driver_data(dev, priv);
    
    // Initialize hardware (see specific examples below)
    int ret = my_init_hardware(priv);
    if (ret < 0) {
        goto err_hw_init;
    }
    
    return 0;
    
err_hw_init:
    kfree(priv);
    return ret;
}

/**
 * Remove - Called when device is removed
 */
static int my_remove(device_t* dev) {
    my_device_data_t* priv = device_get_driver_data(dev);
    
    kprintf("[MYDRV] Removing device: %s\n", dev->name);
    
    // Shutdown hardware
    my_shutdown_hardware(priv);
    
    // Free resources
    kfree(priv);
    
    return 0;
}

/**
 * Power management callbacks
 */
static int my_suspend(device_t* dev) {
    my_device_data_t* priv = device_get_driver_data(dev);
    
    // Save hardware state
    my_save_state(priv);
    
    // Power down device
    my_power_down(priv);
    
    return 0;
}

static int my_resume(device_t* dev) {
    my_device_data_t* priv = device_get_driver_data(dev);
    
    // Restore hardware state
    my_restore_state(priv);
    
    // Power up device
    my_power_up(priv);
    
    return 0;
}

static const device_pm_ops_t my_pm_ops = {
    .suspend = my_suspend,
    .resume = my_resume,
};

/**
 * Driver structure
 */
static driver_t my_driver = {
    .name = "my_device_driver",
    .bus = &pci_bus_type,  // Or usb_bus_type, platform_bus_type
    .probe = my_probe,
    .remove = my_remove,
    .pm_ops = &my_pm_ops,
    .priority = 100,
};

/**
 * Driver initialization
 */
void my_driver_init(void) {
    int ret = driver_register(&my_driver);
    if (ret < 0) {
        kprintf("[MYDRV] Failed to register driver\n");
    }
}
```

---

## PCI Driver Example

### Example: Simple Network Card Driver

```c
/*
 * Simple Network Card Driver
 * Vendor: 0x8086, Device: 0x100E (Intel 82540EM)
 */

#include "../../include/device.h"
#include "../../include/pci.h"
#include "../../include/dma.h"
#include "../../include/irq.h"
#include "../../include/kernel.h"
#include "../../include/mem.h"

#define VENDOR_ID 0x8086
#define DEVICE_ID 0x100E

// Hardware registers
#define REG_CTRL     0x0000
#define REG_STATUS   0x0008
#define REG_EEPROM   0x0014
#define REG_ICR      0x00C0  // Interrupt Cause Read
#define REG_IMS      0x00D0  // Interrupt Mask Set
#define REG_RDBAL    0x2800  // RX Descriptor Base Low
#define REG_RDBAH    0x2804  // RX Descriptor Base High
#define REG_RDLEN    0x2808  // RX Descriptor Length
#define REG_TDBAL    0x3800  // TX Descriptor Base Low
#define REG_TDBAH    0x3804  // TX Descriptor Base High
#define REG_TDLEN    0x3808  // TX Descriptor Length

// Interrupt flags
#define INT_TXDW     0x00000001  // TX Descriptor Written Back
#define INT_RXDMT0   0x00000010  // RX Descriptor Min Threshold
#define INT_RXT0     0x00000080  // RX Timer

// Device private data
typedef struct {
    device_t* dev;
    pci_device_t* pci_dev;
    void* mmio_base;
    uint32_t irq;
    
    // MAC address
    uint8_t mac_addr[6];
    
    // RX ring
    void* rx_desc_ring;
    uint64_t rx_desc_phys;
    void** rx_buffers;
    uint32_t rx_tail;
    
    // TX ring
    void* tx_desc_ring;
    uint64_t tx_desc_phys;
    void** tx_buffers;
    uint32_t tx_tail;
    
    spinlock_t tx_lock;
} netcard_device_t;

/**
 * Read register
 */
static inline uint32_t netcard_read32(netcard_device_t* nic, uint32_t reg) {
    return *(volatile uint32_t*)((uint8_t*)nic->mmio_base + reg);
}

/**
 * Write register
 */
static inline void netcard_write32(netcard_device_t* nic, uint32_t reg, uint32_t value) {
    *(volatile uint32_t*)((uint8_t*)nic->mmio_base + reg) = value;
}

/**
 * Read MAC address from EEPROM
 */
static int netcard_read_mac(netcard_device_t* nic) {
    // Simplified EEPROM read (real implementation more complex)
    for (int i = 0; i < 3; i++) {
        uint32_t temp = netcard_read32(nic, REG_EEPROM + i * 4);
        nic->mac_addr[i * 2] = (temp & 0xFF);
        nic->mac_addr[i * 2 + 1] = (temp >> 8) & 0xFF;
    }
    
    kprintf("[NETCARD] MAC: %02x:%02x:%02x:%02x:%02x:%02x\n",
            nic->mac_addr[0], nic->mac_addr[1], nic->mac_addr[2],
            nic->mac_addr[3], nic->mac_addr[4], nic->mac_addr[5]);
    
    return 0;
}

/**
 * Initialize RX ring
 */
static int netcard_init_rx(netcard_device_t* nic) {
    uint32_t ring_size = 256;
    uint32_t desc_size = 16;  // 16 bytes per descriptor
    
    // Allocate descriptor ring
    nic->rx_desc_ring = dma_alloc_coherent(nic->dev, ring_size * desc_size,
                                          &nic->rx_desc_phys, 0);
    if (!nic->rx_desc_ring) {
        return -ENOMEM;
    }
    
    // Allocate RX buffers
    nic->rx_buffers = kmalloc(ring_size * sizeof(void*));
    if (!nic->rx_buffers) {
        dma_free_coherent(nic->dev, ring_size * desc_size, nic->rx_desc_ring, nic->rx_desc_phys);
        return -ENOMEM;
    }
    
    // Allocate and setup buffers
    for (uint32_t i = 0; i < ring_size; i++) {
        uint64_t buf_phys;
        nic->rx_buffers[i] = dma_alloc_coherent(nic->dev, 2048, &buf_phys, 0);
        if (!nic->rx_buffers[i]) {
            // TODO: Free previously allocated buffers
            return -ENOMEM;
        }
        
        // Fill descriptor
        volatile uint64_t* desc = (uint64_t*)((uint8_t*)nic->rx_desc_ring + i * desc_size);
        desc[0] = buf_phys;  // Buffer address
        desc[1] = 0;         // Status
    }
    
    // Program hardware
    netcard_write32(nic, REG_RDBAL, (uint32_t)(nic->rx_desc_phys & 0xFFFFFFFF));
    netcard_write32(nic, REG_RDBAH, (uint32_t)(nic->rx_desc_phys >> 32));
    netcard_write32(nic, REG_RDLEN, ring_size * desc_size);
    
    nic->rx_tail = ring_size - 1;
    netcard_write32(nic, REG_RDBAL + 0x10, nic->rx_tail);  // RDT
    
    return 0;
}

/**
 * Initialize TX ring (similar to RX)
 */
static int netcard_init_tx(netcard_device_t* nic) {
    // Similar to netcard_init_rx()
    // ... (implementation omitted for brevity)
    return 0;
}

/**
 * IRQ handler
 */
static irq_return_t netcard_irq_handler(uint32_t irq, void* dev_id) {
    netcard_device_t* nic = (netcard_device_t*)dev_id;
    
    // Read interrupt cause
    uint32_t icr = netcard_read32(nic, REG_ICR);
    
    if (icr == 0) {
        return IRQ_NONE;  // Not our interrupt
    }
    
    // Handle RX
    if (icr & INT_RXT0) {
        // TODO: Process received packets
        kprintf("[NETCARD] RX interrupt\n");
    }
    
    // Handle TX
    if (icr & INT_TXDW) {
        // TODO: Free transmitted buffers
        kprintf("[NETCARD] TX interrupt\n");
    }
    
    return IRQ_HANDLED;
}

/**
 * Probe function
 */
static int netcard_probe(device_t* dev) {
    pci_device_t* pci_dev = (pci_device_t*)dev->platform_data;
    
    kprintf("[NETCARD] Probing device %04x:%04x\n", pci_dev->vendor_id, pci_dev->device_id);
    
    // Allocate driver data
    netcard_device_t* nic = kmalloc(sizeof(netcard_device_t));
    if (!nic) {
        return -ENOMEM;
    }
    
    memset(nic, 0, sizeof(netcard_device_t));
    nic->dev = dev;
    nic->pci_dev = pci_dev;
    device_set_driver_data(dev, nic);
    
    // Map BAR0 (MMIO registers)
    nic->mmio_base = pci_map_bar(pci_dev, 0);
    if (!nic->mmio_base) {
        kprintf("[NETCARD] Failed to map BAR0\n");
        kfree(nic);
        return -ENOMEM;
    }
    
    // Enable bus mastering for DMA
    pci_enable_bus_master(pci_dev);
    
    // Read MAC address
    netcard_read_mac(nic);
    
    // Initialize RX/TX rings
    int ret = netcard_init_rx(nic);
    if (ret < 0) {
        goto err_rx;
    }
    
    ret = netcard_init_tx(nic);
    if (ret < 0) {
        goto err_tx;
    }
    
    // Request IRQ
    nic->irq = pci_dev->irq;
    ret = request_irq(nic->irq, netcard_irq_handler, IRQ_FLAG_SHARED, "netcard", nic);
    if (ret < 0) {
        goto err_irq;
    }
    
    // Enable interrupts
    netcard_write32(nic, REG_IMS, INT_RXT0 | INT_TXDW | INT_RXDMT0);
    
    kprintf("[NETCARD] Device initialized successfully\n");
    return 0;
    
err_irq:
    // TODO: Free TX ring
err_tx:
    // TODO: Free RX ring
err_rx:
    pci_unmap_bar(pci_dev, 0, nic->mmio_base);
    kfree(nic);
    return ret;
}

/**
 * Remove function
 */
static int netcard_remove(device_t* dev) {
    netcard_device_t* nic = device_get_driver_data(dev);
    
    kprintf("[NETCARD] Removing device\n");
    
    // Disable interrupts
    netcard_write32(nic, REG_IMS, 0);
    
    // Free IRQ
    free_irq(nic->irq, nic);
    
    // TODO: Free RX/TX rings and buffers
    
    // Unmap BAR0
    pci_unmap_bar(nic->pci_dev, 0, nic->mmio_base);
    
    kfree(nic);
    return 0;
}

/**
 * PCI match table
 */
static const pci_device_id_t netcard_pci_ids[] = {
    { VENDOR_ID, DEVICE_ID },
    { 0, 0 }  // Terminator
};

/**
 * PCI driver
 */
static driver_t netcard_driver = {
    .name = "netcard",
    .bus = &pci_bus_type,
    .probe = netcard_probe,
    .remove = netcard_remove,
    .match_table = netcard_pci_ids,
};

void netcard_init(void) {
    driver_register(&netcard_driver);
}
```

### Key Points for PCI Drivers

1. **BAR Mapping:** Use `pci_map_bar()` to access MMIO registers
2. **Bus Mastering:** Enable with `pci_enable_bus_master()` for DMA
3. **IRQ:** PCI devices provide `pci_dev->irq`
4. **MSI/MSI-X:** Call `msi_enable()` or `msix_enable()` for better interrupt handling
5. **Device Matching:** Use PCI device ID table in `match_table`

---

## USB Driver Example

### Example: USB Keyboard Driver

```c
/*
 * USB HID Keyboard Driver
 */

#include "../../include/device.h"
#include "../../include/usb.h"
#include "../../include/input.h"
#include "../../include/kernel.h"
#include "../../include/mem.h"

// HID keyboard report (8 bytes)
typedef struct {
    uint8_t modifiers;  // Ctrl, Shift, Alt, GUI
    uint8_t reserved;
    uint8_t keys[6];    // Up to 6 simultaneous keys
} PACKED usb_kbd_report_t;

// Device private data
typedef struct {
    device_t* dev;
    usb_device_t* usb_dev;
    uint8_t interface_num;
    uint8_t endpoint_addr;
    uint16_t max_packet_size;
    
    usb_kbd_report_t report;
    usb_kbd_report_t prev_report;
    
    urb_t* int_urb;  // Interrupt URB for polling
} usb_kbd_device_t;

/**
 * URB completion callback
 */
static void usb_kbd_urb_complete(urb_t* urb) {
    usb_kbd_device_t* kbd = (usb_kbd_device_t*)urb->context;
    
    if (urb->status != 0) {
        kprintf("[USBKBD] URB error: %d\n", urb->status);
        // Resubmit URB
        usb_submit_urb(urb);
        return;
    }
    
    // Copy report
    memcpy(&kbd->report, urb->transfer_buffer, sizeof(usb_kbd_report_t));
    
    // Detect key changes
    for (int i = 0; i < 6; i++) {
        uint8_t key = kbd->report.keys[i];
        
        // Key pressed (not in previous report)
        if (key != 0) {
            bool was_pressed = false;
            for (int j = 0; j < 6; j++) {
                if (kbd->prev_report.keys[j] == key) {
                    was_pressed = true;
                    break;
                }
            }
            
            if (!was_pressed) {
                kprintf("[USBKBD] Key pressed: 0x%02x\n", key);
                input_report_key(key, 1);  // Send to input subsystem
            }
        }
    }
    
    // Check for key releases
    for (int i = 0; i < 6; i++) {
        uint8_t key = kbd->prev_report.keys[i];
        if (key == 0) continue;
        
        bool still_pressed = false;
        for (int j = 0; j < 6; j++) {
            if (kbd->report.keys[j] == key) {
                still_pressed = true;
                break;
            }
        }
        
        if (!still_pressed) {
            kprintf("[USBKBD] Key released: 0x%02x\n", key);
            input_report_key(key, 0);  // Send to input subsystem
        }
    }
    
    // Save for next comparison
    memcpy(&kbd->prev_report, &kbd->report, sizeof(usb_kbd_report_t));
    
    // Resubmit URB for next poll
    usb_submit_urb(urb);
}

/**
 * Probe function
 */
static int usb_kbd_probe(device_t* dev) {
    usb_device_t* usb_dev = (usb_device_t*)dev->platform_data;
    
    kprintf("[USBKBD] Probing USB keyboard\n");
    
    // Allocate driver data
    usb_kbd_device_t* kbd = kmalloc(sizeof(usb_kbd_device_t));
    if (!kbd) {
        return -ENOMEM;
    }
    
    memset(kbd, 0, sizeof(usb_kbd_device_t));
    kbd->dev = dev;
    kbd->usb_dev = usb_dev;
    device_set_driver_data(dev, kbd);
    
    // Find HID interface (class 0x03)
    usb_interface_desc_t* iface = usb_find_interface(usb_dev, 0x03, 0x01, 0x01);
    if (!iface) {
        kprintf("[USBKBD] HID interface not found\n");
        kfree(kbd);
        return -ENODEV;
    }
    
    kbd->interface_num = iface->interface_num;
    
    // Find interrupt IN endpoint
    usb_endpoint_desc_t* ep = usb_find_endpoint(iface, USB_DIR_IN, USB_EP_TYPE_INT);
    if (!ep) {
        kprintf("[USBKBD] Interrupt IN endpoint not found\n");
        kfree(kbd);
        return -ENODEV;
    }
    
    kbd->endpoint_addr = ep->endpoint_addr;
    kbd->max_packet_size = ep->max_packet_size;
    
    // Allocate and setup interrupt URB
    kbd->int_urb = usb_alloc_urb();
    if (!kbd->int_urb) {
        kfree(kbd);
        return -ENOMEM;
    }
    
    usb_fill_int_urb(kbd->int_urb, usb_dev, kbd->endpoint_addr,
                     &kbd->report, sizeof(usb_kbd_report_t),
                     usb_kbd_urb_complete, kbd, ep->interval);
    
    // Submit URB to start polling
    int ret = usb_submit_urb(kbd->int_urb);
    if (ret < 0) {
        usb_free_urb(kbd->int_urb);
        kfree(kbd);
        return ret;
    }
    
    kprintf("[USBKBD] Keyboard initialized (polling every %d ms)\n", ep->interval);
    return 0;
}

/**
 * Remove function
 */
static int usb_kbd_remove(device_t* dev) {
    usb_kbd_device_t* kbd = device_get_driver_data(dev);
    
    kprintf("[USBKBD] Removing keyboard\n");
    
    // Cancel and free URB
    usb_kill_urb(kbd->int_urb);
    usb_free_urb(kbd->int_urb);
    
    kfree(kbd);
    return 0;
}

/**
 * USB match table
 */
static const usb_device_id_t usb_kbd_ids[] = {
    // Match any HID keyboard (class 0x03, subclass 0x01, protocol 0x01)
    { USB_MATCH_CLASS(0x03, 0x01, 0x01) },
    { }
};

/**
 * USB driver
 */
static driver_t usb_kbd_driver = {
    .name = "usb_keyboard",
    .bus = &usb_bus_type,
    .probe = usb_kbd_probe,
    .remove = usb_kbd_remove,
    .match_table = usb_kbd_ids,
};

void usb_kbd_init(void) {
    driver_register(&usb_kbd_driver);
}
```

### Key Points for USB Drivers

1. **USB Device:** Access via `usb_device_t* usb_dev = (usb_device_t*)dev->platform_data`
2. **Interfaces:** Find interfaces by class/subclass/protocol
3. **Endpoints:** Find IN/OUT endpoints by direction and type
4. **URBs:** Use `usb_alloc_urb()`, `usb_submit_urb()`, `usb_kill_urb()`
5. **Interrupts:** Interrupt endpoints poll at `interval` (ms)

---

## Platform Driver Example

### Example: Serial UART Driver

```c
/*
 * Serial UART Platform Driver
 * For memory-mapped UART devices (SoC, embedded)
 */

#include "../../include/device.h"
#include "../../include/platform.h"
#include "../../include/irq.h"
#include "../../include/kernel.h"

// UART registers (16550 compatible)
#define UART_RBR  0x00  // Receive Buffer Register
#define UART_THR  0x00  // Transmit Holding Register
#define UART_IER  0x01  // Interrupt Enable Register
#define UART_IIR  0x02  // Interrupt Identification Register
#define UART_LCR  0x03  // Line Control Register
#define UART_LSR  0x05  // Line Status Register

#define LSR_DATA_READY  0x01
#define LSR_THR_EMPTY   0x20

// Device private data
typedef struct {
    device_t* dev;
    void* base;      // MMIO base address
    uint32_t irq;
    uint32_t baudrate;
} uart_device_t;

/**
 * Read register
 */
static inline uint8_t uart_read(uart_device_t* uart, uint32_t reg) {
    return *(volatile uint8_t*)((uint8_t*)uart->base + reg);
}

/**
 * Write register
 */
static inline void uart_write(uart_device_t* uart, uint32_t reg, uint8_t value) {
    *(volatile uint8_t*)((uint8_t*)uart->base + reg) = value;
}

/**
 * IRQ handler
 */
static irq_return_t uart_irq_handler(uint32_t irq, void* dev_id) {
    uart_device_t* uart = (uart_device_t*)dev_id;
    
    // Read interrupt identification
    uint8_t iir = uart_read(uart, UART_IIR);
    
    if (iir & 0x01) {
        return IRQ_NONE;  // No interrupt pending
    }
    
    // Check line status
    uint8_t lsr = uart_read(uart, UART_LSR);
    
    // Receive data available
    if (lsr & LSR_DATA_READY) {
        uint8_t ch = uart_read(uart, UART_RBR);
        kprintf("[UART] Received: 0x%02x ('%c')\n", ch, ch);
        // TODO: Add to RX buffer
    }
    
    // Transmit holding register empty
    if (lsr & LSR_THR_EMPTY) {
        // TODO: Send next byte from TX buffer
    }
    
    return IRQ_HANDLED;
}

/**
 * Initialize UART hardware
 */
static int uart_init_hw(uart_device_t* uart) {
    // Disable interrupts
    uart_write(uart, UART_IER, 0x00);
    
    // Set baud rate (simplified, assumes 115200)
    // Real implementation: calculate divisor from baudrate
    
    // Set line control: 8N1 (8 bits, no parity, 1 stop bit)
    uart_write(uart, UART_LCR, 0x03);
    
    // Enable FIFO
    uart_write(uart, UART_IIR, 0xC7);
    
    // Enable interrupts (RX data available)
    uart_write(uart, UART_IER, 0x01);
    
    return 0;
}

/**
 * Probe function
 */
static int uart_probe(device_t* dev) {
    platform_device_t* pdev = (platform_device_t*)dev->platform_data;
    
    kprintf("[UART] Probing UART device\n");
    
    // Allocate driver data
    uart_device_t* uart = kmalloc(sizeof(uart_device_t));
    if (!uart) {
        return -ENOMEM;
    }
    
    memset(uart, 0, sizeof(uart_device_t));
    uart->dev = dev;
    uart->baudrate = 115200;
    device_set_driver_data(dev, uart);
    
    // Get resources from platform device
    uart->base = platform_get_resource(pdev, PLATFORM_RESOURCE_MMIO, 0);
    if (!uart->base) {
        kprintf("[UART] Failed to get MMIO resource\n");
        kfree(uart);
        return -ENOMEM;
    }
    
    uart->irq = platform_get_irq(pdev, 0);
    if (uart->irq == 0) {
        kprintf("[UART] Failed to get IRQ\n");
        kfree(uart);
        return -EINVAL;
    }
    
    // Initialize hardware
    uart_init_hw(uart);
    
    // Request IRQ
    int ret = request_irq(uart->irq, uart_irq_handler, 0, "uart", uart);
    if (ret < 0) {
        kprintf("[UART] Failed to request IRQ %u\n", uart->irq);
        kfree(uart);
        return ret;
    }
    
    kprintf("[UART] Initialized at %p, IRQ %u\n", uart->base, uart->irq);
    return 0;
}

/**
 * Remove function
 */
static int uart_remove(device_t* dev) {
    uart_device_t* uart = device_get_driver_data(dev);
    
    kprintf("[UART] Removing device\n");
    
    // Disable interrupts
    uart_write(uart, UART_IER, 0x00);
    
    // Free IRQ
    free_irq(uart->irq, uart);
    
    kfree(uart);
    return 0;
}

/**
 * Platform driver
 */
static driver_t uart_driver = {
    .name = "uart",
    .bus = &platform_bus_type,
    .probe = uart_probe,
    .remove = uart_remove,
};

void uart_init(void) {
    driver_register(&uart_driver);
}
```

### Key Points for Platform Drivers

1. **Resources:** Use `platform_get_resource()` for MMIO, IRQ, DMA channels
2. **Device Tree:** Platform devices often come from device tree (ARM SoCs)
3. **No Hot-Plug:** Platform devices are typically static
4. **Clocks:** May need to enable clocks: `platform_clk_enable()`

---

## DMA Usage

### Coherent DMA for Control Structures

```c
// Allocate 4KB descriptor ring
uint64_t dma_addr;
void* ring = dma_alloc_coherent(dev, 4096, &dma_addr, 0);

// CPU can write directly
((uint32_t*)ring)[0] = 0xDEADBEEF;

// Give address to device
my_device_set_ring_addr(dma_addr);

// Free when done
dma_free_coherent(dev, 4096, ring, dma_addr);
```

### Streaming DMA for Data Buffers

```c
// Allocate buffer
uint8_t buffer[4096];
memcpy(buffer, data, 4096);

// Map for DMA
uint64_t dma_addr = dma_map_single(dev, buffer, 4096, DMA_TO_DEVICE);

// Give to device
my_device_start_transfer(dma_addr, 4096);

// Wait for completion
my_device_wait();

// Unmap
dma_unmap_single(dev, dma_addr, 4096, DMA_TO_DEVICE);
```

### Scatter-Gather DMA

```c
// Create scatter-gather table
dma_sg_table_t sgt;
dma_sg_alloc(&sgt, 4);

dma_sg_init_entry(&sgt, 0, buffer1, 1024);
dma_sg_init_entry(&sgt, 1, buffer2, 2048);
dma_sg_init_entry(&sgt, 2, buffer3, 4096);
dma_sg_init_entry(&sgt, 3, buffer4, 512);

// Map
dma_map_sg(dev, &sgt, DMA_TO_DEVICE);

// Pass to device
for (uint32_t i = 0; i < sgt.num_entries; i++) {
    my_device_add_prdt(sgt.entries[i].dma_addr, sgt.entries[i].length);
}

// Start transfer
my_device_start_sg_transfer();

// Wait and unmap
my_device_wait();
dma_unmap_sg(dev, &sgt, DMA_TO_DEVICE);
dma_sg_free(&sgt);
```

---

## IRQ Handling

### Standard IRQ

```c
static irq_return_t my_irq_handler(uint32_t irq, void* dev_id) {
    my_device_t* dev = (my_device_t*)dev_id;
    
    // Read status
    uint32_t status = my_device_read_status(dev);
    
    if (status == 0) {
        return IRQ_NONE;
    }
    
    // Handle interrupt
    my_handle_interrupt(dev, status);
    
    // Clear interrupt
    my_device_clear_status(dev, status);
    
    return IRQ_HANDLED;
}

// Request
request_irq(dev->irq, my_irq_handler, IRQ_FLAG_SHARED, "mydev", dev);
```

### Threaded IRQ

```c
static irq_return_t my_fast_irq(uint32_t irq, void* dev_id) {
    // Fast check
    if (my_device_check_irq(dev_id)) {
        return IRQ_WAKE_THREAD;
    }
    return IRQ_NONE;
}

static irq_return_t my_slow_irq(uint32_t irq, void* dev_id) {
    my_device_t* dev = (my_device_t*)dev_id;
    
    // Can sleep here
    my_process_data(dev);
    
    return IRQ_HANDLED;
}

// Request
request_threaded_irq(dev->irq, my_fast_irq, my_slow_irq,
                    IRQ_FLAG_SHARED, "mydev", dev);
```

### Tasklet

```c
static void my_tasklet_fn(uint64_t data) {
    my_device_t* dev = (my_device_t*)data;
    
    // Process deferred work
    my_process_batch(dev);
}

// Initialize
tasklet_t my_tasklet;
tasklet_init(&my_tasklet, my_tasklet_fn, (uint64_t)dev);

// Schedule from IRQ handler
tasklet_schedule(&my_tasklet);
```

---

## Power Management

### Implement PM Callbacks

```c
static int my_suspend(device_t* dev) {
    my_device_t* priv = device_get_driver_data(dev);
    
    // Stop DMA
    my_stop_dma(priv);
    
    // Save registers
    priv->saved_regs[0] = my_read_reg(priv, REG_CTRL);
    priv->saved_regs[1] = my_read_reg(priv, REG_CONFIG);
    
    // Power down
    my_write_reg(priv, REG_POWER, POWER_OFF);
    
    return 0;
}

static int my_resume(device_t* dev) {
    my_device_t* priv = device_get_driver_data(dev);
    
    // Power up
    my_write_reg(priv, REG_POWER, POWER_ON);
    
    // Restore registers
    my_write_reg(priv, REG_CTRL, priv->saved_regs[0]);
    my_write_reg(priv, REG_CONFIG, priv->saved_regs[1]);
    
    // Restart DMA
    my_start_dma(priv);
    
    return 0;
}

static const device_pm_ops_t my_pm_ops = {
    .suspend = my_suspend,
    .resume = my_resume,
};
```

### Enable Runtime PM

```c
// In probe:
device_pm_runtime_enable(dev);
device_pm_runtime_set_autosuspend_delay(dev, 2000);  // 2 seconds

// When device is used:
device_pm_runtime_get(dev);
my_do_operation(dev);
device_pm_runtime_put_autosuspend(dev);
```

---

## Testing

### Unit Tests

Create test file `tests/drivers/my_driver_test.c`:

```c
#include "../../kernel/drivers/my_driver.c"
#include "../drivers/driver_test_framework.h"

void test_my_init(void) {
    my_device_t dev;
    int ret = my_init_device(&dev);
    assert(ret == 0);
}

void test_my_irq_handler(void) {
    my_device_t dev;
    // Setup mock device
    
    irq_return_t ret = my_irq_handler(0, &dev);
    assert(ret == IRQ_HANDLED);
}

int main(void) {
    test_my_init();
    test_my_irq_handler();
    
    kprintf("[TEST] All tests passed\n");
    return 0;
}
```

### Integration Tests

Test with QEMU:

```bash
# NVMe
qemu-system-x86_64 -drive file=disk.img,if=none,id=nvm \
                   -device nvme,serial=deadbeef,drive=nvm

# Network
qemu-system-x86_64 -netdev user,id=net0 \
                   -device e1000,netdev=net0

# USB
qemu-system-x86_64 -device usb-kbd
```

---

## Debugging

### Print Debugging

```c
kprintf("[MYDRV] Probing device: vendor=%04x device=%04x\n",
        pci_dev->vendor_id, pci_dev->device_id);
```

### Register Dumps

```c
void my_dump_regs(my_device_t* dev) {
    kprintf("[MYDRV] Register dump:\n");
    kprintf("  CTRL:   0x%08x\n", my_read_reg(dev, REG_CTRL));
    kprintf("  STATUS: 0x%08x\n", my_read_reg(dev, REG_STATUS));
    kprintf("  IRQ:    0x%08x\n", my_read_reg(dev, REG_IRQ));
}
```

### DMA Debugging

```c
// Enable DMA debugging
dma_debug_enable();

// Your code here

// Check for leaks
dma_check_leaks();
dma_print_stats();
```

### IRQ Debugging

```c
// Print IRQ statistics
irq_print_stats();

// Check specific IRQ
uint64_t count = irq_get_count(dev->irq);
uint64_t unhandled = irq_get_unhandled_count(dev->irq);
kprintf("[MYDRV] IRQ %u: count=%llu unhandled=%llu\n",
        dev->irq, count, unhandled);
```

---

## Common Pitfalls

### 1. Forgetting to Enable Bus Mastering

```c
// WRONG: Device can't do DMA
pci_dev->...

// RIGHT:
pci_enable_bus_master(pci_dev);
```

### 2. Using Wrong DMA Direction

```c
// WRONG: Device writes to memory
dma_map_single(dev, buf, size, DMA_TO_DEVICE);

// RIGHT:
dma_map_single(dev, buf, size, DMA_FROM_DEVICE);
```

### 3. Not Checking Return Values

```c
// WRONG:
void* buf = kmalloc(4096);
// Use buf (may be NULL!)

// RIGHT:
void* buf = kmalloc(4096);
if (!buf) {
    return -ENOMEM;
}
```

### 4. Sleeping in IRQ Context

```c
// WRONG:
static irq_return_t my_irq(uint32_t irq, void* dev_id) {
    my_sleep(100);  // CANNOT SLEEP IN IRQ!
    return IRQ_HANDLED;
}

// RIGHT: Use threaded IRQ or tasklet
```

### 5. Forgetting to Free Resources

```c
static int my_probe(device_t* dev) {
    void* buf = kmalloc(4096);
    
    int ret = do_something();
    if (ret < 0) {
        return ret;  // WRONG: buf leaked!
    }
    
    return 0;
}

// RIGHT:
static int my_probe(device_t* dev) {
    void* buf = kmalloc(4096);
    
    int ret = do_something();
    if (ret < 0) {
        kfree(buf);
        return ret;
    }
    
    return 0;
}
```

---

## Checklist

Before submitting a driver:

- [ ] Implements `probe()` and `remove()` callbacks
- [ ] Frees all resources in reverse order of allocation
- [ ] Checks all return values
- [ ] Uses appropriate DMA direction
- [ ] Enables bus mastering (PCI)
- [ ] Requests IRQ with correct flags
- [ ] Implements power management callbacks
- [ ] Uses reference counting (`device_get`/`device_put`)
- [ ] No sleeping in IRQ handlers
- [ ] DMA buffers properly aligned
- [ ] Registers cleared/disabled in remove
- [ ] Tested with QEMU or real hardware
- [ ] No DMA leaks (`dma_check_leaks()`)
- [ ] No IRQ storms (check `irq_print_stats()`)
- [ ] Added to appropriate Makefile
- [ ] Documentation updated

---

## Additional Resources

- **Driver Framework:** `docs/DRIVER_FRAMEWORK.md`
- **Expansion Plan:** `docs/DRIVER_EXPANSION_PLAN.md`
- **Existing Drivers:** `kernel/drivers/`
- **Test Framework:** `tests/drivers/driver_test_framework.h`

---

**Happy driver development!**
