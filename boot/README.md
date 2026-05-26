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
    void* framebuffer_addr;          // Linear framebuffer address
    uint32_t framebuffer_width;      // Width in pixels
    uint32_t framebuffer_height;     // Height in pixels
    uint32_t framebuffer_pitch;      // Bytes per scanline
    void* kernel_entry;              // Kernel entry point
} boot_info_t;
```

## Current Implementation Status

### ✅ Completed

- Boot info header structures
- UEFI entry point assembly
- Basic C loader skeleton
- memset utility function
- Kernel entry jump mechanism
- Build system (Makefile + linker script)

### 🚧 TODO (Marked in code)

1. **Get memory map from UEFI** (`loader.c:34`)
   - Call `EFI_BOOT_SERVICES->GetMemoryMap()`
   - Convert UEFI memory descriptors to our format
   - Mark usable vs reserved regions

2. **Setup graphics mode** (`loader.c:42`)
   - Query EFI Graphics Output Protocol (GOP)
   - Set desired resolution (or detect best available)
   - Get framebuffer address and format

3. **Load kernel from disk** (`loader.c:48`)
   - Use EFI Simple File System Protocol
   - Read kernel ELF binary from `\boot\kernel.elf`
   - Parse ELF headers and load segments to memory
   - Resolve kernel entry point address

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

Place `BOOTX64.EFI` in the ESP (EFI System Partition) at:
```
/EFI/BOOT/BOOTX64.EFI
```

Or in ISO structure:
```
iso/
└── EFI/
    └── BOOT/
        └── BOOTX64.EFI
```

## Next Steps

1. Implement UEFI service calls (memory map, GOP, file system)
2. Add ELF loader to properly load kernel segments
3. Setup identity mapping for kernel entry
4. Add error handling and diagnostic output
5. Test with OVMF (UEFI firmware for QEMU)

## References

- UEFI Specification 2.10
- [OSDev Wiki - UEFI](https://wiki.osdev.org/UEFI)
- [EFI Boot Services](https://uefi.org/specs/UEFI/2.10/07_Services_Boot_Services.html)
- [EFI Graphics Output Protocol](https://uefi.org/specs/UEFI/2.10/12_Protocols_Console_Support.html#efi-graphics-output-protocol)
