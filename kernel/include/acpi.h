#ifndef ACPI_H
#define ACPI_H

#include "types.h"

/*
 * ACPI (Advanced Configuration and Power Interface)
 * Complete ACPI implementation for AutomationOS
 */

// ACPI Table Signatures
#define ACPI_SIG_RSDP   "RSD PTR "
#define ACPI_SIG_RSDT   "RSDT"
#define ACPI_SIG_XSDT   "XSDT"
#define ACPI_SIG_FADT   "FACP"
#define ACPI_SIG_MADT   "APIC"
#define ACPI_SIG_HPET   "HPET"
#define ACPI_SIG_MCFG   "MCFG"
#define ACPI_SIG_DSDT   "DSDT"
#define ACPI_SIG_SSDT   "SSDT"
#define ACPI_SIG_BGRT   "BGRT"
#define ACPI_SIG_BERT   "BERT"
#define ACPI_SIG_CPEP   "CPEP"
#define ACPI_SIG_ECDT   "ECDT"
#define ACPI_SIG_SLIT   "SLIT"
#define ACPI_SIG_SRAT   "SRAT"

// RSDP (Root System Description Pointer)
typedef struct {
    char signature[8];          // "RSD PTR "
    uint8_t checksum;
    char oem_id[6];
    uint8_t revision;
    uint32_t rsdt_address;      // RSDT physical address

    // ACPI 2.0+
    uint32_t length;
    uint64_t xsdt_address;      // XSDT physical address
    uint8_t extended_checksum;
    uint8_t reserved[3];
} __attribute__((packed)) acpi_rsdp_t;

// ACPI Table Header (common to all tables)
typedef struct {
    char signature[4];
    uint32_t length;
    uint8_t revision;
    uint8_t checksum;
    char oem_id[6];
    char oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
} __attribute__((packed)) acpi_table_header_t;

// RSDT (Root System Description Table)
typedef struct {
    acpi_table_header_t header;
    uint32_t entries[];         // Array of 32-bit physical addresses
} __attribute__((packed)) acpi_rsdt_t;

// XSDT (Extended System Description Table)
typedef struct {
    acpi_table_header_t header;
    uint64_t entries[];         // Array of 64-bit physical addresses
} __attribute__((packed)) acpi_xsdt_t;

// Generic Address Structure
typedef struct {
    uint8_t address_space_id;   // 0=System Memory, 1=System I/O, etc.
    uint8_t register_bit_width;
    uint8_t register_bit_offset;
    uint8_t access_size;
    uint64_t address;
} __attribute__((packed)) acpi_generic_address_t;

// FADT (Fixed ACPI Description Table)
typedef struct {
    acpi_table_header_t header;
    uint32_t firmware_ctrl;     // Physical address of FACS
    uint32_t dsdt;              // Physical address of DSDT

    uint8_t reserved;
    uint8_t preferred_pm_profile;
    uint16_t sci_interrupt;
    uint32_t smi_command_port;
    uint8_t acpi_enable;
    uint8_t acpi_disable;
    uint8_t s4bios_req;
    uint8_t pstate_control;

    uint32_t pm1a_event_block;
    uint32_t pm1b_event_block;
    uint32_t pm1a_control_block;
    uint32_t pm1b_control_block;
    uint32_t pm2_control_block;
    uint32_t pm_timer_block;
    uint32_t gpe0_block;
    uint32_t gpe1_block;

    uint8_t pm1_event_length;
    uint8_t pm1_control_length;
    uint8_t pm2_control_length;
    uint8_t pm_timer_length;
    uint8_t gpe0_length;
    uint8_t gpe1_length;
    uint8_t gpe1_base;

    uint8_t cstate_control;
    uint16_t worst_c2_latency;
    uint16_t worst_c3_latency;
    uint16_t flush_size;
    uint16_t flush_stride;
    uint8_t duty_offset;
    uint8_t duty_width;

    uint8_t day_alarm;
    uint8_t month_alarm;
    uint8_t century;

    uint16_t boot_arch_flags;
    uint8_t reserved2;
    uint32_t flags;

    acpi_generic_address_t reset_reg;
    uint8_t reset_value;
    uint16_t arm_boot_arch;
    uint8_t fadt_minor_version;

    uint64_t x_firmware_ctrl;
    uint64_t x_dsdt;

    acpi_generic_address_t x_pm1a_event_block;
    acpi_generic_address_t x_pm1b_event_block;
    acpi_generic_address_t x_pm1a_control_block;
    acpi_generic_address_t x_pm1b_control_block;
    acpi_generic_address_t x_pm2_control_block;
    acpi_generic_address_t x_pm_timer_block;
    acpi_generic_address_t x_gpe0_block;
    acpi_generic_address_t x_gpe1_block;
} __attribute__((packed)) acpi_fadt_t;

// FADT Flags
#define ACPI_FADT_WBINVD                (1 << 0)
#define ACPI_FADT_WBINVD_FLUSH          (1 << 1)
#define ACPI_FADT_PROC_C1               (1 << 2)
#define ACPI_FADT_P_LVL2_UP             (1 << 3)
#define ACPI_FADT_PWR_BUTTON            (1 << 4)
#define ACPI_FADT_SLP_BUTTON            (1 << 5)
#define ACPI_FADT_FIX_RTC               (1 << 6)
#define ACPI_FADT_RTC_S4                (1 << 7)
#define ACPI_FADT_TMR_VAL_EXT           (1 << 8)
#define ACPI_FADT_DCK_CAP               (1 << 9)
#define ACPI_FADT_RESET_REG_SUP         (1 << 10)
#define ACPI_FADT_SEALED_CASE           (1 << 11)
#define ACPI_FADT_HEADLESS              (1 << 12)
#define ACPI_FADT_CPU_SW_SLP            (1 << 13)

// MADT (Multiple APIC Description Table)
typedef struct {
    acpi_table_header_t header;
    uint32_t local_apic_address;
    uint32_t flags;
    uint8_t entries[];
} __attribute__((packed)) acpi_madt_t;

// MADT Entry Types
#define ACPI_MADT_TYPE_LOCAL_APIC       0
#define ACPI_MADT_TYPE_IO_APIC          1
#define ACPI_MADT_TYPE_INTERRUPT_OVERRIDE 2
#define ACPI_MADT_TYPE_NMI_SOURCE       3
#define ACPI_MADT_TYPE_LOCAL_APIC_NMI   4
#define ACPI_MADT_TYPE_LOCAL_APIC_OVERRIDE 5
#define ACPI_MADT_TYPE_IO_SAPIC         6
#define ACPI_MADT_TYPE_LOCAL_SAPIC      7
#define ACPI_MADT_TYPE_INTERRUPT_SOURCE 8
#define ACPI_MADT_TYPE_LOCAL_X2APIC     9
#define ACPI_MADT_TYPE_LOCAL_X2APIC_NMI 10

// MADT Entry Header
typedef struct {
    uint8_t type;
    uint8_t length;
} __attribute__((packed)) acpi_madt_entry_header_t;

// MADT Local APIC
typedef struct {
    acpi_madt_entry_header_t header;
    uint8_t acpi_processor_id;
    uint8_t apic_id;
    uint32_t flags;
} __attribute__((packed)) acpi_madt_local_apic_t;

// MADT I/O APIC
typedef struct {
    acpi_madt_entry_header_t header;
    uint8_t io_apic_id;
    uint8_t reserved;
    uint32_t io_apic_address;
    uint32_t global_system_interrupt_base;
} __attribute__((packed)) acpi_madt_io_apic_t;

// MADT Interrupt Override
typedef struct {
    acpi_madt_entry_header_t header;
    uint8_t bus;
    uint8_t source;
    uint32_t global_system_interrupt;
    uint16_t flags;
} __attribute__((packed)) acpi_madt_interrupt_override_t;

// HPET (High Precision Event Timer)
typedef struct {
    acpi_table_header_t header;
    uint32_t event_timer_block_id;
    acpi_generic_address_t base_address;
    uint8_t hpet_number;
    uint16_t minimum_tick;
    uint8_t page_protection;
} __attribute__((packed)) acpi_hpet_t;

// MCFG (PCI Express Memory Mapped Configuration)
typedef struct {
    acpi_table_header_t header;
    uint64_t reserved;
    uint8_t entries[];
} __attribute__((packed)) acpi_mcfg_t;

// MCFG Entry
typedef struct {
    uint64_t base_address;
    uint16_t pci_segment_group;
    uint8_t start_bus;
    uint8_t end_bus;
    uint32_t reserved;
} __attribute__((packed)) acpi_mcfg_entry_t;

// ACPI State Management
typedef enum {
    ACPI_STATE_S0 = 0,          // Working
    ACPI_STATE_S1,              // Standby
    ACPI_STATE_S2,              // Suspend (rarely used)
    ACPI_STATE_S3,              // Sleep (Suspend to RAM)
    ACPI_STATE_S4,              // Hibernate (Suspend to Disk)
    ACPI_STATE_S5,              // Soft Off
} acpi_sleep_state_t;

// ACPI Power States
typedef enum {
    ACPI_POWER_D0 = 0,          // Fully on
    ACPI_POWER_D1,              // Light sleep
    ACPI_POWER_D2,              // Deeper sleep
    ACPI_POWER_D3_HOT,          // Off but can wake
    ACPI_POWER_D3_COLD,         // Off, no wake
} acpi_power_state_t;

// CPU C-States (idle states)
typedef enum {
    ACPI_CSTATE_C0 = 0,         // Running
    ACPI_CSTATE_C1,             // Halt
    ACPI_CSTATE_C2,             // Stop-Clock
    ACPI_CSTATE_C3,             // Deep Sleep
    ACPI_CSTATE_C6,             // Deep Power Down
} acpi_cstate_t;

// ACPI Initialization
int acpi_init(void);
void acpi_shutdown(void);

// RSDP and Table Discovery
acpi_rsdp_t* acpi_find_rsdp(void);
void* acpi_find_table(const char* signature);
bool acpi_verify_checksum(const void* table, size_t length);

// Table Parsing
int acpi_parse_rsdt(acpi_rsdt_t* rsdt);
int acpi_parse_xsdt(acpi_xsdt_t* xsdt);
int acpi_parse_fadt(acpi_fadt_t* fadt);
int acpi_parse_madt(acpi_madt_t* madt);
int acpi_parse_hpet(acpi_hpet_t* hpet);
int acpi_parse_mcfg(acpi_mcfg_t* mcfg);

// ACPI Mode Control
int acpi_enable(void);
int acpi_disable(void);
bool acpi_is_enabled(void);

// Sleep State Management
int acpi_enter_sleep_state(acpi_sleep_state_t state);
int acpi_prepare_sleep(acpi_sleep_state_t state);
int acpi_wake_from_sleep(void);

// Power Management
int acpi_set_power_state(acpi_power_state_t state);
acpi_power_state_t acpi_get_power_state(void);

// System Control
int acpi_reboot(void);
int acpi_poweroff(void);

/*
 * High-level power entry points (never return).
 *   power_off()    -- ACPI soft-off (S5). QEMU exits its process on this.
 *   power_reboot() -- ACPI reset register, falling back to the 8042 reset.
 * These are the names the syscall layer / powermenu app should call.
 */
void power_off(void);
void power_reboot(void);

/*
 * kernel/drivers/acpi/acpi.c public API
 * ======================================
 * These names are used by the INTEGRATOR when wiring SYS_POWEROFF (46) and
 * SYS_REBOOT (47) syscall handlers. The symbol names are distinct from the
 * kernel/acpi/acpi.c symbols (acpi_poweroff / acpi_reboot) to allow both
 * drivers to coexist in the same link unit without clashes.
 *
 *   acpi_init()       -- Call once from kernel_main() after pci_init().
 *   acpi_power_off()  -- Soft-off (S5). Does not return on success.
 *   acpi_reboot()     -- (re-declared below as void) Reset. Does not return.
 *   acpi_present()    -- 1 if RSDP+FADT found, 0 otherwise.
 *
 * NOTE: acpi_reboot() is already declared above as "int acpi_reboot(void)"
 * (from kernel/acpi/acpi.c). The drivers/acpi/acpi.c version is "void" and
 * is a distinct link symbol only when kernel/acpi/acpi.c is NOT in the build.
 * The integrator must ensure only one acpi_reboot() is linked at a time.
 */
void acpi_power_off(void);
int  acpi_present(void);

// Event Handling
typedef void (*acpi_event_handler_t)(uint32_t event);
int acpi_install_event_handler(uint32_t event, acpi_event_handler_t handler);
int acpi_remove_event_handler(uint32_t event);

// ACPI Events
#define ACPI_EVENT_POWER_BUTTON         (1 << 0)
#define ACPI_EVENT_SLEEP_BUTTON         (1 << 1)
#define ACPI_EVENT_LID                  (1 << 2)
#define ACPI_EVENT_AC_ADAPTER           (1 << 3)
#define ACPI_EVENT_BATTERY              (1 << 4)
#define ACPI_EVENT_THERMAL              (1 << 5)

// PM1 Control Register Bits
#define ACPI_PM1_SCI_EN                 (1 << 0)
#define ACPI_PM1_BM_RLD                 (1 << 1)
#define ACPI_PM1_GBL_RLS                (1 << 2)
#define ACPI_PM1_SLP_TYP_SHIFT          10
#define ACPI_PM1_SLP_EN                 (1 << 13)

// Global ACPI State
typedef struct {
    bool initialized;
    bool enabled;

    acpi_rsdp_t* rsdp;
    acpi_rsdt_t* rsdt;
    acpi_xsdt_t* xsdt;
    acpi_fadt_t* fadt;
    acpi_madt_t* madt;
    acpi_hpet_t* hpet;
    acpi_mcfg_t* mcfg;
    acpi_table_header_t* dsdt;   // Differentiated System Description Table (AML)

    // PM registers
    uint16_t pm1a_control_port;
    uint16_t pm1b_control_port;
    uint16_t pm1a_status_port;
    uint16_t pm1b_status_port;
    uint16_t pm_timer_port;

    // Sleep type values for S3, S4, S5 (SLP_TYPa); S5 also tracks SLP_TYPb
    uint16_t s3_sleep_type;
    uint16_t s4_sleep_type;
    uint16_t s5_sleep_type;
    uint16_t s5_sleep_type_b;

    // CPU information
    uint32_t num_cpus;
    uint32_t local_apic_address;
    uint32_t num_io_apics;

    // HPET
    uint64_t hpet_address;
    bool hpet_available;

    // PCIe
    uint64_t pcie_config_base;
    uint16_t pcie_segment;
    uint8_t pcie_start_bus;
    uint8_t pcie_end_bus;
} acpi_state_t;

extern acpi_state_t acpi_state;

// Utility functions
uint8_t acpi_read_pm1_control(void);
void acpi_write_pm1_control(uint8_t value);
uint16_t acpi_read_pm1_status(void);
void acpi_write_pm1_status(uint16_t value);
uint32_t acpi_read_pm_timer(void);

// ACPI Resource Management
int acpi_allocate_resources(void);
void acpi_free_resources(void);

// Debug
void acpi_dump_tables(void);
void acpi_print_info(void);

#endif // ACPI_H
