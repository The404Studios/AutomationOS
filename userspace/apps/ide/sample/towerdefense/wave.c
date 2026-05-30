/* wave.c - enemy wave generation. */
#include "game.h"

/* Current wave counter, bumped once per spawned wave. */
int g_wave = 0;

/* Scale enemy toughness with the wave number so later waves hit harder. */
static int wave_hp(int wave) {
    int hp = 10 + wave * 2;
    if (hp > 80) {
        hp = 80;
    }
    return hp;
}

/* Spread the spawn columns so enemies do not all stack on column zero. */
static int wave_column(int index) {
    return (index * 3) % GRID_W;
}

/* Spawn n enemies along the top edge of the grid. Delegates the actual
 * pool write to spawn_enemy in enemy.c. */
void wave_spawn(int n) {
    g_wave += 1;

    int hp = wave_hp(g_wave);

    for (int i = 0; i < n; i++) {
        int x = wave_column(i);
        int y = 0;
        if (spawn_enemy(x, y, hp) < 0) {
            /* Pool is full; stop early rather than spinning. */
            break;
        }
    }
}
