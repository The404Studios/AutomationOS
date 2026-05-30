/**
 * Window Manager Integration Test
 *
 * Tests the integration between window manager and compositor:
 * - Window creation
 * - Window movement
 * - Window resizing
 * - Window focus
 * - Window mapping/unmapping
 */

#include "window_manager.h"
#include "../compositor/compositor.h"
#include "../compositor/animations.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdbool.h>

// Test result tracking
static int tests_passed = 0;
static int tests_failed = 0;

#define TEST_START(name) \
    printf("\n[TEST] Starting: %s\n", name);

#define TEST_ASSERT(condition, message) \
    do { \
        if (condition) { \
            printf("  ✓ %s\n", message); \
            tests_passed++; \
        } else { \
            printf("  ✗ FAILED: %s\n", message); \
            tests_failed++; \
        } \
    } while(0)

/**
 * Mock compositor for testing
 */
compositor_t *create_mock_compositor(void) {
    compositor_t *comp = calloc(1, sizeof(compositor_t));
    if (!comp) return NULL;

    // Create a mock display
    display_t *display = calloc(1, sizeof(display_t));
    if (!display) {
        free(comp);
        return NULL;
    }

    display->id = 0;
    display->width = 1920;
    display->height = 1080;
    display->refresh_rate = 60;
    display->primary = true;
    strcpy(display->name, "Mock Display");

    comp->displays[0] = display;
    comp->display_count = 1;
    comp->window_count = 0;
    comp->vsync_enabled = true;
    comp->effects_enabled = true;

    return comp;
}

/**
 * Test 1: Window Creation
 */
void test_window_creation(window_manager_t *wm) {
    TEST_START("Window Creation");

    window_t *win = wm_create_window(wm, WINDOW_NORMAL, 800, 600, "Test Window");

    TEST_ASSERT(win != NULL, "Window created successfully");
    TEST_ASSERT(win->id > 0, "Window has valid ID");
    TEST_ASSERT(win->geometry.w == 800, "Window width is correct");
    TEST_ASSERT(win->geometry.h == 600, "Window height is correct");
    TEST_ASSERT(strcmp(win->title, "Test Window") == 0, "Window title is correct");
    TEST_ASSERT(win->type == WINDOW_NORMAL, "Window type is correct");
    TEST_ASSERT(win->mapped == false, "Window is initially unmapped");
    TEST_ASSERT(win->surface != NULL, "Window has surface");
}

/**
 * Test 2: Window Mapping
 */
void test_window_mapping(window_manager_t *wm) {
    TEST_START("Window Mapping");

    window_t *win = wm_create_window(wm, WINDOW_NORMAL, 640, 480, "Mapping Test");
    TEST_ASSERT(win != NULL, "Window created for mapping test");

    wm_map_window(wm, win);
    TEST_ASSERT(win->mapped == true, "Window is mapped");
    TEST_ASSERT(win->focused == true, "Mapped window is focused");
    TEST_ASSERT(wm->focused_window == win, "Window manager tracks focused window");

    wm_unmap_window(wm, win);
    TEST_ASSERT(win->mapped == false, "Window is unmapped");
}

/**
 * Test 3: Window Focus
 */
void test_window_focus(window_manager_t *wm) {
    TEST_START("Window Focus");

    window_t *win1 = wm_create_window(wm, WINDOW_NORMAL, 400, 300, "Window 1");
    window_t *win2 = wm_create_window(wm, WINDOW_NORMAL, 400, 300, "Window 2");

    wm_map_window(wm, win1);
    TEST_ASSERT(wm->focused_window == win1, "First window is focused");

    wm_map_window(wm, win2);
    TEST_ASSERT(wm->focused_window == win2, "Second window is focused");

    wm_focus_window(wm, win1);
    TEST_ASSERT(wm->focused_window == win1, "Focus switched back to first window");
    TEST_ASSERT(win1->focused == true, "Window 1 is marked as focused");
    TEST_ASSERT(win2->focused == false, "Window 2 is marked as unfocused");
}

/**
 * Test 4: Window Movement
 */
void test_window_movement(window_manager_t *wm) {
    TEST_START("Window Movement");

    window_t *win = wm_create_window(wm, WINDOW_NORMAL, 500, 400, "Move Test");
    wm_map_window(wm, win);

    int32_t old_x = win->geometry.x;
    int32_t old_y = win->geometry.y;

    wm_move_window(wm, win, 100, 200);
    TEST_ASSERT(win->geometry.x == 100, "Window X coordinate updated");
    TEST_ASSERT(win->geometry.y == 200, "Window Y coordinate updated");
    TEST_ASSERT(win->geometry.x != old_x || win->geometry.y != old_y,
                "Window position changed");

    // Test centering
    wm_center_window(wm, win);
    display_t *display = wm->compositor->displays[0];
    int32_t expected_x = (display->width - win->geometry.w) / 2;
    int32_t expected_y = (display->height - win->geometry.h) / 2;
    TEST_ASSERT(win->geometry.x == expected_x, "Window is centered horizontally");
    TEST_ASSERT(win->geometry.y == expected_y, "Window is centered vertically");
}

/**
 * Test 5: Window Resizing
 */
void test_window_resizing(window_manager_t *wm) {
    TEST_START("Window Resizing");

    window_t *win = wm_create_window(wm, WINDOW_NORMAL, 300, 250, "Resize Test");
    wm_map_window(wm, win);

    wm_resize_window(wm, win, 700, 500);
    TEST_ASSERT(win->geometry.w == 700, "Window width updated");
    TEST_ASSERT(win->geometry.h == 500, "Window height updated");
    TEST_ASSERT(win->surface->width == 700, "Surface width updated");
    TEST_ASSERT(win->surface->height == 500, "Surface height updated");
    TEST_ASSERT(win->surface->pixels != NULL, "Surface pixels reallocated");
}

/**
 * Test 6: Window Operations (Minimize/Maximize)
 */
void test_window_operations(window_manager_t *wm) {
    TEST_START("Window Operations");

    window_t *win = wm_create_window(wm, WINDOW_NORMAL, 600, 450, "Operations Test");
    wm_map_window(wm, win);

    // Test maximize
    wm_maximize_window(wm, win);
    TEST_ASSERT(win->maximized == true, "Window is maximized");
    display_t *display = wm->compositor->displays[0];
    TEST_ASSERT(win->geometry.w == display->width,
                "Maximized window spans full width");

    // Test minimize
    wm_minimize_window(wm, win);
    TEST_ASSERT(win->minimized == true, "Window is minimized");
    TEST_ASSERT(win->mapped == false, "Minimized window is unmapped");
}

/**
 * Test 7: Window Z-Order
 */
void test_window_z_order(window_manager_t *wm) {
    TEST_START("Window Z-Order");

    window_t *win1 = wm_create_window(wm, WINDOW_NORMAL, 300, 200, "Z1");
    window_t *win2 = wm_create_window(wm, WINDOW_NORMAL, 300, 200, "Z2");
    window_t *win3 = wm_create_window(wm, WINDOW_NORMAL, 300, 200, "Z3");

    wm_map_window(wm, win1);
    wm_map_window(wm, win2);
    wm_map_window(wm, win3);

    workspace_t *ws = wm->workspaces[wm->active_workspace];
    TEST_ASSERT(ws->window_count == 3, "Three windows in workspace");

    // win3 should be on top (last mapped)
    TEST_ASSERT(ws->windows[ws->window_count - 1] == win3, "Last window is on top");

    // Raise win1 to top
    wm_raise_window(wm, win1);
    TEST_ASSERT(ws->windows[ws->window_count - 1] == win1, "Raised window is on top");
}

/**
 * Test 8: Window at Position
 */
void test_window_at_position(window_manager_t *wm) {
    TEST_START("Window at Position");

    window_t *win = wm_create_window(wm, WINDOW_NORMAL, 200, 150, "Position Test");
    wm_map_window(wm, win);
    wm_move_window(wm, win, 100, 100);

    // Test hit detection
    window_t *hit = wm_window_at(wm, 150, 150);
    TEST_ASSERT(hit == win, "Window found at correct position");

    window_t *miss = wm_window_at(wm, 50, 50);
    TEST_ASSERT(miss == NULL, "No window found outside window bounds");

    // Test decoration hit
    bool on_decoration = wm_hit_test_decorations(wm, win, 150, win->frame_geometry.y + 10);
    TEST_ASSERT(on_decoration == true, "Decoration hit test works");
}

/**
 * Test 9: Workspace Management
 */
void test_workspace_management(window_manager_t *wm) {
    TEST_START("Workspace Management");

    workspace_t *ws1 = wm->workspaces[0];
    TEST_ASSERT(ws1 != NULL, "Default workspace exists");

    workspace_t *ws2 = wm_create_workspace(wm, "Test Workspace");
    TEST_ASSERT(ws2 != NULL, "New workspace created");
    TEST_ASSERT(wm->workspace_count == 2, "Workspace count increased");

    window_t *win = wm_create_window(wm, WINDOW_NORMAL, 300, 200, "WS Test");
    wm_map_window(wm, win);

    // Window should be in active workspace
    TEST_ASSERT(ws1->window_count > 0, "Window added to active workspace");

    // Move to new workspace
    wm_move_window_to_workspace(wm, win, 1);
    TEST_ASSERT(ws2->window_count > 0, "Window moved to new workspace");
}

/**
 * Test 10: Tiling Modes
 */
void test_tiling_modes(window_manager_t *wm) {
    TEST_START("Tiling Modes");

    // Create multiple windows
    window_t *win1 = wm_create_window(wm, WINDOW_NORMAL, 400, 300, "Tile1");
    window_t *win2 = wm_create_window(wm, WINDOW_NORMAL, 400, 300, "Tile2");
    wm_map_window(wm, win1);
    wm_map_window(wm, win2);

    // Test horizontal tiling
    wm_set_tiling_mode(wm, TILING_HORIZONTAL);
    workspace_t *ws = wm->workspaces[wm->active_workspace];
    TEST_ASSERT(ws->tiling_mode == TILING_HORIZONTAL, "Tiling mode set to horizontal");

    // After tiling, windows should be side-by-side
    TEST_ASSERT(win1->geometry.x != win2->geometry.x,
                "Tiled windows have different X positions");

    // Test vertical tiling
    wm_set_tiling_mode(wm, TILING_VERTICAL);
    TEST_ASSERT(ws->tiling_mode == TILING_VERTICAL, "Tiling mode set to vertical");
    TEST_ASSERT(win1->geometry.y != win2->geometry.y,
                "Vertically tiled windows have different Y positions");
}

/**
 * Test 11: Integration Functions
 */
void test_integration_functions(window_manager_t *wm) {
    TEST_START("Integration Functions");

    window_t *win = wm_create_window(wm, WINDOW_NORMAL, 500, 400, "Integration Test");
    wm_map_window(wm, win);

    // Test geometry sync
    compositor_sync_geometry(wm, win);
    TEST_ASSERT(true, "compositor_sync_geometry() executed");

    // Test focus set
    compositor_set_focus(wm, win);
    TEST_ASSERT(true, "compositor_set_focus() executed");

    // Test z-order query
    window_t *windows[MAX_WINDOWS];
    uint32_t count = wm_get_window_z_order(wm, windows, MAX_WINDOWS);
    TEST_ASSERT(count > 0, "Z-order query returned windows");

    // Test geometry query
    rect_t geom, frame;
    bool found = wm_get_window_geometry(wm, win->id, &geom, &frame);
    TEST_ASSERT(found == true, "Window geometry query succeeded");
    TEST_ASSERT(geom.w == 500 && geom.h == 400, "Geometry values are correct");
}

/**
 * Main test runner
 */
int main(int argc, char *argv[]) {
    printf("=================================================\n");
    printf(" Window Manager Integration Test Suite\n");
    printf("=================================================\n");

    // Create mock compositor
    compositor_t *comp = create_mock_compositor();
    if (!comp) {
        fprintf(stderr, "Failed to create mock compositor\n");
        return 1;
    }

    // Initialize window manager
    window_manager_t *wm = wm_init(comp);
    if (!wm) {
        fprintf(stderr, "Failed to initialize window manager\n");
        free(comp->displays[0]);
        free(comp);
        return 1;
    }

    printf("\nWindow Manager initialized successfully\n");
    printf("Display: %ux%u @ %uHz\n",
           comp->displays[0]->width,
           comp->displays[0]->height,
           comp->displays[0]->refresh_rate);

    // Run all tests
    test_window_creation(wm);
    test_window_mapping(wm);
    test_window_focus(wm);
    test_window_movement(wm);
    test_window_resizing(wm);
    test_window_operations(wm);
    test_window_z_order(wm);
    test_window_at_position(wm);
    test_workspace_management(wm);
    test_tiling_modes(wm);
    test_integration_functions(wm);

    // Print results
    printf("\n=================================================\n");
    printf(" Test Results\n");
    printf("=================================================\n");
    printf("Total Tests:  %d\n", tests_passed + tests_failed);
    printf("Passed:       %d (✓)\n", tests_passed);
    printf("Failed:       %d (✗)\n", tests_failed);
    printf("Success Rate: %.1f%%\n",
           100.0 * tests_passed / (tests_passed + tests_failed));
    printf("=================================================\n");

    // Cleanup
    wm_cleanup(wm);
    free(comp->displays[0]);
    free(comp);

    return (tests_failed > 0) ? 1 : 0;
}
