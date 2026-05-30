#ifndef DEVICE_H
#define DEVICE_H

#include "types.h"

// Forward declarations
struct device;
struct driver;
struct bus_type;

// Device states
typedef enum {
    DEVICE_STATE_UNINITIALIZED = 0,
    DEVICE_STATE_PROBING,
    DEVICE_STATE_ACTIVE,
    DEVICE_STATE_SUSPENDED,
    DEVICE_STATE_REMOVING,
    DEVICE_STATE_REMOVED,
    DEVICE_STATE_ERROR
} device_state_t;

// Device power states (ACPI D-states)
typedef enum {
    DEVICE_POWER_D0 = 0,  // Fully on
    DEVICE_POWER_D1,      // Light sleep
    DEVICE_POWER_D2,      // Deeper sleep
    DEVICE_POWER_D3_HOT,  // Off, can wake
    DEVICE_POWER_D3_COLD  // Off, no wake
} device_power_state_t;

// Device power management operations
typedef struct {
    int (*suspend)(struct device* dev);
    int (*resume)(struct device* dev);
    int (*runtime_suspend)(struct device* dev);
    int (*runtime_resume)(struct device* dev);
    int (*prepare)(struct device* dev);
    void (*complete)(struct device* dev);
} device_pm_ops_t;

// Device structure - unified representation of all hardware
typedef struct device {
    char name[64];                    // Device name
    char* path;                       // Sysfs-like path
    struct device* parent;            // Parent device
    struct device* children;          // First child
    struct device* sibling;           // Next sibling
    struct bus_type* bus;             // Bus type
    struct driver* driver;            // Bound driver
    void* driver_data;                // Driver private data
    void* platform_data;              // Platform-specific data

    // Device state
    device_state_t state;
    device_power_state_t power_state;
    uint32_t flags;

    // Power management
    const device_pm_ops_t* pm_ops;
    bool can_wakeup;
    bool should_wakeup;
    uint64_t runtime_idle_time;
    uint64_t runtime_suspend_time;

    // Reference counting
    uint32_t refcount;

    // Device ID for matching
    uint32_t device_id;

    // Lock for concurrent access
    void* lock;  // spinlock_t

    // List node for bus device list
    struct device* bus_next;
} device_t;

// Driver structure - represents a device driver
typedef struct driver {
    const char* name;
    struct bus_type* bus;

    // Driver lifecycle callbacks
    int (*probe)(device_t* dev);
    int (*remove)(device_t* dev);
    void (*shutdown)(device_t* dev);

    // Power management callbacks
    const device_pm_ops_t* pm_ops;

    // Device matching
    const void* match_table;  // Bus-specific match table
    int (*match)(device_t* dev, struct driver* drv);

    // Priority for device binding
    uint32_t priority;

    // Module owner (for loadable modules)
    void* owner;

    // List node for bus driver list
    struct driver* bus_next;
} driver_t;

// Bus type - represents a hardware bus
typedef struct bus_type {
    const char* name;

    // Device and driver lists
    device_t* devices;
    driver_t* drivers;

    // Bus operations
    int (*match)(device_t* dev, driver_t* drv);
    int (*probe)(device_t* dev);
    int (*remove)(device_t* dev);
    void (*shutdown)(device_t* dev);

    // Power management
    int (*suspend)(device_t* dev);
    int (*resume)(device_t* dev);

    // Lock for bus lists
    void* lock;  // spinlock_t
} bus_type_t;

// Device registration flags
#define DEVICE_FLAG_REGISTERED    0x0001
#define DEVICE_FLAG_PROBED        0x0002
#define DEVICE_FLAG_SUSPENDED     0x0004
#define DEVICE_FLAG_RUNTIME_PM    0x0008
#define DEVICE_FLAG_WAKEUP_ARMED  0x0010
#define DEVICE_FLAG_HOTPLUG       0x0020

// Device core API
int device_init(void);
device_t* device_alloc(const char* name);
void device_free(device_t* dev);
int device_register(device_t* dev);
void device_unregister(device_t* dev);
void device_del(device_t* dev);
int device_add(device_t* dev);

// Device tree operations
void device_set_parent(device_t* dev, device_t* parent);
device_t* device_get_parent(device_t* dev);
void device_add_child(device_t* parent, device_t* child);
void device_remove_child(device_t* parent, device_t* child);

// Device reference counting
void device_get(device_t* dev);
void device_put(device_t* dev);

// Device data access
void device_set_driver_data(device_t* dev, void* data);
void* device_get_driver_data(device_t* dev);

// Device naming and path
int device_set_name(device_t* dev, const char* fmt, ...);
const char* device_get_name(device_t* dev);

// Driver API
int driver_register(driver_t* drv);
void driver_unregister(driver_t* drv);
int driver_attach(device_t* dev);
void driver_detach(device_t* dev);
int driver_bind(device_t* dev, driver_t* drv);
void driver_unbind(device_t* dev);

// Bus API
int bus_register(bus_type_t* bus);
void bus_unregister(bus_type_t* bus);
int bus_add_device(bus_type_t* bus, device_t* dev);
void bus_remove_device(bus_type_t* bus, device_t* dev);
int bus_add_driver(bus_type_t* bus, driver_t* drv);
void bus_remove_driver(bus_type_t* bus, driver_t* drv);
device_t* bus_find_device(bus_type_t* bus, const char* name);
driver_t* bus_find_driver(bus_type_t* bus, const char* name);

// Device matching and probing
int device_match_driver(device_t* dev, driver_t* drv);
int device_probe(device_t* dev);
void device_reprobe(device_t* dev);

// Hot-plug support
typedef enum {
    DEVICE_EVENT_ADD = 1,
    DEVICE_EVENT_REMOVE,
    DEVICE_EVENT_CHANGE,
    DEVICE_EVENT_ONLINE,
    DEVICE_EVENT_OFFLINE
} device_event_t;

typedef void (*device_event_callback_t)(device_t* dev, device_event_t event);

int device_register_event_callback(device_event_callback_t callback);
void device_unregister_event_callback(device_event_callback_t callback);
void device_notify_event(device_t* dev, device_event_t event);

// Power management API
int device_pm_suspend(device_t* dev);
int device_pm_resume(device_t* dev);
int device_pm_runtime_suspend(device_t* dev);
int device_pm_runtime_resume(device_t* dev);
void device_pm_enable_wakeup(device_t* dev);
void device_pm_disable_wakeup(device_t* dev);
bool device_pm_can_wakeup(device_t* dev);

// System power management
int device_pm_suspend_all(void);
int device_pm_resume_all(void);
int device_pm_prepare_all(void);
void device_pm_complete_all(void);

// Runtime power management
void device_pm_runtime_enable(device_t* dev);
void device_pm_runtime_disable(device_t* dev);
int device_pm_runtime_get(device_t* dev);
void device_pm_runtime_put(device_t* dev);
void device_pm_runtime_put_autosuspend(device_t* dev);
void device_pm_runtime_set_autosuspend_delay(device_t* dev, uint32_t delay_ms);

// Common buses (to be implemented)
extern bus_type_t pci_bus_type;
extern bus_type_t usb_bus_type;
extern bus_type_t platform_bus_type;

#endif
