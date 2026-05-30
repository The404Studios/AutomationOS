#!/bin/bash
# AutomationOS Diagram Generator
# Generates all diagrams from source files in docs/diagrams/src/
#
# Requirements:
#   - Graphviz (dot command) for .dot files
#   - Mermaid CLI (mmdc command) for .mmd files
#   - PlantUML for .puml files (optional)
#
# Installation:
#   Ubuntu/Debian:
#     sudo apt install graphviz
#     npm install -g @mermaid-js/mermaid-cli
#
#   macOS:
#     brew install graphviz
#     npm install -g @mermaid-js/mermaid-cli
#
#   Arch Linux:
#     sudo pacman -S graphviz
#     npm install -g @mermaid-js/mermaid-cli

set -e  # Exit on error

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
SRC_DIR="$PROJECT_ROOT/docs/diagrams/src"
OUT_DIR="$PROJECT_ROOT/docs/diagrams"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

echo -e "${BLUE}╔════════════════════════════════════════════╗${NC}"
echo -e "${BLUE}║   AutomationOS Diagram Generator          ║${NC}"
echo -e "${BLUE}╚════════════════════════════════════════════╝${NC}"
echo

# Check for required tools
echo -e "${YELLOW}Checking for required tools...${NC}"

check_tool() {
    if command -v "$1" &> /dev/null; then
        echo -e "  ${GREEN}✓${NC} $1 found"
        return 0
    else
        echo -e "  ${RED}✗${NC} $1 not found"
        return 1
    fi
}

HAS_GRAPHVIZ=false
HAS_MERMAID=false
HAS_PLANTUML=false

if check_tool dot; then
    HAS_GRAPHVIZ=true
fi

if check_tool mmdc; then
    HAS_MERMAID=true
fi

if check_tool plantuml || check_tool java; then
    HAS_PLANTUML=true
fi

echo

# Create output directory if needed
mkdir -p "$OUT_DIR"

# Statistics
TOTAL_FILES=0
SUCCESS_COUNT=0
SKIP_COUNT=0
FAIL_COUNT=0

# Generate Graphviz diagrams (.dot → .svg)
if [ "$HAS_GRAPHVIZ" = true ]; then
    echo -e "${BLUE}Generating Graphviz diagrams...${NC}"
    for src in "$SRC_DIR"/*.dot; do
        if [ -f "$src" ]; then
            TOTAL_FILES=$((TOTAL_FILES + 1))
            filename=$(basename "$src" .dot)
            out="$OUT_DIR/${filename}.svg"
            echo -n "  Processing $filename.dot ... "

            if dot -Tsvg "$src" -o "$out" 2>/dev/null; then
                echo -e "${GREEN}✓${NC}"
                SUCCESS_COUNT=$((SUCCESS_COUNT + 1))
            else
                echo -e "${RED}✗ (failed)${NC}"
                FAIL_COUNT=$((FAIL_COUNT + 1))
            fi
        fi
    done
else
    echo -e "${YELLOW}Skipping Graphviz diagrams (dot not found)${NC}"
    for src in "$SRC_DIR"/*.dot; do
        [ -f "$src" ] && SKIP_COUNT=$((SKIP_COUNT + 1)) && TOTAL_FILES=$((TOTAL_FILES + 1))
    done
fi

echo

# Generate Mermaid diagrams (.mmd → .svg)
if [ "$HAS_MERMAID" = true ]; then
    echo -e "${BLUE}Generating Mermaid diagrams...${NC}"
    for src in "$SRC_DIR"/*.mmd; do
        if [ -f "$src" ]; then
            TOTAL_FILES=$((TOTAL_FILES + 1))
            filename=$(basename "$src" .mmd)
            out="$OUT_DIR/${filename}.svg"
            echo -n "  Processing $filename.mmd ... "

            # Mermaid CLI options: -b transparent for transparent background
            if mmdc -i "$src" -o "$out" -b transparent 2>/dev/null; then
                echo -e "${GREEN}✓${NC}"
                SUCCESS_COUNT=$((SUCCESS_COUNT + 1))
            else
                echo -e "${RED}✗ (failed)${NC}"
                FAIL_COUNT=$((FAIL_COUNT + 1))
            fi
        fi
    done
else
    echo -e "${YELLOW}Skipping Mermaid diagrams (mmdc not found)${NC}"
    echo -e "${YELLOW}Install with: npm install -g @mermaid-js/mermaid-cli${NC}"
    for src in "$SRC_DIR"/*.mmd; do
        [ -f "$src" ] && SKIP_COUNT=$((SKIP_COUNT + 1)) && TOTAL_FILES=$((TOTAL_FILES + 1))
    done
fi

echo

# Generate PlantUML diagrams (.puml → .svg) [optional]
if [ "$HAS_PLANTUML" = true ]; then
    echo -e "${BLUE}Generating PlantUML diagrams...${NC}"
    puml_count=0
    for src in "$SRC_DIR"/*.puml; do
        if [ -f "$src" ]; then
            TOTAL_FILES=$((TOTAL_FILES + 1))
            puml_count=$((puml_count + 1))
            filename=$(basename "$src" .puml)
            out="$OUT_DIR/${filename}.svg"
            echo -n "  Processing $filename.puml ... "

            if command -v plantuml &> /dev/null; then
                if plantuml -tsvg "$src" -o "$OUT_DIR" 2>/dev/null; then
                    echo -e "${GREEN}✓${NC}"
                    SUCCESS_COUNT=$((SUCCESS_COUNT + 1))
                else
                    echo -e "${RED}✗ (failed)${NC}"
                    FAIL_COUNT=$((FAIL_COUNT + 1))
                fi
            else
                echo -e "${YELLOW}✗ (skipped)${NC}"
                SKIP_COUNT=$((SKIP_COUNT + 1))
            fi
        fi
    done

    if [ "$puml_count" -eq 0 ]; then
        echo -e "  ${YELLOW}No PlantUML files found${NC}"
    fi
else
    for src in "$SRC_DIR"/*.puml; do
        [ -f "$src" ] && SKIP_COUNT=$((SKIP_COUNT + 1)) && TOTAL_FILES=$((TOTAL_FILES + 1))
    done
fi

echo

# Summary
echo -e "${BLUE}═══════════════════════════════════════════${NC}"
echo -e "${BLUE}Summary${NC}"
echo -e "${BLUE}═══════════════════════════════════════════${NC}"
echo -e "Total files:    $TOTAL_FILES"
echo -e "${GREEN}Generated:      $SUCCESS_COUNT${NC}"
[ "$SKIP_COUNT" -gt 0 ] && echo -e "${YELLOW}Skipped:        $SKIP_COUNT${NC}"
[ "$FAIL_COUNT" -gt 0 ] && echo -e "${RED}Failed:         $FAIL_COUNT${NC}"
echo

if [ "$SUCCESS_COUNT" -gt 0 ]; then
    echo -e "${GREEN}✓ Diagrams generated in: $OUT_DIR${NC}"
fi

# List generated files
if [ "$SUCCESS_COUNT" -gt 0 ]; then
    echo
    echo -e "${BLUE}Generated files:${NC}"
    ls -lh "$OUT_DIR"/*.svg 2>/dev/null | awk '{print "  " $9 " (" $5 ")"}'
fi

echo

# Exit with error if any failures
if [ "$FAIL_COUNT" -gt 0 ]; then
    echo -e "${RED}⚠ Some diagrams failed to generate${NC}"
    exit 1
fi

if [ "$TOTAL_FILES" -eq 0 ]; then
    echo -e "${YELLOW}⚠ No diagram source files found in $SRC_DIR${NC}"
    exit 1
fi

echo -e "${GREEN}✓ All diagrams generated successfully!${NC}"
