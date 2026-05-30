/**
 * AutomationOS Settings - Panel Creation
 *
 * Creates content panels for each settings category with appropriate widgets.
 */

#include "settings.h"
#include <stdio.h>
#include <string.h>

// ============================================================================
// CALLBACK FUNCTIONS
// ============================================================================

// Display callbacks
static void on_resolution_change(int32_t index, void *user_data) {
    settings_app_t *app = (settings_app_t*)user_data;
    app->display.resolution = (display_resolution_t)index;
    app->dirty = true;
    settings_apply_display(app);
}

static void on_refresh_rate_change(int32_t index, void *user_data) {
    settings_app_t *app = (settings_app_t*)user_data;
    app->display.refresh_rate = (display_refresh_t)index;
    app->dirty = true;
    settings_apply_display(app);
}

static void on_scale_change(int32_t index, void *user_data) {
    settings_app_t *app = (settings_app_t*)user_data;
    app->display.scale = (display_scale_t)index;
    app->dirty = true;
    settings_apply_display(app);
}

static void on_night_light_toggle(bool value, void *user_data) {
    settings_app_t *app = (settings_app_t*)user_data;
    app->display.night_light_enabled = value;
    app->dirty = true;
    settings_apply_display(app);
}

static void on_vsync_toggle(bool value, void *user_data) {
    settings_app_t *app = (settings_app_t*)user_data;
    app->display.vsync_enabled = value;
    app->dirty = true;
    settings_apply_display(app);
}

// Appearance callbacks
static void on_theme_change(int32_t index, void *user_data) {
    settings_app_t *app = (settings_app_t*)user_data;
    app->appearance.theme_mode = (theme_mode_t)index;
    app->dirty = true;
    settings_apply_appearance(app);
}

static void on_accent_color_change(int32_t index, void *user_data) {
    settings_app_t *app = (settings_app_t*)user_data;
    app->appearance.accent_color = (accent_color_t)index;
    app->dirty = true;
    settings_apply_appearance(app);
}

static void on_animations_toggle(bool value, void *user_data) {
    settings_app_t *app = (settings_app_t*)user_data;
    app->appearance.animations_enabled = value;
    app->dirty = true;
    settings_apply_appearance(app);
}

// Sound callbacks
static void on_master_volume_change(float value, void *user_data) {
    settings_app_t *app = (settings_app_t*)user_data;
    app->sound.master_volume = value;
    app->dirty = true;
    settings_apply_sound(app);
}

static void on_system_sounds_toggle(bool value, void *user_data) {
    settings_app_t *app = (settings_app_t*)user_data;
    app->sound.system_sounds_enabled = value;
    app->dirty = true;
    settings_apply_sound(app);
}

// Network callbacks
static void on_wifi_toggle(bool value, void *user_data) {
    settings_app_t *app = (settings_app_t*)user_data;
    app->network.wifi_enabled = value;
    app->dirty = true;
    settings_apply_network(app);
}

static void on_ethernet_toggle(bool value, void *user_data) {
    settings_app_t *app = (settings_app_t*)user_data;
    app->network.ethernet_enabled = value;
    app->dirty = true;
    settings_apply_network(app);
}

// Privacy callbacks
static void on_firewall_toggle(bool value, void *user_data) {
    settings_app_t *app = (settings_app_t*)user_data;
    app->privacy.firewall_enabled = value;
    app->dirty = true;
    settings_apply_privacy(app);
}

static void on_sandboxing_toggle(bool value, void *user_data) {
    settings_app_t *app = (settings_app_t*)user_data;
    app->privacy.sandboxing_enabled = value;
    app->dirty = true;
    settings_apply_privacy(app);
}

// System callbacks
static void on_auto_updates_toggle(bool value, void *user_data) {
    settings_app_t *app = (settings_app_t*)user_data;
    app->system.auto_check_updates = value;
    app->dirty = true;
    settings_apply_system(app);
}

static void on_developer_mode_toggle(bool value, void *user_data) {
    settings_app_t *app = (settings_app_t*)user_data;
    app->system.developer_mode = value;
    app->dirty = true;
    settings_apply_system(app);
}

static void on_check_updates_click(void *user_data) {
    settings_app_t *app = (settings_app_t*)user_data;
    printf("[Settings] Checking for updates...\n");
    // TODO: Trigger update check
}

// ============================================================================
// DISPLAY PANEL
// ============================================================================

void settings_create_display_panel(settings_app_t *app) {
    if (!app) return;

    settings_panel_t *panel = &app->panels[CATEGORY_DISPLAY];
    strncpy(panel->title, "Display Settings", sizeof(panel->title) - 1);
    panel->category = CATEGORY_DISPLAY;
    panel->widgets = NULL;
    panel->widget_count = 0;
    panel->scroll_offset = 0;

    uint32_t y = CONTENT_PADDING;

    // Section: Resolution & Refresh Rate
    widget_t *header1 = widget_create(WIDGET_SECTION_HEADER, "Resolution & Refresh Rate");
    header1->bounds = (rect_t){CONTENT_PADDING, y, 700, 32};
    widget_add_to_panel(panel, header1);
    y += 40;

    // Resolution dropdown
    const char *resolutions[] = {
        "1920 × 1080 (Full HD)",
        "2560 × 1440 (QHD)",
        "3840 × 2160 (4K)",
        "1366 × 768",
        "1600 × 900"
    };
    widget_t *res_dropdown = widget_create_dropdown("Resolution", resolutions, 5,
                                                     app->display.resolution,
                                                     on_resolution_change, app);
    res_dropdown->bounds = (rect_t){CONTENT_PADDING, y, 700, ITEM_HEIGHT};
    widget_add_to_panel(panel, res_dropdown);
    y += ITEM_HEIGHT + 8;

    // Refresh rate dropdown
    const char *refresh_rates[] = {
        "60 Hz",
        "75 Hz",
        "120 Hz",
        "144 Hz",
        "240 Hz"
    };
    widget_t *refresh_dropdown = widget_create_dropdown("Refresh Rate", refresh_rates, 5,
                                                         app->display.refresh_rate,
                                                         on_refresh_rate_change, app);
    refresh_dropdown->bounds = (rect_t){CONTENT_PADDING, y, 700, ITEM_HEIGHT};
    widget_add_to_panel(panel, refresh_dropdown);
    y += ITEM_HEIGHT + 8;

    // Scale dropdown
    const char *scales[] = {
        "100%",
        "125%",
        "150%",
        "175%",
        "200%"
    };
    widget_t *scale_dropdown = widget_create_dropdown("Scale", scales, 5,
                                                       app->display.scale,
                                                       on_scale_change, app);
    scale_dropdown->bounds = (rect_t){CONTENT_PADDING, y, 700, ITEM_HEIGHT};
    widget_add_to_panel(panel, scale_dropdown);
    y += ITEM_HEIGHT + 24;

    // Section: Night Light
    widget_t *header2 = widget_create(WIDGET_SECTION_HEADER, "Night Light");
    header2->bounds = (rect_t){CONTENT_PADDING, y, 700, 32};
    widget_add_to_panel(panel, header2);
    y += 40;

    // Night light toggle
    widget_t *night_light = widget_create_toggle("Enable Night Light",
                                                  app->display.night_light_enabled,
                                                  on_night_light_toggle, app);
    night_light->bounds = (rect_t){CONTENT_PADDING, y, 700, ITEM_HEIGHT};
    widget_add_to_panel(panel, night_light);
    y += ITEM_HEIGHT + 8;

    // Night light schedule label
    widget_t *schedule_label = widget_create(WIDGET_LABEL, "Schedule: Sunset to Sunrise");
    schedule_label->bounds = (rect_t){CONTENT_PADDING + 20, y, 700, 24};
    widget_add_to_panel(panel, schedule_label);
    y += 32;

    // Section: Advanced
    widget_t *header3 = widget_create(WIDGET_SECTION_HEADER, "Advanced");
    header3->bounds = (rect_t){CONTENT_PADDING, y, 700, 32};
    widget_add_to_panel(panel, header3);
    y += 40;

    // VSync toggle
    widget_t *vsync = widget_create_toggle("Enable VSync",
                                           app->display.vsync_enabled,
                                           on_vsync_toggle, app);
    vsync->bounds = (rect_t){CONTENT_PADDING, y, 700, ITEM_HEIGHT};
    widget_add_to_panel(panel, vsync);
    y += ITEM_HEIGHT + 8;

    panel->content_height = y;
}

// ============================================================================
// APPEARANCE PANEL
// ============================================================================

void settings_create_appearance_panel(settings_app_t *app) {
    if (!app) return;

    settings_panel_t *panel = &app->panels[CATEGORY_APPEARANCE];
    strncpy(panel->title, "Appearance", sizeof(panel->title) - 1);
    panel->category = CATEGORY_APPEARANCE;
    panel->widgets = NULL;
    panel->widget_count = 0;
    panel->scroll_offset = 0;

    uint32_t y = CONTENT_PADDING;

    // Section: Theme
    widget_t *header1 = widget_create(WIDGET_SECTION_HEADER, "Theme");
    header1->bounds = (rect_t){CONTENT_PADDING, y, 700, 32};
    widget_add_to_panel(panel, header1);
    y += 40;

    // Theme mode dropdown
    const char *themes[] = {"Light", "Dark", "Auto"};
    widget_t *theme_dropdown = widget_create_dropdown("Appearance", themes, 3,
                                                       app->appearance.theme_mode,
                                                       on_theme_change, app);
    theme_dropdown->bounds = (rect_t){CONTENT_PADDING, y, 700, ITEM_HEIGHT};
    widget_add_to_panel(panel, theme_dropdown);
    y += ITEM_HEIGHT + 8;

    // Accent color dropdown
    const char *accents[] = {"Blue", "Purple", "Green", "Orange", "Red", "Pink", "Custom"};
    widget_t *accent_dropdown = widget_create_dropdown("Accent Color", accents, 7,
                                                        app->appearance.accent_color,
                                                        on_accent_color_change, app);
    accent_dropdown->bounds = (rect_t){CONTENT_PADDING, y, 700, ITEM_HEIGHT};
    widget_add_to_panel(panel, accent_dropdown);
    y += ITEM_HEIGHT + 24;

    // Section: Wallpaper
    widget_t *header2 = widget_create(WIDGET_SECTION_HEADER, "Wallpaper");
    header2->bounds = (rect_t){CONTENT_PADDING, y, 700, 32};
    widget_add_to_panel(panel, header2);
    y += 40;

    // Wallpaper preview (TODO: Add image widget)
    widget_t *wallpaper_label = widget_create(WIDGET_LABEL, "Current: default.jpg");
    wallpaper_label->bounds = (rect_t){CONTENT_PADDING, y, 700, 24};
    widget_add_to_panel(panel, wallpaper_label);
    y += 32;

    // Change wallpaper button
    widget_t *wallpaper_btn = widget_create_button("Choose Wallpaper...", NULL, app);
    wallpaper_btn->bounds = (rect_t){CONTENT_PADDING, y, 200, 40};
    widget_add_to_panel(panel, wallpaper_btn);
    y += 56;

    // Section: Effects
    widget_t *header3 = widget_create(WIDGET_SECTION_HEADER, "Effects");
    header3->bounds = (rect_t){CONTENT_PADDING, y, 700, 32};
    widget_add_to_panel(panel, header3);
    y += 40;

    // Animations toggle
    widget_t *animations = widget_create_toggle("Enable Animations",
                                                app->appearance.animations_enabled,
                                                on_animations_toggle, app);
    animations->bounds = (rect_t){CONTENT_PADDING, y, 700, ITEM_HEIGHT};
    widget_add_to_panel(panel, animations);
    y += ITEM_HEIGHT + 8;

    // Transparency toggle
    widget_t *transparency = widget_create_toggle("Transparency Effects",
                                                  app->appearance.transparency_effects,
                                                  on_animations_toggle, app);
    transparency->bounds = (rect_t){CONTENT_PADDING, y, 700, ITEM_HEIGHT};
    widget_add_to_panel(panel, transparency);
    y += ITEM_HEIGHT + 8;

    panel->content_height = y;
}

// ============================================================================
// SOUND PANEL
// ============================================================================

void settings_create_sound_panel(settings_app_t *app) {
    if (!app) return;

    settings_panel_t *panel = &app->panels[CATEGORY_SOUND];
    strncpy(panel->title, "Sound", sizeof(panel->title) - 1);
    panel->category = CATEGORY_SOUND;
    panel->widgets = NULL;
    panel->widget_count = 0;
    panel->scroll_offset = 0;

    uint32_t y = CONTENT_PADDING;

    // Section: Output
    widget_t *header1 = widget_create(WIDGET_SECTION_HEADER, "Output");
    header1->bounds = (rect_t){CONTENT_PADDING, y, 700, 32};
    widget_add_to_panel(panel, header1);
    y += 40;

    // Output device dropdown
    const char *output_devices[] = {"Built-in Speakers", "HDMI Audio", "USB Audio"};
    widget_t *output_dropdown = widget_create_dropdown("Output Device", output_devices, 3, 0, NULL, app);
    output_dropdown->bounds = (rect_t){CONTENT_PADDING, y, 700, ITEM_HEIGHT};
    widget_add_to_panel(panel, output_dropdown);
    y += ITEM_HEIGHT + 8;

    // Master volume slider
    widget_t *volume_slider = widget_create_slider("Master Volume",
                                                    app->sound.master_volume,
                                                    0.0f, 1.0f,
                                                    on_master_volume_change, app);
    volume_slider->bounds = (rect_t){CONTENT_PADDING, y, 700, ITEM_HEIGHT};
    widget_add_to_panel(panel, volume_slider);
    y += ITEM_HEIGHT + 24;

    // Section: Input
    widget_t *header2 = widget_create(WIDGET_SECTION_HEADER, "Input");
    header2->bounds = (rect_t){CONTENT_PADDING, y, 700, 32};
    widget_add_to_panel(panel, header2);
    y += 40;

    // Input device dropdown
    const char *input_devices[] = {"Built-in Microphone", "USB Microphone"};
    widget_t *input_dropdown = widget_create_dropdown("Input Device", input_devices, 2, 0, NULL, app);
    input_dropdown->bounds = (rect_t){CONTENT_PADDING, y, 700, ITEM_HEIGHT};
    widget_add_to_panel(panel, input_dropdown);
    y += ITEM_HEIGHT + 8;

    // Input volume slider
    widget_t *input_slider = widget_create_slider("Input Volume",
                                                   app->sound.input_volume,
                                                   0.0f, 1.0f,
                                                   NULL, app);
    input_slider->bounds = (rect_t){CONTENT_PADDING, y, 700, ITEM_HEIGHT};
    widget_add_to_panel(panel, input_slider);
    y += ITEM_HEIGHT + 24;

    // Section: Sound Effects
    widget_t *header3 = widget_create(WIDGET_SECTION_HEADER, "Sound Effects");
    header3->bounds = (rect_t){CONTENT_PADDING, y, 700, 32};
    widget_add_to_panel(panel, header3);
    y += 40;

    // System sounds toggle
    widget_t *system_sounds = widget_create_toggle("System Sounds",
                                                   app->sound.system_sounds_enabled,
                                                   on_system_sounds_toggle, app);
    system_sounds->bounds = (rect_t){CONTENT_PADDING, y, 700, ITEM_HEIGHT};
    widget_add_to_panel(panel, system_sounds);
    y += ITEM_HEIGHT + 8;

    // Notification sounds toggle
    widget_t *notif_sounds = widget_create_toggle("Notification Sounds",
                                                  app->sound.notification_sounds,
                                                  on_system_sounds_toggle, app);
    notif_sounds->bounds = (rect_t){CONTENT_PADDING, y, 700, ITEM_HEIGHT};
    widget_add_to_panel(panel, notif_sounds);
    y += ITEM_HEIGHT + 8;

    panel->content_height = y;
}

// ============================================================================
// NETWORK PANEL
// ============================================================================

void settings_create_network_panel(settings_app_t *app) {
    if (!app) return;

    settings_panel_t *panel = &app->panels[CATEGORY_NETWORK];
    strncpy(panel->title, "Network", sizeof(panel->title) - 1);
    panel->category = CATEGORY_NETWORK;
    panel->widgets = NULL;
    panel->widget_count = 0;
    panel->scroll_offset = 0;

    uint32_t y = CONTENT_PADDING;

    // Section: Wi-Fi
    widget_t *header1 = widget_create(WIDGET_SECTION_HEADER, "Wi-Fi");
    header1->bounds = (rect_t){CONTENT_PADDING, y, 700, 32};
    widget_add_to_panel(panel, header1);
    y += 40;

    // Wi-Fi toggle
    widget_t *wifi_toggle = widget_create_toggle("Enable Wi-Fi",
                                                 app->network.wifi_enabled,
                                                 on_wifi_toggle, app);
    wifi_toggle->bounds = (rect_t){CONTENT_PADDING, y, 700, ITEM_HEIGHT};
    widget_add_to_panel(panel, wifi_toggle);
    y += ITEM_HEIGHT + 8;

    // Wi-Fi networks list (TODO: Add list widget)
    widget_t *networks_label = widget_create(WIDGET_LABEL, "Available Networks:");
    networks_label->bounds = (rect_t){CONTENT_PADDING, y, 700, 24};
    widget_add_to_panel(panel, networks_label);
    y += 32;

    // Section: Ethernet
    widget_t *header2 = widget_create(WIDGET_SECTION_HEADER, "Ethernet");
    header2->bounds = (rect_t){CONTENT_PADDING, y, 700, 32};
    widget_add_to_panel(panel, header2);
    y += 40;

    // Ethernet toggle
    widget_t *ethernet_toggle = widget_create_toggle("Enable Ethernet",
                                                     app->network.ethernet_enabled,
                                                     on_ethernet_toggle, app);
    ethernet_toggle->bounds = (rect_t){CONTENT_PADDING, y, 700, ITEM_HEIGHT};
    widget_add_to_panel(panel, ethernet_toggle);
    y += ITEM_HEIGHT + 8;

    // DHCP toggle
    widget_t *dhcp_toggle = widget_create_toggle("Use DHCP",
                                                 app->network.ethernet_dhcp,
                                                 NULL, app);
    dhcp_toggle->bounds = (rect_t){CONTENT_PADDING, y, 700, ITEM_HEIGHT};
    widget_add_to_panel(panel, dhcp_toggle);
    y += ITEM_HEIGHT + 24;

    // Section: VPN
    widget_t *header3 = widget_create(WIDGET_SECTION_HEADER, "VPN");
    header3->bounds = (rect_t){CONTENT_PADDING, y, 700, 32};
    widget_add_to_panel(panel, header3);
    y += 40;

    // Add VPN button
    widget_t *add_vpn = widget_create_button("Add VPN Connection...", NULL, app);
    add_vpn->bounds = (rect_t){CONTENT_PADDING, y, 200, 40};
    widget_add_to_panel(panel, add_vpn);
    y += 56;

    panel->content_height = y;
}

// ============================================================================
// USERS PANEL
// ============================================================================

void settings_create_users_panel(settings_app_t *app) {
    if (!app) return;

    settings_panel_t *panel = &app->panels[CATEGORY_USERS];
    strncpy(panel->title, "Users & Accounts", sizeof(panel->title) - 1);
    panel->category = CATEGORY_USERS;
    panel->widgets = NULL;
    panel->widget_count = 0;
    panel->scroll_offset = 0;

    uint32_t y = CONTENT_PADDING;

    // Section: Current User
    widget_t *header1 = widget_create(WIDGET_SECTION_HEADER, "Current User");
    header1->bounds = (rect_t){CONTENT_PADDING, y, 700, 32};
    widget_add_to_panel(panel, header1);
    y += 40;

    // User info label
    widget_t *user_label = widget_create(WIDGET_LABEL, "Administrator");
    user_label->bounds = (rect_t){CONTENT_PADDING, y, 700, 24};
    widget_add_to_panel(panel, user_label);
    y += 32;

    // Change password button
    widget_t *password_btn = widget_create_button("Change Password...", NULL, app);
    password_btn->bounds = (rect_t){CONTENT_PADDING, y, 200, 40};
    widget_add_to_panel(panel, password_btn);
    y += 56;

    // Section: Other Users
    widget_t *header2 = widget_create(WIDGET_SECTION_HEADER, "Other Users");
    header2->bounds = (rect_t){CONTENT_PADDING, y, 700, 32};
    widget_add_to_panel(panel, header2);
    y += 40;

    // Add user button
    widget_t *add_user = widget_create_button("Add User...", NULL, app);
    add_user->bounds = (rect_t){CONTENT_PADDING, y, 200, 40};
    widget_add_to_panel(panel, add_user);
    y += 56;

    // Section: Login Options
    widget_t *header3 = widget_create(WIDGET_SECTION_HEADER, "Login Options");
    header3->bounds = (rect_t){CONTENT_PADDING, y, 700, 32};
    widget_add_to_panel(panel, header3);
    y += 40;

    // Require password toggle
    widget_t *require_pwd = widget_create_toggle("Require Password on Login",
                                                 app->users.require_password,
                                                 NULL, app);
    require_pwd->bounds = (rect_t){CONTENT_PADDING, y, 700, ITEM_HEIGHT};
    widget_add_to_panel(panel, require_pwd);
    y += ITEM_HEIGHT + 8;

    panel->content_height = y;
}

// ============================================================================
// APPLICATIONS PANEL
// ============================================================================

void settings_create_applications_panel(settings_app_t *app) {
    if (!app) return;

    settings_panel_t *panel = &app->panels[CATEGORY_APPLICATIONS];
    strncpy(panel->title, "Applications", sizeof(panel->title) - 1);
    panel->category = CATEGORY_APPLICATIONS;
    panel->widgets = NULL;
    panel->widget_count = 0;
    panel->scroll_offset = 0;

    uint32_t y = CONTENT_PADDING;

    // Section: Default Applications
    widget_t *header1 = widget_create(WIDGET_SECTION_HEADER, "Default Applications");
    header1->bounds = (rect_t){CONTENT_PADDING, y, 700, 32};
    widget_add_to_panel(panel, header1);
    y += 40;

    // Web browser dropdown
    const char *browsers[] = {"Firefox", "Chrome", "Safari"};
    widget_t *browser_dropdown = widget_create_dropdown("Web Browser", browsers, 3, 0, NULL, app);
    browser_dropdown->bounds = (rect_t){CONTENT_PADDING, y, 700, ITEM_HEIGHT};
    widget_add_to_panel(panel, browser_dropdown);
    y += ITEM_HEIGHT + 8;

    // Email client dropdown
    const char *email_clients[] = {"Mail", "Thunderbird", "Outlook"};
    widget_t *email_dropdown = widget_create_dropdown("Email", email_clients, 3, 0, NULL, app);
    email_dropdown->bounds = (rect_t){CONTENT_PADDING, y, 700, ITEM_HEIGHT};
    widget_add_to_panel(panel, email_dropdown);
    y += ITEM_HEIGHT + 8;

    // Text editor dropdown
    const char *editors[] = {"TextEdit", "VSCode", "Vim"};
    widget_t *editor_dropdown = widget_create_dropdown("Text Editor", editors, 3, 0, NULL, app);
    editor_dropdown->bounds = (rect_t){CONTENT_PADDING, y, 700, ITEM_HEIGHT};
    widget_add_to_panel(panel, editor_dropdown);
    y += ITEM_HEIGHT + 24;

    // Section: Startup Applications
    widget_t *header2 = widget_create(WIDGET_SECTION_HEADER, "Startup Applications");
    header2->bounds = (rect_t){CONTENT_PADDING, y, 700, 32};
    widget_add_to_panel(panel, header2);
    y += 40;

    // Add startup app button
    widget_t *add_startup = widget_create_button("Add Startup Application...", NULL, app);
    add_startup->bounds = (rect_t){CONTENT_PADDING, y, 250, 40};
    widget_add_to_panel(panel, add_startup);
    y += 56;

    panel->content_height = y;
}

// ============================================================================
// PRIVACY & SECURITY PANEL
// ============================================================================

void settings_create_privacy_panel(settings_app_t *app) {
    if (!app) return;

    settings_panel_t *panel = &app->panels[CATEGORY_PRIVACY];
    strncpy(panel->title, "Privacy & Security", sizeof(panel->title) - 1);
    panel->category = CATEGORY_PRIVACY;
    panel->widgets = NULL;
    panel->widget_count = 0;
    panel->scroll_offset = 0;

    uint32_t y = CONTENT_PADDING;

    // Section: Capabilities
    widget_t *header1 = widget_create(WIDGET_SECTION_HEADER, "Application Capabilities");
    header1->bounds = (rect_t){CONTENT_PADDING, y, 700, 32};
    widget_add_to_panel(panel, header1);
    y += 40;

    // Sandboxing toggle
    widget_t *sandbox = widget_create_toggle("Enable Sandboxing",
                                            app->privacy.sandboxing_enabled,
                                            on_sandboxing_toggle, app);
    sandbox->bounds = (rect_t){CONTENT_PADDING, y, 700, ITEM_HEIGHT};
    widget_add_to_panel(panel, sandbox);
    y += ITEM_HEIGHT + 8;

    // Audit violations toggle
    widget_t *audit = widget_create_toggle("Audit Sandbox Violations",
                                          app->privacy.audit_sandbox_violations,
                                          NULL, app);
    audit->bounds = (rect_t){CONTENT_PADDING, y, 700, ITEM_HEIGHT};
    widget_add_to_panel(panel, audit);
    y += ITEM_HEIGHT + 24;

    // Section: Firewall
    widget_t *header2 = widget_create(WIDGET_SECTION_HEADER, "Firewall");
    header2->bounds = (rect_t){CONTENT_PADDING, y, 700, 32};
    widget_add_to_panel(panel, header2);
    y += 40;

    // Firewall toggle
    widget_t *firewall = widget_create_toggle("Enable Firewall",
                                             app->privacy.firewall_enabled,
                                             on_firewall_toggle, app);
    firewall->bounds = (rect_t){CONTENT_PADDING, y, 700, ITEM_HEIGHT};
    widget_add_to_panel(panel, firewall);
    y += ITEM_HEIGHT + 8;

    // Block incoming toggle
    widget_t *block_incoming = widget_create_toggle("Block Incoming Connections",
                                                   app->privacy.block_incoming,
                                                   NULL, app);
    block_incoming->bounds = (rect_t){CONTENT_PADDING, y, 700, ITEM_HEIGHT};
    widget_add_to_panel(panel, block_incoming);
    y += ITEM_HEIGHT + 24;

    // Section: Privacy
    widget_t *header3 = widget_create(WIDGET_SECTION_HEADER, "Privacy");
    header3->bounds = (rect_t){CONTENT_PADDING, y, 700, 32};
    widget_add_to_panel(panel, header3);
    y += 40;

    // Location services toggle
    widget_t *location = widget_create_toggle("Location Services",
                                             app->privacy.location_services,
                                             NULL, app);
    location->bounds = (rect_t){CONTENT_PADDING, y, 700, ITEM_HEIGHT};
    widget_add_to_panel(panel, location);
    y += ITEM_HEIGHT + 8;

    // Diagnostics reporting toggle
    widget_t *diagnostics = widget_create_toggle("Diagnostics & Usage Reporting",
                                                app->privacy.diagnostics_reporting,
                                                NULL, app);
    diagnostics->bounds = (rect_t){CONTENT_PADDING, y, 700, ITEM_HEIGHT};
    widget_add_to_panel(panel, diagnostics);
    y += ITEM_HEIGHT + 8;

    panel->content_height = y;
}

// ============================================================================
// SYSTEM PANEL
// ============================================================================

void settings_create_system_panel(settings_app_t *app) {
    if (!app) return;

    settings_panel_t *panel = &app->panels[CATEGORY_SYSTEM];
    strncpy(panel->title, "System", sizeof(panel->title) - 1);
    panel->category = CATEGORY_SYSTEM;
    panel->widgets = NULL;
    panel->widget_count = 0;
    panel->scroll_offset = 0;

    uint32_t y = CONTENT_PADDING;

    // Section: About
    widget_t *header1 = widget_create(WIDGET_SECTION_HEADER, "About This System");
    header1->bounds = (rect_t){CONTENT_PADDING, y, 700, 32};
    widget_add_to_panel(panel, header1);
    y += 40;

    // OS info labels
    char os_info[256];
    snprintf(os_info, sizeof(os_info), "%s %s",
             app->system.info.os_name, app->system.info.os_version);
    widget_t *os_label = widget_create(WIDGET_LABEL, os_info);
    os_label->bounds = (rect_t){CONTENT_PADDING, y, 700, 24};
    widget_add_to_panel(panel, os_label);
    y += 28;

    char cpu_info[256];
    snprintf(cpu_info, sizeof(cpu_info), "%s (%u cores)",
             app->system.info.cpu_model, app->system.info.cpu_cores);
    widget_t *cpu_label = widget_create(WIDGET_LABEL, cpu_info);
    cpu_label->bounds = (rect_t){CONTENT_PADDING, y, 700, 24};
    widget_add_to_panel(panel, cpu_label);
    y += 28;

    char mem_info[256];
    snprintf(mem_info, sizeof(mem_info), "Memory: %lu MB / %lu MB",
             app->system.info.memory_used_mb, app->system.info.memory_total_mb);
    widget_t *mem_label = widget_create(WIDGET_LABEL, mem_info);
    mem_label->bounds = (rect_t){CONTENT_PADDING, y, 700, 24};
    widget_add_to_panel(panel, mem_label);
    y += 28;

    char disk_info[256];
    snprintf(disk_info, sizeof(disk_info), "Disk: %lu GB / %lu GB",
             app->system.info.disk_used_gb, app->system.info.disk_total_gb);
    widget_t *disk_label = widget_create(WIDGET_LABEL, disk_info);
    disk_label->bounds = (rect_t){CONTENT_PADDING, y, 700, 24};
    widget_add_to_panel(panel, disk_label);
    y += 36;

    // Section: Software Update
    widget_t *header2 = widget_create(WIDGET_SECTION_HEADER, "Software Update");
    header2->bounds = (rect_t){CONTENT_PADDING, y, 700, 32};
    widget_add_to_panel(panel, header2);
    y += 40;

    // Auto-check updates toggle
    widget_t *auto_update = widget_create_toggle("Automatically Check for Updates",
                                                 app->system.auto_check_updates,
                                                 on_auto_updates_toggle, app);
    auto_update->bounds = (rect_t){CONTENT_PADDING, y, 700, ITEM_HEIGHT};
    widget_add_to_panel(panel, auto_update);
    y += ITEM_HEIGHT + 8;

    // Check now button
    widget_t *check_btn = widget_create_button("Check for Updates Now",
                                               on_check_updates_click, app);
    check_btn->bounds = (rect_t){CONTENT_PADDING, y, 220, 40};
    widget_add_to_panel(panel, check_btn);
    y += 56;

    // Section: Developer Options
    widget_t *header3 = widget_create(WIDGET_SECTION_HEADER, "Developer Options");
    header3->bounds = (rect_t){CONTENT_PADDING, y, 700, 32};
    widget_add_to_panel(panel, header3);
    y += 40;

    // Developer mode toggle
    widget_t *dev_mode = widget_create_toggle("Enable Developer Mode",
                                             app->system.developer_mode,
                                             on_developer_mode_toggle, app);
    dev_mode->bounds = (rect_t){CONTENT_PADDING, y, 700, ITEM_HEIGHT};
    widget_add_to_panel(panel, dev_mode);
    y += ITEM_HEIGHT + 8;

    // Debug info toggle
    widget_t *debug_info = widget_create_toggle("Show Debug Information",
                                               app->system.show_debug_info,
                                               NULL, app);
    debug_info->bounds = (rect_t){CONTENT_PADDING, y, 700, ITEM_HEIGHT};
    widget_add_to_panel(panel, debug_info);
    y += ITEM_HEIGHT + 8;

    panel->content_height = y;
}
