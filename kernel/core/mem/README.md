# Memory Management Subsystem

This directory contains the kernel's memory management components.

## Components

### heap.c - Kernel Heap Allocator

Simple slab allocator implementing kmalloc() and kfree() for dynamic kernel memory allocation.

**Features:**
- 16MB heap at virtual address 0xFFFFFFFF90000000
- First-fit allocation strategy
- Block coalescing to reduce fragmentation
- 16-byte alignment for all allocations

**Implementation Details:**
- Each allocation has a header (block_t) containing size, free status, and next pointer
- On allocation: searches for first free block large enough, splits if oversized
- On free: marks block as free and coalesces with adjacent free blocks
- Panics on out-of-memory conditions (no graceful degradation in Phase 1)

**Usage:**
```c
void* ptr = kmalloc(1024);  // Allocate 1KB
// Use memory...
kfree(ptr);  // Free memory
```

**Limitations (Phase 1):**
- No support for realloc()
- No memory statistics/debugging beyond heap_used counter
- Linear search (O(n) allocation time)
- No protection against double-free or use-after-free

**Future Enhancements (Phase 2+):**
- Add slab caching for common sizes
- Implement buddy allocator for larger blocks
- Add heap debugging (guard bytes, leak detection)
- Memory usage statistics and profiling
