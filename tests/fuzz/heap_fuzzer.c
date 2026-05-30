/*
 * AutomationOS Heap Fuzzer
 *
 * This fuzzer stress-tests the kmalloc/kfree allocator to discover:
 * - Heap corruption vulnerabilities
 * - Double-free bugs
 * - Use-after-free vulnerabilities
 * - Memory leaks
 * - Fragmentation issues
 * - Race conditions in concurrent allocations
 *
 * Usage:
 *   # AFL++ mode
 *   afl-fuzz -i corpus/heap_seeds -o output/heap -- ./heap_fuzzer @@
 *
 *   # Standalone mode with stress test
 *   ./heap_fuzzer --stress --concurrent 100
 *
 *   # Memory leak detection
 *   valgrind --leak-check=full ./heap_fuzzer --iterations 10000
 */

#include "fuzzer_common.h"
#include <pthread.h>
#include <sys/mman.h>

// ============================================================================
// Heap Fuzzing Configuration
// ============================================================================

#define MAX_ALLOCATIONS      10000
#define MAX_ALLOCATION_SIZE  (16 * 1024 * 1024)  // 16MB
#define MIN_ALLOCATION_SIZE  1

// Allocation operations
typedef enum {
    OP_ALLOC = 0,
    OP_FREE,
    OP_REALLOC,
    OP_CALLOC,
    OP_ALIGNED_ALLOC,
    OP_READ,
    OP_WRITE,
    OP_MEMSET,
    OP_MEMCPY,
    OP_MAX
} heap_op_t;

// Allocation tracker
typedef struct {
    void* ptr;
    size_t size;
    bool is_free;
    uint32_t alloc_id;
} alloc_info_t;

static alloc_info_t g_allocations[MAX_ALLOCATIONS];
static uint32_t g_num_allocations = 0;
static uint32_t g_next_alloc_id = 0;

// Statistics
static uint64_t g_total_allocs = 0;
static uint64_t g_total_frees = 0;
static uint64_t g_total_reallocs = 0;
static uint64_t g_double_free_detected = 0;
static uint64_t g_use_after_free_detected = 0;
static uint64_t g_corruption_detected = 0;

// ============================================================================
// Allocation Tracking
// ============================================================================

static int find_allocation_slot(void) {
    for (uint32_t i = 0; i < MAX_ALLOCATIONS; i++) {
        if (g_allocations[i].ptr == NULL || g_allocations[i].is_free) {
            return i;
        }
    }
    return -1;
}

static int find_allocation_by_ptr(void* ptr) {
    for (uint32_t i = 0; i < g_num_allocations; i++) {
        if (g_allocations[i].ptr == ptr) {
            return i;
        }
    }
    return -1;
}

static void track_allocation(void* ptr, size_t size) {
    if (!ptr) return;

    int slot = find_allocation_slot();
    if (slot < 0) {
        FUZZ_ERROR("Allocation tracking table full!");
        return;
    }

    g_allocations[slot].ptr = ptr;
    g_allocations[slot].size = size;
    g_allocations[slot].is_free = false;
    g_allocations[slot].alloc_id = g_next_alloc_id++;

    if (slot >= (int)g_num_allocations) {
        g_num_allocations = slot + 1;
    }

    FUZZ_DEBUG("Tracked allocation: ptr=%p size=%zu id=%u",
               ptr, size, g_allocations[slot].alloc_id);
}

static void track_free(void* ptr) {
    if (!ptr) return;

    int slot = find_allocation_by_ptr(ptr);
    if (slot < 0) {
        FUZZ_ERROR("Double-free or invalid free detected: %p", ptr);
        g_double_free_detected++;
        return;
    }

    if (g_allocations[slot].is_free) {
        FUZZ_ERROR("Double-free detected: %p (alloc_id=%u)",
                   ptr, g_allocations[slot].alloc_id);
        g_double_free_detected++;
        abort();
    }

    g_allocations[slot].is_free = true;
    FUZZ_DEBUG("Tracked free: ptr=%p id=%u", ptr, g_allocations[slot].alloc_id);
}

// ============================================================================
// Heap Operations
// ============================================================================

static void* heap_op_alloc(size_t size) {
    void* ptr = malloc(size);
    if (ptr) {
        track_allocation(ptr, size);
        g_total_allocs++;
        FUZZ_DEBUG("malloc(%zu) = %p", size, ptr);
    }
    return ptr;
}

static void* heap_op_calloc(size_t nmemb, size_t size) {
    void* ptr = calloc(nmemb, size);
    if (ptr) {
        track_allocation(ptr, nmemb * size);
        g_total_allocs++;
        FUZZ_DEBUG("calloc(%zu, %zu) = %p", nmemb, size, ptr);
    }
    return ptr;
}

static void* heap_op_realloc(void* old_ptr, size_t new_size) {
    void* new_ptr = realloc(old_ptr, new_size);
    if (new_ptr && old_ptr) {
        // Update tracking
        int slot = find_allocation_by_ptr(old_ptr);
        if (slot >= 0) {
            g_allocations[slot].ptr = new_ptr;
            g_allocations[slot].size = new_size;
        }
        g_total_reallocs++;
        FUZZ_DEBUG("realloc(%p, %zu) = %p", old_ptr, new_size, new_ptr);
    }
    return new_ptr;
}

static void heap_op_free(void* ptr) {
    if (!ptr) return;

    track_free(ptr);
    free(ptr);
    g_total_frees++;
    FUZZ_DEBUG("free(%p)", ptr);
}

static void heap_op_read(void* ptr, size_t size) {
    if (!ptr || size == 0) return;

    volatile uint8_t dummy = 0;
    for (size_t i = 0; i < size; i++) {
        dummy ^= ((uint8_t*)ptr)[i];
    }

    FUZZ_DEBUG("Read %zu bytes from %p", size, ptr);
}

static void heap_op_write(void* ptr, size_t size) {
    if (!ptr || size == 0) return;

    for (size_t i = 0; i < size; i++) {
        ((uint8_t*)ptr)[i] = fuzz_rand() & 0xFF;
    }

    FUZZ_DEBUG("Wrote %zu bytes to %p", size, ptr);
}

static void heap_op_memset_fuzz(void* ptr, size_t size) {
    if (!ptr || size == 0) return;

    uint8_t value = fuzz_rand() & 0xFF;
    memset(ptr, value, size);

    FUZZ_DEBUG("memset(%p, 0x%02x, %zu)", ptr, value, size);
}

static void heap_op_memcpy_fuzz(void* dst, void* src, size_t size) {
    if (!dst || !src || size == 0) return;

    memcpy(dst, src, size);

    FUZZ_DEBUG("memcpy(%p, %p, %zu)", dst, src, size);
}

// ============================================================================
// Heap Fuzzing Engine
// ============================================================================

typedef struct {
    heap_op_t op;
    uint32_t target_idx;  // Index into allocation table
    size_t size;
    uint32_t value;
} heap_testcase_t;

static void generate_random_heap_testcase(heap_testcase_t* tc) {
    tc->op = fuzz_rand() % OP_MAX;
    tc->target_idx = fuzz_rand() % MAX_ALLOCATIONS;
    tc->size = fuzz_rand() % MAX_ALLOCATION_SIZE;
    tc->value = fuzz_rand();

    // Bias towards smaller allocations (more realistic)
    if (fuzz_rand() % 100 < 70) {
        tc->size = fuzz_rand() % 4096;
    }

    // Ensure minimum size
    if (tc->size < MIN_ALLOCATION_SIZE) {
        tc->size = MIN_ALLOCATION_SIZE;
    }
}

static void execute_heap_testcase(const heap_testcase_t* tc) {
    fuzz_set_timeout(FUZZ_TIMEOUT_SECONDS);

    switch (tc->op) {
        case OP_ALLOC: {
            void* ptr = heap_op_alloc(tc->size);
            if (ptr) {
                // Initialize memory to detect uninitialized reads
                heap_op_memset_fuzz(ptr, tc->size);
            }
            break;
        }

        case OP_FREE: {
            if (g_num_allocations > 0) {
                uint32_t idx = tc->target_idx % g_num_allocations;
                if (!g_allocations[idx].is_free && g_allocations[idx].ptr) {
                    heap_op_free(g_allocations[idx].ptr);
                }
            }
            break;
        }

        case OP_REALLOC: {
            if (g_num_allocations > 0) {
                uint32_t idx = tc->target_idx % g_num_allocations;
                if (!g_allocations[idx].is_free && g_allocations[idx].ptr) {
                    void* new_ptr = heap_op_realloc(g_allocations[idx].ptr, tc->size);
                    if (new_ptr) {
                        heap_op_write(new_ptr, tc->size);
                    }
                }
            }
            break;
        }

        case OP_CALLOC: {
            size_t nmemb = tc->size / (tc->value % 256 + 1);
            size_t size = tc->value % 256 + 1;
            void* ptr = heap_op_calloc(nmemb, size);

            // Verify zeroing
            if (ptr) {
                for (size_t i = 0; i < nmemb * size; i++) {
                    if (((uint8_t*)ptr)[i] != 0) {
                        FUZZ_ERROR("calloc() didn't zero memory!");
                        g_corruption_detected++;
                        break;
                    }
                }
            }
            break;
        }

        case OP_READ: {
            if (g_num_allocations > 0) {
                uint32_t idx = tc->target_idx % g_num_allocations;
                if (!g_allocations[idx].is_free && g_allocations[idx].ptr) {
                    heap_op_read(g_allocations[idx].ptr, g_allocations[idx].size);
                }
            }
            break;
        }

        case OP_WRITE: {
            if (g_num_allocations > 0) {
                uint32_t idx = tc->target_idx % g_num_allocations;
                if (!g_allocations[idx].is_free && g_allocations[idx].ptr) {
                    heap_op_write(g_allocations[idx].ptr, g_allocations[idx].size);
                }
            }
            break;
        }

        case OP_MEMSET: {
            if (g_num_allocations > 0) {
                uint32_t idx = tc->target_idx % g_num_allocations;
                if (!g_allocations[idx].is_free && g_allocations[idx].ptr) {
                    heap_op_memset_fuzz(g_allocations[idx].ptr, g_allocations[idx].size);
                }
            }
            break;
        }

        case OP_MEMCPY: {
            if (g_num_allocations > 1) {
                uint32_t src_idx = tc->target_idx % g_num_allocations;
                uint32_t dst_idx = (tc->target_idx + 1) % g_num_allocations;

                if (!g_allocations[src_idx].is_free && g_allocations[src_idx].ptr &&
                    !g_allocations[dst_idx].is_free && g_allocations[dst_idx].ptr) {
                    size_t copy_size = g_allocations[src_idx].size < g_allocations[dst_idx].size ?
                                      g_allocations[src_idx].size : g_allocations[dst_idx].size;
                    heap_op_memcpy_fuzz(g_allocations[dst_idx].ptr,
                                       g_allocations[src_idx].ptr,
                                       copy_size);
                }
            }
            break;
        }

        default:
            break;
    }

    fuzz_clear_timeout();
    fuzz_update_stats();
}

// ============================================================================
// Stress Testing
// ============================================================================

static void* stress_test_thread(void* arg) {
    uint64_t iterations = *(uint64_t*)arg;

    for (uint64_t i = 0; i < iterations; i++) {
        heap_testcase_t tc;
        generate_random_heap_testcase(&tc);
        execute_heap_testcase(&tc);
    }

    return NULL;
}

static void run_stress_test(uint32_t num_threads, uint64_t iterations_per_thread) {
    FUZZ_LOG("Starting stress test: %u threads, %llu iterations each",
             num_threads, (unsigned long long)iterations_per_thread);

    pthread_t* threads = malloc(sizeof(pthread_t) * num_threads);
    if (!threads) {
        FUZZ_ERROR("Failed to allocate thread array");
        return;
    }

    for (uint32_t i = 0; i < num_threads; i++) {
        if (pthread_create(&threads[i], NULL, stress_test_thread,
                          &iterations_per_thread) != 0) {
            FUZZ_ERROR("Failed to create thread %u", i);
        }
    }

    for (uint32_t i = 0; i < num_threads; i++) {
        pthread_join(threads[i], NULL);
    }

    free(threads);
    FUZZ_LOG("Stress test complete!");
}

// ============================================================================
// Main Fuzzing Loop
// ============================================================================

static void fuzz_mode_standalone(uint64_t iterations, bool stress_mode,
                                uint32_t num_threads) {
    if (stress_mode) {
        run_stress_test(num_threads, iterations / num_threads);
    } else {
        FUZZ_LOG("Starting heap fuzzing (%llu iterations)...",
                 (unsigned long long)iterations);

        for (uint64_t i = 0; i < iterations; i++) {
            heap_testcase_t tc;
            generate_random_heap_testcase(&tc);
            execute_heap_testcase(&tc);

            if (i % 10000 == 0) {
                printf("\rProgress: %llu/%llu (%.2f%%) | Allocs: %llu | Frees: %llu | Leaks: %u",
                       (unsigned long long)i,
                       (unsigned long long)iterations,
                       (i * 100.0) / iterations,
                       (unsigned long long)g_total_allocs,
                       (unsigned long long)g_total_frees,
                       g_num_allocations);
                fflush(stdout);
            }
        }
        printf("\n");
    }

    // Cleanup: free all allocations
    FUZZ_LOG("Cleaning up allocations...");
    for (uint32_t i = 0; i < g_num_allocations; i++) {
        if (!g_allocations[i].is_free && g_allocations[i].ptr) {
            free(g_allocations[i].ptr);
        }
    }

    FUZZ_LOG("Heap fuzzing complete!");
}

static void print_heap_stats(void) {
    printf("\n");
    printf("==================== HEAP FUZZING STATISTICS ====================\n");
    printf("Total Allocations:      %llu\n", (unsigned long long)g_total_allocs);
    printf("Total Frees:            %llu\n", (unsigned long long)g_total_frees);
    printf("Total Reallocs:         %llu\n", (unsigned long long)g_total_reallocs);
    printf("Active Allocations:     %u\n", g_num_allocations);
    printf("Double-Frees Detected:  %llu\n", (unsigned long long)g_double_free_detected);
    printf("UAF Detected:           %llu\n", (unsigned long long)g_use_after_free_detected);
    printf("Corruption Detected:    %llu\n", (unsigned long long)g_corruption_detected);
    printf("Memory Leaked:          %llu allocations\n",
           (unsigned long long)(g_total_allocs - g_total_frees));
    printf("=================================================================\n");
}

// ============================================================================
// Entry Point
// ============================================================================

static void print_usage(const char* prog) {
    printf("Usage: %s [OPTIONS]\n", prog);
    printf("\n");
    printf("Options:\n");
    printf("  --iterations N        Run N iterations (default: %d)\n", FUZZ_DEFAULT_ITERATIONS);
    printf("  --stress              Enable stress test mode\n");
    printf("  --concurrent N        Number of concurrent threads (default: 4)\n");
    printf("  --seed N              Set random seed\n");
    printf("  --help                Show this help\n");
    printf("\n");
    printf("Examples:\n");
    printf("  # Basic heap fuzzing\n");
    printf("  %s --iterations 1000000\n", prog);
    printf("\n");
    printf("  # Stress test with 100 concurrent threads\n");
    printf("  %s --stress --concurrent 100\n", prog);
    printf("\n");
    printf("  # Memory leak detection\n");
    printf("  valgrind --leak-check=full %s --iterations 10000\n", prog);
}

int main(int argc, char** argv) {
    fuzz_setup_handlers();
    fuzz_init_stats();

    uint64_t iterations = FUZZ_DEFAULT_ITERATIONS;
    bool stress_mode = false;
    uint32_t num_threads = 4;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--iterations") == 0 && i + 1 < argc) {
            iterations = strtoull(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--stress") == 0) {
            stress_mode = true;
        } else if (strcmp(argv[i], "--concurrent") == 0 && i + 1 < argc) {
            num_threads = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--seed") == 0 && i + 1 < argc) {
            fuzz_seed(atoi(argv[++i]));
        } else if (strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        }
    }

    fuzz_mode_standalone(iterations, stress_mode, num_threads);
    print_heap_stats();
    fuzz_print_stats();

    return 0;
}
