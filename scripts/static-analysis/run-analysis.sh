#!/bin/bash
#
# AutomationOS Static Analysis Runner
#
# Convenience script to run static analysis with various options.

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
ANALYSIS_DIR="${PROJECT_ROOT}/build/static-analysis"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Function to print colored output
print_status() {
    echo -e "${BLUE}==>${NC} $1"
}

print_success() {
    echo -e "${GREEN}✓${NC} $1"
}

print_warning() {
    echo -e "${YELLOW}⚠${NC} $1"
}

print_error() {
    echo -e "${RED}✗${NC} $1"
}

# Function to check if a tool is installed
check_tool() {
    if command -v "$1" &> /dev/null; then
        print_success "$1 found"
        return 0
    else
        print_warning "$1 not found"
        return 1
    fi
}

# Function to install tools
install_tools() {
    print_status "Checking for static analysis tools..."

    local missing=()

    check_tool "scan-build" || missing+=("clang-tools")
    check_tool "cppcheck" || missing+=("cppcheck")
    check_tool "sparse" || missing+=("sparse")
    check_tool "clang-tidy" || missing+=("clang-tidy")

    if [ ${#missing[@]} -eq 0 ]; then
        print_success "All tools installed"
        return 0
    fi

    print_warning "Missing tools: ${missing[*]}"
    echo ""
    echo "To install missing tools:"
    echo ""
    echo "  Ubuntu/Debian:"
    echo "    sudo apt-get install clang clang-tools clang-tidy cppcheck sparse"
    echo ""
    echo "  Arch Linux:"
    echo "    sudo pacman -S clang clang-tools-extra cppcheck sparse"
    echo ""
    echo "  macOS:"
    echo "    brew install llvm cppcheck sparse"
    echo ""

    return 1
}

# Function to run quick analysis (cppcheck + sparse)
run_quick() {
    print_status "Running quick analysis (cppcheck + sparse)..."
    cd "${PROJECT_ROOT}"

    mkdir -p "${ANALYSIS_DIR}"

    print_status "Running cppcheck..."
    make cppcheck

    print_status "Running sparse..."
    make sparse

    print_success "Quick analysis complete"
    echo ""
    echo "Results:"
    echo "  Cppcheck: ${ANALYSIS_DIR}/cppcheck.log"
    echo "  Sparse:   ${ANALYSIS_DIR}/sparse.log"
}

# Function to run full analysis
run_full() {
    print_status "Running full static analysis suite..."
    cd "${PROJECT_ROOT}"

    make analyze-all

    print_success "Full analysis complete"
    echo ""
    cat "${ANALYSIS_DIR}/summary.txt"
}

# Function to run only on changed files
run_incremental() {
    print_status "Running incremental analysis (changed files only)..."
    cd "${PROJECT_ROOT}"

    if [ ! -d ".git" ]; then
        print_error "Not a git repository. Run full analysis instead."
        exit 1
    fi

    make analyze-incremental

    print_success "Incremental analysis complete"
}

# Function to run specific tool
run_tool() {
    local tool="$1"
    print_status "Running $tool..."
    cd "${PROJECT_ROOT}"

    case "$tool" in
        clang|analyzer)
            make analyze
            ;;
        cppcheck)
            make cppcheck
            ;;
        sparse)
            make sparse
            ;;
        clang-tidy|tidy)
            make clang-tidy
            ;;
        *)
            print_error "Unknown tool: $tool"
            print_status "Available tools: clang, cppcheck, sparse, clang-tidy"
            exit 1
            ;;
    esac

    print_success "$tool analysis complete"
}

# Function to display help
show_help() {
    cat << EOF
AutomationOS Static Analysis Runner

Usage: $0 [COMMAND] [OPTIONS]

Commands:
  check       Check if all analysis tools are installed
  install     Show installation instructions for missing tools
  quick       Run quick analysis (cppcheck + sparse) - ~1 minute
  full        Run full analysis suite (all tools) - ~5 minutes
  incremental Run analysis on changed files only (git)
  tool <name> Run specific tool (clang, cppcheck, sparse, clang-tidy)
  weekly      Run weekly comprehensive scan with report
  help        Show this help message

Examples:
  $0 check              # Check if tools are installed
  $0 quick              # Quick scan before committing
  $0 full               # Full analysis
  $0 incremental        # Analyze only changed files
  $0 tool cppcheck      # Run only cppcheck
  $0 weekly             # Weekly comprehensive scan

Output:
  Results are saved to: build/static-analysis/
  Summary:   build/static-analysis/summary.txt
  Full logs: build/static-analysis/*.log

EOF
}

# Main script logic
main() {
    local command="${1:-help}"

    case "$command" in
        check)
            install_tools
            ;;
        install)
            install_tools || true
            ;;
        quick)
            run_quick
            ;;
        full)
            run_full
            ;;
        incremental)
            run_incremental
            ;;
        tool)
            if [ -z "$2" ]; then
                print_error "Tool name required"
                echo "Usage: $0 tool <name>"
                echo "Available: clang, cppcheck, sparse, clang-tidy"
                exit 1
            fi
            run_tool "$2"
            ;;
        weekly)
            print_status "Running weekly comprehensive scan..."
            cd "${PROJECT_ROOT}"
            make analyze-weekly
            ;;
        help|--help|-h)
            show_help
            ;;
        *)
            print_error "Unknown command: $command"
            echo ""
            show_help
            exit 1
            ;;
    esac
}

# Run main function
main "$@"
