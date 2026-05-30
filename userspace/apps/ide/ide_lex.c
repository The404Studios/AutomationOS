/*
 * ide_lex.c -- C tokenizer + single-line highlighter for the Semantic LEGO IDE.
 *
 * Freestanding: no libc, no malloc, no stdio. Only the helpers declared in
 * ide_sys.h are used (none are actually required here -- self-contained).
 *
 * TK_EOF return convention:
 *   lex_tokenize() returns the number of REAL tokens written, i.e. the count
 *   EXCLUDING the trailing TK_EOF sentinel. The TK_EOF entry is still appended
 *   to toks[] when there is room (so toks[return_value] holds the TK_EOF if it
 *   fit). Callers therefore iterate [0, return_value) for content and may check
 *   toks[return_value].kind == TK_EOF when ret < max.
 */

#include "ide_lex.h"

/* ---- tiny local character predicates (no <ctype.h> in freestanding) ---- */

static int lex_is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' ||
           c == '\v' || c == '\f';
}

static int lex_is_digit(char c)
{
    return c >= '0' && c <= '9';
}

static int lex_is_alpha(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

static int lex_is_ident(char c)
{
    return lex_is_alpha(c) || lex_is_digit(c);
}

/* hex digit (for 0x.. number scanning) */
static int lex_is_hex(char c)
{
    return lex_is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

/* ---- keyword tables --------------------------------------------------- */

/* compare [s,len) to NUL-terminated literal kw, exact full-length match */
static int lex_word_eq(const char* s, int len, const char* kw)
{
    int i = 0;
    for (i = 0; i < len; i++) {
        if (kw[i] == '\0' || s[i] != kw[i])
            return 0;
    }
    return kw[len] == '\0';
}

/* recognise intN_t / uintN_t / for N in 8,16,32,64 (and uintptr_t handled
 * separately in the type table). Expects [s,len). */
static int lex_is_fixed_int(const char* s, int len)
{
    /* possible forms: int8_t int16_t int32_t int64_t
     *                 uint8_t uint16_t uint32_t uint64_t */
    int p = 0;

    if (len < 6) /* shortest is "int8_t" == 6 */
        return 0;

    if (len >= 1 && s[0] == 'u') {
        if (!lex_word_eq(s, 4 < len ? 4 : len, "uint") || len < 7)
            return 0;
        /* match prefix "uint" */
        if (!(s[0] == 'u' && s[1] == 'i' && s[2] == 'n' && s[3] == 't'))
            return 0;
        p = 4;
    } else {
        if (!(s[0] == 'i' && s[1] == 'n' && s[2] == 't'))
            return 0;
        p = 3;
    }

    /* now expect one of: 8_t 16_t 32_t 64_t */
    {
        int rem = len - p;
        if (rem == 3) { /* "8_t" */
            return s[p] == '8' && s[p + 1] == '_' && s[p + 2] == 't';
        } else if (rem == 4) { /* "16_t" "32_t" "64_t" */
            char a = s[p], b = s[p + 1];
            int ok = (a == '1' && b == '6') ||
                     (a == '3' && b == '2') ||
                     (a == '6' && b == '4');
            return ok && s[p + 2] == '_' && s[p + 3] == 't';
        }
    }
    return 0;
}

/* 1 if [s,len) is a C keyword; 2 if it is a type keyword; 0 otherwise. */
int lex_keyword_kind(const char* s, int len)
{
    if (len <= 0 || s == 0)
        return 0;

    /* fixed-width integer typedefs first (intN_t / uintN_t) */
    if (lex_is_fixed_int(s, len))
        return 2;

    /* TYPE keywords */
    {
        static const char* const types[] = {
            "void", "char", "short", "int", "long", "float", "double",
            "unsigned", "signed", "struct", "union", "enum",
            "const", "volatile", "static", "extern", "inline",
            "register", "bool", "_Bool",
            "size_t", "ssize_t", "uintptr_t",
            0
        };
        int i;
        for (i = 0; types[i]; i++) {
            if (lex_word_eq(s, len, types[i]))
                return 2;
        }
    }

    /* CONTROL keywords */
    {
        static const char* const ctrl[] = {
            "if", "else", "for", "while", "do", "switch", "case",
            "default", "break", "continue", "return", "goto",
            "sizeof", "typedef",
            0
        };
        int i;
        for (i = 0; ctrl[i]; i++) {
            if (lex_word_eq(s, len, ctrl[i]))
                return 1;
        }
    }

    return 0;
}

/* ---- tokenizer -------------------------------------------------------- */

/* Emit one token into toks[] if room. Returns 1 if it (logically) counts as a
 * real token slot used, regardless of whether it physically fit; the caller
 * tracks both 'count' (logical) and writes only when n < max. */
static void lex_emit(Tok* toks, int max, int* np, TokKind kind,
                     const char* s, int tlen, int line, int col)
{
    int n = *np;
    if (n < max) {
        toks[n].kind = kind;
        toks[n].s    = s;
        toks[n].len  = tlen;
        toks[n].line = line;
        toks[n].col  = col;
    }
    *np = n + 1;
}

/*
 * Tokenize src[0..len). Returns number of REAL tokens (excluding TK_EOF).
 * A trailing TK_EOF is appended when there is room in toks[].
 * Never reads past src[len-1]; never writes past toks[max-1].
 */
int lex_tokenize(const char* src, int len, Tok* toks, int max)
{
    int i = 0;
    int line = 1;     /* 1-based */
    int col  = 0;     /* 0-based */
    int n    = 0;     /* logical real-token count */

    if (src == 0 || len < 0)
        len = 0;

    while (i < len) {
        char c = src[i];

        /* --- whitespace (advance line/col) --- */
        if (lex_is_space(c)) {
            if (c == '\n') {
                line++;
                col = 0;
            } else if (c == '\t') {
                col++;   /* count tab as one column (simple, consistent) */
            } else if (c == '\r') {
                /* treat lone \r as column reset-ish; keep col advance minimal */
                col++;
            } else {
                col++;
            }
            i++;
            continue;
        }

        /* --- preprocessor: line starting (after optional spaces) with '#' ---
         * We detect '#' as the first non-space char of a line. Since we already
         * skipped leading whitespace above, a '#' here qualifies only if col is
         * at the line's first non-space position. Track that via a flag. */
        if (c == '#') {
            /* verify nothing but spaces precede on this line */
            int j = i - 1;
            int line_start = 1;
            while (j >= 0 && src[j] != '\n') {
                if (!lex_is_space(src[j])) { line_start = 0; break; }
                j--;
            }
            if (line_start) {
                int start = i;
                int scol = col;
                int sline = line;
                while (i < len && src[i] != '\n') {
                    col++;
                    i++;
                }
                lex_emit(toks, max, &n, TK_PREPROC, src + start,
                         i - start, sline, scol);
                continue;
            }
            /* else fall through: treat '#' as punctuation below */
        }

        /* --- comments: line and block --- */
        if (c == '/' && i + 1 < len && src[i + 1] == '/') {
            int start = i;
            int scol = col;
            int sline = line;
            while (i < len && src[i] != '\n') {
                col++;
                i++;
            }
            lex_emit(toks, max, &n, TK_COMMENT, src + start,
                     i - start, sline, scol);
            continue;
        }
        if (c == '/' && i + 1 < len && src[i + 1] == '*') {
            int start = i;
            int scol = col;
            int sline = line;
            i += 2;
            col += 2;
            while (i < len) {
                if (src[i] == '*' && i + 1 < len && src[i + 1] == '/') {
                    i += 2;
                    col += 2;
                    break;
                }
                if (src[i] == '\n') {
                    line++;
                    col = 0;
                } else {
                    col++;
                }
                i++;
            }
            lex_emit(toks, max, &n, TK_COMMENT, src + start,
                     i - start, sline, scol);
            continue;
        }

        /* --- string literal "..." with \ escapes --- */
        if (c == '"') {
            int start = i;
            int scol = col;
            int sline = line;
            i++;
            col++;
            while (i < len) {
                char d = src[i];
                if (d == '\\') {
                    /* escape: consume next char too (if any) */
                    i++;
                    col++;
                    if (i < len) {
                        if (src[i] == '\n') { line++; col = 0; }
                        else { col++; }
                        i++;
                    }
                    continue;
                }
                if (d == '"') {
                    i++;
                    col++;
                    break;
                }
                if (d == '\n') {
                    line++;
                    col = 0;
                } else {
                    col++;
                }
                i++;
            }
            lex_emit(toks, max, &n, TK_STR, src + start,
                     i - start, sline, scol);
            continue;
        }

        /* --- char literal '...' with \ escapes --- */
        if (c == '\'') {
            int start = i;
            int scol = col;
            int sline = line;
            i++;
            col++;
            while (i < len) {
                char d = src[i];
                if (d == '\\') {
                    i++;
                    col++;
                    if (i < len) {
                        if (src[i] == '\n') { line++; col = 0; }
                        else { col++; }
                        i++;
                    }
                    continue;
                }
                if (d == '\'') {
                    i++;
                    col++;
                    break;
                }
                if (d == '\n') {
                    line++;
                    col = 0;
                } else {
                    col++;
                }
                i++;
            }
            lex_emit(toks, max, &n, TK_CHAR, src + start,
                     i - start, sline, scol);
            continue;
        }

        /* --- numbers: digit-led, or '.' followed by a digit --- */
        if (lex_is_digit(c) ||
            (c == '.' && i + 1 < len && lex_is_digit(src[i + 1]))) {
            int start = i;
            int scol = col;
            int sline = line;
            int is_hex = 0;

            if (c == '0' && i + 1 < len &&
                (src[i + 1] == 'x' || src[i + 1] == 'X')) {
                is_hex = 1;
                i += 2;
                col += 2;
            }
            while (i < len) {
                char d = src[i];
                if (is_hex ? lex_is_hex(d) : lex_is_digit(d)) {
                    i++; col++; continue;
                }
                if (d == '.') { i++; col++; continue; }
                /* exponent markers in decimal floats: e/E with optional sign */
                if (!is_hex && (d == 'e' || d == 'E') && i + 1 < len &&
                    (src[i + 1] == '+' || src[i + 1] == '-')) {
                    i += 2; col += 2; continue;
                }
                /* hex float exponent p/P */
                if (is_hex && (d == 'p' || d == 'P') && i + 1 < len &&
                    (src[i + 1] == '+' || src[i + 1] == '-')) {
                    i += 2; col += 2; continue;
                }
                /* suffixes / remaining ident-ish run: u U l L f F z Z, etc. */
                if (lex_is_alpha(d)) { i++; col++; continue; }
                break;
            }
            lex_emit(toks, max, &n, TK_NUM, src + start,
                     i - start, sline, scol);
            continue;
        }

        /* --- identifiers / keywords --- */
        if (lex_is_alpha(c)) {
            int start = i;
            int scol = col;
            int sline = line;
            int kind;
            while (i < len && lex_is_ident(src[i])) {
                i++;
                col++;
            }
            {
                int tlen = i - start;
                int kw = lex_keyword_kind(src + start, tlen);
                if (kw == 2)      kind = TK_TYPE;
                else if (kw == 1) kind = TK_KW;
                else              kind = TK_ID;
                lex_emit(toks, max, &n, (TokKind)kind, src + start,
                         tlen, sline, scol);
            }
            continue;
        }

        /* --- operators / punctuation (longest-match multi-char) --- */
        {
            int oplen = 1;
            int rem = len - i;
            const char* p = src + i;
            if (rem >= 3 &&
                ((p[0] == '<' && p[1] == '<' && p[2] == '=') ||
                 (p[0] == '>' && p[1] == '>' && p[2] == '=') ||
                 (p[0] == '.' && p[1] == '.' && p[2] == '.'))) {
                oplen = 3;
            } else if (rem >= 2) {
                char a = p[0], b = p[1];
                if ((a == '-' && b == '>') || (a == '+' && b == '+') ||
                    (a == '-' && b == '-') || (a == '<' && b == '<') ||
                    (a == '>' && b == '>') || (a == '<' && b == '=') ||
                    (a == '>' && b == '=') || (a == '=' && b == '=') ||
                    (a == '!' && b == '=') || (a == '&' && b == '&') ||
                    (a == '|' && b == '|') || (a == '+' && b == '=') ||
                    (a == '-' && b == '=') || (a == '*' && b == '=') ||
                    (a == '/' && b == '=') || (a == '%' && b == '=') ||
                    (a == '&' && b == '=') || (a == '|' && b == '=') ||
                    (a == '^' && b == '=') || (a == '#' && b == '#')) {
                    oplen = 2;
                }
            }
            lex_emit(toks, max, &n, TK_PUNCT, src + i, oplen, line, col);
            i   += oplen;
            col += oplen;
        }
    }

    /* trailing TK_EOF sentinel, appended only if room (not counted in return) */
    if (n < max) {
        toks[n].kind = TK_EOF;
        toks[n].s    = src + len;
        toks[n].len  = 0;
        toks[n].line = line;
        toks[n].col  = col;
    }

    return n;
}

/* ---- single-line highlighter ----------------------------------------- */

/* Paint cls[a..b) (clamped to [0,len)) with class value v. */
static void lex_paint(unsigned char* cls, int len, int a, int b,
                      unsigned char v)
{
    int k;
    if (a < 0) a = 0;
    if (b > len) b = len;
    for (k = a; k < b; k++)
        cls[k] = v;
}

/*
 * Classify a single source line [0,len) into cls[0..len-1].
 * No cross-line state: block comments are approximated within the line.
 * Never writes cls past index len-1; never reads line past len-1.
 */
void lex_classify_line(const char* line, int len, unsigned char* cls)
{
    int i = 0;
    int j;
    int first_nonspace = -1;

    if (line == 0 || cls == 0 || len <= 0)
        return;

    /* default everything to NORMAL */
    for (j = 0; j < len; j++)
        cls[j] = LEXCLS_NORMAL;

    /* find first non-space character for preproc detection */
    for (j = 0; j < len; j++) {
        if (!lex_is_space(line[j])) { first_nonspace = j; break; }
    }

    /* leading '#' => whole line is PREPROC */
    if (first_nonspace >= 0 && line[first_nonspace] == '#') {
        lex_paint(cls, len, first_nonspace, len, LEXCLS_PREPROC);
        return;
    }

    while (i < len) {
        char c = line[i];

        /* // line comment to EOL */
        if (c == '/' && i + 1 < len && line[i + 1] == '/') {
            lex_paint(cls, len, i, len, LEXCLS_COMMENT);
            return;
        }

        /* block comment: to matching close on this line, or EOL */
        if (c == '/' && i + 1 < len && line[i + 1] == '*') {
            int start = i;
            int end = len;       /* default: comment runs to EOL */
            int k = i + 2;
            while (k < len) {
                if (line[k] == '*' && k + 1 < len && line[k + 1] == '/') {
                    end = k + 2;
                    break;
                }
                k++;
            }
            lex_paint(cls, len, start, end, LEXCLS_COMMENT);
            i = end;
            continue;
        }

        /* "string" with \ escapes (single-line) */
        if (c == '"') {
            int start = i;
            int k = i + 1;
            while (k < len) {
                if (line[k] == '\\') {
                    k += 2;       /* skip escape + escaped char */
                    continue;
                }
                if (line[k] == '"') { k++; break; }
                k++;
            }
            if (k > len) k = len;
            lex_paint(cls, len, start, k, LEXCLS_STRING);
            i = k;
            continue;
        }

        /* 'char' with \ escapes (single-line) */
        if (c == '\'') {
            int start = i;
            int k = i + 1;
            while (k < len) {
                if (line[k] == '\\') {
                    k += 2;
                    continue;
                }
                if (line[k] == '\'') { k++; break; }
                k++;
            }
            if (k > len) k = len;
            lex_paint(cls, len, start, k, LEXCLS_STRING);
            i = k;
            continue;
        }

        /* number: digit-led, or '.' before a digit */
        if (lex_is_digit(c) ||
            (c == '.' && i + 1 < len && lex_is_digit(line[i + 1]))) {
            int start = i;
            int k = i;
            int is_hex = 0;
            if (c == '0' && i + 1 < len &&
                (line[i + 1] == 'x' || line[i + 1] == 'X')) {
                is_hex = 1;
                k += 2;
            }
            while (k < len) {
                char d = line[k];
                if (is_hex ? lex_is_hex(d) : lex_is_digit(d)) { k++; continue; }
                if (d == '.') { k++; continue; }
                if (!is_hex && (d == 'e' || d == 'E') && k + 1 < len &&
                    (line[k + 1] == '+' || line[k + 1] == '-')) { k += 2; continue; }
                if (is_hex && (d == 'p' || d == 'P') && k + 1 < len &&
                    (line[k + 1] == '+' || line[k + 1] == '-')) { k += 2; continue; }
                if (lex_is_alpha(d)) { k++; continue; }
                break;
            }
            lex_paint(cls, len, start, k, LEXCLS_NUMBER);
            i = k;
            continue;
        }

        /* identifier / keyword / type / call */
        if (lex_is_alpha(c)) {
            int start = i;
            int k = i;
            int kw;
            while (k < len && lex_is_ident(line[k]))
                k++;
            kw = lex_keyword_kind(line + start, k - start);
            if (kw == 2) {
                lex_paint(cls, len, start, k, LEXCLS_TYPE);
            } else if (kw == 1) {
                lex_paint(cls, len, start, k, LEXCLS_KEYWORD);
            } else {
                /* identifier: CALL if immediately followed by '(' (skip no
                 * spaces -- "immediately"). */
                if (k < len && line[k] == '(') {
                    lex_paint(cls, len, start, k, LEXCLS_CALL);
                } else {
                    lex_paint(cls, len, start, k, LEXCLS_NORMAL);
                }
            }
            i = k;
            continue;
        }

        /* punctuation / other: leave NORMAL */
        i++;
    }
}
