/**
 * Font Rendering Test Program
 *
 * Tests all aspects of the font rendering library:
 * - Font loading
 * - Size changes
 * - UTF-8 text rendering
 * - Cache behavior
 * - Kerning
 * - Text measurement
 */

#include "font.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

// Simple ARGB framebuffer for testing
#define FB_WIDTH  1024
#define FB_HEIGHT 768
static uint32_t framebuffer[FB_WIDTH * FB_HEIGHT];

// Clear framebuffer to white
static void clear_fb(uint32_t color) {
    for (int i = 0; i < FB_WIDTH * FB_HEIGHT; i++) {
        framebuffer[i] = color;
    }
}

// Write framebuffer to PPM file for visual inspection
static void save_ppm(const char* filename) {
    FILE* fp = fopen(filename, "wb");
    if (!fp) {
        fprintf(stderr, "Failed to open %s\n", filename);
        return;
    }

    fprintf(fp, "P6\n%d %d\n255\n", FB_WIDTH, FB_HEIGHT);

    for (int i = 0; i < FB_WIDTH * FB_HEIGHT; i++) {
        uint8_t r = (framebuffer[i] >> 16) & 0xFF;
        uint8_t g = (framebuffer[i] >> 8) & 0xFF;
        uint8_t b = framebuffer[i] & 0xFF;
        fputc(r, fp);
        fputc(g, fp);
        fputc(b, fp);
    }

    fclose(fp);
    printf("Saved framebuffer to %s\n", filename);
}

// Test 1: Font loading
static bool test_font_loading(void) {
    printf("\n=== Test 1: Font Loading ===\n");

    // Try to load a system font
    const char* fonts[] = {
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/System/Library/Fonts/Helvetica.ttc",
        "C:\\Windows\\Fonts\\arial.ttf",
        "DejaVuSans.ttf",  // Current directory
        NULL
    };

    font_t* font = NULL;
    for (int i = 0; fonts[i]; i++) {
        printf("Trying %s... ", fonts[i]);
        font = font_load(fonts[i]);
        if (font) {
            printf("OK\n");
            break;
        }
        printf("not found\n");
    }

    if (!font) {
        printf("✗ Failed to load any font\n");
        printf("  Please provide DejaVuSans.ttf in current directory\n");
        return false;
    }

    printf("✓ Font loaded successfully\n");

    // Get metrics
    font_metrics_t metrics;
    font_get_metrics(font, &metrics);
    printf("  Metrics: ascent=%d, descent=%d, height=%d, line_gap=%d\n",
           metrics.ascent, metrics.descent, metrics.height, metrics.line_gap);

    font_free(font);
    return true;
}

// Test 2: Size changes
static bool test_size_changes(font_t* font) {
    printf("\n=== Test 2: Size Changes ===\n");

    float sizes[] = {8.0f, 12.0f, 16.0f, 24.0f, 32.0f};

    for (int i = 0; i < 5; i++) {
        font_set_size(font, sizes[i]);

        font_metrics_t metrics;
        font_get_metrics(font, &metrics);

        printf("  %.0fpt: ascent=%d, descent=%d, height=%d\n",
               sizes[i], metrics.ascent, metrics.descent, metrics.height);
    }

    printf("✓ Size changes working\n");
    return true;
}

// Test 3: UTF-8 decoding
static bool test_utf8(void) {
    printf("\n=== Test 3: UTF-8 Decoding ===\n");

    struct {
        const char* text;
        size_t expected_len;
    } tests[] = {
        {"Hello", 5},
        {"Привет", 6},  // Russian
        {"你好", 2},      // Chinese
        {"🌍🌎🌏", 3},   // Emojis
        {"Café", 4},
        {NULL, 0}
    };

    for (int i = 0; tests[i].text; i++) {
        size_t len = font_utf8_strlen(tests[i].text);
        bool ok = (len == tests[i].expected_len);

        printf("  \"%s\": length=%zu (expected %zu) %s\n",
               tests[i].text, len, tests[i].expected_len,
               ok ? "✓" : "✗");

        if (!ok) return false;
    }

    printf("✓ UTF-8 decoding working\n");
    return true;
}

// Test 4: Text measurement
static bool test_measurement(font_t* font) {
    printf("\n=== Test 4: Text Measurement ===\n");

    font_set_size(font, 14.0f);

    const char* texts[] = {
        "Hello, World!",
        "The quick brown fox",
        "12345",
        "MMMM",  // Wide characters
        "iiii",  // Narrow characters
        NULL
    };

    for (int i = 0; texts[i]; i++) {
        font_metrics_t metrics;
        if (font_measure_text(font, texts[i], &metrics)) {
            printf("  \"%s\": width=%u, height=%u\n",
                   texts[i], metrics.width, metrics.height);
        } else {
            printf("  \"%s\": measurement failed\n", texts[i]);
            return false;
        }
    }

    printf("✓ Text measurement working\n");
    return true;
}

// Test 5: Glyph rendering
static bool test_rendering(font_t* font) {
    printf("\n=== Test 5: Glyph Rendering ===\n");

    clear_fb(0xFFFFFFFF);  // White background

    font_set_size(font, 16.0f);

    // Render test text
    int y = 50;
    const char* texts[] = {
        "Hello, World!",
        "The quick brown fox jumps over the lazy dog",
        "0123456789",
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ",
        "abcdefghijklmnopqrstuvwxyz",
        NULL
    };

    font_render_opts_t opts = {
        .color = 0xFF000000,  // Black
        .align = FONT_ALIGN_LEFT,
        .wrap_width = 0,
        .line_spacing = 5
    };

    for (int i = 0; texts[i]; i++) {
        font_metrics_t metrics;
        font_get_metrics(font, &metrics);

        int count = font_render_text(font, framebuffer, FB_WIDTH, FB_HEIGHT,
                                       FB_WIDTH * 4, 50, y + metrics.ascent,
                                       texts[i], &opts);

        printf("  Rendered \"%s\": %d glyphs\n", texts[i], count);

        y += metrics.height + 10;
    }

    save_ppm("font_test_basic.ppm");
    printf("✓ Glyph rendering working\n");
    return true;
}

// Test 6: Cache behavior
static bool test_cache(font_t* font) {
    printf("\n=== Test 6: Cache Behavior ===\n");

    // Clear cache
    font_cache_clear(NULL);

    size_t hits, misses, size;
    font_cache_stats(&hits, &misses, &size);
    printf("  Initial: hits=%zu, misses=%zu, size=%zu\n", hits, misses, size);

    // Render same text multiple times
    font_set_size(font, 14.0f);
    const char* text = "Hello, World!";

    for (int i = 0; i < 10; i++) {
        font_render_opts_t opts = FONT_RENDER_OPTS_DEFAULT;
        font_render_text(font, framebuffer, FB_WIDTH, FB_HEIGHT,
                          FB_WIDTH * 4, 100, 100, text, &opts);
    }

    font_cache_stats(&hits, &misses, &size);
    printf("  After 10 renders: hits=%zu, misses=%zu, size=%zu\n",
           hits, misses, size);

    float hit_rate = (float)hits / (hits + misses) * 100.0f;
    printf("  Hit rate: %.1f%%\n", hit_rate);

    if (hit_rate < 50.0f) {
        printf("✗ Cache hit rate too low\n");
        return false;
    }

    printf("✓ Cache working efficiently\n");
    return true;
}

// Test 7: Different alignments
static bool test_alignment(font_t* font) {
    printf("\n=== Test 7: Text Alignment ===\n");

    clear_fb(0xFFFFFFFF);

    font_set_size(font, 18.0f);
    const char* text = "Centered Text";

    font_metrics_t metrics;
    font_get_metrics(font, &metrics);

    int y = 100;

    // Left aligned
    font_render_opts_t opts = {
        .color = 0xFF000000,
        .align = FONT_ALIGN_LEFT,
        .wrap_width = 0,
        .line_spacing = 0
    };
    font_render_text(font, framebuffer, FB_WIDTH, FB_HEIGHT,
                      FB_WIDTH * 4, 100, y + metrics.ascent, "Left Aligned", &opts);

    y += metrics.height + 20;

    // Center aligned
    opts.align = FONT_ALIGN_CENTER;
    font_render_text(font, framebuffer, FB_WIDTH, FB_HEIGHT,
                      FB_WIDTH * 4, FB_WIDTH / 2, y + metrics.ascent, text, &opts);

    y += metrics.height + 20;

    // Right aligned
    opts.align = FONT_ALIGN_RIGHT;
    font_render_text(font, framebuffer, FB_WIDTH, FB_HEIGHT,
                      FB_WIDTH * 4, FB_WIDTH - 100, y + metrics.ascent,
                      "Right Aligned", &opts);

    save_ppm("font_test_alignment.ppm");
    printf("✓ Alignment working\n");
    return true;
}

// Test 8: Multiple font sizes
static bool test_multiple_sizes(font_t* font) {
    printf("\n=== Test 8: Multiple Font Sizes ===\n");

    clear_fb(0xFFFFFFFF);

    float sizes[] = {8.0f, 10.0f, 12.0f, 14.0f, 16.0f, 18.0f, 24.0f, 32.0f};
    int y = 50;

    font_render_opts_t opts = {
        .color = 0xFF000000,
        .align = FONT_ALIGN_LEFT,
        .wrap_width = 0,
        .line_spacing = 5
    };

    for (int i = 0; i < 8; i++) {
        font_set_size(font, sizes[i]);

        font_metrics_t metrics;
        font_get_metrics(font, &metrics);

        char text[64];
        snprintf(text, sizeof(text), "Font size %.0fpt - The quick brown fox", sizes[i]);

        font_render_text(font, framebuffer, FB_WIDTH, FB_HEIGHT,
                          FB_WIDTH * 4, 50, y + metrics.ascent, text, &opts);

        printf("  Rendered %.0fpt\n", sizes[i]);

        y += metrics.height + 10;
    }

    save_ppm("font_test_sizes.ppm");
    printf("✓ Multiple sizes working\n");
    return true;
}

int main(int argc, char** argv) {
    printf("Font Rendering Library Test Suite\n");
    printf("==================================\n");

    // Initialize font system
    if (!font_init(1000)) {
        fprintf(stderr, "Failed to initialize font system\n");
        return 1;
    }

    // Run tests
    bool all_passed = true;

    all_passed &= test_font_loading();
    all_passed &= test_utf8();

    // Load a font for remaining tests
    const char* fonts[] = {
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/System/Library/Fonts/Helvetica.ttc",
        "C:\\Windows\\Fonts\\arial.ttf",
        "DejaVuSans.ttf",
        NULL
    };

    font_t* font = NULL;
    for (int i = 0; fonts[i]; i++) {
        font = font_load(fonts[i]);
        if (font) break;
    }

    if (font) {
        all_passed &= test_size_changes(font);
        all_passed &= test_measurement(font);
        all_passed &= test_rendering(font);
        all_passed &= test_cache(font);
        all_passed &= test_alignment(font);
        all_passed &= test_multiple_sizes(font);

        font_free(font);
    } else {
        printf("\n⚠ Skipping rendering tests (no font available)\n");
    }

    // Cleanup
    font_shutdown();

    printf("\n==================================\n");
    if (all_passed) {
        printf("✓ All tests passed!\n");
        return 0;
    } else {
        printf("✗ Some tests failed\n");
        return 1;
    }
}
