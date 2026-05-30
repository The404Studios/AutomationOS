#ifndef INIT_USERMODE_H
#define INIT_USERMODE_H

/*
 * User Mode Initialization
 * ========================
 *
 * Functions for initializing and testing user mode support.
 */

// Initialize user mode support (TSS, etc.)
void init_usermode_support(void);

// Test user mode by switching to a test program (never returns)
void test_usermode_switch(void);

#endif
