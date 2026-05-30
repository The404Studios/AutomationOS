#ifndef LAPIC_H
#define LAPIC_H

#include "types.h"

/*
 * Local APIC (Advanced Programmable Interrupt Controller)
 * ========================================================
 *
 * Each CPU core has its own Local APIC for:
 * - Receiving external interrupts
 * - Sending Inter-Processor Interrupts (IPI)
 * - Local timer interrupts
 * - Performance monitoring interrupts
 * - Thermal sensor interrupts
 */

// Local APIC register offsets
#define LAPIC_ID                0x0020  // Local APIC ID
#define LAPIC_VERSION           0x0030  // Local APIC Version
#define LAPIC_TPR               0x0080  // Task Priority Register
#define LAPIC_APR               0x0090  // Arbitration Priority Register
#define LAPIC_PPR               0x00A0  // Processor Priority Register
#define LAPIC_EOI               0x00B0  // End Of Interrupt
#define LAPIC_RRD               0x00C0  // Remote Read Register
#define LAPIC_LDR               0x00D0  // Logical Destination Register
#define LAPIC_DFR               0x00E0  // Destination Format Register
#define LAPIC_SIVR              0x00F0  // Spurious Interrupt Vector Register
#define LAPIC_ISR               0x0100  // In-Service Register (0x100-0x170)
#define LAPIC_TMR               0x0180  // Trigger Mode Register (0x180-0x1F0)
#define LAPIC_IRR               0x0200  // Interrupt Request Register (0x200-0x270)
#define LAPIC_ESR               0x0280  // Error Status Register
#define LAPIC_ICR_LOW           0x0300  // Interrupt Command Register (low)
#define LAPIC_ICR_HIGH          0x0310  // Interrupt Command Register (high)
#define LAPIC_TIMER             0x0320  // Timer Local Vector Table Entry
#define LAPIC_THERMAL           0x0330  // Thermal Local Vector Table Entry
#define LAPIC_PERF              0x0340  // Performance Counter LVT Entry
#define LAPIC_LINT0             0x0350  // Local Interrupt 0 Vector Table Entry
#define LAPIC_LINT1             0x0360  // Local Interrupt 1 Vector Table Entry
#define LAPIC_ERROR             0x0370  // Error Vector Table Entry
#define LAPIC_TIMER_ICR         0x0380  // Timer Initial Count Register
#define LAPIC_TIMER_CCR         0x0390  // Timer Current Count Register
#define LAPIC_TIMER_DCR         0x03E0  // Timer Divide Configuration Register

// LAPIC Spurious Interrupt Vector Register bits
#define LAPIC_SIVR_ENABLE       0x00000100  // APIC Software Enable
#define LAPIC_SIVR_FOCUS        0x00000200  // Focus Processor Checking

// LAPIC Timer bits
#define LAPIC_TIMER_PERIODIC    0x00020000  // Periodic mode
#define LAPIC_TIMER_MASKED      0x00010000  // Masked
#define LAPIC_TIMER_DIV_1       0x0B        // Divide by 1
#define LAPIC_TIMER_DIV_2       0x00        // Divide by 2
#define LAPIC_TIMER_DIV_4       0x01        // Divide by 4
#define LAPIC_TIMER_DIV_8       0x02        // Divide by 8
#define LAPIC_TIMER_DIV_16      0x03        // Divide by 16
#define LAPIC_TIMER_DIV_32      0x08        // Divide by 32
#define LAPIC_TIMER_DIV_64      0x09        // Divide by 64
#define LAPIC_TIMER_DIV_128     0x0A        // Divide by 128

// LAPIC Delivery Mode
#define LAPIC_DM_FIXED          0x00000000
#define LAPIC_DM_LOWEST         0x00000100
#define LAPIC_DM_SMI            0x00000200
#define LAPIC_DM_NMI            0x00000400
#define LAPIC_DM_INIT           0x00000500
#define LAPIC_DM_STARTUP        0x00000600

// LAPIC Destination Mode
#define LAPIC_DEST_PHYSICAL     0x00000000
#define LAPIC_DEST_LOGICAL      0x00000800

// LAPIC Delivery Status
#define LAPIC_DS_IDLE           0x00000000
#define LAPIC_DS_PENDING        0x00001000

// LAPIC Level
#define LAPIC_LEVEL_DEASSERT    0x00000000
#define LAPIC_LEVEL_ASSERT      0x00004000

// LAPIC Trigger Mode
#define LAPIC_TM_EDGE           0x00000000
#define LAPIC_TM_LEVEL          0x00008000

// LAPIC Destination Shorthand
#define LAPIC_DSH_NONE          0x00000000
#define LAPIC_DSH_SELF          0x00040000
#define LAPIC_DSH_ALL_INC_SELF  0x00080000
#define LAPIC_DSH_ALL_EXC_SELF  0x000C0000

// IPI types
#define IPI_RESCHEDULE          0x40  // Force reschedule
#define IPI_TLB_FLUSH           0x41  // TLB flush
#define IPI_FUNCTION_CALL       0x42  // Call function on remote CPU
#define IPI_STOP                0x43  // Stop CPU
#define IPI_TEST                0x44  // Test IPI

// LAPIC initialization
void lapic_init(void);
void lapic_enable(void);
void lapic_disable(void);

// LAPIC register access
uint32_t lapic_read(uint32_t reg);
void lapic_write(uint32_t reg, uint32_t value);

// LAPIC identification
uint32_t lapic_get_id(void);
uint32_t lapic_get_version(void);

// End Of Interrupt
void lapic_eoi(void);

// IPI (Inter-Processor Interrupt) operations
void lapic_send_ipi(uint32_t apic_id, uint32_t vector);
void lapic_send_ipi_all_but_self(uint32_t vector);
void lapic_send_ipi_all(uint32_t vector);
void lapic_send_init(uint32_t apic_id);
void lapic_send_startup(uint32_t apic_id, uint32_t vector);

// LAPIC timer
void lapic_timer_init(uint32_t frequency);
void lapic_timer_oneshot(uint64_t microseconds);
void lapic_timer_periodic(uint32_t frequency);
void lapic_timer_stop(void);
uint32_t lapic_timer_read(void);

// LAPIC error handling
uint32_t lapic_get_error(void);
void lapic_clear_error(void);

// x2APIC support (for > 255 CPUs)
bool lapic_has_x2apic(void);
void x2apic_enable(void);
uint32_t x2apic_read(uint32_t reg);
void x2apic_write(uint32_t reg, uint64_t value);

// Global LAPIC state
extern void* lapic_base;
extern bool lapic_enabled;
extern bool x2apic_mode;

#endif // LAPIC_H
