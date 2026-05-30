/* renderer.c - draws the current frame. Read-only over the pools. */
#include "game.h"

/* Single-character glyphs blitted into a scratch grid. */
#define GLYPH_EMPTY   ' '
#define GLYPH_ENEMY   'E'
#define GLYPH_BULLET  '*'

static char s_canvas[GRID_H][GRID_W];

static void clear_canvas(void) {
    for (int y = 0; y < GRID_H; y++) {
        for (int x = 0; x < GRID_W; x++) {
            s_canvas[y][x] = GLYPH_EMPTY;
        }
    }
}

static int on_grid(int x, int y) {
    return x >= 0 && x < GRID_W && y >= 0 && y < GRID_H;
}

/* Compose the frame by reading both pools. This function never mutates
 * g_enemies or g_bullets; it only samples them to paint the canvas. */
void render_frame(void) {
    clear_canvas();

    for (int i = 0; i < MAX_ENEMIES; i++) {
        if (!g_enemies[i].alive) continue;
        if (on_grid(g_enemies[i].x, g_enemies[i].y)) {
            s_canvas[g_enemies[i].y][g_enemies[i].x] = GLYPH_ENEMY;
        }
    }

    for (int i = 0; i < MAX_BULLETS; i++) {
        if (!g_bullets[i].active) continue;
        if (on_grid(g_bullets[i].x, g_bullets[i].y)) {
            s_canvas[g_bullets[i].y][g_bullets[i].x] = GLYPH_BULLET;
        }
    }
}
