/**
 * Symbol Resolution Test Suite
 * =============================
 *
 * Tests symbol lookup in shared objects with various hash tables.
 */

#include "../linker.h"
#include <stdio.h>
#include <assert.h>
#include <string.h>

// Test fixtures
static shared_object_t test_so;
static linker_context_t test_ctx;

// Mock symbol table
static elf64_sym_t test_symbols[] = {
    // Symbol 0: undefined (STN_UNDEF)
    { .st_name = 0, .st_info = 0, .st_shndx = SHN_UNDEF, .st_value = 0, .st_size = 0 },

    // Symbol 1: global function "foo"
    { .st_name = 1, .st_info = ELF64_ST_INFO(STB_GLOBAL, STT_FUNC),
      .st_shndx = 1, .st_value = 0x1000, .st_size = 64 },

    // Symbol 2: global data "bar"
    { .st_name = 5, .st_info = ELF64_ST_INFO(STB_GLOBAL, STT_OBJECT),
      .st_shndx = 2, .st_value = 0x2000, .st_size = 8 },

    // Symbol 3: weak function "baz"
    { .st_name = 9, .st_info = ELF64_ST_INFO(STB_WEAK, STT_FUNC),
      .st_shndx = 1, .st_value = 0x1100, .st_size = 32 },

    // Symbol 4: local function "qux"
    { .st_name = 13, .st_info = ELF64_ST_INFO(STB_LOCAL, STT_FUNC),
      .st_shndx = 1, .st_value = 0x1200, .st_size = 16 },
};

// Mock string table
static const char test_strtab[] =
    "\0"        // 0: empty string
    "foo\0"     // 1: "foo"
    "bar\0"     // 5: "bar"
    "baz\0"     // 9: "baz"
    "qux\0";    // 13: "qux"

// Mock ELF hash table
// Format: [nbucket, nchain, bucket[], chain[]]
static uint32_t test_hash[] = {
    2,  // nbucket
    5,  // nchain (number of symbols)
    // buckets[2]
    1,  // bucket[0] -> symbol 1 (foo)
    2,  // bucket[1] -> symbol 2 (bar)
    // chain[5]
    0,  // chain[0]
    3,  // chain[1] -> symbol 3 (baz)
    4,  // chain[2] -> symbol 4 (qux)
    0,  // chain[3]
    0,  // chain[4]
};

void setup_test_so(void) {
    memset(&test_so, 0, sizeof(test_so));
    memset(&test_ctx, 0, sizeof(test_ctx));

    strcpy(test_so.name, "libtest.so");
    test_so.base_addr = 0x10000;
    test_so.symtab = test_symbols;
    test_so.num_symbols = sizeof(test_symbols) / sizeof(test_symbols[0]);
    test_so.strtab = (char*)test_strtab;
    test_so.strtab_size = sizeof(test_strtab);
    test_so.hash = test_hash;

    linker_init(&test_ctx);
    test_ctx.objects[0] = &test_so;
    test_ctx.num_objects = 1;
}

void test_lookup_global_symbol(void) {
    printf("Testing global symbol lookup...\n");

    setup_test_so();

    // Look up "foo"
    elf64_sym_t* sym = linker_lookup_symbol_in_object(&test_so, "foo");
    assert(sym != NULL);
    assert(sym == &test_symbols[1]);
    assert(ELF64_ST_BIND(sym->st_info) == STB_GLOBAL);

    printf("  ✓ Found global symbol 'foo'\n");
}

void test_lookup_weak_symbol(void) {
    printf("Testing weak symbol lookup...\n");

    setup_test_so();

    // Look up "baz"
    elf64_sym_t* sym = linker_lookup_symbol_in_object(&test_so, "baz");
    assert(sym != NULL);
    assert(sym == &test_symbols[3]);
    assert(linker_symbol_is_weak(sym));

    printf("  ✓ Found weak symbol 'baz'\n");
}

void test_lookup_undefined_symbol(void) {
    printf("Testing undefined symbol lookup...\n");

    setup_test_so();

    // Look up non-existent symbol
    elf64_sym_t* sym = linker_lookup_symbol_in_object(&test_so, "nonexistent");
    assert(sym == NULL);

    printf("  ✓ Correctly returned NULL for undefined symbol\n");
}

void test_get_symbol_addr(void) {
    printf("Testing symbol address calculation...\n");

    setup_test_so();

    elf64_sym_t* sym = linker_lookup_symbol_in_object(&test_so, "foo");
    assert(sym != NULL);

    uint64_t addr = linker_get_symbol_addr(&test_so, sym);
    assert(addr == 0x10000 + 0x1000);  // base + value

    printf("  ✓ Symbol address: 0x%lx\n", addr);
}

void test_global_lookup(void) {
    printf("Testing global symbol lookup...\n");

    setup_test_so();
    test_ctx.main_object = &test_so;

    symbol_info_t* info = linker_lookup_symbol_global(&test_ctx, "foo", 0);
    assert(info != NULL);
    assert(info->object == &test_so);
    assert(info->symbol == &test_symbols[1]);

    printf("  ✓ Global lookup found symbol in main object\n");
}

int main(void) {
    printf("Symbol Resolution Test Suite\n");
    printf("=============================\n\n");

    test_lookup_global_symbol();
    test_lookup_weak_symbol();
    test_lookup_undefined_symbol();
    test_get_symbol_addr();
    test_global_lookup();

    printf("\n✅ All tests passed!\n");
    return 0;
}
