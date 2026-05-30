# SMP Integration Guide

This guide explains how to integrate the SMP implementation into AutomationOS.

## Prerequisites

Before integrating SMP support, ensure you have:

1. ✅ ACPI support (for CPU detection)
2. ✅ IDT (Interrupt Descriptor Table) setup
3. ✅ Memory management (PMM, VMM, Heap)
4. ✅ Basic interrupt handling

## Step 1: Build System Integration

### Add to Makefile

Add the new SMP source files to your kernel build:

```makefile
# In kernel/arch/x86_64/Makefile or main Makefile

KERNEL_OBJS += \
    kernel/arch/x86_64/smp.o \
    kernel/arch/x86_64/lapic.o \
    kernel/arch/x86_64/ipi.o \
    kernel/arch/x86_64/ipi_handlers.o

# AP boot trampoline (in boot directory)
BOOT_OBJS += \
    boot/ap_boot.o

# Assembly files need special handling
kernel/arch/x86_64/ipi_handlers.o: kernel/arch/x86_64/ipi_handlers.asm
	nasm -f elf64 -o $@ $<

boot/ap_boot.o: boot/ap_boot.S
	$(AS) $(ASFLAGS) -o $@ $<
```

### NASM for Assembly

If using NASM for IPI handlers:
```bash
nasm -f elf64 kernel/arch/x86_64/ipi_handlers.asm -o kernel/arch/x86_64/ipi_handlers.o
```

If using GAS (GNU Assembler):
```bash
as --64 boot/ap_boot.S -o boot/ap_boot.o
```

## Step 2: Kernel Initialization

### Add to kernel_main()

Modify your `kernel_main()` function:

```c
#include "kernel/include/smp.h"
#include "kernel/include/lapic.h"
#include "kernel/include/ipi.h"
#include "kernel/include/acpi.h"

void kernel_main(void) {
    // Early initialization
    serial_init();
    gdt_init();
    idt_init();
    
    kprintf("AutomationOS booting...\n");
    
    // Memory initialization
    pmm_init(memory_map, memory_map_count);
    vmm_init();
    heap_init();
    
    // ACPI initialization (must come before SMP)
    kprintf("[KERNEL] Initializing ACPI...\n");
    acpi_init();
    
    // SMP initialization
    kprintf("[KERNEL] Initializing SMP...\n");
    smp_init();          // Detect CPUs, initialize BSP
    
    kprintf("[KERNEL] Starting application processors...\n");
    smp_start_aps();     // Start all other CPUs
    
    kprintf("[KERNEL] Initializing IPI subsystem...\n");
    ipi_init();          // Initialize Inter-Processor Interrupts
    
    // Print SMP status
    kprintf("[KERNEL] SMP initialized: %u CPUs online\n", smp_num_online);
    smp_print_info();
    
    // Continue with rest of initialization...
    scheduler_init();
    // ...
}
```

## Step 3: IDT Integration

### Register IPI Handlers

Add IPI interrupt handlers to your IDT:

```c
// In idt_init() or after IDT is set up

extern void ipi_reschedule_handler(void);
extern void ipi_tlb_flush_handler(void);
extern void ipi_function_call_handler(void);
extern void ipi_stop_handler(void);

void idt_init(void) {
    // ... existing IDT setup ...
    
    // Register IPI handlers
    idt_set_gate(0x40, (uint64_t)ipi_reschedule_handler, 0x08, 0x8E);
    idt_set_gate(0x41, (uint64_t)ipi_tlb_flush_handler, 0x08, 0x8E);
    idt_set_gate(0x42, (uint64_t)ipi_function_call_handler, 0x08, 0x8E);
    idt_set_gate(0x43, (uint64_t)ipi_stop_handler, 0x08, 0x8E);
    
    kprintf("[IDT] IPI handlers registered\n");
}
```

### IDT Entry Format

If you need the `idt_set_gate` function:

```c
void idt_set_gate(uint8_t vector, uint64_t handler, uint16_t selector, uint8_t flags) {
    idt_entries[vector].offset_low = handler & 0xFFFF;
    idt_entries[vector].selector = selector;
    idt_entries[vector].ist = 0;
    idt_entries[vector].flags = flags;
    idt_entries[vector].offset_mid = (handler >> 16) & 0xFFFF;
    idt_entries[vector].offset_high = (handler >> 32) & 0xFFFFFFFF;
    idt_entries[vector].reserved = 0;
}
```

## Step 4: Linker Script

### Reserve AP Boot Trampoline

Ensure the AP boot code can be copied to 0x8000:

```ld
SECTIONS {
    /* ... */
    
    /* AP boot trampoline (will be copied to 0x8000) */
    .ap_boot ALIGN(4096) : {
        ap_boot_start = .;
        *(.ap_boot)
        ap_boot_end = .;
    }
    
    /* ... */
}
```

### Physical Memory Reservation

In your memory map, ensure address 0x8000 is marked as usable or reserved for the trampoline:

```c
// Don't allocate physical pages at 0x8000-0x9000
// This range is used for AP boot trampoline
```

## Step 5: PMM Integration (Already Done!)

The PMM already has per-CPU caches. Just verify the integration:

```c
// In kernel/core/mem/pmm.c

#include "../../include/smp.h"

void* pmm_alloc_page(void) {
    uint32_t cpu = cpu_id();  // Get current CPU ID
    
    // Try per-CPU cache first
    per_cpu_page_cache_t* cache = &cpu_caches[cpu];
    
    spin_lock(&cache->lock);
    if (cache->count > 0) {
        void* page = cache->pages[--cache->count];
        cache->alloc_fast++;
        spin_unlock(&cache->lock);
        return page;
    }
    spin_unlock(&cache->lock);
    
    // Cache miss - allocate from global pool
    return pmm_alloc_page_slow();
}
```

## Step 6: Scheduler Integration (Optional)

### Per-CPU Run Queues

If you want per-CPU scheduling:

```c
#include "kernel/include/smp.h"

typedef struct runqueue {
    spinlock_t lock;
    process_t* head;
    process_t* tail;
    uint32_t nr_running;
    process_t* current;
} runqueue_t;

// Initialize per-CPU run queues
void scheduler_init(void) {
    for_each_cpu(cpu) {
        runqueue_t* rq = kmalloc(sizeof(runqueue_t));
        spin_lock_init(&rq->lock);
        rq->head = NULL;
        rq->tail = NULL;
        rq->nr_running = 0;
        rq->current = NULL;
        
        percpu_data[cpu].runqueue = rq;
    }
}

// Schedule on current CPU
void schedule(void) {
    percpu_data_t* cpu = this_cpu();
    runqueue_t* rq = cpu->runqueue;
    
    spin_lock(&rq->lock);
    
    process_t* prev = rq->current;
    process_t* next = pick_next_process(rq);
    
    if (prev != next) {
        rq->current = next;
        context_switch(prev, next);
    }
    
    spin_unlock(&rq->lock);
}
```

### Load Balancing

Simple load balancing:

```c
void load_balance(void) {
    runqueue_t* this_rq = this_cpu()->runqueue;
    runqueue_t* busiest_rq = find_busiest_runqueue();
    
    if (busiest_rq->nr_running > this_rq->nr_running + 1) {
        process_t* proc = steal_process(busiest_rq);
        if (proc) {
            enqueue_process(this_rq, proc);
        }
    }
}
```

## Step 7: TLB Shootdown Integration

### Page Table Updates

Whenever you modify page tables, flush TLB on all CPUs:

```c
#include "kernel/include/ipi.h"

void vmm_map_page(void* virt, void* phys, uint32_t flags) {
    // Update page tables
    // ...
    
    // Flush TLB on all CPUs
    ipi_tlb_flush_all();
}

void vmm_unmap_page(void* virt) {
    // Remove page table entry
    // ...
    
    // Flush TLB for this page on all CPUs
    ipi_tlb_flush_page(virt);
}
```

### Optimization

For batch updates:

```c
void vmm_unmap_range(void* start, void* end) {
    // Unmap all pages
    for (void* addr = start; addr < end; addr += PAGE_SIZE) {
        // Remove page table entry (don't flush yet)
    }
    
    // Single TLB flush for entire range
    ipi_tlb_flush_all();
}
```

## Step 8: Testing

### Basic Test

```c
void test_smp_basic(void) {
    kprintf("=== SMP Basic Test ===\n");
    
    // Test 1: CPU count
    uint32_t cpus = cpu_count();
    kprintf("Detected %u CPUs\n", cpus);
    assert(cpus > 0);
    
    // Test 2: Current CPU
    uint32_t cpu = cpu_id();
    kprintf("Current CPU: %u\n", cpu);
    assert(cpu < cpus);
    
    // Test 3: Per-CPU data
    percpu_data_t* data = this_cpu();
    kprintf("Per-CPU data: %p\n", data);
    assert(data != NULL);
    assert(data->cpu_id == cpu);
    
    kprintf("Basic test passed!\n");
}
```

### IPI Test

```c
void test_ipi_function(void* arg) {
    kprintf("IPI function called on CPU %u\n", cpu_id());
}

void test_smp_ipi(void) {
    kprintf("=== SMP IPI Test ===\n");
    
    // Test remote function call
    for (uint32_t cpu = 0; cpu < cpu_count(); cpu++) {
        kprintf("Calling function on CPU %u...\n", cpu);
        ipi_call_function(cpu, test_ipi_function, NULL, true);
    }
    
    // Test broadcast
    kprintf("Broadcasting to all CPUs...\n");
    ipi_call_function_all(test_ipi_function, NULL, true);
    
    kprintf("IPI test passed!\n");
}
```

### TLB Test

```c
void test_smp_tlb(void) {
    kprintf("=== SMP TLB Test ===\n");
    
    for (int i = 0; i < 1000; i++) {
        ipi_tlb_flush_all();
    }
    
    kprintf("TLB test passed (1000 flushes)\n");
}
```

### Stress Test

```c
void test_smp_stress(void) {
    kprintf("=== SMP Stress Test ===\n");
    
    // Create 100 processes per CPU
    for (uint32_t cpu = 0; cpu < cpu_count(); cpu++) {
        for (int i = 0; i < 100; i++) {
            process_t* proc = process_create("stress_test", stress_function);
            scheduler_add_process(proc);
        }
    }
    
    // Let them run for 10 seconds
    sleep(10000);
    
    kprintf("Stress test completed\n");
}
```

## Step 9: Debugging

### Enable Debug Output

Add to kernel command line or config:
```
smp.debug=1
ipi.debug=1
```

### Print SMP Info

```c
// At any time, call:
smp_print_info();
smp_print_stats();
ipi_print_stats();
```

### GDB Integration

Debug multi-core system in QEMU:

```bash
qemu-system-x86_64 \
    -smp 4 \
    -kernel kernel.bin \
    -s -S

# In another terminal
gdb kernel.elf
(gdb) target remote :1234
(gdb) break smp_init
(gdb) continue

# Show all CPUs
(gdb) info threads

# Switch to CPU 2
(gdb) thread 3

# Show backtrace
(gdb) bt
```

## Step 10: QEMU Testing

### Run with Multiple CPUs

```bash
qemu-system-x86_64 \
    -smp 4 \
    -m 2G \
    -kernel kernel.bin \
    -serial stdio \
    -no-reboot -no-shutdown
```

### Test Different CPU Counts

```bash
# 2 CPUs
qemu-system-x86_64 -smp 2 ...

# 8 CPUs
qemu-system-x86_64 -smp 8 ...

# 16 CPUs
qemu-system-x86_64 -smp 16 ...
```

## Common Issues

### Issue 1: APs Don't Start

**Symptoms**: Only BSP (CPU 0) is online

**Solutions**:
1. Check ACPI MADT is present
2. Verify AP trampoline is at 0x8000
3. Ensure interrupts are enabled
4. Check LAPIC is initialized
5. Increase timeout in `cpu_start_ap()`

### Issue 2: System Hangs During Boot

**Symptoms**: Boot process freezes

**Solutions**:
1. Disable SMP temporarily (`smp.enabled=0`)
2. Check for deadlocks in initialization
3. Verify IDT is set up correctly
4. Check stack allocation for APs

### Issue 3: Page Faults After TLB Flush

**Symptoms**: Random page faults

**Solutions**:
1. Ensure `ipi_tlb_flush_all()` is called after page table changes
2. Check all CPUs are responding to TLB IPIs
3. Verify page tables are correctly set up

### Issue 4: Lock Contention

**Symptoms**: Poor performance, high CPU usage

**Solutions**:
1. Use per-CPU data instead of global locks
2. Keep critical sections short
3. Profile with `smp_print_stats()`
4. Consider lock-free algorithms

### Issue 5: IPI Not Delivered

**Symptoms**: Remote function calls don't execute

**Solutions**:
1. Check IDT has IPI handlers registered
2. Verify LAPIC is enabled
3. Check target CPU is online
4. Enable IPI debugging

## Performance Tuning

### 1. Per-CPU Caches

Increase cache size for better allocation performance:

```c
#define PER_CPU_CACHE_SIZE 32  // Was 16
```

### 2. IPI Queue Size

For high IPI workloads:

```c
#define IPI_CALL_QUEUE_SIZE 64  // Was 16
```

### 3. CPU Affinity

Pin critical tasks to specific CPUs:

```c
// Pin network interrupt to CPU 0
irq_set_affinity(net_irq, CPUMASK_CPU(0));

// Pin disk I/O to CPU 1
thread_set_affinity(disk_thread, CPUMASK_CPU(1));
```

### 4. Lock-Free Data Structures

For highly contended data:

```c
// Use atomic operations instead of locks
atomic_inc(&global_counter);
atomic_add(&global_value, delta);
```

## Next Steps

After successful integration:

1. ✅ Run test suite
2. ✅ Verify all CPUs are online
3. ✅ Test under load
4. ✅ Profile performance
5. ✅ Optimize hot paths

Then consider:
- [ ] Per-CPU run queues for scheduler
- [ ] Load balancing
- [ ] NUMA awareness
- [ ] CPU frequency scaling
- [ ] Advanced power management

## Reference

See these files for more details:
- `docs/SMP_ARCHITECTURE.md` - Complete architecture
- `docs/SMP_QUICK_REFERENCE.md` - API quick reference
- `kernel/include/smp.h` - SMP API
- `kernel/include/ipi.h` - IPI API
- `kernel/include/lapic.h` - LAPIC API

## Support

For issues or questions:
1. Check the documentation
2. Enable debug output
3. Run test suite
4. Check common issues above
5. Review code comments

**Use all the cores!**
