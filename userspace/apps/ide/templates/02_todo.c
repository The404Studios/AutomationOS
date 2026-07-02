/* ============================================================================
 * 02_todo.c -- an in-memory to-do list driven by a scripted command sequence,
 *              for the AutomationOS on-device C compiler.
 *
 * WHAT THIS PROGRAM DOES (in words):
 *   There is a fixed CATALOG of eight possible task titles (slot 0..7). The list
 *   has two pieces of state, and that is ALL the state there is:
 *     - which slots are currently ON the list   (the "active" set)
 *     - which slots are marked DONE             (the "done" set)
 *   A short, hard-coded script of commands -- add, done, list -- is replayed,
 *   and after the interesting commands we print the whole list so you can watch
 *   it change.
 *
 * THE KEY IDEA -- A SET STORED AS THE BITS OF ONE INTEGER:
 *   The on-device cc has no usable arrays, so we cannot keep an array of task
 *   structs. Instead each task is a SLOT NUMBER 0..7, and a set of slots is
 *   stored as the bits of a single int:
 *       bit i == 1  means  "slot i is in the set".
 *   Adding slot i to a set:        set = set | (1 << i)
 *   Testing whether i is in a set: (set >> i) & 1
 *   This is the same trick a real kernel uses for small fixed collections, and
 *   it fits the compiler's scalar-only world perfectly. g_active holds the
 *   "on the list" set; g_done holds the "completed" set. The titles themselves
 *   are string literals returned by title(i) -- read-only, which is why we can
 *   keep them even without writable arrays.
 *
 * COMPILER RULES THAT SHAPED THIS FILE:
 *   - NO #include / #define; self-contained.
 *   - GLOBALS START AT ZERO and initializers are ignored -> we set g_active and
 *     g_done to 0 explicitly in main.
 *   - NO stdin is proven for a terminal-spawned program -> the commands are a
 *     fixed script. A real version would parse typed commands instead.
 *
 * HOW TO EXTEND IT:
 *   - More tasks: extend title() with more "if (i == N) return \"...\";" lines
 *     (you have room up to bit 30 in an int; keep titles read-only literals).
 *   - New command: write a function that edits g_active / g_done with the bit
 *     operations above, then call it from main's script.
 *
 * Exit code = number of completed tasks at the end.
 * ==========================================================================*/

/* On-device builtin (prototype only for the host syntax check; see 01_login.c). */
void sys_write(int fd, char *buf, int len);

/* ---- output layer --------------------------------------------------------- */
char g_ch;
/* emit: write a single character to stdout. */
void emit(int c) { g_ch = c; sys_write(1, &g_ch, 1); }
/* puts0: write a NUL-terminated string. */
void puts0(char *s) { int n = 0; while (s[n]) n = n + 1; sys_write(1, s, n); }
/* putu: print a non-negative integer in decimal (recursive, no buffer). */
void putu(int v) { if (v >= 10) putu(v / 10); emit('0' + v % 10); }

/* ---- the fixed title catalog --------------------------------------------
 * title: return the read-only title text for slot i (0..7), or a placeholder. */
char *title(int i) {
    if (i == 0) return "water the plants";
    if (i == 1) return "write the design doc";
    if (i == 2) return "review pull request";
    if (i == 3) return "back up the disk image";
    if (i == 4) return "answer email";
    if (i == 5) return "refactor the parser";
    if (i == 6) return "call the dentist";
    if (i == 7) return "ship the release";
    return "(empty slot)";
}

/* ---- the two sets, each stored as the bits of one int -------------------- */
int g_active;     /* bit i set => task i is on the list   (set to 0 in main) */
int g_done;       /* bit i set => task i is completed      (set to 0 in main) */

/* is_active: 1 if slot i is currently on the list, else 0. */
int is_active(int i) { return (g_active >> i) & 1; }
/* is_done: 1 if slot i is marked completed, else 0. */
int is_done(int i)   { return (g_done >> i) & 1; }

/* add_task: put slot i on the list (idempotent). Prints what it did. */
void add_task(int i) {
    g_active = g_active | (1 << i);
    puts0("  + added: ");
    puts0(title(i));
    emit('\n');
}

/* complete_task: mark slot i done IF it is on the list. Prints the outcome. */
void complete_task(int i) {
    if (!is_active(i)) {
        puts0("  ! cannot complete slot ");
        putu(i);
        puts0(" -- not on the list\n");
        return;
    }
    g_done = g_done | (1 << i);
    puts0("  * done: ");
    puts0(title(i));
    emit('\n');
}

/* count_done: return how many slots are both active and done. */
int count_done(void) {
    int i = 0;
    int n = 0;
    while (i < 8) {
        if (is_active(i) && is_done(i)) n = n + 1;
        i = i + 1;
    }
    return n;
}

/* list_tasks: print every active task with a [x] (done) or [ ] (todo) box. */
void list_tasks(void) {
    int i = 0;
    int shown = 0;
    puts0("  --- current list ---\n");
    while (i < 8) {
        if (is_active(i)) {
            if (is_done(i)) puts0("  [x] ");
            else            puts0("  [ ] ");
            puts0(title(i));
            emit('\n');
            shown = shown + 1;
        }
        i = i + 1;
    }
    if (shown == 0) puts0("  (the list is empty)\n");
}

/* main: replay a fixed script of commands and watch the list evolve.
 * The sequence, numbered:
 *   1. zero both sets (globals start at 0; we are explicit for clarity).
 *   2. add tasks 0, 1, 2.
 *   3. list.
 *   4. complete task 1.
 *   5. try to complete task 5 (not on the list -> refused).
 *   6. add task 3; complete task 0.
 *   7. final list + a count summary.
 */
int main(void) {
    /* step 1 */
    g_active = 0;
    g_done   = 0;
    puts0("== AutomationOS to-do demo ==\n\n");

    /* step 2 */
    puts0("command: add 0,1,2\n");
    add_task(0);
    add_task(1);
    add_task(2);

    /* step 3 */
    puts0("command: list\n");
    list_tasks();

    /* step 4 */
    puts0("command: done 1\n");
    complete_task(1);

    /* step 5 */
    puts0("command: done 5\n");
    complete_task(5);

    /* step 6 */
    puts0("command: add 3; done 0\n");
    add_task(3);
    complete_task(0);

    /* step 7 */
    puts0("command: list\n");
    list_tasks();
    puts0("\nsummary: ");
    putu(count_done());
    puts0(" task(s) completed.\n");
    return count_done();
}
