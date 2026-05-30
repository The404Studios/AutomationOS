/**
 * File Explorer - Preview Implementation (stub)
 */

#include "preview.h"
#include "../../lib/image/image.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

preview_window_t* preview_create(void) {
    preview_window_t *preview = calloc(1, sizeof(preview_window_t));
    if (!preview) return NULL;

    preview->visible = false;
    preview->zoom_level = 1.0f;
    preview->playing = false;

    return preview;
}

void preview_destroy(preview_window_t *preview) {
    if (!preview) return;

    if (preview->content) {
        preview_content_destroy(preview->content);
    }

    free(preview);
}

void preview_show(preview_window_t *preview, const char *file_path) {
    if (!preview || !file_path) return;

    strncpy(preview->file_path, file_path, sizeof(preview->file_path) - 1);

    // Load content
    preview->content = preview_load_content(file_path);

    preview->visible = true;
}

void preview_hide(preview_window_t *preview) {
    if (!preview) return;
    preview->visible = false;
}

static bool is_image_file(const char *path) {
    const char *ext = strrchr(path, '.');
    if (!ext) return false;

    const char *image_exts[] = {".png", ".jpg", ".jpeg", ".bmp", ".gif", NULL};
    for (int i = 0; image_exts[i]; i++) {
        if (strcasecmp(ext, image_exts[i]) == 0) {
            return true;
        }
    }
    return false;
}

preview_content_t* preview_load_content(const char *file_path) {
    if (!file_path) return NULL;

    preview_content_t *content = calloc(1, sizeof(preview_content_t));
    if (!content) return NULL;

    // Detect file type and load appropriate content
    if (is_image_file(file_path)) {
        content->type = PREVIEW_IMAGE;

        // Load image using libimage
        image_t *img = image_load(file_path, IMAGE_LOAD_RGBA);
        if (img) {
            content->image.width = img->width;
            content->image.height = img->height;
            content->image.channels = img->channels;

            // Convert to ARGB32 format for preview
            content->image.pixels = malloc(img->width * img->height * sizeof(uint32_t));
            if (content->image.pixels) {
                for (uint32_t y = 0; y < img->height; y++) {
                    for (uint32_t x = 0; x < img->width; x++) {
                        content->image.pixels[y * img->width + x] = image_get_pixel(img, x, y);
                    }
                }
            }

            image_free(img);

            printf("[Preview] Loaded image: %ux%u\n", content->image.width, content->image.height);
        } else {
            fprintf(stderr, "[Preview] ERROR: Failed to load image: %s\n", image_get_error());
            free(content);
            return NULL;
        }
    } else {
        // Unsupported file type
        content->type = PREVIEW_NONE;
    }

    return content;
}

void preview_content_destroy(preview_content_t *content) {
    if (!content) return;

    // Free type-specific resources
    switch (content->type) {
        case PREVIEW_IMAGE:
            if (content->image.pixels) free(content->image.pixels);
            break;
        case PREVIEW_TEXT:
            if (content->text.text) free(content->text.text);
            break;
        default:
            break;
    }

    free(content);
}

void preview_zoom_in(preview_window_t *preview) {
    if (!preview) return;
    preview->zoom_level *= 1.2f;
}

void preview_zoom_out(preview_window_t *preview) {
    if (!preview) return;
    preview->zoom_level /= 1.2f;
}

void preview_zoom_fit(preview_window_t *preview) {
    if (!preview) return;
    preview->zoom_level = 1.0f;
}

void preview_zoom_actual(preview_window_t *preview) {
    if (!preview) return;
    preview->zoom_level = 1.0f;
}

void preview_pan(preview_window_t *preview, int32_t dx, int32_t dy) {
    if (!preview) return;
    preview->pan_x += dx;
    preview->pan_y += dy;
}

void preview_render(preview_window_t *preview) {
    if (!preview || !preview->visible) return;

    // TODO: Render based on content type
}

// ============================================================================
// IMAGE PREVIEW IMPLEMENTATION
// ============================================================================

image_preview_t* load_image_preview(const char *path) {
    if (!path) return NULL;

    image_preview_t *preview = calloc(1, sizeof(image_preview_t));
    if (!preview) return NULL;

    // Load image
    image_t *img = image_load(path, IMAGE_LOAD_RGBA);
    if (!img) {
        free(preview);
        return NULL;
    }

    preview->width = img->width;
    preview->height = img->height;
    preview->channels = img->channels;

    // Convert to ARGB32 format
    preview->pixels = malloc(img->width * img->height * sizeof(uint32_t));
    if (preview->pixels) {
        for (uint32_t y = 0; y < img->height; y++) {
            for (uint32_t x = 0; x < img->width; x++) {
                preview->pixels[y * img->width + x] = image_get_pixel(img, x, y);
            }
        }
    }

    image_free(img);

    // Load EXIF metadata (TODO: implement EXIF parsing)
    load_image_exif(path, preview);

    return preview;
}

void load_image_exif(const char *path, image_preview_t *preview) {
    if (!path || !preview) return;

    // TODO: Parse EXIF data from JPEG files
    // For now, just zero out the fields
    memset(preview->camera_make, 0, sizeof(preview->camera_make));
    memset(preview->camera_model, 0, sizeof(preview->camera_model));
    memset(preview->date_taken, 0, sizeof(preview->date_taken));
    preview->focal_length = 0.0f;
    preview->aperture = 0.0f;
    preview->iso = 0.0f;
    preview->exposure_time = 0.0f;
}

// ============================================================================
// STUB IMPLEMENTATIONS
// ============================================================================

void preview_navigate_next(preview_window_t *preview) {
    if (!preview) return;
    // TODO: Navigate to next file in list
}

void preview_navigate_prev(preview_window_t *preview) {
    if (!preview) return;
    // TODO: Navigate to previous file in list
}

video_preview_t* load_video_preview(const char *path) {
    (void)path;
    return NULL;
}

audio_preview_t* load_audio_preview(const char *path) {
    (void)path;
    return NULL;
}

void load_audio_metadata(const char *path, audio_preview_t *preview) {
    (void)path;
    (void)preview;
}

text_preview_t* load_text_preview(const char *path, uint32_t max_lines) {
    (void)path;
    (void)max_lines;
    return NULL;
}

void syntax_highlight_text(text_preview_t *text, const char *language) {
    (void)text;
    (void)language;
}

pdf_preview_t* load_pdf_preview(const char *path) {
    (void)path;
    return NULL;
}

void pdf_navigate_to_page(pdf_preview_t *pdf, uint32_t page) {
    (void)pdf;
    (void)page;
}

archive_preview_t* load_archive_preview(const char *path) {
    (void)path;
    return NULL;
}

void preview_play(preview_window_t *preview) {
    if (!preview) return;
    preview->playing = true;
}

void preview_pause(preview_window_t *preview) {
    if (!preview) return;
    preview->playing = false;
}

void preview_stop(preview_window_t *preview) {
    if (!preview) return;
    preview->playing = false;
    preview->playback_position = 0;
}

void preview_seek(preview_window_t *preview, uint64_t position_ms) {
    if (!preview) return;
    preview->playback_position = position_ms;
}
