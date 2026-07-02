#!/bin/bash
set -e
cd /mnt/c/Users/wilde/Desktop/Kernel
CF="-std=gnu11 -ffreestanding -nostdlib -nostdinc -fno-pic -fno-pie -fno-stack-protector -mno-red-zone -mcmodel=kernel -DBOOT_QUIET -Wno-unused-variable -Wno-unused-function -Wno-builtin-declaration-mismatch -Wno-implicit-function-declaration -Wno-int-conversion -Wno-incompatible-pointer-types -Ikernel/include -Ikernel/include/compat"
echo "=== compiling pe_loader.c with kernel flags ==="
gcc $CF -c kernel/pe/pe_loader.c -o /tmp/pe_loader.o && echo "PE_LOADER_COMPILE_OK"
echo "=== compiling dll_loader.c ==="
gcc $CF -c kernel/pe/dll_loader.c -o /tmp/dll_loader.o && echo "DLL_LOADER_COMPILE_OK"
