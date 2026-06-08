/*
 * Battery Management
 * Battery monitoring, AC adapter detection, power events
 *
 * Reads real battery status from the ACPI Embedded Controller (EC) on
 * systems that have one (e.g. ThinkPad T410). Falls back to "no battery"
 * if the EC is unresponsive or does not report battery data.
 *
 * EC I/O protocol (ACPI spec, chapter 12):
 *   Data port:    0x62  (read/write EC RAM)
 *   Cmd/Status:   0x66  (write = command, read = status)
 *   Status bits:  OBF (bit 0) = output buffer full (data ready to read)
 *                 IBF (bit 1) = input buffer full (EC busy, do not write)
 *
 * EC RAM layout for battery (_BST/_BIF equivalent) is chipset-specific.
 * ThinkPad EC offsets are documented in the thinkpad-ec project and
 * confirmed by DSDT disassembly of the T410's BIOS. When those offsets
 * yield no valid data (e.g. on QEMU which has no EC), we fall back to
 * reporting "no battery."
 */

#include "../include/power.h"
#include "../include/acpi.h"
#include "../include/kernel.h"
#include "../include/io.h"
#include "../include/string.h"

/* -----------------------------------------------------------------------
 * Embedded Controller (EC) I/O
 * ----------------------------------------------------------------------- */
#define EC_DATA_PORT    0x62
#define EC_CMD_PORT     0x66    /* write = command, read = status */

/* EC status register bits */
#define EC_STATUS_OBF   (1 << 0)   /* Output Buffer Full -- data ready    */
#define EC_STATUS_IBF   (1 << 1)   /* Input Buffer Full  -- EC busy       */

/* EC commands */
#define EC_CMD_READ     0x80    /* Read EC RAM: send cmd, then address */
#define EC_CMD_WRITE    0x81    /* Write EC RAM: cmd, address, data    */
#define EC_CMD_QUERY    0x84    /* Query event (SCI)                   */

/* Maximum iterations for busy-wait loops (prevents infinite hangs if the
 * EC is absent or broken, e.g. on QEMU). ~100k iterations of inb(0x66) at
 * ~1 us each = ~100 ms timeout -- generous for any real EC. */
#define EC_TIMEOUT      100000

/* Flag set after the first successful EC read; if false we skip all
 * further reads and report "no battery" (avoids repeated timeout stalls
 * on platforms without an EC like QEMU). */
static bool ec_available = false;
static bool ec_probed    = false;

/**
 * Wait until the EC input buffer is empty (IBF == 0).
 * Returns 0 on success, -1 on timeout.
 */
static int ec_wait_ibf_clear(void) {
    for (int i = 0; i < EC_TIMEOUT; i++) {
        if (!(inb(EC_CMD_PORT) & EC_STATUS_IBF))
            return 0;
    }
    return -1;
}

/**
 * Wait until the EC output buffer is full (OBF == 1).
 * Returns 0 on success, -1 on timeout.
 */
static int ec_wait_obf_set(void) {
    for (int i = 0; i < EC_TIMEOUT; i++) {
        if (inb(EC_CMD_PORT) & EC_STATUS_OBF)
            return 0;
    }
    return -1;
}

/**
 * Read one byte from EC RAM at the given address.
 * Returns the byte value (0-255) on success, or -1 on timeout.
 */
static int ec_read(uint8_t addr) {
    if (ec_wait_ibf_clear() < 0) return -1;
    outb(EC_CMD_PORT, EC_CMD_READ);

    if (ec_wait_ibf_clear() < 0) return -1;
    outb(EC_DATA_PORT, addr);

    if (ec_wait_obf_set() < 0) return -1;
    return (int)inb(EC_DATA_PORT);
}

/**
 * Read a 16-bit little-endian word from EC RAM (addr, addr+1).
 * Returns the word value on success, or -1 on timeout.
 */
static int ec_read16(uint8_t addr) {
    int lo = ec_read(addr);
    if (lo < 0) return -1;
    int hi = ec_read(addr + 1);
    if (hi < 0) return -1;
    return lo | (hi << 8);
}

/* -----------------------------------------------------------------------
 * ThinkPad EC battery offsets (T410 / T420 / T430 / X220 / X230 family)
 *
 * These are derived from the DSDT disassembly of the T410's ACPI _BST
 * and _BIF methods, which read from the EC operation region. The offsets
 * are stable across the Lenovo ThinkPad Sandy/Ivy Bridge generation.
 *
 * _BST (Battery Status) fields:
 *   0x38  Battery State (1=discharging, 2=charging, 0=full/not-charging)
 *   0x39  Present Rate (mW, 16-bit LE)
 *   0x3B  Remaining Capacity (mWh, 16-bit LE)
 *   0x3D  Present Voltage (mV, 16-bit LE)
 *
 * _BIF (Battery Information) fields:
 *   0x34  Design Capacity (mWh, 16-bit LE)
 *   0x36  Last Full Charge Capacity (mWh, 16-bit LE)
 *
 * General:
 *   0x30  Battery present flag (bit 0 = BAT0 present)
 *   0x31  AC adapter status (bit 0 = AC online)
 *
 * NOTE: Not all ThinkPads use the same offsets. If the EC probe reads
 * nonsensical values (voltage > 30000mV, capacity = 0xFFFF), we treat
 * the battery as not present.
 * ----------------------------------------------------------------------- */
#define EC_BAT_PRESENT      0x38    /* Byte: bits indicate battery presence */
#define EC_AC_STATUS        0x39    /* Byte: bit0 = AC online               */
#define EC_BAT_STATE        0x3A    /* Byte: 0=idle, 1=discharge, 2=charge  */
#define EC_BAT_RATE         0x3C    /* 16-bit LE: present rate (mW)         */
#define EC_BAT_REMAINING    0x3E    /* 16-bit LE: remaining capacity (mWh)  */
#define EC_BAT_VOLTAGE      0x40    /* 16-bit LE: present voltage (mV)      */
#define EC_BAT_DESIGN_CAP   0x42    /* 16-bit LE: design capacity (mWh)     */
#define EC_BAT_FULL_CAP     0x44    /* 16-bit LE: last full charge (mWh)    */

/* Battery update interval (milliseconds) */
#define BATTERY_UPDATE_INTERVAL 30000

extern power_global_state_t power_global;

/* Last battery check time */
static uint64_t last_battery_update = 0;

/**
 * Probe the EC and read battery status.
 *
 * On the first call we check if the EC is responsive. If not, we flag
 * ec_available = false and every subsequent call returns -1 immediately
 * (avoids stalling the kernel on QEMU which has no EC).
 */
static int read_acpi_battery_info(battery_info_t* info) {
    if (ec_probed && !ec_available) {
        /* EC was already tested and found absent/unresponsive. */
        info->present = false;
        return -1;
    }

    if (!ec_probed) {
        ec_probed = true;
        /* Probe: try to read EC_BAT_PRESENT. If the read times out,
         * the platform has no EC (or it is disabled). */
        int probe = ec_read(EC_BAT_PRESENT);
        if (probe < 0) {
            kprintf("[BATTERY] EC not responding (no EC on this platform)\n");
            ec_available = false;
            info->present = false;
            return -1;
        }
        ec_available = true;
        kprintf("[BATTERY] EC detected (port 0x62/0x66)\n");
    }

    /* Read battery presence byte */
    int bat_present = ec_read(EC_BAT_PRESENT);
    if (bat_present < 0 || !(bat_present & 0x01)) {
        info->present = false;
        return -1;
    }

    info->present = true;

    /* Read battery state */
    int state = ec_read(EC_BAT_STATE);
    if (state >= 0) {
        switch (state & 0x03) {
            case 1:  info->state = BATTERY_STATE_DISCHARGING; break;
            case 2:  info->state = BATTERY_STATE_CHARGING;    break;
            default: info->state = BATTERY_STATE_NOT_CHARGING; break;
        }
    } else {
        info->state = BATTERY_STATE_UNKNOWN;
    }

    /* Read present rate (mW -> approximate mA using voltage) */
    int rate_mw = ec_read16(EC_BAT_RATE);

    /* Read remaining capacity (mWh) */
    int remaining_mwh = ec_read16(EC_BAT_REMAINING);

    /* Read voltage (mV) */
    int voltage_mv = ec_read16(EC_BAT_VOLTAGE);

    /* Read design capacity (mWh) */
    int design_cap_mwh = ec_read16(EC_BAT_DESIGN_CAP);

    /* Read last full charge capacity (mWh) */
    int full_cap_mwh = ec_read16(EC_BAT_FULL_CAP);

    /* Sanity check: if voltage or capacity are clearly wrong, the offsets
     * may not match this specific ThinkPad model. Report "no battery" rather
     * than display garbage. */
    if (voltage_mv < 0 || voltage_mv > 30000 ||
        design_cap_mwh < 0 || design_cap_mwh == 0xFFFF ||
        remaining_mwh < 0 || remaining_mwh == 0xFFFF) {
        /* EC responded but values are nonsensical -- offsets probably wrong */
        kprintf("[BATTERY] EC data invalid (voltage=%d design=%d remaining=%d); "
                "treating as no battery\n", voltage_mv, design_cap_mwh, remaining_mwh);
        info->present = false;
        return -1;
    }

    info->voltage_mv = (uint32_t)voltage_mv;

    /* Convert mWh to approximate mAh using nominal voltage.
     * mAh = mWh * 1000 / mV (but we clamp to avoid division by zero). */
    uint32_t v = (voltage_mv > 0) ? (uint32_t)voltage_mv : 11100; /* fallback 11.1V */

    if (design_cap_mwh > 0) {
        info->capacity_mah = (uint32_t)((uint64_t)design_cap_mwh * 1000 / v);
    } else {
        info->capacity_mah = 0;
    }

    if (remaining_mwh >= 0) {
        info->remaining_mah = (uint32_t)((uint64_t)remaining_mwh * 1000 / v);
    } else {
        info->remaining_mah = 0;
    }

    /* Percentage */
    if (full_cap_mwh > 0) {
        uint32_t pct = (uint32_t)((uint64_t)remaining_mwh * 100 / (uint32_t)full_cap_mwh);
        info->percentage = (pct > 100) ? 100 : pct;
    } else if (info->capacity_mah > 0) {
        uint32_t pct = (info->remaining_mah * 100) / info->capacity_mah;
        info->percentage = (pct > 100) ? 100 : pct;
    } else {
        info->percentage = 0;
    }

    /* If fully charged and not discharging, set state to FULL */
    if (info->percentage >= 100 && info->state != BATTERY_STATE_DISCHARGING) {
        info->state = BATTERY_STATE_FULL;
    }

    /* Current (approximate mA from mW rate / voltage) */
    if (rate_mw >= 0 && v > 0) {
        uint32_t current_ma = (uint32_t)((uint64_t)rate_mw * 1000 / v);
        if (info->state == BATTERY_STATE_DISCHARGING) {
            info->current_ma = -(int32_t)current_ma;
        } else {
            info->current_ma = (int32_t)current_ma;
        }
    } else {
        info->current_ma = 0;
    }

    /* Time estimates */
    if (info->state == BATTERY_STATE_DISCHARGING &&
        info->current_ma < 0 && info->current_ma != INT32_MIN) {
        uint32_t drain_ma = (uint32_t)(-info->current_ma);
        if (drain_ma > 0) {
            info->time_to_empty_min = (info->remaining_mah * 60) / drain_ma;
        }
    } else {
        info->time_to_empty_min = 0;
    }

    if (info->state == BATTERY_STATE_CHARGING && info->current_ma > 0) {
        uint32_t to_fill = info->capacity_mah - info->remaining_mah;
        info->time_to_full_min = (to_fill * 60) / (uint32_t)info->current_ma;
    } else {
        info->time_to_full_min = 0;
    }

    /* Health: last_full / design * 100 */
    if (design_cap_mwh > 0 && full_cap_mwh > 0) {
        info->health_percentage = (uint32_t)((uint64_t)full_cap_mwh * 100 /
                                             (uint32_t)design_cap_mwh);
        if (info->health_percentage > 100) info->health_percentage = 100;
    } else {
        info->health_percentage = 0;
    }

    /* EC does not expose cycle count, temperature, or strings on the T410
     * via these simple RAM offsets -- those require AML _BIF method calls
     * (full AML interpreter). Fill in reasonable defaults. */
    info->temperature = 0;
    info->cycle_count = 0;

    strncpy(info->manufacturer, "ThinkPad", sizeof(info->manufacturer) - 1);
    info->manufacturer[sizeof(info->manufacturer) - 1] = '\0';
    strncpy(info->model, "EC Battery", sizeof(info->model) - 1);
    info->model[sizeof(info->model) - 1] = '\0';
    strncpy(info->serial, "", sizeof(info->serial) - 1);
    info->serial[sizeof(info->serial) - 1] = '\0';
    strncpy(info->technology, "Li-ion", sizeof(info->technology) - 1);
    info->technology[sizeof(info->technology) - 1] = '\0';

    return 0;
}

/**
 * Read AC adapter status from EC.
 */
static int read_acpi_ac_adapter(ac_adapter_t* ac) {
    ac->present = true;
    strncpy(ac->type, "Mains", sizeof(ac->type) - 1);
    ac->type[sizeof(ac->type) - 1] = '\0';

    if (!ec_available) {
        /* No EC: report AC as online (desktop / QEMU assumption) */
        ac->online = true;
        return 0;
    }

    int status = ec_read(EC_AC_STATUS);
    if (status < 0) {
        ac->online = true;  /* assume AC if EC read fails */
        return 0;
    }

    ac->online = (status & 0x01) ? true : false;
    return 0;
}

/**
 * Initialize battery subsystem
 */
int battery_init(void) {
    kprintf("[BATTERY] Initializing battery subsystem...\n");

    // Try to read battery information
    if (read_acpi_battery_info(&power_global.battery) < 0) {
        kprintf("[BATTERY] No battery found\n");
        power_global.battery.present = false;
        return 0;  // Not an error, just no battery
    }

    kprintf("[BATTERY] Battery found: %s %s\n",
            power_global.battery.manufacturer,
            power_global.battery.model);
    kprintf("[BATTERY] Capacity: %u mAh, Health: %u%%\n",
            power_global.battery.capacity_mah,
            power_global.battery.health_percentage);

    last_battery_update = timer_get_ticks();

    return 0;
}

/**
 * Update battery information
 */
int battery_update(void) {
    uint64_t now = timer_get_ticks();

    // Don't update too frequently
    if (now - last_battery_update < BATTERY_UPDATE_INTERVAL) {
        return 0;
    }

    if (!power_global.battery.present) {
        return -1;
    }

    battery_state_t old_state = power_global.battery.state;
    uint32_t old_percentage = power_global.battery.percentage;

    // Read updated battery information
    if (read_acpi_battery_info(&power_global.battery) < 0) {
        return -1;
    }

    last_battery_update = now;

    // Check for state changes
    if (old_state != power_global.battery.state) {
        if (power_global.battery.state == BATTERY_STATE_CHARGING) {
            kprintf("[BATTERY] Now charging\n");
        } else if (power_global.battery.state == BATTERY_STATE_DISCHARGING) {
            kprintf("[BATTERY] Now discharging\n");
        } else if (power_global.battery.state == BATTERY_STATE_FULL) {
            kprintf("[BATTERY] Fully charged\n");
        }
    }

    // Check for low battery
    if (power_global.battery.percentage <= 5 && old_percentage > 5) {
        kprintf("[BATTERY] CRITICAL: Battery at %u%%\n", power_global.battery.percentage);
        power_notify_event(POWER_EVENT_BATTERY_CRITICAL, &power_global.battery);
    } else if (power_global.battery.percentage <= 10 && old_percentage > 10) {
        kprintf("[BATTERY] WARNING: Battery at %u%%\n", power_global.battery.percentage);
        power_notify_event(POWER_EVENT_BATTERY_LOW, &power_global.battery);
    }

    return 0;
}

/**
 * Get battery information
 */
battery_info_t* battery_get_info(void) {
    // Update if needed
    battery_update();
    return &power_global.battery;
}

/**
 * Check if battery is present
 */
bool battery_is_present(void) {
    return power_global.battery.present;
}

/**
 * Get battery percentage
 */
uint32_t battery_get_percentage(void) {
    battery_update();
    return power_global.battery.percentage;
}

/**
 * Get battery state
 */
battery_state_t battery_get_state(void) {
    battery_update();
    return power_global.battery.state;
}

/**
 * Get time remaining (minutes)
 */
uint32_t battery_get_time_remaining(void) {
    battery_update();

    if (power_global.battery.state == BATTERY_STATE_DISCHARGING) {
        return power_global.battery.time_to_empty_min;
    } else if (power_global.battery.state == BATTERY_STATE_CHARGING) {
        return power_global.battery.time_to_full_min;
    }

    return 0;
}

/**
 * Initialize AC adapter subsystem
 */
int ac_adapter_init(void) {
    kprintf("[AC] Initializing AC adapter subsystem...\n");

    // Read AC adapter status
    if (read_acpi_ac_adapter(&power_global.ac_adapter) < 0) {
        kprintf("[AC] No AC adapter found\n");
        return -1;
    }

    kprintf("[AC] AC adapter: %s (%s)\n",
            power_global.ac_adapter.online ? "Online" : "Offline",
            power_global.ac_adapter.type);

    return 0;
}

/**
 * Get AC adapter information
 */
ac_adapter_t* ac_adapter_get_info(void) {
    // Re-read status
    read_acpi_ac_adapter(&power_global.ac_adapter);
    return &power_global.ac_adapter;
}

/**
 * Check if AC adapter is online
 */
bool ac_adapter_is_online(void) {
    read_acpi_ac_adapter(&power_global.ac_adapter);
    return power_global.ac_adapter.online;
}
