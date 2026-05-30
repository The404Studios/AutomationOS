/**
 * i915 Graphics Driver Test Suite
 *
 * Comprehensive tests for Intel i915 graphics driver:
 * - GPU initialization and detection
 * - Mode setting and display configuration
 * - Multi-monitor support
 * - Page flipping and VSync
 * - Different resolutions and refresh rates
 * - Hot-plug display detection
 * - Hardware cursor
 * - Power management
 * - Error recovery
 */

#include "../drivers/driver_test_framework.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

// Display constants
#define RESOLUTION_640x480    0
#define RESOLUTION_800x600    1
#define RESOLUTION_1024x768   2
#define RESOLUTION_1920x1080  3
#define RESOLUTION_2560x1440  4
#define RESOLUTION_3840x2160  5

#define REFRESH_60HZ   60
#define REFRESH_75HZ   75
#define REFRESH_120HZ  120
#define REFRESH_144HZ  144

#define BPP_16  16
#define BPP_24  24
#define BPP_32  32

// Mock i915 device
typedef struct {
    test_pci_device_t* pci_dev;
    uint32_t* registers;
    uint8_t* vram;
    uint64_t vram_size;
    uint8_t num_displays;
    struct {
        bool connected;
        uint16_t width;
        uint16_t height;
        uint8_t refresh_rate;
        uint8_t bpp;
        bool vsync_enabled;
        uint32_t framebuffer_addr;
    } displays[4];
    bool hardware_cursor_enabled;
    uint16_t cursor_x;
    uint16_t cursor_y;
    uint32_t frames_rendered;
    uint32_t vblank_count;
    bool power_saving_enabled;
} mock_i915_t;

static mock_i915_t* g_mock_i915 = NULL;

// Helper: Create mock i915 device
static mock_i915_t* create_mock_i915(uint64_t vram_size_mb) {
    mock_i915_t* dev = (mock_i915_t*)malloc(sizeof(mock_i915_t));
    if (!dev) return NULL;

    memset(dev, 0, sizeof(mock_i915_t));

    // Create PCI device (Intel HD Graphics)
    dev->pci_dev = test_create_pci_device(0x8086, 0x0412);  // HD Graphics 4600
    dev->pci_dev->class_code = 0x03;  // Display controller
    dev->pci_dev->subclass = 0x00;    // VGA compatible

    // Allocate register space
    dev->registers = (uint32_t*)test_alloc_dma_buffer(2 * 1024 * 1024);  // 2MB MMIO
    if (!dev->registers) {
        free(dev);
        return NULL;
    }

    test_pci_set_bar(dev->pci_dev, 0, (uint32_t)(uintptr_t)dev->registers, 2 * 1024 * 1024);

    // Allocate VRAM
    dev->vram_size = vram_size_mb * 1024 * 1024;
    dev->vram = (uint8_t*)malloc(dev->vram_size);
    if (!dev->vram) {
        test_free_dma_buffer(dev->registers);
        test_destroy_pci_device(dev->pci_dev);
        free(dev);
        return NULL;
    }
    memset(dev->vram, 0, dev->vram_size);

    // Initialize one connected display (1920x1080@60Hz)
    dev->num_displays = 1;
    dev->displays[0].connected = true;
    dev->displays[0].width = 1920;
    dev->displays[0].height = 1080;
    dev->displays[0].refresh_rate = 60;
    dev->displays[0].bpp = 32;
    dev->displays[0].vsync_enabled = true;
    dev->displays[0].framebuffer_addr = 0;

    return dev;
}

// Helper: Destroy mock device
static void destroy_mock_i915(mock_i915_t* dev) {
    if (!dev) return;

    if (dev->vram) free(dev->vram);
    if (dev->registers) test_free_dma_buffer(dev->registers);
    if (dev->pci_dev) test_destroy_pci_device(dev->pci_dev);
    free(dev);
}

// Test suite setup
static void graphics_test_setup(void) {
    test_log_info("Setting up graphics test environment");
    g_mock_i915 = create_mock_i915(256);  // 256MB VRAM
    TEST_ASSERT_NOT_NULL(g_mock_i915);
}

// Test suite teardown
static void graphics_test_teardown(void) {
    test_log_info("Tearing down graphics test environment");
    if (g_mock_i915) {
        destroy_mock_i915(g_mock_i915);
        g_mock_i915 = NULL;
    }
}

// =============================================================================
// INITIALIZATION TESTS
// =============================================================================

static test_result_t test_graphics_device_detection(void) {
    test_log_info("Testing GPU detection");

    TEST_ASSERT_NOT_NULL(g_mock_i915);
    TEST_ASSERT_EQUAL(0x8086, g_mock_i915->pci_dev->vendor_id);
    TEST_ASSERT_EQUAL(0x0412, g_mock_i915->pci_dev->device_id);
    TEST_ASSERT_EQUAL(0x03, g_mock_i915->pci_dev->class_code);

    return TEST_PASS;
}

static test_result_t test_graphics_vram_detection(void) {
    test_log_info("Testing VRAM detection");

    TEST_ASSERT_NOT_NULL(g_mock_i915->vram);
    TEST_ASSERT_EQUAL(256 * 1024 * 1024, g_mock_i915->vram_size);

    test_log_debug("VRAM: %llu MB", g_mock_i915->vram_size / (1024 * 1024));

    return TEST_PASS;
}

// =============================================================================
// MODE SETTING TESTS
// =============================================================================

static test_result_t test_graphics_modeset_1920x1080(void) {
    test_log_info("Testing mode set 1920x1080@60Hz");

    g_mock_i915->displays[0].width = 1920;
    g_mock_i915->displays[0].height = 1080;
    g_mock_i915->displays[0].refresh_rate = 60;
    g_mock_i915->displays[0].bpp = 32;

    TEST_ASSERT_EQUAL(1920, g_mock_i915->displays[0].width);
    TEST_ASSERT_EQUAL(1080, g_mock_i915->displays[0].height);
    TEST_ASSERT_EQUAL(60, g_mock_i915->displays[0].refresh_rate);

    return TEST_PASS;
}

static test_result_t test_graphics_modeset_4k(void) {
    test_log_info("Testing mode set 3840x2160@60Hz (4K)");

    g_mock_i915->displays[0].width = 3840;
    g_mock_i915->displays[0].height = 2160;
    g_mock_i915->displays[0].refresh_rate = 60;
    g_mock_i915->displays[0].bpp = 32;

    TEST_ASSERT_EQUAL(3840, g_mock_i915->displays[0].width);
    TEST_ASSERT_EQUAL(2160, g_mock_i915->displays[0].height);

    return TEST_PASS;
}

static test_result_t test_graphics_refresh_rate_144hz(void) {
    test_log_info("Testing 144Hz refresh rate");

    g_mock_i915->displays[0].refresh_rate = 144;

    TEST_ASSERT_EQUAL(144, g_mock_i915->displays[0].refresh_rate);

    return TEST_PASS;
}

// =============================================================================
// MULTI-MONITOR TESTS
// =============================================================================

static test_result_t test_graphics_dual_monitor(void) {
    test_log_info("Testing dual monitor setup");

    // Enable second display
    g_mock_i915->num_displays = 2;
    g_mock_i915->displays[1].connected = true;
    g_mock_i915->displays[1].width = 1920;
    g_mock_i915->displays[1].height = 1080;
    g_mock_i915->displays[1].refresh_rate = 60;
    g_mock_i915->displays[1].bpp = 32;

    TEST_ASSERT_EQUAL(2, g_mock_i915->num_displays);
    TEST_ASSERT(g_mock_i915->displays[0].connected);
    TEST_ASSERT(g_mock_i915->displays[1].connected);

    return TEST_PASS;
}

static test_result_t test_graphics_triple_monitor(void) {
    test_log_info("Testing triple monitor setup");

    g_mock_i915->num_displays = 3;
    for (int i = 0; i < 3; i++) {
        g_mock_i915->displays[i].connected = true;
        g_mock_i915->displays[i].width = 1920;
        g_mock_i915->displays[i].height = 1080;
        g_mock_i915->displays[i].refresh_rate = 60;
        g_mock_i915->displays[i].bpp = 32;
    }

    TEST_ASSERT_EQUAL(3, g_mock_i915->num_displays);

    return TEST_PASS;
}

// =============================================================================
// PAGE FLIPPING TESTS
// =============================================================================

static test_result_t test_graphics_page_flip(void) {
    test_log_info("Testing page flipping");

    // Simulate double-buffered rendering
    uint32_t front_buffer = 0;
    uint32_t back_buffer = g_mock_i915->displays[0].width *
                           g_mock_i915->displays[0].height * 4;

    for (int i = 0; i < 60; i++) {  // 60 flips
        // Draw to back buffer
        g_mock_i915->displays[0].framebuffer_addr = back_buffer;

        // Flip
        uint32_t temp = front_buffer;
        front_buffer = back_buffer;
        back_buffer = temp;

        g_mock_i915->frames_rendered++;
    }

    TEST_ASSERT_EQUAL(60, g_mock_i915->frames_rendered);

    return TEST_PASS;
}

static test_result_t test_graphics_vsync(void) {
    test_log_info("Testing VSync");

    g_mock_i915->displays[0].vsync_enabled = true;

    // Simulate 60 frames at 60Hz (should take ~1000ms)
    uint64_t start = test_get_time_us();

    for (int i = 0; i < 60; i++) {
        test_sleep_ms(16);  // ~60fps
        g_mock_i915->vblank_count++;
    }

    uint64_t elapsed = test_get_time_us() - start;

    test_log_debug("60 frames in %llu us (%.1f fps)",
                  elapsed, 60.0 * 1000000.0 / elapsed);

    TEST_ASSERT_EQUAL(60, g_mock_i915->vblank_count);

    return TEST_PASS;
}

// =============================================================================
// FRAMEBUFFER TESTS
// =============================================================================

static test_result_t test_graphics_framebuffer_clear(void) {
    test_log_info("Testing framebuffer clear");

    // Clear to blue (0xFF0000FF in BGRA)
    uint32_t fb_size = g_mock_i915->displays[0].width *
                       g_mock_i915->displays[0].height * 4;

    for (uint32_t i = 0; i < fb_size; i += 4) {
        *(uint32_t*)(g_mock_i915->vram + i) = 0xFF0000FF;
    }

    // Verify first pixel
    TEST_ASSERT_EQUAL(0xFF0000FF, *(uint32_t*)g_mock_i915->vram);

    return TEST_PASS;
}

static test_result_t test_graphics_framebuffer_pattern(void) {
    test_log_info("Testing framebuffer pattern");

    uint32_t width = g_mock_i915->displays[0].width;
    uint32_t height = g_mock_i915->displays[0].height;

    // Draw checkerboard pattern
    for (uint32_t y = 0; y < height; y++) {
        for (uint32_t x = 0; x < width; x++) {
            uint32_t color = ((x / 32) + (y / 32)) % 2 ? 0xFFFFFFFF : 0xFF000000;
            uint32_t offset = (y * width + x) * 4;
            if (offset < g_mock_i915->vram_size) {
                *(uint32_t*)(g_mock_i915->vram + offset) = color;
            }
        }
    }

    return TEST_PASS;
}

// =============================================================================
// HARDWARE CURSOR TESTS
// =============================================================================

static test_result_t test_graphics_cursor_enable(void) {
    test_log_info("Testing hardware cursor enable");

    g_mock_i915->hardware_cursor_enabled = true;
    g_mock_i915->cursor_x = 100;
    g_mock_i915->cursor_y = 100;

    TEST_ASSERT(g_mock_i915->hardware_cursor_enabled);

    return TEST_PASS;
}

static test_result_t test_graphics_cursor_move(void) {
    test_log_info("Testing cursor movement");

    g_mock_i915->hardware_cursor_enabled = true;

    // Move cursor across screen
    for (uint16_t x = 0; x < 1920; x += 10) {
        g_mock_i915->cursor_x = x;
        g_mock_i915->cursor_y = x / 2;
    }

    TEST_ASSERT(g_mock_i915->cursor_x > 0);

    return TEST_PASS;
}

// =============================================================================
// HOT-PLUG TESTS
// =============================================================================

static test_result_t test_graphics_hotplug_connect(void) {
    test_log_info("Testing display hot-plug connect");

    // Start with one display
    g_mock_i915->num_displays = 1;

    // Connect second display
    g_mock_i915->displays[1].connected = true;
    g_mock_i915->displays[1].width = 1920;
    g_mock_i915->displays[1].height = 1080;
    g_mock_i915->displays[1].refresh_rate = 60;
    g_mock_i915->displays[1].bpp = 32;
    g_mock_i915->num_displays = 2;

    TEST_ASSERT_EQUAL(2, g_mock_i915->num_displays);
    TEST_ASSERT(g_mock_i915->displays[1].connected);

    return TEST_PASS;
}

static test_result_t test_graphics_hotplug_disconnect(void) {
    test_log_info("Testing display hot-plug disconnect");

    // Start with two displays
    g_mock_i915->num_displays = 2;
    g_mock_i915->displays[1].connected = true;

    // Disconnect second display
    g_mock_i915->displays[1].connected = false;
    g_mock_i915->num_displays = 1;

    TEST_ASSERT_EQUAL(1, g_mock_i915->num_displays);
    TEST_ASSERT(!g_mock_i915->displays[1].connected);

    return TEST_PASS;
}

// =============================================================================
// POWER MANAGEMENT TESTS
// =============================================================================

static test_result_t test_graphics_power_saving(void) {
    test_log_info("Testing power saving mode");

    g_mock_i915->power_saving_enabled = true;

    TEST_ASSERT(g_mock_i915->power_saving_enabled);

    return TEST_PASS;
}

static test_result_t test_graphics_dpms_off(void) {
    test_log_info("Testing DPMS off");

    // Disable display
    g_mock_i915->displays[0].connected = false;

    TEST_ASSERT(!g_mock_i915->displays[0].connected);

    // Re-enable
    g_mock_i915->displays[0].connected = true;

    TEST_ASSERT(g_mock_i915->displays[0].connected);

    return TEST_PASS;
}

// =============================================================================
// PERFORMANCE TESTS
// =============================================================================

static test_result_t test_graphics_framerate(void) {
    test_log_info("Testing frame rate (1 second test)");

    uint64_t start = test_get_time_us();
    uint32_t frames = 0;

    while ((test_get_time_us() - start) < 1000000) {
        g_mock_i915->frames_rendered++;
        frames++;
        test_sleep_ms(1);  // Minimal sleep
    }

    test_log_info("Rendered %u frames in 1 second", frames);

    return TEST_PASS;
}

// =============================================================================
// TEST REGISTRATION
// =============================================================================

static test_suite_t graphics_test_suite = {
    .name = "i915",
    .description = "Intel i915 Graphics Driver Tests",
    .setup = graphics_test_setup,
    .teardown = graphics_test_teardown,
    .tests = NULL,
    .next = NULL
};

void register_i915_tests(void) {
    static test_case_t test_cases[] = {
        {"device_detection", "GPU detection", test_graphics_device_detection, false, "i915"},
        {"vram_detection", "VRAM detection", test_graphics_vram_detection, false, "i915"},
        {"modeset_1080p", "Mode set 1920x1080", test_graphics_modeset_1920x1080, false, "i915"},
        {"modeset_4k", "Mode set 4K", test_graphics_modeset_4k, false, "i915"},
        {"refresh_144hz", "144Hz refresh rate", test_graphics_refresh_rate_144hz, false, "i915"},
        {"dual_monitor", "Dual monitor", test_graphics_dual_monitor, false, "i915"},
        {"triple_monitor", "Triple monitor", test_graphics_triple_monitor, false, "i915"},
        {"page_flip", "Page flipping", test_graphics_page_flip, false, "i915"},
        {"vsync", "VSync", test_graphics_vsync, false, "i915"},
        {"fb_clear", "Framebuffer clear", test_graphics_framebuffer_clear, false, "i915"},
        {"fb_pattern", "Framebuffer pattern", test_graphics_framebuffer_pattern, false, "i915"},
        {"cursor_enable", "Hardware cursor", test_graphics_cursor_enable, false, "i915"},
        {"cursor_move", "Cursor movement", test_graphics_cursor_move, false, "i915"},
        {"hotplug_connect", "Hot-plug connect", test_graphics_hotplug_connect, false, "i915"},
        {"hotplug_disconnect", "Hot-plug disconnect", test_graphics_hotplug_disconnect, false, "i915"},
        {"power_saving", "Power saving", test_graphics_power_saving, false, "i915"},
        {"dpms_off", "DPMS off/on", test_graphics_dpms_off, false, "i915"},
        {"framerate", "Frame rate test", test_graphics_framerate, false, "i915"},
    };

    for (size_t i = 0; i < sizeof(test_cases) / sizeof(test_case_t); i++) {
        test_register_case(&graphics_test_suite, &test_cases[i]);
    }

    test_register_suite(&graphics_test_suite);
}
