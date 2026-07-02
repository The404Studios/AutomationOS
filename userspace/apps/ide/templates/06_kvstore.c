/* ============================================================================
 * 06_kvstore.c -- a fixed-slot key/value store with set / get / has / delete,
 *                 plus a self-test that prints PASS / FAIL lines. For the
 *                 AutomationOS on-device C compiler. This template teaches the
 *                 house "prove it with a self-test" style.
 *
 * WHAT THIS PROGRAM DOES (in words):
 *   It is a tiny dictionary that maps an integer KEY to an integer VALUE, with
 *   room for exactly FOUR entries. You can:
 *       set(key, value)  -- insert a new pair, or overwrite an existing key
 *       get(key)         -- fetch the value for a key (0 if absent)
 *       has(key)         -- ask whether a key is present (1 or 0)
 *       del(key)         -- remove a key
 *   After defining the store, a SELF-TEST runs a sequence of operations whose
 *   answers are known in advance, and prints "PASS <name>" or "FAIL <name>" for
 *   each check. The program's exit code is the number of FAILED checks, so a
 *   clean run exits with 0 -- the same convention the OS test suite uses.
 *
 * HOW THE STORE IS BUILT WITHOUT ARRAYS:
 *   The on-device cc has no usable arrays, so the four slots are eight plain
 *   integer globals -- k0..k3 for the keys and v0..v3 for the values -- plus one
 *   integer g_used whose bit i means "slot i currently holds a pair". Because we
 *   cannot write "slots[i]", we reach a slot through ACCESSOR functions that map
 *   an index 0..3 to the right global with a small if/else ladder:
 *       get_key(i), set_key(i, x), get_val(i), set_val(i, x)
 *   Everything else (find, set, get, has, del) is written once, in terms of
 *   those accessors, so the four-slot limit lives in exactly one place. This is
 *   the standard way to express an indexed table in this compiler's scalar-only
 *   world; to grow the store you extend the ladders and the slot count together.
 *
 * COMPILER RULES THAT SHAPED THIS FILE:
 *   - NO #include / #define; self-contained, integers only.
 *   - GLOBALS START AT ZERO and initializers are ignored -> g_used and the slots
 *     begin empty automatically, and the self-test sets values at run time.
 *   - NO switch statement in the subset -> the accessors use if/else ladders.
 *   - NO stdin needed: the self-test is a fixed script.
 *
 * HOW TO EXTEND IT:
 *   - More slots: add k4/v4..., extend each accessor ladder by one line, and
 *     raise the "< 4" bounds in find()/first_free() to the new count.
 *   - String keys: store a char* per slot instead of an int and compare with a
 *     string-equality helper (see 01_login.c's streq).
 *
 * Exit code = number of failed self-test checks (0 means all passed).
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

/* ---- the four slots, as plain scalar globals ----------------------------- */
int k0; int k1; int k2; int k3;     /* the keys   (start at 0) */
int v0; int v1; int v2; int v3;     /* the values (start at 0) */
int g_used;                          /* bit i set => slot i holds a pair */

/* get_key: return the key stored in slot i (0..3). */
int get_key(int i) {
    if (i == 0) return k0;
    if (i == 1) return k1;
    if (i == 2) return k2;
    return k3;
}
/* set_key: store key x into slot i (0..3). */
void set_key(int i, int x) {
    if (i == 0) k0 = x;
    else if (i == 1) k1 = x;
    else if (i == 2) k2 = x;
    else k3 = x;
}
/* get_val: return the value stored in slot i (0..3). */
int get_val(int i) {
    if (i == 0) return v0;
    if (i == 1) return v1;
    if (i == 2) return v2;
    return v3;
}
/* set_val: store value x into slot i (0..3). */
void set_val(int i, int x) {
    if (i == 0) v0 = x;
    else if (i == 1) v1 = x;
    else if (i == 2) v2 = x;
    else v3 = x;
}

/* slot_used: 1 if slot i currently holds a pair, else 0. */
int slot_used(int i) { return (g_used >> i) & 1; }

/* find: return the slot index holding `key`, or -1 if the key is not present. */
int find(int key) {
    int i = 0;
    while (i < 4) {
        if (slot_used(i) && get_key(i) == key) return i;
        i = i + 1;
    }
    return -1;
}

/* first_free: return the index of the first empty slot, or -1 if the store is full. */
int first_free(void) {
    int i = 0;
    while (i < 4) {
        if (!slot_used(i)) return i;
        i = i + 1;
    }
    return -1;
}

/* kv_set: insert or overwrite (key -> val). Returns 1 on success, 0 if full. */
int kv_set(int key, int val) {
    int i = find(key);             /* overwrite if the key already exists */
    if (i < 0) i = first_free();   /* otherwise take an empty slot */
    if (i < 0) return 0;           /* store is full */
    set_key(i, key);
    set_val(i, val);
    g_used = g_used | (1 << i);    /* mark the slot occupied */
    return 1;
}

/* kv_has: 1 if `key` is present, else 0. */
int kv_has(int key) { return find(key) >= 0; }

/* kv_get: the value for `key`, or 0 if the key is absent (use kv_has to tell
 * a real stored 0 from a missing key). */
int kv_get(int key) {
    int i = find(key);
    if (i < 0) return 0;
    return get_val(i);
}

/* kv_del: remove `key` if present (clearing its occupied bit frees the slot). */
void kv_del(int key) {
    int i = find(key);
    if (i >= 0) g_used = g_used & (~(1 << i));
}

/* ---- the self-test -------------------------------------------------------- */
int g_fails;      /* number of failed checks (start at 0) */

/* check: print "PASS name" if cond is true, else "FAIL name" and count it.
 *   Contract: cond is the already-computed truth of the thing being asserted. */
void check(int cond, char *name) {
    if (cond) {
        puts0("  PASS  ");
    } else {
        puts0("  FAIL  ");
        g_fails = g_fails + 1;
    }
    puts0(name);
    emit('\n');
}

/* main: exercise the store against known-good answers and report.
 *
 * The script, numbered:
 *   1. start empty: nothing is present.
 *   2. set three pairs; confirm get/has for each.
 *   3. overwrite an existing key; confirm the new value replaced the old.
 *   4. delete a key; confirm it is gone but the others remain.
 *   5. fill the store and prove a fifth distinct key is rejected (full).
 *   6. print a summary and return the failure count.
 */
int main(void) {
    /* step 1 */
    g_used  = 0;
    g_fails = 0;
    puts0("== AutomationOS key/value store self-test ==\n\n");
    check(kv_has(10) == 0, "empty store has no key 10");

    /* step 2 */
    kv_set(10, 100);
    kv_set(20, 200);
    kv_set(30, 300);
    check(kv_get(10) == 100, "get(10) == 100");
    check(kv_get(20) == 200, "get(20) == 200");
    check(kv_has(30) == 1,   "has(30) is true");
    check(kv_has(99) == 0,   "has(99) is false");

    /* step 3 */
    kv_set(20, 222);
    check(kv_get(20) == 222, "overwrite: get(20) == 222");

    /* step 4 */
    kv_del(10);
    check(kv_has(10) == 0,   "after delete: has(10) is false");
    check(kv_get(30) == 300, "delete left get(30) == 300");

    /* step 5 -- three slots are full (20,30 plus two fresh), prove rejection */
    kv_set(40, 400);          /* now slots hold 20,30,40 ... and one freed by del(10) */
    kv_set(50, 500);          /* fills the fourth slot: 20,30,40,50 */
    check(kv_set(60, 600) == 0, "full store rejects a 5th key");
    check(kv_has(60) == 0,      "rejected key 60 is absent");

    /* step 6 */
    puts0("\nresult: ");
    if (g_fails == 0) puts0("ALL CHECKS PASSED");
    else { puts0("FAILURES = "); puti(g_fails); }
    emit('\n');
    return g_fails;
}
