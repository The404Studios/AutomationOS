# brick record: BROWSER2-IMG-0

> `<img>` rendering in browser2 from code already in-tree: fetch/source the
> bytes, sniff the type, decode via imgcodec, lay out an LB_IMAGE box, blit
> clipped in the paint pass, placeholder on failure. The brick also surfaced
> and fixed the c70ee87 malloc regression that had silently broken the ENTIRE
> browser pipeline since June 8 -- and pinned a second, still-open kernel bug.

```yaml
brick: BROWSER2-IMG-0
status: complete
branch: brick/browser2-img-0
base: brick/model-bridge-0            # off the frozen MODEL-BRIDGE-0 HEAD (86a5c92)
request: >
  HTML <img> support ONLY -- fetch the image resource, sniff the type, decode via imgcodec, create an
  LB_IMAGE layout box, paint the bitmap during the render pass, fallback placeholder on failure.
  HARD NO's: no CSS overhaul, no JS, no forms, no JPEG, no network-stack rewrite, no engine replacement.
  Acceptance: BROWSER2-IMG: PASS png=1 gif=1 bmp=1 missing_safe=1 bounded=1; desktop clean; 0 panic.
decisions:
  - layout stays ignorant of fetching/decoding: an embedder-registered intrinsic-dims provider
    (layout_set_img_dims_provider) sizes LB_IMAGE boxes; no provider/dims -> a fixed placeholder box.
    <img> is an ATOMIC INLINE box (handled before the block/inline split), width clamped to the
    content width -- the "bounded" guarantee lives in the engine, the clipped blit in the renderer.
  - the acceptance page (about:imgtest) sources EMBEDDED fixture bytes ("fixture:" scheme, generated
    header) instead of initrd files, because of the open kernel bug below; the /path file-read and the
    network resolve+GET loaders are implemented and stay in for real pages.
  - fixtures are generated, not committed binaries: scripts/gen_img_fixtures.py emits deterministic
    PNG (8x8), GIF (clear-per-pixel LZW, zero encoder cleverness), BMP (24-bit), and a 1000x800
    solid PNG (wider than the 800px viewport = the bounded case), as initrd files + the C header.
discovered:
  - id: MALLOC-TCACHE-REGRESSION (FIXED here, commit below)
    what: >
      c70ee87 (June 8, the "captures prior uncommitted multi-session source" sweep) added double-free
      detection to userspace/libc/malloc.c by marking blocks free=1 BEFORE parking them in the tcache.
      The arena first-fit walker treats any free!=0 block as allocatable, so the same memory was handed
      out twice (and split/header-rewritten) while its payload sat in the cache. Every malloc-heavy app
      corrupted: browser2 crashed at boot (cross-linked layout tree -> unbounded count_boxes recursion
      -> user stack overflow at CR2 ~ stack_top-68KB), domtest GPF'd, HTMLTEST/CSSTEST/WEBTEST FAILed
      -- all since June 8, masked because smokes only grepped rail markers. Prime suspect for
      DESKTOP-PROJECT-REGRESSION-0's color-changing stray window (aliased heap/SHM memory).
    fix: >
      three-state flag: 0=in-use, 1=arena-free, 2=in-tcache. Only state 1 may be allocated, split, or
      coalesced; free() marks 2 then caches; tcache pop marks 0; double-free detection (free!=0) kept.
  - id: INITRD-ALIAS-0 (OPEN -- needs its own kernel brick)
    what: >
      VFS reads of initrd-backed (zero-copy) files return the EXACT byte count but ALL-ZERO data when
      the reader is an mmap-heavy process (browser2: wl SHM + JS arenas). Proven not the bytes (tar on
      disk correct; tool_read reads toolset0.txt fine from the SAME 4 KiB initrd page that t.png reads
      as zeros), not the destination (stack-bounce + page-pre-touch both still zero). Everything points
      at per-process address-space aliasing over the identity/kernel mapping of the initrd region --
      the same family as the fixed higher-half/identity PD alias bug. Until fixed, initrd file content
      is untrustworthy inside mmap-heavy processes; light processes (init, tool_read) are fine.
checkpoints:
  - id: IMG0
    title: malloc fix -> pipeline green again; embedded fixtures -> the img pipeline proven
    commits: [8a0aafc, f597d3b]   # fix(malloc), feat(browser2)
    files:
      - userspace/libc/malloc.c                      # the three-state tcache fix
      - userspace/lib/layout/layout.h                # LB_IMAGE + dims-provider API + placeholder dims
      - userspace/lib/layout/layout.c                # atomic-inline <img> box, width clamp
      - userspace/apps/browser2/browser2.c           # cache/loaders/blit/placeholder/imgtest/verdict
      - userspace/apps/browser2/b2_img_fixtures.h    # generated embedded fixtures (committed for builds)
      - scripts/gen_img_fixtures.py                  # deterministic PNG/GIF/BMP/big generator
      - scripts/build_all.sh                         # header gen + initrd fixtures + imgcodec into browser2
      - userspace/init/main.c                        # spawn_args browser2 about:imgtest (non-minimal)
    tests: [build_test/browser2_img_verify.sh]
    result: >
      serial 'BROWSER2-IMG: PASS png=1 gif=1 bmp=1 missing_safe=1 bounded=1' -- 8x8 PNG/GIF/BMP decode
      and blit, the missing source draws the bordered placeholder, the 1000x800 PNG decodes and is
      clamped/clipped at the 800px viewport without overflow. Screenshot imgcheck7.png shows all five
      cases rendered in the browser2 window on a clean desktop. THE BIGGER RESULT: the whole browser
      pipeline is resurrected -- DOMTEST/HTMLTEST/CSSTEST/LAYOUTTEST/WEBTEST/WEBAPITEST all PASS and
      'BROWSER2: rendered N boxes' is back (zero browser renders + 3 FAILs + 2 crashes before the
      malloc fix). Rail green (MODELBRIDGE SKIPs cleanly without its endpoint); kernel unchanged;
      0 panic.
    review:
      default_build_changed: false      # userspace-only (malloc.c is userspace libc); kernel untouched
      all_waits_bounded: true           # bounded read/walk/decode loops; imgcodec is bounded by design
      hardware_init_gated: n/a
      touches_userspace: true
      touches_kernel: false
      preserves_known_good_t410: true   # imgtest spawn is #ifndef DESKTOP_MINIMAL; default page unchanged
      smoke_proves_claim: true          # PASS line + per-image loader lines + screenshot
      raw_pointers_or_truncation: none  # caps: 8 images, 96KB encoded, 1M px decoded, clipped blits
    verdict: pass
done: >
  BROWSER2-IMG-0 COMPLETE. browser2 renders images end-to-end through the real pipeline (source ->
  sniff -> imgcodec decode -> LB_IMAGE layout -> clipped blit), fails safe (placeholder), and stays
  bounded (clamped box + clipped blit, no panic at 1000x800). En route it un-broke the whole browser
  wave (the malloc tcache regression) and pinned INITRD-ALIAS-0 with a same-page discriminator.
next:
  - INITRD-ALIAS-0: kernel brick -- root-cause per-process aliasing of initrd-backed inode data
    (then about:imgtest can switch back to real /etc/imgtest files as a regression test).
  - JPEG baseline decoder (~1k LOC) -- the one missing codec for real pages.
  - <img> on network pages end-to-end (the resolve+GET loader exists; needs a served test page).
  - net-stack Phase 1 -> E1000-PCH-0 -> TOOL-AUTH-0/TOOL-RESULT-0 -> real llama.cpp bridge (user order).
```
