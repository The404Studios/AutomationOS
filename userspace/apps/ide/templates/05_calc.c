/* ============================================================================
 * 05_calc.c -- a tiny integer expression evaluator: + - * / with correct
 *              operator precedence and parentheses, parsed by RECURSIVE DESCENT
 *              over a string. For the AutomationOS on-device C compiler. This is
 *              the "learn recursion" template.
 *
 * WHAT THIS PROGRAM DOES (in words):
 *   It reads a small arithmetic expression written as text, such as "2+3*4" or
 *   "(2+3)*4", and computes its integer value the way you would expect: * and /
 *   bind tighter than + and -, and parentheses override everything. It prints
 *   each expression next to its result. A handful of expressions are evaluated
 *   to show it working.
 *
 * THE BIG IDEA -- A GRAMMAR MADE OF THREE FUNCTIONS THAT CALL EACH OTHER:
 *   Precedence is expressed by LAYERS, each a function, each handling one
 *   precedence level and deferring tighter-binding work to the next layer down:
 *
 *       expr   :=  term   { ('+' | '-') term }      <- lowest precedence
 *       term   :=  factor { ('*' | '/') factor }    <- higher precedence
 *       factor :=  number | '(' expr ')' | '-' factor   <- highest; recurses to expr
 *
 *   Reading top to bottom: an expression is a chain of terms added/subtracted; a
 *   term is a chain of factors multiplied/divided; a factor is a number, or a
 *   whole parenthesized expression (which is why factor calls expr again -- that
 *   call is the recursion that makes nesting work to any depth).
 *
 * HOW THE STRING IS READ -- A CURSOR, NOT AN ARRAY:
 *   The on-device cc has no writable arrays, but a STRING LITERAL is real
 *   read-only memory and a char* can walk it. So the "input" is two globals:
 *       g_src  : a char* pointing at the expression text
 *       g_pos  : how many characters we have consumed so far
 *   peek() looks at the current character (g_src[g_pos]); advance() moves the
 *   cursor forward by one. The terminating NUL of the string stops every loop,
 *   so we never read past the end.
 *
 * COMPILER RULES THAT SHAPED THIS FILE:
 *   - NO #include / #define; self-contained, integers only.
 *   - GLOBALS START AT ZERO; g_src and g_pos are set at the top of eval().
 *   - Forward prototypes are declared so the mutually-recursive functions can
 *     refer to each other (and so a host syntax check passes).
 *   - NO stdin needed: the expressions are string literals.
 *
 * HOW TO EXTEND IT:
 *   - Add '%' (modulo): handle it in parse_term beside '*' and '/'.
 *   - Add comparison or power: introduce a new precedence LAYER (another
 *     function) above or below the right level and chain it in.
 *   - Evaluate user input: point g_src at a typed string once a proven stdin
 *     path exists; the parser does not change.
 *
 * Exit code = 0.
 * ==========================================================================*/

/* On-device builtin (prototype only for the host syntax check; see 01_login.c). */
void sys_write(int fd, char *buf, int len);

/* ---- output layer --------------------------------------------------------- */
char g_ch;
/* emit: write a single character to stdout. */
void emit(int c) { g_ch = c; sys_write(1, &g_ch, 1); }
/* puts0: write a NUL-terminated string. */
void puts0(char *s) { int n = 0; while (s[n]) n = n + 1; sys_write(1, s, n); }
/* puti: print a signed integer in decimal. */
void putu(int v) { if (v >= 10) putu(v / 10); emit('0' + v % 10); }
void puti(int v) { if (v < 0) { emit('-'); v = 0 - v; } putu(v); }

/* ---- the input cursor ----------------------------------------------------- */
char *g_src;      /* the expression text (set in eval) */
int   g_pos;      /* index of the next unread character (set to 0 in eval) */

/* peek: the current character, or 0 (NUL) at the end of the string. */
int peek(void) { return g_src[g_pos]; }
/* advance: consume the current character (move the cursor forward by one). */
void advance(void) { g_pos = g_pos + 1; }
/* skip_ws: consume any run of spaces so " 2 + 3 " parses like "2+3". */
void skip_ws(void) { while (peek() == ' ') advance(); }

/* Forward declarations: parse_factor needs to call parse_expr for parentheses,
 * but parse_expr is defined later. On-device these prototypes are ignored (the
 * call is resolved by name); on the host they let the file type-check. */
int parse_expr(void);
int parse_term(void);
int parse_factor(void);

/* parse_number: read a run of digits and return its value.
 *   Contract: the cursor is on a digit; on return it is just past the number. */
int parse_number(void) {
    int v = 0;
    while (peek() >= '0' && peek() <= '9') {
        v = v * 10 + (peek() - '0');
        advance();
    }
    return v;
}

/* parse_factor: the highest-precedence layer.
 *   Handles: a parenthesized sub-expression, a unary minus, or a plain number.
 *   Contract: returns the factor's value; the cursor ends just past it. */
int parse_factor(void) {
    skip_ws();
    if (peek() == '(') {
        advance();                 /* consume '(' */
        int v = parse_expr();      /* THE RECURSION: a full expression inside */
        skip_ws();
        if (peek() == ')') advance();   /* consume the matching ')' */
        return v;
    }
    if (peek() == '-') {
        advance();                 /* unary minus: negate the next factor */
        return 0 - parse_factor();
    }
    return parse_number();
}

/* parse_term: the * and / layer. Start with one factor, then fold in each
 *   following '*' factor or '/' factor, left to right.
 *   Contract: returns the term's value; cursor ends just past it. */
int parse_term(void) {
    int v = parse_factor();
    skip_ws();
    while (peek() == '*' || peek() == '/') {
        int op = peek();
        advance();
        int r = parse_factor();
        if (op == '*') v = v * r;
        else           v = v / r;
        skip_ws();
    }
    return v;
}

/* parse_expr: the + and - layer (lowest precedence). Start with one term, then
 *   fold in each following '+' term or '-' term, left to right.
 *   Contract: returns the expression's value; cursor ends at NUL or ')'. */
int parse_expr(void) {
    int v = parse_term();
    skip_ws();
    while (peek() == '+' || peek() == '-') {
        int op = peek();
        advance();
        int r = parse_term();
        if (op == '+') v = v + r;
        else           v = v - r;
        skip_ws();
    }
    return v;
}

/* eval: point the cursor at s, reset the position, and evaluate the whole thing. */
int eval(char *s) {
    g_src = s;
    g_pos = 0;
    return parse_expr();
}

/* show: print "<expr> = <value>" on its own line. */
void show(char *s) {
    puts0(s);
    puts0(" = ");
    puti(eval(s));
    emit('\n');
}

/* main: evaluate several expressions that together prove precedence, left-to-
 * right folding, parentheses, and unary minus all work.
 *   1. print a header.
 *   2. evaluate the sample expressions.
 *   3. return 0.
 */
int main(void) {
    /* step 1 */
    puts0("== AutomationOS expression evaluator ==\n");
    puts0("integer + - * / with precedence and parentheses\n\n");

    /* step 2 */
    show("2+3*4");          /* precedence: 2 + 12        = 14 */
    show("(2+3)*4");        /* parentheses: 5 * 4        = 20 */
    show("100/5-7");        /* left to right: 20 - 7     = 13 */
    show("2*2*2*2*2");      /* repeated multiply         = 32 */
    show("10 - 2 - 3");     /* left-assoc subtraction    =  5 */
    show("-(4+1)*2");       /* unary minus then multiply = -10 */
    show("7 + 6 * (5 - 2)");/* nested parens: 7 + 18     = 25 */

    /* step 3 */
    puts0("\ndone.\n");
    return 0;
}
