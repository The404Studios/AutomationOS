/*
 * Audit Logging Demo
 *
 * Demonstrates the audit logging API and tools.
 * Shows how to:
 * - Enable/disable auditing
 * - Add audit rules
 * - Generate various audit events
 * - Search and report on audit logs
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>

// Syscall numbers (must match kernel)
#define SYS_AUDIT_ENABLE 200
#define SYS_AUDIT_DISABLE 201
#define SYS_AUDIT_READ 202
#define SYS_AUDIT_RULE_ADD 203
#define SYS_AUDIT_RULE_DEL 204
#define SYS_AUDIT_GET_STATS 205

#include "../../kernel/include/audit.h"

// Syscall wrapper
static inline long syscall3(long number, long arg1, long arg2, long arg3) {
    long ret;
    asm volatile (
        "mov %1, %%rax\n"
        "mov %2, %%rdi\n"
        "mov %3, %%rsi\n"
        "mov %4, %%rdx\n"
        "syscall\n"
        "mov %%rax, %0\n"
        : "=r"(ret)
        : "r"(number), "r"(arg1), "r"(arg2), "r"(arg3)
        : "rax", "rdi", "rsi", "rdx", "rcx", "r11", "memory"
    );
    return ret;
}

void print_banner(const char* title) {
    printf("\n");
    printf("============================================================\n");
    printf("%s\n", title);
    printf("============================================================\n\n");
}

void demo_enable_disable(void) {
    print_banner("Demo 1: Enable/Disable Auditing");

    printf("Enabling audit logging...\n");
    long result = syscall3(SYS_AUDIT_ENABLE, 0, 0, 0);
    if (result == 0) {
        printf("  SUCCESS: Audit logging enabled\n");
    } else {
        printf("  FAILED: Error code %ld\n", result);
        return;
    }

    printf("\nDisabling audit logging...\n");
    result = syscall3(SYS_AUDIT_DISABLE, 0, 0, 0);
    if (result == 0) {
        printf("  SUCCESS: Audit logging disabled\n");
    } else {
        printf("  FAILED: Error code %ld\n", result);
    }

    printf("\nRe-enabling for other demos...\n");
    syscall3(SYS_AUDIT_ENABLE, 0, 0, 0);
    printf("  Audit logging re-enabled\n");
}

void demo_add_rules(void) {
    print_banner("Demo 2: Adding Audit Rules");

    // Rule 1: Alert on security violations
    printf("Adding rule: Alert on capability denied...\n");
    audit_rule_t rule1;
    memset(&rule1, 0, sizeof(rule1));
    rule1.filter_type = AUDIT_FILTER_TYPE;
    rule1.criteria.event_type = AUDIT_SECURITY_CAP_DENIED;
    rule1.action = AUDIT_ACTION_ALERT;
    rule1.enabled = true;

    long result = syscall3(SYS_AUDIT_RULE_ADD, (long)&rule1, 0, 0);
    if (result >= 0) {
        printf("  SUCCESS: Rule added (ID: %ld)\n", result);
    } else {
        printf("  FAILED: Error code %ld\n", result);
    }

    // Rule 2: Log /etc access
    printf("\nAdding rule: Log /etc/* access...\n");
    audit_rule_t rule2;
    memset(&rule2, 0, sizeof(rule2));
    rule2.filter_type = AUDIT_FILTER_PATH;
    strncpy(rule2.criteria.path_pattern, "/etc/*", AUDIT_PATH_MAX - 1);
    rule2.action = AUDIT_ACTION_LOG;
    rule2.enabled = true;

    result = syscall3(SYS_AUDIT_RULE_ADD, (long)&rule2, 0, 0);
    if (result >= 0) {
        printf("  SUCCESS: Rule added (ID: %ld)\n", result);
    } else {
        printf("  FAILED: Error code %ld\n", result);
    }

    // Rule 3: Ignore UID 0 (root)
    printf("\nAdding rule: Ignore UID 0 events...\n");
    audit_rule_t rule3;
    memset(&rule3, 0, sizeof(rule3));
    rule3.filter_type = AUDIT_FILTER_UID;
    rule3.criteria.uid = 0;
    rule3.action = AUDIT_ACTION_IGNORE;
    rule3.enabled = true;

    result = syscall3(SYS_AUDIT_RULE_ADD, (long)&rule3, 0, 0);
    if (result >= 0) {
        printf("  SUCCESS: Rule added (ID: %ld)\n", result);
    } else {
        printf("  FAILED: Error code %ld\n", result);
    }
}

void demo_generate_events(void) {
    print_banner("Demo 3: Generating Audit Events");

    printf("Performing operations that generate audit events...\n\n");

    // File operations
    printf("1. File operations:\n");
    const char* test_file = "/tmp/audit_demo_file.txt";

    printf("   - Creating file: %s\n", test_file);
    int fd = open(test_file, O_CREAT | O_WRONLY, 0644);
    if (fd >= 0) {
        printf("   - Writing to file\n");
        write(fd, "test data\n", 10);
        close(fd);

        printf("   - Reading file\n");
        fd = open(test_file, O_RDONLY);
        if (fd >= 0) {
            char buf[64];
            read(fd, buf, sizeof(buf));
            close(fd);
        }

        printf("   - Deleting file\n");
        unlink(test_file);
    }

    // Process operations
    printf("\n2. Process operations:\n");
    printf("   - Forking process\n");
    pid_t pid = fork();
    if (pid == 0) {
        // Child process
        printf("   - Child process executing /bin/echo\n");
        execlp("/bin/echo", "echo", "Audit demo child process", NULL);
        exit(0);
    } else if (pid > 0) {
        // Parent process
        wait(NULL);
        printf("   - Child process completed\n");
    }

    // Network operations (will fail without privileges, but generates events)
    printf("\n3. Network operations (may fail, but generates events):\n");
    printf("   - Attempting to bind to privileged port 80\n");
    printf("     (Expected to fail and generate security violation event)\n");

    // This will fail and generate AUDIT_SECURITY_CAP_DENIED event
    // (Commented out to avoid actual permission errors in demo)
    // int sock = socket(AF_INET, SOCK_STREAM, 0);
    // if (sock >= 0) {
    //     struct sockaddr_in addr;
    //     addr.sin_family = AF_INET;
    //     addr.sin_port = htons(80);
    //     addr.sin_addr.s_addr = INADDR_ANY;
    //     bind(sock, (struct sockaddr*)&addr, sizeof(addr));
    //     close(sock);
    // }

    printf("\nEvents generated! Use 'ausearch' to view them.\n");
}

void demo_read_events(void) {
    print_banner("Demo 4: Reading Audit Events");

    printf("Reading recent audit events...\n\n");

    // Read up to 20 events
    audit_event_t events[20];
    long count = syscall3(SYS_AUDIT_READ, (long)events, 20, 0);

    if (count < 0) {
        printf("FAILED: Could not read audit log (error: %ld)\n", count);
        return;
    }

    printf("Read %ld events from audit log\n\n", count);

    // Display events
    for (long i = 0; i < count && i < 10; i++) {
        audit_event_t* e = &events[i];

        printf("Event %ld:\n", i + 1);
        printf("  Sequence: %llu\n", (unsigned long long)e->sequence);
        printf("  Type: %d\n", e->type);
        printf("  Result: %d (", e->result);
        switch (e->result) {
            case AUDIT_SUCCESS: printf("SUCCESS"); break;
            case AUDIT_FAILURE: printf("FAILURE"); break;
            case AUDIT_DENIED: printf("DENIED"); break;
            default: printf("UNKNOWN");
        }
        printf(")\n");
        printf("  PID: %u\n", e->pid);
        printf("  UID: %u\n", e->uid);
        if (e->comm[0]) {
            printf("  Command: %s\n", e->comm);
        }
        if (e->path[0]) {
            printf("  Path: %s\n", e->path);
        }
        printf("\n");
    }

    if (count > 10) {
        printf("... and %ld more events (use ausearch for full list)\n", count - 10);
    }
}

void demo_statistics(void) {
    print_banner("Demo 5: Audit Statistics");

    printf("Retrieving audit statistics...\n\n");

    uint64_t total = 0;
    uint64_t dropped = 0;

    long result = syscall3(SYS_AUDIT_GET_STATS,
                          (long)&total,
                          (long)&dropped,
                          0);

    if (result != 0) {
        printf("FAILED: Could not retrieve statistics (error: %ld)\n", result);
        return;
    }

    printf("Audit Statistics:\n");
    printf("  Total events logged:  %llu\n", (unsigned long long)total);
    printf("  Events dropped:       %llu\n", (unsigned long long)dropped);

    if (total > 0) {
        double drop_rate = (double)dropped / total * 100.0;
        printf("  Drop rate:            %.2f%%\n", drop_rate);
    }

    if (dropped > 0) {
        printf("\n  WARNING: Events were dropped due to buffer overflow!\n");
        printf("  Consider increasing buffer size or filtering events.\n");
    } else {
        printf("\n  All events successfully logged (no drops)\n");
    }
}

void demo_command_line_tools(void) {
    print_banner("Demo 6: Command Line Tools");

    printf("The audit subsystem includes three command-line tools:\n\n");

    printf("1. auditctl - Audit control and rule management\n");
    printf("   Examples:\n");
    printf("     auditctl enable                    # Enable auditing\n");
    printf("     auditctl status                    # Show status\n");
    printf("     auditctl add -t 5000 -A alert      # Alert on cap denied\n");
    printf("     auditctl stats                     # Show statistics\n\n");

    printf("2. ausearch - Search audit logs\n");
    printf("   Examples:\n");
    printf("     ausearch -t 5000                   # Security violations\n");
    printf("     ausearch -u 1000 -r failure        # Failed ops by UID 1000\n");
    printf("     ausearch -f /etc/*                 # /etc access\n");
    printf("     ausearch -c sshd -v                # sshd events (verbose)\n\n");

    printf("3. aureport - Generate audit reports\n");
    printf("   Examples:\n");
    printf("     aureport --summary                 # Summary report\n");
    printf("     aureport --security                # Security violations\n");
    printf("     aureport --failed                  # Failed operations\n");
    printf("     aureport --auth                    # Authentication events\n\n");

    printf("Try running these commands to explore the audit logs!\n");
}

int main(void) {
    printf("\n");
    printf("════════════════════════════════════════════════════════════\n");
    printf("           AutomationOS Audit Logging Demo\n");
    printf("════════════════════════════════════════════════════════════\n");
    printf("\n");
    printf("This demo shows the audit logging capabilities of AutomationOS.\n");
    printf("Audit logging tracks security-relevant events for compliance\n");
    printf("and forensic analysis.\n");

    // Run demos
    demo_enable_disable();
    sleep(1);

    demo_add_rules();
    sleep(1);

    demo_generate_events();
    sleep(2);  // Wait for events to be logged

    demo_read_events();
    sleep(1);

    demo_statistics();
    sleep(1);

    demo_command_line_tools();

    printf("\n");
    printf("════════════════════════════════════════════════════════════\n");
    printf("                    Demo Complete!\n");
    printf("════════════════════════════════════════════════════════════\n");
    printf("\n");
    printf("Next steps:\n");
    printf("  1. Run 'auditctl status' to check audit status\n");
    printf("  2. Run 'ausearch' to search audit logs\n");
    printf("  3. Run 'aureport --summary' for a summary report\n");
    printf("  4. See kernel/audit/README.md for full documentation\n");
    printf("\n");

    return 0;
}
