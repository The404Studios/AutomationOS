#ifndef LAPIC_CONSTANTS_H
#define LAPIC_CONSTANTS_H

/*
 * Local APIC Constants
 * ====================
 *
 * Defines bit flags and magic values for Local APIC operations.
 */

/* MSR Bits */
#define APIC_MSR_X2APIC_ENABLE  (1 << 10)   /* x2APIC enable bit in IA32_APIC_BASE */
#define APIC_MSR_GLOBAL_ENABLE  (1 << 11)   /* APIC global enable bit */
#define APIC_MSR_BSP            (1 << 8)    /* Bootstrap processor flag */

/* CPUID Feature Flags */
#define CPUID_FEAT_APIC         (1 << 9)    /* APIC present (EDX bit 9) */
#define CPUID_FEAT_X2APIC       (1 << 21)   /* x2APIC support (ECX bit 21) */

/* Timer Constants */
#define LAPIC_TIMER_VECTOR      0x20        /* Default timer interrupt vector */
#define LAPIC_TIMER_DIVIDE_16   16          /* Timer divisor value */

/* TSC Frequency Estimation */
#define TSC_FREQ_ESTIMATE_MHZ   3000        /* Approximate TSC frequency (3 GHz) */
#define TSC_TICKS_PER_US        (TSC_FREQ_ESTIMATE_MHZ)

/* Calibration Timing */
#define TIMER_CALIBRATE_US      10000       /* 10ms calibration period */
#define TIMER_ONESHOT_SCALE     100         /* Ticks per microsecond estimate */

/* IPI Delivery Timeout */
#define IPI_DELIVERY_TIMEOUT    100000      /* Maximum wait cycles for IPI delivery */

/* Spurious Interrupt Vector */
#define SPURIOUS_VECTOR         0xFF        /* Spurious interrupt vector number */

#endif
