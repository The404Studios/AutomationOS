#include "../../kernel/include/rlimit.h"
#include "../../kernel/include/kernel.h"
#include "../../kernel/include/sched.h"

// Test helper macros
#define TEST_ASSERT(cond, msg) do { \
    if (!(cond)) { \
        kprintf("[TEST FAIL] %s: %s\n", __func__, msg); \
        return; \
    } \
} while(0)

#define TEST_PASS() kprintf("[TEST PASS] %s\n", __func__)

// Mock timer ticks
static uint64_t mock_ticks = 0;
uint64_t get_timer_ticks(void) {
    return mock_ticks;
}

// Test 1: Container creation and destruction
void test_rlimit_container_create_destroy(void) {
    rlimit_container_t* rl = rlimit_create_container();
    TEST_ASSERT(rl != NULL, "Failed to create container");

    // Check default limits are set
    TEST_ASSERT(rl->limits[RLIMIT_CPU].soft == RLIMIT_DEFAULT_CPU,
                "CPU soft limit not set correctly");
    TEST_ASSERT(rl->limits[RLIMIT_MEMORY].soft == RLIMIT_DEFAULT_MEMORY,
                "Memory soft limit not set correctly");
    TEST_ASSERT(rl->limits[RLIMIT_NOFILE].soft == RLIMIT_DEFAULT_NOFILE,
                "FD soft limit not set correctly");

    // Check usage is initialized to zero
    TEST_ASSERT(rl->usage.cpu_time == 0, "CPU time not zero");
    TEST_ASSERT(rl->usage.memory_current == 0, "Memory usage not zero");
    TEST_ASSERT(rl->usage.fd_count == 0, "FD count not zero");

    rlimit_destroy_container(rl);
    TEST_PASS();
}

// Test 2: Setting and getting limits
void test_rlimit_set_get(void) {
    rlimit_container_t* rl = rlimit_create_container();
    TEST_ASSERT(rl != NULL, "Failed to create container");

    // Set custom CPU limit
    rlimit_t new_limit = {
        .soft = 5000,  // 5 seconds
        .hard = 10000  // 10 seconds
    };

    int result = rlimit_set(rl, RLIMIT_CPU, &new_limit);
    TEST_ASSERT(result == 0, "Failed to set limit");

    // Get and verify
    rlimit_t retrieved;
    result = rlimit_get(rl, RLIMIT_CPU, &retrieved);
    TEST_ASSERT(result == 0, "Failed to get limit");
    TEST_ASSERT(retrieved.soft == 5000, "Soft limit incorrect");
    TEST_ASSERT(retrieved.hard == 10000, "Hard limit incorrect");

    rlimit_destroy_container(rl);
    TEST_PASS();
}

// Test 3: CPU limit enforcement
void test_rlimit_cpu_enforcement(void) {
    rlimit_container_t* rl = rlimit_create_container();
    TEST_ASSERT(rl != NULL, "Failed to create container");

    // Set strict CPU limit
    rlimit_t limit = { .soft = 100, .hard = 200 };
    rlimit_set(rl, RLIMIT_CPU, &limit);

    // Account CPU time within soft limit - should pass
    rlimit_account_cpu(rl, 50);
    TEST_ASSERT(rl->usage.cpu_time == 50, "CPU time not tracked");
    TEST_ASSERT(rlimit_check_cpu(rl, 40), "Should allow CPU usage within soft limit");

    // Exceed soft limit - should still pass but signal
    rlimit_account_cpu(rl, 60);
    TEST_ASSERT(rl->usage.cpu_time == 110, "CPU time not accumulated");
    TEST_ASSERT(rlimit_check_cpu(rl, 50), "Should allow CPU usage above soft but below hard");
    TEST_ASSERT(rl->soft_limit_signaled[RLIMIT_CPU], "Soft limit not signaled");

    // Exceed hard limit - should fail
    TEST_ASSERT(!rlimit_check_cpu(rl, 100), "Should deny CPU usage exceeding hard limit");

    rlimit_destroy_container(rl);
    TEST_PASS();
}

// Test 4: CPU quota enforcement
void test_rlimit_cpu_quota(void) {
    rlimit_container_t* rl = rlimit_create_container();
    TEST_ASSERT(rl != NULL, "Failed to create container");

    // Set 50% CPU quota (50ms per 100ms period)
    mock_ticks = 0;
    rlimit_set_cpu_quota(rl, 50000, 100000);  // 50ms per 100ms

    // Use 30ms - should be OK
    rlimit_account_cpu(rl, 30);
    TEST_ASSERT(!rlimit_cpu_quota_exceeded(rl), "Quota should not be exceeded");

    // Use another 25ms - should exceed quota
    rlimit_account_cpu(rl, 25);
    TEST_ASSERT(rlimit_cpu_quota_exceeded(rl), "Quota should be exceeded");
    TEST_ASSERT(rl->cpu_quota.throttled, "Process should be throttled");

    // Advance time to next period
    mock_ticks = 101;
    rlimit_cpu_quota_refill(rl);
    TEST_ASSERT(!rl->cpu_quota.throttled, "Throttle should be cleared");
    TEST_ASSERT(rl->cpu_quota.used_us == 0, "Quota usage should reset");

    rlimit_destroy_container(rl);
    TEST_PASS();
}

// Test 5: Memory limit enforcement
void test_rlimit_memory_enforcement(void) {
    rlimit_container_t* rl = rlimit_create_container();
    TEST_ASSERT(rl != NULL, "Failed to create container");

    // Set strict memory limit (1MB)
    rlimit_t limit = { .soft = 1024*1024, .hard = 2*1024*1024 };
    rlimit_set(rl, RLIMIT_MEMORY, &limit);

    // Allocate within soft limit
    TEST_ASSERT(rlimit_check_memory(rl, 512*1024), "Should allow allocation within soft limit");
    rlimit_account_memory_alloc(rl, 512*1024);
    TEST_ASSERT(rl->usage.memory_current == 512*1024, "Memory usage not tracked");

    // Allocate beyond soft limit but within hard limit
    TEST_ASSERT(rlimit_check_memory(rl, 600*1024), "Should allow allocation above soft but below hard");
    rlimit_account_memory_alloc(rl, 600*1024);
    TEST_ASSERT(rl->soft_limit_signaled[RLIMIT_MEMORY], "Soft limit not signaled");

    // Exceed hard limit
    TEST_ASSERT(!rlimit_check_memory(rl, 1024*1024), "Should deny allocation exceeding hard limit");

    // Free memory
    rlimit_account_memory_free(rl, 512*1024);
    TEST_ASSERT(rl->usage.memory_current == 600*1024, "Memory not freed correctly");

    rlimit_destroy_container(rl);
    TEST_PASS();
}

// Test 6: File descriptor limit enforcement
void test_rlimit_fd_enforcement(void) {
    rlimit_container_t* rl = rlimit_create_container();
    TEST_ASSERT(rl != NULL, "Failed to create container");

    // Set strict FD limit
    rlimit_t limit = { .soft = 10, .hard = 20 };
    rlimit_set(rl, RLIMIT_NOFILE, &limit);

    // Open files within soft limit
    for (int i = 0; i < 10; i++) {
        TEST_ASSERT(rlimit_check_fd(rl), "Should allow FD within soft limit");
        rlimit_account_fd_open(rl);
    }
    TEST_ASSERT(rl->usage.fd_count == 10, "FD count incorrect");

    // Open beyond soft limit
    TEST_ASSERT(rlimit_check_fd(rl), "Should allow FD above soft but below hard");
    rlimit_account_fd_open(rl);

    // Open up to hard limit
    for (int i = 11; i < 20; i++) {
        rlimit_account_fd_open(rl);
    }
    TEST_ASSERT(rl->usage.fd_count == 20, "FD count at hard limit incorrect");

    // Exceed hard limit
    TEST_ASSERT(!rlimit_check_fd(rl), "Should deny FD exceeding hard limit");

    // Close some FDs
    for (int i = 0; i < 5; i++) {
        rlimit_account_fd_close(rl);
    }
    TEST_ASSERT(rl->usage.fd_count == 15, "FD count after close incorrect");
    TEST_ASSERT(rlimit_check_fd(rl), "Should allow FD after closing some");

    rlimit_destroy_container(rl);
    TEST_PASS();
}

// Test 7: Token bucket rate limiting
void test_token_bucket(void) {
    token_bucket_t bucket;
    mock_ticks = 0;

    // Initialize bucket: 1000 bytes/sec capacity, 2000 byte burst
    token_bucket_init(&bucket, 1000, 2000);
    TEST_ASSERT(bucket.tokens == 2000, "Initial tokens incorrect");
    TEST_ASSERT(bucket.capacity == 2000, "Capacity incorrect");

    // Consume within capacity
    TEST_ASSERT(token_bucket_consume(&bucket, 1000, mock_ticks), "Should allow consume within capacity");
    TEST_ASSERT(bucket.tokens == 1000, "Tokens after consume incorrect");

    // Consume remaining
    TEST_ASSERT(token_bucket_consume(&bucket, 1000, mock_ticks), "Should allow consume remaining tokens");
    TEST_ASSERT(bucket.tokens == 0, "Tokens should be zero");

    // Try to consume more - should fail
    TEST_ASSERT(!token_bucket_consume(&bucket, 500, mock_ticks), "Should deny when no tokens");

    // Advance time by 1 second (1000 ticks)
    mock_ticks = 1000;
    token_bucket_refill(&bucket, mock_ticks);
    TEST_ASSERT(bucket.tokens == 1000, "Tokens should refill at rate");

    // Consume again
    TEST_ASSERT(token_bucket_consume(&bucket, 500, mock_ticks), "Should allow after refill");

    TEST_PASS();
}

// Test 8: Network bandwidth limiting
void test_rlimit_network(void) {
    rlimit_container_t* rl = rlimit_create_container();
    TEST_ASSERT(rl != NULL, "Failed to create container");

    mock_ticks = 0;

    // Set network TX rate limit: 1MB/s
    token_bucket_init(&rl->net_tx_bucket, 1024*1024, 2*1024*1024);

    // Send 1MB - should pass
    TEST_ASSERT(rlimit_check_network_tx(rl, 1024*1024), "Should allow TX within rate");
    rlimit_account_network_tx(rl, 1024*1024);

    // Send another 1.5MB - should fail (bucket empty)
    TEST_ASSERT(!rlimit_check_network_tx(rl, 1536*1024), "Should deny TX exceeding rate");

    // Advance 1 second
    mock_ticks = 1000;

    // Should allow again (bucket refilled)
    TEST_ASSERT(rlimit_check_network_tx(rl, 1024*1024), "Should allow TX after refill");

    rlimit_destroy_container(rl);
    TEST_PASS();
}

// Test 9: Resource limit inheritance
void test_rlimit_inheritance(void) {
    rlimit_container_t* parent = rlimit_create_container();
    TEST_ASSERT(parent != NULL, "Failed to create parent container");

    // Set custom limits on parent
    rlimit_t limit = { .soft = 5000, .hard = 10000 };
    rlimit_set(parent, RLIMIT_CPU, &limit);

    // Parent uses some resources
    rlimit_account_cpu(parent, 2000);
    rlimit_account_memory_alloc(parent, 1024*1024);

    // Create child container
    rlimit_container_t* child = rlimit_inherit_container(parent);
    TEST_ASSERT(child != NULL, "Failed to create child container");

    // Child should inherit limits but not usage
    rlimit_t child_limit;
    rlimit_get(child, RLIMIT_CPU, &child_limit);
    TEST_ASSERT(child_limit.soft == 5000, "Child did not inherit soft limit");
    TEST_ASSERT(child_limit.hard == 10000, "Child did not inherit hard limit");
    TEST_ASSERT(child->usage.cpu_time == 0, "Child should start with zero usage");
    TEST_ASSERT(child->usage.memory_current == 0, "Child should start with zero memory");

    rlimit_destroy_container(parent);
    rlimit_destroy_container(child);
    TEST_PASS();
}

// Test 10: Memory pressure detection
void test_memory_pressure(void) {
    rlimit_container_t* rl = rlimit_create_container();
    TEST_ASSERT(rl != NULL, "Failed to create container");

    // Set memory limit to 1MB
    rlimit_t limit = { .soft = 1024*1024, .hard = 1024*1024 };
    rlimit_set(rl, RLIMIT_MEMORY, &limit);

    // No pressure at 50% usage
    rlimit_account_memory_alloc(rl, 512*1024);
    TEST_ASSERT(rlimit_get_memory_pressure(rl) == 0, "Should be LOW pressure");

    // Medium pressure at 75%
    rlimit_account_memory_alloc(rl, 256*1024);
    TEST_ASSERT(rlimit_get_memory_pressure(rl) == 1, "Should be MEDIUM pressure");

    // High pressure at 90%
    rlimit_account_memory_alloc(rl, 180*1024);
    TEST_ASSERT(rlimit_get_memory_pressure(rl) == 2, "Should be HIGH pressure");

    // Critical pressure at 98%
    rlimit_account_memory_alloc(rl, 80*1024);
    TEST_ASSERT(rlimit_get_memory_pressure(rl) == 3, "Should be CRITICAL pressure");

    rlimit_destroy_container(rl);
    TEST_PASS();
}

// Test runner
void run_rlimit_tests(void) {
    kprintf("\n[TEST] Running resource limit tests...\n");
    kprintf("==========================================\n");

    test_rlimit_container_create_destroy();
    test_rlimit_set_get();
    test_rlimit_cpu_enforcement();
    test_rlimit_cpu_quota();
    test_rlimit_memory_enforcement();
    test_rlimit_fd_enforcement();
    test_token_bucket();
    test_rlimit_network();
    test_rlimit_inheritance();
    test_memory_pressure();

    kprintf("==========================================\n");
    kprintf("[TEST] All resource limit tests completed\n\n");
}
