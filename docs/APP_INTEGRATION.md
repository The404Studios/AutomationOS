# App Integration with Desktop Shell

**Version:** 1.0  
**Date:** 2026-05-26

---

## Overview

This guide shows how applications integrate with the AutomationOS Desktop Shell. Apps can send notifications, register in the dock, update progress, and interact with system services.

---

## Communication Architecture

### IPC (Inter-Process Communication)

Apps communicate with the shell via IPC messages:

```
┌─────────────┐         IPC          ┌──────────────┐
│             │  ─────────────────>  │              │
│   Your App  │                       │ Desktop Shell│
│             │  <─────────────────  │              │
└─────────────┘                       └──────────────┘
```

### Message Types

```c
typedef enum {
    MSG_NOTIFY,          // Send notification
    MSG_DOCK_ADD,        // Add app to dock
    MSG_DOCK_REMOVE,     // Remove app from dock
    MSG_PROGRESS,        // Update progress
    MSG_WINDOW_CREATE,   // Create window
    MSG_WINDOW_DESTROY,  // Destroy window
    MSG_WINDOW_FOCUS,    // Focus window
} shell_message_type_t;
```

---

## Sending Notifications

### Basic Notification

```c
#include <shell_ipc.h>

void send_notification(const char *title, const char *message) {
    notification_data_t notif = {
        .app_name = "My App",
        .summary = title,
        .body = message,
        .urgency = NOTIF_INFO,
        .timeout_ms = 5000  // Auto-dismiss after 5 seconds
    };

    ipc_send(SHELL_SERVICE, MSG_NOTIFY, &notif);
}

// Example usage
send_notification("Download Complete", "MyFile.pdf has finished downloading.");
```

### Notification with Actions

```c
void send_notification_with_actions(void) {
    notification_data_t notif = {
        .app_name = "Email Client",
        .summary = "New Email",
        .body = "You have 3 new messages from John Doe.",
        .urgency = NOTIF_INFO,
        .timeout_ms = 0,  // Persist until dismissed
        .action_count = 2
    };

    // Add actions
    strcpy(notif.actions[0].label, "View");
    notif.actions[0].callback_id = 1;

    strcpy(notif.actions[1].label, "Dismiss");
    notif.actions[1].callback_id = 2;

    ipc_send(SHELL_SERVICE, MSG_NOTIFY, &notif);
}
```

### Urgency Levels

```c
// Info notification (default)
notif.urgency = NOTIF_INFO;

// Warning notification (orange)
notif.urgency = NOTIF_WARNING;

// Error notification (red, persistent)
notif.urgency = NOTIF_ERROR;

// Success notification (green)
notif.urgency = NOTIF_SUCCESS;
```

---

## Dock Integration

### Registering in Dock

```c
#include <shell_ipc.h>

void register_app_in_dock(void) {
    dock_app_data_t app = {
        .app_id = "com.example.myapp",
        .name = "My App",
        .icon_path = "/usr/share/icons/myapp.png",
        .exec_path = "/usr/bin/myapp",
        .pinned = true,  // Pin to dock by default
        .running = true  // Mark as running
    };

    ipc_send(SHELL_SERVICE, MSG_DOCK_ADD, &app);
}
```

### Updating Running State

```c
void mark_app_running(bool running) {
    dock_update_data_t update = {
        .app_id = "com.example.myapp",
        .running = running,
        .window_count = running ? 1 : 0
    };

    ipc_send(SHELL_SERVICE, MSG_DOCK_UPDATE, &update);
}
```

### Updating Progress

For download managers, media players, etc:

```c
void update_progress(float progress) {
    dock_progress_data_t data = {
        .app_id = "com.example.myapp",
        .progress = progress  // 0.0 - 1.0
    };

    ipc_send(SHELL_SERVICE, MSG_PROGRESS, &data);
}

// Example usage
void download_file(const char *url) {
    for (int i = 0; i <= 100; i++) {
        // ... download ...
        update_progress((float)i / 100.0f);
        sleep(1);
    }
}
```

### Notification Badge

Show notification count in dock:

```c
void update_notification_count(uint32_t count) {
    dock_badge_data_t badge = {
        .app_id = "com.example.myapp",
        .notification_count = count
    };

    ipc_send(SHELL_SERVICE, MSG_DOCK_BADGE, &badge);
}
```

---

## Window Management

### Creating Windows

```c
#include <shell_window.h>

window_handle_t create_app_window(void) {
    window_create_data_t win = {
        .title = "My App",
        .app_id = "com.example.myapp",
        .width = 800,
        .height = 600,
        .flags = WINDOW_RESIZABLE | WINDOW_DECORATED
    };

    window_handle_t handle;
    ipc_send(SHELL_SERVICE, MSG_WINDOW_CREATE, &win, &handle);

    return handle;
}
```

### Updating Window Title

```c
void set_window_title(window_handle_t win, const char *title) {
    window_update_data_t update = {
        .handle = win,
        .title = title
    };

    ipc_send(SHELL_SERVICE, MSG_WINDOW_UPDATE, &update);
}

// Example: Update title with document name
set_window_title(win, "Document.txt - My Editor");
```

### Focusing Windows

```c
void focus_window(window_handle_t win) {
    ipc_send(SHELL_SERVICE, MSG_WINDOW_FOCUS, &win);
}
```

### Destroying Windows

```c
void close_window(window_handle_t win) {
    ipc_send(SHELL_SERVICE, MSG_WINDOW_DESTROY, &win);
}
```

---

## Application Manifest

### App Metadata (JSON)

Create `manifest.json` in your app directory:

```json
{
  "id": "com.example.myapp",
  "name": "My App",
  "version": "1.0.0",
  "description": "A sample application",
  "icon": "/usr/share/icons/myapp.png",
  "exec": "/usr/bin/myapp",
  "categories": ["Utility", "Development"],
  "capabilities": [
    "cap:file_read:/home/user/Documents",
    "cap:file_write:/home/user/Documents",
    "cap:net_connect:*:*",
    "cap:notify"
  ],
  "dock": {
    "pinned": false,
    "show_progress": true,
    "show_badge": true
  },
  "shortcuts": [
    {
      "name": "New Window",
      "command": "myapp --new-window"
    }
  ]
}
```

### Required Fields

- **id**: Unique app identifier (reverse DNS)
- **name**: Display name
- **version**: Semantic version (1.0.0)
- **exec**: Path to executable
- **icon**: Path to icon (PNG, 256x256)

### Optional Fields

- **description**: Short description
- **categories**: App categories for overview
- **capabilities**: Required permissions
- **dock**: Dock-specific settings
- **shortcuts**: Context menu shortcuts

---

## Example Applications

### 1. Simple Notification Sender

```c
#include <shell_ipc.h>
#include <stdio.h>
#include <unistd.h>

int main(int argc, char *argv[]) {
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <title> <message>\n", argv[0]);
        return 1;
    }

    notification_data_t notif = {
        .app_name = "notify-send",
        .summary = argv[1],
        .body = argv[2],
        .urgency = NOTIF_INFO,
        .timeout_ms = 5000
    };

    ipc_send(SHELL_SERVICE, MSG_NOTIFY, &notif);

    return 0;
}
```

Usage:
```bash
$ notify-send "Hello" "This is a notification"
```

### 2. Download Manager

```c
#include <shell_ipc.h>
#include <stdio.h>
#include <unistd.h>

void download_file(const char *url, const char *dest) {
    // Register in dock
    dock_app_data_t app = {
        .app_id = "com.example.downloader",
        .name = "Download Manager",
        .running = true
    };
    ipc_send(SHELL_SERVICE, MSG_DOCK_ADD, &app);

    // Send notification
    notification_data_t notif = {
        .app_name = "Download Manager",
        .summary = "Download Started",
        .body = url,
        .urgency = NOTIF_INFO,
        .timeout_ms = 3000
    };
    ipc_send(SHELL_SERVICE, MSG_NOTIFY, &notif);

    // Simulate download with progress
    for (int i = 0; i <= 100; i += 5) {
        printf("Downloading... %d%%\n", i);

        // Update dock progress
        dock_progress_data_t progress = {
            .app_id = "com.example.downloader",
            .progress = (float)i / 100.0f
        };
        ipc_send(SHELL_SERVICE, MSG_PROGRESS, &progress);

        usleep(200000);  // 200ms
    }

    // Completion notification
    notif.summary = "Download Complete";
    notif.body = "File saved to Downloads";
    notif.urgency = NOTIF_SUCCESS;
    ipc_send(SHELL_SERVICE, MSG_NOTIFY, &notif);

    // Clear progress
    dock_progress_data_t progress = {
        .app_id = "com.example.downloader",
        .progress = -1.0f  // -1 = no progress
    };
    ipc_send(SHELL_SERVICE, MSG_PROGRESS, &progress);
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <url>\n", argv[0]);
        return 1;
    }

    download_file(argv[1], "~/Downloads");

    return 0;
}
```

### 3. Background Service

```c
#include <shell_ipc.h>
#include <stdio.h>
#include <unistd.h>
#include <stdbool.h>

void check_disk_space(void) {
    // TODO: Get actual disk space
    uint64_t free_space = 1024 * 1024 * 500;  // 500 MB

    if (free_space < 1024 * 1024 * 1000) {  // < 1 GB
        notification_data_t notif = {
            .app_name = "System Monitor",
            .summary = "Low Disk Space",
            .body = "You are running out of disk space. Please free up some space.",
            .urgency = NOTIF_WARNING,
            .timeout_ms = 0  // Persist
        };
        ipc_send(SHELL_SERVICE, MSG_NOTIFY, &notif);
    }
}

int main(void) {
    // Register in system tray
    tray_icon_data_t tray = {
        .app_id = "com.example.monitor",
        .icon_path = "/usr/share/icons/disk.png",
        .tooltip = "Disk Monitor"
    };
    ipc_send(SHELL_SERVICE, MSG_TRAY_ADD, &tray);

    // Run monitoring loop
    while (true) {
        check_disk_space();
        sleep(60);  // Check every minute
    }

    return 0;
}
```

---

## Best Practices

### Notifications

1. **Use appropriate urgency**
   - INFO: General information
   - WARNING: User should be aware
   - ERROR: Action required
   - SUCCESS: Positive feedback

2. **Set reasonable timeouts**
   - INFO: 3-5 seconds
   - WARNING: 0 (persist)
   - ERROR: 0 (persist)
   - SUCCESS: 2-3 seconds

3. **Limit notification frequency**
   - Don't spam the user
   - Group similar notifications
   - Use progress instead of multiple notifications

### Dock Integration

1. **Provide high-quality icon**
   - PNG format, 256x256 pixels
   - Transparent background
   - Consistent style

2. **Update running state accurately**
   - Mark as running when windows open
   - Clear when all windows close

3. **Use progress indicators appropriately**
   - Long-running operations only
   - Update at reasonable intervals (not every 1%)
   - Clear progress when complete

### Window Management

1. **Set descriptive window titles**
   - Include document name
   - Update on document change

2. **Request focus sparingly**
   - Only when user initiated
   - Avoid stealing focus

3. **Clean up on exit**
   - Destroy all windows
   - Remove from dock if not pinned

---

## Debugging

### Testing IPC

```bash
# Monitor IPC messages
$ ipc-monitor SHELL_SERVICE

# Send test notification
$ ipc-send SHELL_SERVICE MSG_NOTIFY '{"app_name":"Test","summary":"Hello"}'

# Check dock state
$ ipc-query SHELL_SERVICE dock_list
```

### Common Issues

**Notification not appearing:**
- Check Do Not Disturb is disabled
- Verify app_name is not empty
- Check urgency level

**App not showing in dock:**
- Verify app_id is unique
- Check icon_path exists
- Ensure manifest.json is valid

**Progress not updating:**
- Check progress value is 0.0-1.0
- Verify app_id matches registered app
- Ensure updates are not too frequent

---

## API Reference

### IPC Functions

```c
// Send message to shell
int ipc_send(service_id_t service, message_type_t type, void *data);

// Send message and wait for reply
int ipc_send_sync(service_id_t service, message_type_t type, void *data, void *reply);

// Register IPC handler
int ipc_register_handler(message_type_t type, ipc_handler_fn handler);
```

### Data Structures

```c
// Notification data
typedef struct {
    char app_name[64];
    char summary[128];
    char body[512];
    notif_urgency_t urgency;
    uint32_t timeout_ms;
    uint32_t action_count;
    struct {
        char label[64];
        uint32_t callback_id;
    } actions[4];
} notification_data_t;

// Dock app data
typedef struct {
    char app_id[64];
    char name[128];
    char icon_path[512];
    char exec_path[512];
    bool pinned;
    bool running;
} dock_app_data_t;

// Window create data
typedef struct {
    char title[256];
    char app_id[64];
    uint32_t width;
    uint32_t height;
    uint32_t flags;
} window_create_data_t;
```

---

## Examples Repository

Complete examples available at:
```
examples/shell_integration/
├── notify_example.c          # Simple notifications
├── dock_example.c            # Dock integration
├── window_example.c          # Window management
├── progress_example.c        # Progress updates
└── service_example.c         # Background service
```

---

**Integrate seamlessly with AutomationOS Desktop Shell!**
