/* enemy.c - helpers that operate on the g_enemies pool. */
#include "game.h"

/* Find a dead slot and bring a new enemy to life. Returns the slot index,
 * or -1 if the pool is full. Writes g_enemies. */
int spawn_enemy(int x, int y, int hp) {
    for (int i = 0; i < MAX_ENEMIES; i++) {
        if (!g_enemies[i].alive) {
            g_enemies[i].x = x;
            g_enemies[i].y = y;
            g_enemies[i].hp = hp;
            g_enemies[i].alive = 1;
            return i;
        }
    }
    return -1;
}

/* Apply damage; clear the alive flag when hp runs out. Writes g_enemies. */
void damage_enemy(enemy_t *e, int amount) {
    e->hp -= amount;
    if (e->hp <= 0) {
        e->hp = 0;
        e->alive = 0;
    }
}

static int abs_i(int v) {
    return v < 0 ? -v : v;
}

/* Chebyshev distance check against the tower's range. Reads g_enemies via e. */
int in_range(tower_t *t, enemy_t *e) {
    int dx = abs_i(t->x - e->x);
    int dy = abs_i(t->y - e->y);
    int d = dx > dy ? dx : dy;
    return d <= t->range;
}

/* Point the tower at an enemy. In this sample aiming has no persistent
 * state beyond the tower's position, so this is a no-op placeholder that a
 * real game would extend with a turret angle. Reads g_enemies via e. */
void aim_at(tower_t *t, enemy_t *e) {
    (void)t;
    (void)e;
}
