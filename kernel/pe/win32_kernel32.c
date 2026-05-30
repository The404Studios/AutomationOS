/**
 * Win32 Kernel32.dll API Stubs
 *
 * Implementation of essential Kernel32 APIs for Windows compatibility.
 * Provides file I/O, memory management, process/thread management, and synchronization.
 */

#include <kernel/pe_win32.h>
#include <kernel/process.h>
#include <kernel/memory.h>
#include <kernel/vfs.h>
#include <kernel/scheduler.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

// Handle table
static win32_handle_table_t handle_table;

/**
 * Initialize Kernel32 subsystem
 */
void kernel32_init(void) {
    handle_table_init(&handle_table);
    printf("Kernel32: Initialized\n");
}

// ============================================================================
// File I/O
// ============================================================================

/**
 * CreateFileW - Create or open a file
 */
HANDLE WINAPI CreateFileW(
    LPCWSTR lpFileName,
    DWORD dwDesiredAccess,
    DWORD dwShareMode,
    LPSECURITY_ATTRIBUTES lpSecurityAttributes,
    DWORD dwCreationDisposition,
    DWORD dwFlagsAndAttributes,
    HANDLE hTemplateFile)
{
    // Convert wide string to UTF-8
    char path[MAX_PATH];
    wchar_to_utf8(lpFileName, path, sizeof(path));

    // Convert flags
    int flags = 0;
    int mode = 0644;

    if (dwDesiredAccess & GENERIC_READ) {
        if (dwDesiredAccess & GENERIC_WRITE) {
            flags |= O_RDWR;
        } else {
            flags |= O_RDONLY;
        }
    } else if (dwDesiredAccess & GENERIC_WRITE) {
        flags |= O_WRONLY;
    }

    switch (dwCreationDisposition) {
        case CREATE_NEW:
            flags |= O_CREAT | O_EXCL;
            break;
        case CREATE_ALWAYS:
            flags |= O_CREAT | O_TRUNC;
            break;
        case OPEN_EXISTING:
            // No additional flags
            break;
        case OPEN_ALWAYS:
            flags |= O_CREAT;
            break;
        case TRUNCATE_EXISTING:
            flags |= O_TRUNC;
            break;
    }

    // Open file
    int fd = vfs_open(path, flags, mode);
    if (fd < 0) {
        win32_set_last_error(ERROR_FILE_NOT_FOUND);
        return INVALID_HANDLE_VALUE;
    }

    // Create handle
    HANDLE handle = handle_create(&handle_table, (void *)(uint64_t)fd, HANDLE_TYPE_FILE);
    printf("CreateFileW: %s -> handle %p (fd %d)\n", path, handle, fd);

    return handle;
}

/**
 * ReadFile - Read from file
 */
BOOL WINAPI ReadFile(
    HANDLE hFile,
    LPVOID lpBuffer,
    DWORD nNumberOfBytesToRead,
    LPDWORD lpNumberOfBytesRead,
    LPOVERLAPPED lpOverlapped)
{
    if (lpOverlapped) {
        // Async I/O not implemented yet
        win32_set_last_error(ERROR_NOT_SUPPORTED);
        return FALSE;
    }

    int fd = (int)(uint64_t)handle_get_object(&handle_table, hFile);
    if (fd < 0) {
        win32_set_last_error(ERROR_INVALID_HANDLE);
        return FALSE;
    }

    ssize_t bytes_read = vfs_read(fd, lpBuffer, nNumberOfBytesToRead);
    if (bytes_read < 0) {
        win32_set_last_error(ERROR_READ_FAULT);
        return FALSE;
    }

    if (lpNumberOfBytesRead) {
        *lpNumberOfBytesRead = bytes_read;
    }

    return TRUE;
}

/**
 * WriteFile - Write to file
 */
BOOL WINAPI WriteFile(
    HANDLE hFile,
    LPCVOID lpBuffer,
    DWORD nNumberOfBytesToWrite,
    LPDWORD lpNumberOfBytesWritten,
    LPOVERLAPPED lpOverlapped)
{
    if (lpOverlapped) {
        win32_set_last_error(ERROR_NOT_SUPPORTED);
        return FALSE;
    }

    int fd = (int)(uint64_t)handle_get_object(&handle_table, hFile);
    if (fd < 0) {
        win32_set_last_error(ERROR_INVALID_HANDLE);
        return FALSE;
    }

    ssize_t bytes_written = vfs_write(fd, lpBuffer, nNumberOfBytesToWrite);
    if (bytes_written < 0) {
        win32_set_last_error(ERROR_WRITE_FAULT);
        return FALSE;
    }

    if (lpNumberOfBytesWritten) {
        *lpNumberOfBytesWritten = bytes_written;
    }

    return TRUE;
}

/**
 * CloseHandle - Close handle
 */
BOOL WINAPI CloseHandle(HANDLE hObject) {
    handle_entry_t *entry = handle_get_entry(&handle_table, hObject);
    if (!entry) {
        win32_set_last_error(ERROR_INVALID_HANDLE);
        return FALSE;
    }

    // Close based on type
    switch (entry->type) {
        case HANDLE_TYPE_FILE: {
            int fd = (int)(uint64_t)entry->object;
            vfs_close(fd);
            break;
        }

        case HANDLE_TYPE_THREAD: {
            thread_t *thread = (thread_t *)entry->object;
            // Thread cleanup
            break;
        }

        case HANDLE_TYPE_PROCESS: {
            process_t *proc = (process_t *)entry->object;
            // Process cleanup
            break;
        }

        case HANDLE_TYPE_MUTEX:
        case HANDLE_TYPE_SEMAPHORE:
        case HANDLE_TYPE_EVENT: {
            // Synchronization object cleanup
            free(entry->object);
            break;
        }
    }

    handle_close(&handle_table, hObject);
    return TRUE;
}

/**
 * GetFileSize - Get file size
 */
DWORD WINAPI GetFileSize(HANDLE hFile, LPDWORD lpFileSizeHigh) {
    int fd = (int)(uint64_t)handle_get_object(&handle_table, hFile);
    if (fd < 0) {
        win32_set_last_error(ERROR_INVALID_HANDLE);
        return INVALID_FILE_SIZE;
    }

    struct stat st;
    if (vfs_fstat(fd, &st) < 0) {
        win32_set_last_error(ERROR_FILE_INVALID);
        return INVALID_FILE_SIZE;
    }

    if (lpFileSizeHigh) {
        *lpFileSizeHigh = (DWORD)(st.st_size >> 32);
    }

    return (DWORD)(st.st_size & 0xFFFFFFFF);
}

/**
 * SetFilePointer - Seek in file
 */
DWORD WINAPI SetFilePointer(
    HANDLE hFile,
    LONG lDistanceToMove,
    PLONG lpDistanceToMoveHigh,
    DWORD dwMoveMethod)
{
    int fd = (int)(uint64_t)handle_get_object(&handle_table, hFile);
    if (fd < 0) {
        win32_set_last_error(ERROR_INVALID_HANDLE);
        return INVALID_SET_FILE_POINTER;
    }

    int whence;
    switch (dwMoveMethod) {
        case FILE_BEGIN:   whence = SEEK_SET; break;
        case FILE_CURRENT: whence = SEEK_CUR; break;
        case FILE_END:     whence = SEEK_END; break;
        default:
            win32_set_last_error(ERROR_INVALID_PARAMETER);
            return INVALID_SET_FILE_POINTER;
    }

    off_t offset = lDistanceToMove;
    if (lpDistanceToMoveHigh) {
        offset |= ((off_t)*lpDistanceToMoveHigh) << 32;
    }

    off_t new_pos = vfs_lseek(fd, offset, whence);
    if (new_pos < 0) {
        win32_set_last_error(ERROR_NEGATIVE_SEEK);
        return INVALID_SET_FILE_POINTER;
    }

    if (lpDistanceToMoveHigh) {
        *lpDistanceToMoveHigh = (LONG)(new_pos >> 32);
    }

    return (DWORD)(new_pos & 0xFFFFFFFF);
}

// ============================================================================
// Memory Management
// ============================================================================

/**
 * VirtualAlloc - Allocate virtual memory
 */
LPVOID WINAPI VirtualAlloc(
    LPVOID lpAddress,
    SIZE_T dwSize,
    DWORD flAllocationType,
    DWORD flProtect)
{
    int prot = 0;

    if (flProtect & PAGE_EXECUTE) prot |= PROT_EXEC;
    if (flProtect & PAGE_EXECUTE_READ) prot |= PROT_EXEC | PROT_READ;
    if (flProtect & PAGE_EXECUTE_READWRITE) prot |= PROT_EXEC | PROT_READ | PROT_WRITE;
    if (flProtect & PAGE_READONLY) prot |= PROT_READ;
    if (flProtect & PAGE_READWRITE) prot |= PROT_READ | PROT_WRITE;

    void *addr;
    if (lpAddress) {
        addr = vmm_alloc_at(current_process->vmm, lpAddress, dwSize, prot);
    } else {
        addr = vmm_alloc(current_process->vmm, dwSize, prot);
    }

    if (!addr) {
        win32_set_last_error(ERROR_NOT_ENOUGH_MEMORY);
        return NULL;
    }

    printf("VirtualAlloc: %p (size 0x%lx)\n", addr, dwSize);
    return addr;
}

/**
 * VirtualFree - Free virtual memory
 */
BOOL WINAPI VirtualFree(LPVOID lpAddress, SIZE_T dwSize, DWORD dwFreeType) {
    if (!lpAddress) {
        win32_set_last_error(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    if (dwFreeType & MEM_RELEASE) {
        vmm_free(current_process->vmm, lpAddress, dwSize);
    } else if (dwFreeType & MEM_DECOMMIT) {
        // Decommit pages (mark as inaccessible but keep address space)
        vmm_protect(current_process->vmm, lpAddress, dwSize, PROT_NONE);
    }

    return TRUE;
}

/**
 * VirtualProtect - Change memory protection
 */
BOOL WINAPI VirtualProtect(
    LPVOID lpAddress,
    SIZE_T dwSize,
    DWORD flNewProtect,
    PDWORD lpflOldProtect)
{
    if (!lpAddress) {
        win32_set_last_error(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    // Get old protection (not implemented - just return current)
    if (lpflOldProtect) {
        *lpflOldProtect = PAGE_READWRITE;
    }

    int prot = 0;
    if (flNewProtect & PAGE_EXECUTE) prot |= PROT_EXEC;
    if (flNewProtect & PAGE_EXECUTE_READ) prot |= PROT_EXEC | PROT_READ;
    if (flNewProtect & PAGE_EXECUTE_READWRITE) prot |= PROT_EXEC | PROT_READ | PROT_WRITE;
    if (flNewProtect & PAGE_READONLY) prot |= PROT_READ;
    if (flNewProtect & PAGE_READWRITE) prot |= PROT_READ | PROT_WRITE;

    if (vmm_protect(current_process->vmm, lpAddress, dwSize, prot) < 0) {
        win32_set_last_error(ERROR_INVALID_ADDRESS);
        return FALSE;
    }

    return TRUE;
}

// ============================================================================
// Process/Thread Management
// ============================================================================

/**
 * CreateThread - Create a thread
 */
HANDLE WINAPI CreateThread(
    LPSECURITY_ATTRIBUTES lpThreadAttributes,
    SIZE_T dwStackSize,
    LPTHREAD_START_ROUTINE lpStartAddress,
    LPVOID lpParameter,
    DWORD dwCreationFlags,
    LPDWORD lpThreadId)
{
    thread_t *thread = thread_create(current_process, lpStartAddress, lpParameter, dwStackSize);
    if (!thread) {
        win32_set_last_error(ERROR_NOT_ENOUGH_MEMORY);
        return NULL;
    }

    if (lpThreadId) {
        *lpThreadId = thread->tid;
    }

    if (!(dwCreationFlags & CREATE_SUSPENDED)) {
        scheduler_add_thread(thread);
    }

    HANDLE handle = handle_create(&handle_table, thread, HANDLE_TYPE_THREAD);
    printf("CreateThread: tid %u -> handle %p\n", thread->tid, handle);

    return handle;
}

/**
 * ExitProcess - Terminate process
 */
VOID WINAPI ExitProcess(UINT uExitCode) {
    printf("ExitProcess: code %u\n", uExitCode);
    process_exit(current_process, uExitCode);
    // Never returns
}

/**
 * ExitThread - Terminate thread
 */
VOID WINAPI ExitThread(DWORD dwExitCode) {
    printf("ExitThread: code %u\n", dwExitCode);
    thread_exit(current_thread, dwExitCode);
    // Never returns
}

/**
 * GetCurrentProcess - Get current process handle
 */
HANDLE WINAPI GetCurrentProcess(VOID) {
    return (HANDLE)-1; // Pseudo-handle
}

/**
 * GetCurrentThread - Get current thread handle
 */
HANDLE WINAPI GetCurrentThread(VOID) {
    return (HANDLE)-2; // Pseudo-handle
}

/**
 * GetCurrentProcessId - Get current process ID
 */
DWORD WINAPI GetCurrentProcessId(VOID) {
    return current_process ? current_process->pid : 0;
}

/**
 * GetCurrentThreadId - Get current thread ID
 */
DWORD WINAPI GetCurrentThreadId(VOID) {
    return current_thread ? current_thread->tid : 0;
}

/**
 * Sleep - Sleep for milliseconds
 */
VOID WINAPI Sleep(DWORD dwMilliseconds) {
    scheduler_sleep(dwMilliseconds);
}

// ============================================================================
// Synchronization
// ============================================================================

/**
 * CreateMutexW - Create mutex
 */
HANDLE WINAPI CreateMutexW(
    LPSECURITY_ATTRIBUTES lpMutexAttributes,
    BOOL bInitialOwner,
    LPCWSTR lpName)
{
    mutex_t *mutex = malloc(sizeof(mutex_t));
    if (!mutex) {
        win32_set_last_error(ERROR_NOT_ENOUGH_MEMORY);
        return NULL;
    }

    mutex_init(mutex);

    if (bInitialOwner) {
        mutex_lock(mutex);
    }

    HANDLE handle = handle_create(&handle_table, mutex, HANDLE_TYPE_MUTEX);
    return handle;
}

/**
 * ReleaseMutex - Release mutex
 */
BOOL WINAPI ReleaseMutex(HANDLE hMutex) {
    mutex_t *mutex = (mutex_t *)handle_get_object(&handle_table, hMutex);
    if (!mutex) {
        win32_set_last_error(ERROR_INVALID_HANDLE);
        return FALSE;
    }

    mutex_unlock(mutex);
    return TRUE;
}

/**
 * WaitForSingleObject - Wait for object
 */
DWORD WINAPI WaitForSingleObject(HANDLE hHandle, DWORD dwMilliseconds) {
    handle_entry_t *entry = handle_get_entry(&handle_table, hHandle);
    if (!entry) {
        win32_set_last_error(ERROR_INVALID_HANDLE);
        return WAIT_FAILED;
    }

    switch (entry->type) {
        case HANDLE_TYPE_MUTEX: {
            mutex_t *mutex = (mutex_t *)entry->object;
            if (dwMilliseconds == INFINITE) {
                mutex_lock(mutex);
            } else {
                // Timed lock not implemented
                mutex_lock(mutex);
            }
            return WAIT_OBJECT_0;
        }

        case HANDLE_TYPE_THREAD: {
            thread_t *thread = (thread_t *)entry->object;
            // Wait for thread to terminate
            while (thread->state != THREAD_STATE_TERMINATED) {
                scheduler_yield();
            }
            return WAIT_OBJECT_0;
        }

        case HANDLE_TYPE_PROCESS: {
            process_t *proc = (process_t *)entry->object;
            // Wait for process to terminate
            while (proc->state != PROCESS_STATE_TERMINATED) {
                scheduler_yield();
            }
            return WAIT_OBJECT_0;
        }

        default:
            win32_set_last_error(ERROR_NOT_SUPPORTED);
            return WAIT_FAILED;
    }
}

// ============================================================================
// Time Functions
// ============================================================================

/**
 * GetSystemTime - Get system time
 */
VOID WINAPI GetSystemTime(LPSYSTEMTIME lpSystemTime) {
    if (!lpSystemTime) return;

    time_t now = time(NULL);
    struct tm *tm = gmtime(&now);

    lpSystemTime->wYear = tm->tm_year + 1900;
    lpSystemTime->wMonth = tm->tm_mon + 1;
    lpSystemTime->wDayOfWeek = tm->tm_wday;
    lpSystemTime->wDay = tm->tm_mday;
    lpSystemTime->wHour = tm->tm_hour;
    lpSystemTime->wMinute = tm->tm_min;
    lpSystemTime->wSecond = tm->tm_sec;
    lpSystemTime->wMilliseconds = 0;
}

/**
 * GetTickCount - Get uptime in milliseconds
 */
DWORD WINAPI GetTickCount(VOID) {
    return (DWORD)(scheduler_get_uptime() * 1000);
}

/**
 * GetTickCount64 - Get uptime in milliseconds (64-bit)
 */
ULONGLONG WINAPI GetTickCount64(VOID) {
    return scheduler_get_uptime() * 1000;
}

/**
 * QueryPerformanceCounter - High-resolution counter
 */
BOOL WINAPI QueryPerformanceCounter(LARGE_INTEGER *lpPerformanceCount) {
    if (!lpPerformanceCount) {
        win32_set_last_error(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    lpPerformanceCount->QuadPart = rdtsc();
    return TRUE;
}

/**
 * QueryPerformanceFrequency - Get counter frequency
 */
BOOL WINAPI QueryPerformanceFrequency(LARGE_INTEGER *lpFrequency) {
    if (!lpFrequency) {
        win32_set_last_error(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    lpFrequency->QuadPart = cpu_get_frequency();
    return TRUE;
}

// ============================================================================
// String Functions
// ============================================================================

/**
 * lstrlenW - String length
 */
int WINAPI lstrlenW(LPCWSTR lpString) {
    if (!lpString) return 0;

    int len = 0;
    while (lpString[len]) {
        len++;
    }
    return len;
}

/**
 * lstrcpyW - String copy
 */
LPWSTR WINAPI lstrcpyW(LPWSTR lpString1, LPCWSTR lpString2) {
    if (!lpString1 || !lpString2) return lpString1;

    LPWSTR dest = lpString1;
    while (*lpString2) {
        *lpString1++ = *lpString2++;
    }
    *lpString1 = 0;

    return dest;
}

/**
 * GetLastError - Get last error code
 */
DWORD WINAPI GetLastError(VOID) {
    return current_thread ? current_thread->last_error : 0;
}

/**
 * SetLastError - Set last error code
 */
VOID WINAPI SetLastError(DWORD dwErrCode) {
    if (current_thread) {
        current_thread->last_error = dwErrCode;
    }
}

/**
 * Helper: Set last error
 */
void win32_set_last_error(DWORD error) {
    SetLastError(error);
}
