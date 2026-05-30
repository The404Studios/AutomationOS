// userspace/system/services/logging.c - Centralized service logging
// Log management, rotation, compression, and querying (600+ LOC)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <dirent.h>
#include <pthread.h>
#include <zlib.h>

// ============================================================================
// Constants and Definitions
// ============================================================================

#define LOG_DIR "/var/log/services"
#define LOG_MAX_SIZE_MB 10          // Rotate after 10 MB
#define LOG_KEEP_DAYS 30            // Keep logs for 30 days
#define LOG_COMPRESS_AFTER_DAYS 7   // Compress logs older than 7 days

// Log levels
typedef enum {
    LOG_LEVEL_DEBUG,
    LOG_LEVEL_INFO,
    LOG_LEVEL_WARNING,
    LOG_LEVEL_ERROR,
    LOG_LEVEL_CRITICAL
} log_level_t;

// Log entry structure
typedef struct {
    time_t timestamp;
    log_level_t level;
    char service[128];
    char message[1024];
} log_entry_t;

// ============================================================================
// Utility Functions
// ============================================================================

// Get log level string
static const char* log_level_to_string(log_level_t level) {
    switch (level) {
        case LOG_LEVEL_DEBUG: return "DEBUG";
        case LOG_LEVEL_INFO: return "INFO";
        case LOG_LEVEL_WARNING: return "WARNING";
        case LOG_LEVEL_ERROR: return "ERROR";
        case LOG_LEVEL_CRITICAL: return "CRITICAL";
        default: return "UNKNOWN";
    }
}

// Parse log level from string
static log_level_t parse_log_level(const char *str) {
    if (strcmp(str, "DEBUG") == 0) return LOG_LEVEL_DEBUG;
    if (strcmp(str, "INFO") == 0) return LOG_LEVEL_INFO;
    if (strcmp(str, "WARNING") == 0) return LOG_LEVEL_WARNING;
    if (strcmp(str, "ERROR") == 0) return LOG_LEVEL_ERROR;
    if (strcmp(str, "CRITICAL") == 0) return LOG_LEVEL_CRITICAL;
    return LOG_LEVEL_INFO;
}

// Get file size
static off_t get_file_size(const char *path) {
    struct stat st;
    if (stat(path, &st) == 0) {
        return st.st_size;
    }
    return 0;
}

// Get file age in days
static int get_file_age_days(const char *path) {
    struct stat st;
    if (stat(path, &st) == 0) {
        time_t now = time(NULL);
        return (int)((now - st.st_mtime) / (24 * 3600));
    }
    return 0;
}

// ============================================================================
// Log Writing
// ============================================================================

// Write log entry to file
int log_write(const char *service, log_level_t level, const char *format, ...) {
    char log_path[1024];
    char timestamp[64];
    char message[1024];
    char line[2048];
    time_t now;
    struct tm *tm_info;
    va_list args;

    // Create log directory if needed
    mkdir(LOG_DIR, 0755);

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
             timestamp, service, log_level_to_string(level), message);

    // Open log file (append mode)
    snprintf(log_path, sizeof(log_path), "%s/%s.log", LOG_DIR, service);

    FILE *fp = fopen(log_path, "a");
    if (!fp) {
        fprintf(stderr, "Failed to open log file: %s\n", log_path);
        return -1;
    }

    // Write log line
    fputs(line, fp);
    fflush(fp);

    fclose(fp);

    return 0;
}

// ============================================================================
// Log Rotation
// ============================================================================

// Rotate log file
static int rotate_log_file(const char *log_path) {
    char rotated_path[1024];
    char timestamp[64];
    time_t now;
    struct tm *tm_info;

    // Get current time for rotation filename
    time(&now);
    tm_info = localtime(&now);
    strftime(timestamp, sizeof(timestamp), "%Y%m%d-%H%M%S", tm_info);

    // Create rotated filename
    snprintf(rotated_path, sizeof(rotated_path), "%s.%s", log_path, timestamp);

    // Rename current log to rotated name
    if (rename(log_path, rotated_path) < 0) {
        fprintf(stderr, "Failed to rotate log file: %s\n", log_path);
        return -1;
    }

    printf("Rotated log: %s -> %s\n", log_path, rotated_path);

    return 0;
}

// Check if log needs rotation
static bool should_rotate_log(const char *log_path) {
    off_t size = get_file_size(log_path);
    off_t max_size = LOG_MAX_SIZE_MB * 1024 * 1024;

    return (size > max_size);
}

// Rotate all logs if needed
int log_rotate_all(void) {
    DIR *dir = opendir(LOG_DIR);
    if (!dir) {
        return -1;
    }

    struct dirent *entry;
    int rotated = 0;

    while ((entry = readdir(dir))) {
        // Skip non-.log files
        char *ext = strrchr(entry->d_name, '.');
        if (!ext || strcmp(ext, ".log") != 0) {
            continue;
        }

        // Check if needs rotation
        char log_path[1024];
        snprintf(log_path, sizeof(log_path), "%s/%s", LOG_DIR, entry->d_name);

        if (should_rotate_log(log_path)) {
            if (rotate_log_file(log_path) == 0) {
                rotated++;
            }
        }
    }

    closedir(dir);

    if (rotated > 0) {
        printf("Rotated %d log file(s)\n", rotated);
    }

    return 0;
}

// ============================================================================
// Log Compression
// ============================================================================

// Compress file using gzip
static int compress_file(const char *input_path, const char *output_path) {
    FILE *input = fopen(input_path, "rb");
    if (!input) {
        fprintf(stderr, "Failed to open input file: %s\n", input_path);
        return -1;
    }

    gzFile output = gzopen(output_path, "wb9");  // Maximum compression
    if (!output) {
        fprintf(stderr, "Failed to create compressed file: %s\n", output_path);
        fclose(input);
        return -1;
    }

    // Compress data
    char buffer[8192];
    size_t bytes_read;

    while ((bytes_read = fread(buffer, 1, sizeof(buffer), input)) > 0) {
        if (gzwrite(output, buffer, bytes_read) != (int)bytes_read) {
            fprintf(stderr, "Failed to write compressed data\n");
            gzclose(output);
            fclose(input);
            return -1;
        }
    }

    gzclose(output);
    fclose(input);

    // Delete original file
    unlink(input_path);

    printf("Compressed: %s -> %s\n", input_path, output_path);

    return 0;
}

// Compress old logs
int log_compress_old(void) {
    DIR *dir = opendir(LOG_DIR);
    if (!dir) {
        return -1;
    }

    struct dirent *entry;
    int compressed = 0;

    while ((entry = readdir(dir))) {
        // Look for rotated log files (name contains timestamp)
        if (strstr(entry->d_name, ".log.") == NULL) {
            continue;
        }

        // Skip already compressed files
        if (strstr(entry->d_name, ".gz") != NULL) {
            continue;
        }

        char log_path[1024];
        snprintf(log_path, sizeof(log_path), "%s/%s", LOG_DIR, entry->d_name);

        // Check age
        int age_days = get_file_age_days(log_path);

        if (age_days >= LOG_COMPRESS_AFTER_DAYS) {
            // Compress this file
            char compressed_path[1024];
            snprintf(compressed_path, sizeof(compressed_path), "%s.gz", log_path);

            if (compress_file(log_path, compressed_path) == 0) {
                compressed++;
            }
        }
    }

    closedir(dir);

    if (compressed > 0) {
        printf("Compressed %d old log file(s)\n", compressed);
    }

    return 0;
}

// ============================================================================
// Log Cleanup
// ============================================================================

// Delete old logs
int log_delete_old(void) {
    DIR *dir = opendir(LOG_DIR);
    if (!dir) {
        return -1;
    }

    struct dirent *entry;
    int deleted = 0;

    while ((entry = readdir(dir))) {
        // Skip current logs (no timestamp in name)
        if (strstr(entry->d_name, ".log.") == NULL) {
            continue;
        }

        char log_path[1024];
        snprintf(log_path, sizeof(log_path), "%s/%s", LOG_DIR, entry->d_name);

        // Check age
        int age_days = get_file_age_days(log_path);

        if (age_days >= LOG_KEEP_DAYS) {
            // Delete this file
            if (unlink(log_path) == 0) {
                printf("Deleted old log: %s (age: %d days)\n", entry->d_name, age_days);
                deleted++;
            }
        }
    }

    closedir(dir);

    if (deleted > 0) {
        printf("Deleted %d old log file(s)\n", deleted);
    }

    return 0;
}

// ============================================================================
// Log Querying
// ============================================================================

// Query logs by service
int log_query_service(const char *service, log_entry_t *entries, int max_entries, log_level_t min_level) {
    char log_path[1024];
    snprintf(log_path, sizeof(log_path), "%s/%s.log", LOG_DIR, service);

    FILE *fp = fopen(log_path, "r");
    if (!fp) {
        return 0;
    }

    char line[2048];
    int count = 0;

    while (fgets(line, sizeof(line), fp) && count < max_entries) {
        // Parse log line
        // Format: [timestamp] [service] [level] message

        char *timestamp_start = strchr(line, '[');
        if (!timestamp_start) continue;

        char *timestamp_end = strchr(timestamp_start + 1, ']');
        if (!timestamp_end) continue;

        char *service_start = strchr(timestamp_end + 1, '[');
        if (!service_start) continue;

        char *service_end = strchr(service_start + 1, ']');
        if (!service_end) continue;

        char *level_start = strchr(service_end + 1, '[');
        if (!level_start) continue;

        char *level_end = strchr(level_start + 1, ']');
        if (!level_end) continue;

        char *message_start = level_end + 1;
        while (*message_start == ' ') message_start++;

        // Extract level
        char level_str[32];
        size_t level_len = level_end - level_start - 1;
        if (level_len >= sizeof(level_str)) continue;

        strncpy(level_str, level_start + 1, level_len);
        level_str[level_len] = '\0';

        // Trim whitespace from level
        char *ptr = level_str;
        while (*ptr == ' ') ptr++;
        char *end = level_str + strlen(level_str) - 1;
        while (end > ptr && (*end == ' ' || *end == '\n')) {
            *end = '\0';
            end--;
        }
        if (ptr != level_str) {
            memmove(level_str, ptr, strlen(ptr) + 1);
        }

        log_level_t level = parse_log_level(level_str);

        // Filter by level
        if (level < min_level) continue;

        // Store entry
        log_entry_t *entry = &entries[count++];
        entry->timestamp = time(NULL);  // TODO: Parse timestamp
        entry->level = level;
        strncpy(entry->service, service, sizeof(entry->service) - 1);

        // Copy message (remove trailing newline)
        strncpy(entry->message, message_start, sizeof(entry->message) - 1);
        char *newline = strchr(entry->message, '\n');
        if (newline) *newline = '\0';
    }

    fclose(fp);

    return count;
}

// Print log entries
void log_print_entries(const log_entry_t *entries, int count) {
    for (int i = 0; i < count; i++) {
        const log_entry_t *entry = &entries[i];

        char timestamp[64];
        struct tm *tm_info = localtime(&entry->timestamp);
        strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", tm_info);

        printf("[%s] [%s] [%s] %s\n",
               timestamp,
               entry->service,
               log_level_to_string(entry->level),
               entry->message);
    }
}

// Tail log file (follow mode)
int log_tail_follow(const char *service) {
    char log_path[1024];
    snprintf(log_path, sizeof(log_path), "%s/%s.log", LOG_DIR, service);

    // Use tail -f command
    char cmd[2048];
    snprintf(cmd, sizeof(cmd), "tail -f %s", log_path);

    return system(cmd);
}

// ============================================================================
// Log Statistics
// ============================================================================

// Get log statistics
typedef struct {
    uint64_t total_entries;
    uint64_t debug_count;
    uint64_t info_count;
    uint64_t warning_count;
    uint64_t error_count;
    uint64_t critical_count;
    uint64_t total_size_bytes;
} log_stats_t;

int log_get_stats(const char *service, log_stats_t *stats) {
    memset(stats, 0, sizeof(*stats));

    char log_path[1024];
    snprintf(log_path, sizeof(log_path), "%s/%s.log", LOG_DIR, service);

    // Get file size
    stats->total_size_bytes = get_file_size(log_path);

    // Count entries by level
    FILE *fp = fopen(log_path, "r");
    if (!fp) {
        return -1;
    }

    char line[2048];

    while (fgets(line, sizeof(line), fp)) {
        stats->total_entries++;

        // Extract level
        if (strstr(line, "[DEBUG]")) {
            stats->debug_count++;
        } else if (strstr(line, "[INFO]")) {
            stats->info_count++;
        } else if (strstr(line, "[WARNING]")) {
            stats->warning_count++;
        } else if (strstr(line, "[ERROR]")) {
            stats->error_count++;
        } else if (strstr(line, "[CRITICAL]")) {
            stats->critical_count++;
        }
    }

    fclose(fp);

    return 0;
}

// Print log statistics
void log_print_stats(const char *service, const log_stats_t *stats) {
    printf("\nLog Statistics for %s:\n", service);
    printf("=====================================\n");
    printf("Total Entries:    %lu\n", stats->total_entries);
    printf("  DEBUG:          %lu\n", stats->debug_count);
    printf("  INFO:           %lu\n", stats->info_count);
    printf("  WARNING:        %lu\n", stats->warning_count);
    printf("  ERROR:          %lu\n", stats->error_count);
    printf("  CRITICAL:       %lu\n", stats->critical_count);
    printf("Total Size:       %lu bytes (%.2f MB)\n",
           stats->total_size_bytes,
           stats->total_size_bytes / (1024.0 * 1024.0));
    printf("\n");
}

// ============================================================================
// Log Maintenance
// ============================================================================

// Run log maintenance (rotation, compression, cleanup)
int log_maintenance(void) {
    printf("Running log maintenance...\n");

    // Rotate large logs
    log_rotate_all();

    // Compress old logs
    log_compress_old();

    // Delete very old logs
    log_delete_old();

    printf("Log maintenance complete\n");

    return 0;
}

// ============================================================================
// Initialization
// ============================================================================

// Initialize logging system
int log_init(void) {
    // Create log directory
    mkdir(LOG_DIR, 0755);

    printf("Logging system initialized\n");

    return 0;
}
