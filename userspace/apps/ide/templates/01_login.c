/* ============================================================================
 * 01_login.c -- a login / authentication demo for the AutomationOS on-device C
 *               compiler (the "cc" the Semantic LEGO IDE builds with).
 *
 * WHAT THIS PROGRAM DOES (in words, for a reader who does not picture code):
 *   It pretends to be the front door of a system. There is a tiny fixed table
 *   of known users, each with a secret password. A short, hard-coded SCRIPT of
 *   login attempts is replayed. For each attempt we hash the typed password and
 *   compare it to the stored hash for that user. We count failures; after three
 *   failures the account is LOCKED and further attempts are refused even if the
 *   password becomes correct. Everything is printed, line by line, so you can
 *   read the whole story top to bottom.
 *
 * WHY THE STYLE IS UNUSUAL (the compiler's rules you must respect):
 *   The on-device cc is a small single-file C subset. The things it CANNOT do
 *   shaped every choice below:
 *     - NO #include / NO #define   : this file is fully self-contained.
 *     - NO usable arrays           : a declared array gives you only 8 bytes and
 *                                    indexing it is broken, so we DO NOT store a
 *                                    "struct User table[N]". Instead the table is
 *                                    expressed as code: a function that maps a
 *                                    user name to that user's stored password
 *                                    hash. This is the table -- just written as a
 *                                    decision tree instead of an array of rows.
 *     - GLOBALS START AT ZERO       : an initializer on a global is ignored, so
 *                                    every global is set at run time, never with
 *                                    "= value" at the declaration.
 *     - STRING LITERALS are fine    : they live in read-only data and you may
 *                                    walk them through a char* with s[i].
 *     - NO stdin is proven for a    : so the login attempts are HARD-CODED below.
 *       terminal-spawned program      A real interactive version would read the
 *                                      user name + password from the keyboard.
 *
 * HOW TO EXTEND IT:
 *   - Add a user: add one more "if (streq(user, \"name\")) return hash(\"pw\");"
 *     line inside pass_hash_for(). That is literally adding a row to the table.
 *   - Change the lockout threshold: edit LOCKOUT_AT in main().
 *   - Make it interactive: replace the scripted attempt(...) calls in main with
 *     reads of real input once a proven stdin path exists.
 *
 * The toolchain provides _start: it calls main() and turns main's return value
 * into the process exit code. We return the number of failed attempts.
 * ==========================================================================*/

/* On-device builtins. The cc recognizes these names as direct syscalls; the
 * prototypes exist ONLY so a host "gcc -fsyntax-only" check passes. The
 * on-device compiler ignores prototypes (it emits only function bodies). */
void sys_write(int fd, char *buf, int len);

/* ---- tiny output layer (one byte at a time through a 1-byte global) -------
 * g_ch is the single mutable byte the writer points the kernel at. Pointing at
 * &g_ch sidesteps the subset's lack of real arrays. */
char g_ch;

/* emit: write one character to standard output (fd 1). */
void emit(int c) { g_ch = c; sys_write(1, &g_ch, 1); }

/* puts0: write a NUL-terminated C string by first measuring its length. */
void puts0(char *s) {
    int n = 0;
    while (s[n]) n = n + 1;
    sys_write(1, s, n);
}

/* putu: print a non-negative integer in decimal using recursion (no buffer). */
void putu(int v) {
    if (v >= 10) putu(v / 10);
    emit('0' + v % 10);
}

/* puti: print a signed integer (handles the leading minus, then the digits). */
void puti(int v) {
    if (v < 0) { emit('-'); v = 0 - v; }
    putu(v);
}

/* ---- string + hash primitives -------------------------------------------- */

/* streq: return 1 if the two NUL-terminated strings are exactly equal, else 0. */
int streq(char *a, char *b) {
    int i = 0;
    while (a[i] && b[i]) {
        if (a[i] != b[i]) return 0;
        i = i + 1;
    }
    return a[i] == b[i];          /* both ended together => equal */
}

/* hash: the classic djb2 string hash, written out so nothing is hidden.
 *   start at 5381; for each byte:  h = h*33 + byte.  (h*33 == (h<<5)+h)
 * Returns a 32-bit-ish integer fingerprint of the string. */
int hash(char *s) {
    int h = 5381;
    int i = 0;
    while (s[i]) {
        h = ((h << 5) + h) + s[i];
        i = i + 1;
    }
    return h;
}

/* ---- the "user table", expressed as a function ---------------------------
 * pass_hash_for: given a user name, return the stored hash of that user's
 * password, or 0 if the user is unknown. Storing the HASH (not the password)
 * is how real systems avoid keeping plaintext secrets. Each "if" line is one
 * row of the table. */
int pass_hash_for(char *user) {
    if (streq(user, "alice")) return hash("wonderland");
    if (streq(user, "bob"))   return hash("builder");
    if (streq(user, "root"))  return hash("toor");
    return 0;                     /* unknown user */
}

/* ---- the failure / lockout counter --------------------------------------- */
int g_fail;                       /* number of failed attempts so far (set to 0 in main) */

/* try_login: return 1 if (user,pass) is a valid pair, else 0. Unknown users
 * always fail. This does NOT touch the lockout counter -- that is policy, kept
 * in attempt() so this function stays a pure check. */
int try_login(char *user, char *pass) {
    int expected = pass_hash_for(user);
    if (expected == 0) return 0;          /* no such user */
    return hash(pass) == expected;        /* compare fingerprints */
}

/* attempt: run ONE scripted login attempt with the lockout policy applied.
 * Prints the outcome. Returns 1 if the user is now logged in, else 0. */
int attempt(char *user, char *pass, int lockout_at) {
    puts0("login: user='");
    puts0(user);
    puts0("' ... ");

    if (g_fail >= lockout_at) {            /* policy: refuse once locked */
        puts0("ACCOUNT LOCKED (too many failures)\n");
        return 0;
    }
    if (try_login(user, pass)) {
        puts0("ACCESS GRANTED -- welcome, ");
        puts0(user);
        emit('\n');
        return 1;
    }
    g_fail = g_fail + 1;                    /* record the failure */
    puts0("ACCESS DENIED (failures=");
    putu(g_fail);
    puts0(")\n");
    return 0;
}

/* main: replay a fixed script of attempts that demonstrates success, repeated
 * failure, and lockout. Returns the number of failed attempts as the exit code.
 *
 * The main sequence, numbered:
 *   1. set the failure counter to 0 (globals start at 0, but we are explicit).
 *   2. a CORRECT login for 'alice'                  -> granted.
 *   3..5. three WRONG passwords for 'bob'           -> denied, denied, locked.
 *   6. the CORRECT password for 'bob' AFTER lockout -> still refused.
 *   7. print a summary line.
 */
int main(void) {
    int lockout_at = 3;

    /* step 1 */
    g_fail = 0;
    puts0("== AutomationOS login demo ==\n");
    puts0("known users: alice, bob, root (lockout after ");
    putu(lockout_at);
    puts0(" failures)\n\n");

    /* step 2 */
    attempt("alice", "wonderland", lockout_at);

    /* steps 3..5 */
    attempt("bob", "hunter2",   lockout_at);
    attempt("bob", "password1", lockout_at);
    attempt("bob", "letmein",   lockout_at);

    /* step 6: correct now, but the account is already locked */
    attempt("bob", "builder",   lockout_at);

    /* step 7 */
    puts0("\nsummary: ");
    putu(g_fail);
    puts0(" failed attempt(s).\n");
    return g_fail;
}
