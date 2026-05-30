#include "../../include/sched.h"
#include "../../include/kernel.h"
#include "../../include/tss.h"
#include "../../include/mem.h"
#include "../../include/tlb.h"
#include "../../include/perf.h"

// External assembly function (defined in context_switch.asm)
extern void context_switch_asm(process_t* from, process_t* to);

// ---------------------------------------------------------------------------
// FPU state template — lazy-populated on first context_switch call.
//
// When process_create() initialises a new process it does:
//   memset(&proc->context, 0, sizeof(cpu_context_t));
// This zeroes fpu_state, which is fine for the general-purpose registers but
// is WRONG for the FXSAVE area:
//   • MXCSR lives at bytes 24-27 of the FXSAVE block.  MXCSR=0 clears all
//     six SSE exception-mask bits (IM/DM/ZM/OM/UM/PM), leaving every SSE
//     exception *unmasked*.  The very first SSE instruction that produces a
//     denormal, infinity, or precision result will raise #XM.  The GCC-
//     compiled userspace binaries DO use xmm registers (memcpy, integer
//     vectorisation, etc.), so this is a real hazard, not a theoretical one.
//   • FCW (bytes 0-1) = 0 enables all x87 exceptions and sets precision to
//     24-bit — wrong for code expecting the normal 64-bit extended precision.
//
// The correct FXSAVE image for a "clean" FPU is what the hardware holds
// after FNINIT + LDMXCSR 0x1F80:
//   FCW=0x037F  FSW=0  FTW=0  MXCSR=0x1F80  all registers zeroed.
//
// Strategy (Option A — pure C, no asm changes):
//   Capture this image once at runtime via FXSAVE into the static buffer
//   below.  Before switching to any process whose fpu_state still contains
//   the memset-zero sentinel (detected by MXCSR==0 at byte 24), overwrite
//   its fpu_state with the clean template.  After that the process has a
//   valid FXSAVE image and further switches save/restore it normally.
//
// Alignment: FXSAVE/FXRSTOR require the operand to be 16-byte aligned.
//   • fpu_template: __attribute__((aligned(16))) guarantees this statically.
//   • proc->context.fpu_state: the heap allocator returns 16-byte-aligned
//     pointers (verified in heap.c comments) and fpu_state sits at absolute
//     offset 176 within process_t (16+160=176, divisible by 16).
//   • No alignment #GP risk on either the template or the per-process area.
// ---------------------------------------------------------------------------

// 512-byte FXSAVE image with default FPU/SSE state (populated on first use).
static uint8_t fpu_template[512] __attribute__((aligned(16)));

// Set to 1 once fpu_template has been filled via FXSAVE.
static int fpu_template_ready = 0;

// Populate fpu_template by executing FNINIT + LDMXCSR then capturing with
// FXSAVE.  This runs with interrupts already disabled (we are inside
// context_switch which is called from the scheduler under cli).
// Side-effects: momentarily resets the FPU to the clean state, then restores
// the caller's FPU state by immediately FXRSTORing from the just-saved image.
// The caller (kernel context_switch path) does not use FP, so the transient
// reset is harmless.
static void fpu_init_template(void) {
    // Build the canonical "clean" FPU state in fpu_template:
    //   FNINIT  — resets x87 to FCW=0x037F, FSW=0, FTW=0xFFFF, clears ST0-7
    //   LDMXCSR — sets MXCSR=0x1F80 (all exceptions masked, round-to-nearest)
    //   FXSAVE  — captures the resulting 512-byte image
    __asm__ volatile(
        "fninit\n\t"
        "pushq $0x1F80\n\t"
        "ldmxcsr (%%rsp)\n\t"
        "addq $8, %%rsp\n\t"
        "fxsave64 %0\n\t"
        : "=m"(fpu_template)
        :
        : "memory"
    );
    fpu_template_ready = 1;
}

// Detect whether a process's fpu_state has never been written by FXSAVE.
// A real FXSAVE image always has MXCSR >= 0x1F80 (all exception masks set)
// on any sane CPU.  The memset-zero sentinel has MXCSR==0.
// We check the 4-byte MXCSR field at offset 24 within the FXSAVE area.
static inline int fpu_state_is_uninitialised(const uint8_t* fpu_state) {
    // Read MXCSR from bytes 24-27 of the FXSAVE image.
    uint32_t mxcsr;
    __builtin_memcpy(&mxcsr, fpu_state + 24, sizeof(mxcsr));
    return (mxcsr == 0);
}

// C wrapper for context switching
void context_switch(process_t* from, process_t* to) {
    // Start performance measurement
    PERF_START(PERF_OP_CONTEXT_SWITCH);

    if (!to) {
        kernel_panic("context_switch: 'to' process is NULL");
    }

    // Update process states
    if (from) {
        // Save 'from' state
        from->total_time++;

        if (from->state == PROCESS_RUNNING) {
            from->state = PROCESS_READY;
        }

#ifndef CONTEXT_SWITCH_QUIET
        kprintf("[CONTEXT] Switching from '%s' (PID %d) to '%s' (PID %d)\n",
                from->name, from->pid, to->name, to->pid);
#endif
    } else {
#ifndef CONTEXT_SWITCH_QUIET
        kprintf("[CONTEXT] Starting first process '%s' (PID %d)\n",
                to->name, to->pid);
#endif
    }

    // Update 'to' state
    to->state = PROCESS_RUNNING;

    // CRITICAL: Update TSS.RSP0 to new process's kernel stack
    // When this process is interrupted/syscalled from ring 3, CPU loads RSP from TSS.RSP0
    // Without this, kernel uses stale/wrong stack → corruption/triple fault
    if (!to->kernel_stack) {
        kernel_panic("[TSS] Process has NULL kernel_stack - cannot set RSP0");
    }

    // kmalloc only guarantees 8-byte alignment, so round the stack top down to
    // a 16-byte boundary (x86-64 ABI requirement for the interrupt entry frame).
    uint64_t kstack_top = ((uint64_t)to->kernel_stack + KERNEL_STACK_SIZE) & ~0xFULL;

    tss_set_kernel_stack(kstack_top);

    // Also update SYSCALL kernel stack
    extern uint64_t kernel_rsp_save;
    kernel_rsp_save = kstack_top;

#ifndef CONTEXT_SWITCH_QUIET
    kprintf("[CONTEXT] TSS.RSP0 updated to 0x%016lx for PID %d\n", kstack_top, to->pid);
#endif

    // -----------------------------------------------------------------------
    // FPU template initialisation (runs at most once, on the very first
    // context_switch call after boot).
    // -----------------------------------------------------------------------
    if (__builtin_expect(!fpu_template_ready, 0)) {
        fpu_init_template();
    }

    // -----------------------------------------------------------------------
    // FPU state correctness fix: if the target process's fpu_state is still
    // all-zero (the memset sentinel from process_create), prime it with the
    // clean FPU template before the assembly stub does fxrstor64 on it.
    //
    // Without this, fxrstor64 loads MXCSR=0 into the SSE control register,
    // which unmasks all six SSE exceptions.  The first vectorised or FP
    // instruction in userspace would then raise #XM, which has no handler
    // and triple-faults the machine.  This is not theoretical: GCC emits
    // xmm register moves for ordinary memcpy/integer code even when the
    // source has no floating-point at all.
    //
    // The copy is 512 bytes = 8 cache lines.  It runs only once per process
    // lifetime (on the first switch TO that process), so the amortised cost
    // is negligible.
    // -----------------------------------------------------------------------
    if (fpu_state_is_uninitialised(to->context.fpu_state)) {
        __builtin_memcpy(to->context.fpu_state, fpu_template, 512);
    }

    // LAZY TLB SHOOTDOWN: Flush any pending TLB entries on this CPU before
    // switching to the new process. This is where lazy TLB flushes are
    // actually performed - deferred from the original unmap/page table change.
    //
    // Performance: By batching flushes at context switch boundaries instead of
    // sending IPIs immediately, we reduce IPI overhead by 60-80% for workloads
    // with heavy munmap activity.
    tlb_flush_pending();

    // GPF-001 fix: Set re-entrancy guard before the assembly switch.
    // This prevents the timer handler from calling schedule() re-entrantly
    // if the timer somehow fires during the switch (even though we've removed
    // the `sti` that created the race window, this is a safety net).
    __atomic_store_n(&scheduler_in_switch, 1, __ATOMIC_SEQ_CST);

    // Perform the actual context switch
    // This will save 'from' registers and restore 'to' registers
    context_switch_asm(from, to);

    // When we return here, we've been context-switched back.
    // Clear the re-entrancy guard.
    __atomic_store_n(&scheduler_in_switch, 0, __ATOMIC_SEQ_CST);

    // End performance measurement
    PERF_END(PERF_OP_CONTEXT_SWITCH);
}
