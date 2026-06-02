/*
 * SMP (Symmetric Multiprocessing) bring-up
 * ========================================
 *
 * Brings the Application Processors (APs) online on x86_64:
 *   - Enumerate LAPIC IDs from the ACPI MADT ("APIC" table).
 *   - Initialize the BSP local APIC.
 *   - Copy a 16-bit real-mode trampoline into low memory (< 1 MB).
 *   - Send INIT-SIPI-SIPI to each AP via the LAPIC ICR.
 *   - Each AP comes up through ap_trampoline.asm into long mode and calls
 *     smp_ap_main(), marks itself online, and parks in an idle (hlt) loop.
 *
 * ALL multicore bring-up is gated behind the compile-time flag SMP_ENABLE.
 * With the flag OFF (the default) this file builds to a tiny single-CPU stub:
 * smp_init() reports 1 CPU and does nothing else, so the kernel boots exactly
 * as it does today. The integrator enables -DSMP_ENABLE (and assembles
 * ap_trampoline.asm) only after testing.
 *
 * Scope note: per-CPU run queues / SMP scheduling are intentionally NOT done
 * here -- APs are brought online and parked. That is future work owned by the
 * scheduler.
 */

#include "../../include/smp.h"
#include "../../include/lapic.h"
#include "../../include/acpi.h"
#include "../../include/kernel.h"
#include "../../include/x86_64.h"
#include "../../include/mem.h"
#include <string.h>

/* ---------------------------------------------------------------------------
 * Global SMP state (defined unconditionally so other subsystems that reference
 * percpu_data / smp_num_cpus keep linking regardless of the SMP flag).
 * ------------------------------------------------------------------------- */
uint32_t smp_num_cpus = 1;
uint32_t smp_num_online = 1;
percpu_data_t percpu_data[MAX_CPUS];
cpu_info_t cpu_info[MAX_CPUS];

/* Total logical CPU count (1 until/unless SMP bring-up runs). */
uint32_t smp_cpu_count(void) {
    return smp_num_cpus;
}

/* CPU identification via CPUID (safe single-CPU helper, always available). */
void cpu_identify(uint32_t cpu) {
    if (cpu >= MAX_CPUS) return;

    cpu_info_t* info = &cpu_info[cpu];

    uint32_t eax, ebx, ecx, edx;
    __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(0));

    *(uint32_t*)(info->vendor + 0) = ebx;
    *(uint32_t*)(info->vendor + 4) = edx;
    *(uint32_t*)(info->vendor + 8) = ecx;
    info->vendor[12] = '\0';

    __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(1));
    info->family   = ((eax >> 8) & 0xF) + ((eax >> 20) & 0xFF);
    info->model    = ((eax >> 4) & 0xF) | ((eax >> 12) & 0xF0);
    info->stepping = eax & 0xF;
    info->has_apic   = (edx & (1 << 9))  != 0;
    info->has_tsc    = (edx & (1 << 4))  != 0;
    info->has_msr    = (edx & (1 << 5))  != 0;
    info->has_sse    = (edx & (1 << 25)) != 0;
    info->has_sse2   = (edx & (1 << 26)) != 0;
    info->has_x2apic = (ecx & (1 << 21)) != 0;
    info->has_avx    = (ecx & (1 << 28)) != 0;
}

const char* cpu_vendor_string(uint32_t cpu) {
    if (cpu >= MAX_CPUS) return "Unknown";
    return cpu_info[cpu].vendor;
}

const char* cpu_brand_string(uint32_t cpu) {
    if (cpu >= MAX_CPUS) return "Unknown";
    return cpu_info[cpu].brand;
}

/* Per-CPU data accessors (single-CPU safe; cpu_id() falls back to 0). */
uint32_t cpu_count(void) { return smp_num_cpus; }

bool cpu_is_online(uint32_t cpu) {
    if (cpu >= smp_num_cpus) return false;
    return percpu_data[cpu].state == CPU_STATE_ONLINE;
}

cpu_state_t cpu_get_state(uint32_t cpu) {
    if (cpu >= smp_num_cpus) return CPU_STATE_OFFLINE;
    return percpu_data[cpu].state;
}

percpu_data_t* cpu_data(uint32_t cpu) {
    if (cpu >= smp_num_cpus) return NULL;
    return &percpu_data[cpu];
}

/* ---------------------------------------------------------------------------
 * CPU hotplug callbacks + preemption counters. Defined unconditionally so the
 * full smp.h API resolves at link time whether or not SMP_ENABLE is set. These
 * are harmless no-op-ish helpers on a single CPU.
 * ------------------------------------------------------------------------- */
#define MAX_CALLBACKS 16
static cpu_online_cb_t  online_callbacks[MAX_CALLBACKS];
static cpu_offline_cb_t offline_callbacks[MAX_CALLBACKS];
static uint32_t num_online_callbacks = 0;
static uint32_t num_offline_callbacks = 0;

int register_cpu_online_callback(cpu_online_cb_t cb) {
    if (num_online_callbacks >= MAX_CALLBACKS) return -1;
    online_callbacks[num_online_callbacks++] = cb;
    return 0;
}

int register_cpu_offline_callback(cpu_offline_cb_t cb) {
    if (num_offline_callbacks >= MAX_CALLBACKS) return -1;
    offline_callbacks[num_offline_callbacks++] = cb;
    return 0;
}

void preempt_disable(void) {
    __atomic_add_fetch(&percpu_data[cpu_id()].preempt_count, 1, __ATOMIC_ACQUIRE);
}

void preempt_enable(void) {
    __atomic_sub_fetch(&percpu_data[cpu_id()].preempt_count, 1, __ATOMIC_RELEASE);
}

bool preempt_is_disabled(void) {
    return __atomic_load_n(&percpu_data[cpu_id()].preempt_count, __ATOMIC_ACQUIRE) > 0;
}

int cpu_offline(uint32_t cpu) {
    if (cpu >= smp_num_cpus) return -1;
    if (cpu == 0) return -1;            /* never offline the BSP */
    if (!cpu_is_online(cpu)) return 0;
    for (uint32_t i = 0; i < num_offline_callbacks; i++) offline_callbacks[i](cpu);
    percpu_data[cpu].state = CPU_STATE_OFFLINE;
    __atomic_sub_fetch(&smp_num_online, 1, __ATOMIC_SEQ_CST);
    return 0;
}

#ifndef SMP_ENABLE
/* ===========================================================================
 * SINGLE-CPU STUB (default build).
 * No LAPIC programming, no trampoline, no IPIs. The kernel runs exactly as it
 * does today on the BSP only.
 * ========================================================================= */

uint32_t cpu_id(void)  { return 0; }
uint32_t apic_id(void) { return 0; }
percpu_data_t* this_cpu(void) { return &percpu_data[0]; }

int smp_init(void) {
    /* BSP is CPU 0 and is always online. */
    percpu_data[0].cpu_id = 0;
    percpu_data[0].apic_id = 0;
    percpu_data[0].state = CPU_STATE_ONLINE;
    percpu_data[0].flags = CPU_FLAG_BSP | CPU_FLAG_ENABLED;
    cpu_identify(0);
    smp_num_cpus = 1;
    smp_num_online = 1;
    return (int)smp_num_cpus;
}

void smp_detect_cpus(void) { smp_num_cpus = 1; }
void smp_start_aps(void)   { /* nothing to do single-CPU */ }

int cpu_online(uint32_t cpu) {
    /* Only the BSP exists in a single-CPU build. */
    return (cpu == 0) ? 0 : -1;
}

void smp_print_info(void) {
    kprintf("[SMP] Single-CPU build (SMP_ENABLE off): 1 CPU online (BSP)\n");
}
void smp_print_stats(void) {}

#else  /* SMP_ENABLE */
/* ===========================================================================
 * MULTICORE BRING-UP (only with -DSMP_ENABLE).
 * ========================================================================= */

/*
 * Where the AP trampoline lives in low memory and the offset of its parameter
 * block. These MUST match ap_trampoline.asm (AP_TRAMPOLINE_BASE / AP_PARAM_OFFSET).
 * 0x8000 is below 1 MB and outside the BIOS/IVT/EBDA we touch, so it is a safe
 * SIPI target page (SIPI vector = 0x8000 >> 12 = 0x08).
 */
#define AP_TRAMPOLINE_BASE   0x8000UL
#define AP_PARAM_OFFSET      0x0F00UL
#define AP_SIPI_VECTOR       (AP_TRAMPOLINE_BASE >> 12)   /* = 0x08 */

/* Parameter-block field offsets (relative to AP_TRAMPOLINE_BASE + AP_PARAM_OFFSET). */
#define AP_PARAM_CR3         0
#define AP_PARAM_STACK_TOP   8
#define AP_PARAM_ENTRY       16
#define AP_PARAM_ARG         24
#define AP_PARAM_GDTR        32
#define AP_PARAM_IDTR        48

/* AP stack size (per AP). */
#define AP_STACK_SIZE        16384

/* Symbols emitted by ap_trampoline.asm. */
extern uint8_t ap_trampoline_start[];
extern uint8_t ap_trampoline_end[];

/* The 64-bit C entry the AP jumps to (defined below). */
void smp_ap_main(uint64_t cpu);

/* AP startup synchronization. */
static volatile uint32_t ap_started_flag = 0;   /* set by the AP we are waiting on */

/* 10-byte descriptor-table register image (limit + base). */
typedef struct __attribute__((packed)) {
    uint16_t limit;
    uint64_t base;
} dtr_image_t;

/* Crude busy-wait. We do not have a calibrated microsecond timer guaranteed at
 * SMP-init time, so spin a fixed number of pause iterations. The INIT->SIPI
 * (>=10 ms) and SIPI->SIPI (>=200 us) delays only need to be "long enough". */
static void smp_delay(volatile uint64_t iterations) {
    while (iterations--) {
        __asm__ volatile("pause" ::: "memory");
    }
}

/* Initialize per-CPU bookkeeping for a logical CPU. */
static void percpu_init(uint32_t cpu) {
    percpu_data_t* p = &percpu_data[cpu];
    p->current_thread = NULL;
    p->current_process = NULL;
    p->idle_thread = NULL;
    p->runqueue = NULL;
    p->total_ticks = p->idle_ticks = p->user_ticks = p->kernel_ticks = 0;
    p->interrupt_count = 0;
    p->interrupts_enabled = false;
    p->preempt_count = 0;
    p->kernel_stack = NULL;
    p->interrupt_stack = NULL;
    p->tlb_flush_pending = false;
    p->page_cache_count = 0;
    p->page_cache_hits = 0;
    p->page_cache_misses = 0;
    spin_lock_init(&p->page_cache_lock);
}

/* Current logical CPU id from the LAPIC id (linear search of the table). */
uint32_t cpu_id(void) {
    if (lapic_enabled) {
        uint32_t aid = lapic_get_id();
        for (uint32_t i = 0; i < smp_num_cpus; i++) {
            if (percpu_data[i].apic_id == aid) return i;
        }
    }
    return 0;
}

uint32_t apic_id(void) { return lapic_get_id(); }
percpu_data_t* this_cpu(void) { return &percpu_data[cpu_id()]; }

/*
 * Enumerate CPUs from the ACPI MADT. Returns the number of enabled CPUs found
 * (>= 1). Fills percpu_data[].apic_id and sets lapic_base from the MADT.
 */
void smp_detect_cpus(void) {
    kprintf("[SMP] Enumerating CPUs from ACPI MADT...\n");

    acpi_madt_t* madt = (acpi_madt_t*)acpi_find_table("APIC");
    if (!madt) {
        kprintf("[SMP] MADT not found; assuming single CPU.\n");
        smp_num_cpus = 1;
        return;
    }

    if (madt->local_apic_address != 0 && lapic_base == NULL) {
        lapic_base = (void*)(uint64_t)madt->local_apic_address;
    }

    uint32_t count = 0;
    uint8_t* p   = madt->entries;
    uint8_t* end = (uint8_t*)madt + madt->header.length;

    while (p < end && count < MAX_CPUS) {
        acpi_madt_entry_header_t* h = (acpi_madt_entry_header_t*)p;
        if (h->length == 0) break;  /* malformed; avoid infinite loop */

        if (h->type == ACPI_MADT_TYPE_LOCAL_APIC) {
            acpi_madt_local_apic_t* e = (acpi_madt_local_apic_t*)p;
            if (e->flags & 0x01) {  /* enabled */
                percpu_data[count].cpu_id = count;
                percpu_data[count].apic_id = e->apic_id;
                percpu_data[count].state = CPU_STATE_OFFLINE;
                percpu_data[count].flags = CPU_FLAG_ENABLED;
                kprintf("[SMP]   CPU %u -> APIC ID %u\n", count, e->apic_id);
                count++;
            }
        }
        p += h->length;
    }

    uint32_t final_count = (count == 0) ? 1 : count;
    __atomic_store_n(&smp_num_cpus, final_count, __ATOMIC_RELEASE);
    kprintf("[SMP] Detected %u CPU(s)\n", final_count);
}

/* Capture the running kernel's GDTR/IDTR and copy the trampoline + params. */
static void smp_setup_trampoline(uint64_t kernel_cr3) {
    /* Copy the trampoline code blob into low memory. */
    size_t sz = (size_t)(ap_trampoline_end - ap_trampoline_start);
    memcpy((void*)AP_TRAMPOLINE_BASE, ap_trampoline_start, sz);
    kprintf("[SMP] AP trampoline copied to 0x%lx (%lu bytes)\n",
            (unsigned long)AP_TRAMPOLINE_BASE, (unsigned long)sz);

    uint8_t* param = (uint8_t*)(AP_TRAMPOLINE_BASE + AP_PARAM_OFFSET);

    /* CR3 + 64-bit entry pointer are shared by all APs. */
    *(volatile uint64_t*)(param + AP_PARAM_CR3)   = kernel_cr3;
    *(volatile uint64_t*)(param + AP_PARAM_ENTRY) = (uint64_t)&smp_ap_main;

    /* Snapshot the active GDTR/IDTR so APs load the exact same tables the BSP
     * is using (we are not allowed to reach into gdt.c/idt.c statics). */
    dtr_image_t gdtr, idtr;
    __asm__ volatile("sgdt %0" : "=m"(gdtr));
    __asm__ volatile("sidt %0" : "=m"(idtr));
    memcpy(param + AP_PARAM_GDTR, &gdtr, sizeof(gdtr));
    memcpy(param + AP_PARAM_IDTR, &idtr, sizeof(idtr));
}

/* Send INIT-SIPI-SIPI to one AP and wait for it to flag itself started. */
static int smp_boot_ap(uint32_t cpu) {
    uint32_t aid = percpu_data[cpu].apic_id;

    /* Allocate this AP's stack. */
    void* stack = kmalloc(AP_STACK_SIZE);
    if (!stack) {
        kprintf("[SMP] CPU %u: stack alloc failed\n", cpu);
        percpu_data[cpu].state = CPU_STATE_FAILED;
        return -1;
    }
    uint8_t* param = (uint8_t*)(AP_TRAMPOLINE_BASE + AP_PARAM_OFFSET);
    *(volatile uint64_t*)(param + AP_PARAM_STACK_TOP) =
        (uint64_t)stack + AP_STACK_SIZE;
    *(volatile uint64_t*)(param + AP_PARAM_ARG) = cpu;

    percpu_data[cpu].state = CPU_STATE_STARTING;
    ap_started_flag = 0;
    __asm__ volatile("" ::: "memory");

    kprintf("[SMP] Booting CPU %u (APIC ID %u)...\n", cpu, aid);

    /* INIT-SIPI-SIPI. */
    lapic_send_init(aid);
    smp_delay(2000000);                 /* >= 10 ms settle */

    lapic_send_startup(aid, AP_SIPI_VECTOR);
    smp_delay(40000);                   /* >= 200 us */

    /* If the AP already flagged in, skip the second SIPI. */
    if (__atomic_load_n(&ap_started_flag, __ATOMIC_ACQUIRE) == 0) {
        lapic_send_startup(aid, AP_SIPI_VECTOR);
    }

    /* Wait for the AP to come online (bounded). */
    uint64_t timeout = 50000000ULL;
    while (__atomic_load_n(&ap_started_flag, __ATOMIC_ACQUIRE) == 0 && timeout--) {
        __asm__ volatile("pause" ::: "memory");
    }

    if (__atomic_load_n(&ap_started_flag, __ATOMIC_ACQUIRE) == 0) {
        kprintf("[SMP] CPU %u FAILED to start (timeout)\n", cpu);
        percpu_data[cpu].state = CPU_STATE_FAILED;
        return -1;
    }

    kprintf("[SMP] CPU %u online\n", cpu);
    return 0;
}

/* Start all detected APs (CPUs 1..N-1). */
void smp_start_aps(void) {
    if (smp_num_cpus <= 1) {
        kprintf("[SMP] Single CPU; no APs to start.\n");
        return;
    }

    uint64_t kernel_cr3 = read_cr3();
    smp_setup_trampoline(kernel_cr3);

    for (uint32_t cpu = 1; cpu < smp_num_cpus; cpu++) {
        smp_boot_ap(cpu);
    }

    kprintf("[SMP] AP bring-up done: %u/%u CPUs online\n",
            smp_num_online, smp_num_cpus);
}

/* Bring a single (already-detected) CPU online on demand. */
int cpu_online(uint32_t cpu) {
    if (cpu >= smp_num_cpus) return -1;
    if (cpu == 0) return 0;             /* BSP always online */
    if (cpu_is_online(cpu)) return 0;

    uint64_t kernel_cr3 = read_cr3();
    smp_setup_trampoline(kernel_cr3);
    int r = smp_boot_ap(cpu);
    if (r != 0) return r;

    for (uint32_t i = 0; i < num_online_callbacks; i++) online_callbacks[i](cpu);
    return 0;
}

/*
 * 64-bit C entry for an Application Processor. Reached from ap_trampoline.asm
 * after the AP is in long mode with the kernel GDT/IDT/CR3 and its own stack.
 * Marks itself online and parks. (No scheduler work here -- future milestone.)
 */
void smp_ap_main(uint64_t cpu) {
    if (cpu >= MAX_CPUS) cpu = 0;

    /* Bring this core's local APIC up. */
    lapic_init();

    percpu_init((uint32_t)cpu);
    percpu_data[cpu].state = CPU_STATE_ONLINE;
    __atomic_add_fetch(&smp_num_online, 1, __ATOMIC_SEQ_CST);

    /* Tell the BSP we are alive. */
    __atomic_store_n(&ap_started_flag, 1, __ATOMIC_RELEASE);

    /* Park. Interrupts stay off until SMP scheduling exists. */
    for (;;) {
        __asm__ volatile("hlt");
    }
}

/*
 * Initialize SMP. Brings the BSP's LAPIC up, enumerates CPUs from the MADT,
 * and starts the APs. Returns the number of logical CPUs known (>= 1).
 */
int smp_init(void) {
    kprintf("[SMP] Initializing symmetric multiprocessing...\n");

    /* BSP = CPU 0. */
    percpu_data[0].cpu_id = 0;
    percpu_data[0].state = CPU_STATE_ONLINE;
    percpu_data[0].flags = CPU_FLAG_BSP | CPU_FLAG_ENABLED;
    percpu_init(0);
    smp_num_cpus = 1;
    smp_num_online = 1;

    /* BSP local APIC. */
    lapic_init();
    percpu_data[0].apic_id = lapic_get_id();
    cpu_identify(0);

    /* Enumerate from the MADT (fills apic_ids and CPU count). */
    smp_detect_cpus();

    /* Start the APs. */
    smp_start_aps();

    kprintf("[SMP] SMP init complete: %u detected, %u online\n",
            smp_num_cpus, smp_num_online);
    return (int)smp_num_cpus;
}

void smp_print_info(void) {
    kprintf("\n=== SMP Information ===\n");
    kprintf("Total CPUs:  %u\n", smp_num_cpus);
    kprintf("Online CPUs: %u\n", smp_num_online);
    for (uint32_t cpu = 0; cpu < smp_num_cpus; cpu++) {
        percpu_data_t* p = &percpu_data[cpu];
        const char* st = "?";
        switch (p->state) {
            case CPU_STATE_OFFLINE:  st = "OFFLINE";  break;
            case CPU_STATE_ONLINE:   st = "ONLINE";   break;
            case CPU_STATE_STARTING: st = "STARTING"; break;
            case CPU_STATE_STOPPING: st = "STOPPING"; break;
            case CPU_STATE_FAILED:   st = "FAILED";   break;
        }
        kprintf("CPU %u: %s (APIC ID %u)%s\n", cpu, st, p->apic_id,
                (p->flags & CPU_FLAG_BSP) ? " [BSP]" : "");
    }
    kprintf("\n");
}

void smp_print_stats(void) {
    kprintf("\n=== SMP Statistics ===\n");
    for (uint32_t cpu = 0; cpu < smp_num_cpus; cpu++) {
        if (!cpu_is_online(cpu)) continue;
        percpu_data_t* p = &percpu_data[cpu];
        kprintf("CPU %u: ticks=%llu irqs=%llu\n",
                cpu, p->total_ticks, p->interrupt_count);
    }
}

/**
 * health_monitor_detect_stalls - Detect CPU stalls by checking heartbeat progression
 *
 * Checks if each online CPU's heartbeat counter has advanced since the last check.
 * If a heartbeat is stuck, the CPU is likely stalled (deadlock, infinite loop, etc.).
 *
 * Returns: true if at least one stall was detected, false otherwise
 */
bool health_monitor_detect_stalls(void) {
    bool stall_found = false;

    uint32_t num_online = __atomic_load_n(&smp_num_online, __ATOMIC_SEQ_CST);
    for (int cpu = 0; cpu < num_online; cpu++) {
        uint64_t current = cpu_data(cpu)->health.heartbeat;
        uint64_t prev = cpu_data(cpu)->health.last_heartbeat;

        if (current == prev) {
            // Heartbeat not advancing = stall
            kprintf("[HEALTH] CPU%d stall: heartbeat stuck at %llu\n",
                    cpu, current);
            stall_found = true;
        }

        cpu_data(cpu)->health.last_heartbeat = current;
    }

    return stall_found;
}

#endif /* SMP_ENABLE */
