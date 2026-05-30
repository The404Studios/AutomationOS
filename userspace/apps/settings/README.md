# AutomationOS Settings Application

Complete system control panel for managing all operating system settings.

## Overview

The Settings application provides a comprehensive interface for configuring all aspects of AutomationOS, from display settings to privacy controls. Built with a two-panel layout (sidebar + content), it offers an intuitive macOS-style user experience.

## Features

### 8 Settings Categories

#### 1. Display
- **Resolution & Refresh Rate**
  - Resolution: 1920×1080, 2560×1440, 3840×2160, etc.
  - Refresh rate: 60Hz, 75Hz, 120Hz, 144Hz, 240Hz
  - Display scaling: 100%, 125%, 150%, 175%, 200%
  - Screen orientation: 0°, 90°, 180°, 270°

- **Multi-Monitor**
  - Enable/disable multi-monitor support
  - Mirror displays
  - Set primary monitor
  - Arrange displays

- **Night Light**
  - Enable/disable night light
  - Color temperature adjustment
  - Schedule (sunset to sunrise or custom)

- **Advanced**
  - VSync enable/disable
  - Hardware acceleration

#### 2. Appearance
- **Theme**
  - Light, Dark, or Auto mode
  - Accent color selection (Blue, Purple, Green, Orange, Red, Pink, Custom)
  - Custom accent color picker

- **Wallpaper**
  - Choose wallpaper image
  - Fit or fill options
  - Wallpaper preview

- **Icon Theme**
  - Select icon theme
  - Icon size: 32px, 48px, 64px

- **Fonts**
  - System font selection
  - Monospace font selection
  - Font size presets (small, normal, large)

- **Effects**
  - Enable/disable animations
  - Transparency effects
  - Animation speed (slow, normal, fast)
  - Blur radius adjustment

#### 3. Sound
- **Output Devices**
  - Select default output device
  - Device-specific settings (sample rate, channels)
  - Master volume control

- **Input Devices**
  - Select default input device
  - Input volume control
  - Microphone sensitivity

- **Volume Levels**
  - Master volume slider
  - Output volume slider
  - Input volume slider
  - Mute toggle

- **Sound Effects**
  - Enable/disable system sounds
  - Notification sounds
  - Startup sound
  - Effects volume

#### 4. Network
- **Wi-Fi**
  - Enable/disable Wi-Fi
  - Available networks list
  - Connect to network
  - Forget network
  - Network security (WEP, WPA, WPA2, WPA3)

- **Ethernet**
  - Enable/disable Ethernet
  - IP address display
  - MAC address display
  - DHCP or manual IP configuration

- **VPN**
  - Add VPN connection
  - Configure VPN settings
  - Connect/disconnect VPN

- **Proxy**
  - Enable/disable proxy
  - Proxy host and port
  - Proxy authentication

- **Advanced**
  - DNS server configuration
  - IPv6 enable/disable

#### 5. Users & Accounts
- **Current User**
  - View user information
  - Change password
  - Change avatar
  - Set auto-login

- **Other Users**
  - Add new user
  - Modify existing users
  - Set user roles (Admin, Standard, Guest)
  - Enable/disable users

- **Login Options**
  - Require password on login
  - Auto-logout timeout
  - Show users on login screen
  - Fast user switching

#### 6. Applications
- **Default Applications**
  - Web browser
  - Email client
  - Text editor
  - Terminal
  - File manager

- **Startup Applications**
  - Add/remove startup applications
  - Enable/disable autostart for apps

- **All Applications**
  - List of installed applications
  - App information and settings

- **App Permissions**
  - Location access
  - Camera access
  - Microphone access
  - File system access

#### 7. Privacy & Security
- **Application Capabilities**
  - Enable/disable sandboxing
  - Strict mode
  - Audit sandbox violations
  - Per-app capability permissions

- **Firewall**
  - Enable/disable firewall
  - Block incoming connections
  - Stealth mode
  - Firewall rules

- **File Encryption**
  - Encrypt home folder
  - Encrypt swap partition
  - Secure file deletion

- **Privacy**
  - Location services
  - Diagnostics and usage reporting
  - Usage analytics

#### 8. System
- **About This System**
  - OS name and version
  - Kernel version
  - Build date
  - CPU model and cores
  - Memory (used/total)
  - GPU model
  - Disk space (used/total)
  - System uptime

- **Software Update**
  - Automatically check for updates
  - Automatically download updates
  - Automatically install updates
  - Update channel (stable, beta, dev)
  - Check for updates now

- **Recovery**
  - Recovery partition status
  - Last backup timestamp
  - Backup location
  - Create backup

- **Developer Options**
  - Enable developer mode
  - Show debug information
  - Enable core dumps
  - Kernel logging verbosity

## Architecture

### Files

```
userspace/apps/settings/
├── settings.h              # Main header with all data structures
├── settings.c              # Core application logic and lifecycle
├── panels.c                # Panel creation for each category
├── render.c                # UI rendering implementation
├── system_integration.c    # System-level settings application
├── main.c                  # Entry point and theme stubs
├── Makefile                # Build configuration
└── README.md               # This file
```

### Data Structures

- **settings_app_t**: Main application state
- **settings_panel_t**: Content panel for each category
- **widget_t**: Generic UI widget (toggle, slider, dropdown, etc.)
- **display_settings_t**: Display configuration
- **appearance_settings_t**: Theme and appearance
- **sound_settings_t**: Audio configuration
- **network_settings_t**: Network configuration
- **users_settings_t**: User accounts
- **applications_settings_t**: Application preferences
- **privacy_settings_t**: Security and privacy
- **system_settings_t**: System information and updates

### UI Layout

```
┌──────────────────────────────────────────────────────┐
│ ← Settings                                    ○ ○ ○  │  Titlebar (40px)
├──────────────┬───────────────────────────────────────┤
│  Display     │  Display Settings                     │
│  Appearance  │                                       │
│  Sound       │  Resolution & Refresh Rate            │
│  Network     │  ─────────────────────────────────   │
│  Users       │  Resolution: [1920×1080 ▼]           │
│  Apps        │  Refresh Rate: [60 Hz ▼]             │
│  Privacy     │  Scale: [100% ▼]                      │
│  System      │                                       │
│              │  Night Light                          │
│              │  ─────────────────────────────────   │
│              │  [Night Light]         [ ON ]        │
│              │  Schedule: Sunset → Sunrise           │
│              │                                       │
│              │  Advanced                             │
│              │  ─────────────────────────────────   │
│              │  [Enable VSync]        [ ON ]        │
│              │                                       │
│  (240px)     │           (760px)                     │
└──────────────┴───────────────────────────────────────┘
```

## Building

```bash
cd userspace/apps/settings
make
```

Output: `build/userspace/apps/settings/bin/settings`

## Installation

```bash
make install
```

Installs to: `/usr/bin/settings`

## Usage

### Basic Launch
```bash
settings
```

### Open Specific Category
```bash
settings -c display
settings --category appearance
settings -c network
```

### Reset All Settings
```bash
settings --reset
```

### Command-Line Options
```
-h, --help             Show help message
-v, --version          Show version information
-c, --category NAME    Open specific category
-r, --reset            Reset all settings to defaults
```

## Integration Points

### System APIs

The settings application integrates with various system components:

1. **Compositor API**
   - Set display resolution/refresh rate
   - Configure VSync
   - Enable/disable effects
   - Apply night light

2. **Audio System**
   - Set master/output/input volumes
   - Switch audio devices
   - Configure audio routing

3. **Network Manager**
   - Enable/disable network interfaces
   - Configure IP addresses (DHCP/static)
   - Manage Wi-Fi connections
   - Configure VPN

4. **User Management**
   - Create/modify user accounts (useradd, usermod)
   - Set passwords
   - Configure PAM

5. **Application Manager**
   - Set default applications (XDG)
   - Configure autostart entries
   - Manage MIME type associations

6. **Security System**
   - Grant/revoke capabilities
   - Configure firewall rules
   - Enable/disable sandboxing
   - Configure file encryption

7. **System Services**
   - Configure automatic updates
   - Enable/disable developer mode
   - Set system logging verbosity

### Configuration Files

Settings are persisted to:
- `/etc/automationos/settings.conf` - Main settings file
- `~/.config/automationos/settings.conf` - User-specific overrides

## Widget Types

The settings UI supports 9 widget types:

1. **Section Header**: Category section titles
2. **Label**: Static text
3. **Toggle**: On/off switch (macOS-style)
4. **Slider**: Value adjustment (0.0 - 1.0)
5. **Dropdown**: Selection from list
6. **Button**: Clickable action button
7. **Text Input**: Text entry field
8. **Color Picker**: Color selection
9. **List**: Scrollable item list

## Event Handling

- Mouse clicks for widget interaction
- Scroll wheel for content scrolling
- Keyboard navigation (Tab, Enter, Escape)
- Keyboard shortcuts (Ctrl+S to save, Ctrl+F to search)

## Theme Support

The application respects system theme settings:
- Light mode
- Dark mode
- Auto (time-based switching)

Colors are provided by the desktop shell theme system.

## Future Enhancements

- [ ] Search functionality across all settings
- [ ] Keyboard shortcuts for all actions
- [ ] Settings import/export
- [ ] Settings profiles
- [ ] Quick settings shortcuts
- [ ] Context-sensitive help
- [ ] Undo/redo for setting changes
- [ ] Settings change history
- [ ] Advanced mode toggle
- [ ] Settings synchronization across devices

## Code Statistics

- **Total Lines**: ~3,500+ LOC
- **Header**: ~800 lines
- **Main Logic**: ~500 lines
- **Panel Creation**: ~900 lines
- **Rendering**: ~450 lines
- **System Integration**: ~500 lines
- **Entry Point**: ~350 lines

## Dependencies

- **libc**: Userspace C library (syscalls, string, stdio)
- **compositor**: GPU-accelerated rendering
- **desktop shell**: Theme and window management

## License

Part of AutomationOS - Copyright (c) 2026 AutomationOS Project

## Contributors

Built by: System Settings Engineer for AutomationOS Phase 1
