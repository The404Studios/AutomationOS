/*
 * cryptotest.h -- known-answer self-test entry point.
 * ===================================================
 *
 * Pure computation: no libc, no syscalls, no malloc. Runs a fixed battery of
 * published test vectors against every primitive in this directory and
 * returns 0 only if every check matches. A nonzero return is a distinct
 * positive code identifying the first failing test (see cryptotest.c), so a
 * caller can log which primitive is broken without any I/O dependency.
 */

#ifndef CRYPTO_CRYPTOTEST_H
#define CRYPTO_CRYPTOTEST_H

/* Returns 0 if ALL known-answer tests pass; nonzero (the failing test's id)
 * on the first failure. */
int crypto_selftest(void);

#endif /* CRYPTO_CRYPTOTEST_H */
