/**
 * TrueType Font Parser using stb_truetype
 *
 * This file integrates the stb_truetype single-header library.
 * stb_truetype is public domain software by Sean Barrett.
 */

#include "font.h"
#include "font_internal.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

// Define STB_TRUETYPE_IMPLEMENTATION before including the header
#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#define STBTT_malloc(x,u)  malloc(x)
#define STBTT_free(x,u)    free(x)
#define STBTT_assert(x)    ((void)0)  // No asserts in kernel code

// Note: stb_truetype.h should be in the same directory or include path
// Download from: https://raw.githubusercontent.com/nothings/stb/master/stb_truetype.h
#include "stb_truetype.h"

// Font structure (internal)
struct font {
    stbtt_fontinfo info;      // stb_truetype font info
    uint8_t* data;            // Font data buffer (owned)
    float scale;              // Current scale factor
    float size_pt;            // Current size in points
    int ascent;               // Font ascent
    int descent;              // Font descent
    int line_gap;             // Line gap
    font_quality_t quality;   // Rendering quality
    bool owns_data;           // Whether we allocated the data
};

/**
 * Load font from file
 */
font_t* font_load(const char* path) {
    FILE* fp = fopen(path, "rb");
    if (!fp) {
        fprintf(stderr, "font_load: failed to open %s\n", path);
        return NULL;
    }

    // Get file size
    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    if (size <= 0 || size > 10 * 1024 * 1024) {  // Max 10MB
        fprintf(stderr, "font_load: invalid file size %ld\n", size);
        fclose(fp);
        return NULL;
    }

    // Allocate buffer
    uint8_t* data = malloc(size);
    if (!data) {
        fprintf(stderr, "font_load: failed to allocate %ld bytes\n", size);
        fclose(fp);
        return NULL;
    }

    // Read file
    if (fread(data, 1, size, fp) != (size_t)size) {
        fprintf(stderr, "font_load: failed to read file\n");
        free(data);
        fclose(fp);
        return NULL;
    }
    fclose(fp);

    // Load from memory
    font_t* font = font_load_memory(data, size);
    if (!font) {
        free(data);
        return NULL;
    }

    font->owns_data = true;
    return font;
}

/**
 * Load font from memory buffer
 */
font_t* font_load_memory(const void* data, size_t size) {
    if (!data || size == 0) {
        return NULL;
    }

    font_t* font = calloc(1, sizeof(font_t));
    if (!font) {
        return NULL;
    }

    // Initialize stb_truetype
    font->data = (uint8_t*)data;
    font->owns_data = false;

    if (!stbtt_InitFont(&font->info, font->data, 0)) {
        fprintf(stderr, "font_load_memory: stbtt_InitFont failed\n");
        free(font);
        return NULL;
    }

    // Get font metrics
    stbtt_GetFontVMetrics(&font->info, &font->ascent, &font->descent, &font->line_gap);

    // Set default size and quality
    font_set_size(font, 12.0f);  // Default 12pt
    font->quality = FONT_QUALITY_HIGH;

    return font;
}

/**
 * Free font
 */
void font_free(font_t* font) {
    if (!font) return;

    // Clear cache entries for this font
    font_cache_clear(font);

    // Free data if we own it
    if (font->owns_data && font->data) {
        free(font->data);
    }

    free(font);
}

/**
 * Set font size in points
 */
void font_set_size(font_t* font, float size_pt) {
    if (!font || size_pt <= 0.0f) return;

    font->size_pt = size_pt;

    // Calculate scale factor for this size
    // stb_truetype works at pixels-per-em, we want points at 72 DPI
    font->scale = stbtt_ScaleForPixelHeight(&font->info, size_pt);

    // Update cached metrics
    int ascent, descent, line_gap;
    stbtt_GetFontVMetrics(&font->info, &ascent, &descent, &line_gap);
    font->ascent = (int)(ascent * font->scale);
    font->descent = (int)(descent * font->scale);
    font->line_gap = (int)(line_gap * font->scale);

    // Clear cache since size changed
    font_cache_clear(font);
}

/**
 * Set rendering quality
 */
void font_set_quality(font_t* font, font_quality_t quality) {
    if (!font) return;
    font->quality = quality;

    // Clear cache since quality changed
    font_cache_clear(font);
}

/**
 * Get font metrics
 */
void font_get_metrics(font_t* font, font_metrics_t* metrics) {
    if (!font || !metrics) return;

    metrics->ascent = font->ascent;
    metrics->descent = -font->descent;  // Make positive
    metrics->height = font->ascent - font->descent;
    metrics->line_gap = font->line_gap;
}

/**
 * Rasterize a single glyph (internal function)
 * Called by cache.c when glyph not in cache
 */
font_glyph_t* font_rasterize_glyph_internal(font_t* font, uint32_t codepoint) {
    if (!font) return NULL;

    // Get glyph index
    int glyph_index = stbtt_FindGlyphIndex(&font->info, codepoint);
    if (glyph_index == 0 && codepoint != 0) {
        // Glyph not found, try replacement character U+FFFD
        glyph_index = stbtt_FindGlyphIndex(&font->info, 0xFFFD);
        if (glyph_index == 0) {
            // No replacement, use space
            glyph_index = stbtt_FindGlyphIndex(&font->info, ' ');
        }
    }

    // Get glyph metrics
    int advance, lsb;
    stbtt_GetGlyphHMetrics(&font->info, glyph_index, &advance, &lsb);

    int x0, y0, x1, y1;
    stbtt_GetGlyphBitmapBox(&font->info, glyph_index, font->scale, font->scale,
                            &x0, &y0, &x1, &y1);

    int width = x1 - x0;
    int height = y1 - y0;

    /* Clamp glyph dimensions. A hostile font (tiny unitsPerEm -> huge scale, or
     * extreme glyph bbox) can drive width/height into the tens of thousands,
     * overflowing the int width*height in the malloc below into a small/negative
     * size while MakeGlyphBitmap still rasters the full width*height extent
     * (heap overflow). Also caps a pure memory-exhaustion DoS. 4096px is far
     * beyond any real rendered glyph. */
    if (width  < 0) width  = 0;
    if (height < 0) height = 0;
    if (width  > 4096) width  = 4096;
    if (height > 4096) height = 4096;

    // Allocate glyph structure
    font_glyph_t* glyph = calloc(1, sizeof(font_glyph_t));
    if (!glyph) return NULL;

    glyph->width = width;
    glyph->height = height;
    glyph->bearing_x = x0;
    glyph->bearing_y = -y1;  // stb uses top-down, we use bottom-up
    glyph->advance = (int)(advance * font->scale);

    // Rasterize bitmap
    if (width > 0 && height > 0) {
        glyph->bitmap = malloc(width * height);
        if (!glyph->bitmap) {
            free(glyph);
            return NULL;
        }

        stbtt_MakeGlyphBitmap(&font->info, glyph->bitmap, width, height, width,
                              font->scale, font->scale, glyph_index);
    }

    return glyph;
}

/**
 * Get glyph advance width
 */
int font_get_glyph_advance(font_t* font, uint32_t codepoint) {
    if (!font) return 0;

    int glyph_index = stbtt_FindGlyphIndex(&font->info, codepoint);
    int advance, lsb;
    stbtt_GetGlyphHMetrics(&font->info, glyph_index, &advance, &lsb);

    return (int)(advance * font->scale);
}

/**
 * Get kerning adjustment between two glyphs
 */
int font_get_kerning(font_t* font, uint32_t codepoint1, uint32_t codepoint2) {
    if (!font) return 0;

    int glyph1 = stbtt_FindGlyphIndex(&font->info, codepoint1);
    int glyph2 = stbtt_FindGlyphIndex(&font->info, codepoint2);

    int kern = stbtt_GetGlyphKernAdvance(&font->info, glyph1, glyph2);
    return (int)(kern * font->scale);
}
