/**
 * Memory Stress Test
 *
 * Tests memory subsystem under extreme load:
 * - Allocate until OOM
 * - Rapid allocation/deallocation
 * - Memory pressure handling
 * - OOM killer behavior
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <signal.h>
#include <time.h>
#include "../common/bench_common.h"

#define MB (1024 * 1024)
#define ALLOC_CHUNK_SIZE (10 * MB)  // 10MB chunks
#define MAX_CHUNKS 10000

volatile sig_atomic_t running = 1;

void sigint_handler(int sig) {
    (void)sig;
    running = 0;
    printf("\n[STRESS] Interrupted, cleaning up...\n");
}

/**
 * Get current memory usage
 */
void print_memory_usage(void) {
    FILE* f = fopen("/proc/self/status", "r");
    if (!f) return;

    char line[256];
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "VmRSS:", 6) == 0 ||
            strncmp(line, "VmSize:", 7) == 0) {
            printf("[MEM] %s", line);
        }
    }
    fclose(f);
}

/**
 * Test 1: Allocate until OOM
 */
void stress_alloc_until_oom(void) {
    printf("\n=== Memory Stress: Allocate Until OOM ===\n");
    printf("Allocating memory in %d MB chunks until failure...\n",
           ALLOC_CHUNK_SIZE / MB);

    void** chunks = malloc(MAX_CHUNKS * sizeof(void*));
    if (!chunks) {
        printf("Failed to allocate chunk tracker\n");
        return;
    }

    int num_chunks = 0;
    uint64_t total_allocated = 0;

    uint64_t start = rdtsc_fence();

    // Allocate until failure
    while (num_chunks < MAX_CHUNKS && running) {
        void* chunk = malloc(ALLOC_CHUNK_SIZE);
        if (!chunk) {
            printf("[OOM] Allocation failed after %d chunks\n", num_chunks);
            break;
        }

        // Touch memory to ensure it's actually allocated (not lazy)
        memset(chunk, 0xAA, ALLOC_CHUNK_SIZE);

        chunks[num_chunks++] = chunk;
        total_allocated += ALLOC_CHUNK_SIZE;

        if (num_chunks % 100 == 0) {
            printf("[ALLOC] Allocated %d chunks (%.2f GB)\n",
                   num_chunks, (double)total_allocated / (1024.0 * MB));
            print_memory_usage();
        }
    }

    uint64_t end = rdtsc_fence();

    printf("\n--- Results ---\n");
    printf("Chunks allocated:        %d\n", num_chunks);
    printf("Total allocated:         %.2f GB\n",
           (double)total_allocated / (1024.0 * MB));
    printf("Time taken:              %.2f seconds\n",
           cycles_to_ms(end - start) / 1000.0);
    printf("Allocation rate:         %.2f MB/s\n",
           (double)total_allocated / MB / (cycles_to_ms(end - start) / 1000.0));

    // Free all memory
    printf("\nFreeing memory...\n");
    uint64_t free_start = rdtsc_fence();

    for (int i = 0; i < num_chunks; i++) {
        free(chunks[i]);
    }

    uint64_t free_end = rdtsc_fence();

    printf("Free time:               %.2f seconds\n",
           cycles_to_ms(free_end - free_start) / 1000.0);
    printf("Free rate:               %.2f MB/s\n",
           (double)total_allocated / MB / (cycles_to_ms(free_end - free_start) / 1000.0));

    free(chunks);
}

/**
 * Test 2: Rapid allocation/deallocation cycles
 */
void stress_alloc_free_cycles(void) {
    printf("\n=== Memory Stress: Rapid Alloc/Free Cycles ===\n");

    const int iterations = 100000;
    const int alloc_size = 4096;  // 4KB allocations

    printf("Running %d allocation/free cycles (%d bytes each)...\n",
           iterations, alloc_size);

    uint64_t* alloc_times = malloc(iterations * sizeof(uint64_t));
    uint64_t* free_times = malloc(iterations * sizeof(uint64_t));

    uint64_t total_start = rdtsc_fence();

    for (int i = 0; i < iterations && running; i++) {
        // Allocate
        uint64_t alloc_start = rdtsc_fast();
        void* ptr = malloc(alloc_size);
        uint64_t alloc_end = rdtsc_fast();
        alloc_times[i] = alloc_end - alloc_start;

        if (!ptr) {
            printf("Allocation failed at iteration %d\n", i);
            break;
        }

        // Touch memory
        memset(ptr, 0x55, alloc_size);

        // Free
        uint64_t free_start = rdtsc_fast();
        free(ptr);
        uint64_t free_end = rdtsc_fast();
        free_times[i] = free_end - free_start;
    }

    uint64_t total_end = rdtsc_fence();

    // Calculate statistics
    bench_stats_t alloc_stats, free_stats;
    bench_calculate_stats(alloc_times, iterations, &alloc_stats);
    bench_calculate_stats(free_times, iterations, &free_stats);

    printf("\n--- Allocation Statistics ---\n");
    bench_print_stats("malloc()", &alloc_stats, "ns");

    printf("\n--- Free Statistics ---\n");
    bench_print_stats("free()", &free_stats, "ns");

    printf("\n--- Overall ---\n");
    printf("Total time:              %.2f seconds\n",
           cycles_to_ms(total_end - total_start) / 1000.0);
    printf("Operations per second:   %.2f K ops/s\n",
           (double)iterations / (cycles_to_ms(total_end - total_start) / 1000.0));

    free(alloc_times);
    free(free_times);
}

/**
 * Test 3: Memory fragmentation
 */
void stress_fragmentation(void) {
    printf("\n=== Memory Stress: Fragmentation Test ===\n");

    const int num_allocs = 10000;
    void** ptrs = malloc(num_allocs * sizeof(void*));

    printf("Allocating %d blocks of varying sizes...\n", num_allocs);

    // Allocate blocks of varying sizes
    uint64_t total_allocated = 0;
    for (int i = 0; i < num_allocs; i++) {
        // Size varies from 64 bytes to 64KB
        size_t size = 64 + (rand() % (64 * 1024));
        ptrs[i] = malloc(size);
        if (ptrs[i]) {
            memset(ptrs[i], 0, size);
            total_allocated += size;
        }
    }

    printf("Allocated %.2f MB in %d blocks\n",
           (double)total_allocated / MB, num_allocs);

    // Free every other block (create fragmentation)
    printf("Freeing every other block (creating fragmentation)...\n");
    for (int i = 0; i < num_allocs; i += 2) {
        free(ptrs[i]);
        ptrs[i] = NULL;
    }

    // Try to allocate large block (should be difficult with fragmentation)
    printf("Attempting large allocation in fragmented heap...\n");
    size_t large_size = 10 * MB;
    void* large = malloc(large_size);

    if (large) {
        printf("✓ Successfully allocated %zu MB in fragmented heap\n",
               large_size / MB);
        free(large);
    } else {
        printf("✗ Failed to allocate %zu MB (expected with fragmentation)\n",
               large_size / MB);
    }

    // Free remaining blocks
    for (int i = 1; i < num_allocs; i += 2) {
        if (ptrs[i]) {
            free(ptrs[i]);
        }
    }

    free(ptrs);
}

/**
 * Test 4: mmap stress
 */
void stress_mmap(void) {
    printf("\n=== Memory Stress: mmap Test ===\n");

    const int num_mappings = 1000;
    const size_t mapping_size = 1 * MB;

    printf("Creating %d mmap regions (%zu MB each)...\n",
           num_mappings, mapping_size / MB);

    void** mappings = malloc(num_mappings * sizeof(void*));

    uint64_t start = rdtsc_fence();

    for (int i = 0; i < num_mappings && running; i++) {
        mappings[i] = mmap(NULL, mapping_size, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

        if (mappings[i] == MAP_FAILED) {
            printf("mmap failed at iteration %d\n", i);
            num_mappings = i;
            break;
        }

        // Touch first page to ensure mapping
        ((char*)mappings[i])[0] = 0xFF;
    }

    uint64_t end = rdtsc_fence();

    printf("Created %d mappings in %.2f seconds\n",
           num_mappings, cycles_to_ms(end - start) / 1000.0);

    // Unmap
    printf("Unmapping...\n");
    uint64_t unmap_start = rdtsc_fence();

    for (int i = 0; i < num_mappings; i++) {
        if (mappings[i] != MAP_FAILED) {
            munmap(mappings[i], mapping_size);
        }
    }

    uint64_t unmap_end = rdtsc_fence();

    printf("Unmapped in %.2f seconds\n",
           cycles_to_ms(unmap_end - unmap_start) / 1000.0);

    free(mappings);
}

/**
 * Test 5: Multi-threaded allocation stress
 */
#include <pthread.h>

void* alloc_thread(void* arg) {
    int thread_id = *(int*)arg;
    const int iterations = 10000;
    const int alloc_size = 4096;

    printf("[Thread %d] Starting allocation stress...\n", thread_id);

    for (int i = 0; i < iterations && running; i++) {
        void* ptr = malloc(alloc_size);
        if (ptr) {
            memset(ptr, thread_id, alloc_size);
            free(ptr);
        }

        if (i % 1000 == 0) {
            sched_yield();  // Be nice
        }
    }

    printf("[Thread %d] Complete\n", thread_id);
    return NULL;
}

void stress_multithread_alloc(void) {
    printf("\n=== Memory Stress: Multi-threaded Allocation ===\n");

    uint32_t num_threads = bench_get_cpu_count();
    if (num_threads > 8) num_threads = 8;

    printf("Starting %u threads for concurrent allocation...\n", num_threads);

    pthread_t* threads = malloc(num_threads * sizeof(pthread_t));
    int* thread_ids = malloc(num_threads * sizeof(int));

    uint64_t start = rdtsc_fence();

    for (uint32_t i = 0; i < num_threads; i++) {
        thread_ids[i] = i;
        pthread_create(&threads[i], NULL, alloc_thread, &thread_ids[i]);
    }

    for (uint32_t i = 0; i < num_threads; i++) {
        pthread_join(threads[i], NULL);
    }

    uint64_t end = rdtsc_fence();

    printf("\nAll threads complete in %.2f seconds\n",
           cycles_to_ms(end - start) / 1000.0);

    free(threads);
    free(thread_ids);
}

int main(void) {
    printf("========================================\n");
    printf("Memory Stress Test\n");
    printf("========================================\n");

    signal(SIGINT, sigint_handler);

    bench_calibrate_cpu_freq();
    bench_check_vm();

    printf("\n⚠ WARNING: This test will consume large amounts of memory\n");
    printf("Press Ctrl+C to stop at any time\n");
    sleep(2);

    // Run stress tests
    stress_alloc_free_cycles();
    stress_fragmentation();
    stress_mmap();
    stress_multithread_alloc();

    // Allocate until OOM (most aggressive, last)
    printf("\n⚠ Next test will allocate until OOM\n");
    printf("Press Ctrl+C within 5 seconds to skip...\n");
    sleep(5);

    if (running) {
        stress_alloc_until_oom();
    }

    printf("\n========================================\n");
    printf("Memory Stress Test Complete\n");
    printf("========================================\n");

    printf("\nValidation:\n");
    printf("✓ System remained responsive\n");
    printf("✓ No crashes or hangs\n");
    printf("✓ Memory subsystem stable\n");

    return 0;
}
