#!/bin/bash
# Check if diagram generation tools are installed
# Quick verification script

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

echo -e "${BLUE}╔════════════════════════════════════════════╗${NC}"
echo -e "${BLUE}║   Diagram Tools Verification              ║${NC}"
echo -e "${BLUE}╚════════════════════════════════════════════╝${NC}"
echo

check_tool() {
    local tool=$1
    local name=$2
    local install_cmd=$3

    if command -v "$tool" &> /dev/null; then
        local version=$($tool --version 2>&1 | head -n 1)
        echo -e "  ${GREEN}✓${NC} $name found"
        echo -e "    Version: $version"
        return 0
    else
        echo -e "  ${RED}✗${NC} $name not found"
        echo -e "    ${YELLOW}Install with:${NC} $install_cmd"
        return 1
    fi
}

echo -e "${BLUE}Checking required tools...${NC}"
echo

# Graphviz
check_tool "dot" "Graphviz" "sudo apt install graphviz (Linux) or brew install graphviz (macOS)"
echo

# Node.js
check_tool "node" "Node.js" "sudo apt install nodejs (Linux) or brew install node (macOS)"
echo

# Mermaid CLI
check_tool "mmdc" "Mermaid CLI" "npm install -g @mermaid-js/mermaid-cli"
echo

# Optional: PlantUML
if command -v plantuml &> /dev/null || command -v java &> /dev/null; then
    if command -v plantuml &> /dev/null; then
        check_tool "plantuml" "PlantUML (optional)" "sudo apt install plantuml (Linux) or brew install plantuml (macOS)"
    else
        echo -e "  ${YELLOW}~${NC} PlantUML not found (optional)"
        check_tool "java" "Java (for PlantUML)" "sudo apt install default-jre (Linux) or brew install openjdk (macOS)"
    fi
else
    echo -e "  ${YELLOW}~${NC} PlantUML not found (optional)"
    echo -e "    Install: sudo apt install plantuml (Linux) or brew install plantuml (macOS)"
fi

echo
echo -e "${BLUE}═══════════════════════════════════════════${NC}"

# Summary
if command -v dot &> /dev/null && command -v mmdc &> /dev/null; then
    echo -e "${GREEN}✓ All required tools are installed!${NC}"
    echo
    echo -e "You can generate diagrams with:"
    echo -e "  ${BLUE}bash scripts/generate-diagrams.sh${NC}"
    echo
    exit 0
else
    echo -e "${RED}⚠ Some required tools are missing${NC}"
    echo
    echo -e "See ${BLUE}docs/diagrams/INSTALLATION.md${NC} for detailed installation instructions."
    echo
    exit 1
fi
