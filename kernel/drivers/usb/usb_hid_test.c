/*
 * USB HID Test Driver
 * Simulates USB HID devices for testing without real hardware
 */

#include "../../include/usb.h"
#include "../../include/input.h"
#include "../../include/kernel.h"
#include "../../include/types.h"
#include "../../include/mem.h"
#include <string.h>

// Forward declarations
extern int usb_add_device(usb_device_t* device);
extern int usb_probe_device(usb_device_t* device, usb_interface_descriptor_t* interface);
extern usb_device_t* usb_allocate_device(void);
extern usb_endpoint_t* usb_create_endpoint(uint8_t address, usb_endpoint_type_t type,
                                           uint16_t max_packet_size, uint8_t interval);

// Simulate a USB keyboard being connected
static int usb_hid_test_add_keyboard(void) {
    kprintf("[USB_TEST] Simulating USB keyboard connection\n");

    // Allocate device
    usb_device_t* device = usb_allocate_device();
    if (!device) {
        kprintf("[USB_TEST] Failed to allocate device\n");
        return -1;
    }

    // Fill device descriptor
    device->address = 1;
    device->port = 1;
    device->speed = USB_SPEED_FULL;

    device->device_desc.length = 18;
    device->device_desc.descriptor_type = USB_DESC_DEVICE;
    device->device_desc.usb_version = 0x0110;  // USB 1.1
    device->device_desc.device_class = 0x00;   // Interface-defined
    device->device_desc.device_subclass = 0x00;
    device->device_desc.device_protocol = 0x00;
    device->device_desc.max_packet_size = 8;
    device->device_desc.vendor_id = 0x046D;    // Logitech
    device->device_desc.product_id = 0xC31C;   // Keyboard
    device->device_desc.device_version = 0x0110;
    device->device_desc.manufacturer_string = 1;
    device->device_desc.product_string = 2;
    device->device_desc.serial_number_string = 0;
    device->device_desc.num_configurations = 1;

    // Create control endpoint (EP0)
    usb_endpoint_t* ep0 = usb_create_endpoint(0x00, USB_ENDPOINT_CONTROL, 8, 0);
    if (!ep0) {
        kfree(device);
        return -1;
    }
    device->endpoints[0] = ep0;
    device->num_endpoints = 1;

    // Create interrupt IN endpoint (EP1)
    usb_endpoint_t* ep1_in = usb_create_endpoint(0x81, USB_ENDPOINT_INTERRUPT, 8, 10);
    if (!ep1_in) {
        kfree(ep0);
        kfree(device);
        return -1;
    }
    device->endpoints[1] = ep1_in;
    device->num_endpoints = 2;

    // Add device to USB core
    if (usb_add_device(device) < 0) {
        kfree(ep1_in);
        kfree(ep0);
        kfree(device);
        return -1;
    }

    // Create interface descriptor
    usb_interface_descriptor_t interface;
    interface.length = 9;
    interface.descriptor_type = USB_DESC_INTERFACE;
    interface.interface_number = 0;
    interface.alternate_setting = 0;
    interface.num_endpoints = 1;
    interface.interface_class = USB_CLASS_HID;
    interface.interface_subclass = USB_HID_SUBCLASS_BOOT;
    interface.interface_protocol = USB_HID_PROTOCOL_KEYBOARD;
    interface.interface_string = 0;

    // Probe device with drivers
    if (usb_probe_device(device, &interface) < 0) {
        kprintf("[USB_TEST] Failed to probe keyboard\n");
        return -1;
    }

    kprintf("[USB_TEST] Keyboard successfully added\n");
    return 0;
}

// Simulate a USB mouse being connected
static int usb_hid_test_add_mouse(void) {
    kprintf("[USB_TEST] Simulating USB mouse connection\n");

    // Allocate device
    usb_device_t* device = usb_allocate_device();
    if (!device) {
        kprintf("[USB_TEST] Failed to allocate device\n");
        return -1;
    }

    // Fill device descriptor
    device->address = 2;
    device->port = 2;
    device->speed = USB_SPEED_FULL;

    device->device_desc.length = 18;
    device->device_desc.descriptor_type = USB_DESC_DEVICE;
    device->device_desc.usb_version = 0x0110;  // USB 1.1
    device->device_desc.device_class = 0x00;   // Interface-defined
    device->device_desc.device_subclass = 0x00;
    device->device_desc.device_protocol = 0x00;
    device->device_desc.max_packet_size = 8;
    device->device_desc.vendor_id = 0x046D;    // Logitech
    device->device_desc.product_id = 0xC077;   // Mouse
    device->device_desc.device_version = 0x0110;
    device->device_desc.manufacturer_string = 1;
    device->device_desc.product_string = 2;
    device->device_desc.serial_number_string = 0;
    device->device_desc.num_configurations = 1;

    // Create control endpoint (EP0)
    usb_endpoint_t* ep0 = usb_create_endpoint(0x00, USB_ENDPOINT_CONTROL, 8, 0);
    if (!ep0) {
        kfree(device);
        return -1;
    }
    device->endpoints[0] = ep0;
    device->num_endpoints = 1;

    // Create interrupt IN endpoint (EP1)
    usb_endpoint_t* ep1_in = usb_create_endpoint(0x81, USB_ENDPOINT_INTERRUPT, 4, 10);
    if (!ep1_in) {
        kfree(ep0);
        kfree(device);
        return -1;
    }
    device->endpoints[1] = ep1_in;
    device->num_endpoints = 2;

    // Add device to USB core
    if (usb_add_device(device) < 0) {
        kfree(ep1_in);
        kfree(ep0);
        kfree(device);
        return -1;
    }

    // Create interface descriptor
    usb_interface_descriptor_t interface;
    interface.length = 9;
    interface.descriptor_type = USB_DESC_INTERFACE;
    interface.interface_number = 0;
    interface.alternate_setting = 0;
    interface.num_endpoints = 1;
    interface.interface_class = USB_CLASS_HID;
    interface.interface_subclass = USB_HID_SUBCLASS_BOOT;
    interface.interface_protocol = USB_HID_PROTOCOL_MOUSE;
    interface.interface_string = 0;

    // Probe device with drivers
    if (usb_probe_device(device, &interface) < 0) {
        kprintf("[USB_TEST] Failed to probe mouse\n");
        return -1;
    }

    kprintf("[USB_TEST] Mouse successfully added\n");
    return 0;
}

// Simulate keyboard input
void usb_hid_test_keyboard_input(void) {
    // This would be called by the test framework to inject keyboard events
    // In a real scenario, these would come from USB interrupt transfers

    kprintf("[USB_TEST] Simulating keyboard input: 'Hello'\n");

    // Simulate typing "Hello" (would need actual HID device handle)
    // This is just for demonstration
}

// Simulate mouse movement
void usb_hid_test_mouse_movement(void) {
    // This would be called by the test framework to inject mouse events

    kprintf("[USB_TEST] Simulating mouse movement\n");

    // Simulate mouse movement (would need actual HID device handle)
}

// Initialize USB HID test devices
void usb_hid_test_init(void) {
    kprintf("[USB_TEST] Initializing USB HID test devices\n");

    // Add test keyboard
    if (usb_hid_test_add_keyboard() < 0) {
        kprintf("[USB_TEST] Failed to add test keyboard\n");
    }

    // Add test mouse
    if (usb_hid_test_add_mouse() < 0) {
        kprintf("[USB_TEST] Failed to add test mouse\n");
    }

    kprintf("[USB_TEST] USB HID test initialization complete\n");
}
