# AutomationOS - Getting Started Guide

**Version:** 1.0.0  
**Target Audience:** New Users  
**Estimated Time:** 30-60 minutes  
**Last Updated:** 2026-05-26

---

## Table of Contents

1. [Welcome to AutomationOS](#welcome-to-automationos)
2. [What is AutomationOS?](#what-is-automationos)
3. [System Requirements](#system-requirements)
4. [Installation Methods](#installation-methods)
5. [First Boot Experience](#first-boot-experience)
6. [Desktop Tour](#desktop-tour)
7. [Essential Applications](#essential-applications)
8. [Basic System Configuration](#basic-system-configuration)
9. [File Management](#file-management)
10. [Network Setup](#network-setup)
11. [Installing Applications](#installing-applications)
12. [Getting Help](#getting-help)
13. [Next Steps](#next-steps)

---

## Welcome to AutomationOS

Welcome to AutomationOS, an AI-native operating system designed for modern computing and automation workloads. This guide will walk you through everything you need to know to get started with your new system.

### What You'll Learn

By the end of this guide, you will know how to:

- Install and boot AutomationOS
- Navigate the desktop environment
- Use essential applications
- Manage files and folders
- Configure system settings
- Connect to networks
- Install new applications
- Find help when you need it

### Prerequisites

- Basic computer literacy (using a mouse and keyboard)
- Ability to download files from the internet
- USB drive with at least 4GB capacity (for USB installation)
- Or a virtual machine platform like VirtualBox or QEMU

---

## What is AutomationOS?

AutomationOS is a modern operating system built from the ground up with the following principles:

### Key Features

**🚀 Fast & Lightweight**
- Minimal resource usage
- Quick boot times (< 5 seconds on modern hardware)
- Responsive user interface

**🔒 Secure by Design**
- Capability-based security model
- Sandboxed applications
- Regular security updates

**🤖 AI-Native Architecture**
- Built-in AI services and APIs
- Optimized for machine learning workloads
- Intelligent automation features

**🎨 Modern User Experience**
- Clean, intuitive interface
- Customizable desktop environment
- Full support for touch and pen input

### System Architecture

AutomationOS consists of several key components:

```
┌──────────────────────────────────────┐
│     Applications & User Programs     │
├──────────────────────────────────────┤
│        Desktop Environment           │
├──────────────────────────────────────┤
│        System Services               │
├──────────────────────────────────────┤
│        AutomationOS Kernel           │
├──────────────────────────────────────┤
│        Hardware Drivers              │
└──────────────────────────────────────┘
```

---

## System Requirements

### Minimum Requirements

To run AutomationOS, your computer should meet these minimum specifications:

**Processor:**
- 64-bit x86_64 CPU (Intel Core 2 Duo or AMD equivalent)
- 1 GHz clock speed minimum
- SSE2 support required

**Memory:**
- 512 MB RAM minimum
- 1 GB RAM recommended for desktop use

**Storage:**
- 4 GB free disk space for base installation
- 10 GB recommended for applications and user data

**Graphics:**
- VGA compatible graphics adapter
- 1024x768 resolution minimum
- VESA framebuffer support

**Firmware:**
- UEFI firmware (Secure Boot can be disabled)
- Legacy BIOS mode not supported

### Recommended Requirements

For the best experience with AutomationOS:

**Processor:**
- Intel Core i3 / AMD Ryzen 3 or better
- 2.0 GHz or higher
- Multi-core recommended

**Memory:**
- 2 GB RAM or more
- 4 GB for AI/ML workloads

**Storage:**
- 20 GB SSD recommended
- NVMe drive for best performance

**Graphics:**
- Intel HD Graphics 4000 or better
- AMD Radeon or NVIDIA GPU for GPU-accelerated AI

**Network:**
- Ethernet or WiFi adapter
- Intel, Realtek, or Broadcom chipsets recommended

### Supported Hardware

AutomationOS currently supports:

**CPU Architectures:**
- x86_64 (Intel 64-bit / AMD64)
- ARM64 support coming in Phase 4

**Storage Controllers:**
- AHCI SATA controllers
- NVMe PCIe SSDs
- VirtIO block devices (virtual machines)

**Network Adapters:**
- Intel E1000 series
- VirtIO network devices
- USB Ethernet adapters

**Input Devices:**
- PS/2 keyboards and mice
- USB HID keyboards and mice
- Touchscreens and touchpads

**Display Devices:**
- VESA-compatible framebuffers
- Intel integrated graphics
- Basic NVIDIA and AMD GPU support

### Virtual Machine Support

AutomationOS runs excellently in virtual machines:

**Supported Platforms:**
- ✅ QEMU/KVM (recommended for development)
- ✅ VirtualBox
- ✅ VMware Workstation/Fusion
- ✅ Hyper-V (Windows)
- ✅ Parallels (macOS)

**Recommended VM Settings:**
- 2 GB RAM or more
- 2 CPU cores or more
- 20 GB virtual disk
- Enable VirtIO drivers for best performance
- UEFI firmware mode

---

## Installation Methods

AutomationOS can be installed in several ways, depending on your needs and hardware.

### Method 1: Virtual Machine Installation (Recommended for First-Time Users)

Virtual machine installation is the easiest way to try AutomationOS without modifying your existing system.

#### Using QEMU (Linux/macOS/Windows)

**Step 1: Install QEMU**

```bash
# Ubuntu/Debian
sudo apt install qemu-system-x86

# macOS (with Homebrew)
brew install qemu

# Windows (download from qemu.org)
# Or use Chocolatey:
choco install qemu
```

**Step 2: Download AutomationOS ISO**

Download the latest AutomationOS ISO from:
- Official website: https://automationos.org/download
- GitHub releases: https://github.com/automationos/automationos/releases

**Step 3: Create Virtual Disk**

```bash
qemu-img create -f qcow2 automationos.qcow2 20G
```

**Step 4: Boot the ISO**

```bash
qemu-system-x86_64 \
    -enable-kvm \
    -m 2048 \
    -smp 2 \
    -bios /usr/share/ovmf/OVMF.fd \
    -drive file=automationos.qcow2,format=qcow2 \
    -cdrom AutomationOS.iso \
    -boot d
```

**Step 5: Follow Installation Wizard**

Once booted, follow the on-screen installation wizard to install AutomationOS to the virtual disk.

#### Using VirtualBox

**Step 1: Install VirtualBox**

Download and install VirtualBox from https://www.virtualbox.org/

**Step 2: Create New Virtual Machine**

1. Click "New" in VirtualBox Manager
2. Name: "AutomationOS"
3. Type: Linux
4. Version: Other Linux (64-bit)
5. Click "Next"

**Step 3: Configure Memory**

1. Allocate at least 2048 MB (2 GB) of RAM
2. Click "Next"

**Step 4: Create Virtual Hard Disk**

1. Select "Create a virtual hard disk now"
2. Click "Create"
3. Choose "VDI (VirtualBox Disk Image)"
4. Select "Dynamically allocated"
5. Set size to 20 GB or more
6. Click "Create"

**Step 5: Configure VM Settings**

1. Select your VM and click "Settings"
2. System → Motherboard:
   - Enable EFI (required!)
   - Boot order: Optical, Hard Disk
3. System → Processor:
   - Allocate 2 or more CPU cores
4. Display → Screen:
   - Video Memory: 128 MB
   - Graphics Controller: VMSVGA
5. Storage:
   - Click the empty optical drive
   - Click disk icon → "Choose a disk file"
   - Select AutomationOS.iso
6. Click "OK"

**Step 6: Start VM and Install**

1. Click "Start"
2. Follow the installation wizard

#### Using VMware

**Step 1: Create New Virtual Machine**

1. Open VMware Workstation/Fusion
2. File → New Virtual Machine
3. Select "Typical" configuration
4. Choose "Installer disc image file (iso)"
5. Browse to AutomationOS.iso
6. Click "Next"

**Step 2: Configure Guest OS**

1. Guest operating system: Linux
2. Version: Other Linux 5.x kernel 64-bit
3. Click "Next"

**Step 3: Name and Location**

1. Name: AutomationOS
2. Choose installation location
3. Click "Next"

**Step 4: Disk Configuration**

1. Disk size: 20 GB
2. Select "Store virtual disk as a single file"
3. Click "Next"

**Step 5: Customize Hardware**

1. Click "Customize Hardware"
2. Memory: 2048 MB or more
3. Processors: 2 cores or more
4. Firmware Type: UEFI
5. Click "Close"

**Step 6: Finish and Start**

1. Click "Finish"
2. Power on the virtual machine
3. Follow installation wizard

### Method 2: USB Drive Installation

For installing on physical hardware or testing on real machines.

#### Creating Bootable USB (Linux)

**Step 1: Identify USB Device**

```bash
lsblk
# Look for your USB drive (e.g., /dev/sdb)
# Make sure you identify the correct device!
```

**Step 2: Write ISO to USB**

```bash
sudo dd if=AutomationOS.iso of=/dev/sdX bs=4M status=progress
sudo sync
```

Replace `/dev/sdX` with your actual USB device (e.g., `/dev/sdb`).

**⚠️ WARNING:** This will erase all data on the USB drive!

#### Creating Bootable USB (Windows)

**Using Rufus (Recommended):**

1. Download Rufus from https://rufus.ie/
2. Insert USB drive (4GB or larger)
3. Launch Rufus
4. Device: Select your USB drive
5. Boot selection: Click "SELECT" and choose AutomationOS.iso
6. Partition scheme: GPT
7. Target system: UEFI (non CSM)
8. Click "START"
9. Wait for completion

**Using Etcher:**

1. Download balenaEtcher from https://www.balena.io/etcher/
2. Insert USB drive
3. Launch Etcher
4. Click "Flash from file" and select AutomationOS.iso
5. Click "Select target" and choose your USB drive
6. Click "Flash!"

#### Creating Bootable USB (macOS)

**Step 1: Identify USB Device**

```bash
diskutil list
# Look for your USB drive (e.g., /dev/disk2)
```

**Step 2: Unmount the Drive**

```bash
diskutil unmountDisk /dev/diskN
```

Replace `N` with your disk number.

**Step 3: Write ISO to USB**

```bash
sudo dd if=AutomationOS.iso of=/dev/rdiskN bs=1m
```

The `r` prefix (rdisk instead of disk) provides faster raw access.

**Step 4: Eject**

```bash
diskutil eject /dev/diskN
```

#### Booting from USB

1. Insert the USB drive into your computer
2. Restart your computer
3. Enter BIOS/UEFI settings (usually F2, F12, Del, or Esc during boot)
4. Disable Secure Boot if enabled
5. Set boot order to prioritize USB
6. Save and exit
7. The AutomationOS installer should boot

### Method 3: Direct Disk Installation

For advanced users who want to install directly to a disk.

**⚠️ WARNING:** This method will erase all data on the target disk!

**Step 1: Boot Installation Media**

Boot from the AutomationOS USB or ISO in a VM.

**Step 2: Start Installation**

The installation wizard will start automatically.

**Step 3: Select Language**

Choose your preferred language from the list.

**Step 4: Select Keyboard Layout**

Choose your keyboard layout (e.g., US English, UK English, German, etc.)

**Step 5: Partition Disk**

The installer offers two options:

**Automatic (Recommended):**
- Installer creates optimal partition layout
- Recommended for most users

**Manual:**
- For advanced users who want custom partitioning
- Allows full control over partitions

**Recommended Partition Layout:**

```
/dev/sda1  512 MB   EFI System Partition (ESP)
/dev/sda2  4 GB     Swap partition
/dev/sda3  Rest     Root filesystem (/)
```

**Step 6: Create User Account**

1. Enter your full name
2. Choose a username (lowercase, no spaces)
3. Create a strong password
4. Confirm password

**Step 7: Configure Hostname**

Choose a name for your computer (e.g., "my-automationos")

**Step 8: Review and Install**

1. Review your choices
2. Click "Install" to begin
3. Wait for installation to complete (5-15 minutes)

**Step 9: Reboot**

1. Remove installation media
2. Reboot into your new AutomationOS installation

---

## First Boot Experience

When you boot AutomationOS for the first time, you'll experience a guided setup process.

### Boot Sequence

```
UEFI Firmware
      ↓
AutoBoot (Bootloader)
      ↓
Kernel Initialization
      ↓
System Services
      ↓
Desktop Environment
      ↓
Login Screen
```

**Boot Time:** Typically 3-7 seconds on modern hardware

### Login Screen

The login screen provides:

**Features:**
- Username field
- Password field
- Session selection (if multiple desktop environments installed)
- Power options (shutdown, restart)
- Accessibility options

**First Login:**

1. Enter your username (created during installation)
2. Enter your password
3. Press Enter or click "Log In"

### Initial Setup Wizard

On first login, you'll see the Initial Setup Wizard:

**Step 1: Welcome**
- Introduction to AutomationOS
- Quick overview of features

**Step 2: Privacy**
- Choose telemetry preferences
- Configure data collection settings
- All can be disabled if desired

**Step 3: Online Accounts (Optional)**
- Connect email accounts
- Link cloud storage
- Can be skipped and configured later

**Step 4: Time Zone**
- Select your time zone
- Configure automatic time synchronization

**Step 5: Software Updates**
- Choose update preferences
- Automatic, manual, or notification only

**Step 6: Ready to Use**
- Complete setup
- Launch tutorial (optional)

### Desktop First Impressions

After setup, you'll see the AutomationOS desktop:

```
┌────────────────────────────────────────────────┐
│ [≡] AutomationOS         [WiFi] [🔊] [⚙] [👤] │  ← Top Bar
├────────────────────────────────────────────────┤
│                                                │
│                                                │
│                                                │
│            Desktop Area                        │
│                                                │
│                                                │
│                                                │
├────────────────────────────────────────────────┤
│  [📁] [🌐] [📝] [⚙]  AutomationOS  [◀ ▶]     │  ← Dock
└────────────────────────────────────────────────┘
```

**Key Elements:**

- **Top Bar:** System status and quick settings
- **Desktop:** Main workspace area
- **Dock:** Quick access to favorite applications
- **Application Menu:** Access all installed software

---

## Desktop Tour

Let's explore the AutomationOS desktop environment in detail.

### Top Bar

The top bar runs across the top of your screen and provides:

#### Left Side

**Application Menu (≡)**
- Click to access all applications
- Search for apps by name
- Browse by category
- Recently used apps

#### Right Side

**System Indicators:**

**Network Indicator**
- Shows connection status
- Click to see available networks
- Quick WiFi on/off toggle

**Volume Indicator**
- Current volume level
- Click to adjust volume
- Mute/unmute quickly

**Settings Indicator (⚙)**
- Quick settings panel
- Display brightness
- Night mode toggle
- Power settings

**User Indicator (👤)**
- User account menu
- System status
- Lock screen
- Log out
- Power off/restart

### Desktop Area

The main desktop area is your workspace:

**Features:**

**Background:**
- Customizable wallpaper
- Right-click for context menu
- Clean, distraction-free workspace

**Icons (Optional):**
- Can show desktop icons
- Files, folders, and shortcuts
- Drag and drop support

**Context Menu (Right-Click):**
- New file/folder
- Change wallpaper
- Display settings
- Desktop preferences

### Dock

The dock provides quick access to applications:

**Default Applications:**
- 📁 File Explorer
- 🌐 Web Browser
- 📝 Text Editor
- ⚙ Settings
- 📧 Mail (if configured)

**Dock Features:**

**Adding Apps:**
1. Open Application Menu
2. Find desired app
3. Right-click
4. Select "Add to Dock"

**Removing Apps:**
1. Right-click app in dock
2. Select "Remove from Dock"

**Rearranging Apps:**
- Click and drag apps to reorder

**App Indicators:**
- Running apps have a dot indicator
- Notifications show badge counters
- Active window is highlighted

### Workspaces

AutomationOS supports multiple virtual workspaces:

**What are Workspaces?**
- Separate desktop areas
- Organize different tasks
- Switch quickly between contexts

**Using Workspaces:**
- **Create:** Top bar → Workspace switcher → "+"
- **Switch:** Ctrl+Alt+Left/Right arrows
- **Move Window:** Ctrl+Alt+Shift+Left/Right

**Typical Workflow:**
- Workspace 1: Web browsing and email
- Workspace 2: Development and coding
- Workspace 3: Documents and writing
- Workspace 4: Media and entertainment

### Window Management

Managing application windows is intuitive:

**Window Controls:**
- 🗕 Minimize: Hide window (still running)
- 🗖 Maximize: Full screen
- 🗙 Close: Exit application

**Window Operations:**

**Moving Windows:**
- Click and drag title bar
- Or: Hold Alt + drag anywhere

**Resizing Windows:**
- Drag window edges
- Drag corners for proportional resize

**Snapping Windows:**
- Drag to left edge: Snap left half
- Drag to right edge: Snap right half
- Drag to top: Maximize
- Drag to corner: Quarter screen

**Keyboard Shortcuts:**
- Super+Left: Snap window left
- Super+Right: Snap window right
- Super+Up: Maximize window
- Super+Down: Restore window

### Notifications

The notification system keeps you informed:

**Notification Center:**
- Click clock in top bar
- See all notifications
- Grouped by application
- Clear individually or all at once

**Notification Types:**
- System updates available
- Application alerts
- Calendar events
- Download complete
- Low battery warning

**Do Not Disturb:**
- Settings → Notifications → Enable DND
- Suppresses all notifications
- Useful during presentations

---

## Essential Applications

AutomationOS comes with a suite of essential applications pre-installed.

### File Explorer

The File Explorer is your gateway to managing files and folders.

**Features:**
- Browse files and folders
- Multiple view modes (icons, list, columns)
- Search functionality
- File preview
- Batch operations
- Network browsing

**Interface:**

```
┌─────────────────────────────────────────┐
│ ← → ↑  /home/user/Documents       🔍   │
├──────┬──────────────────────────────────┤
│      │  Name      Size    Modified      │
│ Home ├──────────────────────────────────┤
│ Desk │  📄 Report.txt    12 KB  Today   │
│ Docs │  📁 Projects      --     May 20  │
│ Down │  📊 Data.xlsx     340 KB May 18  │
│ Pics │  🖼 Photo.jpg     2.1 MB May 15  │
│      │                                  │
│ Net  │                                  │
│ Trash│                                  │
└──────┴──────────────────────────────────┘
```

**Common Tasks:**

**Creating Folders:**
1. Right-click in empty space
2. Select "New Folder"
3. Enter folder name
4. Press Enter

**Copying Files:**
1. Select file(s)
2. Press Ctrl+C (or right-click → Copy)
3. Navigate to destination
4. Press Ctrl+V (or right-click → Paste)

**Moving Files:**
- Drag and drop to move
- Or: Cut (Ctrl+X) and Paste (Ctrl+V)

**Deleting Files:**
1. Select file(s)
2. Press Delete key
3. Files move to Trash
4. Empty Trash to permanently delete

**See also:** [File Management](#file-management) section below

### Terminal

The Terminal provides command-line access to your system.

**Features:**
- Multiple tabs
- Command history
- Text selection and copy/paste
- Customizable colors and fonts
- Profile support

**Opening Terminal:**
- Application Menu → Terminal
- Or: Ctrl+Alt+T

**Basic Commands:**

```bash
# List files
ls

# Change directory
cd Documents

# Print working directory
pwd

# Create directory
mkdir new_folder

# Create file
touch newfile.txt

# View file contents
cat file.txt

# Remove file
rm file.txt

# Copy file
cp source.txt destination.txt

# Move file
mv oldname.txt newname.txt

# System information
uname -a

# Disk usage
df -h

# Process list
ps aux
```

**Tips:**
- Use Tab for auto-completion
- Up/Down arrows for command history
- Ctrl+C to cancel running command
- Ctrl+D to exit terminal
- Ctrl+Shift+C/V for copy/paste

### Text Editor

The integrated text editor handles all your writing needs.

**Features:**
- Syntax highlighting for code
- Line numbers
- Search and replace
- Multiple file tabs
- Auto-save
- Dark/light themes

**Supported File Types:**
- Plain text (.txt)
- Code files (.c, .py, .js, etc.)
- Configuration files
- Markdown (.md)
- And more

**Common Operations:**

**New File:**
- File → New (Ctrl+N)

**Open File:**
- File → Open (Ctrl+O)
- Browse to file
- Click Open

**Save File:**
- File → Save (Ctrl+S)
- Choose location and filename
- Click Save

**Find and Replace:**
- Edit → Find (Ctrl+F)
- Enter search term
- Click arrows to navigate matches
- Edit → Replace (Ctrl+H) for find/replace

**Keyboard Shortcuts:**
- Ctrl+N: New file
- Ctrl+O: Open file
- Ctrl+S: Save
- Ctrl+Shift+S: Save As
- Ctrl+F: Find
- Ctrl+H: Replace
- Ctrl+Z: Undo
- Ctrl+Y: Redo

### System Settings

The Settings application controls all system configuration.

**Categories:**

**System:**
- About (version, hardware info)
- Date & Time
- Language & Region
- Accessibility

**Appearance:**
- Background
- Themes
- Fonts
- Dock preferences

**Hardware:**
- Displays
- Sound
- Power
- Printers
- Keyboard
- Mouse & Touchpad

**Network:**
- WiFi
- Ethernet
- VPN
- Proxy

**Users:**
- User accounts
- Login options
- Parental controls

**Applications:**
- Default applications
- Installed apps
- Application permissions

**Privacy & Security:**
- Screen lock
- File encryption
- Firewall
- Application sandboxing

**See also:** [Basic System Configuration](#basic-system-configuration) section below

### Task Manager

Monitor system resources and manage processes.

**Features:**
- CPU usage graph
- Memory usage
- Running processes
- System uptime
- Kill/terminate processes

**Opening Task Manager:**
- Ctrl+Shift+Esc
- Or: Application Menu → System → Task Manager

**Interface:**

```
┌──────────────────────────────────────┐
│ Task Manager                    [✕]  │
├──────────────────────────────────────┤
│ CPU: [▓▓▓▓░░░░░░] 45%               │
│ RAM: [▓▓▓▓▓▓▓░░░] 72% (1.4/2.0 GB)  │
├──────────────────────────────────────┤
│ Process        CPU%   Memory         │
├──────────────────────────────────────┤
│ Desktop Shell  12%    180 MB  [End] │
│ File Explorer  5%     95 MB   [End] │
│ Terminal       3%     45 MB   [End] │
│ System         15%    250 MB         │
└──────────────────────────────────────┘
```

**Operations:**
- View process details
- Sort by CPU or memory
- End unresponsive applications
- Monitor system performance

### Calculator

Perform calculations quickly and easily.

**Features:**
- Basic arithmetic
- Scientific functions
- Programmer mode
- Unit conversions
- History

**Opening Calculator:**
- Application Menu → Calculator
- Or: Search for "Calculator"

**Modes:**
- **Basic:** Addition, subtraction, multiplication, division
- **Scientific:** Trigonometry, logarithms, exponents
- **Programmer:** Binary, hexadecimal, bitwise operations
- **Converter:** Length, weight, temperature, currency

---

## Basic System Configuration

Let's configure essential system settings for optimal experience.

### Display Settings

**Accessing Display Settings:**
Settings → Hardware → Displays

**Resolution:**
1. Select your display
2. Choose resolution from dropdown
3. Click "Apply"
4. Confirm if display looks correct

**Scaling:**
- 100%: Standard (default)
- 125%: Slightly larger (common for laptops)
- 150%: Large (for 4K displays)
- 200%: Extra large (for high DPI)

**Orientation:**
- Landscape (default)
- Portrait
- Landscape (flipped)
- Portrait (flipped)

**Multiple Displays:**
1. Connect second display
2. Settings → Displays
3. Arrange displays by dragging
4. Choose primary display
5. Set mode:
   - **Mirror:** Same content on both
   - **Extend:** Extended desktop
   - **Single:** Use only one display

**Night Mode:**
- Settings → Displays → Night Mode
- Reduces blue light at night
- Set schedule or manual toggle

### Sound Configuration

**Accessing Sound Settings:**
Settings → Hardware → Sound

**Output Device:**
1. Select output device (speakers, headphones)
2. Adjust volume level
3. Test with test sound

**Input Device:**
1. Select microphone
2. Adjust input level
3. Test recording

**Sound Effects:**
- System sounds on/off
- Volume of alerts
- Choose sound theme

### Power Management

**Accessing Power Settings:**
Settings → Hardware → Power

**Power Plan:**
- **Balanced:** Good performance and battery life
- **Performance:** Maximum performance
- **Power Saver:** Extend battery life

**Screen Blanking:**
- Never
- 1 minute
- 5 minutes (default)
- 10 minutes
- 15 minutes

**Automatic Suspend:**
- On battery: 15 minutes (default)
- When plugged in: Never (default)

**Laptop Lid:**
- Close lid behavior:
  - Suspend
  - Hibernate
  - Lock screen
  - Nothing

**Battery Indicator:**
- Top bar shows battery percentage
- Warning at 20%
- Critical warning at 10%

### Date and Time

**Accessing Date & Time Settings:**
Settings → System → Date & Time

**Automatic Time:**
- Enable "Set automatically"
- Syncs with network time servers
- Recommended for most users

**Manual Time:**
1. Disable "Set automatically"
2. Click date/time to adjust
3. Click "Set" to confirm

**Time Zone:**
1. Click "Time Zone"
2. Search for your city
3. Or click map location
4. Confirm selection

**Time Format:**
- 24-hour (default): 14:30
- 12-hour: 2:30 PM

### Language and Region

**Accessing Language Settings:**
Settings → System → Language & Region

**Display Language:**
1. Click "Language"
2. Select preferred language
3. Log out and back in to apply

**Input Sources:**
1. Click "+"
2. Select language/keyboard layout
3. Choose specific variant
4. Click "Add"

**Switching Input:**
- Super+Space
- Or: Click language indicator in top bar

**Region Format:**
- Affects date, time, number formats
- Usually matches language
- Can customize separately

### User Accounts

**Managing User Accounts:**
Settings → Users

**Current User:**
- Change profile picture
- Modify full name
- Change password

**Changing Password:**
1. Click "Change Password"
2. Enter current password
3. Enter new password
4. Confirm new password
5. Click "Change"

**Adding Users:**
1. Click "Add User"
2. Enter account details
3. Choose account type:
   - **Administrator:** Full system access
   - **Standard:** Regular user
4. Set password
5. Click "Add"

**Account Types:**

**Administrator:**
- Install software
- Change system settings
- Manage other users
- Full system access

**Standard User:**
- Use installed applications
- Change own settings
- Limited system changes
- No user management

### Appearance

**Accessing Appearance Settings:**
Settings → Appearance

**Theme:**
- **Light:** Bright theme (default)
- **Dark:** Dark theme (easier on eyes)
- **Auto:** Follows time of day

**Accent Color:**
- Choose system accent color
- Affects buttons, links, highlights

**Background:**
- Choose from wallpaper collection
- Or select custom image
- Solid color option
- Slideshow mode

**Fonts:**
- Interface font
- Document font
- Monospace font (for Terminal)
- Font size

**Dock:**
- Position: Bottom, Left, Right
- Icon size: Small, Medium, Large
- Auto-hide: On/Off
- Show running indicator

---

## File Management

Effective file management is essential for productivity.

### File System Basics

**Understanding the File System:**

```
/                     (Root of filesystem)
├── home/             (User home directories)
│   └── username/     (Your personal files)
│       ├── Desktop/  (Desktop files)
│       ├── Documents/(Your documents)
│       ├── Downloads/(Downloaded files)
│       ├── Pictures/ (Photos and images)
│       ├── Music/    (Audio files)
│       └── Videos/   (Video files)
├── bin/              (System binaries)
├── etc/              (Configuration files)
├── usr/              (User programs)
└── tmp/              (Temporary files)
```

**Important Locations:**

**Home Directory (`~`):**
- Your personal files
- Each user has their own
- Full read/write access

**Desktop:**
- Files visible on desktop
- Quick access location

**Documents:**
- Your work files
- Organized storage

**Downloads:**
- Files from internet
- Default download location

**Trash:**
- Deleted files (recoverable)
- Empty to permanently delete

### Working with Files

**Selecting Files:**

**Single File:**
- Click once on file

**Multiple Files:**
- Ctrl+Click each file
- Or: Click and drag to select area
- Or: Ctrl+A to select all

**Range of Files:**
- Click first file
- Shift+Click last file
- All files between selected

**File Operations:**

**Copying Files:**
1. Select file(s)
2. Method 1: Ctrl+C, navigate, Ctrl+V
3. Method 2: Right-click → Copy, navigate, right-click → Paste
4. Method 3: Drag with Ctrl held

**Moving Files:**
1. Select file(s)
2. Method 1: Ctrl+X, navigate, Ctrl+V
3. Method 2: Drag and drop
4. Method 3: Right-click → Cut, navigate, paste

**Renaming Files:**
1. Select file
2. Method 1: Press F2
3. Method 2: Right-click → Rename
4. Type new name
5. Press Enter

**Deleting Files:**
1. Select file(s)
2. Method 1: Press Delete
3. Method 2: Right-click → Move to Trash
4. File moved to Trash (not permanent)

**Permanently Deleting:**
1. Open Trash
2. Select file(s)
3. Right-click → Delete Permanently
4. Or: Empty Trash (deletes all)

**File Properties:**
1. Right-click file
2. Select "Properties"
3. View:
   - Size
   - Type
   - Location
   - Created/modified dates
   - Permissions

### Organizing Files

**Creating Folders:**

1. Navigate to location
2. Method 1: Right-click → New Folder
3. Method 2: Ctrl+Shift+N
4. Enter folder name
5. Press Enter

**Folder Structure Example:**

```
Documents/
├── Work/
│   ├── 2026/
│   │   ├── Q1/
│   │   ├── Q2/
│   │   └── Q3/
│   └── Projects/
│       ├── Project_A/
│       └── Project_B/
├── Personal/
│   ├── Finance/
│   ├── Health/
│   └── Travel/
└── Archive/
    ├── 2024/
    └── 2025/
```

**Naming Best Practices:**

**Good Names:**
- Use descriptive names
- Include dates: `2026-05-26-report.txt`
- Use underscores: `my_document.txt`
- Be consistent

**Avoid:**
- Special characters (except `-`, `_`)
- Spaces in names (use underscores)
- Very long names
- Non-English characters (for compatibility)

### Searching for Files

**Quick Search:**
1. Open File Explorer
2. Click search box (or Ctrl+F)
3. Type search term
4. Results appear as you type

**Advanced Search:**

**By Name:**
- Just type filename

**By Type:**
- `type:document` - Documents
- `type:image` - Images
- `type:video` - Videos
- `type:audio` - Audio files

**By Date:**
- `modified:today`
- `modified:yesterday`
- `modified:thisweek`
- `modified:lastweek`

**By Size:**
- `size:>10MB` - Larger than 10 MB
- `size:<1MB` - Smaller than 1 MB

**Combined:**
- `report type:document modified:thisweek`

### File Compression

**Creating Archives:**

1. Select file(s) to compress
2. Right-click
3. Select "Compress"
4. Choose format:
   - `.zip` (most compatible)
   - `.tar.gz` (best compression)
   - `.7z` (very good compression)
5. Enter archive name
6. Click "Create"

**Extracting Archives:**

1. Right-click archive file
2. Select "Extract Here" or "Extract To..."
3. Files extracted to location

**Command Line:**

```bash
# Create zip archive
zip -r archive.zip folder/

# Extract zip
unzip archive.zip

# Create tar.gz
tar -czf archive.tar.gz folder/

# Extract tar.gz
tar -xzf archive.tar.gz
```

---

## Network Setup

Connecting to networks enables internet access and resource sharing.

### WiFi Connection

**Connecting to WiFi:**

1. Click Network indicator in top bar
2. List of networks appears
3. Click your network name
4. Enter password if required
5. Click "Connect"
6. Connection established

**WiFi Settings:**

Settings → Network → WiFi

**Options:**
- Enable/disable WiFi
- Forget saved networks
- View connection details
- Set up hotspot

**Troubleshooting WiFi:**

**Can't see network:**
- Check WiFi is enabled
- Check router is on
- Try scanning again

**Wrong password:**
- Re-enter carefully
- Check Caps Lock
- Try forgetting and reconnecting

**Connected but no internet:**
- Check router has internet
- Try disconnecting/reconnecting
- Restart router if needed

### Ethernet Connection

**Wired Connection:**

1. Plug Ethernet cable into computer
2. Connection usually automatic
3. Check top bar for connected status

**Ethernet Settings:**

Settings → Network → Wired

**Options:**
- View connection status
- Configure IP address (DHCP or static)
- Set DNS servers
- View connection speed

### VPN Configuration

**Setting up VPN:**

1. Settings → Network → VPN
2. Click "+" to add VPN
3. Choose VPN type:
   - OpenVPN
   - L2TP/IPSec
   - PPTP
   - WireGuard
4. Enter connection details:
   - Server address
   - Username
   - Password
   - Certificates (if required)
5. Click "Add"

**Connecting to VPN:**

1. Click Network indicator in top bar
2. Click "VPN"
3. Select your VPN
4. Click "Connect"
5. Enter password if prompted
6. Wait for connection

**VPN Status:**
- Lock icon shows VPN active
- All traffic routed through VPN
- IP address changed

### Network Sharing

**File Sharing:**

1. Right-click folder to share
2. Select "Sharing Options"
3. Enable "Share this folder"
4. Choose permissions:
   - Read-only
   - Read and write
5. Set password if desired
6. Click "Create Share"

**Accessing Shared Folders:**

1. File Explorer → Network
2. Browse to computer
3. Enter credentials if required
4. Access shared folders

**Printer Sharing:**

1. Settings → Hardware → Printers
2. Select printer
3. Enable "Share this printer"
4. Set share name
5. Others can now add shared printer

---

## Installing Applications

AutomationOS makes installing software simple and safe.

### Application Store

**Opening App Store:**
- Application Menu → Software
- Or: Search for "Software"

**Browsing Applications:**

**Categories:**
- Productivity
- Development
- Graphics & Design
- Audio & Video
- Games
- Utilities
- Education
- Internet

**Featured:**
- Editor's picks
- New releases
- Popular apps
- Recommended for you

**Searching:**
1. Click search icon
2. Type app name or description
3. Results appear instantly
4. Click to view details

**Installing Applications:**

1. Find desired application
2. Click application
3. Read description and reviews
4. Click "Install"
5. Enter password if prompted
6. Wait for installation
7. Click "Launch" when complete

**Updating Applications:**

**Automatic Updates:**
- Settings → Software → Automatic Updates
- Enable for automatic updates
- Or choose "Notify only"

**Manual Updates:**
1. Open Software app
2. Click "Updates" tab
3. Click "Update All"
4. Or update individually

**Removing Applications:**

1. Open Software app
2. Click "Installed" tab
3. Find application
4. Click "Remove"
5. Confirm removal

### Package Manager (Advanced)

For advanced users, command-line package management is available.

**Package Manager: `autopkg`**

**Installing Packages:**

```bash
# Update package list
sudo autopkg update

# Install package
sudo autopkg install package-name

# Install multiple packages
sudo autopkg install pkg1 pkg2 pkg3
```

**Searching Packages:**

```bash
# Search for package
autopkg search keyword

# Show package info
autopkg info package-name

# List files in package
autopkg files package-name
```

**Updating System:**

```bash
# Update all packages
sudo autopkg upgrade

# Update specific package
sudo autopkg upgrade package-name
```

**Removing Packages:**

```bash
# Remove package
sudo autopkg remove package-name

# Remove package and dependencies
sudo autopkg autoremove package-name
```

**Managing Repositories:**

```bash
# List repositories
autopkg repos

# Add repository
sudo autopkg add-repo repo-url

# Remove repository
sudo autopkg remove-repo repo-name
```

### Installing from Source

For developers and advanced users:

**General Process:**

```bash
# Download source code
git clone https://github.com/author/project.git
cd project

# Read installation instructions
cat README.md

# Configure
./configure

# Build
make

# Install
sudo make install
```

**Dependencies:**

Most projects require build tools:

```bash
# Install build essentials
sudo autopkg install build-essential automake autoconf
```

### Third-Party Applications

**Installing .pkg Files:**

1. Download .pkg file
2. Double-click to open
3. Follow installation wizard
4. Enter password when prompted
5. Click "Install"

**⚠️ Security Note:**
Only install .pkg files from trusted sources!

---

## Getting Help

AutomationOS provides several ways to get help when you need it.

### Built-in Help System

**Opening Help:**
- Application Menu → Help
- Or: Press F1 in most applications
- Or: Help menu in application menu bar

**Help Browser:**
- Searchable help topics
- Step-by-step tutorials
- Troubleshooting guides
- Keyboard shortcut reference

**Context-Sensitive Help:**
- Press F1 in application
- Gets help for current feature
- Explains what you're viewing

### Documentation

**User Documentation:**
- `/usr/share/doc/automationos/user/`
- Browse in File Explorer
- Or view online at docs.automationos.org

**Keyboard Shortcuts:**

Press F1 and search for "shortcuts" or:

**Essential Shortcuts:**

**System:**
- Super: Open application menu
- Super+L: Lock screen
- Alt+F4: Close window
- Print Screen: Screenshot
- Ctrl+Alt+T: Open terminal

**Window Management:**
- Super+Left/Right: Snap window
- Super+Up: Maximize
- Super+Down: Restore
- Alt+Tab: Switch windows
- Alt+`: Switch windows of same app

**File Management:**
- Ctrl+C: Copy
- Ctrl+X: Cut
- Ctrl+V: Paste
- Ctrl+Z: Undo
- Ctrl+Shift+Z: Redo
- Ctrl+A: Select all
- Ctrl+F: Find
- Delete: Move to trash

### Community Support

**Forums:**
- forum.automationos.org
- Ask questions
- Share knowledge
- Connect with users

**Chat:**
- Discord: discord.gg/automationos
- IRC: #automationos on libera.chat
- Real-time community help

**Bug Reports:**
- github.com/automationos/automationos/issues
- Report bugs
- Request features
- Track development

### Professional Support

**Commercial Support:**
- support@automationos.org
- Enterprise support contracts
- Priority assistance
- Custom development

---

## Next Steps

Now that you're familiar with AutomationOS basics, here are some next steps:

### Learning More

**User Manual:**
Read the complete user manual:
- `docs/user/USER_MANUAL.md`
- Comprehensive feature documentation
- Advanced techniques
- Pro tips

**Video Tutorials:**
- tutorial.automationos.org
- Watch step-by-step guides
- Visual learning

**Blog:**
- blog.automationos.org
- Tips and tricks
- Feature highlights
- News and updates

### Exploring Features

**Try These Applications:**
1. Install a game from Software Center
2. Edit a photo in Image Viewer
3. Create a document in Office suite
4. Browse web with included browser
5. Customize your desktop appearance

**Advanced Features:**
- Multiple workspaces
- Keyboard shortcuts
- Command line basics
- Automation with scripts

### Contributing

**Help Improve AutomationOS:**

**Report Bugs:**
- Help us fix issues
- github.com/automationos/issues

**Translate:**
- Help translate AutomationOS
- translate.automationos.org

**Documentation:**
- Improve documentation
- Write tutorials

**Spread the Word:**
- Tell friends about AutomationOS
- Write blog posts
- Share on social media

### Customization

**Make it Yours:**

**Appearance:**
- Try different themes
- Change wallpaper collection
- Customize dock
- Choose favorite fonts

**Workflow:**
- Set up custom keyboard shortcuts
- Organize your files
- Configure favorite applications
- Create startup programs

**Extensions:**
- Browse available extensions
- Add functionality
- Customize behavior

---

## Congratulations!

You've completed the AutomationOS Getting Started Guide. You now know:

✅ How to install and boot AutomationOS  
✅ Navigate the desktop environment  
✅ Use essential applications  
✅ Configure system settings  
✅ Manage files and folders  
✅ Connect to networks  
✅ Install applications  
✅ Get help when needed

**What's Next?**

Continue your journey with:
- **[User Manual](USER_MANUAL.md)** - Comprehensive reference
- **[Application Guides](apps/)** - Detailed app documentation
- **[Developer Guide](../dev/APP_DEVELOPMENT.md)** - Build your own apps

Welcome to the AutomationOS community! 🎉

---

**Document Information**

- **Title:** AutomationOS - Getting Started Guide
- **Version:** 1.0.0
- **Words:** ~8,000
- **Lines:** ~2,000
- **Last Updated:** 2026-05-26
- **Maintained By:** AutomationOS Documentation Team
- **License:** Same as AutomationOS project

---

**Need Help?**

- 📧 Email: support@automationos.org
- 💬 Forum: forum.automationos.org
- 🐛 Issues: github.com/automationos/automationos/issues
- 📚 Docs: docs.automationos.org
