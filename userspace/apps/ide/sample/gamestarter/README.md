# Block Catcher — Game Starter Template

A complete, playable 2D game for AutomationOS. Clone this folder to start
your own game. Every customisable value is marked `// CHANGE:` in the source.

## How to play

| Key          | Action                       |
|--------------|------------------------------|
| Left arrow   | Move paddle left             |
| Right arrow  | Move paddle right            |
| R            | Restart (on game-over screen)|
| ESC          | Quit                         |

Catch the falling blocks with your paddle. Miss one and you lose a life.
Blocks fall faster as your score climbs. Purple blocks fall faster than
orange ones.

## What to change

| Want to...                  | Edit this in game.c             |
|-----------------------------|---------------------------------|
| Resize the window           | `WIN_W`, `WIN_H`                |
| Widen/narrow the paddle     | `PADDLE_W`                      |
| Start with more/fewer lives | `START_LIVES`                   |
| Speed up block falling      | `BASE_SPEED`, speed-cap `14`    |
| Spawn blocks faster         | `SPAWN_MS`                      |
| Change colours              | `COL_*` constants               |
| Add WASD controls           | comment in `KEY_A`/`KEY_D` case |
| Add a high-score display    | extend the HUD render section   |

## Build

```
gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin \
    -fno-stack-protector -fno-pic -fno-pie -mno-red-zone -O2 \
    -c userspace/apps/ide/sample/gamestarter/game.c  -o /tmp/game.o
gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin \
    -fno-stack-protector -fno-pic -fno-pie -mno-red-zone -O2 \
    -c userspace/lib/wl/wl_client.c  -o /tmp/wlc.o
gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin \
    -fno-stack-protector -fno-pic -fno-pie -mno-red-zone -O2 \
    -c userspace/lib/font/bitfont.c  -o /tmp/bf.o
ld -nostdlib -static -n -no-pie -e _start \
    -T userspace/userspace.ld \
    /tmp/game.o /tmp/wlc.o /tmp/bf.o \
    -o /tmp/gamestarter.elf
objdump -d /tmp/gamestarter.elf | grep fs:0x28   # must be empty
```

## Key concepts demonstrated

- `wl_connect` / `wl_create_window` / `wl_poll_event` / `wl_commit` — the
  full wl client lifecycle in one file.
- `fill_rect` — direct ARGB32 pixel writes into the shm framebuffer.
- `font_draw_string` / `font_text_width` — HUD text without libc.
- Inline `syscall` with AOS syscall numbers (not Linux).
- Fixed-rate game tick decoupled from render rate via `SYS_GET_TICKS_MS`.
- Key press/release tracking for smooth held-key movement.
- LCG PRNG seeded from tick count — no stdlib rand needed.
