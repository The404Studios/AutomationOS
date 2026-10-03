#!/bin/bash
# make_release_isos.sh <version> -- build the flashable T410 images + SHA256SUMS into build/release/.
#
#   automationos-<ver>-t410-single.iso         single core, T410-safe                       (the conservative baseline)
#   automationos-<ver>-t410-multicore.iso      MULTICORE=1 (full SMP stack, SMP_PRODUCT)    (two cores; QEMU-proven, hardware-unproven)
#   automationos-<ver>-t410-single-eth.iso     single core + PCH_NIC=1                      (opt-in 82577LM wired NIC bring-up: `nicup`)
#   automationos-<ver>-t410-multicore-eth.iso  multicore  + PCH_NIC=1
#
# All four carry the SAME lean desktop userspace (what a user runs). They do NOT carry the test-only `livenet`
# (which contacts a dozen real websites at every boot); the internet proof uses an otherwise identical NET_LIVE=1 build.
# Every image is the T410_SAFE profile (Westmere-safe, no on-screen scheduler debug).
#
#   wsl -d Arch bash /mnt/c/Users/wilde/Desktop/Kernel/scripts/make_release_isos.sh v0.2.0
set -u
cd "$(dirname "$0")/.." || exit 1
VER="${1:-dev}"
OUT=build/release
mkdir -p "$OUT"
die() { echo "make_release_isos: $*"; exit 1; }

kernel() { # kernel <tag> <KEY=VAL...>  -> build/rk-<tag>.elf
    local tag=$1; shift
    echo "=== kernel: $tag ($*) ==="
    env T410_SAFE=1 SCHED_DEBUG=0 "$@" bash scripts/quick_build.sh > "/tmp/rel_k_$tag.log" 2>&1
    grep -qF 'Link OK -- no unresolved symbols' "/tmp/rel_k_$tag.log" || { grep -E '^FAIL|error:|undefined' "/tmp/rel_k_$tag.log" | head -5; die "kernel $tag failed to link"; }
    local src=build/kernel.elf
    case " $* " in *" MULTICORE=1 "*|*" SMP=1 "*) src=build/kernel-smp.elf ;; esac
    [ -s "$src" ] || die "no $src for $tag"
    cp "$src" "build/rk-$tag.elf"
}

kernel single
kernel multicore         MULTICORE=1
kernel single-eth        PCH_NIC=1
kernel multicore-eth     MULTICORE=1 PCH_NIC=1
# the multicore kernels must be the shipping (SMP_PRODUCT) profile, not the proof kernel
grep -qF 'SMP_PRODUCT: boot-time proof storms are NOT launched' /tmp/rel_k_multicore.log     || die "multicore kernel is not an SMP_PRODUCT build"
grep -qF 'SMP_PRODUCT: boot-time proof storms are NOT launched' /tmp/rel_k_multicore-eth.log || die "multicore-eth kernel is not an SMP_PRODUCT build"
grep -qF 'PCH_NIC' /tmp/rel_k_single-eth.log || echo "note: quick_build did not echo PCH_NIC for single-eth (check the flag is honoured)"

echo "=== lean userspace (no test-only services) ==="
bash scripts/build_all.sh > /tmp/rel_ua.log 2>&1
grep -qE 'error:|undefined reference' /tmp/rel_ua.log && { grep -E 'error:|undefined reference' /tmp/rel_ua.log | head -5; die "userspace build failed"; }
tail -1 /tmp/rel_ua.log
[ -s iso/boot/initrd.img ] || die "no initrd"
grep -qac 'sbin/livenet' iso/boot/initrd.img && echo "note: initrd mentions livenet"   # informational; init only spawns it under NET_LIVE

for t in single multicore single-eth multicore-eth; do
    cp "build/rk-$t.elf" iso/boot/kernel.elf
    grub-mkrescue -o "$OUT/automationos-$VER-t410-$t.iso" iso/ > /tmp/rel_iso.log 2>&1 || { tail -5 /tmp/rel_iso.log; die "grub-mkrescue failed for $t"; }
    echo "  $(stat -c %s "$OUT/automationos-$VER-t410-$t.iso") bytes  $OUT/automationos-$VER-t410-$t.iso"
done
cp build/rk-single.elf iso/boot/kernel.elf             # leave the iso tree on the default kernel
( cd "$OUT" && sha256sum automationos-"$VER"-t410-*.iso > SHA256SUMS && cat SHA256SUMS )
echo "RELEASE-ISOS: DONE"
