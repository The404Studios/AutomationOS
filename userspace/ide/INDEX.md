# AutomationOS IDE - Complete Index

## Quick Navigation

### Getting Started
1. [README.md](README.md) - Start here for overview and usage
2. [BUILD_SUMMARY.md](BUILD_SUMMARY.md) - Build instructions and status
3. [Makefile](Makefile) or [build.sh](build.sh) - Build the IDE

### Understanding the System
1. [ARCHITECTURE.md](ARCHITECTURE.md) - System design and technical details
2. [DIAGRAMS.txt](DIAGRAMS.txt) - Visual diagrams and flow charts
3. [FEATURES.md](FEATURES.md) - Feature checklist and progress

### Implementation Files
1. [ide.h](ide.h) + [ide_main.c](ide_main.c) - Core IDE
2. [editor.h](editor.h) + [editor.c](editor.c) - Code editor
3. [blueprint.h](blueprint.h) + [blueprint.c](blueprint.c) - Visual editor
4. [debugger.h](debugger.h) + [debugger.c](debugger.c) - Debugger
5. [project.h](project.h) + [project.c](project.c) - Project manager
6. [ui.h](ui.h) - UI framework

---

## File Reference Guide

### 📘 Documentation (What to read)

| File | Size | Purpose | Read When |
|------|------|---------|-----------|
| **README.md** | 17.5 KB | Main documentation | Starting out |
| **ARCHITECTURE.md** | 13.8 KB | System design | Implementing features |
| **FEATURES.md** | 11.2 KB | Feature status | Planning work |
| **BUILD_SUMMARY.md** | 14.5 KB | Build info & status | Building the IDE |
| **DIAGRAMS.txt** | 8.7 KB | Visual references | Understanding flow |
| **INDEX.md** | This file | Navigation guide | Finding things |

**Total Documentation: ~80 KB / ~2,100 lines**

### 💻 Header Files (API Definitions)

| File | Lines | Functions | Structures | Purpose |
|------|-------|-----------|------------|---------|
| **ide.h** | 42 | 8 | 1 | Core IDE context |
| **editor.h** | 92 | 24 | 4 | Code editing |
| **blueprint.h** | 138 | 23 | 7 | Visual programming |
| **debugger.h** | 159 | 25 | 8 | Debugging |
| **project.h** | 112 | 15 | 5 | Project management |
| **ui.h** | 96 | 20 | 6 | User interface |

**Total Headers: 639 lines / 115 functions / 31 structures**

### 🔧 Implementation Files (Code)

| File | Lines | Status | Completion |
|------|-------|--------|------------|
| **ide_main.c** | 245 | Stub | 40% |
| **editor.c** | 110 | Stub | 30% |
| **blueprint.c** | 155 | Stub | 35% |
| **debugger.c** | 225 | Stub | 35% |
| **project.c** | 245 | Stub | 40% |

**Total Implementation: 980 lines / ~40% complete**

### 🛠 Build Files

| File | Lines | Purpose |
|------|-------|---------|
| **Makefile** | 70 | GNU Make build system |
| **build.sh** | 62 | Standalone build script |

**Total Build System: 132 lines**

---

## Topic Index

### By Component

#### Code Editor
- **Overview:** README.md § Code Editor
- **API:** editor.h
- **Implementation:** editor.c
- **Architecture:** ARCHITECTURE.md § Editor Component
- **Features:** FEATURES.md § Code Editor
- **Diagrams:** DIAGRAMS.txt § Editor Data Flow

#### Blueprint Visual Editor
- **Overview:** README.md § Blueprint Editor Integration
- **API:** blueprint.h
- **Implementation:** blueprint.c
- **Architecture:** ARCHITECTURE.md § Blueprint Editor
- **Features:** FEATURES.md § Blueprint Editor
- **Diagrams:** DIAGRAMS.txt § Blueprint Editor Structure

#### Debugger
- **Overview:** README.md § Debugger Integration
- **API:** debugger.h
- **Implementation:** debugger.c
- **Architecture:** ARCHITECTURE.md § Debugger Component
- **Features:** FEATURES.md § Debugger
- **Diagrams:** DIAGRAMS.txt § Debugger State Machine

#### Project Manager
- **Overview:** README.md § Project Structure
- **API:** project.h
- **Implementation:** project.c
- **Architecture:** ARCHITECTURE.md § Project Manager
- **Features:** FEATURES.md § Project Manager
- **Diagrams:** DIAGRAMS.txt § Project Build Workflow

#### UI Framework
- **Overview:** README.md § Features
- **API:** ui.h
- **Architecture:** ARCHITECTURE.md § UI Framework
- **Features:** FEATURES.md § UI Features
- **Diagrams:** DIAGRAMS.txt § UI Panel Layout

---

## By Task

### I want to...

#### Build the IDE
→ [BUILD_SUMMARY.md](BUILD_SUMMARY.md) § Build Instructions  
→ [Makefile](Makefile) or [build.sh](build.sh)  
→ [README.md](README.md) § Building

#### Understand the architecture
→ [ARCHITECTURE.md](ARCHITECTURE.md)  
→ [DIAGRAMS.txt](DIAGRAMS.txt)  
→ [README.md](README.md) § Architecture

#### See what features are implemented
→ [FEATURES.md](FEATURES.md)  
→ [BUILD_SUMMARY.md](BUILD_SUMMARY.md) § Implementation Status

#### Implement a new feature
1. Read [ARCHITECTURE.md](ARCHITECTURE.md) for component design
2. Check [FEATURES.md](FEATURES.md) for status
3. Find API in relevant .h file
4. Implement in corresponding .c file
5. Update [FEATURES.md](FEATURES.md) checklist

#### Add a new node type (Blueprint)
→ [blueprint.h](blueprint.h) § Node Types  
→ [ARCHITECTURE.md](ARCHITECTURE.md) § Custom Node Types  
→ [README.md](README.md) § Blueprint Editor

#### Integrate with GDB
→ [debugger.h](debugger.h) § GDB Interface  
→ [ARCHITECTURE.md](ARCHITECTURE.md) § GDB MI Protocol  
→ [FEATURES.md](FEATURES.md) § Debugger

#### Add syntax highlighting
→ [editor.h](editor.h) § Syntax Highlighting  
→ [ARCHITECTURE.md](ARCHITECTURE.md) § Language Support Extension  
→ [FEATURES.md](FEATURES.md) § Syntax Highlighting

#### Create a project file format
→ [project.h](project.h) § Project Operations  
→ [ARCHITECTURE.md](ARCHITECTURE.md) § Project File (.autoproj)  
→ [README.md](README.md) § Project File

#### Understand memory layout
→ [ARCHITECTURE.md](ARCHITECTURE.md) § Memory Management  
→ [DIAGRAMS.txt](DIAGRAMS.txt) § Memory Layout  
→ [BUILD_SUMMARY.md](BUILD_SUMMARY.md) § Memory Footprint

---

## API Quick Reference

### Core IDE Functions
```c
ide_context_t* ide_init(const char *project_path);
void ide_run(ide_context_t *ctx);
void ide_shutdown(ide_context_t *ctx);
```
→ [ide.h](ide.h)

### Editor Operations
```c
int editor_open_file(editor_state_t *ed, const char *path);
int editor_save_file(editor_state_t *ed);
void editor_move_cursor(editor_state_t *ed, int dx, int dy);
void editor_insert_char(editor_state_t *ed, char c);
```
→ [editor.h](editor.h)

### Blueprint Operations
```c
blueprint_graph_t* blueprint_create_graph(const char *name);
blueprint_node_t* blueprint_add_node(blueprint_graph_t *graph, node_type_t type, int x, int y);
blueprint_connection_t* blueprint_connect(blueprint_graph_t *graph, ...);
int blueprint_compile_to_c(blueprint_graph_t *graph, const char *output_path);
```
→ [blueprint.h](blueprint.h)

### Debugger Operations
```c
int debugger_launch(debugger_state_t *dbg, const char *path, char **args);
int debugger_add_breakpoint(debugger_state_t *dbg, const char *file, int line);
int debugger_continue(debugger_state_t *dbg);
int debugger_step_over(debugger_state_t *dbg);
```
→ [debugger.h](debugger.h)

### Project Operations
```c
int project_create(project_info_t *proj, const char *path, project_type_t type);
int project_load(project_info_t *proj, const char *path);
int project_build(project_info_t *proj);
int project_run(project_info_t *proj);
```
→ [project.h](project.h)

---

## Data Structure Quick Reference

### Core Structures
- `ide_context_t` - Main IDE state (ide.h)
- `editor_state_t` - Editor state (editor.h)
- `blueprint_editor_t` - Blueprint editor state (blueprint.h)
- `debugger_state_t` - Debugger state (debugger.h)
- `project_info_t` - Project state (project.h)
- `ui_state_t` - UI state (ui.h)

### Editor Structures
- `file_buffer_t` - File contents (editor.h)
- `cursor_pos_t` - Cursor position (editor.h)
- `language_type_t` - Language enum (editor.h)

### Blueprint Structures
- `blueprint_graph_t` - Node graph (blueprint.h)
- `blueprint_node_t` - Visual node (blueprint.h)
- `blueprint_connection_t` - Node connection (blueprint.h)
- `node_type_t` - Node type enum (blueprint.h)
- `pin_type_t` - Pin type enum (blueprint.h)
- `pin_t` - Node pin (blueprint.h)

### Debugger Structures
- `breakpoint_t` - Breakpoint (debugger.h)
- `watchpoint_t` - Watchpoint (debugger.h)
- `stack_frame_t` - Stack frame (debugger.h)
- `variable_t` - Variable value (debugger.h)
- `thread_info_t` - Thread info (debugger.h)

### Project Structures
- `build_target_t` - Build target (project.h)
- `source_file_t` - Source file (project.h)
- `project_type_t` - Project type enum (project.h)
- `build_system_t` - Build system enum (project.h)

### UI Structures
- `panel_t` - UI panel (ui.h)
- `menu_t` - Menu container (ui.h)
- `menu_item_t` - Menu item (ui.h)
- `panel_type_t` - Panel type enum (ui.h)

---

## Constant Reference

### Resource Limits
```c
MAX_OPEN_FILES       = 32      // editor.h
MAX_LINES            = 10000   // editor.h
MAX_LINE_LENGTH      = 1024    // editor.h
MAX_NODES            = 512     // blueprint.h
MAX_CONNECTIONS      = 1024    // blueprint.h
MAX_BREAKPOINTS      = 256     // debugger.h
MAX_WATCHPOINTS      = 64      // debugger.h
MAX_STACK_FRAMES     = 128     // debugger.h
MAX_VARIABLES        = 512     // debugger.h
MAX_SOURCE_FILES     = 1024    // project.h
MAX_BUILD_TARGETS    = 32      // project.h
MAX_PANELS           = 16      // ui.h
MAX_MENU_ITEMS       = 64      // ui.h
```

---

## Statistics Summary

### Code Statistics
```
Total Files:          18
  Headers:            6  (639 lines)
  Implementation:     6  (980 lines)
  Documentation:      6  (2,100+ lines)

Total Functions:      115
Total Structures:     31
Total Enums:          12

Lines of Code:        1,619
Lines of Docs:        2,100+
Total Lines:          ~3,800
```

### Implementation Status
```
Architecture:         100% ████████████████████
API Design:           91%  ██████████████████░░
Implementation:       39%  ████████░░░░░░░░░░░░
Testing:              0%   ░░░░░░░░░░░░░░░░░░░░
Documentation:        100% ████████████████████

Overall Progress:     45%  █████████░░░░░░░░░░░
```

### Component Status
```
Editor:               40%  ████████░░░░░░░░░░░░
Blueprint:            45%  █████████░░░░░░░░░░░
Debugger:             43%  █████████░░░░░░░░░░░
Project:              43%  █████████░░░░░░░░░░░
UI:                   37%  ███████░░░░░░░░░░░░░
Build System:         75%  ███████████████░░░░░
```

---

## Keyboard Shortcuts Reference

Quick reference (full list in [README.md](README.md)):

```
File:      Ctrl+N/O/S/W/Q
Edit:      Ctrl+Z/Y/F/H/G
Build:     Ctrl+B, Ctrl+Shift+B/C
Debug:     F5/F9/F10/F11, Shift+F5/F11
View:      Ctrl+P/`/Shift+E/D
```

---

## Next Steps Reference

### Immediate Tasks (Week 1-2)
→ [BUILD_SUMMARY.md](BUILD_SUMMARY.md) § Next Steps § Immediate  
→ [FEATURES.md](FEATURES.md) § Milestone 1

### Short-term (Month 1)
→ [BUILD_SUMMARY.md](BUILD_SUMMARY.md) § Next Steps § Short-term  
→ [FEATURES.md](FEATURES.md) § Milestones 1-2

### Long-term Roadmap
→ [ARCHITECTURE.md](ARCHITECTURE.md) § Roadmap  
→ [FEATURES.md](FEATURES.md) § Next Milestones

---

## Search Index

### Keywords → Files

**API Definition** → .h files  
**Implementation** → .c files  
**Architecture** → ARCHITECTURE.md, DIAGRAMS.txt  
**Features** → FEATURES.md, README.md  
**Building** → BUILD_SUMMARY.md, Makefile, build.sh  
**Status** → FEATURES.md, BUILD_SUMMARY.md  

**Breakpoint** → debugger.h, debugger.c  
**Node** → blueprint.h, blueprint.c  
**File Buffer** → editor.h, editor.c  
**Build Target** → project.h, project.c  
**Panel** → ui.h  

**GDB** → debugger.h, ARCHITECTURE.md  
**Compiler** → project.h, ARCHITECTURE.md  
**Git** → project.h, project.c  
**Syntax** → editor.h, editor.c  

---

## Version History

| Version | Date | Status | Description |
|---------|------|--------|-------------|
| 1.0.0-skeleton | 2026-05-26 | Current | Initial skeleton with complete architecture |

---

## Contributing Guide

1. **Choose a component** from [FEATURES.md](FEATURES.md)
2. **Read the architecture** in [ARCHITECTURE.md](ARCHITECTURE.md)
3. **Review the API** in corresponding .h file
4. **Implement in** corresponding .c file
5. **Update checklist** in [FEATURES.md](FEATURES.md)
6. **Test thoroughly**
7. **Document changes** in README.md if user-facing

---

## Support

- **Issue:** Check [FEATURES.md](FEATURES.md) for known limitations
- **Build:** See [BUILD_SUMMARY.md](BUILD_SUMMARY.md)
- **Design:** See [ARCHITECTURE.md](ARCHITECTURE.md)
- **Usage:** See [README.md](README.md)

---

**Last Updated:** 2026-05-26  
**IDE Version:** 1.0.0-skeleton  
**Document Maintainer:** AutomationOS IDE Project
