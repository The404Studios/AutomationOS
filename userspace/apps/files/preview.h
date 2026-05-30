/**
 * File Explorer - Preview / Quick Look
 *
 * Provides quick preview of files without opening them
 */

#ifndef PREVIEW_H
#define PREVIEW_H

#include <stdint.h>
#include <stdbool.h>
#include "../../compositor/compositor.h"
#include "file_types.h"

// Forward declarations
typedef struct preview_window preview_window_t;
typedef struct preview_content preview_content_t;

/**
 * Preview window structure
 */
struct preview_window {
    void *window;               // Window handle
    bool visible;

    // File being previewed
    char file_path[4096];
    file_entry_t file_info;

    // Preview content
    preview_content_t *content;

    // Navigation (for multi-file previews)
    char **file_list;
    uint32_t file_count;
    uint32_t current_index;

    // UI
    rect_t close_button;
    rect_t prev_button;
    rect_t next_button;
    rect_t info_button;

    // Zoom (for images/PDFs)
    float zoom_level;
    int32_t pan_x, pan_y;

    // Video/audio playback
    bool playing;
    uint64_t playback_position;
    uint64_t total_duration;
};

/**
 * Preview content types
 */
typedef enum {
    PREVIEW_NONE,
    PREVIEW_IMAGE,
    PREVIEW_VIDEO,
    PREVIEW_AUDIO,
    PREVIEW_TEXT,
    PREVIEW_PDF,
    PREVIEW_CODE,
    PREVIEW_ARCHIVE,
} preview_type_t;

/**
 * Image preview content
 */
typedef struct {
    uint32_t *pixels;           // ARGB32
    uint32_t width;
    uint32_t height;
    uint32_t channels;

    // EXIF metadata
    char camera_make[64];
    char camera_model[64];
    char date_taken[32];
    float focal_length;
    float aperture;
    float iso;
    float exposure_time;
} image_preview_t;

/**
 * Video preview content
 */
typedef struct {
    uint32_t *thumbnail;        // Poster frame
    uint32_t width;
    uint32_t height;

    // Video metadata
    uint64_t duration_ms;
    uint32_t fps;
    char codec[32];
    uint64_t bitrate;
} video_preview_t;

/**
 * Audio preview content
 */
typedef struct {
    uint32_t *waveform;         // Waveform visualization
    uint32_t waveform_width;

    // ID3 metadata
    char title[256];
    char artist[256];
    char album[256];
    char genre[64];
    uint32_t year;
    uint32_t track_number;
    uint64_t duration_ms;
    uint32_t sample_rate;
    uint32_t bitrate;
} audio_preview_t;

/**
 * Text preview content
 */
typedef struct {
    char *text;
    uint32_t length;
    uint32_t line_count;
    bool syntax_highlighted;
    char language[32];
} text_preview_t;

/**
 * PDF preview content
 */
typedef struct {
    uint32_t *page_thumbnail;
    uint32_t width;
    uint32_t height;

    uint32_t page_count;
    uint32_t current_page;

    char title[256];
    char author[256];
    char subject[256];
    uint64_t creation_date;
} pdf_preview_t;

/**
 * Archive preview content
 */
typedef struct {
    struct {
        char name[256];
        uint64_t size;
        uint64_t compressed_size;
        bool is_directory;
    } entries[1000];
    uint32_t entry_count;

    uint64_t total_size;
    uint64_t compressed_size;
    uint32_t file_count;
    uint32_t directory_count;
} archive_preview_t;

/**
 * Preview content union
 */
struct preview_content {
    preview_type_t type;

    union {
        image_preview_t image;
        video_preview_t video;
        audio_preview_t audio;
        text_preview_t text;
        pdf_preview_t pdf;
        archive_preview_t archive;
    };
};

// Preview window management
preview_window_t* preview_create(void);
void preview_destroy(preview_window_t *preview);
void preview_show(preview_window_t *preview, const char *file_path);
void preview_hide(preview_window_t *preview);
void preview_navigate_next(preview_window_t *preview);
void preview_navigate_prev(preview_window_t *preview);

// Content loading
preview_content_t* preview_load_content(const char *file_path);
void preview_content_destroy(preview_content_t *content);

// Image preview
image_preview_t* load_image_preview(const char *path);
void load_image_exif(const char *path, image_preview_t *preview);

// Video preview
video_preview_t* load_video_preview(const char *path);

// Audio preview
audio_preview_t* load_audio_preview(const char *path);
void load_audio_metadata(const char *path, audio_preview_t *preview);

// Text preview
text_preview_t* load_text_preview(const char *path, uint32_t max_lines);
void syntax_highlight_text(text_preview_t *text, const char *language);

// PDF preview
pdf_preview_t* load_pdf_preview(const char *path);
void pdf_navigate_to_page(pdf_preview_t *pdf, uint32_t page);

// Archive preview
archive_preview_t* load_archive_preview(const char *path);

// Zoom and pan
void preview_zoom_in(preview_window_t *preview);
void preview_zoom_out(preview_window_t *preview);
void preview_zoom_fit(preview_window_t *preview);
void preview_zoom_actual(preview_window_t *preview);
void preview_pan(preview_window_t *preview, int32_t dx, int32_t dy);

// Playback controls (video/audio)
void preview_play(preview_window_t *preview);
void preview_pause(preview_window_t *preview);
void preview_stop(preview_window_t *preview);
void preview_seek(preview_window_t *preview, uint64_t position_ms);

// Rendering
void preview_render(preview_window_t *preview);

#endif // PREVIEW_H
