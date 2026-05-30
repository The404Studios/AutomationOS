# GUI App Starter Template

A minimal, heavily-commented windowed application you can clone and modify
in the AutomationOS IDE to build your own ring-3 GUI app.

## What the app shows

- A titled window (360 x 280) with an indigo title bar and a [Quit] button.
- **Counter section** -- [+1] increments a live label; [Reset] zeroes it.
- **Text input section** -- click the textbox, type freely, press [Echo] to
  mirror the text in a label below.
- **Elapsed time** -- a per-frame tick callback updates "Elapsed: Xs" live.
- **Hint bar** -- a static footer with usage tips.

## wl / draw / input calls used

| Call | Purpose |
|---|---|
| `ui_app_create(title, w, h)` | open window, connect to compositor |
| `ui_app_root(app)` | get the root panel |
| `ui_panel(parent, x, y, w, h, bg)` | filled rectangle / container |
| `ui_label(parent, x, y, text, color)` | static or live text |
| `ui_button(parent, x, y, w, h, text, cb, ud)` | clickable button |
| `ui_textbox(parent, x, y, w, maxlen)` | single-line text input |
| `ui_textbox_text(widget)` | read textbox content |
| `ui_label_set_text(widget, text)` | update a label in a callback |
| `ui_app_set_tick(app, cb, ud)` | per-frame update hook |
| `ui_app_run(app)` | enter the event loop (never returns) |

Input (keyboard + pointer) is dispatched automatically by the toolkit.
The app never calls `wl_poll_event` directly; that is handled inside
`ui_app_run`.

## Build (on-device cc or host gcc)

```sh
gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin \
    -fno-stack-protector -fno-pic -fno-pie -mno-red-zone -O2 \
    -I userspace \
    -c userspace/apps/ide/sample/appstarter/app.c -o /tmp/app.o

gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin \
    -fno-stack-protector -fno-pic -fno-pie -mno-red-zone -O2 \
    -I userspace -c userspace/lib/ui/ui.c        -o /tmp/ui.o

gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin \
    -fno-stack-protector -fno-pic -fno-pie -mno-red-zone -O2 \
    -I userspace -c userspace/lib/wl/wl_client.c -o /tmp/wlc.o

gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin \
    -fno-stack-protector -fno-pic -fno-pie -mno-red-zone -O2 \
    -I userspace -c userspace/lib/font/bitfont.c -o /tmp/bf.o

ld -nostdlib -static -n -no-pie -e _start \
    -T userspace/userspace.ld \
    /tmp/app.o /tmp/ui.o /tmp/wlc.o /tmp/bf.o -o /tmp/app.elf

# Must produce NO output (no stack-canary references):
objdump -d /tmp/app.elf | grep fs:0x28
```

## How to customise

1. **Add a button** -- call `ui_button(panel, x, y, w, h, "Label", my_cb, ud)`
   and write `static void my_cb(void *ud) { ... }`.
2. **Update a label** -- store the pointer returned by `ui_label(...)` in your
   state struct, then call `ui_label_set_text(ptr, new_text)` anywhere.
3. **Live data** -- add fields to `app_state_t` and poll them in `on_tick()`.
4. **More widgets** -- `ui_checkbox`, `ui_slider`, `ui_progress`, `ui_scroll`
   are all available; see `userspace/lib/ui/ui.h` for their signatures.

All mutable state **must** live in file-static variables (or static structs)
because `ui_app_run()` never returns -- stack-allocated state would be
unreachable from callbacks.
