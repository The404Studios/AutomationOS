#!/usr/bin/env bash
# =============================================================================
# ABLoader build script  (v2 — initrd + VBE support)
# Author: fourzerofour
#
# Produces: abloader.img  (flat raw disk image)
# Layout:
#   Sector 0          (512 B)  : stage1.bin  (MBR / boot sector)
#   Sectors 1-8      (4096 B)  : stage2.bin  (protected-mode loader)
#   Sectors 9+                 : kernel.elf  (KERNEL_SECTOR_COUNT sectors)
#   Sectors 9+KSC+             : initrd.img  (INITRD_SECTOR_COUNT sectors)
#
# Usage (from boot/abloader/ or repo root):
#   cd boot/abloader && bash build.sh
#   bash boot/abloader/build.sh
#
# Toolchain required:
#   nasm >= 2.14
#   qemu-system-x86_64 (for testing)
# =============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
OUT="${SCRIPT_DIR}"

KERNEL_ELF="${REPO_ROOT}/build/kernel.elf"
INITRD_IMG="${REPO_ROOT}/iso/boot/initrd.img"

echo "============================================"
echo "  ABLoader Build v2"
echo "  Author: fourzerofour"
echo "============================================"
echo ""

# ---- Sanity checks ----
if ! command -v nasm &>/dev/null; then
    echo "ERROR: nasm not found. Install with: pacman -S nasm  or  apt install nasm"
    exit 1
fi

# ---- Assemble stage 1 ----
echo "[1/5] Assembling stage1.asm..."
nasm -f bin \
     -o "${OUT}/stage1.bin" \
     "${SCRIPT_DIR}/stage1.asm"
echo "      stage1.bin: $(wc -c < "${OUT}/stage1.bin") bytes (expect 512)"

STAGE1_SIZE=$(wc -c < "${OUT}/stage1.bin")
if [ "${STAGE1_SIZE}" -ne 512 ]; then
    echo "ERROR: stage1.bin is ${STAGE1_SIZE} bytes, expected 512!"
    exit 1
fi

BOOT_SIG=$(dd if="${OUT}/stage1.bin" bs=1 skip=510 count=2 2>/dev/null | od -An -tx1 | tr -d ' \n')
if [ "${BOOT_SIG}" != "55aa" ]; then
    echo "ERROR: Boot signature missing (got ${BOOT_SIG}, expected 55aa)"
    exit 1
fi
echo "      Boot signature: 0xAA55 - OK"

# ---- Locate kernel ELF ----
echo "[2/5] Locating kernel ELF..."
if [ ! -f "${KERNEL_ELF}" ]; then
    echo "WARNING: ${KERNEL_ELF} not found."
    echo "         Building a dummy kernel placeholder for testing."
    python3 "${SCRIPT_DIR}/gen_stub_kernel.py" "${OUT}/kernel_stub.elf"
    KERNEL_ELF="${OUT}/kernel_stub.elf"
fi

KERNEL_SIZE=$(wc -c < "${KERNEL_ELF}")
echo "      Kernel ELF: ${KERNEL_SIZE} bytes"

ELF_MAGIC=$(dd if="${KERNEL_ELF}" bs=1 count=4 2>/dev/null | od -An -tx1 | tr -d ' \n')
if [ "${ELF_MAGIC}" != "7f454c46" ]; then
    echo "ERROR: ${KERNEL_ELF} does not look like an ELF (magic: ${ELF_MAGIC})"
    exit 1
fi
echo "      ELF magic: OK"

KERNEL_SECTORS=$(( (KERNEL_SIZE + 511) / 512 ))
echo "      Kernel sectors: ${KERNEL_SECTORS}"

# ---- Locate initrd ----
echo "[3/5] Locating initrd..."
INITRD_SECTORS=0
INITRD_LBA=$(( 9 + KERNEL_SECTORS ))
HAS_INITRD=0

if [ -f "${INITRD_IMG}" ]; then
    INITRD_SIZE=$(wc -c < "${INITRD_IMG}")
    INITRD_SECTORS=$(( (INITRD_SIZE + 511) / 512 ))
    HAS_INITRD=1
    echo "      Initrd: ${INITRD_IMG} (${INITRD_SIZE} bytes, ${INITRD_SECTORS} sectors)"
    echo "      Initrd LBA start: ${INITRD_LBA}"
else
    echo "      WARNING: ${INITRD_IMG} not found — building without initrd."
    echo "      The kernel will boot but skip initrd/userspace init."
fi

# ---- Assemble stage 2 with patched constants ----
echo "[4/5] Assembling stage2.asm..."

# Patch KERNEL_SECTOR_COUNT, INITRD_LBA, INITRD_SECTOR_COUNT in stage2.asm
# We use sed to replace the equ lines exactly.
sed -i \
    -e "s/^KERNEL_SECTOR_COUNT.*$/KERNEL_SECTOR_COUNT  equ ${KERNEL_SECTORS}/" \
    -e "s/^INITRD_LBA.*$/INITRD_LBA           equ ${INITRD_LBA}/" \
    -e "s/^INITRD_SECTOR_COUNT.*$/INITRD_SECTOR_COUNT  equ ${INITRD_SECTORS}/" \
    "${SCRIPT_DIR}/stage2.asm"

nasm -f bin \
     -o "${OUT}/stage2.bin" \
     "${SCRIPT_DIR}/stage2.asm"
STAGE2_SIZE=$(wc -c < "${OUT}/stage2.bin")
echo "      stage2.bin: ${STAGE2_SIZE} bytes (expect 4096)"

if [ "${STAGE2_SIZE}" -ne 4096 ]; then
    echo "ERROR: stage2.bin is ${STAGE2_SIZE} bytes, expected 4096!"
    echo "       stage2.asm code has grown beyond 4096 bytes."
    echo "       Reduce code size or expand to more sectors."
    exit 1
fi

# ---- Assemble final disk image ----
echo "[5/5] Building abloader.img..."

IMG="${OUT}/abloader.img"

# Total sectors: 1 (stage1) + 8 (stage2) + kernel + initrd + 1 padding
TOTAL_SECTORS=$(( 9 + KERNEL_SECTORS + INITRD_SECTORS + 1 ))
TOTAL_BYTES=$(( TOTAL_SECTORS * 512 ))

dd if=/dev/zero of="${IMG}" bs=512 count="${TOTAL_SECTORS}" 2>/dev/null

# Write stage 1 at sector 0
dd if="${OUT}/stage1.bin"  of="${IMG}" bs=512 seek=0              count=1               conv=notrunc 2>/dev/null
# Write stage 2 at sectors 1-8
dd if="${OUT}/stage2.bin"  of="${IMG}" bs=512 seek=1              count=8               conv=notrunc 2>/dev/null
# Write kernel at sector 9
dd if="${KERNEL_ELF}"      of="${IMG}" bs=512 seek=9              conv=notrunc          2>/dev/null
# Write initrd immediately after kernel (if present)
if [ "${HAS_INITRD}" -eq 1 ]; then
    dd if="${INITRD_IMG}"  of="${IMG}" bs=512 seek="${INITRD_LBA}" conv=notrunc         2>/dev/null
fi

echo ""
echo "============================================"
echo "  Build complete!"
echo "============================================"
echo "  Image:           ${IMG}"
echo "  Size:            $(wc -c < "${IMG}") bytes (${TOTAL_SECTORS} sectors)"
echo "  Kernel LBA:      9  (${KERNEL_SECTORS} sectors)"
if [ "${HAS_INITRD}" -eq 1 ]; then
    echo "  Initrd LBA:      ${INITRD_LBA}  (${INITRD_SECTORS} sectors)"
else
    echo "  Initrd:          not included"
fi
echo ""
echo "  Test command (headless serial log):"
echo ""
echo "    timeout 25 qemu-system-x86_64 \\"
echo "      -drive format=raw,file=${IMG},if=ide,index=0 \\"
echo "      -m 512M \\"
echo "      -serial file:/tmp/abl.log \\"
echo "      -display none \\"
echo "      -no-reboot"
echo "    sleep 3 && cat /tmp/abl.log"
echo ""
echo "  Test command (with display):"
echo ""
echo "    qemu-system-x86_64 \\"
echo "      -drive format=raw,file=${IMG},if=ide,index=0 \\"
echo "      -m 512M \\"
echo "      -serial stdio \\"
echo "      -display sdl \\"
echo "      -no-reboot"
echo ""
echo "  GDB debugging:"
echo ""
echo "    qemu-system-x86_64 \\"
echo "      -drive format=raw,file=${IMG},if=ide,index=0 \\"
echo "      -m 512M -s -S -serial stdio -display none -no-reboot"
echo "    # then: gdb ${KERNEL_ELF}"
echo "    # (gdb) target remote :1234"
echo "    # (gdb) hbreak *0x101000"
echo "    # (gdb) c"
echo ""
