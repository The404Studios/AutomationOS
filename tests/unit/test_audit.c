/*
 * Unit tests for audit logging subsystem
 */

#include "../../kernel/include/audit.h"
#include "../../kernel/include/kernel.h"
#include "../../kernel/include/mem.h"

// External functions from audit implementation
extern audit_buffer_t* audit_buffer_create(void);
extern void audit_buffer_destroy(audit_buffer_t* buffer);
extern int audit_buffer_write(audit_buffer_t* buffer, audit_event_t* event);
extern int audit_buffer_read(audit_buffer_t* buffer, audit_event_t* event);
extern uint32_t audit_buffer_count(audit_buffer_t* buffer);
extern bool audit_buffer_verify_integrity(audit_buffer_t* buffer);
extern uint64_t audit_hash_event(audit_event_t* event);

// Test helpers
static uint32_t tests_passed = 0;
static uint32_t tests_failed = 0;

#define TEST_ASSERT(cond, msg) do { \
    if (!(cond)) { \
        kprintf("[TEST FAIL] %s: %s\n", __func__, msg); \
        tests_failed++; \
        return; \
    } \
} while(0)

#define TEST_PASS() do { \
    kprintf("[TEST PASS] %s\n", __func__); \
    tests_passed++; \
} while(0)

// Test 1: Buffer creation and destruction
void test_audit_buffer_create_destroy(void) {
    audit_buffer_t* buffer = audit_buffer_create();
    TEST_ASSERT(buffer != NULL, "Buffer creation failed");
    TEST_ASSERT(buffer->events != NULL, "Buffer events array is NULL");
    TEST_ASSERT(buffer->count == 0, "Initial count should be 0");
    TEST_ASSERT(buffer->head == 0, "Initial head should be 0");
    TEST_ASSERT(buffer->tail == 0, "Initial tail should be 0");

    audit_buffer_destroy(buffer);
    TEST_PASS();
}

// Test 2: Single event write and read
void test_audit_single_event(void) {
    audit_buffer_t* buffer = audit_buffer_create();
    TEST_ASSERT(buffer != NULL, "Buffer creation failed");

    // Create test event
    audit_event_t event_write;
    memset(&event_write, 0, sizeof(audit_event_t));
    event_write.timestamp = 12345;
    event_write.type = AUDIT_FILE_OPEN;
    event_write.result = AUDIT_SUCCESS;
    event_write.pid = 100;
    event_write.uid = 1000;
    strncpy(event_write.path, "/test/file.txt", AUDIT_PATH_MAX - 1);

    // Write event
    int result = audit_buffer_write(buffer, &event_write);
    TEST_ASSERT(result == 0, "Write failed");
    TEST_ASSERT(audit_buffer_count(buffer) == 1, "Count should be 1");

    // Read event
    audit_event_t event_read;
    result = audit_buffer_read(buffer, &event_read);
    TEST_ASSERT(result == 0, "Read failed");
    TEST_ASSERT(event_read.type == AUDIT_FILE_OPEN, "Event type mismatch");
    TEST_ASSERT(event_read.pid == 100, "PID mismatch");
    TEST_ASSERT(event_read.uid == 1000, "UID mismatch");
    TEST_ASSERT(strcmp(event_read.path, "/test/file.txt") == 0, "Path mismatch");

    TEST_ASSERT(audit_buffer_count(buffer) == 0, "Count should be 0 after read");

    audit_buffer_destroy(buffer);
    TEST_PASS();
}

// Test 3: Multiple events
void test_audit_multiple_events(void) {
    audit_buffer_t* buffer = audit_buffer_create();
    TEST_ASSERT(buffer != NULL, "Buffer creation failed");

    // Write multiple events
    for (uint32_t i = 0; i < 10; i++) {
        audit_event_t event;
        memset(&event, 0, sizeof(audit_event_t));
        event.timestamp = i;
        event.type = AUDIT_PROC_EXEC;
        event.pid = 100 + i;
        event.uid = 1000;

        int result = audit_buffer_write(buffer, &event);
        TEST_ASSERT(result == 0, "Write failed");
    }

    TEST_ASSERT(audit_buffer_count(buffer) == 10, "Count should be 10");

    // Read all events
    for (uint32_t i = 0; i < 10; i++) {
        audit_event_t event;
        int result = audit_buffer_read(buffer, &event);
        TEST_ASSERT(result == 0, "Read failed");
        TEST_ASSERT(event.timestamp == i, "Timestamp mismatch");
        TEST_ASSERT(event.pid == 100 + i, "PID mismatch");
    }

    TEST_ASSERT(audit_buffer_count(buffer) == 0, "Count should be 0");

    audit_buffer_destroy(buffer);
    TEST_PASS();
}

// Test 4: Buffer overflow (circular behavior)
void test_audit_buffer_overflow(void) {
    audit_buffer_t* buffer = audit_buffer_create();
    TEST_ASSERT(buffer != NULL, "Buffer creation failed");

    // Write more than buffer size
    uint32_t write_count = AUDIT_BUFFER_SIZE + 100;

    for (uint32_t i = 0; i < write_count; i++) {
        audit_event_t event;
        memset(&event, 0, sizeof(audit_event_t));
        event.timestamp = i;
        event.type = AUDIT_FILE_READ;
        event.pid = i;

        audit_buffer_write(buffer, &event);
    }

    // Buffer should be full
    TEST_ASSERT(audit_buffer_count(buffer) == AUDIT_BUFFER_SIZE,
                "Buffer should be at max size");

    // Dropped count should reflect overflow
    TEST_ASSERT(buffer->dropped == 100, "Dropped count mismatch");

    // Read all events - should get the last AUDIT_BUFFER_SIZE events
    for (uint32_t i = 0; i < AUDIT_BUFFER_SIZE; i++) {
        audit_event_t event;
        int result = audit_buffer_read(buffer, &event);
        TEST_ASSERT(result == 0, "Read failed");

        // Should have events starting from index 100
        uint32_t expected_timestamp = 100 + i;
        TEST_ASSERT(event.timestamp == expected_timestamp,
                   "Timestamp mismatch in overflow scenario");
    }

    audit_buffer_destroy(buffer);
    TEST_PASS();
}

// Test 5: Hash chain integrity
void test_audit_hash_chain(void) {
    audit_buffer_t* buffer = audit_buffer_create();
    TEST_ASSERT(buffer != NULL, "Buffer creation failed");

    // Write events
    for (uint32_t i = 0; i < 5; i++) {
        audit_event_t event;
        memset(&event, 0, sizeof(audit_event_t));
        event.timestamp = i;
        event.type = AUDIT_SECURITY_CAP_DENIED;
        event.pid = 200;

        audit_buffer_write(buffer, &event);
    }

    // Verify hash chain
    bool valid = audit_buffer_verify_integrity(buffer);
    TEST_ASSERT(valid, "Hash chain verification failed");

    // Tamper with an event
    buffer->events[2].pid = 999;  // Corrupt event

    // Verification should fail now
    valid = audit_buffer_verify_integrity(buffer);
    TEST_ASSERT(!valid, "Hash chain should detect tampering");

    audit_buffer_destroy(buffer);
    TEST_PASS();
}

// Test 6: Event hash computation
void test_audit_event_hash(void) {
    audit_event_t event1;
    memset(&event1, 0, sizeof(audit_event_t));
    event1.timestamp = 12345;
    event1.type = AUDIT_FILE_WRITE;
    event1.pid = 100;

    uint64_t hash1 = audit_hash_event(&event1);
    TEST_ASSERT(hash1 != 0, "Hash should not be zero");

    // Same event should produce same hash
    uint64_t hash2 = audit_hash_event(&event1);
    TEST_ASSERT(hash1 == hash2, "Hash should be deterministic");

    // Different event should produce different hash
    audit_event_t event2;
    memcpy(&event2, &event1, sizeof(audit_event_t));
    event2.pid = 101;  // Change one field

    uint64_t hash3 = audit_hash_event(&event2);
    TEST_ASSERT(hash3 != hash1, "Different events should have different hashes");

    TEST_PASS();
}

// Test 7: Rule matching
void test_audit_rule_matching(void) {
    audit_rules_init();

    // Create test rule
    audit_rule_t rule;
    memset(&rule, 0, sizeof(rule));
    rule.filter_type = AUDIT_FILTER_TYPE;
    rule.criteria.event_type = AUDIT_SECURITY_CAP_DENIED;
    rule.action = AUDIT_ACTION_ALERT;
    rule.enabled = true;

    int rule_id = audit_rule_add(&rule);
    TEST_ASSERT(rule_id > 0, "Rule add failed");

    // Create matching event
    audit_event_t event;
    memset(&event, 0, sizeof(event));
    event.type = AUDIT_SECURITY_CAP_DENIED;
    event.pid = 100;

    // Evaluate rules
    audit_action_t action = audit_rules_evaluate(&event);
    TEST_ASSERT(action == AUDIT_ACTION_ALERT, "Rule should match and return ALERT");

    // Non-matching event
    event.type = AUDIT_FILE_READ;
    action = audit_rules_evaluate(&event);
    TEST_ASSERT(action == AUDIT_ACTION_LOG, "Non-matching event should return LOG");

    // Clean up
    audit_rule_delete(rule_id);

    TEST_PASS();
}

// Test 8: Filter by UID
void test_audit_filter_uid(void) {
    audit_rules_init();

    // Create UID filter rule
    audit_rule_t rule;
    memset(&rule, 0, sizeof(rule));
    rule.filter_type = AUDIT_FILTER_UID;
    rule.criteria.uid = 1000;
    rule.action = AUDIT_ACTION_IGNORE;
    rule.enabled = true;

    int rule_id = audit_rule_add(&rule);
    TEST_ASSERT(rule_id > 0, "Rule add failed");

    // Event from UID 1000 should be ignored
    audit_event_t event;
    memset(&event, 0, sizeof(event));
    event.uid = 1000;

    audit_action_t action = audit_rules_evaluate(&event);
    TEST_ASSERT(action == AUDIT_ACTION_IGNORE, "UID 1000 should be ignored");

    // Event from different UID should be logged
    event.uid = 1001;
    action = audit_rules_evaluate(&event);
    TEST_ASSERT(action == AUDIT_ACTION_LOG, "UID 1001 should be logged");

    audit_rule_delete(rule_id);
    TEST_PASS();
}

// Test 9: Path pattern matching
void test_audit_path_pattern(void) {
    audit_rules_init();

    // Create path filter rule
    audit_rule_t rule;
    memset(&rule, 0, sizeof(rule));
    rule.filter_type = AUDIT_FILTER_PATH;
    strncpy(rule.criteria.path_pattern, "/etc/*", AUDIT_PATH_MAX - 1);
    rule.action = AUDIT_ACTION_ALERT;
    rule.enabled = true;

    int rule_id = audit_rule_add(&rule);
    TEST_ASSERT(rule_id > 0, "Rule add failed");

    // Matching path
    audit_event_t event;
    memset(&event, 0, sizeof(event));
    strncpy(event.path, "/etc/passwd", AUDIT_PATH_MAX - 1);

    audit_action_t action = audit_rules_evaluate(&event);
    TEST_ASSERT(action == AUDIT_ACTION_ALERT, "/etc/passwd should match /etc/*");

    // Non-matching path
    strncpy(event.path, "/home/user/file", AUDIT_PATH_MAX - 1);
    action = audit_rules_evaluate(&event);
    TEST_ASSERT(action == AUDIT_ACTION_LOG, "/home/user/file should not match /etc/*");

    audit_rule_delete(rule_id);
    TEST_PASS();
}

// Test 10: Sequence numbers
void test_audit_sequence_numbers(void) {
    audit_buffer_t* buffer = audit_buffer_create();
    TEST_ASSERT(buffer != NULL, "Buffer creation failed");

    // Write events
    for (uint32_t i = 0; i < 5; i++) {
        audit_event_t event;
        memset(&event, 0, sizeof(audit_event_t));
        event.type = AUDIT_PROC_FORK;
        audit_buffer_write(buffer, &event);
    }

    // Read and verify sequence numbers
    for (uint32_t i = 0; i < 5; i++) {
        audit_event_t event;
        audit_buffer_read(buffer, &event);
        TEST_ASSERT(event.sequence == i, "Sequence number mismatch");
    }

    audit_buffer_destroy(buffer);
    TEST_PASS();
}

// Main test runner
void run_audit_tests(void) {
    kprintf("\n");
    kprintf("=============================================================================\n");
    kprintf("Running Audit Subsystem Unit Tests\n");
    kprintf("=============================================================================\n\n");

    tests_passed = 0;
    tests_failed = 0;

    // Run all tests
    test_audit_buffer_create_destroy();
    test_audit_single_event();
    test_audit_multiple_events();
    test_audit_buffer_overflow();
    test_audit_hash_chain();
    test_audit_event_hash();
    test_audit_rule_matching();
    test_audit_filter_uid();
    test_audit_path_pattern();
    test_audit_sequence_numbers();

    // Print summary
    kprintf("\n");
    kprintf("=============================================================================\n");
    kprintf("Test Summary\n");
    kprintf("=============================================================================\n");
    kprintf("Tests passed: %u\n", tests_passed);
    kprintf("Tests failed: %u\n", tests_failed);
    kprintf("Total tests:  %u\n", tests_passed + tests_failed);

    if (tests_failed == 0) {
        kprintf("\nALL TESTS PASSED!\n");
    } else {
        kprintf("\nSOME TESTS FAILED!\n");
    }

    kprintf("=============================================================================\n\n");
}
