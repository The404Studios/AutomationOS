// userspace/libc/dirent.c - Directory operations implementation

#include "dirent.h"
#include "syscall.h"
#include "stdlib.h"
#include "string.h"

// Default buffer size for directory entries
#define DIRENT_BUF_SIZE 4096

// ============================================================================
// DIRECTORY OPERATIONS
// ============================================================================

// Open directory
DIR* opendir(const char* name) {
    if (!name) {
        return NULL;
    }

    // Call kernel opendir syscall
    int dirfd = sys_opendir_raw(name);
    if (dirfd < 0) {
        return NULL;
    }

    // Allocate DIR structure
    DIR* dirp = (DIR*)malloc(sizeof(DIR));
    if (!dirp) {
        sys_closedir_raw(dirfd);
        return NULL;
    }

    // Allocate buffer for directory entries (for compatibility, though not used currently)
    dirp->buffer = (char*)malloc(DIRENT_BUF_SIZE);
    if (!dirp->buffer) {
        sys_closedir_raw(dirfd);
        free(dirp);
        return NULL;
    }

    dirp->fd = dirfd;
    dirp->buf_size = DIRENT_BUF_SIZE;
    dirp->buf_pos = 0;
    dirp->buf_count = 0;
    dirp->tell_pos = 0;
    memset(&dirp->entry, 0, sizeof(struct dirent));

    return dirp;
}

// Internal syscall wrapper for opendir
static inline int sys_opendir_raw(const char* path) {
    long result;
    __asm__ volatile (
        "movq $30, %%rax\n"    // SYS_OPENDIR = 30
        "movq %1, %%rdi\n"
        "syscall\n"
        "movq %%rax, %0\n"
        : "=r"(result)
        : "r"(path)
        : "rax", "rdi", "rcx", "r11", "memory"
    );
    return (int)result;
}

// Internal syscall wrapper for readdir
static inline int sys_readdir_raw(int dirfd, struct dirent* entry) {
    long result;
    __asm__ volatile (
        "movq $31, %%rax\n"    // SYS_READDIR = 31
        "movl %1, %%edi\n"
        "movq %2, %%rsi\n"
        "syscall\n"
        "movq %%rax, %0\n"
        : "=r"(result)
        : "r"(dirfd), "r"(entry)
        : "rax", "rdi", "rsi", "rcx", "r11", "memory"
    );
    return (int)result;
}

// Internal syscall wrapper for closedir
static inline int sys_closedir_raw(int dirfd) {
    long result;
    __asm__ volatile (
        "movq $32, %%rax\n"    // SYS_CLOSEDIR = 32
        "movl %1, %%edi\n"
        "syscall\n"
        "movq %%rax, %0\n"
        : "=r"(result)
        : "r"(dirfd)
        : "rax", "rdi", "rcx", "r11", "memory"
    );
    return (int)result;
}

// Read directory entry
struct dirent* readdir(DIR* dirp) {
    if (!dirp) {
        return NULL;
    }

    // Call kernel readdir syscall
    int result = sys_readdir_raw(dirp->fd, &dirp->entry);
    if (result < 0) {
        // End of directory or error
        return NULL;
    }

    // Update position
    dirp->tell_pos++;

    return &dirp->entry;
}

// Reentrant version of readdir
int readdir_r(DIR* dirp, struct dirent* entry, struct dirent** result) {
    if (!dirp || !entry || !result) {
        return -1;
    }

    struct dirent* d = readdir(dirp);
    if (d) {
        memcpy(entry, d, sizeof(struct dirent));
        *result = entry;
        return 0;
    }

    *result = NULL;
    return 0;
}

// Rewind directory to beginning
void rewinddir(DIR* dirp) {
    if (!dirp) {
        return;
    }

    // Reset buffer position
    dirp->buf_pos = 0;
    dirp->buf_count = 0;
    dirp->tell_pos = 0;

    // Seek to beginning of directory
    lseek(dirp->fd, 0, SEEK_SET);
}

// Get current location in directory stream
long telldir(DIR* dirp) {
    if (!dirp) {
        return -1;
    }

    return dirp->tell_pos;
}

// Set position in directory stream
void seekdir(DIR* dirp, long loc) {
    if (!dirp) {
        return;
    }

    dirp->tell_pos = loc;
    // In a real implementation, would seek to the specified position
}

// Close directory
int closedir(DIR* dirp) {
    if (!dirp) {
        return -1;
    }

    int result = sys_closedir_raw(dirp->fd);

    if (dirp->buffer) {
        free(dirp->buffer);
    }

    free(dirp);

    return result;
}

// Get file descriptor of directory stream
int dirfd(DIR* dirp) {
    if (!dirp) {
        return -1;
    }

    return dirp->fd;
}

// ============================================================================
// DIRECTORY SCANNING
// ============================================================================

// Scan directory and build list of entries
int scandir(const char* dirp, struct dirent*** namelist,
            int (*filter)(const struct dirent*),
            int (*compar)(const struct dirent**, const struct dirent**)) {
    if (!dirp || !namelist) {
        return -1;
    }

    DIR* dir = opendir(dirp);
    if (!dir) {
        return -1;
    }

    // Allocate initial array
    size_t capacity = 32;
    size_t count = 0;
    struct dirent** entries = (struct dirent**)malloc(capacity * sizeof(struct dirent*));
    if (!entries) {
        closedir(dir);
        return -1;
    }

    // Read all directory entries
    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL) {
        // Apply filter if provided
        if (filter && !filter(entry)) {
            continue;
        }

        // Expand array if needed
        if (count >= capacity) {
            capacity *= 2;
            struct dirent** new_entries = (struct dirent**)realloc(entries,
                                          capacity * sizeof(struct dirent*));
            if (!new_entries) {
                // Free allocated entries
                for (size_t i = 0; i < count; i++) {
                    free(entries[i]);
                }
                free(entries);
                closedir(dir);
                return -1;
            }
            entries = new_entries;
        }

        // Allocate and copy entry
        entries[count] = (struct dirent*)malloc(sizeof(struct dirent));
        if (!entries[count]) {
            for (size_t i = 0; i < count; i++) {
                free(entries[i]);
            }
            free(entries);
            closedir(dir);
            return -1;
        }

        memcpy(entries[count], entry, sizeof(struct dirent));
        count++;
    }

    closedir(dir);

    // Sort entries if comparator provided
    if (compar && count > 0) {
        qsort(entries, count, sizeof(struct dirent*),
              (int (*)(const void*, const void*))compar);
    }

    *namelist = entries;
    return (int)count;
}

// Compare directory entries alphabetically
int alphasort(const struct dirent** a, const struct dirent** b) {
    if (!a || !*a || !b || !*b) {
        return 0;
    }

    return strcmp((*a)->d_name, (*b)->d_name);
}
