# AutomationOS Driver Framework

**Version:** 2.0  
**Status:** Production  
**Last Updated:** 2026-05-26

## Overview

The AutomationOS driver framework provides a modern, unified infrastructure for device drivers inspired by the Linux driver model. It features a hierarchical device tree, automatic driver binding, hot-plug support, comprehensive power management, and DMA/IRQ abstractions.

## Architecture

### Core Components

```
Driver Framework
├── Device Model       - Unified device representation
├── Bus Infrastructure - Device enumeration and matching
├── Driver Registration - Driver lifecycle management
├── DMA Framework      - Memory allocation and mapping
├── IRQ Framework      - Interrupt handling
├── Power Management   - Suspend/resume, runtime PM
└── Hot-Plug Support   - Dynamic device add/remove
```

### Key Design Principles

1. **Unified Device Model:** All hardware represented by `struct device`
2. **Bus Abstraction:** Platform, PCI, USB, I2C, SPI buses
3. **Automatic Binding:** Devices and drivers matched automatically
4. **Lifecycle Management:** Probe, remove, suspend, resume callbacks
5. **Resource Management:** Reference counting prevents use-after-free
6. **Power Awareness:** Runtime PM reduces idle power consumption
7. **DMA Safety:** Type-safe APIs prevent addressing errors
8. **Interrupt Efficiency:** Threaded IRQs, softirqs, tasklets

---

## Device Model

### Device Structure

Every hardware device is represented by `struct device`:

```c
typedef struct device {
    char name[64];                    // Device name (e.g., "nvme0")
    char* path;                       // Sysfs path ("/sys/devices/pci0/0000:00:1f.2/nvme0")
    struct device* parent;            // Parent device
    struct device* children;          // First child
    struct device* sibling;           // Next sibling
    struct bus_type* bus;             // Bus type (pci_bus, usb_bus, etc.)
    struct driver* driver;            // Bound driver
    void* driver_data;                // Driver private data
    void* platform_data;              // Platform-specific data

    // Device state
    device_state_t state;             // UNINITIALIZED, PROBING, ACTIVE, SUSPENDED, etc.
    device_power_state_t power_state; // D0 (on), D1/D2 (sleep), D3 (off)
    uint32_t flags;                   // REGISTERED, PROBED, SUSPENDED, etc.

    // Power management
    const device_pm_ops_t* pm_ops;
    bool can_wakeup;
    bool should_wakeup;
    uint64_t runtime_idle_time;
    uint64_t runtime_suspend_time;

    // Reference counting
    uint32_t refcount;

    // Device ID for matching
    uint32_t device_id;

    // Lock for concurrent access
    void* lock;  // spinlock_t

    // List node for bus device list
    struct device* bus_next;
} device_t;
```

### Device Lifecycle

```
         device_alloc()
              |
              v
      [UNINITIALIZED]
              |
      device_register()
              |
              v
       [REGISTERED] ----> driver_attach()
              |                  |
              |                  v
              |          device_match_driver()
              |                  |
              |                  v
              |           driver_bind()
              |                  |
              |                  v
              |          driver->probe()
              |                  |
              v                  v
         [ACTIVE] <----- [PROBED]
              |
              |--- device_pm_suspend() --> [SUSPENDED]
              |                                  |
              |<--- device_pm_resume() ----------
              |
              v
    device_unregister()
              |
              v
       [REMOVING] ----> driver->remove()
              |
              v
        [REMOVED]
              |
        device_free()
```

### Device Tree

Devices form a hierarchical tree:

```
root
├── pci0 (PCI host bridge)
│   ├── 0000:00:1f.2 (AHCI controller)
│   │   ├── ata0 (SATA port 0)
│   │   │   └── sda (disk)
│   │   └── ata1 (SATA port 1)
│   │       └── sr0 (DVD drive)
│   ├── 0000:00:1c.0 (PCIe root port)
│   │   └── 0000:01:00.0 (NVMe controller)
│   │       └── nvme0 (NVMe device)
│   │           ├── nvme0n1 (namespace 1)
│   │           └── nvme0n2 (namespace 2)
│   └── 0000:00:14.0 (xHCI USB controller)
│       ├── usb1 (USB 3.0 root hub)
│       │   ├── usb1-1 (USB keyboard)
│       │   └── usb1-2 (USB storage)
│       └── usb2 (USB 2.0 root hub)
│           └── usb2-1 (USB mouse)
└── platform
    ├── serial0 (UART)
    └── rtc0 (Real-Time Clock)
```

### Device API

#### Device Allocation

```c
device_t* device_alloc(const char* name);
void device_free(device_t* dev);
```

Creates a new device structure. Must call `device_register()` to add to system.

#### Device Registration

```c
int device_register(device_t* dev);
void device_unregister(device_t* dev);
```

Registers device with the system. Triggers automatic driver binding.

#### Device Tree Operations

```c
void device_set_parent(device_t* dev, device_t* parent);
device_t* device_get_parent(device_t* dev);
void device_add_child(device_t* parent, device_t* child);
void device_remove_child(device_t* parent, device_t* child);
```

Manages parent-child relationships.

#### Device Naming

```c
int device_set_name(device_t* dev, const char* fmt, ...);
const char* device_get_name(device_t* dev);
```

Sets device name with printf-style formatting.

#### Reference Counting

```c
void device_get(device_t* dev);  // Increment refcount
void device_put(device_t* dev);  // Decrement, free if zero
```

Prevents premature deletion of devices.

#### Driver Data

```c
void device_set_driver_data(device_t* dev, void* data);
void* device_get_driver_data(device_t* dev);
```

Stores driver private data.

---

## Driver Model

### Driver Structure

```c
typedef struct driver {
    const char* name;                 // Driver name
    struct bus_type* bus;             // Bus type

    // Driver lifecycle callbacks
    int (*probe)(device_t* dev);
    int (*remove)(device_t* dev);
    void (*shutdown)(device_t* dev);

    // Power management callbacks
    const device_pm_ops_t* pm_ops;

    // Device matching
    const void* match_table;          // Bus-specific match table
    int (*match)(device_t* dev, struct driver* drv);

    // Priority for device binding
    uint32_t priority;

    // Module owner (for loadable modules)
    void* owner;

    // List node for bus driver list
    struct driver* bus_next;
} driver_t;
```

### Driver Registration

```c
int driver_register(driver_t* drv);
void driver_unregister(driver_t* drv);
```

Example:

```c
static driver_t my_driver = {
    .name = "my_device_driver",
    .bus = &pci_bus_type,
    .probe = my_probe,
    .remove = my_remove,
    .pm_ops = &my_pm_ops,
    .priority = 100,
};

int my_driver_init(void) {
    return driver_register(&my_driver);
}
```

### Probe and Remove

```c
static int my_probe(device_t* dev) {
    // Allocate driver private data
    my_device_data_t* priv = kmalloc(sizeof(my_device_data_t));
    if (!priv) return -ENOMEM;
    
    // Initialize hardware
    my_hardware_init(dev);
    
    // Store private data
    device_set_driver_data(dev, priv);
    
    kprintf("[MYDRV] Probed device: %s\n", dev->name);
    return 0;
}

static int my_remove(device_t* dev) {
    my_device_data_t* priv = device_get_driver_data(dev);
    
    // Shutdown hardware
    my_hardware_shutdown(dev);
    
    // Free resources
    kfree(priv);
    
    kprintf("[MYDRV] Removed device: %s\n", dev->name);
    return 0;
}
```

---

## Bus Infrastructure

### Bus Type Structure

```c
typedef struct bus_type {
    const char* name;

    // Device and driver lists
    device_t* devices;
    driver_t* drivers;

    // Bus operations
    int (*match)(device_t* dev, driver_t* drv);
    int (*probe)(device_t* dev);
    int (*remove)(device_t* dev);
    void (*shutdown)(device_t* dev);

    // Power management
    int (*suspend)(device_t* dev);
    int (*resume)(device_t* dev);

    // Lock for bus lists
    void* lock;
} bus_type_t;
```

### Bus Registration

```c
int bus_register(bus_type_t* bus);
void bus_unregister(bus_type_t* bus);
```

### Standard Buses

- **PCI Bus:** `pci_bus_type`
- **USB Bus:** `usb_bus_type`
- **Platform Bus:** `platform_bus_type` (for SoC devices)

---

## DMA Framework

### DMA Allocation Types

1. **Coherent (Consistent) Memory:** CPU and device see the same data without explicit sync
2. **Streaming Mappings:** Requires explicit sync before CPU/device access
3. **Scatter-Gather:** Transfer data from/to multiple non-contiguous buffers

### Coherent DMA

```c
void* dma_alloc_coherent(device_t* dev, size_t size, uint64_t* dma_handle, uint32_t flags);
void dma_free_coherent(device_t* dev, size_t size, void* cpu_addr, uint64_t dma_handle);
```

Example:

```c
// Allocate 4KB coherent buffer
uint64_t dma_addr;
void* buffer = dma_alloc_coherent(dev, 4096, &dma_addr, 0);

// Use buffer (CPU can write, device can read)
*(uint32_t*)buffer = 0xDEADBEEF;

// Give DMA address to device
my_device_set_buffer_addr(dma_addr);

// Free when done
dma_free_coherent(dev, 4096, buffer, dma_addr);
```

### Streaming DMA

```c
uint64_t dma_map_single(device_t* dev, void* ptr, size_t size, dma_direction_t dir);
void dma_unmap_single(device_t* dev, uint64_t dma_addr, size_t size, dma_direction_t dir);

void dma_sync_single_for_cpu(device_t* dev, uint64_t dma_addr, size_t size, dma_direction_t dir);
void dma_sync_single_for_device(device_t* dev, uint64_t dma_addr, size_t size, dma_direction_t dir);
```

Example:

```c
// Map existing buffer for DMA
uint8_t buffer[4096];
uint64_t dma_addr = dma_map_single(dev, buffer, 4096, DMA_TO_DEVICE);

// Device reads from buffer
my_device_start_transfer(dma_addr, 4096);

// Wait for transfer to complete
my_device_wait_completion();

// Unmap
dma_unmap_single(dev, dma_addr, 4096, DMA_TO_DEVICE);
```

### Scatter-Gather DMA

```c
int dma_sg_alloc(dma_sg_table_t* sgt, uint32_t num_entries);
void dma_sg_free(dma_sg_table_t* sgt);
void dma_sg_init_entry(dma_sg_table_t* sgt, uint32_t index, void* buf, uint32_t len);
int dma_map_sg(device_t* dev, dma_sg_table_t* sgt, dma_direction_t dir);
void dma_unmap_sg(device_t* dev, dma_sg_table_t* sgt, dma_direction_t dir);
```

Example:

```c
// Allocate scatter-gather table
dma_sg_table_t sgt;
dma_sg_alloc(&sgt, 4);

// Initialize entries
dma_sg_init_entry(&sgt, 0, buffer1, 1024);
dma_sg_init_entry(&sgt, 1, buffer2, 2048);
dma_sg_init_entry(&sgt, 2, buffer3, 4096);
dma_sg_init_entry(&sgt, 3, buffer4, 512);

// Map for DMA
dma_map_sg(dev, &sgt, DMA_TO_DEVICE);

// Pass to device
for (uint32_t i = 0; i < sgt.num_entries; i++) {
    my_device_add_sg_entry(sgt.entries[i].dma_addr, sgt.entries[i].length);
}

// Unmap after transfer
dma_unmap_sg(dev, &sgt, DMA_TO_DEVICE);

// Free
dma_sg_free(&sgt);
```

### DMA Pools

For frequently allocated small buffers (e.g., descriptor rings):

```c
dma_pool_t* dma_pool_create(const char* name, device_t* dev, size_t size,
                           size_t align, size_t boundary);
void dma_pool_destroy(dma_pool_t* pool);
void* dma_pool_alloc(dma_pool_t* pool, uint64_t* dma_handle);
void dma_pool_free(dma_pool_t* pool, void* cpu_addr, uint64_t dma_handle);
```

Example:

```c
// Create pool for 64-byte descriptors
dma_pool_t* pool = dma_pool_create("nvme_cmd_pool", dev, 64, 64, 0);

// Allocate descriptor
uint64_t dma_addr;
nvme_cmd_t* cmd = dma_pool_alloc(pool, &dma_addr);

// Use descriptor
cmd->opcode = NVME_CMD_READ;
cmd->nsid = 1;

// Free
dma_pool_free(pool, cmd, dma_addr);

// Destroy pool when driver unloads
dma_pool_destroy(pool);
```

### DMA Addressing Constraints

```c
void dma_set_mask(device_t* dev, uint64_t mask);
uint64_t dma_get_mask(device_t* dev);
```

Common masks:
- `DMA_MASK_24BIT` (16 MB) - ISA devices
- `DMA_MASK_32BIT` (4 GB) - Legacy PCI
- `DMA_MASK_64BIT` (full) - Modern PCIe

---

## IRQ Framework

### IRQ Handler Registration

```c
int request_irq(uint32_t irq, irq_handler_t handler, uint32_t flags,
               const char* name, void* dev_id);
void free_irq(uint32_t irq, void* dev_id);
```

Example:

```c
static irq_return_t my_irq_handler(uint32_t irq, void* dev_id) {
    my_device_t* dev = (my_device_t*)dev_id;
    
    // Read interrupt status
    uint32_t status = my_device_read_isr(dev);
    
    if (status == 0) {
        return IRQ_NONE;  // Not our interrupt
    }
    
    // Handle interrupt
    if (status & MY_IRQ_TX_COMPLETE) {
        my_handle_tx_complete(dev);
    }
    if (status & MY_IRQ_RX_READY) {
        my_handle_rx_ready(dev);
    }
    
    // Clear interrupt
    my_device_write_isr(dev, status);
    
    return IRQ_HANDLED;
}

// In probe:
int ret = request_irq(dev->irq, my_irq_handler, IRQ_FLAG_SHARED, "my_device", dev);
```

### Threaded IRQs

For handlers that need to sleep (e.g., I2C transactions):

```c
int request_threaded_irq(uint32_t irq, irq_handler_t handler,
                        irq_thread_fn_t thread_fn, uint32_t flags,
                        const char* name, void* dev_id);
```

Example:

```c
static irq_return_t my_fast_handler(uint32_t irq, void* dev_id) {
    // Fast path: just check if it's our interrupt
    if (my_device_check_interrupt(dev_id)) {
        return IRQ_WAKE_THREAD;  // Wake threaded handler
    }
    return IRQ_NONE;
}

static irq_return_t my_threaded_handler(uint32_t irq, void* dev_id) {
    my_device_t* dev = (my_device_t*)dev_id;
    
    // Slow path: can sleep here
    my_device_process_data(dev);  // May sleep
    
    return IRQ_HANDLED;
}

// Register
request_threaded_irq(dev->irq, my_fast_handler, my_threaded_handler,
                    IRQ_FLAG_SHARED, "my_device", dev);
```

### MSI/MSI-X

```c
int msi_enable(device_t* dev);
void msi_disable(device_t* dev);
int msix_enable(device_t* dev, uint32_t num_vectors);
void msix_disable(device_t* dev);
```

### Softirqs and Tasklets

For deferred processing outside IRQ context:

```c
void tasklet_init(tasklet_t* t, void (*func)(uint64_t), uint64_t data);
void tasklet_schedule(tasklet_t* t);
```

---

## Power Management

### Device PM Operations

```c
typedef struct {
    int (*suspend)(device_t* dev);
    int (*resume)(device_t* dev);
    int (*runtime_suspend)(device_t* dev);
    int (*runtime_resume)(device_t* dev);
    int (*prepare)(device_t* dev);
    void (*complete)(device_t* dev);
} device_pm_ops_t;
```

### System Sleep States

- **S3 (Suspend to RAM):** `device_pm_suspend_all()` -> suspend all devices
- **S4 (Hibernate):** Save state to disk, power off
- **S5 (Soft Off):** Full shutdown

### Runtime Power Management

Automatically suspends idle devices:

```c
// In probe:
device_pm_runtime_enable(dev);
device_pm_runtime_set_autosuspend_delay(dev, 2000);  // 2 seconds

// When device is used:
device_pm_runtime_get(dev);  // Resume if suspended
my_device_do_operation(dev);
device_pm_runtime_put_autosuspend(dev);  // Mark idle, auto-suspend after delay
```

### Wake-up Events

```c
device_pm_enable_wakeup(dev);  // Allow device to wake system
```

---

## Hot-Plug Support

### Device Events

```c
typedef enum {
    DEVICE_EVENT_ADD = 1,
    DEVICE_EVENT_REMOVE,
    DEVICE_EVENT_CHANGE,
    DEVICE_EVENT_ONLINE,
    DEVICE_EVENT_OFFLINE
} device_event_t;
```

### Event Callbacks

```c
typedef void (*device_event_callback_t)(device_t* dev, device_event_t event);

int device_register_event_callback(device_event_callback_t callback);
void device_unregister_event_callback(device_event_callback_t callback);
```

Example:

```c
static void my_device_event_handler(device_t* dev, device_event_t event) {
    switch (event) {
        case DEVICE_EVENT_ADD:
            kprintf("[MYDRV] Device added: %s\n", dev->name);
            break;
        case DEVICE_EVENT_REMOVE:
            kprintf("[MYDRV] Device removed: %s\n", dev->name);
            // Cleanup any references
            break;
    }
}

device_register_event_callback(my_device_event_handler);
```

### Hot-Plug Flow

```
USB device plugged in
       |
       v
USB hub detects connection
       |
       v
device_alloc("usb1-1")
       |
       v
device_register()
       |
       v
device_notify_event(DEVICE_EVENT_ADD)
       |
       v
driver_attach() finds matching driver
       |
       v
driver->probe() initializes device
       |
       v
Device is ready

USB device unplugged
       |
       v
USB hub detects disconnect
       |
       v
device_notify_event(DEVICE_EVENT_REMOVE)
       |
       v
driver->remove() shuts down device
       |
       v
device_unregister()
       |
       v
device_free()
```

---

## Best Practices

### Resource Management

1. **Always free resources in reverse order of allocation**
2. **Use reference counting:** `device_get()` / `device_put()`
3. **Check return values:** All allocation functions can fail
4. **Use DMA pools** for frequently allocated buffers

### Locking

1. **IRQ handlers:** No sleeping, use spinlocks
2. **Threaded handlers:** Can sleep, use mutexes
3. **Softirqs/tasklets:** No sleeping
4. **Process context:** Can sleep

### Error Handling

```c
int my_probe(device_t* dev) {
    int ret;
    
    my_data_t* priv = kmalloc(sizeof(my_data_t));
    if (!priv) {
        ret = -ENOMEM;
        goto err_alloc;
    }
    
    ret = my_init_hardware(dev);
    if (ret < 0) {
        goto err_hw_init;
    }
    
    ret = request_irq(dev->irq, my_handler, 0, "mydev", dev);
    if (ret < 0) {
        goto err_irq;
    }
    
    device_set_driver_data(dev, priv);
    return 0;

err_irq:
    my_shutdown_hardware(dev);
err_hw_init:
    kfree(priv);
err_alloc:
    return ret;
}
```

### Power Management

1. **Always implement suspend/resume** if hardware supports it
2. **Use runtime PM** for idle devices
3. **Wake-up sources:** Enable for keyboards, network cards

---

## Performance Optimization

### DMA

- Use **coherent memory** for control structures (small, frequently accessed)
- Use **streaming mappings** for data buffers (large, infrequent)
- Use **scatter-gather** for network/storage to avoid memcpy

### IRQs

- Keep IRQ handlers **short and fast**
- Use **threaded IRQs** for slow operations
- Use **tasklets** for batch processing
- Enable **MSI-X** for multi-queue devices (one IRQ per CPU)

### Power

- Enable **runtime PM** for all devices
- Set appropriate **autosuspend delays** (not too short, causes thrashing)
- Use **wake-up sources** sparingly

---

## Future Enhancements

### Phase 4 (Planned)

- **IOMMU Support:** Secure DMA remapping, isolation
- **Device Tree (DT) Support:** ARM SoC device enumeration
- **ACPI Integration:** Full ACPI power management
- **Tracepoints:** Kernel event tracing (eBPF-like)
- **Driver Versioning:** ABI stability
- **Module Auto-loading:** Load drivers on device hotplug

---

## References

- **Linux Driver Model:** [kernel.org/doc/driver-model](https://www.kernel.org/doc/html/latest/driver-api/driver-model/)
- **DMA API:** [kernel.org/doc/dma-api](https://www.kernel.org/doc/html/latest/core-api/dma-api.html)
- **Device Tree:** [devicetree.org](https://www.devicetree.org/)
- **ACPI Specification:** [uefi.org/specifications](https://uefi.org/specifications)

---

**End of Driver Framework Documentation**
