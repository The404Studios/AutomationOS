/*
 * tc_driver.c -- AutomationOS native toolchain DRIVER.
 *
 * Ties the on-device pipeline stages together so the IDE can turn a source
 * file into a runnable static ELF64:
 *
 *   <path> --ide_read_file--> g_src
 *      C/C++ : parser_init + parse_translation_unit --cc_compile--> g_asm
 *      ASM   : g_src copied verbatim into          --------------->  g_asm
 *   g_asm  --as_assemble--> g_code (machine code, absolute @ TC_ENTRY_VADDR)
 *   g_code --elf_write---->  g_elf (static ELF64)
 *   g_elf  --ide_write_file-> /Desktop/<basename-without-ext> (fallback /tmp)
 *
 * Freestanding: no libc, no malloc, no stdio. All working buffers are STATIC
 * and every copy is bounded; the driver is NULL-safe and never overflows.
 */

#include "tc.h"
#include "ide_ast.h"
#include "ide_lex.h"
#include "ide_parser.h"
#include "ide_sys.h"

/* ---- tiny local helpers (no libc) ------------------------------------- */

/* Copy NUL-terminated `s` into d[0..cap-1], always NUL-terminating. Cap may
 * be 0 (no-op). s may be NULL. Returns bytes copied (excluding NUL). */
static int sk_copy(char* d, const char* s, int cap)
{
    int i = 0;
    if (!d || cap <= 0)
        return 0;
    if (s) {
        while (i < cap - 1 && s[i]) {
            d[i] = s[i];
            i++;
        }
    }
    d[i] = '\0';
    return i;
}

/* Set res->message to `m` (bounded). */
static void set_msg(TcResult* res, const char* m)
{
    sk_copy(res->message, m, (int)sizeof(res->message));
}

/* Copy up to 1500 bytes of generated asm into res->asm_preview, NUL-terminated. */
static void copy_preview(TcResult* res, const char* asm_text)
{
    int cap = (int)sizeof(res->asm_preview);   /* 1536 */
    if (cap > 1501)
        cap = 1501;                             /* ~1500 bytes + NUL */
    sk_copy(res->asm_preview, asm_text, cap);
}

/* One ASCII char to lowercase. */
static char sk_lower(char c)
{
    if (c >= 'A' && c <= 'Z')
        return (char)(c + ('a' - 'A'));
    return c;
}

/* Case-insensitive compare of `ext` (the dotted suffix of path) against the
 * lowercase literal `lit`. Returns 1 on match. */
static int ext_eq(const char* ext, const char* lit)
{
    int i = 0;
    if (!ext || !lit)
        return 0;
    for (;;) {
        char a = sk_lower(ext[i]);
        char b = lit[i];
        if (a != b)
            return 0;
        if (b == '\0')
            return 1;
        i++;
    }
}

/* Return pointer to the last '.' in `path`'s basename, or NULL if none. */
static const char* find_ext(const char* path)
{
    const char* dot = (void*)0;
    int i;
    if (!path)
        return (void*)0;
    for (i = 0; path[i]; i++) {
        char c = path[i];
        if (c == '/' || c == '\\')
            dot = (void*)0;          /* directory separator resets */
        else if (c == '.')
            dot = &path[i];
    }
    return dot;
}

/* Derive the basename (no directory, no extension) into out[0..cap-1]. */
static void basename_noext(const char* path, char* out, int cap)
{
    const char* base = path;
    const char* dot;
    int i;
    if (!out || cap <= 0)
        return;
    out[0] = '\0';
    if (!path)
        return;
    /* advance base past the final separator */
    for (i = 0; path[i]; i++) {
        if (path[i] == '/' || path[i] == '\\')
            base = &path[i + 1];
    }
    dot = find_ext(path);
    i = 0;
    while (base[i] && &base[i] != dot && i < cap - 1) {
        out[i] = base[i];
        i++;
    }
    out[i] = '\0';
    if (i == 0)                       /* empty basename -> fallback */
        sk_copy(out, "out", cap);
}

/* ---- static working buffers ------------------------------------------- */

static char    g_src[TC_ASM_CAP];
static char    g_asm[TC_ASM_CAP];
static uint8_t g_code[TC_CODE_CAP];
static uint8_t g_elf[TC_ELF_CAP];
static Tok     g_toks[PARSE_MAX_TOKS];
static Parser  g_P;

/* ----------------------------------------------------------------------- */

TcLang tc_lang_of(const char* path)
{
    const char* ext = find_ext(path);
    if (!ext)
        return LANG_UNKNOWN;
    if (ext_eq(ext, ".c"))
        return LANG_C;
    if (ext_eq(ext, ".asm") || ext_eq(ext, ".s"))
        return LANG_ASM;
    if (ext_eq(ext, ".cpp") || ext_eq(ext, ".cc") || ext_eq(ext, ".cxx"))
        return LANG_CPP;
    if (ext_eq(ext, ".cs"))
        return LANG_CSHARP;
    return LANG_UNKNOWN;
}

int tc_build(const char* path, TcResult* res)
{
    int srclen;
    int elen;
    char base[160];

    if (!res)
        return 0;

    /* 1. zero result, classify, read source. */
    {
        char* z = (char*)res;
        int i;
        for (i = 0; i < (int)sizeof(*res); i++)
            z[i] = 0;
    }
    res->lang = tc_lang_of(path);

    srclen = ide_read_file(path, g_src, TC_ASM_CAP);
    if (srclen < 0) {
        res->ok = 0;
        set_msg(res, "cannot read file");
        return 0;
    }
    if (srclen > TC_ASM_CAP - 1)      /* clamp; keep room for NUL */
        srclen = TC_ASM_CAP - 1;
    g_src[srclen] = '\0';

    /* 3. dispatch: produce g_asm. */
    switch (res->lang) {
    case LANG_C:
    case LANG_CPP: {
        AstNode* tu;
        parser_init(&g_P, g_src, srclen, g_toks, PARSE_MAX_TOKS);
        tu = parse_translation_unit(&g_P);
        if (!cc_compile(tu, g_asm, TC_ASM_CAP, res->diags, &res->ndiags)) {
            res->ok = 0;
            set_msg(res, "C compile failed");
            copy_preview(res, g_asm);
            return 0;
        }
        if (res->lang == LANG_CPP)
            set_msg(res, "C++ built as C (best-effort). ");
        break;
    }
    case LANG_ASM:
        sk_copy(g_asm, g_src, TC_ASM_CAP);   /* the asm IS the source */
        break;
    case LANG_CSHARP:
        res->ok = 0;
        set_msg(res, "C# needs a CLR/.NET runtime - not supported on AutomationOS");
        return 0;
    case LANG_UNKNOWN:
    default:
        res->ok = 0;
        set_msg(res, "unknown file type (use .c, .asm)");
        return 0;
    }

    /* 4. assemble g_asm -> machine code. */
    if (!as_assemble(g_asm, TC_ENTRY_VADDR, g_code, TC_CODE_CAP,
                     &res->code_len, res->diags, &res->ndiags)
        || res->code_len <= 0) {
        res->ok = 0;
        set_msg(res, "assembly failed");
        copy_preview(res, g_asm);
        return 0;
    }

    /* 5. machine code -> static ELF64. */
    elen = elf_write(g_code, res->code_len, g_elf, TC_ELF_CAP);
    if (elen <= 0) {
        res->ok = 0;
        set_msg(res, "elf write failed");
        return 0;
    }
    res->elf_len = elen;

    /* 6. output path "/Desktop/<base>" and write the ELF; fall back to
     *    "/tmp/<base>" if the Desktop write fails for any reason so a build
     *    never silently fails. */
    basename_noext(path, base, (int)sizeof(base));

    /* preview is independent of where we write. */
    copy_preview(res, g_asm);

    /* Ensure /Desktop exists (ignore "already exists" / any mkdir error). */
    ide_sc(67 /* SYS_MKDIR */, (long)"/Desktop", 0755, 0, 0, 0, 0);

    /* Try /Desktop first. */
    sk_copy(res->out_path, "/Desktop/", (int)sizeof(res->out_path));
    {
        int n = ide_strlen(res->out_path);
        sk_copy(res->out_path + n, base,
                (int)sizeof(res->out_path) - n);
    }

    /* 7. write the ELF, with /tmp fallback. */
    if (ide_write_file(res->out_path, (char*)g_elf, elen) < 0) {
        /* Fall back to /tmp/<base> (the original behavior). */
        sk_copy(res->out_path, "/tmp/", (int)sizeof(res->out_path));
        {
            int n = ide_strlen(res->out_path);
            sk_copy(res->out_path + n, base,
                    (int)sizeof(res->out_path) - n);
        }
        if (ide_write_file(res->out_path, (char*)g_elf, elen) < 0) {
            res->ok = 0;
            set_msg(res, "elf write failed (/Desktop and /tmp)");
            return 0;
        }
    }

    res->ok = 1;
    if (res->message[0] == '\0' || res->lang == LANG_CPP) {
        /* keep any C++ prefix, then append the success summary. */
        char tail[80];
        char num[16];
        int nn = ide_itoa(elen, num);
        int p = 0;
        num[nn] = '\0';
        p += sk_copy(tail + p, "Built ", (int)sizeof(tail) - p);
        p += sk_copy(tail + p, base, (int)sizeof(tail) - p);
        p += sk_copy(tail + p, " (", (int)sizeof(tail) - p);
        p += sk_copy(tail + p, num, (int)sizeof(tail) - p);
        p += sk_copy(tail + p, " bytes)", (int)sizeof(tail) - p);
        if (res->lang == LANG_CPP) {
            int m = ide_strlen(res->message);
            sk_copy(res->message + m, tail,
                    (int)sizeof(res->message) - m);
        } else {
            set_msg(res, tail);
        }
    }
    return 1;
}
