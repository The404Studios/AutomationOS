# AHCI/SATA Driver Integration Guide

## Executive Summary

This document provides step-by-step instructions for integrating the AHCI/SATA driver into AutomationOS kernel. The driver provides production-grade SATA III storage support with NCQ, hot-plug detection, and comprehensive error handling.

**Deliverables:**
- AHCI driver core (1500+ LOC)
- Block device layer
- Test suite
- Integration documentation

**Timeline:** Complete and ready for integration

## Prerequisites

### Required Kernel Components

1. **PCI Subsystem**
   - `pci_init()`
   - `pci_find_class()`
   - `pci_config_read_*() / pci_config_write_*()`
   - `pci_enable_bus_master()`
   - `pci_enable_memory_space()`
   - `pci_get_bar()`

2. **Memory Management**
   - `kmalloc() / kfree()`
   - Physical memory allocation (for DMA buffers)
   - Page-aligned allocations

3. **Timer Subsystem**
   - `timer_get_ticks()`
   - `timer_get_frequency()`
   - `timer_sleep()`

4. **Kernel Printf**
   - `kprintf()` for logging
   - `ksnprintf()` for string formatting (optional)

### Verify Prerequisites

```c
// Test PCI enumeration
pci_device_t* dev = pci_find_class(0x01, 0x06, 0x01);
if (dev) {
    kprintf("AHCI controller found: %04x:%04x\n", 
            dev->vendor_id, dev->device_id);
}

// Test memory allocation
void* test_mem = kmalloc(1024);
if (test_mem) {
    kfree(test_mem);
    kprintf("Memory allocation working\n");
}

// Test timer
uint64_t start = timer_get_ticks();
timer_sleep(100);
uint64_t elapsed = timer_get_ticks() - start;
kprintf("Timer working, elapsed ticks: %llu\n", elapsed);
```

## Step 1: Add Header Files

### Location
Place these headers in `kernel/include/`:

1. **ahci.h** - AHCI driver interface
2. **block.h** - Block device layer interface

### Verification
```bash
ls kernel/include/ahci.h kernel/include/block.h
```

## Step 2: Add Source Files

### Location
Create directory and place source files:

```bash
mkdir -p kernel/drivers/storage/
```

Place these files in `kernel/drivers/storage/`:
1. **ahci.c** - Core AHCI driver
2. **block.c** - Block device layer
3. **ahci_block.c** - AHCI-to-block adapter
4. **ahci_test.c** - Test suite

### Verification
```bash
ls kernel/drivers/storage/
# Expected: ahci.c block.c ahci_block.c ahci_test.c README.md
```

## Step 3: Update Build System

### Add to Makefile

```makefile
# Storage drivers
STORAGE_DIR = kernel/drivers/storage
STORAGE_OBJS = $(STORAGE_DIR)/ahci.o \
               $(STORAGE_DIR)/block.o \
               $(STORAGE_DIR)/ahci_block.o \
               $(STORAGE_DIR)/ahci_test.o

# Add to kernel objects
KERNEL_OBJS += $(STORAGE_OBJS)
```

### Compilation Flags

If using specific optimizations:
```makefile
$(STORAGE_DIR)/%.o: $(STORAGE_DIR)/%.c
	$(CC) $(CFLAGS) -O2 -c $< -o $@
```

### Verify Build

```bash
make clean
make
# Check for ahci.o, block.o, ahci_block.o, ahci_test.o in build output
```

## Step 4: Update Driver Interface

### Edit kernel/include/drivers.h

Add these declarations:

```c
// Storage subsystem
void block_init(void);
void ahci_init(void);
void ahci_run_tests(void);
bool ahci_register_block_device(ahci_port_t* port, uint8_t port_num);

// Block device operations
void block_list_devices(void);
```

### Optional: Add AHCI structure to global namespace

If you need external access to controller:

```c
// In ahci.h or drivers.h
extern ahci_controller_t* g_ahci_controller;
```

## Step 5: Update Kernel Initialization

### Edit kernel/kernel.c (or equivalent main file)

Add initialization sequence:

```c
#include "drivers.h"
#include "ahci.h"
#include "block.h"

void kernel_main(void) {
    // ... existing initialization ...
    
    // PCI must be initialized first
    pci_init();
    
    // Initialize block device layer
    kprintf("Initializing block device layer...\n");
    block_init();
    
    // Initialize AHCI/SATA driver
    kprintf("Initializing AHCI/SATA driver...\n");
    ahci_init();
    
    // Optional: Run tests
    #ifdef AHCI_RUN_TESTS
    kprintf("Running AHCI test suite...\n");
    ahci_run_tests();
    #endif
    
    // List detected storage devices
    kprintf("Listing storage devices...\n");
    block_list_devices();
    
    // ... continue with rest of initialization ...
}
```

### Conditional Compilation

To enable tests only in debug builds:

```c
// In kernel config or Makefile
#ifdef DEBUG
#define AHCI_RUN_TESTS 1
#endif
```

## Step 6: PCI Implementation Check

### Verify PCI Functions Exist

The AHCI driver requires these PCI functions. Verify they're implemented:

```c
// In pci.c or equivalent
uint32_t pci_config_read_dword(uint8_t bus, uint8_t device, 
                               uint8_t function, uint8_t offset);

void pci_config_write_dword(uint8_t bus, uint8_t device, 
                            uint8_t function, uint8_t offset, 
                            uint32_t value);

void pci_enable_bus_master(pci_device_t* dev) {
    uint16_t cmd = pci_config_read_word(dev->bus, dev->device, 
                                        dev->function, PCI_CONFIG_COMMAND);
    cmd |= PCI_COMMAND_BUS_MASTER;
    pci_config_write_word(dev->bus, dev->device, dev->function, 
                          PCI_CONFIG_COMMAND, cmd);
}

void pci_enable_memory_space(pci_device_t* dev) {
    uint16_t cmd = pci_config_read_word(dev->bus, dev->device, 
                                        dev->function, PCI_CONFIG_COMMAND);
    cmd |= PCI_COMMAND_MEMORY_SPACE;
    pci_config_write_word(dev->bus, dev->device, dev->function, 
                          PCI_CONFIG_COMMAND, cmd);
}

uint64_t pci_get_bar(pci_device_t* dev, uint8_t bar_num) {
    uint32_t bar_offset = PCI_CONFIG_BAR0 + (bar_num * 4);
    uint32_t bar_low = pci_config_read_dword(dev->bus, dev->device, 
                                             dev->function, bar_offset);
    
    // Check if 64-bit BAR
    if ((bar_low & PCI_BAR_64BIT) && bar_num < 5) {
        uint32_t bar_high = pci_config_read_dword(dev->bus, dev->device,
                                                   dev->function, 
                                                   bar_offset + 4);
        return ((uint64_t)bar_high << 32) | (bar_low & ~0xF);
    }
    
    return bar_low & ~0xF;
}
```

### If PCI Functions Are Missing

Create `kernel/drivers/pci_helpers.c`:

```c
#include "pci.h"
#include "kernel.h"

void pci_enable_bus_master(pci_device_t* dev) {
    uint16_t cmd = pci_config_read_word(dev->bus, dev->device, 
                                        dev->function, PCI_CONFIG_COMMAND);
    cmd |= PCI_COMMAND_BUS_MASTER;
    pci_config_write_word(dev->bus, dev->device, dev->function, 
                          PCI_CONFIG_COMMAND, cmd);
    kprintf("[PCI] Enabled bus mastering for %02x:%02x.%x\n",
            dev->bus, dev->device, dev->function);
}

// ... implement other helpers ...
```

Add to Makefile:
```makefile
KERNEL_OBJS += kernel/drivers/pci_helpers.o
```

## Step 7: Memory Considerations

### Alignment Requirements

AHCI requires specific memory alignment:
- Command List: 1KB aligned
- Received FIS: 256-byte aligned
- Command Tables: 128-byte aligned

### Update Heap Allocator (if needed)

If your `kmalloc()` doesn't guarantee alignment, add:

```c
void* kmalloc_aligned(size_t size, size_t alignment) {
    void* ptr = kmalloc(size + alignment - 1);
    if (!ptr) return NULL;
    
    uintptr_t addr = (uintptr_t)ptr;
    uintptr_t aligned = (addr + alignment - 1) & ~(alignment - 1);
    return (void*)aligned;
}
```

Or use existing page allocator:

```c
// In ahci.c, replace kmalloc calls:
port->cmd_list = (ahci_cmd_header_t*)pmm_alloc_page(); // 4KB page
```

## Step 8: Virtual Memory Mapping

### Physical Address Access

AHCI uses physical addresses for DMA. If you have virtual memory:

```c
// In ahci.c, update BAR mapping:
uint64_t abar_phys = pci_get_bar(pci_dev, 5);

// Map to virtual memory
controller->abar = (ahci_hba_mem_t*)vmm_map_device(abar_phys, 4096);
```

Implement `vmm_map_device()` if needed:

```c
void* vmm_map_device(uint64_t phys_addr, size_t size) {
    // Map physical address to kernel virtual space
    // Use identity mapping or dedicated device region
    return (void*)(uintptr_t)phys_addr; // Identity mapping
}
```

### Buffer Physical Addresses

For DMA buffers:

```c
// In ahci.c, get physical address for DMA:
void* virt_buffer = kmalloc(512);
uint64_t phys_buffer = vmm_get_physical(virt_buffer);

// Use phys_buffer in PRD table
cmd_table->prdt[0].dba = phys_buffer;
```

## Step 9: Testing

### QEMU Configuration

Create `run_ahci_test.sh`:

```bash
#!/bin/bash

# Create test disk if it doesn't exist
if [ ! -f test_disk.img ]; then
    qemu-img create -f qcow2 test_disk.img 1G
fi

# Run QEMU with AHCI
qemu-system-x86_64 \
    -kernel build/kernel.bin \
    -drive file=test_disk.img,if=none,id=hd0 \
    -device ahci,id=ahci \
    -device ide-hd,drive=hd0,bus=ahci.0 \
    -m 512M \
    -serial stdio \
    -no-reboot \
    -no-shutdown
```

### Run Tests

```bash
chmod +x run_ahci_test.sh
./run_ahci_test.sh
```

### Expected Console Output

```
[KERNEL] AutomationOS booting...
[PCI] Initializing PCI subsystem...
[PCI] Scanning PCI bus...
[BLOCK] Initializing block device layer...
[AHCI] Initializing AHCI/SATA driver...
[AHCI] Found AHCI controller: 8086:2922
[AHCI] Capabilities:
  Ports: 6
  Command Slots: 32
  64-bit: Yes
  NCQ: Yes
  Interface Speed: Gen 3
[AHCI] Initializing port 0...
[AHCI] Port 0: SATA drive detected
[AHCI] Port 0: Device initialized successfully
  Model: QEMU HARDDISK
  Serial: QM00001
  Firmware: 2.5+
  Sectors: 2097152 (1024 MB)
  NCQ: Yes (depth 32)
[AHCI] AHCI driver initialized successfully
[BLOCK] Registered device: sata0 (2097152 sectors, 512 bytes/sector)

========================================
  AHCI/SATA Driver Test Suite
========================================
[TEST] Using port 0 for testing

[TEST] Basic Read/Write Test
[TEST] Writing 512 bytes to LBA 1000...
[TEST] Reading 512 bytes from LBA 1000...
[TEST] PASSED: Data verified successfully

[TEST] Multi-Sector Read/Write Test
[TEST] Writing 16 sectors to LBA 2000...
[TEST] Reading 16 sectors from LBA 2000...
[TEST] PASSED: 16 sectors verified

... (more tests) ...

========================================
  Test Results: 6/6 passed
========================================
```

## Step 10: Real Hardware Testing

### Supported Controllers

Tested with:
- Intel ICH9 AHCI (QEMU emulation)
- Intel 6/7/8 Series SATA controllers
- AMD SB series SATA controllers

### BIOS Configuration

Ensure SATA mode is set to **AHCI** (not IDE or RAID):
1. Enter BIOS/UEFI setup
2. Navigate to SATA configuration
3. Set SATA mode to "AHCI"
4. Save and reboot

### Boot Process

1. Burn kernel to bootable USB/CD
2. Boot on target machine
3. Watch serial console or screen output
4. Verify AHCI controller detection
5. Check device enumeration

## Step 11: Integration with Filesystem (Future)

### Block Device Interface

Once filesystems are implemented:

```c
// Example: Read boot sector
block_device_t* boot_dev = block_devices[0];
uint8_t* mbr = kmalloc(512);

if (block_read(boot_dev, 0, 1, mbr)) {
    // Parse MBR
    if (mbr[510] == 0x55 && mbr[511] == 0xAA) {
        kprintf("Valid MBR found\n");
        // Parse partition table...
    }
}
```

### Partition Table Support

Future enhancement:

```c
typedef struct {
    uint8_t bootable;
    uint8_t start_chs[3];
    uint8_t type;
    uint8_t end_chs[3];
    uint32_t start_lba;
    uint32_t sector_count;
} __attribute__((packed)) mbr_partition_t;

void parse_mbr(block_device_t* dev) {
    uint8_t* mbr = kmalloc(512);
    block_read(dev, 0, 1, mbr);
    
    mbr_partition_t* partitions = (mbr_partition_t*)(mbr + 446);
    for (int i = 0; i < 4; i++) {
        if (partitions[i].type != 0) {
            kprintf("Partition %d: type=0x%02x, LBA=%u, sectors=%u\n",
                    i, partitions[i].type, partitions[i].start_lba,
                    partitions[i].sector_count);
        }
    }
    
    kfree(mbr);
}
```

## Troubleshooting

### Problem: No AHCI Controller Found

**Symptoms:**
```
[AHCI] No AHCI controller found
```

**Solutions:**
1. Verify PCI enumeration: `pci_init()` called before `ahci_init()`
2. Check BIOS SATA mode (must be AHCI)
3. Verify PCI class code search: 01:06:01
4. Check QEMU device configuration

### Problem: Port Initialization Failed

**Symptoms:**
```
[AHCI] Port 0: Failed to stop command engine
```

**Solutions:**
1. Verify timer functions work correctly
2. Check timeout values (increase if needed)
3. Ensure BAR5 is properly mapped
4. Check for conflicting drivers

### Problem: Command Timeout

**Symptoms:**
```
[AHCI] Port 0: Command timeout (slot 0)
```

**Solutions:**
1. Verify DMA buffer physical addresses
2. Check command FIS structure
3. Ensure bus mastering is enabled
4. Verify interrupts (if using interrupt mode)
5. Check SATA cable/device connection

### Problem: Data Corruption

**Symptoms:**
```
[TEST] FAILED: Data mismatch
```

**Solutions:**
1. Verify cache coherency (flush CPU caches)
2. Check PRD table setup (byte count, address)
3. Ensure proper alignment of buffers
4. Verify physical address translation
5. Check for buffer overruns

### Problem: Build Errors

**Symptoms:**
```
undefined reference to `pci_enable_bus_master'
```

**Solutions:**
1. Implement missing PCI functions (see Step 6)
2. Check Makefile includes all .o files
3. Verify header paths are correct
4. Check for circular dependencies

## Performance Tuning

### NCQ Optimization

Adjust NCQ threshold in `ahci_block.c`:

```c
// Use NCQ for transfers > N sectors
#define NCQ_THRESHOLD 8

if (port->supports_ncq && count > NCQ_THRESHOLD) {
    return ahci_read_sectors_ncq(port, lba, count, buffer);
}
```

### Command Queue Depth

Increase queue depth for high-performance SSDs:

```c
// In ahci.h
#define AHCI_MAX_CMD_SLOTS 32  // Keep at 32 (spec maximum)
```

### Timeout Adjustment

For slow HDDs, increase timeouts:

```c
// In ahci.h
#define AHCI_TIMEOUT_MS 5000  // 5 seconds instead of 1
```

## Success Criteria

✅ AHCI controller detected  
✅ SATA devices enumerated  
✅ Read/write operations successful  
✅ NCQ operations working (if supported)  
✅ Error handling tested  
✅ Hot-plug detection functional  
✅ All test suite tests pass  

## Next Steps

After successful integration:

1. **Add filesystem support** (ext4, FAT32)
2. **Implement partition parsing** (MBR, GPT)
3. **Add boot loader support** (boot from SATA)
4. **Implement SMART monitoring**
5. **Add SSD TRIM support**
6. **Integrate with VFS layer**

## References

- **AHCI Specification**: See AHCI_README.md
- **AutomationOS Driver Expansion Plan**: `docs/DRIVER_EXPANSION_PLAN.md`
- **Block Device Layer**: See `kernel/include/block.h`

## Support

For issues or questions:
1. Check the troubleshooting section above
2. Review test suite output for diagnostics
3. Enable AHCI_DEBUG for verbose logging
4. Consult AHCI specification for register details

---

**Document Version**: 1.0  
**Last Updated**: 2026-05-26  
**Status**: Ready for Integration  
**Estimated Integration Time**: 2-4 hours
