#ifndef USB_H
#define USB_H

#include "types.h"
#include "pci.h"

/* PACKED is normally provided by kernel.h, but usb.h is sometimes included
 * before kernel.h (as in the existing usb_core.c / hid.c). Define it here so
 * usb.h is self-contained and the packed descriptor structs below compile
 * regardless of include order. */
#ifndef PACKED
#define PACKED __attribute__((packed))
#endif

// USB Speeds
typedef enum {
    USB_SPEED_LOW = 0,      // 1.5 Mbps (USB 1.0)
    USB_SPEED_FULL = 1,     // 12 Mbps (USB 1.1)
    USB_SPEED_HIGH = 2,     // 480 Mbps (USB 2.0)
    USB_SPEED_SUPER = 3,    // 5 Gbps (USB 3.0)
    USB_SPEED_SUPER_PLUS = 4 // 10 Gbps (USB 3.1)
} usb_speed_t;

// USB Endpoint Types
typedef enum {
    USB_ENDPOINT_CONTROL = 0,
    USB_ENDPOINT_ISOCHRONOUS = 1,
    USB_ENDPOINT_BULK = 2,
    USB_ENDPOINT_INTERRUPT = 3
} usb_endpoint_type_t;

// USB Device Descriptor
typedef struct PACKED {
    uint8_t length;
    uint8_t descriptor_type;
    uint16_t usb_version;
    uint8_t device_class;
    uint8_t device_subclass;
    uint8_t device_protocol;
    uint8_t max_packet_size;
    uint16_t vendor_id;
    uint16_t product_id;
    uint16_t device_version;
    uint8_t manufacturer_string;
    uint8_t product_string;
    uint8_t serial_number_string;
    uint8_t num_configurations;
} usb_device_descriptor_t;

// USB Configuration Descriptor
typedef struct PACKED {
    uint8_t length;
    uint8_t descriptor_type;
    uint16_t total_length;
    uint8_t num_interfaces;
    uint8_t configuration_value;
    uint8_t configuration_string;
    uint8_t attributes;
    uint8_t max_power;
} usb_config_descriptor_t;

// USB Interface Descriptor
typedef struct PACKED {
    uint8_t length;
    uint8_t descriptor_type;
    uint8_t interface_number;
    uint8_t alternate_setting;
    uint8_t num_endpoints;
    uint8_t interface_class;
    uint8_t interface_subclass;
    uint8_t interface_protocol;
    uint8_t interface_string;
} usb_interface_descriptor_t;

// USB Endpoint Descriptor
typedef struct PACKED {
    uint8_t length;
    uint8_t descriptor_type;
    uint8_t endpoint_address;
    uint8_t attributes;
    uint16_t max_packet_size;
    uint8_t interval;
} usb_endpoint_descriptor_t;

// USB HID Descriptor
typedef struct PACKED {
    uint8_t length;
    uint8_t descriptor_type;
    uint16_t hid_version;
    uint8_t country_code;
    uint8_t num_descriptors;
    uint8_t report_descriptor_type;
    uint16_t report_descriptor_length;
} usb_hid_descriptor_t;

// USB Standard Requests
#define USB_REQ_GET_STATUS        0x00
#define USB_REQ_CLEAR_FEATURE     0x01
#define USB_REQ_SET_FEATURE       0x03
#define USB_REQ_SET_ADDRESS       0x05
#define USB_REQ_GET_DESCRIPTOR    0x06
#define USB_REQ_SET_DESCRIPTOR    0x07
#define USB_REQ_GET_CONFIGURATION 0x08
#define USB_REQ_SET_CONFIGURATION 0x09
#define USB_REQ_GET_INTERFACE     0x0A
#define USB_REQ_SET_INTERFACE     0x0B

// USB Descriptor Types
#define USB_DESC_DEVICE           0x01
#define USB_DESC_CONFIGURATION    0x02
#define USB_DESC_STRING           0x03
#define USB_DESC_INTERFACE        0x04
#define USB_DESC_ENDPOINT         0x05
#define USB_DESC_HID              0x21
#define USB_DESC_REPORT           0x22

// USB Class Codes
#define USB_CLASS_HID             0x03
#define USB_CLASS_MASS_STORAGE    0x08
#define USB_CLASS_HUB             0x09

// HID Subclass Codes
#define USB_HID_SUBCLASS_NONE     0x00
#define USB_HID_SUBCLASS_BOOT     0x01

// HID Protocol Codes
#define USB_HID_PROTOCOL_NONE     0x00
#define USB_HID_PROTOCOL_KEYBOARD 0x01
#define USB_HID_PROTOCOL_MOUSE    0x02

// HID Class Requests
#define USB_HID_REQ_GET_REPORT    0x01
#define USB_HID_REQ_GET_IDLE      0x02
#define USB_HID_REQ_GET_PROTOCOL  0x03
#define USB_HID_REQ_SET_REPORT    0x09
#define USB_HID_REQ_SET_IDLE      0x0A
#define USB_HID_REQ_SET_PROTOCOL  0x0B

// Forward declarations
struct usb_device;
struct usb_endpoint;
struct usb_transfer;

// USB Transfer callback
typedef void (*usb_transfer_callback_t)(struct usb_transfer* transfer);

// USB Transfer structure
typedef struct usb_transfer {
    struct usb_device* device;
    struct usb_endpoint* endpoint;
    void* buffer;
    uint32_t length;
    uint32_t actual_length;
    uint32_t status;
    usb_transfer_callback_t callback;
    void* user_data;
} usb_transfer_t;

// USB Endpoint structure
typedef struct usb_endpoint {
    uint8_t address;
    usb_endpoint_type_t type;
    uint16_t max_packet_size;
    uint8_t interval;
    void* driver_data;
} usb_endpoint_t;

// USB Device structure
typedef struct usb_device {
    uint8_t address;
    uint8_t port;
    usb_speed_t speed;
    usb_device_descriptor_t device_desc;
    usb_endpoint_t* endpoints[32];
    uint8_t num_endpoints;
    void* controller;
    void* driver_data;
} usb_device_t;

// USB Driver structure
typedef struct usb_driver {
    const char* name;
    int (*probe)(usb_device_t* device, usb_interface_descriptor_t* interface);
    void (*disconnect)(usb_device_t* device);
    uint8_t class_code;
    uint8_t subclass;
    uint8_t protocol;
} usb_driver_t;

// USB Core Functions
void usb_init(void);
int usb_register_driver(usb_driver_t* driver);
int usb_control_transfer(usb_device_t* device, uint8_t request_type, uint8_t request,
                         uint16_t value, uint16_t index, void* data, uint16_t length);
int usb_interrupt_transfer(usb_device_t* device, usb_endpoint_t* endpoint,
                           void* buffer, uint32_t length, usb_transfer_callback_t callback);

/* ------------------------------------------------------------------ *
 * UHCI host-controller driver (kernel/drivers/usb/uhci.c)
 * ------------------------------------------------------------------ *
 *
 * This is the REAL host controller bring-up: it detects a UHCI USB 1.1
 * controller on the PCI bus (class 0x0C, subclass 0x03, prog_if 0x00 --
 * the PIIX3/PIIX4 companion present on the QEMU default PC), resets it,
 * builds the frame list + a QH/TD schedule, resets/enables the root-hub
 * ports, enumerates attached devices via real control transfers
 * (GET_DESCRIPTOR / SET_ADDRESS / SET_CONFIGURATION), finds a HID boot
 * interface (keyboard/mouse) and polls its interrupt IN endpoint,
 * delivering reports to the input subsystem (input_report_key/rel).
 *
 * USB controller PCI prog_if values (subclass 0x03):
 *   0x00 = UHCI (USB 1.1, I/O-port registers)   <-- implemented here
 *   0x10 = OHCI (USB 1.1, MMIO)
 *   0x20 = EHCI (USB 2.0, MMIO)
 *   0x30 = xHCI (USB 3.x, MMIO)
 */
#define PCI_CLASS_SERIAL_BUS      0x0C
#define PCI_SUBCLASS_USB          0x03
#define PCI_PROGIF_UHCI           0x00
#define PCI_PROGIF_OHCI           0x10
#define PCI_PROGIF_EHCI           0x20
#define PCI_PROGIF_XHCI           0x30

/*
 * Bring up the first UHCI controller found on the PCI bus, enumerate the
 * root hub and attach HID devices to the input subsystem.
 * Returns 0 on success (controller found + reset), negative if no UHCI
 * controller is present. Safe to call once after pci_init().
 */
int uhci_init(void);

/*
 * Poll all enumerated UHCI HID endpoints once for new input reports and
 * push key/rel events into the input subsystem. Call this periodically
 * (e.g. from the timer tick or the idle loop) -- this driver is
 * poll-driven, it does not rely on USB interrupts.
 */
void uhci_poll(void);

/*
 * Top-level USB entry point. Wraps uhci_init(); provided so the integrator
 * has a single int usb_init_hc(void) symbol to call from kernel_main after
 * pci_init(). (Named usb_init_hc to avoid colliding with the legacy
 * void usb_init(void) in usb_core.c.)
 */
int usb_init_hc(void);

#endif
