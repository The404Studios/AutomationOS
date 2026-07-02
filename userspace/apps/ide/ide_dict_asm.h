/*
 * ide_dict_asm.h -- x86-64 assembly dictionary for the Semantic-LEGO IDE
 *                   autocomplete engine (ide_complete.c).
 * =========================================================================
 *
 * Registers, ~90 mnemonics, and the working idioms (prologue/epilogue, syscall,
 * spin loop, string ops). Each entry teaches the OPERAND SHAPE plus the one-line
 * semantics, rendered in the autocomplete preview pane. ASCII only.
 *
 * Offered when the editor is on a .asm / .s file (gate try_add_dict on language;
 * see the INTEGRATION CONTRACT banner in ide_dict_c.h -- same merge site).
 *
 * Syntax: INTEL (dest, src) -- the form the IDE's native assembler and the
 * cc backend both emit (cc_codegen.c). NASM/GAS users get the same mnemonics.
 *
 * ====================== ON-DEVICE as_x64 ASSEMBLER SUBSET ===================
 * The IDE can assemble a .asm file on-device, but as_x64 accepts only a SUBSET
 * (the exact set cc_codegen.c emits; see tc.h "SUPPORTED ASSEMBLY SUBSET").
 * Entries OUTSIDE that subset are marked "[host as/nasm only]" -- they are still
 * correct x86-64, just not handled by the on-device assembler.
 *   Directives: `section .text` | `section .data` | `global NAME` | labels
 *               `name:` | data `db <bytes/"str">` | `dq <imm/label>`.
 *   Registers : rax rbx rcx rdx rsi rdi rbp rsp r8..r15, and `al` (low byte).
 *   Operands  : reg | imm (decimal/0x) | label | [reg] | [reg+disp] | [reg-disp]
 *               with optional `byte`/`qword` size override on memory.
 *   Mnemonics : mov movzx lea push pop add sub imul cqo idiv and or xor
 *               shl shr neg not cmp test  sete setne setl setle setg setge
 *               jmp je jne jl jle jg jge  call ret leave  syscall.
 * Anything else (sar, mul, div, inc, dec, movsx, movabs, xchg, rol/ror, the
 * string/rep ops, int/iretq/cli/sti/hlt, rdtsc/cpuid/in/out, bt/bsf/popcnt,
 * the j(b/a/s/c/o) unsigned/flag jumps, SSE) is HOST-ONLY here.
 *
 * Integration: shares IdeDictEntry (defined in ide_dict_c.h) and exposes
 * IDE_DICT_ASM[] + IDE_DICT_ASM_COUNT. See ide_dict_c.h for the full glue.
 */
#ifndef IDE_DICT_ASM_H
#define IDE_DICT_ASM_H

#ifndef IDE_DICT_ENTRY_DEFINED
#define IDE_DICT_ENTRY_DEFINED
typedef struct {
    const char* word;
    const char* sig;
    const char* doc;
    const char* snippet;
    char        kind;
} IdeDictEntry;
#endif

static const IdeDictEntry IDE_DICT_ASM[] = {
    /* ===================================================================== *
     *  REGISTERS  (kind 'r')  -- roles per the SysV AMD64 + syscall ABIs
     * ===================================================================== */
    { "rax", "rax (eax/ax/al)", "Accumulator. Return value of calls; SYSCALL NUMBER on entry and the syscall result on return.", 0, 'r' },
    { "rbx", "rbx (ebx/bx/bl)", "General purpose. CALLEE-SAVED: a function that uses it must push/pop it.", 0, 'r' },
    { "rcx", "rcx (ecx/cx/cl)", "4th CALL arg (SysV). WARNING: the `syscall` instruction CLOBBERS rcx (it stores the return RIP there).", 0, 'r' },
    { "rdx", "rdx (edx/dx/dl)", "3rd arg (call AND syscall). High half of the idiv/imul/cqo 128-bit product/dividend.", 0, 'r' },
    { "rsi", "rsi (esi/si/sil)", "2nd arg (call AND syscall). Source pointer for string ops.", 0, 'r' },
    { "rdi", "rdi (edi/di/dil)", "1st arg (call AND syscall). Destination pointer for string ops.", 0, 'r' },
    { "rbp", "rbp (ebp)", "Frame/base pointer. CALLEE-SAVED. Locals are addressed as [rbp-off] after the prologue.", 0, 'r' },
    { "rsp", "rsp (esp)", "Stack pointer (grows DOWN). Must be 16-byte aligned at every `call`. push/pop move it by 8.", 0, 'r' },
    { "r8",  "r8 (r8d/r8w/r8b)", "5th call arg. General purpose, CALLER-saved.", 0, 'r' },
    { "r9",  "r9 (r9d/r9w/r9b)", "6th call arg. General purpose, CALLER-saved.", 0, 'r' },
    { "r10", "r10 (r10d..)", "4th SYSCALL arg (the kernel reads arg4 from r10, NOT rcx, because syscall clobbers rcx).", 0, 'r' },
    { "r11", "r11 (r11d..)", "Scratch. WARNING: the `syscall` instruction CLOBBERS r11 (it stores RFLAGS there).", 0, 'r' },
    { "r12", "r12", "General purpose. CALLEE-SAVED.", 0, 'r' },
    { "r13", "r13", "General purpose. CALLEE-SAVED.", 0, 'r' },
    { "r14", "r14", "General purpose. CALLEE-SAVED.", 0, 'r' },
    { "r15", "r15", "General purpose. CALLEE-SAVED.", 0, 'r' },
    { "al",  "al (low 8 of rax)", "Low byte of rax. setcc writes here; movzx rax, al zero-extends it back up.", 0, 'r' },
    { "eax", "eax (low 32 of rax)", "Low 32 bits of rax. Writing eax ZEROES the upper 32 bits of rax. [as_x64 emits full rax].", 0, 'r' },
    { "rip", "rip (instr pointer)", "Address of the next instruction. Not directly writable; changed by jmp/call/ret. RIP-relative [rip+sym].", 0, 'r' },
    { "rflags", "rflags (ZF CF SF OF)", "Status flags set by cmp/test/arith; read by jcc/setcc. ZF=zero, CF=carry, SF=sign, OF=overflow.", 0, 'r' },

    /* ===================================================================== *
     *  MNEMONICS  (kind 'i')  -- on-device-accepted first, then host-only
     * ===================================================================== */
    /* ---- data movement (on-device OK) ---- */
    { "mov", "mov dst, src", "Copy src into dst. Forms: reg,reg | reg,imm | reg,[mem] | [mem],reg | reg,label.", 0, 'i' },
    { "lea", "lea reg, [base+disp]", "Load Effective Address: put the COMPUTED address into reg (no memory read). Great for ptr math.", 0, 'i' },
    { "movzx", "movzx reg, src8", "Move + Zero-eXtend a smaller value into a 64-bit reg (e.g. movzx rax, al).", 0, 'i' },
    { "push", "push reg", "Decrement rsp by 8, store reg at [rsp]. Save a value / pass a temp.", 0, 'i' },
    { "pop", "pop reg", "Load [rsp] into reg, increment rsp by 8. Restore a saved value.", 0, 'i' },
    /* ---- arithmetic (on-device OK) ---- */
    { "add", "add dst, src", "dst = dst + src. Sets ZF/CF/SF/OF.", 0, 'i' },
    { "sub", "sub dst, src", "dst = dst - src. Sets flags (used by cmp under the hood).", 0, 'i' },
    { "imul", "imul dst, src", "Signed multiply: dst = dst * src (two-operand form). Sets OF/CF on overflow.", 0, 'i' },
    { "cqo", "cqo", "Sign-extend rax into rdx:rax (128-bit) so idiv divides correctly. Emit BEFORE idiv.", 0, 'i' },
    { "idiv", "idiv reg", "Signed divide rdx:rax by reg -> quotient in rax, remainder in rdx. Pair with cqo.", 0, 'i' },
    { "neg", "neg reg", "Two's-complement negate: reg = -reg.", 0, 'i' },
    /* ---- logic / shifts (on-device OK) ---- */
    { "and", "and dst, src", "Bitwise AND: dst = dst & src. Mask bits / test-and-set patterns.", 0, 'i' },
    { "or", "or dst, src", "Bitwise OR: dst = dst | src. Set selected bits.", 0, 'i' },
    { "xor", "xor dst, src", "Bitwise XOR. `xor reg, reg` is the idiomatic 1-byte way to zero a register.", 0, 'i' },
    { "not", "not reg", "Bitwise NOT (one's complement): flip every bit.", 0, 'i' },
    { "shl", "shl reg, imm/cl", "Shift Left logical by imm or by cl. Each step multiplies by 2.", 0, 'i' },
    { "shr", "shr reg, imm/cl", "Shift Right logical (zero-fill). Each step divides an UNSIGNED value by 2.", 0, 'i' },
    /* ---- compare / test (on-device OK) ---- */
    { "cmp", "cmp a, b", "Compute a - b, set flags, DISCARD the result. Precedes a jcc/setcc.", 0, 'i' },
    { "test", "test a, b", "Compute a & b, set flags, discard. `test reg,reg` checks for zero (ZF=1 means zero).", 0, 'i' },
    /* ---- setcc (on-device OK; write to al) ---- */
    { "sete", "sete al", "Set al=1 if the last cmp/test gave EQUAL (ZF=1), else 0. (a.k.a setz).", 0, 'i' },
    { "setne", "setne al", "al = (last compare NOT equal) ? 1 : 0. (setnz).", 0, 'i' },
    { "setl", "setl al", "al = (signed a <  b) ? 1 : 0.", 0, 'i' },
    { "setle", "setle al", "al = (signed a <= b) ? 1 : 0.", 0, 'i' },
    { "setg", "setg al", "al = (signed a >  b) ? 1 : 0.", 0, 'i' },
    { "setge", "setge al", "al = (signed a >= b) ? 1 : 0.", 0, 'i' },
    /* ---- branches (on-device OK) ---- */
    { "jmp", "jmp label", "Unconditional jump to label.", 0, 'i' },
    { "je", "je label", "Jump if EQUAL (ZF=1) after cmp/test. (a.k.a jz).", 0, 'i' },
    { "jne", "jne label", "Jump if NOT equal (ZF=0). (jnz).", 0, 'i' },
    { "jl", "jl label", "Jump if signed LESS (SF!=OF).", 0, 'i' },
    { "jle", "jle label", "Jump if signed LESS-OR-EQUAL.", 0, 'i' },
    { "jg", "jg label", "Jump if signed GREATER.", 0, 'i' },
    { "jge", "jge label", "Jump if signed GREATER-OR-EQUAL.", 0, 'i' },
    /* ---- call / return (on-device OK) ---- */
    { "call", "call label", "Push the return address, jump to label. Keep rsp 16-byte aligned at the call.", 0, 'i' },
    { "ret", "ret", "Pop the return address into rip -> return to the caller.", 0, 'i' },
    { "leave", "leave", "Tear down the frame: mov rsp,rbp ; pop rbp. The epilogue before ret.", 0, 'i' },
    { "syscall", "syscall", "Enter the kernel. rax=number, args rdi rsi rdx r10 r8 r9, result in rax. CLOBBERS rcx and r11.", 0, 'i' },

    /* ---- host-only mnemonics (correct x86-64, but as_x64 won't assemble) ---- */
    { "movsx", "movsx reg, src", "Move + SIGN-extend a smaller value. [host as/nasm only -- as_x64 has movzx only].", 0, 'i' },
    { "movabs", "movabs reg, imm64", "Load a full 64-bit immediate into reg. [host as/nasm only].", 0, 'i' },
    { "xchg", "xchg a, b", "Atomically swap two operands (implicit LOCK on memory). [host as/nasm only].", 0, 'i' },
    { "inc", "inc reg", "reg = reg + 1 (leaves CF). On-device: emit `add reg, 1` instead. [host as/nasm only].", 0, 'i' },
    { "dec", "dec reg", "reg = reg - 1. On-device: emit `sub reg, 1` instead. [host as/nasm only].", 0, 'i' },
    { "mul", "mul reg", "UNSIGNED multiply rax*reg -> rdx:rax. [host as/nasm only -- as_x64 has imul].", 0, 'i' },
    { "div", "div reg", "UNSIGNED divide rdx:rax by reg. Zero rdx first (xor rdx,rdx). [host as/nasm only].", 0, 'i' },
    { "sar", "sar reg, imm/cl", "Shift Arithmetic Right (sign-fill) = signed /2. [host as/nasm only -- as_x64 has shr].", 0, 'i' },
    { "rol", "rol reg, imm/cl", "Rotate bits left (wraps top bit to bottom). [host as/nasm only].", 0, 'i' },
    { "ror", "ror reg, imm/cl", "Rotate bits right. [host as/nasm only].", 0, 'i' },
    { "adc", "adc dst, src", "Add WITH carry: dst = dst + src + CF. Multi-word addition. [host as/nasm only].", 0, 'i' },
    { "sbb", "sbb dst, src", "Subtract with borrow. Multi-word subtraction. [host as/nasm only].", 0, 'i' },
    { "jz",  "jz label", "Alias of je (jump if zero). [host as/nasm only -- emit je].", 0, 'i' },
    { "jnz", "jnz label", "Alias of jne. [host as/nasm only -- emit jne].", 0, 'i' },
    { "jb",  "jb label", "Jump if UNSIGNED below (CF=1). [host as/nasm only].", 0, 'i' },
    { "jbe", "jbe label", "Jump if unsigned below-or-equal. [host as/nasm only].", 0, 'i' },
    { "ja",  "ja label", "Jump if unsigned above. [host as/nasm only].", 0, 'i' },
    { "jae", "jae label", "Jump if unsigned above-or-equal (CF=0). [host as/nasm only].", 0, 'i' },
    { "js",  "js label", "Jump if SIGN flag set (result negative). [host as/nasm only].", 0, 'i' },
    { "jns", "jns label", "Jump if sign flag clear (result non-negative). [host as/nasm only].", 0, 'i' },
    { "jc",  "jc label", "Jump if CARRY. [host as/nasm only].", 0, 'i' },
    { "loop", "loop label", "Decrement rcx; jump to label while rcx!=0. [host as/nasm only].", 0, 'i' },
    { "nop", "nop", "No operation (padding / alignment). [host as/nasm only].", 0, 'i' },
    { "pause", "pause", "Spin-loop hint: relax the pipeline in a busy-wait. [host as/nasm only].", 0, 'i' },
    { "hlt", "hlt", "Halt the CPU until the next interrupt (ring 0). [host as/nasm only].", 0, 'i' },
    { "cli", "cli", "Clear IF: disable maskable interrupts (ring 0). [host as/nasm only].", 0, 'i' },
    { "sti", "sti", "Set IF: enable interrupts (ring 0). [host as/nasm only].", 0, 'i' },
    { "int", "int imm8", "Raise software interrupt/vector. [host as/nasm only].", 0, 'i' },
    { "iretq", "iretq", "Interrupt return (pop rip,cs,rflags,rsp,ss). Ring-0 ISR exit. [host as/nasm only].", 0, 'i' },
    { "rdtsc", "rdtsc", "Read CPU timestamp counter into edx:eax. [host as/nasm only].", 0, 'i' },
    { "cpuid", "cpuid", "Query CPU features; input in eax, results in eax/ebx/ecx/edx. [host as/nasm only].", 0, 'i' },
    { "rdmsr", "rdmsr", "Read model-specific register ecx -> edx:eax (ring 0). [host as/nasm only].", 0, 'i' },
    { "wrmsr", "wrmsr", "Write edx:eax to MSR ecx (ring 0; e.g. LSTAR/EFER). [host as/nasm only].", 0, 'i' },
    { "invlpg", "invlpg [mem]", "Invalidate one TLB entry for a page (ring 0). [host as/nasm only].", 0, 'i' },
    { "in", "in al/ax, dx", "Read a byte/word from an I/O port (ring 0). [host as/nasm only].", 0, 'i' },
    { "out", "out dx, al/ax", "Write a byte/word to an I/O port (ring 0). [host as/nasm only].", 0, 'i' },
    { "lock", "lock <rmw-insn>", "Prefix: make the next read-modify-write atomic (lock add/xchg/cmpxchg). [host as/nasm only].", 0, 'i' },
    { "bt", "bt reg, n", "Bit Test: copy bit n into CF. [host as/nasm only].", 0, 'i' },
    { "bsf", "bsf dst, src", "Bit Scan Forward: index of lowest set bit. [host as/nasm only].", 0, 'i' },
    { "bsr", "bsr dst, src", "Bit Scan Reverse: index of highest set bit. [host as/nasm only].", 0, 'i' },
    { "popcnt", "popcnt dst, src", "Count set bits in src -> dst (SSE4.2). [host as/nasm only].", 0, 'i' },
    { "mfence", "mfence", "Full memory barrier (orders loads+stores). [host as/nasm only].", 0, 'i' },
    { "rep", "rep movsb/stosb", "Repeat the string op rcx times. [host as/nasm only].", 0, 'i' },
    { "movsb", "movsb (rep)", "Copy byte [rsi]->[rdi], advance both. With `rep`, a memcpy of rcx bytes. [host as/nasm only].", 0, 'i' },
    { "stosb", "stosb (rep)", "Store al to [rdi], advance rdi. With `rep`, a memset of rcx bytes. [host as/nasm only].", 0, 'i' },

    /* ===================================================================== *
     *  DIRECTIVES  (kind 'd')  -- on-device as_x64 accepts these
     * ===================================================================== */
    { "section", "section .text/.data", "Start a section: `.text` for code, `.data` for initialized bytes.", 0, 'd' },
    { "global", "global name", "Export a label (e.g. `global _start`) so the loader/linker can find the entry.", 0, 'd' },
    { "db", "db 0x41, \"hi\", 0", "Define Bytes in .data: numbers and/or string literals, comma-separated.", 0, 'd' },
    { "dq", "dq value/label", "Define Quad-word (8 bytes) in .data: an immediate or a label address.", 0, 'd' },

    /* ===================================================================== *
     *  IDIOMS  (kind 's')  -- ready to insert
     * ===================================================================== */
    { "prologue", "frame setup", "Standard function entry: save caller's frame, make room for N bytes of locals.",
      "push rbp\nmov rbp, rsp\nsub rsp, ${1:16}        ; locals at [rbp-8], [rbp-16], ...\n$0", 's' },
    { "epilogue", "frame teardown + ret", "Standard function exit (mirrors `prologue`). `leave` undoes the frame.",
      "leave                  ; mov rsp,rbp ; pop rbp\nret$0", 's' },
    { "startstub", "_start: call main; exit", "AutomationOS program entry. Exit value = main's return. SYS_EXIT is 0 here.",
      "global _start\n_start:\n    call main\n    mov rdi, rax        ; exit code = main() return\n    mov rax, 0          ; SYS_EXIT (== 0 on AutomationOS)\n    syscall\n$0", 's' },
    { "sysinvoke", "syscall NR(a1,a2,a3)", "House syscall sequence. rcx and r11 are destroyed; arg4 (if any) goes in r10, not rcx.",
      "mov rax, ${1:NR}        ; syscall number\nmov rdi, ${2:a1}\nmov rsi, ${3:a2}\nmov rdx, ${4:a3}\nsyscall                ; result -> rax (rcx,r11 clobbered)\n$0", 's' },
    { "syswrite", "write(1, msg, len)", "Print a string to fd 1 via SYS_WRITE (number 3).",
      "mov rax, 3             ; SYS_WRITE\nmov rdi, 1             ; fd = stdout\nlea rsi, [${1:msg}]\nmov rdx, ${2:len}\nsyscall$0", 's' },
    { "zeroreg", "xor reg, reg", "The 1-byte idiom to set a register to 0 (shorter/faster than mov reg,0).",
      "xor ${1:rax}, ${1:rax}$0", 's' },
    { "spinpause", "bounded spin loop", "Busy-wait a fixed number of iterations, relaxing the pipeline each pass.",
      "mov rcx, ${1:1000}\n.${2:spin}:\n    pause              ; [host as/nasm] hint; omit for as_x64\n    sub rcx, 1\n    cmp rcx, 0\n    jne .${2:spin}\n$0", 's' },
    { "loopcount", "for i in 0..n", "Counted asm loop using a register as the index.",
      "xor ${1:rbx}, ${1:rbx}     ; i = 0\n.${2:loop}:\n    cmp ${1:rbx}, ${3:n}\n    jge .${2:done}\n    $0\n    add ${1:rbx}, 1\n    jmp .${2:loop}\n.${2:done}:", 's' },
    { "datastr", "section .data string", "Declare a NUL-terminated string and its length for SYS_WRITE.",
      "section .data\n${1:msg}: db \"${2:hello}\", 10, 0\n${1:msg}_len: equ $ - ${1:msg} - 1\n$0", 's' },
};
#define IDE_DICT_ASM_COUNT ((int)(sizeof(IDE_DICT_ASM) / sizeof(IDE_DICT_ASM[0])))

#endif /* IDE_DICT_ASM_H */
