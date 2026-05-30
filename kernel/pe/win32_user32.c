/**
 * Win32 User32.dll API Stubs
 *
 * Implementation of User32 APIs for window management and message handling.
 * Provides basic windowing functionality for Windows GUI applications.
 */

#include <kernel/pe_win32.h>
#include <kernel/process.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

// Window management structures
typedef struct window {
    HWND hwnd;
    WCHAR class_name[256];
    WCHAR window_name[256];
    DWORD style;
    DWORD ex_style;
    int x, y, width, height;
    HWND parent;
    WNDPROC wnd_proc;
    void *user_data;
    bool visible;
    struct window *next;
} window_t;

static window_t *window_list = NULL;
static HWND next_hwnd = (HWND)0x1000;
static mutex_t window_lock;

/**
 * Initialize User32 subsystem
 */
void user32_init(void) {
    mutex_init(&window_lock);
    window_list = NULL;
    printf("User32: Initialized\n");
}

/**
 * Create window
 */
static window_t* window_create(void) {
    window_t *wnd = calloc(1, sizeof(window_t));
    if (!wnd) return NULL;

    mutex_lock(&window_lock);

    wnd->hwnd = next_hwnd;
    next_hwnd = (HWND)((uint64_t)next_hwnd + 1);

    wnd->next = window_list;
    window_list = wnd;

    mutex_unlock(&window_lock);

    return wnd;
}

/**
 * Find window by HWND
 */
static window_t* window_find(HWND hwnd) {
    mutex_lock(&window_lock);

    window_t *wnd = window_list;
    while (wnd) {
        if (wnd->hwnd == hwnd) {
            mutex_unlock(&window_lock);
            return wnd;
        }
        wnd = wnd->next;
    }

    mutex_unlock(&window_lock);
    return NULL;
}

/**
 * CreateWindowExW - Create a window
 */
HWND WINAPI CreateWindowExW(
    DWORD dwExStyle,
    LPCWSTR lpClassName,
    LPCWSTR lpWindowName,
    DWORD dwStyle,
    int X,
    int Y,
    int nWidth,
    int nHeight,
    HWND hWndParent,
    HMENU hMenu,
    HINSTANCE hInstance,
    LPVOID lpParam)
{
    char class_name[256], window_name[256];
    wchar_to_utf8(lpClassName, class_name, sizeof(class_name));
    wchar_to_utf8(lpWindowName, window_name, sizeof(window_name));

    printf("CreateWindowExW: class='%s' name='%s' pos=(%d,%d) size=(%d,%d)\n",
           class_name, window_name, X, Y, nWidth, nHeight);

    window_t *wnd = window_create();
    if (!wnd) {
        win32_set_last_error(ERROR_NOT_ENOUGH_MEMORY);
        return NULL;
    }

    // Set window properties
    if (lpClassName) {
        wcsncpy(wnd->class_name, lpClassName, 255);
    }
    if (lpWindowName) {
        wcsncpy(wnd->window_name, lpWindowName, 255);
    }

    wnd->style = dwStyle;
    wnd->ex_style = dwExStyle;
    wnd->x = X;
    wnd->y = Y;
    wnd->width = nWidth;
    wnd->height = nHeight;
    wnd->parent = hWndParent;
    wnd->visible = false;

    printf("CreateWindowExW: Created window %p\n", wnd->hwnd);

    return wnd->hwnd;
}

/**
 * DestroyWindow - Destroy a window
 */
BOOL WINAPI DestroyWindow(HWND hWnd) {
    printf("DestroyWindow: %p\n", hWnd);

    window_t *wnd = window_find(hWnd);
    if (!wnd) {
        win32_set_last_error(ERROR_INVALID_HANDLE);
        return FALSE;
    }

    // Remove from list
    mutex_lock(&window_lock);

    window_t **prev = &window_list;
    window_t *curr = window_list;

    while (curr) {
        if (curr == wnd) {
            *prev = curr->next;
            break;
        }
        prev = &curr->next;
        curr = curr->next;
    }

    mutex_unlock(&window_lock);

    free(wnd);

    return TRUE;
}

/**
 * ShowWindow - Show or hide a window
 */
BOOL WINAPI ShowWindow(HWND hWnd, int nCmdShow) {
    printf("ShowWindow: %p cmd=%d\n", hWnd, nCmdShow);

    window_t *wnd = window_find(hWnd);
    if (!wnd) {
        return FALSE;
    }

    // nCmdShow values:
    // 0 = SW_HIDE
    // 1 = SW_SHOWNORMAL
    // etc.

    wnd->visible = (nCmdShow != 0);

    return TRUE;
}

/**
 * UpdateWindow - Update window
 */
BOOL WINAPI UpdateWindow(HWND hWnd) {
    printf("UpdateWindow: %p\n", hWnd);

    window_t *wnd = window_find(hWnd);
    if (!wnd) {
        return FALSE;
    }

    // Would trigger repaint here
    return TRUE;
}

/**
 * GetMessageW - Get message from message queue
 */
BOOL WINAPI GetMessageW(
    LPMSG lpMsg,
    HWND hWnd,
    UINT wMsgFilterMin,
    UINT wMsgFilterMax)
{
    if (!lpMsg) {
        return FALSE;
    }

    // Simple implementation: return WM_QUIT after a delay
    // Real implementation would have message queue per thread

    static int message_count = 0;
    message_count++;

    if (message_count > 100) {
        // Return WM_QUIT
        lpMsg->hwnd = hWnd;
        lpMsg->message = 0x0012; // WM_QUIT
        lpMsg->wParam = 0;
        lpMsg->lParam = 0;
        lpMsg->time = GetTickCount();
        lpMsg->pt.x = 0;
        lpMsg->pt.y = 0;

        printf("GetMessageW: WM_QUIT\n");
        return FALSE;
    }

    // Return dummy message
    lpMsg->hwnd = hWnd;
    lpMsg->message = 0x0001; // WM_CREATE
    lpMsg->wParam = 0;
    lpMsg->lParam = 0;
    lpMsg->time = GetTickCount();
    lpMsg->pt.x = 0;
    lpMsg->pt.y = 0;

    Sleep(10); // Simulate delay

    return TRUE;
}

/**
 * TranslateMessage - Translate message
 */
BOOL WINAPI TranslateMessage(const MSG *lpMsg) {
    // Would translate virtual key messages to character messages
    return TRUE;
}

/**
 * DispatchMessageW - Dispatch message to window procedure
 */
LONG WINAPI DispatchMessageW(const MSG *lpMsg) {
    if (!lpMsg) {
        return 0;
    }

    window_t *wnd = window_find(lpMsg->hwnd);
    if (!wnd || !wnd->wnd_proc) {
        return 0;
    }

    // Call window procedure
    return wnd->wnd_proc(lpMsg->hwnd, lpMsg->message, lpMsg->wParam, lpMsg->lParam);
}

/**
 * PostQuitMessage - Post quit message
 */
VOID WINAPI PostQuitMessage(int nExitCode) {
    printf("PostQuitMessage: exit code %d\n", nExitCode);
    // Would post WM_QUIT to message queue
}

/**
 * GetAsyncKeyState - Get key state
 */
SHORT WINAPI GetAsyncKeyState(int vKey) {
    // Would check keyboard state
    // For now, return not pressed
    return 0;
}

/**
 * GetCursorPos - Get cursor position
 */
BOOL WINAPI GetCursorPos(LPPOINT lpPoint) {
    if (!lpPoint) {
        win32_set_last_error(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    // Return dummy position
    lpPoint->x = 0;
    lpPoint->y = 0;

    return TRUE;
}

/**
 * DefWindowProcW - Default window procedure
 */
LONG WINAPI DefWindowProcW(HWND hWnd, UINT Msg, DWORD wParam, DWORD lParam) {
    // Default message processing
    return 0;
}

/**
 * Print window list (debug)
 */
void user32_print_windows(void) {
    printf("=== Window List ===\n");

    mutex_lock(&window_lock);

    window_t *wnd = window_list;
    int count = 0;

    while (wnd) {
        char class_name[256], window_name[256];
        wchar_to_utf8(wnd->class_name, class_name, sizeof(class_name));
        wchar_to_utf8(wnd->window_name, window_name, sizeof(window_name));

        printf("[%d] HWND %p: '%s' (%s) %dx%d at (%d,%d) %s\n",
               count, wnd->hwnd, window_name, class_name,
               wnd->width, wnd->height, wnd->x, wnd->y,
               wnd->visible ? "visible" : "hidden");

        wnd = wnd->next;
        count++;
    }

    mutex_unlock(&window_lock);

    printf("Total: %d windows\n", count);
    printf("===================\n");
}
