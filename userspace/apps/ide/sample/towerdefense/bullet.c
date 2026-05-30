/* bullet.c - projectile spawning and movement over the g_bullets pool. */
#include "game.h"

/* Initialise a bullet so it travels from the tower toward an enemy.
 * Writes g_bullets (through the b pointer the caller passes in). */
void spawn_bullet(bullet_t *b, tower_t *t, enemy_t *e) {
    b->x = t->x;
    b->y = t->y;

    int dx = e->x - t->x;
    int dy = e->y - t->y;

    /* Normalise to a unit step on each axis. */
    b->dx = (dx > 0) - (dx < 0);
    b->dy = (dy > 0) - (dy < 0);
    b->active = 1;
}

/* Scan the pool for an inactive slot. Reads g_bullets. Returns -1 if full. */
int find_free_bullet_slot(void) {
    for (int i = 0; i < MAX_BULLETS; i++) {
        if (!g_bullets[i].active) {
            return i;
        }
    }
    return -1;
}

/* Advance every live bullet one step and retire those that leave the grid.
 * Writes g_bullets. */
void bullet_update(void) {
    for (int i = 0; i < MAX_BULLETS; i++) {
        if (!g_bullets[i].active) continue;

        g_bullets[i].x += g_bullets[i].dx;
        g_bullets[i].y += g_bullets[i].dy;

        if (g_bullets[i].x < 0 || g_bullets[i].x >= GRID_W ||
            g_bullets[i].y < 0 || g_bullets[i].y >= GRID_H) {
            g_bullets[i].active = 0;
        }
    }
}
