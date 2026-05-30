# Power Management & ACPI

Complete power management implementation for AutomationOS with ACPI support, battery monitoring, CPU frequency scaling, thermal management, and sleep states.

## Table of Contents

1. [Overview](#overview)
2. [ACPI Implementation](#acpi-implementation)
3. [Power States](#power-states)
4. [Battery Management](#battery-management)
5. [CPU Power Management](#cpu-power-management)
6. [Thermal Management](#thermal-management)
7. [Power Profiles](#power-profiles)
8. [User Interface](#user-interface)
9. [API Reference](#api-reference)
10. [Tools](#tools)

## Overview

AutomationOS includes a comprehensive power management system that provides:

- **ACPI Support**: Full ACPI 2.0+ implementation with table parsing
- **Sleep States**: S1 (Standby), S3 (Sleep), S4 (Hibernate), S5 (Power off)
- **Battery Monitoring**: Real-time battery status, percentage, time remaining
- **CPU Frequency Scaling**: Dynamic frequency adjustment with multiple governors
- **Thermal Management**: Temperature monitoring with automatic cooling
- **Power Profiles**: Performance, Balanced, and Power Saver modes
- **Runtime PM**: Per-device power management with autosuspend

### Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                      User Applications                       │
└───────────────────────────┬─────────────────────────────────┘
                            │
┌───────────────────────────┴─────────────────────────────────┐
│                     Power Management UI                      │
│  • Battery indicator  • Settings panel  • Notifications     │
└───────────────────────────┬─────────────────────────────────┘
                            │
┌───────────────────────────┴─────────────────────────────────┐
│                    Power Daemon (powerd)                     │
│  • Profile selection  • Event handling  • Monitoring        │
└───────────────────────────┬─────────────────────────────────┘
                            │
┌───────────────────────────┴─────────────────────────────────┐
│                   Kernel Power Subsystem                     │
│  ┌────────────┬──────────┬──────────┬───────────┬────────┐ │
│  │  Battery   │ CPUFreq  │ Thermal  │  Display  │Profile │ │
│  │Management  │ Scaling  │Management│   Power   │Manager │ │
│  └────────────┴──────────┴──────────┴───────────┴────────┘ │
└───────────────────────────┬─────────────────────────────────┘
                            │
┌───────────────────────────┴─────────────────────────────────┐
│                       ACPI Layer                             │
│  • Table parsing  • Sleep states  • Event handling          │
└───────────────────────────┬─────────────────────────────────┘
                            │
┌───────────────────────────┴─────────────────────────────────┐
│                         Hardware                             │
│  • CPU  • Battery  • Thermal sensors  • Power controllers   │
└─────────────────────────────────────────────────────────────┘
```

## ACPI Implementation

### ACPI Tables

The ACPI implementation parses and uses the following tables:

#### RSDP (Root System Description Pointer)
- Located in EBDA or BIOS memory (0xE0000-0xFFFFF)
- Points to RSDT or XSDT

#### RSDT/XSDT (Root/Extended System Description Table)
- Contains pointers to all other ACPI tables
- RSDT uses 32-bit pointers, XSDT uses 64-bit

#### FADT (Fixed ACPI Description Table)
- Power management registers (PM1a, PM1b, PM Timer)
- Sleep state information (_S3, _S4, _S5)
- Reset register
- Boot architecture flags

#### MADT (Multiple APIC Description Table)
- Local APIC information for each CPU
- I/O APIC information
- Interrupt overrides
- Used for SMP initialization

#### HPET (High Precision Event Timer)
- Base address of HPET
- Timer capabilities

#### MCFG (PCI Express Configuration)
- PCIe MMIO configuration space base address
- Segment and bus range

### ACPI Initialization

```c
int acpi_init(void) {
    // 1. Find RSDP in memory
    acpi_rsdp_t* rsdp = acpi_find_rsdp();
    
    // 2. Parse RSDT/XSDT
    if (rsdp->revision >= 2 && rsdp->xsdt_address) {
        acpi_parse_xsdt(rsdp->xsdt_address);
    } else {
        acpi_parse_rsdt(rsdp->rsdt_address);
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

### ACPI Mode

ACPI mode is enabled by writing the `ACPI_ENABLE` value to the `SMI_CMD` port:

```c
int acpi_enable(void) {
    // Write ACPI_ENABLE to SMI_CMD port
    outb(fadt->smi_command_port, fadt->acpi_enable);
    
    // Wait for ACPI mode (check PM1_CNT.SCI_EN)
    while (!(inw(pm1a_control_port) & ACPI_PM1_SCI_EN)) {
        // Wait...
    }
    
    return 0;
}
```

## Power States

### System Sleep States

AutomationOS supports the following ACPI sleep states:

#### S0 - Working
- Normal operating state
- All devices powered and active
- CPU running at full or scaled frequency

#### S1 - Standby (CPU Stop)
- CPU stopped, context preserved
- RAM powered
- Devices may be suspended
- **Wake time**: < 1 second

#### S3 - Sleep (Suspend to RAM)
- CPU powered off
- RAM powered and refreshed
- Most hardware powered off
- **Wake time**: 2-3 seconds
- **Power consumption**: 1-5W

#### S4 - Hibernate (Suspend to Disk)
- Complete system state saved to disk
- All hardware powered off
- **Wake time**: 10-30 seconds (full boot)
- **Power consumption**: 0W

#### S5 - Soft Off
- Complete shutdown
- Power supply may remain on
- Can wake via power button or WoL

### Entering Sleep States

#### Suspend to RAM (S3)

```c
int power_suspend_to_ram(void) {
    // 1. Prepare system
    power_prepare_suspend();
    
    // 2. Suspend devices
    power_suspend_devices();
    
    // 3. Save CPU state
    cpu_save_state();
    
    // 4. Enter S3 via ACPI
    acpi_enter_sleep_state(ACPI_STATE_S3);
    
    // === System suspended ===
    // === Wake from S3 ===
    
    // 5. Restore CPU state
    cpu_restore_state();
    
    // 6. Resume devices
    power_resume_devices();
    
    // 7. Complete resume
    power_complete_resume();
    
    return 0;
}
```

#### Hibernate (S4)

```c
int power_hibernate(void) {
    // 1. Prepare system
    power_prepare_suspend();
    
    // 2. Suspend devices
    power_suspend_devices();
    
    // 3. Create hibernation image
    hibernation_create_image("/dev/sda2");
    
    // 4. Write to disk
    hibernation_write_image();
    
    // 5. Power off
    acpi_enter_sleep_state(ACPI_STATE_S5);
    
    return 0;
}
```

On next boot, the bootloader detects the hibernation image and restores it.

### Wake Sources

Configure which events can wake the system:

```c
wake_sources_t sources = {
    .power_button = true,
    .lid = true,
    .keyboard = true,
    .mouse = false,
    .network = false,
    .rtc_alarm = true,
    .usb = true,
};

power_set_wake_sources(&sources);
```

## Battery Management

### Battery Information

```c
typedef struct {
    bool present;
    battery_state_t state;      // CHARGING, DISCHARGING, FULL
    
    uint32_t capacity_mah;      // Design capacity
    uint32_t remaining_mah;     // Current charge
    uint32_t percentage;        // 0-100
    
    uint32_t voltage_mv;        // Voltage (mV)
    int32_t current_ma;         // Current (mA, negative = discharging)
    uint32_t temperature;       // Temperature (°C * 10)
    
    uint32_t time_to_empty_min; // Minutes until empty
    uint32_t time_to_full_min;  // Minutes until full
    
    uint32_t cycle_count;
    uint32_t health_percentage; // Battery health
    
    char manufacturer[32];
    char model[32];
    char technology[16];        // "Li-ion", "Li-poly"
} battery_info_t;
```

### Reading Battery Status

```c
battery_info_t* info = battery_get_info();

printf("Battery: %u%% (%s)\n", 
       info->percentage,
       info->state == BATTERY_STATE_CHARGING ? "Charging" :
       info->state == BATTERY_STATE_DISCHARGING ? "Discharging" :
       "Full");

if (info->state == BATTERY_STATE_DISCHARGING) {
    printf("Time remaining: %uh %02um\n",
           info->time_to_empty_min / 60,
           info->time_to_empty_min % 60);
}
```

### Battery Events

The power management system generates events for battery state changes:

- **POWER_EVENT_BATTERY_LOW**: Battery ≤ 10%
- **POWER_EVENT_BATTERY_CRITICAL**: Battery ≤ 5%
- **POWER_EVENT_AC_PLUGGED**: AC adapter connected
- **POWER_EVENT_AC_UNPLUGGED**: AC adapter disconnected

Register an event handler:

```c
void battery_event_handler(power_event_t event, void* data) {
    if (event == POWER_EVENT_BATTERY_CRITICAL) {
        // Show critical battery warning
        // Suspend or hibernate
    }
}

power_register_event_handler(POWER_EVENT_BATTERY_CRITICAL,
                             battery_event_handler);
```

## CPU Power Management

### CPU Frequency Scaling (CPUFreq)

Dynamic frequency and voltage scaling to reduce power consumption.

#### Governors

- **performance**: Always maximum frequency
- **powersave**: Always minimum frequency
- **ondemand**: Scale based on CPU load (recommended)
- **conservative**: Like ondemand but slower transitions
- **userspace**: User/application controls frequency

#### Setting Governor

```c
// Set all CPUs to ondemand governor
for (uint32_t cpu = 0; cpu < num_cpus; cpu++) {
    cpufreq_set_governor(cpu, CPUFREQ_GOVERNOR_ONDEMAND);
}
```

#### Manual Frequency Control

```c
// Set CPU 0 to 2.4 GHz
cpufreq_set_frequency(0, 2400);

// Get current frequency
uint32_t freq = cpufreq_get_frequency(0);
printf("CPU 0: %u MHz\n", freq);
```

### CPU Idle States (C-States)

When the CPU is idle, it can enter low-power C-states:

- **C0**: Running (no power saving)
- **C1**: Halt (CPU stopped, instant wake)
- **C2**: Stop-Clock (CPU stopped, slower wake)
- **C3**: Deep Sleep (cache flushed, slow wake)
- **C6+**: Even deeper sleep

The scheduler automatically enters appropriate C-states during idle periods.

```c
// Enter C1 state (called by idle loop)
cpuidle_enter_state(cpu, ACPI_CSTATE_C1);
```

## Thermal Management

### Temperature Monitoring

The thermal subsystem monitors temperatures from multiple zones:

- **CPU**: CPU core temperature
- **GPU**: GPU temperature
- **ACPI**: ACPI thermal zone (motherboard)
- **System**: Overall system temperature

```c
// Read CPU temperature
uint32_t temp = thermal_get_temperature("CPU");
printf("CPU: %u.%03u°C\n", temp / 1000, temp % 1000);
```

### Trip Points

Each thermal zone has four trip points:

1. **Active** (60°C): Turn on fan
2. **Passive** (75°C): Start throttling CPU
3. **Hot** (85°C): Urgent cooling, aggressive throttling
4. **Critical** (95°C): Emergency shutdown

### Cooling Actions

#### Fan Control

```c
// Set fan speed (0-100%)
thermal_set_cooling_state("CPU", 70);  // 70% cooling
```

#### CPU Throttling

```c
// Throttle CPU 0 to 80% of max frequency
thermal_throttle_cpu(0, 80);
```

#### Emergency Shutdown

If temperature reaches critical (95°C), the system automatically:
1. Logs critical thermal event
2. Notifies all applications
3. Waits 5 seconds
4. Powers off via ACPI

## Power Profiles

AutomationOS provides three power profiles:

### Performance

Optimized for maximum performance:

- **CPU**: Performance governor (always max frequency)
- **Display**: Never blank or power off
- **Backlight**: 100%
- **Sleep**: Disabled
- **Bluetooth/WiFi**: Enabled

**Power consumption**: High (15-25W)
**Battery life**: 2-4 hours

### Balanced (Default)

Balance between performance and battery life:

- **CPU**: Ondemand governor (scale based on load)
- **Display**: Blank after 5 min, power off after 10 min
- **Backlight**: 100%
- **Sleep**: After 15 min idle
- **Bluetooth/WiFi**: Enabled

**Power consumption**: Medium (8-15W)
**Battery life**: 5-8 hours

### Power Saver

Optimized for maximum battery life:

- **CPU**: Conservative governor (lower frequencies)
- **Display**: Blank after 2 min, power off after 5 min
- **Backlight**: 80%
- **Sleep**: After 5 min idle
- **Bluetooth**: Disabled
- **WiFi**: Enabled

**Power consumption**: Low (5-10W)
**Battery life**: 10+ hours

### Setting Profile

```c
// Set power profile
power_profile_set(POWER_PROFILE_POWER_SAVER);

// Get current profile
power_profile_t profile = power_profile_get();
```

### Automatic Profile Selection

The power daemon (`powerd`) can automatically select profiles based on battery level:

- **AC plugged**: Balanced
- **Battery > 50%**: Balanced
- **Battery 20-50%**: Power Saver
- **Battery < 20%**: Aggressive Power Saver

## User Interface

### Battery Indicator

The desktop panel shows battery status:

```
┌─────────────────────────────┐
│  [85%] 🔋 2:50 remaining    │
└─────────────────────────────┘
```

**Features**:
- Battery percentage
- Charging/discharging icon
- Time remaining estimate
- Warning at 10% (orange)
- Critical at 5% (red)

### Power Settings

**Settings → Power**

```
Power Settings
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

Power Profile:
  ○ Performance
  ● Balanced
  ○ Power Saver

Display:
  Screen blank:    [5] minutes
  Screen off:      [10] minutes
  Brightness:      [████████░░] 80%

Sleep:
  Suspend when idle: [15] minutes
  ☑ Suspend when lid closed
  ☑ Ask before suspending

Battery:
  ☑ Show percentage
  ☑ Show time remaining
  ☐ Notify at 10%
  ☑ Action at 5%: Suspend
```

### Quick Settings

Press **Super+P** or click power icon:

```
┌─────────────────────────────┐
│  Power                      │
├─────────────────────────────┤
│  Suspend                    │
│  Hibernate                  │
│  Shut Down                  │
│  Restart                    │
├─────────────────────────────┤
│  Profile: Balanced     ▼    │
│  Brightness: ████████░░ 80% │
└─────────────────────────────┘
```

## API Reference

### ACPI Functions

```c
// Initialization
int acpi_init(void);
void acpi_shutdown(void);

// Table discovery
acpi_rsdp_t* acpi_find_rsdp(void);
void* acpi_find_table(const char* signature);

// ACPI mode
int acpi_enable(void);
int acpi_disable(void);
bool acpi_is_enabled(void);

// Sleep states
int acpi_enter_sleep_state(acpi_sleep_state_t state);
int acpi_prepare_sleep(acpi_sleep_state_t state);
int acpi_wake_from_sleep(void);

// System control
int acpi_reboot(void);
int acpi_poweroff(void);
```

### Power Management Functions

```c
// Initialization
int power_init(void);
void power_shutdown(void);

// Sleep states
int power_suspend_to_ram(void);         // S3
int power_hibernate(void);              // S4
int power_suspend_to_idle(void);        // Freeze

// Battery
int battery_init(void);
battery_info_t* battery_get_info(void);
uint32_t battery_get_percentage(void);
battery_state_t battery_get_state(void);

// CPU frequency scaling
int cpufreq_init(void);
int cpufreq_set_frequency(uint32_t cpu, uint32_t freq_mhz);
uint32_t cpufreq_get_frequency(uint32_t cpu);
int cpufreq_set_governor(uint32_t cpu, cpufreq_governor_t gov);

// Thermal management
int thermal_init(void);
uint32_t thermal_get_temperature(const char* zone);
int thermal_throttle_cpu(uint32_t cpu, uint32_t percent);

// Display power
int display_blank(void);
int display_poweroff(void);
int backlight_set_brightness(uint32_t percent);

// Power profiles
int power_profile_set(power_profile_t profile);
power_profile_t power_profile_get(void);

// Events
int power_register_event_handler(power_event_t event,
                                 power_event_handler_t handler);
void power_notify_event(power_event_t event, void* data);
```

## Tools

### powerd - Power Management Daemon

Background daemon that manages power policies and responds to events.

```bash
# Start power daemon
powerd --daemon

# Start with specific profile
powerd --profile powersaver --daemon

# Run in foreground (verbose)
powerd --verbose
```

### powerstat - Power Statistics

Display current power statistics.

```bash
# Single shot
powerstat

# Continuous monitoring
powerstat --continuous

# Update every 5 seconds
powerstat -c -i 5
```

Output:
```
=== AutomationOS Power Statistics ===

CPU:
  Frequency: 2400 MHz (min 800, max 3600)
  Load:      30%
  Temp:      45°C

Battery:
  Status:    Discharging
  Charge:    85% (4250 / 5000 mAh)
  Voltage:   11.400 V
  Current:   -1500 mA
  Temp:      35.0°C
  Remaining: 2h 50m

Power Draw: 15.2 W (estimated)
```

### powertop - Power Consumption Analyzer

Analyze power consumption by process and device.

```bash
# Run power analysis
powertop

# 20 second measurement
powertop --time 20
```

Output:
```
=== PowerTop - Power Consumption Analysis ===

Top Power Consumers (Processes):
Name                             Power      CPU   Wakeups/s
--------------------------------------------------------------------
desktop_shell                    850.0 mW   5.2%        120
compositor                       650.0 mW   3.8%         80
systemd                          200.0 mW   0.5%         10

Device Power Consumption:
Device               Power           State
--------------------------------------------------------------------
Display             3500.0 mW          Active
CPU                 5000.0 mW        2400 MHz
WiFi                 800.0 mW          Active
SSD                  500.0 mW          Active
--------------------------------------------------------------------
Total estimated power: 10.65 W

Power Saving Recommendations:
  • Reduce display brightness to save ~1.0 W
  • Enable CPU frequency scaling (ondemand governor)
  • Disable unused devices (Bluetooth, unused USB ports)
  • Close unnecessary background processes
```

## Performance Targets

- **Suspend (S3)**: < 2 seconds
- **Resume (S3)**: < 3 seconds
- **Battery Life**: 10+ hours (modern laptop, balanced profile)
- **Thermal**: Keep CPU under 80°C during normal use
- **Power Draw**: < 10W on battery (Power Saver profile)

## Implementation Status

### Completed ✓

- [x] ACPI table parsing (RSDP, RSDT, XSDT, FADT, MADT, HPET, MCFG)
- [x] ACPI mode enable/disable
- [x] Sleep state framework (S1/S3/S4/S5)
- [x] Battery monitoring framework
- [x] AC adapter detection
- [x] CPU frequency scaling framework
- [x] CPU idle states (C-states)
- [x] Thermal monitoring and cooling
- [x] Display power management (blank, DPMS)
- [x] Backlight control
- [x] Power profiles (Performance, Balanced, Power Saver)
- [x] Power event system
- [x] Power daemon (powerd)
- [x] Power tools (powerstat, powertop)
- [x] API and documentation

### Hardware-Dependent (Requires Platform Support)

- [ ] ACPI AML interpreter (for advanced features)
- [ ] Battery ACPI methods (_BIF, _BST)
- [ ] Thermal ACPI methods (_TMP, _AC0, _PSV)
- [ ] CPU P-states from ACPI
- [ ] Fan control via ACPI or EC
- [ ] Wake sources (GPE configuration)

## References

- [ACPI Specification 6.4](https://uefi.org/specifications)
- [Intel Software Developer's Manual](https://software.intel.com/content/www/us/en/develop/articles/intel-sdm.html)
- [Linux Power Management](https://www.kernel.org/doc/html/latest/power/index.html)
- [Advanced Power Management](https://wiki.osdev.org/APM)

---

**Last Updated**: 2026-05-26
**Version**: 1.0
**Author**: Power Management Engineer, AutomationOS Team
