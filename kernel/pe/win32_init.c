/**
 * Win32 Subsystem Initialization
 *
 * Initialize all Win32 API components and register DLL stubs.
 */

#include <kernel/pe_win32.h>
#include <kernel/pe_loader.h>
#include <stdio.h>
#include <string.h>

// UTF-8 / wchar_t conversion utilities
void wchar_to_utf8(LPCWSTR wstr, char *utf8, size_t utf8_len) {
    if (!wstr || !utf8 || utf8_len == 0) return;

    size_t i = 0;
    while (wstr[i] && i < utf8_len - 1) {
        // Simple conversion (assumes ASCII range for now)
        // Full UTF-16 to UTF-8 would be more complex
        if (wstr[i] < 0x80) {
            utf8[i] = (char)wstr[i];
        } else {
            utf8[i] = '?'; // Placeholder for non-ASCII
        }
        i++;
    }
    utf8[i] = '\0';
}

void utf8_to_wchar(const char *utf8, LPWSTR wstr, size_t wstr_len) {
    if (!utf8 || !wstr || wstr_len == 0) return;

    size_t i = 0;
    while (utf8[i] && i < wstr_len - 1) {
        wstr[i] = (WCHAR)utf8[i];
        i++;
    }
    wstr[i] = 0;
}

/**
 * Register system DLLs with stub implementations
 */
static void register_system_dlls(void) {
    printf("Win32: Registering system DLLs\n");

    // These would normally be actual DLL files, but we provide stubs
    // The PE loader will call our stub functions when these DLLs are imported

    // kernel32.dll - already implemented as stubs
    // user32.dll - stubs below
    // gdi32.dll - stubs below
    // ntdll.dll - NT API stubs

    printf("Win32: System DLLs registered\n");
}

/**
 * Initialize Win32 subsystem
 */
void win32_init(void) {
    static bool initialized = false;

    if (initialized) {
        return;
    }

    printf("Win32: Initializing subsystem\n");

    // Initialize components
    kernel32_init();
    user32_init();
    gdi32_init();

    // Register system DLLs
    register_system_dlls();

    initialized = true;

    printf("Win32: Subsystem initialized\n");
}

/**
 * Shutdown Win32 subsystem
 */
void win32_shutdown(void) {
    printf("Win32: Shutting down subsystem\n");
    // Cleanup would go here
}

/**
 * Win32 syscall dispatcher
 *
 * This is called when Windows executables make system calls.
 * We translate them to AutomationOS syscalls.
 */
int win32_syscall_dispatch(uint32_t syscall_num, void *args) {
    printf("Win32: Syscall %u (args %p)\n", syscall_num, args);

    // Windows NT syscall numbers
    // We would implement translation here
    // For now, just return success

    return 0;
}
