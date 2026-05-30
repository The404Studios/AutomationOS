/**
 * File Explorer - Search and Indexing
 *
 * Provides fast file search with indexing and filtering
 */

#ifndef SEARCH_H
#define SEARCH_H

#include <stdint.h>
#include <stdbool.h>
#include "file_types.h"

// Forward declarations
typedef struct search_index search_index_t;
typedef struct search_query search_query_t;
typedef struct search_result search_result_t;
typedef struct search_results search_results_t;

/**
 * File type filters
 */
typedef enum {
    FILTER_ALL_FILES,
    FILTER_DOCUMENTS,      // Text, PDF, Word, etc.
    FILTER_IMAGES,         // PNG, JPG, etc.
    FILTER_VIDEOS,         // MP4, AVI, etc.
    FILTER_AUDIO,          // MP3, FLAC, etc.
    FILTER_ARCHIVES,       // ZIP, TAR, etc.
    FILTER_CODE,           // C, Python, etc.
    FILTER_EXECUTABLES,    // EXE, BIN, etc.
} file_type_filter_t;

/**
 * Date filter presets
 */
typedef enum {
    DATE_ANY,
    DATE_TODAY,
    DATE_YESTERDAY,
    DATE_THIS_WEEK,
    DATE_THIS_MONTH,
    DATE_THIS_YEAR,
    DATE_CUSTOM,
} date_filter_t;

/**
 * Search query structure
 */
struct search_query {
    // Search text
    char query[256];

    // Filters
    file_type_filter_t type_filter;
    uint64_t min_size;
    uint64_t max_size;
    uint64_t date_from;
    uint64_t date_to;
    date_filter_t date_filter;

    // Search options
    bool case_sensitive;
    bool regex_mode;
    bool content_search;        // Search inside files (slow)
    bool include_hidden;

    // Search scope
    char search_path[4096];     // Root path to search from
    bool recursive;             // Search subdirectories

    // Result limits
    uint32_t max_results;
    uint32_t offset;            // For pagination
};

/**
 * Search result item
 */
struct search_result {
    file_entry_t file;

    // Match information
    uint32_t match_score;       // Relevance score
    char match_context[512];    // Context around match (for content search)
    uint32_t match_offset;      // Byte offset in file (for content search)

    // Highlight positions (for UI)
    struct {
        uint32_t start;
        uint32_t length;
    } highlights[16];
    uint32_t highlight_count;
};

/**
 * Search results collection
 */
struct search_results {
    search_result_t *results;
    uint32_t count;
    uint32_t total_matches;     // Total matches (may be > count if limited)
    uint64_t search_time_us;    // Time taken to search
    bool truncated;             // Results were limited
};

/**
 * Search index entry
 */
typedef struct {
    char filename[256];
    char full_path[4096];
    uint64_t size;
    uint64_t modified_time;
    file_type_t type;
    bool is_directory;
    uint32_t hash;              // Filename hash for quick lookup
} index_entry_t;

/**
 * Search index structure (in-memory)
 */
struct search_index {
    index_entry_t *entries;
    uint32_t entry_count;
    uint32_t entry_capacity;

    // Index metadata
    char indexed_path[4096];
    uint64_t last_update_time;
    bool is_valid;

    // Statistics
    uint32_t total_files;
    uint32_t total_directories;
    uint64_t total_size;
};

// Index management
search_index_t* index_create(const char *path);
void index_destroy(search_index_t *index);
void index_update(search_index_t *index);
void index_add_file(search_index_t *index, const char *path, const file_entry_t *entry);
void index_remove_file(search_index_t *index, const char *path);
bool index_is_up_to_date(search_index_t *index);

// Background indexer
void index_start_background_update(search_index_t *index);
void index_stop_background_update(search_index_t *index);

// Search functions
search_results_t* search_execute(search_index_t *index, const search_query_t *query);
void search_results_destroy(search_results_t *results);

// Query building
search_query_t* query_create(const char *text);
void query_set_type_filter(search_query_t *query, file_type_filter_t filter);
void query_set_size_range(search_query_t *query, uint64_t min, uint64_t max);
void query_set_date_filter(search_query_t *query, date_filter_t filter);
void query_set_date_range(search_query_t *query, uint64_t from, uint64_t to);
void query_set_path(search_query_t *query, const char *path);
void query_destroy(search_query_t *query);

// Fast filename search (no index)
search_results_t* search_filename_fast(const char *path, const char *pattern, bool recursive);

// Content search (very slow, searches inside files)
search_results_t* search_content(const char *path, const char *text, bool recursive);

// Utility functions
uint32_t hash_string(const char *str);
bool match_pattern(const char *text, const char *pattern, bool case_sensitive);
bool match_regex(const char *text, const char *regex);

// Result sorting
void results_sort_by_name(search_results_t *results, bool ascending);
void results_sort_by_date(search_results_t *results, bool ascending);
void results_sort_by_size(search_results_t *results, bool ascending);
void results_sort_by_relevance(search_results_t *results);

#endif // SEARCH_H
