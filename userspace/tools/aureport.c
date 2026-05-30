/*
 * aureport - Audit report generator
 *
 * Generates summary reports and statistics from audit logs.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

// Syscall numbers
#define SYS_AUDIT_READ 202
#define SYS_AUDIT_GET_STATS 205

// Import audit structures
#include "../../kernel/include/audit.h"

// Report types
typedef enum {
    REPORT_SUMMARY,
    REPORT_AUTH,
    REPORT_FILE,
    REPORT_PROCESS,
    REPORT_NETWORK,
    REPORT_SECURITY,
    REPORT_FAILED
} report_type_t;

// Statistics structures
typedef struct {
    uint32_t total_events;
    uint32_t success_events;
    uint32_t failed_events;
    uint32_t denied_events;
} event_stats_t;

typedef struct {
    audit_event_type_t type;
    uint32_t count;
} type_count_t;

// Function prototypes
void print_usage(const char* prog);
int generate_report(report_type_t type);
int report_summary(audit_event_t* events, uint32_t count);
int report_auth(audit_event_t* events, uint32_t count);
int report_file(audit_event_t* events, uint32_t count);
int report_process(audit_event_t* events, uint32_t count);
int report_network(audit_event_t* events, uint32_t count);
int report_security(audit_event_t* events, uint32_t count);
int report_failed(audit_event_t* events, uint32_t count);
const char* event_type_to_string(audit_event_type_t type);

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

int main(int argc, char** argv) {
    report_type_t report_type = REPORT_SUMMARY;

    // Parse command line arguments
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--summary") == 0 || strcmp(argv[i], "-s") == 0) {
            report_type = REPORT_SUMMARY;
        }
        else if (strcmp(argv[i], "--auth") == 0) {
            report_type = REPORT_AUTH;
        }
        else if (strcmp(argv[i], "--file") == 0) {
            report_type = REPORT_FILE;
        }
        else if (strcmp(argv[i], "--process") == 0) {
            report_type = REPORT_PROCESS;
        }
        else if (strcmp(argv[i], "--network") == 0) {
            report_type = REPORT_NETWORK;
        }
        else if (strcmp(argv[i], "--security") == 0 || strcmp(argv[i], "-x") == 0) {
            report_type = REPORT_SECURITY;
        }
        else if (strcmp(argv[i], "--failed") == 0 || strcmp(argv[i], "-f") == 0) {
            report_type = REPORT_FAILED;
        }
        else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        }
        else {
            printf("Unknown option: %s\n", argv[i]);
            print_usage(argv[0]);
            return 1;
        }
    }

    return generate_report(report_type);
}

void print_usage(const char* prog) {
    printf("Usage: %s [report_type]\n\n", prog);
    printf("Report Types:\n");
    printf("  --summary, -s     Summary report (default)\n");
    printf("  --auth            Authentication report\n");
    printf("  --file            File access report\n");
    printf("  --process         Process events report\n");
    printf("  --network         Network events report\n");
    printf("  --security, -x    Security violations report\n");
    printf("  --failed, -f      Failed operations report\n");
    printf("\n");
    printf("Examples:\n");
    printf("  %s --summary\n", prog);
    printf("  %s --security\n", prog);
    printf("  %s --failed\n", prog);
}

int generate_report(report_type_t type) {
    // Allocate buffer for events (read up to 10000 events)
    uint32_t max_events = 10000;
    audit_event_t* events = malloc(sizeof(audit_event_t) * max_events);
    if (!events) {
        printf("Failed to allocate memory\n");
        return 1;
    }

    // Read events from kernel
    long count = syscall3(SYS_AUDIT_READ,
                         (long)events,
                         max_events,
                         0);

    if (count < 0) {
        printf("Failed to read audit log (error: %ld)\n", count);
        free(events);
        return 1;
    }

    printf("Audit Report\n");
    printf("=============================================================================\n");
    printf("Events analyzed: %ld\n\n", count);

    int result = 0;

    switch (type) {
        case REPORT_SUMMARY:
            result = report_summary(events, count);
            break;
        case REPORT_AUTH:
            result = report_auth(events, count);
            break;
        case REPORT_FILE:
            result = report_file(events, count);
            break;
        case REPORT_PROCESS:
            result = report_process(events, count);
            break;
        case REPORT_NETWORK:
            result = report_network(events, count);
            break;
        case REPORT_SECURITY:
            result = report_security(events, count);
            break;
        case REPORT_FAILED:
            result = report_failed(events, count);
            break;
    }

    free(events);
    return result;
}

int report_summary(audit_event_t* events, uint32_t count) {
    event_stats_t stats = {0};

    // Count event types
    #define MAX_TYPES 100
    type_count_t type_counts[MAX_TYPES] = {0};
    uint32_t unique_types = 0;

    for (uint32_t i = 0; i < count; i++) {
        audit_event_t* e = &events[i];
        stats.total_events++;

        switch (e->result) {
            case AUDIT_SUCCESS:
                stats.success_events++;
                break;
            case AUDIT_FAILURE:
            case AUDIT_ERROR:
                stats.failed_events++;
                break;
            case AUDIT_DENIED:
                stats.denied_events++;
                break;
        }

        // Count by type
        bool found = false;
        for (uint32_t j = 0; j < unique_types; j++) {
            if (type_counts[j].type == e->type) {
                type_counts[j].count++;
                found = true;
                break;
            }
        }

        if (!found && unique_types < MAX_TYPES) {
            type_counts[unique_types].type = e->type;
            type_counts[unique_types].count = 1;
            unique_types++;
        }
    }

    // Print summary
    printf("Event Summary:\n");
    printf("  Total events:         %u\n", stats.total_events);
    printf("  Successful:           %u (%.1f%%)\n",
           stats.success_events,
           stats.total_events > 0 ? (float)stats.success_events / stats.total_events * 100 : 0);
    printf("  Failed:               %u (%.1f%%)\n",
           stats.failed_events,
           stats.total_events > 0 ? (float)stats.failed_events / stats.total_events * 100 : 0);
    printf("  Denied:               %u (%.1f%%)\n",
           stats.denied_events,
           stats.total_events > 0 ? (float)stats.denied_events / stats.total_events * 100 : 0);
    printf("\n");

    printf("Top Event Types:\n");
    // Simple bubble sort to show top types
    for (uint32_t i = 0; i < unique_types - 1; i++) {
        for (uint32_t j = 0; j < unique_types - i - 1; j++) {
            if (type_counts[j].count < type_counts[j + 1].count) {
                type_count_t temp = type_counts[j];
                type_counts[j] = type_counts[j + 1];
                type_counts[j + 1] = temp;
            }
        }
    }

    for (uint32_t i = 0; i < 10 && i < unique_types; i++) {
        printf("  %3u. %-30s %u events\n",
               i + 1,
               event_type_to_string(type_counts[i].type),
               type_counts[i].count);
    }

    return 0;
}

int report_auth(audit_event_t* events, uint32_t count) {
    uint32_t login_count = 0;
    uint32_t logout_count = 0;
    uint32_t failed_count = 0;

    printf("Authentication Events:\n");
    printf("-----------------------------------------------------------------------------\n");

    for (uint32_t i = 0; i < count; i++) {
        audit_event_t* e = &events[i];

        if (e->type >= AUDIT_AUTH_LOGIN && e->type <= AUDIT_AUTH_FAILED) {
            switch (e->type) {
                case AUDIT_AUTH_LOGIN:
                    login_count++;
                    printf("[%llu] LOGIN  uid=%u pid=%u comm=\"%s\"\n",
                           (unsigned long long)e->sequence, e->uid, e->pid, e->comm);
                    break;
                case AUDIT_AUTH_LOGOUT:
                    logout_count++;
                    printf("[%llu] LOGOUT uid=%u pid=%u comm=\"%s\"\n",
                           (unsigned long long)e->sequence, e->uid, e->pid, e->comm);
                    break;
                case AUDIT_AUTH_FAILED:
                    failed_count++;
                    printf("[%llu] FAILED uid=%u pid=%u comm=\"%s\"\n",
                           (unsigned long long)e->sequence, e->uid, e->pid, e->comm);
                    break;
            }
        }
    }

    printf("\nSummary:\n");
    printf("  Logins:          %u\n", login_count);
    printf("  Logouts:         %u\n", logout_count);
    printf("  Failed attempts: %u\n", failed_count);

    return 0;
}

int report_security(audit_event_t* events, uint32_t count) {
    uint32_t cap_denied = 0;
    uint32_t mac_denied = 0;
    uint32_t sandbox_violations = 0;

    printf("Security Violations:\n");
    printf("-----------------------------------------------------------------------------\n");

    for (uint32_t i = 0; i < count; i++) {
        audit_event_t* e = &events[i];

        if (e->type >= AUDIT_SECURITY_CAP_DENIED &&
            e->type <= AUDIT_SECURITY_PRIVILEGE_ESCALATION) {

            printf("[%llu] ", (unsigned long long)e->sequence);

            switch (e->type) {
                case AUDIT_SECURITY_CAP_DENIED:
                    cap_denied++;
                    printf("CAP_DENIED");
                    break;
                case AUDIT_SECURITY_MAC_DENIED:
                    mac_denied++;
                    printf("MAC_DENIED");
                    break;
                case AUDIT_SECURITY_SANDBOX_VIOLATION:
                    sandbox_violations++;
                    printf("SANDBOX_VIOLATION");
                    break;
                default:
                    printf("SECURITY_VIOLATION");
            }

            printf(" uid=%u pid=%u comm=\"%s\"", e->uid, e->pid, e->comm);
            if (e->path[0]) {
                printf(" path=\"%s\"", e->path);
            }
            printf("\n");
        }
    }

    printf("\nSummary:\n");
    printf("  Capability denied:    %u\n", cap_denied);
    printf("  MAC denied:           %u\n", mac_denied);
    printf("  Sandbox violations:   %u\n", sandbox_violations);

    if (cap_denied + mac_denied + sandbox_violations > 0) {
        printf("\nWARNING: Security violations detected!\n");
    }

    return 0;
}

int report_failed(audit_event_t* events, uint32_t count) {
    printf("Failed Operations:\n");
    printf("-----------------------------------------------------------------------------\n");

    uint32_t failed_count = 0;

    for (uint32_t i = 0; i < count; i++) {
        audit_event_t* e = &events[i];

        if (e->result == AUDIT_FAILURE || e->result == AUDIT_DENIED) {
            failed_count++;
            printf("[%llu] %s result=%s uid=%u pid=%u comm=\"%s\"",
                   (unsigned long long)e->sequence,
                   event_type_to_string(e->type),
                   e->result == AUDIT_FAILURE ? "FAILURE" : "DENIED",
                   e->uid, e->pid, e->comm);

            if (e->path[0]) {
                printf(" path=\"%s\"", e->path);
            }

            if (e->error_code != 0) {
                printf(" errno=%d", e->error_code);
            }

            printf("\n");
        }
    }

    printf("\nTotal failed operations: %u\n", failed_count);

    return 0;
}

int report_file(audit_event_t* events, uint32_t count) {
    printf("File Access Events:\n");
    printf("-----------------------------------------------------------------------------\n");

    uint32_t file_count = 0;

    for (uint32_t i = 0; i < count; i++) {
        audit_event_t* e = &events[i];

        if (e->type >= AUDIT_FILE_OPEN && e->type <= AUDIT_FILE_RENAME) {
            file_count++;
            if (file_count <= 100) {  // Limit output
                printf("[%llu] %s path=\"%s\" uid=%u pid=%u\n",
                       (unsigned long long)e->sequence,
                       event_type_to_string(e->type),
                       e->path, e->uid, e->pid);
            }
        }
    }

    printf("\nTotal file access events: %u\n", file_count);

    return 0;
}

int report_process(audit_event_t* events, uint32_t count) {
    printf("Process Events:\n");
    printf("-----------------------------------------------------------------------------\n");

    uint32_t proc_count = 0;

    for (uint32_t i = 0; i < count; i++) {
        audit_event_t* e = &events[i];

        if (e->type >= AUDIT_PROC_EXEC && e->type <= AUDIT_PROC_SETGID) {
            proc_count++;
            if (proc_count <= 100) {
                printf("[%llu] %s pid=%u uid=%u comm=\"%s\"\n",
                       (unsigned long long)e->sequence,
                       event_type_to_string(e->type),
                       e->pid, e->uid, e->comm);
            }
        }
    }

    printf("\nTotal process events: %u\n", proc_count);

    return 0;
}

int report_network(audit_event_t* events, uint32_t count) {
    printf("Network Events:\n");
    printf("-----------------------------------------------------------------------------\n");

    uint32_t net_count = 0;

    for (uint32_t i = 0; i < count; i++) {
        audit_event_t* e = &events[i];

        if (e->type >= AUDIT_NET_CONNECT && e->type <= AUDIT_NET_RECV) {
            net_count++;
            if (net_count <= 100) {
                printf("[%llu] %s uid=%u pid=%u comm=\"%s\"",
                       (unsigned long long)e->sequence,
                       event_type_to_string(e->type),
                       e->uid, e->pid, e->comm);

                if (e->path[0]) {
                    printf(" host=\"%s\" port=%llu",
                           e->path, (unsigned long long)e->flags);
                }
                printf("\n");
            }
        }
    }

    printf("\nTotal network events: %u\n", net_count);

    return 0;
}

const char* event_type_to_string(audit_event_type_t type) {
    switch (type) {
        case AUDIT_AUTH_LOGIN: return "AUTH_LOGIN";
        case AUDIT_AUTH_LOGOUT: return "AUTH_LOGOUT";
        case AUDIT_AUTH_FAILED: return "AUTH_FAILED";
        case AUDIT_FILE_OPEN: return "FILE_OPEN";
        case AUDIT_FILE_READ: return "FILE_READ";
        case AUDIT_FILE_WRITE: return "FILE_WRITE";
        case AUDIT_FILE_DELETE: return "FILE_DELETE";
        case AUDIT_PROC_EXEC: return "PROC_EXEC";
        case AUDIT_PROC_FORK: return "PROC_FORK";
        case AUDIT_PROC_EXIT: return "PROC_EXIT";
        case AUDIT_PROC_KILL: return "PROC_KILL";
        case AUDIT_NET_CONNECT: return "NET_CONNECT";
        case AUDIT_NET_BIND: return "NET_BIND";
        case AUDIT_SECURITY_CAP_DENIED: return "CAP_DENIED";
        case AUDIT_SECURITY_MAC_DENIED: return "MAC_DENIED";
        case AUDIT_SECURITY_SANDBOX_VIOLATION: return "SANDBOX_VIOLATION";
        case AUDIT_KERNEL_MODULE_LOAD: return "MODULE_LOAD";
        case AUDIT_KERNEL_PANIC: return "KERNEL_PANIC";
        case AUDIT_KERNEL_BOOT: return "KERNEL_BOOT";
        default: return "UNKNOWN";
    }
}
