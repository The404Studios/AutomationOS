# File Explorer User Guide

Beautiful, powerful file manager for AutomationOS.

## Table of Contents

1. [Overview](#overview)
2. [Getting Started](#getting-started)
3. [User Interface](#user-interface)
4. [View Modes](#view-modes)
5. [Navigation](#navigation)
6. [File Operations](#file-operations)
7. [Search](#search)
8. [Preview](#preview)
9. [Keyboard Shortcuts](#keyboard-shortcuts)
10. [Settings](#settings)

## Overview

File Explorer is AutomationOS's native file manager, designed for speed, beauty, and power. It features:

- **Multiple view modes**: Icons, List, Columns, Gallery
- **Fast search**: Indexed file search with filters
- **Quick Look**: Preview files without opening them
- **Background operations**: Copy, move, delete with progress tracking
- **Rich previews**: Images, videos, documents, code files
- **Drag and drop**: Intuitive file management
- **Smooth animations**: Polished, responsive UI

## Getting Started

### Launching

```bash
# Open in home directory
explorer

# Open specific directory
explorer /path/to/directory

# Open with options
explorer --view list --show-hidden /home/documents
```

### Command Line Options

- `--view MODE` - Set view mode (icons, list, columns, gallery)
- `--sort MODE` - Set sort mode (name, size, date, type)
- `--show-hidden` - Show hidden files
- `--no-thumbnails` - Disable thumbnail generation

## User Interface

```
┌────────────────────────────────────────────────────────────┐
│ [<] [>] [Home] ~/Documents/Work          [🔍 Search...] [⚙]│ ← Toolbar
├──────────┬─────────────────────────────────────────────────┤
│ Recent   │  📁 Project1/                                   │
│ Home     │  📁 Project2/                                   │
│ Desktop  │  🖼️ screenshot.png      128 KB                  │ ← File View
│ Docs     │  📄 notes.txt            4 KB                   │
│ ↓        │  🎬 demo.mp4            2.3 GB                  │
├──────────┴─────────────────────────────────────────────────┤
│ 5 items | 2 selected | 2.4 GB                              │ ← Status Bar
└────────────────────────────────────────────────────────────┘
```

### Toolbar

The top toolbar provides quick access to common functions:

- **Back/Forward buttons**: Navigate history
- **Home button**: Go to home directory
- **Parent button**: Go to parent directory
- **Search box**: Search for files
- **View button**: Switch view modes
- **Sort button**: Change sort order
- **Settings button**: Open preferences

### Sidebar

The sidebar shows favorite locations and devices:

**Favorites:**
- Recent files
- Home directory
- Desktop
- Documents
- Downloads
- Pictures
- Videos
- Music

**Devices:**
- System drive
- External drives (USB, etc.)
- Network locations

**Network:**
- Shared folders
- Cloud storage

### File View

The main area displays files and folders. Click to select, double-click to open.

### Status Bar

Shows:
- Item count
- Selected count
- Total size
- Operation progress

## View Modes

### Icons View (Default)

Grid layout with large icons and thumbnails.

- **Best for**: Visual browsing, photos
- **Icon sizes**: 64, 96, 128, 256 pixels
- **Shows**: Icon, filename, optional size/date

```
┌────────────────────────────────────┐
│  📁        📁        🖼️        📄    │
│  folder1   folder2   image.png  doc.txt │
│                                    │
│  🎬        📦        📊        💾    │
│  video.mp4 archive.zip data.xlsx disk.iso │
└────────────────────────────────────┘
```

**Features:**
- Thumbnail previews for images/videos
- Adjustable icon size (Ctrl + Mouse Wheel)
- Smooth grid layout

### List View

Detailed multi-column view with sortable headers.

- **Best for**: Detailed information, sorting
- **Shows**: Name, Size, Type, Modified date, Permissions

```
┌─────────────────────────────────────────────┐
│ Name          Size      Type       Modified │
├─────────────────────────────────────────────┤
│ 📁 folder1    --        Folder     Today    │
│ 📄 doc.txt    4 KB      Text       Yesterday│
│ 🖼️ image.png  128 KB    PNG Image  Jan 15   │
│ 🎬 video.mp4  2.3 GB    Video      Jan 14   │
└─────────────────────────────────────────────┘
```

**Features:**
- Click column headers to sort
- Resizable columns
- Compact layout
- Alternating row colors

### Column View (Miller Columns)

Multi-pane browsing showing directory hierarchy.

- **Best for**: Deep directory structures
- **Shows**: Parent → Current → Child directories

```
┌────────┬────────┬────────┬────────────┐
│ Home   │ Docs   │ Work   │ project/   │
│ ─────  │ ─────  │ ─────  │ ────────── │
│ Desktop│►Work   │ src/   │ main.c     │
│►Docs   │ School │►project│ utils.c    │
│ Music  │ Games  │ notes/ │ Makefile   │
└────────┴────────┴────────┴────────────┘
```

**Features:**
- Navigate by clicking folders
- See hierarchy horizontally
- Quick navigation

### Gallery View

Large thumbnails optimized for photos.

- **Best for**: Photo collections, media libraries
- **Shows**: Large thumbnails (256px) with minimal text

```
┌─────────────────────────────────────┐
│ ▓▓▓▓▓  ▓▓▓▓▓  ▓▓▓▓▓  ▓▓▓▓▓  ▓▓▓▓▓ │
│ ▓▓▓▓▓  ▓▓▓▓▓  ▓▓▓▓▓  ▓▓▓▓▓  ▓▓▓▓▓ │
│                                     │
│ ▓▓▓▓▓  ▓▓▓▓▓  ▓▓▓▓▓  ▓▓▓▓▓  ▓▓▓▓▓ │
└─────────────────────────────────────┘
```

**Features:**
- Large thumbnails
- EXIF metadata display
- Slideshow mode (Space to start)

## Navigation

### Mouse Navigation

- **Single click**: Select file/folder
- **Double click**: Open file/folder
- **Right click**: Context menu
- **Drag**: Move/copy files

### Keyboard Navigation

- **Arrow keys**: Move selection
- **Enter**: Open selected item
- **Backspace**: Go to parent directory
- **Home**: Go to home directory

### Breadcrumb

Click path segments to jump to that directory:

```
Home / Documents / Work / Projects
  ↑       ↑         ↑        ↑
Click any segment to navigate
```

### History

- **Back button** (Alt+Left): Go back
- **Forward button** (Alt+Right): Go forward
- Recent locations stored in history

## File Operations

### Copy

1. Select files (Ctrl+Click for multiple)
2. Press **Ctrl+C** or right-click → Copy
3. Navigate to destination
4. Press **Ctrl+V** or right-click → Paste

**Background operation** shows progress dialog:

```
┌─────────────────────────────────────┐
│ Copying 42 files...                 │
├─────────────────────────────────────┤
│ ████████████████░░░░ 75%            │
│ Current: documents/report.pdf       │
│ 1.2 GB of 1.6 GB                    │
│ 15 MB/s • 30 seconds remaining      │
├─────────────────────────────────────┤
│ [Pause] [Cancel]          [Details]│
└─────────────────────────────────────┘
```

### Move

1. Select files
2. Press **Ctrl+X** or right-click → Cut
3. Navigate to destination
4. Press **Ctrl+V** or right-click → Paste

**Note**: Files are grayed out when cut.

### Delete

1. Select files
2. Press **Delete** or right-click → Delete
3. Files moved to trash (can be restored)

**Permanent delete**: **Shift+Delete** (cannot be undone)

### Rename

1. Select file
2. Press **F2** or right-click → Rename
3. Edit name inline
4. Press **Enter** to confirm

### Create Folder

1. Right-click empty space
2. Select "New Folder"
3. Enter folder name
4. Press **Enter**

### Properties

1. Select file
2. Press **Alt+Enter** or right-click → Properties
3. View/edit:
   - General info (size, dates)
   - Permissions
   - Metadata (EXIF, ID3, etc.)

## Search

### Quick Search

Type in search box to find files instantly:

```
┌─────────────────────────────────────┐
│ [Search in ~/Documents...]        🔍 │
├─────────────────────────────────────┤
│ Filters: [Images ▼] [Last Month ▼]  │
├─────────────────────────────────────┤
│ Results (42):                        │
│  🖼️ photo1.jpg    ~/Pictures        │
│  🖼️ screenshot.png ~/Desktop        │
│  📄 report.pdf    ~/Documents       │
└─────────────────────────────────────┘
```

### Advanced Search

Click filter button to access:

**File Type Filters:**
- All Files
- Documents
- Images
- Videos
- Audio
- Archives
- Code
- Executables

**Size Filters:**
- Tiny (< 10 KB)
- Small (< 1 MB)
- Medium (< 100 MB)
- Large (< 1 GB)
- Huge (> 1 GB)
- Custom range

**Date Filters:**
- Today
- Yesterday
- This week
- This month
- This year
- Custom range

**Search Options:**
- Case sensitive
- Regular expressions
- Content search (searches inside files)

### Search Index

File Explorer maintains a search index for fast results:

- **Background indexer** runs on startup
- **Incremental updates** when files change
- **Statistics** shown in status bar

## Preview (Quick Look)

Press **Space** to preview selected file without opening:

```
┌─────────────────────────────────────┐
│ screenshot.png               [× ]   │
├─────────────────────────────────────┤
│                                     │
│         [Image Preview]             │
│                                     │
│     1920×1080 • 2.3 MB • PNG       │
├─────────────────────────────────────┤
│ [Open] [Share] [Info]       [< >]  │
└─────────────────────────────────────┘
```

### Supported Formats

**Images**: PNG, JPG, GIF, BMP, SVG, WebP
- Zoom in/out (Ctrl +/-)
- Pan (drag with mouse)
- EXIF metadata

**Videos**: MP4, AVI, MKV, WebM
- Playback controls
- Timeline scrubbing
- Volume control

**Audio**: MP3, FLAC, WAV, OGG
- Waveform visualization
- Playback controls
- ID3 metadata

**Documents**: PDF, TXT, MD
- Page navigation
- Text rendering
- Markdown preview

**Code**: C, Python, JavaScript, etc.
- Syntax highlighting
- Line numbers
- Language detection

**Archives**: ZIP, TAR, 7Z
- File list
- Size statistics
- Extract options

### Navigation in Preview

- **Arrow keys**: Next/previous file
- **Escape**: Close preview
- **Space**: Play/pause (video/audio)
- **+/-**: Zoom in/out

## Keyboard Shortcuts

### General

| Shortcut | Action |
|----------|--------|
| Ctrl+N | New window |
| Ctrl+T | New tab |
| Ctrl+W | Close window/tab |
| Ctrl+Q | Quit |
| F11 | Fullscreen |
| Ctrl+, | Preferences |

### Navigation

| Shortcut | Action |
|----------|--------|
| Alt+Left | Back |
| Alt+Right | Forward |
| Alt+Up | Parent directory |
| Alt+Home | Home directory |
| Ctrl+L | Focus path bar |
| / | Focus search |

### Selection

| Shortcut | Action |
|----------|--------|
| Ctrl+A | Select all |
| Ctrl+Shift+A | Deselect all |
| Ctrl+Click | Add to selection |
| Shift+Click | Range selection |
| Ctrl+I | Invert selection |

### File Operations

| Shortcut | Action |
|----------|--------|
| Ctrl+C | Copy |
| Ctrl+X | Cut |
| Ctrl+V | Paste |
| Delete | Delete (to trash) |
| Shift+Delete | Permanent delete |
| Ctrl+D | Duplicate |
| F2 | Rename |
| Ctrl+Z | Undo |
| Ctrl+Y | Redo |

### View

| Shortcut | Action |
|----------|--------|
| Ctrl+1 | Icons view |
| Ctrl+2 | List view |
| Ctrl+3 | Column view |
| Ctrl+4 | Gallery view |
| Ctrl+H | Toggle hidden files |
| Ctrl++ | Larger icons |
| Ctrl+- | Smaller icons |
| Ctrl+0 | Default icon size |
| F5 | Refresh |

### Preview

| Shortcut | Action |
|----------|--------|
| Space | Quick Look |
| Esc | Close Quick Look |
| Ctrl++ | Zoom in |
| Ctrl+- | Zoom out |
| Ctrl+0 | Actual size |

## Settings

Access via **Ctrl+,** or menu:

### General

- **Default view mode**: Icons, List, Columns, Gallery
- **Default sort order**: Name, Size, Date, Type
- **Show hidden files by default**
- **Confirm file deletions**
- **Open folders in new tab/window**

### View Options

- **Icon size**: 64, 96, 128, 256 pixels
- **List row height**: Compact, Normal, Spacious
- **Show file extensions**
- **Show full path in title bar**

### Thumbnails

- **Enable thumbnails**: Yes/No
- **Thumbnail size**: 128, 256, 512 pixels
- **Cache location**: ~/.cache/thumbnails
- **Generate for videos**: Yes/No
- **Max file size**: 10 MB, 50 MB, 100 MB, Unlimited

### Performance

- **Smooth animations**: On/Off
- **Kinetic scrolling**: On/Off
- **Background indexing**: On/Off
- **Max search results**: 100, 500, 1000, Unlimited

### Advanced

- **Show dotfiles as hidden**
- **Follow symlinks**
- **Preserve permissions on copy**
- **Enable debug logging**

---

## Tips and Tricks

### Bulk Rename

1. Select multiple files
2. Press F2
3. Enter pattern: `photo_{n}` → photo_1, photo_2, ...

### Quick Navigation

- **Alt+Number**: Jump to sidebar place (Alt+1 = Recent, Alt+2 = Home, etc.)
- **Ctrl+Tab**: Switch between tabs
- **Drag folder to sidebar**: Add to favorites

### Custom Sorting

1. List view
2. Click column header once: Ascending
3. Click again: Descending
4. Shift+Click: Secondary sort

### Batch Operations

Select multiple files with Ctrl+Click, then:
- Right-click → Operations → Compress
- Right-click → Operations → Change permissions

### Search Tips

- Use `*` wildcard: `*.png` finds all PNG files
- Use size filters: `size:>10MB` finds files over 10 MB
- Use date filters: `modified:today` finds files modified today
- Combine filters: `*.jpg size:>1MB modified:this-week`

---

**Version**: 1.0.0  
**Last Updated**: 2026-05-26  
**Feedback**: File issues at github.com/AutomationOS/explorer
