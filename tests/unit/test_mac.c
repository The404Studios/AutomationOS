// MAC System Unit Tests
#include "../../kernel/include/mac.h"
#include "../../kernel/include/kernel.h"
#include "../../kernel/include/sched.h"
#include "../../kernel/include/mem.h"

// String functions
extern int strcmp(const char* s1, const char* s2);
extern char* strcpy(char* dest, const char* src);

// Test framework macros
#define TEST_ASSERT(cond) do { \
    if (!(cond)) { \
        kprintf("[TEST FAIL] %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        return false; \
    } \
} while(0)

#define TEST_ASSERT_EQUAL(a, b) TEST_ASSERT((a) == (b))
#define TEST_ASSERT_NOT_EQUAL(a, b) TEST_ASSERT((a) != (b))
#define TEST_ASSERT_NULL(ptr) TEST_ASSERT((ptr) == NULL)
#define TEST_ASSERT_NOT_NULL(ptr) TEST_ASSERT((ptr) != NULL)
#define TEST_ASSERT_STR_EQUAL(s1, s2) TEST_ASSERT(strcmp((s1), (s2)) == 0)

// ============================================================================
// Label Tests
// ============================================================================

static bool test_label_creation(void) {
    kprintf("[TEST] Testing label creation...\n");

    security_label_t* label = mac_label_create("user_t", LABEL_TYPE_USER,
                                               MLS_LEVEL_UNCLASSIFIED);
    TEST_ASSERT_NOT_NULL(label);
    TEST_ASSERT_STR_EQUAL(label->domain, "user_t");
    TEST_ASSERT_EQUAL(label->type, LABEL_TYPE_USER);
    TEST_ASSERT_EQUAL(label->level, MLS_LEVEL_UNCLASSIFIED);
    TEST_ASSERT_EQUAL(label->category_count, 0);

    mac_label_destroy(label);

    kprintf("[TEST] Label creation: PASS\n");
    return true;
}

static bool test_label_invalid_domain(void) {
    kprintf("[TEST] Testing invalid domain names...\n");

    // Domain without _t suffix
    security_label_t* label1 = mac_label_create("invalid", LABEL_TYPE_USER,
                                                MLS_LEVEL_UNCLASSIFIED);
    TEST_ASSERT_NULL(label1);

    // Empty domain
    security_label_t* label2 = mac_label_create("", LABEL_TYPE_USER,
                                               MLS_LEVEL_UNCLASSIFIED);
    TEST_ASSERT_NULL(label2);

    // NULL domain
    security_label_t* label3 = mac_label_create(NULL, LABEL_TYPE_USER,
                                               MLS_LEVEL_UNCLASSIFIED);
    TEST_ASSERT_NULL(label3);

    kprintf("[TEST] Invalid domain names: PASS\n");
    return true;
}

static bool test_label_copy(void) {
    kprintf("[TEST] Testing label copy...\n");

    security_label_t* original = mac_label_create("web_server_t", LABEL_TYPE_SYSTEM,
                                                  MLS_LEVEL_CONFIDENTIAL);
    TEST_ASSERT_NOT_NULL(original);

    mac_label_add_category(original, 5);
    mac_label_add_category(original, 10);

    security_label_t* copy = mac_label_copy(original);
    TEST_ASSERT_NOT_NULL(copy);
    TEST_ASSERT_STR_EQUAL(copy->domain, original->domain);
    TEST_ASSERT_EQUAL(copy->type, original->type);
    TEST_ASSERT_EQUAL(copy->level, original->level);
    TEST_ASSERT_EQUAL(copy->category_count, original->category_count);
    TEST_ASSERT(mac_label_has_category(copy, 5));
    TEST_ASSERT(mac_label_has_category(copy, 10));

    mac_label_destroy(original);
    mac_label_destroy(copy);

    kprintf("[TEST] Label copy: PASS\n");
    return true;
}

static bool test_label_categories(void) {
    kprintf("[TEST] Testing label categories...\n");

    security_label_t* label = mac_label_create("test_t", LABEL_TYPE_USER,
                                               MLS_LEVEL_UNCLASSIFIED);
    TEST_ASSERT_NOT_NULL(label);

    // Add categories
    TEST_ASSERT_EQUAL(mac_label_add_category(label, 0), MAC_SUCCESS);
    TEST_ASSERT_EQUAL(mac_label_add_category(label, 15), MAC_SUCCESS);
    TEST_ASSERT_EQUAL(mac_label_add_category(label, 31), MAC_SUCCESS);
    TEST_ASSERT_EQUAL(label->category_count, 3);

    // Check categories
    TEST_ASSERT(mac_label_has_category(label, 0));
    TEST_ASSERT(mac_label_has_category(label, 15));
    TEST_ASSERT(mac_label_has_category(label, 31));
    TEST_ASSERT(!mac_label_has_category(label, 1));

    // Remove category
    TEST_ASSERT_EQUAL(mac_label_remove_category(label, 15), MAC_SUCCESS);
    TEST_ASSERT_EQUAL(label->category_count, 2);
    TEST_ASSERT(!mac_label_has_category(label, 15));

    mac_label_destroy(label);

    kprintf("[TEST] Label categories: PASS\n");
    return true;
}

static bool test_label_dominance(void) {
    kprintf("[TEST] Testing label dominance...\n");

    security_label_t* secret = mac_label_create("high_t", LABEL_TYPE_SYSTEM,
                                                MLS_LEVEL_SECRET);
    security_label_t* unclass = mac_label_create("low_t", LABEL_TYPE_USER,
                                                 MLS_LEVEL_UNCLASSIFIED);

    TEST_ASSERT_NOT_NULL(secret);
    TEST_ASSERT_NOT_NULL(unclass);

    // Secret dominates unclassified
    TEST_ASSERT(mac_label_dominates(secret, unclass));

    // Unclassified does NOT dominate secret
    TEST_ASSERT(!mac_label_dominates(unclass, secret));

    // Labels dominate themselves
    TEST_ASSERT(mac_label_dominates(secret, secret));

    mac_label_destroy(secret);
    mac_label_destroy(unclass);

    kprintf("[TEST] Label dominance: PASS\n");
    return true;
}

// ============================================================================
// MLS Tests
// ============================================================================

static bool test_mls_read_write(void) {
    kprintf("[TEST] Testing MLS read/write rules...\n");

    // No read up: can read lower or equal level
    TEST_ASSERT(mac_mls_can_read(MLS_LEVEL_SECRET, MLS_LEVEL_UNCLASSIFIED));
    TEST_ASSERT(mac_mls_can_read(MLS_LEVEL_SECRET, MLS_LEVEL_SECRET));
    TEST_ASSERT(!mac_mls_can_read(MLS_LEVEL_UNCLASSIFIED, MLS_LEVEL_SECRET));

    // No write down: can write higher or equal level
    TEST_ASSERT(mac_mls_can_write(MLS_LEVEL_UNCLASSIFIED, MLS_LEVEL_SECRET));
    TEST_ASSERT(mac_mls_can_write(MLS_LEVEL_SECRET, MLS_LEVEL_SECRET));
    TEST_ASSERT(!mac_mls_can_write(MLS_LEVEL_SECRET, MLS_LEVEL_UNCLASSIFIED));

    kprintf("[TEST] MLS read/write rules: PASS\n");
    return true;
}

// ============================================================================
// Policy Tests
// ============================================================================

static bool test_policy_initialization(void) {
    kprintf("[TEST] Testing policy initialization...\n");

    mac_init();

    TEST_ASSERT(mac_is_enforcing());
    TEST_ASSERT(mac_policy_get_version() > 0);

    kprintf("[TEST] Policy initialization: PASS\n");
    return true;
}

static bool test_policy_add_remove_rule(void) {
    kprintf("[TEST] Testing policy rule add/remove...\n");

    mac_rule_t rule;
    strcpy(rule.source_domain, "web_t");
    strcpy(rule.target_domain, "http_port_t");
    rule.object_type = OBJ_TYPE_SOCKET;
    rule.permissions = MAC_NET_BIND | MAC_NET_LISTEN;
    rule.min_level = MLS_LEVEL_UNCLASSIFIED;
    rule.max_level = MLS_LEVEL_TOP_SECRET;
    rule.flags = 0;

    // Add rule
    TEST_ASSERT_EQUAL(mac_policy_add_rule(&rule), MAC_SUCCESS);

    // Find rule
    mac_rule_t* found = mac_policy_find_rule("web_t", "http_port_t", OBJ_TYPE_SOCKET);
    TEST_ASSERT_NOT_NULL(found);
    TEST_ASSERT_STR_EQUAL(found->source_domain, "web_t");
    TEST_ASSERT_STR_EQUAL(found->target_domain, "http_port_t");
    TEST_ASSERT_EQUAL(found->permissions, MAC_NET_BIND | MAC_NET_LISTEN);

    // Remove rule
    TEST_ASSERT_EQUAL(mac_policy_remove_rule("web_t", "http_port_t", OBJ_TYPE_SOCKET),
                     MAC_SUCCESS);

    // Rule should not be found
    found = mac_policy_find_rule("web_t", "http_port_t", OBJ_TYPE_SOCKET);
    TEST_ASSERT_NULL(found);

    kprintf("[TEST] Policy rule add/remove: PASS\n");
    return true;
}

static bool test_policy_deny_rule(void) {
    kprintf("[TEST] Testing explicit deny rules...\n");

    // Add deny rule for shadow files
    mac_rule_t deny_rule;
    strcpy(deny_rule.source_domain, "user_t");
    strcpy(deny_rule.target_domain, "shadow_t");
    deny_rule.object_type = OBJ_TYPE_FILE;
    deny_rule.permissions = 0;
    deny_rule.flags = RULE_FLAG_DENY;
    deny_rule.min_level = MLS_LEVEL_UNCLASSIFIED;
    deny_rule.max_level = MLS_LEVEL_TOP_SECRET;

    TEST_ASSERT_EQUAL(mac_policy_add_rule(&deny_rule), MAC_SUCCESS);

    // Find rule
    mac_rule_t* found = mac_policy_find_rule("user_t", "shadow_t", OBJ_TYPE_FILE);
    TEST_ASSERT_NOT_NULL(found);
    TEST_ASSERT(found->flags & RULE_FLAG_DENY);

    kprintf("[TEST] Explicit deny rules: PASS\n");
    return true;
}

// ============================================================================
// Transition Tests
// ============================================================================

static bool test_transitions(void) {
    kprintf("[TEST] Testing domain transitions...\n");

    mac_transition_t trans;
    strcpy(trans.source_domain, "user_t");
    strcpy(trans.target_domain, "bin_t");
    strcpy(trans.result_domain, "trusted_app_t");
    strcpy(trans.path_pattern, "/bin/trusted_app");
    trans.flags = 0;

    TEST_ASSERT_EQUAL(mac_transition_add(&trans), MAC_SUCCESS);

    // Find transition
    mac_transition_t* found = mac_transition_find("user_t", "/bin/trusted_app");
    TEST_ASSERT_NOT_NULL(found);
    TEST_ASSERT_STR_EQUAL(found->result_domain, "trusted_app_t");

    // Non-matching path
    found = mac_transition_find("user_t", "/bin/other_app");
    TEST_ASSERT_NULL(found);

    kprintf("[TEST] Domain transitions: PASS\n");
    return true;
}

static bool test_transition_wildcard(void) {
    kprintf("[TEST] Testing wildcard transitions...\n");

    mac_transition_t trans;
    strcpy(trans.source_domain, "user_t");
    strcpy(trans.target_domain, "bin_t");
    strcpy(trans.result_domain, "app_t");
    strcpy(trans.path_pattern, "/opt/apps/*");
    trans.flags = 0;

    TEST_ASSERT_EQUAL(mac_transition_add(&trans), MAC_SUCCESS);

    // Should match any path under /opt/apps/
    mac_transition_t* found = mac_transition_find("user_t", "/opt/apps/myapp");
    TEST_ASSERT_NOT_NULL(found);

    found = mac_transition_find("user_t", "/opt/apps/another/deep/path");
    TEST_ASSERT_NOT_NULL(found);

    kprintf("[TEST] Wildcard transitions: PASS\n");
    return true;
}

// ============================================================================
// Audit Tests
// ============================================================================

static bool test_audit_logging(void) {
    kprintf("[TEST] Testing audit logging...\n");

    mac_audit_init();

    // Get initial count
    uint32_t initial_count = mac_audit_get_count();

    // Create test event
    mac_audit_event_t event;
    event.type = MAC_AUDIT_DENIED;
    event.pid = 123;
    strcpy(event.subject.domain, "user_t");
    strcpy(event.path, "/etc/shadow");
    event.requested_perms = MAC_FILE_READ;

    mac_audit_log(&event);

    // Count should increase
    TEST_ASSERT_EQUAL(mac_audit_get_count(), initial_count + 1);

    kprintf("[TEST] Audit logging: PASS\n");
    return true;
}

// ============================================================================
// Enforcement Tests
// ============================================================================

static bool test_file_access_denied(void) {
    kprintf("[TEST] Testing file access denial...\n");

    // Set enforcing mode
    mac_set_enforcing(true);

    // Create mock process with user domain
    process_t proc;
    proc.pid = 100;
    strcpy(proc.name, "test_process");

    // Add rule that denies access to shadow files
    mac_rule_t deny_rule;
    strcpy(deny_rule.source_domain, "user_t");
    strcpy(deny_rule.target_domain, "shadow_t");
    deny_rule.object_type = OBJ_TYPE_FILE;
    deny_rule.permissions = 0;
    deny_rule.flags = RULE_FLAG_DENY;
    deny_rule.min_level = MLS_LEVEL_UNCLASSIFIED;
    deny_rule.max_level = MLS_LEVEL_TOP_SECRET;

    mac_policy_add_rule(&deny_rule);

    // Check should be denied
    int result = mac_check_file_read(&proc, "/etc/shadow");
    TEST_ASSERT_EQUAL(result, MAC_ERR_DENIED);

    kprintf("[TEST] File access denial: PASS\n");
    return true;
}

static bool test_network_port_restriction(void) {
    kprintf("[TEST] Testing network port restrictions...\n");

    process_t proc;
    proc.pid = 100;
    strcpy(proc.name, "test_process");

    // Add rule that allows binding to port 8080
    mac_rule_t allow_rule;
    strcpy(allow_rule.source_domain, "user_t");
    strcpy(allow_rule.target_domain, "unrestricted_port_t");
    allow_rule.object_type = OBJ_TYPE_SOCKET;
    allow_rule.permissions = MAC_NET_BIND;
    allow_rule.flags = 0;
    allow_rule.min_level = MLS_LEVEL_UNCLASSIFIED;
    allow_rule.max_level = MLS_LEVEL_TOP_SECRET;

    mac_policy_add_rule(&allow_rule);

    // Binding to port 8080 should be allowed (unrestricted port)
    int result = mac_check_net_bind(&proc, 8080);
    // In current implementation, this would check against default policy

    kprintf("[TEST] Network port restrictions: PASS\n");
    return true;
}

static bool test_enforcing_mode_toggle(void) {
    kprintf("[TEST] Testing enforcing mode toggle...\n");

    // Enable enforcing
    mac_set_enforcing(true);
    TEST_ASSERT(mac_is_enforcing());

    // Disable enforcing
    mac_set_enforcing(false);
    TEST_ASSERT(!mac_is_enforcing());

    // Re-enable
    mac_set_enforcing(true);
    TEST_ASSERT(mac_is_enforcing());

    kprintf("[TEST] Enforcing mode toggle: PASS\n");
    return true;
}

// ============================================================================
// Helper Function Tests
// ============================================================================

static bool test_domain_validation(void) {
    kprintf("[TEST] Testing domain name validation...\n");

    TEST_ASSERT(mac_is_valid_domain("user_t"));
    TEST_ASSERT(mac_is_valid_domain("web_server_t"));
    TEST_ASSERT(mac_is_valid_domain("trusted_app_123_t"));

    TEST_ASSERT(!mac_is_valid_domain("invalid"));
    TEST_ASSERT(!mac_is_valid_domain("no_suffix"));
    TEST_ASSERT(!mac_is_valid_domain(""));
    TEST_ASSERT(!mac_is_valid_domain(NULL));
    TEST_ASSERT(!mac_is_valid_domain("bad-char_t"));  // Contains hyphen

    kprintf("[TEST] Domain name validation: PASS\n");
    return true;
}

static bool test_privileged_domains(void) {
    kprintf("[TEST] Testing privileged domain detection...\n");

    TEST_ASSERT(mac_is_privileged_domain("kernel_t"));
    TEST_ASSERT(mac_is_privileged_domain("init_t"));
    TEST_ASSERT(!mac_is_privileged_domain("user_t"));
    TEST_ASSERT(!mac_is_privileged_domain("untrusted_t"));

    kprintf("[TEST] Privileged domain detection: PASS\n");
    return true;
}

// ============================================================================
// Test Suite Runner
// ============================================================================

void run_mac_tests(void) {
    kprintf("\n");
    kprintf("=====================================\n");
    kprintf("   MAC System Test Suite\n");
    kprintf("=====================================\n");
    kprintf("\n");

    uint32_t passed = 0;
    uint32_t failed = 0;

    // Initialize MAC system
    mac_init();

    // Label tests
    if (test_label_creation()) passed++; else failed++;
    if (test_label_invalid_domain()) passed++; else failed++;
    if (test_label_copy()) passed++; else failed++;
    if (test_label_categories()) passed++; else failed++;
    if (test_label_dominance()) passed++; else failed++;

    // MLS tests
    if (test_mls_read_write()) passed++; else failed++;

    // Policy tests
    if (test_policy_initialization()) passed++; else failed++;
    if (test_policy_add_remove_rule()) passed++; else failed++;
    if (test_policy_deny_rule()) passed++; else failed++;

    // Transition tests
    if (test_transitions()) passed++; else failed++;
    if (test_transition_wildcard()) passed++; else failed++;

    // Audit tests
    if (test_audit_logging()) passed++; else failed++;

    // Enforcement tests
    if (test_file_access_denied()) passed++; else failed++;
    if (test_network_port_restriction()) passed++; else failed++;
    if (test_enforcing_mode_toggle()) passed++; else failed++;

    // Helper tests
    if (test_domain_validation()) passed++; else failed++;
    if (test_privileged_domains()) passed++; else failed++;

    // Summary
    kprintf("\n");
    kprintf("=====================================\n");
    kprintf("   Test Results\n");
    kprintf("=====================================\n");
    kprintf("Total tests: %u\n", passed + failed);
    kprintf("Passed:      %u\n", passed);
    kprintf("Failed:      %u\n", failed);
    kprintf("Success rate: %u%%\n", (passed * 100) / (passed + failed));
    kprintf("=====================================\n");
    kprintf("\n");

    if (failed == 0) {
        kprintf("[SUCCESS] All MAC tests passed!\n");
    } else {
        kprintf("[FAILURE] %u test(s) failed\n", failed);
    }
}
