/**
 * Windows Registry Emulation
 *
 * Emulate Windows registry using file system mapping.
 * Maps registry hives to directories:
 * - HKEY_LOCAL_MACHINE -> /etc/registry/machine/
 * - HKEY_CURRENT_USER -> ~/.registry/
 * - HKEY_CLASSES_ROOT -> /etc/registry/classes/
 */

#include <kernel/pe_win32.h>
#include <kernel/vfs.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

// Registry key handle
typedef struct registry_key {
    HKEY hkey;
    char path[512];
    int access;
    struct registry_key *next;
} registry_key_t;

static registry_key_t *key_list = NULL;
static HKEY next_hkey = (HKEY)0x80000000;
static mutex_t registry_lock;

// Predefined keys
#define HKEY_CLASSES_ROOT   ((HKEY)0x80000000)
#define HKEY_CURRENT_USER   ((HKEY)0x80000001)
#define HKEY_LOCAL_MACHINE  ((HKEY)0x80000002)
#define HKEY_USERS          ((HKEY)0x80000003)

/**
 * Get registry path for predefined key
 */
static const char* registry_get_base_path(HKEY hKey) {
    switch ((uint64_t)hKey) {
        case 0x80000000: return "/etc/registry/classes";
        case 0x80000001: return "/home/user/.registry";
        case 0x80000002: return "/etc/registry/machine";
        case 0x80000003: return "/etc/registry/users";
        default: return NULL;
    }
}

/**
 * Check if key is predefined
 */
static bool registry_is_predefined(HKEY hKey) {
    uint64_t key = (uint64_t)hKey;
    return (key >= 0x80000000 && key <= 0x80000003);
}

/**
 * Find registry key
 */
static registry_key_t* registry_find_key(HKEY hKey) {
    if (registry_is_predefined(hKey)) {
        return NULL; // Predefined keys don't have entries
    }

    mutex_lock(&registry_lock);

    registry_key_t *key = key_list;
    while (key) {
        if (key->hkey == hKey) {
            mutex_unlock(&registry_lock);
            return key;
        }
        key = key->next;
    }

    mutex_unlock(&registry_lock);
    return NULL;
}

/**
 * RegOpenKeyExW - Open registry key
 */
LONG WINAPI RegOpenKeyExW(
    HKEY hKey,
    LPCWSTR lpSubKey,
    DWORD ulOptions,
    DWORD samDesired,
    HKEY *phkResult)
{
    char subkey[512];
    wchar_to_utf8(lpSubKey, subkey, sizeof(subkey));

    printf("RegOpenKeyExW: key=%p subkey='%s'\n", hKey, subkey);

    if (!phkResult) {
        return ERROR_INVALID_PARAMETER;
    }

    // Build full path
    char full_path[1024];
    const char *base_path = registry_get_base_path(hKey);

    if (!base_path) {
        // Try to get path from existing key
        registry_key_t *parent = registry_find_key(hKey);
        if (!parent) {
            return ERROR_INVALID_HANDLE;
        }
        base_path = parent->path;
    }

    if (lpSubKey && *lpSubKey) {
        snprintf(full_path, sizeof(full_path), "%s/%s", base_path, subkey);
    } else {
        strncpy(full_path, base_path, sizeof(full_path));
    }

    // Create key entry
    registry_key_t *key = calloc(1, sizeof(registry_key_t));
    if (!key) {
        return ERROR_NOT_ENOUGH_MEMORY;
    }

    mutex_lock(&registry_lock);

    key->hkey = next_hkey;
    next_hkey = (HKEY)((uint64_t)next_hkey + 1);
    strncpy(key->path, full_path, sizeof(key->path) - 1);
    key->access = samDesired;

    key->next = key_list;
    key_list = key;

    mutex_unlock(&registry_lock);

    *phkResult = key->hkey;

    printf("RegOpenKeyExW: opened %p -> %s\n", key->hkey, key->path);

    return ERROR_SUCCESS;
}

/**
 * RegQueryValueExW - Query registry value
 */
LONG WINAPI RegQueryValueExW(
    HKEY hKey,
    LPCWSTR lpValueName,
    LPDWORD lpReserved,
    LPDWORD lpType,
    LPBYTE lpData,
    LPDWORD lpcbData)
{
    char value_name[256];
    wchar_to_utf8(lpValueName, value_name, sizeof(value_name));

    printf("RegQueryValueExW: key=%p value='%s'\n", hKey, value_name);

    // Get key path
    const char *base_path = registry_get_base_path(hKey);
    if (!base_path) {
        registry_key_t *key = registry_find_key(hKey);
        if (!key) {
            return ERROR_INVALID_HANDLE;
        }
        base_path = key->path;
    }

    // Build value file path
    char value_path[1024];
    snprintf(value_path, sizeof(value_path), "%s/%s", base_path, value_name);

    // Try to read value from file
    int fd = vfs_open(value_path, O_RDONLY, 0);
    if (fd < 0) {
        printf("RegQueryValueExW: value not found: %s\n", value_path);
        return ERROR_FILE_NOT_FOUND;
    }

    if (lpData && lpcbData) {
        ssize_t bytes_read = vfs_read(fd, lpData, *lpcbData);
        if (bytes_read >= 0) {
            *lpcbData = bytes_read;
        }
    } else if (lpcbData) {
        // Just query size
        struct stat st;
        if (vfs_fstat(fd, &st) == 0) {
            *lpcbData = st.st_size;
        }
    }

    if (lpType) {
        *lpType = 1; // REG_SZ (string)
    }

    vfs_close(fd);

    return ERROR_SUCCESS;
}

/**
 * RegSetValueExW - Set registry value
 */
LONG WINAPI RegSetValueExW(
    HKEY hKey,
    LPCWSTR lpValueName,
    DWORD Reserved,
    DWORD dwType,
    const BYTE *lpData,
    DWORD cbData)
{
    char value_name[256];
    wchar_to_utf8(lpValueName, value_name, sizeof(value_name));

    printf("RegSetValueExW: key=%p value='%s' type=%u size=%u\n",
           hKey, value_name, dwType, cbData);

    // Get key path
    const char *base_path = registry_get_base_path(hKey);
    if (!base_path) {
        registry_key_t *key = registry_find_key(hKey);
        if (!key) {
            return ERROR_INVALID_HANDLE;
        }
        base_path = key->path;
    }

    // Build value file path
    char value_path[1024];
    snprintf(value_path, sizeof(value_path), "%s/%s", base_path, value_name);

    // Create directory if needed
    vfs_mkdir_recursive(base_path, 0755);

    // Write value to file
    int fd = vfs_open(value_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        return ERROR_ACCESS_DENIED;
    }

    if (lpData && cbData > 0) {
        vfs_write(fd, lpData, cbData);
    }

    vfs_close(fd);

    return ERROR_SUCCESS;
}

/**
 * RegCloseKey - Close registry key
 */
LONG WINAPI RegCloseKey(HKEY hKey) {
    printf("RegCloseKey: %p\n", hKey);

    if (registry_is_predefined(hKey)) {
        return ERROR_SUCCESS;
    }

    registry_key_t *key = registry_find_key(hKey);
    if (!key) {
        return ERROR_INVALID_HANDLE;
    }

    // Remove from list
    mutex_lock(&registry_lock);

    registry_key_t **prev = &key_list;
    registry_key_t *curr = key_list;

    while (curr) {
        if (curr == key) {
            *prev = curr->next;
            break;
        }
        prev = &curr->next;
        curr = curr->next;
    }

    mutex_unlock(&registry_lock);

    free(key);

    return ERROR_SUCCESS;
}

/**
 * RegCreateKeyExW - Create registry key
 */
LONG WINAPI RegCreateKeyExW(
    HKEY hKey,
    LPCWSTR lpSubKey,
    DWORD Reserved,
    LPWSTR lpClass,
    DWORD dwOptions,
    DWORD samDesired,
    LPSECURITY_ATTRIBUTES lpSecurityAttributes,
    HKEY *phkResult,
    LPDWORD lpdwDisposition)
{
    char subkey[512];
    wchar_to_utf8(lpSubKey, subkey, sizeof(subkey));

    printf("RegCreateKeyExW: key=%p subkey='%s'\n", hKey, subkey);

    // Get base path
    const char *base_path = registry_get_base_path(hKey);
    if (!base_path) {
        registry_key_t *parent = registry_find_key(hKey);
        if (!parent) {
            return ERROR_INVALID_HANDLE;
        }
        base_path = parent->path;
    }

    // Build full path
    char full_path[1024];
    snprintf(full_path, sizeof(full_path), "%s/%s", base_path, subkey);

    // Create directory
    int result = vfs_mkdir_recursive(full_path, 0755);

    if (lpdwDisposition) {
        *lpdwDisposition = (result == 0) ? 1 : 2; // REG_CREATED_NEW_KEY or REG_OPENED_EXISTING_KEY
    }

    // Open the key
    return RegOpenKeyExW(hKey, lpSubKey, 0, samDesired, phkResult);
}

/**
 * RegDeleteKeyW - Delete registry key
 */
LONG WINAPI RegDeleteKeyW(HKEY hKey, LPCWSTR lpSubKey) {
    char subkey[512];
    wchar_to_utf8(lpSubKey, subkey, sizeof(subkey));

    printf("RegDeleteKeyW: key=%p subkey='%s'\n", hKey, subkey);

    // Get base path
    const char *base_path = registry_get_base_path(hKey);
    if (!base_path) {
        registry_key_t *parent = registry_find_key(hKey);
        if (!parent) {
            return ERROR_INVALID_HANDLE;
        }
        base_path = parent->path;
    }

    // Build full path
    char full_path[1024];
    snprintf(full_path, sizeof(full_path), "%s/%s", base_path, subkey);

    // Delete directory
    if (vfs_rmdir(full_path) < 0) {
        return ERROR_ACCESS_DENIED;
    }

    return ERROR_SUCCESS;
}

/**
 * Initialize registry emulation
 */
void registry_init(void) {
    mutex_init(&registry_lock);
    key_list = NULL;

    // Create registry directories
    vfs_mkdir_recursive("/etc/registry/machine", 0755);
    vfs_mkdir_recursive("/etc/registry/classes", 0755);
    vfs_mkdir_recursive("/etc/registry/users", 0755);

    printf("Registry: Initialized\n");
}

/**
 * Print registry keys (debug)
 */
void registry_print_keys(void) {
    printf("=== Registry Keys ===\n");

    mutex_lock(&registry_lock);

    registry_key_t *key = key_list;
    int count = 0;

    while (key) {
        printf("[%d] HKEY %p: %s\n", count, key->hkey, key->path);
        key = key->next;
        count++;
    }

    mutex_unlock(&registry_lock);

    printf("Total: %d keys\n", count);
    printf("=====================\n");
}
