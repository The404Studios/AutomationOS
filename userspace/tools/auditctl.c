/*
 * auditctl - Audit control utility
 *
 * Manages audit rules, enables/disables auditing, and configures
 * audit system parameters.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

// Syscall numbers (placeholder - actual numbers from kernel)
#define SYS_AUDIT_ENABLE 200
#define SYS_AUDIT_DISABLE 201
#define SYS_AUDIT_READ 202
#define SYS_AUDIT_RULE_ADD 203
#define SYS_AUDIT_RULE_DEL 204
#define SYS_AUDIT_GET_STATS 205

// Import audit structures from kernel header
#include "../../kernel/include/audit.h"

// Function prototypes
void print_usage(const char* prog);
int cmd_enable(void);
int cmd_disable(void);
int cmd_status(void);
int cmd_list_rules(void);
int cmd_add_rule(int argc, char** argv);
int cmd_delete_rule(uint32_t rule_id);
int cmd_stats(void);

// Syscall wrappers
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

int main(int argc, char** argv) {
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    const char* cmd = argv[1];

    if (strcmp(cmd, "enable") == 0 || strcmp(cmd, "-e") == 0) {
        return cmd_enable();
    }
    else if (strcmp(cmd, "disable") == 0 || strcmp(cmd, "-d") == 0) {
        return cmd_disable();
    }
    else if (strcmp(cmd, "status") == 0 || strcmp(cmd, "-s") == 0) {
        return cmd_status();
    }
    else if (strcmp(cmd, "list") == 0 || strcmp(cmd, "-l") == 0) {
        return cmd_list_rules();
    }
    else if (strcmp(cmd, "add") == 0 || strcmp(cmd, "-a") == 0) {
        return cmd_add_rule(argc - 2, argv + 2);
    }
    else if (strcmp(cmd, "delete") == 0 || strcmp(cmd, "-D") == 0) {
        if (argc < 3) {
            printf("Error: delete requires rule ID\n");
            return 1;
        }
        uint32_t rule_id = atoi(argv[2]);
        return cmd_delete_rule(rule_id);
    }
    else if (strcmp(cmd, "stats") == 0) {
        return cmd_stats();
    }
    else {
        printf("Unknown command: %s\n", cmd);
        print_usage(argv[0]);
        return 1;
    }

    return 0;
}

void print_usage(const char* prog) {
    printf("Usage: %s <command> [options]\n\n", prog);
    printf("Commands:\n");
    printf("  enable, -e              Enable audit logging\n");
    printf("  disable, -d             Disable audit logging\n");
    printf("  status, -s              Show audit status\n");
    printf("  list, -l                List all audit rules\n");
    printf("  add, -a <options>       Add audit rule\n");
    printf("  delete, -D <id>         Delete audit rule by ID\n");
    printf("  stats                   Show audit statistics\n");
    printf("\n");
    printf("Add rule options:\n");
    printf("  -t <type>               Event type to match\n");
    printf("  -u <uid>                User ID to match\n");
    printf("  -p <pid>                Process ID to match\n");
    printf("  -f <path>               File path pattern to match\n");
    printf("  -A <action>             Action: log, ignore, alert\n");
    printf("\n");
    printf("Examples:\n");
    printf("  %s enable\n", prog);
    printf("  %s add -t 5000 -A alert     # Alert on capability denied\n", prog);
    printf("  %s add -u 1000 -A ignore    # Ignore events from UID 1000\n", prog);
    printf("  %s add -f /etc/* -A log     # Log all /etc access\n", prog);
    printf("  %s list\n", prog);
    printf("  %s delete 5\n", prog);
}

int cmd_enable(void) {
    long result = syscall3(SYS_AUDIT_ENABLE, 0, 0, 0);
    if (result == 0) {
        printf("Audit logging enabled\n");
        return 0;
    } else {
        printf("Failed to enable audit logging (error: %ld)\n", result);
        return 1;
    }
}

int cmd_disable(void) {
    long result = syscall3(SYS_AUDIT_DISABLE, 0, 0, 0);
    if (result == 0) {
        printf("Audit logging disabled\n");
        return 0;
    } else {
        printf("Failed to disable audit logging (error: %ld)\n", result);
        return 1;
    }
}

int cmd_status(void) {
    uint64_t total = 0;
    uint64_t dropped = 0;

    long result = syscall3(SYS_AUDIT_GET_STATS,
                          (long)&total, (long)&dropped, 0);

    if (result == 0) {
        printf("Audit Status:\n");
        printf("  Total events: %llu\n", (unsigned long long)total);
        printf("  Dropped events: %llu\n", (unsigned long long)dropped);

        if (total > 0) {
            double drop_rate = (double)dropped / total * 100.0;
            printf("  Drop rate: %.2f%%\n", drop_rate);
        }

        return 0;
    } else {
        printf("Failed to get audit status (error: %ld)\n", result);
        return 1;
    }
}

int cmd_list_rules(void) {
    // TODO: Implement rule listing
    // Need to add syscall to read rule list from kernel

    printf("List rules: Not yet implemented\n");
    printf("(Requires SYS_AUDIT_RULE_LIST syscall)\n");

    return 0;
}

int cmd_add_rule(int argc, char** argv) {
    audit_rule_t rule;
    memset(&rule, 0, sizeof(rule));

    rule.id = 0;  // Auto-assign
    rule.enabled = true;
    rule.action = AUDIT_ACTION_LOG;  // Default action
    rule.filter_type = AUDIT_FILTER_TYPE;  // Default filter

    // Parse arguments
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "-t") == 0 && i + 1 < argc) {
            // Event type filter
            rule.filter_type = AUDIT_FILTER_TYPE;
            rule.criteria.event_type = atoi(argv[++i]);
        }
        else if (strcmp(argv[i], "-u") == 0 && i + 1 < argc) {
            // UID filter
            rule.filter_type = AUDIT_FILTER_UID;
            rule.criteria.uid = atoi(argv[++i]);
        }
        else if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
            // PID filter
            rule.filter_type = AUDIT_FILTER_PID;
            rule.criteria.pid = atoi(argv[++i]);
        }
        else if (strcmp(argv[i], "-f") == 0 && i + 1 < argc) {
            // Path filter
            rule.filter_type = AUDIT_FILTER_PATH;
            strncpy(rule.criteria.path_pattern, argv[++i], AUDIT_PATH_MAX - 1);
            rule.criteria.path_pattern[AUDIT_PATH_MAX - 1] = '\0';
        }
        else if (strcmp(argv[i], "-A") == 0 && i + 1 < argc) {
            // Action
            const char* action = argv[++i];
            if (strcmp(action, "log") == 0) {
                rule.action = AUDIT_ACTION_LOG;
            } else if (strcmp(action, "ignore") == 0) {
                rule.action = AUDIT_ACTION_IGNORE;
            } else if (strcmp(action, "alert") == 0) {
                rule.action = AUDIT_ACTION_ALERT;
            } else {
                printf("Unknown action: %s\n", action);
                return 1;
            }
        }
    }

    // Add rule via syscall
    long result = syscall3(SYS_AUDIT_RULE_ADD, (long)&rule, 0, 0);

    if (result >= 0) {
        printf("Rule added successfully (ID: %ld)\n", result);
        return 0;
    } else {
        printf("Failed to add rule (error: %ld)\n", result);
        return 1;
    }
}

int cmd_delete_rule(uint32_t rule_id) {
    long result = syscall3(SYS_AUDIT_RULE_DEL, rule_id, 0, 0);

    if (result == 0) {
        printf("Rule %u deleted successfully\n", rule_id);
        return 0;
    } else {
        printf("Failed to delete rule %u (error: %ld)\n", rule_id, result);
        return 1;
    }
}

int cmd_stats(void) {
    uint64_t total = 0;
    uint64_t dropped = 0;

    long result = syscall3(SYS_AUDIT_GET_STATS,
                          (long)&total, (long)&dropped, 0);

    if (result == 0) {
        printf("Audit Statistics:\n");
        printf("=====================================\n");
        printf("  Total events logged:     %llu\n", (unsigned long long)total);
        printf("  Events dropped:          %llu\n", (unsigned long long)dropped);

        if (total > 0) {
            printf("  Drop rate:               %.2f%%\n",
                   (double)dropped / total * 100.0);
        }

        printf("\n");
        printf("Use 'ausearch' to search audit logs\n");
        printf("Use 'aureport' to generate reports\n");

        return 0;
    } else {
        printf("Failed to get audit statistics (error: %ld)\n", result);
        return 1;
    }
}
