# Page Protection Bug - Visual Explanation

## Before Fix (BROKEN)

```
User tries to access 0x400000 (init's first instruction)

┌─────────────────────────────────────────────┐
│ Step 1: CPU walks page tables from CR3     │
└─────────────────────────────────────────────┘

CR3 → PML4[0]
      ├─ flags: PRESENT | WRITE  ❌ (no USER bit!)
      │
      └→ PDPT[0]
         ├─ flags: PRESENT | WRITE  ❌ (no USER bit!)
         │
         └→ PD[0]
            ├─ flags: PRESENT | WRITE  ❌ (no USER bit!)
            │
            └→ PT[0]
               ├─ flags: PRESENT | WRITE | USER  ✅
               │
               └→ Physical page 0xABCD1000

┌─────────────────────────────────────────────┐
│ Step 2: CPU checks access rights           │
└─────────────────────────────────────────────┘

CPU: "User mode trying to access 0x400000"
CPU: "Checking PML4[0]... no USER bit! 🚫"
CPU: "SUPERVISOR PAGE ACCESSED FROM USER MODE"

┌─────────────────────────────────────────────┐
│ Result: Page Fault (#PF)                   │
│ Error code: 0x05                            │
│ (P=1, W/R=0, U/S=1)                        │
└─────────────────────────────────────────────┘
```

## After Fix (WORKING)

```
User tries to access 0x400000 (init's first instruction)

┌─────────────────────────────────────────────┐
│ Step 1: CPU walks page tables from CR3     │
└─────────────────────────────────────────────┘

CR3 → PML4[0]
      ├─ flags: PRESENT | WRITE | USER  ✅
      │
      └→ PDPT[0]
         ├─ flags: PRESENT | WRITE | USER  ✅
         │
         └→ PD[0]
            ├─ flags: PRESENT | WRITE | USER  ✅
            │
            └→ PT[0]
               ├─ flags: PRESENT | WRITE | USER  ✅
               │
               └→ Physical page 0xABCD1000

┌─────────────────────────────────────────────┐
│ Step 2: CPU checks access rights           │
└─────────────────────────────────────────────┘

CPU: "User mode trying to access 0x400000"
CPU: "Checking PML4[0]... USER bit set ✅"
CPU: "Checking PDPT[0]... USER bit set ✅"
CPU: "Checking PD[0]... USER bit set ✅"
CPU: "Checking PT[0]... USER bit set ✅"
CPU: "Access granted! 🎉"

┌─────────────────────────────────────────────┐
│ Result: Page access succeeds                │
│ Init's first instruction executes           │
└─────────────────────────────────────────────┘
```

## Code Change

```c
void paging_map_page(void* virt, void* phys, uint32_t flags) {
    // ... index calculations ...

    // ✨ NEW: Propagate USER bit to intermediate tables
    uint64_t intermediate_flags = PTE_PRESENT | PTE_WRITE;
    if (flags & PAGE_USER) {
        intermediate_flags |= PTE_USER;  // ← Critical fix!
    }

    // Create PML4 → PDPT mapping
    if (!(kernel_pml4->entries[pml4_idx] & PTE_PRESENT)) {
        pdpt = alloc_page_table();
        kernel_pml4->entries[pml4_idx] = (uint64_t)pdpt | intermediate_flags;
        //                                                  ^^^^^^^^^^^^^^^^^^
        //                                                  Now includes USER!
    }

    // Create PDPT → PD mapping
    if (!(pdpt->entries[pdpt_idx] & PTE_PRESENT)) {
        pd = alloc_page_table();
        pdpt->entries[pdpt_idx] = (uint64_t)pd | intermediate_flags;
        //                                        ^^^^^^^^^^^^^^^^^^
        //                                        Now includes USER!
    }

    // Create PD → PT mapping
    if (!(pd->entries[pd_idx] & PTE_PRESENT)) {
        pt = alloc_page_table();
        pd->entries[pd_idx] = (uint64_t)pt | intermediate_flags;
        //                                    ^^^^^^^^^^^^^^^^^^
        //                                    Now includes USER!
    }

    // Map final page (this was already correct)
    pt->entries[pt_idx] = (uint64_t)phys | flags;
}
```

## Memory Protection Matrix

| Address Range | USER bit | Accessible from Ring 3? |
|---------------|----------|-------------------------|
| 0x00000000400000 (init code) | ✅ All levels | ✅ Yes |
| 0x00007FFFFFFFE000 (user stack) | ✅ All levels | ✅ Yes |
| 0xFFFFFFFF90000000 (kernel heap) | ❌ No USER bit | ❌ No (protected) |
| 0xFFFFFFFFFFFFE000 (kernel stack) | ❌ No USER bit | ❌ No (protected) |

## x86_64 Page Table Entry Format

```
Bit 63                                                           Bit 0
┌────┬─────────────────────────────────────────────────────┬─────────┐
│ NX │         Physical Address (bits 51-12)               │  Flags  │
└────┴─────────────────────────────────────────────────────┴─────────┘
       bits 51-12: Physical page address (4KB aligned)
       
       bits 0-11: Flags
       ┌───────────────────────────────────────┐
       │ 0: P   - Present                      │
       │ 1: R/W - Read/Write                   │
       │ 2: U/S - User/Supervisor ← Critical!  │
       │ 3: PWT - Page-level Write-Through     │
       │ 4: PCD - Page-level Cache Disable     │
       │ 5: A   - Accessed                     │
       │ 6: D   - Dirty                        │
       │ 7: PS  - Page Size (huge page)        │
       │ 8: G   - Global                       │
       │ 9-11:  - Available for OS use         │
       └───────────────────────────────────────┘
```

## Key Insight

The U/S bit (bit 2) is **checked at every level** of the page table walk:

```
PML4 entry U/S=0 → ❌ Access denied, even if lower levels have U/S=1
PML4 entry U/S=1 → ✅ Continue to PDPT

PDPT entry U/S=0 → ❌ Access denied, even if lower levels have U/S=1
PDPT entry U/S=1 → ✅ Continue to PD

PD entry U/S=0 → ❌ Access denied, even if PT has U/S=1
PD entry U/S=1 → ✅ Continue to PT

PT entry U/S=0 → ❌ Access denied
PT entry U/S=1 → ✅ Access granted!
```

**Rule:** U/S bit acts as a **permission gate** at each level. If ANY level denies access (U/S=0 when in user mode), the access fails regardless of lower levels.

## Intel SDM Reference

From Intel® 64 and IA-32 Architectures Software Developer's Manual, Volume 3A, Section 4.6:

> "For each paging-structure entry, bit 2 is the U/S flag. If it is 0, user-mode accesses are not allowed to the region controlled by this entry."

> "If the U/S flag is 0 in **any** paging-structure entry controlling a translation, user-mode accesses to any address in the region will result in a page-fault exception."

This is why our fix is critical: **all intermediate entries must have U/S=1** for user pages.
