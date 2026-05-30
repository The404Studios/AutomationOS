/*
 * Display Power Management
 * Screen blanking, backlight control
 */

#include "../include/power.h"
#include "../include/kernel.h"

extern power_global_state_t power_global;

/**
 * Initialize display power management
 */
int display_power_init(void) {
    kprintf("[DISPLAY] Initializing display power management...\n");

    power_global.display.blanked = false;
    power_global.display.powered_off = false;
    power_global.display.backlight_percentage = 100;
    power_global.display.blank_timeout_sec = 300;      // 5 minutes
    power_global.display.poweroff_timeout_sec = 600;   // 10 minutes

    return 0;
}

/**
 * Set blank timeout
 */
int display_set_blank_timeout(uint32_t seconds) {
    power_global.display.blank_timeout_sec = seconds;
    kprintf("[DISPLAY] Blank timeout set to %u seconds\n", seconds);
    return 0;
}

/**
 * Set power off timeout
 */
int display_set_poweroff_timeout(uint32_t seconds) {
    power_global.display.poweroff_timeout_sec = seconds;
    kprintf("[DISPLAY] Power off timeout set to %u seconds\n", seconds);
    return 0;
}

/**
 * Blank display
 */
int display_blank(void) {
    if (power_global.display.blanked) {
        return 0;
    }

    kprintf("[DISPLAY] Blanking display...\n");

    // TODO: Send blank command to display driver
    power_global.display.blanked = true;

    return 0;
}

/**
 * Unblank display
 */
int display_unblank(void) {
    if (!power_global.display.blanked) {
        return 0;
    }

    kprintf("[DISPLAY] Unblanking display...\n");

    // TODO: Send unblank command to display driver
    power_global.display.blanked = false;

    return 0;
}

/**
 * Power off display
 */
int display_poweroff(void) {
    if (power_global.display.powered_off) {
        return 0;
    }

    kprintf("[DISPLAY] Powering off display...\n");

    display_blank();

    // TODO: Send DPMS power off command
    power_global.display.powered_off = true;

    return 0;
}

/**
 * Power on display
 */
int display_poweron(void) {
    if (!power_global.display.powered_off) {
        return 0;
    }

    kprintf("[DISPLAY] Powering on display...\n");

    // TODO: Send DPMS power on command
    power_global.display.powered_off = false;

    display_unblank();

    return 0;
}

/**
 * Initialize backlight
 */
int backlight_init(void) {
    kprintf("[BACKLIGHT] Initializing backlight control...\n");
    power_global.display.backlight_percentage = 100;
    return 0;
}

/**
 * Set backlight brightness (percentage)
 */
int backlight_set_brightness(uint32_t percentage) {
    if (percentage > 100) {
        percentage = 100;
    }

    kprintf("[BACKLIGHT] Setting brightness to %u%%\n", percentage);

    // TODO: Write to backlight hardware
    // Usually /sys/class/backlight/*/brightness

    power_global.display.backlight_percentage = percentage;
    return 0;
}

/**
 * Get backlight brightness
 */
uint32_t backlight_get_brightness(void) {
    return power_global.display.backlight_percentage;
}

/**
 * Set backlight brightness (raw value)
 */
int backlight_set_brightness_raw(uint32_t value) {
    // TODO: Convert to percentage and set
    uint32_t max = backlight_get_max_brightness();
    if (max == 0) {
        return -1;
    }

    uint32_t percentage = (value * 100) / max;
    return backlight_set_brightness(percentage);
}

/**
 * Get maximum backlight brightness
 */
uint32_t backlight_get_max_brightness(void) {
    // TODO: Read from hardware
    return 255;  // Common max value
}
