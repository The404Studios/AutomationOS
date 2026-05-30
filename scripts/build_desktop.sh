#!/bin/bash
# Build the expanded desktop userspace (compositor_m5 + apps), package into the
# initrd, and rebuild the GRUB ISO. Run: wsl -d Arch bash scripts/build_desktop.sh
set -e
cd "$(dirname "$0")/.."

CFLAGS="-std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector -fno-pic -fno-pie -mno-red-zone -O2"
LDFLAGS="-nostdlib -static -n -no-pie -e _start -T userspace/userspace.ld"

cc() { gcc $CFLAGS -c "$1" -o "$2"; }

echo "[build] compiling..."
cc userspace/lib/font/bitfont.c        /tmp/bf.o
cc userspace/lib/wl/wl_client.c        /tmp/wlc.o
cc userspace/lib/ui/ui.c               /tmp/ui.o
cc userspace/compositor/compositor_m5.c /tmp/cm5.o
cc userspace/apps/terminal/terminal_m3.c /tmp/term.o
cc userspace/apps/filemanager/filemanager.c /tmp/fm.o
cc userspace/apps/calculator/calculator.c   /tmp/calc.o
cc userspace/apps/clock/clock.c        /tmp/clock.o
cc userspace/apps/sysinfo/sysinfo.c    /tmp/sysinfo.o
cc userspace/init/main.c               /tmp/init.o

echo "[build] linking..."
ld $LDFLAGS /tmp/cm5.o   /tmp/bf.o                          -o /tmp/comp.elf
ld $LDFLAGS /tmp/term.o  /tmp/wlc.o /tmp/bf.o               -o /tmp/term.elf
ld $LDFLAGS /tmp/fm.o    /tmp/ui.o  /tmp/wlc.o /tmp/bf.o    -o /tmp/fm.elf
ld $LDFLAGS /tmp/calc.o  /tmp/ui.o  /tmp/wlc.o /tmp/bf.o    -o /tmp/calc.elf
ld $LDFLAGS /tmp/clock.o /tmp/ui.o  /tmp/wlc.o /tmp/bf.o    -o /tmp/clock.elf
ld $LDFLAGS /tmp/sysinfo.o /tmp/ui.o /tmp/wlc.o /tmp/bf.o   -o /tmp/sysinfo.elf
ld $LDFLAGS /tmp/init.o                                     -o /tmp/init.elf

echo "[build] canary check (all must be 0):"
for e in comp term fm calc clock sysinfo init; do
    n=$(objdump -d /tmp/$e.elf | grep -c "fs:0x28" || true)
    echo "  $e=$n"
done

echo "[build] packaging initrd..."
rm -rf /tmp/ird && mkdir -p /tmp/ird
( cd /tmp/ird && tar xf /mnt/c/Users/wilde/Desktop/Kernel/iso/boot/initrd.img )
cp /tmp/comp.elf    /tmp/ird/sbin/compositor
cp /tmp/term.elf    /tmp/ird/sbin/terminal
cp /tmp/fm.elf      /tmp/ird/sbin/filemanager
cp /tmp/calc.elf    /tmp/ird/sbin/calculator
cp /tmp/clock.elf   /tmp/ird/sbin/clock
cp /tmp/sysinfo.elf /tmp/ird/sbin/sysinfo
cp /tmp/init.elf    /tmp/ird/sbin/init
( cd /tmp/ird && tar --format=ustar --owner=0 --group=0 -cf /mnt/c/Users/wilde/Desktop/Kernel/iso/boot/initrd.img . )

echo "[build] rebuilding ISO..."
grub-mkrescue -o build/automationos.iso iso/ 2>/dev/null
echo "[build] DONE: $(stat -c%s build/automationos.iso) byte ISO"
