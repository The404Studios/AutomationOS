/*
 * tlb_uni.c -- single-CPU TLB management.
 * =======================================
 * The overhaul added a lazy multi-CPU TLB-shootdown design (arch/x86_64/tlb.c)
 * that depends on the SMP/IPI/LAPIC stack. AutomationOS currently boots
 * single-CPU, so we provide the tlb.h API with correct LOCAL-only semantics and
 * no SMP dependency. tlb.c + the smp/ipi/lapic sources remain in the tree for a
 * future multi-core bring-up; only this file is compiled today.
 *
 * On one CPU there are no remote TLBs to shoot down, so "lazy" deferral and IPIs
 * collapse to: flush this CPU's TLB immediately (INVLPG for a page, CR3 reload
 * for a full flush). tlb_flush_pending() is a no-op because nothing is deferred.
 */
#include "../../include/tlb.h"
#include "../../include/x86_64.h"   /* read_cr3 / write_cr3 */
#include "../../include/kernel.h"   /* kprintf */

void tlb_init(void) {
    kprintf("[TLB] single-CPU TLB management initialized\n");
}

void tlb_flush_page_lazy(void* addr, uint64_t cr3) {
    (void)cr3;   /* single address space view per CPU; just flush this page */
    __asm__ volatile("invlpg (%0)" :: "r"(addr) : "memory");
}

void tlb_flush_all_lazy(uint64_t cr3) {
    (void)cr3;
    /* Reload CR3 to flush all non-global TLB entries on this CPU. */
    write_cr3(read_cr3());
}

void tlb_flush_pending(void) {
    /* Nothing is deferred on a single CPU. */
}

void tlb_handle_ipi_flush(void) {
    /* No IPIs on a single CPU. */
}

void tlb_flush_all_contexts_local(void) {
    /* On single-CPU: just reload CR3 to flush all TLB entries. */
    write_cr3(read_cr3());
}

void tlb_handle_ipi_flush_all_contexts(void) {
    /* No IPIs on a single CPU. */
}

void tlb_print_stats(void) {
    kprintf("[TLB] single-CPU: no shootdown statistics\n");
}

void tlb_reset_stats(void) {
}
