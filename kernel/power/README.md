# Power Management Subsystem

This directory contains the power management subsystem for AutomationOS.

## Overview

The power management subsystem provides comprehensive power management features including:

- Battery monitoring and management
- CPU frequency scaling (CPUFreq)
- CPU idle states (CPUIdle)
- Thermal management and cooling
- Display power management
- Power profiles (Performance, Balanced, Power Saver)
- Power event handling

## Directory Structure

```
power/
├── power.c       - Main power management (suspend/resume/hibernate)
├── battery.c     - Battery and AC adapter management
├── cpufreq.c     - CPU frequency scaling
├── cpuidle.c     - CPU idle state management
├── thermal.c     - Thermal monitoring and cooling
├── display.c     - Display power and backlight
├── profile.c     - Power profile management
├── events.c      - Power event system
├── Makefile      - Build configuration
└── README.md     - This file
```

## Components

### power.c - Core Power Management

Main power management including:
- System suspend to RAM (S3)
- Hibernation (S4)
- Suspend to idle
- Device power management coordination
- Power statistics
- Wake source configuration

**Key Functions**:
```c
int power_init(void);
int power_suspend_to_ram(void);
int power_hibernate(void);
power_stats_t* power_get_stats(void);
```

### battery.c - Battery Management

Battery and AC adapter monitoring:
- Battery information (percentage, voltage, current)
- AC adapter detection
- Time remaining calculation
- Low/critical battery warnings
- Automatic event generation

**Key Functions**:
```c
battery_info_t* battery_get_info(void);
uint32_t battery_get_percentage(void);
bool ac_adapter_is_online(void);
```

### cpufreq.c - CPU Frequency Scaling

Dynamic CPU frequency adjustment:
- Five governors (performance, powersave, ondemand, conservative, userspace)
- Per-CPU frequency policies
- Frequency range configuration
- MSR-based hardware control

**Key Functions**:
```c
int cpufreq_set_frequency(uint32_t cpu, uint32_t freq_mhz);
int cpufreq_set_governor(uint32_t cpu, cpufreq_governor_t gov);
uint32_t cpufreq_get_frequency(uint32_t cpu);
```

### cpuidle.c - CPU Idle States

CPU power management during idle:
- C-state support (C0/C1/C2/C3)
- Per-CPU idle tracking
- Latency management
- Time-in-state statistics

**Key Functions**:
```c
int cpuidle_enter_state(uint32_t cpu, acpi_cstate_t state);
cpuidle_state_t* cpuidle_get_state(uint32_t cpu);
```

### thermal.c - Thermal Management

Temperature monitoring and cooling:
- Multi-zone monitoring (CPU, GPU, ACPI, System)
- Four trip points (Active, Passive, Hot, Critical)
- Automatic fan control
- CPU throttling
- Emergency shutdown protection

**Key Functions**:
```c
uint32_t thermal_get_temperature(const char* zone);
int thermal_throttle_cpu(uint32_t cpu, uint32_t percent);
void thermal_check_trip_points(thermal_zone_t* zone);
```

### display.c - Display Power

Display and backlight management:
- Screen blanking with timeout
- DPMS power states
- Backlight control (0-100%)
- Automatic timeout management

**Key Functions**:
```c
int display_blank(void);
int display_poweroff(void);
int backlight_set_brightness(uint32_t percent);
```

### profile.c - Power Profiles

Predefined power management profiles:
- **Performance**: Maximum performance, high power
- **Balanced**: Balance performance and battery (default)
- **Power Saver**: Maximum battery life

**Key Functions**:
```c
int power_profile_set(power_profile_t profile);
power_profile_t power_profile_get(void);
```

### events.c - Event System

Power event management:
- Event registration and notification
- Battery events (low, critical)
- AC adapter events
- Thermal events
- Lid and button events

**Key Functions**:
```c
int power_register_event_handler(power_event_t event,
                                 power_event_handler_t handler);
void power_notify_event(power_event_t event, void* data);
```

## Initialization

The power management subsystem must be initialized after ACPI:

```c
// In kernel init
acpi_init();        // Initialize ACPI first
power_init();       // Then power management
```

This will initialize all sub-components:
1. Battery management
2. AC adapter detection
3. CPU frequency scaling
4. CPU idle states
5. Thermal management
6. Display power
7. Backlight control
8. Power profiles (default: Balanced)

## Usage Examples

### Suspending the System

```c
// Suspend to RAM (S3)
int ret = power_suspend_to_ram();
if (ret < 0) {
    kprintf("Suspend failed: %d\n", ret);
}

// System is now suspended
// ... User presses power button ...
// System resumes here

kprintf("Resumed from suspend\n");
```

### Reading Battery Status

```c
battery_info_t* bat = battery_get_info();

if (bat->present) {
    kprintf("Battery: %u%% ", bat->percentage);
    
    if (bat->state == BATTERY_STATE_CHARGING) {
        kprintf("(charging, %u min to full)\n",
                bat->time_to_full_min);
    } else if (bat->state == BATTERY_STATE_DISCHARGING) {
        kprintf("(discharging, %u min remaining)\n",
                bat->time_to_empty_min);
    } else {
        kprintf("(full)\n");
    }
}
```

### Setting CPU Frequency

```c
// Set CPU 0 to ondemand governor
cpufreq_set_governor(0, CPUFREQ_GOVERNOR_ONDEMAND);

// Set specific frequency (2.4 GHz)
cpufreq_set_frequency(0, 2400);

// Get current frequency
uint32_t freq = cpufreq_get_frequency(0);
kprintf("CPU 0: %u MHz\n", freq);
```

### Monitoring Temperature

```c
// Get CPU temperature
uint32_t temp = thermal_get_temperature("CPU");

if (temp > 75000) {  // > 75°C
    kprintf("CPU hot: %u.%03u°C\n", temp / 1000, temp % 1000);
    
    // Throttle CPU
    thermal_throttle_cpu(0, 80);  // Throttle to 80%
}
```

### Handling Power Events

```c
void battery_low_handler(power_event_t event, void* data) {
    battery_info_t* bat = (battery_info_t*)data;
    
    kprintf("WARNING: Battery low (%u%%)\n", bat->percentage);
    
    // Switch to power saver mode
    power_profile_set(POWER_PROFILE_POWER_SAVER);
    
    // Show notification to user
    notify_user("Battery low", "Please connect charger");
}

// Register handler
power_register_event_handler(POWER_EVENT_BATTERY_LOW,
                             battery_low_handler);
```

### Setting Power Profile

```c
// On AC power: use Balanced
if (ac_adapter_is_online()) {
    power_profile_set(POWER_PROFILE_BALANCED);
}
// On battery: use Power Saver if < 20%
else if (battery_get_percentage() < 20) {
    power_profile_set(POWER_PROFILE_POWER_SAVER);
}
```

## Integration with Device Model

Devices should implement power management callbacks:

```c
// Device power management operations
static const device_pm_ops_t my_device_pm_ops = {
    .suspend = my_device_suspend,
    .resume = my_device_resume,
    .runtime_suspend = my_device_runtime_suspend,
    .runtime_resume = my_device_runtime_resume,
};

// Register device
device_t* dev = device_alloc("my_device");
dev->pm_ops = &my_device_pm_ops;
device_register(dev);
```

During system suspend, the power management core will call:
1. `prepare()` - Prepare device for suspend
2. `suspend()` - Suspend device
3. **System enters sleep state**
4. **System wakes up**
5. `resume()` - Resume device
6. `complete()` - Complete resume

## Power States

| State | Name            | Power | Wake Time | RAM |
|-------|-----------------|-------|-----------|-----|
| S0    | Working         | Full  | N/A       | On  |
| S1    | Standby         | Low   | < 1s      | On  |
| S3    | Sleep (STR)     | Very Low | 2-3s   | On  |
| S4    | Hibernate (STD) | None  | 10-30s    | Off |
| S5    | Soft Off        | None  | Full Boot | Off |

## CPU Frequency Governors

| Governor      | Behavior                           | Use Case                    |
|---------------|------------------------------------|-----------------------------|
| performance   | Always max frequency               | Gaming, video editing       |
| powersave     | Always min frequency               | Maximum battery life        |
| ondemand      | Scale based on CPU load            | General use (recommended)   |
| conservative  | Like ondemand, slower transitions  | Gentle power saving         |
| userspace     | Application controlled             | Custom control needed       |

## Thermal Trip Points

| Trip Point | Temp  | Action                          |
|------------|-------|---------------------------------|
| Active     | 60°C  | Turn on fan                     |
| Passive    | 75°C  | CPU throttle to 80%             |
| Hot        | 85°C  | Max fan + throttle to 50%       |
| Critical   | 95°C  | Emergency shutdown              |

## Power Profiles Comparison

| Feature        | Performance | Balanced      | Power Saver   |
|----------------|-------------|---------------|---------------|
| CPU Governor   | performance | ondemand      | conservative  |
| CPU Max Freq   | 100%        | 100%          | 80%           |
| Screen Blank   | Never       | 5 min         | 2 min         |
| Screen Off     | Never       | 10 min        | 5 min         |
| Backlight      | 100%        | 100%          | 80%           |
| Sleep Timeout  | Never       | 15 min        | 5 min         |
| Bluetooth      | On          | On            | Off           |
| Battery Life   | 2-4 hours   | 5-8 hours     | 10+ hours     |
| Power Draw     | 15-25W      | 8-15W         | 5-10W         |

## Debugging

Enable verbose power management logging:

```c
// Set log level
power_set_log_level(LOG_DEBUG);

// Dump power info
power_print_info();

// Dump battery info
power_dump_battery_info();

// Dump thermal zones
power_dump_thermal_zones();

// Dump CPU info
power_dump_cpu_info();
```

## Build

To build the power management subsystem:

```bash
cd kernel/power
make
```

This will compile all power management objects:
- power.o
- battery.o
- cpufreq.o
- cpuidle.o
- thermal.o
- display.o
- profile.o
- events.o

These objects are then linked into the kernel.

## Testing

### Unit Testing

Test individual components:

```c
// Test battery reading
battery_info_t* bat = battery_get_info();
assert(bat != NULL);
assert(bat->percentage <= 100);

// Test CPU frequency
cpufreq_set_frequency(0, 2400);
uint32_t freq = cpufreq_get_frequency(0);
assert(freq == 2400);

// Test temperature
uint32_t temp = thermal_get_temperature("CPU");
assert(temp > 0 && temp < 100000);  // 0-100°C
```

### Integration Testing

Test full suspend/resume:

```c
void test_suspend_resume(void) {
    kprintf("Testing suspend/resume...\n");
    
    // Suspend
    int ret = power_suspend_to_ram();
    assert(ret == 0);
    
    // Resume happens here
    kprintf("Resumed successfully\n");
    
    // Verify devices resumed
    assert(device_pm_check_all_resumed());
}
```

### Hardware Testing

Test on real hardware:

1. **Battery test**: Unplug AC, monitor discharge rate
2. **Suspend test**: Suspend, wait 1 hour, resume
3. **Thermal test**: Run CPU stress test, check throttling
4. **Profile test**: Switch profiles, measure power draw
5. **Lid test**: Close/open lid, verify suspend

## Dependencies

The power management subsystem depends on:

- **ACPI** (`kernel/acpi/`): ACPI table parsing and sleep states
- **Device model** (`kernel/drivers/core/`): Device suspend/resume
- **Timer** (`kernel/core/timer/`): Timeouts and delays
- **Interrupts** (`kernel/core/irq/`): ACPI events

## Related Documentation

- [ACPI Specification](https://uefi.org/specifications)
- [Power Management Documentation](../../docs/POWER_MANAGEMENT.md)
- [Power Management Quick Reference](../../docs/POWER_MANAGEMENT_QUICK_REFERENCE.md)
- [Device Model Documentation](../drivers/core/README.md)

## TODOs

- [ ] ACPI AML interpreter for advanced features
- [ ] Full hibernation with disk image
- [ ] Wake source configuration via ACPI GPE
- [ ] GPU power management
- [ ] USB selective suspend
- [ ] PCIe ASPM

## Maintainer

**Power Management Engineer**
AutomationOS Team

For questions or issues, please check the documentation first:
- `docs/POWER_MANAGEMENT.md` - Complete documentation
- `docs/POWER_MANAGEMENT_QUICK_REFERENCE.md` - Quick reference
