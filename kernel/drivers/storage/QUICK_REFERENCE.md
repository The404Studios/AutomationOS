# AHCI Driver Quick Reference

## Quick Start

### 1. Initialize
```c
#include "ahci.h"
#include "block.h"

block_init();    // Initialize block layer
ahci_init();     // Initialize AHCI driver
block_list_devices();  // List detected devices
```

### 2. Read Data
```c
// Get first AHCI port
ahci_port_t* port = &g_ahci_controller->ports[0];
uint8_t* buffer = kmalloc(512);

// Read 1 sector from LBA 0
if (ahci_read_sectors(port, 0, 1, buffer)) {
    kprintf("Success!\n");
}

kfree(buffer);
```

### 3. Write Data
```c
uint8_t* buffer = kmalloc(512);
memset(buffer, 0xAA, 512);

// Write 1 sector to LBA 100
if (ahci_write_sectors(port, 100, 1, buffer)) {
    kprintf("Write successful\n");
}

kfree(buffer);
```

### 4. Using Block Device Layer
```c
block_device_t* dev = /* get from registration */;

// Read
block_read(dev, lba, count, buffer);

// Write
block_write(dev, lba, count, buffer);

// Flush
block_flush(dev);
```

## Common Operations

### Read Multiple Sectors
```c
uint32_t sector_count = 16;
uint8_t* buffer = kmalloc(sector_count * 512);
ahci_read_sectors(port, lba, sector_count, buffer);
kfree(buffer);
```

### Write with NCQ
```c
// NCQ automatically used for large transfers
uint8_t* buffer = kmalloc(32 * 512);  // 32 sectors
ahci_write_sectors_ncq(port, lba, 32, buffer);
kfree(buffer);
```

### Flush Cache
```c
ahci_flush_cache(port);  // Force write to disk
```

### Check Device Info
```c
kprintf("Model: %s\n", port->model);
kprintf("Sectors: %llu\n", port->sectors);
kprintf("NCQ: %s\n", port->supports_ncq ? "Yes" : "No");
kprintf("Queue Depth: %u\n", port->queue_depth);
```

## Error Checking

```c
if (!ahci_read_sectors(port, lba, count, buffer)) {
    kprintf("Read failed: port error count = %u\n", 
            port->error_count);
    kprintf("Last error: 0x%08x\n", port->last_error);
}
```

## Performance Tips

1. **Use NCQ for large transfers** (> 8 sectors)
2. **Batch multiple operations** before flushing
3. **Align buffers** to 4KB boundaries
4. **Avoid small random writes** (prefer sequential)

## Register Dumps

### Controller Info
```c
kprintf("Controller at BAR5: 0x%llx\n", 
        (uint64_t)controller->abar);
kprintf("Capabilities: 0x%08x\n", 
        controller->abar->cap);
kprintf("Ports Implemented: 0x%08x\n", 
        controller->ports_implemented);
```

### Port Status
```c
kprintf("Port %u Status:\n", port->port_num);
kprintf("  CMD: 0x%08x\n", port->regs->cmd);
kprintf("  SSTS: 0x%08x\n", port->regs->ssts);
kprintf("  TFD: 0x%08x\n", port->regs->tfd);
kprintf("  SIG: 0x%08x\n", port->regs->sig);
```

## Debugging

### Enable Verbose Logging
```c
#define AHCI_DEBUG 1
```

### Check Command Progress
```c
kprintf("CI: 0x%08x (commands issued)\n", port->regs->ci);
kprintf("SACT: 0x%08x (NCQ active)\n", port->regs->sact);
kprintf("Slot bitmap: 0x%08x\n", port->slot_bitmap);
```

### Monitor Statistics
```c
kprintf("Total reads: %llu\n", controller->total_reads);
kprintf("Total writes: %llu\n", controller->total_writes);
kprintf("Total errors: %llu\n", controller->total_errors);
```

## QEMU Testing

```bash
# Create disk
qemu-img create -f qcow2 test.img 1G

# Run with AHCI
qemu-system-x86_64 \
    -drive file=test.img,if=none,id=hd0 \
    -device ahci,id=ahci \
    -device ide-hd,drive=hd0,bus=ahci.0 \
    -kernel kernel.bin -m 512M
```

## Common Issues

| Problem | Solution |
|---------|----------|
| No controller found | Check PCI init, BIOS AHCI mode |
| Device not detected | Verify SATA connection, check port implemented |
| Command timeout | Increase timeout, check DMA addresses |
| Data corruption | Verify alignment, check cache coherency |

## Memory Requirements

- Command List: 1KB per port
- RX FIS: 256 bytes per port
- Command Tables: 8KB per port
- **Total per port**: ~10KB

## Constants

```c
#define AHCI_MAX_PORTS 32
#define AHCI_MAX_CMD_SLOTS 32
#define AHCI_TIMEOUT_MS 1000
#define SECTOR_SIZE 512
```

## Key Structures

```c
ahci_controller_t* controller;  // Main controller
ahci_port_t* port;              // Per-port structure
block_device_t* dev;            // Block device
```

## Function Reference

| Function | Purpose |
|----------|---------|
| `ahci_init()` | Initialize driver |
| `ahci_read_sectors()` | Read (non-NCQ) |
| `ahci_write_sectors()` | Write (non-NCQ) |
| `ahci_read_sectors_ncq()` | Read (NCQ) |
| `ahci_write_sectors_ncq()` | Write (NCQ) |
| `ahci_flush_cache()` | Sync to disk |
| `block_read()` | Generic read |
| `block_write()` | Generic write |
| `block_flush()` | Generic flush |

## Example: Boot Sector Read

```c
// Read MBR
uint8_t* mbr = kmalloc(512);
if (ahci_read_sectors(port, 0, 1, mbr)) {
    if (mbr[510] == 0x55 && mbr[511] == 0xAA) {
        kprintf("Valid boot sector\n");
    }
}
kfree(mbr);
```

## Example: Sequential Write

```c
uint8_t* buffer = kmalloc(8192);  // 16 sectors

for (uint64_t lba = 0; lba < 1000; lba += 16) {
    // Fill buffer
    for (int i = 0; i < 8192; i++) {
        buffer[i] = (lba + i) & 0xFF;
    }
    
    // Write
    ahci_write_sectors(port, lba, 16, buffer);
}

ahci_flush_cache(port);
kfree(buffer);
```

## Example: Random Read

```c
uint8_t* buffer = kmalloc(4096);

for (int i = 0; i < 100; i++) {
    uint64_t random_lba = rand() % port->sectors;
    ahci_read_sectors(port, random_lba, 8, buffer);
    // Process data...
}

kfree(buffer);
```

---

**Quick Ref Version**: 1.0  
**Last Updated**: 2026-05-26
