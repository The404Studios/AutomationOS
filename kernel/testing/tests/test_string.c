#include "../../include/ktest.h"
#include "../../include/kernel.h"

/*
 * String Utility Tests
 * Tests kernel string manipulation functions
 */

KTEST_SUITE(string);

KTEST_CASE(string, strlen_empty_string) {
    const char* str = "";
    size_t len = kstrlen(str);
    KTEST_ASSERT_EQ(len, 0);
}

KTEST_CASE(string, strlen_normal_string) {
    const char* str = "hello";
    size_t len = kstrlen(str);
    KTEST_ASSERT_EQ(len, 5);
}

KTEST_CASE(string, strlen_long_string) {
    const char* str = "this is a longer string for testing";
    size_t len = kstrlen(str);
    KTEST_ASSERT_EQ(len, 36);
}

KTEST_CASE(string, strcmp_equal_strings) {
    const char* s1 = "hello";
    const char* s2 = "hello";
    int result = kstrcmp(s1, s2);
    KTEST_ASSERT_EQ(result, 0);
}

KTEST_CASE(string, strcmp_different_strings) {
    const char* s1 = "hello";
    const char* s2 = "world";
    int result = kstrcmp(s1, s2);
    KTEST_ASSERT_NE(result, 0);
}

KTEST_CASE(string, strcmp_prefix) {
    const char* s1 = "hello";
    const char* s2 = "hello world";
    int result = kstrcmp(s1, s2);
    KTEST_ASSERT_NE(result, 0);
}

KTEST_CASE(string, memcmp_equal_memory) {
    uint8_t buf1[] = {1, 2, 3, 4, 5};
    uint8_t buf2[] = {1, 2, 3, 4, 5};
    int result = kmemcmp(buf1, buf2, 5);
    KTEST_ASSERT_EQ(result, 0);
}

KTEST_CASE(string, memcmp_different_memory) {
    uint8_t buf1[] = {1, 2, 3, 4, 5};
    uint8_t buf2[] = {1, 2, 9, 4, 5};
    int result = kmemcmp(buf1, buf2, 5);
    KTEST_ASSERT_NE(result, 0);
}

KTEST_CASE(string, memcmp_partial_equal) {
    uint8_t buf1[] = {1, 2, 3, 4, 5};
    uint8_t buf2[] = {1, 2, 3, 9, 9};
    // First 3 bytes are equal
    int result = kmemcmp(buf1, buf2, 3);
    KTEST_ASSERT_EQ(result, 0);
}

KTEST_CASE(string, memcmp_zero_length) {
    uint8_t buf1[] = {1, 2, 3};
    uint8_t buf2[] = {9, 8, 7};
    // Zero length comparison should be equal
    int result = kmemcmp(buf1, buf2, 0);
    KTEST_ASSERT_EQ(result, 0);
}

KTEST_CASE(string, memset_fills_memory) {
    uint8_t buffer[16];
    kmemset(buffer, 0xAA, sizeof(buffer));

    for (size_t i = 0; i < sizeof(buffer); i++) {
        KTEST_ASSERT_EQ(buffer[i], 0xAA);
    }
}

KTEST_CASE(string, memset_zero) {
    uint8_t buffer[16];
    // Fill with non-zero first
    kmemset(buffer, 0xFF, sizeof(buffer));
    // Then zero
    kmemset(buffer, 0, sizeof(buffer));

    for (size_t i = 0; i < sizeof(buffer); i++) {
        KTEST_ASSERT_EQ(buffer[i], 0);
    }
}

KTEST_CASE(string, memcpy_copies_memory) {
    uint8_t src[] = {1, 2, 3, 4, 5, 6, 7, 8};
    uint8_t dst[8] = {0};

    kmemcpy(dst, src, 8);

    for (int i = 0; i < 8; i++) {
        KTEST_ASSERT_EQ(dst[i], src[i]);
    }
}

KTEST_CASE(string, memcpy_partial) {
    uint8_t src[] = {1, 2, 3, 4, 5, 6, 7, 8};
    uint8_t dst[8] = {0};

    kmemcpy(dst, src, 4);  // Copy only first 4 bytes

    KTEST_ASSERT_EQ(dst[0], 1);
    KTEST_ASSERT_EQ(dst[1], 2);
    KTEST_ASSERT_EQ(dst[2], 3);
    KTEST_ASSERT_EQ(dst[3], 4);
    KTEST_ASSERT_EQ(dst[4], 0);  // Rest should be zero
}

KTEST_CASE(string, memmove_non_overlapping) {
    uint8_t buffer[16];
    for (int i = 0; i < 8; i++) buffer[i] = i;

    kmemmove(buffer + 8, buffer, 8);

    for (int i = 0; i < 8; i++) {
        KTEST_ASSERT_EQ(buffer[i + 8], i);
    }
}

KTEST_CASE(string, memmove_overlapping_forward) {
    uint8_t buffer[16] = {1, 2, 3, 4, 5, 6, 7, 8};

    kmemmove(buffer + 2, buffer, 6);

    KTEST_ASSERT_EQ(buffer[2], 1);
    KTEST_ASSERT_EQ(buffer[3], 2);
    KTEST_ASSERT_EQ(buffer[4], 3);
}

KTEST_CASE(string, memmove_overlapping_backward) {
    uint8_t buffer[16] = {0, 0, 1, 2, 3, 4, 5, 6};

    kmemmove(buffer, buffer + 2, 6);

    KTEST_ASSERT_EQ(buffer[0], 1);
    KTEST_ASSERT_EQ(buffer[1], 2);
    KTEST_ASSERT_EQ(buffer[2], 3);
}

KTEST_CASE(string, strcpy_copies_string) {
    char src[] = "hello";
    char dst[16] = {0};

    kstrcpy(dst, src);

    KTEST_ASSERT_STR_EQ(dst, src);
}

KTEST_CASE(string, strncpy_copies_n_chars) {
    char src[] = "hello world";
    char dst[16] = {0};

    kstrncpy(dst, src, 5);

    KTEST_ASSERT_EQ(dst[0], 'h');
    KTEST_ASSERT_EQ(dst[1], 'e');
    KTEST_ASSERT_EQ(dst[2], 'l');
    KTEST_ASSERT_EQ(dst[3], 'l');
    KTEST_ASSERT_EQ(dst[4], 'o');
}

KTEST_CASE(string, strcat_concatenates) {
    char dst[32] = "hello";
    char src[] = " world";

    kstrcat(dst, src);

    KTEST_ASSERT_STR_EQ(dst, "hello world");
}

KTEST_CASE(string, strchr_finds_character) {
    char str[] = "hello world";

    char* found = kstrchr(str, 'w');
    KTEST_ASSERT_NOT_NULL(found);
    KTEST_ASSERT_EQ(*found, 'w');
}

KTEST_CASE(string, strchr_not_found) {
    char str[] = "hello world";

    char* found = kstrchr(str, 'x');
    KTEST_ASSERT_NULL(found);
}

KTEST_CASE(string, strstr_finds_substring) {
    char str[] = "hello world";
    char substr[] = "world";

    char* found = kstrstr(str, substr);
    KTEST_ASSERT_NOT_NULL(found);
    KTEST_ASSERT_STR_EQ(found, "world");
}

KTEST_CASE(string, strstr_not_found) {
    char str[] = "hello world";
    char substr[] = "xyz";

    char* found = kstrstr(str, substr);
    KTEST_ASSERT_NULL(found);
}

KTEST_CASE(string, atoi_converts_positive) {
    const char* str = "12345";
    int value = katoi(str);
    KTEST_ASSERT_EQ(value, 12345);
}

KTEST_CASE(string, atoi_converts_negative) {
    const char* str = "-678";
    int value = katoi(str);
    KTEST_ASSERT_EQ(value, -678);
}

KTEST_CASE(string, atoi_handles_zero) {
    const char* str = "0";
    int value = katoi(str);
    KTEST_ASSERT_EQ(value, 0);
}

KTEST_CASE(string, itoa_converts_positive) {
    char buffer[32];
    kitoa(12345, buffer, 10);
    KTEST_ASSERT_STR_EQ(buffer, "12345");
}

KTEST_CASE(string, itoa_converts_negative) {
    char buffer[32];
    kitoa(-678, buffer, 10);
    KTEST_ASSERT_STR_EQ(buffer, "-678");
}

KTEST_CASE(string, itoa_converts_hex) {
    char buffer[32];
    kitoa(255, buffer, 16);
    KTEST_ASSERT_STR_EQ(buffer, "FF");
}

KTEST_CASE(string, itoa_converts_binary) {
    char buffer[64];
    kitoa(5, buffer, 2);
    KTEST_ASSERT_STR_EQ(buffer, "101");
}
