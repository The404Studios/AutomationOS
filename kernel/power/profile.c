/*
 * Power Profiles
 * Performance, Balanced, Power Saver
 */

#include "../include/power.h"
#include "../include/kernel.h"

extern power_global_state_t power_global;

// Profile configurations (from power.c)
extern power_profile_config_t power_profiles[3];

/**
 * Initialize power profile system
 */
int power_profile_init(void) {
    kprintf("[PROFILE] Initializing power profiles...\n");
    power_global.current_profile = POWER_PROFILE_BALANCED;
    return 0;
}

/**
 * Set power profile
 */
int power_profile_set(power_profile_t profile) {
    if (profile >= 3) {
        return -1;
    }

    kprintf("[PROFILE] Switching to %s profile\n",
            profile == POWER_PROFILE_PERFORMANCE ? "Performance" :
            profile == POWER_PROFILE_BALANCED ? "Balanced" :
            "Power Saver");

    power_global.current_profile = profile;
    return power_profile_apply(profile);
}

/**
 * Get current power profile
 */
power_profile_t power_profile_get(void) {
    return power_global.current_profile;
}

/**
 * Get power profile configuration
 */
power_profile_config_t* power_profile_get_config(power_profile_t profile) {
    if (profile >= 3) {
        return NULL;
    }
    return &power_profiles[profile];
}

/**
 * Apply power profile
 */
int power_profile_apply(power_profile_t profile) {
    power_profile_config_t* config = power_profile_get_config(profile);
    if (!config) {
        return -1;
    }

    kprintf("[PROFILE] Applying %s profile...\n",
            profile == POWER_PROFILE_PERFORMANCE ? "Performance" :
            profile == POWER_PROFILE_BALANCED ? "Balanced" :
            "Power Saver");

    // Apply CPU governor to all CPUs
    for (uint32_t i = 0; i < power_global.num_cpus; i++) {
        cpufreq_set_governor(i, config->cpu_governor);

        // Apply frequency limit derived from hardware max (not current max,
        // which would ratchet down on repeated profile application)
        cpufreq_policy_t* policy = cpufreq_get_policy(i);
        if (policy) {
            uint32_t hw_max = policy->min_freq_mhz;  // fallback
            if (policy->available_freqs && policy->num_freqs > 0) {
                hw_max = policy->available_freqs[policy->num_freqs - 1];
            }
            policy->max_freq_mhz = (hw_max * config->cpu_max_freq_percent) / 100;
        }
    }

    // Apply display settings
    display_set_blank_timeout(config->display_blank_sec);
    display_set_poweroff_timeout(config->display_poweroff_sec);
    backlight_set_brightness(config->backlight_percent);

    // TODO: Apply Bluetooth/WiFi settings
    if (!config->enable_bluetooth) {
        kprintf("[PROFILE] Disabling Bluetooth for power saving\n");
    }

    kprintf("[PROFILE] Profile applied successfully\n");
    return 0;
}
