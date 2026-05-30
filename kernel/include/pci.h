#ifndef PCI_H
#define PCI_H

#include "types.h"

// PCI Configuration Space Registers
#define PCI_CONFIG_VENDOR_ID    0x00
#define PCI_CONFIG_DEVICE_ID    0x02
#define PCI_CONFIG_COMMAND      0x04
#define PCI_CONFIG_STATUS       0x06
#define PCI_CONFIG_REVISION_ID  0x08
#define PCI_CONFIG_PROG_IF      0x09
#define PCI_CONFIG_SUBCLASS     0x0A
#define PCI_CONFIG_CLASS_CODE   0x0B
#define PCI_CONFIG_HEADER_TYPE  0x0E
#define PCI_CONFIG_BAR0         0x10
#define PCI_CONFIG_BAR1         0x14
#define PCI_CONFIG_BAR2         0x18
#define PCI_CONFIG_BAR3         0x1C
#define PCI_CONFIG_BAR4         0x20
#define PCI_CONFIG_BAR5         0x24
#define PCI_CONFIG_INTERRUPT_LINE 0x3C
#define PCI_CONFIG_INTERRUPT_PIN  0x3D

// PCI Command Register Bits
#define PCI_COMMAND_IO_SPACE      0x0001
#define PCI_COMMAND_MEMORY_SPACE  0x0002
#define PCI_COMMAND_BUS_MASTER    0x0004
#define PCI_COMMAND_INTERRUPT_DISABLE 0x0400

// PCI Class Codes
#define PCI_CLASS_STORAGE         0x01
#define PCI_SUBCLASS_NVME         0x08
#define PCI_PROG_IF_NVME          0x02

// PCI BAR Types
#define PCI_BAR_TYPE_MMIO         0x00
#define PCI_BAR_TYPE_IO           0x01
#define PCI_BAR_64BIT             0x04

// PCI device structure
typedef struct {
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t bus;
    uint8_t device;
    uint8_t function;
    uint8_t class_code;
    uint8_t subclass;
    uint8_t prog_if;
    uint8_t revision_id;
    uint8_t interrupt_line;
    uint8_t interrupt_pin;
    uint64_t bar[6];
} pci_device_t;

// PCI Functions
void pci_init(void);
uint32_t pci_config_read_dword(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset);
uint16_t pci_config_read_word(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset);
uint8_t pci_config_read_byte(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset);
void pci_config_write_dword(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset, uint32_t value);
void pci_config_write_word(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset, uint16_t value);
void pci_config_write_byte(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset, uint8_t value);

pci_device_t* pci_find_device(uint16_t vendor_id, uint16_t device_id);
pci_device_t* pci_find_class(uint8_t class_code, uint8_t subclass, uint8_t prog_if);
uint64_t pci_get_bar(pci_device_t* dev, uint8_t bar_num);
void pci_enable_bus_master(pci_device_t* dev);
void pci_enable_memory_space(pci_device_t* dev);

#endif
