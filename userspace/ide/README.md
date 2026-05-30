# AutomationOS Native IDE

A comprehensive Integrated Development Environment designed specifically for AutomationOS kernel and userspace development.

## Overview

The AutomationOS IDE is a native IDE featuring:

- **Code Editor**: Multi-file editing with syntax highlighting
- **Blueprint Visual Editor**: Node-based visual programming (INTEGRATED)
- **Debugger**: GDB integration with breakpoints, watchpoints, and variable inspection
- **Project Manager**: Build system integration (Make/CMake)
- **Compiler Integration**: GCC/Clang with error reporting
- **Git Integration**: Version control operations
- **Terminal**: Embedded terminal emulator

## Architecture

### Core Components

```
ide/
├── ide.h              - Main IDE context and API
├── ide_main.c         - Entry point and main event loop
├── editor.h/c         - Code editor component
├── blueprint.h/c      - Blueprint visual editor
├── debugger.h/c       - Debugger component (GDB integration)
├── project.h/c        - Project management
├── ui.h               - UI framework
└── Makefile           - Build system
```

### Component Details

#### 1. Code Editor (`editor.h/c`)

**Features:**
- Multi-file buffer management (up to 32 files)
- Syntax highlighting for C/C++/Assembly/Python/JavaScript
- Line numbers and cursor navigation
- Auto-indentation
- Search and replace
- Language detection

**Key Structures:**
```c
editor_state_t      - Main editor state
file_buffer_t       - Individual file buffer
cursor_pos_t        - Cursor position
language_type_t     - Supported languages
```

**Operations:**
- `editor_open_file()` - Open file into buffer
- `editor_save_file()` - Save current buffer
- `editor_move_cursor()` - Navigate
- `editor_insert_char()` - Edit operations
- `editor_find()` - Search functionality

#### 2. Blueprint Visual Editor (`blueprint.h/c`)

**Features:**
- Node-based visual programming
- Multiple graph support
- Drag-and-drop node placement
- Connection management
- Grid snapping
- Minimap navigation
- Code generation to C

**Key Structures:**
```c
blueprint_editor_t      - Editor state
blueprint_graph_t       - Graph container
blueprint_node_t        - Visual node
blueprint_connection_t  - Node connection
```

**Node Types:**
- `NODE_FUNCTION` - Function call
- `NODE_VARIABLE` - Variable access
- `NODE_BRANCH` - Conditional branching
- `NODE_LOOP` - Loop structures
- `NODE_EVENT` - Event handlers
- `NODE_COMMENT` - Documentation
- `NODE_MACRO` - Macro expansion

**Pin Types:**
- `PIN_EXEC` - Execution flow
- `PIN_INT/FLOAT/STRING/BOOL` - Data types
- `PIN_POINTER` - Reference types
- `PIN_STRUCT` - Complex types
- `PIN_ARRAY` - Array types

**Operations:**
- `blueprint_create_graph()` - Create new graph
- `blueprint_add_node()` - Add node to graph
- `blueprint_connect()` - Connect nodes
- `blueprint_compile_to_c()` - Generate C code
- `blueprint_validate()` - Validate graph
- `blueprint_load/save()` - File I/O

#### 3. Debugger (`debugger.h/c`)

**Features:**
- GDB MI interface integration
- Breakpoints (normal, conditional, temporary, hardware)
- Watchpoints (read/write)
- Stack frame inspection
- Variable inspection
- Expression evaluation
- Multi-thread support
- Memory inspection
- Disassembly view

**Key Structures:**
```c
debugger_state_t    - Debugger state
breakpoint_t        - Breakpoint definition
watchpoint_t        - Watchpoint definition
stack_frame_t       - Stack frame
variable_t          - Variable value
thread_info_t       - Thread information
```

**Debugger States:**
- `DBG_IDLE` - Not debugging
- `DBG_RUNNING` - Target running
- `DBG_PAUSED` - Target paused
- `DBG_STEP` - Single-stepping
- `DBG_STOPPED` - Target terminated
- `DBG_ERROR` - Error state

**Operations:**
- `debugger_launch()` - Start debugging session
- `debugger_attach()` - Attach to running process
- `debugger_continue/pause()` - Execution control
- `debugger_step_over/into/out()` - Single-stepping
- `debugger_add_breakpoint()` - Breakpoint management
- `debugger_evaluate()` - Expression evaluation
- `debugger_read_memory()` - Memory inspection

#### 4. Project Manager (`project.h/c`)

**Features:**
- Project file management
- Build system integration
- Multi-target support
- Dependency resolution
- Git integration
- Build configuration

**Key Structures:**
```c
project_info_t      - Project state
build_target_t      - Build target
source_file_t       - Source file info
```

**Project Types:**
- `PROJ_C_EXECUTABLE` - C executable
- `PROJ_CPP_EXECUTABLE` - C++ executable
- `PROJ_C_LIBRARY` - C library
- `PROJ_CPP_LIBRARY` - C++ library
- `PROJ_KERNEL_MODULE` - Kernel module
- `PROJ_BOOTLOADER` - Bootloader
- `PROJ_CUSTOM` - Custom project

**Build Systems:**
- `BUILD_MAKE` - GNU Make
- `BUILD_CMAKE` - CMake
- `BUILD_CUSTOM` - Custom build system

**Operations:**
- `project_create/load/save()` - Project lifecycle
- `project_build/clean()` - Build operations
- `project_run()` - Execute target
- `project_git_*()` - Git operations

#### 5. UI Framework (`ui.h`)

**Features:**
- Panel management
- Menu system
- Dialogs
- Status messages
- Theming
- Input handling

**Key Structures:**
```c
ui_state_t      - UI state
panel_t         - UI panel
menu_t          - Menu container
menu_item_t     - Menu item
```

**Panel Types:**
- `PANEL_EDITOR` - Code editor panel
- `PANEL_BLUEPRINT` - Blueprint editor panel
- `PANEL_PROJECT` - Project explorer
- `PANEL_CONSOLE` - Build output console
- `PANEL_DEBUGGER` - Debugger controls
- `PANEL_VARIABLES` - Variable inspector
- `PANEL_CALLSTACK` - Call stack view
- `PANEL_BREAKPOINTS` - Breakpoint list
- `PANEL_OUTPUT` - General output
- `PANEL_TERMINAL` - Terminal emulator

## Building

### Prerequisites

- GCC or Clang compiler
- Make build system
- Standard C library

### Build Commands

```bash
# Build the IDE
make

# Clean build artifacts
make clean

# Install to bin directory
make install

# Run the IDE
make run

# Build with debug symbols
make debug

# Show help
make help
```

### Build Output

- Binary: `autoos-ide`
- Install location: `../bin/autoos-ide`

## Usage

### Basic Usage

```bash
# Start IDE without project
./autoos-ide

# Open existing project
./autoos-ide /path/to/project

# Open with specific file
./autoos-ide /path/to/file.c
```

### Keyboard Shortcuts

#### File Operations
- `Ctrl+N` - New File
- `Ctrl+O` - Open File
- `Ctrl+S` - Save File
- `Ctrl+W` - Close File
- `Ctrl+Q` - Quit IDE

#### Editing
- `Ctrl+Z` - Undo
- `Ctrl+Y` - Redo
- `Ctrl+F` - Find
- `Ctrl+H` - Replace
- `Ctrl+G` - Go to Line

#### Build Operations
- `Ctrl+B` - Build Project
- `Ctrl+Shift+B` - Rebuild All
- `Ctrl+Shift+C` - Clean

#### Debugging
- `F5` - Start Debugging / Continue
- `Shift+F5` - Stop Debugging
- `F9` - Toggle Breakpoint
- `F10` - Step Over
- `F11` - Step Into
- `Shift+F11` - Step Out
- `Ctrl+Shift+F9` - Delete All Breakpoints

#### View
- `Ctrl+P` - Toggle Blueprint Editor
- `Ctrl+`` - Toggle Terminal
- `Ctrl+Shift+E` - Toggle Project Explorer
- `Ctrl+Shift+D` - Toggle Debug Panel

## Blueprint Editor Integration

The Blueprint Visual Editor allows visual programming with automatic C code generation.

### Creating a Blueprint

1. Open Blueprint Editor (`Ctrl+P`)
2. Add nodes by right-clicking canvas
3. Connect nodes by dragging between pins
4. Save blueprint (`.bp` file)
5. Generate C code: Build → Compile Blueprint

### Blueprint to C Compilation

The blueprint compiler performs:

1. **Graph Validation** - Check for errors
2. **Topological Sort** - Determine execution order
3. **Code Generation** - Generate C functions
4. **Connection Translation** - Wire data flow
5. **Output** - Write `.c` and `.h` files

### Example Blueprint

```
[Event] OnStart
   |
   v (exec)
[Function] printf("Hello, AutomationOS!")
   |
   v (exec)
[Branch] if (condition)
   |                    |
   v (true)            v (false)
[SetVariable] x=1     [SetVariable] x=0
```

Generates:

```c
void blueprint_OnStart(void) {
    printf("Hello, AutomationOS!\n");
    
    if (condition) {
        x = 1;
    } else {
        x = 0;
    }
}
```

## Project Structure

### Project File (`.autoproj`)

JSON format:

```json
{
  "name": "MyProject",
  "type": "c_executable",
  "build_system": "make",
  "compiler": "gcc",
  "targets": [
    {
      "name": "main",
      "output": "myapp",
      "sources": ["main.c", "lib.c"],
      "includes": ["include/"],
      "libraries": ["pthread"],
      "cflags": "-Wall -O2",
      "ldflags": "-lm"
    }
  ],
  "dependencies": []
}
```

## Debugger Integration

### Starting Debug Session

1. Set breakpoints in editor (`F9`)
2. Start debugging (`F5`)
3. IDE launches GDB with target
4. Execution pauses at breakpoints

### Debug Views

- **Variables Panel** - Local/global variables
- **Call Stack Panel** - Stack frames
- **Breakpoints Panel** - Manage breakpoints
- **Console Panel** - GDB output
- **Memory Panel** - Memory inspection
- **Disassembly Panel** - Assembly view

### Watch Expressions

Add watch expressions to monitor values:

```
variable_name
*pointer
array[index]
struct.member
function_call()
```

## Extending the IDE

### Adding Language Support

1. Add language enum to `editor.h`
2. Implement syntax highlighting rules
3. Update `editor_detect_language()`
4. Add keywords and patterns

### Creating Custom Node Types

1. Add node type to `blueprint.h`
2. Implement code generation in `blueprint_compile_to_c()`
3. Define default pins and behavior
4. Register in blueprint editor

### Custom Build Systems

1. Implement build system interface
2. Parse build configuration
3. Generate build commands
4. Handle output parsing

## Status

**Current Implementation Status:**

- ✅ Core architecture defined
- ✅ Component headers complete
- ✅ Stub implementations created
- ✅ Build system configured
- ⚠️ Full implementations in progress
- ⚠️ UI rendering not implemented
- ⚠️ GDB integration not connected
- ⚠️ File I/O not implemented

**Next Steps:**

1. Implement file I/O for editor
2. Connect GDB MI interface
3. Implement UI rendering (ncurses or GUI)
4. Complete blueprint code generator
5. Add project file format
6. Implement syntax highlighting
7. Add terminal emulator

## License

Part of AutomationOS project.

## Contributing

This IDE is designed specifically for AutomationOS development. Contributions should focus on:

- Kernel-aware debugging features
- AutomationOS-specific build configurations
- Blueprint nodes for OS operations
- Integration with AutomationOS APIs
