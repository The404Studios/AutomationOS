/**
 * Win32 API Types and Definitions
 *
 * Windows API type definitions, constants, and structures for PE compatibility.
 */

#ifndef PE_WIN32_H
#define PE_WIN32_H

#include "types.h"
#include "spinlock.h"

// =============================================================================
// Basic Types
// =============================================================================

typedef unsigned int UINT;
typedef void VOID;
typedef short SHORT;

/* Simple mutex for Win32 subsystem (wraps spinlock) */
typedef struct {
    spinlock_t spinlock;
    uint32_t owner;
    bool locked;
} mutex_t;

static inline void mutex_init(mutex_t* m) {
    spin_lock_init(&m->spinlock);
    m->owner = 0;
    m->locked = false;
}

static inline void mutex_lock(mutex_t* m) {
    spin_lock(&m->spinlock);
    m->locked = true;
}

static inline void mutex_unlock(mutex_t* m) {
    m->locked = false;
    spin_unlock(&m->spinlock);
}

typedef int BOOL;
typedef unsigned char BYTE;
typedef unsigned short WORD;
typedef unsigned int DWORD;
typedef unsigned long DWORD_PTR;
typedef unsigned long long ULONGLONG;
typedef long LONG;
typedef long long LONGLONG;
typedef size_t SIZE_T;
typedef void* PVOID;
typedef void* LPVOID;
typedef const void* LPCVOID;
typedef char CHAR;
typedef CHAR* LPSTR;
typedef const CHAR* LPCSTR;
typedef uint16_t WCHAR;  /* wchar_t substitute for freestanding kernel */
typedef WCHAR* LPWSTR;
typedef const WCHAR* LPCWSTR;
typedef void* HANDLE;
typedef HANDLE HWND;
typedef HANDLE HDC;
typedef HANDLE HMODULE;
typedef HANDLE HINSTANCE;
typedef HANDLE HICON;
typedef HANDLE HCURSOR;
typedef HANDLE HBRUSH;
typedef HANDLE HMENU;
typedef HANDLE HFONT;
typedef DWORD* PDWORD;
typedef DWORD* LPDWORD;
typedef LONG* PLONG;

#ifndef TRUE
#define TRUE 1
#endif

#ifndef FALSE
#define FALSE 0
#endif

#ifndef NULL
#define NULL ((void*)0)
#endif

// WINAPI calling convention (stdcall on x86, default on x64)
#define WINAPI
#define CALLBACK
#define APIENTRY

// =============================================================================
// Handle Constants
// =============================================================================

#define INVALID_HANDLE_VALUE ((HANDLE)(long long)-1)
#define INVALID_FILE_SIZE ((DWORD)0xFFFFFFFF)
#define INVALID_SET_FILE_POINTER ((DWORD)-1)

// =============================================================================
// File Access Flags
// =============================================================================

#define GENERIC_READ    0x80000000
#define GENERIC_WRITE   0x40000000
#define GENERIC_EXECUTE 0x20000000
#define GENERIC_ALL     0x10000000

// File creation disposition
#define CREATE_NEW        1
#define CREATE_ALWAYS     2
#define OPEN_EXISTING     3
#define OPEN_ALWAYS       4
#define TRUNCATE_EXISTING 5

// File share modes
#define FILE_SHARE_READ   0x00000001
#define FILE_SHARE_WRITE  0x00000002
#define FILE_SHARE_DELETE 0x00000004

// File attributes
#define FILE_ATTRIBUTE_NORMAL     0x00000080
#define FILE_ATTRIBUTE_DIRECTORY  0x00000010
#define FILE_ATTRIBUTE_READONLY   0x00000001

// File seek methods
#define FILE_BEGIN   0
#define FILE_CURRENT 1
#define FILE_END     2

#define MAX_PATH 260

// =============================================================================
// Memory Protection Flags
// =============================================================================

#define PAGE_NOACCESS          0x01
#define PAGE_READONLY          0x02
#define PAGE_READWRITE         0x04
#define PAGE_WRITECOPY         0x08
#define PAGE_EXECUTE           0x10
#define PAGE_EXECUTE_READ      0x20
#define PAGE_EXECUTE_READWRITE 0x40
#define PAGE_EXECUTE_WRITECOPY 0x80

// Memory allocation types
#define MEM_COMMIT   0x1000
#define MEM_RESERVE  0x2000
#define MEM_DECOMMIT 0x4000
#define MEM_RELEASE  0x8000

// =============================================================================
// Process/Thread Flags
// =============================================================================

#define CREATE_SUSPENDED 0x00000004
#define INFINITE 0xFFFFFFFF

// Wait return values
#define WAIT_OBJECT_0  0x00000000
#define WAIT_TIMEOUT   0x00000102
#define WAIT_ABANDONED 0x00000080
#define WAIT_FAILED    0xFFFFFFFF

// Thread start routine
typedef DWORD (WINAPI *LPTHREAD_START_ROUTINE)(LPVOID lpParameter);

// =============================================================================
// Error Codes
// =============================================================================

#define ERROR_SUCCESS                0
#define ERROR_INVALID_FUNCTION       1
#define ERROR_FILE_NOT_FOUND         2
#define ERROR_PATH_NOT_FOUND         3
#define ERROR_ACCESS_DENIED          5
#define ERROR_INVALID_HANDLE         6
#define ERROR_NOT_ENOUGH_MEMORY      8
#define ERROR_INVALID_PARAMETER      87
#define ERROR_INSUFFICIENT_BUFFER    122
#define ERROR_ALREADY_EXISTS         183
#define ERROR_NOT_SUPPORTED          50
#define ERROR_READ_FAULT             30
#define ERROR_WRITE_FAULT            29
#define ERROR_FILE_INVALID           1006
#define ERROR_NEGATIVE_SEEK          131
#define ERROR_INVALID_ADDRESS        487

// =============================================================================
// Structures
// =============================================================================

/**
 * Security attributes
 */
typedef struct {
    DWORD nLength;
    LPVOID lpSecurityDescriptor;
    BOOL bInheritHandle;
} SECURITY_ATTRIBUTES, *PSECURITY_ATTRIBUTES, *LPSECURITY_ATTRIBUTES;

/**
 * Overlapped I/O structure
 */
typedef struct {
    DWORD_PTR Internal;
    DWORD_PTR InternalHigh;
    union {
        struct {
            DWORD Offset;
            DWORD OffsetHigh;
        };
        PVOID Pointer;
    };
    HANDLE hEvent;
} OVERLAPPED, *LPOVERLAPPED;

/**
 * System time
 */
typedef struct {
    WORD wYear;
    WORD wMonth;
    WORD wDayOfWeek;
    WORD wDay;
    WORD wHour;
    WORD wMinute;
    WORD wSecond;
    WORD wMilliseconds;
} SYSTEMTIME, *LPSYSTEMTIME;

/**
 * Large integer (64-bit)
 */
typedef union {
    struct {
        DWORD LowPart;
        LONG HighPart;
    };
    LONGLONG QuadPart;
} LARGE_INTEGER, *PLARGE_INTEGER;

/**
 * Point structure
 */
typedef struct {
    LONG x;
    LONG y;
} POINT, *PPOINT, *LPPOINT;

/**
 * Rectangle structure
 */
typedef struct {
    LONG left;
    LONG top;
    LONG right;
    LONG bottom;
} RECT, *PRECT, *LPRECT;

/**
 * Message structure
 */
typedef struct {
    HWND   hwnd;
    UINT   message;
    DWORD  wParam;
    DWORD  lParam;
    DWORD  time;
    POINT  pt;
} MSG, *PMSG, *LPMSG;

/**
 * Window procedure
 */
typedef LONG (CALLBACK *WNDPROC)(HWND, UINT, DWORD, DWORD);

/**
 * Paint structure
 */
typedef struct {
    HDC  hdc;
    BOOL fErase;
    RECT rcPaint;
    BOOL fRestore;
    BOOL fIncUpdate;
    BYTE rgbReserved[32];
} PAINTSTRUCT, *PPAINTSTRUCT, *LPPAINTSTRUCT;

// =============================================================================
// Handle Management
// =============================================================================

typedef enum {
    HANDLE_TYPE_FILE,
    HANDLE_TYPE_THREAD,
    HANDLE_TYPE_PROCESS,
    HANDLE_TYPE_MUTEX,
    HANDLE_TYPE_SEMAPHORE,
    HANDLE_TYPE_EVENT,
    HANDLE_TYPE_PIPE,
    HANDLE_TYPE_SOCKET,
} handle_type_t;

typedef struct {
    void *object;
    handle_type_t type;
    DWORD access_mask;
    uint32_t ref_count;
    bool in_use;
} handle_entry_t;

#define MAX_HANDLES 4096

typedef struct {
    handle_entry_t entries[MAX_HANDLES];
    uint32_t next_handle;
    mutex_t lock;
} win32_handle_table_t;

// Handle management functions
void handle_table_init(win32_handle_table_t *table);
HANDLE handle_create(win32_handle_table_t *table, void *object, handle_type_t type);
void* handle_get_object(win32_handle_table_t *table, HANDLE handle);
handle_entry_t* handle_get_entry(win32_handle_table_t *table, HANDLE handle);
void handle_close(win32_handle_table_t *table, HANDLE handle);

// =============================================================================
// Win32 API Function Declarations
// =============================================================================

// Initialization
void win32_init(void);
void kernel32_init(void);
void user32_init(void);
void gdi32_init(void);

// Error handling
void win32_set_last_error(DWORD error);

// Utility functions
void wchar_to_utf8(LPCWSTR wstr, char *utf8, size_t utf8_len);
void utf8_to_wchar(const char *utf8, LPWSTR wstr, size_t wstr_len);

// ============================================================================
// Kernel32.dll APIs
// ============================================================================

// File I/O
HANDLE WINAPI CreateFileW(LPCWSTR lpFileName, DWORD dwDesiredAccess, DWORD dwShareMode,
                          LPSECURITY_ATTRIBUTES lpSecurityAttributes, DWORD dwCreationDisposition,
                          DWORD dwFlagsAndAttributes, HANDLE hTemplateFile);
BOOL WINAPI ReadFile(HANDLE hFile, LPVOID lpBuffer, DWORD nNumberOfBytesToRead,
                     LPDWORD lpNumberOfBytesRead, LPOVERLAPPED lpOverlapped);
BOOL WINAPI WriteFile(HANDLE hFile, LPCVOID lpBuffer, DWORD nNumberOfBytesToWrite,
                      LPDWORD lpNumberOfBytesWritten, LPOVERLAPPED lpOverlapped);
BOOL WINAPI CloseHandle(HANDLE hObject);
DWORD WINAPI GetFileSize(HANDLE hFile, LPDWORD lpFileSizeHigh);
DWORD WINAPI SetFilePointer(HANDLE hFile, LONG lDistanceToMove,
                             PLONG lpDistanceToMoveHigh, DWORD dwMoveMethod);

// Memory management
LPVOID WINAPI VirtualAlloc(LPVOID lpAddress, SIZE_T dwSize, DWORD flAllocationType, DWORD flProtect);
BOOL WINAPI VirtualFree(LPVOID lpAddress, SIZE_T dwSize, DWORD dwFreeType);
BOOL WINAPI VirtualProtect(LPVOID lpAddress, SIZE_T dwSize, DWORD flNewProtect, PDWORD lpflOldProtect);

// Process/Thread management
HANDLE WINAPI CreateThread(LPSECURITY_ATTRIBUTES lpThreadAttributes, SIZE_T dwStackSize,
                           LPTHREAD_START_ROUTINE lpStartAddress, LPVOID lpParameter,
                           DWORD dwCreationFlags, LPDWORD lpThreadId);
VOID WINAPI ExitProcess(UINT uExitCode);
VOID WINAPI ExitThread(DWORD dwExitCode);
HANDLE WINAPI GetCurrentProcess(VOID);
HANDLE WINAPI GetCurrentThread(VOID);
DWORD WINAPI GetCurrentProcessId(VOID);
DWORD WINAPI GetCurrentThreadId(VOID);
VOID WINAPI Sleep(DWORD dwMilliseconds);

// Synchronization
HANDLE WINAPI CreateMutexW(LPSECURITY_ATTRIBUTES lpMutexAttributes, BOOL bInitialOwner, LPCWSTR lpName);
BOOL WINAPI ReleaseMutex(HANDLE hMutex);
DWORD WINAPI WaitForSingleObject(HANDLE hHandle, DWORD dwMilliseconds);

// Time
VOID WINAPI GetSystemTime(LPSYSTEMTIME lpSystemTime);
DWORD WINAPI GetTickCount(VOID);
ULONGLONG WINAPI GetTickCount64(VOID);
BOOL WINAPI QueryPerformanceCounter(LARGE_INTEGER *lpPerformanceCount);
BOOL WINAPI QueryPerformanceFrequency(LARGE_INTEGER *lpFrequency);

// String functions
int WINAPI lstrlenW(LPCWSTR lpString);
LPWSTR WINAPI lstrcpyW(LPWSTR lpString1, LPCWSTR lpString2);

// Error handling
DWORD WINAPI GetLastError(VOID);
VOID WINAPI SetLastError(DWORD dwErrCode);

// ============================================================================
// User32.dll APIs
// ============================================================================

// Window management
HWND WINAPI CreateWindowExW(DWORD dwExStyle, LPCWSTR lpClassName, LPCWSTR lpWindowName,
                            DWORD dwStyle, int X, int Y, int nWidth, int nHeight,
                            HWND hWndParent, HMENU hMenu, HINSTANCE hInstance, LPVOID lpParam);
BOOL WINAPI DestroyWindow(HWND hWnd);
BOOL WINAPI ShowWindow(HWND hWnd, int nCmdShow);
BOOL WINAPI UpdateWindow(HWND hWnd);

// Message loop
BOOL WINAPI GetMessageW(LPMSG lpMsg, HWND hWnd, UINT wMsgFilterMin, UINT wMsgFilterMax);
BOOL WINAPI TranslateMessage(const MSG *lpMsg);
LONG WINAPI DispatchMessageW(const MSG *lpMsg);
VOID WINAPI PostQuitMessage(int nExitCode);

// Input
SHORT WINAPI GetAsyncKeyState(int vKey);
BOOL WINAPI GetCursorPos(LPPOINT lpPoint);

// ============================================================================
// GDI32.dll APIs
// ============================================================================

// Drawing
HDC WINAPI BeginPaint(HWND hWnd, LPPAINTSTRUCT lpPaint);
BOOL WINAPI EndPaint(HWND hWnd, const PAINTSTRUCT *lpPaint);
BOOL WINAPI TextOutW(HDC hdc, int x, int y, LPCWSTR lpString, int c);
BOOL WINAPI Rectangle(HDC hdc, int left, int top, int right, int bottom);

#endif // PE_WIN32_H
