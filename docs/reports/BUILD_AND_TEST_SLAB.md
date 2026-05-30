# Build and Test Instructions - Slab Allocator

## Prerequisites

Ensure you have the AutomationOS toolchain installed:

```bash
# Check toolchain
which x86_64-elf-gcc
which nasm
which qemu-system-x86_64

# Expected versions
x86_64-elf-gcc --version  # Should show cross-compiler
nasm -v                    # Should show 2.14 or newer
```

## Build Process

### Step 1: Clean Previous Build

```bash
cd C:\Users\wilde\Desktop\Kernel  # Or your AutomationOS path
make clean
```

**Expected output**:
```
rm -rf build iso
```

### Step 2: Compile Kernel

```bash
make kernel
```

**Expected output** (abbreviated):
```
x86_64-elf-gcc -std=gnu11 -ffreestanding -nostdlib -nostdinc -mno-red-zone \
  -mcmodel=kernel -Wall -Wextra -O2 -Iinclude -isystem include/compat \
  -c kernel/core/mem/slab.c -o build/kernel/core/mem/slab.o

x86_64-elf-gcc ... -c kernel/core/mem/heap.c -o build/kernel/core/mem/heap.o

x86_64-elf-gcc ... -c kernel/kernel.c -o build/kernel/kernel.o

x86_64-elf-ld -T linker.ld -nostdlib -z max-page-size=0x1000 \
  -o build/kernel.elf <all .o files>
```

**Success criteria**:
- ✅ No compilation errors
- ✅ No warnings (except expected ones)
- ✅ `build/kernel.elf` created

**Common issues**:

| Error | Cause | Fix |
|-------|-------|-----|
| `slab.h: No such file` | File not in include/ | Check `kernel/include/slab.h` exists |
| `undefined reference to slab_alloc` | slab.c not compiled | Run `make clean && make kernel` |
| `-PAGE_SIZE` undeclared | Missing include | Verify `#include "../../include/kernel.h"` in slab.c |

### Step 3: Build ISO Image

```bash
make iso
```

**Expected output**:
```
Building kernel ISO...
Kernel size: 1234 KB
Creating ISO image at build/kernel.iso
```

**Success criteria**:
- ✅ `build/kernel.iso` created
- ✅ Size > 1 MB (includes kernel + initrd)

## Testing

### Test 1: Boot in QEMU

```bash
make qemu
```

**Watch for these boot messages** (in order):

#### A. Heap Initialization
```
[HEAP] Kernel heap initialized at 0xFFFFFFFF90000000 (16 MiB, block_t=64 bytes)
[HEAP] FINAL CHECK: heap_first=0xFFFFFFFF90000000, size=16777152, is_free=1
```

#### B. Slab Cache Creation
```
[HEAP] Initializing slab caches for common sizes...
[SLAB] cache 'kmalloc-16': obj=16 align=16 slots/slab=255
[SLAB] cache 'kmalloc-32': obj=32 align=16 slots/slab=127
[SLAB] cache 'kmalloc-64': obj=64 align=16 slots/slab=63
[SLAB] cache 'kmalloc-128': obj=128 align=16 slots/slab=31
[SLAB] cache 'kmalloc-256': obj=256 align=16 slots/slab=15
[SLAB] cache 'kmalloc-512': obj=512 align=16 slots/slab=7
[SLAB] cache 'kmalloc-1024': obj=1024 align=16 slots/slab=3
[SLAB] cache 'kmalloc-2048': obj=2048 align=16 slots/slab=1
[SLAB] cache 'kmalloc-4096': obj=4096 align=16 slots/slab=0
[HEAP] Slab caches initialized (9 caches)
```

**Verification**:
- ✅ All 9 caches created (kmalloc-16 through kmalloc-4096)
- ✅ Slots per slab matches expected values (see table below)

| Size | Expected Slots | Calculation |
|------|----------------|-------------|
| 16   | ~255          | (4096-72)/16 = 251 |
| 32   | ~127          | (4096-72)/32 = 125 |
| 64   | ~63           | (4096-72)/64 = 62  |
| 128  | ~31           | (4096-72)/128 = 31 |
| 256  | ~15           | (4096-72)/256 = 15 |

#### C. Slab Self-Test
```
[SLAB] cache 'selftest-64': obj=64 align=16 slots/slab=62
[SLAB] SELFTEST: PASS
```

**Critical**: Must see `PASS`. If `FAIL`, check error message:
- `FAIL cache_create returned NULL` → PMM out of memory (unlikely)
- `FAIL alloc X returned NULL` → Slab growth failed
- `FAIL duplicate ptr` → Object aliasing bug (critical!)
- `FAIL stamp clobbered` → Memory corruption (critical!)

#### D. Performance Benchmark
```
[HEAP] ========== SLAB ALLOCATOR BENCHMARK ==========
[HEAP] Running 10000 iterations per size class...

[HEAP] --- Testing size 64 bytes ---
[HEAP]   First alloc cost: 4096 bytes (1 pages)
[HEAP]   PMM delta after 10000 allocs: 8192 bytes (2 pages)
[HEAP]   Without slab (estimated): ~10000 pages
[HEAP]   REDUCTION FACTOR: 5000x (slab vs heap-only)
[HEAP]   SUCCESS: Slab allocator achieved 5000x reduction!

[HEAP] --- Testing size 256 bytes ---
[HEAP]   First alloc cost: 4096 bytes (1 pages)
[HEAP]   PMM delta after 10000 allocs: 12288 bytes (3 pages)
[HEAP]   Without slab (estimated): ~10000 pages
[HEAP]   REDUCTION FACTOR: 3333x (slab vs heap-only)
[HEAP]   SUCCESS: Slab allocator achieved 3333x reduction!

[HEAP] --- Testing size 1024 bytes ---
[HEAP]   First alloc cost: 4096 bytes (1 pages)
[HEAP]   PMM delta after 10000 allocs: 16384 bytes (4 pages)
[HEAP]   Without slab (estimated): ~10000 pages
[HEAP]   REDUCTION FACTOR: 2500x (slab vs heap-only)
[HEAP]   SUCCESS: Slab allocator achieved 2500x reduction!

[HEAP] ========== BENCHMARK COMPLETE ==========
```

**Success criteria**:
- ✅ All three size tests complete
- ✅ Reduction factor ≥ 10x for each size (typically 100x-5000x)
- ✅ No "OOM" or "NULL allocation" messages

**Interpreting results**:
- **High reduction (>1000x)**: Slab cache is very effective (many objects fit per page)
- **Moderate reduction (10-100x)**: Still good, larger objects = fewer per page
- **Low reduction (<10x)**: Warning! Something may be wrong

### Test 2: System Stability

After boot completes, the system should remain stable:

```
[SCHED] Scheduler initialized
[SCHED] Process 1 created: init
[SCHED] Process 2 created: shell
```

**Success criteria**:
- ✅ No kernel panics
- ✅ Processes can be created
- ✅ System responds to input

**Common issues**:

| Symptom | Likely Cause | Debug Step |
|---------|--------------|------------|
| Panic at first `kmalloc` | Slab cache not initialized | Check `slab_enabled = true` in heap_init() |
| Panic in `kfree` | Bad magic detection | Verify SLAB_MAGIC matches between slab.c and heap.c |
| OOM after many allocs | Slab not freeing empty pages | Check `slab_free()` returns pages to PMM |

### Test 3: Manual Verification (Optional)

If you want to verify slab usage manually:

1. **Add debug print in kmalloc**:
```c
// In kernel/core/mem/heap.c, kmalloc():
if (slab_enabled && size <= 4096) {
    for (int i = 0; i < NUM_SLAB_CACHES; i++) {
        if (size <= slab_sizes[i] && slab_caches[i]) {
            void* ptr = slab_alloc(slab_caches[i]);
            if (ptr) {
                kprintf("[SLAB] kmalloc(%lu) → slab-%lu: %p\n",
                        (unsigned long)size, (unsigned long)slab_sizes[i], ptr);
                return ptr;
            }
            break;
        }
    }
}
```

2. **Rebuild and boot**:
```bash
make clean && make iso && make qemu
```

3. **Expected output**:
```
[SLAB] kmalloc(64) → slab-64: 0xDEADBEEF
[SLAB] kmalloc(256) → slab-256: 0xCAFEBABE
```

4. **Remove debug print** after verification.

## Debugging Failed Tests

### If slab_selftest() fails:

1. **Enable verbose logging**:
```c
// In kernel/core/mem/slab.c, slab_alloc():
void* slab_alloc(slab_cache_t* c) {
    // ... existing code ...
    kprintf("[SLAB] alloc from '%s': obj=%p slab=%p\n", c->name, obj, s);
    return obj;
}
```

2. **Rebuild and check output**:
```bash
make clean && make iso && make qemu
```

3. **Look for**:
- Objects from different slabs (normal)
- Same object returned twice before free (BUG!)
- Null pointers (PMM OOM)

### If benchmark shows low reduction:

1. **Check PMM usage**:
```c
// In heap_slab_benchmark():
kprintf("[HEAP] PMM free memory: %lu MB\n",
        (unsigned long)(pmm_get_free_memory() / (1024*1024)));
```

2. **Possible causes**:
- PMM fragmentation → Slab can't get contiguous pages
- Memory leak → Freed objects not returned to PMM
- Slab not being used → `slab_enabled` is false

### If system panics in kfree:

1. **Check magic number consistency**:
```bash
grep "SLAB_MAGIC" kernel/core/mem/slab.c kernel/core/mem/heap.c
```

Both should show `0x51AB0BACE51AB0BULL`.

2. **Verify page alignment**:
```c
// In kfree():
kprintf("[KFREE] ptr=%p page_base=%p magic=0x%lx\n",
        ptr, (void*)page_base, *magic_ptr);
```

## Performance Profiling (Advanced)

### Measure Allocation Latency

```c
#include "include/x86_64.h"  // For rdtsc()

uint64_t start = rdtsc();
for (int i = 0; i < 1000; i++) {
    void* p = kmalloc(64);
    kfree(p);
}
uint64_t end = rdtsc();
kprintf("1000 alloc/free cycles: %llu CPU cycles\n", end - start);
```

**Expected** (rough estimates):
- With slab: ~100,000 cycles (100 cycles/op)
- Without slab: ~1,000,000 cycles (1000 cycles/op)

### Monitor Cache Statistics

Add to your test code:

```c
extern void slab_dump(void);

void print_memory_stats(void) {
    slab_dump();
    kprintf("PMM free: %lu MB\n",
            (unsigned long)(pmm_get_free_memory() / (1024*1024)));
}
```

Call after workload:
```
[SLAB] ==== cache report ====
[SLAB]   'kmalloc-64' obj=64 inuse=50/126 slabs=2 (allocs=500 frees=450)
[SLAB]   'kmalloc-256' obj=256 inuse=10/15 slabs=1 (allocs=100 frees=90)
```

## Regression Testing

### Before Merging to Main

1. **Run full test suite**:
```bash
make test
```

2. **Boot test**:
```bash
bash scripts/test-boot.sh
```

3. **Check for memory leaks**:
   - Boot system
   - Create/destroy 100 processes
   - Call `slab_dump()` → Check `allocs == frees + inuse`

4. **Stress test**:
```c
// Rapid alloc/free
for (int round = 0; round < 100; round++) {
    void* ptrs[1000];
    for (int i = 0; i < 1000; i++) ptrs[i] = kmalloc(64);
    for (int i = 0; i < 1000; i++) kfree(ptrs[i]);
}
```

**Expected**: No panics, PMM usage returns to baseline.

## Expected Results Summary

| Test | Expected Outcome |
|------|------------------|
| Build | Clean compile, 0 errors |
| Boot | Reaches scheduler init |
| Slab init | 9 caches created |
| Self-test | `SELFTEST: PASS` |
| Benchmark | ≥10x reduction for all sizes |
| Process creation | Works normally |
| Memory leaks | `slab_dump` shows balanced allocs/frees |

## Troubleshooting Quick Reference

| Symptom | Quick Fix |
|---------|-----------|
| Build error | `make clean && make kernel` |
| Missing slab.o | Check `kernel/core/mem/slab.c` exists |
| Panic in kmalloc | Verify `heap_init()` called first |
| Panic in kfree | Check SLAB_MAGIC constant matches |
| Low benchmark score | Enable debug prints, check PMM |
| Memory leak | Verify `slab_free()` returns pages to PMM |

## Next Steps After Successful Test

1. ✅ **Commit changes**:
```bash
git add kernel/core/mem/slab.c kernel/include/slab.h
git add kernel/core/mem/heap.c kernel/kernel.c kernel/include/mem.h
git commit -m "feat(kernel): implement slab allocator with 100x PMM reduction"
```

2. ✅ **Update documentation**:
   - Link to `SLAB_ALLOCATOR_IMPLEMENTATION.md` in README
   - Note performance improvements in changelog

3. ✅ **Monitor production**:
   - Add `slab_dump()` to periodic health checks
   - Watch for cache hit rates in logs

4. ⏳ **Future optimizations**:
   - Per-CPU caches (when SMP added)
   - Red zones (debug builds)
   - Profiling per-subsystem allocation patterns

---

**Ready to test?** Run:
```bash
make clean && make iso && make qemu
```

Watch for `[SLAB] SELFTEST: PASS` and `[HEAP] SUCCESS: Slab allocator achieved 100+x reduction!`
