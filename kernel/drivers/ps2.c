#include "../include/x86_64.h"
#include "../include/types.h"
#include "../include/kernel.h"

// PS/2 controller ports
#define PS2_DATA_PORT    0x60
#define PS2_STATUS_PORT  0x64
#define PS2_COMMAND_PORT 0x64

// PS/2 controller status bits
#define PS2_STATUS_OUTPUT_FULL  0x01
#define PS2_STATUS_INPUT_FULL   0x02

// PS/2 controller commands
#define PS2_CMD_READ_CONFIG     0x20
#define PS2_CMD_WRITE_CONFIG    0x60
#define PS2_CMD_DISABLE_PORT2   0xA7
#define PS2_CMD_ENABLE_PORT1    0xAE
#define PS2_CMD_TEST_PORT1      0xAB

// Keyboard commands
#define KB_CMD_ENABLE_SCAN      0xF4
#define KB_CMD_RESET            0xFF

// Keyboard buffer
#define KB_BUFFER_SIZE 256

static char keyboard_buffer[KB_BUFFER_SIZE];
static volatile uint32_t kb_read_pos = 0;
static volatile uint32_t kb_write_pos = 0;
static bool ps2_initialized = false;

// Scancode to ASCII translation table (US QWERTY, scancode set 1)
// 0 means no ASCII equivalent (control keys, etc.)
static const char scancode_to_ascii[128] = {
    0,    27,  '1', '2', '3', '4', '5', '6',  // 0x00-0x07
    '7', '8', '9', '0', '-', '=', '\b', '\t', // 0x08-0x0F
    'q', 'w', 'e', 'r', 't', 'y', 'u', 'i',   // 0x10-0x17
    'o', 'p', '[', ']', '\n', 0,   'a', 's',  // 0x18-0x1F
    'd', 'f', 'g', 'h', 'j', 'k', 'l', ';',   // 0x20-0x27
    '\'', '`', 0,   '\\', 'z', 'x', 'c', 'v', // 0x28-0x2F
    'b', 'n', 'm', ',', '.', '/', 0,   '*',   // 0x30-0x37
    0,   ' ', 0,   0,   0,   0,   0,   0,     // 0x38-0x3F (Alt, Space, Caps, F1-F5)
    0,   0,   0,   0,   0,   0,   0,   '7',   // 0x40-0x47 (F6-F10, Num Lock, Scroll, Keypad 7)
    '8', '9', '-', '4', '5', '6', '+', '1',   // 0x48-0x4F (Keypad)
    '2', '3', '0', '.', 0,   0,   0,   0,     // 0x50-0x57 (Keypad, F11, F12)
    0,   0,   0,   0,   0,   0,   0,   0      // 0x58-0x5F
};

// Shifted characters
static const char scancode_to_ascii_shift[128] = {
    0,    27,  '!', '@', '#', '$', '%', '^',  // 0x00-0x07
    '&', '*', '(', ')', '_', '+', '\b', '\t', // 0x08-0x0F
    'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I',   // 0x10-0x17
    'O', 'P', '{', '}', '\n', 0,   'A', 'S',  // 0x18-0x1F
    'D', 'F', 'G', 'H', 'J', 'K', 'L', ':',   // 0x20-0x27
    '"', '~', 0,   '|', 'Z', 'X', 'C', 'V',   // 0x28-0x2F
    'B', 'N', 'M', '<', '>', '?', 0,   '*',   // 0x30-0x37
    0,   ' ', 0,   0,   0,   0,   0,   0,     // 0x38-0x3F
    0,   0,   0,   0,   0,   0,   0,   '7',   // 0x40-0x47
    '8', '9', '-', '4', '5', '6', '+', '1',   // 0x48-0x4F
    '2', '3', '0', '.', 0,   0,   0,   0,     // 0x50-0x57
    0,   0,   0,   0,   0,   0,   0,   0      // 0x58-0x5F
};

// Shift state tracking
static bool shift_pressed = false;
static bool ctrl_pressed = false;
static bool alt_pressed = false;
static bool caps_lock = false;

// Wait for PS/2 controller to be ready for input
static void ps2_wait_input(void) {
    uint32_t timeout = 100000;
    while (timeout--) {
        if (!(inb(PS2_STATUS_PORT) & PS2_STATUS_INPUT_FULL)) {
            return;
        }
    }
}

// Wait for PS/2 controller to have output available
static void ps2_wait_output(void) {
    uint32_t timeout = 100000;
    while (timeout--) {
        if (inb(PS2_STATUS_PORT) & PS2_STATUS_OUTPUT_FULL) {
            return;
        }
    }
}

// Read byte from PS/2 data port
static uint8_t ps2_read_data(void) {
    ps2_wait_output();
    return inb(PS2_DATA_PORT);
}

// Write byte to PS/2 data port
static void ps2_write_data(uint8_t data) {
    ps2_wait_input();
    outb(PS2_DATA_PORT, data);
}

// Write command to PS/2 command port
static void ps2_write_command(uint8_t cmd) {
    ps2_wait_input();
    outb(PS2_COMMAND_PORT, cmd);
}

// Add character to keyboard buffer
static void kb_buffer_put(char c) {
    uint32_t next_pos = (kb_write_pos + 1) % KB_BUFFER_SIZE;
    if (next_pos != kb_read_pos) {
        keyboard_buffer[kb_write_pos] = c;
        kb_write_pos = next_pos;
    }
}

// Handle keyboard scancode
static void ps2_handle_scancode(uint8_t scancode) {
    // Check for key release (high bit set)
    bool key_released = (scancode & 0x80) != 0;
    scancode &= 0x7F;  // Clear release bit

    // Handle modifier keys
    if (scancode == 0x2A || scancode == 0x36) {  // Left/Right Shift
        shift_pressed = !key_released;
        return;
    }
    if (scancode == 0x1D) {  // Ctrl
        ctrl_pressed = !key_released;
        return;
    }
    if (scancode == 0x38) {  // Alt
        alt_pressed = !key_released;
        return;
    }
    if (scancode == 0x3A && !key_released) {  // Caps Lock (toggle on press)
        caps_lock = !caps_lock;
        return;
    }

    // Only process key press events (not releases) for regular keys
    if (key_released) {
        return;
    }

    // Translate scancode to ASCII
    char c;
    if (shift_pressed) {
        c = scancode_to_ascii_shift[scancode];
    } else {
        c = scancode_to_ascii[scancode];
    }

    // Apply caps lock to letters
    if (caps_lock && c >= 'a' && c <= 'z') {
        c = c - 'a' + 'A';
    } else if (caps_lock && c >= 'A' && c <= 'Z') {
        c = c - 'A' + 'a';
    }

    // Handle Ctrl+key combinations
    if (ctrl_pressed && c >= 'a' && c <= 'z') {
        c = c - 'a' + 1;  // Convert to control character
    } else if (ctrl_pressed && c >= 'A' && c <= 'Z') {
        c = c - 'A' + 1;
    }

    // Add to buffer if it's a valid character
    if (c != 0) {
        kb_buffer_put(c);
    }
}

// Keyboard interrupt handler (will be called by IRQ1 handler when IDT is implemented)
void ps2_irq_handler(void) {
    if (inb(PS2_STATUS_PORT) & PS2_STATUS_OUTPUT_FULL) {
        uint8_t scancode = inb(PS2_DATA_PORT);
        ps2_handle_scancode(scancode);
    }
}

// Initialize PS/2 keyboard
void ps2_init(void) {
    kprintf("[PS/2] Initializing keyboard controller...\n");

    // Disable first PS/2 port
    ps2_write_command(0xAD);

    // Disable second PS/2 port (if it exists)
    ps2_write_command(PS2_CMD_DISABLE_PORT2);

    // Flush output buffer
    while (inb(PS2_STATUS_PORT) & PS2_STATUS_OUTPUT_FULL) {
        inb(PS2_DATA_PORT);
    }

    // Read controller configuration
    ps2_write_command(PS2_CMD_READ_CONFIG);
    uint8_t config = ps2_read_data();

    // Enable interrupts and translation (when IDT is ready)
    config |= 0x01;  // Enable keyboard interrupt
    config &= ~0x10; // Enable keyboard

    // Write modified configuration
    ps2_write_command(PS2_CMD_WRITE_CONFIG);
    ps2_write_data(config);

    // Enable first PS/2 port
    ps2_write_command(PS2_CMD_ENABLE_PORT1);

    // Test PS/2 port
    ps2_write_command(PS2_CMD_TEST_PORT1);
    uint8_t test_result = ps2_read_data();
    if (test_result != 0x00) {
        kprintf("[PS/2] Warning: Port test failed (0x%x)\n", test_result);
    }

    // Reset and enable keyboard
    ps2_write_data(KB_CMD_RESET);
    uint8_t reset_response = ps2_read_data();
    if (reset_response != 0xFA) {  // 0xFA = ACK
        kprintf("[PS/2] Warning: Keyboard reset failed (0x%x)\n", reset_response);
    }

    // Wait for self-test result (0xAA = passed)
    uint8_t self_test = ps2_read_data();
    if (self_test != 0xAA) {
        kprintf("[PS/2] Warning: Keyboard self-test failed (0x%x)\n", self_test);
    }

    // Enable scanning
    ps2_write_data(KB_CMD_ENABLE_SCAN);
    uint8_t enable_response = ps2_read_data();
    if (enable_response != 0xFA) {
        kprintf("[PS/2] Warning: Enable scan failed (0x%x)\n", enable_response);
    }

    ps2_initialized = true;

    kprintf("[PS/2] Keyboard initialized\n");
    kprintf("[PS/2] Note: IRQ1 handler registration requires IDT (Task 10)\n");

    // TODO: When IDT is implemented (Task 10), register ps2_irq_handler for IRQ1
    // Example: register_irq_handler(1, ps2_irq_handler);
}

// Get a character from keyboard buffer (non-blocking)
// Returns 0 if no character available
char ps2_getchar(void) {
    if (!ps2_initialized) {
        return 0;
    }

    // Check if buffer is empty
    if (kb_read_pos == kb_write_pos) {
        // In polling mode (before IDT is implemented), check for data
        if (inb(PS2_STATUS_PORT) & PS2_STATUS_OUTPUT_FULL) {
            uint8_t scancode = inb(PS2_DATA_PORT);
            ps2_handle_scancode(scancode);
        }

        // Check again after polling
        if (kb_read_pos == kb_write_pos) {
            return 0;
        }
    }

    // Get character from buffer
    char c = keyboard_buffer[kb_read_pos];
    kb_read_pos = (kb_read_pos + 1) % KB_BUFFER_SIZE;

    return c;
}

// Blocking version - wait for a character
char ps2_getchar_blocking(void) {
    char c;
    while ((c = ps2_getchar()) == 0) {
        // Wait for character
        hlt();  // Save power while waiting
    }
    return c;
}
