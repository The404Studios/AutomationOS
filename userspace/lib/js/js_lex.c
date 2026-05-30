/*
 * js_lex.c -- tokenizer for the AutomationOS JS engine.
 * =====================================================
 *
 * Scans source into js_token values. Handles:
 *   - numbers: integer, float, hex (0x), exponent (1e3, 1.5E-2)
 *   - strings: single- and double-quoted with escapes (\n \t \\ \" \' \r
 *     \b \f \0 \xHH \uHHHH), template literals `...${...}` (lexed as plain
 *     template strings without interpolation splitting -- see note)
 *   - identifiers and keywords
 *   - all operators and punctuation, multi-char ops greedily
 *   - // line comments and / * ... * / block comments
 *   - newline tracking for ASI-lite (tok.nl_before)
 *
 * Template literals: we tokenize a backtick string into T_TEMPLATE holding the
 * raw text with ${...} left intact; the parser turns simple `${expr}` runs
 * into string + (expr) concatenations. (Documented limitation: nested
 * backticks inside ${} are not supported.)
 */

#include "js_internal.h"

/* ------------------------------------------------------------------ */
static int is_digit(int c)  { return c >= '0' && c <= '9'; }
static int is_hex(int c)
{
    return (c>='0'&&c<='9')||(c>='a'&&c<='f')||(c>='A'&&c<='F');
}
static int hexval(int c)
{
    if (c>='0'&&c<='9') return c-'0';
    if (c>='a'&&c<='f') return c-'a'+10;
    return c-'A'+10;
}
static int is_ident_start(int c)
{
    return (c>='a'&&c<='z')||(c>='A'&&c<='Z')||c=='_'||c=='$';
}
static int is_ident_part(int c)
{
    return is_ident_start(c) || is_digit(c);
}

/* ------------------------------------------------------------------ */
void js_lex_init(js_lexer *lx, js_vm *vm, const char *src, js_usize len)
{
    lx->vm = vm;
    lx->src = src;
    lx->cur = src;
    lx->end = src + len;
    lx->line = 1;
    lx->had_nl = 0;
    lx->error = 0;
    lx->tok.kind = T_EOF;
    /* prime first token */
    js_lex_next(lx);
}

static int peek(js_lexer *lx)  { return lx->cur < lx->end ? (js_u8)*lx->cur : -1; }
static int peek2(js_lexer *lx) { return (lx->cur+1) < lx->end ? (js_u8)lx->cur[1] : -1; }
static int adv(js_lexer *lx)   { int c = peek(lx); if (c>=0) lx->cur++; return c; }

/* skip whitespace + comments, recording whether a newline was crossed */
static void skip_trivia(js_lexer *lx)
{
    lx->had_nl = 0;
    for (;;) {
        int c = peek(lx);
        if (c == ' ' || c == '\t' || c == '\r' || c == '\v' || c == '\f') {
            lx->cur++;
        } else if (c == '\n') {
            lx->cur++; lx->line++; lx->had_nl = 1;
        } else if (c == '/' && peek2(lx) == '/') {
            lx->cur += 2;
            while (peek(lx) >= 0 && peek(lx) != '\n') lx->cur++;
        } else if (c == '/' && peek2(lx) == '*') {
            lx->cur += 2;
            while (lx->cur < lx->end) {
                if (peek(lx) == '*' && peek2(lx) == '/') { lx->cur += 2; break; }
                if (peek(lx) == '\n') { lx->line++; lx->had_nl = 1; }
                lx->cur++;
            }
        } else {
            break;
        }
    }
}

/* ------------------------------------------------------------------ */
/*  String/escape scanning into a temporary buffer                    */
/* ------------------------------------------------------------------ */
static void emit_utf8(char *buf, js_usize *n, js_u32 cp)
{
    if (cp < 0x80) {
        buf[(*n)++] = (char)cp;
    } else if (cp < 0x800) {
        buf[(*n)++] = (char)(0xC0 | (cp >> 6));
        buf[(*n)++] = (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        buf[(*n)++] = (char)(0xE0 | (cp >> 12));
        buf[(*n)++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        buf[(*n)++] = (char)(0x80 | (cp & 0x3F));
    } else {
        buf[(*n)++] = (char)(0xF0 | (cp >> 18));
        buf[(*n)++] = (char)(0x80 | ((cp >> 12) & 0x3F));
        buf[(*n)++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        buf[(*n)++] = (char)(0x80 | (cp & 0x3F));
    }
}

/* Reads a quoted string after the opening quote has been consumed. */
static js_string *scan_string(js_lexer *lx, int quote, int is_template)
{
    /* Decode into arena scratch sized to source remaining (upper bound). */
    js_usize remain = (js_usize)(lx->end - lx->cur) + 1;
    char *buf = (char *)js_arena_alloc(lx->vm, remain);
    if (!buf) { lx->error = 1; return NULL; }
    js_usize n = 0;

    for (;;) {
        int c = peek(lx);
        if (c < 0) { lx->error = 1; return NULL; }
        if (c == quote) { lx->cur++; break; }
        if (c == '\n' && !is_template) { lx->error = 1; return NULL; }
        if (c == '\n') { lx->line++; }
        if (c == '\\') {
            lx->cur++;
            int e = adv(lx);
            switch (e) {
            case 'n': buf[n++]='\n'; break;
            case 't': buf[n++]='\t'; break;
            case 'r': buf[n++]='\r'; break;
            case 'b': buf[n++]='\b'; break;
            case 'f': buf[n++]='\f'; break;
            case 'v': buf[n++]='\v'; break;
            case '0': buf[n++]='\0'; break;
            case '\\': buf[n++]='\\'; break;
            case '\'': buf[n++]='\''; break;
            case '"': buf[n++]='"'; break;
            case '`': buf[n++]='`'; break;
            case '\n': lx->line++; break;        /* line continuation */
            case 'x': {
                if (is_hex(peek(lx)) && is_hex(peek2(lx))) {
                    int hi = hexval(adv(lx)), lo = hexval(adv(lx));
                    emit_utf8(buf, &n, (js_u32)(hi*16+lo));
                } else { buf[n++]='x'; }
                break;
            }
            case 'u': {
                js_u32 cp = 0;
                if (peek(lx) == '{') {
                    lx->cur++;
                    while (is_hex(peek(lx))) cp = cp*16 + hexval(adv(lx));
                    if (peek(lx) == '}') lx->cur++;
                } else {
                    for (int k = 0; k < 4 && is_hex(peek(lx)); k++)
                        cp = cp*16 + hexval(adv(lx));
                }
                emit_utf8(buf, &n, cp);
                break;
            }
            default: buf[n++] = (char)e; break;
            }
        } else {
            buf[n++] = (char)adv(lx);
        }
    }
    js_string *s = js_str_new(lx->vm, buf, n);
    return s;
}

/* ------------------------------------------------------------------ */
/*  Keyword lookup                                                    */
/* ------------------------------------------------------------------ */
static js_tok_kind keyword_kind(const char *s, js_usize len)
{
    /* switch on first char + length for speed and simplicity */
    #define KW(str, tok) do { \
        if (len == sizeof(str)-1 && js_memcmp(s, str, len) == 0) return tok; \
    } while (0)
    switch (s[0]) {
    case 'b': KW("break", T_BREAK); break;
    case 'c': KW("const", T_CONST); KW("continue", T_CONTINUE);
              KW("catch", T_CATCH); break;
    case 'd': KW("do", T_DO); KW("delete", T_DELETE); break;
    case 'e': KW("else", T_ELSE); break;
    case 'f': KW("function", T_FUNCTION); KW("for", T_FOR);
              KW("false", T_FALSE); KW("finally", T_FINALLY); break;
    case 'i': KW("if", T_IF); KW("in", T_IN);
              KW("instanceof", T_INSTANCEOF); break;
    case 'l': KW("let", T_LET); break;
    case 'n': KW("new", T_NEW); KW("null", T_NULL); break;
    case 'o': KW("of", T_OF); break;
    case 'r': KW("return", T_RETURN); break;
    case 't': KW("typeof", T_TYPEOF); KW("true", T_TRUE);
              KW("this", T_THIS); KW("throw", T_THROW); KW("try", T_TRY);
              break;
    case 'u': KW("undefined", T_UNDEFINED); break;
    case 'v': KW("var", T_VAR); KW("void", T_VOID); break;
    case 'w': KW("while", T_WHILE); break;
    }
    #undef KW
    return T_IDENT;
}

/* ------------------------------------------------------------------ */
/*  Main scanner                                                      */
/* ------------------------------------------------------------------ */
static void set_simple(js_lexer *lx, js_tok_kind k, const char *start)
{
    lx->tok.kind = k;
    lx->tok.start = start;
    lx->tok.len = (js_usize)(lx->cur - start);
}

void js_lex_next(js_lexer *lx)
{
    skip_trivia(lx);
    int nl = lx->had_nl;
    const char *start = lx->cur;
    lx->tok.nl_before = nl;
    lx->tok.line = lx->line;
    lx->tok.str = NULL;

    int c = peek(lx);
    if (c < 0) { set_simple(lx, T_EOF, start); return; }

    /* numbers */
    if (is_digit(c) || (c == '.' && is_digit(peek2(lx)))) {
        int ok = 0;
        /* hex */
        if (c == '0' && (peek2(lx)=='x'||peek2(lx)=='X')) {
            lx->cur += 2;
            while (is_hex(peek(lx))) lx->cur++;
        } else {
            while (is_digit(peek(lx))) lx->cur++;
            if (peek(lx) == '.') { lx->cur++; while (is_digit(peek(lx))) lx->cur++; }
            if (peek(lx)=='e'||peek(lx)=='E') {
                lx->cur++;
                if (peek(lx)=='+'||peek(lx)=='-') lx->cur++;
                while (is_digit(peek(lx))) lx->cur++;
            }
        }
        js_usize len = (js_usize)(lx->cur - start);
        lx->tok.num = js_parse_double(start, len, &ok);
        if (!ok) lx->tok.num = js_nan();
        set_simple(lx, T_NUMBER, start);
        return;
    }

    /* identifiers / keywords */
    if (is_ident_start(c)) {
        lx->cur++;
        while (is_ident_part(peek(lx))) lx->cur++;
        js_usize len = (js_usize)(lx->cur - start);
        js_tok_kind k = keyword_kind(start, len);
        lx->tok.kind = k;
        lx->tok.start = start;
        lx->tok.len = len;
        if (k == T_IDENT) lx->tok.str = js_str_intern(lx->vm, start, len);
        return;
    }

    /* strings */
    if (c == '"' || c == '\'') {
        lx->cur++;
        lx->tok.str = scan_string(lx, c, 0);
        set_simple(lx, lx->error ? T_ERROR : T_STRING, start);
        return;
    }
    if (c == '`') {
        lx->cur++;
        lx->tok.str = scan_string(lx, '`', 1);
        set_simple(lx, lx->error ? T_ERROR : T_TEMPLATE, start);
        return;
    }

    /* operators / punctuation (greedy multi-char) */
    lx->cur++;   /* consume first char of operator */
    switch (c) {
    case '(' : set_simple(lx, T_LPAREN, start); return;
    case ')' : set_simple(lx, T_RPAREN, start); return;
    case '{' : set_simple(lx, T_LBRACE, start); return;
    case '}' : set_simple(lx, T_RBRACE, start); return;
    case '[' : set_simple(lx, T_LBRACKET, start); return;
    case ']' : set_simple(lx, T_RBRACKET, start); return;
    case ';' : set_simple(lx, T_SEMI, start); return;
    case ',' : set_simple(lx, T_COMMA, start); return;
    case ':' : set_simple(lx, T_COLON, start); return;
    case '~' : set_simple(lx, T_BNOT, start); return;
    case '.' : set_simple(lx, T_DOT, start); return;
    case '?' :
        if (peek(lx)=='?') { lx->cur++; set_simple(lx, T_NULLISH, start); return; }
        set_simple(lx, T_QUESTION, start); return;
    case '+' :
        if (peek(lx)=='+') { lx->cur++; set_simple(lx, T_INC, start); return; }
        if (peek(lx)=='=') { lx->cur++; set_simple(lx, T_PLUSEQ, start); return; }
        set_simple(lx, T_PLUS, start); return;
    case '-' :
        if (peek(lx)=='-') { lx->cur++; set_simple(lx, T_DEC, start); return; }
        if (peek(lx)=='=') { lx->cur++; set_simple(lx, T_MINUSEQ, start); return; }
        set_simple(lx, T_MINUS, start); return;
    case '*' :
        if (peek(lx)=='*') {
            lx->cur++;
            set_simple(lx, T_STARSTAR, start); return;
        }
        if (peek(lx)=='=') { lx->cur++; set_simple(lx, T_STAREQ, start); return; }
        set_simple(lx, T_STAR, start); return;
    case '/' :
        if (peek(lx)=='=') { lx->cur++; set_simple(lx, T_SLASHEQ, start); return; }
        set_simple(lx, T_SLASH, start); return;
    case '%' :
        if (peek(lx)=='=') { lx->cur++; set_simple(lx, T_PERCENTEQ, start); return; }
        set_simple(lx, T_PERCENT, start); return;
    case '=' :
        if (peek(lx)=='=') {
            lx->cur++;
            if (peek(lx)=='=') { lx->cur++; set_simple(lx, T_SEQ, start); return; }
            set_simple(lx, T_EQ, start); return;
        }
        if (peek(lx)=='>') { lx->cur++; set_simple(lx, T_ARROW, start); return; }
        set_simple(lx, T_ASSIGN, start); return;
    case '!' :
        if (peek(lx)=='=') {
            lx->cur++;
            if (peek(lx)=='=') { lx->cur++; set_simple(lx, T_SNEQ, start); return; }
            set_simple(lx, T_NEQ, start); return;
        }
        set_simple(lx, T_NOT, start); return;
    case '<' :
        if (peek(lx)=='<') {
            lx->cur++;
            if (peek(lx)=='=') { lx->cur++; set_simple(lx, T_SHLEQ, start); return; }
            set_simple(lx, T_SHL, start); return;
        }
        if (peek(lx)=='=') { lx->cur++; set_simple(lx, T_LE, start); return; }
        set_simple(lx, T_LT, start); return;
    case '>' :
        if (peek(lx)=='>') {
            lx->cur++;
            if (peek(lx)=='>') {
                lx->cur++;
                if (peek(lx)=='=') { lx->cur++; set_simple(lx, T_USHREQ, start); return; }
                set_simple(lx, T_USHR, start); return;
            }
            if (peek(lx)=='=') { lx->cur++; set_simple(lx, T_SHREQ, start); return; }
            set_simple(lx, T_SHR, start); return;
        }
        if (peek(lx)=='=') { lx->cur++; set_simple(lx, T_GE, start); return; }
        set_simple(lx, T_GT, start); return;
    case '&' :
        if (peek(lx)=='&') {
            lx->cur++;
            if (peek(lx)=='=') { lx->cur++; set_simple(lx, T_ANDEQ, start); return; }
            set_simple(lx, T_AND, start); return;
        }
        if (peek(lx)=='=') { lx->cur++; set_simple(lx, T_BANDEQ, start); return; }
        set_simple(lx, T_BAND, start); return;
    case '|' :
        if (peek(lx)=='|') {
            lx->cur++;
            if (peek(lx)=='=') { lx->cur++; set_simple(lx, T_OREQ, start); return; }
            set_simple(lx, T_OR, start); return;
        }
        if (peek(lx)=='=') { lx->cur++; set_simple(lx, T_BOREQ, start); return; }
        set_simple(lx, T_BOR, start); return;
    case '^' :
        if (peek(lx)=='=') { lx->cur++; set_simple(lx, T_BXOREQ, start); return; }
        set_simple(lx, T_BXOR, start); return;
    }

    /* unknown char */
    lx->error = 1;
    set_simple(lx, T_ERROR, start);
}
