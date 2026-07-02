/* ============================================================================
 * 03_guess.c -- a number-guessing game against a deterministic pseudo-random
 *               number generator, played automatically by a self-play driver,
 *               for the AutomationOS on-device C compiler.
 *
 * WHAT THIS PROGRAM DOES (in words):
 *   The computer "thinks of" a secret number from 0 to 99. Then a SELF-PLAYER
 *   tries to find it using binary search: it keeps a low bound and a high bound,
 *   always guesses the middle, and after each guess it learns "too low" or
 *   "too high" and shrinks the range by half. Every guess and hint is printed,
 *   and the search is guaranteed to finish in at most 7 guesses (because
 *   halving 100 possibilities reaches 1 in 7 steps).
 *
 * WHERE THE SECRET COMES FROM -- A LINEAR CONGRUENTIAL GENERATOR (LCG):
 *   There is no hardware randomness here and we want the run to be repeatable,
 *   so we use an LCG, the simplest classic pseudo-random generator, written out
 *   in full:
 *       state = state * 1103515245 + 12345        (the glibc constants)
 *       output = (state >> 16) & 32767            (take some high bits, keep them positive)
 *   Feeding the same SEED always produces the same sequence -- that determinism
 *   is a feature here, so the demo behaves the same every time. The secret is
 *   simply the first output, taken modulo 100.
 *
 * COMPILER RULES THAT SHAPED THIS FILE:
 *   - NO #include / #define; self-contained, integers only (the cc has no FPU).
 *   - GLOBALS START AT ZERO and initializers are ignored -> the LCG state is
 *     seeded at run time inside main, never with "= seed" at the declaration.
 *   - NO stdin needed: this is a zero-player game; the driver plays itself.
 *
 * HOW TO EXTEND IT:
 *   - Change the secret: edit the SEED in main, or the "% 100" range.
 *   - Make it interactive: replace the self-play loop's computed guess with a
 *     real keyboard read once a proven stdin path exists; the hint logic is the
 *     same.
 *
 * Exit code = the number of guesses the self-player needed.
 * ==========================================================================*/

/* On-device builtin (prototype only for the host syntax check; see 01_login.c). */
void sys_write(int fd, char *buf, int len);

/* ---- output layer --------------------------------------------------------- */
char g_ch;
/* emit: write a single character to stdout. */
void emit(int c) { g_ch = c; sys_write(1, &g_ch, 1); }
/* puts0: write a NUL-terminated string. */
void puts0(char *s) { int n = 0; while (s[n]) n = n + 1; sys_write(1, s, n); }
/* putu: print a non-negative integer in decimal (recursive). */
void putu(int v) { if (v >= 10) putu(v / 10); emit('0' + v % 10); }

/* ---- the pseudo-random generator ----------------------------------------- */
int g_rng;        /* the LCG state (seeded in main; globals start at 0) */

/* rng_next: advance the LCG and return a non-negative pseudo-random int.
 *   Contract: deterministic for a given starting g_rng; result is 0..32767. */
int rng_next(void) {
    g_rng = g_rng * 1103515245 + 12345;   /* the LCG recurrence (wraps; fine) */
    return (g_rng >> 16) & 32767;          /* high bits, masked positive */
}

/* pick_secret: choose the secret number in [0, 99] from the next LCG output. */
int pick_secret(void) {
    return rng_next() % 100;
}

/* main: seed the generator, pick a secret, and let the binary-search self-player
 * find it. Returns the guess count.
 *
 * The self-play loop, numbered:
 *   1. seed the LCG and choose the secret (kept hidden from the search logic).
 *   2. set lo = 0, hi = 99, tries = 0.
 *   3. while lo <= hi and we are under the safety cap:
 *        a. guess = the midpoint (lo + hi) / 2; count the try; print it.
 *        b. if guess == secret  -> announce success and stop.
 *        c. if guess <  secret  -> print "too low",  raise lo to guess + 1.
 *        d. else                -> print "too high", lower hi to guess - 1.
 *   4. print how many guesses it took.
 */
int main(void) {
    int lo;
    int hi;
    int tries;
    int secret;
    int guess;

    /* step 1 */
    g_rng = 20260610;                 /* the SEED: change it for a different secret */
    secret = pick_secret();
    puts0("== AutomationOS guessing game (self-play) ==\n");
    puts0("the computer picked a secret in 0..99; binary search will find it.\n\n");

    /* step 2 */
    lo = 0;
    hi = 99;
    tries = 0;

    /* step 3 */
    while (lo <= hi && tries < 16) {
        guess = (lo + hi) / 2;        /* a */
        tries = tries + 1;
        puts0("guess #");
        putu(tries);
        puts0(" -> ");
        putu(guess);
        puts0(" : ");

        if (guess == secret) {        /* b */
            puts0("correct!\n");
            lo = hi + 1;              /* force the loop to end */
        } else if (guess < secret) {  /* c */
            puts0("too low\n");
            lo = guess + 1;
        } else {                      /* d */
            puts0("too high\n");
            hi = guess - 1;
        }
    }

    /* step 4 */
    puts0("\nfound ");
    putu(secret);
    puts0(" in ");
    putu(tries);
    puts0(" guess(es).\n");
    return tries;
}
