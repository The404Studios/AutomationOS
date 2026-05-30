# GDB initialization file for AutomationOS kernel debugging
#
# Usage:
#   1. Start QEMU in debug mode:
#      make qemu-debug
#
#   2. In another terminal, start GDB:
#      gdb build/kernel.elf
#
# This file is automatically loaded by GDB

# Connect to QEMU
target remote :1234

# Set architecture
set architecture i386:x86-64:intel

# Load symbols
symbol-file build/kernel.elf

# Set disassembly flavor to Intel syntax
set disassembly-flavor intel

# Enable pretty printing
set print pretty on
set print array on
set print array-indexes on

# Break on kernel_panic
break kernel_panic

# Break on kernel_main
break kernel_main

# Display useful information
define hook-stop
    info registers
    x/10i $rip
end

# Custom commands
define pmm-status
    printf "PMM Status:\n"
    printf "  Total memory: %lu bytes\n", total_memory
    printf "  Used memory:  %lu bytes\n", used_memory
    printf "  Free memory:  %lu bytes\n", total_memory - used_memory
end

document pmm-status
Display Physical Memory Manager status
Usage: pmm-status
end

# Helper aliases
alias -a si = stepi
alias -a ni = nexti
alias -a bt = backtrace

# Print welcome message
printf "\n"
printf "=========================================\n"
printf "  AutomationOS Kernel Debugger\n"
printf "=========================================\n"
printf "\n"
printf "Connected to QEMU on localhost:1234\n"
printf "\n"
printf "Breakpoints:\n"
printf "  - kernel_main\n"
printf "  - kernel_panic\n"
printf "\n"
printf "Custom commands:\n"
printf "  pmm-status   - Display PMM status\n"
printf "\n"
printf "Type 'continue' to start execution\n"
printf "\n"
