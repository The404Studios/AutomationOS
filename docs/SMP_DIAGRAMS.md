# SMP Visual Diagrams

Visual representations of the SMP architecture.

## 1. Overall System Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│                      AutomationOS Kernel                         │
│                                                                   │
│  ┌──────────────────────────────────────────────────────────┐   │
│  │                   Application Layer                        │   │
│  │  ┌────────┐  ┌────────┐  ┌────────┐  ┌────────┐         │   │
│  │  │Process │  │Process │  │Process │  │Process │         │   │
│  │  │   1    │  │   2    │  │   3    │  │   4    │  ...    │   │
│  │  └────────┘  └────────┘  └────────┘  └────────┘         │   │
│  └──────────────────────────────────────────────────────────┘   │
│                              ↓                                    │
│  ┌──────────────────────────────────────────────────────────┐   │
│  │                     Scheduler                              │   │
│  │  ┌───────────┐  ┌───────────┐  ┌───────────┐           │   │
│  │  │ RunQueue  │  │ RunQueue  │  │ RunQueue  │  ...       │   │
│  │  │  CPU 0    │  │  CPU 1    │  │  CPU 2    │           │   │
│  │  └─────┬─────┘  └─────┬─────┘  └─────┬─────┘           │   │
│  └────────┼──────────────┼──────────────┼──────────────────┘   │
│           │              │              │                        │
│  ┌────────▼──────────────▼──────────────▼──────────────────┐   │
│  │                   SMP Subsystem                           │   │
│  │  ┌─────────┐  ┌─────────┐  ┌─────────┐  ┌─────────┐    │   │
│  │  │Per-CPU  │  │Per-CPU  │  │Per-CPU  │  │Per-CPU  │    │   │
│  │  │ Data 0  │  │ Data 1  │  │ Data 2  │  │ Data 3  │    │   │
│  │  └────┬────┘  └────┬────┘  └────┬────┘  └────┬────┘    │   │
│  │       │            │            │            │           │   │
│  │  ┌────▼────┐  ┌───▼────┐  ┌───▼────┐  ┌───▼────┐      │   │
│  │  │  CPU 0  │  │ CPU 1  │  │ CPU 2  │  │ CPU 3  │      │   │
│  │  │  (BSP)  │  │  (AP)  │  │  (AP)  │  │  (AP)  │      │   │
│  │  └────┬────┘  └───┬────┘  └───┬────┘  └───┬────┘      │   │
│  └───────┼───────────┼───────────┼───────────┼───────────┘   │
│          │           │           │           │                  │
│  ┌───────▼───────────▼───────────▼───────────▼───────────┐   │
│  │              Memory Management                         │   │
│  │  ┌───────────────────────────────────────────────┐    │   │
│  │  │  Physical Memory (Shared)                      │    │   │
│  │  └───────────────────────────────────────────────┘    │   │
│  │  ┌─────────┐  ┌─────────┐  ┌─────────┐  ┌─────────┐ │   │
│  │  │ Cache 0 │  │ Cache 1 │  │ Cache 2 │  │ Cache 3 │ │   │
│  │  │ (16 pg) │  │ (16 pg) │  │ (16 pg) │  │ (16 pg) │ │   │
│  │  └─────────┘  └─────────┘  └─────────┘  └─────────┘ │   │
│  └────────────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────────────┘
         │              │              │              │
┌────────▼──────────────▼──────────────▼──────────────▼───────────┐
│                     Hardware Layer                                │
│  ┌─────────┐  ┌─────────┐  ┌─────────┐  ┌─────────┐            │
│  │ LAPIC 0 │  │ LAPIC 1 │  │ LAPIC 2 │  │ LAPIC 3 │            │
│  └────┬────┘  └────┬────┘  └────┬────┘  └────┬────┘            │
│       │            │            │            │                   │
│       └────────────┴────────────┴────────────┘                   │
│                    │                                              │
│            ┌───────▼────────┐                                    │
│            │    I/O APIC    │                                    │
│            └────────────────┘                                    │
└───────────────────────────────────────────────────────────────────┘
```

## 2. CPU Startup Sequence

```
BSP (CPU 0)                               AP (CPU 1, 2, 3...)
════════════                               ═══════════════════

[Boot]
  │
  ├─ Initialize kernel
  ├─ Setup memory
  ├─ Initialize ACPI
  │
  ├─ smp_init()
  │    ├─ Initialize BSP
  │    ├─ Detect CPUs (MADT)
  │    └─ Setup LAPIC
  │
  ├─ smp_start_aps()
  │    │
  │    ├─ Copy trampoline to 0x8000
  │    │
  │    ├─ For each AP:
  │    │    ├─ Send INIT IPI ───────────────────────►  [Reset]
  │    │    │                                             │
  │    │    ├─ Wait 10ms                                 ├─ CPU halted
  │    │    │                                             │
  │    │    ├─ Send SIPI #1 ──────────────────────►     ├─ Wake up
  │    │    │     (vector=0x8000>>12)                    │
  │    │    │                                             ├─ Start at 0x8000
  │    │    ├─ Wait 200μs                                │   (16-bit real mode)
  │    │    │                                             │
  │    │    ├─ Send SIPI #2 ──────────────────────►     ├─ Load GDT
  │    │    │                                             │
  │    │    │                                             ├─ Protected mode
  │    │    │                                             │
  │    │    │                                             ├─ Enable PAE
  │    │    │                                             │
  │    │    │                                             ├─ Enable long mode
  │    │    │                                             │
  │    │    │                                             ├─ 64-bit mode
  │    │    │                                             │
  │    │    │                                             ├─ ap_main()
  │    │    │                                             │   ├─ Init LAPIC
  │    │    │                                             │   ├─ Init per-CPU
  │    │    │                                             │   └─ Mark online
  │    │    │                                             │
  │    │    └─ Wait for ready ◄──────────────────────────┘
  │    │         (ap_ready_count++)
  │    │
  │    └─ All APs online
  │
  ├─ ipi_init()
  │
  └─ Continue kernel init                      [Idle loop]
                                                    │
                                                    └─ Wait for work
```

## 3. IPI Communication Flow

```
CPU 0                        LAPIC 0              LAPIC 1                CPU 1
═════                        ═══════              ═══════                ═════

1. Send IPI
   │
   ├─ ipi_send(1, IPI_TLB_FLUSH)
   │    │
   │    └─ lapic_send_ipi(apic_id=1, vector=0x41)
   │         │
   │         └─ Write to ICR ──────►  APIC
   │              [dest=1, vec=0x41]    │
   │                                    │
   │                            ┌───────▼────────┐
   │                            │ Deliver IPI    │
   │                            │ to CPU 1       │
   │                            └───────┬────────┘
   │                                    │
2. Wait for response                   │
   │                                    │
   ├─ while (ack_count == 0)           │
   │    cpu_relax();                   │
   │                                    └─────────► APIC
   │                                                  │
   │                                          ┌───────▼────────┐
   │                                          │ Interrupt CPU  │
   │                                          └───────┬────────┘
   │                                                  │
   │                                          ┌───────▼────────┐
   │                                          │ Save state     │
   │                                          │ (push regs)    │
   │                                          └───────┬────────┘
   │                                                  │
   │                                          ┌───────▼────────────┐
   │                                          │ ipi_tlb_flush_     │
   │                                          │ handler() called   │
   │                                          └───────┬────────────┘
   │                                                  │
   │                                          ┌───────▼────────────┐
   │                                          │ Flush TLB          │
   │                                          │ (reload CR3)       │
   │                                          └───────┬────────────┘
   │                                                  │
   │                                          ┌───────▼────────────┐
   │                                          │ ack_count++        │
3. Response received                         └───────┬────────────┘
   │                                                  │
   ◄──────────────────────────────────────────────────┘
   │
   ├─ Response acknowledged
   │
   └─ Continue execution

Time: ~3-5 microseconds total
```

## 4. Per-CPU Data Layout

```
Memory Layout:
═════════════

┌─────────────────────────────────────────────────────────────┐
│                     percpu_data[256]                         │  Global array
├─────────────────────────────────────────────────────────────┤
│                                                               │
│  CPU 0                    CPU 1                    CPU 2     │
│  ┌─────────────────┐     ┌─────────────────┐     ┌───...    │
│  │ cpu_id = 0      │     │ cpu_id = 1      │     │          │
│  │ apic_id = 0     │     │ apic_id = 1     │     │          │
│  │ state = ONLINE  │     │ state = ONLINE  │     │          │
│  │ flags = BSP     │     │ flags = 0       │     │          │
│  │                 │     │                 │     │          │
│  │ current_thread ───┐   │ current_thread ───┐   │          │
│  │ idle_thread ───┐ │   │ idle_thread ───┐ │   │          │
│  │ runqueue ────┐ │ │   │ runqueue ────┐ │ │   │          │
│  │              │ │ │   │              │ │ │   │          │
│  │ total_ticks  │ │ │   │ total_ticks  │ │ │   │          │
│  │ idle_ticks   │ │ │   │ idle_ticks   │ │ │   │          │
│  │ interrupt_cnt│ │ │   │ interrupt_cnt│ │ │   │          │
│  │              │ │ │   │              │ │ │   │          │
│  │ page_cache[16]  │ │  │ page_cache[16]  │ │  │          │
│  │  ┌───┬───┬───┐ │ │  │  ┌───┬───┬───┐ │ │  │          │
│  │  │ 0 │ 1 │...│ │ │  │  │ 0 │ 1 │...│ │ │  │          │
│  │  └───┴───┴───┘ │ │  │  └───┴───┴───┘ │ │  │          │
│  │ cache_count = 12│ │  │ cache_count = 8 │ │  │          │
│  │ cache_lock      │ │  │ cache_lock      │ │  │          │
│  │                 │ │  │                 │ │  │          │
│  └─────────────────┘ │  └─────────────────┘ │  └───...    │
│         │            │           │           │              │
│         │            │           │           │              │
│         ▼            ▼           ▼           ▼              │
│    ┌──────────┐ ┌──────┐   ┌──────────┐ ┌──────┐          │
│    │RunQueue 0│ │Thread│   │RunQueue 1│ │Thread│          │
│    └──────────┘ └──────┘   └──────────┘ └──────┘          │
│                                                              │
└──────────────────────────────────────────────────────────────┘

Access Pattern:
══════════════

CPU 0 executes:                    CPU 1 executes:
  percpu_data_t* cpu = this_cpu();   percpu_data_t* cpu = this_cpu();
  cpu->total_ticks++;                 cpu->total_ticks++;
  // No lock needed!                  // No lock needed!

Result: Zero contention, perfect scalability
```

## 5. TLB Shootdown Protocol

```
Scenario: CPU 0 unmaps a page, all CPUs must flush TLB
═══════════════════════════════════════════════════════

CPU 0                   CPU 1                   CPU 2                   CPU 3
═════                   ═════                   ═════                   ═════

1. Modify page table
   │
   ├─ unmap_page(addr)
   │    │
   │    ├─ Remove PTE
   │    │
   │    ├─ ipi_tlb_flush_all()
   │    │    │
   │    │    ├─ tlb_flush_ack_count = 0
   │    │    │
2. Flush local TLB     │
   │                   │
   ├─ Reload CR3       │
   │                   │
3. Send IPIs           │
   │                   │
   ├─ Send IPI ─────────────────►  Interrupt!      Interrupt!      Interrupt!
   │                               │                │                │
4. Wait for ACKs                   │                │                │
   │                               │                │                │
   ├─ while (ack < 3)              │                │                │
   │    cpu_relax()                │                │                │
   │                        ┌──────▼──────┐  ┌──────▼──────┐  ┌──────▼──────┐
   │                        │ Save state  │  │ Save state  │  │ Save state  │
   │                        ├─────────────┤  ├─────────────┤  ├─────────────┤
   │                        │ Reload CR3  │  │ Reload CR3  │  │ Reload CR3  │
   │                        ├─────────────┤  ├─────────────┤  ├─────────────┤
   │                        │ ack_count++ │  │ ack_count++ │  │ ack_count++ │
   │                        ├─────────────┤  ├─────────────┤  ├─────────────┤
   │                        │ Send EOI    │  │ Send EOI    │  │ Send EOI    │
   │                        ├─────────────┤  ├─────────────┤  ├─────────────┤
   │                        │ Restore st. │  │ Restore st. │  │ Restore st. │
   │                        └─────────────┘  └─────────────┘  └─────────────┘
   │                               │                │                │
5. All CPUs ACKed ◄────────────────┴────────────────┴────────────────┘
   │                        (ack_count = 3)
   │
   ├─ TLB flush complete
   │
   └─ Continue

Timeline:
0μs    ──┬── Unmap page
       │
1μs    ──┼── Send IPIs ─────────────────────────►
       │                                          │
2μs    ──┼── Wait...                              ├─ Interrupt
       │                                          ├─ Flush TLB
3μs    ──┼── Wait...                              └─ ACK
       │
4μs    ──┼── All ACKs received ◄──────────────────
       │
5μs    ──┴── Continue

Total latency: ~5μs for 4 CPUs
```

## 6. Spinlock with IRQ Disable

```
Timeline of spin_lock_irqsave() and spin_unlock_irqrestore()
═════════════════════════════════════════════════════════════

CPU 0                                    CPU 1
═════                                    ═════

T0: Normal execution                     T0: Normal execution
    IRQs enabled ──────►                     IRQs enabled ──────►

T1: spin_lock_irqsave(&lock, &flags)
    ├─ Save RFLAGS (with IF flag)
    ├─ CLI (disable interrupts)
    ├─ Acquire lock (atomic)
    └─ IRQs disabled ──────►             T1: Continue...
                                             IRQs enabled ──────►

T2: Critical section
    IRQs disabled ──────►                T2: spin_lock_irqsave(&lock, &flags)
    (safe from interrupts)                   ├─ Save RFLAGS
                                             ├─ CLI
                                             ├─ Try lock... BUSY!
                                             │  IRQs disabled ──────►
                                             │
T3: Critical section                     T3: Spinning...
    IRQs disabled ──────►                    ├─ while (locked)
                                             │    pause;
                                             │  IRQs disabled ──────►

T4: spin_unlock_irqrestore(&lock, flags)
    ├─ Release lock (atomic)
    └─ Restore RFLAGS                    T4: Lock acquired!
       (may re-enable IRQs)                  ├─ Lock obtained
       IRQs enabled ──────►                  └─ Enter critical section
                                                IRQs disabled ──────►

T5: Normal execution                     T5: Critical section
    IRQs enabled ──────►                     IRQs disabled ──────►

                                         T6: spin_unlock_irqrestore(&lock, flags)
                                             ├─ Release lock
                                             └─ Restore RFLAGS
                                                IRQs enabled ──────►

                                         T7: Normal execution
                                             IRQs enabled ──────►

Key Points:
- IRQs disabled while holding lock (prevents deadlock)
- Original IRQ state restored after unlock
- Other CPU spins with IRQs disabled
- Short critical sections essential!
```

## 7. Memory Allocation Fast Path (Per-CPU Cache)

```
Allocation Flow with Per-CPU Caches
════════════════════════════════════

┌─────────────────────────────────────────────────────────────┐
│                     pmm_alloc_page()                         │
│                                                               │
│  CPU 0 calls:                    CPU 1 calls:                │
│  ┌─────────────┐                 ┌─────────────┐            │
│  │ alloc_page()│                 │ alloc_page()│            │
│  └──────┬──────┘                 └──────┬──────┘            │
│         │                                │                   │
│  ┌──────▼──────────────┐         ┌──────▼──────────────┐    │
│  │ cpu = cpu_id() = 0  │         │ cpu = cpu_id() = 1  │    │
│  └──────┬──────────────┘         └──────┬──────────────┘    │
│         │                                │                   │
│  ┌──────▼───────────────────────────────▼──────────┐        │
│  │      Check per-CPU cache                        │        │
│  └──────┬───────────────────────────┬──────────────┘        │
│         │                            │                       │
│    Cache HIT (95%)              Cache HIT (95%)              │
│         │                            │                       │
│  ┌──────▼──────────┐          ┌─────▼───────────┐           │
│  │ Cache 0         │          │ Cache 1         │           │
│  │ ┌───┬───┬───┐   │          │ ┌───┬───┬───┐   │           │
│  │ │ 0 │ 1 │...│   │          │ │ 0 │ 1 │...│   │           │
│  │ └─▲─┴───┴───┘   │          │ └─▲─┴───┴───┘   │           │
│  │   │ Pop page    │          │   │ Pop page    │           │
│  │   │ count=11    │          │   │ count=7     │           │
│  └───┼─────────────┘          └───┼─────────────┘           │
│      │                            │                          │
│      └───────┬────────────────────┘                          │
│              │                                                │
│  ┌───────────▼──────────────────────────────────┐            │
│  │  Return page (fast: ~50 cycles)              │            │
│  └───────────────────────────────────────────────┘            │
│                                                               │
│                                                               │
│  Cache MISS (5%)                                              │
│      │                                                        │
│  ┌───▼───────────────────────────────────────────┐           │
│  │  Acquire global PMM lock                      │           │
│  │  ┌──────────────────────────────────────┐    │           │
│  │  │ Global Free Lists                     │    │           │
│  │  │ ┌──────┬──────┬──────┬──────┐        │    │           │
│  │  │ │Order0│Order1│Order2│Order3│  ...   │    │           │
│  │  │ └──┬───┴──────┴──────┴──────┘        │    │           │
│  │  │    │ Allocate page                    │    │           │
│  │  │    └──► Refill cache (16 pages)      │    │           │
│  │  └──────────────────────────────────────┘    │           │
│  │  Release global PMM lock                      │           │
│  └───────────┬───────────────────────────────────┘           │
│              │                                                │
│  ┌───────────▼──────────────────────────────────┐            │
│  │  Return page (slow: ~500 cycles)             │            │
│  └───────────────────────────────────────────────┘            │
│                                                               │
└─────────────────────────────────────────────────────────────┘

Performance:
- Cache hit: 95%+ → 10x faster
- No contention between CPUs (separate caches)
- Automatic refill on miss
- Cache size: 16 pages per CPU (tunable)
```

## 8. CPU State Machine

```
CPU Lifecycle States
════════════════════

┌────────────┐
│  OFFLINE   │  Initial state
└──────┬─────┘
       │
       │ cpu_online(cpu)
       │
       ▼
┌────────────┐
│  STARTING  │  AP receiving INIT/SIPI
└──────┬─────┘
       │
       │ ap_main() completes
       │
       ▼
┌────────────┐
│   ONLINE   │  ◄─── Normal operation
└──────┬─────┘       │
       │             │
       │ cpu_offline(cpu)
       │             │
       ▼             │
┌────────────┐       │
│  STOPPING  │       │
└──────┬─────┘       │
       │             │
       │ Migration complete
       │             │
       ▼             │
┌────────────┐       │
│  OFFLINE   │  ─────┘
└────────────┘

┌────────────┐
│   FAILED   │  Error during startup
└────────────┘
       ▲
       │
       │ Timeout or error
       │
       │

State Transitions:
==================

OFFLINE → STARTING:  cpu_online() called, INIT/SIPI sent
STARTING → ONLINE:   AP successfully boots to ap_main()
STARTING → FAILED:   Timeout waiting for AP
ONLINE → STOPPING:   cpu_offline() called
STOPPING → OFFLINE:  Migration complete, CPU halted
FAILED → OFFLINE:    (manual reset)
```

This concludes the SMP visual diagrams document.
