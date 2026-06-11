#!/bin/bash
# Test the SMP kernel ISO in QEMU
# Usage: ./test_smp_iso.sh [cpus]

CPUS=2
ISO="build/automationos-smp.iso"

if [ ! -f "" ]; then
    echo "Error:  not found. Run 'make iso' first."
    exit 1
fi

echo "Booting AutomationOS SMP kernel with  CPUs..."
echo "Press Ctrl-A X to exit QEMU"
echo ""

qemu-system-x86_64     -cdrom ""     -m 128M     -smp ""     -display none     -serial stdio
