# SMP Quick Reference

## Initialization

```c
// In kernel_main()
acpi_init();          // Initialize ACPI
smp_init();           // Detect CPUs and init BSP
smp_start_aps();      // Start application processors
ipi_init();           // Initialize IPI subsystem
```

## Get CPU Information

```c
uint32_t cpu = cpu_id();              // Current CPU ID (0, 1, 2...)
uint32_t total = cpu_count();         // Total CPUs
uint32_t apic = apic_id();            // Current APIC ID

percpu_data_t* data = this_cpu();     // Current CPU data
bool online = cpu_is_online(2);       // Check if CPU 2 is online
```

## Per-CPU Data Access

```c
// Get current CPU's data
percpu_data_t* cpu = this_cpu();

// Access fields
uint64_t ticks = cpu->total_ticks;
uint32_t interrupts = cpu->interrupt_count;
process_t* current = cpu->current_thread;

// Get specific CPU's data
percpu_data_t* cpu2 = cpu_data(2);
```

## Spinlocks

### Basic Spinlock

```c
spinlock_t lock;
spin_lock_init(&lock);

spin_lock(&lock);
// Critical section
spin_unlock(&lock);
```

### IRQ-Safe Spinlock

```c
spinlock_t lock;
uint64_t flags;

spin_lock_irqsave(&lock, &flags);
// Critical section (interrupts disabled)
spin_unlock_irqrestore(&lock, flags);
```

### Try Lock

```c
if (spin_trylock(&lock)) {
    // Got the lock
    spin_unlock(&lock);
} else {
    // Lock was busy
}
```

## Inter-Processor Interrupts

### Send IPI

```c
// To specific CPU
ipi_send(2, IPI_RESCHEDULE);

// To all except self
ipi_send_all_but_self(IPI_TLB_FLUSH);

// To multiple CPUs
cpumask_t mask = CPUMASK_CPU(0) | CPUMASK_CPU(2);
ipi_send_mask(mask, IPI_RESCHEDULE);
```

### Remote Function Call

```c
void my_function(void* data) {
    kprintf("Running on CPU %u\n", cpu_id());
}

// Call on CPU 2 (wait for completion)
ipi_call_function(2, my_function, NULL, true);

// Call on all CPUs (wait)
ipi_call_function_all(my_function, NULL, true);

// Call without waiting
ipi_call_function(2, my_function, NULL, false);
```

### TLB Shootdown

```c
// Flush TLB on all CPUs
ipi_tlb_flush_all();

// Flush specific page (all CPUs)
ipi_tlb_flush_page((void*)0x400000);
```

## CPU Affinity

```c
// CPU mask operations
cpumask_t mask = CPUMASK_NONE;

// Set CPU 0
cpumask_set(&mask, 0);

// Set CPUs 1 and 3
cpumask_set(&mask, 1);
cpumask_set(&mask, 3);

// Test if CPU 2 is in mask
if (cpumask_test(mask, 2)) {
    // CPU 2 is in mask
}

// Count CPUs in mask
uint32_t count = cpumask_count(mask);

// Get first CPU in mask
uint32_t first = cpumask_first(mask);

// Set thread affinity
thread_set_affinity(thread, mask);
```

## Preemption Control

```c
preempt_disable();
// Non-preemptible code (use sparingly!)
preempt_enable();

// Check if preemption is disabled
if (preempt_is_disabled()) {
    // Preemption disabled
}
```

## CPU Control

```c
// Bring CPU online
cpu_online(2);

// Take CPU offline
cpu_offline(3);

// Check state
cpu_state_t state = cpu_get_state(1);
```

## Local APIC

### Initialization

```c
lapic_init();     // Initialize LAPIC
lapic_enable();   // Enable LAPIC
```

### End Of Interrupt

```c
// In interrupt handler
void timer_interrupt(void) {
    // Handle interrupt
    lapic_eoi();  // MUST call this!
}
```

### Timer

```c
// Start periodic timer at 100 Hz
lapic_timer_periodic(100);

// Stop timer
lapic_timer_stop();

// One-shot timer (microseconds)
lapic_timer_oneshot(1000);  // 1ms
```

## Debugging

### Print Info

```c
smp_print_info();    // CPU information
smp_print_stats();   // CPU statistics
ipi_print_stats();   // IPI statistics
```

### CPU Iteration

```c
// Iterate all CPUs
for_each_cpu(cpu) {
    kprintf("CPU %u: state=%d\n", cpu, cpu_get_state(cpu));
}

// Iterate only online CPUs
for_each_online_cpu(cpu) {
    kprintf("CPU %u is online\n", cpu);
}
```

## Common Patterns

### Per-CPU Counter

```c
// Each CPU has its own counter
typedef struct {
    uint64_t count;
    spinlock_t lock;
} percpu_counter_t;

percpu_counter_t counters[MAX_CPUS];

void increment_counter(void) {
    uint32_t cpu = cpu_id();
    spin_lock(&counters[cpu].lock);
    counters[cpu].count++;
    spin_unlock(&counters[cpu].lock);
}

uint64_t read_total(void) {
    uint64_t total = 0;
    for_each_online_cpu(cpu) {
        spin_lock(&counters[cpu].lock);
        total += counters[cpu].count;
        spin_unlock(&counters[cpu].lock);
    }
    return total;
}
```

### Global Data with Lock

```c
typedef struct {
    spinlock_t lock;
    uint32_t value;
} shared_data_t;

shared_data_t data;

void modify_shared(void) {
    uint64_t flags;
    spin_lock_irqsave(&data.lock, &flags);
    data.value++;
    spin_unlock_irqrestore(&data.lock, flags);
}
```

### Work Distribution

```c
void work_function(void* arg) {
    int* work = (int*)arg;
    kprintf("CPU %u processing work: %d\n", cpu_id(), *work);
}

void distribute_work(void) {
    int work[4] = {1, 2, 3, 4};
    
    for (uint32_t cpu = 0; cpu < cpu_count(); cpu++) {
        ipi_call_function(cpu, work_function, &work[cpu], false);
    }
}
```

## Performance Tips

1. **Minimize lock contention**
   - Use per-CPU data instead of global locks
   - Keep critical sections short
   - Use lock-free algorithms when possible

2. **Cache locality**
   - Keep related data together
   - Avoid false sharing (align to cache line size)
   - Use CPU affinity for cache-sensitive tasks

3. **Preemption**
   - Avoid long preempt_disable() sections
   - Use IRQ-safe locks in interrupt context
   - Check preempt_is_disabled() when debugging

4. **IPI efficiency**
   - Batch IPIs when possible
   - Use broadcast IPIs instead of loops
   - Avoid IPIs in hot paths

5. **TLB shootdown**
   - Batch page table updates
   - Use lazy TLB invalidation
   - Consider per-CPU page tables

## Common Mistakes

### ❌ Wrong

```c
// Global lock everywhere
spinlock_t global_lock;

void function_on_all_cpus(void) {
    spin_lock(&global_lock);
    // Everyone waits for the lock!
    spin_unlock(&global_lock);
}
```

### ✅ Right

```c
// Per-CPU data, no lock needed
void function_on_all_cpus(void) {
    percpu_data_t* cpu = this_cpu();
    cpu->value++;  // No lock needed!
}
```

### ❌ Wrong

```c
// Forgetting to save/restore IRQ flags
void irq_handler(void) {
    spin_lock(&lock);  // Bug! Can deadlock
    // ...
    spin_unlock(&lock);
}
```

### ✅ Right

```c
// IRQ-safe locking
void irq_handler(void) {
    uint64_t flags;
    spin_lock_irqsave(&lock, &flags);
    // ...
    spin_unlock_irqrestore(&lock, flags);
}
```

### ❌ Wrong

```c
// Sleeping while holding spinlock
spin_lock(&lock);
sleep(100);  // Bug! Never sleep with lock held
spin_unlock(&lock);
```

### ✅ Right

```c
// Use mutex for long operations
mutex_lock(&mutex);
sleep(100);  // OK with mutex
mutex_unlock(&mutex);
```

## SMP-Safe Memory Allocation

```c
// PMM already uses per-CPU caches
void* page = pmm_alloc_page();  // Fast! Uses local cache
pmm_free_page(page);            // Returns to local cache

// Heap uses global lock (future: per-CPU heaps)
void* ptr = kmalloc(1024);      // Protected by heap lock
kfree(ptr);
```

## Example: SMP-Safe Driver

```c
typedef struct {
    spinlock_t lock;
    uint32_t value;
    uint64_t irq_count[MAX_CPUS];
} my_device_t;

my_device_t device;

void device_init(void) {
    spin_lock_init(&device.lock);
    device.value = 0;
    
    for (uint32_t i = 0; i < MAX_CPUS; i++) {
        device.irq_count[i] = 0;
    }
}

irq_return_t device_irq_handler(uint32_t irq, void* dev_id) {
    uint32_t cpu = cpu_id();
    device.irq_count[cpu]++;  // Per-CPU, no lock needed
    
    uint64_t flags;
    spin_lock_irqsave(&device.lock, &flags);
    device.value++;  // Global, needs lock
    spin_unlock_irqrestore(&device.lock, flags);
    
    lapic_eoi();
    return IRQ_HANDLED;
}
```

## Troubleshooting

### Deadlock Detection

```c
// If system hangs, check:
1. Are interrupts enabled? (check RFLAGS.IF)
2. Is spinlock held? (spin_is_locked(&lock))
3. Which CPU holds the lock? (lock->owner_cpu)
4. Is preemption disabled? (preempt_is_disabled())
```

### IPI Not Working

```c
// Check:
1. Is LAPIC initialized? (lapic_enabled)
2. Are interrupts enabled? (sti())
3. Is target CPU online? (cpu_is_online(cpu))
4. Check LAPIC error status (lapic_get_error())
```

### TLB Issues

```c
// Symptoms: Random page faults, memory corruption
// Check:
1. Did you flush TLB after page table changes?
2. Did ipi_tlb_flush_all() complete successfully?
3. Are all CPUs online and responding?
```

## Resources

- Full documentation: `docs/SMP_ARCHITECTURE.md`
- Header files:
  - `kernel/include/smp.h` - SMP core
  - `kernel/include/lapic.h` - Local APIC
  - `kernel/include/ipi.h` - IPIs
  - `kernel/include/spinlock.h` - Spinlocks
- Implementation:
  - `kernel/arch/x86_64/smp.c` - SMP initialization
  - `kernel/arch/x86_64/lapic.c` - LAPIC driver
  - `kernel/arch/x86_64/ipi.c` - IPI subsystem
  - `boot/ap_boot.S` - AP startup code
