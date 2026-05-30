# Power Management Quick Reference

Quick reference guide for AutomationOS power management.

## Common Operations

### Check Battery Status

```bash
# Get current battery percentage
powerstat | grep "Battery"

# Continuous monitoring
powerstat -c
```

### Suspend/Resume

```bash
# Suspend to RAM (S3)
echo mem > /sys/power/state

# Hibernate (S4)
echo disk > /sys/power/state
```

From code:
```c
power_suspend_to_ram();  // Suspend to RAM
power_hibernate();       // Hibernate
```

### Change Power Profile

```bash
# Via powerd
powerd --profile performance
powerd --profile balanced
powerd --profile powersaver
```

From code:
```c
power_profile_set(POWER_PROFILE_PERFORMANCE);
power_profile_set(POWER_PROFILE_BALANCED);
power_profile_set(POWER_PROFILE_POWER_SAVER);
```

### CPU Frequency Scaling

```bash
# Get current frequency
cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq

# Set governor
echo ondemand > /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor
```

From code:
```c
// Set governor
cpufreq_set_governor(0, CPUFREQ_GOVERNOR_ONDEMAND);

// Set frequency
cpufreq_set_frequency(0, 2400);  // 2.4 GHz

// Get current frequency
uint32_t freq = cpufreq_get_frequency(0);
```

### Display Power

```bash
# Blank screen
xset dpms force standby

# Turn off screen
xset dpms force off

# Set backlight (percentage)
echo 50 > /sys/class/backlight/intel_backlight/brightness
```

From code:
```c
display_blank();
display_poweroff();
backlight_set_brightness(50);  // 50%
```

## Power Events

### Battery Events

```c
void battery_handler(power_event_t event, void* data) {
    if (event == POWER_EVENT_BATTERY_LOW) {
        // Show low battery warning
    } else if (event == POWER_EVENT_BATTERY_CRITICAL) {
        // Critical battery, prepare to suspend
        power_suspend_to_ram();
    }
}

power_register_event_handler(POWER_EVENT_BATTERY_LOW, battery_handler);
power_register_event_handler(POWER_EVENT_BATTERY_CRITICAL, battery_handler);
```

### Lid Events

```c
void lid_handler(power_event_t event, void* data) {
    if (event == POWER_EVENT_LID_CLOSED) {
        if (!ac_adapter_is_online()) {
            // On battery, suspend when lid closes
            power_suspend_to_ram();
        }
    }
}

power_register_event_handler(POWER_EVENT_LID_CLOSED, lid_handler);
```

## Debugging

### ACPI Info

```c
// Print ACPI tables
acpi_dump_tables();

// Print ACPI status
acpi_print_info();

// Check if ACPI is enabled
if (acpi_is_enabled()) {
    printf("ACPI enabled\n");
}
```

### Power Statistics

```c
// Get power stats
power_stats_t* stats = power_get_stats();

printf("Uptime: %lu sec\n", stats->uptime_sec);
printf("On battery: %s\n", stats->on_battery ? "Yes" : "No");
printf("Battery: %u%%\n", stats->battery_percent);
printf("Power draw: %u W\n", stats->power_draw_watts);
```

### Battery Info

```c
battery_info_t* bat = battery_get_info();

printf("Battery: %u%% (%s)\n",
       bat->percentage,
       bat->state == BATTERY_STATE_CHARGING ? "Charging" :
       bat->state == BATTERY_STATE_DISCHARGING ? "Discharging" : "Full");

printf("Voltage: %u mV\n", bat->voltage_mv);
printf("Current: %d mA\n", bat->current_ma);
printf("Temperature: %u.%u°C\n",
       bat->temperature / 10, bat->temperature % 10);
```

### Thermal Info

```c
// Get CPU temperature
uint32_t temp = thermal_get_temperature("CPU");
printf("CPU: %u.%03u°C\n", temp / 1000, temp % 1000);

// Dump all thermal zones
power_dump_thermal_zones();
```

## Syscalls

### Power Management Syscalls

```c
#define SYS_POWER_GET_STATE     300
#define SYS_POWER_SET_PROFILE   301
#define SYS_POWER_SUSPEND       302
#define SYS_POWER_HIBERNATE     303
#define SYS_BATTERY_GET_INFO    304
#define SYS_THERMAL_GET_TEMP    305
#define SYS_CPUFREQ_SET_FREQ    306
#define SYS_CPUFREQ_GET_FREQ    307
#define SYS_CPUFREQ_SET_GOV     308
#define SYS_BACKLIGHT_SET       309
#define SYS_BACKLIGHT_GET       310
```

Usage:
```c
// Get battery info
battery_info_t info;
syscall(SYS_BATTERY_GET_INFO, &info);

// Suspend to RAM
syscall(SYS_POWER_SUSPEND, POWER_STATE_S3);

// Set CPU frequency
syscall(SYS_CPUFREQ_SET_FREQ, 0, 2400);  // CPU 0, 2.4 GHz
```

## Power Profiles

### Performance Profile

**Use when**: Gaming, video editing, compilation, benchmarks
**Battery life**: 2-4 hours
**Power draw**: 15-25W

```c
power_profile_set(POWER_PROFILE_PERFORMANCE);
```

Settings:
- CPU: Always max frequency
- Display: Never blank
- Backlight: 100%
- Sleep: Disabled

### Balanced Profile (Default)

**Use when**: General use, web browsing, office work
**Battery life**: 5-8 hours
**Power draw**: 8-15W

```c
power_profile_set(POWER_PROFILE_BALANCED);
```

Settings:
- CPU: Scale based on load
- Display: Blank after 5 min
- Backlight: 100%
- Sleep: After 15 min

### Power Saver Profile

**Use when**: On battery, low battery, maximum battery life
**Battery life**: 10+ hours
**Power draw**: 5-10W

```c
power_profile_set(POWER_PROFILE_POWER_SAVER);
```

Settings:
- CPU: Conservative scaling
- Display: Blank after 2 min
- Backlight: 80%
- Sleep: After 5 min
- Bluetooth: Disabled

## Governors

### performance
Always max frequency. Best for latency-sensitive workloads.

```c
cpufreq_set_governor(cpu, CPUFREQ_GOVERNOR_PERFORMANCE);
```

### powersave
Always min frequency. Maximum power saving but slowest.

```c
cpufreq_set_governor(cpu, CPUFREQ_GOVERNOR_POWERSAVE);
```

### ondemand
Scale based on load. Best general-purpose governor.

```c
cpufreq_set_governor(cpu, CPUFREQ_GOVERNOR_ONDEMAND);
```

### conservative
Like ondemand but slower frequency changes.

```c
cpufreq_set_governor(cpu, CPUFREQ_GOVERNOR_CONSERVATIVE);
```

## Sleep States

| State | Name            | Power | Wake Time | RAM |
|-------|-----------------|-------|-----------|-----|
| S0    | Working         | Full  | N/A       | On  |
| S1    | Standby         | Low   | < 1s      | On  |
| S3    | Sleep (STR)     | Very Low | 2-3s   | On  |
| S4    | Hibernate (STD) | None  | 10-30s    | Off |
| S5    | Soft Off        | None  | Full Boot | Off |

**STR** = Suspend to RAM
**STD** = Suspend to Disk

## Thermal Trip Points

| Trip Point | Temperature | Action              |
|------------|-------------|---------------------|
| Active     | 60°C        | Turn on fan         |
| Passive    | 75°C        | CPU throttle (80%)  |
| Hot        | 85°C        | Max fan + throttle (50%) |
| Critical   | 95°C        | Emergency shutdown  |

## Power Estimation Formulas

### Battery Life

```
Battery Life (hours) = Remaining mAh / Current draw (mA)
```

Example:
```
4250 mAh / 1500 mA = 2.83 hours
```

### Power Draw

```
Power (W) = Voltage (V) × Current (A)
```

Example:
```
11.4V × 1.5A = 17.1W
```

### Time to Full

```
Time to Full (hours) = (Capacity - Remaining) / Charge current
```

Example:
```
(5000 - 4250) mAh / 2000 mA = 0.375 hours (22.5 min)
```

## Troubleshooting

### System won't suspend

1. Check ACPI is enabled:
   ```c
   if (!acpi_is_enabled()) {
       kprintf("ACPI not enabled!\n");
   }
   ```

2. Check device suspend:
   ```c
   // Try suspending devices individually
   device_pm_suspend(dev);
   ```

3. Check kernel logs for errors

### System won't resume

1. Check wake sources are configured
2. Verify BIOS/UEFI settings allow wake
3. Check power button is enabled as wake source

### Battery not detected

1. Check ACPI battery device exists
2. Verify battery is properly seated
3. Try reinitializing:
   ```c
   battery_init();
   ```

### CPU frequency stuck

1. Check governor is correct:
   ```c
   cpufreq_get_governor(0);
   ```

2. Try different governor:
   ```c
   cpufreq_set_governor(0, CPUFREQ_GOVERNOR_ONDEMAND);
   ```

3. Check BIOS/UEFI CPU settings

### Thermal throttling too aggressive

1. Clean cooling system (dust)
2. Check thermal paste
3. Adjust trip points in code:
   ```c
   zone->trip_point_passive = 80000;  // 80°C instead of 75°C
   ```

## Best Practices

### Battery Longevity

1. **Avoid deep discharge**: Don't let battery drop below 20%
2. **Avoid full charge**: Don't keep at 100% constantly
3. **Moderate temperature**: Keep system cool (< 40°C)
4. **Calibrate monthly**: Full discharge/charge cycle once per month

### Power Efficiency

1. **Use appropriate profile**: Balanced for general use
2. **Close unused apps**: Each process consumes power
3. **Reduce brightness**: Display is major power consumer
4. **Disable unused hardware**: Bluetooth, WiFi when not needed
5. **Use SSD over HDD**: SSDs use less power

### Suspend/Resume

1. **Prefer S3 over S4**: Faster and less wear on SSD
2. **Save work before suspend**: In case of issues
3. **Close VMs before suspend**: They may not resume properly
4. **Unplug USB devices**: Some may prevent suspend

---

**Quick Tip**: Press **Super+P** to access quick power settings from desktop!
