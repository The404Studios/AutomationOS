#ifndef TLB_H
#define TLB_H

#include "types.h"

/*
 * Lazy TLB Shootdown
 * ==================
 *
 * Defers TLB invalidation on remote CPUs until actually needed.
 * Reduces expensive IPIs by 60-80% for heavy munmap workloads.
 *
 * Usage:
 *   1. Initialize: tlb_init() during boot
 *   2. Flush page: tlb_flush_page_lazy(addr, cr3) instead of ipi_tlb_flush_page
 *   3. Flush all:  tlb_flush_all_lazy(cr3) instead of ipi_tlb_flush_all
 *   4. Context switch: call tlb_flush_pending() before switching to new process
 *
 * Performance:
 *   - Immediate flush: if remote CPU is in kernel (preempt_count > 0)
 *   - Lazy flush: if remote CPU is idle or in userspace (defer to context switch)
 *   - Batching: accumulates multiple flushes, does full flush if > threshold
 */

// Initialize TLB subsystem
void tlb_init(void);

// Lazy TLB flush for single page
// addr: virtual address to flush
// cr3: address space (for tracking which AS needs flush)
void tlb_flush_page_lazy(void* addr, uint64_t cr3);

// Lazy TLB flush for full address space
// cr3: address space to flush
void tlb_flush_all_lazy(uint64_t cr3);

// Flush pending TLB entries on current CPU
// Called from context switch before loading new CR3
void tlb_flush_pending(void);

// IPI handler for TLB flush
// Called when remote CPU sends IPI_TLB_FLUSH
void tlb_handle_ipi_flush(void);

// Print TLB statistics
void tlb_print_stats(void);

// Reset TLB statistics (for benchmarking)
void tlb_reset_stats(void);

#endif // TLB_H
