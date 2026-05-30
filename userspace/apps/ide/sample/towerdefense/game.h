/* game.h - shared types and globals for the tower-defense sample.
 *
 * This header is intentionally self-contained: it pulls in no libc or OS
 * headers so the IDE can parse the project as a closed unit. Every
 * translation unit (.c) in this sample includes this file.
 */
#ifndef TOWERDEFENSE_GAME_H
#define TOWERDEFENSE_GAME_H

#define MAX_ENEMIES   64
#define MAX_BULLETS   128
#define GRID_W        32
#define GRID_H        24

/* A single marching enemy. Enemies live in the g_enemies pool. */
typedef struct enemy {
    int x, y;
    int hp;
    int alive;
} enemy_t;

/* A projectile fired by a tower. Bullets live in the g_bullets pool. */
typedef struct bullet {
    int x, y;
    int dx, dy;
    int active;
} bullet_t;

/* A placed tower. Towers are owned by the caller (stack/array), not pooled. */
typedef struct tower {
    int x, y;
    int range;
    int cooldown;
    int level;
} tower_t;

/* Canonical global pools. Defined once in tower.c, referenced everywhere. */
extern enemy_t  g_enemies[MAX_ENEMIES];
extern bullet_t g_bullets[MAX_BULLETS];

/* tower.c */
void tower_init(tower_t *t);
void tower_tick(tower_t *t);
int  tower_find_target(tower_t *t);
void tower_fire(tower_t *t);
void tower_on_upgrade(tower_t *t);
void tower_destroy(tower_t *t);

/* enemy.c */
int  spawn_enemy(int x, int y, int hp);
void damage_enemy(enemy_t *e, int amount);
int  in_range(tower_t *t, enemy_t *e);
void aim_at(tower_t *t, enemy_t *e);

/* bullet.c */
void spawn_bullet(bullet_t *b, tower_t *t, enemy_t *e);
int  find_free_bullet_slot(void);
void bullet_update(void);

/* wave.c */
void wave_spawn(int n);

/* main.c */
void game_main(void);

/* renderer.c */
void render_frame(void);

#endif /* TOWERDEFENSE_GAME_H */
