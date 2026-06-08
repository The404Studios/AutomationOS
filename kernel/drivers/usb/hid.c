/*
 * USB HID (Human Interface Device) Driver
 * Supports keyboards, mice, and gamepads
 */

#include "../../include/usb.h"
#include "../../include/input.h"
#include "../../include/kernel.h"
#include "../../include/types.h"
#include "../../include/mem.h"
#include <string.h>

// HID Report Descriptor Item Tags
#define HID_ITEM_TYPE_MAIN        0x0
#define HID_ITEM_TYPE_GLOBAL      0x1
#define HID_ITEM_TYPE_LOCAL       0x2

#define HID_MAIN_INPUT            0x8
#define HID_MAIN_OUTPUT           0x9
#define HID_MAIN_COLLECTION       0xA
#define HID_MAIN_FEATURE          0xB
#define HID_MAIN_END_COLLECTION   0xC

#define HID_GLOBAL_USAGE_PAGE     0x0
#define HID_GLOBAL_LOGICAL_MIN    0x1
#define HID_GLOBAL_LOGICAL_MAX    0x2
#define HID_GLOBAL_PHYSICAL_MIN   0x3
#define HID_GLOBAL_PHYSICAL_MAX   0x4
#define HID_GLOBAL_REPORT_SIZE    0x7
#define HID_GLOBAL_REPORT_ID      0x8
#define HID_GLOBAL_REPORT_COUNT   0x9

#define HID_LOCAL_USAGE           0x0
#define HID_LOCAL_USAGE_MIN       0x1
#define HID_LOCAL_USAGE_MAX       0x2

// HID Usage Pages
#define HID_USAGE_PAGE_GENERIC    0x01
#define HID_USAGE_PAGE_KEYBOARD   0x07
#define HID_USAGE_PAGE_LEDS       0x08
#define HID_USAGE_PAGE_BUTTON     0x09

// HID Generic Desktop Usages
#define HID_USAGE_POINTER         0x01
#define HID_USAGE_MOUSE           0x02
#define HID_USAGE_JOYSTICK        0x04
#define HID_USAGE_GAMEPAD         0x05
#define HID_USAGE_KEYBOARD        0x06
#define HID_USAGE_X               0x30
#define HID_USAGE_Y               0x31
#define HID_USAGE_Z               0x32
#define HID_USAGE_RX              0x33
#define HID_USAGE_RY              0x34
#define HID_USAGE_RZ              0x35
#define HID_USAGE_WHEEL           0x38

// HID Device Types
typedef enum {
    HID_DEVICE_KEYBOARD = 0,
    HID_DEVICE_MOUSE = 1,
    HID_DEVICE_GAMEPAD = 2,
    HID_DEVICE_UNKNOWN = 3
} hid_device_type_t;

// HID Report structure
typedef struct {
    uint16_t usage_page;
    uint16_t usage;
    uint8_t report_id;
    uint8_t report_size;
    uint8_t report_count;
    int32_t logical_min;
    int32_t logical_max;
    bool is_relative;
    bool is_constant;
    bool is_array;
} hid_report_field_t;

// HID Device structure
typedef struct {
    usb_device_t* usb_dev;
    usb_endpoint_t* interrupt_in;
    input_device_t* input_dev;

    hid_device_type_t device_type;
    uint8_t interface_num;

    // Report descriptor
    uint8_t* report_descriptor;
    uint16_t report_desc_size;

    // Parsed report fields
    hid_report_field_t* fields;
    uint16_t num_fields;

    // Input report buffer
    uint8_t report_buffer[64];
    uint16_t report_size;

    // Transfer for interrupt endpoint
    usb_transfer_t* transfer;

    // Keyboard state
    uint8_t prev_keys[6];
    uint8_t modifiers;

    // Mouse state
    int32_t mouse_x;
    int32_t mouse_y;
    uint8_t mouse_buttons;
} hid_device_t;

// USB HID scancode to Linux keycode translation (for boot protocol)
static const uint16_t usb_kbd_keycode[256] = {
    [0] = KEY_RESERVED,     [1] = KEY_ESC,          [2] = KEY_1,
    [3] = KEY_2,            [4] = KEY_3,            [5] = KEY_4,
    [6] = KEY_5,            [7] = KEY_6,            [8] = KEY_7,
    [9] = KEY_8,            [10] = KEY_9,           [11] = KEY_0,
    [12] = KEY_MINUS,       [13] = KEY_EQUAL,       [14] = KEY_BACKSPACE,
    [15] = KEY_TAB,         [16] = KEY_Q,           [17] = KEY_W,
    [18] = KEY_E,           [19] = KEY_R,           [20] = KEY_T,
    [21] = KEY_Y,           [22] = KEY_U,           [23] = KEY_I,
    [24] = KEY_O,           [25] = KEY_P,           [26] = KEY_LEFTBRACE,
    [27] = KEY_RIGHTBRACE,  [28] = KEY_ENTER,       [29] = KEY_LEFTCTRL,
    [30] = KEY_A,           [31] = KEY_S,           [32] = KEY_D,
    [33] = KEY_F,           [34] = KEY_G,           [35] = KEY_H,
    [36] = KEY_J,           [37] = KEY_K,           [38] = KEY_L,
    [39] = KEY_SEMICOLON,   [40] = KEY_APOSTROPHE,  [41] = KEY_GRAVE,
    [42] = KEY_LEFTSHIFT,   [43] = KEY_BACKSLASH,   [44] = KEY_Z,
    [45] = KEY_X,           [46] = KEY_C,           [47] = KEY_V,
    [48] = KEY_B,           [49] = KEY_N,           [50] = KEY_M,
    [51] = KEY_COMMA,       [52] = KEY_DOT,         [53] = KEY_SLASH,
    [54] = KEY_RIGHTSHIFT,  [55] = KEY_KPASTERISK,  [56] = KEY_LEFTALT,
    [57] = KEY_SPACE,       [58] = KEY_CAPSLOCK,    [59] = KEY_F1,
    [60] = KEY_F2,          [61] = KEY_F3,          [62] = KEY_F4,
    [63] = KEY_F5,          [64] = KEY_F6,          [65] = KEY_F7,
    [66] = KEY_F8,          [67] = KEY_F9,          [68] = KEY_F10,
    [69] = KEY_NUMLOCK,     [70] = KEY_SCROLLLOCK,
};

// Forward declarations
static void hid_interrupt_callback(usb_transfer_t* transfer);
static void hid_process_keyboard_report(hid_device_t* hid, uint8_t* data, uint32_t len);
static void hid_process_mouse_report(hid_device_t* hid, uint8_t* data, uint32_t len);
static void hid_process_gamepad_report(hid_device_t* hid, uint8_t* data, uint32_t len);
static int hid_parse_report_descriptor(hid_device_t* hid);
static hid_device_type_t hid_detect_device_type(hid_device_t* hid);

// Keyboard LED control callback
static int hid_keyboard_set_led(input_device_t* dev, uint16_t led, bool on) {
    hid_device_t* hid = (hid_device_t*)dev->driver_data;
    uint8_t led_report = 0;

    // Read current LED state (simplified)
    if (led == LED_NUML) led_report |= (on ? 0x01 : 0);
    if (led == LED_CAPSL) led_report |= (on ? 0x02 : 0);
    if (led == LED_SCROLLL) led_report |= (on ? 0x04 : 0);

    // Send SET_REPORT request
    return usb_control_transfer(hid->usb_dev,
        0x21,  // Host to Device, Class, Interface
        USB_HID_REQ_SET_REPORT,
        0x0200,  // Report Type: Output, Report ID: 0
        hid->interface_num,
        &led_report, 1);
}

// HID device probe
static int hid_probe(usb_device_t* device, usb_interface_descriptor_t* interface) {
    kprintf("[HID] Probing device (class=%02x subclass=%02x protocol=%02x)\n",
            interface->interface_class, interface->interface_subclass,
            interface->interface_protocol);

    // Allocate HID device structure
    hid_device_t* hid = (hid_device_t*)kmalloc(sizeof(hid_device_t));
    if (!hid) {
        kprintf("[HID] Failed to allocate device structure\n");
        return -1;
    }
    memset(hid, 0, sizeof(hid_device_t));

    hid->usb_dev = device;
    hid->interface_num = interface->interface_number;

    // Find interrupt IN endpoint
    for (int i = 0; i < device->num_endpoints; i++) {
        usb_endpoint_t* ep = device->endpoints[i];
        if (ep->type == USB_ENDPOINT_INTERRUPT && (ep->address & 0x80)) {
            hid->interrupt_in = ep;
            break;
        }
    }

    if (!hid->interrupt_in) {
        kprintf("[HID] No interrupt IN endpoint found\n");
        kfree(hid);
        return -1;
    }

    // Get HID descriptor (follows interface descriptor)
    // In a real implementation, we'd parse the configuration descriptor properly
    // For now, assume standard layout

    // Get report descriptor
    hid->report_desc_size = 256;  // Maximum size
    hid->report_descriptor = (uint8_t*)kmalloc(hid->report_desc_size);
    if (!hid->report_descriptor) {
        kprintf("[HID] Failed to allocate report descriptor buffer\n");
        kfree(hid);
        return -1;
    }

    int result = usb_control_transfer(device,
        0x81,  // Device to Host, Class, Interface
        USB_REQ_GET_DESCRIPTOR,
        (USB_DESC_REPORT << 8),
        interface->interface_number,
        hid->report_descriptor,
        hid->report_desc_size);

    if (result < 0) {
        kprintf("[HID] Failed to get report descriptor\n");
        // Fall back to boot protocol
        if (interface->interface_subclass == USB_HID_SUBCLASS_BOOT) {
            kprintf("[HID] Using boot protocol\n");
            kfree(hid->report_descriptor);
            hid->report_descriptor = NULL;
            hid->report_desc_size = 0;

            if (interface->interface_protocol == USB_HID_PROTOCOL_KEYBOARD) {
                hid->device_type = HID_DEVICE_KEYBOARD;
                hid->report_size = 8;
            } else if (interface->interface_protocol == USB_HID_PROTOCOL_MOUSE) {
                hid->device_type = HID_DEVICE_MOUSE;
                hid->report_size = 4;
            }
        } else {
            kfree(hid->report_descriptor);
            kfree(hid);
            return -1;
        }
    } else {
        hid->report_desc_size = result;

        // Parse report descriptor
        if (hid_parse_report_descriptor(hid) < 0) {
            kprintf("[HID] Failed to parse report descriptor\n");
            kfree(hid->report_descriptor);
            kfree(hid);
            return -1;
        }

        // Detect device type
        hid->device_type = hid_detect_device_type(hid);
    }

    // Set protocol to report protocol (if not boot)
    if (interface->interface_subclass == USB_HID_SUBCLASS_BOOT) {
        usb_control_transfer(device,
            0x21,  // Host to Device, Class, Interface
            USB_HID_REQ_SET_PROTOCOL,
            1,  // Report protocol
            interface->interface_number,
            NULL, 0);
    }

    // Set idle rate (0 = infinite, only send on change)
    usb_control_transfer(device,
        0x21,  // Host to Device, Class, Interface
        USB_HID_REQ_SET_IDLE,
        0,  // Duration: 0 = infinite
        interface->interface_number,
        NULL, 0);

    // Create input device
    const char* device_name = "Unknown HID";
    switch (hid->device_type) {
        case HID_DEVICE_KEYBOARD:
            device_name = "USB Keyboard";
            break;
        case HID_DEVICE_MOUSE:
            device_name = "USB Mouse";
            break;
        case HID_DEVICE_GAMEPAD:
            device_name = "USB Gamepad";
            break;
        default:
            break;
    }

    hid->input_dev = input_allocate_device(device_name);
    if (!hid->input_dev) {
        kprintf("[HID] Failed to allocate input device\n");
        if (hid->report_descriptor) kfree(hid->report_descriptor);
        if (hid->fields) kfree(hid->fields);
        kfree(hid);
        return -1;
    }

    // Set capabilities
    hid->input_dev->driver_data = hid;
    switch (hid->device_type) {
        case HID_DEVICE_KEYBOARD:
            hid->input_dev->supports_key = true;
            hid->input_dev->supports_led = true;
            hid->input_dev->set_led = hid_keyboard_set_led;
            break;
        case HID_DEVICE_MOUSE:
            hid->input_dev->supports_key = true;  // Mouse buttons
            hid->input_dev->supports_rel = true;
            break;
        case HID_DEVICE_GAMEPAD:
            hid->input_dev->supports_key = true;  // Buttons
            hid->input_dev->supports_abs = true;  // Axes
            break;
        default:
            break;
    }

    input_register_device(hid->input_dev);

    kprintf("[HID] Registered %s\n", device_name);

    // Start interrupt transfer
    hid->transfer = (usb_transfer_t*)kmalloc(sizeof(usb_transfer_t));
    if (!hid->transfer) {
        kprintf("[HID] Failed to allocate transfer\n");
        input_unregister_device(hid->input_dev);
        input_free_device(hid->input_dev);
        if (hid->report_descriptor) kfree(hid->report_descriptor);
        if (hid->fields) kfree(hid->fields);
        kfree(hid);
        return -1;
    }

    hid->transfer->device = device;
    hid->transfer->endpoint = hid->interrupt_in;
    hid->transfer->buffer = hid->report_buffer;
    hid->transfer->length = sizeof(hid->report_buffer);
    hid->transfer->callback = hid_interrupt_callback;
    hid->transfer->user_data = hid;

    result = usb_interrupt_transfer(device, hid->interrupt_in,
                                   hid->report_buffer, sizeof(hid->report_buffer),
                                   hid_interrupt_callback);

    if (result < 0) {
        kprintf("[HID] Failed to start interrupt transfer\n");
        kfree(hid->transfer);
        input_unregister_device(hid->input_dev);
        input_free_device(hid->input_dev);
        if (hid->report_descriptor) kfree(hid->report_descriptor);
        if (hid->fields) kfree(hid->fields);
        kfree(hid);
        return -1;
    }

    device->driver_data = hid;
    return 0;
}

// HID device disconnect
static void hid_disconnect(usb_device_t* device) {
    hid_device_t* hid = (hid_device_t*)device->driver_data;
    if (!hid) return;

    kprintf("[HID] Disconnecting device\n");

    // Cancel interrupt transfer (would need USB core support)

    // Unregister input device
    input_unregister_device(hid->input_dev);
    input_free_device(hid->input_dev);

    // Free resources
    if (hid->transfer) kfree(hid->transfer);
    if (hid->report_descriptor) kfree(hid->report_descriptor);
    if (hid->fields) kfree(hid->fields);
    kfree(hid);

    device->driver_data = NULL;
}

// Interrupt transfer callback
static void hid_interrupt_callback(usb_transfer_t* transfer) {
    hid_device_t* hid = (hid_device_t*)transfer->user_data;

    if (transfer->status != 0) {
        kprintf("[HID] Transfer error: %d\n", transfer->status);
        return;
    }

    if (transfer->actual_length == 0) {
        return;
    }

    // Process report based on device type
    switch (hid->device_type) {
        case HID_DEVICE_KEYBOARD:
            hid_process_keyboard_report(hid, (uint8_t*)transfer->buffer,
                                       transfer->actual_length);
            break;
        case HID_DEVICE_MOUSE:
            hid_process_mouse_report(hid, (uint8_t*)transfer->buffer,
                                    transfer->actual_length);
            break;
        case HID_DEVICE_GAMEPAD:
            hid_process_gamepad_report(hid, (uint8_t*)transfer->buffer,
                                      transfer->actual_length);
            break;
        default:
            break;
    }

    // Resubmit transfer (in a real implementation, USB core handles this)
}

// Process keyboard report (boot protocol format)
static void hid_process_keyboard_report(hid_device_t* hid, uint8_t* data, uint32_t len) {
    if (len < 8) return;

    uint8_t modifiers = data[0];
    uint8_t* keys = &data[2];  // Skip reserved byte

    // Process modifier changes
    uint8_t mod_changes = modifiers ^ hid->modifiers;
    if (mod_changes & 0x01) input_report_key(hid->input_dev, KEY_LEFTCTRL, modifiers & 0x01);
    if (mod_changes & 0x02) input_report_key(hid->input_dev, KEY_LEFTSHIFT, (modifiers & 0x02) >> 1);
    if (mod_changes & 0x04) input_report_key(hid->input_dev, KEY_LEFTALT, (modifiers & 0x04) >> 2);
    if (mod_changes & 0x20) input_report_key(hid->input_dev, KEY_RIGHTSHIFT, (modifiers & 0x20) >> 5);
    hid->modifiers = modifiers;

    // Find released keys
    for (int i = 0; i < 6; i++) {
        uint8_t prev_key = hid->prev_keys[i];
        if (prev_key == 0) continue;

        bool still_pressed = false;
        for (int j = 0; j < 6; j++) {
            if (keys[j] == prev_key) {
                still_pressed = true;
                break;
            }
        }

        if (!still_pressed && prev_key < 256) {
            uint16_t keycode = usb_kbd_keycode[prev_key];
            if (keycode != KEY_RESERVED) {
                input_report_key(hid->input_dev, keycode, KEY_STATE_RELEASED);
            }
        }
    }

    // Find newly pressed keys
    for (int i = 0; i < 6; i++) {
        uint8_t key = keys[i];
        if (key == 0) continue;

        bool was_pressed = false;
        for (int j = 0; j < 6; j++) {
            if (hid->prev_keys[j] == key) {
                was_pressed = true;
                break;
            }
        }

        if (!was_pressed && key < 256) {
            uint16_t keycode = usb_kbd_keycode[key];
            if (keycode != KEY_RESERVED) {
                input_report_key(hid->input_dev, keycode, KEY_STATE_PRESSED);
            }
        }
    }

    // Save current key state
    memcpy(hid->prev_keys, keys, 6);

    input_sync(hid->input_dev);
}

// Process mouse report (boot protocol format)
static void hid_process_mouse_report(hid_device_t* hid, uint8_t* data, uint32_t len) {
    if (len < 3) return;

    uint8_t buttons = data[0];
    int8_t dx = (int8_t)data[1];
    int8_t dy = (int8_t)data[2];
    int8_t wheel = (len >= 4) ? (int8_t)data[3] : 0;

    // Process button changes
    uint8_t button_changes = buttons ^ hid->mouse_buttons;
    if (button_changes & 0x01)
        input_report_key(hid->input_dev, BTN_LEFT, (buttons & 0x01) ? 1 : 0);
    if (button_changes & 0x02)
        input_report_key(hid->input_dev, BTN_RIGHT, (buttons & 0x02) ? 1 : 0);
    if (button_changes & 0x04)
        input_report_key(hid->input_dev, BTN_MIDDLE, (buttons & 0x04) ? 1 : 0);
    hid->mouse_buttons = buttons;

    // Report relative movement
    if (dx != 0) input_report_rel(hid->input_dev, REL_X, dx);
    if (dy != 0) input_report_rel(hid->input_dev, REL_Y, dy);
    if (wheel != 0) input_report_rel(hid->input_dev, REL_WHEEL, wheel);

    input_sync(hid->input_dev);
}

// Process gamepad report (simplified - would need proper parsing)
static void hid_process_gamepad_report(hid_device_t* hid, uint8_t* data, uint32_t len) {
    // This is a simplified implementation
    // Real implementation would parse based on report descriptor
    if (len < 8) return;

    // Assume standard gamepad layout:
    // Byte 0-1: Left stick X/Y
    // Byte 2-3: Right stick X/Y
    // Byte 4-5: Buttons
    // Byte 6-7: Triggers

    int16_t lx = (int16_t)((data[1] << 8) | data[0]) - 32768;
    int16_t ly = (int16_t)((data[3] << 8) | data[2]) - 32768;

    input_report_abs(hid->input_dev, ABS_X, lx);
    input_report_abs(hid->input_dev, ABS_Y, ly);

    if (len >= 8) {
        int16_t rx = (int16_t)((data[5] << 8) | data[4]) - 32768;
        int16_t ry = (int16_t)((data[7] << 8) | data[6]) - 32768;
        input_report_abs(hid->input_dev, ABS_RX, rx);
        input_report_abs(hid->input_dev, ABS_RY, ry);
    }

    input_sync(hid->input_dev);
}

// Maximum sane report descriptor size (HID spec allows up to 4 KB).
#define HID_REPORT_DESC_MAX 4096

// Parse report descriptor (simplified parser)
static int hid_parse_report_descriptor(hid_device_t* hid) {
    // This is a very simplified parser
    // A full implementation would build a complete report structure

    if (!hid || !hid->report_descriptor) return -1;

    uint8_t* desc = hid->report_descriptor;
    uint16_t size = hid->report_desc_size;

    // Reject obviously oversized or empty descriptors.
    if (size == 0 || size > HID_REPORT_DESC_MAX) {
        kprintf("[HID] Report descriptor invalid (size=%u)\n", size);
        return -1;
    }

    uint16_t usage_page = 0;
    uint16_t usage = 0;

    for (uint16_t i = 0; i < size;) {
        uint8_t item = desc[i++];

        // Long items (bSize == 3 in the low 2 bits AND tag == 0xF) are rare
        // in practice; reject them cleanly rather than mis-parsing.
        if (item == 0xFE) {
            // Long item: next byte is data size, byte after is tag.
            if (i + 1 >= size) break;
            uint8_t long_size = desc[i++];
            i++;  // skip long item tag
            if (long_size > size - i) break;  // would overrun
            i += long_size;
            continue;
        }

        uint8_t item_type = (item >> 2) & 0x03;
        uint8_t item_tag = (item >> 4) & 0x0F;
        uint8_t item_size = item & 0x03;

        if (item_size == 3) item_size = 4;

        // Bounds check: ensure item_size bytes remain in the descriptor.
        if (item_size > size - i) {
            kprintf("[HID] Report descriptor truncated at offset %u\n", i);
            break;
        }

        uint32_t data = 0;
        for (uint8_t j = 0; j < item_size; j++) {
            data |= (uint32_t)desc[i++] << (j * 8);
        }

        if (item_type == HID_ITEM_TYPE_GLOBAL) {
            if (item_tag == HID_GLOBAL_USAGE_PAGE) {
                usage_page = data;
            }
        } else if (item_type == HID_ITEM_TYPE_LOCAL) {
            if (item_tag == HID_LOCAL_USAGE) {
                usage = data;
            }
        }
    }

    (void)usage_page;
    (void)usage;

    return 0;
}

// Detect device type from report descriptor
static hid_device_type_t hid_detect_device_type(hid_device_t* hid) {
    uint8_t* desc = hid->report_descriptor;
    uint16_t size = hid->report_desc_size;

    uint16_t usage_page = 0;
    uint16_t usage = 0;

    // Scan for primary usage
    for (uint16_t i = 0; i < size && i < 32;) {  // Check first 32 bytes
        uint8_t item = desc[i++];
        uint8_t item_type = (item >> 2) & 0x03;
        uint8_t item_tag = (item >> 4) & 0x0F;
        uint8_t item_size = item & 0x03;

        if (item_size == 3) item_size = 4;

        uint32_t data = 0;
        for (uint8_t j = 0; j < item_size; j++) {
            if (i >= size) break;
            data |= (uint32_t)desc[i++] << (j * 8);
        }

        if (item_type == HID_ITEM_TYPE_GLOBAL && item_tag == HID_GLOBAL_USAGE_PAGE) {
            usage_page = data;
        } else if (item_type == HID_ITEM_TYPE_LOCAL && item_tag == HID_LOCAL_USAGE) {
            usage = data;

            // Check for known device types
            if (usage_page == HID_USAGE_PAGE_GENERIC) {
                if (usage == HID_USAGE_KEYBOARD) {
                    return HID_DEVICE_KEYBOARD;
                } else if (usage == HID_USAGE_MOUSE || usage == HID_USAGE_POINTER) {
                    return HID_DEVICE_MOUSE;
                } else if (usage == HID_USAGE_GAMEPAD || usage == HID_USAGE_JOYSTICK) {
                    return HID_DEVICE_GAMEPAD;
                }
            }
        }
    }

    return HID_DEVICE_UNKNOWN;
}

// USB HID Driver structure
static usb_driver_t hid_driver = {
    .name = "usb-hid",
    .probe = hid_probe,
    .disconnect = hid_disconnect,
    .class_code = USB_CLASS_HID,
    .subclass = 0xFF,  // Match any subclass
    .protocol = 0xFF   // Match any protocol
};

// Initialize USB HID driver
void usb_hid_init(void) {
    kprintf("[HID] Initializing USB HID driver\n");
    usb_register_driver(&hid_driver);
}
