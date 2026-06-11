/*
 * USB Core - Simplified USB subsystem
 * This is a minimal implementation for HID device support
 * A full USB stack would require XHCI/EHCI/UHCI controller drivers
 */

#include "../../include/usb.h"
#include "../../include/kernel.h"
#include "../../include/types.h"
#include "../../include/mem.h"
#include <string.h>

#define MAX_USB_DRIVERS 16
#define MAX_USB_DEVICES 128

// Global USB driver registry
static usb_driver_t* usb_drivers[MAX_USB_DRIVERS];
static uint32_t num_usb_drivers = 0;

// Global USB device list
static usb_device_t* usb_devices[MAX_USB_DEVICES];
static uint32_t num_usb_devices = 0;

// Forward declarations
extern void usb_hid_init(void);

// Initialize USB subsystem
void usb_init(void) {
    kprintf("[USB] Initializing USB subsystem\n");

    memset(usb_drivers, 0, sizeof(usb_drivers));
    memset(usb_devices, 0, sizeof(usb_devices));
    num_usb_drivers = 0;
    num_usb_devices = 0;

    // Initialize USB HID driver
    usb_hid_init();

    kprintf("[USB] USB subsystem initialized\n");
}

// Register USB driver
int usb_register_driver(usb_driver_t* driver) {
    if (!driver) return -1;

    if (num_usb_drivers >= MAX_USB_DRIVERS) {
        kprintf("[USB] Maximum number of drivers reached\n");
        return -1;
    }

    usb_drivers[num_usb_drivers++] = driver;

    kprintf("[USB] Registered driver: %s (class=%02x subclass=%02x protocol=%02x)\n",
            driver->name, driver->class_code, driver->subclass, driver->protocol);

    return 0;
}

// Add USB device (called by controller driver)
int usb_add_device(usb_device_t* device) {
    if (!device) return -1;

    if (num_usb_devices >= MAX_USB_DEVICES) {
        kprintf("[USB] Maximum number of devices reached\n");
        return -1;
    }

    usb_devices[num_usb_devices++] = device;

    kprintf("[USB] Device added: addr=%u port=%u speed=%u\n",
            device->address, device->port, device->speed);
    kprintf("[USB]   VID:PID=%04x:%04x Class=%02x Subclass=%02x Protocol=%02x\n",
            device->device_desc.vendor_id, device->device_desc.product_id,
            device->device_desc.device_class, device->device_desc.device_subclass,
            device->device_desc.device_protocol);

    return 0;
}

// Remove USB device
void usb_remove_device(usb_device_t* device) {
    if (!device) return;

    // Call disconnect on driver
    for (uint32_t i = 0; i < num_usb_drivers; i++) {
        usb_driver_t* driver = usb_drivers[i];
        if (driver && driver->disconnect) {
            driver->disconnect(device);
        }
    }

    // Remove from device list
    for (uint32_t i = 0; i < num_usb_devices; i++) {
        if (usb_devices[i] == device) {
            for (uint32_t j = i; j < num_usb_devices - 1; j++) {
                usb_devices[j] = usb_devices[j + 1];
            }
            num_usb_devices--;
            usb_devices[num_usb_devices] = NULL;
            break;
        }
    }

    kprintf("[USB] Device removed: addr=%u\n", device->address);
}

// Probe device with drivers
int usb_probe_device(usb_device_t* device, usb_interface_descriptor_t* interface) {
    if (!device || !interface) return -1;

    kprintf("[USB] Probing interface: class=%02x subclass=%02x protocol=%02x\n",
            interface->interface_class, interface->interface_subclass,
            interface->interface_protocol);

    // Try each registered driver
    for (uint32_t i = 0; i < num_usb_drivers; i++) {
        usb_driver_t* driver = usb_drivers[i];
        if (!driver || !driver->probe) continue;

        // Match by class
        bool class_match = (driver->class_code == 0xFF ||
                           driver->class_code == interface->interface_class);
        bool subclass_match = (driver->subclass == 0xFF ||
                              driver->subclass == interface->interface_subclass);
        bool protocol_match = (driver->protocol == 0xFF ||
                              driver->protocol == interface->interface_protocol);

        if (class_match && subclass_match && protocol_match) {
            kprintf("[USB] Trying driver: %s\n", driver->name);

            int result = driver->probe(device, interface);
            if (result == 0) {
                kprintf("[USB] Driver %s claimed device\n", driver->name);
                return 0;
            }
        }
    }

    kprintf("[USB] No driver found for interface\n");
    return -1;
}

// USB control transfer (simplified)
int usb_control_transfer(usb_device_t* device, uint8_t request_type, uint8_t request,
                        uint16_t value, uint16_t index, void* data, uint16_t length) {
    if (!device) return -1;

    // In a real implementation, this would communicate with the USB controller
    // to perform the control transfer

    // For now, this is a stub that simulates success for certain requests
    kprintf("[USB] Control transfer: rt=%02x req=%02x val=%04x idx=%04x len=%u\n",
            request_type, request, value, index, length);

    // Simulate success for SET_IDLE, SET_PROTOCOL, SET_REPORT
    if (request == USB_HID_REQ_SET_IDLE ||
        request == USB_HID_REQ_SET_PROTOCOL ||
        request == USB_HID_REQ_SET_REPORT) {
        return 0;
    }

    // Simulate failure for GET_DESCRIPTOR (report descriptor)
    // In real implementation, this would return the actual descriptor
    if (request == USB_REQ_GET_DESCRIPTOR && (value >> 8) == USB_DESC_REPORT) {
        return -1;  // Force fallback to boot protocol
    }

    return length;
}

// USB interrupt transfer (simplified)
int usb_interrupt_transfer(usb_device_t* device, usb_endpoint_t* endpoint,
                          void* buffer, uint32_t length, usb_transfer_callback_t callback) {
    if (!device || !endpoint || !buffer) return -1;

    // In a real implementation, this would set up an interrupt transfer
    // with the USB controller

    kprintf("[USB] Interrupt transfer setup: ep=%02x len=%u\n",
            endpoint->address, length);

    // For now, we simulate success
    // The actual data would come from USB controller interrupts
    return 0;
}

// Allocate USB device
usb_device_t* usb_allocate_device(void) {
    usb_device_t* dev = (usb_device_t*)kmalloc(sizeof(usb_device_t));
    if (!dev) return NULL;

    memset(dev, 0, sizeof(usb_device_t));
    return dev;
}

// Maximum endpoints per device -- must match the endpoints[] array size in usb.h.
#define USB_MAX_ENDPOINTS 32

// Free USB device
void usb_free_device(usb_device_t* device) {
    if (!device) return;

    // Cap at the array size to prevent an out-of-bounds walk if
    // num_endpoints was corrupted by a malicious/broken descriptor.
    uint8_t ep_count = device->num_endpoints;
    if (ep_count > USB_MAX_ENDPOINTS)
        ep_count = USB_MAX_ENDPOINTS;

    // Free endpoints
    for (uint8_t i = 0; i < ep_count; i++) {
        if (device->endpoints[i]) {
            kfree(device->endpoints[i]);
            device->endpoints[i] = NULL;
        }
    }

    kfree(device);
}

// Create endpoint
usb_endpoint_t* usb_create_endpoint(uint8_t address, usb_endpoint_type_t type,
                                    uint16_t max_packet_size, uint8_t interval) {
    usb_endpoint_t* ep = (usb_endpoint_t*)kmalloc(sizeof(usb_endpoint_t));
    if (!ep) return NULL;

    ep->address = address;
    ep->type = type;
    ep->max_packet_size = max_packet_size;
    ep->interval = interval;
    ep->driver_data = NULL;

    return ep;
}

// Get string descriptor (simplified)
int usb_get_string_descriptor(usb_device_t* device, uint8_t index, char* buffer, size_t length) {
    if (!device || !buffer) return -1;

    // In a real implementation, this would fetch the string descriptor
    // For now, return a placeholder
    const char* str = "USB Device";
    size_t str_len = 0;
    while (str[str_len] && str_len < length - 1) {
        buffer[str_len] = str[str_len];
        str_len++;
    }
    buffer[str_len] = '\0';

    return str_len;
}

// List USB devices (for debugging)
void usb_list_devices(void) {
    kprintf("[USB] Registered devices (%u):\n", num_usb_devices);
    for (uint32_t i = 0; i < num_usb_devices; i++) {
        usb_device_t* dev = usb_devices[i];
        kprintf("[USB]   Device %u: addr=%u VID:PID=%04x:%04x\n",
                i, dev->address,
                dev->device_desc.vendor_id, dev->device_desc.product_id);
    }
}

// List USB drivers (for debugging)
void usb_list_drivers(void) {
    kprintf("[USB] Registered drivers (%u):\n", num_usb_drivers);
    for (uint32_t i = 0; i < num_usb_drivers; i++) {
        usb_driver_t* driver = usb_drivers[i];
        kprintf("[USB]   Driver: %s (class=%02x)\n",
                driver->name, driver->class_code);
    }
}
