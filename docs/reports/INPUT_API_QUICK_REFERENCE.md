# Input API Quick Reference

Quick reference for using the input event system in AutomationOS.

---

## Userspace Application

### Opening an Input Device

```c
#include "libinput/input.h"

int fd = input_open("/dev/input/event0");
if (fd < 0) {
    fprintf(stderr, "Failed to open input device\n");
    return -1;
}
```

### Reading Events

**Single Event:**
```c
struct input_event event;
if (input_read_event(fd, &event) == 0) {
    // Process event
    if (event.type == EV_KEY && event.value == KEY_PRESSED) {
        printf("Key %s pressed\n", input_key_name(event.code));
    }
}
```

**Multiple Events:**
```c
struct input_event events[32];
int count = input_read_events(fd, events, 32);
for (int i = 0; i < count; i++) {
    input_print_event(&events[i]);
}
```

### Closing Device

```c
input_close(fd);
```

---

## Event Types

| Type | Value | Description | Use Case |
|------|-------|-------------|----------|
| EV_SYN | 0x00 | Synchronization | Event batch marker |
| EV_KEY | 0x01 | Keys and buttons | Keyboard, mouse buttons |
| EV_REL | 0x02 | Relative axes | Mouse movement |
| EV_ABS | 0x03 | Absolute axes | Touchscreen, joystick |

---

## Key Events (EV_KEY)

### Values
- `0` - KEY_RELEASED (key up)
- `1` - KEY_PRESSED (key down)
- `2` - KEY_REPEAT (auto-repeat)

### Common Key Codes
```c
KEY_ESC         // Escape
KEY_1 - KEY_0   // Number row
KEY_A - KEY_Z   // Letters
KEY_ENTER       // Enter
KEY_SPACE       // Spacebar
KEY_BACKSPACE   // Backspace
KEY_TAB         // Tab
KEY_LEFTSHIFT   // Left Shift
KEY_LEFTCTRL    // Left Control
KEY_LEFTALT     // Left Alt
KEY_F1 - KEY_F10 // Function keys
```

### Mouse Buttons
```c
BTN_LEFT        // 0x110
BTN_RIGHT       // 0x111
BTN_MIDDLE      // 0x112
```

---

## Relative Events (EV_REL)

### Values
Delta movement (signed integer)

### Axes
```c
REL_X           // Horizontal movement
REL_Y           // Vertical movement
REL_WHEEL       // Scroll wheel
REL_HWHEEL      // Horizontal wheel
```

### Example
```c
if (event.type == EV_REL) {
    if (event.code == REL_X) {
        mouse_x += event.value;
    } else if (event.code == REL_Y) {
        mouse_y += event.value;
    }
}
```

---

## Example: Compositor Input Loop

```c
#include "libinput/input.h"

void compositor_input_loop() {
    int kbd_fd = input_open("/dev/input/event0");
    if (kbd_fd < 0) return;
    
    struct input_event event;
    
    while (running) {
        if (input_read_event(kbd_fd, &event) == 0) {
            switch (event.type) {
                case EV_KEY:
                    if (event.value == KEY_PRESSED) {
                        handle_key_press(event.code);
                    } else if (event.value == KEY_RELEASED) {
                        handle_key_release(event.code);
                    }
                    break;
                    
                case EV_REL:
                    if (event.code == REL_X) {
                        cursor_x += event.value;
                    } else if (event.code == REL_Y) {
                        cursor_y += event.value;
                    }
                    break;
            }
        }
        
        // Render frame
        compositor_render();
    }
    
    input_close(kbd_fd);
}
```

---

## Example: Keyboard Shortcut Handler

```c
#include "libinput/input.h"

static bool ctrl_pressed = false;
static bool alt_pressed = false;

void handle_key_event(struct input_event* event) {
    if (event->type != EV_KEY) return;
    
    bool pressed = (event->value == KEY_PRESSED);
    
    // Track modifiers
    switch (event->code) {
        case KEY_LEFTCTRL:
        case KEY_RIGHTCTRL:
            ctrl_pressed = pressed;
            return;
        case KEY_LEFTALT:
        case KEY_RIGHTALT:
            alt_pressed = pressed;
            return;
    }
    
    // Handle shortcuts on press only
    if (!pressed) return;
    
    // Ctrl+C
    if (ctrl_pressed && event->code == KEY_C) {
        send_signal(focused_window, SIGINT);
    }
    
    // Alt+Tab
    if (alt_pressed && event->code == KEY_TAB) {
        switch_window();
    }
    
    // Alt+F4
    if (alt_pressed && event->code == KEY_F4) {
        close_window(focused_window);
    }
}
```

---

## Example: Text Input

```c
static char input_buffer[256];
static size_t input_pos = 0;

void handle_text_input(struct input_event* event) {
    if (event->type != EV_KEY) return;
    if (event->value != KEY_PRESSED) return;
    
    // Backspace
    if (event->code == KEY_BACKSPACE) {
        if (input_pos > 0) {
            input_pos--;
            input_buffer[input_pos] = '\0';
        }
        return;
    }
    
    // Enter
    if (event->code == KEY_ENTER) {
        // Submit input
        submit_text(input_buffer);
        input_pos = 0;
        input_buffer[0] = '\0';
        return;
    }
    
    // Convert keycode to ASCII (simplified)
    char c = keycode_to_ascii(event->code);
    if (c && input_pos < sizeof(input_buffer) - 1) {
        input_buffer[input_pos++] = c;
        input_buffer[input_pos] = '\0';
    }
}
```

---

## Kernel Driver: Reporting Events

### Registering an Input Device

```c
#include "kernel/include/input.h"

input_device_t* kbd = input_allocate_device("My Keyboard");
kbd->supports_key = true;
kbd->supports_rel = false;
kbd->supports_abs = false;

input_register_device(kbd);
```

### Reporting Key Events

```c
// Key press
input_report_key(kbd, KEY_A, KEY_PRESSED);
input_sync(kbd);

// Key release
input_report_key(kbd, KEY_A, KEY_RELEASED);
input_sync(kbd);
```

### Reporting Mouse Movement

```c
input_report_rel(mouse, REL_X, dx);
input_report_rel(mouse, REL_Y, dy);
input_sync(mouse);
```

### Reporting Mouse Buttons

```c
// Left button press
input_report_key(mouse, BTN_LEFT, KEY_PRESSED);
input_sync(mouse);

// Left button release
input_report_key(mouse, BTN_LEFT, KEY_RELEASED);
input_sync(mouse);
```

---

## Device Nodes

| Device | Description |
|--------|-------------|
| `/dev/input/event0` | First input device (usually keyboard) |
| `/dev/input/event1` | Second input device (usually mouse) |
| `/dev/input/eventN` | Additional devices |

---

## Debugging

### Enable Debug Output

In kernel:
```c
#define INPUT_DEBUG 1
```

### Test with Simple Program

```bash
/bin/test_input_simple /dev/input/event0 10
```

Output should show:
```
[1] Event: type=KEY code=A value=1 (pressed)
[2] Event: type=KEY code=A value=0 (released)
```

### Check Kernel Logs

```
[INPUT] Initializing input subsystem
[EVDEV] Initializing evdev subsystem
[PS/2] Registered keyboard as input device
[EVDEV] Registered event0 for device PS/2 Keyboard
```

---

## Common Issues

### "Failed to open /dev/input/event0"
- Check device exists: `ls /dev/input/`
- Check permissions: should be `rw-rw-rw-`
- Verify input subsystem initialized

### "No events received"
- Verify PS/2 driver initialized
- Check IRQ handler registered
- Enable debug output to trace event flow

### "Partial events / garbled data"
- Ensure reading full `sizeof(struct input_event)` (16 bytes)
- Check for buffer alignment issues
- Verify event structure packing

---

## Performance Tips

1. **Batch reads** - Use `input_read_events()` to read multiple events at once
2. **Non-blocking I/O** - Open with `O_NONBLOCK` and check for EAGAIN
3. **Event filtering** - Ignore events you don't care about early
4. **Reduce syscalls** - Read as many events as possible per syscall

---

## See Also

- `INPUT_PIPELINE_IMPLEMENTATION.md` - Full implementation details
- `kernel/include/input.h` - Kernel API
- `userspace/libinput/input.h` - Userspace API
- `userspace/test_input.c` - Example application

---

**Last Updated:** 2026-05-27
