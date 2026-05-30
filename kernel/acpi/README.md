# ACPI Subsystem

This directory contains the ACPI (Advanced Configuration and Power Interface) implementation for AutomationOS.

## Overview

The ACPI subsystem provides hardware discovery, power management, and system configuration through ACPI tables and methods.

**Features**:
- ACPI 1.0, 2.0, 3.0+ support
- Complete table parsing (RSDP, RSDT, XSDT, FADT, MADT, HPET, MCFG)
- ACPI mode enable/disable
- Sleep state management (S1/S3/S4/S5)
- PM register access
- System control (reboot, poweroff)
- Multi-CPU discovery via MADT

## Files

```
acpi/
├── acpi.c       - Main ACPI implementation (3,000 LOC)
├── Makefile     - Build configuration
└── README.md    - This file
```

## ACPI Tables

### RSDP (Root System Description Pointer)

The starting point for ACPI. Located in:
1. EBDA (Extended BIOS Data Area) at `[0x40E]`
2. BIOS memory region (0xE0000 - 0xFFFFF)

**Signature**: `"RSD PTR "`

**Structure**:
```c
typedef struct {
    char signature[8];      // "RSD PTR "
    uint8_t checksum;
    char oem_id[6];
    uint8_t revision;
    uint32_t rsdt_address;  // RSDT physical address
    
    // ACPI 2.0+
    uint32_t length;
    uint64_t xsdt_address;  // XSDT physical address
    uint8_t extended_checksum;
    uint8_t reserved[3];
} acpi_rsdp_t;
```

### RSDT (Root System Description Table)

32-bit table containing pointers to other ACPI tables.

**Signature**: `"RSDT"`

**Structure**:
```c
typedef struct {
    acpi_table_header_t header;
    uint32_t entries[];  // Array of 32-bit physical addresses
} acpi_rsdt_t;
```

### XSDT (Extended System Description Table)

64-bit version of RSDT (ACPI 2.0+).

**Signature**: `"XSDT"`

**Structure**:
```c
typedef struct {
    acpi_table_header_t header;
    uint64_t entries[];  // Array of 64-bit physical addresses
} acpi_xsdt_t;
```

### FADT (Fixed ACPI Description Table)

Contains power management and boot architecture information.

**Signature**: `"FACP"` (Fixed ACPI)

**Key fields**:
- PM1a/PM1b control registers
- PM1a/PM1b status registers
- PM Timer register
- Reset register
- Sleep type values (_S3, _S4, _S5)
- SMI command port
- ACPI enable/disable values

### MADT (Multiple APIC Description Table)

Contains information about APICs and CPUs for SMP.

**Signature**: `"APIC"`

**Entry types**:
- Local APIC (per CPU)
- I/O APIC
- Interrupt Source Override
- NMI Source
- Local APIC NMI

### HPET (High Precision Event Timer)

Contains HPET information.

**Signature**: `"HPET"`

**Key fields**:
- Base address
- Timer block ID
- Minimum tick period

### MCFG (PCI Express Memory Mapped Configuration)

Contains PCIe MMIO configuration space base address.

**Signature**: `"MCFG"`

**Key fields**:
- Base address
- Segment group
- Start/end bus numbers

## Initialization

### Step-by-Step Initialization

```c
int acpi_init(void) {
    // 1. Find RSDP
    acpi_rsdp_t* rsdp = acpi_find_rsdp();
    if (!rsdp) {
        return -1;
    }
    
    // 2. Parse RSDT or XSDT
    if (rsdp->revision >= 2 && rsdp->xsdt_address) {
        // ACPI 2.0+: Use XSDT (64-bit)
        acpi_xsdt_t* xsdt = phys_to_virt(rsdp->xsdt_address);
        acpi_parse_xsdt(xsdt);
    } else {
        // ACPI 1.0: Use RSDT (32-bit)
        acpi_rsdt_t* rsdt = phys_to_virt(rsdp->rsdt_address);
        acpi_parse_rsdt(rsdt);
    }
    
    // 3. Find and parse FADT (power management)
    acpi_fadt_t* fadt = acpi_find_table("FACP");
    acpi_parse_fadt(fadt);
    
    // 4. Find and parse MADT (APICs/CPUs)
    acpi_madt_t* madt = acpi_find_table("APIC");
    acpi_parse_madt(madt);
    
    // 5. Find and parse HPET
    acpi_hpet_t* hpet = acpi_find_table("HPET");
    acpi_parse_hpet(hpet);
    
    // 6. Find and parse MCFG (PCIe)
    acpi_mcfg_t* mcfg = acpi_find_table("MCFG");
    acpi_parse_mcfg(mcfg);
    
    // 7. Enable ACPI mode
    acpi_enable();
    
    return 0;
}
```

## ACPI Mode

ACPI has two modes:
1. **Legacy mode**: BIOS control
2. **ACPI mode**: OS control

### Enabling ACPI Mode

```c
int acpi_enable(void) {
    // Check if already in ACPI mode
    if (inw(pm1a_control_port) & ACPI_PM1_SCI_EN) {
        return 0;  // Already enabled
    }
    
    // Write ACPI_ENABLE to SMI_CMD port
    outb(fadt->smi_command_port, fadt->acpi_enable);
    
    // Wait for ACPI mode (timeout 3 seconds)
    for (int i = 0; i < 300; i++) {
        if (inw(pm1a_control_port) & ACPI_PM1_SCI_EN) {
            return 0;  // Success
        }
        timer_sleep(10);  // 10ms
    }
    
    return -1;  // Timeout
}
```

## Sleep States

ACPI defines six sleep states (S0-S5):

| State | Name       | Description                    | Power | Wake Time |
|-------|------------|--------------------------------|-------|-----------|
| S0    | Working    | Normal operation               | Full  | N/A       |
| S1    | Standby    | CPU stopped, RAM powered       | Low   | < 1s      |
| S2    | Suspend    | CPU off, RAM powered (rare)    | Low   | < 1s      |
| S3    | Sleep      | Suspend to RAM                 | Very Low | 2-3s  |
| S4    | Hibernate  | Suspend to Disk                | None  | 10-30s    |
| S5    | Soft Off   | Complete shutdown              | None  | Full boot |

### Entering Sleep States

```c
int acpi_enter_sleep_state(acpi_sleep_state_t state) {
    // Get sleep type value (from DSDT _Sx methods)
    uint16_t sleep_type;
    switch (state) {
        case ACPI_STATE_S1:
            sleep_type = 0x00;
            break;
        case ACPI_STATE_S3:
            sleep_type = acpi_state.s3_sleep_type;
            break;
        case ACPI_STATE_S4:
            sleep_type = acpi_state.s4_sleep_type;
            break;
        case ACPI_STATE_S5:
            sleep_type = acpi_state.s5_sleep_type;
            break;
        default:
            return -1;
    }
    
    // Disable interrupts
    cli();
    
    // Read PM1 control register
    uint16_t pm1_control = inw(pm1a_control_port);
    
    // Set SLP_TYP and SLP_EN
    pm1_control &= ~(0x7 << 10);            // Clear SLP_TYP
    pm1_control |= (sleep_type << 10);      // Set SLP_TYP
    pm1_control |= (1 << 13);               // Set SLP_EN
    
    // Write to PM1 control register
    outw(pm1a_control_port, pm1_control);
    if (pm1b_control_port) {
        outw(pm1b_control_port, pm1_control);
    }
    
    // Wait for sleep (should not return for S4/S5)
    for (;;) {
        halt();
    }
}
```

## PM Registers

### PM1 Control Register

Controls power management and sleep states.

**Bits**:
- `[0]` SCI_EN: Enable ACPI mode
- `[1]` BM_RLD: Bus master reload
- `[2]` GBL_RLS: Global release
- `[12:10]` SLP_TYP: Sleep type
- `[13]` SLP_EN: Sleep enable

**Access**:
```c
// Read PM1 control
uint16_t pm1_control = inw(pm1a_control_port);

// Write PM1 control
outw(pm1a_control_port, pm1_control);
```

### PM1 Status Register

Status of power management events.

**Bits**:
- `[0]` TMR_STS: PM Timer carry
- `[4]` BM_STS: Bus master status
- `[5]` GBL_STS: Global status
- `[8]` PWRBTN_STS: Power button pressed
- `[9]` SLPBTN_STS: Sleep button pressed
- `[10]` RTC_STS: RTC alarm
- `[15]` WAK_STS: Wake status

**Access**:
```c
// Read PM1 status
uint16_t pm1_status = inw(pm1a_status_port);

// Clear status bit (write 1 to clear)
outw(pm1a_status_port, (1 << 8));  // Clear power button
```

### PM Timer

24-bit or 32-bit timer that runs at 3.579545 MHz.

**Access**:
```c
// Read PM Timer
uint32_t timer_value = inl(pm_timer_port);

// Check if 32-bit timer
if (fadt->flags & ACPI_FADT_TMR_VAL_EXT) {
    // 32-bit timer
} else {
    // 24-bit timer (mask upper bits)
    timer_value &= 0xFFFFFF;
}
```

## System Control

### Reboot

```c
int acpi_reboot(void) {
    // Check if reset register is supported
    if (!(fadt->flags & ACPI_FADT_RESET_REG_SUP)) {
        return -1;
    }
    
    // Use ACPI reset register
    acpi_generic_address_t* reset_reg = &fadt->reset_reg;
    uint8_t reset_value = fadt->reset_value;
    
    if (reset_reg->address_space_id == 1) {
        // System I/O
        outb((uint16_t)reset_reg->address, reset_value);
    } else if (reset_reg->address_space_id == 0) {
        // System Memory
        *(volatile uint8_t*)phys_to_virt(reset_reg->address) = reset_value;
    }
    
    // Wait for reset
    for (;;) {
        halt();
    }
}
```

### Power Off

```c
int acpi_poweroff(void) {
    // Enter S5 sleep state
    return acpi_enter_sleep_state(ACPI_STATE_S5);
}
```

## Usage Examples

### Get CPU Count

```c
// Initialize ACPI
acpi_init();

// Get number of CPUs from MADT
uint32_t num_cpus = acpi_state.num_cpus;
kprintf("System has %u CPUs\n", num_cpus);
```

### Find HPET Base Address

```c
// Initialize ACPI
acpi_init();

// Check if HPET is available
if (acpi_state.hpet_available) {
    uint64_t hpet_addr = acpi_state.hpet_address;
    kprintf("HPET base address: 0x%016lx\n", hpet_addr);
}
```

### Get PCIe Configuration Base

```c
// Initialize ACPI
acpi_init();

// Get PCIe config base from MCFG
uint64_t pcie_base = acpi_state.pcie_config_base;
uint8_t start_bus = acpi_state.pcie_start_bus;
uint8_t end_bus = acpi_state.pcie_end_bus;

kprintf("PCIe config: 0x%016lx, buses %u-%u\n",
        pcie_base, start_bus, end_bus);
```

### Read PM Timer

```c
// Read PM Timer (runs at 3.579545 MHz)
uint32_t t1 = acpi_read_pm_timer();

// Do something...
delay(1000);  // 1ms

uint32_t t2 = acpi_read_pm_timer();

// Calculate elapsed time
uint32_t ticks = (t2 - t1) & 0xFFFFFF;  // 24-bit mask
uint32_t microseconds = (ticks * 1000000) / 3579545;

kprintf("Elapsed: %u μs\n", microseconds);
```

## Debugging

### Dump All Tables

```c
void acpi_dump_tables(void) {
    kprintf("[ACPI] ===== ACPI Tables =====\n");
    
    // RSDP
    if (acpi_state.rsdp) {
        kprintf("[ACPI] RSDP: %.6s, revision %u\n",
                acpi_state.rsdp->oem_id,
                acpi_state.rsdp->revision);
    }
    
    // RSDT/XSDT
    if (acpi_state.xsdt) {
        kprintf("[ACPI] XSDT: %u bytes\n",
                acpi_state.xsdt->header.length);
    } else if (acpi_state.rsdt) {
        kprintf("[ACPI] RSDT: %u bytes\n",
                acpi_state.rsdt->header.length);
    }
    
    // FADT
    if (acpi_state.fadt) {
        kprintf("[ACPI] FADT: Revision %u\n",
                acpi_state.fadt->header.revision);
    }
    
    // MADT
    if (acpi_state.madt) {
        kprintf("[ACPI] MADT: %u CPUs, %u I/O APICs\n",
                acpi_state.num_cpus,
                acpi_state.num_io_apics);
    }
    
    // HPET
    if (acpi_state.hpet) {
        kprintf("[ACPI] HPET: 0x%016lx\n",
                acpi_state.hpet_address);
    }
    
    // MCFG
    if (acpi_state.mcfg) {
        kprintf("[ACPI] MCFG: PCIe base 0x%016lx\n",
                acpi_state.pcie_config_base);
    }
}
```

### Verify Checksum

```c
bool acpi_verify_checksum(const void* table, size_t length) {
    const uint8_t* bytes = (const uint8_t*)table;
    uint8_t sum = 0;
    
    for (size_t i = 0; i < length; i++) {
        sum += bytes[i];
    }
    
    return sum == 0;
}

// Use it
if (!acpi_verify_checksum(fadt, fadt->header.length)) {
    kprintf("ERROR: FADT checksum invalid!\n");
}
```

## Global ACPI State

The ACPI subsystem maintains a global state structure:

```c
typedef struct {
    bool initialized;
    bool enabled;
    
    // Tables
    acpi_rsdp_t* rsdp;
    acpi_rsdt_t* rsdt;
    acpi_xsdt_t* xsdt;
    acpi_fadt_t* fadt;
    acpi_madt_t* madt;
    acpi_hpet_t* hpet;
    acpi_mcfg_t* mcfg;
    
    // PM registers
    uint16_t pm1a_control_port;
    uint16_t pm1b_control_port;
    uint16_t pm1a_status_port;
    uint16_t pm1b_status_port;
    uint16_t pm_timer_port;
    
    // Sleep types
    uint16_t s3_sleep_type;
    uint16_t s4_sleep_type;
    uint16_t s5_sleep_type;
    
    // CPU info
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
```

## API Reference

### Initialization

```c
int acpi_init(void);
void acpi_shutdown(void);
```

### Table Discovery

```c
acpi_rsdp_t* acpi_find_rsdp(void);
void* acpi_find_table(const char* signature);
bool acpi_verify_checksum(const void* table, size_t length);
```

### ACPI Mode

```c
int acpi_enable(void);
int acpi_disable(void);
bool acpi_is_enabled(void);
```

### Sleep States

```c
int acpi_enter_sleep_state(acpi_sleep_state_t state);
int acpi_prepare_sleep(acpi_sleep_state_t state);
int acpi_wake_from_sleep(void);
```

### System Control

```c
int acpi_reboot(void);
int acpi_poweroff(void);
```

### PM Registers

```c
uint8_t acpi_read_pm1_control(void);
void acpi_write_pm1_control(uint8_t value);
uint16_t acpi_read_pm1_status(void);
void acpi_write_pm1_status(uint16_t value);
uint32_t acpi_read_pm_timer(void);
```

### Debug

```c
void acpi_dump_tables(void);
void acpi_print_info(void);
```

## Limitations

Current limitations (future enhancements):

1. **No AML interpreter**: Cannot execute ACPI methods (_PTS, _WAK, etc.)
2. **Static sleep types**: Uses default values instead of parsing _S3/_S4/_S5
3. **No GPE support**: General Purpose Events not implemented
4. **No EC support**: Embedded Controller not implemented
5. **No ACPI devices**: Only table parsing, no device enumeration

## Future Work

- [ ] AML interpreter for DSDT/SSDT
- [ ] ACPI device enumeration
- [ ] GPE (General Purpose Events)
- [ ] Embedded Controller (EC)
- [ ] ACPI thermal zone methods
- [ ] ACPI battery methods (_BIF, _BST)
- [ ] ACPI processor methods (_CST, _PSS)

## References

- [ACPI Specification 6.4](https://uefi.org/specifications)
- [OSDev ACPI](https://wiki.osdev.org/ACPI)
- [Intel ACPI Component Architecture](https://acpica.org/)

## Maintainer

**Power Management Engineer**
AutomationOS Team
