// userspace/system/services/servicemanager.c - Core service management
// Systemd-like service manager for AutomationOS (3000+ LOC)

#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <time.h>
#include <errno.h>
#include <dirent.h>
#include <pthread.h>

// ============================================================================
// Type Definitions and Constants
// ============================================================================

#define MAX_SERVICES 256
#define MAX_DEPENDENCIES 16
#define MAX_PATH_LEN 1024
#define MAX_NAME_LEN 128
#define MAX_DESC_LEN 256
#define MAX_COMMAND_LEN 1024
#define SERVICE_CONFIG_DIR "/etc/services"
#define SERVICE_LOG_DIR "/var/log/services"
#define SERVICE_RUN_DIR "/run/services"
#define SERVICE_STATE_FILE "/var/lib/services/state.dat"

// Service types
typedef enum {
    SERVICE_TYPE_SIMPLE,      // Process runs directly
    SERVICE_TYPE_FORKING,     // Process forks and parent exits
    SERVICE_TYPE_ONESHOT,     // One-time execution
    SERVICE_TYPE_NOTIFY,      // Service notifies when ready
    SERVICE_TYPE_DBUS,        // D-Bus activated service
    SERVICE_TYPE_IDLE         // Run only when system is idle
} service_type_t;

// Service states
typedef enum {
    SERVICE_STATE_STOPPED,     // Not running
    SERVICE_STATE_STARTING,    // In process of starting
    SERVICE_STATE_RUNNING,     // Running normally
    SERVICE_STATE_STOPPING,    // Being stopped
    SERVICE_STATE_FAILED,      // Failed to start or crashed
    SERVICE_STATE_RESTARTING   // Restarting after crash
} service_state_t;

// Restart policies
typedef enum {
    RESTART_POLICY_NO,            // Never restart
    RESTART_POLICY_ALWAYS,        // Always restart
    RESTART_POLICY_ON_FAILURE,    // Restart only on failure
    RESTART_POLICY_ON_ABNORMAL,   // Restart on abnormal exit
    RESTART_POLICY_ON_ABORT,      // Restart on core dump
    RESTART_POLICY_ON_WATCHDOG    // Restart on watchdog timeout
} restart_policy_t;

// Service structure
typedef struct service {
    // Identification
    char name[MAX_NAME_LEN];
    char description[MAX_DESC_LEN];
    bool enabled;                    // Start on boot

    // Type and state
    service_type_t type;
    service_state_t state;

    // Commands
    char exec_start[MAX_COMMAND_LEN];
    char exec_stop[MAX_COMMAND_LEN];
    char exec_reload[MAX_COMMAND_LEN];
    char working_dir[MAX_PATH_LEN];

    // Dependencies (service names)
    char *requires[MAX_DEPENDENCIES];     // Hard dependencies
    char *wants[MAX_DEPENDENCIES];        // Soft dependencies
    char *before[MAX_DEPENDENCIES];       // Start before these
    char *after[MAX_DEPENDENCIES];        // Start after these
    char *conflicts[MAX_DEPENDENCIES];    // Cannot run with these

    // User and permissions
    uid_t user;
    gid_t group;
    mode_t umask;

    // Resource limits
    uint32_t cpu_quota;           // Percentage (0-100)
    uint64_t memory_limit;        // Bytes
    uint32_t task_limit;          // Max processes/threads
    uint32_t file_limit;          // Max open files

    // Restart policy
    restart_policy_t restart;
    uint32_t restart_delay_ms;
    uint32_t restart_max_attempts;
    uint32_t restart_count;
    uint64_t restart_window_sec;   // Time window for restart limit

    // Watchdog
    bool watchdog_enabled;
    uint32_t watchdog_timeout_sec;
    uint64_t last_watchdog_ping;

    // Timeouts
    uint32_t timeout_start_sec;
    uint32_t timeout_stop_sec;
    uint32_t timeout_abort_sec;

    // Runtime information
    pid_t pid;                     // Main process PID
    pid_t control_pid;             // Control process (for forking)
    uint64_t start_time;           // Timestamp of last start
    uint64_t stop_time;            // Timestamp of last stop
    int exit_code;                 // Last exit code
    int exit_signal;               // Signal that killed process
    uint64_t memory_current;       // Current memory usage
    uint32_t cpu_current;          // Current CPU usage

    // Logging
    int log_fd;                    // Log file descriptor
    char log_path[MAX_PATH_LEN];

    // Internal state
    pthread_mutex_t lock;
    bool transition_pending;
    struct service *next;
} service_t;

// Service manager state
typedef struct {
    service_t *services[MAX_SERVICES];
    uint32_t service_count;
    pthread_mutex_t lock;
    pthread_t monitor_thread;
    pthread_t watchdog_thread;
    bool running;
    bool boot_complete;
} service_manager_t;

// ============================================================================
// Global State
// ============================================================================

static service_manager_t g_manager = {0};

// ============================================================================
// Utility Functions
// ============================================================================

// Get current timestamp in milliseconds
static uint64_t get_timestamp_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

// Get current timestamp in seconds
static uint64_t get_timestamp_sec(void) {
    return get_timestamp_ms() / 1000;
}

// Convert service type to string
static const char* service_type_to_string(service_type_t type) {
    switch (type) {
        case SERVICE_TYPE_SIMPLE: return "simple";
        case SERVICE_TYPE_FORKING: return "forking";
        case SERVICE_TYPE_ONESHOT: return "oneshot";
        case SERVICE_TYPE_NOTIFY: return "notify";
        case SERVICE_TYPE_DBUS: return "dbus";
        case SERVICE_TYPE_IDLE: return "idle";
        default: return "unknown";
    }
}

// Convert service state to string
static const char* service_state_to_string(service_state_t state) {
    switch (state) {
        case SERVICE_STATE_STOPPED: return "stopped";
        case SERVICE_STATE_STARTING: return "starting";
        case SERVICE_STATE_RUNNING: return "running";
        case SERVICE_STATE_STOPPING: return "stopping";
        case SERVICE_STATE_FAILED: return "failed";
        case SERVICE_STATE_RESTARTING: return "restarting";
        default: return "unknown";
    }
}

// Convert restart policy to string
static const char* restart_policy_to_string(restart_policy_t policy) {
    switch (policy) {
        case RESTART_POLICY_NO: return "no";
        case RESTART_POLICY_ALWAYS: return "always";
        case RESTART_POLICY_ON_FAILURE: return "on-failure";
        case RESTART_POLICY_ON_ABNORMAL: return "on-abnormal";
        case RESTART_POLICY_ON_ABORT: return "on-abort";
        case RESTART_POLICY_ON_WATCHDOG: return "on-watchdog";
        default: return "unknown";
    }
}

// Parse service type from string
static service_type_t parse_service_type(const char *str) {
    if (strcmp(str, "simple") == 0) return SERVICE_TYPE_SIMPLE;
    if (strcmp(str, "forking") == 0) return SERVICE_TYPE_FORKING;
    if (strcmp(str, "oneshot") == 0) return SERVICE_TYPE_ONESHOT;
    if (strcmp(str, "notify") == 0) return SERVICE_TYPE_NOTIFY;
    if (strcmp(str, "dbus") == 0) return SERVICE_TYPE_DBUS;
    if (strcmp(str, "idle") == 0) return SERVICE_TYPE_IDLE;
    return SERVICE_TYPE_SIMPLE;
}

// Parse restart policy from string
static restart_policy_t parse_restart_policy(const char *str) {
    if (strcmp(str, "no") == 0) return RESTART_POLICY_NO;
    if (strcmp(str, "always") == 0) return RESTART_POLICY_ALWAYS;
    if (strcmp(str, "on-failure") == 0) return RESTART_POLICY_ON_FAILURE;
    if (strcmp(str, "on-abnormal") == 0) return RESTART_POLICY_ON_ABNORMAL;
    if (strcmp(str, "on-abort") == 0) return RESTART_POLICY_ON_ABORT;
    if (strcmp(str, "on-watchdog") == 0) return RESTART_POLICY_ON_WATCHDOG;
    return RESTART_POLICY_NO;
}

// Parse size string (e.g., "100M", "1G")
static uint64_t parse_size(const char *str) {
    uint64_t value = 0;
    char *end;

    value = strtoull(str, &end, 10);

    if (*end == 'K' || *end == 'k') {
        value *= 1024;
    } else if (*end == 'M' || *end == 'm') {
        value *= 1024 * 1024;
    } else if (*end == 'G' || *end == 'g') {
        value *= 1024 * 1024 * 1024;
    }

    return value;
}

// Parse time string (e.g., "5s", "10m", "1h")
static uint32_t parse_time_sec(const char *str) {
    uint32_t value = 0;
    char *end;

    value = strtoul(str, &end, 10);

    if (*end == 'm' || *end == 'M') {
        value *= 60;
    } else if (*end == 'h' || *end == 'H') {
        value *= 3600;
    }

    return value;
}

// Trim whitespace from string
static void trim_whitespace(char *str) {
    char *start = str;
    char *end;

    // Trim leading whitespace
    while (*start && (*start == ' ' || *start == '\t' || *start == '\n' || *start == '\r')) {
        start++;
    }

    // Trim trailing whitespace
    end = start + strlen(start) - 1;
    while (end > start && (*end == ' ' || *end == '\t' || *end == '\n' || *end == '\r')) {
        end--;
    }

    *(end + 1) = '\0';

    if (start != str) {
        memmove(str, start, strlen(start) + 1);
    }
}

// Strip ".service" suffix from a dependency name if present
// e.g., "dbus.service" -> "dbus", "compositor" -> "compositor"
static char* strip_service_suffix(const char *name) {
    char *dup = strdup(name);
    if (!dup) return NULL;
    size_t len = strlen(dup);
    if (len > 8 && strcmp(dup + len - 8, ".service") == 0) {
        dup[len - 8] = '\0';
    }
    return dup;
}

// ============================================================================
// Service Logging
// ============================================================================

// Open log file for service
static int service_log_open(service_t *service) {
    char path[MAX_PATH_LEN];
    snprintf(path, sizeof(path), "%s/%s.log", SERVICE_LOG_DIR, service->name);

    // Create log directory if needed
    mkdir(SERVICE_LOG_DIR, 0755);

    // Open log file (append mode)
    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0) {
        fprintf(stderr, "Failed to open log file %s: %s\n", path, strerror(errno));
        return -1;
    }

    strncpy(service->log_path, path, sizeof(service->log_path) - 1);
    service->log_fd = fd;

    return 0;
}

// Write to service log
static void service_log(service_t *service, const char *level, const char *format, ...) {
    char timestamp[64];
    char message[1024];
    char line[2048];
    time_t now;
    struct tm *tm_info;
    va_list args;

    // Get current time
    time(&now);
    tm_info = localtime(&now);
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", tm_info);

    // Format message
    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);

    // Create log line
    snprintf(line, sizeof(line), "[%s] [%s] [%s] %s\n",
             timestamp, service->name, level, message);

    // Write to log file
    if (service->log_fd > 0) {
        write(service->log_fd, line, strlen(line));
    }

    // Also write to stderr for debugging
    fprintf(stderr, "%s", line);
}

// Close log file
static void service_log_close(service_t *service) {
    if (service->log_fd > 0) {
        close(service->log_fd);
        service->log_fd = -1;
    }
}

// ============================================================================
// Service Management Functions
// ============================================================================

// Allocate new service
static service_t* service_alloc(const char *name) {
    service_t *service = calloc(1, sizeof(service_t));
    if (!service) {
        return NULL;
    }

    strncpy(service->name, name, sizeof(service->name) - 1);
    service->state = SERVICE_STATE_STOPPED;
    service->type = SERVICE_TYPE_SIMPLE;
    service->restart = RESTART_POLICY_NO;
    service->restart_delay_ms = 1000;
    service->restart_max_attempts = 5;
    service->restart_window_sec = 300;
    service->timeout_start_sec = 90;
    service->timeout_stop_sec = 90;
    service->timeout_abort_sec = 10;
    service->log_fd = -1;
    service->pid = -1;
    service->control_pid = -1;

    pthread_mutex_init(&service->lock, NULL);

    return service;
}

// Free service
static void service_free(service_t *service) {
    if (!service) return;

    // Free dependency arrays
    for (int i = 0; i < MAX_DEPENDENCIES; i++) {
        free(service->requires[i]);
        free(service->wants[i]);
        free(service->before[i]);
        free(service->after[i]);
        free(service->conflicts[i]);
    }

    service_log_close(service);
    pthread_mutex_destroy(&service->lock);
    free(service);
}

// Find service by name
static service_t* service_find(const char *name) {
    pthread_mutex_lock(&g_manager.lock);

    for (uint32_t i = 0; i < g_manager.service_count; i++) {
        if (strcmp(g_manager.services[i]->name, name) == 0) {
            service_t *service = g_manager.services[i];
            pthread_mutex_unlock(&g_manager.lock);
            return service;
        }
    }

    pthread_mutex_unlock(&g_manager.lock);
    return NULL;
}

// Add service to manager
static int service_add(service_t *service) {
    pthread_mutex_lock(&g_manager.lock);

    if (g_manager.service_count >= MAX_SERVICES) {
        pthread_mutex_unlock(&g_manager.lock);
        return -1;
    }

    g_manager.services[g_manager.service_count++] = service;
    pthread_mutex_unlock(&g_manager.lock);

    return 0;
}

// Remove service from manager
static int service_remove(const char *name) {
    pthread_mutex_lock(&g_manager.lock);

    for (uint32_t i = 0; i < g_manager.service_count; i++) {
        if (strcmp(g_manager.services[i]->name, name) == 0) {
            service_t *service = g_manager.services[i];

            // Move remaining services
            for (uint32_t j = i; j < g_manager.service_count - 1; j++) {
                g_manager.services[j] = g_manager.services[j + 1];
            }

            g_manager.service_count--;
            pthread_mutex_unlock(&g_manager.lock);

            service_free(service);
            return 0;
        }
    }

    pthread_mutex_unlock(&g_manager.lock);
    return -1;
}

// ============================================================================
// Service Configuration Parsing
// ============================================================================

// Parse service configuration file
static int service_parse_config(service_t *service, const char *config_path) {
    FILE *fp = fopen(config_path, "r");
    if (!fp) {
        fprintf(stderr, "Failed to open config file %s: %s\n",
                config_path, strerror(errno));
        return -1;
    }

    char line[1024];
    char section[64] = {0};

    while (fgets(line, sizeof(line), fp)) {
        trim_whitespace(line);

        // Skip empty lines and comments
        if (line[0] == '\0' || line[0] == '#' || line[0] == ';') {
            continue;
        }

        // Parse section header [Section]
        if (line[0] == '[') {
            char *end = strchr(line, ']');
            if (end) {
                *end = '\0';
                strncpy(section, line + 1, sizeof(section) - 1);
            }
            continue;
        }

        // Parse key=value pair
        char *eq = strchr(line, '=');
        if (!eq) continue;

        *eq = '\0';
        char *key = line;
        char *value = eq + 1;

        trim_whitespace(key);
        trim_whitespace(value);

        // Parse [Service] section
        if (strcmp(section, "Service") == 0) {
            if (strcmp(key, "Description") == 0) {
                strncpy(service->description, value, sizeof(service->description) - 1);
            } else if (strcmp(key, "Type") == 0) {
                service->type = parse_service_type(value);
            } else if (strcmp(key, "ExecStart") == 0) {
                strncpy(service->exec_start, value, sizeof(service->exec_start) - 1);
            } else if (strcmp(key, "ExecStop") == 0) {
                strncpy(service->exec_stop, value, sizeof(service->exec_stop) - 1);
            } else if (strcmp(key, "ExecReload") == 0) {
                strncpy(service->exec_reload, value, sizeof(service->exec_reload) - 1);
            } else if (strcmp(key, "WorkingDirectory") == 0) {
                strncpy(service->working_dir, value, sizeof(service->working_dir) - 1);
            } else if (strcmp(key, "User") == 0) {
                // TODO: Look up user ID from name
                service->user = 1000;
            } else if (strcmp(key, "Group") == 0) {
                // TODO: Look up group ID from name
                service->group = 1000;
            } else if (strcmp(key, "Requires") == 0) {
                // Store dependency name (strip .service suffix for matching)
                service->requires[0] = strip_service_suffix(value);
            } else if (strcmp(key, "Wants") == 0) {
                service->wants[0] = strip_service_suffix(value);
            } else if (strcmp(key, "After") == 0) {
                service->after[0] = strip_service_suffix(value);
            } else if (strcmp(key, "Before") == 0) {
                service->before[0] = strip_service_suffix(value);
            } else if (strcmp(key, "Conflicts") == 0) {
                service->conflicts[0] = strip_service_suffix(value);
            } else if (strcmp(key, "Restart") == 0) {
                service->restart = parse_restart_policy(value);
            } else if (strcmp(key, "RestartDelay") == 0) {
                service->restart_delay_ms = parse_time_sec(value) * 1000;
            } else if (strcmp(key, "RestartMaxAttempts") == 0) {
                service->restart_max_attempts = atoi(value);
            } else if (strcmp(key, "CPUQuota") == 0) {
                service->cpu_quota = atoi(value);
            } else if (strcmp(key, "MemoryLimit") == 0) {
                service->memory_limit = parse_size(value);
            } else if (strcmp(key, "TaskLimit") == 0) {
                service->task_limit = atoi(value);
            } else if (strcmp(key, "FileLimit") == 0) {
                service->file_limit = atoi(value);
            } else if (strcmp(key, "WatchdogSec") == 0) {
                service->watchdog_enabled = true;
                service->watchdog_timeout_sec = parse_time_sec(value);
            } else if (strcmp(key, "TimeoutStartSec") == 0) {
                service->timeout_start_sec = parse_time_sec(value);
            } else if (strcmp(key, "TimeoutStopSec") == 0) {
                service->timeout_stop_sec = parse_time_sec(value);
            }
        }
    }

    fclose(fp);
    return 0;
}

// Load service from configuration file
static service_t* service_load(const char *name) {
    char config_path[MAX_PATH_LEN];
    snprintf(config_path, sizeof(config_path), "%s/%s.service",
             SERVICE_CONFIG_DIR, name);

    // Check if config file exists
    if (access(config_path, F_OK) != 0) {
        fprintf(stderr, "Service config not found: %s\n", config_path);
        return NULL;
    }

    // Allocate service
    service_t *service = service_alloc(name);
    if (!service) {
        fprintf(stderr, "Failed to allocate service\n");
        return NULL;
    }

    // Parse configuration
    if (service_parse_config(service, config_path) < 0) {
        service_free(service);
        return NULL;
    }

    // Open log file
    service_log_open(service);

    return service;
}

// Reload service configuration
static int service_reload_config(service_t *service) {
    char config_path[MAX_PATH_LEN];
    snprintf(config_path, sizeof(config_path), "%s/%s.service",
             SERVICE_CONFIG_DIR, service->name);

    return service_parse_config(service, config_path);
}

// ============================================================================
// Service Execution
// ============================================================================

// Execute service command
static int service_execute(service_t *service, const char *command) {
    if (!command || command[0] == '\0') {
        return -1;
    }

    // Check if we're in mock mode (for testing without actual binaries)
    char *mock_mode = getenv("SERVICE_MOCK_MODE");
    if (mock_mode && strcmp(mock_mode, "1") == 0) {
        service_log(service, "INFO", "MOCK MODE: Would execute: %s", command);
        printf("[SERVICE] MOCK: Simulating start of %s\n", service->name);
        return 99999; // Return a fake PID
    }

    // Fork child process
    pid_t pid = fork();
    if (pid < 0) {
        service_log(service, "ERROR", "Failed to fork: %s", strerror(errno));
        return -1;
    }

    if (pid == 0) {
        // Child process

        // Change working directory
        if (service->working_dir[0] != '\0') {
            if (chdir(service->working_dir) < 0) {
                fprintf(stderr, "Failed to chdir to %s: %s\n",
                        service->working_dir, strerror(errno));
                exit(1);
            }
        }

        // Drop privileges
        if (service->group > 0) {
            if (setgid(service->group) < 0) {
                fprintf(stderr, "Failed to setgid: %s\n", strerror(errno));
                exit(1);
            }
        }

        if (service->user > 0) {
            if (setuid(service->user) < 0) {
                fprintf(stderr, "Failed to setuid: %s\n", strerror(errno));
                exit(1);
            }
        }

        // Redirect stdout/stderr to log file
        if (service->log_fd > 0) {
            dup2(service->log_fd, STDOUT_FILENO);
            dup2(service->log_fd, STDERR_FILENO);
        }

        // Parse command into argv
        char *argv[64] = {0};
        int argc = 0;

        char cmd_copy[MAX_COMMAND_LEN];
        strncpy(cmd_copy, command, sizeof(cmd_copy) - 1);

        char *token = strtok(cmd_copy, " \t");
        while (token && argc < 63) {
            argv[argc++] = token;
            token = strtok(NULL, " \t");
        }
        argv[argc] = NULL;

        // Execute command
        execvp(argv[0], argv);

        // If execvp returns, it failed
        fprintf(stderr, "Failed to execute %s: %s\n", argv[0], strerror(errno));
        exit(1);
    }

    // Parent process
    return pid;
}

// Start service
static int service_start_internal(service_t *service) {
    pthread_mutex_lock(&service->lock);

    // Check if already running
    if (service->state == SERVICE_STATE_RUNNING ||
        service->state == SERVICE_STATE_STARTING) {
        pthread_mutex_unlock(&service->lock);
        return 0;
    }

    service_log(service, "INFO", "Starting service...");
    printf("[SERVICE] Starting %s...\n", service->name);
    service->state = SERVICE_STATE_STARTING;
    service->start_time = get_timestamp_sec();

    pthread_mutex_unlock(&service->lock);

    // Execute start command
    int pid = service_execute(service, service->exec_start);
    if (pid < 0) {
        pthread_mutex_lock(&service->lock);
        service->state = SERVICE_STATE_FAILED;
        service_log(service, "ERROR", "Failed to start service");
        printf("[SERVICE] ERROR: Failed to start %s\n", service->name);
        pthread_mutex_unlock(&service->lock);
        return -1;
    }

    pthread_mutex_lock(&service->lock);
    service->pid = pid;

    // For simple services, immediately transition to running
    if (service->type == SERVICE_TYPE_SIMPLE) {
        service->state = SERVICE_STATE_RUNNING;
        service_log(service, "INFO", "Service started (PID %d)", pid);
        printf("[SERVICE] %s started (PID %d)\n", service->name, pid);
    }

    pthread_mutex_unlock(&service->lock);

    return 0;
}

// Stop service
static int service_stop_internal(service_t *service) {
    pthread_mutex_lock(&service->lock);

    // Check if already stopped
    if (service->state == SERVICE_STATE_STOPPED ||
        service->state == SERVICE_STATE_STOPPING) {
        pthread_mutex_unlock(&service->lock);
        return 0;
    }

    service_log(service, "INFO", "Stopping service...");
    service->state = SERVICE_STATE_STOPPING;

    pid_t pid = service->pid;
    pthread_mutex_unlock(&service->lock);

    // Execute stop command if provided
    if (service->exec_stop[0] != '\0') {
        service_execute(service, service->exec_stop);
    } else if (pid > 0) {
        // Send SIGTERM to process
        kill(pid, SIGTERM);

        // Wait for process to exit (with timeout)
        uint32_t timeout = service->timeout_stop_sec;
        for (uint32_t i = 0; i < timeout; i++) {
            int status;
            pid_t result = waitpid(pid, &status, WNOHANG);

            if (result == pid) {
                // Process exited
                pthread_mutex_lock(&service->lock);
                service->state = SERVICE_STATE_STOPPED;
                service->pid = -1;
                service->stop_time = get_timestamp_sec();
                service_log(service, "INFO", "Service stopped");
                pthread_mutex_unlock(&service->lock);
                return 0;
            }

            sleep(1);
        }

        // Timeout - send SIGKILL
        service_log(service, "WARNING", "Service did not stop gracefully, sending SIGKILL");
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
    }

    pthread_mutex_lock(&service->lock);
    service->state = SERVICE_STATE_STOPPED;
    service->pid = -1;
    service->stop_time = get_timestamp_sec();
    service_log(service, "INFO", "Service stopped");
    pthread_mutex_unlock(&service->lock);

    return 0;
}

// Restart service
static int service_restart_internal(service_t *service) {
    service_log(service, "INFO", "Restarting service...");

    service_stop_internal(service);

    // Wait for restart delay
    if (service->restart_delay_ms > 0) {
        usleep(service->restart_delay_ms * 1000);
    }

    return service_start_internal(service);
}

// ============================================================================
// Dependency Resolution
// ============================================================================

// Check if service dependencies are satisfied
static bool service_dependencies_satisfied(service_t *service) {
    // Check required dependencies
    for (int i = 0; i < MAX_DEPENDENCIES && service->requires[i]; i++) {
        service_t *dep = service_find(service->requires[i]);
        if (!dep || dep->state != SERVICE_STATE_RUNNING) {
            return false;
        }
    }

    // Check conflicts
    for (int i = 0; i < MAX_DEPENDENCIES && service->conflicts[i]; i++) {
        service_t *conflict = service_find(service->conflicts[i]);
        if (conflict && conflict->state == SERVICE_STATE_RUNNING) {
            return false;
        }
    }

    return true;
}

// Start service dependencies
static int service_start_dependencies(service_t *service) {
    // Start required dependencies
    for (int i = 0; i < MAX_DEPENDENCIES && service->requires[i]; i++) {
        service_t *dep = service_find(service->requires[i]);
        if (dep && dep->state != SERVICE_STATE_RUNNING) {
            service_log(service, "INFO", "Starting required dependency: %s", dep->name);
            service_start_internal(dep);
        }
    }

    // Start wanted dependencies (best effort)
    for (int i = 0; i < MAX_DEPENDENCIES && service->wants[i]; i++) {
        service_t *dep = service_find(service->wants[i]);
        if (dep && dep->state != SERVICE_STATE_RUNNING) {
            service_log(service, "INFO", "Starting wanted dependency: %s", dep->name);
            service_start_internal(dep);
        }
    }

    return 0;
}

// ============================================================================
// Public API
// ============================================================================

// Start service (with dependency resolution)
int service_start(const char *name) {
    service_t *service = service_find(name);
    if (!service) {
        fprintf(stderr, "[SERVICE] Service not found: %s\n", name);
        return -1;
    }

    // Start dependencies first
    service_start_dependencies(service);

    // Wait for dependencies (shorter timeout)
    for (int retry = 0; retry < 10; retry++) {
        if (service_dependencies_satisfied(service)) {
            break;
        }
        sleep(1);
    }

    if (!service_dependencies_satisfied(service)) {
        service_log(service, "WARNING", "Some dependencies not satisfied, attempting to start anyway");
        printf("[SERVICE] WARNING: %s dependencies not fully satisfied\n", service->name);
    }

    return service_start_internal(service);
}

// Stop service
int service_stop(const char *name) {
    service_t *service = service_find(name);
    if (!service) {
        fprintf(stderr, "Service not found: %s\n", name);
        return -1;
    }

    return service_stop_internal(service);
}

// Restart service
int service_restart(const char *name) {
    service_t *service = service_find(name);
    if (!service) {
        fprintf(stderr, "Service not found: %s\n", name);
        return -1;
    }

    return service_restart_internal(service);
}

// Reload service configuration
int service_reload(const char *name) {
    service_t *service = service_find(name);
    if (!service) {
        fprintf(stderr, "Service not found: %s\n", name);
        return -1;
    }

    service_log(service, "INFO", "Reloading configuration...");

    if (service_reload_config(service) < 0) {
        service_log(service, "ERROR", "Failed to reload configuration");
        return -1;
    }

    // If service has reload command, execute it
    if (service->exec_reload[0] != '\0' && service->pid > 0) {
        service_execute(service, service->exec_reload);
    } else if (service->pid > 0) {
        // Send SIGHUP to trigger reload
        kill(service->pid, SIGHUP);
    }

    service_log(service, "INFO", "Configuration reloaded");
    return 0;
}

// Enable service (start on boot)
int service_enable(const char *name) {
    service_t *service = service_find(name);
    if (!service) {
        fprintf(stderr, "Service not found: %s\n", name);
        return -1;
    }

    pthread_mutex_lock(&service->lock);
    service->enabled = true;
    service_log(service, "INFO", "Service enabled");
    pthread_mutex_unlock(&service->lock);

    return 0;
}

// Disable service (don't start on boot)
int service_disable(const char *name) {
    service_t *service = service_find(name);
    if (!service) {
        fprintf(stderr, "Service not found: %s\n", name);
        return -1;
    }

    pthread_mutex_lock(&service->lock);
    service->enabled = false;
    service_log(service, "INFO", "Service disabled");
    pthread_mutex_unlock(&service->lock);

    return 0;
}

// Get service status
int service_status(const char *name, char *buffer, size_t size) {
    service_t *service = service_find(name);
    if (!service) {
        return -1;
    }

    pthread_mutex_lock(&service->lock);

    snprintf(buffer, size,
             "Service: %s\n"
             "Description: %s\n"
             "Type: %s\n"
             "State: %s\n"
             "Enabled: %s\n"
             "PID: %d\n"
             "Memory: %lu KB\n"
             "CPU: %u%%\n"
             "Restart Count: %u\n"
             "Uptime: %lu seconds\n",
             service->name,
             service->description,
             service_type_to_string(service->type),
             service_state_to_string(service->state),
             service->enabled ? "yes" : "no",
             service->pid,
             service->memory_current / 1024,
             service->cpu_current,
             service->restart_count,
             service->state == SERVICE_STATE_RUNNING ?
                 (get_timestamp_sec() - service->start_time) : 0);

    pthread_mutex_unlock(&service->lock);

    return 0;
}

// List all services
int service_list(void (*callback)(const char *name, const char *state, void *userdata), void *userdata) {
    pthread_mutex_lock(&g_manager.lock);

    for (uint32_t i = 0; i < g_manager.service_count; i++) {
        service_t *service = g_manager.services[i];
        callback(service->name, service_state_to_string(service->state), userdata);
    }

    pthread_mutex_unlock(&g_manager.lock);

    return 0;
}

// ============================================================================
// Service Monitor Thread
// ============================================================================

// Monitor thread - checks service health and handles restarts
static void* monitor_thread_func(void *arg) {
    (void)arg;

    while (g_manager.running) {
        pthread_mutex_lock(&g_manager.lock);

        for (uint32_t i = 0; i < g_manager.service_count; i++) {
            service_t *service = g_manager.services[i];

            pthread_mutex_lock(&service->lock);

            // Check if process is still running
            if (service->state == SERVICE_STATE_RUNNING && service->pid > 0) {
                int status;
                pid_t result = waitpid(service->pid, &status, WNOHANG);

                if (result == service->pid) {
                    // Process exited
                    service->pid = -1;
                    service->stop_time = get_timestamp_sec();

                    if (WIFEXITED(status)) {
                        service->exit_code = WEXITSTATUS(status);
                        service_log(service, "INFO", "Service exited with code %d", service->exit_code);
                    } else if (WIFSIGNALED(status)) {
                        service->exit_signal = WTERMSIG(status);
                        service_log(service, "ERROR", "Service killed by signal %d", service->exit_signal);
                    }

                    // Check restart policy
                    bool should_restart = false;

                    if (service->restart == RESTART_POLICY_ALWAYS) {
                        should_restart = true;
                    } else if (service->restart == RESTART_POLICY_ON_FAILURE && service->exit_code != 0) {
                        should_restart = true;
                    } else if (service->restart == RESTART_POLICY_ON_ABNORMAL && WIFSIGNALED(status)) {
                        should_restart = true;
                    }

                    if (should_restart && service->restart_count < service->restart_max_attempts) {
                        service->state = SERVICE_STATE_RESTARTING;
                        service->restart_count++;
                        service_log(service, "INFO", "Scheduling restart (attempt %u/%u)",
                                    service->restart_count, service->restart_max_attempts);
                        pthread_mutex_unlock(&service->lock);

                        // Restart will be handled in next iteration
                        continue;
                    }

                    service->state = SERVICE_STATE_STOPPED;

                    if (service->restart_count >= service->restart_max_attempts) {
                        service->state = SERVICE_STATE_FAILED;
                        service_log(service, "ERROR", "Service failed (max restart attempts reached)");
                    }
                }
            }

            // Handle restart state
            if (service->state == SERVICE_STATE_RESTARTING) {
                pthread_mutex_unlock(&service->lock);
                service_restart_internal(service);
                continue;
            }

            pthread_mutex_unlock(&service->lock);
        }

        pthread_mutex_unlock(&g_manager.lock);

        // Sleep for 1 second
        sleep(1);
    }

    return NULL;
}

// ============================================================================
// Watchdog Thread
// ============================================================================

// Watchdog thread - monitors service health via watchdog pings
static void* watchdog_thread_func(void *arg) {
    (void)arg;

    while (g_manager.running) {
        pthread_mutex_lock(&g_manager.lock);

        uint64_t now = get_timestamp_sec();

        for (uint32_t i = 0; i < g_manager.service_count; i++) {
            service_t *service = g_manager.services[i];

            pthread_mutex_lock(&service->lock);

            if (service->watchdog_enabled &&
                service->state == SERVICE_STATE_RUNNING &&
                service->pid > 0) {

                uint64_t elapsed = now - service->last_watchdog_ping;

                if (elapsed > service->watchdog_timeout_sec) {
                    service_log(service, "ERROR", "Watchdog timeout (no ping for %lu seconds)", elapsed);

                    // Kill the process
                    kill(service->pid, SIGKILL);
                    service->state = SERVICE_STATE_FAILED;
                    service->pid = -1;

                    // Check if should restart
                    if (service->restart == RESTART_POLICY_ON_WATCHDOG ||
                        service->restart == RESTART_POLICY_ALWAYS) {
                        service->state = SERVICE_STATE_RESTARTING;
                    }
                }
            }

            pthread_mutex_unlock(&service->lock);
        }

        pthread_mutex_unlock(&g_manager.lock);

        // Sleep for 1 second
        sleep(1);
    }

    return NULL;
}

// ============================================================================
// Service Manager Initialization
// ============================================================================

// Initialize service manager
int service_manager_init(void) {
    memset(&g_manager, 0, sizeof(g_manager));
    pthread_mutex_init(&g_manager.lock, NULL);
    g_manager.running = true;

    // Create required directories
    mkdir(SERVICE_CONFIG_DIR, 0755);
    mkdir(SERVICE_LOG_DIR, 0755);
    mkdir(SERVICE_RUN_DIR, 0755);
    mkdir("/var/lib/services", 0755);

    // Load boot.conf to see which services should be enabled
    char boot_enabled[MAX_SERVICES][MAX_NAME_LEN] = {0};
    uint32_t boot_count = 0;

    FILE *boot_conf = fopen("/etc/services/boot.conf", "r");
    if (boot_conf) {
        char line[MAX_NAME_LEN];
        while (fgets(line, sizeof(line), boot_conf) && boot_count < MAX_SERVICES) {
            trim_whitespace(line);
            if (line[0] && line[0] != '#') {
                strncpy(boot_enabled[boot_count++], line, MAX_NAME_LEN - 1);
            }
        }
        fclose(boot_conf);
        printf("[SERVICE MANAGER] Loaded %u boot services from boot.conf\n", boot_count);
    }

    // Load all service configurations
    DIR *dir = opendir(SERVICE_CONFIG_DIR);
    if (dir) {
        struct dirent *entry;
        while ((entry = readdir(dir))) {
            if (entry->d_type != DT_REG) continue;

            // Check if .service file
            char *ext = strrchr(entry->d_name, '.');
            if (!ext || strcmp(ext, ".service") != 0) continue;

            // Extract service name
            char name[MAX_NAME_LEN];
            size_t len = ext - entry->d_name;
            if (len >= sizeof(name)) continue;

            strncpy(name, entry->d_name, len);
            name[len] = '\0';

            // Load service
            service_t *service = service_load(name);
            if (service) {
                // Check if this service should be enabled at boot
                for (uint32_t i = 0; i < boot_count; i++) {
                    if (strcmp(name, boot_enabled[i]) == 0) {
                        service->enabled = true;
                        break;
                    }
                }

                service_add(service);
                printf("[SERVICE MANAGER] Loaded service: %s %s\n",
                       name, service->enabled ? "(enabled)" : "(disabled)");
            }
        }
        closedir(dir);
    }

    // Start monitor thread
    if (pthread_create(&g_manager.monitor_thread, NULL, monitor_thread_func, NULL) != 0) {
        fprintf(stderr, "Failed to create monitor thread\n");
        return -1;
    }

    // Start watchdog thread
    if (pthread_create(&g_manager.watchdog_thread, NULL, watchdog_thread_func, NULL) != 0) {
        fprintf(stderr, "Failed to create watchdog thread\n");
        return -1;
    }

    printf("[SERVICE MANAGER] Initialized (%u services loaded)\n", g_manager.service_count);

    return 0;
}

// Start all enabled services (boot sequence)
int service_manager_start_boot_services(void) {
    printf("[SERVICE] Starting boot services...\n");

    pthread_mutex_lock(&g_manager.lock);

    // Count enabled services
    uint32_t enabled_count = 0;
    uint32_t started_count = 0;

    for (uint32_t i = 0; i < g_manager.service_count; i++) {
        if (g_manager.services[i]->enabled) {
            enabled_count++;
        }
    }

    pthread_mutex_unlock(&g_manager.lock);

    // Start enabled services using public API (which handles dependencies)
    for (uint32_t i = 0; i < g_manager.service_count; i++) {
        service_t *service = g_manager.services[i];

        if (service->enabled) {
            printf("[SERVICE] Starting boot service %u/%u: %s\n",
                   started_count + 1, enabled_count, service->name);

            if (service_start(service->name) == 0) {
                started_count++;
            } else {
                printf("[SERVICE] WARNING: Failed to start %s (continuing boot)\n",
                       service->name);
            }

            // Small delay between services
            usleep(100000); // 100ms
        }
    }

    g_manager.boot_complete = true;
    printf("[SERVICE] Boot sequence complete (%u/%u services started)\n",
           started_count, enabled_count);

    return 0;
}

// Shutdown service manager
int service_manager_shutdown(void) {
    printf("Shutting down service manager...\n");

    g_manager.running = false;

    // Wait for threads
    pthread_join(g_manager.monitor_thread, NULL);
    pthread_join(g_manager.watchdog_thread, NULL);

    // Stop all services
    pthread_mutex_lock(&g_manager.lock);

    for (uint32_t i = 0; i < g_manager.service_count; i++) {
        service_t *service = g_manager.services[i];
        service_stop_internal(service);
    }

    pthread_mutex_unlock(&g_manager.lock);

    printf("Service manager shutdown complete\n");

    return 0;
}
