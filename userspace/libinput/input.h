/*
 * Userspace Input Event Library
 * Compatible with Linux evdev protocol
 */

#ifndef LIBINPUT_H
#define LIBINPUT_H

#include <stdint.h>
#include <stdbool.h>
#include <sys/time.h>

// Event types
#define EV_SYN          0x00
#define EV_KEY          0x01
#define EV_REL          0x02
#define EV_ABS          0x03
#define EV_MSC          0x04
#define EV_SW           0x05
#define EV_LED          0x11
#define EV_SND          0x12
#define EV_REP          0x14
#define EV_MAX          0x1f

// Synchronization events
#define SYN_REPORT      0
#define SYN_CONFIG      1

// Key states
#define KEY_RELEASED    0
#define KEY_PRESSED     1
#define KEY_REPEAT      2

// Key codes (subset)
#define KEY_RESERVED    0
#define KEY_ESC         1
#define KEY_1           2
#define KEY_2           3
#define KEY_3           4
#define KEY_4           5
#define KEY_5           6
#define KEY_6           7
#define KEY_7           8
#define KEY_8           9
#define KEY_9           10
#define KEY_0           11
#define KEY_MINUS       12
#define KEY_EQUAL       13
#define KEY_BACKSPACE   14
#define KEY_TAB         15
#define KEY_Q           16
#define KEY_W           17
#define KEY_E           18
#define KEY_R           19
#define KEY_T           20
#define KEY_Y           21
#define KEY_U           22
#define KEY_I           23
#define KEY_O           24
#define KEY_P           25
#define KEY_LEFTBRACE   26
#define KEY_RIGHTBRACE  27
#define KEY_ENTER       28
#define KEY_LEFTCTRL    29
#define KEY_A           30
#define KEY_S           31
#define KEY_D           32
#define KEY_F           33
#define KEY_G           34
#define KEY_H           35
#define KEY_J           36
#define KEY_K           37
#define KEY_L           38
#define KEY_SEMICOLON   39
#define KEY_APOSTROPHE  40
#define KEY_GRAVE       41
#define KEY_LEFTSHIFT   42
#define KEY_BACKSLASH   43
#define KEY_Z           44
#define KEY_X           45
#define KEY_C           46
#define KEY_V           47
#define KEY_B           48
#define KEY_N           49
#define KEY_M           50
#define KEY_COMMA       51
#define KEY_DOT         52
#define KEY_SLASH       53
#define KEY_RIGHTSHIFT  54
#define KEY_KPASTERISK  55
#define KEY_LEFTALT     56
#define KEY_SPACE       57
#define KEY_CAPSLOCK    58
#define KEY_F1          59
#define KEY_F2          60
#define KEY_F3          61
#define KEY_F4          62
#define KEY_F5          63
#define KEY_F6          64
#define KEY_F7          65
#define KEY_F8          66
#define KEY_F9          67
#define KEY_F10         68

// Relative axes
#define REL_X           0x00
#define REL_Y           0x01
#define REL_Z           0x02
#define REL_WHEEL       0x08
#define REL_HWHEEL      0x09

// Mouse buttons
#define BTN_MOUSE       0x110
#define BTN_LEFT        0x110
#define BTN_RIGHT       0x111
#define BTN_MIDDLE      0x112
#define BTN_SIDE        0x113
#define BTN_EXTRA       0x114

// Input event structure
struct input_event {
    struct timeval time;
    uint16_t type;
    uint16_t code;
    int32_t value;
} __attribute__((packed));

// Simplified version without timeval dependency
typedef struct {
    uint64_t timestamp;  // Microseconds since boot
    uint16_t type;
    uint16_t code;
    int32_t value;
} input_event_simple_t;

// Library functions
int input_open(const char* device);
int input_close(int fd);
int input_read_event(int fd, struct input_event* event);
int input_read_events(int fd, struct input_event* events, int count);
const char* input_event_type_name(uint16_t type);
const char* input_key_name(uint16_t code);
void input_print_event(const struct input_event* event);

#endif
