# ELF Loader Quick Start

**Goal:** Load and execute userspace programs in ring 3

## Prerequisites Checklist

- [ ] PMM working (`pmm_alloc_page()` returns pages)
- [ ] VMM working (`vmm_map_page()` maps pages)
- [ ] Heap working (`kmalloc(64)` returns non-NULL)
- [ ] GDT initialized (`gdt_init()` called)
- [ ] TSS initialized (`tss_init()` called)
- [ ] Initrd loaded and accessible

## 3-Minute Test

```c
// In your kernel_main() after initialization:
#include "../include/elf_loader_test.h"

void kernel_main(void) {
    // ... pmm_init(), vmm_init(), heap_init(), etc ...
    
    // Quick test (30 seconds)
    elf_loader_test_suite(1);  // Test heap
    
    // Full safe tests (2 minutes)
    elf_loader_test_suite(0);  // All tests except ring 3
    
    // DANGER: Enters ring 3, doesn't return
    // elf_loader_test_suite(6);
}
```

## Build Userspace Test Program

```bash
cd userspace
make tests
# Creates: bin/test_minimal
```

## Add to Initrd

```bash
# Option 1: Tar-based initrd
mkdir -p initrd_files
cp userspace/bin/test_minimal initrd_files/
tar -C initrd_files -cf initrd.tar test_minimal

# Option 2: Use your existing initrd build script
./scripts/build-initrd.sh
```

## Verify

```bash
# Check test program is valid ELF
file userspace/bin/test_minimal
# Should say: ELF 64-bit LSB executable, x86-64

# Check entry point
readelf -h userspace/bin/test_minimal | grep Entry
# Should be in user space (e.g., 0x400000)

# Check in initrd
tar -tf initrd.tar | grep test_minimal
# Should list: test_minimal
```

## Expected Output

### If heap not working:
```
[TEST 1] Heap Allocation Test
[FAIL] kmalloc(64) returned NULL - heap not working!
```
→ Fix heap first

### If everything works:
```
[TEST 1] Heap Allocation Test
[PASS] kmalloc(64) = 0x...
[PASS] Heap working correctly

[TEST 5] ELF Load (Dry Run)
[ELF] Loading ELF: test_minimal
[ELF] Load complete: entry=0x400000 stack=0x7FFFFFFFE000
[PASS] ELF loaded successfully
```
→ Ready for ring 3!

### After running test 6:
```
[TEST 6] ELF Load and Execute (Ring 3)
[USERMODE] Switching to user mode...

Hello from Ring 3!
```
→ SUCCESS! Usermode working!

## Troubleshooting

| Problem | Solution |
|---------|----------|
| Test 1 fails | Fix heap: check `heap_init()`, PMM, VMM |
| Test 3 fails | test_minimal not in initrd - rebuild initrd |
| Test 4 fails | Call `gdt_init()` and `tss_init()` |
| Test 5 fails | Out of memory - check PMM has free pages |
| Test 6 triple faults | Check GDT user segments, page mappings |
| Test 6 hangs | Syscall handlers not implemented |

## Files You Need

| File | Purpose |
|------|---------|
| `kernel/fs/elf_loader.c` | Already exists |
| `kernel/arch/x86_64/usermode.asm` | Already exists |
| `kernel/fs/elf_loader_test.c` | ✅ NEW - test suite |
| `userspace/test_minimal.c` | ✅ NEW - test program |

## Full Documentation

See `ELF_USERMODE_IMPLEMENTATION_GUIDE.md` for complete details.

## One-Liner Test

```c
elf_loader_test_suite(0);  // Run all safe tests
```

If this prints all `[PASS]`, you're ready for ring 3!
