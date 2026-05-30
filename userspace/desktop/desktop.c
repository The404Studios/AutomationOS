// desktop.c - AutomationOS Desktop Environment
// Renders directly to framebuffer mapped at 0x40000000
// Built by Elijah Isaiah Roberts (fourzerofour)

typedef unsigned int uint32_t;
typedef unsigned char uint8_t;

#include "../libc/syscall.h"
#include "../libc/string.h"

// Framebuffer at fixed userspace address (mapped by kernel)
#define FB_ADDR   0x40000000UL
#define FB_WIDTH  1024
#define FB_HEIGHT 768
#define FB_PITCH  (FB_WIDTH * 4)

// Colors (ARGB)
#define COLOR_BG        0x002B4570  // Dark blue background
#define COLOR_TASKBAR   0x001A1A2E  // Dark taskbar
#define COLOR_ACCENT    0x00E94560  // Red accent
#define COLOR_WHITE     0x00FFFFFF
#define COLOR_GRAY      0x00808080
#define COLOR_DARKGRAY  0x00404040
#define COLOR_LIGHTGRAY 0x00C0C0C0
#define COLOR_WINDOW    0x00F0F0F0  // Window background
#define COLOR_TITLEBAR  0x003A7BD5  // Window title bar
#define COLOR_SHADOW    0x00111111
#define COLOR_GREEN     0x0050C878
#define COLOR_DESKTOP   0x001E3A5F

static volatile uint32_t* fb = (volatile uint32_t*)FB_ADDR;

static void put_pixel(int x, int y, uint32_t color) {
    if (x >= 0 && x < FB_WIDTH && y >= 0 && y < FB_HEIGHT)
        fb[y * FB_WIDTH + x] = color;
}

static void fill_rect(int x, int y, int w, int h, uint32_t color) {
    for (int j = y; j < y + h && j < FB_HEIGHT; j++)
        for (int i = x; i < x + w && i < FB_WIDTH; i++)
            put_pixel(i, j, color);
}

static void draw_hline(int x, int y, int w, uint32_t color) {
    for (int i = x; i < x + w; i++) put_pixel(i, y, color);
}

static void draw_vline(int x, int y, int h, uint32_t color) {
    for (int j = y; j < y + h; j++) put_pixel(x, j, color);
}

static void draw_rect_outline(int x, int y, int w, int h, uint32_t color) {
    draw_hline(x, y, w, color);
    draw_hline(x, y + h - 1, w, color);
    draw_vline(x, y, h, color);
    draw_vline(x + w - 1, y, h, color);
}

// Simple 5x7 font bitmap (ASCII 32-126)
static const unsigned char font_5x7[][5] = {
    {0x00,0x00,0x00,0x00,0x00}, // space
    {0x00,0x00,0x5F,0x00,0x00}, // !
    {0x00,0x07,0x00,0x07,0x00}, // "
    {0x14,0x7F,0x14,0x7F,0x14}, // #
    {0x24,0x2A,0x7F,0x2A,0x12}, // $
    {0x23,0x13,0x08,0x64,0x62}, // %
    {0x36,0x49,0x56,0x20,0x50}, // &
    {0x00,0x08,0x07,0x03,0x00}, // '
    {0x00,0x1C,0x22,0x41,0x00}, // (
    {0x00,0x41,0x22,0x1C,0x00}, // )
    {0x2A,0x1C,0x7F,0x1C,0x2A}, // *
    {0x08,0x08,0x3E,0x08,0x08}, // +
    {0x00,0x80,0x70,0x30,0x00}, // ,
    {0x08,0x08,0x08,0x08,0x08}, // -
    {0x00,0x00,0x60,0x60,0x00}, // .
    {0x20,0x10,0x08,0x04,0x02}, // /
    {0x3E,0x51,0x49,0x45,0x3E}, // 0
    {0x00,0x42,0x7F,0x40,0x00}, // 1
    {0x72,0x49,0x49,0x49,0x46}, // 2
    {0x21,0x41,0x49,0x4D,0x33}, // 3
    {0x18,0x14,0x12,0x7F,0x10}, // 4
    {0x27,0x45,0x45,0x45,0x39}, // 5
    {0x3C,0x4A,0x49,0x49,0x31}, // 6
    {0x41,0x21,0x11,0x09,0x07}, // 7
    {0x36,0x49,0x49,0x49,0x36}, // 8
    {0x46,0x49,0x49,0x29,0x1E}, // 9
    {0x00,0x00,0x14,0x00,0x00}, // :
    {0x00,0x40,0x34,0x00,0x00}, // ;
    {0x00,0x08,0x14,0x22,0x41}, // <
    {0x14,0x14,0x14,0x14,0x14}, // =
    {0x00,0x41,0x22,0x14,0x08}, // >
    {0x02,0x01,0x59,0x09,0x06}, // ?
    {0x3E,0x41,0x5D,0x59,0x4E}, // @
    {0x7C,0x12,0x11,0x12,0x7C}, // A
    {0x7F,0x49,0x49,0x49,0x36}, // B
    {0x3E,0x41,0x41,0x41,0x22}, // C
    {0x7F,0x41,0x41,0x41,0x3E}, // D
    {0x7F,0x49,0x49,0x49,0x41}, // E
    {0x7F,0x09,0x09,0x09,0x01}, // F
    {0x3E,0x41,0x41,0x51,0x73}, // G
    {0x7F,0x08,0x08,0x08,0x7F}, // H
    {0x00,0x41,0x7F,0x41,0x00}, // I
    {0x20,0x40,0x41,0x3F,0x01}, // J
    {0x7F,0x08,0x14,0x22,0x41}, // K
    {0x7F,0x40,0x40,0x40,0x40}, // L
    {0x7F,0x02,0x1C,0x02,0x7F}, // M
    {0x7F,0x04,0x08,0x10,0x7F}, // N
    {0x3E,0x41,0x41,0x41,0x3E}, // O
    {0x7F,0x09,0x09,0x09,0x06}, // P
    {0x3E,0x41,0x51,0x21,0x5E}, // Q
    {0x7F,0x09,0x19,0x29,0x46}, // R
    {0x26,0x49,0x49,0x49,0x32}, // S
    {0x03,0x01,0x7F,0x01,0x03}, // T
    {0x3F,0x40,0x40,0x40,0x3F}, // U
    {0x1F,0x20,0x40,0x20,0x1F}, // V
    {0x3F,0x40,0x38,0x40,0x3F}, // W
    {0x63,0x14,0x08,0x14,0x63}, // X
    {0x03,0x04,0x78,0x04,0x03}, // Y
    {0x61,0x59,0x49,0x4D,0x43}, // Z
};

static void draw_char(int x, int y, char c, uint32_t color) {
    if (c < 32 || c > 90) return;
    int idx = c - 32;
    if (idx >= (int)(sizeof(font_5x7)/5)) return;
    for (int col = 0; col < 5; col++) {
        unsigned char bits = font_5x7[idx][col];
        for (int row = 0; row < 7; row++) {
            if (bits & (1 << row)) {
                put_pixel(x + col, y + row, color);
            }
        }
    }
}

static void draw_string(int x, int y, const char* s, uint32_t color) {
    for (int i = 0; s[i]; i++) {
        char c = s[i];
        if (c >= 'a' && c <= 'z') c -= 32; // font only has uppercase
        draw_char(x + i * 6, y, c, color);
    }
}

// ─── Desktop Drawing ────────────────────────────────────

static void draw_desktop_background(void) {
    // Gradient-ish background
    for (int y = 0; y < FB_HEIGHT; y++) {
        int r = 0x1E + (y * 0x20 / FB_HEIGHT);
        int g = 0x3A + (y * 0x15 / FB_HEIGHT);
        int b = 0x5F + (y * 0x30 / FB_HEIGHT);
        uint32_t color = (r << 16) | (g << 8) | b;
        for (int x = 0; x < FB_WIDTH; x++)
            fb[y * FB_WIDTH + x] = color;
    }
}

static void draw_taskbar(void) {
    // Bottom taskbar
    int bar_h = 40;
    int bar_y = FB_HEIGHT - bar_h;
    fill_rect(0, bar_y, FB_WIDTH, bar_h, COLOR_TASKBAR);
    draw_hline(0, bar_y, FB_WIDTH, COLOR_DARKGRAY);

    // Start button
    fill_rect(5, bar_y + 5, 80, 30, COLOR_ACCENT);
    draw_string(15, bar_y + 12, "AOS", COLOR_WHITE);

    // Clock area
    draw_string(FB_WIDTH - 80, bar_y + 12, "12:00", COLOR_LIGHTGRAY);

    // Status indicators
    fill_rect(FB_WIDTH - 120, bar_y + 15, 8, 8, COLOR_GREEN);  // "online" dot
    draw_string(FB_WIDTH - 108, bar_y + 12, "OK", COLOR_LIGHTGRAY);
}

static void draw_window(int x, int y, int w, int h, const char* title) {
    // Shadow
    fill_rect(x + 4, y + 4, w, h, COLOR_SHADOW);

    // Window body
    fill_rect(x, y, w, h, COLOR_WINDOW);

    // Title bar
    fill_rect(x, y, w, 28, COLOR_TITLEBAR);
    draw_string(x + 10, y + 8, title, COLOR_WHITE);

    // Close button
    fill_rect(x + w - 24, y + 4, 20, 20, COLOR_ACCENT);
    draw_string(x + w - 20, y + 8, "X", COLOR_WHITE);

    // Border
    draw_rect_outline(x, y, w, h, COLOR_DARKGRAY);
}

static void draw_welcome_window(void) {
    int wx = 200, wy = 150, ww = 500, wh = 350;
    draw_window(wx, wy, ww, wh, "WELCOME");

    int tx = wx + 20;
    int ty = wy + 50;

    draw_string(tx, ty,       "AUTOMATIONOS DESKTOP", COLOR_TITLEBAR);
    draw_string(tx, ty + 20,  "========================", COLOR_GRAY);
    draw_string(tx, ty + 45,  "BUILT FROM SCRATCH IN ONE SESSION", COLOR_DARKGRAY);
    draw_string(tx, ty + 65,  "BY ELIJAH ISAIAH ROBERTS", COLOR_DARKGRAY);
    draw_string(tx, ty + 85,  "(FOURZEROFOUR)", COLOR_DARKGRAY);
    draw_string(tx, ty + 115, "ARCHITECTURE:", COLOR_TITLEBAR);
    draw_string(tx, ty + 135, "  X86_64 LONG MODE", COLOR_DARKGRAY);
    draw_string(tx, ty + 155, "  PER-PROCESS ISOLATION", COLOR_DARKGRAY);
    draw_string(tx, ty + 175, "  RING 0/3 PROTECTION", COLOR_DARKGRAY);
    draw_string(tx, ty + 195, "  SYSCALL/SYSRET", COLOR_DARKGRAY);
    draw_string(tx, ty + 225, "STATUS: ALL SYSTEMS OPERATIONAL", COLOR_GREEN);
    draw_string(tx, ty + 255, "NO LIBRARIES. NO BORROWED CODE.", COLOR_ACCENT);
}

static void draw_desktop_icons(void) {
    // Terminal icon
    fill_rect(50, 50, 48, 48, COLOR_DARKGRAY);
    fill_rect(52, 52, 44, 44, 0x00202020);
    draw_string(56, 62, ">_", COLOR_GREEN);
    draw_string(42, 105, "TERMINAL", COLOR_WHITE);

    // Files icon
    fill_rect(50, 160, 48, 48, 0x00D4A017);
    fill_rect(52, 170, 44, 36, 0x00E8C838);
    draw_string(50, 215, "FILES", COLOR_WHITE);

    // Settings icon
    fill_rect(50, 270, 48, 48, COLOR_GRAY);
    draw_rect_outline(58, 278, 32, 32, COLOR_WHITE);
    draw_string(38, 325, "SETTINGS", COLOR_WHITE);
}

// ─── Entry Point ────────────────────────────────────────

void main(void) {
    // Draw the desktop
    draw_desktop_background();
    draw_taskbar();
    draw_desktop_icons();
    draw_welcome_window();

    // Keep process alive
    while (1) {
        yield();
    }
}
