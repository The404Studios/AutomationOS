# brick record: INITRD-ALIAS-0

> The kernel file-read alias bug, killed: initrd-backed VFS reads returned
> exact byte counts but ALL-ZERO (or worse: another variable's) data inside
> big-image processes, because the initrd was read through the LOW IDENTITY
> map that user ELF images can shadow. Now it is read through the DIRECT MAP
> -- supervisor-only, shared into every CR3, unshadowable -- and the same
> bytes come back in EVERY process. Proven with a failing-then-passing pair.

```yaml
brick: INITRD-ALIAS-0
status: complete
branch: brick/initrd-alias-0
base: brick/browser2-img-0            # off the frozen BROWSER2-IMG-0 HEAD (a064b12)
request: >
  Prove WHY initrd-backed reads return zeroes in mmap-heavy processes; fix the address-space
  aliasing / kernel initrd mapping access; add a regression test using browser2 or an mmap-heavy
  reader. HARD NO's: no browser layout, no network, no image decode. Single-core runtime; T410
  (2010 hardware) safe. Acceptance: INITRD-ALIAS: PASS pristine_read=1 mmapheavy_read=1
  same_bytes=1 browser_file_img=1 zero_bug_gone=1; desktop 0 panic.
root_cause: >
  The boot rescue copies the initrd to PHYSICAL 16 MiB (boot.asm); initrd_init kept that address
  verbatim and every consumer (tar parse, zero-copy inode->data, initrd_get_file -> ELF loads) read
  it through the LOW IDENTITY map at VA 16 MiB. But user ELF images load at VA 0x800000 INSIDE the
  identity region, mapped per-process as private 4 KiB pages that REPLACE identity PTEs in that
  process's (private, split-on-demand) low page-table chain. browser2's image grew to memsz
  0x214f888 (~35 MB; imgcodec scratch + framebuffer + caches), spanning VA 8..44 MiB -- fully
  covering the initrd's VA 16..21.3 MiB. Kernel reads of inode->data on browser2's CR3 therefore
  returned browser2's OWN private pages: first all-zero BSS, later live data (the pre-fix probe
  read hdr=180,110,60,255 -- literally a decoded big.png pixel from browser2's own image scratch
  sitting at the aliased VA). Tiny processes (tool_read, init) never extend past 16 MiB, so their
  identity view stayed intact -- the same 4 KiB initrd page read fine from one process and as
  zeroes from another, which had falsified every memory-corruption theory during BROWSER2-IMG-0.
  "mmap-heavy" was a correlate; BIG-IMAGE is the cause (anon mmaps land at 4 GiB, harmless).
decisions:
  - fix INSIDE initrd_init (one seam): convert the physical address to its DIRECT-MAP alias
    (PHYS_TO_DIRECT, PML4[256] -- supervisor-only, NX, dedicated never-split chain, shared BY
    REFERENCE into every CR3 since the #20 fix). Everything downstream inherits the unshadowable
    address; pmm_reserve_initrd keeps the RAW physical range (kernel.c passes boot_info verbatim),
    so frames stay protected AND the mapping is correct. Out-of-span fallback keeps the old
    behavior (boots). Single-core/T410-safe: pure VA selection at boot, no hardware touched, the
    direct map has been in every build since the NXE/#20 work.
  - verifier discipline: the regression test was run AGAINST the unfixed kernel first and had to
    FAIL before the fix run had to PASS. The first attempt at the big-image reader silently shipped
    with memsz=0x100 -- gcc dead-store-eliminated the 16 MiB pad (written, never read) -- producing
    a false pre-fix PASS caught by objdump -p; `volatile` on the pad is load-bearing.
checkpoints:
  - id: IA0
    title: direct-map initrd access + a failing-then-passing big-image regression pair
    commits: [9dad3ac, 1588c2a]   # fix(initrd) kernel, test(initrdalias) pair
    files:
      - kernel/init/initrd.c                      # THE FIX: initrd_addr = PHYS_TO_DIRECT(addr)
      - userspace/apps/initrdp/initrdp.c          # tiny pristine control reader (exit code = verdict)
      - userspace/apps/initrdalias/initrdalias.c  # 16 MiB-volatile-pad big-image + 2x4 MiB mmaps;
                                                  # spawns initrdp; prints the INITRD-ALIAS line
      - userspace/apps/browser2/browser2.c        # 6th <img src="/etc/imgtest/t.png"> + the
                                                  # BROWSER2-IMG-FILE probe line (frozen flags untouched)
      - scripts/build_all.sh                      # build/stage the pair (100th/101st sbin entries)
      - userspace/init/main.c                     # spawn initrdalias (non-minimal builds)
    tests: [build_test/initrd_alias_verify.sh]    # quick_build (KERNEL) + build_all + boot + asserts
    result: >
      PRE-FIX (kernel fix stashed, same tests): INITRD-ALIAS: FAIL pristine_read=1 mmapheavy_read=0
      same_bytes=0 zero_bug_gone=0 + BROWSER2-IMG-FILE: FAIL initrd_img=0 (hdr bytes = browser2's
      own big.png pixels -- the alias photographed). POST-FIX: boot line "[INITRD] Initrd phys
      0x0000000001000000 -> direct-map 0xffff800001000000"; INITRD-ALIAS: PASS pristine_read=1
      mmapheavy_read=1 same_bytes=1 zero_bug_gone=1; BROWSER2-IMG-FILE: PASS initrd_img=1 (browser2,
      the ORIGINAL failing process, decodes the initrd-backed png to a real 8x8); frozen
      BROWSER2-IMG line + whole browser pipeline + whole rail green; iacheck2.png clean; 0 panic.
      Composite: INITRD-ALIAS: PASS pristine_read=1 mmapheavy_read=1 same_bytes=1
      browser_file_img=1 zero_bug_gone=1.
    design:
      - the pristine/big pair reads the SAME initrd file and byte-compares against the SAME embedded
        generated fixture (b2_img_fixtures.h) -- same file, same byte count, same expected bytes,
        two address-space shapes; the big half folds the pristine half's exit code in via
        spawn+waitpid, so one serial line carries both.
      - found-but-fine: the ELF loader copies segments on the CHILD's CR3, so spawning FROM a
        shadowed process still loaded correct binaries (initrdp ran fine pre-fix when spawned by
        the shadowed initrdalias). The latent spawn-corruption risk dies with the fix anyway.
      - residual (documented, not this brick): user images at VA 0x800000 still overlay the
        identity map of OTHER low physical RAM; anything else the kernel reads via low identity
        VAs inside a big process's image span has the same exposure class. Initrd was the only
        zero-copy long-lived consumer found; a later audit brick could sweep identity-VA reads.
    review:
      default_build_changed: true       # KERNEL change (first in this track): initrd VA selection
      all_waits_bounded: true           # tests: bounded read/wait loops; kernel: no new loops
      hardware_init_gated: n/a          # no hardware touched; pure paging/VA selection
      touches_userspace: true
      touches_kernel: true
      preserves_known_good_t410: true   # direct map ships in every current build; boot-time only;
                                        # T410_SAFE profile unaffected; single-core only
      smoke_proves_claim: true          # failing-then-passing pair + the boot mapping line
      raw_pointers_or_truncation: none
    verdict: pass
done: >
  INITRD-ALIAS-0 COMPLETE. The kernel reads the initrd through the direct map; initrd file content
  is now trustworthy in EVERY process, proven by a regression pair that fails on the old kernel and
  passes on the new one -- including browser2 itself, the process that exposed the bug. The
  embedded-fixture workaround in BROWSER2-IMG-0 is no longer load-bearing (kept as a deterministic
  baseline; the /etc file probe now exercises the real path every boot).
next:
  - net-stack Phase 1 (SOCK_MAX, SYN queue, UDP/OOO depth) -> E1000-PCH-0 (user order).
  - T410 retest with the malloc fix + this fix on real hardware (the desktop-regression suspects).
  - later: an identity-VA read audit (the residual exposure class above) · JPEG decoder.
```
