# Performance Optimization Quick Start

**Agent 17: Performance Optimizer**  
**5-minute integration guide**

---

## Build & Test

```bash
cd userspace/compositor
make -f Makefile.performance clean all
make -f Makefile.performance benchmark
```

**Expected:** ✓ SUCCESS: Achieved 35+ FPS (target: 30+ FPS)

---

## Integration

### 1. Include Headers

```c
#include "simd_blit.h"
#include "damage_opt.h"
#include "mempool.h"
#include "profiling.h"
```

### 2. Initialize (once)

```c
simd_blit_init();
mempool_init();
profiler_init();
damage_init(&comp->damage);
```

### 3. Frame Loop

```c
void fb_compositor_frame(fb_compositor_t *comp) {
    profiler_frame_begin();
    
    // Clear (skip if damage tracking)
    if (comp->damage.full_redraw) {
        memset(comp->back_buffer, 0, fb_size);
    }
    
    // Composite windows (use SIMD!)
    for (each window) {
        simd_blit_surface(comp->back_buffer, width, height,
                         win->surface, &win->geometry,
                         win->alpha, use_alpha);
    }
    
    // Flip
    memcpy(comp->fb->pixels, comp->back_buffer, fb_size);
    
    // Clear damage
    damage_clear(&comp->damage);
    
    profiler_frame_end();
}
```

### 4. Window Management

```c
// Create window
window->surface->pixels = mempool_alloc_surface(width, height);

// Destroy window
mempool_free_surface(window->surface->pixels);

// Track changes
damage_add_region(&comp->damage, &window->geometry);
```

---

## Verify

```c
printf("SIMD: %s\n", simd_get_capabilities());
printf("FPS: %u\n", profiler_get_fps());
profiler_print_report();
```

---

**Full details:** See `PERFORMANCE_OPTIMIZATION_REPORT.md`
