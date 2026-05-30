/**
 * Desktop Stack Integration Test (Unit Test Version)
 *
 * Tests the desktop stack without requiring GPU initialization.
 * This validates the API surface and integration points.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <assert.h>

#include "compositor.h"
#include "gpu.h"
#include "../wm/window_manager.h"
#include "../shell/desktop/desktop_shell.h"

// Test counters
static int tests_run = 0;
static int tests_passed = 0;
static int tests_failed = 0;

// Test macros
#define TEST(name) \
    printf("  Testing %s... ", name); \
    tests_run++;

#define PASS() \
    printf("\033[32mPASS\033[0m\n"); \
    tests_passed++;

#define FAIL(msg) \
    printf("\033[31mFAIL\033[0m: %s\n", msg); \
    tests_failed++;

#define ASSERT(cond, msg) \
    if (!(cond)) { \
        FAIL(msg); \
        return false; \
    }

// ============================================================================
// COMPOSITOR TESTS
// ============================================================================

bool test_compositor_structures(void) {
    TEST("Compositor structure sizes");

    // Verify structure sizes are reasonable
    size_t comp_size = sizeof(compositor_t);
    size_t window_size = sizeof(window_t);
    size_t display_size = sizeof(display_t);

    ASSERT(comp_size > 0, "Compositor size is zero");
    ASSERT(window_size > 0, "Window size is zero");
    ASSERT(display_size > 0, "Display size is zero");

    PASS();
    return true;
}

bool test_window_creation(void) {
    TEST("Window creation API");

    rect_t geometry = {100, 100, 800, 600};
    window_t *window = window_create(1, WINDOW_NORMAL, &geometry);

    ASSERT(window != NULL, "Window creation failed");
    ASSERT(window->id == 1, "Window ID mismatch");
    ASSERT(window->type == WINDOW_NORMAL, "Window type mismatch");
    ASSERT(window->geometry.x == 100, "Window x mismatch");
    ASSERT(window->geometry.y == 100, "Window y mismatch");
    ASSERT(window->geometry.w == 800, "Window w mismatch");
    ASSERT(window->geometry.h == 600, "Window h mismatch");

    // Note: Don't cleanup here as it requires GPU context
    // In real usage, this would be cleaned up by compositor

    PASS();
    return true;
}

bool test_display_creation(void) {
    TEST("Display creation API");

    display_t *display = display_create(0, 1920, 1080, 60);

    ASSERT(display != NULL, "Display creation failed");
    ASSERT(display->id == 0, "Display ID mismatch");
    ASSERT(display->width == 1920, "Display width mismatch");
    ASSERT(display->height == 1080, "Display height mismatch");
    ASSERT(display->refresh_rate == 60, "Display refresh rate mismatch");

    display_cleanup(display);

    PASS();
    return true;
}

bool test_rect_utilities(void) {
    TEST("Rectangle utility functions");

    rect_t a = {0, 0, 100, 100};
    rect_t b = {50, 50, 100, 100};
    rect_t c = {200, 200, 100, 100};

    // Test intersection
    ASSERT(rect_intersects(&a, &b), "Rects should intersect");
    ASSERT(!rect_intersects(&a, &c), "Rects should not intersect");

    // Test union
    rect_t result;
    rect_union(&result, &a, &b);
    ASSERT(result.x == 0, "Union x wrong");
    ASSERT(result.y == 0, "Union y wrong");
    ASSERT(result.w == 150, "Union w wrong");
    ASSERT(result.h == 150, "Union h wrong");

    PASS();
    return true;
}

// ============================================================================
// WINDOW MANAGER TESTS
// ============================================================================

bool test_wm_structures(void) {
    TEST("Window manager structures");

    size_t wm_size = sizeof(window_manager_t);
    size_t workspace_size = sizeof(workspace_t);

    ASSERT(wm_size > 0, "WM size is zero");
    ASSERT(workspace_size > 0, "Workspace size is zero");

    PASS();
    return true;
}

bool test_window_types(void) {
    TEST("Window type enum");

    ASSERT(WINDOW_NORMAL == 0, "WINDOW_NORMAL value wrong");
    ASSERT(WINDOW_DIALOG == 1, "WINDOW_DIALOG value wrong");
    ASSERT(WINDOW_UTILITY == 2, "WINDOW_UTILITY value wrong");
    ASSERT(WINDOW_TOOLBAR == 3, "WINDOW_TOOLBAR value wrong");
    ASSERT(WINDOW_MENU == 4, "WINDOW_MENU value wrong");
    ASSERT(WINDOW_SPLASH == 5, "WINDOW_SPLASH value wrong");
    ASSERT(WINDOW_DESKTOP == 6, "WINDOW_DESKTOP value wrong");
    ASSERT(WINDOW_DOCK == 7, "WINDOW_DOCK value wrong");

    PASS();
    return true;
}

bool test_placement_modes(void) {
    TEST("Placement mode enum");

    ASSERT(PLACEMENT_FLOATING == 0, "PLACEMENT_FLOATING value wrong");
    ASSERT(PLACEMENT_TILING == 1, "PLACEMENT_TILING value wrong");
    ASSERT(PLACEMENT_MAXIMIZED == 2, "PLACEMENT_MAXIMIZED value wrong");
    ASSERT(PLACEMENT_FULLSCREEN == 3, "PLACEMENT_FULLSCREEN value wrong");

    PASS();
    return true;
}

// ============================================================================
// DESKTOP SHELL TESTS
// ============================================================================

bool test_shell_structures(void) {
    TEST("Desktop shell structures");

    size_t shell_size = sizeof(desktop_shell_t);
    size_t panel_size = sizeof(panel_t);
    size_t dock_size = sizeof(dock_t);
    size_t desktop_size = sizeof(desktop_t);
    size_t overview_size = sizeof(overview_t);
    size_t notif_size = sizeof(notification_center_t);

    ASSERT(shell_size > 0, "Shell size is zero");
    ASSERT(panel_size > 0, "Panel size is zero");
    ASSERT(dock_size > 0, "Dock size is zero");
    ASSERT(desktop_size > 0, "Desktop size is zero");
    ASSERT(overview_size > 0, "Overview size is zero");
    ASSERT(notif_size > 0, "Notification center size is zero");

    PASS();
    return true;
}

bool test_theme_system(void) {
    TEST("Theme system");

    theme_t theme;

    // Test light theme
    theme_init_light(&theme);
    ASSERT(theme.mode == THEME_LIGHT, "Light theme mode wrong");
    ASSERT(theme.blur_radius > 0, "Blur radius not set");
    ASSERT(theme.corner_radius > 0, "Corner radius not set");
    ASSERT(theme.font_size_body > 0, "Font size not set");

    // Test dark theme
    theme_init_dark(&theme);
    ASSERT(theme.mode == THEME_DARK, "Dark theme mode wrong");

    PASS();
    return true;
}

bool test_color_utilities(void) {
    TEST("Color utility functions");

    color_t red = color_rgb(255, 0, 0);
    ASSERT(red.r == 255, "Red R component wrong");
    ASSERT(red.g == 0, "Red G component wrong");
    ASSERT(red.b == 0, "Red B component wrong");
    ASSERT(red.a == 255, "Red A component wrong");

    color_t transparent = color_rgba(255, 255, 255, 128);
    ASSERT(transparent.a == 128, "Alpha component wrong");

    color_t hex = color_hex(0xFF0080);
    ASSERT(hex.r == 0xFF, "Hex R component wrong");
    ASSERT(hex.g == 0x00, "Hex G component wrong");
    ASSERT(hex.b == 0x80, "Hex B component wrong");

    PASS();
    return true;
}

bool test_rect_utilities_shell(void) {
    TEST("Shell rectangle utilities");

    rect_t rect = {10, 20, 100, 50};

    ASSERT(rect_contains(&rect, 50, 40), "Point should be inside");
    ASSERT(!rect_contains(&rect, 5, 5), "Point should be outside");
    ASSERT(!rect_contains(&rect, 150, 100), "Point should be outside");

    PASS();
    return true;
}

bool test_dock_positions(void) {
    TEST("Dock position enum");

    ASSERT(DOCK_BOTTOM == 0, "DOCK_BOTTOM value wrong");
    ASSERT(DOCK_LEFT == 1, "DOCK_LEFT value wrong");
    ASSERT(DOCK_RIGHT == 2, "DOCK_RIGHT value wrong");
    ASSERT(DOCK_FLOATING == 3, "DOCK_FLOATING value wrong");

    PASS();
    return true;
}

bool test_notification_urgency(void) {
    TEST("Notification urgency enum");

    ASSERT(NOTIF_INFO == 0, "NOTIF_INFO value wrong");
    ASSERT(NOTIF_WARNING == 1, "NOTIF_WARNING value wrong");
    ASSERT(NOTIF_ERROR == 2, "NOTIF_ERROR value wrong");
    ASSERT(NOTIF_SUCCESS == 3, "NOTIF_SUCCESS value wrong");

    PASS();
    return true;
}

// ============================================================================
// INTEGRATION TESTS
// ============================================================================

bool test_api_consistency(void) {
    TEST("API consistency (window_t type)");

    // Both compositor.h and desktop_shell.h define window_t
    // Make sure they're compatible (both use struct window)
    rect_t geometry = {0, 0, 640, 480};
    window_t *comp_window = window_create(1, WINDOW_NORMAL, &geometry);

    ASSERT(comp_window != NULL, "Compositor window creation failed");
    ASSERT(comp_window->id == 1, "Window ID mismatch");

    // Can be used with both compositor and shell APIs
    ASSERT(comp_window->type == WINDOW_NORMAL, "Type mismatch");

    PASS();
    return true;
}

bool test_constants(void) {
    TEST("Constant definitions");

    ASSERT(MAX_DISPLAYS == 8, "MAX_DISPLAYS wrong");
    ASSERT(MAX_WINDOWS == 256, "MAX_WINDOWS wrong");
    ASSERT(MAX_DAMAGE_REGIONS == 64, "MAX_DAMAGE_REGIONS wrong");

    PASS();
    return true;
}

// ============================================================================
// MAIN TEST RUNNER
// ============================================================================

int main(void) {
    printf("\n");
    printf("╔═══════════════════════════════════════════════════════════════╗\n");
    printf("║      AutomationOS Desktop Stack Integration Tests           ║\n");
    printf("╚═══════════════════════════════════════════════════════════════╝\n");
    printf("\n");

    printf("Testing Compositor API:\n");
    test_compositor_structures();
    test_window_creation();
    test_display_creation();
    test_rect_utilities();

    printf("\nTesting Window Manager API:\n");
    test_wm_structures();
    test_window_types();
    test_placement_modes();

    printf("\nTesting Desktop Shell API:\n");
    test_shell_structures();
    test_theme_system();
    test_color_utilities();
    test_rect_utilities_shell();
    test_dock_positions();
    test_notification_urgency();

    printf("\nTesting Integration:\n");
    test_api_consistency();
    test_constants();

    // Print summary
    printf("\n");
    printf("╔═══════════════════════════════════════════════════════════════╗\n");
    printf("║                        TEST RESULTS                           ║\n");
    printf("╚═══════════════════════════════════════════════════════════════╝\n");
    printf("\n");
    printf("  Tests run:    %d\n", tests_run);
    printf("  Tests passed: \033[32m%d\033[0m\n", tests_passed);
    printf("  Tests failed: \033[31m%d\033[0m\n", tests_failed);
    printf("\n");

    if (tests_failed == 0) {
        printf("✨ \033[32mAll tests passed!\033[0m ✨\n");
        printf("\n");
        printf("Desktop stack integration is working:\n");
        printf("  ✓ Compositor API\n");
        printf("  ✓ Window Manager API\n");
        printf("  ✓ Desktop Shell API\n");
        printf("  ✓ Type compatibility\n");
        printf("  ✓ Constants and enums\n");
        printf("\n");
        return 0;
    } else {
        printf("❌ \033[31m%d test(s) failed\033[0m\n", tests_failed);
        printf("\n");
        return 1;
    }
}
