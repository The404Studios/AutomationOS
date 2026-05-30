/* main.c - top-level game loop tying the systems together. */
#include "game.h"

#define FRAME_COUNT       600
#define WAVE_INTERVAL     90
#define ENEMIES_PER_WAVE  6
#define TOWER_COUNT       3

/* The game's main loop. Sets up a handful of towers, then steps the
 * simulation forward one frame at a time: spawn waves on a cadence, let
 * every tower act, then move all bullets. */
void game_main(void) {
    tower_t towers[TOWER_COUNT];

    for (int i = 0; i < TOWER_COUNT; i++) {
        tower_init(&towers[i]);
        towers[i].x = 4 + i * 8;
        towers[i].y = 12;
    }

    for (int frame = 0; frame < FRAME_COUNT; frame++) {
        if (frame % WAVE_INTERVAL == 0) {
            wave_spawn(ENEMIES_PER_WAVE);
        }

        for (int i = 0; i < TOWER_COUNT; i++) {
            if (towers[i].cooldown > 0) {
                towers[i].cooldown -= 1;
            }
            tower_tick(&towers[i]);
        }

        bullet_update();
        render_frame();
    }
}
