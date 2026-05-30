# AutoBoot UEFI Bootloader

A custom UEFI bootloader for AutomationOS that loads the kernel and passes boot information.

## Architecture

### Entry Flow

1. **UEFI Firmware** loads `BOOTX64.EFI`
2. **boot.asm** (`_start`) receives UEFI handles and calls `efi_main`
3. **loader.c** (`efi_main`) queries UEFI services, loads kernel, jumps to kernel
4. **Kernel** receives `boot_info_t` structure with memory map and framebuffer

### Files

- `boot.h` - Boot info structures shared between bootloader and kernel
- `boot.asm` - UEFI entry point assembly (receives ImageHandle and SystemTable)
- `loader.c` - C bootloader implementation
- `boot.ld` - Linker script for UEFI PE executable
- `Makefile` - Build script

## Boot Info Structure

The bootloader passes this structure to the kernel:

```c
typedef struct {
    memory_map_entry_t* memory_map;  // Physical memory regions
    uint32_t memory_map_count;       // Number of entries
    uint64_t framebuffer_addr;       // Linear framebuffer address
    uint64_t framebuffer_size;       // Framebuffer size in bytes
    uint32_t framebuffer_width;      // Width in pixels
    uint32_t framebuffer_height;     // Height in pixels
    uint32_t framebuffer_pitch;      // Bytes per scanline
    uint32_t pixels_per_scanline;    // Pixels per scanline
    void* kernel_entry;              // Kernel entry point
} boot_info_t;
```

## Implementation Status

### ✅ Phase 1: COMPLETE (2026-05-26)

**All bootloader functionality implemented and ready for production:**

- ✅ Boot info header structures
- ✅ UEFI entry point assembly
- ✅ Full UEFI protocol implementations
- ✅ UEFI memory map query and conversion
- ✅ Graphics Output Protocol (GOP) setup
- ✅ ELF kernel loading from ESP
- ✅ ELF64 parsing and segment loading
- ✅ Boot services exit and kernel handoff
- ✅ Error handling and retry logic
- ✅ memset/memcpy utility functions
- ✅ Build system (Makefile + linker script)
- ✅ Testing scripts and documentation

**Key Features:**
- Queries UEFI for complete system memory map
- Configures framebuffer via Graphics Output Protocol
- Loads 64-bit ELF kernels from disk
- Properly exits UEFI boot services
- Passes comprehensive boot_info to kernel

See `IMPLEMENTATION.md` for detailed technical documentation.
See `VERIFICATION.md` for testing and verification procedures.

## Building

Requires:
- `x86_64-elf-gcc` (cross-compiler)
- `nasm` (assembler)
- `x86_64-elf-ld` (linker)

```bash
make
```

Output: `../build/BOOTX64.EFI`

## Testing

### Quick Test (QEMU with OVMF)

```bash
./test-qemu.sh
```

### Manual Testing

Place `BOOTX64.EFI` and `KERNEL.ELF` in the ESP (EFI System Partition):
```
ESP/
└── EFI/
    └── BOOT/
        ├── BOOTX64.EFI  (bootloader)
        └── KERNEL.ELF   (kernel)
```

Boot in UEFI mode (not Legacy/CSM).

### Expected Output

```
AutomationOS UEFI Bootloader
=============================
[1/4] Querying memory map...
  Memory map acquired: N usable regions
[2/4] Setting up graphics...
  Graphics mode: WxH
[3/4] Loading kernel...
  Kernel size: NNNN bytes
  Kernel loaded
[4/4] Parsing ELF...
  Entry point: 0xXXXXXXXXXXXXXXXX

Exiting boot services...
[Kernel takes over]
```

## Implementation Details

### UEFI Protocols Used
- **EFI_BOOT_SERVICES:** Memory allocation, protocol location
- **EFI_GRAPHICS_OUTPUT_PROTOCOL:** Display mode configuration
- **EFI_SIMPLE_FILE_SYSTEM_PROTOCOL:** Disk access
- **EFI_FILE_PROTOCOL:** File I/O operations
- **EFI_LOADED_IMAGE_PROTOCOL:** Boot device information

### Memory Map
- Queries UEFI memory map via GetMemoryMap()
- Filters for EfiConventionalMemory (usable RAM)
- Converts to simplified boot_info format
- Handles two-phase allocation pattern

### Graphics Setup
- Locates Graphics Output Protocol
- Extracts current mode information
- Captures framebuffer base address
- Falls back to safe defaults if GOP unavailable

### ELF Loading
- Accesses boot device filesystem
- Reads kernel from \EFI\BOOT\KERNEL.ELF
- Validates ELF64 magic and headers
- Loads PT_LOAD segments to memory
- Zero-initializes BSS sections
- Extracts entry point address

### Boot Handoff
- Exits UEFI boot services (with retry)
- Disables UEFI interrupts
- Jumps to kernel entry point
- Passes boot_info via RDI (System V ABI)

## Next Steps (Phase 2)

1. ✅ Bootloader complete - proceed to kernel initialization
2. Kernel should validate boot_info
3. Initialize memory management with provided map
4. Initialize framebuffer driver with GOP info
5. Set up proper page tables (UEFI's are temporary)

## References

- UEFI Specification 2.10
- [OSDev Wiki - UEFI](https://wiki.osdev.org/UEFI)
- [EFI Boot Services](https://uefi.org/specs/UEFI/2.10/07_Services_Boot_Services.html)
- [EFI Graphics Output Protocol](https://uefi.org/specs/UEFI/2.10/12_Protocols_Console_Support.html#efi-graphics-output-protocol)
