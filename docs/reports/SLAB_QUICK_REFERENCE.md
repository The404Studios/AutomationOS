# Slab Allocator - Quick Reference

## For Kernel Developers

### Basic Usage (No Changes Required!)

```c
// Allocate memory (automatically uses slab for ≤4KB)
process_t* proc = kmalloc(sizeof(process_t));  // → kmalloc-256 slab
if (!proc) return -ENOMEM;

// Use the memory
proc->pid = 123;
strcpy(proc->name, "my_process");

// Free memory (automatically detected as slab)
kfree(proc);
```

### What Changed?

**Nothing!** Existing code works unchanged. The slab allocator is a transparent optimization.

### Size Classes and Routing

| Request Size     | Cache Used      | Objects/Page | PMM Calls Saved |
|------------------|-----------------|--------------|-----------------|
| 1-16 bytes       | kmalloc-16      | ~255         | **255x**        |
| 17-32 bytes      | kmalloc-32      | ~127         | **127x**        |
| 33-64 bytes      | kmalloc-64      | ~63          | **63x**         |
| 65-128 bytes     | kmalloc-128     | ~31          | **31x**         |
| 129-256 bytes    | kmalloc-256     | ~15          | **15x**         |
| 257-512 bytes    | kmalloc-512     | ~7           | **7x**          |
| 513-1024 bytes   | kmalloc-1024    | ~3           | **3x**          |
| 1025-2048 bytes  | kmalloc-2048    | ~1           | **1x**          |
| 2049-4096 bytes  | kmalloc-4096    | ~0           | **0x**          |
| >4096 bytes      | heap (fallback) | N/A          | —               |

### Common Kernel Objects

```c
// Small structs → 64B slab
typedef struct { int x, y; char name[32]; } widget_t;
widget_t* w = kmalloc(sizeof(widget_t));  // → kmalloc-64

// Process control blocks → 256B slab
process_t* p = kmalloc(sizeof(process_t));  // → kmalloc-256

// Network packets → 512B or 1024B slab
packet_t* pkt = kmalloc(1500);  // → kmalloc-2048

// Large buffers → heap allocator
char* bigbuf = kmalloc(16384);  // → heap (>4KB)
```

## For Performance Analysis

### View Cache Statistics

```c
#include "slab.h"

void debug_memory_usage(void) {
    kprintf("=== Memory Allocator Statistics ===\n");
    slab_dump();  // Per-cache utilization
}
```

**Output**:
```
[SLAB] ==== cache report ====
[SLAB]   'kmalloc-16' obj=16 inuse=50/255 slabs=1 (allocs=100 frees=50)
[SLAB]   'kmalloc-64' obj=64 inuse=120/126 slabs=2 (allocs=500 frees=380)
[SLAB]   'kmalloc-256' obj=256 inuse=5/15 slabs=1 (allocs=50 frees=45)
```

**Interpretation**:
- `inuse`: Currently allocated objects
- `total`: Capacity (inuse + free)
- `slabs`: 4KB pages backing this cache
- `allocs/frees`: Lifetime counters

**Health Check**:
- ✅ `allocs - frees == inuse` (no leaks)
- ✅ High utilization (`inuse/total > 50%`)
- ⚠️ Many slabs with low utilization → workload doesn't match cache size

### Benchmark Memory Performance

```c
#include "mem.h"

// Run the built-in benchmark
heap_slab_benchmark();
```

**Expected Output**:
```
[HEAP] ========== SLAB ALLOCATOR BENCHMARK ==========
[HEAP] --- Testing size 64 bytes ---
[HEAP]   REDUCTION FACTOR: 62x (slab vs heap-only)
[HEAP]   SUCCESS: Slab allocator achieved 62x reduction!
```

## For Debugging

### Common Issues

#### 1. Double-Free Detection
```
[SLAB] BLOCKED double/over free of 0xDEADBEEF in cache 'kmalloc-64'
```

**Cause**: `kfree(ptr)` called twice on the same pointer.

**Fix**: Check your cleanup logic. Use defensive patterns:
```c
kfree(ptr);
ptr = NULL;  // Prevent accidental double-free
```

#### 2. Cross-Cache Free
```
[SLAB] BLOCKED free of 0xCAFEBABE: belongs to 'kmalloc-64', freed via 'kmalloc-256'
```

**Cause**: Pointer corruption or freed via wrong cache.

**Fix**: This shouldn't happen with `kfree()` (auto-detects cache). Check for buffer overruns.

#### 3. Non-Slab Pointer
```
[SLAB] BLOCKED free of non-slab pointer 0xFFFF8000... (bad magic)
```

**Cause**: Freeing a heap or stack pointer via slab.

**Fix**: Use `kfree()` (auto-routing), not `slab_free()` directly.

### Enable Debug Logging

Edit `slab.c` and add verbose prints:

```c
void* slab_alloc(slab_cache_t* c) {
    // ... existing code ...
    kprintf("[SLAB] alloc from '%s': returning %p\n", c->name, obj);
    return obj;
}
```

## For Subsystem Maintainers

### Custom Size Classes (Advanced)

If your subsystem has a hot allocation path with a specific size:

```c
// In your_subsystem_init():
static slab_cache_t* my_widget_cache = NULL;

void widget_subsystem_init(void) {
    my_widget_cache = slab_cache_create("widget-42", 42, 8);
    if (!my_widget_cache) {
        kprintf("[WIDGET] Failed to create slab cache\n");
        return;
    }
}

widget_t* widget_alloc(void) {
    return slab_alloc(my_widget_cache);
}

void widget_free(widget_t* w) {
    slab_free(my_widget_cache, w);
}
```

**When to use**:
- You allocate/free >1000 objects of the same size
- Standard kmalloc size classes don't fit well
- You want subsystem-specific statistics

### Cleanup on Shutdown

```c
void widget_subsystem_shutdown(void) {
    slab_cache_destroy(my_widget_cache);
    my_widget_cache = NULL;
}
```

⚠️ **Warning**: Only call if you've freed all objects first!

## Architecture Overview

```
┌─────────────────────────────────────────────┐
│           kmalloc(size)                      │
└────────────────┬────────────────────────────┘
                 │
         ┌───────▼────────┐
         │ Size ≤ 4KB?    │
         └───┬────────┬───┘
            YES      NO
             │        │
     ┌───────▼───┐   │
     │ SLAB FAST │   │
     │   PATH    │   │
     │ O(1) pop  │   │
     │ from free │   │
     │   list    │   │
     └─────┬─────┘   │
           │         │
           ▼         ▼
    ┌──────────────────────┐
    │   Return pointer     │
    └──────────────────────┘
           │
           ▼
    ┌──────────────────────┐
    │      kfree(ptr)      │
    └──────────────────────┘
           │
    ┌──────▼──────────┐
    │ Read magic at   │
    │ page boundary   │
    └──────┬──────────┘
           │
     ┌─────▼─────┐
     │ SLAB_MAGIC?│
     └──┬────┬───┘
       YES   NO
        │     │
   ┌────▼──┐ │
   │ SLAB  │ │
   │ FREE  │ │
   └───────┘ │
             │
        ┌────▼──┐
        │ HEAP  │
        │ FREE  │
        └───────┘
```

## Performance Tips

### DO ✅
- Use `kmalloc()` for objects ≤4KB (automatic slab routing)
- Free allocations promptly (enables slot reuse)
- Batch allocate if possible (amortizes slab growth)
- Match allocation size to cache boundaries when possible

### DON'T ❌
- Don't call `slab_alloc()` directly (use `kmalloc()`)
- Don't hold allocations longer than needed (wastes slab slots)
- Don't allocate <16 bytes (wasteful, rounds up to 16)
- Don't free NULL pointers (harmless but wastes cycles)

### Optimization Examples

**Before** (inefficient):
```c
for (int i = 0; i < 1000; i++) {
    char* buf = kmalloc(100);  // → kmalloc-128 (28 bytes wasted)
    // ... use buf ...
    kfree(buf);
}
```

**After** (optimized):
```c
typedef struct { char data[64]; } packet_t;  // Exactly 64 bytes
for (int i = 0; i < 1000; i++) {
    packet_t* pkt = kmalloc(sizeof(packet_t));  // → kmalloc-64 (0 waste)
    // ... use pkt->data ...
    kfree(pkt);
}
```

**Savings**: 128B → 64B = **50% memory reduction**, **2x better cache density**

## Monitoring and Tuning

### Key Metrics

1. **Cache Hit Rate**: `allocs / (allocs that triggered slab_grow)`
   - Goal: >95% (most allocs reuse freed slots)

2. **Utilization**: `inuse / (slabs * slots_per_slab)`
   - Goal: >50% (not wasting backing pages)

3. **Fragmentation**: Number of slabs with low utilization
   - Goal: <10% of slabs underutilized

### Tuning Workflow

1. Run `slab_dump()` after representative workload
2. Identify underutilized caches (`inuse/total < 30%`)
3. Either:
   - Remove cache if rarely used
   - Merge with adjacent size class
   - Increase object size to fill page better

## Emergency Procedures

### Disable Slab Allocator

If slab allocator is causing issues:

```c
// In kernel/core/mem/heap.c, heap_init():
slab_enabled = false;  // Revert to heap-only allocation
```

Rebuild and reboot. System will use traditional heap for all allocations.

### Force OOM for Testing

```c
// Exhaust a specific cache
for (int i = 0; i < 100000; i++) {
    void* p = kmalloc(64);
    if (!p) {
        kprintf("OOM at iteration %d\n", i);
        break;
    }
}
```

Expected: Eventually returns NULL when PMM runs out.

## Further Reading

- **Full Documentation**: `SLAB_ALLOCATOR_IMPLEMENTATION.md`
- **Testing Guide**: `SLAB_VERIFICATION_CHECKLIST.md`
- **Implementation Details**: `kernel/core/mem/slab.c` (heavily commented)
- **Linux SLUB**: `Documentation/vm/slub.rst`

---

**Quick Start**: Just use `kmalloc()`/`kfree()` as always. The slab allocator works transparently! 🚀
