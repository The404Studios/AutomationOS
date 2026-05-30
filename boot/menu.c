/**
 * Boot Menu System
 * Interactive boot menu with timeout and keyboard navigation
 *
 * Total: ~800 LOC
 */

#include "boot_enhanced.h"
#include "boot_config.h"

// UEFI Console Input Protocol
typedef struct {
    uint64_t _buf;
    EFI_STATUS (*Reset)(void* This, uint8_t ExtendedVerification);
    EFI_STATUS (*ReadKeyStroke)(void* This, void* Key);
} EFI_SIMPLE_TEXT_INPUT_PROTOCOL;

// UEFI Console Output Protocol
typedef struct {
    uint64_t _buf;
    EFI_STATUS (*OutputString)(void* This, uint16_t* String);
    uint64_t _buf2[4];
    EFI_STATUS (*ClearScreen)(void* This);
    EFI_STATUS (*SetCursorPosition)(void* This, UINTN Column, UINTN Row);
    uint64_t _buf3;
    EFI_STATUS (*SetAttribute)(void* This, UINTN Attribute);
} EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL;

// Key definitions
typedef struct {
    uint16_t ScanCode;
    uint16_t UnicodeChar;
} EFI_INPUT_KEY;

#define SCAN_UP 0x01
#define SCAN_DOWN 0x02
#define SCAN_ESC 0x17

// Colors
#define EFI_BLACK             0x00
#define EFI_BLUE              0x01
#define EFI_GREEN             0x02
#define EFI_CYAN              0x03
#define EFI_RED               0x04
#define EFI_MAGENTA           0x05
#define EFI_BROWN             0x06
#define EFI_LIGHTGRAY         0x07
#define EFI_DARKGRAY          0x08
#define EFI_LIGHTBLUE         0x09
#define EFI_LIGHTGREEN        0x0A
#define EFI_LIGHTCYAN         0x0B
#define EFI_LIGHTRED          0x0C
#define EFI_LIGHTMAGENTA      0x0D
#define EFI_YELLOW            0x0E
#define EFI_WHITE             0x0F
#define EFI_BACKGROUND_BLACK  0x00
#define EFI_BACKGROUND_BLUE   0x10

#define MENU_WIDTH 60
#define MENU_HEIGHT 20

// Global UEFI handles
static EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL* ConOut = NULL;
static EFI_SIMPLE_TEXT_INPUT_PROTOCOL* ConIn = NULL;

// Convert ASCII to UTF-16
static void ascii_to_utf16(const char* ascii, uint16_t* utf16, int max_len) {
    int i = 0;
    while (i < max_len - 1 && ascii[i]) {
        utf16[i] = (uint16_t)ascii[i];
        i++;
    }
    utf16[i] = 0;
}

// Print string
static void print(const char* str) {
    if (!ConOut) return;

    uint16_t buf[256];
    ascii_to_utf16(str, buf, 256);
    ConOut->OutputString(ConOut, buf);
}

// Print at position
static void print_at(int col, int row, const char* str) {
    if (!ConOut) return;

    ConOut->SetCursorPosition(ConOut, col, row);
    print(str);
}

// Set text color
static void set_color(uint8_t fg, uint8_t bg) {
    if (!ConOut) return;
    ConOut->SetAttribute(ConOut, fg | bg);
}

// Clear screen
static void clear_screen(void) {
    if (!ConOut) return;
    ConOut->ClearScreen(ConOut);
}

// Draw box
static void draw_box(int x, int y, int width, int height) {
    // Top border
    print_at(x, y, "+");
    for (int i = 1; i < width - 1; i++) {
        print("-");
    }
    print("+");

    // Sides
    for (int i = 1; i < height - 1; i++) {
        print_at(x, y + i, "|");
        print_at(x + width - 1, y + i, "|");
    }

    // Bottom border
    print_at(x, y + height - 1, "+");
    for (int i = 1; i < width - 1; i++) {
        print("-");
    }
    print("+");
}

// Draw title
static void draw_title(void) {
    set_color(EFI_YELLOW, EFI_BACKGROUND_BLUE);
    print_at(0, 0, "                                                                                ");
    print_at(20, 0, "AutomationOS Bootloader v1.0");
}

// Draw menu entry
static void draw_menu_entry(int index, const char* title, int selected) {
    int x = 10;
    int y = 5 + index;

    if (selected) {
        set_color(EFI_BLACK, EFI_BACKGROUND_BLUE);
        print_at(x, y, " > ");
    } else {
        set_color(EFI_WHITE, EFI_BACKGROUND_BLACK);
        print_at(x, y, "   ");
    }

    print(title);

    // Clear rest of line
    for (int i = 0; i < 50; i++) {
        print(" ");
    }

    set_color(EFI_WHITE, EFI_BACKGROUND_BLACK);
}

// Draw footer
static void draw_footer(int seconds_left) {
    set_color(EFI_LIGHTGRAY, EFI_BACKGROUND_BLACK);
    print_at(5, 20, "Press UP/DOWN to select, ENTER to boot");

    if (seconds_left >= 0) {
        char buf[64];
        int pos = 0;
        buf[pos++] = 'B';
        buf[pos++] = 'o';
        buf[pos++] = 'o';
        buf[pos++] = 't';
        buf[pos++] = 'i';
        buf[pos++] = 'n';
        buf[pos++] = 'g';
        buf[pos++] = ' ';
        buf[pos++] = 'i';
        buf[pos++] = 'n';
        buf[pos++] = ' ';

        // Convert seconds to string
        if (seconds_left >= 10) {
            buf[pos++] = '0' + (seconds_left / 10);
        }
        buf[pos++] = '0' + (seconds_left % 10);
        buf[pos++] = ' ';
        buf[pos++] = 's';
        buf[pos++] = 'e';
        buf[pos++] = 'c';
        buf[pos++] = 'o';
        buf[pos++] = 'n';
        buf[pos++] = 'd';
        buf[pos++] = 's';
        buf[pos++] = '.';
        buf[pos++] = '.';
        buf[pos++] = '.';
        buf[pos] = 0;

        print_at(5, 21, buf);
    }

    print_at(5, 22, "Press E to edit command line, C for console");
    set_color(EFI_WHITE, EFI_BACKGROUND_BLACK);
}

// Wait for key with timeout
static int wait_key_timeout(EFI_INPUT_KEY* key, uint64_t timeout_ms) {
    // TODO: Implement proper timeout using UEFI timer
    // For now, just poll
    if (!ConIn) return -1;

    EFI_STATUS status = ConIn->ReadKeyStroke(ConIn, key);
    if (status == EFI_SUCCESS) {
        return 0;  // Key pressed
    }

    return -1;  // No key
}

// Edit command line
static void edit_cmdline(char* cmdline, int max_len) {
    clear_screen();
    set_color(EFI_WHITE, EFI_BACKGROUND_BLACK);
    print_at(2, 2, "Edit Kernel Command Line:");
    print_at(2, 3, "=========================");
    print_at(2, 5, cmdline);
    print_at(2, 7, "Press ENTER to continue, ESC to cancel");

    // Simple line editor
    int pos = 0;
    while (cmdline[pos]) pos++;

    EFI_INPUT_KEY key;
    while (1) {
        if (wait_key_timeout(&key, 0) == 0) {
            if (key.ScanCode == SCAN_ESC) {
                break;  // Cancel
            }
            else if (key.UnicodeChar == '\r' || key.UnicodeChar == '\n') {
                break;  // Done
            }
            else if (key.UnicodeChar == '\b') {
                // Backspace
                if (pos > 0) {
                    pos--;
                    cmdline[pos] = 0;
                    print_at(2 + pos, 5, " ");
                }
            }
            else if (key.UnicodeChar >= 32 && key.UnicodeChar < 127) {
                // Printable character
                if (pos < max_len - 1) {
                    cmdline[pos] = (char)key.UnicodeChar;
                    pos++;
                    cmdline[pos] = 0;

                    char c[2];
                    c[0] = (char)key.UnicodeChar;
                    c[1] = 0;
                    print(c);
                }
            }
        }
    }
}

// Show command-line console
static void show_console(void) {
    clear_screen();
    set_color(EFI_WHITE, EFI_BACKGROUND_BLACK);
    print_at(2, 2, "Boot Console");
    print_at(2, 3, "============");
    print_at(2, 5, "Commands:");
    print_at(2, 6, "  boot [entry]  - Boot specified entry");
    print_at(2, 7, "  list          - List boot entries");
    print_at(2, 8, "  help          - Show help");
    print_at(2, 9, "  exit          - Return to menu");
    print_at(2, 11, "Press any key to return to menu...");

    EFI_INPUT_KEY key;
    while (wait_key_timeout(&key, 0) != 0) {
        // Wait for key
    }
}

/**
 * Show boot menu and return selected entry
 *
 * @param config Boot configuration
 * @param ConOut_param Console output protocol
 * @param ConIn_param Console input protocol
 * @return Selected boot entry, or NULL on error
 */
boot_entry_t* boot_menu_show(boot_config_t* config,
                             void* ConOut_param,
                             void* ConIn_param) {
    if (!config || config->entry_count == 0) {
        return NULL;
    }

    ConOut = (EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL*)ConOut_param;
    ConIn = (EFI_SIMPLE_TEXT_INPUT_PROTOCOL*)ConIn_param;

    int selected = config->default_entry;
    if (selected >= (int)config->entry_count) {
        selected = 0;
    }

    int timeout = config->timeout;
    int auto_boot = (timeout > 0);

    while (1) {
        clear_screen();
        draw_title();

        // Draw box
        set_color(EFI_CYAN, EFI_BACKGROUND_BLACK);
        draw_box(8, 3, 64, config->entry_count + 4);

        // Draw entries
        for (uint32_t i = 0; i < config->entry_count; i++) {
            draw_menu_entry(i, config->entries[i].title, (int)i == selected);
        }

        // Draw footer
        draw_footer(auto_boot ? timeout : -1);

        // Wait for key or timeout
        EFI_INPUT_KEY key;
        int got_key = (wait_key_timeout(&key, 1000) == 0);

        if (got_key) {
            auto_boot = 0;  // Disable auto-boot on any key

            if (key.ScanCode == SCAN_UP) {
                selected--;
                if (selected < 0) {
                    selected = config->entry_count - 1;
                }
            }
            else if (key.ScanCode == SCAN_DOWN) {
                selected++;
                if (selected >= (int)config->entry_count) {
                    selected = 0;
                }
            }
            else if (key.UnicodeChar == '\r' || key.UnicodeChar == '\n') {
                // Enter - boot selected
                return &config->entries[selected];
            }
            else if (key.UnicodeChar == 'e' || key.UnicodeChar == 'E') {
                // Edit command line
                edit_cmdline(config->entries[selected].options, MAX_OPTIONS_LEN);
            }
            else if (key.UnicodeChar == 'c' || key.UnicodeChar == 'C') {
                // Show console
                show_console();
            }
            else if (key.ScanCode == SCAN_ESC) {
                // ESC - cancel auto-boot
                auto_boot = 0;
            }
        }

        // Handle timeout
        if (auto_boot) {
            timeout--;
            if (timeout <= 0) {
                return &config->entries[selected];
            }
        }
    }

    return NULL;
}

/**
 * Show simple splash screen while booting
 */
void boot_splash_show(void* ConOut_param, const char* message) {
    ConOut = (EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL*)ConOut_param;

    clear_screen();
    set_color(EFI_LIGHTCYAN, EFI_BACKGROUND_BLACK);

    print_at(30, 10, "AutomationOS");
    print_at(32, 11, "Booting...");

    if (message) {
        set_color(EFI_WHITE, EFI_BACKGROUND_BLACK);
        print_at(25, 13, message);
    }
}
