# VFS Directory Syscalls - Quick Start Guide

## Basic Usage Examples

### 1. Listing Directory Contents

```c
#include "libc/dirent.h"
#include "libc/stdio.h"

void list_directory(const char* path) {
    DIR* dir = opendir(path);
    if (!dir) {
        printf("Failed to open directory: %s\n", path);
        return;
    }

    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL) {
        printf("%s\n", entry->d_name);
    }

    closedir(dir);
}
```

### 2. Checking File Type

```c
#include "libc/dirent.h"

void check_file_type(const char* path) {
    DIR* dir = opendir(path);
    if (!dir) return;

    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL) {
        switch (entry->d_type) {
            case DT_DIR:
                printf("[DIR]  %s\n", entry->d_name);
                break;
            case DT_REG:
                printf("[FILE] %s\n", entry->d_name);
                break;
            default:
                printf("[?]    %s\n", entry->d_name);
                break;
        }
    }

    closedir(dir);
}
```

### 3. Getting File Information

```c
#include "libc/sys_stat.h"
#include "libc/stdio.h"

void print_file_info(const char* path) {
    struct stat st;

    if (stat(path, &st) < 0) {
        printf("Failed to stat: %s\n", path);
        return;
    }

    printf("File: %s\n", path);
    printf("  Size:   %lu bytes\n", st.st_size);
    printf("  Inode:  %lu\n", st.st_ino);
    printf("  Mode:   0%o\n", st.st_mode);
    printf("  Links:  %u\n", st.st_nlink);

    if (S_ISDIR(st.st_mode)) {
        printf("  Type:   Directory\n");
    } else if (S_ISREG(st.st_mode)) {
        printf("  Type:   Regular file\n");
    }
}
```

### 4. Deleting a File

```c
#include "libc/sys_stat.h"
#include "libc/stdio.h"

int delete_file(const char* path) {
    if (unlink(path) < 0) {
        printf("Failed to delete: %s\n", path);
        return -1;
    }

    printf("Deleted: %s\n", path);
    return 0;
}
```

### 5. Renaming a File

```c
#include "libc/sys_stat.h"
#include "libc/stdio.h"

int rename_file(const char* old_name, const char* new_name) {
    if (rename(old_name, new_name) < 0) {
        printf("Failed to rename %s to %s\n", old_name, new_name);
        return -1;
    }

    printf("Renamed: %s -> %s\n", old_name, new_name);
    return 0;
}
```

### 6. Recursive Directory Listing

```c
#include "libc/dirent.h"
#include "libc/sys_stat.h"
#include "libc/stdio.h"
#include "libc/string.h"

void list_recursive(const char* path, int depth) {
    DIR* dir = opendir(path);
    if (!dir) return;

    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL) {
        // Skip . and ..
        if (strcmp(entry->d_name, ".") == 0 ||
            strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        // Print with indentation
        for (int i = 0; i < depth; i++) {
            printf("  ");
        }

        printf("%s", entry->d_name);

        if (entry->d_type == DT_DIR) {
            printf("/\n");

            // Build full path and recurse
            char full_path[256];
            snprintf(full_path, sizeof(full_path), "%s/%s",
                     path, entry->d_name);
            list_recursive(full_path, depth + 1);
        } else {
            printf("\n");
        }
    }

    closedir(dir);
}
```

### 7. Checking if File Exists

```c
#include "libc/sys_stat.h"

int file_exists(const char* path) {
    struct stat st;
    return (stat(path, &st) == 0);
}

int is_directory(const char* path) {
    struct stat st;
    if (stat(path, &st) < 0) {
        return 0;
    }
    return S_ISDIR(st.st_mode);
}

int is_regular_file(const char* path) {
    struct stat st;
    if (stat(path, &st) < 0) {
        return 0;
    }
    return S_ISREG(st.st_mode);
}
```

### 8. Safe File Operations

```c
#include "libc/sys_stat.h"
#include "libc/stdio.h"

int safe_delete(const char* path) {
    struct stat st;

    // Check if file exists
    if (stat(path, &st) < 0) {
        printf("File not found: %s\n", path);
        return -1;
    }

    // Don't delete directories
    if (S_ISDIR(st.st_mode)) {
        printf("Cannot delete directory: %s\n", path);
        return -1;
    }

    // Delete the file
    return unlink(path);
}

int safe_rename(const char* old_name, const char* new_name) {
    struct stat st;

    // Check if source exists
    if (stat(old_name, &st) < 0) {
        printf("Source not found: %s\n", old_name);
        return -1;
    }

    // Warn if destination exists
    if (stat(new_name, &st) == 0) {
        printf("Warning: %s will be overwritten\n", new_name);
    }

    // Perform rename
    return rename(old_name, new_name);
}
```

## Error Handling

### Checking Return Values

```c
DIR* dir = opendir("/some/path");
if (!dir) {
    // opendir returns NULL on error
    printf("Failed to open directory\n");
    return;
}

struct dirent* entry;
while ((entry = readdir(dir)) != NULL) {
    // Process entry
}
// readdir returns NULL at end of directory (this is normal)

if (closedir(dir) < 0) {
    // closedir returns -1 on error
    printf("Failed to close directory\n");
}
```

### Stat Error Handling

```c
struct stat st;
if (stat("/path/to/file", &st) < 0) {
    // stat returns -1 on error
    // Common errors:
    // - File not found
    // - Permission denied
    // - Invalid path
    printf("Failed to stat file\n");
    return;
}
```

### Unlink Error Handling

```c
if (unlink("/path/to/file") < 0) {
    // unlink returns -1 on error
    // Common errors:
    // - File not found
    // - Is a directory (use rmdir instead)
    // - Permission denied
    printf("Failed to delete file\n");
}
```

### Rename Error Handling

```c
if (rename("/old/path", "/new/path") < 0) {
    // rename returns -1 on error
    // Common errors:
    // - Source not found
    // - Permission denied
    // - Cross-device link (different filesystems)
    printf("Failed to rename file\n");
}
```

## Complete Example Program

```c
#include "libc/stdio.h"
#include "libc/dirent.h"
#include "libc/sys_stat.h"
#include "libc/string.h"

void print_file_details(const char* dir_path, const char* filename) {
    char full_path[256];
    snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, filename);

    struct stat st;
    if (stat(full_path, &st) == 0) {
        printf("  %-20s %8lu bytes  inode:%lu\n",
               filename, st.st_size, st.st_ino);
    } else {
        printf("  %-20s (stat failed)\n", filename);
    }
}

int main(void) {
    const char* dir_path = "/";

    printf("Listing directory: %s\n\n", dir_path);

    DIR* dir = opendir(dir_path);
    if (!dir) {
        printf("Failed to open directory\n");
        return 1;
    }

    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL) {
        char type = '?';
        switch (entry->d_type) {
            case DT_DIR:  type = 'd'; break;
            case DT_REG:  type = '-'; break;
            case DT_LNK:  type = 'l'; break;
            case DT_CHR:  type = 'c'; break;
            default:      type = '?'; break;
        }

        printf("[%c] ", type);
        print_file_details(dir_path, entry->d_name);
    }

    closedir(dir);

    return 0;
}
```

## Tips and Best Practices

1. **Always check return values** - All syscalls can fail
2. **Close directories** - Use closedir() to free resources
3. **Handle end-of-directory** - readdir() returns NULL when done (not an error)
4. **Validate paths** - Check if files/directories exist before operations
5. **Don't unlink directories** - Use rmdir() instead (when implemented)
6. **Rename can overwrite** - Check destination before renaming
7. **Use full paths** - Relative paths may not work as expected
8. **Skip . and ..** - These entries are meta-directories

## Syscall Numbers Reference

| Syscall     | Number | Arguments                      |
|-------------|--------|--------------------------------|
| SYS_OPENDIR | 30     | path                           |
| SYS_READDIR | 31     | dirfd, entry_ptr               |
| SYS_CLOSEDIR| 32     | dirfd                          |
| SYS_STAT    | 33     | path, stat_buf_ptr             |
| SYS_UNLINK  | 34     | path                           |
| SYS_RENAME  | 35     | oldpath, newpath               |

## Common Pitfalls

1. **Not checking for NULL** - Always validate pointers
2. **Buffer overflows** - Use snprintf() for path construction
3. **Memory leaks** - Always closedir() after opendir()
4. **Infinite loops** - Check readdir() return value properly
5. **Path length** - Maximum path is 4096 bytes
6. **Concurrent access** - Directory modifications during iteration may cause issues
7. **Type assumptions** - Always check d_type, don't assume

## Next Steps

- Implement directory creation (mkdir syscall)
- Implement file permissions (chmod syscall)
- Add symbolic link support
- Implement recursive directory removal
- Add directory notification (inotify-like)
