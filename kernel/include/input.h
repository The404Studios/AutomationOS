#ifndef INPUT_H
#define INPUT_H

#include "types.h"

// Input Event Types
typedef enum {
    INPUT_EVENT_KEY = 0,
    INPUT_EVENT_REL = 1,  // Relative movement (mouse)
    INPUT_EVENT_ABS = 2,  // Absolute position (touchscreen)
    INPUT_EVENT_MSC = 3,  // Miscellaneous
    INPUT_EVENT_SW = 4,   // Switches
    INPUT_EVENT_LED = 5,  // LEDs
    INPUT_EVENT_SND = 6,  // Sound
    INPUT_EVENT_REP = 7   // Key repeat
} input_event_type_t;

// Key Event Codes (subset of Linux input codes)
#define KEY_RESERVED        0
#define KEY_ESC             1
#define KEY_1               2
#define KEY_2               3
#define KEY_3               4
#define KEY_4               5
#define KEY_5               6
#define KEY_6               7
#define KEY_7               8
#define KEY_8               9
#define KEY_9               10
#define KEY_0               11
#define KEY_MINUS           12
#define KEY_EQUAL           13
#define KEY_BACKSPACE       14
#define KEY_TAB             15
#define KEY_Q               16
#define KEY_W               17
#define KEY_E               18
#define KEY_R               19
#define KEY_T               20
#define KEY_Y               21
#define KEY_U               22
#define KEY_I               23
#define KEY_O               24
#define KEY_P               25
#define KEY_LEFTBRACE       26
#define KEY_RIGHTBRACE      27
#define KEY_ENTER           28
#define KEY_LEFTCTRL        29
#define KEY_A               30
#define KEY_S               31
#define KEY_D               32
#define KEY_F               33
#define KEY_G               34
#define KEY_H               35
#define KEY_J               36
#define KEY_K               37
#define KEY_L               38
#define KEY_SEMICOLON       39
#define KEY_APOSTROPHE      40
#define KEY_GRAVE           41
#define KEY_LEFTSHIFT       42
#define KEY_BACKSLASH       43
#define KEY_Z               44
#define KEY_X               45
#define KEY_C               46
#define KEY_V               47
#define KEY_B               48
#define KEY_N               49
#define KEY_M               50
#define KEY_COMMA           51
#define KEY_DOT             52
#define KEY_SLASH           53
#define KEY_RIGHTSHIFT      54
#define KEY_KPASTERISK      55
#define KEY_LEFTALT         56
#define KEY_SPACE           57
#define KEY_CAPSLOCK        58
#define KEY_F1              59
#define KEY_F2              60
#define KEY_F3              61
#define KEY_F4              62
#define KEY_F5              63
#define KEY_F6              64
#define KEY_F7              65
#define KEY_F8              66
#define KEY_F9              67
#define KEY_F10             68
#define KEY_NUMLOCK         69
#define KEY_SCROLLLOCK      70

// Relative Axes (for mouse)
#define REL_X               0
#define REL_Y               1
#define REL_Z               2  // Scroll wheel
#define REL_WHEEL           8
#define REL_HWHEEL          9  // Horizontal wheel

// Absolute Axes (for gamepad)
#define ABS_X               0
#define ABS_Y               1
#define ABS_Z               2
#define ABS_RX              3
#define ABS_RY              4
#define ABS_RZ              5
#define ABS_THROTTLE        6
#define ABS_RUDDER          7
#define ABS_WHEEL           8
#define ABS_GAS             9
#define ABS_BRAKE           10
#define ABS_HAT0X           16
#define ABS_HAT0Y           17

// Button Codes
#define BTN_MISC            0x100
#define BTN_0               0x100
#define BTN_1               0x101
#define BTN_2               0x102
#define BTN_3               0x103
#define BTN_4               0x104
#define BTN_5               0x105
#define BTN_MOUSE           0x110
#define BTN_LEFT            0x110
#define BTN_RIGHT           0x111
#define BTN_MIDDLE          0x112
#define BTN_SIDE            0x113
#define BTN_EXTRA           0x114
#define BTN_GAMEPAD         0x130
#define BTN_A               0x130
#define BTN_B               0x131
#define BTN_C               0x132
#define BTN_X               0x133
#define BTN_Y               0x134
#define BTN_Z               0x135
#define BTN_TL              0x136  // Top-left trigger
#define BTN_TR              0x137  // Top-right trigger
#define BTN_SELECT          0x13A
#define BTN_START           0x13B
#define BTN_MODE            0x13C

// LED Codes
#define LED_NUML            0
#define LED_CAPSL           1
#define LED_SCROLLL         2

// Key States
#define KEY_STATE_RELEASED  0
#define KEY_STATE_PRESSED   1
#define KEY_STATE_REPEAT    2

// Input Event structure
//
// This is the AUTHORITATIVE kernel input event layout (16 bytes):
//   offset 0:  uint64_t timestamp
//   offset 8:  uint16_t type
//   offset 10: uint16_t code
//   offset 12: int32_t  value
// Userspace MUST match this exact layout when reading /dev/input/eventN.
typedef struct {
    uint64_t timestamp;     // Microseconds since boot
    uint16_t type;          // Event type
    uint16_t code;          // Event code
    int32_t value;          // Event value
} input_event_t;

/*
 * Generic character-device node descriptor.
 *
 * The VFS in this kernel (kernel/fs/vfs.c) does not yet support per-inode
 * file operations: vfs_open() unconditionally installs ramfs_file_ops.  To
 * make a VFS_TYPE_DEVICE inode dispatch to a custom driver (evdev here), the
 * inode's ->private_data points at one of these descriptors.  The first
 * member is a magic value so vfs_open() can recognise a device node, followed
 * by the driver's file ops and an opaque driver pointer.
 *
 * INTEGRATION NOTE: a small dispatch must be added to vfs_open() in vfs.c:
 *   if ((inode->type & VFS_TYPE_DEVICE) && inode->private_data &&
 *       *(uint32_t*)inode->private_data == VFS_DEVNODE_MAGIC) {
 *       file->ops = ((vfs_devnode_t*)inode->private_data)->fops;
 *   }
 * placed just before the existing `file->ops = &ramfs_file_ops;` assignment.
 * Without this, opening the node succeeds but read()/open() route to ramfs.
 */
#define VFS_DEVNODE_MAGIC 0x44455644u  /* "DEVD" */

struct vfs_file_operations;  /* from vfs.h */
struct vfs_inode;            /* from vfs.h */

typedef struct vfs_devnode {
    uint32_t magic;                       /* == VFS_DEVNODE_MAGIC */
    struct vfs_file_operations* fops;     /* file ops for this device */
    void* driver_data;                    /* driver-private (e.g. evdev_device_t*) */
} vfs_devnode_t;

// Input Device structure
typedef struct input_device {
    char name[64];
    uint32_t id;

    // Device capabilities
    bool supports_key;
    bool supports_rel;
    bool supports_abs;
    bool supports_led;

    // Event queue
    input_event_t* event_queue;
    uint32_t queue_size;
    uint32_t queue_head;
    uint32_t queue_tail;

    // Driver data
    void* driver_data;

    // Callbacks
    int (*set_led)(struct input_device* dev, uint16_t led, bool on);
    int (*set_repeat)(struct input_device* dev, uint32_t delay, uint32_t period);
} input_device_t;

// Input System Functions
void input_init(void);
input_device_t* input_allocate_device(const char* name);
void input_free_device(input_device_t* dev);
int input_register_device(input_device_t* dev);
void input_unregister_device(input_device_t* dev);
void input_report_key(input_device_t* dev, uint16_t keycode, int32_t value);
void input_report_rel(input_device_t* dev, uint16_t axis, int32_t value);
void input_report_abs(input_device_t* dev, uint16_t axis, int32_t value);
void input_sync(input_device_t* dev);
int input_get_event(input_event_t* event);

// Event device (evdev) interface
void evdev_init(void);
void evdev_handle_event(input_device_t* dev, input_event_t* event);
int evdev_register_device(input_device_t* input_dev);
void evdev_unregister_device(input_device_t* input_dev);

// /dev/input device nodes
void dev_input_init(void);
int dev_input_register_device(input_device_t* input_dev);
int dev_input_open(const char* path);
void dev_input_list(void);

// Insert an already-built device inode as a child of the /dev/input ramfs
// directory so that vfs_path_lookup("/dev/input/<name>") resolves it.
// Returns 0 on success, <0 on error. Requires /dev/input to already exist
// (created via dev_input_create_dir() AFTER the rootfs is mounted and /dev
// has been created).
int dev_input_link_node(const char* name, struct vfs_inode* inode);

// PS/2 driver entry points (defined in kernel/drivers/ps2.c).
// ps2_init() also brings up the mouse and registers IRQ1/IRQ12.
void ps2_init(void);
void ps2_keyboard_irq_handler(uint32_t irq, void* dev_id);
void ps2_mouse_irq_handler(uint32_t irq, void* dev_id);

#endif
