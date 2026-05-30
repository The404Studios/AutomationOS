# HDA Driver Build and Integration Guide

## Build Configuration

### Makefile Integration

Add the following to your kernel Makefile:

```makefile
# HDA Audio driver object files
HDA_OBJS = kernel/drivers/hda.o \
           kernel/drivers/hda_stream.o \
           kernel/drivers/hda_test.o

# Add to kernel objects
KERNEL_OBJS += $(HDA_OBJS)

# Compilation rules
kernel/drivers/hda.o: kernel/drivers/hda.c kernel/include/hda.h
	$(CC) $(CFLAGS) -c $< -o $@

kernel/drivers/hda_stream.o: kernel/drivers/hda_stream.c kernel/include/hda.h
	$(CC) $(CFLAGS) -c $< -o $@

kernel/drivers/hda_test.o: kernel/drivers/hda_test.c kernel/include/hda.h
	$(CC) $(CFLAGS) -c $< -o $@
```

### Compiler Flags

Recommended flags for HDA driver:

```makefile
CFLAGS = -Wall -Wextra -Werror \
         -ffreestanding -nostdlib -nostdinc \
         -mcmodel=kernel -mno-red-zone \
         -mno-sse -mno-sse2 -mno-sse3 -mno-mmx \
         -O2 -g
```

**Important**: The `-mno-sse*` flags prevent the compiler from using SSE instructions in kernel code, which could corrupt FPU state.

### Dependencies

The HDA driver requires:

1. **PCI Subsystem** (`kernel/include/pci.h`)
   - PCI device enumeration
   - BAR mapping
   - Bus mastering enable

2. **Memory Management** (`kernel/include/mem.h`)
   - Physical page allocator (`pmm_alloc_page`, `pmm_free_page`)
   - Kernel heap (`kmalloc`, `kfree`)

3. **Interrupt Handling** (`kernel/include/x86_64.h`)
   - IRQ registration (`irq_register_handler`)

4. **Timer** (`kernel/include/drivers.h`)
   - Delay functions (`timer_get_ticks`, `timer_get_frequency`)

5. **Serial Output** (`kernel/include/drivers.h`)
   - Debug logging (`serial_write`, `serial_putchar`)

## Kernel Integration

### 1. Early Boot Initialization

In your kernel main function (`kernel/main.c`):

```c
#include "drivers.h"
#include "pci.h"
#include "mem.h"

void kernel_main(void) {
    // Initialize basic subsystems
    serial_init();
    pmm_init(memory_map, memory_map_count);
    vmm_init();
    heap_init();
    
    // Initialize PCI
    pci_init();
    
    // Initialize timer (required for delays)
    pit_init(1000);  // 1000 Hz
    
    // Initialize HDA audio
    hda_init();
    
    // Run audio tests (optional)
    // hda_run_tests();
    
    // ... rest of kernel init ...
}
```

### 2. Interrupt Setup

The HDA driver requires interrupt support. Ensure your IDT is configured:

```c
// In kernel/interrupts.c or equivalent

#include "hda.h"

// IRQ handlers array
irq_handler_t irq_handlers[16];

void irq_register_handler(uint8_t irq, irq_handler_t handler) {
    irq_handlers[irq] = handler;
}

// Common IRQ handler (called by assembly stub)
void irq_common_handler(uint8_t irq) {
    if (irq_handlers[irq]) {
        irq_handlers[irq]();
    }
    
    // Send EOI to PIC/APIC
    if (irq >= 8) {
        outb(0xA0, 0x20);  // Slave PIC
    }
    outb(0x20, 0x20);      // Master PIC
}
```

### 3. PCI Initialization

Ensure PCI enumeration is working:

```c
// In kernel/pci.c

void pci_init(void) {
    // Scan all PCI buses (0-255)
    for (uint16_t bus = 0; bus < 256; bus++) {
        for (uint8_t device = 0; device < 32; device++) {
            for (uint8_t function = 0; function < 8; function++) {
                uint16_t vendor_id = pci_config_read_word(bus, device, function, PCI_CONFIG_VENDOR_ID);
                
                if (vendor_id == 0xFFFF) {
                    continue;  // No device
                }
                
                // Read device info
                uint16_t device_id = pci_config_read_word(bus, device, function, PCI_CONFIG_DEVICE_ID);
                uint8_t class_code = pci_config_read_byte(bus, device, function, PCI_CONFIG_CLASS_CODE);
                uint8_t subclass = pci_config_read_byte(bus, device, function, PCI_CONFIG_SUBCLASS);
                
                // Store device in list for later lookup
                // ...
            }
        }
    }
}
```

### 4. Memory Mapping

Ensure PCI BAR mapping works with your VMM:

```c
// In kernel/mem.c or pci.c

uint64_t pci_get_bar(pci_device_t* dev, uint8_t bar_num) {
    uint32_t bar = pci_config_read_dword(dev->bus, dev->device, dev->function,
                                          PCI_CONFIG_BAR0 + (bar_num * 4));
    
    if (bar & 1) {
        // I/O space BAR
        return bar & 0xFFFFFFFC;
    } else {
        // Memory space BAR
        if ((bar & 0x6) == 0x4) {
            // 64-bit BAR
            uint32_t bar_high = pci_config_read_dword(dev->bus, dev->device, dev->function,
                                                       PCI_CONFIG_BAR0 + ((bar_num + 1) * 4));
            return ((uint64_t)bar_high << 32) | (bar & 0xFFFFFFF0);
        } else {
            // 32-bit BAR
            return bar & 0xFFFFFFF0;
        }
    }
}
```

**Important**: For HDA, BAR0 must be mapped as **uncached** memory (write-through or write-combining), not cached. This ensures MMIO writes are visible to the hardware immediately.

## Testing in QEMU

### Basic Test

```bash
qemu-system-x86_64 \
  -kernel automationos.elf \
  -device intel-hda \
  -device hda-duplex \
  -audiodev pa,id=snd0 \
  -device intel-hda,audiodev=snd0 \
  -serial stdio \
  -m 512M
```

### With Debugging

```bash
qemu-system-x86_64 \
  -kernel automationos.elf \
  -device intel-hda \
  -device hda-duplex \
  -audiodev pa,id=snd0,server=/run/user/1000/pulse/native \
  -serial stdio \
  -m 512M \
  -d int,cpu_reset \
  -D qemu.log
```

### Expected QEMU Output

```
HDA: Found HD Audio controller
HDA: MMIO base at 0xFEBF8000
HDA: Resetting controller
HDA: Controller reset complete
HDA: Version 1.0
HDA: Streams - Output: 4, Input: 4
HDA: Initializing CORB/RIRB
HDA: CORB/RIRB initialized
HDA: Enumerating codecs
HDA: STATESTS = 0x0001
HDA: Found codec at address 0
HDA: Vendor ID = 0x10EC0888
HDA: Found AFG at NID 1
HDA: Read 34 widgets
HDA: Found DAC at NID 2
HDA: Found output pin at NID 20
HDA: Output path configured
HDA: Found 1 codec(s)
HDA: Initialization complete
```

## Real Hardware Testing

### Prerequisites

1. **Disable Secure Boot** (some systems may interfere)
2. **Enable HDA in BIOS** (Audio Controller)
3. **Load kernel via GRUB or bootloader**

### Supported Chipsets

- Intel 5/6/7/8/9/100/200/300/400/500 Series Chipsets
- AMD FCH Azalia (most AM4/AM5 platforms)
- NVIDIA MCP HDA (older chipsets)

### Debug Output

Connect a serial cable or use QEMU's serial output to see HDA initialization logs:

```bash
# View serial output in QEMU
-serial stdio

# Or redirect to file
-serial file:hda_debug.log
```

### Common Issues

1. **No MMIO base address**:
   - Check PCI BAR0 is valid (not 0x0)
   - Verify memory space is enabled in PCI command register

2. **Controller reset timeout**:
   - Wait longer (increase timeout to 5 seconds)
   - Check for conflicting drivers (disable in BIOS if possible)

3. **No codecs detected** (STATESTS = 0x0000):
   - Increase delay after controller reset (100ms → 500ms)
   - Some codecs are slow to initialize
   - Check for audio disabled in BIOS

4. **Audio output but no sound**:
   - Check physical cable connections
   - Verify speaker/headphone enabled in BIOS
   - Try different output jack (front vs rear)
   - Check volume/mute settings

## Performance Considerations

### DMA Buffer Size

Default: 64 KB (16 pages)

```c
#define HDA_DMA_BUFFER_SIZE (16 * 4096)  // 64 KB
```

**Trade-offs**:
- Smaller buffers: Lower latency, more CPU overhead (more interrupts)
- Larger buffers: Higher latency, less CPU overhead

**Recommendations**:
- Real-time audio (games): 16-32 KB
- Music playback: 64-128 KB
- Background audio: 256 KB+

### Interrupt Coalescing

The driver generates an interrupt for each BDL entry. Reduce entries to lower interrupt rate:

```c
stream->bdl_entries = 4;  // 4 interrupts per buffer (16KB chunks)
```

Or disable interrupts on most entries:

```c
for (uint32_t i = 0; i < stream->bdl_entries; i++) {
    stream->bdl_virt[i].address = ...;
    stream->bdl_virt[i].length = ...;
    stream->bdl_virt[i].ioc = (i == stream->bdl_entries - 1) ? 1 : 0;  // Only last entry
}
```

### Memory Allocation

The driver requires:
- 1 page (4KB) for CORB
- 1 page (4KB) for RIRB
- N pages for each stream buffer (default: 16 pages = 64KB)
- 1 page (4KB) for each stream's BDL

**Total**: ~22 pages (88 KB) for single-stream playback.

## Advanced Configuration

### Multiple Codecs

If your system has multiple codecs (e.g., onboard + HDMI):

```c
// Enumerate all codecs
for (uint8_t i = 0; i < ctrl->num_codecs; i++) {
    hda_codec_t* codec = ctrl->codecs[i];
    printf("Codec %d: Vendor 0x%08X\n", i, codec->vendor_id);
}

// Use specific codec
hda_codec_t* hdmi_codec = ctrl->codecs[1];
```

### Custom Sample Rates

For non-standard rates, calculate format register manually:

```c
// 96 kHz = 48 kHz * 2
uint16_t format = HDA_FMT_BASE_48KHZ | HDA_FMT_MULT(1) | HDA_FMT_BITS_16 | HDA_FMT_CHAN(1);

// 22.05 kHz = 44.1 kHz / 2
uint16_t format = HDA_FMT_BASE_44KHZ | HDA_FMT_DIV(1) | HDA_FMT_BITS_16 | HDA_FMT_CHAN(1);
```

### High-Resolution Audio

24-bit, 192 kHz:

```c
uint16_t format = HDA_FMT_BASE_48KHZ | HDA_FMT_MULT(3) | HDA_FMT_BITS_24 | HDA_FMT_CHAN(1);
hda_stream_setup(ctrl, stream, 192000, 24, 2);
```

**Note**: Not all codecs support 192 kHz. Check `HDA_PARAM_PCM_SIZE_RATE` parameter.

## Troubleshooting Build Issues

### Undefined Reference Errors

```
undefined reference to `pmm_alloc_page'
```

**Solution**: Ensure memory management is linked:

```makefile
KERNEL_OBJS = kernel/mem.o kernel/pmm.o kernel/vmm.o kernel/heap.o
```

### Implicit Function Declaration

```
warning: implicit declaration of function 'hda_msleep'
```

**Solution**: Add `extern` or move function definition above usage:

```c
// Add to hda.c before first use
extern void hda_msleep(uint32_t ms);
```

Or make it `static` and define at top of file.

### Linker Script Issues

If HDA driver is not being linked, add to linker script:

```ld
/* In kernel.ld */
.text : {
    *(.text)
    kernel/drivers/*.o(.text)  /* Ensure driver text sections are included */
}
```

## Integration Checklist

- [ ] PCI subsystem initialized before `hda_init()`
- [ ] Physical memory allocator working (`pmm_alloc_page`)
- [ ] Kernel heap initialized (`kmalloc` available)
- [ ] Timer running (`timer_get_ticks` works)
- [ ] Interrupts enabled (IDT configured)
- [ ] Serial output working (for debug logs)
- [ ] HDA driver files compiled and linked
- [ ] QEMU test successful (see initialization messages)
- [ ] Real hardware test (if available)

## Next Steps

After successful integration:

1. **Add userspace API**:
   - Syscalls for audio playback/recording
   - Device file interface (`/dev/audio`)

2. **Implement audio mixing**:
   - Allow multiple applications to play audio
   - Software mixer for combining streams

3. **Add ALSA compatibility**:
   - Emulate ALSA API for Linux application compatibility

4. **Advanced features**:
   - Jack detection events
   - HDMI audio output
   - S/PDIF digital output

---

**END OF BUILD GUIDE**
