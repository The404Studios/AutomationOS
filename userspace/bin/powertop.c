/*
 * powertop - Power consumption analyzer
 * Shows power consumption by process and device
 *
 * Usage: powertop [options]
 *   -t, --time <s>    Measurement time in seconds (default: 10)
 *   -h, --help        Show this help
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct {
    char name[64];
    double power_mw;
    double cpu_percent;
    unsigned int wakeups_per_sec;
} process_power_t;

typedef struct {
    char name[32];
    double power_mw;
    char state[16];
} device_power_t;

/**
 * Measure process power consumption (stub)
 */
int measure_process_power(process_power_t* processes, int max_count) {
    // Simulate data
    int count = 0;

    strcpy(processes[count].name, "desktop_shell");
    processes[count].power_mw = 850.0;
    processes[count].cpu_percent = 5.2;
    processes[count].wakeups_per_sec = 120;
    count++;

    strcpy(processes[count].name, "compositor");
    processes[count].power_mw = 650.0;
    processes[count].cpu_percent = 3.8;
    processes[count].wakeups_per_sec = 80;
    count++;

    strcpy(processes[count].name, "systemd");
    processes[count].power_mw = 200.0;
    processes[count].cpu_percent = 0.5;
    processes[count].wakeups_per_sec = 10;
    count++;

    strcpy(processes[count].name, "powerd");
    processes[count].power_mw = 50.0;
    processes[count].cpu_percent = 0.1;
    processes[count].wakeups_per_sec = 2;
    count++;

    return count;
}

/**
 * Measure device power consumption (stub)
 */
int measure_device_power(device_power_t* devices, int max_count) {
    // Simulate data
    int count = 0;

    strcpy(devices[count].name, "Display");
    devices[count].power_mw = 3500.0;
    strcpy(devices[count].state, "Active");
    count++;

    strcpy(devices[count].name, "CPU");
    devices[count].power_mw = 5000.0;
    strcpy(devices[count].state, "2400 MHz");
    count++;

    strcpy(devices[count].name, "WiFi");
    devices[count].power_mw = 800.0;
    strcpy(devices[count].state, "Active");
    count++;

    strcpy(devices[count].name, "SSD");
    devices[count].power_mw = 500.0;
    strcpy(devices[count].state, "Active");
    count++;

    strcpy(devices[count].name, "USB");
    devices[count].power_mw = 200.0;
    strcpy(devices[count].state, "Active");
    count++;

    return count;
}

/**
 * Display power consumption report
 */
void display_report(int measurement_time) {
    process_power_t processes[32];
    device_power_t devices[32];

    printf("=== PowerTop - Power Consumption Analysis ===\n");
    printf("Measurement time: %d seconds\n\n", measurement_time);

    // Measure
    int proc_count = measure_process_power(processes, 32);
    int dev_count = measure_device_power(devices, 32);

    // Display top power consumers (processes)
    printf("Top Power Consumers (Processes):\n");
    printf("%-30s %10s %8s %12s\n", "Name", "Power", "CPU", "Wakeups/s");
    printf("--------------------------------------------------------------------\n");

    for (int i = 0; i < proc_count; i++) {
        printf("%-30s %8.1f mW %6.1f%% %10u\n",
               processes[i].name,
               processes[i].power_mw,
               processes[i].cpu_percent,
               processes[i].wakeups_per_sec);
    }

    printf("\n");

    // Display device power consumption
    printf("Device Power Consumption:\n");
    printf("%-20s %10s %15s\n", "Device", "Power", "State");
    printf("--------------------------------------------------------------------\n");

    double total_power = 0.0;
    for (int i = 0; i < dev_count; i++) {
        printf("%-20s %8.1f mW %15s\n",
               devices[i].name,
               devices[i].power_mw,
               devices[i].state);
        total_power += devices[i].power_mw;
    }

    printf("--------------------------------------------------------------------\n");
    printf("Total estimated power: %.2f W\n", total_power / 1000.0);

    printf("\n");

    // Power saving recommendations
    printf("Power Saving Recommendations:\n");
    printf("  • Reduce display brightness to save ~1.0 W\n");
    printf("  • Enable CPU frequency scaling (ondemand governor)\n");
    printf("  • Disable unused devices (Bluetooth, unused USB ports)\n");
    printf("  • Close unnecessary background processes\n");
}

/**
 * Print usage
 */
void print_usage(const char* prog) {
    printf("Usage: %s [options]\n", prog);
    printf("Options:\n");
    printf("  -t, --time <s>    Measurement time in seconds (default: 10)\n");
    printf("  -h, --help        Show this help\n");
}

/**
 * Main
 */
int main(int argc, char* argv[]) {
    int measurement_time = 10;

    // Parse arguments
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-t") == 0 || strcmp(argv[i], "--time") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Error: --time requires an argument\n");
                return 1;
            }
            measurement_time = atoi(argv[++i]);
            if (measurement_time <= 0) {
                fprintf(stderr, "Error: Invalid time\n");
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

    printf("Measuring power consumption");
    fflush(stdout);

    // Simulate measurement
    for (int i = 0; i < measurement_time; i++) {
        sleep(1);
        printf(".");
        fflush(stdout);
    }

    printf("\n\n");

    // Display report
    display_report(measurement_time);

    return 0;
}
