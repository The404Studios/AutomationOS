# AutomationOS IDE - Architecture Documentation

## System Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                     AutomationOS IDE                         │
│                      (ide_main.c)                            │
└─────────────────────────────────────────────────────────────┘
                              │
                              │ Main Event Loop
                              │
        ┌─────────────────────┼─────────────────────┐
        │                     │                     │
        ▼                     ▼                     ▼
┌──────────────┐      ┌──────────────┐     ┌──────────────┐
│   UI Layer   │      │ Input Handler│     │Event Manager │
│   (ui.h)     │      │              │     │              │
└──────────────┘      └──────────────┘     └──────────────┘
        │                     │                     │
        └─────────────────────┴─────────────────────┘
                              │
        ┌─────────────────────┼─────────────────────┐
        │                     │                     │
        ▼                     ▼                     ▼
┌──────────────┐      ┌──────────────┐     ┌──────────────┐
│    Editor    │      │  Blueprint   │     │   Debugger   │
│  (editor.h)  │      │(blueprint.h) │     │(debugger.h)  │
└──────────────┘      └──────────────┘     └──────────────┘
        │                     │                     │
        └─────────────────────┴─────────────────────┘
                              │
                              ▼
                      ┌──────────────┐
                      │   Project    │
                      │ (project.h)  │
                      └──────────────┘
                              │
        ┌─────────────────────┼─────────────────────┐
        │                     │                     │
        ▼                     ▼                     ▼
┌──────────────┐      ┌──────────────┐     ┌──────────────┐
│  Compiler    │      │     Git      │     │  Terminal    │
│   (GCC)      │      │              │     │              │
└──────────────┘      └──────────────┘     └──────────────┘
```

## Data Flow

### 1. File Editing Flow

```
User Input → UI Handler → Editor Component → File Buffer
                                    ↓
                            Syntax Highlighter
                                    ↓
                              Render to UI
```

### 2. Build Flow

```
User Trigger → Project Manager → Build System Generator
                      ↓
                  Compiler (GCC)
                      ↓
              Output Parser → Error Reporter → UI
```

### 3. Debug Flow

```
User Action → Debugger → GDB MI Interface → Target Process
                ↓
        State Update → UI Panels
                ↓
        Variables/Stack/Breakpoints
```

### 4. Blueprint Compilation Flow

```
Blueprint Graph → Validator → Topological Sort
                                    ↓
                            Code Generator
                                    ↓
                        C Source Files → Compiler
```

## Component Interaction Matrix

```
             │ Editor │ Blueprint │ Debugger │ Project │ UI
─────────────┼────────┼───────────┼──────────┼─────────┼────
Editor       │   -    │    R      │    R     │   RW    │ RW
Blueprint    │   W    │    -      │    -     │   RW    │ RW
Debugger     │   R    │    -      │    -     │   R     │ RW
Project      │   RW   │    RW     │    R     │   -     │ RW
UI           │   RW   │    RW     │    RW    │   RW    │  -

Legend: R = Read, W = Write, RW = Read/Write
```

## Memory Management

### IDE Context Lifecycle

```c
1. Initialization:
   ide_init()
   ├─ Allocate ide_context_t
   ├─ ide_editor_init()
   ├─ ide_blueprint_init()
   ├─ ide_debugger_init()
   └─ ide_project_init()

2. Runtime:
   ide_run()
   └─ Main event loop (processes events)

3. Shutdown:
   ide_shutdown()
   ├─ ide_editor_cleanup()
   ├─ ide_blueprint_cleanup()
   ├─ ide_debugger_cleanup()
   ├─ ide_project_cleanup()
   └─ Free ide_context_t
```

### Resource Limits

```c
MAX_OPEN_FILES       = 32      // Editor buffers
MAX_LINES            = 10000   // Lines per file
MAX_LINE_LENGTH      = 1024    // Characters per line
MAX_NODES            = 512     // Blueprint nodes
MAX_CONNECTIONS      = 1024    // Blueprint connections
MAX_BREAKPOINTS      = 256     // Debug breakpoints
MAX_WATCHPOINTS      = 64      // Debug watchpoints
MAX_STACK_FRAMES     = 128     // Call stack depth
MAX_VARIABLES        = 512     // Watched variables
MAX_SOURCE_FILES     = 1024    // Project files
MAX_BUILD_TARGETS    = 32      // Build targets
```

## Threading Model

### Current Implementation (Single-threaded)

```
Main Thread:
  ├─ UI Rendering
  ├─ Event Processing
  ├─ File I/O
  └─ Debugger Communication
```

### Future Implementation (Multi-threaded)

```
UI Thread:
  ├─ Event Loop
  └─ Rendering

Worker Thread Pool:
  ├─ Syntax Highlighting
  ├─ File I/O
  ├─ Build Execution
  └─ Code Analysis

Debugger Thread:
  ├─ GDB Communication
  └─ Event Processing
```

## File Format Specifications

### Project File (`.autoproj`)

```json
{
  "version": "1.0",
  "name": "string",
  "type": "c_executable|cpp_executable|library|kernel_module",
  "build_system": "make|cmake|custom",
  "compiler": "gcc|clang",
  "debugger": "gdb|lldb",
  "root_dir": "path",
  "build_dir": "path",
  "output_dir": "path",
  "targets": [
    {
      "name": "string",
      "output": "string",
      "sources": ["file1.c", "file2.c"],
      "includes": ["include/"],
      "libraries": ["pthread", "m"],
      "cflags": "-Wall -O2",
      "ldflags": "-lm"
    }
  ],
  "dependencies": ["lib1", "lib2"],
  "git": {
    "enabled": true,
    "branch": "main"
  }
}
```

### Blueprint File (`.bp`)

```json
{
  "version": "1.0",
  "name": "string",
  "nodes": [
    {
      "id": 1,
      "type": "function|variable|branch|loop|event",
      "name": "string",
      "x": 100,
      "y": 200,
      "inputs": [
        {"name": "string", "type": "exec|int|float|...", "default": null}
      ],
      "outputs": [
        {"name": "string", "type": "exec|int|float|..."}
      ],
      "properties": {}
    }
  ],
  "connections": [
    {
      "id": 1,
      "source_node": 1,
      "source_pin": 0,
      "target_node": 2,
      "target_pin": 0
    }
  ],
  "viewport": {"x": 0, "y": 0, "zoom": 1.0}
}
```

## External Interface Specifications

### GDB MI Protocol

Commands sent to GDB:

```
-break-insert file.c:123        # Set breakpoint
-break-delete 1                 # Remove breakpoint
-exec-continue                  # Continue execution
-exec-next                      # Step over
-exec-step                      # Step into
-exec-finish                    # Step out
-stack-list-frames              # Get stack trace
-stack-list-variables           # Get local variables
-data-evaluate-expression expr  # Evaluate expression
```

Responses parsed:

```
*stopped,reason="breakpoint-hit",bkptno="1"
^done,frame={level="0",addr="0x...",func="main"}
^done,variables=[{name="x",value="42"}]
```

### Compiler Integration

Build command generation:

```bash
# For Make projects
make -C build_dir -j4

# For CMake projects
cmake -B build_dir -S source_dir
cmake --build build_dir

# Direct compilation
gcc -Wall -O2 -Iinclude src/*.c -o output -lm
```

Error parsing (GCC format):

```
file.c:123:45: error: message
file.c:123:45: warning: message
file.c:123:45: note: message
```

## Performance Considerations

### Optimization Strategies

1. **Lazy Loading**
   - Load files on-demand
   - Defer syntax highlighting until visible
   - Load project files incrementally

2. **Caching**
   - Cache parsed syntax trees
   - Cache build results
   - Cache git status

3. **Incremental Updates**
   - Only re-highlight changed lines
   - Incremental compilation
   - Partial UI redraws

4. **Async Operations**
   - Background file scanning
   - Async build execution
   - Non-blocking git operations

### Memory Optimization

1. **Buffer Management**
   - Unload unused file buffers
   - Limit maximum open files
   - Use memory-mapped files for large files

2. **Blueprint Graphs**
   - Node pooling
   - Connection compression
   - Viewport culling (only render visible nodes)

## Security Considerations

### Input Validation

- Sanitize file paths
- Validate blueprint node connections
- Limit recursion depth in code generation
- Bounds checking on all arrays

### Process Isolation

- Run compiler in separate process
- Sandbox debugger communication
- Isolate target process

### File System Security

- Validate project paths
- Restrict file access to project directory
- Check file permissions before write

## Extension Points

### Plugin Architecture (Future)

```c
typedef struct {
    const char *name;
    const char *version;
    int (*init)(ide_context_t *ctx);
    void (*cleanup)(void);
    void (*on_file_open)(const char *path);
    void (*on_file_save)(const char *path);
    void (*on_build)(void);
} ide_plugin_t;
```

### Custom Node Types (Blueprint)

```c
typedef struct {
    const char *type_name;
    void (*create)(blueprint_node_t *node);
    int (*validate)(blueprint_node_t *node);
    void (*generate_code)(blueprint_node_t *node, FILE *out);
} blueprint_node_type_t;
```

### Language Support Extension

```c
typedef struct {
    const char *name;
    const char **extensions;
    void (*highlight)(const char *line, char *output);
    void (*indent)(const char *line, int *indent_level);
    void (*autocomplete)(const char *context, char ***suggestions);
} language_support_t;
```

## State Persistence

### Session Save/Restore

Save on exit:
- Open files and cursor positions
- Breakpoint locations
- Window layout
- Recent projects

Restore on startup:
- Reopen previous session
- Restore editor state
- Reinitialize debugger

### Configuration Files

```
~/.autoos-ide/
├── config.json          # Global settings
├── sessions/
│   └── last.session     # Last session state
├── themes/
│   ├── default.theme    # Default theme
│   └── dark.theme       # Dark theme
└── keybindings.json     # Custom key bindings
```

## Error Handling

### Error Categories

1. **Fatal Errors** - Terminate IDE
   - Out of memory
   - Critical file system errors
   - Corrupted project files

2. **Recoverable Errors** - Show error, continue
   - File not found
   - Build failures
   - Debugger disconnection

3. **Warnings** - Notify user
   - Unsaved changes
   - Deprecated features
   - Performance issues

### Error Reporting

```c
typedef enum {
    ERR_NONE = 0,
    ERR_NOMEM,
    ERR_FILE_NOT_FOUND,
    ERR_PERMISSION_DENIED,
    ERR_INVALID_PROJECT,
    ERR_BUILD_FAILED,
    ERR_DEBUGGER_ERROR
} ide_error_t;

void ide_error(ide_error_t code, const char *message);
void ide_warning(const char *message);
void ide_info(const char *message);
```

## Testing Strategy

### Unit Tests

- Test each component in isolation
- Mock dependencies
- Cover edge cases

### Integration Tests

- Test component interactions
- Test file I/O operations
- Test build system integration

### System Tests

- End-to-end workflows
- Performance benchmarks
- Memory leak detection

## Build Configuration

### Debug Build

```bash
make debug
# Enables: -g -DDEBUG
# Features: Assertions, debug logging, memory tracking
```

### Release Build

```bash
make
# Enables: -O2 -DNDEBUG
# Features: Optimizations, stripped symbols
```

### Profile Build

```bash
make profile
# Enables: -O2 -g -pg
# Features: Profiling instrumentation
```

## Dependencies

### Required

- GCC or Clang (compiler)
- Make (build system)
- GDB (debugger)
- Standard C library

### Optional

- Git (version control)
- CMake (alternative build system)
- ncurses (terminal UI)
- SDL2 (graphical UI)

## Roadmap

### Version 1.0 (Current - Skeleton)

- ✅ Core architecture
- ✅ Component interfaces
- ✅ Stub implementations
- ✅ Build system

### Version 1.1 (Basic Editor)

- ⚠️ File I/O implementation
- ⚠️ Basic text editing
- ⚠️ Syntax highlighting
- ⚠️ Project file support

### Version 1.2 (Debugger)

- ⚠️ GDB MI integration
- ⚠️ Breakpoint management
- ⚠️ Variable inspection
- ⚠️ Stack traces

### Version 1.3 (Blueprint Editor)

- ⚠️ Node rendering
- ⚠️ Connection management
- ⚠️ Code generation
- ⚠️ Blueprint validation

### Version 2.0 (Full IDE)

- ⚠️ Complete UI implementation
- ⚠️ Terminal emulator
- ⚠️ Git integration
- ⚠️ Plugin system
