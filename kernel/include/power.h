#ifndef POWER_H
#define POWER_H

#include "types.h"
#include "acpi.h"

/*
 * Power Management for AutomationOS
 * Sleep states, battery monitoring, CPU frequency scaling, thermal management
 */

// System Power States
typedef enum {
    POWER_STATE_S0 = 0,         // Working (On)
    POWER_STATE_S1,             // Standby
    POWER_STATE_S2,             // Suspend (rare)
    POWER_STATE_S3,             // Sleep (Suspend to RAM)
    POWER_STATE_S4,             // Hibernate (Suspend to Disk)
    POWER_STATE_S5,             // Soft Off
} power_state_t;

// Battery State
typedef enum {
    BATTERY_STATE_UNKNOWN = 0,
    BATTERY_STATE_CHARGING,
    BATTERY_STATE_DISCHARGING,
    BATTERY_STATE_FULL,
    BATTERY_STATE_NOT_CHARGING,
} battery_state_t;

// Battery Information
typedef struct {
    bool present;
    battery_state_t state;

    uint32_t capacity_mah;      // Design capacity (mAh)
    uint32_t remaining_mah;     // Current charge (mAh)
    uint32_t percentage;        // 0-100

    uint32_t voltage_mv;        // Voltage (mV)
    int32_t current_ma;         // Current (mA, negative = discharging)
    uint32_t temperature;       // Temperature (Celsius * 10)

    uint32_t time_to_empty_min; // Minutes until empty
    uint32_t time_to_full_min;  // Minutes until full

    uint32_t cycle_count;
    uint32_t health_percentage; // Battery health 0-100

    char manufacturer[32];
    char model[32];
    char serial[32];
    char technology[16];        // "Li-ion", "Li-poly", etc.
} battery_info_t;

// AC Adapter State
typedef struct {
    bool online;
    bool present;
    char type[32];              // "Mains", "USB", etc.
} ac_adapter_t;

// CPU Frequency Governor
typedef enum {
    CPUFREQ_GOVERNOR_PERFORMANCE = 0,  // Always max frequency
    CPUFREQ_GOVERNOR_POWERSAVE,        // Always min frequency
    CPUFREQ_GOVERNOR_ONDEMAND,         // Scale based on load
    CPUFREQ_GOVERNOR_CONSERVATIVE,     // Like ondemand but slower
    CPUFREQ_GOVERNOR_USERSPACE,        // User controlled
} cpufreq_governor_t;

// CPU Frequency Policy
typedef struct {
    uint32_t cpu;
    uint32_t min_freq_mhz;
    uint32_t max_freq_mhz;
    uint32_t current_freq_mhz;
    cpufreq_governor_t governor;

    // Available frequencies
    uint32_t* available_freqs;
    uint32_t num_freqs;

    // Transition latency (nanoseconds)
    uint32_t transition_latency_ns;
} cpufreq_policy_t;

// CPU C-State Information
typedef struct {
    uint32_t cpu;
    acpi_cstate_t current_state;

    // Latencies (microseconds)
    uint32_t c1_latency;
    uint32_t c2_latency;
    uint32_t c3_latency;

    // Time spent in each state (milliseconds)
    uint64_t c0_time_ms;
    uint64_t c1_time_ms;
    uint64_t c2_time_ms;
    uint64_t c3_time_ms;
} cpuidle_state_t;

// Thermal Zone
typedef struct {
    char name[32];              // "CPU", "GPU", "ACPI", etc.
    uint32_t temperature;       // Celsius * 1000

    // Trip points (thresholds)
    uint32_t trip_point_active;     // Fan on
    uint32_t trip_point_passive;    // Throttle
    uint32_t trip_point_hot;        // Urgent
    uint32_t trip_point_critical;   // Shutdown

    // Current cooling state
    uint32_t cooling_state;         // 0 = no cooling, higher = more cooling
    uint32_t max_cooling_state;
} thermal_zone_t;

// Thermal Action
typedef enum {
    THERMAL_ACTION_NONE = 0,
    THERMAL_ACTION_NOTIFY,
    THERMAL_ACTION_THROTTLE,
    THERMAL_ACTION_SHUTDOWN,
} thermal_action_t;

// Display Power State
typedef struct {
    bool blanked;
    bool powered_off;
    uint32_t backlight_percentage;  // 0-100
    uint32_t blank_timeout_sec;
    uint32_t poweroff_timeout_sec;
} display_power_t;

// Power Profile
typedef enum {
    POWER_PROFILE_PERFORMANCE = 0,
    POWER_PROFILE_BALANCED,
    POWER_PROFILE_POWER_SAVER,
} power_profile_t;

// Power Profile Configuration
typedef struct {
    power_profile_t profile;

    // CPU
    cpufreq_governor_t cpu_governor;
    uint32_t cpu_max_freq_percent;  // Limit max frequency (%)

    // Display
    uint32_t display_blank_sec;
    uint32_t display_poweroff_sec;
    uint32_t backlight_percent;

    // Sleep
    uint32_t sleep_timeout_sec;

    // Other
    bool enable_bluetooth;
    bool enable_wifi;
} power_profile_config_t;

// Power Event
typedef enum {
    POWER_EVENT_NONE = 0,
    POWER_EVENT_POWER_BUTTON,
    POWER_EVENT_SLEEP_BUTTON,
    POWER_EVENT_LID_CLOSED,
    POWER_EVENT_LID_OPENED,
    POWER_EVENT_AC_PLUGGED,
    POWER_EVENT_AC_UNPLUGGED,
    POWER_EVENT_BATTERY_LOW,        // < 10%
    POWER_EVENT_BATTERY_CRITICAL,   // < 5%
    POWER_EVENT_THERMAL_WARNING,
    POWER_EVENT_THERMAL_CRITICAL,
} power_event_t;

// Power Event Handler
typedef void (*power_event_handler_t)(power_event_t event, void* data);

// Hibernation Image
typedef struct {
    uint32_t magic;             // Magic number
    uint32_t version;
    uint64_t timestamp;

    // Memory info
    uint64_t memory_size;
    uint64_t num_pages;

    // CPU state
    uint64_t cr3;               // Page directory
    uint64_t rip;               // Instruction pointer
    uint64_t rsp;               // Stack pointer
    uint64_t rflags;

    // Checksums
    uint32_t header_checksum;
    uint32_t data_checksum;
} hibernation_header_t;

#define HIBERNATION_MAGIC   0x48494245  // "HIBE"
#define HIBERNATION_VERSION 1

// Power Management Initialization
int power_init(void);
void power_shutdown(void);

// System Sleep States
int power_suspend_to_ram(void);         // S3 sleep
int power_hibernate(void);              // S4 hibernate
int power_suspend_to_idle(void);        // Freeze (no ACPI)
int power_resume(void);

// Sleep Preparation and Resume
int power_prepare_suspend(void);
int power_suspend_devices(void);
int power_resume_devices(void);
void power_complete_resume(void);

// Hibernation
int hibernation_create_image(const char* device);
int hibernation_restore_image(const char* device);
int hibernation_write_image(hibernation_header_t* header, const char* device);
int hibernation_read_image(const char* device);

// Battery Management
int battery_init(void);
battery_info_t* battery_get_info(void);
int battery_update(void);
bool battery_is_present(void);
uint32_t battery_get_percentage(void);
battery_state_t battery_get_state(void);
uint32_t battery_get_time_remaining(void);

// AC Adapter
int ac_adapter_init(void);
ac_adapter_t* ac_adapter_get_info(void);
bool ac_adapter_is_online(void);

// CPU Frequency Scaling
int cpufreq_init(void);
int cpufreq_set_frequency(uint32_t cpu, uint32_t freq_mhz);
uint32_t cpufreq_get_frequency(uint32_t cpu);
int cpufreq_set_governor(uint32_t cpu, cpufreq_governor_t governor);
cpufreq_governor_t cpufreq_get_governor(uint32_t cpu);
cpufreq_policy_t* cpufreq_get_policy(uint32_t cpu);
int cpufreq_set_policy(uint32_t cpu, cpufreq_policy_t* policy);

// CPU Idle (C-States)
int cpuidle_init(void);
int cpuidle_enter_state(uint32_t cpu, acpi_cstate_t state);
cpuidle_state_t* cpuidle_get_state(uint32_t cpu);
uint64_t cpuidle_get_time_in_state(uint32_t cpu, acpi_cstate_t state);

// Thermal Management
int thermal_init(void);
thermal_zone_t* thermal_get_zone(const char* name);
uint32_t thermal_get_temperature(const char* name);
int thermal_set_cooling_state(const char* name, uint32_t state);
int thermal_throttle_cpu(uint32_t cpu, uint32_t percentage);
void thermal_check_trip_points(thermal_zone_t* zone);

// Display Power Management
int display_power_init(void);
int display_set_blank_timeout(uint32_t seconds);
int display_set_poweroff_timeout(uint32_t seconds);
int display_blank(void);
int display_unblank(void);
int display_poweroff(void);
int display_poweron(void);

// Backlight Control
int backlight_init(void);
int backlight_set_brightness(uint32_t percentage);
uint32_t backlight_get_brightness(void);
int backlight_set_brightness_raw(uint32_t value);
uint32_t backlight_get_max_brightness(void);

// Power Profiles
int power_profile_init(void);
int power_profile_set(power_profile_t profile);
power_profile_t power_profile_get(void);
power_profile_config_t* power_profile_get_config(power_profile_t profile);
int power_profile_apply(power_profile_t profile);

// Runtime Power Management (per-device)
int runtime_pm_init(void);
int runtime_pm_suspend_device(void* device);
int runtime_pm_resume_device(void* device);
int runtime_pm_autosuspend(void* device);
void runtime_pm_set_autosuspend_delay(void* device, uint32_t delay_ms);

// Power Events
int power_event_init(void);
int power_register_event_handler(power_event_t event, power_event_handler_t handler);
int power_unregister_event_handler(power_event_t event);
void power_notify_event(power_event_t event, void* data);

// Power Button/Lid Handling
void power_handle_power_button(void);
void power_handle_sleep_button(void);
void power_handle_lid_event(bool closed);

// Power Statistics
typedef struct {
    // System state
    power_state_t current_state;
    uint64_t uptime_sec;

    // Battery
    bool on_battery;
    uint32_t battery_percent;
    uint32_t time_remaining_min;

    // Power consumption (estimated)
    uint32_t power_draw_watts;
    uint32_t avg_power_draw_watts;

    // CPU
    uint32_t cpu_freq_avg_mhz;
    uint32_t cpu_load_percent;

    // Display
    bool display_on;
    uint32_t backlight_percent;

    // Time in states
    uint64_t time_active_sec;
    uint64_t time_idle_sec;
    uint64_t time_suspended_sec;
} power_stats_t;

power_stats_t* power_get_stats(void);
void power_update_stats(void);
void power_reset_stats(void);

// Power Estimation
uint32_t power_estimate_draw(void);        // Current power draw (watts)
uint32_t power_estimate_battery_life(void); // Minutes remaining
uint32_t power_estimate_time_to_full(void); // Minutes to full charge

// Wake Sources
typedef struct {
    bool power_button;
    bool lid;
    bool keyboard;
    bool mouse;
    bool network;
    bool rtc_alarm;
    bool usb;
} wake_sources_t;

int power_set_wake_sources(wake_sources_t* sources);
wake_sources_t* power_get_wake_sources(void);

// Global Power State
typedef struct {
    bool initialized;

    power_state_t current_state;
    power_profile_t current_profile;

    battery_info_t battery;
    ac_adapter_t ac_adapter;

    cpufreq_policy_t* cpu_policies;  // One per CPU
    cpuidle_state_t* cpu_idle_states;
    uint32_t num_cpus;

    thermal_zone_t* thermal_zones;
    uint32_t num_thermal_zones;

    display_power_t display;

    power_stats_t stats;
    wake_sources_t wake_sources;

    // Event handlers
    power_event_handler_t event_handlers[16];
} power_global_state_t;

extern power_global_state_t power_global;

// Debug/Info
void power_print_info(void);
void power_dump_battery_info(void);
void power_dump_thermal_zones(void);
void power_dump_cpu_info(void);

#endif // POWER_H
