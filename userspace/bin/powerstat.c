/*
 * powerstat - Display power management statistics
 * Shows battery, CPU frequency, temperature, and power consumption
 *
 * Usage: powerstat [options]
 *   -c, --continuous    Continuously update display
 *   -i, --interval <s>  Update interval in seconds (default: 2)
 *   -h, --help          Show this help
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

// Battery states
#define BATTERY_STATE_UNKNOWN       0
#define BATTERY_STATE_CHARGING      1
#define BATTERY_STATE_DISCHARGING   2
#define BATTERY_STATE_FULL          3

typedef struct {
    int present;
    int state;
    unsigned int capacity_mah;
    unsigned int remaining_mah;
    unsigned int percentage;
    unsigned int voltage_mv;
    int current_ma;
    unsigned int temperature;
    unsigned int time_to_empty_min;
    unsigned int time_to_full_min;
} battery_info_t;

typedef struct {
    unsigned int cpu;
    unsigned int current_freq_mhz;
    unsigned int min_freq_mhz;
    unsigned int max_freq_mhz;
} cpu_freq_t;

/**
 * Get battery information (stub)
 */
int get_battery_info(battery_info_t* info) {
    // Simulate battery
    info->present = 1;
    info->state = BATTERY_STATE_DISCHARGING;
    info->capacity_mah = 5000;
    info->remaining_mah = 4250;
    info->percentage = 85;
    info->voltage_mv = 11400;
    info->current_ma = -1500;
    info->temperature = 350;
    info->time_to_empty_min = 170;
    info->time_to_full_min = 0;
    return 0;
}

/**
 * Get CPU frequency (stub)
 */
int get_cpu_freq(unsigned int cpu, cpu_freq_t* freq) {
    freq->cpu = cpu;
    freq->current_freq_mhz = 2400;
    freq->min_freq_mhz = 800;
    freq->max_freq_mhz = 3600;
    return 0;
}

/**
 * Get CPU temperature (stub)
 */
int get_cpu_temperature(void) {
    return 45;
}

/**
 * Get CPU load percentage (stub)
 */
int get_cpu_load(void) {
    return 30;
}

/**
 * Estimate power draw
 */
double estimate_power_draw(battery_info_t* battery, cpu_freq_t* freq, int temp) {
    double watts = 5.0;  // Base system

    // CPU power (rough estimate based on frequency)
    watts += (freq->current_freq_mhz / 1000.0) * 2.5;

    // Temperature affects power (higher temp = more fan power)
    if (temp > 60) {
        watts += 2.0;
    } else if (temp > 50) {
        watts += 1.0;
    }

    // Battery charging
    if (battery->state == BATTERY_STATE_CHARGING) {
        watts += 10.0;
    }

    return watts;
}

/**
 * Display power statistics
 */
void display_stats(void) {
    battery_info_t battery;
    cpu_freq_t freq;
    int temp;
    int load;

    // Clear screen (ANSI escape code)
    printf("\033[2J\033[H");

    printf("=== AutomationOS Power Statistics ===\n\n");

    // CPU info
    if (get_cpu_freq(0, &freq) == 0) {
        load = get_cpu_load();
        temp = get_cpu_temperature();

        printf("CPU:\n");
        printf("  Frequency: %u MHz (min %u, max %u)\n",
               freq.current_freq_mhz, freq.min_freq_mhz, freq.max_freq_mhz);
        printf("  Load:      %d%%\n", load);
        printf("  Temp:      %d°C\n", temp);
        printf("\n");
    }

    // Battery info
    if (get_battery_info(&battery) == 0 && battery.present) {
        const char* state_name = battery.state == BATTERY_STATE_CHARGING ? "Charging" :
                                battery.state == BATTERY_STATE_DISCHARGING ? "Discharging" :
                                battery.state == BATTERY_STATE_FULL ? "Full" :
                                "Unknown";

        printf("Battery:\n");
        printf("  Status:    %s\n", state_name);
        printf("  Charge:    %u%% (%u / %u mAh)\n",
               battery.percentage, battery.remaining_mah, battery.capacity_mah);
        printf("  Voltage:   %u.%03u V\n",
               battery.voltage_mv / 1000, battery.voltage_mv % 1000);
        printf("  Current:   %d mA\n", battery.current_ma);
        printf("  Temp:      %u.%u°C\n",
               battery.temperature / 10, battery.temperature % 10);

        if (battery.state == BATTERY_STATE_DISCHARGING && battery.time_to_empty_min > 0) {
            printf("  Remaining: %uh %02um\n",
                   battery.time_to_empty_min / 60,
                   battery.time_to_empty_min % 60);
        } else if (battery.state == BATTERY_STATE_CHARGING && battery.time_to_full_min > 0) {
            printf("  To full:   %uh %02um\n",
                   battery.time_to_full_min / 60,
                   battery.time_to_full_min % 60);
        }

        printf("\n");
    }

    // Power draw estimate
    double watts = estimate_power_draw(&battery, &freq, temp);
    printf("Power Draw: %.1f W (estimated)\n", watts);

    // Update time
    time_t now = time(NULL);
    printf("\nUpdated: %s", ctime(&now));
}

/**
 * Print usage
 */
void print_usage(const char* prog) {
    printf("Usage: %s [options]\n", prog);
    printf("Options:\n");
    printf("  -c, --continuous    Continuously update display\n");
    printf("  -i, --interval <s>  Update interval in seconds (default: 2)\n");
    printf("  -h, --help          Show this help\n");
}

/**
 * Main
 */
int main(int argc, char* argv[]) {
    int continuous = 0;
    int interval = 2;

    // Parse arguments
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-c") == 0 || strcmp(argv[i], "--continuous") == 0) {
            continuous = 1;
        } else if (strcmp(argv[i], "-i") == 0 || strcmp(argv[i], "--interval") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Error: --interval requires an argument\n");
                return 1;
            }
            interval = atoi(argv[++i]);
            if (interval <= 0) {
                fprintf(stderr, "Error: Invalid interval\n");
                return 1;
            }
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "Error: Unknown option '%s'\n", argv[i]);
            print_usage(argv[0]);
            return 1;
        }
    }

    if (continuous) {
        // Continuous mode
        while (1) {
            display_stats();
            sleep(interval);
        }
    } else {
        // Single shot
        display_stats();
    }

    return 0;
}
