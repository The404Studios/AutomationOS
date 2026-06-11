#include "../../include/x86_64.h"
#include "../../include/kernel.h"
#include "../../include/sched.h"
#include "../../include/vma.h"
#ifdef SMP_SCHED
#include "../../include/lapic_constants.h"  /* AP_LAPIC_TIMER_VECTOR (Brick E) */
#endif

#define IDT_ENTRIES 256

// IDT gate types
#define IDT_GATE_INTERRUPT 0x8E  // Present, DPL=0, Type=Interrupt Gate
#define IDT_GATE_TRAP      0x8F  // Present, DPL=0, Type=Trap Gate

// IDT entry structure
typedef struct {
    uint16_t offset_low;    // Offset bits 0-15
    uint16_t selector;      // Code segment selector
    uint8_t ist;            // Interrupt stack table offset
    uint8_t type_attr;      // Type and attributes
    uint16_t offset_mid;    // Offset bits 16-31
    uint32_t offset_high;   // Offset bits 32-63
    uint32_t zero;          // Reserved
} PACKED idt_entry_t;

// IDT pointer structure
typedef struct {
    uint16_t limit;
    uint64_t base;
} PACKED idt_ptr_t;

// IDT table and pointer
static idt_entry_t idt[IDT_ENTRIES];
static idt_ptr_t idt_ptr;

// External interrupt stubs from interrupt.asm
extern void isr0(void);
extern void isr1(void);
extern void isr2(void);
extern void isr3(void);
extern void isr4(void);
extern void isr5(void);
extern void isr6(void);
extern void isr7(void);
extern void isr8(void);
extern void isr9(void);
extern void isr10(void);
extern void isr11(void);
extern void isr12(void);
extern void isr13(void);
extern void isr14(void);
extern void isr15(void);
extern void isr16(void);
extern void isr17(void);
extern void isr18(void);
extern void isr19(void);
extern void isr20(void);
extern void isr21(void);
extern void isr22(void);
extern void isr23(void);
extern void isr24(void);
extern void isr25(void);
extern void isr26(void);
extern void isr27(void);
extern void isr28(void);
extern void isr29(void);
extern void isr30(void);
extern void isr31(void);

// IRQ stubs (IRQ 0-15 mapped to ISR 32-47)
extern void irq0(void);
extern void irq1(void);
extern void irq2(void);
extern void irq3(void);
extern void irq4(void);
extern void irq5(void);
extern void irq6(void);
extern void irq7(void);
extern void irq8(void);
extern void irq9(void);
extern void irq10(void);
extern void irq11(void);
extern void irq12(void);
extern void irq13(void);
extern void irq14(void);
extern void irq15(void);

// Preemptive timer (IRQ0) entry stub. Defined in interrupt.asm ONLY under
// -DPREEMPTIVE (gated %ifdef block). When the flag is off this symbol does not
// exist and IDT[32] keeps pointing at the cooperative irq0 stub below.
#ifdef PREEMPTIVE
extern void irq0_preempt(void);
#endif

#ifdef SMP_SCHED
// SMP scheduler Brick E: CPU1's LAPIC timer ISR (vector 0x40) and the LAPIC
// spurious ISR (vector 0xFF). Shared IDT gates; only CPU1 (with an armed LAPIC
// timer + sti) actually triggers them. See interrupt.asm.
extern void ap_lapic_timer_isr(void);
extern void ap_spurious_isr(void);
#endif

// External assembly function to load IDT
extern void idt_flush(uint64_t idt_ptr);

// Set an IDT gate
void idt_set_gate(uint8_t num, uint64_t handler, uint16_t selector, uint8_t flags) {
    idt[num].offset_low = handler & 0xFFFF;
    idt[num].offset_mid = (handler >> 16) & 0xFFFF;
    idt[num].offset_high = (handler >> 32) & 0xFFFFFFFF;

    idt[num].selector = selector;  // Kernel code segment (0x08)
    idt[num].ist = 0;              // No IST
    idt[num].type_attr = flags;
    idt[num].zero = 0;
}

#ifdef SMP_IPI
// Is an IDT gate already claimed? (non-zero handler offset). SMP-G0: ipi_init
// verifies each IPI vector is FREE before claiming it -- a silent idt_set_gate
// over a live vector (the original IPI block sat dead-on the CPU1 LAPIC timer
// gate at 0x40) kills the prior owner with no diagnostic. Gated so every
// non-SMP_IPI build (default AND the frozen F3-5 SMP config) stays
// byte-for-byte unchanged.
int idt_gate_present(uint8_t num) {
    return (idt[num].offset_low | idt[num].offset_mid | idt[num].offset_high) != 0;
}
#endif

// Route the fault vectors that must NOT run on a possibly-corrupt kernel stack
// onto IST1: #DF (8) and NMI (2). Without this a kernel-stack overflow that
// triggers #DF would re-fault pushing the #DF frame onto the same broken stack
// and triple-fault (silent reset). Called from tss_init() AFTER tss.ist1 points
// at a valid stack, so a gate never references IST1 before the stack exists.
void idt_enable_ist_stacks(void) {
    idt[8].ist = 1;   // #DF -> IST1
    idt[2].ist = 1;   // NMI -> IST1
}

// PIC commands and ports
#define PIC1_COMMAND 0x20
#define PIC1_DATA    0x21
#define PIC2_COMMAND 0xA0
#define PIC2_DATA    0xA1

#define ICW1_INIT    0x10
#define ICW1_ICW4    0x01
#define ICW4_8086    0x01

// Remap PIC to IRQ 32-47 (to avoid conflict with CPU exceptions 0-31)
static void pic_remap(void) {
    // Save masks (but we'll mask everything initially for safety)
    uint8_t mask1 = inb(PIC1_DATA);
    uint8_t mask2 = inb(PIC2_DATA);

    // Start initialization sequence
    outb(PIC1_COMMAND, ICW1_INIT | ICW1_ICW4);
    outb(PIC2_COMMAND, ICW1_INIT | ICW1_ICW4);

    // Set vector offsets
    outb(PIC1_DATA, 32);  // IRQ 0-7 mapped to 32-39
    outb(PIC2_DATA, 40);  // IRQ 8-15 mapped to 40-47

    // Configure cascade
    outb(PIC1_DATA, 0x04);  // Tell master PIC there's a slave at IRQ2
    outb(PIC2_DATA, 0x02);  // Tell slave PIC its cascade identity

    // Set mode
    outb(PIC1_DATA, ICW4_8086);
    outb(PIC2_DATA, ICW4_8086);

    // SAFETY: Mask ALL IRQs initially (0xFF = all masked)
    // IRQs will be unmasked individually when handlers are registered via irq_register_handler()
    // This prevents spurious interrupts before handlers are ready
    outb(PIC1_DATA, 0xFF);
    outb(PIC2_DATA, 0xFF);
}

// Initialize IDT
void idt_init(void) {
    kprintf("[IDT] Initializing Interrupt Descriptor Table...\n");

    // Zero out IDT
    for (int i = 0; i < IDT_ENTRIES; i++) {
        idt[i].offset_low = 0;
        idt[i].offset_mid = 0;
        idt[i].offset_high = 0;
        idt[i].selector = 0;
        idt[i].ist = 0;
        idt[i].type_attr = 0;
        idt[i].zero = 0;
    }

    // Setup IDT pointer
    idt_ptr.limit = (sizeof(idt_entry_t) * IDT_ENTRIES) - 1;
    idt_ptr.base = (uint64_t)&idt;

    // Remap PIC
    pic_remap();
    kprintf("[IDT] PIC remapped (IRQ 0-15 -> INT 32-47)\n");

    // Install exception handlers (ISR 0-31)
    idt_set_gate(0, (uint64_t)isr0, 0x08, IDT_GATE_INTERRUPT);
    idt_set_gate(1, (uint64_t)isr1, 0x08, IDT_GATE_INTERRUPT);
    idt_set_gate(2, (uint64_t)isr2, 0x08, IDT_GATE_INTERRUPT);
    idt_set_gate(3, (uint64_t)isr3, 0x08, IDT_GATE_INTERRUPT);
    idt_set_gate(4, (uint64_t)isr4, 0x08, IDT_GATE_INTERRUPT);
    idt_set_gate(5, (uint64_t)isr5, 0x08, IDT_GATE_INTERRUPT);
    idt_set_gate(6, (uint64_t)isr6, 0x08, IDT_GATE_INTERRUPT);
    idt_set_gate(7, (uint64_t)isr7, 0x08, IDT_GATE_INTERRUPT);
    idt_set_gate(8, (uint64_t)isr8, 0x08, IDT_GATE_INTERRUPT);
    idt_set_gate(9, (uint64_t)isr9, 0x08, IDT_GATE_INTERRUPT);
    idt_set_gate(10, (uint64_t)isr10, 0x08, IDT_GATE_INTERRUPT);
    idt_set_gate(11, (uint64_t)isr11, 0x08, IDT_GATE_INTERRUPT);
    idt_set_gate(12, (uint64_t)isr12, 0x08, IDT_GATE_INTERRUPT);
    idt_set_gate(13, (uint64_t)isr13, 0x08, IDT_GATE_INTERRUPT);
    idt_set_gate(14, (uint64_t)isr14, 0x08, IDT_GATE_INTERRUPT);
    idt_set_gate(15, (uint64_t)isr15, 0x08, IDT_GATE_INTERRUPT);
    idt_set_gate(16, (uint64_t)isr16, 0x08, IDT_GATE_INTERRUPT);
    idt_set_gate(17, (uint64_t)isr17, 0x08, IDT_GATE_INTERRUPT);
    idt_set_gate(18, (uint64_t)isr18, 0x08, IDT_GATE_INTERRUPT);
    idt_set_gate(19, (uint64_t)isr19, 0x08, IDT_GATE_INTERRUPT);
    idt_set_gate(20, (uint64_t)isr20, 0x08, IDT_GATE_INTERRUPT);
    idt_set_gate(21, (uint64_t)isr21, 0x08, IDT_GATE_INTERRUPT);
    idt_set_gate(22, (uint64_t)isr22, 0x08, IDT_GATE_INTERRUPT);
    idt_set_gate(23, (uint64_t)isr23, 0x08, IDT_GATE_INTERRUPT);
    idt_set_gate(24, (uint64_t)isr24, 0x08, IDT_GATE_INTERRUPT);
    idt_set_gate(25, (uint64_t)isr25, 0x08, IDT_GATE_INTERRUPT);
    idt_set_gate(26, (uint64_t)isr26, 0x08, IDT_GATE_INTERRUPT);
    idt_set_gate(27, (uint64_t)isr27, 0x08, IDT_GATE_INTERRUPT);
    idt_set_gate(28, (uint64_t)isr28, 0x08, IDT_GATE_INTERRUPT);
    idt_set_gate(29, (uint64_t)isr29, 0x08, IDT_GATE_INTERRUPT);
    idt_set_gate(30, (uint64_t)isr30, 0x08, IDT_GATE_INTERRUPT);
    idt_set_gate(31, (uint64_t)isr31, 0x08, IDT_GATE_INTERRUPT);

    // Install IRQ handlers (INT 32-47)
    // IRQ0 (timer, vector 32): under -DPREEMPTIVE the timer drives the
    // preemptive scheduler via irq0_preempt (which builds an interrupt_frame_t
    // and calls schedule_from_irq). With the flag OFF this is the cooperative
    // irq0 stub -- byte-for-byte unchanged default behavior.
#ifdef PREEMPTIVE
    idt_set_gate(32, (uint64_t)irq0_preempt, 0x08, IDT_GATE_INTERRUPT);
#else
    idt_set_gate(32, (uint64_t)irq0, 0x08, IDT_GATE_INTERRUPT);
#endif
    idt_set_gate(33, (uint64_t)irq1, 0x08, IDT_GATE_INTERRUPT);
    idt_set_gate(34, (uint64_t)irq2, 0x08, IDT_GATE_INTERRUPT);
    idt_set_gate(35, (uint64_t)irq3, 0x08, IDT_GATE_INTERRUPT);
    idt_set_gate(36, (uint64_t)irq4, 0x08, IDT_GATE_INTERRUPT);
    idt_set_gate(37, (uint64_t)irq5, 0x08, IDT_GATE_INTERRUPT);
    idt_set_gate(38, (uint64_t)irq6, 0x08, IDT_GATE_INTERRUPT);
    idt_set_gate(39, (uint64_t)irq7, 0x08, IDT_GATE_INTERRUPT);
    idt_set_gate(40, (uint64_t)irq8, 0x08, IDT_GATE_INTERRUPT);
    idt_set_gate(41, (uint64_t)irq9, 0x08, IDT_GATE_INTERRUPT);
    idt_set_gate(42, (uint64_t)irq10, 0x08, IDT_GATE_INTERRUPT);
    idt_set_gate(43, (uint64_t)irq11, 0x08, IDT_GATE_INTERRUPT);
    idt_set_gate(44, (uint64_t)irq12, 0x08, IDT_GATE_INTERRUPT);
    idt_set_gate(45, (uint64_t)irq13, 0x08, IDT_GATE_INTERRUPT);
    idt_set_gate(46, (uint64_t)irq14, 0x08, IDT_GATE_INTERRUPT);
    idt_set_gate(47, (uint64_t)irq15, 0x08, IDT_GATE_INTERRUPT);

#ifdef SMP_SCHED
    // Brick E: CPU1 LAPIC timer (0x40) + LAPIC spurious (0xFF). Both are interrupt
    // gates on the kernel code selector. The BSP never fires these (its LAPIC timer
    // stays masked; it uses PIC irq0). Installed here so they exist in the shared
    // IDT before CPU1 arms its timer and does sti.
    idt_set_gate(AP_LAPIC_TIMER_VECTOR, (uint64_t)ap_lapic_timer_isr, 0x08, IDT_GATE_INTERRUPT);
    idt_set_gate(0xFF, (uint64_t)ap_spurious_isr, 0x08, IDT_GATE_INTERRUPT);
#endif

    // Load IDT
    idt_flush((uint64_t)&idt_ptr);

    kprintf("[IDT] Interrupt Descriptor Table initialized (%d entries)\n", IDT_ENTRIES);
}

// Enhanced exception name and description lookup
static const char* get_exception_name(uint64_t int_no) {
    static const char* exception_messages[] = {
        "Division By Zero",
        "Debug",
        "Non-Maskable Interrupt",
        "Breakpoint",
        "Overflow",
        "Bound Range Exceeded",
        "Invalid Opcode",
        "Device Not Available",
        "Double Fault",
        "Coprocessor Segment Overrun",
        "Invalid TSS",
        "Segment Not Present",
        "Stack-Segment Fault",
        "General Protection Fault",
        "Page Fault",
        "Reserved",
        "x87 Floating-Point Exception",
        "Alignment Check",
        "Machine Check",
        "SIMD Floating-Point Exception",
        "Virtualization Exception",
        "Reserved",
        "Reserved",
        "Reserved",
        "Reserved",
        "Reserved",
        "Reserved",
        "Reserved",
        "Reserved",
        "Reserved",
        "Security Exception",
        "Reserved"
    };

    return (int_no < 32) ? exception_messages[int_no] : "Unknown Exception";
}

// Decode page fault error code
static void print_page_fault_details(uint64_t err_code, uint64_t cr2) {
    kprintf("  Page Fault Details:\n");
    kprintf("    Faulting address (CR2): 0x%016llx\n", cr2);
    kprintf("    Error code: 0x%llx [ ", err_code);

    if (err_code & (1 << 0)) {
        kprintf("PROTECTION-VIOLATION ");
    } else {
        kprintf("NOT-PRESENT ");
    }

    if (err_code & (1 << 1)) {
        kprintf("WRITE ");
    } else {
        kprintf("READ ");
    }

    if (err_code & (1 << 2)) {
        kprintf("USER-MODE ");
    } else {
        kprintf("KERNEL-MODE ");
    }

    if (err_code & (1 << 3)) {
        kprintf("RESERVED-BIT ");
    }

    if (err_code & (1 << 4)) {
        kprintf("INSTRUCTION-FETCH ");
    }

    if (err_code & (1 << 5)) {
        kprintf("PROTECTION-KEY ");
    }

    kprintf("]\n");

    // Provide hints based on error code
    kprintf("    Probable cause: ");
    if (!(err_code & (1 << 0))) {
        kprintf("Page not mapped\n");
    } else if (err_code & (1 << 2)) {
        kprintf("User trying to access kernel page\n");
    } else if (err_code & (1 << 1)) {
        kprintf("Write to read-only page\n");
    } else if (err_code & (1 << 4)) {
        kprintf("Instruction fetch from NX page\n");
    } else {
        kprintf("Permission violation\n");
    }

}

// Decode GPF error code
static void print_gpf_details(uint64_t err_code) {
    kprintf("  General Protection Fault Details:\n");
    kprintf("    Error code: 0x%llx\n", err_code);

    if (err_code != 0) {
        bool external = (err_code & 0x1) != 0;
        uint8_t tbl = (err_code >> 1) & 0x3;
        uint16_t index = (err_code >> 3) & 0x1FFF;

        kprintf("    Selector: 0x%x (index=%d, table=%s, %s)\n",
                (uint32_t)err_code,
                index,
                (tbl == 0) ? "GDT" : (tbl == 1 || tbl == 3) ? "IDT" : "LDT",
                external ? "external" : "internal");
    } else {
        kprintf("    No specific segment information (error code 0)\n");
        kprintf("    Probable cause: Invalid instruction, privilege violation, or limit check\n");
    }
}

// Exception handler
#ifdef SLAB_WATCH
// Debug-branch slab-corruption watchpoint: arm one of DR0-3 as an 8-byte WRITE
// breakpoint on `va` (a live slab's magic word, identity alias). The #DB handler
// below logs the exact RIP that stray-writes a zero over it. The pre-existing
// corruptor writes via the IDENTITY map (it predates the direct map), so identity
// VAs suffice -- freeing all 4 DRs to watch 4 distinct slab pages.
void slab_arm_watch(int dr, uint64_t va) {
    static volatile uint64_t dr7_accum = 0;
    switch (dr & 3) {
        case 0: __asm__ volatile("mov %0, %%dr0" :: "r"(va) : "memory"); break;
        case 1: __asm__ volatile("mov %0, %%dr1" :: "r"(va) : "memory"); break;
        case 2: __asm__ volatile("mov %0, %%dr2" :: "r"(va) : "memory"); break;
        case 3: __asm__ volatile("mov %0, %%dr3" :: "r"(va) : "memory"); break;
    }
    // Ln enable (bit 2n); R/Wn=01 write (bit 16+4n); LENn=10 8-byte (bit 18+4n).
    dr7_accum |= (1ULL << (2 * (dr & 3)))
               | (1ULL << (16 + 4 * (dr & 3)))
               | (2ULL << (18 + 4 * (dr & 3)));
    __asm__ volatile("mov %0, %%dr7" :: "r"(dr7_accum) : "memory");
}
#endif

void exception_handler(uint64_t int_no, uint64_t err_code, uint64_t rip, uint64_t cs) {
    // Check if exception came from user mode (CS & 3 == 3)
    bool user_mode = (cs & 0x3) == 0x3;

#ifdef SLAB_WATCH
    // SLAB-WATCH: a hardware data breakpoint (DR0 identity / DR1 direct-map) fired ->
    // log the writer RIP + CR3 + the value now at the watched magic, clear DR6, and
    // RESUME (a watchpoint trap is NOT fatal). newval==0 == the corruptor caught.
    if (int_no == 1) {
        uint64_t dr6, dr0, dr1, dr2, dr3, cr3;
        __asm__ volatile("mov %%dr6,%0; mov %%dr0,%1; mov %%dr1,%2; mov %%dr2,%3; mov %%dr3,%4; mov %%cr3,%5"
                         : "=r"(dr6), "=r"(dr0), "=r"(dr1), "=r"(dr2), "=r"(dr3), "=r"(cr3));
        uint64_t hit = (dr6 & 1) ? dr0 : (dr6 & 2) ? dr1 : (dr6 & 4) ? dr2 : (dr6 & 8) ? dr3 : 0;
        if (hit) {
            uint64_t newval = *(volatile uint64_t*)hit;
            kprintf("[SLABWATCH] WRITE slab-magic @0x%lx rip=0x%lx cr3=0x%lx newval=0x%lx\n",
                    (unsigned long)hit, (unsigned long)rip, (unsigned long)cr3,
                    (unsigned long)newval);
            // Once the magic is no longer SLAB_MAGIC, the corruptor was just caught --
            // DISARM (clear DR7) so we log it exactly once, not on every later reuse.
            if (newval != 0x51AB0BACE51AB0BULL) {
                __asm__ volatile("mov %0, %%dr7" :: "r"(0UL) : "memory");
            }
        }
        __asm__ volatile("mov %0, %%dr6" :: "r"(0UL));   // clear DR6 status
        return;  // resume the interrupted instruction stream
    }
#endif

    // Recoverable page fault: try to fault the page in from the VMA layer BEFORE
    // printing the diagnostic banner or killing. Under the default EAGER load
    // policy every page is pre-mapped so this resolves nothing for normal apps;
    // it is the hook that makes lazy/CoW paging work. Purely additive -- on any
    // unresolved fault we fall through to the existing kill path unchanged.
    if (int_no == 14 && user_mode) {
        uint64_t cr2;
        __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
        if (handle_page_fault(cr2, err_code)) {
            return;  // resolved: iretq retries the faulting instruction
        }
    }

    // Print detailed exception banner
    kprintf("\n");
    kprintf("================================================================================\n");
    kprintf("                             CPU EXCEPTION                                      \n");
    kprintf("================================================================================\n");
    kprintf("Exception: %s (vector %llu)\n", get_exception_name(int_no), int_no);
    kprintf("Privilege level: %s (CPL=%d)\n", user_mode ? "User" : "Kernel", (int)(cs & 0x3));
    kprintf("\n");

    // Instruction pointer info
    kprintf("Faulting instruction:\n");
    kprintf("  RIP: 0x%016llx\n", rip);
    kprintf("  CS:  0x%04x\n", (uint16_t)cs);
    kprintf("\n");

    // Exception-specific details
    if (int_no == 14) {  // Page Fault
        uint64_t cr2;
        __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
        print_page_fault_details(err_code, cr2);
    } else if (int_no == 13) {  // General Protection Fault
        print_gpf_details(err_code);
    } else if (int_no == 8) {  // Double Fault
        kprintf("  DOUBLE FAULT - Catastrophic error during exception handling!\n");
        kprintf("  Error code: 0x%llx (always zero on #DF)\n", err_code);
    } else {
        kprintf("  Error code: 0x%llx\n", err_code);
    }
    kprintf("\n");

    kprintf("================================================================================\n");

    if (user_mode) {
        process_t* current = process_get_current();
        if (current) {
            // VICTIM SELECTION: a user fault happened in the LIVE address space,
            // so the faulting process is whoever owns the page tables in CR3 --
            // NOT necessarily the global `current`, which can name a different
            // process after a context swap (or under SMP, where current is
            // per-CPU). Confirm `current` is the faulting process by matching the
            // live CR3 against its saved context.cr3 before killing it. On a
            // mismatch, killing `current` would take down an unrelated process
            // (e.g. the compositor / whole desktop), so refuse and panic with the
            // details instead of killing the wrong victim.
            uint64_t live_cr3 = read_cr3();
            if (live_cr3 != current->context.cr3) {
                kprintf("\n[CRITICAL] CR3 mismatch detected!\n");
                kprintf("  Live CR3:    0x%016llx\n", live_cr3);
                kprintf("  Process:     '%s' (PID %d)\n", current->name, current->pid);
                kprintf("  Process CR3: 0x%016llx\n", current->context.cr3);
                kernel_panic("CR3 mismatch: refusing to kill wrong process");
            }
            kprintf("\n[EXCEPTION] Terminating faulting process '%s' (PID %d)\n",
                    current->name, current->pid);
            current->state = PROCESS_TERMINATED;
            current->exit_status = 139;          // 128 + SIGSEGV: died on a fault
            process_on_terminate(current);       // wake a parent blocked in waitpid()
            scheduler_remove_process(current);
            schedule();
            // schedule() must NOT return here: the current process is
            // TERMINATED, so context_switch() hands the CPU to another process
            // and never resumes this (dead) kernel stack frame. If it ever does
            // return, falling through to the ISR stub's iretq would resume the
            // faulting instruction in the dead process's (possibly freed)
            // address space. Panic loudly instead of silently corrupting state.
            kernel_panic("schedule() returned after process termination");
        } else {
            kernel_panic("User-mode exception with no current process");
        }
    } else {
        // Kernel mode exception - always fatal
        char panic_msg[256];
        kprintf("\n[FATAL] Exception in kernel mode - system cannot continue\n");

        // Format detailed panic message
        if (int_no == 14) {
            uint64_t cr2;
            __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
            kprintf("Kernel Page Fault at RIP=0x%016llx, CR2=0x%016llx, err=0x%llx\n",
                    rip, cr2, err_code);
        } else if (int_no == 13) {
            kprintf("Kernel General Protection Fault at RIP=0x%016llx, err=0x%llx\n",
                    rip, err_code);
        } else {
            kprintf("Kernel Exception #%llu (%s) at RIP=0x%016llx\n",
                    int_no, get_exception_name(int_no), rip);
        }

        kernel_panic("Unhandled kernel exception - see details above");
    }
}

// IRQ handler registry (simple legacy handlers for PIC IRQs)
typedef void (*simple_irq_handler_t)(void);
static simple_irq_handler_t irq_handlers[16] = {0};

// Unmask IRQ (enable specific IRQ line in PIC)
static void irq_unmask(uint8_t irq) {
    uint16_t port;
    uint8_t value;

    if (irq < 8) {
        port = PIC1_DATA;
    } else {
        port = PIC2_DATA;
        irq -= 8;
        // CRITICAL: slave-PIC IRQs (8-15) are routed to the CPU through the
        // master's IRQ2 cascade line. If IRQ2 stays masked, NO slave IRQ ever
        // fires — which silently killed the PS/2 mouse (IRQ12) while the
        // keyboard (IRQ1, master) worked. Unmask the cascade on the master.
        uint8_t m = inb(PIC1_DATA) & ~(1 << 2);
        outb(PIC1_DATA, m);
    }

    value = inb(port) & ~(1 << irq);
    outb(port, value);
}

// Register IRQ handler
void irq_register_handler(uint8_t irq, simple_irq_handler_t handler) {
    if (irq >= 16) return;
    irq_handlers[irq] = handler;
    irq_unmask(irq);  // Unmask the IRQ line
}

// IRQ handler
void irq_handler(uint64_t int_no) {
    uint8_t irq = int_no - 32;

    // Call registered handler if present
    if (irq < 16 && irq_handlers[irq]) {
        irq_handlers[irq]();
    } else {
        // No handler registered - spurious interrupt
        // Still need to send EOI
    }

    // Send EOI (End of Interrupt) to PIC
    if (irq >= 8) {
        outb(PIC2_COMMAND, 0x20);  // Send EOI to slave PIC
    }
    outb(PIC1_COMMAND, 0x20);  // Send EOI to master PIC
}
