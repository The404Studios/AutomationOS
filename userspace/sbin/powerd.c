/*
 * powerd - Power Management Daemon
 * Monitors battery, temperature, and applies power policies
 *
 * Usage: powerd [options]
 *   -p, --profile <profile>  Set power profile (performance/balanced/powersaver)
 *   -d, --daemon             Run as daemon
 *   -v, --verbose            Verbose output
 *   -h, --help               Show this help
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

// Power management interface (syscalls)
#define SYS_POWER_GET_STATE     300
#define SYS_POWER_SET_PROFILE   301
#define SYS_BATTERY_GET_INFO    302
#define SYS_THERMAL_GET_TEMP    303
#define SYS_CPUFREQ_SET_GOV     304

// Power profiles
#define POWER_PROFILE_PERFORMANCE   0
#define POWER_PROFILE_BALANCED      1
#define POWER_PROFILE_POWER_SAVER   2

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

// Global state
static int running = 1;
static int daemon_mode = 0;
static int verbose = 0;
static int current_profile = POWER_PROFILE_BALANCED;

/**
 * Signal handler
 */
void signal_handler(int sig) {
    if (sig == SIGTERM || sig == SIGINT) {
        printf("powerd: Shutting down...\n");
        running = 0;
    }
}

/**
 * Daemonize process
 */
void daemonize(void) {
    pid_t pid = fork();
    if (pid < 0) {
        exit(1);
    }
    if (pid > 0) {
        exit(0);  // Parent exits
    }

    // Child continues
    umask(0);
    setsid();
    chdir("/");

    // Close standard file descriptors
    close(STDIN_FILENO);
    close(STDOUT_FILENO);
    close(STDERR_FILENO);
}

/**
 * Get battery information (syscall stub)
 */
int get_battery_info(battery_info_t* info) {
    // TODO: Real syscall implementation
    // For now, simulate battery
    info->present = 1;
    info->state = BATTERY_STATE_DISCHARGING;
    info->capacity_mah = 5000;
    info->remaining_mah = 4250;
    info->percentage = 85;
    info->voltage_mv = 11400;
    info->current_ma = -1500;
    info->temperature = 350;
    info->time_to_empty_min = 170;  // ~2.8 hours
    info->time_to_full_min = 0;
    return 0;
}

/**
 * Get CPU temperature (syscall stub)
 */
int get_cpu_temperature(void) {
    // TODO: Real syscall implementation
    return 45;  // 45°C
}

/**
 * Set power profile (syscall stub)
 */
int set_power_profile(int profile) {
    // TODO: Real syscall implementation
    current_profile = profile;

    if (verbose) {
        const char* name = profile == POWER_PROFILE_PERFORMANCE ? "Performance" :
                          profile == POWER_PROFILE_BALANCED ? "Balanced" :
                          "Power Saver";
        printf("powerd: Switched to %s profile\n", name);
    }

    return 0;
}

/**
 * Automatic profile selection based on battery/AC
 */
void auto_select_profile(battery_info_t* battery, int ac_online) {
    int new_profile = current_profile;

    if (ac_online) {
        // On AC: Use Performance or Balanced
        new_profile = POWER_PROFILE_BALANCED;
    } else {
        // On battery: Adjust based on percentage
        if (battery->percentage > 50) {
            new_profile = POWER_PROFILE_BALANCED;
        } else if (battery->percentage > 20) {
            new_profile = POWER_PROFILE_POWER_SAVER;
        } else {
            // Critical battery: Aggressive power saving
            new_profile = POWER_PROFILE_POWER_SAVER;
        }
    }

    if (new_profile != current_profile) {
        set_power_profile(new_profile);
    }
}

/**
 * Monitor and log battery status
 */
void monitor_battery(battery_info_t* battery) {
    static int last_percentage = -1;
    static int last_state = -1;

    // Check for significant changes
    if (battery->state != last_state) {
        const char* state_name = battery->state == BATTERY_STATE_CHARGING ? "charging" :
                                battery->state == BATTERY_STATE_DISCHARGING ? "discharging" :
                                battery->state == BATTERY_STATE_FULL ? "full" :
                                "unknown";

        if (verbose) {
            printf("powerd: Battery now %s\n", state_name);
        }

        last_state = battery->state;
    }

    // Log every 10% change
    if (last_percentage == -1 || abs((int)battery->percentage - last_percentage) >= 10) {
        if (verbose) {
            printf("powerd: Battery at %u%%", battery->percentage);

            if (battery->state == BATTERY_STATE_DISCHARGING && battery->time_to_empty_min > 0) {
                printf(" (%uh %02um remaining)",
                       battery->time_to_empty_min / 60,
                       battery->time_to_empty_min % 60);
            }

            printf("\n");
        }

        last_percentage = battery->percentage;
    }

    // Warn on low battery
    if (battery->percentage <= 10 && battery->percentage > 5) {
        printf("powerd: WARNING: Battery low (%u%%)\n", battery->percentage);
    } else if (battery->percentage <= 5) {
        printf("powerd: CRITICAL: Battery critical (%u%%), suspending soon...\n",
               battery->percentage);
    }
}

/**
 * Monitor thermal status
 */
void monitor_thermal(void) {
    static int last_temp = -1;

    int temp = get_cpu_temperature();

    // Log significant temperature changes
    if (last_temp == -1 || abs(temp - last_temp) >= 5) {
        if (temp >= 85) {
            printf("powerd: WARNING: CPU temperature high (%d°C)\n", temp);
        } else if (verbose && abs(temp - last_temp) >= 10) {
            printf("powerd: CPU temperature: %d°C\n", temp);
        }

        last_temp = temp;
    }

    // Emergency thermal throttling
    if (temp >= 90) {
        printf("powerd: CRITICAL: CPU temperature %d°C, forcing power saver mode\n", temp);
        set_power_profile(POWER_PROFILE_POWER_SAVER);
    }
}

/**
 * Main power management loop
 */
void power_management_loop(void) {
    battery_info_t battery;
    int ac_online = 0;  // Assume on battery for now

    while (running) {
        // Update battery info
        if (get_battery_info(&battery) == 0 && battery.present) {
            monitor_battery(&battery);

            // Determine if on AC (charging or full = AC online)
            ac_online = (battery.state == BATTERY_STATE_CHARGING ||
                        battery.state == BATTERY_STATE_FULL);

            // Auto-select profile
            auto_select_profile(&battery, ac_online);
        }

        // Monitor temperature
        monitor_thermal();

        // Sleep for 30 seconds
        sleep(30);
    }
}

/**
 * Print usage
 */
void print_usage(const char* prog) {
    printf("Usage: %s [options]\n", prog);
    printf("Options:\n");
    printf("  -p, --profile <profile>  Set power profile (performance/balanced/powersaver)\n");
    printf("  -d, --daemon             Run as daemon\n");
    printf("  -v, --verbose            Verbose output\n");
    printf("  -h, --help               Show this help\n");
}

/**
 * Main
 */
int main(int argc, char* argv[]) {
    // Parse command line arguments
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-p") == 0 || strcmp(argv[i], "--profile") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Error: --profile requires an argument\n");
                return 1;
            }

            const char* profile = argv[++i];
            if (strcmp(profile, "performance") == 0) {
                current_profile = POWER_PROFILE_PERFORMANCE;
            } else if (strcmp(profile, "balanced") == 0) {
                current_profile = POWER_PROFILE_BALANCED;
            } else if (strcmp(profile, "powersaver") == 0) {
                current_profile = POWER_PROFILE_POWER_SAVER;
            } else {
                fprintf(stderr, "Error: Invalid profile '%s'\n", profile);
                return 1;
            }
        } else if (strcmp(argv[i], "-d") == 0 || strcmp(argv[i], "--daemon") == 0) {
            daemon_mode = 1;
        } else if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--verbose") == 0) {
            verbose = 1;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "Error: Unknown option '%s'\n", argv[i]);
            print_usage(argv[0]);
            return 1;
        }
    }

    // Setup signal handlers
    signal(SIGTERM, signal_handler);
    signal(SIGINT, signal_handler);

    // Apply initial profile
    set_power_profile(current_profile);

    printf("powerd: Starting power management daemon...\n");
    printf("powerd: Profile: %s\n",
           current_profile == POWER_PROFILE_PERFORMANCE ? "Performance" :
           current_profile == POWER_PROFILE_BALANCED ? "Balanced" :
           "Power Saver");

    // Daemonize if requested
    if (daemon_mode) {
        printf("powerd: Running as daemon\n");
        daemonize();
    }

    // Run main loop
    power_management_loop();

    printf("powerd: Stopped\n");
    return 0;
}
