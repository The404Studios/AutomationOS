/**
 * Image Library Test Program
 *
 * Tests core image loading functionality
 */

#include "image.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void test_image_create(void) {
    printf("Testing image_create()...\n");

    image_t *img = image_create(100, 100, 4);
    if (!img) {
        printf("  ✗ FAIL: Could not create image\n");
        return;
    }

    if (img->width != 100 || img->height != 100 || img->channels != 4) {
        printf("  ✗ FAIL: Wrong dimensions\n");
        image_free(img);
        return;
    }

    printf("  ✓ PASS: Created 100x100 RGBA image\n");
    image_free(img);
}

void test_image_fill(void) {
    printf("Testing image_fill()...\n");

    image_t *img = image_create(10, 10, 4);
    if (!img) {
        printf("  ✗ FAIL: Could not create image\n");
        return;
    }

    // Fill with red (0xFFFF0000)
    image_fill(img, 0xFFFF0000);

    // Check a few pixels
    uint32_t pixel = image_get_pixel(img, 5, 5);
    if (pixel != 0xFFFF0000) {
        printf("  ✗ FAIL: Pixel not red (got 0x%08X)\n", pixel);
        image_free(img);
        return;
    }

    printf("  ✓ PASS: Filled image with red\n");
    image_free(img);
}

void test_image_resize(void) {
    printf("Testing image_resize()...\n");

    image_t *img = image_create(100, 100, 4);
    if (!img) {
        printf("  ✗ FAIL: Could not create image\n");
        return;
    }

    image_fill(img, 0xFF00FF00);  // Green

    image_t *resized = image_resize(img, 50, 50);
    if (!resized) {
        printf("  ✗ FAIL: Could not resize image\n");
        image_free(img);
        return;
    }

    if (resized->width != 50 || resized->height != 50) {
        printf("  ✗ FAIL: Wrong resized dimensions\n");
        image_free(img);
        image_free(resized);
        return;
    }

    // Check pixel is still green
    uint32_t pixel = image_get_pixel(resized, 25, 25);
    if ((pixel & 0xFFFFFF) != 0x00FF00) {
        printf("  ✗ FAIL: Pixel not green after resize\n");
        image_free(img);
        image_free(resized);
        return;
    }

    printf("  ✓ PASS: Resized 100x100 -> 50x50\n");
    image_free(img);
    image_free(resized);
}

void test_image_flip(void) {
    printf("Testing image_flip_vertical()...\n");

    image_t *img = image_create(10, 10, 4);
    if (!img) {
        printf("  ✗ FAIL: Could not create image\n");
        return;
    }

    // Set top-left pixel red, bottom-right blue
    image_set_pixel(img, 0, 0, 0xFFFF0000);  // Red at top-left
    image_set_pixel(img, 9, 9, 0xFF0000FF);  // Blue at bottom-right

    image_flip_vertical(img);

    // After vertical flip, red should be at bottom-left
    uint32_t top_left = image_get_pixel(img, 0, 0);
    uint32_t bottom_left = image_get_pixel(img, 0, 9);

    if (bottom_left != 0xFFFF0000) {
        printf("  ✗ FAIL: Red pixel not at bottom-left after flip\n");
        image_free(img);
        return;
    }

    printf("  ✓ PASS: Vertical flip working\n");
    image_free(img);
}

void test_image_blit(void) {
    printf("Testing image_blit()...\n");

    image_t *dest = image_create(100, 100, 4);
    image_t *src = image_create(50, 50, 4);

    if (!dest || !src) {
        printf("  ✗ FAIL: Could not create images\n");
        if (dest) image_free(dest);
        if (src) image_free(src);
        return;
    }

    image_fill(dest, 0xFF000000);  // Black
    image_fill(src, 0xFFFFFFFF);   // White

    image_blit(dest, src, 25, 25);

    // Check center pixel (should be white)
    uint32_t center = image_get_pixel(dest, 50, 50);
    if ((center & 0xFFFFFF) != 0xFFFFFF) {
        printf("  ✗ FAIL: Blit failed (center not white)\n");
        image_free(dest);
        image_free(src);
        return;
    }

    // Check corner pixel (should be black)
    uint32_t corner = image_get_pixel(dest, 0, 0);
    if ((corner & 0xFFFFFF) != 0x000000) {
        printf("  ✗ FAIL: Blit failed (corner not black)\n");
        image_free(dest);
        image_free(src);
        return;
    }

    printf("  ✓ PASS: Blit working\n");
    image_free(dest);
    image_free(src);
}

void test_image_load(const char *path) {
    printf("Testing image_load(\"%s\")...\n", path);

    image_t *img = image_load(path, IMAGE_LOAD_RGBA);
    if (!img) {
        printf("  ✗ FAIL: Could not load image: %s\n", image_get_error());
        return;
    }

    printf("  ✓ PASS: Loaded %ux%u image (%u channels)\n",
           img->width, img->height, img->channels);

    image_free(img);
}

void test_icon_load(const char *icon_name) {
    printf("Testing icon_load_set(\"%s\")...\n", icon_name);

    icon_set_t *set = icon_load_set(icon_name);
    if (!set) {
        printf("  ✗ FAIL: Could not load icon set: %s\n", image_get_error());
        return;
    }

    printf("  ✓ PASS: Loaded icon set with %u sizes\n", set->count);

    // Test getting best size
    image_t *icon = icon_get_best_size(set, 48);
    if (icon) {
        printf("  ✓ PASS: Got best icon for 48x48: %ux%u\n", icon->width, icon->height);
    }

    icon_free_set(set);
}

int main(int argc, char **argv) {
    printf("========================================\n");
    printf("Image Library Test Suite\n");
    printf("========================================\n\n");

    // Run basic tests
    test_image_create();
    test_image_fill();
    test_image_resize();
    test_image_flip();
    test_image_blit();

    printf("\n");

    // Test file loading if path provided
    if (argc > 1) {
        test_image_load(argv[1]);
    } else {
        printf("Run with image path to test loading:\n");
        printf("  %s /path/to/image.png\n", argv[0]);
    }

    printf("\n");

    // Test icon loading
    test_icon_load("terminal");
    test_icon_load("folder");

    printf("\n========================================\n");
    printf("Tests complete!\n");
    printf("========================================\n");

    return 0;
}
