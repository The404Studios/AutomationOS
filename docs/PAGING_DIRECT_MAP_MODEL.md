# Kernel Paging / Direct-Map Model (and the #20 alias-coherence bug)

Phase-1 deliverable for fixing #20 (the direct-map / phys==virt alias corruption that
the SMP stress harness reproduces). This documents the **current** model from the code,
so the fix targets the architecture, not `matmul_band_n`. All citations are file:line.

Repro of the bug this explains: `IDE=1 bash scripts/build_all.sh && bash scripts/smp_smoke.sh`
→ CPU1 `#PF` (err=0x8) in `matmul_band_n` reading an offload operand buffer at a
**direct-map** address (`0xffff80000b92f048`, phys ~194MB) after smpstress's heap churn.

---

## The six questions

**1. Low identity map — permanent? range? page size.**
PERMANENT at runtime. Built by the bootloader (`boot/paging.c:177-181`, identity-maps
0–4GB with 2MB huge pages) and EXTENDED by the kernel to 16GB (`kernel/arch/x86_64/paging.c:174-206`).
Lives at `PML4[0] → PDPT[0] → PD(s)`, phys 0–16GB, **2MB huge pages**. Never torn down.

**2. Higher-half direct map — where, range, permanence, macro.**
`PHYS_TO_DIRECT(p) = DIRECT_MAP_BASE + p`, `DIRECT_MAP_BASE = 0xFFFF800000000000`,
span 16GB (`kernel/include/mem.h:24-26`). Built in `paging_init` (`paging.c:262-266`) by
**aliasing the identity PDPT into PML4[256]** (supervisor-only: USER cleared; NX set).
Permanent. A boot self-test writes via the direct map and reads via low-identity to prove
they hit the same RAM (`paging.c:268-287`).

**3. CRUX — kernel half SHARED or COPIED per process?**
The kernel HIGH half (PML4[256–511]) is **SHARED BY REFERENCE**: `paging_create_address_space`
sets `child_pml4->entries[i] = kernel_pml4->entries[i]` for `i ∈ [256,512)` (`paging.c:826-827`;
comment `paging.c:253-254`), so every CR3 points at the SAME physical PDPT/PD/PT pages for
the kernel half. The LOW half (PML4[0–255], incl. the low identity map) is **DEEP-COPIED**
per process (`paging.c:838-875`); `paging_destroy_address_space` frees only 0–255.
→ A later edit to a SHARED kernel-half table page is visible in EVERY CR3 (incl. the AP's).

**4. Huge-PDE split — which hierarchy, does it propagate?**
`paging_map_page` splits a 2MB huge PDE into a 512-entry PT and writes the PT pointer into
`pd->entries[pd_idx]` of the **`active_pml4`** target (`paging.c:388-412`). For kernel-heap
growth `active_pml4 == kernel_pml4`, so the edit lands in the shared kernel half and DOES
propagate to all CR3s (incl. the AP). The kernel heap lives in its OWN un-aliased PD at the
top of the higher half (PML4[511]), separate from the identity/direct-map PD.

**5. AP / CPU1 CR3.**
The AP runs on the BSP's **master kernel CR3**, captured ONCE at AP boot
(`ap_boot.c:1396 read_cr3()` → `1327` trampoline `AP_PARAM_CR3`). Same physical PML4 as the
BSP's `kernel_pml4`; never updated after boot. The AP only runs kernel job callbacks
(coprocessor mode), so it never calls `paging_map_page` itself.

**6. Ownership of the roots.**
`kernel_pml4` = master (`paging.c:20`), authoritative. `active_pml4` = current
`paging_map_page` target (`paging.c:21`, set by `paging_set_target` `:301`). No separate
`kernel_cr3` global (`paging_kernel_cr3()` returns `(uint64_t)kernel_pml4`). `paging_map_page`
reads/writes page-table pages via **raw physical pointers** (the low-identity assumption,
e.g. `paging.c:354,370,414`) — correct only while the low identity for those PT-page
physical addresses is intact and unsplit.

---

## PML4 layout (current)

```
PML4[0]        → identity PDPT  → PD(s): phys 0..16GB as 2MB HUGE.  DEEP-COPIED per process.
PML4[256]      → THE SAME identity PDPT (aliased), USER-cleared + NX. SHARED by reference.  ← direct map
PML4[257..510] → other shared kernel higher-half (per-CPU, MMIO, etc). SHARED by reference.
PML4[511]      → kernel heap / image PDPT (own un-aliased PD). SHARED by reference.
```

`kmalloc_ref` buffers are PMM frames addressed via `PHYS_TO_DIRECT` → resolved through
**PML4[256] → identity PDPT → identity PD → 2MB huge PDE**. That is the exact path that
faults on CPU1.

---

## Why it breaks (hypothesis, evidence-backed)

The direct map (PML4[256], shared) and the low identity (PML4[0], deep-copied) **share one
PDPT → one PD → one set of 2MB huge PDEs**. The boot comment asserts the invariant *"the
shared direct-map PDPT is NEVER mutated by a process's exec huge-page splits (those touch
only its private PML4[0])."* The harness shows that invariant does NOT hold under stress:

- Software walk of CPU1's CR3 reads the operand's PDE as present-huge (`e=0xb8000e3`).
- Hardware walk of the SAME CR3/VA faults `err=0x8`, microseconds later, BSP blocked.
- Ruled out: lifetime, refcount, pointer-domain, publish/consume ordering, stale-TLB
  (a CPU1 CR3 reload before the read did NOT fix it).

That leaves an **alias incoherence in the shared identity/direct-map PD**: something splits
or mutates a 2MB huge PDE that the direct map depends on (or the software-walk's low-identity
view diverges from the hardware's), so a direct-map read of a frame in that 2MB window
faults even though the PDE *looks* present-huge. The exact mutator under offload churn is the
open question (suspects: a low-VA 4KB mapping that splits the shared identity PD; the
`[SLABPROBE] split from huge kernel PDE` path; PMM/heap touching that PD).

---

## Recommended invariant + fix direction (Phase 2/4 — next session)

**Invariant: there is exactly ONE authoritative, never-split mapping for "all physical RAM"
— the direct map — and it does NOT share structure with the splittable identity map.**

Concretely:
- **Give the direct map (PML4[256]) its OWN dedicated PDPT + PD chain** of stable 2MB (or
  1GB) huge pages covering phys 0..N, instead of *aliasing* the identity PDPT. Then a split
  of the identity PD (driven by a process mapping a low 4KB VA) can never perturb the direct
  map, and `PHYS_TO_DIRECT` always resolves via a stable huge page on every CR3 (incl. the
  AP). This keeps the existing "shared higher-half, no per-access CR3 switch" benefit while
  removing the dangerous structural coupling.
- Make `paging_map_page` (and any page-table walk) read/edit table pages via the **direct
  map** (`PHYS_TO_DIRECT`), not the raw-physical/low-identity assumption, so software walks
  and hardware always agree even if the low identity is later split/retired.
- Longer term (Phase 5): stop relying on the permanent low identity for kernel runtime;
  restrict it to boot/AP-trampoline.

**Do NOT** patch around `matmul_band_n`, add CR3 reloads, or add allocator "avoid this region
on CPU1" hacks — those mask the architectural alias, not fix it.

## Gate before resuming SMP scaling
`smp_smoke.sh` green (`SMPSTRESS: PASS`, `CPU1OFFLOAD: PASS`, no `#PF` in `matmul_band_n`),
plus a permanent alias-coherence test (`PAGINGALIAS: PASS`: write via direct map on CPU0,
read via direct map on CPU1 across the 2MB-split region; fail if software-walk and hardware
access disagree). Only then: per-CPU `rq_lock` → ring-3 dispatch → IPI/TLB.
