#include "../../include/syscall.h"
#include "../../include/kernel.h"
#include "../../include/sched.h"
#include "../../include/mem.h"
#include "../../include/kref.h"
#include "../../include/drivers.h"
#include "../../include/vfs.h"
#include "../../include/input.h"
#include "../../include/tss.h"
#include "../../include/string.h"
#include "../../include/x86_64.h"
#include "../../include/ipc.h"
#include "../../include/rtc.h"
#include "../../include/procapi.h"
#include "../../include/clipboard.h"
#include "../../include/ahci.h"
#include "../../include/perf.h"

// External serial driver functions
extern void serial_write(const char* str, size_t len);

// cleanup helper for heap-allocated path buffers. A 4096-byte path buffer on
// the kernel stack, combined with the VFS chain's own buffers (vfs_mkdir_recursive
// -> vfs_mkdir -> vfs_path_lookup, each 4096B), overflows the 8KB kernel stack
// (no guard page) -- a ring-3 mkdir of a nested path is enough. Path-taking
// handlers therefore heap-allocate; __attribute__((cleanup)) auto-frees on EVERY
// return path so we never leak. kfree(NULL) is safe.
static inline void free_path_buf(char** p) { if (*p) kfree(*p); }

// SYS_EXIT - Terminate calling process
int64_t sys_exit(uint64_t status, uint64_t arg2, uint64_t arg3,
                 uint64_t arg4, uint64_t arg5, uint64_t arg6) {
    (void)arg2; (void)arg3; (void)arg4; (void)arg5; (void)arg6;

    process_t* current = process_get_current();
    if (!current) {
        // BUG-016 fix: Can't return from sys_exit with no process
        kprintf("[SYSCALL] sys_exit: No current process - kernel panic\n");
        kernel_panic("sys_exit called with no current process");
        return ESRCH;  // Never reached
    }

    kprintf("[SYSCALL] sys_exit: Process '%s' (PID %d) exiting with status %d\n",
            current->name, current->pid, (int)status);

    // Mark process as terminated
    current->state = PROCESS_TERMINATED;
    current->exit_status = (int)status;

    // Wake a parent blocked in waitpid() (and drain our own wait queue). In the
    // cooperative model the parent is already enqueued+BLOCKED by the time it
    // switched to us, so there is no lost-wakeup race.
    process_on_terminate(current);

    // Remove from scheduler
    scheduler_remove_process(current);

    // Schedule next process
    // Note: This will not return to this process
    schedule();

    // Should never reach here
    return ESUCCESS;
}

// ── Page-table walk helpers for fork address-space copy ───────────────

#define FORK_PML4I(va)  (((uint64_t)(va) >> 39) & 0x1FF)
#define FORK_PDPTI(va)  (((uint64_t)(va) >> 30) & 0x1FF)
#define FORK_PDI(va)    (((uint64_t)(va) >> 21) & 0x1FF)
#define FORK_PTI(va)    (((uint64_t)(va) >> 12) & 0x1FF)

// Copy every user-mapped 4KiB page from the parent (current CR3) into the
// child's address space.  Returns 0 on success, -1 if any allocation fails.
static int fork_copy_user_pages(process_t* child) {
    uint64_t parent_cr3 = read_cr3() & ~0xFFFULL;
    // Walk the parent's page tables + copy its pages via the DIRECT MAP. fork runs
    // under the PARENT's CR3 and NEVER switches to kernel CR3, so dereferencing the
    // parent's PT/PD/PDPT pages and copying its frames via raw phys==virt would #PF
    // on any frame the parent's mutated identity map doesn't cover (the phys==virt
    // bug family; the twin of the exec/cow/vma sites). PHYS_TO_DIRECT resolves on the
    // shared higher-half alias regardless of CR3. (vmm_map_page() args below stay RAW
    // physical -- they are PTE values, not derefs.)
    uint64_t* parent_pml4 = (uint64_t*)PHYS_TO_DIRECT(parent_cr3);

    // Point all subsequent vmm_map_page() calls at the child's PML4.
    paging_set_target(child->context.cr3);

    // CoW (#20): when enabled, share process-private pages with the child
    // read-only instead of deep-copying; the first write to a shared page traps
    // and is copied on demand (cow_handle_write). parent_pte_changed records
    // whether we demoted any of the PARENT's own PTEs to RO+COW (needs a TLB
    // flush before fork returns so the parent's writes actually trap).
    int use_cow = cow_enabled();
    int parent_pte_changed = 0;
    int n_cow = 0, n_eager = 0, n_shared = 0;   // accounting / CoW evidence

    for (int pml4i = 0; pml4i < 256; pml4i++) {
        if (!(parent_pml4[pml4i] & PAGE_PRESENT)) continue;

        uint64_t* pdpt = (uint64_t*)PHYS_TO_DIRECT(parent_pml4[pml4i] & ~0xFFFULL);
        for (int pdpti = 0; pdpti < 512; pdpti++) {
            if (!(pdpt[pdpti] & PAGE_PRESENT)) continue;

            uint64_t* pd = (uint64_t*)PHYS_TO_DIRECT(pdpt[pdpti] & ~0xFFFULL);
            for (int pdi = 0; pdi < 512; pdi++) {
                if (!(pd[pdi] & PAGE_PRESENT)) continue;

                // 2 MiB huge page – split into 4 KiB pages.
                if (pd[pdi] & (1ULL << 7)) {
                    uint64_t base_phys = pd[pdi] & ~0x1FFFFFULL;
                    uint32_t base_flags = (uint32_t)(pd[pdi] & 0xFFF);
                    base_flags &= ~(1ULL << 7);

                    // Only copy user-accessible huge pages.
                    if (!(base_flags & PAGE_USER)) continue;

                    uint64_t base_va = ((uint64_t)pml4i << 39)
                                     | ((uint64_t)pdpti << 30)
                                     | ((uint64_t)pdi << 21);
                    for (int pti = 0; pti < 512; pti++) {
                        uint64_t va = base_va + (pti * PAGE_SIZE);
                        void* new_page = pmm_alloc_page();
                        if (!new_page) goto fail;
                        memcpy(PHYS_TO_DIRECT(new_page),
                               PHYS_TO_DIRECT(base_phys + pti * PAGE_SIZE), PAGE_SIZE);
                        vmm_map_page((void*)va, new_page, base_flags);
                    }
                    continue;
                }

                uint64_t* pt = (uint64_t*)PHYS_TO_DIRECT(pd[pdi] & ~0xFFFULL);
                for (int pti = 0; pti < 512; pti++) {
                    if (!(pt[pti] & PAGE_PRESENT)) continue;
                    if (!(pt[pti] & PAGE_USER))   continue;   // skip kernel pages

                    uint64_t va = ((uint64_t)pml4i << 39)
                                | ((uint64_t)pdpti << 30)
                                | ((uint64_t)pdi  << 21)
                                | ((uint64_t)pti  << 12);
                    // Phys-addr mask must clear BOTH low flag bits and high
                    // flag bits: with W^X, data PTEs carry PAGE_NX (bit 63), so
                    // `& ~0xFFF` alone would leave bit 63 set and make `phys`
                    // non-canonical (the subsequent memcpy would #GP). Likewise
                    // preserve NX in the copied flags (full 64-bit, not 0xFFF)
                    // so the child's data pages stay no-execute.
                    uint64_t phys  = pt[pti] & 0x000FFFFFFFFFF000ULL;
                    uint64_t flags = pt[pti] & ~0x000FFFFFFFFFF000ULL;

                    // Non-owned pages are shared windows (framebuffer MMIO, shm,
                    // read-only shared data) owned by another subsystem. Map the
                    // SAME physical frame into the child — no copy, no ownership,
                    // no refcount — so the child sees the real resource and
                    // teardown never frees it. (The old eager fork wastefully
                    // COPIED these every fork AND leaked the copies on child exit,
                    // since a non-PTE_OWNED copy is never reclaimed.)
                    if (!(flags & PTE_OWNED)) {
                        vmm_map_page((void*)va, (void*)phys, flags);
                        n_shared++;
                        continue;
                    }

                    // Process-private (PTE_OWNED) page: CoW-share with the child.
                    // cow_incref() registers the child as a co-owner (and fails on
                    // refcount saturation -> eager fallback below).
                    if (use_cow && cow_incref(phys)) {
                        uint64_t child_flags = flags;
                        if (flags & PAGE_WRITE) {
                            // Writable: demote BOTH copies to RO+COW so the first
                            // write on either side triggers copy-on-write.
                            child_flags = (flags & ~(uint64_t)PAGE_WRITE) | PTE_COW;
                            pt[pti] = phys | child_flags;   // parent, in place
                            parent_pte_changed = 1;
                        }
                        // Read-only owned pages (code/rodata) are shared as-is;
                        // a write to them is a genuine W^X fault (no PTE_COW).
                        vmm_map_page((void*)va, (void*)phys, child_flags);
                        n_cow++;
                        continue;
                    }

                    // Eager fallback: private deep copy (CoW disabled or refcount
                    // saturated) of an owned page.
                    void* new_page = pmm_alloc_page();
                    if (!new_page) goto fail;
                    memcpy(PHYS_TO_DIRECT(new_page), PHYS_TO_DIRECT(phys), PAGE_SIZE);
                    vmm_map_page((void*)va, new_page, flags);
                    n_eager++;
                }
            }
        }
    }

    // Flush the parent's TLB if we demoted any of its PTEs to RO+COW in place,
    // so its first post-fork write to a shared page actually traps. Reloading
    // CR3 flushes all non-global entries for the live (parent) address space.
    if (parent_pte_changed) {
        __asm__ volatile("mov %%cr3, %%rax; mov %%rax, %%cr3" ::: "rax", "memory");
    }

    kprintf("[FORK] address space: %d CoW-shared, %d eager-copied, %d window-shared\n",
            n_cow, n_eager, n_shared);
    paging_reset_target();
    return 0;

fail:
    kprintf("[FORK] Out of memory while copying address space\n");
    paging_reset_target();
    return -1;
}

// ── IRETQ trampoline – the child's first instruction after a fork ────
// Mirrors enter_usermode (usermode.asm): load the user DATA selector (0x1B)
// into DS/ES/FS/GS, then iretq the 5-qword frame sys_fork built on the child's
// kernel stack (RIP/CS=0x23/RFLAGS/RSP/SS=0x1B). Without the data-segment loads
// the child would run ring 3 with stale kernel selectors in DS/ES.
static void __attribute__((naked)) fork_do_iretq(void) {
    // Preserve RAX across the segment loads: it holds the child's fork() return
    // value (0). `mov $0x1B,%ax` would clobber AX and make the child see a
    // non-zero fork() result (taking the parent branch). push/pop keeps it intact
    // (and is robust if fork later inherits the full parent register set).
    __asm__ volatile(
        "push %rax\n\t"
        "mov $0x1B, %ax\n\t"
        "mov %ax, %ds\n\t"
        "mov %ax, %es\n\t"
        "mov %ax, %fs\n\t"
        "mov %ax, %gs\n\t"
        "pop %rax\n\t"
        "iretq\n\t"
    );
}

// ── SYS_FORK ──────────────────────────────────────────────────────────
// Creates a child process that is a nearly-exact copy of the caller.
// Parent gets back the child's PID; the child sees return value 0.
int64_t sys_fork(uint64_t arg1, uint64_t arg2, uint64_t arg3,
                 uint64_t arg4, uint64_t arg5, uint64_t arg6) {
    (void)arg1; (void)arg2; (void)arg3; (void)arg4; (void)arg5; (void)arg6;

    process_t* parent = process_get_current();
    if (!parent) {
        kprintf("[SYSCALL] sys_fork: No current process\n");
        return ESRCH;
    }

    kprintf("[SYSCALL] sys_fork: Forking process '%s' (PID %d)\n",
            parent->name, parent->pid);

    // Read user RIP / RSP / RFLAGS from the parent's kernel stack.
    // These were saved by syscall.asm at fixed offsets from KSTACK_TOP:
    //   KSTACK_TOP - 8  = user RSP
    //   KSTACK_TOP - 16 = user RIP (originally RCX)
    //   KSTACK_TOP - 24 = user RFLAGS (originally R11)
    uint64_t p_kstop = (uint64_t)parent->kernel_stack + KERNEL_STACK_SIZE;
    uint64_t user_rsp    = *(uint64_t*)(p_kstop - 8);
    uint64_t user_rip    = *(uint64_t*)(p_kstop - 16);
    uint64_t user_rflags = *(uint64_t*)(p_kstop - 24);

    // Build a descriptive name: "parentname-forked"
    char child_name[64];
    size_t nlen = strlen(parent->name);
    if (nlen > 55) nlen = 55;
    memcpy(child_name, parent->name, nlen);
    memcpy(child_name + nlen, "-forked", 8);

    // ── Create child process ───────────────────────────────────────
    // process_create allocates: proc struct, kernel stack, new PML4
    // (shallow-copied kernel higher-half + deep-copied identity map).
    process_t* child = process_create(child_name, (void*)user_rip);
    if (!child) {
        kprintf("[SYSCALL] sys_fork: process_create failed\n");
        return ENOMEM;
    }

    // ── Clone user-space address space ─────────────────────────────
    if (fork_copy_user_pages(child) < 0) {
        kprintf("[SYSCALL] sys_fork: address-space copy failed\n");
        process_destroy(child);
        return ENOMEM;
    }

    // ── Build a fresh IRETQ frame at the top of the child's kernel stack ──
    // The child will run for the first time via fork_do_iretq, which pops
    // SS / RSP / RFLAGS / CS / RIP and lands in user mode with RAX = 0.
    // This kernel's GDT is SYSRET-compatible: user DATA=0x1B, user CODE=0x23
    // (see usermode.asm). The iretq frame must therefore carry CS=0x23 and
    // SS=0x1B — the original code had them swapped (CS=0x1B), which made iretq
    // reject CS (a data selector) with #GP err=0x18 the first time the child ran.
    // Sanitize RFLAGS exactly like enter_usermode: clear TF (no single-step),
    // force IF=1, clear IOPL so the child has no I/O privilege.
    user_rflags = (user_rflags & ~0x100ULL & ~0x3000ULL) | 0x200ULL;

    uint64_t c_kstop = (uint64_t)child->kernel_stack + KERNEL_STACK_SIZE;
    uint64_t* sp = (uint64_t*)c_kstop;

    *--sp = 0x1B;            // SS     – user data segment, RPL 3
    *--sp = user_rsp;        // RSP    – parent's user stack pointer
    *--sp = user_rflags;     // RFLAGS – sanitized copy of the fork caller's
    *--sp = 0x23;            // CS     – user code segment, RPL 3
    *--sp = user_rip;        // RIP    – instruction after fork() syscall

    // ── Set up CPU context for first context-switch ────────────────
    // Preserve the child's CR3 (set by process_create) before zeroing.
    uint64_t child_cr3 = child->context.cr3;

    // Zero the context; most GP regs will be overwritten by IRETQ anyway.
    memset(&child->context, 0, sizeof(cpu_context_t));
    child->context.rax    = 0;                        // child sees 0
    child->context.cr3    = child_cr3;                // child's own PML4
    child->context.rsp    = (uint64_t)sp;             // point at IRETQ frame
    child->context.rip    = (uint64_t)fork_do_iretq;  // first instruction
    child->context.rflags = 0x202;                    // IF enabled

    // Stash user entry/rsp for the scheduler if it ever inspects them.
    child->user_entry = user_rip;
    child->user_rsp   = user_rsp;

    // The parent may have been forked already – override the pid chain.
    child->parent_pid = parent->pid;

    // ── Make the child schedulable ─────────────────────────────────
    scheduler_add_process(child);

    kprintf("[SYSCALL] sys_fork: child '%s' (PID %d) ready, parent '%s' (PID %d) returning %d\n",
            child->name, child->pid, parent->name, parent->pid, child->pid);

    // Parent returns child PID; the child will see 0 because we set
    // child->context.rax = 0 above.
    return (int64_t)child->pid;
}

// ── SYS_THREAD_CREATE ─────────────────────────────────────────────────
// Create a thread that SHARES the caller's address space (CR3) but has its own
// kernel stack, user stack, registers and FPU state. The thread begins executing
// entry(arg) in ring 3. Returns the new tid (== the thread's pid), or a negative
// errno. The heavy lifting (CR3/as_refcount sharing, kernel stack, trampoline
// setup) is in thread_create(); here we validate args and schedule it.
//
// Args: (entry, arg, user_stack). user_stack is the TOP of a caller-allocated
// stack (it grows down); thread_create 16-aligns it. We do a light sanity check
// that entry + user_stack are in the user half — a thread runs the caller's own
// code/data, so we cannot fully validate them, but we reject obvious garbage.
int64_t sys_thread_create(uint64_t entry, uint64_t arg, uint64_t user_stack,
                          uint64_t arg4, uint64_t arg5, uint64_t arg6) {
    (void)arg4; (void)arg5; (void)arg6;

    process_t* caller = process_get_current();
    if (!caller) {
        return ESRCH;
    }
    if (entry == 0 || entry >= USER_SPACE_END) {
        return EINVAL;
    }
    if (user_stack == 0 || user_stack >= USER_SPACE_END) {
        return EINVAL;
    }

    process_t* t = thread_create(caller, entry, arg, user_stack);
    if (!t) {
        return ENOMEM;
    }

    kprintf("[SYSCALL] sys_thread_create: tid=%d tgid=%d entry=0x%lx arg=0x%lx "
            "stack=0x%lx (creator '%s' pid=%d)\n",
            t->pid, t->tgid, entry, arg, user_stack, caller->name, caller->pid);

    // Make the thread schedulable. (thread_create published it into the process
    // table but did NOT enqueue it — mirror exec/fork which enqueue here.)
    scheduler_add_process(t);

    return (int64_t)t->pid;
}

// ── SYS_THREAD_EXIT ───────────────────────────────────────────────────
// Terminate the calling thread: record its return value, wake any joiner blocked
// on its join wait_object, mark it TERMINATED, and switch away. The AS-refcount
// is dropped (and the CR3 torn down iff this was the last user) later, in
// process_unref(), when the joiner reaps the PCB — exactly the zombie model
// waitpid uses. Re-uses the existing terminate plumbing (process_on_terminate +
// scheduler_remove_process + schedule), so a thread with no joiner is reaped by
// its parent's waitpid path just like any process. Does not return.
int64_t sys_thread_exit(uint64_t retval, uint64_t arg2, uint64_t arg3,
                        uint64_t arg4, uint64_t arg5, uint64_t arg6) {
    (void)arg2; (void)arg3; (void)arg4; (void)arg5; (void)arg6;

    process_t* current = process_get_current();
    if (!current) {
        kernel_panic("sys_thread_exit called with no current process");
        return ESRCH;
    }

    kprintf("[SYSCALL] sys_thread_exit: tid=%d (tgid=%d) exiting, retval=%d\n",
            current->pid, current->tgid, (int)retval);

    // Record the return value for the joiner BEFORE marking TERMINATED, so a
    // joiner woken by the signal below reads the final value.
    current->thread_retval = (int)retval;
    current->exit_status = (int)retval;  // also expose via waitpid

    current->state = PROCESS_TERMINATED;

    // Wake any thread blocked in sys_thread_join on THIS thread's join object.
    // wait_object_signal wakes-all (a thread can in principle be joined by one,
    // but waking all is harmless: a late joiner just finds it TERMINATED).
    wait_object_signal(&current->thread_join_wo, 1);

    // Also wake a parent blocked in waitpid() and drain our own wait queue.
    process_on_terminate(current);

    // Remove from the scheduler and switch away. schedule() drops the scheduler's
    // "current" reference for a TERMINATED process; the PCB then lives on as a
    // zombie until the joiner (or parent waitpid) reaps it, which is where
    // process_unref() finally runs the AS-refcount-gated CR3 teardown.
    scheduler_remove_process(current);
    schedule();

    // Not reached.
    return ESUCCESS;
}

// ── SYS_THREAD_JOIN ───────────────────────────────────────────────────
// Block until the target thread (same thread group) exits, copy out its return
// value, and reap it. Returns 0 on success, or a negative errno:
//   ESRCH  - no such tid
//   EPERM  - target is not in the caller's thread group (tgid mismatch)
//   EINVAL - target is the caller itself, or not a thread
int64_t sys_thread_join(uint64_t tid, uint64_t retval_out, uint64_t arg3,
                        uint64_t arg4, uint64_t arg5, uint64_t arg6) {
    (void)arg3; (void)arg4; (void)arg5; (void)arg6;

    process_t* caller = process_get_current();
    if (!caller) {
        return ESRCH;
    }
    if ((uint32_t)tid == caller->pid) {
        return EINVAL;  // a thread cannot join itself
    }

    // process_get_by_pid returns the target WITH A REFERENCE HELD, which keeps
    // its PCB (and its join wait_object) alive across our block below.
    process_t* target = process_get_by_pid((uint32_t)tid);
    if (!target) {
        return ESRCH;
    }

    // Permission: only threads of the SAME group may join each other.
    if (target->tgid != caller->tgid) {
        process_unref(target);
        return EPERM;
    }

    // Block until the target has terminated. The cooperative scheduler makes the
    // "check TERMINATED then block" sequence atomic from our perspective (the
    // target can't run between them), so there is no lost-wakeup race: if it is
    // already TERMINATED we skip the block entirely; otherwise we block on its
    // join object and thread_exit's wait_object_signal wakes us.
    while (target->state != PROCESS_TERMINATED) {
        (void)wait_object_block(&target->thread_join_wo, 0);
        // Re-loop to re-check the state (defensive against spurious wakeups).
    }

    // Copy out the thread's return value, if requested.
    if (retval_out) {
        int rv = target->thread_retval;
        if (copy_to_user((void*)retval_out, &rv, sizeof(rv)) != COPY_SUCCESS) {
            process_unref(target);
            return EFAULT;
        }
    }

    int joined_tid = (int)target->pid;

    // Reap the zombie — IDENTICAL discipline to sys_waitpid's reap. We still hold
    // the get_by_pid reference in `target`; we DO NOT drop it separately. Orphan
    // it first so a concurrent parent waitpid scan can't also try to reap it, then
    // process_destroy() does scheduler_remove_process (no-op for a terminated,
    // off-queue thread) + a single process_unref that mirrors waitpid. The final
    // process_unref inside the teardown runs the AS-refcount-gated CR3 teardown,
    // tearing the SHARED address space down only if this thread was its last user.
    target->parent_pid = 0;
    process_destroy(target);

    kprintf("[SYSCALL] sys_thread_join: tid=%d joined by pid=%d\n",
            joined_tid, caller->pid);
    return 0;
}

// SYS_READ - Read from file descriptor
int64_t sys_read(uint64_t fd, uint64_t buf, uint64_t count,
                 uint64_t arg4, uint64_t arg5, uint64_t arg6) {
    (void)arg4; (void)arg5; (void)arg6;

#ifndef SYSCALL_QUIET
    kprintf("[SYSCALL] sys_read: fd=%d buf=%p count=%d\n",
            (int)fd, (void*)buf, (int)count);
#endif

    // Validate file descriptor
    if (fd >= MAX_FDS) {
        kprintf("[SYSCALL] sys_read: Invalid file descriptor %d\n", (int)fd);
        return EBADF;
    }

    // Validate count
    if (count > MAX_READ_SIZE) {
        kprintf("[SYSCALL] sys_read: Count too large (%zu bytes, max %d)\n",
                (size_t)count, MAX_READ_SIZE);
        return EINVAL;
    }

    if (count == 0) {
        return 0;
    }

    // Handle stdin specially (fd 0)
    if (fd == 0) {
        // Read from PS/2 keyboard
        char c = ps2_getchar();
        if (c == 0) {
            return 0;  // No data available
        }

        // Copy to user space
        if (copy_to_user((void*)buf, &c, 1) != COPY_SUCCESS) {
            return EFAULT;
        }

        return 1;
    }

    // Use VFS for file reads
    char* kernel_buf = kmalloc(count);
    if (!kernel_buf) {
        return ENOMEM;
    }

    ssize_t result = vfs_read((int)fd, kernel_buf, count);
    if (result > 0) {
        if (copy_to_user((void*)buf, kernel_buf, result) != COPY_SUCCESS) {
            kfree(kernel_buf);
            return EFAULT;
        }
    }

    kfree(kernel_buf);
    return result;
}

// SYS_WRITE - Write to file descriptor
int64_t sys_write(uint64_t fd, uint64_t buf, uint64_t count,
                  uint64_t arg4, uint64_t arg5, uint64_t arg6) {
    (void)arg4; (void)arg5; (void)arg6;

    // Debug logging disabled for performance

    // Validate file descriptor
    if (fd >= MAX_FDS) {
        kprintf("[SYSCALL] sys_write: Invalid file descriptor %d\n", (int)fd);
        return EBADF;
    }

    // Validate count
    if (count > MAX_WRITE_SIZE) {
        kprintf("[SYSCALL] sys_write: Count too large (%zu bytes, max %d)\n",
                (size_t)count, MAX_WRITE_SIZE);
        return EINVAL;
    }

    if (count == 0) {
        return 0;
    }

    // BUG-011 fix: Add additional safety check for extremely large allocations
    // Even though MAX_WRITE_SIZE is checked, add defense in depth
    if (count > 16 * 1024 * 1024) {  // 16MB absolute maximum
        kprintf("[SYSCALL] sys_write: Count too large (%zu bytes, max 16MB)\n",
                (size_t)count);
        return EINVAL;
    }

    // Allocate kernel buffer
    char* kernel_buf = kmalloc(count);
    if (!kernel_buf) {
        kprintf("[SYSCALL] sys_write: Failed to allocate kernel buffer\n");
        return ENOMEM;
    }

    // Copy from user space
    if (copy_from_user(kernel_buf, (const void*)buf, count) != COPY_SUCCESS) {
        kfree(kernel_buf);
        return EFAULT;
    }

    // Handle stdout/stderr specially (fd 1, 2)
    if (fd == 1 || fd == 2) {
        // Write to serial console
        serial_write(kernel_buf, (size_t)count);

        // Also write to VGA text buffer so user can see output in QEMU
        {
            static uint16_t* vga = (uint16_t*)0xb8000;
            static int vga_pos = 0;
            for (size_t i = 0; i < count; i++) {
                char c = kernel_buf[i];
                if (c == '\b' || c == 127) {
                    // Backspace: move cursor back, erase character
                    if (vga_pos > 0 && (vga_pos % 80) > 0) {
                        vga_pos--;
                        vga[vga_pos] = 0x0F20;  // space, white on black
                    }
                } else if (c == '\n') {
                    vga_pos = ((vga_pos / 80) + 1) * 80;
                } else if (c >= 32) {
                    vga[vga_pos++] = (uint16_t)c | 0x0F00;
                }
                if (vga_pos >= 80 * 25) {
                    for (int j = 0; j < 80 * 24; j++)
                        vga[j] = vga[j + 80];
                    for (int j = 80 * 24; j < 80 * 25; j++)
                        vga[j] = 0x0F20;
                    vga_pos = 80 * 24;
                }
            }
        }

        kfree(kernel_buf);
        return (int64_t)count;
    }

    // Use VFS for file writes
    ssize_t result = vfs_write((int)fd, kernel_buf, count);
    kfree(kernel_buf);
    return result;
}

// SYS_OPEN - Open file
int64_t sys_open(uint64_t path, uint64_t flags, uint64_t mode,
                 uint64_t arg4, uint64_t arg5, uint64_t arg6) {
    (void)arg4; (void)arg5; (void)arg6;

#ifndef SYSCALL_QUIET
    kprintf("[SYSCALL] sys_open: path=%p flags=%d mode=%d\n",
            (void*)path, (int)flags, (int)mode);
#endif

    // Validate path pointer
    if (!path) {
        return EFAULT;
    }

    // Copy path from user space
    char* kernel_path __attribute__((cleanup(free_path_buf))) = (char*)kmalloc(MAX_PATH_LEN);
    if (!kernel_path) {
        return ENOMEM;
    }
    if (copy_user_string(kernel_path, (const void*)path, MAX_PATH_LEN) != COPY_SUCCESS) {
        return EFAULT;
    }

    // Ensure null termination
    kernel_path[MAX_PATH_LEN - 1] = '\0';

    // Open file via VFS
    int fd = vfs_open(kernel_path, (int)flags, (int)mode);
    if (fd < 0) {
        kprintf("[SYSCALL] sys_open: Failed to open %s\n", kernel_path);
        return EBADF;
    }

#ifndef SYSCALL_QUIET
    kprintf("[SYSCALL] sys_open: Opened %s as fd %d\n", kernel_path, fd);
#endif
    return fd;
}

// SYS_CLOSE - Close file
int64_t sys_close(uint64_t fd, uint64_t arg2, uint64_t arg3,
                  uint64_t arg4, uint64_t arg5, uint64_t arg6) {
    (void)arg2; (void)arg3; (void)arg4; (void)arg5; (void)arg6;

#ifndef SYSCALL_QUIET
    kprintf("[SYSCALL] sys_close: fd=%d\n", (int)fd);
#endif

    // Validate file descriptor
    if (fd >= MAX_FDS) {
        kprintf("[SYSCALL] sys_close: Invalid file descriptor %d\n", (int)fd);
        return EBADF;
    }

    // Don't close stdin/stdout/stderr
    if (fd <= 2) {
        return 0;
    }

    // Close file via VFS
    int result = vfs_close((int)fd);
    if (result < 0) {
        return EBADF;
    }

    return 0;
}

// SYS_GETPID - Get process ID
int64_t sys_getpid(uint64_t arg1, uint64_t arg2, uint64_t arg3,
                    uint64_t arg4, uint64_t arg5, uint64_t arg6) {
    (void)arg1; (void)arg2; (void)arg3; (void)arg4; (void)arg5; (void)arg6;

    process_t* current = process_get_current();
    if (!current) {
        kprintf("[SYSCALL] sys_getpid: No current process\n");
        return ESRCH;
    }

    kprintf("[SYSCALL] sys_getpid: Returning PID %d\n", current->pid);

    return (int64_t)current->pid;
}

// SYS_SLEEP - Real blocking sleep in MILLISECONDS.
//
// The PIT runs at 1000 Hz (1 tick == 1 ms), so arg1 (milliseconds) maps 1:1 onto
// timer ticks. This BLOCKS the caller (zero CPU) until its deadline. It is now
// expressed in terms of the UNIFIED wait primitive: sleep == wait(timer). We
// block on a process-global timer-only wait_object, passing an absolute deadline;
// wait_object_block() arms the timer sleep list and runs the shared block/resume
// discipline (set BLOCKED, link, pick a successor, sti/hlt idle if needed,
// cooperative_switch_to, and on resume unlink from both lists with no stale
// locals). The timer wakeup scan (sleep_list_wake_due, once per tick in BOTH the
// cooperative pit.c handler and the preemptive schedule_from_irq) re-readies us
// at the deadline. Works identically in both builds.
//
// g_sleep_wobj is a dedicated timer-wait object: nothing ever calls
// wait_object_signal() on it, so a sleeper is only ever woken by its timeout
// (then it self-unlinks from the object on resume). This preserves the exact old
// sleep semantics while sharing one code path with event waits.
static wait_object_t g_sleep_wobj = WAIT_OBJECT_INITIALIZER;

int64_t sys_sleep(uint64_t ms, uint64_t arg2, uint64_t arg3,
                  uint64_t arg4, uint64_t arg5, uint64_t arg6) {
    (void)arg2; (void)arg3; (void)arg4; (void)arg5; (void)arg6;

    // ms == 0: nothing to wait for — just yield the CPU (same as before).
    if (ms == 0) {
        return sys_yield(0, 0, 0, 0, 0, 0);
    }

    process_t* current = process_get_current();
    if (!current) {
        return ESRCH;
    }

    // Absolute deadline in tick units (== ms at 1000 Hz). Overflow is not a
    // practical concern for a 64-bit tick counter, but stay defensive.
    uint64_t now = timer_get_ticks();
    uint64_t deadline = now + ms;
    if (deadline < now) {
        return EINVAL;
    }

    // Single block primitive: a timer-armed wait. Returns 0 (timeout) for a real
    // sleep; the return value is irrelevant here since this object is never
    // signalled. All of the block/park/switch/resume/cleanup lives in
    // wait_object_block(), shared with the event-wait (wait queue) path.
    (void)wait_object_block(&g_sleep_wobj, deadline);

    // Resumed here after the timer woke us at the deadline.
    return ESUCCESS;
}

// SYS_READ_EVENT - Read one character from keyboard buffer (non-blocking)
// Returns: ASCII char (>0) if available, 0 if no input
int64_t sys_read_event(uint64_t arg1, uint64_t arg2, uint64_t arg3,
                       uint64_t arg4, uint64_t arg5, uint64_t arg6) {
    (void)arg1; (void)arg2; (void)arg3; (void)arg4; (void)arg5; (void)arg6;

    extern char ps2_getchar(void);
    char c = ps2_getchar();
    return (int64_t)(unsigned char)c;
}

// SYS_EXECVE - Execute program (replaces current process)
// Minimal implementation: spawns new process, exits current
int64_t sys_execve(uint64_t path, uint64_t argv, uint64_t envp,
                   uint64_t arg4, uint64_t arg5, uint64_t arg6) {
    (void)argv; (void)envp; (void)arg4; (void)arg5; (void)arg6;
    if (!path) return EFAULT;
    return sys_spawn(path, 0, 0, 0, 0, 0);
}

// SYS_WAITPID - Wait for child process to change state
// Simple non-blocking implementation: scans for terminated children
int64_t sys_waitpid(uint64_t pid, uint64_t status_ptr, uint64_t options,
                    uint64_t arg4, uint64_t arg5, uint64_t arg6) {
    (void)arg4; (void)arg5; (void)arg6;
    process_t* current = process_get_current();
    if (!current) return ESRCH;

    uint32_t target_pid = (uint32_t)pid;

    // Blocking waitpid: loop scanning for a terminated matching child. If a
    // matching child is still alive, sleep on our child_wait queue until a child
    // exits (sys_exit wakes us), then re-scan. The cooperative scheduler makes
    // scan+block atomic from our perspective (a child can't run between them), so
    // there is no lost-wakeup race.
    for (;;) {
        process_t* found = NULL;
        int have_child = 0;   // a matching child is still alive?

        // process_get_by_pid() returns the process WITH A REFERENCE HELD, so
        // every entry we don't keep must be unref'd.
        for (uint32_t i = 0; i < 256; i++) {
            process_t* p = process_get_by_pid(i);
            if (!p) continue;
            int match = (p->parent_pid == current->pid) &&
                        (pid == (uint64_t)-1 || p->pid == target_pid);
            if (match) {
                if (p->state == PROCESS_TERMINATED) {
                    found = p;          // keep ref; released by process_destroy
                    break;
                }
                have_child = 1;         // alive matching child -> may block
            }
            process_unref(p);
        }

        if (found) {
            // Write the child's real exit status. If the user pointer is bad, do
            // NOT reap (status would be lost) — release the ref and report EFAULT.
            if (status_ptr) {
                int status = found->exit_status;
                if (copy_to_user((void*)status_ptr, &status, sizeof(status)) != COPY_SUCCESS) {
                    process_unref(found);
                    return EFAULT;
                }
            }
            int child_pid = found->pid;
            // Orphan before teardown so a later scan can't re-match this PID.
            found->parent_pid = 0;
            process_destroy(found);
            return child_pid;
        }

        if (!have_child) {
            return ECHILD;   // no matching child exists at all
        }

        // A matching child is still running: block until a child exits. Lazily
        // allocate our wait queue (zeroed pointer until first wait).
        if (!current->child_wait) {
            current->child_wait = kmalloc(sizeof(wait_queue_t));
            if (current->child_wait) {
                wq_init((wait_queue_t*)current->child_wait);
            }
        }
        if (!current->child_wait) {
            return 0;   // OOM: fall back to non-blocking (caller retries/yields)
        }
        wq_block_current((wait_queue_t*)current->child_wait);
        // Resumed after a child exited -> loop re-scans.
    }
}

// SYS_YIELD - Voluntarily give up CPU timeslice
int64_t sys_yield(uint64_t arg1, uint64_t arg2, uint64_t arg3,
                  uint64_t arg4, uint64_t arg5, uint64_t arg6) {
    (void)arg1; (void)arg2; (void)arg3; (void)arg4; (void)arg5; (void)arg6;

    process_t* current = process_get_current();
    if (!current) {
        kprintf("[SYSCALL] sys_yield: No current process\n");
        return ESRCH;
    }

    // Add current back to the ready queue (priority-weighted: a higher-priority
    // yielder re-enters ACTIVE for a bounded number of bonus turns so it accrues
    // proportionally more CPU than lower-priority peers; nice >= 0 routes
    // straight to expired, unchanged), then pick next.
    scheduler_yield_requeue(current);
    process_t* next = scheduler_pick_next();
    // Never switch to a process with no kernel stack (it has nowhere to take a
    // trap/syscall) -- current simply keeps running.
    if (next && next != current && next->kernel_stack) {
#ifndef SCHEDULER_QUIET
        kprintf("[YIELD] '%s' (PID %d) -> '%s' (PID %d)\n",
                current->name, current->pid, next->name, next->pid);
#endif
        next->state = PROCESS_RUNNING;
        process_set_current(next);
        // cooperative_switch_to() sets next's TSS.RSP0 + SYSCALL kernel stack
        // and resumes next via context_switch (RESUME_CRETURN) or iretq
        // (RESUME_IRETQ, i.e. a timer-preempted process) as appropriate. This is
        // what lets a yielding cooperative task hand the CPU to a preempted one
        // (e.g. the never-yielding CPU burners) instead of starving them.
        current->resume_mode = RESUME_CRETURN;
        cooperative_switch_to(current, next);
    }

    return ESUCCESS;
}

// SYS_SPAWN - Create new process from ELF path in initrd
int64_t sys_spawn(uint64_t path, uint64_t arg2, uint64_t arg3,
                  uint64_t arg4, uint64_t arg5, uint64_t arg6) {
    (void)arg3; (void)arg4; (void)arg5; (void)arg6;

    if (!path) return EFAULT;

    char kpath[128];
    if (copy_from_user(kpath, (const void*)path, sizeof(kpath) - 1) != COPY_SUCCESS) {
        return EFAULT;
    }
    kpath[127] = '\0';

    // Spawn arguments: arg2 = user pointer to a space-separated args string (or 0
    // for none). Copy it into the exec layer's buffer NOW, while the caller's
    // address space is still live (before the CR3 switch below) — exec.c builds
    // the child's argv = [path, tokens...] from it. Fault-safe copy degrades to
    // "no args" on a bad pointer.
    extern char g_exec_spawn_args[256];
    g_exec_spawn_args[0] = '\0';
    if (arg2) {
        char kargs[256];
        if (copy_from_user(kargs, (const void*)arg2, sizeof(kargs) - 1) == COPY_SUCCESS) {
            kargs[255] = '\0';
            int ai = 0;
            for (; ai < 255 && kargs[ai]; ai++) g_exec_spawn_args[ai] = kargs[ai];
            g_exec_spawn_args[ai] = '\0';
        }
    }

    kprintf("[SPAWN] Spawning '%s' (args: '%s')...\n", kpath, g_exec_spawn_args);

    // The initrd lives at low physical (identity-mapped) addresses that OVERLAP
    // the user load region (0x200000+). When sys_spawn is called from a userspace
    // process (e.g. the compositor), the live CR3 remaps 0x200000+ to that
    // process's PRIVATE pages — so reading the initrd via its identity address
    // would read the CALLER's memory, not the initrd (the file would not be
    // found even though it is intact). Switch to the kernel CR3, where the
    // identity map is not shadowed, for the lookup + load, then restore.
    extern void* initrd_get_file(const char* path, uint64_t* size_out);
    extern int   elf_load_and_exec(void* elf_data, size_t elf_size, const char* name);
    extern uint64_t paging_kernel_cr3(void);

    uint64_t caller_cr3 = read_cr3();
    write_cr3(paging_kernel_cr3());

    uint64_t elf_size = 0;
    void* elf_data = initrd_get_file(kpath, &elf_size);
    int pid = -1;
    if (elf_data && elf_size != 0) {
        // elf_load_and_exec reads the ELF (still in the initrd) and builds the
        // new address space; it manages its own CR3 internally and leaves the
        // kernel CR3 active, which we restore below.
        pid = elf_load_and_exec(elf_data, elf_size, kpath);
    }

    // Not in the initrd (or it failed to load): try the VFS/ramfs, so the IDE
    // can run programs it just compiled (e.g. /tmp/sum). We read the file into a
    // KERNEL heap buffer while on the kernel CR3 -- this sidesteps the user-CR3
    // identity shadowing entirely (the buffer lives in kernel space).
    if (pid <= 0) {
        vfs_stat_t st;
        if (vfs_stat(kpath, &st) == 0 && st.st_size > 0 &&
            st.st_size < (16ULL * 1024 * 1024)) {
            size_t sz = (size_t)st.st_size;
            void* buf = kmalloc(sz);
            if (buf) {
                int vfd = vfs_open(kpath, 0 /* O_RDONLY */, 0);
                if (vfd >= 0) {
                    size_t got = 0;
                    ssize_t n;
                    while (got < sz &&
                           (n = vfs_read(vfd, (char*)buf + got, sz - got)) > 0) {
                        got += (size_t)n;
                    }
                    vfs_close(vfd);
                    if (got > 0) {
                        pid = elf_load_and_exec(buf, got, kpath);
                    }
                }
                kfree(buf);
            }
        }
    }

    write_cr3(caller_cr3);   // restore the caller's address space before returning

    if (pid <= 0) {
        kprintf("[SPAWN] not found / load failed: %s\n", kpath);
        return ESRCH;
    }

    kprintf("[SPAWN] Process '%s' created with PID %d\n", kpath, pid);
    return (int64_t)pid;
}

// SYS_MAP_FILE - Map initrd-backed file into calling process address space (zero-copy)
// Arguments:
//   path     - pointer to file path string in userspace
//   out_addr - pointer to write the mapped virtual address (uint64_t*)
//   out_size - pointer to write the file size (uint64_t*)
// Returns: 0 on success, negative error code on failure
int64_t sys_map_file(uint64_t path, uint64_t out_addr, uint64_t out_size,
                     uint64_t arg4, uint64_t arg5, uint64_t arg6) {
    (void)arg4; (void)arg5; (void)arg6;

    if (!path || !out_addr || !out_size) {
        return EFAULT;
    }

    // Copy path from userspace
    char kpath[256];
    if (copy_from_user(kpath, (const void*)path, 255) != COPY_SUCCESS) {
        return EFAULT;
    }
    kpath[255] = '\0';

    kprintf("[MAP_FILE] Mapping file: %s\n", kpath);

    // Look up file in VFS
    vfs_inode_t* inode = vfs_path_lookup(kpath);
    if (!inode) {
        kprintf("[MAP_FILE] File not found: %s\n", kpath);
        return ESRCH;
    }

    // Get the physical address of file data
    // Initrd-backed files point directly into identity-mapped physical memory
    uint64_t phys_data = (uint64_t)inode->data;
    uint64_t file_size = inode->size;

    if (!phys_data || file_size == 0) {
        kprintf("[MAP_FILE] Empty file or no data: %s\n", kpath);
        vfs_inode_put(inode);
                return EINVAL;
    }

    // Calculate pages needed (round up to page boundary)
    uint64_t pages = (file_size + PAGE_SIZE - 1) / PAGE_SIZE;

    kprintf("[MAP_FILE] phys=0x%lx size=%lu pages=%lu\n",
            phys_data, file_size, pages);

    // Use 0x50000000 as base VA for model file mappings
    uint64_t base_va = 0x50000000ULL;

    // Map each page into the current process's address space
    // The process's CR3 is active during syscall, so vmm_map_page targets it
    for (uint64_t i = 0; i < pages; i++) {
        vmm_map_page(
            (void*)(base_va + i * PAGE_SIZE),
            (void*)(phys_data + i * PAGE_SIZE),
            PAGE_PRESENT | PAGE_USER   // Read-only (no PAGE_WRITE)
        );
    }

    kprintf("[MAP_FILE] Mapped %s at 0x%lx (%lu bytes, %lu pages)\n",
            kpath, base_va, file_size, pages);

    // Write results to userspace. Check BOTH copies: a failure (bad out_addr/
    // out_size pointer) must surface as EFAULT, not a false ESUCCESS that leaves
    // the caller reading stale stack for the mapped address/size.
    if (copy_to_user((void*)out_addr, &base_va, sizeof(base_va)) != COPY_SUCCESS ||
        copy_to_user((void*)out_size, &file_size, sizeof(file_size)) != COPY_SUCCESS) {
        vfs_inode_put(inode);
        return EFAULT;
    }

    vfs_inode_put(inode);
    return ESUCCESS;
}

// ============================================================================
// I/O Control System Call
// ============================================================================

/**
 * SYS_IOCTL - I/O control operations
 * Used for device-specific operations (PTY window size, termios, etc.)
 */
int64_t sys_ioctl(uint64_t fd, uint64_t request, uint64_t argp,
                  uint64_t arg4, uint64_t arg5, uint64_t arg6) {
    (void)arg4; (void)arg5; (void)arg6;

    process_t* current = process_get_current();
    if (!current) {
        return ESRCH;
    }

    // Validate file descriptor (VFS owns the global fd table)
    vfs_file_t* file = vfs_fd_get((int)fd);
    if (fd >= MAX_FDS || !file) {
        kprintf("[SYSCALL] sys_ioctl: Invalid file descriptor %llu\n", fd);
        return EBADF;
    }

    // PTY ioctl commands (from kernel/drivers/pty/pty.h)
    #define TIOCGWINSZ  0x5413  // Get window size
    #define TIOCSWINSZ  0x5414  // Set window size
    #define TCGETS      0x5401  // Get termios
    #define TCSETS      0x5402  // Set termios

    // For now, we'll handle PTY ioctls directly
    // In the future, this should delegate to file->ops->ioctl()
    extern int pty_ioctl(vfs_file_t* file, uint32_t request, void* argp);

    // Check if this is a PTY device
    // TODO: Add proper device type checking
    int result = pty_ioctl(file, (uint32_t)request, (void*)argp);

    if (result < 0) {
        kprintf("[SYSCALL] sys_ioctl: ioctl failed (fd=%llu, request=0x%x)\n",
                fd, (uint32_t)request);
        return EINVAL;
    }

    return ESUCCESS;
}

// ============================================================================
// Directory System Calls
// ============================================================================

// SYS_OPENDIR - Open directory for reading
int64_t sys_opendir(uint64_t path, uint64_t arg2, uint64_t arg3,
                    uint64_t arg4, uint64_t arg5, uint64_t arg6) {
    (void)arg2; (void)arg3; (void)arg4; (void)arg5; (void)arg6;

    if (!path) {
        return EFAULT;
    }

    // Copy path from userspace
    char* kernel_path __attribute__((cleanup(free_path_buf))) = (char*)kmalloc(MAX_PATH_LEN);
    if (!kernel_path) {
        return ENOMEM;
    }
    if (copy_user_string(kernel_path, (const void*)path, MAX_PATH_LEN) != COPY_SUCCESS) {
        return EFAULT;
    }
    kernel_path[MAX_PATH_LEN - 1] = '\0';

    // Call VFS opendir
    int dirfd = vfs_opendir(kernel_path);
    if (dirfd < 0) {
        kprintf("[SYSCALL] sys_opendir: Failed to open directory %s\n", kernel_path);
        return dirfd;
    }

    kprintf("[SYSCALL] sys_opendir: Opened directory %s as dirfd %d\n", kernel_path, dirfd);
    return dirfd;
}

// SYS_READDIR - Read directory entry
int64_t sys_readdir(uint64_t dirfd, uint64_t entry_ptr, uint64_t arg3,
                    uint64_t arg4, uint64_t arg5, uint64_t arg6) {
    (void)arg3; (void)arg4; (void)arg5; (void)arg6;

    if (!entry_ptr) {
        return EFAULT;
    }

    // Allocate kernel buffer for dirent
    struct dirent kernel_entry;
    memset(&kernel_entry, 0, sizeof(kernel_entry));

    // Read directory entry
    int result = vfs_readdir((int)dirfd, &kernel_entry);
    if (result < 0) {
        // End of directory or error
        return result;
    }

    // Copy to userspace
    if (copy_to_user((void*)entry_ptr, &kernel_entry, sizeof(kernel_entry)) != COPY_SUCCESS) {
        return EFAULT;
    }

    return 0;  // Success
}

// SYS_CLOSEDIR - Close directory
int64_t sys_closedir(uint64_t dirfd, uint64_t arg2, uint64_t arg3,
                     uint64_t arg4, uint64_t arg5, uint64_t arg6) {
    (void)arg2; (void)arg3; (void)arg4; (void)arg5; (void)arg6;

    kprintf("[SYSCALL] sys_closedir: dirfd=%d\n", (int)dirfd);

    int result = vfs_closedir((int)dirfd);
    if (result < 0) {
        return EBADF;
    }

    return 0;
}

// SYS_STAT - Get file status
int64_t sys_stat(uint64_t path, uint64_t buf_ptr, uint64_t arg3,
                 uint64_t arg4, uint64_t arg5, uint64_t arg6) {
    (void)arg3; (void)arg4; (void)arg5; (void)arg6;

    if (!path || !buf_ptr) {
        return EFAULT;
    }

    // Copy path from userspace
    char* kernel_path __attribute__((cleanup(free_path_buf))) = (char*)kmalloc(MAX_PATH_LEN);
    if (!kernel_path) {
        return ENOMEM;
    }
    if (copy_user_string(kernel_path, (const void*)path, MAX_PATH_LEN) != COPY_SUCCESS) {
        return EFAULT;
    }
    kernel_path[MAX_PATH_LEN - 1] = '\0';

    // Get file status
    vfs_stat_t kernel_stat;
    int result = vfs_stat(kernel_path, &kernel_stat);
    if (result < 0) {
        kprintf("[SYSCALL] sys_stat: Failed to stat %s\n", kernel_path);
        return result;
    }

    // Copy to userspace
    if (copy_to_user((void*)buf_ptr, &kernel_stat, sizeof(kernel_stat)) != COPY_SUCCESS) {
        return EFAULT;
    }

    kprintf("[SYSCALL] sys_stat: %s - size=%lu mode=%o\n",
            kernel_path, kernel_stat.st_size, kernel_stat.st_mode);

    return 0;
}

// SYS_UNLINK - Delete file
int64_t sys_unlink(uint64_t path, uint64_t arg2, uint64_t arg3,
                   uint64_t arg4, uint64_t arg5, uint64_t arg6) {
    (void)arg2; (void)arg3; (void)arg4; (void)arg5; (void)arg6;

    if (!path) {
        return EFAULT;
    }

    // Copy path from userspace
    char* kernel_path __attribute__((cleanup(free_path_buf))) = (char*)kmalloc(MAX_PATH_LEN);
    if (!kernel_path) {
        return ENOMEM;
    }
    if (copy_user_string(kernel_path, (const void*)path, MAX_PATH_LEN) != COPY_SUCCESS) {
        return EFAULT;
    }
    kernel_path[MAX_PATH_LEN - 1] = '\0';

    // Delete file
    int result = vfs_unlink(kernel_path);
    if (result < 0) {
        kprintf("[SYSCALL] sys_unlink: Failed to unlink %s\n", kernel_path);
        return result;
    }

    kprintf("[SYSCALL] sys_unlink: Deleted %s\n", kernel_path);
    return 0;
}

// SYS_RENAME - Rename/move file
int64_t sys_rename(uint64_t oldpath, uint64_t newpath, uint64_t arg3,
                   uint64_t arg4, uint64_t arg5, uint64_t arg6) {
    (void)arg3; (void)arg4; (void)arg5; (void)arg6;

    if (!oldpath || !newpath) {
        return EFAULT;
    }

    // MAX_PATH_LEN is 4096 and KERNEL_STACK_SIZE is 8192, so TWO such buffers on
    // the kernel stack overflow it (ring-3 triggerable stack smash). Heap-allocate
    // both path buffers instead.
    char* kernel_oldpath = (char*)kmalloc(MAX_PATH_LEN);
    char* kernel_newpath = (char*)kmalloc(MAX_PATH_LEN);
    if (!kernel_oldpath || !kernel_newpath) {
        if (kernel_oldpath) kfree(kernel_oldpath);
        if (kernel_newpath) kfree(kernel_newpath);
        return ENOMEM;
    }

    if (copy_user_string(kernel_oldpath, (const void*)oldpath, MAX_PATH_LEN) != COPY_SUCCESS) {
        kfree(kernel_oldpath); kfree(kernel_newpath);
        return EFAULT;
    }
    kernel_oldpath[MAX_PATH_LEN - 1] = '\0';

    if (copy_user_string(kernel_newpath, (const void*)newpath, MAX_PATH_LEN) != COPY_SUCCESS) {
        kfree(kernel_oldpath); kfree(kernel_newpath);
        return EFAULT;
    }
    kernel_newpath[MAX_PATH_LEN - 1] = '\0';

    // Rename file
    int result = vfs_rename(kernel_oldpath, kernel_newpath);
    if (result < 0) {
        kprintf("[SYSCALL] sys_rename: Failed to rename %s to %s\n",
                kernel_oldpath, kernel_newpath);
    } else {
        kprintf("[SYSCALL] sys_rename: Renamed %s to %s\n",
                kernel_oldpath, kernel_newpath);
    }
    kfree(kernel_oldpath); kfree(kernel_newpath);
    return result < 0 ? result : 0;
}

// SYS_MKDIR - create a directory (parents created as needed)
int64_t sys_mkdir(uint64_t path, uint64_t mode, uint64_t arg3,
                  uint64_t arg4, uint64_t arg5, uint64_t arg6) {
    (void)arg3; (void)arg4; (void)arg5; (void)arg6;

    if (!path) {
        return EFAULT;
    }

    char* kernel_path __attribute__((cleanup(free_path_buf))) = (char*)kmalloc(MAX_PATH_LEN);
    if (!kernel_path) {
        return ENOMEM;
    }
    if (copy_user_string(kernel_path, (const void*)path, MAX_PATH_LEN) != COPY_SUCCESS) {
        return EFAULT;
    }
    kernel_path[MAX_PATH_LEN - 1] = '\0';

    uint32_t m = (uint32_t)(mode & 0xFFF);
    if (m == 0) {
        m = 0755;
    }

    int result = vfs_mkdir_recursive(kernel_path, m);
    if (result < 0) {
        return result;
    }
    return 0;
}

// SYS_TRUNCATE - truncate file to specified length
int64_t sys_truncate(uint64_t path, uint64_t length, uint64_t arg3,
                     uint64_t arg4, uint64_t arg5, uint64_t arg6) {
    (void)arg3; (void)arg4; (void)arg5; (void)arg6;

    if (!path) {
        return EFAULT;
    }

    // Copy path from userspace
    char* kernel_path __attribute__((cleanup(free_path_buf))) = (char*)kmalloc(MAX_PATH_LEN);
    if (!kernel_path) {
        return ENOMEM;
    }
    if (copy_user_string(kernel_path, (const void*)path, MAX_PATH_LEN) != COPY_SUCCESS) {
        return EFAULT;
    }
    kernel_path[MAX_PATH_LEN - 1] = '\0';

    // Truncate file
    int result = vfs_truncate(kernel_path, (off_t)length);
    if (result < 0) {
        kprintf("[SYSCALL] sys_truncate: Failed to truncate %s to %lld\n",
                kernel_path, (long long)length);
        return result;
    }

    return 0;
}

// SYS_FTRUNCATE - truncate file via file descriptor
int64_t sys_ftruncate(uint64_t fd, uint64_t length, uint64_t arg3,
                      uint64_t arg4, uint64_t arg5, uint64_t arg6) {
    (void)arg3; (void)arg4; (void)arg5; (void)arg6;

    // Truncate file
    int result = vfs_ftruncate((int)fd, (off_t)length);
    if (result < 0) {
        kprintf("[SYSCALL] sys_ftruncate: Failed to truncate fd %lld to %lld\n",
                (long long)fd, (long long)length);
        return result;
    }

    return 0;
}

// SYS_FSYNC - flush file data to storage
int64_t sys_fsync(uint64_t fd, uint64_t arg2, uint64_t arg3,
                  uint64_t arg4, uint64_t arg5, uint64_t arg6) {
    (void)arg2; (void)arg3; (void)arg4; (void)arg5; (void)arg6;

    // Flush file
    int result = vfs_fsync((int)fd);
    if (result < 0) {
        kprintf("[SYSCALL] sys_fsync: Failed to sync fd %lld\n", (long long)fd);
        return result;
    }

    return 0;
}

// SYS_SYNC - flush all dirty data to storage
int64_t sys_sync(uint64_t arg1, uint64_t arg2, uint64_t arg3,
                 uint64_t arg4, uint64_t arg5, uint64_t arg6) {
    (void)arg1; (void)arg2; (void)arg3; (void)arg4; (void)arg5; (void)arg6;

    // Flush all dirty data
    int result = vfs_sync();
    if (result < 0) {
        kprintf("[SYSCALL] sys_sync: Failed to sync all filesystems\n");
        return result;
    }

    return 0;
}

// ============================================================================
// Memory / framebuffer / time syscalls (M1 graphics platform)
// ============================================================================

// SYS_MMAP - allocate an anonymous memory region in the caller's address space
int64_t sys_mmap(uint64_t hint, uint64_t len, uint64_t prot, uint64_t flags,
                 uint64_t arg5, uint64_t arg6) {
    (void)hint; (void)flags; (void)arg5; (void)arg6;
    process_t* current = process_get_current();
    if (!current) return ESRCH;
    if (len == 0) return EINVAL;
    /* Cap len so the page-count rounding (len + PAGE_SIZE - 1) can't overflow
     * and so a single mmap can't be asked to back an absurd region. */
    if (len > (256ULL * 1024 * 1024)) return EINVAL;
    void* p = vmm_mmap_anon(current->context.cr3, len, (uint32_t)prot);
    if (!p) return ENOMEM;
    return (int64_t)(uint64_t)p;
}

// SYS_MUNMAP - free a region previously returned by sys_mmap
int64_t sys_munmap(uint64_t addr, uint64_t len, uint64_t arg3,
                   uint64_t arg4, uint64_t arg5, uint64_t arg6) {
    (void)arg3; (void)arg4; (void)arg5; (void)arg6;
    process_t* current = process_get_current();
    if (!current) return ESRCH;
    if (addr == 0 || len == 0) return EINVAL;
    /* Cap len so the unmap loop (len/PAGE_SIZE iterations, no yield point in the
     * cooperative kernel) can't be driven to billions of iterations by a hostile
     * userspace, hanging the whole system. Matches sys_mmap's cap. */
    if (len > (256ULL * 1024 * 1024)) return EINVAL;
    return vmm_munmap(current->context.cr3, addr, len);
}

// SYS_FB_ACQUIRE - map the framebuffer into the caller and report its geometry
int64_t sys_fb_acquire(uint64_t out_info, uint64_t arg2, uint64_t arg3,
                       uint64_t arg4, uint64_t arg5, uint64_t arg6) {
    (void)arg2; (void)arg3; (void)arg4; (void)arg5; (void)arg6;
    process_t* current = process_get_current();
    if (!current) return ESRCH;

    fb_info_t info;
    if (framebuffer_get_info(&info) != 0) return ENOTSUP;

    // Map the framebuffer into the FB shared VA window (not freed on teardown).
    const uint64_t fb_user_va = 0x40000000ULL;
    uint64_t size = (uint64_t)info.pitch * (uint64_t)info.height;
    if (vmm_map_phys_into(current->context.cr3, fb_user_va, info.phys_base, size,
                          PAGE_PRESENT | PAGE_WRITE | PAGE_USER) != 0) {
        return ENOMEM;
    }

    if (out_info) {
        fb_acquire_t out;
        out.vaddr  = fb_user_va;
        out.width  = info.width;
        out.height = info.height;
        out.pitch  = info.pitch;
        out.bpp    = info.bpp;
        if (copy_to_user((void*)out_info, &out, sizeof(out)) != COPY_SUCCESS) {
            return EFAULT;
        }
    }
    return 0;
}

// SYS_GET_TICKS_MS - monotonic milliseconds since boot
int64_t sys_get_ticks_ms(uint64_t arg1, uint64_t arg2, uint64_t arg3,
                         uint64_t arg4, uint64_t arg5, uint64_t arg6) {
    (void)arg1; (void)arg2; (void)arg3; (void)arg4; (void)arg5; (void)arg6;
    return (int64_t)timer_get_ticks_ms();
}

// SYS_TIME - seconds since the Unix epoch, read from the CMOS RTC
int64_t sys_time(uint64_t arg1, uint64_t arg2, uint64_t arg3,
                 uint64_t arg4, uint64_t arg5, uint64_t arg6) {
    (void)arg1; (void)arg2; (void)arg3; (void)arg4; (void)arg5; (void)arg6;
    return rtc_unix_time();
}

// SYS_GETTIME - fill a user rtc_time_t with broken-down wall-clock time
int64_t sys_gettime(uint64_t uptr, uint64_t arg2, uint64_t arg3,
                    uint64_t arg4, uint64_t arg5, uint64_t arg6) {
    (void)arg2; (void)arg3; (void)arg4; (void)arg5; (void)arg6;
    if (!uptr) return EFAULT;
    rtc_time_t t;
    rtc_read(&t);
    if (copy_to_user((void*)uptr, &t, sizeof(t)) != COPY_SUCCESS) return EFAULT;
    return ESUCCESS;
}

// SYS_RANDOM - fill a user buffer with random bytes (RDRAND or PRNG fallback)
int64_t sys_random(uint64_t buf, uint64_t len, uint64_t arg3,
                   uint64_t arg4, uint64_t arg5, uint64_t arg6) {
    (void)arg3; (void)arg4; (void)arg5; (void)arg6;
    if (!buf) return EFAULT;
    if (len == 0) return 0;
    if (len > MAX_READ_SIZE) len = MAX_READ_SIZE;
    extern void rng_bytes(void* b, uint64_t n);
    uint8_t chunk[256];
    uint64_t done = 0;
    while (done < len) {
        uint64_t n = len - done;
        if (n > sizeof(chunk)) n = sizeof(chunk);
        rng_bytes(chunk, n);
        if (copy_to_user((void*)(buf + done), chunk, n) != COPY_SUCCESS) return EFAULT;
        done += n;
    }
    return (int64_t)done;
}

// SYS_PROCLIST - snapshot the process table into a user proc_info_t[] array
int64_t sys_proclist(uint64_t ubuf, uint64_t max, uint64_t arg3,
                     uint64_t arg4, uint64_t arg5, uint64_t arg6) {
    (void)arg3; (void)arg4; (void)arg5; (void)arg6;
    if (!ubuf) return EFAULT;
    if (max == 0) return 0;
    // Cooperative, non-preemptive kernel -> a file-static snapshot buffer is safe.
    // Sized to the full process table (MAX_PROCESSES, process.c) so a caller can
    // see EVERY live process, not just the first 64. The old 64-entry cap
    // silently truncated late/high-PID processes (e.g. a test's freshly-forked
    // children at high table slots vanished from the snapshot), which made
    // per-process CPU accounting via SYS_PROCLIST miss exactly the processes a
    // scheduler test needs. 256 * 64 B = 16 KiB of .bss -- acceptable.
    #define PROCLIST_SNAP_MAX 256
    static proc_info_t list[PROCLIST_SNAP_MAX];
    int cap = (int)(max > PROCLIST_SNAP_MAX ? PROCLIST_SNAP_MAX : max);
    int n = process_list(list, cap);
    if (n > 0) {
        if (copy_to_user((void*)ubuf, list, (uint64_t)n * sizeof(proc_info_t)) != COPY_SUCCESS) {
            return EFAULT;
        }
    }
    return (int64_t)n;
}

// SYS_PROC_QUERY - rich per-process detail (procapi)
int64_t sys_proc_query(uint64_t pid, uint64_t out, uint64_t arg3,
                       uint64_t arg4, uint64_t arg5, uint64_t arg6) {
    (void)arg3; (void)arg4; (void)arg5; (void)arg6;
    proc_detail_t d;
    int rc = procapi_query((uint32_t)pid, &d);
    if (rc != 0) return rc;
    if (copy_to_user((void*)out, &d, sizeof(d)) != COPY_SUCCESS) return EFAULT;
    return 0;
}

// SYS_PROC_CTL - suspend/resume/kill/setprio (procapi)
int64_t sys_proc_ctl(uint64_t pid, uint64_t verb, uint64_t arg,
                     uint64_t arg4, uint64_t arg5, uint64_t arg6) {
    (void)arg4; (void)arg5; (void)arg6;
    return procapi_ctl((uint32_t)pid, (uint32_t)verb, arg);
}

// SYS_SYSINFO - system memory/uptime/proc-count (procapi)
int64_t sys_sysinfo(uint64_t out, uint64_t arg2, uint64_t arg3,
                    uint64_t arg4, uint64_t arg5, uint64_t arg6) {
    (void)arg2; (void)arg3; (void)arg4; (void)arg5; (void)arg6;
    sysinfo_t s;
    int rc = procapi_sysinfo(&s);
    if (rc != 0) return rc;
    if (copy_to_user((void*)out, &s, sizeof(s)) != COPY_SUCCESS) return EFAULT;
    return 0;
}

// SYS_CLIP_SET / SYS_CLIP_GET - system clipboard (clipboard.c validates the
// user pointers via copy_from/to_user internally).
int64_t sys_clip_set(uint64_t user_buf, uint64_t len, uint64_t arg3,
                     uint64_t arg4, uint64_t arg5, uint64_t arg6) {
    (void)arg3; (void)arg4; (void)arg5; (void)arg6;
    return (int64_t)clipboard_set((const void*)user_buf, (uint32_t)len);
}
int64_t sys_clip_get(uint64_t user_buf, uint64_t max, uint64_t arg3,
                     uint64_t arg4, uint64_t arg5, uint64_t arg6) {
    (void)arg3; (void)arg4; (void)arg5; (void)arg6;
    return (int64_t)clipboard_get((void*)user_buf, (uint32_t)max);
}

// ============================================================================
// Block device system calls (SYS_BLK_READ=49 / SYS_BLK_WRITE=50)
// ============================================================================
// Userspace ABI for both: (lba: uint64_t, count: uint64_t sectors, ubuf: void*).
// 512-byte sectors. The kernel bounces through a kmalloc'd staging buffer and
// validates the user buffer via copy_to/from_user; user count/ptr are NEVER
// trusted. See the handler-body recipe documented at the bottom of ahci.c.

// Cap a single block transfer to keep the kmalloc bounded (256 * 512 = 128 KiB).
#define BLK_MAX_SECTORS 256

// SYS_BLK_READ - read `count` 512-byte sectors at `lba` into user buffer.
int64_t sys_blk_read(uint64_t lba, uint64_t count, uint64_t ubuf,
                     uint64_t arg4, uint64_t arg5, uint64_t arg6) {
    (void)arg4; (void)arg5; (void)arg6;

    if (!ahci_present()) {
        return ENODEV;
    }
    // Bound the sector count and reject zero-length requests.
    if (count == 0 || count > BLK_MAX_SECTORS) {
        return EINVAL;
    }
    // len = count * 512 cannot overflow here (count <= 256), but compute via
    // size_t and re-check defensively in case BLK_MAX_SECTORS ever grows.
    size_t len = (size_t)count * 512u;
    if (len / 512u != (size_t)count) {
        return EINVAL;  // multiplication overflow guard
    }

    void* kbuf = kmalloc(len);
    if (!kbuf) {
        return ENOMEM;
    }

    int r = ahci_read(lba, (uint32_t)count, kbuf);
    if (r != 0) {
        kfree(kbuf);
        return EIO;
    }

    if (copy_to_user((void*)ubuf, kbuf, len) != COPY_SUCCESS) {
        kfree(kbuf);
        return EFAULT;
    }

    kfree(kbuf);
    return 0;
}

// SYS_BLK_WRITE - write `count` 512-byte sectors at `lba` from user buffer.
int64_t sys_blk_write(uint64_t lba, uint64_t count, uint64_t ubuf,
                      uint64_t arg4, uint64_t arg5, uint64_t arg6) {
    (void)arg4; (void)arg5; (void)arg6;

    if (!ahci_present()) {
        return ENODEV;
    }
    if (count == 0 || count > BLK_MAX_SECTORS) {
        return EINVAL;
    }
    size_t len = (size_t)count * 512u;
    if (len / 512u != (size_t)count) {
        return EINVAL;  // multiplication overflow guard
    }

    void* kbuf = kmalloc(len);
    if (!kbuf) {
        return ENOMEM;
    }

    if (copy_from_user(kbuf, (const void*)ubuf, len) != COPY_SUCCESS) {
        kfree(kbuf);
        return EFAULT;
    }

    int r = ahci_write(lba, (uint32_t)count, kbuf);
    kfree(kbuf);
    if (r != 0) {
        return EIO;
    }
    return 0;
}

// ============================================================================
// Performance Monitoring System Call
// ============================================================================

// SYS_PERF_REPORT - Print performance monitoring statistics
int64_t sys_perf_report(uint64_t arg1, uint64_t arg2, uint64_t arg3,
                        uint64_t arg4, uint64_t arg5, uint64_t arg6) {
    (void)arg1; (void)arg2; (void)arg3; (void)arg4; (void)arg5; (void)arg6;

    // Print the performance report to kernel console
    perf_report();

    return 0;
}

// ============================================================================
// IPC System Call Wrappers
// ============================================================================
// Note: These forward to the implementations in kernel/ipc/shm.c and msgqueue.c

#ifdef SMP_FOUNDATION
// ============================================================================
// SMP COPROCESSOR OFFLOAD  (the userspace -> CPU1 bridge)
// ============================================================================
// SYS_CPU1_OFFLOAD: a NORMAL ring-3 process hands a kernel-owned compute job to
// CPU1 (the TRUSTED coprocessor) and gets the result back. This whole block is
// compiled ONLY for the SMP build (SMP_FOUNDATION defined); in the DEFAULT build
// it vanishes entirely and the syscall is never registered (syscall.c also guards
// the table entry), so the unregistered number returns ENOTSUP and the default
// kernel binary is byte-for-byte unchanged.
//
// CPU1 stays a pure trusted coprocessor: this handler does ALL user-pointer
// validation and copy-in/out with the kernel's safe copy_from_user/copy_to_user
// (never a raw user deref), copies the operands into trusted kernel buffers, and
// only THEN hands the trusted kernel matmul to CPU1 (cpu1_offload_matmul ->
// cpu1_submit/cpu1_wait in ap_boot.c). No user pointer, no user code, no
// scheduler, and no migration ever reaches the AP.

// The trusted-coprocessor matmul driver lives in ap_boot.c (SMP-only). MM_N is
// the max dimension of the fixed CPU1-reachable buffers (128).
extern int cpu1_offload_matmul(const int32_t *A, const int32_t *B, int n,
                               int64_t *C_out, int *by_apic_out);
#ifndef SMP_MM_N
#define SMP_MM_N 128   /* must match ap_boot.c MM_N */
#endif

// sys_cpu1_offload(job_type, user_arg, arg_len, user_res, res_len)
//
//   CPU1_JOB_MATMUL: user_arg -> { int32_t n; int32_t A[n*n]; int32_t B[n*n]; }
//     VALIDATE  n in (0, SMP_MM_N], arg_len == 4 + 2*n*n*4, res_len == n*n*8.
//     COPY A,B in (safe), dispatch the WHOLE matmul to CPU1, wait (~2s bound),
//     COPY the int64 result to user_res. Returns 0 on success, negative errno on
//     any bad pointer / size mismatch / out-of-range n / CPU1 timeout.
//
// Buffers are reference-counted (kmalloc_ref). CPU1 takes its own kref at submit
// (kget in cpu1_offload_matmul) and releases at completion (mm_offload_release),
// enabling safe cleanup on timeout and preparing for concurrent offloads.
int64_t sys_cpu1_offload(uint64_t job_type, uint64_t user_arg, uint64_t arg_len,
                         uint64_t user_res, uint64_t res_len) {
    if (job_type != CPU1_JOB_MATMUL) {
        return ENOTSUP;                 // only the matmul job is defined today
    }
    if (user_arg == 0 || user_res == 0) {
        return EFAULT;                  // null user pointers
    }

    // 1. Pull just the leading dimension n out of the user arg block (safe copy),
    //    then bound it BEFORE trusting it for any size arithmetic.
    int32_t n = 0;
    if (copy_from_user(&n, (const void*)user_arg, sizeof(n)) != COPY_SUCCESS) {
        return EFAULT;
    }
    if (n <= 0 || n > SMP_MM_N) {
        return EINVAL;                  // out of range (also guards size overflow)
    }

    // 2. Exact size contract. n <= 128, so these products fit a uint64 trivially.
    uint64_t elems     = (uint64_t)n * (uint64_t)n;
    uint64_t want_arg  = sizeof(int32_t) + 2ULL * elems * sizeof(int32_t);
    uint64_t want_res  = elems * sizeof(int64_t);
    if (arg_len != want_arg || res_len != want_res) {
        return EINVAL;                  // caller's buffer sizes must match exactly
    }

    // 3. Trusted kernel scratch for the operands + result. CPU1 only ever touches
    //    the kernel buffers inside cpu1_offload_matmul, never these or any user ptr.
    //    Use kmalloc_ref so each buffer is independently refcounted -- allows
    //    multiple concurrent offloads and safe cleanup on any error path.
    size_t ab_bytes = (size_t)(elems * sizeof(int32_t));
    size_t c_bytes  = (size_t)(elems * sizeof(int64_t));
    int32_t* kA = (int32_t*)kmalloc_ref(ab_bytes);
    int32_t* kB = (int32_t*)kmalloc_ref(ab_bytes);
    int64_t* kC = (int64_t*)kmalloc_ref(c_bytes);
    if (!kA || !kB || !kC) {
        if (kA) kput(kA);
        if (kB) kput(kB);
        if (kC) kput(kC);
        return ENOMEM;
    }

    // 4. Safe copy-in of A and B (A starts after the int32 n; B right after A).
    const void* uA = (const void*)(user_arg + sizeof(int32_t));
    const void* uB = (const void*)(user_arg + sizeof(int32_t) + ab_bytes);
    if (copy_from_user(kA, uA, ab_bytes) != COPY_SUCCESS ||
        copy_from_user(kB, uB, ab_bytes) != COPY_SUCCESS) {
        kput(kA); kput(kB); kput(kC);
        return EFAULT;
    }

    // 5. Dispatch the WHOLE matmul to CPU1 (trusted coprocessor) and wait the
    //    bounded deadline. by_apic must read 1 (the AP recorded its own apic id).
    //    CPU1 takes its own kref on each buffer at submit (via kget in
    //    cpu1_offload_matmul) and releases in mm_offload_release when done. Refcount
    //    semantics: CPU0 handler holds 1, CPU1 bumps to 2 at submit. On SUCCESS:
    //    CPU1 releases first (2->1), then CPU0 here (1->0, free). On TIMEOUT: CPU0's
    //    kput drives 2->1 (NOT freed), CPU1's later release drives 1->0 (real free,
    //    after CPU1 is provably done). No UAF, no leak, no double-free.
    int by_apic = -1;
    int ok = cpu1_offload_matmul(kA, kB, (int)n, kC, &by_apic);
    if (!ok) {
        kput(kA); kput(kB); kput(kC);
        return EAGAIN;                  // CPU1 wedged/absent/timed out -> not done
    }

    // 6. Copy the int64 result back to userspace (safe).
    if (copy_to_user((void*)user_res, kC, c_bytes) != COPY_SUCCESS) {
        kput(kA); kput(kB); kput(kC);
        return EFAULT;
    }

    // 7. Drop our references. On success CPU1 has already released (in
    //    mm_offload_release at job completion), so this drives count 1->0 and frees.
    kput(kA); kput(kB); kput(kC);

    // Return the apic id that ran the job (>= 0) so the caller can prove CPU1 did
    // the work. 0 on success is the n=... etc. path; here a positive apic id is the
    // success signal AND the proof. by_apic is 1 for CPU1.
    return (int64_t)by_apic;
}
#endif /* SMP_FOUNDATION */
