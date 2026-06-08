/*
 * Thermal Management
 * Temperature monitoring, cooling, and throttling
 *
 * T410 THERMAL SAFETY
 * -------------------
 * The ThinkPad T410's Core i5 (Arrandale / Westmere) has a Digital Thermal
 * Sensor (DTS) readable via MSR 0x19C (IA32_THERM_STATUS).  We use it to:
 *   1. Read real CPU temperature (TjMax - thermal margin)
 *   2. Detect if the CPU is hardware-throttling (PROCHOT#)
 *   3. Log warnings at 85 C and 90 C; critical at 95 C
 *   4. Provide a once-per-second thermal_safety_tick() hook for pit.c
 *
 * Fan control is NOT attempted here -- the T410 fan is managed by the
 * Embedded Controller (EC register 0x2F), which requires an ACPI EC driver
 * we don't have yet.  The safety net is detection + warning only.
 */

#include "../include/power.h"
#include "../include/kernel.h"
#include "../include/x86_64.h"
#include "../include/io.h"
#include "../include/string.h"

extern power_global_state_t power_global;

/* ---- MSR / CPUID constants for Digital Thermal Sensor ---- */

#define MSR_IA32_THERM_STATUS       0x19C
#define MSR_IA32_TEMPERATURE_TARGET 0x1A2

/*
 * IA32_THERM_STATUS layout (read-only status bits):
 *   Bit  0   : Thermal Status             (1 = PROCHOT# / throttling active)
 *   Bit  1   : Thermal Status Log         (sticky; 1 = was ever throttled)
 *   Bits 22:16: Digital Readout            (distance below TjMax in degrees C)
 *   Bit  31  : Reading Valid               (1 = readout is valid)
 *
 * Temperature = TjMax - readout.
 * TjMax is 100 C for Arrandale i5 (also readable from MSR 0x1A2 bits 23:16
 * on CPUs that support it, but Arrandale may not expose that MSR reliably,
 * so we default to 100).
 */
#define THERM_STATUS_THROTTLING     (1U << 0)
#define THERM_STATUS_THROTTLE_LOG   (1U << 1)
#define THERM_STATUS_READOUT_MASK   0x007F0000U   /* bits 22:16 */
#define THERM_STATUS_READOUT_SHIFT  16
#define THERM_STATUS_VALID          (1U << 31)

/* Default TjMax for Arrandale / Westmere i5.  If the CPU exposes
 * MSR_IA32_TEMPERATURE_TARGET we read from it; otherwise use this. */
#define TJMAX_DEFAULT_C  100

/* Thermal thresholds (degrees C, NOT millidegrees, for the safety tick) */
#define THERMAL_WARN_C     85
#define THERMAL_HIGH_C     90
#define THERMAL_CRITICAL_C 95

/* Runtime state for the DTS reader */
static bool     dts_available = false;   /* CPU has Digital Thermal Sensor */
static uint32_t tjmax_c       = TJMAX_DEFAULT_C;
static bool     throttle_warned = false; /* rate-limit the throttle message */

// Thermal zone names
static const char* thermal_zone_names[] = {
    "CPU",
    "GPU",
    "ACPI",
    "System",
};

#define NUM_THERMAL_ZONES (sizeof(thermal_zone_names) / sizeof(const char*))

// Temperature thresholds (Celsius * 1000) -- used by the trip-point framework
#define TEMP_ACTIVE     60000   // 60 C - Turn on fan
#define TEMP_PASSIVE    75000   // 75 C - Start throttling
#define TEMP_HOT        85000   // 85 C - Urgent
#define TEMP_CRITICAL   95000   // 95 C - Emergency shutdown

/*
 * Probe for DTS support via CPUID.
 *
 * CPUID leaf 6 (Thermal and Power Management), EAX bit 0 = Digital Thermal
 * Sensor available.  Arrandale supports leaf 6 (max leaf >= 0xB).
 */
static void thermal_detect_dts(void) {
    uint32_t max_leaf;
    asm volatile("cpuid" : "=a"(max_leaf) : "a"(0) : "ebx", "ecx", "edx");
    if (max_leaf < 6) {
        kprintf("[THERMAL] CPUID max leaf %u < 6 -- no DTS support\n", max_leaf);
        return;
    }

    uint32_t eax, ebx, ecx, edx;
    asm volatile("cpuid"
                 : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                 : "a"(6), "c"(0));

    if (eax & 1) {
        dts_available = true;
        kprintf("[THERMAL] Digital Thermal Sensor detected (CPUID.06H:EAX[0]=1)\n");
    } else {
        kprintf("[THERMAL] No DTS (CPUID.06H:EAX[0]=0) -- temperature unavailable\n");
        return;
    }

    /*
     * Try to read TjMax from MSR_IA32_TEMPERATURE_TARGET (bits 23:16).
     * Not all Arrandale steppings expose this MSR; if the read #GPs (we
     * can't catch that without an exception handler for rdmsr), we just
     * keep the default.  In practice Arrandale DOES expose it, but we
     * guard with a CPUID family/model check to be safe.
     *
     * Family 6, Model 0x25 = Arrandale (Westmere mobile).
     * Family 6, Model 0x2C = Westmere-EP.
     * Both support MSR 0x1A2.
     */
    asm volatile("cpuid"
                 : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                 : "a"(1), "c"(0));
    uint32_t family = (eax >> 8) & 0xF;
    uint32_t model  = ((eax >> 4) & 0xF) | (((eax >> 16) & 0xF) << 4);
    kprintf("[THERMAL] CPU family=%u model=0x%x\n", family, model);

    /* Read MSR 0x1A2 only on known-safe families. Family 6 covers all
     * Core / Core i-series parts that have DTS. */
    if (family == 6) {
        uint64_t target = rdmsr(MSR_IA32_TEMPERATURE_TARGET);
        uint32_t read_tj = (target >> 16) & 0xFF;
        if (read_tj >= 80 && read_tj <= 120) {
            tjmax_c = read_tj;
            kprintf("[THERMAL] TjMax from MSR 0x1A2 = %u C\n", tjmax_c);
        } else {
            kprintf("[THERMAL] MSR 0x1A2 readout %u out of range, using default %u C\n",
                    read_tj, tjmax_c);
        }
    }
}

/**
 * Read CPU temperature from MSR (Model Specific Register)
 *
 * Returns temperature in millidegrees Celsius (Celsius * 1000) to match the
 * existing thermal_zone_t convention.  Returns 0 if DTS is not available or
 * the readout is marked invalid.
 */
static uint32_t read_cpu_temperature(void) {
    if (!dts_available)
        return 0;

    uint64_t raw = rdmsr(MSR_IA32_THERM_STATUS);
    uint32_t status = (uint32_t)raw;

    /* Bit 31 = reading valid.  Some emulators (QEMU without KVM) don't
     * set this; tolerate it if the readout field is non-zero. */
    uint32_t readout = (status & THERM_STATUS_READOUT_MASK) >> THERM_STATUS_READOUT_SHIFT;

    if (readout == 0 && !(status & THERM_STATUS_VALID))
        return 0;   /* genuinely unavailable */

    uint32_t temp_c = (readout <= tjmax_c) ? (tjmax_c - readout) : 0;
    return temp_c * 1000;   /* millidegrees */
}

/**
 * Read GPU temperature
 */
static uint32_t read_gpu_temperature(void) {
    // TODO: Read from GPU registers
    return 50000;  // 50°C
}

/**
 * Read ACPI thermal zone temperature
 */
static uint32_t read_acpi_temperature(void) {
    // TODO: Read from ACPI thermal zone
    return 40000;  // 40°C
}

/**
 * Set fan speed (0-100%)
 */
static int set_fan_speed(uint32_t percentage) {
    // TODO: Control fan via ACPI or EC (Embedded Controller)
    kprintf("[THERMAL] Setting fan speed to %u%%\n", percentage);
    return 0;
}

/**
 * Initialize thermal management
 */
int thermal_init(void) {
    kprintf("[THERMAL] Initializing thermal management...\n");

    /* Probe for the Digital Thermal Sensor (T410 safety) */
    thermal_detect_dts();

    // Allocate thermal zones
    power_global.num_thermal_zones = NUM_THERMAL_ZONES;
    power_global.thermal_zones = kmalloc(sizeof(thermal_zone_t) * NUM_THERMAL_ZONES);
    if (!power_global.thermal_zones) {
        kprintf("[THERMAL] ERROR: Failed to allocate memory\n");
        return -1;
    }

    // Initialize thermal zones
    for (uint32_t i = 0; i < NUM_THERMAL_ZONES; i++) {
        thermal_zone_t* zone = &power_global.thermal_zones[i];

        strncpy(zone->name, thermal_zone_names[i], sizeof(zone->name) - 1);
        zone->name[sizeof(zone->name) - 1] = '\0';
        zone->temperature = 0;

        // Set trip points
        zone->trip_point_active = TEMP_ACTIVE;
        zone->trip_point_passive = TEMP_PASSIVE;
        zone->trip_point_hot = TEMP_HOT;
        zone->trip_point_critical = TEMP_CRITICAL;

        zone->cooling_state = 0;
        zone->max_cooling_state = 10;

        kprintf("[THERMAL] Zone '%s': Trip points: %u/%u/%u/%u°C\n",
                zone->name,
                zone->trip_point_active / 1000,
                zone->trip_point_passive / 1000,
                zone->trip_point_hot / 1000,
                zone->trip_point_critical / 1000);
    }

    // Read initial temperatures
    power_global.thermal_zones[0].temperature = read_cpu_temperature();
    power_global.thermal_zones[1].temperature = read_gpu_temperature();
    power_global.thermal_zones[2].temperature = read_acpi_temperature();
    power_global.thermal_zones[3].temperature = (read_cpu_temperature() + read_gpu_temperature()) / 2;

    return 0;
}

/**
 * Get thermal zone by name
 */
thermal_zone_t* thermal_get_zone(const char* name) {
    for (uint32_t i = 0; i < power_global.num_thermal_zones; i++) {
        if (strcmp(power_global.thermal_zones[i].name, name) == 0) {
            return &power_global.thermal_zones[i];
        }
    }
    return NULL;
}

/**
 * Get temperature of thermal zone
 */
uint32_t thermal_get_temperature(const char* name) {
    thermal_zone_t* zone = thermal_get_zone(name);
    if (!zone) {
        return 0;
    }

    // Update temperature
    if (strcmp(name, "CPU") == 0) {
        zone->temperature = read_cpu_temperature();
    } else if (strcmp(name, "GPU") == 0) {
        zone->temperature = read_gpu_temperature();
    } else if (strcmp(name, "ACPI") == 0) {
        zone->temperature = read_acpi_temperature();
    }

    return zone->temperature;
}

/**
 * Set cooling state
 */
int thermal_set_cooling_state(const char* name, uint32_t state) {
    thermal_zone_t* zone = thermal_get_zone(name);
    if (!zone) {
        return -1;
    }

    if (state > zone->max_cooling_state) {
        state = zone->max_cooling_state;
    }

    zone->cooling_state = state;

    // Apply cooling
    uint32_t fan_speed = (state * 100) / zone->max_cooling_state;
    set_fan_speed(fan_speed);

    return 0;
}

/**
 * Throttle CPU to reduce temperature
 */
int thermal_throttle_cpu(uint32_t cpu, uint32_t percentage) {
    if (cpu >= power_global.num_cpus || percentage > 100) {
        return -1;
    }

    kprintf("[THERMAL] Throttling CPU %u to %u%%\n", cpu, percentage);

    // Reduce max frequency
    cpufreq_policy_t* policy = cpufreq_get_policy(cpu);
    if (!policy) {
        return -1;
    }

    uint32_t throttled_freq = (policy->max_freq_mhz * percentage) / 100;
    cpufreq_set_frequency(cpu, throttled_freq);

    return 0;
}

/**
 * Check trip points and take action
 */
void thermal_check_trip_points(thermal_zone_t* zone) {
    if (!zone) {
        return;
    }

    uint32_t temp = zone->temperature;

    // Critical: Emergency shutdown
    if (temp >= zone->trip_point_critical) {
        kprintf("[THERMAL] CRITICAL: %s temperature %u°C! Shutting down...\n",
                zone->name, temp / 1000);
        power_notify_event(POWER_EVENT_THERMAL_CRITICAL, zone);

        // Emergency shutdown
        timer_sleep(5000);  // Give 5 seconds warning
        acpi_poweroff();
    }

    // Hot: Urgent cooling
    else if (temp >= zone->trip_point_hot) {
        kprintf("[THERMAL] HOT: %s temperature %u°C\n", zone->name, temp / 1000);
        power_notify_event(POWER_EVENT_THERMAL_WARNING, zone);

        // Max cooling
        thermal_set_cooling_state(zone->name, zone->max_cooling_state);

        // Aggressive throttling
        if (strcmp(zone->name, "CPU") == 0) {
            for (uint32_t i = 0; i < power_global.num_cpus; i++) {
                thermal_throttle_cpu(i, 50);  // Throttle to 50%
            }
        }
    }

    // Passive: Start throttling
    else if (temp >= zone->trip_point_passive) {
        if (zone->cooling_state < zone->max_cooling_state * 7 / 10) {
            kprintf("[THERMAL] Passive cooling for %s (%u°C)\n",
                    zone->name, temp / 1000);

            // Increase cooling
            thermal_set_cooling_state(zone->name, zone->max_cooling_state * 7 / 10);

            // Light throttling
            if (strcmp(zone->name, "CPU") == 0) {
                for (uint32_t i = 0; i < power_global.num_cpus; i++) {
                    thermal_throttle_cpu(i, 80);  // Throttle to 80%
                }
            }
        }
    }

    // Active: Turn on fan
    else if (temp >= zone->trip_point_active) {
        if (zone->cooling_state < zone->max_cooling_state * 4 / 10) {
            kprintf("[THERMAL] Active cooling for %s (%u°C)\n",
                    zone->name, temp / 1000);

            // Moderate cooling
            thermal_set_cooling_state(zone->name, zone->max_cooling_state * 4 / 10);
        }
    }

    // Normal: Reduce cooling
    else if (temp < zone->trip_point_active - 5000) {  // 5°C hysteresis
        if (zone->cooling_state > 0) {
            // Reduce cooling gradually
            if (zone->cooling_state > 2) {
                zone->cooling_state -= 2;
            } else {
                zone->cooling_state = 0;
            }

            uint32_t fan_speed = (zone->cooling_state * 100) / zone->max_cooling_state;
            set_fan_speed(fan_speed);

            // Remove throttling
            if (strcmp(zone->name, "CPU") == 0) {
                for (uint32_t i = 0; i < power_global.num_cpus; i++) {
                    thermal_throttle_cpu(i, 100);  // Full speed
                }
            }
        }
    }
}

/**
 * Thermal monitoring thread (called periodically)
 */
void thermal_monitor(void) {
    // Update all thermal zones
    for (uint32_t i = 0; i < power_global.num_thermal_zones; i++) {
        thermal_zone_t* zone = &power_global.thermal_zones[i];

        // Read temperature
        thermal_get_temperature(zone->name);

        // Check trip points
        thermal_check_trip_points(zone);
    }
}

/* ================================================================
 * T410 THERMAL SAFETY TICK
 * ================================================================
 * Called from pit.c timer_handler() every 1000 ticks (once per second at
 * 1000 Hz).  This is the lightweight safety net: read the DTS, check for
 * throttling, and emit log warnings.  It does NOT attempt active cooling
 * (fan / frequency reduction) -- that's thermal_check_trip_points()'s job.
 *
 * This function runs in IRQ context with interrupts disabled, so it must
 * be fast and non-blocking (rdmsr + a few comparisons + conditional kprintf).
 */

void thermal_safety_tick(void) {
    if (!dts_available)
        return;

    uint64_t raw = rdmsr(MSR_IA32_THERM_STATUS);
    uint32_t status = (uint32_t)raw;

    /* --- 1. Detect hardware throttling (PROCHOT#) --- */
    if (status & THERM_STATUS_THROTTLING) {
        if (!throttle_warned) {
            kprintf("[THERMAL] WARNING: CPU is hardware-throttling (PROCHOT# active)\n");
            throttle_warned = true;
        }
    } else {
        throttle_warned = false;  /* reset so we warn again next time */
    }

    /* --- 2. Read temperature --- */
    uint32_t readout = (status & THERM_STATUS_READOUT_MASK) >> THERM_STATUS_READOUT_SHIFT;
    if (readout == 0 && !(status & THERM_STATUS_VALID))
        return;   /* no valid reading */

    uint32_t temp_c = (readout <= tjmax_c) ? (tjmax_c - readout) : 0;

    /* --- 3. Graded warnings ---
     * 90+ C : logged every second (urgent)
     * 85-89 : logged once per crossing (one-shot with hysteresis)
     * < 85  : quiet; resets the one-shot so we warn again if temp climbs */
    static bool warm_warned = false;

    if (temp_c >= THERMAL_CRITICAL_C) {
        kprintf("[THERMAL] *** CRITICAL: CPU temperature %u C >= %u C! ***\n",
                temp_c, THERMAL_CRITICAL_C);
        /* The full trip-point framework (thermal_check_trip_points) handles
         * shutdown; we just make sure the message is visible. */
    } else if (temp_c >= THERMAL_HIGH_C) {
        kprintf("[THERMAL] HIGH: CPU temperature %u C (TjMax=%u C, margin=%u C)\n",
                temp_c, tjmax_c, tjmax_c - temp_c);
        warm_warned = true;
    } else if (temp_c >= THERMAL_WARN_C) {
        if (!warm_warned) {
            kprintf("[THERMAL] WARM: CPU temperature %u C -- approaching thermal limit\n",
                    temp_c);
            warm_warned = true;
        }
    } else {
        warm_warned = false;  /* below threshold -- re-arm the one-shot */
    }

    /* Update the CPU thermal zone if the power subsystem is initialized,
     * so thermal_check_trip_points() and userspace tools see real data. */
    if (power_global.thermal_zones && power_global.num_thermal_zones > 0) {
        power_global.thermal_zones[0].temperature = temp_c * 1000;  /* millidegrees */
    }
}
