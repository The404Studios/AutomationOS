#!/bin/bash
# make_multicore_isos.sh -- build the three ISOs the `multicore` proof boots (and the release ships).
#
#   build/mc-single.iso        default (single-core) kernel  + LEAN desktop userspace      (T410-safe fallback)
#   build/mc-smp.iso           MULTICORE=1 kernel            + LEAN desktop userspace      (the multi-core desktop)
#   build/mc-smp-storm.iso     MULTICORE=1 SMP_PRODUCT=0     + LEAN desktop userspace      (the 60 s BKL storms; the project's accepted profile)
#   build/mc-smp-full.iso      MULTICORE=1 SMP_PRODUCT=0     + FULL self-test suite        (every validator + ~130 self-tests on two cores)
#
# The two LEAN images are what a user runs; they carry sbin/livenet (NET_LIVE=1) so the guest itself reports
# whether real DNS / HTTP / certificate-verified HTTPS work. The storm image exists only to run the SMP tests
# (the storm images' PROOF kernel launches two 60 s bklstorm processes that saturate the kernel lock on purpose, so it is NOT a fair
# desktop-responsiveness test -- the shipping MULTICORE=1 kernel is built with SMP_PRODUCT=1 and does not launch them).
#
#   wsl -d Arch bash /mnt/c/Users/wilde/Desktop/Kernel/scripts/make_multicore_isos.sh
set -u
cd "$(dirname "$0")/.." || exit 1
mkdir -p build
die() { echo "make_multicore_isos: $*"; exit 1; }

mkiso() { # mkiso <kernel.elf> <out.iso>
    cp "$1" iso/boot/kernel.elf && grub-mkrescue -o "$2" iso/ > /tmp/mc_iso.log 2>&1 \
        || { tail -5 /tmp/mc_iso.log; die "grub-mkrescue failed for $2"; }
    echo "  $(stat -c %s "$2") bytes  $2"
}

echo "=== [1/6] default (single-core) T410-safe kernel ==="
T410_SAFE=1 SCHED_DEBUG=0 bash scripts/quick_build.sh > /tmp/mc_k0.log 2>&1
grep -qF 'Link OK -- no unresolved symbols' /tmp/mc_k0.log || { grep -E '^FAIL|error:|undefined' /tmp/mc_k0.log | head -5; die "single-core kernel link failed"; }
cp build/kernel.elf build/kernel-t410-default.elf

echo "=== [2/6] MULTICORE=1 T410-safe kernel (full SMP stack, shipping profile: SMP_PRODUCT=1) ==="
T410_SAFE=1 SCHED_DEBUG=0 MULTICORE=1 bash scripts/quick_build.sh > /tmp/mc_k1.log 2>&1
grep -qF 'Link OK -- no unresolved symbols' /tmp/mc_k1.log || { grep -E '^FAIL|error:|undefined' /tmp/mc_k1.log | head -5; die "multi-core kernel link failed"; }
[ -s build/kernel-smp.elf ] || die "MULTICORE=1 did not produce build/kernel-smp.elf"
grep -qF 'SMP_PRODUCT: boot-time proof storms are NOT launched' /tmp/mc_k1.log || die "shipping multi-core kernel is not an SMP_PRODUCT build"
cp build/kernel-smp.elf build/kernel-t410-smp.elf

echo "=== [3/6] MULTICORE=1 SMP_PRODUCT=0 T410-safe kernel (the PROOF kernel: boot-time bklstorm storms ON) ==="
T410_SAFE=1 SCHED_DEBUG=0 MULTICORE=1 SMP_PRODUCT=0 bash scripts/quick_build.sh > /tmp/mc_k2.log 2>&1
grep -qF 'Link OK -- no unresolved symbols' /tmp/mc_k2.log || { grep -E '^FAIL|error:|undefined' /tmp/mc_k2.log | head -5; die "proof kernel link failed"; }
grep -qF "SMP_PRODUCT" /tmp/mc_k2.log && die "SMP_PRODUCT=0 still produced a PRODUCT kernel"
cp build/kernel-smp.elf build/kernel-t410-smp-proof.elf

echo "=== [4/6] LEAN userspace (the shipped desktop) + livenet ==="
NET_LIVE=1 bash scripts/build_all.sh > /tmp/mc_ua_lean.log 2>&1
grep -qE 'error:|undefined reference' /tmp/mc_ua_lean.log && { grep -E 'error:|undefined reference' /tmp/mc_ua_lean.log | head -5; die "lean userspace build failed"; }
tail -1 /tmp/mc_ua_lean.log
mkiso build/kernel-t410-default.elf build/mc-single.iso
mkiso build/kernel-t410-smp.elf     build/mc-smp.iso
mkiso build/kernel-t410-smp-proof.elf build/mc-smp-storm.iso

echo "=== [5/6] FULL userspace (self-test storm) for the SMP stress markers ==="
FULL=1 NET_LIVE=1 bash scripts/build_all.sh > /tmp/mc_ua_full.log 2>&1
grep -qE 'error:|undefined reference' /tmp/mc_ua_full.log && { grep -E 'error:|undefined reference' /tmp/mc_ua_full.log | head -5; die "full userspace build failed"; }
tail -1 /tmp/mc_ua_full.log
mkiso build/kernel-t410-smp-proof.elf build/mc-smp-full.iso

echo "=== [6/6] restore the default kernel + LEAN initrd in the iso tree ==="
cp build/kernel-t410-default.elf iso/boot/kernel.elf
NET_LIVE=1 bash scripts/build_all.sh > /tmp/mc_ua_restore.log 2>&1
ls -la build/mc-single.iso build/mc-smp.iso build/mc-smp-storm.iso build/mc-smp-full.iso | awk '{print "  " $5, $9}'
echo "MC-ISOS: DONE"
