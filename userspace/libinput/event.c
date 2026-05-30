/*
 * Userspace Input Event Library Implementation
 */

#include "input.h"
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>

/*
 * Open input device
 */
int input_open(const char* device) {
    if (!device) {
        return -1;
    }

    int fd = open(device, O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "Failed to open input device: %s\n", device);
        return -1;
    }

    return fd;
}

/*
 * Close input device
 */
int input_close(int fd) {
    if (fd < 0) {
        return -1;
    }

    return close(fd);
}

/*
 * Read single event from device
 */
int input_read_event(int fd, struct input_event* event) {
    if (fd < 0 || !event) {
        return -1;
    }

    ssize_t bytes = read(fd, event, sizeof(struct input_event));
    if (bytes < 0) {
        return -1;
    }

    if (bytes != sizeof(struct input_event)) {
        fprintf(stderr, "Partial read: got %zd bytes, expected %zu\n",
                bytes, sizeof(struct input_event));
        return -1;
    }

    return 0;
}

/*
 * Read multiple events from device
 */
int input_read_events(int fd, struct input_event* events, int count) {
    if (fd < 0 || !events || count <= 0) {
        return -1;
    }

    ssize_t bytes = read(fd, events, sizeof(struct input_event) * count);
    if (bytes < 0) {
        return -1;
    }

    int events_read = bytes / sizeof(struct input_event);
    return events_read;
}

/*
 * Get event type name
 */
const char* input_event_type_name(uint16_t type) {
    switch (type) {
        case EV_SYN: return "SYN";
        case EV_KEY: return "KEY";
        case EV_REL: return "REL";
        case EV_ABS: return "ABS";
        case EV_MSC: return "MSC";
        case EV_SW:  return "SW";
        case EV_LED: return "LED";
        case EV_SND: return "SND";
        case EV_REP: return "REP";
        default:     return "UNK";
    }
}

/*
 * Get key name
 */
const char* input_key_name(uint16_t code) {
    static char buf[32];

    // Common keys
    if (code >= KEY_1 && code <= KEY_0) {
        snprintf(buf, sizeof(buf), "%c", '0' + (code - KEY_1));
        return buf;
    }
    if (code >= KEY_A && code <= KEY_Z) {
        snprintf(buf, sizeof(buf), "%c", 'A' + (code - KEY_A));
        return buf;
    }

    switch (code) {
        case KEY_ESC:        return "ESC";
        case KEY_BACKSPACE:  return "BACKSPACE";
        case KEY_TAB:        return "TAB";
        case KEY_ENTER:      return "ENTER";
        case KEY_LEFTCTRL:   return "LEFTCTRL";
        case KEY_LEFTSHIFT:  return "LEFTSHIFT";
        case KEY_RIGHTSHIFT: return "RIGHTSHIFT";
        case KEY_LEFTALT:    return "LEFTALT";
        case KEY_SPACE:      return "SPACE";
        case KEY_CAPSLOCK:   return "CAPSLOCK";
        case BTN_LEFT:       return "BTN_LEFT";
        case BTN_RIGHT:      return "BTN_RIGHT";
        case BTN_MIDDLE:     return "BTN_MIDDLE";
        default:
            snprintf(buf, sizeof(buf), "KEY_%u", code);
            return buf;
    }
}

/*
 * Print event to stdout
 */
void input_print_event(const struct input_event* event) {
    if (!event) return;

    const char* type_name = input_event_type_name(event->type);

    printf("Event: time=%ld.%06ld type=%s",
           (long)event->time.tv_sec,
           (long)event->time.tv_usec,
           type_name);

    switch (event->type) {
        case EV_KEY:
            printf(" code=%s value=%d (%s)\n",
                   input_key_name(event->code),
                   event->value,
                   event->value == KEY_RELEASED ? "released" :
                   event->value == KEY_PRESSED ? "pressed" : "repeat");
            break;

        case EV_REL:
            if (event->code == REL_X) {
                printf(" code=REL_X value=%d\n", event->value);
            } else if (event->code == REL_Y) {
                printf(" code=REL_Y value=%d\n", event->value);
            } else if (event->code == REL_WHEEL) {
                printf(" code=REL_WHEEL value=%d\n", event->value);
            } else {
                printf(" code=%u value=%d\n", event->code, event->value);
            }
            break;

        case EV_SYN:
            printf(" sync\n");
            break;

        default:
            printf(" code=%u value=%d\n", event->code, event->value);
            break;
    }
}
