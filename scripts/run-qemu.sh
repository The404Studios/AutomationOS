#!/usr/bin/env bash
#
# AutomationOS QEMU Launcher
#
# Launches AutomationOS ISO in QEMU with graphical display, UEFI boot,
# PS/2 input, and optional debug mode.
#
# Usage:
#   ./scripts/run-qemu.sh          # Normal boot (graphical)
#   ./scripts/run-qemu.sh --debug  # Debug mode (GDB on port 1234)
#   ./scripts/run-qemu.sh --help   # Show help
#
# Works on Linux and Windows (Git Bash / WSL).

set -e

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Configuration
ISO="build/automationos.iso"
MEMORY="4G"
CPUS="4"
SERIAL_LOG="build/serial.log"

# Detect QEMU binary
QEMU=""
if command -v qemu-system-x86_64 &> /dev/null; then
    QEMU="qemu-system-x86_64"
elif [ -f "/c/Program Files/qemu/qemu-system-x86_64.exe" ]; then
    QEMU="/c/Program Files/qemu/qemu-system-x86_64.exe"
elif [ -f "/c/Program Files (x86)/qemu/qemu-system-x86_64.exe" ]; then
    QEMU="/c/Program Files (x86)/qemu/qemu-system-x86_64.exe"
fi

# OVMF firmware paths (UEFI)
OVMF_PATHS=(
    "/usr/share/ovmf/OVMF.fd"
    "/usr/share/OVMF/OVMF_CODE.fd"
    "/usr/share/OVMF/x64/OVMF_CODE.fd"
    "/usr/share/edk2-ovmf/x64/OVMF.fd"
    "/usr/share/edk2/x64/OVMF.fd"
    "/usr/share/qemu/OVMF.fd"
    "/opt/homebrew/share/qemu/edk2-x86_64-code.fd"
    "/usr/local/share/qemu/edk2-x86_64-code.fd"
    "/c/Program Files/qemu/share/edk2-x86_64-code.fd"
    "/c/Program Files (x86)/qemu/share/edk2-x86_64-code.fd"
)
OVMF=""
for path in "${OVMF_PATHS[@]}"; do
    if [ -f "$path" ]; then
        OVMF="$path"
        break
    fi
done

# --- Functions -----------------------------------------------------------

print_help() {
    cat << EOF
AutomationOS QEMU Launcher

Usage: $0 [OPTIONS]

Options:
    --debug         Start QEMU with GDB server on port 1234
    --help          Show this help message
    -m, --memory    Set RAM size (default: ${MEMORY})
    -smp           Set CPU count (default: ${CPUS})
    --vnc           Use VNC instead of graphical display
    --headless      No display (serial only)

Examples:
    $0                      # Normal boot (graphical)
    $0 --debug              # Debug mode
    $0 -m 8G -smp 8         # 8GB RAM, 8 CPUs
    $0 --headless            # Serial only

Debug:
    When started with --debug, attach GDB with:
        gdb build/kernel.elf -ex 'target remote :1234'

Serial output:
    Saved to: ${SERIAL_LOG}
    Use Ctrl-A X to exit QEMU
EOF
}

pick_display() {
    # Try -display gtk first, fallback to -vga std, then -display sdl
    if "$QEMU" -display gtk -help &>/dev/null 2>&1; then
        echo "-display gtk"
    elif "$QEMU" -display sdl -help &>/dev/null 2>&1; then
        echo "-display sdl"
    else
        echo "-vga std"
    fi
}

# --- Argument parsing ---------------------------------------------------

DEBUG=0
DISPLAY_FLAGS=""
while [[ $# -gt 0 ]]; do
    case $1 in
        --debug)
            DEBUG=1
            shift
            ;;
        --help)
            print_help
            exit 0
            ;;
        -m|--memory)
            MEMORY="$2"
            shift 2
            ;;
        -smp)
            CPUS="$2"
            shift 2
            ;;
        --vnc)
            DISPLAY_FLAGS="-display vnc=:0"
            shift
            ;;
        --headless)
            DISPLAY_FLAGS="-display none"
            shift
            ;;
        *)
            echo -e "${RED}ERROR: Unknown option: $1${NC}"
            echo "Use --help for usage information"
            exit 1
            ;;
    esac
done

# --- Pre-flight checks --------------------------------------------------

if [ -z "$QEMU" ]; then
    echo -e "${RED}ERROR: qemu-system-x86_64 not found${NC}"
    echo "Install QEMU:"
    echo "  Ubuntu/Debian: sudo apt install qemu-system-x86"
    echo "  Arch:          sudo pacman -S qemu"
    echo "  macOS:         brew install qemu"
    echo "  Windows:       https://qemu.org/download"
    exit 1
fi

if [ ! -f "$ISO" ]; then
    echo -e "${RED}ERROR: ISO not found: $ISO${NC}"
    echo "Run 'make iso' first to build the ISO image"
    exit 1
fi

if [ -z "$OVMF" ]; then
    echo -e "${YELLOW}WARNING: OVMF UEFI firmware not found; boot may fail without it${NC}"
    echo "Install OVMF:"
    echo "  Ubuntu/Debian: sudo apt install ovmf"
    echo "  Arch:          sudo pacman -S edk2-ovmf"
    echo "  macOS:         brew install qemu  (includes OVMF)"
    echo ""
fi

# --- Pick display if not explicitly set ----------------------------------
if [ -z "$DISPLAY_FLAGS" ]; then
    DISPLAY_FLAGS=$(pick_display)
fi

# --- Build command ------------------------------------------------------

QEMU_CMD=("$QEMU")

# UEFI firmware
if [ -n "$OVMF" ]; then
    QEMU_CMD+=(-bios "$OVMF")
fi

# Boot media
QEMU_CMD+=(-cdrom "$ISO")

# Hardware
QEMU_CMD+=(-m "$MEMORY")
QEMU_CMD+=(-smp "$CPUS")

# PS/2 keyboard & mouse
QEMU_CMD+=(-device isa-ps2)

# Serial output: tee to both stdio and file
QEMU_CMD+=(-serial "file:$SERIAL_LOG")
QEMU_CMD+=(-serial mon:stdio)

# Display
# shellcheck disable=SC2206
QEMU_CMD+=($DISPLAY_FLAGS)

# Behaviour
QEMU_CMD+=(-no-reboot)
QEMU_CMD+=(-no-shutdown)

# Optional initrd
INITRD="build/initrd.img"
if [ -f "$INITRD" ]; then
    QEMU_CMD+=(-initrd "$INITRD")
fi

# --- Debug mode ---------------------------------------------------------

if [ "$DEBUG" -eq 1 ]; then
    QEMU_CMD+=(-s -S)
    echo -e "${GREEN}================================${NC}"
    echo -e "${GREEN}  QEMU Debug Mode${NC}"
    echo -e "${GREEN}================================${NC}"
    echo ""
    echo "GDB server listening on port 1234"
    echo ""
    echo "Attach GDB with:"
    echo -e "  ${YELLOW}gdb build/kernel.elf -ex 'target remote :1234'${NC}"
    echo ""
    echo "Press any key to start QEMU..."
    read -n 1 -s
    echo ""
fi

# --- Launch -------------------------------------------------------------

echo -e "${GREEN}================================${NC}"
echo -e "${GREEN}  AutomationOS QEMU${NC}"
echo -e "${GREEN}================================${NC}"
echo ""
echo "QEMU:    $QEMU"
echo "ISO:     $ISO"
echo "OVMF:    ${OVMF:-<none>}"
echo "Memory:  $MEMORY"
echo "CPUs:    $CPUS"
echo "Display: $DISPLAY_FLAGS"
echo "Debug:   $([ $DEBUG -eq 1 ] && echo 'yes' || echo 'no')"
echo "Serial:  $SERIAL_LOG"
echo ""
echo "Starting QEMU..."
echo ""

# Use exec so Ctrl-C kills QEMU directly
exec "${QEMU_CMD[@]}"
