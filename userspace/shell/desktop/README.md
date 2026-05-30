# AutomationOS Desktop Shell

Beautiful, functional desktop environment built from scratch for AutomationOS.

---

## Quick Start

### Build

```bash
make
```

### Run

```bash
make run
```

### Clean

```bash
make clean
```

---

## Components

| Component | File | Description |
|-----------|------|-------------|
| **Panel** | `panel.c` | Top bar with Activities, system tray, clock |
| **Dock** | `dock.c` | Application launcher with magnification |
| **Desktop** | `desktop.c` | Wallpaper and desktop icons |
| **Overview** | `overview.c` | Mission Control / Activities view |
| **Notifications** | `notifications.c` | Notification center |
| **Quick Settings** | `quick_settings.c` | WiFi, volume, brightness controls |
| **System Menu** | `system_menu.c` | Power menu (logout, restart, shutdown) |

---

## Architecture

```
desktop_shell_t
├── desktop_t       → Background + icons
├── panel_t         → Top bar
├── dock_t          → App launcher
├── overview_t      → Activities / search
├── notifications_t → Notification center
├── quick_settings_t → Quick settings
└── system_menu_t   → System menu
```

---

## Features

- ✅ macOS-inspired design
- ✅ Light and dark themes
- ✅ Smooth 60 FPS rendering
- ✅ Notification system
- ✅ Application launcher
- ✅ Search functionality
- ✅ Workspace management
- ✅ System settings integration

---

## Documentation

📖 **[Complete Guide](../../../docs/DESKTOP_SHELL_GUIDE.md)** - User guide with all features  
🎨 **[Theme API](../../../docs/THEME_API.md)** - Customization and theming  
🔌 **[App Integration](../../../docs/APP_INTEGRATION.md)** - How apps integrate with shell

---

## Code Statistics

- **Lines of Code:** ~3,000 LOC
- **Files:** 9 implementation files + 1 header
- **Components:** 7 major components
- **Documentation:** 3,000+ lines across 3 guides

---

## Performance

- **Target FPS:** 60+ (VSync-locked)
- **Idle CPU:** < 0.5%
- **Memory:** < 50 MB
- **Startup:** < 1 second

---

## Example Usage

### Create Desktop Shell

```c
#include "desktop_shell.h"

int main(void) {
    // Create shell (1920x1080)
    desktop_shell_t *shell = desktop_shell_create(1920, 1080);

    // Apply dark theme
    theme_apply(shell, THEME_DARK);

    // Run main loop
    desktop_shell_run(shell);

    // Cleanup
    desktop_shell_destroy(shell);
    return 0;
}
```

### Send Notification

```c
notification_send(shell->notifications,
    "Email Client",
    "New Message",
    "You have 3 new messages.",
    NOTIF_INFO);
```

### Add App to Dock

```c
dock_add_app(shell->dock,
    "com.example.myapp",
    "My App",
    true);  // pinned
```

---

## Directory Structure

```
desktop/
├── desktop_shell.h         # Main header
├── desktop_shell.c         # Core lifecycle
├── panel.c                 # Top bar
├── dock.c                  # App launcher
├── desktop.c               # Desktop & icons
├── overview.c              # Activities view
├── notifications.c         # Notification center
├── quick_settings.c        # Quick settings
├── system_menu.c           # System menu
├── Makefile               # Build config
└── README.md              # This file
```

---

## Contributing

### Code Style

- 4 spaces for indentation
- K&R brace style
- 80 character line limit
- Document public APIs

### Adding Features

1. Add component to `desktop_shell.h`
2. Implement in new `.c` file
3. Update Makefile
4. Add to `desktop_shell_create()`
5. Document in guides

---

## License

Part of AutomationOS. See project LICENSE.

---

**Built with ❤️ for AutomationOS**
