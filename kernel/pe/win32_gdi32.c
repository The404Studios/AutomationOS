/**
 * Win32 GDI32.dll API Stubs
 *
 * Implementation of GDI32 APIs for graphics device interface.
 * Provides basic drawing functionality for Windows GUI applications.
 */

#include <kernel/pe_win32.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

// Device context structure
typedef struct device_context {
    HDC hdc;
    HWND hwnd;
    int width;
    int height;
    void *framebuffer;
    bool in_paint;
    struct device_context *next;
} device_context_t;

static device_context_t *dc_list = NULL;
static HDC next_hdc = (HDC)0x2000;
static mutex_t dc_lock;

/**
 * Initialize GDI32 subsystem
 */
void gdi32_init(void) {
    mutex_init(&dc_lock);
    dc_list = NULL;
    printf("GDI32: Initialized\n");
}

/**
 * Create device context
 */
static device_context_t* dc_create(HWND hwnd) {
    device_context_t *dc = calloc(1, sizeof(device_context_t));
    if (!dc) return NULL;

    mutex_lock(&dc_lock);

    dc->hdc = next_hdc;
    next_hdc = (HDC)((uint64_t)next_hdc + 1);
    dc->hwnd = hwnd;
    dc->width = 800;  // Default size
    dc->height = 600;

    // Allocate framebuffer (simple RGB buffer)
    dc->framebuffer = malloc(dc->width * dc->height * 4);

    // Add to linked list
    dc->next = dc_list;
    dc_list = dc;

    mutex_unlock(&dc_lock);

    return dc;
}

/**
 * Find device context
 */
static device_context_t* dc_find(HDC hdc) {
    mutex_lock(&dc_lock);

    device_context_t *dc = dc_list;
    while (dc) {
        if (dc->hdc == hdc) {
            mutex_unlock(&dc_lock);
            return dc;
        }
        dc = dc->next;
    }

    mutex_unlock(&dc_lock);
    return NULL;
}

/**
 * BeginPaint - Begin painting window
 */
HDC WINAPI BeginPaint(HWND hWnd, LPPAINTSTRUCT lpPaint) {
    printf("BeginPaint: window %p\n", hWnd);

    if (!lpPaint) {
        win32_set_last_error(ERROR_INVALID_PARAMETER);
        return NULL;
    }

    device_context_t *dc = dc_create(hWnd);
    if (!dc) {
        win32_set_last_error(ERROR_NOT_ENOUGH_MEMORY);
        return NULL;
    }

    dc->in_paint = true;

    // Fill paint structure
    lpPaint->hdc = dc->hdc;
    lpPaint->fErase = TRUE;
    lpPaint->rcPaint.left = 0;
    lpPaint->rcPaint.top = 0;
    lpPaint->rcPaint.right = dc->width;
    lpPaint->rcPaint.bottom = dc->height;
    lpPaint->fRestore = FALSE;
    lpPaint->fIncUpdate = FALSE;
    memset(lpPaint->rgbReserved, 0, sizeof(lpPaint->rgbReserved));

    return dc->hdc;
}

/**
 * EndPaint - End painting window
 */
BOOL WINAPI EndPaint(HWND hWnd, const PAINTSTRUCT *lpPaint) {
    printf("EndPaint: window %p\n", hWnd);

    if (!lpPaint) {
        win32_set_last_error(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    device_context_t *dc = dc_find(lpPaint->hdc);
    if (!dc) {
        return FALSE;
    }

    dc->in_paint = false;

    // Would present framebuffer to screen here

    return TRUE;
}

/**
 * GetDC - Get device context for window
 */
HDC WINAPI GetDC(HWND hWnd) {
    printf("GetDC: window %p\n", hWnd);

    device_context_t *dc = dc_create(hWnd);
    if (!dc) {
        win32_set_last_error(ERROR_NOT_ENOUGH_MEMORY);
        return NULL;
    }

    return dc->hdc;
}

/**
 * ReleaseDC - Release device context
 */
int WINAPI ReleaseDC(HWND hWnd, HDC hDC) {
    printf("ReleaseDC: window %p, dc %p\n", hWnd, hDC);

    device_context_t *dc = dc_find(hDC);
    if (!dc) {
        return 0;
    }

    if (dc->framebuffer) {
        free(dc->framebuffer);
    }

    free(dc);

    return 1;
}

/**
 * TextOutW - Output text
 */
BOOL WINAPI TextOutW(HDC hdc, int x, int y, LPCWSTR lpString, int c) {
    char text[256];
    wchar_to_utf8(lpString, text, sizeof(text));

    printf("TextOutW: dc=%p pos=(%d,%d) text='%s'\n", hdc, x, y, text);

    device_context_t *dc = dc_find(hdc);
    if (!dc) {
        win32_set_last_error(ERROR_INVALID_HANDLE);
        return FALSE;
    }

    // Would render text to framebuffer here
    // For now, just log it

    return TRUE;
}

/**
 * Rectangle - Draw rectangle
 */
BOOL WINAPI Rectangle(HDC hdc, int left, int top, int right, int bottom) {
    printf("Rectangle: dc=%p rect=(%d,%d,%d,%d)\n", hdc, left, top, right, bottom);

    device_context_t *dc = dc_find(hdc);
    if (!dc) {
        win32_set_last_error(ERROR_INVALID_HANDLE);
        return FALSE;
    }

    // Would draw rectangle to framebuffer here

    return TRUE;
}

/**
 * SetPixel - Set pixel color
 */
DWORD WINAPI SetPixel(HDC hdc, int x, int y, DWORD color) {
    device_context_t *dc = dc_find(hdc);
    if (!dc) {
        win32_set_last_error(ERROR_INVALID_HANDLE);
        return (DWORD)-1;
    }

    if (x < 0 || x >= dc->width || y < 0 || y >= dc->height) {
        return (DWORD)-1;
    }

    // Set pixel in framebuffer
    if (dc->framebuffer) {
        uint32_t *fb = (uint32_t *)dc->framebuffer;
        fb[y * dc->width + x] = color;
    }

    return color;
}

/**
 * LineTo - Draw line
 */
BOOL WINAPI LineTo(HDC hdc, int x, int y) {
    printf("LineTo: dc=%p to=(%d,%d)\n", hdc, x, y);

    device_context_t *dc = dc_find(hdc);
    if (!dc) {
        win32_set_last_error(ERROR_INVALID_HANDLE);
        return FALSE;
    }

    // Would draw line to framebuffer here

    return TRUE;
}

/**
 * MoveToEx - Move current position
 */
BOOL WINAPI MoveToEx(HDC hdc, int x, int y, LPPOINT lppt) {
    printf("MoveToEx: dc=%p to=(%d,%d)\n", hdc, x, y);

    device_context_t *dc = dc_find(hdc);
    if (!dc) {
        win32_set_last_error(ERROR_INVALID_HANDLE);
        return FALSE;
    }

    // Would set current position here
    // For now, just return success

    if (lppt) {
        lppt->x = 0;
        lppt->y = 0;
    }

    return TRUE;
}

/**
 * CreateSolidBrush - Create solid brush
 */
HBRUSH WINAPI CreateSolidBrush(DWORD color) {
    printf("CreateSolidBrush: color=0x%08x\n", color);

    // Return dummy brush handle
    return (HBRUSH)(uint64_t)0x3000;
}

/**
 * SelectObject - Select GDI object into DC
 */
HANDLE WINAPI SelectObject(HDC hdc, HANDLE h) {
    printf("SelectObject: dc=%p object=%p\n", hdc, h);

    device_context_t *dc = dc_find(hdc);
    if (!dc) {
        win32_set_last_error(ERROR_INVALID_HANDLE);
        return NULL;
    }

    // Would select object (brush, pen, font, etc.) into DC
    // Return previous object
    return (HANDLE)(uint64_t)0x3001;
}

/**
 * DeleteObject - Delete GDI object
 */
BOOL WINAPI DeleteObject(HANDLE h) {
    printf("DeleteObject: %p\n", h);

    // Would free GDI object
    return TRUE;
}

/**
 * Print device context list (debug)
 */
void gdi32_print_contexts(void) {
    printf("=== Device Context List ===\n");

    mutex_lock(&dc_lock);

    device_context_t *dc = dc_list;
    int count = 0;

    while (dc) {
        printf("[%d] HDC %p: window %p, size %dx%d, %s\n",
               count, dc->hdc, dc->hwnd,
               dc->width, dc->height,
               dc->in_paint ? "painting" : "idle");

        dc = dc->next;
        count++;
    }

    mutex_unlock(&dc_lock);

    printf("Total: %d contexts\n", count);
    printf("===========================\n");
}
