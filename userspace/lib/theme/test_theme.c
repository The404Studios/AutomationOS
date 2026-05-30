/**
 * Theme Engine Test Program
 *
 * Tests theme loading, parsing, and API functionality
 */

#include "theme.h"
#include "parser.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void print_color(const char *name, color_rgba_t color) {
    printf("  %s: #%02X%02X%02X", name, color.r, color.g, color.b);
    if (color.a != 255) {
        printf"%02X", color.a);
    }
    printf("\n");
}

void print_theme_info(const theme_t *theme) {
    printf("\n=== Theme Information ===\n");
    printf("Name: %s\n", theme->meta.name);
    printf("Author: %s\n", theme->meta.author);
    printf("Description: %s\n", theme->meta.description);
    printf("Version: %s\n", theme->meta.version);
    printf("Mode: %s\n",
           theme->meta.mode == THEME_MODE_LIGHT ? "Light" :
           theme->meta.mode == THEME_MODE_DARK ? "Dark" : "Auto");

    printf("\n=== Colors ===\n");
    print_color("Primary", theme->colors.primary);
    print_color("Success", theme->colors.success);
    print_color("Warning", theme->colors.warning);
    print_color("Error", theme->colors.error);
    print_color("Background", theme->colors.bg_primary);
    print_color("Text", theme->colors.text_primary);

    printf("\n=== Window Decorations ===\n");
    printf("Titlebar Height: %u\n", theme->window.titlebar_height);
    printf("Border Width: %u\n", theme->window.border_width);
    printf("Corner Radius: %u\n", theme->window.corner_radius);
    printf("Traffic Lights: %s\n", theme->window.show_traffic_lights ? "Yes" : "No");

    printf("\n=== Panel ===\n");
    printf("Height: %u\n", theme->panel.height);
    printf("Padding: %u\n", theme->panel.padding);
    printf("Icon Size: %u\n", theme->panel.icon_size);

    printf("\n=== Dock ===\n");
    printf("Icon Size: %u\n", theme->dock.icon_size);
    printf("Padding: %u\n", theme->dock.padding);
    printf("Margin: %u\n", theme->dock.margin);
    printf("Magnification: %s\n", theme->dock.magnification_enabled ? "Enabled" : "Disabled");

    printf("\n=== Animations ===\n");
    printf("Enabled: %s\n", theme->animations.enabled ? "Yes" : "No");
    printf("Speed: %.2fx\n", theme->animations.speed_multiplier);

    printf("\n=== Accessibility ===\n");
    printf("High Contrast: %s\n", theme->accessibility.high_contrast ? "Yes" : "No");
    printf("Reduce Motion: %s\n", theme->accessibility.reduce_motion ? "Yes" : "No");
    printf("Text Scale: %.2fx\n", theme->accessibility.text_scale);
}

void test_theme_validation(const theme_t *theme) {
    printf("\n=== Accessibility Validation ===\n");

    if (theme_validate_accessibility(theme)) {
        printf("✓ Theme passes WCAG AA accessibility standards\n");
    } else {
        printf("✗ Theme has accessibility issues:\n");
        char report[1024];
        int issues = theme_get_validation_report(theme, report, sizeof(report));
        printf("%s", report);
        printf("Total issues: %d\n", issues);
    }
}

void on_theme_changed(const theme_t *new_theme, void *user_data) {
    printf("\n>>> THEME CHANGE NOTIFICATION <<<\n");
    printf("New theme: %s\n", new_theme->meta.name);
    (void)user_data;
}

void test_theme_engine() {
    printf("\n========================================\n");
    printf("Theme Engine Test\n");
    printf("========================================\n");

    // Initialize engine
    printf("\nInitializing theme engine...\n");
    theme_engine_t *engine = theme_engine_init();
    if (!engine) {
        printf("✗ Failed to initialize engine\n");
        return;
    }
    printf("✓ Engine initialized\n");

    // Register callback
    printf("\nRegistering theme change callback...\n");
    theme_register_callback(engine, on_theme_changed, NULL);
    printf("✓ Callback registered\n");

    // Test default light theme
    printf("\n=== Testing Default Light Theme ===\n");
    const theme_t *current = theme_get_active(engine);
    print_theme_info(current);
    test_theme_validation(current);

    // Create and test dark theme
    printf("\n=== Creating Default Dark Theme ===\n");
    theme_t *dark = theme_create_default_dark();
    if (dark) {
        print_theme_info(dark);
        test_theme_validation(dark);
        theme_free(dark);
    }

    // Test query API
    printf("\n=== Testing Query API ===\n");
    color_rgba_t primary = theme_get_color(engine, "primary");
    printf("Primary color: #%02X%02X%02X\n", primary.r, primary.g, primary.b);

    uint32_t titlebar_h, border_w, corner_r;
    theme_get_window_settings(engine, &titlebar_h, &border_w, &corner_r);
    printf("Window: titlebar=%u, border=%u, radius=%u\n",
           titlebar_h, border_w, corner_r);

    printf("Animations enabled: %s\n",
           theme_animations_enabled(engine) ? "Yes" : "No");
    printf("Animation speed: %.2fx\n", theme_animation_speed(engine));

    // Cleanup
    printf("\nCleaning up...\n");
    theme_engine_cleanup(engine);
    printf("✓ Cleanup complete\n");
}

void test_parser(const char *path) {
    printf("\n========================================\n");
    printf("Theme Parser Test\n");
    printf("========================================\n");
    printf("File: %s\n", path);

    // Validate syntax first
    printf("\nValidating syntax...\n");
    char errors[1024];
    if (theme_validate_syntax(path, errors, sizeof(errors))) {
        printf("✓ Syntax valid\n");
    } else {
        printf("✗ Syntax errors:\n%s", errors);
        return;
    }

    // Parse theme
    printf("\nParsing theme...\n");
    theme_t *theme = theme_parse_file(path);
    if (!theme) {
        printf("✗ Parse failed:\n");
        theme_get_parse_errors(errors, sizeof(errors));
        printf("%s", errors);
        return;
    }
    printf("✓ Parse successful\n");

    // Display theme info
    print_theme_info(theme);
    test_theme_validation(theme);

    // Test serialization
    printf("\n=== Testing Serialization ===\n");
    char output[16384];
    int len = theme_generate_string(theme, output, sizeof(output));
    if (len > 0) {
        printf("✓ Generated %d bytes\n", len);
        printf("\nFirst 500 characters:\n");
        printf("%.500s\n", output);
    } else {
        printf("✗ Serialization failed\n");
    }

    theme_free(theme);
}

void test_color_parsing() {
    printf("\n========================================\n");
    printf("Color Parser Test\n");
    printf("========================================\n");

    const char *test_colors[] = {
        "#FF0000",
        "#00FF00AA",
        "rgb(255, 128, 0)",
        "rgba(128, 0, 255, 200)",
    };

    for (size_t i = 0; i < sizeof(test_colors) / sizeof(test_colors[0]); i++) {
        const char *input = test_colors[i];
        parser_t *parser = parser_init(input, strlen(input));

        color_rgba_t color;
        if (parser_parse_color(parser, &color)) {
            printf("✓ '%s' -> RGBA(%u, %u, %u, %u)\n",
                   input, color.r, color.g, color.b, color.a);
        } else {
            printf("✗ Failed to parse '%s'\n", input);
        }

        parser_cleanup(parser);
    }
}

int main(int argc, char **argv) {
    printf("\n");
    printf("╔═══════════════════════════════════════════════╗\n");
    printf("║   AutomationOS Theme Engine Test Suite       ║\n");
    printf("╚═══════════════════════════════════════════════╝\n");

    // Test 1: Theme engine functionality
    test_theme_engine();

    // Test 2: Color parsing
    test_color_parsing();

    // Test 3: Parse theme file if provided
    if (argc > 1) {
        test_parser(argv[1]);
    } else {
        printf("\nNote: Provide theme file path to test parser\n");
        printf("Usage: %s <path/to/theme.conf>\n", argv[0]);
    }

    printf("\n╔═══════════════════════════════════════════════╗\n");
    printf("║   All Tests Complete                          ║\n");
    printf("╚═══════════════════════════════════════════════╝\n\n");

    return 0;
}
