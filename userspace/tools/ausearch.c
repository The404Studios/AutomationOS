/*
 * ausearch - Audit log search utility
 *
 * Searches audit logs with various filters and displays matching events.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <time.h>

// Syscall numbers
#define SYS_AUDIT_READ 202

// Import audit structures
#include "../../kernel/include/audit.h"

// Search filters
typedef struct {
    bool filter_type;
    audit_event_type_t event_type;

    bool filter_uid;
    uint32_t uid;

    bool filter_pid;
    uint32_t pid;

    bool filter_result;
    audit_result_t result;

    bool filter_path;
    char path_pattern[AUDIT_PATH_MAX];

    bool filter_comm;
    char comm[AUDIT_COMM_MAX];

    uint32_t max_results;
    bool verbose;
} search_filter_t;

// Function prototypes
void print_usage(const char* prog);
int search_audit_log(search_filter_t* filter);
void print_event(audit_event_t* event, bool verbose);
const char* event_type_to_string(audit_event_type_t type);
const char* result_to_string(audit_result_t result);
bool event_matches_filter(audit_event_t* event, search_filter_t* filter);
bool simple_glob_match(const char* text, const char* pattern);

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
    search_filter_t filter;
    memset(&filter, 0, sizeof(filter));

    filter.max_results = 100;  // Default: show 100 results
    filter.verbose = false;

    // Parse command line arguments
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-t") == 0 && i + 1 < argc) {
            // Event type filter
            filter.filter_type = true;
            filter.event_type = atoi(argv[++i]);
        }
        else if (strcmp(argv[i], "-u") == 0 && i + 1 < argc) {
            // UID filter
            filter.filter_uid = true;
            filter.uid = atoi(argv[++i]);
        }
        else if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
            // PID filter
            filter.filter_pid = true;
            filter.pid = atoi(argv[++i]);
        }
        else if (strcmp(argv[i], "-r") == 0 && i + 1 < argc) {
            // Result filter
            filter.filter_result = true;
            const char* result = argv[++i];
            if (strcmp(result, "success") == 0) {
                filter.result = AUDIT_SUCCESS;
            } else if (strcmp(result, "failure") == 0) {
                filter.result = AUDIT_FAILURE;
            } else if (strcmp(result, "denied") == 0) {
                filter.result = AUDIT_DENIED;
            } else {
                printf("Unknown result: %s\n", result);
                return 1;
            }
        }
        else if (strcmp(argv[i], "-f") == 0 && i + 1 < argc) {
            // Path filter
            filter.filter_path = true;
            strncpy(filter.path_pattern, argv[++i], AUDIT_PATH_MAX - 1);
            filter.path_pattern[AUDIT_PATH_MAX - 1] = '\0';
        }
        else if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) {
            // Command filter
            filter.filter_comm = true;
            strncpy(filter.comm, argv[++i], AUDIT_COMM_MAX - 1);
            filter.comm[AUDIT_COMM_MAX - 1] = '\0';
        }
        else if (strcmp(argv[i], "-n") == 0 && i + 1 < argc) {
            // Max results
            filter.max_results = atoi(argv[++i]);
        }
        else if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--verbose") == 0) {
            // Verbose output
            filter.verbose = true;
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

    return search_audit_log(&filter);
}

void print_usage(const char* prog) {
    printf("Usage: %s [options]\n\n", prog);
    printf("Options:\n");
    printf("  -t <type>         Filter by event type\n");
    printf("  -u <uid>          Filter by user ID\n");
    printf("  -p <pid>          Filter by process ID\n");
    printf("  -r <result>       Filter by result (success/failure/denied)\n");
    printf("  -f <path>         Filter by file path pattern\n");
    printf("  -c <comm>         Filter by command name\n");
    printf("  -n <count>        Maximum number of results (default: 100)\n");
    printf("  -v, --verbose     Verbose output\n");
    printf("  -h, --help        Show this help message\n");
    printf("\n");
    printf("Examples:\n");
    printf("  %s -t 5000                    # Capability denied events\n", prog);
    printf("  %s -u 1000 -r failure         # Failed operations by UID 1000\n", prog);
    printf("  %s -f /etc/*                  # All /etc access\n", prog);
    printf("  %s -c sshd -v                 # All events from sshd (verbose)\n", prog);
}

int search_audit_log(search_filter_t* filter) {
    // Allocate buffer for reading events
    audit_event_t* events = malloc(sizeof(audit_event_t) * filter->max_results);
    if (!events) {
        printf("Failed to allocate memory\n");
        return 1;
    }

    // Read events from kernel
    long count = syscall3(SYS_AUDIT_READ,
                         (long)events,
                         filter->max_results,
                         0);

    if (count < 0) {
        printf("Failed to read audit log (error: %ld)\n", count);
        free(events);
        return 1;
    }

    printf("Read %ld events from audit log\n\n", count);

    // Filter and display events
    uint32_t match_count = 0;

    for (long i = 0; i < count; i++) {
        if (event_matches_filter(&events[i], filter)) {
            print_event(&events[i], filter->verbose);
            match_count++;
        }
    }

    printf("\n%u matching events found\n", match_count);

    free(events);
    return 0;
}

bool event_matches_filter(audit_event_t* event, search_filter_t* filter) {
    if (filter->filter_type && event->type != filter->event_type) {
        return false;
    }

    if (filter->filter_uid && event->uid != filter->uid) {
        return false;
    }

    if (filter->filter_pid && event->pid != filter->pid) {
        return false;
    }

    if (filter->filter_result && event->result != filter->result) {
        return false;
    }

    if (filter->filter_path && event->path[0] != '\0') {
        if (!simple_glob_match(event->path, filter->path_pattern)) {
            return false;
        }
    }

    if (filter->filter_comm && event->comm[0] != '\0') {
        if (strcmp(event->comm, filter->comm) != 0) {
            return false;
        }
    }

    return true;
}

void print_event(audit_event_t* event, bool verbose) {
    // Format timestamp (simplified - just sequence for now)
    printf("[%llu] ", (unsigned long long)event->sequence);

    // Event type
    printf("%s ", event_type_to_string(event->type));

    // Result
    printf("result=%s ", result_to_string(event->result));

    // Subject
    printf("pid=%u uid=%u", event->pid, event->uid);

    if (event->comm[0] != '\0') {
        printf(" comm=\"%s\"", event->comm);
    }

    // Object
    if (event->path[0] != '\0') {
        printf(" path=\"%s\"", event->path);
    }

    if (event->object_pid != 0) {
        printf(" target_pid=%u", event->object_pid);
    }

    printf("\n");

    // Verbose details
    if (verbose) {
        printf("  Timestamp: %llu\n", (unsigned long long)event->timestamp);
        printf("  Syscall: %u\n", event->syscall);
        printf("  Error code: %d\n", event->error_code);
        printf("  Flags: 0x%llx\n", (unsigned long long)event->flags);
        printf("  Hash: 0x%llx (prev: 0x%llx)\n",
               (unsigned long long)event->hash,
               (unsigned long long)event->prev_hash);
        printf("\n");
    }
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
        case AUDIT_NET_LISTEN: return "NET_LISTEN";
        case AUDIT_SECURITY_CAP_DENIED: return "CAP_DENIED";
        case AUDIT_SECURITY_MAC_DENIED: return "MAC_DENIED";
        case AUDIT_SECURITY_SANDBOX_VIOLATION: return "SANDBOX_VIOLATION";
        case AUDIT_KERNEL_MODULE_LOAD: return "MODULE_LOAD";
        case AUDIT_KERNEL_MODULE_UNLOAD: return "MODULE_UNLOAD";
        case AUDIT_KERNEL_PANIC: return "KERNEL_PANIC";
        case AUDIT_KERNEL_BOOT: return "KERNEL_BOOT";
        case AUDIT_SYSTEM_SHUTDOWN: return "SYSTEM_SHUTDOWN";
        default: return "UNKNOWN";
    }
}

const char* result_to_string(audit_result_t result) {
    switch (result) {
        case AUDIT_SUCCESS: return "SUCCESS";
        case AUDIT_FAILURE: return "FAILURE";
        case AUDIT_DENIED: return "DENIED";
        case AUDIT_ERROR: return "ERROR";
        default: return "UNKNOWN";
    }
}

bool simple_glob_match(const char* text, const char* pattern) {
    if (!text || !pattern) return false;

    while (*pattern) {
        if (*pattern == '*') {
            pattern++;
            if (!*pattern) return true;

            while (*text) {
                if (simple_glob_match(text, pattern)) {
                    return true;
                }
                text++;
            }
            return false;
        } else if (*pattern == *text) {
            pattern++;
            text++;
        } else {
            return false;
        }
    }

    return *text == '\0';
}
