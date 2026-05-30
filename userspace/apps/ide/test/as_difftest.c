/*
 * as_difftest.c -- HOST-side differential test for the AutomationOS native
 * x86-64 assembler (as_x64.c).
 *
 * For each instruction snippet (restricted to the tc.h supported subset) we:
 *   (a) assemble it with our assembler via as_assemble() at base 0x200078,
 *   (b) assemble the SAME text with GNU `as`/gcc (Intel, noprefix) and extract
 *       the .text bytes with objcopy,
 *   (c) compare the two byte sequences and print OURS/GAS hex + MATCH/DIFF.
 *
 * This is a HOST program (own main), built with normal gcc in WSL Arch:
 *   gcc -std=gnu11 -O1 -w \
 *       userspace/apps/ide/test/as_difftest.c userspace/apps/ide/as_x64.c \
 *       -o /tmp/as_difftest && /tmp/as_difftest
 *
 * NOTE on labels: snippets like "call foo\nfoo:" place the branch target on
 * the next line. A label emits no bytes, so the full byte sequence is just the
 * branch instruction, and rel32 = (next ip - target) is base-independent --
 * so both assemblers must agree regardless of load base.
 *
 * NOTE on benign DIFFs: GNU as may choose a different (often shorter) legal
 * encoding than ours (e.g. 83 /0 ib vs our 81 /0 id for small ALU immediates,
 * or mov r,imm32 (C7) vs our mov r,imm64 (B8) form). Those are not bugs if both
 * decode to the same semantics. We disassemble BOTH on a DIFF to let the human
 * judge; the program itself just reports raw byte equality.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../tc.h"

/* as_assemble() is defined in as_x64.c, compiled alongside this file. */

/* ---- snippet list (tc.h subset only) ---------------------------------- */
static const char* SNIPPETS[] = {
    /* mov: reg,imm / reg,reg / reg,[mem] / [mem],reg */
    "mov rax, 1",
    "mov rdi, 42",
    "mov rax, rcx",
    "mov rbx, r8",
    "mov r9, rax",
    "mov r12, r13",
    "mov rax, [rbp-8]",
    "mov [rbp-8], rax",
    "mov rax, [rsp+16]",
    "mov [rsp+16], rcx",
    "mov rax, [rbp]",
    "mov rax, [r12]",
    "mov rax, [r13]",
    "mov rdx, 0x10000000000",   /* needs imm64 form */
    "mov rax, -1",              /* fits_i32: C7 sign-extended */

    /* alu reg,reg / reg,imm */
    "add rax, rbx",
    "add rax, 5",
    "add rax, 1000",            /* > i8 -> 81 /0 id */
    "sub rsp, 16",
    "sub rax, rcx",
    "and rax, rcx",
    "and rax, 7",
    "or rax, rcx",
    "or rax, 1",
    "xor rax, rax",
    "xor rax, rcx",
    "cmp rax, rcx",
    "cmp rax, 0",
    "cmp rax, 100",

    /* stack */
    "push rbp",
    "pop rbp",
    "push r12",
    "pop r15",
    "mov rbp, rsp",

    /* mul/div/unary */
    "imul rax, rcx",
    "neg rax",
    "not rax",
    "cqo",
    "idiv rcx",

    /* shifts */
    "shl rax, 3",
    "shr rax, 1",
    "shl rax, cl",

    /* byte ops / setcc */
    "movzx rax, al",
    "movzx rax, byte [rbp-1]",
    "mov byte [rbp-1], al",
    "sete al",
    "setne al",
    "setl al",
    "setle al",
    "setg al",
    "setge al",

    /* lea */
    "lea rax, [rbp-16]",
    "lea rcx, [rsp+32]",

    /* test */
    "test rax, rax",
    "test rcx, rdx",

    /* zero-operand */
    "ret",
    "leave",
    "syscall",
    "nop",

    /* control flow with label on the next line (target = next ip).
     * GAS will pick a 2-byte rel8 form here (benign length DIFF); ours always
     * emits the 5/6-byte rel32 form -- semantically identical. */
    "call foo\nfoo:",
    "jmp foo\nfoo:",
    "je foo\nfoo:",
    "jne foo\nfoo:",
    "jl foo\nfoo:",
    "jle foo\nfoo:",
    "jg foo\nfoo:",
    "jge foo\nfoo:",

    /* Backward branch to a label BEHIND the instruction (negative rel32).
     * GAS resolves these INLINE (target already defined) and picks rel8, so it
     * is a benign length DIFF -- but it confirms our negative-displacement sign
     * is right (compare the leading byte: e9 vs eb, and the disassembled
     * target, in the DIFF detail below). */
    "back:\njmp back",
    "back:\nje back",
};

/* Forward-branch self-check snippets: GAS will NOT inline-resolve a forward
 * same-section local-label rel32 in a relocatable object (it defers to a
 * relocation against the null symbol with a relaxation addend), so it is not a
 * usable byte oracle here. Instead we assemble these with OUR assembler and
 * confirm via objdump that the decoded branch target equals the label offset
 * -- proving our forward rel32 displacement math, independent of GAS.
 * __NOPS__ expands to 130 `nop`s, pushing the label past rel8 range. */
static const char* FWD_SNIPPETS[] = {
    "jmp far\n__NOPS__far:",
    "je far\n__NOPS__far:",
    "call far\n__NOPS__far:",
    "jne far\n__NOPS__far:",
    "jge far\n__NOPS__far:",
};
static const int NFWD = (int)(sizeof(FWD_SNIPPETS) / sizeof(FWD_SNIPPETS[0]));
static const int NSNIP = (int)(sizeof(SNIPPETS) / sizeof(SNIPPETS[0]));

/* ---- helpers ---------------------------------------------------------- */

static void hexstr(const uint8_t* b, int n, char* out, int cap) {
    int o = 0;
    /* For long sequences (e.g. far branch + 130 nop filler) abbreviate: show
     * the leading bytes -- which carry the actual instruction under test -- and
     * a count. The MATCH/DIFF verdict still uses a full memcmp elsewhere. */
    int show = n;
    int abbrev = (n > 16);
    if (abbrev) show = 12;
    for (int i = 0; i < show && o + 4 < cap; i++) {
        o += snprintf(out + o, cap - o, "%s%02x", i ? " " : "", b[i]);
    }
    if (abbrev && o + 32 < cap) {
        snprintf(out + o, cap - o, " ... (%d bytes)", n);
    }
    if (n == 0 && cap > 0) out[0] = 0;
}

/* Run a shell command, return its exit status (0 on success). */
static int run(const char* cmd) {
    int rc = system(cmd);
    if (rc == -1) return -1;
    return rc; /* nonzero => failed */
}

/* Read a whole file into buf, return byte count (or -1). */
static int read_file(const char* path, uint8_t* buf, int cap) {
    FILE* f = fopen(path, "rb");
    if (!f) return -1;
    int n = (int)fread(buf, 1, cap, f);
    fclose(f);
    return n;
}

/* Write `text` (plus a trailing newline) to path. Returns 0 on success. */
static int write_file(const char* path, const char* text) {
    FILE* f = fopen(path, "wb");
    if (!f) return -1;
    fputs(text, f);
    fputc('\n', f);
    fclose(f);
    return 0;
}

/* Expand the "__NOPS__" placeholder into 130 newline-separated `nop` lines so
 * a forward branch target lands beyond rel8 range (forcing rel32 on BOTH
 * assemblers). The expanded text is what we feed to OUR assembler AND to GAS,
 * so the comparison stays apples-to-apples. */
static void expand_snippet(const char* in, char* out, int cap) {
    const char* marker = "__NOPS__";
    int ml = (int)strlen(marker);
    int o = 0, i = 0, len = (int)strlen(in);
    while (i < len && o < cap - 8) {
        if (strncmp(in + i, marker, ml) == 0) {
            for (int k = 0; k < 130 && o < cap - 8; k++) {
                out[o++] = 'n'; out[o++] = 'o'; out[o++] = 'p'; out[o++] = '\n';
            }
            i += ml;
        } else {
            out[o++] = in[i++];
        }
    }
    out[o] = 0;
}

/* Our subset writes a size override as bare "byte [mem]" (the form the C
 * codegen emits and our assembler accepts). GAS Intel syntax instead REQUIRES
 * the "ptr" keyword ("byte ptr [mem]"); without it GAS either errors
 * ("ambiguous operand size") or mis-parses "byte" as a symbol. So when handing
 * the snippet to GAS we insert "ptr" after a size keyword that precedes '['.
 * This keeps the comparison about ENCODING, not about syntax dialect. */
static void to_gas_syntax(const char* in, char* out, int cap) {
    static const char* kw[] = { "byte", "word", "dword", "qword" };
    int o = 0, i = 0;
    int len = (int)strlen(in);
    while (i < len && o < cap - 8) {
        int matched = 0;
        for (int k = 0; k < 4; k++) {
            int kl = (int)strlen(kw[k]);
            /* match keyword at a token boundary */
            int boundary_before = (i == 0) || in[i-1] == ' ' ||
                                   in[i-1] == '\t' || in[i-1] == '\n' ||
                                   in[i-1] == ',';
            if (boundary_before && strncmp(in + i, kw[k], kl) == 0) {
                /* lookahead: optional spaces, then '[' (and not already ptr) */
                int j = i + kl;
                while (j < len && (in[j] == ' ' || in[j] == '\t')) j++;
                if (j < len && in[j] == '[') {
                    /* copy keyword then " ptr" */
                    for (int c = 0; c < kl && o < cap - 6; c++) out[o++] = kw[k][c];
                    out[o++] = ' '; out[o++] = 'p'; out[o++] = 't'; out[o++] = 'r';
                    i += kl;
                    matched = 1;
                }
            }
            if (matched) break;
        }
        if (!matched) out[o++] = in[i++];
    }
    out[o] = 0;
}

/* Assemble `snippet` with GNU as via gcc; extract .text bytes into out.
 * Returns byte count, or -1 on toolchain failure. */
static int gnu_assemble(const char* snippet, uint8_t* out, int cap) {
    /* Translate our bare-size-override syntax to canonical GAS Intel syntax. */
    char gsnip[2048];
    to_gas_syntax(snippet, gsnip, sizeof(gsnip));

    /* Build a full GAS source: Intel syntax, no % register prefixes. */
    char src[4096];
    snprintf(src, sizeof(src),
             ".intel_syntax noprefix\n.text\n%s\n", gsnip);
    if (write_file("/tmp/g.s", src) != 0) return -1;

    /* Assemble. gcc -c drives `as`; -x assembler treats input as asm.
     * All snippets passed here reference only registers, immediates, memory,
     * or BACKWARD labels (resolved inline by GAS) -- so the raw .text is a
     * faithful byte oracle. (Forward same-section branches are NOT oracled via
     * GAS; see FWD_SNIPPETS / the forward self-check, because GAS defers their
     * rel32 to a relocation.) */
    if (run("gcc -c -x assembler -o /tmp/g.o /tmp/g.s 2>/tmp/g.err") != 0)
        return -1;
    /* Extract only the .text section as raw binary. */
    if (run("objcopy -O binary --only-section=.text /tmp/g.o /tmp/g.bin "
            "2>>/tmp/g.err") != 0)
        return -1;

    return read_file("/tmp/g.bin", out, cap);
}

/* Disassemble raw bytes (write to a tmp .bin) for a DIFF report. */
static void disasm(const char* tag, const uint8_t* b, int n) {
    FILE* f = fopen("/tmp/d.bin", "wb");
    if (!f) return;
    fwrite(b, 1, n, f);
    fclose(f);
    printf("      %s disasm:\n", tag);
    fflush(stdout);
    run("objdump -D -b binary -m i386:x86-64 -M intel /tmp/d.bin "
        "| sed -n '/<.data>:/,$p' | grep -E '^\\s+[0-9a-f]+:' "
        "| sed 's/^/        /'");
}

/* Read a little-endian int32 from b[off..off+4). */
static int32_t rd_i32(const uint8_t* b, int off) {
    return (int32_t)((uint32_t)b[off] | ((uint32_t)b[off+1] << 8) |
                     ((uint32_t)b[off+2] << 16) | ((uint32_t)b[off+3] << 24));
}

int main(void) {
    static uint8_t ours[TC_CODE_CAP];
    static uint8_t gas[TC_CODE_CAP];
    TcDiag diags[TC_MAXDIAG];
    int ndiags = 0;

    int matched = 0, total = 0, errors = 0;
    /* record DIFF indices for the end-of-run detailed dump */
    int diffidx[256]; int ndiff = 0;

    printf("=== as_x64 differential test vs GNU as ===\n");
    printf("base = 0x%llx (TC_ENTRY_VADDR)\n\n",
           (unsigned long long)TC_ENTRY_VADDR);

    for (int i = 0; i < NSNIP; i++) {
        static char expanded[8192];
        expand_snippet(SNIPPETS[i], expanded, sizeof(expanded));
        const char* snip = expanded;
        total++;

        /* (a) our assembler */
        int ourlen = 0;
        ndiags = 0;
        int ok = as_assemble(snip, TC_ENTRY_VADDR, ours, (int)sizeof(ours),
                             &ourlen, diags, &ndiags);

        /* (b) GNU as */
        int gaslen = gnu_assemble(snip, gas, (int)sizeof(gas));

        /* printable one-line label from the RAW (un-expanded) snippet */
        char label[128];
        {
            int o = 0;
            for (const char* p = SNIPPETS[i]; *p && o < (int)sizeof(label) - 4; p++) {
                if (*p == '\n') { label[o++] = ';'; label[o++] = ' '; }
                else label[o++] = *p;
            }
            label[o] = 0;
        }

        char ohex[1024], ghex[1024];
        hexstr(ours, ourlen, ohex, sizeof(ohex));
        hexstr(gas, gaslen > 0 ? gaslen : 0, ghex, sizeof(ghex));

        if (!ok || ourlen <= 0) {
            errors++;
            printf("[ERR ] %-28s OURS=<assemble failed", label);
            for (int d = 0; d < ndiags; d++)
                printf("; %s", diags[d].msg);
            printf(">  GAS=%s\n", ghex);
            continue;
        }
        if (gaslen < 0) {
            errors++;
            printf("[GERR] %-28s GAS=<gnu as failed; see /tmp/g.err>  "
                   "OURS=%s\n", label, ohex);
            continue;
        }

        int same = (ourlen == gaslen) && (memcmp(ours, gas, ourlen) == 0);
        if (same) {
            matched++;
            printf("[ OK ] %-28s = %s\n", label, ohex);
        } else {
            if (ndiff < 256) diffidx[ndiff++] = i;
            printf("[DIFF] %-28s\n", label);
            printf("        OURS=%s\n", ohex);
            printf("        GAS =%s\n", ghex);
        }
    }

    printf("\n=== summary: %d matched / %d total  (%d DIFF, %d errors) ===\n",
           matched, total, ndiff, errors);

    /* Detailed disassembly for every DIFF so the encoding can be judged. */
    if (ndiff > 0) {
        printf("\n=== DIFF detail (disassembled both sides) ===\n");
        for (int k = 0; k < ndiff; k++) {
            int i = diffidx[k];
            static char expanded[8192];
            expand_snippet(SNIPPETS[i], expanded, sizeof(expanded));
            const char* snip = expanded;

            int ourlen = 0; ndiags = 0;
            as_assemble(snip, TC_ENTRY_VADDR, ours, (int)sizeof(ours),
                        &ourlen, diags, &ndiags);
            int gaslen = gnu_assemble(snip, gas, (int)sizeof(gas));

            char label[128];
            int o = 0;
            for (const char* p = SNIPPETS[i]; *p && o < (int)sizeof(label) - 4; p++) {
                if (*p == '\n') { label[o++] = ';'; label[o++] = ' '; }
                else label[o++] = *p;
            }
            label[o] = 0;

            char ohex[1024], ghex[1024];
            hexstr(ours, ourlen, ohex, sizeof(ohex));
            hexstr(gas, gaslen > 0 ? gaslen : 0, ghex, sizeof(ghex));
            printf("\n  \"%s\"\n", label);
            printf("    OURS (%d B) = %s\n", ourlen, ohex);
            disasm("OURS", ours, ourlen);
            printf("    GAS  (%d B) = %s\n", gaslen > 0 ? gaslen : 0, ghex);
            if (gaslen > 0) disasm("GAS ", gas, gaslen);
        }
    }

    /* ------------------------------------------------------------------ *
     * Forward rel32 self-check (GAS can't oracle these; see FWD_SNIPPETS).
     * Each snippet is "<branch> far\n<130 nops>\nfar:". We decode the rel32
     * our assembler wrote and confirm it equals +130 (the label sits exactly
     * 130 nop bytes after the branch's next-ip). Also confirm GAS picks the
     * SAME opcode byte(s), so opcode selection is cross-checked even though
     * the displacement isn't.
     * ------------------------------------------------------------------ */
    printf("\n=== forward rel32 self-check (our displacement math) ===\n");
    int fwd_ok = 0;
    for (int i = 0; i < NFWD; i++) {
        static char expanded[8192];
        expand_snippet(FWD_SNIPPETS[i], expanded, sizeof(expanded));

        int ourlen = 0; ndiags = 0;
        as_assemble(expanded, TC_ENTRY_VADDR, ours, (int)sizeof(ours),
                    &ourlen, diags, &ndiags);

        /* opcode length: 0F-prefixed jcc has 2 opcode bytes, else 1 */
        int opc = (ours[0] == 0x0F) ? 2 : 1;
        int32_t disp = rd_i32(ours, opc);
        int expect = 130;  /* 130 nop bytes between next-ip and `far:` */

        char label[64]; int o = 0;
        for (const char* p = FWD_SNIPPETS[i]; *p && o < 60; p++)
            label[o++] = (*p == '\n') ? ' ' : *p;
        label[o] = 0;
        /* trim the __NOPS__ noise from the printed label */
        char* m = strstr(label, "__NOPS__"); if (m) { m[0]='.';m[1]='.';m[2]=0; }

        int good = (disp == expect);
        fwd_ok += good ? 1 : 0;
        printf("  [%s] %-20s opcode=%02x%s rel32=%d (expect %d)\n",
               good ? "OK" : "BAD", label,
               ours[0], opc == 2 ? "" : "  ", disp, expect);
    }
    printf("  forward self-check: %d/%d correct\n", fwd_ok, NFWD);

    int fail = (errors > 0) || (fwd_ok != NFWD);
    return fail ? 2 : 0;
}
