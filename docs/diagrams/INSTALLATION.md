# Diagram Generation Tools Installation

This guide covers installing the required tools to generate diagrams from source files.

## Required Tools

1. **Graphviz** - For .dot files (architecture, component, data flow diagrams)
2. **Mermaid CLI** - For .mmd files (sequence, state machine diagrams)
3. **PlantUML** (Optional) - For .puml files (UML diagrams)

---

## Windows Installation

### Method 1: Using Chocolatey (Recommended)

```powershell
# Install Chocolatey if not already installed
# Run as Administrator in PowerShell
Set-ExecutionPolicy Bypass -Scope Process -Force
[System.Net.ServicePointManager]::SecurityProtocol = [System.Net.ServicePointManager]::SecurityProtocol -bor 3072
iex ((New-Object System.Net.WebClient).DownloadString('https://community.chocolatey.org/install.ps1'))

# Install Graphviz
choco install graphviz -y

# Install Node.js (required for Mermaid CLI)
choco install nodejs -y

# Restart terminal, then install Mermaid CLI
npm install -g @mermaid-js/mermaid-cli

# Optional: Install PlantUML
choco install plantuml -y
```

### Method 2: Manual Installation

**Graphviz:**
1. Download from https://graphviz.org/download/
2. Run the installer (graphviz-XXX-win64.exe)
3. Add to PATH: `C:\Program Files\Graphviz\bin`

**Node.js (for Mermaid):**
1. Download from https://nodejs.org/
2. Run the installer (node-vXX.XX.X-x64.msi)
3. Restart terminal
4. Install Mermaid CLI: `npm install -g @mermaid-js/mermaid-cli`

**PlantUML (Optional):**
1. Requires Java: Download from https://www.java.com/
2. Download PlantUML jar: https://plantuml.com/download
3. Create script `plantuml.bat`:
   ```batch
   @echo off
   java -jar "C:\path\to\plantuml.jar" %*
   ```

### Method 3: Using winget

```powershell
# Install Graphviz
winget install Graphviz.Graphviz

# Install Node.js
winget install OpenJS.NodeJS

# Restart terminal, then install Mermaid CLI
npm install -g @mermaid-js/mermaid-cli
```

---

## Linux Installation

### Ubuntu/Debian

```bash
# Install Graphviz
sudo apt update
sudo apt install graphviz -y

# Install Node.js and npm
sudo apt install nodejs npm -y

# Install Mermaid CLI
sudo npm install -g @mermaid-js/mermaid-cli

# Optional: Install PlantUML
sudo apt install plantuml -y
```

### Arch Linux

```bash
# Install Graphviz
sudo pacman -S graphviz

# Install Node.js and npm
sudo pacman -S nodejs npm

# Install Mermaid CLI
sudo npm install -g @mermaid-js/mermaid-cli

# Optional: Install PlantUML
sudo pacman -S plantuml
```

### Fedora/RHEL

```bash
# Install Graphviz
sudo dnf install graphviz -y

# Install Node.js and npm
sudo dnf install nodejs npm -y

# Install Mermaid CLI
sudo npm install -g @mermaid-js/mermaid-cli

# Optional: Install PlantUML
sudo dnf install plantuml -y
```

---

## macOS Installation

### Using Homebrew (Recommended)

```bash
# Install Graphviz
brew install graphviz

# Install Node.js
brew install node

# Install Mermaid CLI
npm install -g @mermaid-js/mermaid-cli

# Optional: Install PlantUML
brew install plantuml
```

---

## Verification

After installation, verify the tools are available:

```bash
# Check Graphviz
dot -V
# Expected output: dot - graphviz version X.X.X

# Check Node.js
node --version
# Expected output: vXX.XX.X

# Check Mermaid CLI
mmdc --version
# Expected output: X.X.X

# Check PlantUML (optional)
plantuml -version
# Expected output: PlantUML version X.XXXX
```

---

## Generating Diagrams

Once tools are installed, generate all diagrams:

```bash
# From project root
cd /c/Users/wilde/Desktop/Kernel

# Run the diagram generation script
bash scripts/generate-diagrams.sh
```

This will:
1. Find all `.dot`, `.mmd`, and `.puml` files in `docs/diagrams/src/`
2. Generate corresponding `.svg` files in `docs/diagrams/`
3. Report success/failure for each diagram

---

## Manual Generation

If you prefer to generate diagrams individually:

### Graphviz (DOT files)

```bash
# Generate SVG from DOT
dot -Tsvg docs/diagrams/src/system_architecture.dot -o docs/diagrams/system_architecture.svg

# Generate PNG from DOT
dot -Tpng docs/diagrams/src/system_architecture.dot -o docs/diagrams/system_architecture.png

# Generate PDF from DOT
dot -Tpdf docs/diagrams/src/system_architecture.dot -o docs/diagrams/system_architecture.pdf
```

### Mermaid (MMD files)

```bash
# Generate SVG from Mermaid (transparent background)
mmdc -i docs/diagrams/src/seq_syscall.mmd -o docs/diagrams/seq_syscall.svg -b transparent

# Generate PNG from Mermaid
mmdc -i docs/diagrams/src/seq_syscall.mmd -o docs/diagrams/seq_syscall.png -b transparent

# Generate PDF from Mermaid
mmdc -i docs/diagrams/src/seq_syscall.mmd -o docs/diagrams/seq_syscall.pdf -b transparent
```

### PlantUML (PUML files)

```bash
# Generate SVG from PlantUML
plantuml -tsvg docs/diagrams/src/example.puml -o docs/diagrams/

# Generate PNG from PlantUML
plantuml -tpng docs/diagrams/src/example.puml -o docs/diagrams/

# Generate PDF from PlantUML
plantuml -tpdf docs/diagrams/src/example.puml -o docs/diagrams/
```

---

## Troubleshooting

### Graphviz: "dot: command not found"

**Windows:**
- Verify Graphviz is installed: Check `C:\Program Files\Graphviz\bin\dot.exe`
- Add to PATH: System Properties → Environment Variables → Path → Add `C:\Program Files\Graphviz\bin`
- Restart terminal

**Linux/macOS:**
- Reinstall: `sudo apt install graphviz` or `brew install graphviz`
- Check PATH: `echo $PATH | grep -o graphviz`

### Mermaid CLI: "mmdc: command not found"

**All platforms:**
- Verify Node.js is installed: `node --version`
- Reinstall globally: `npm install -g @mermaid-js/mermaid-cli`
- Check npm global path: `npm config get prefix`
- On Windows, restart terminal after npm install

### Mermaid CLI: Puppeteer/Chromium errors

**Linux:**
```bash
# Install required dependencies for Puppeteer
sudo apt install -y libnss3 libatk1.0-0 libatk-bridge2.0-0 libcups2 \
    libdrm2 libxkbcommon0 libxcomposite1 libxdamage1 libxrandr2 \
    libgbm1 libasound2
```

**All platforms:**
```bash
# Reinstall with explicit Chromium download
npm install -g @mermaid-js/mermaid-cli --unsafe-perm=true
```

### PlantUML: "java: command not found"

**All platforms:**
- Install Java: https://www.java.com/download/
- Verify: `java -version`
- Restart terminal

### Diagrams not rendering in browser

- SVG files should render directly in modern browsers
- If issues, convert to PNG: `dot -Tpng input.dot -o output.png`
- For GitHub, ensure SVG files are committed (not in .gitignore)

---

## Online Alternatives

If you can't install tools locally, use online editors:

### Graphviz Online
- https://dreampuf.github.io/GraphvizOnline/
- https://edotor.net/

### Mermaid Online
- https://mermaid.live/
- https://mermaid-js.github.io/mermaid-live-editor/

### PlantUML Online
- https://www.plantuml.com/plantuml/
- http://www.plantuml.com/plantuml/uml/

**Workflow:**
1. Copy source code from `docs/diagrams/src/*.dot` or `*.mmd`
2. Paste into online editor
3. Download generated SVG/PNG
4. Save to `docs/diagrams/`

---

## Editor Integrations

### VS Code Extensions

```bash
# Install VS Code extensions for diagram editing
code --install-extension joaompinto.vscode-graphviz
code --install-extension bierner.markdown-mermaid
code --install-extension jebbs.plantuml
```

Features:
- Live preview of diagrams
- Syntax highlighting
- Auto-completion
- Export to various formats

### Vim/Neovim Plugins

```vim
" Add to .vimrc or init.vim
Plug 'aklt/plantuml-syntax'
Plug 'weirongxu/plantuml-previewer.vim'
Plug 'tyru/open-browser.vim'  " Required for previewer
```

---

## CI/CD Integration

For automated diagram generation in CI/CD:

### GitHub Actions

```yaml
name: Generate Diagrams

on:
  push:
    paths:
      - 'docs/diagrams/src/**'

jobs:
  generate:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v3
      
      - name: Install Graphviz
        run: sudo apt install -y graphviz
      
      - name: Setup Node.js
        uses: actions/setup-node@v3
        with:
          node-version: '18'
      
      - name: Install Mermaid CLI
        run: npm install -g @mermaid-js/mermaid-cli
      
      - name: Generate Diagrams
        run: bash scripts/generate-diagrams.sh
      
      - name: Commit Generated Diagrams
        run: |
          git config user.name "GitHub Actions"
          git config user.email "actions@github.com"
          git add docs/diagrams/*.svg
          git commit -m "Auto-generate diagrams" || echo "No changes"
          git push
```

---

## Additional Resources

- **Graphviz Documentation:** https://graphviz.org/documentation/
- **Graphviz Gallery:** https://graphviz.org/gallery/
- **Mermaid Documentation:** https://mermaid-js.github.io/mermaid/
- **Mermaid Examples:** https://github.com/mermaid-js/mermaid/tree/develop/docs
- **PlantUML Guide:** https://plantuml.com/guide
- **DOT Language Reference:** https://graphviz.org/doc/info/lang.html

---

**Questions?** See [docs/diagrams/README.md](README.md) for diagram usage and examples.
