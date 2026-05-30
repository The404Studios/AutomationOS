/**
 * File Explorer - Search and Indexing Implementation
 */

#include "search.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>

#define INITIAL_INDEX_CAPACITY 10000
#define MAX_SEARCH_RESULTS 1000

/**
 * String hash function (DJB2)
 */
uint32_t hash_string(const char *str) {
    uint32_t hash = 5381;
    int c;

    while ((c = *str++)) {
        hash = ((hash << 5) + hash) + c;
    }

    return hash;
}

/**
 * Create search index
 */
search_index_t* index_create(const char *path) {
    search_index_t *index = calloc(1, sizeof(search_index_t));
    if (!index) return NULL;

    index->entry_capacity = INITIAL_INDEX_CAPACITY;
    index->entries = calloc(index->entry_capacity, sizeof(index_entry_t));
    if (!index->entries) {
        free(index);
        return NULL;
    }

    index->entry_count = 0;
    strncpy(index->indexed_path, path, sizeof(index->indexed_path) - 1);
    index->is_valid = false;
    index->last_update_time = 0;
    index->total_files = 0;
    index->total_directories = 0;
    index->total_size = 0;

    return index;
}

/**
 * Destroy search index
 */
void index_destroy(search_index_t *index) {
    if (!index) return;

    if (index->entries) {
        free(index->entries);
    }

    free(index);
}

/**
 * Add file to index
 */
void index_add_file(search_index_t *index, const char *path, const file_entry_t *entry) {
    if (!index || !path || !entry) return;

    // Expand capacity if needed
    if (index->entry_count >= index->entry_capacity) {
        uint32_t new_capacity = index->entry_capacity * 2;
        index_entry_t *new_entries = realloc(index->entries,
                                             new_capacity * sizeof(index_entry_t));
        if (!new_entries) return;

        index->entries = new_entries;
        index->entry_capacity = new_capacity;
    }

    // Add entry
    index_entry_t *idx_entry = &index->entries[index->entry_count];
    strncpy(idx_entry->filename, entry->name, sizeof(idx_entry->filename) - 1);
    strncpy(idx_entry->full_path, entry->path, sizeof(idx_entry->full_path) - 1);
    idx_entry->size = entry->size;
    idx_entry->modified_time = entry->modified_time;
    idx_entry->type = entry->type;
    idx_entry->is_directory = entry->is_directory;
    idx_entry->hash = hash_string(entry->name);

    index->entry_count++;

    // Update statistics
    if (entry->is_directory) {
        index->total_directories++;
    } else {
        index->total_files++;
        index->total_size += entry->size;
    }
}

/**
 * Remove file from index
 */
void index_remove_file(search_index_t *index, const char *path) {
    if (!index || !path) return;

    uint32_t hash = hash_string(path);

    for (uint32_t i = 0; i < index->entry_count; i++) {
        if (index->entries[i].hash == hash &&
            strcmp(index->entries[i].full_path, path) == 0) {

            // Update statistics
            if (index->entries[i].is_directory) {
                index->total_directories--;
            } else {
                index->total_files--;
                index->total_size -= index->entries[i].size;
            }

            // Remove by swapping with last entry
            if (i < index->entry_count - 1) {
                index->entries[i] = index->entries[index->entry_count - 1];
            }
            index->entry_count--;
            return;
        }
    }
}

/**
 * Recursively index directory
 */
static void index_directory_recursive(search_index_t *index, const char *path) {
    DIR *dir = opendir(path);
    if (!dir) return;

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        char full_path[4096];
        snprintf(full_path, sizeof(full_path), "%s/%s", path, entry->d_name);

        // Get file stats
        struct stat st;
        if (stat(full_path, &st) != 0) continue;

        // Create file entry
        file_entry_t file_entry;
        strncpy(file_entry.name, entry->d_name, sizeof(file_entry.name) - 1);
        strncpy(file_entry.path, full_path, sizeof(file_entry.path) - 1);
        file_entry.size = st.st_size;
        file_entry.modified_time = st.st_mtime;
        file_entry.is_directory = S_ISDIR(st.st_mode);
        file_entry.is_hidden = (entry->d_name[0] == '.');
        file_entry.type = detect_file_type(entry->d_name, file_entry.is_directory);

        // Add to index
        index_add_file(index, full_path, &file_entry);

        // Recurse into subdirectories
        if (file_entry.is_directory) {
            index_directory_recursive(index, full_path);
        }
    }

    closedir(dir);
}

/**
 * Update search index
 */
void index_update(search_index_t *index) {
    if (!index) return;

    printf("[Search] Updating index for: %s\n", index->indexed_path);

    // Clear existing entries
    index->entry_count = 0;
    index->total_files = 0;
    index->total_directories = 0;
    index->total_size = 0;

    // Scan directory tree
    struct timeval start_time, end_time;
    gettimeofday(&start_time, NULL);

    index_directory_recursive(index, index->indexed_path);

    gettimeofday(&end_time, NULL);
    uint64_t elapsed = (end_time.tv_sec - start_time.tv_sec) * 1000000 +
                       (end_time.tv_usec - start_time.tv_usec);

    index->is_valid = true;
    index->last_update_time = time(NULL);

    printf("[Search] Index updated: %u files, %u directories (%lu.%03lu seconds)\n",
           index->total_files, index->total_directories,
           elapsed / 1000000, (elapsed % 1000000) / 1000);
}

/**
 * Check if index is up to date
 */
bool index_is_up_to_date(search_index_t *index) {
    if (!index || !index->is_valid) return false;

    // Consider index stale after 1 hour
    time_t now = time(NULL);
    return (now - index->last_update_time) < 3600;
}

/**
 * Create search query
 */
search_query_t* query_create(const char *text) {
    search_query_t *query = calloc(1, sizeof(search_query_t));
    if (!query) return NULL;

    if (text) {
        strncpy(query->query, text, sizeof(query->query) - 1);
    }

    query->type_filter = FILTER_ALL_FILES;
    query->min_size = 0;
    query->max_size = UINT64_MAX;
    query->date_filter = DATE_ANY;
    query->case_sensitive = false;
    query->regex_mode = false;
    query->content_search = false;
    query->include_hidden = false;
    query->recursive = true;
    query->max_results = MAX_SEARCH_RESULTS;
    query->offset = 0;

    return query;
}

/**
 * Destroy search query
 */
void query_destroy(search_query_t *query) {
    free(query);
}

/**
 * Set type filter
 */
void query_set_type_filter(search_query_t *query, file_type_filter_t filter) {
    if (!query) return;
    query->type_filter = filter;
}

/**
 * Set size range
 */
void query_set_size_range(search_query_t *query, uint64_t min, uint64_t max) {
    if (!query) return;
    query->min_size = min;
    query->max_size = max;
}

/**
 * Set date filter
 */
void query_set_date_filter(search_query_t *query, date_filter_t filter) {
    if (!query) return;
    query->date_filter = filter;

    time_t now = time(NULL);
    struct tm *tm_now = localtime(&now);

    switch (filter) {
        case DATE_TODAY:
            tm_now->tm_hour = 0;
            tm_now->tm_min = 0;
            tm_now->tm_sec = 0;
            query->date_from = mktime(tm_now);
            query->date_to = now;
            break;

        case DATE_YESTERDAY:
            tm_now->tm_mday--;
            tm_now->tm_hour = 0;
            tm_now->tm_min = 0;
            tm_now->tm_sec = 0;
            query->date_from = mktime(tm_now);
            tm_now->tm_hour = 23;
            tm_now->tm_min = 59;
            tm_now->tm_sec = 59;
            query->date_to = mktime(tm_now);
            break;

        case DATE_THIS_WEEK:
            tm_now->tm_mday -= tm_now->tm_wday;
            tm_now->tm_hour = 0;
            tm_now->tm_min = 0;
            tm_now->tm_sec = 0;
            query->date_from = mktime(tm_now);
            query->date_to = now;
            break;

        case DATE_THIS_MONTH:
            tm_now->tm_mday = 1;
            tm_now->tm_hour = 0;
            tm_now->tm_min = 0;
            tm_now->tm_sec = 0;
            query->date_from = mktime(tm_now);
            query->date_to = now;
            break;

        case DATE_THIS_YEAR:
            tm_now->tm_mon = 0;
            tm_now->tm_mday = 1;
            tm_now->tm_hour = 0;
            tm_now->tm_min = 0;
            tm_now->tm_sec = 0;
            query->date_from = mktime(tm_now);
            query->date_to = now;
            break;

        case DATE_ANY:
        case DATE_CUSTOM:
        default:
            break;
    }
}

/**
 * Set date range
 */
void query_set_date_range(search_query_t *query, uint64_t from, uint64_t to) {
    if (!query) return;
    query->date_filter = DATE_CUSTOM;
    query->date_from = from;
    query->date_to = to;
}

/**
 * Set search path
 */
void query_set_path(search_query_t *query, const char *path) {
    if (!query || !path) return;
    strncpy(query->search_path, path, sizeof(query->search_path) - 1);
}

/**
 * Simple pattern matching (supports * and ? wildcards)
 */
bool match_pattern(const char *text, const char *pattern, bool case_sensitive) {
    if (!text || !pattern) return false;

    const char *t = text;
    const char *p = pattern;

    while (*t && *p) {
        if (*p == '*') {
            // Skip multiple asterisks
            while (*p == '*') p++;
            if (!*p) return true;  // Pattern ends with *, matches rest

            // Try matching remainder
            while (*t) {
                if (match_pattern(t, p, case_sensitive)) {
                    return true;
                }
                t++;
            }
            return false;

        } else if (*p == '?') {
            // ? matches any single character
            p++;
            t++;

        } else {
            // Exact character match
            char tc = case_sensitive ? *t : tolower(*t);
            char pc = case_sensitive ? *p : tolower(*p);

            if (tc != pc) return false;
            p++;
            t++;
        }
    }

    // Handle trailing asterisks
    while (*p == '*') p++;

    return (*t == 0 && *p == 0);
}

/**
 * Check if file matches type filter
 */
static bool matches_type_filter(file_type_t type, file_type_filter_t filter) {
    switch (filter) {
        case FILTER_ALL_FILES:
            return true;

        case FILTER_DOCUMENTS:
            return type == FILE_TYPE_TEXT ||
                   type == FILE_TYPE_DOCUMENT ||
                   type == FILE_TYPE_PDF ||
                   type == FILE_TYPE_SPREADSHEET ||
                   type == FILE_TYPE_PRESENTATION;

        case FILTER_IMAGES:
            return type == FILE_TYPE_IMAGE;

        case FILTER_VIDEOS:
            return type == FILE_TYPE_VIDEO;

        case FILTER_AUDIO:
            return type == FILE_TYPE_AUDIO;

        case FILTER_ARCHIVES:
            return type == FILE_TYPE_ARCHIVE;

        case FILTER_CODE:
            return type == FILE_TYPE_CODE;

        case FILTER_EXECUTABLES:
            return type == FILE_TYPE_EXECUTABLE;

        default:
            return true;
    }
}

/**
 * Execute search query on index
 */
search_results_t* search_execute(search_index_t *index, const search_query_t *query) {
    if (!index || !query) return NULL;

    struct timeval start_time, end_time;
    gettimeofday(&start_time, NULL);

    search_results_t *results = calloc(1, sizeof(search_results_t));
    if (!results) return NULL;

    // Allocate results array
    results->results = calloc(MAX_SEARCH_RESULTS, sizeof(search_result_t));
    if (!results->results) {
        free(results);
        return NULL;
    }

    results->count = 0;
    results->total_matches = 0;
    results->truncated = false;

    // Search through index
    for (uint32_t i = 0; i < index->entry_count; i++) {
        index_entry_t *entry = &index->entries[i];

        // Skip hidden files if not included
        if (!query->include_hidden && entry->filename[0] == '.') {
            continue;
        }

        // Check type filter
        if (!matches_type_filter(entry->type, query->type_filter)) {
            continue;
        }

        // Check size range
        if (entry->size < query->min_size || entry->size > query->max_size) {
            continue;
        }

        // Check date range
        if (query->date_filter != DATE_ANY) {
            if (entry->modified_time < query->date_from ||
                entry->modified_time > query->date_to) {
                continue;
            }
        }

        // Check filename match
        if (query->query[0] != '\0') {
            if (!match_pattern(entry->filename, query->query, query->case_sensitive)) {
                continue;
            }
        }

        // Match found!
        results->total_matches++;

        // Skip if before offset
        if (results->total_matches <= query->offset) {
            continue;
        }

        // Add to results
        if (results->count < query->max_results) {
            search_result_t *result = &results->results[results->count];

            // Copy file entry
            strncpy(result->file.name, entry->filename, sizeof(result->file.name) - 1);
            strncpy(result->file.path, entry->full_path, sizeof(result->file.path) - 1);
            result->file.size = entry->size;
            result->file.modified_time = entry->modified_time;
            result->file.type = entry->type;
            result->file.is_directory = entry->is_directory;

            // Calculate match score (simple: exact match scores higher)
            result->match_score = 100;
            if (query->query[0] != '\0') {
                if (strcmp(entry->filename, query->query) == 0) {
                    result->match_score = 1000;  // Exact match
                } else if (strstr(entry->filename, query->query) == entry->filename) {
                    result->match_score = 500;   // Starts with query
                }
            }

            results->count++;
        } else {
            results->truncated = true;
        }
    }

    gettimeofday(&end_time, NULL);
    results->search_time_us = (end_time.tv_sec - start_time.tv_sec) * 1000000 +
                              (end_time.tv_usec - start_time.tv_usec);

    return results;
}

/**
 * Destroy search results
 */
void search_results_destroy(search_results_t *results) {
    if (!results) return;

    if (results->results) {
        free(results->results);
    }

    free(results);
}

/**
 * Sort results by name
 */
void results_sort_by_name(search_results_t *results, bool ascending) {
    if (!results || results->count == 0) return;

    // Simple bubble sort (good enough for small result sets)
    for (uint32_t i = 0; i < results->count - 1; i++) {
        for (uint32_t j = 0; j < results->count - i - 1; j++) {
            int cmp = strcmp(results->results[j].file.name,
                           results->results[j + 1].file.name);

            if ((ascending && cmp > 0) || (!ascending && cmp < 0)) {
                // Swap
                search_result_t temp = results->results[j];
                results->results[j] = results->results[j + 1];
                results->results[j + 1] = temp;
            }
        }
    }
}

/**
 * Sort results by date
 */
void results_sort_by_date(search_results_t *results, bool ascending) {
    if (!results || results->count == 0) return;

    for (uint32_t i = 0; i < results->count - 1; i++) {
        for (uint32_t j = 0; j < results->count - i - 1; j++) {
            uint64_t date1 = results->results[j].file.modified_time;
            uint64_t date2 = results->results[j + 1].file.modified_time;

            if ((ascending && date1 > date2) || (!ascending && date1 < date2)) {
                search_result_t temp = results->results[j];
                results->results[j] = results->results[j + 1];
                results->results[j + 1] = temp;
            }
        }
    }
}

/**
 * Sort results by size
 */
void results_sort_by_size(search_results_t *results, bool ascending) {
    if (!results || results->count == 0) return;

    for (uint32_t i = 0; i < results->count - 1; i++) {
        for (uint32_t j = 0; j < results->count - i - 1; j++) {
            uint64_t size1 = results->results[j].file.size;
            uint64_t size2 = results->results[j + 1].file.size;

            if ((ascending && size1 > size2) || (!ascending && size1 < size2)) {
                search_result_t temp = results->results[j];
                results->results[j] = results->results[j + 1];
                results->results[j + 1] = temp;
            }
        }
    }
}

/**
 * Sort results by relevance (match score)
 */
void results_sort_by_relevance(search_results_t *results) {
    if (!results || results->count == 0) return;

    for (uint32_t i = 0; i < results->count - 1; i++) {
        for (uint32_t j = 0; j < results->count - i - 1; j++) {
            if (results->results[j].match_score < results->results[j + 1].match_score) {
                search_result_t temp = results->results[j];
                results->results[j] = results->results[j + 1];
                results->results[j + 1] = temp;
            }
        }
    }
}
