/*
 * Local APIC (Advanced Programmable Interrupt Controller) Driver
 * ===============================================================
 *
 * Implementation of Local APIC functionality for x86_64 SMP systems.
 * Each CPU has its own Local APIC for interrupt delivery and IPI.
 */

#include "../../include/lapic.h"
#include "../../include/kernel.h"
#include "../../include/x86_64.h"
#include "../../include/mem.h"
#include "../../include/perf.h"     /* rdtsc() */
#include "../../include/lapic_constants.h"

// Global LAPIC state
void* lapic_base = NULL;
bool lapic_enabled = false;
bool x2apic_mode = false;

// Default LAPIC base address (can be overridden by ACPI MADT)
#define LAPIC_DEFAULT_BASE  0xFEE00000

// LAPIC register read (memory-mapped)
uint32_t lapic_read(uint32_t reg) {
    if (x2apic_mode) {
        return x2apic_read(reg);
    }
    return *(volatile uint32_t*)((uint64_t)lapic_base + reg);
}

// LAPIC register write (memory-mapped)
void lapic_write(uint32_t reg, uint32_t value) {
    if (x2apic_mode) {
        x2apic_write(reg, value);
        return;
    }
    *(volatile uint32_t*)((uint64_t)lapic_base + reg) = value;
}

// Initialize Local APIC for current CPU
void lapic_init(void) {
    // Check if LAPIC is available via CPUID
    uint32_t eax, ebx, ecx, edx;
    __asm__ volatile("cpuid"
                     : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                     : "a"(1));

    if (!(edx & CPUID_FEAT_APIC)) {
        kprintf("[LAPIC] ERROR: No Local APIC present!\n");
        return;
    }

    // Get LAPIC base address from MSR
    uint64_t apic_base_msr = rdmsr(0x1B);  // IA32_APIC_BASE

    if (lapic_base == NULL) {
        // First CPU - set up base address
        lapic_base = (void*)(apic_base_msr & 0xFFFFF000);

        if (lapic_base == NULL) {
            lapic_base = (void*)LAPIC_DEFAULT_BASE;
        }

        kprintf("[LAPIC] Base address: %p\n", lapic_base);
    }

    // Check for x2APIC support
    if (ecx & CPUID_FEAT_X2APIC) {
        kprintf("[LAPIC] x2APIC supported\n");
        // Enable x2APIC mode
        apic_base_msr |= APIC_MSR_X2APIC_ENABLE;
        wrmsr(0x1B, apic_base_msr);
        x2apic_mode = true;
    }

    // Enable LAPIC
    lapic_enable();

    uint32_t apic_id = lapic_get_id();
    uint32_t version = lapic_get_version();

    kprintf("[LAPIC] CPU APIC ID: %u, Version: 0x%x\n", apic_id, version);
}

// Enable Local APIC
void lapic_enable(void) {
    // Enable APIC via MSR
    uint64_t apic_base_msr = rdmsr(0x1B);
    apic_base_msr |= APIC_MSR_GLOBAL_ENABLE;
    wrmsr(0x1B, apic_base_msr);

    // Set Spurious Interrupt Vector Register
    lapic_write(LAPIC_SIVR, LAPIC_SIVR_ENABLE | SPURIOUS_VECTOR);

    // Set Task Priority Register to 0 (accept all interrupts)
    lapic_write(LAPIC_TPR, 0);

    lapic_enabled = true;
}

// Disable Local APIC
void lapic_disable(void) {
    // Disable APIC via Spurious Interrupt Vector Register
    uint32_t sivr = lapic_read(LAPIC_SIVR);
    lapic_write(LAPIC_SIVR, sivr & ~LAPIC_SIVR_ENABLE);

    lapic_enabled = false;
}

// Get Local APIC ID
uint32_t lapic_get_id(void) {
    if (x2apic_mode) {
        return x2apic_read(LAPIC_ID);
    }
    return lapic_read(LAPIC_ID) >> 24;
}

// Get Local APIC version
uint32_t lapic_get_version(void) {
    return lapic_read(LAPIC_VERSION) & 0xFF;
}

// Send End Of Interrupt
void lapic_eoi(void) {
    lapic_write(LAPIC_EOI, 0);
}

// Wait for IPI delivery
static void lapic_wait_ipi_delivery(void) {
    // Wait for delivery status to be idle
    uint32_t timeout = IPI_DELIVERY_TIMEOUT;
    while (timeout-- > 0) {
        uint32_t icr_low = lapic_read(LAPIC_ICR_LOW);
        if ((icr_low & LAPIC_DS_PENDING) == 0) {
            return;
        }
        __asm__ volatile("pause");
    }
    kprintf("[LAPIC] WARNING: IPI delivery timeout!\n");
}

// Send IPI to specific APIC ID
void lapic_send_ipi(uint32_t apic_id, uint32_t vector) {
    if (x2apic_mode) {
        // x2APIC mode: single 64-bit write
        uint64_t icr = ((uint64_t)apic_id << 32) |
                       LAPIC_DM_FIXED |
                       LAPIC_DEST_PHYSICAL |
                       LAPIC_LEVEL_ASSERT |
                       vector;
        x2apic_write(LAPIC_ICR_LOW, icr);
    } else {
        // xAPIC mode: two 32-bit writes
        lapic_wait_ipi_delivery();
        lapic_write(LAPIC_ICR_HIGH, apic_id << 24);
        lapic_write(LAPIC_ICR_LOW,
                   LAPIC_DM_FIXED |
                   LAPIC_DEST_PHYSICAL |
                   LAPIC_LEVEL_ASSERT |
                   vector);
    }
}

// Send IPI to all CPUs except self
void lapic_send_ipi_all_but_self(uint32_t vector) {
    if (x2apic_mode) {
        uint64_t icr = LAPIC_DSH_ALL_EXC_SELF |
                       LAPIC_DM_FIXED |
                       LAPIC_LEVEL_ASSERT |
                       vector;
        x2apic_write(LAPIC_ICR_LOW, icr);
    } else {
        lapic_wait_ipi_delivery();
        lapic_write(LAPIC_ICR_LOW,
                   LAPIC_DSH_ALL_EXC_SELF |
                   LAPIC_DM_FIXED |
                   LAPIC_LEVEL_ASSERT |
                   vector);
    }
}

// Send IPI to all CPUs including self
void lapic_send_ipi_all(uint32_t vector) {
    if (x2apic_mode) {
        uint64_t icr = LAPIC_DSH_ALL_INC_SELF |
                       LAPIC_DM_FIXED |
                       LAPIC_LEVEL_ASSERT |
                       vector;
        x2apic_write(LAPIC_ICR_LOW, icr);
    } else {
        lapic_wait_ipi_delivery();
        lapic_write(LAPIC_ICR_LOW,
                   LAPIC_DSH_ALL_INC_SELF |
                   LAPIC_DM_FIXED |
                   LAPIC_LEVEL_ASSERT |
                   vector);
    }
}

// Send INIT IPI (reset CPU)
void lapic_send_init(uint32_t apic_id) {
    if (x2apic_mode) {
        uint64_t icr = ((uint64_t)apic_id << 32) |
                       LAPIC_DM_INIT |
                       LAPIC_DEST_PHYSICAL |
                       LAPIC_LEVEL_ASSERT;
        x2apic_write(LAPIC_ICR_LOW, icr);
    } else {
        lapic_wait_ipi_delivery();
        lapic_write(LAPIC_ICR_HIGH, apic_id << 24);
        lapic_write(LAPIC_ICR_LOW,
                   LAPIC_DM_INIT |
                   LAPIC_DEST_PHYSICAL |
                   LAPIC_LEVEL_ASSERT);
    }
}

// Send STARTUP IPI (start AP at given vector)
void lapic_send_startup(uint32_t apic_id, uint32_t vector) {
    if (x2apic_mode) {
        uint64_t icr = ((uint64_t)apic_id << 32) |
                       LAPIC_DM_STARTUP |
                       LAPIC_DEST_PHYSICAL |
                       LAPIC_LEVEL_ASSERT |
                       (vector & 0xFF);
        x2apic_write(LAPIC_ICR_LOW, icr);
    } else {
        lapic_wait_ipi_delivery();
        lapic_write(LAPIC_ICR_HIGH, apic_id << 24);
        lapic_write(LAPIC_ICR_LOW,
                   LAPIC_DM_STARTUP |
                   LAPIC_DEST_PHYSICAL |
                   LAPIC_LEVEL_ASSERT |
                   (vector & 0xFF));
    }
}

// Microsecond delay (busy wait). Uses the kernel rdtsc() helper (perf.h) rather
// than the __rdtsc compiler builtin so it links cleanly in the freestanding
// kernel (no <x86intrin.h> available with -nostdinc).
static void udelay(uint64_t microseconds) {
    uint64_t start = rdtsc();
    uint64_t ticks = microseconds * TSC_TICKS_PER_US;

    while ((rdtsc() - start) < ticks) {
        __asm__ volatile("pause");
    }
}

// Initialize LAPIC timer
void lapic_timer_init(uint32_t frequency) {
    // Disable timer during setup
    lapic_write(LAPIC_TIMER, LAPIC_TIMER_MASKED);

    // Set divide value to 16
    lapic_write(LAPIC_TIMER_DCR, LAPIC_TIMER_DIV_16);

    // Calibrate timer by measuring ticks in 10ms
    lapic_write(LAPIC_TIMER_ICR, 0xFFFFFFFF);
    udelay(TIMER_CALIBRATE_US);
    uint32_t ticks = 0xFFFFFFFF - lapic_read(LAPIC_TIMER_CCR);

    // Calculate ticks per quantum (assumes frequency is in Hz)
    uint32_t ticks_per_quantum = (ticks * frequency) / 1000;

    // Set periodic mode with calculated value
    lapic_write(LAPIC_TIMER, LAPIC_TIMER_VECTOR | LAPIC_TIMER_PERIODIC);
    lapic_write(LAPIC_TIMER_ICR, ticks_per_quantum);

    kprintf("[LAPIC] Timer initialized: %u Hz (%u ticks)\n",
            frequency, ticks_per_quantum);
}

// Start LAPIC timer in one-shot mode
void lapic_timer_oneshot(uint64_t microseconds) {
    // Calculate ticks (approximate)
    uint32_t ticks = microseconds * TIMER_ONESHOT_SCALE;

    lapic_write(LAPIC_TIMER, LAPIC_TIMER_VECTOR);
    lapic_write(LAPIC_TIMER_ICR, ticks);
}

// Start LAPIC timer in periodic mode
void lapic_timer_periodic(uint32_t frequency) {
    lapic_timer_init(frequency);
}

// Stop LAPIC timer
void lapic_timer_stop(void) {
    lapic_write(LAPIC_TIMER, LAPIC_TIMER_MASKED);
}

// Read current timer count
uint32_t lapic_timer_read(void) {
    return lapic_read(LAPIC_TIMER_CCR);
}

// Get LAPIC error status
uint32_t lapic_get_error(void) {
    // Write to ESR to update it
    lapic_write(LAPIC_ESR, 0);
    return lapic_read(LAPIC_ESR);
}

// Clear LAPIC errors
void lapic_clear_error(void) {
    lapic_write(LAPIC_ESR, 0);
    lapic_write(LAPIC_ESR, 0);  // Write twice
}

// Check for x2APIC support
bool lapic_has_x2apic(void) {
    uint32_t eax, ebx, ecx, edx;
    __asm__ volatile("cpuid"
                     : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                     : "a"(1));
    return (ecx & CPUID_FEAT_X2APIC) != 0;
}

// Enable x2APIC mode
void x2apic_enable(void) {
    uint64_t apic_base_msr = rdmsr(0x1B);
    apic_base_msr |= APIC_MSR_X2APIC_ENABLE;
    apic_base_msr |= APIC_MSR_GLOBAL_ENABLE;
    wrmsr(0x1B, apic_base_msr);
    x2apic_mode = true;

    kprintf("[LAPIC] x2APIC mode enabled\n");
}

// Read x2APIC register (MSR-based)
uint32_t x2apic_read(uint32_t reg) {
    uint32_t msr = 0x800 + (reg >> 4);
    return (uint32_t)rdmsr(msr);
}

// Write x2APIC register (MSR-based)
void x2apic_write(uint32_t reg, uint64_t value) {
    uint32_t msr = 0x800 + (reg >> 4);
    wrmsr(msr, value);
}
