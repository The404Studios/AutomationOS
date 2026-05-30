#ifndef ELF_LOADER_TEST_H
#define ELF_LOADER_TEST_H

/**
 * ELF Loader Test Suite
 * ======================
 *
 * Tests ELF loading and usermode transition.
 * Call after heap, VMM, GDT, TSS are initialized.
 */

/**
 * Run ELF loader test suite
 *
 * @param test_number Test to run:
 *   0 = Run all safe tests (no ring 3 transition)
 *   1 = Test heap working
 *   2 = Test ELF header validation
 *   3 = Test initrd file access
 *   4 = Test GDT/TSS setup
 *   5 = Test ELF load (dry run, no execute)
 *   6 = Test ELF load and execute (enters ring 3, does not return)
 */
void elf_loader_test_suite(int test_number);

#endif // ELF_LOADER_TEST_H
