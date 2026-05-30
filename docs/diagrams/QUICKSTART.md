# Diagram Quick Start Guide

Get up and running with AutomationOS diagrams in 5 minutes.

---

## Quick Install

### Linux (Ubuntu/Debian)
```bash
sudo apt update && sudo apt install -y graphviz nodejs npm
sudo npm install -g @mermaid-js/mermaid-cli
```

### macOS (Homebrew)
```bash
brew install graphviz node
npm install -g @mermaid-js/mermaid-cli
```

### Windows (PowerShell as Administrator)
```powershell
choco install graphviz nodejs -y
# Restart terminal
npm install -g @mermaid-js/mermaid-cli
```

---

## Quick Generate

```bash
# From project root
cd /c/Users/wilde/Desktop/Kernel

# Generate all diagrams (takes ~10 seconds)
bash scripts/generate-diagrams.sh

# Output files will be in docs/diagrams/*.svg
```

---

## Quick View

### View in Browser
```bash
# Open any diagram in browser
firefox docs/diagrams/system_architecture.svg
# or
google-chrome docs/diagrams/system_architecture.svg
# or
open docs/diagrams/system_architecture.svg  # macOS
```

### View in Terminal (ASCII versions)
```bash
# View text-based diagrams
cat docs/diagrams/ASCII_DIAGRAMS.md | less
```

---

## Quick Edit

### Edit Source File
```bash
# Edit Graphviz diagram
nano docs/diagrams/src/system_architecture.dot

# Edit Mermaid diagram
nano docs/diagrams/src/seq_syscall.mmd
```

### Regenerate After Edit
```bash
bash scripts/generate-diagrams.sh
```

---

## Quick Reference

### Available Diagrams

| Diagram | File | Purpose |
|---------|------|---------|
| System Architecture | `system_architecture.svg` | Overall system |
| Memory Layout | `memory_layout.svg` | Memory organization |
| Boot Sequence | `boot_sequence.svg` | Boot process |
| Syscall Sequence | `seq_syscall.svg` | System call flow |
| Context Switch | `seq_context_switch.svg` | Process switching |
| Page Fault | `seq_page_fault.svg` | Page fault handling |
| Interrupt | `seq_interrupt.svg` | Hardware interrupts |
| Process States | `fsm_process.svg` | Process lifecycle |
| Scheduler States | `fsm_scheduler.svg` | Scheduler algorithm |
| Page States | `fsm_memory_page.svg` | Memory page lifecycle |
| Kernel Components | `component_kernel.svg` | Kernel subsystems |
| Memory Components | `component_memory.svg` | Memory subsystem |
| Driver Components | `component_drivers.svg` | Device drivers |
| Syscall Data Flow | `dataflow_syscall.svg` | Data transformations |

### Documentation Files

- **[README.md](README.md)** - Main index with previews
- **[INSTALLATION.md](INSTALLATION.md)** - Detailed installation guide
- **[ASCII_DIAGRAMS.md](ASCII_DIAGRAMS.md)** - Text-based diagrams
- **[DELIVERABLES.md](DELIVERABLES.md)** - Project summary

---

## Common Tasks

### Verify Tools Installed
```bash
bash scripts/check-diagram-tools.sh
```

### Generate Single Diagram
```bash
# Graphviz
dot -Tsvg docs/diagrams/src/system_architecture.dot -o docs/diagrams/system_architecture.svg

# Mermaid
mmdc -i docs/diagrams/src/seq_syscall.mmd -o docs/diagrams/seq_syscall.svg -b transparent
```

### View Diagram Source
```bash
cat docs/diagrams/src/system_architecture.dot
```

### Embed in Markdown
```markdown
![System Architecture](docs/diagrams/system_architecture.svg)
```

---

## Troubleshooting

### Tool Not Found
```bash
# Check if installed
which dot
which mmdc

# If missing, see INSTALLATION.md
```

### Generation Failed
- Check syntax in source file
- Try online editor for debugging:
  - Graphviz: https://dreampuf.github.io/GraphvizOnline/
  - Mermaid: https://mermaid.live/

### Can't View SVG
- Open in modern browser (Chrome, Firefox, Safari, Edge)
- Or convert to PNG: `dot -Tpng input.dot -o output.png`

---

## Next Steps

1. ✅ Install tools (see [INSTALLATION.md](INSTALLATION.md))
2. ✅ Generate diagrams (`bash scripts/generate-diagrams.sh`)
3. ✅ View diagrams (open `*.svg` files in browser)
4. 📖 Read [README.md](README.md) for detailed documentation
5. 🎨 Edit source files in `src/` directory
6. 🔄 Regenerate after changes

---

**Questions?** See [README.md](README.md) for comprehensive documentation.
