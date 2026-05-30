// test_stdio_buffering.c - Unit tests for stdio buffering
//
// Verifies that the buffering implementation works correctly.

#include "../libc/stdio.h"
#include "../libc/stdlib.h"
#include "../libc/string.h"
#include "../libc/syscall.h"

#define TEST_FILE "/tmp/test_buffer.txt"

int tests_passed = 0;
int tests_failed = 0;

void test_result(const char* test_name, int passed) {
    if (passed) {
        printf("  [PASS] %s\n", test_name);
        tests_passed++;
    } else {
        printf("  [FAIL] %s\n", test_name);
        tests_failed++;
    }
}

// Test 1: Basic fwrite with buffer
void test_basic_buffering(void) {
    printf("\nTest 1: Basic fwrite with buffering\n");

    FILE* f = fopen(TEST_FILE, "w");
    if (!f) {
        test_result("fopen", 0);
        return;
    }
    test_result("fopen", 1);

    // Write 100 bytes
    char data[100];
    memset(data, 'A', 100);
    size_t written = fwrite(data, 1, 100, f);

    test_result("fwrite returns correct count", written == 100);

    fclose(f);

    // Read back and verify
    f = fopen(TEST_FILE, "r");
    char readback[100];
    size_t nread = fread(readback, 1, 100, f);
    fclose(f);

    test_result("fread returns correct count", nread == 100);
    test_result("data integrity", memcmp(data, readback, 100) == 0);
}

// Test 2: Buffer flush on full
void test_buffer_flush_on_full(void) {
    printf("\nTest 2: Buffer flush when full (8KB)\n");

    FILE* f = fopen(TEST_FILE, "w");
    if (!f) {
        test_result("fopen", 0);
        return;
    }

    // Write exactly BUFSIZ bytes
    char* data = (char*)malloc(BUFSIZ);
    memset(data, 'B', BUFSIZ);

    size_t written = fwrite(data, 1, BUFSIZ, f);
    test_result("fwrite BUFSIZ bytes", written == BUFSIZ);

    // Write one more byte - should trigger flush and new buffer
    char extra = 'X';
    written = fwrite(&extra, 1, 1, f);
    test_result("fwrite additional byte", written == 1);

    fclose(f);

    // Verify file size
    f = fopen(TEST_FILE, "r");
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fclose(f);

    test_result("file size correct", size == BUFSIZ + 1);

    free(data);
}

// Test 3: Explicit fflush
void test_explicit_flush(void) {
    printf("\nTest 3: Explicit fflush\n");

    FILE* f = fopen(TEST_FILE, "w");
    if (!f) {
        test_result("fopen", 0);
        return;
    }

    // Write small amount
    fputs("Hello", f);

    // Flush explicitly
    int result = fflush(f);
    test_result("fflush returns success", result == 0);

    // Close without writing more
    fclose(f);

    // Read back
    f = fopen(TEST_FILE, "r");
    char buf[10];
    fgets(buf, 10, f);
    fclose(f);

    test_result("flushed data readable", strcmp(buf, "Hello") == 0);
}

// Test 4: Line buffering
void test_line_buffering(void) {
    printf("\nTest 4: Line buffering\n");

    FILE* f = fopen(TEST_FILE, "w");
    if (!f) {
        test_result("fopen", 0);
        return;
    }

    // Enable line buffering
    setvbuf(f, NULL, _IOLBF, BUFSIZ);

    // Write without newline - should stay in buffer
    fputs("Line1", f);

    // Write with newline - should flush
    fputs("\n", f);

    fclose(f);

    // Read back
    f = fopen(TEST_FILE, "r");
    char buf[20];
    fgets(buf, 20, f);
    fclose(f);

    test_result("line buffered flush", strcmp(buf, "Line1\n") == 0);
}

// Test 5: Unbuffered mode
void test_unbuffered(void) {
    printf("\nTest 5: Unbuffered mode\n");

    FILE* f = fopen(TEST_FILE, "w");
    if (!f) {
        test_result("fopen", 0);
        return;
    }

    // Disable buffering
    setvbuf(f, NULL, _IONBF, 0);

    // Write should go directly
    fputs("Unbuffered", f);

    fclose(f);

    // Read back
    f = fopen(TEST_FILE, "r");
    char buf[20];
    fgets(buf, 20, f);
    fclose(f);

    test_result("unbuffered write", strcmp(buf, "Unbuffered") == 0);
}

// Test 6: fputc accumulation
void test_fputc_buffering(void) {
    printf("\nTest 6: fputc buffering (many small writes)\n");

    FILE* f = fopen(TEST_FILE, "w");
    if (!f) {
        test_result("fopen", 0);
        return;
    }

    // Write 1000 single characters
    for (int i = 0; i < 1000; i++) {
        fputc('C', f);
    }

    fclose(f);

    // Read back and verify
    f = fopen(TEST_FILE, "r");
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fclose(f);

    test_result("1000 fputc calls", size == 1000);
}

// Test 7: fprintf buffering
void test_fprintf_buffering(void) {
    printf("\nTest 7: fprintf buffering\n");

    FILE* f = fopen(TEST_FILE, "w");
    if (!f) {
        test_result("fopen", 0);
        return;
    }

    // Multiple fprintf calls
    fprintf(f, "Number: %d\n", 42);
    fprintf(f, "String: %s\n", "test");
    fprintf(f, "Hex: 0x%x\n", 255);

    fclose(f);

    // Read back
    f = fopen(TEST_FILE, "r");
    char buf[100];
    fgets(buf, 100, f);
    fclose(f);

    test_result("fprintf buffered correctly", strncmp(buf, "Number: 42", 10) == 0);
}

// Test 8: Read buffering
void test_read_buffering(void) {
    printf("\nTest 8: Read buffering\n");

    // Create test file
    FILE* f = fopen(TEST_FILE, "w");
    for (int i = 0; i < 100; i++) {
        fprintf(f, "Line %d\n", i);
    }
    fclose(f);

    // Read with buffering
    f = fopen(TEST_FILE, "r");
    char buf[20];
    int count = 0;

    while (fgets(buf, 20, f) != NULL) {
        count++;
    }

    fclose(f);

    test_result("read buffering (100 lines)", count == 100);
}

int main(void) {
    printf("\n");
    printf("╔════════════════════════════════════════════════════════════╗\n");
    printf("║       AutomationOS Stdio Buffering Unit Tests             ║\n");
    printf("╚════════════════════════════════════════════════════════════╝\n");

    test_basic_buffering();
    test_buffer_flush_on_full();
    test_explicit_flush();
    test_line_buffering();
    test_unbuffered();
    test_fputc_buffering();
    test_fprintf_buffering();
    test_read_buffering();

    printf("\n");
    printf("────────────────────────────────────────────────────────────\n");
    printf("Test Results:\n");
    printf("  Passed: %d\n", tests_passed);
    printf("  Failed: %d\n", tests_failed);
    printf("  Total:  %d\n", tests_passed + tests_failed);
    printf("────────────────────────────────────────────────────────────\n");
    printf("\n");

    if (tests_failed == 0) {
        printf("✓ All tests passed!\n\n");
        return 0;
    } else {
        printf("✗ Some tests failed.\n\n");
        return 1;
    }
}
