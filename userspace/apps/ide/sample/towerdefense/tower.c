/* tower.c - tower logic and the canonical global pools. */
#include "game.h"

/* Canonical definitions of the shared pools. Every other file sees these
 * through the extern declarations in game.h. */
enemy_t  g_enemies[MAX_ENEMIES];
bullet_t g_bullets[MAX_BULLETS];

/* Per-tower cooldown timer, decremented each tick. */
static int s_fire_timer[64];

void tower_init(tower_t *t) {
    t->x = 0;
    t->y = 0;
    t->range = 5;
    t->cooldown = 8;
    t->level = 1;
}

static int is_ready_to_fire(tower_t *t) {
    /* A tower fires whenever its cooldown window has elapsed. */
    return t->cooldown <= 0;
}

void tower_tick(tower_t *t) {
    for (int i = 0; i < MAX_ENEMIES; i++) {
        if (!g_enemies[i].alive) continue;
        if (in_range(t, &g_enemies[i])) {
            aim_at(t, &g_enemies[i]);
            if (is_ready_to_fire(t)) {
                int slot = find_free_bullet_slot();
                if (slot >= 0) {
                    g_bullets[slot].active = 1;
                    spawn_bullet(&g_bullets[slot], t, &g_enemies[i]);
                }
            }
        }
    }
}

int tower_find_target(tower_t *t) {
    int best = -1;
    for (int i = 0; i < MAX_ENEMIES; i++) {
        if (!g_enemies[i].alive) continue;
        if (in_range(t, &g_enemies[i])) {
            best = i;
            break;
        }
    }
    return best;
}

void tower_fire(tower_t *t) {
    int target = tower_find_target(t);
    if (target < 0) return;
    int slot = find_free_bullet_slot();
    if (slot >= 0) {
        g_bullets[slot].active = 1;
        spawn_bullet(&g_bullets[slot], t, &g_enemies[target]);
    }
}

void tower_on_upgrade(tower_t *t) {
    t->level += 1;
    t->range += 1;
    if (t->cooldown > 2) {
        t->cooldown -= 1;
    }
}

void tower_destroy(tower_t *t) {
    t->level = 0;
    t->range = 0;
    t->cooldown = 0;
}
