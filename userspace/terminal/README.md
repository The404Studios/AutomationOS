# Minimal Terminal for AutomationOS

A simple terminal emulator with basic built-in shell functionality.

## Features

- **Window Management**: 800x600 pixel window via window manager
- **Text Rendering**: VGA 8x16 bitmap font (ASCII 32-126)
- **Terminal Buffer**: 80x25 character grid
- **Scrolling**: Automatic scrolling when buffer fills
- **Cursor**: Visible cursor at input position
- **Keyboard Input**: Full keyboard support with special key handling
- **Built-in Shell**: Minimal shell with basic commands

## Built-in Commands

- `echo <text>` - Print text to screen
- `ls` - List files (via VFS syscalls)
- `clear` - Clear screen
- `help` - Show available commands
- `exit` - Exit terminal (or Ctrl+D)

## Architecture

### Components

1. **main.c** - Main event loop and terminal initialization
2. **window.c** - Window management and framebuffer rendering
3. **font.c** - VGA 8x16 bitmap font rendering
4. **shell.c** - Built-in shell command processor

### Data Flow

```
User Input (keyboard)
    ↓
Window Event System
    ↓
Terminal Input Handler
    ↓
Shell Command Processor
    ↓
Terminal Buffer Update
    ↓
Window Renderer
    ↓
Framebuffer Display
```

### Terminal Buffer

- **Structure**: 2D array of cells (80 cols × 25 rows)
- **Cell**: Character + foreground color + background color
- **Cursor**: Tracks current input position (x, y)
- **Scrolling**: When cursor reaches bottom, scroll buffer up one line

### Font Rendering

- **Font**: VGA 8x16 bitmap font (8 pixels wide, 16 pixels tall)
- **Coverage**: Printable ASCII (32-126)
- **Rendering**: Direct pixel plotting to framebuffer
- **Colors**: RGB color support (foreground/background)

## Building

```bash
cd userspace/terminal
make
```

Output: `../../build/userspace/terminal/terminal`

## Integration Points

### Window Manager API

The terminal uses these window manager functions (currently stubbed):

- `wm_create_window()` - Create application window
- `wm_destroy_window()` - Destroy window
- `window_poll_event()` - Poll for keyboard/mouse events

### System Calls (Future)

Commands that need syscall integration:

- `ls` - Needs `SYS_OPEN`, `SYS_READ` for directory listing
- `cat` - Needs `SYS_OPEN`, `SYS_READ` for file reading
- Process spawning - Needs `SYS_FORK`, `SYS_EXECVE` for running binaries

## Testing

### Manual Testing

1. Build the terminal
2. Run: `./build/userspace/terminal/terminal`
3. Type characters - they should appear on screen
4. Press Enter - should execute command
5. Try commands: `help`, `echo hello`, `clear`, `ls`
6. Press Ctrl+D or type `exit` to quit

### Expected Behavior

- Characters appear as you type
- Backspace removes characters
- Enter executes the command line
- Prompt (`$ `) appears after each command
- Screen scrolls when buffer fills
- Cursor is visible at input position

## Future Enhancements

### v2 Features

1. **PTY Integration**: Real shell spawning (bash, sh)
2. **ANSI Escape Codes**: Color and cursor control
3. **Command History**: Up/Down arrow for history
4. **Tab Completion**: File and command completion
5. **Copy/Paste**: Text selection and clipboard
6. **Resizable Window**: Dynamic buffer resizing

### v3 Features

1. **Multiple Tabs**: Tab management
2. **Split Panes**: Horizontal/vertical splits
3. **Scrollback**: Scroll history with PgUp/PgDn
4. **Search**: Find text in buffer
5. **Themes**: Color schemes

## Code Structure

### Header Files

- `window.h` - Window and buffer structures, function declarations
- `font.h` - Font rendering interface
- `shell.h` - Shell structures and command handlers

### Implementation Files

- `main.c` (165 lines) - Terminal lifecycle and event loop
- `window.c` (295 lines) - Window/buffer management and rendering
- `font.c` (410 lines) - VGA font bitmap and rendering
- `shell.c` (160 lines) - Shell command processing

**Total**: ~1,030 lines of C code

## Differences from Advanced Terminal

This minimal terminal (`userspace/terminal/`) differs from the advanced terminal (`userspace/apps/terminal/`) in:

- **No GPU acceleration** - Software rendering only
- **No tabs/panes** - Single terminal only
- **No VT100 parsing** - Simple text rendering
- **Built-in commands only** - No process spawning yet
- **Fixed size** - 80x25, no resizing
- **Simple implementation** - ~1K lines vs ~10K+ lines

## License

Part of AutomationOS.
