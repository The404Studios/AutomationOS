/**
 * Page Table Setup for Bootloader
 * Identity map kernel and set up initial page tables
 *
 * Total: ~500 LOC
 */

#include "boot_enhanced.h"

// Page table structures (4-level paging)
typedef struct {
    uint64_t entries[512];
} __attribute__((aligned(4096))) page_table_t;

// Global page tables (allocated by bootloader)
static page_table_t* pml4 = NULL;
static page_table_t* pdpt = NULL;
static page_table_t* pd = NULL;

// Memory allocation function (from UEFI)
typedef EFI_STATUS (*AllocatePages_t)(uint32_t Type, uint32_t MemoryType,
                                     UINTN Pages, EFI_PHYSICAL_ADDRESS* Memory);
static AllocatePages_t AllocatePages = NULL;

/**
 * Initialize paging subsystem
 *
 * @param allocate_func UEFI AllocatePages function
 */
void paging_init(void* allocate_func) {
    AllocatePages = (AllocatePages_t)allocate_func;
}

/**
 * Allocate a page table
 *
 * @return Pointer to allocated page table, or NULL on error
 */
static page_table_t* alloc_page_table(void) {
    if (!AllocatePages) {
        return NULL;
    }

    EFI_PHYSICAL_ADDRESS addr = 0;
    EFI_STATUS status = AllocatePages(0, EfiLoaderData, 1, &addr);

    if (status != EFI_SUCCESS) {
        return NULL;
    }

    page_table_t* table = (page_table_t*)addr;

    // Zero the table
    for (int i = 0; i < 512; i++) {
        table->entries[i] = 0;
    }

    return table;
}

/**
 * Map a 2MB page
 *
 * @param pml4 PML4 table
 * @param virt Virtual address
 * @param phys Physical address
 * @param flags Page flags
 */
static void map_page_2mb(page_table_t* pml4_table,
                        uint64_t virt,
                        uint64_t phys,
                        uint64_t flags) {
    if (!pml4_table) {
        return;
    }

    // Extract indices
    uint64_t pml4_idx = (virt >> 39) & 0x1FF;
    uint64_t pdpt_idx = (virt >> 30) & 0x1FF;
    uint64_t pd_idx = (virt >> 21) & 0x1FF;

    // Get or create PDPT
    page_table_t* pdpt_table;
    if (pml4_table->entries[pml4_idx] & PAGE_PRESENT) {
        pdpt_table = (page_table_t*)(pml4_table->entries[pml4_idx] & ~0xFFF);
    } else {
        pdpt_table = alloc_page_table();
        if (!pdpt_table) return;
        pml4_table->entries[pml4_idx] = (uint64_t)pdpt_table | PAGE_PRESENT | PAGE_WRITE;
    }

    // Get or create PD
    page_table_t* pd_table;
    if (pdpt_table->entries[pdpt_idx] & PAGE_PRESENT) {
        pd_table = (page_table_t*)(pdpt_table->entries[pdpt_idx] & ~0xFFF);
    } else {
        pd_table = alloc_page_table();
        if (!pd_table) return;
        pdpt_table->entries[pdpt_idx] = (uint64_t)pd_table | PAGE_PRESENT | PAGE_WRITE;
    }

    // Map 2MB page
    pd_table->entries[pd_idx] = phys | flags | PAGE_SIZE_BIT;
}

/**
 * Map a 4KB page
 *
 * @param pml4 PML4 table
 * @param virt Virtual address
 * @param phys Physical address
 * @param flags Page flags
 */
static void map_page_4kb(page_table_t* pml4_table,
                        uint64_t virt,
                        uint64_t phys,
                        uint64_t flags) {
    if (!pml4_table) {
        return;
    }

    // Extract indices
    uint64_t pml4_idx = (virt >> 39) & 0x1FF;
    uint64_t pdpt_idx = (virt >> 30) & 0x1FF;
    uint64_t pd_idx = (virt >> 21) & 0x1FF;
    uint64_t pt_idx = (virt >> 12) & 0x1FF;

    // Get or create PDPT
    page_table_t* pdpt_table;
    if (pml4_table->entries[pml4_idx] & PAGE_PRESENT) {
        pdpt_table = (page_table_t*)(pml4_table->entries[pml4_idx] & ~0xFFF);
    } else {
        pdpt_table = alloc_page_table();
        if (!pdpt_table) return;
        pml4_table->entries[pml4_idx] = (uint64_t)pdpt_table | PAGE_PRESENT | PAGE_WRITE;
    }

    // Get or create PD
    page_table_t* pd_table;
    if (pdpt_table->entries[pdpt_idx] & PAGE_PRESENT) {
        pd_table = (page_table_t*)(pdpt_table->entries[pdpt_idx] & ~0xFFF);
    } else {
        pd_table = alloc_page_table();
        if (!pd_table) return;
        pdpt_table->entries[pdpt_idx] = (uint64_t)pd_table | PAGE_PRESENT | PAGE_WRITE;
    }

    // Get or create PT
    page_table_t* pt_table;
    if (pd_table->entries[pd_idx] & PAGE_PRESENT) {
        pt_table = (page_table_t*)(pd_table->entries[pd_idx] & ~0xFFF);
    } else {
        pt_table = alloc_page_table();
        if (!pt_table) return;
        pd_table->entries[pd_idx] = (uint64_t)pt_table | PAGE_PRESENT | PAGE_WRITE;
    }

    // Map 4KB page
    pt_table->entries[pt_idx] = phys | flags;
}

/**
 * Set up page tables for kernel
 * Identity map first 4GB and map kernel to higher half
 *
 * @param kernel_phys Physical address of kernel
 * @param kernel_size Size of kernel in bytes
 * @return Pointer to PML4 (to load into CR3), or NULL on error
 */
void* setup_page_tables(uint64_t kernel_phys, uint64_t kernel_size) {
    // Create PML4
    pml4 = alloc_page_table();
    if (!pml4) {
        return NULL;
    }

    // Identity map first 4GB (for bootloader and low memory)
    // Use 2MB pages for efficiency
    for (uint64_t addr = 0; addr < 0x100000000ULL; addr += PAGE_SIZE_2MB) {
        map_page_2mb(pml4, addr, addr, PAGE_PRESENT | PAGE_WRITE);
    }

    // Map kernel to higher half (0xFFFFFFFF80000000)
    // Use 2MB pages
    uint64_t pages = (kernel_size + PAGE_SIZE_2MB - 1) / PAGE_SIZE_2MB;
    for (uint64_t i = 0; i < pages; i++) {
        uint64_t phys = kernel_phys + (i * PAGE_SIZE_2MB);
        uint64_t virt = KERNEL_VMA + (i * PAGE_SIZE_2MB);
        map_page_2mb(pml4, virt, phys, PAGE_PRESENT | PAGE_WRITE);
    }

    return pml4;
}

/**
 * Load page tables into CR3
 *
 * @param pml4_addr Physical address of PML4
 */
static inline void load_cr3(uint64_t pml4_addr) {
    __asm__ volatile("mov %0, %%cr3" : : "r"(pml4_addr) : "memory");
}

/**
 * Enable paging (already enabled by UEFI, but this is for reference)
 */
static inline void enable_paging(void) {
    uint64_t cr0;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 |= (1ULL << 31);  // PG bit
    __asm__ volatile("mov %0, %%cr0" : : "r"(cr0) : "memory");
}

/**
 * Get current CR3 value
 *
 * @return Current CR3 value
 */
uint64_t get_cr3(void) {
    uint64_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    return cr3;
}

/**
 * Map framebuffer into page tables
 *
 * @param pml4 PML4 table
 * @param fb_addr Physical framebuffer address
 * @param fb_size Framebuffer size in bytes
 */
void paging_map_framebuffer(void* pml4_table, uint64_t fb_addr, uint64_t fb_size) {
    if (!pml4_table) {
        return;
    }

    // Map framebuffer using 2MB pages
    uint64_t pages = (fb_size + PAGE_SIZE_2MB - 1) / PAGE_SIZE_2MB;
    for (uint64_t i = 0; i < pages; i++) {
        uint64_t phys = fb_addr + (i * PAGE_SIZE_2MB);
        map_page_2mb((page_table_t*)pml4_table, phys, phys,
                    PAGE_PRESENT | PAGE_WRITE);
    }
}

/**
 * Map ACPI tables into page tables
 *
 * @param pml4 PML4 table
 * @param acpi_addr Physical ACPI address
 * @param acpi_size ACPI region size
 */
void paging_map_acpi(void* pml4_table, uint64_t acpi_addr, uint64_t acpi_size) {
    if (!pml4_table) {
        return;
    }

    // Map ACPI region using 4KB pages (for precision)
    uint64_t pages = (acpi_size + PAGE_SIZE - 1) / PAGE_SIZE;
    for (uint64_t i = 0; i < pages; i++) {
        uint64_t phys = acpi_addr + (i * PAGE_SIZE);
        map_page_4kb((page_table_t*)pml4_table, phys, phys,
                    PAGE_PRESENT | PAGE_WRITE);
    }
}

/**
 * Print page table info (for debugging)
 *
 * @param pml4 PML4 table
 * @param print_func Function to print strings
 */
void paging_print_info(void* pml4_table, void (*print_func)(const char*)) {
    if (!pml4_table || !print_func) {
        return;
    }

    print_func("Page Table Information:\n");
    print_func("  PML4 address: 0x");

    // Print PML4 address in hex
    char buf[32];
    uint64_t addr = (uint64_t)pml4_table;
    for (int i = 0; i < 16; i++) {
        int digit = (addr >> (60 - i * 4)) & 0xF;
        buf[i] = (digit < 10) ? ('0' + digit) : ('A' + digit - 10);
    }
    buf[16] = '\n';
    buf[17] = 0;
    print_func(buf);

    // Count entries
    page_table_t* pml4 = (page_table_t*)pml4_table;
    int pml4_entries = 0;
    for (int i = 0; i < 512; i++) {
        if (pml4->entries[i] & PAGE_PRESENT) {
            pml4_entries++;
        }
    }

    print_func("  PML4 entries: ");
    buf[0] = '0' + (pml4_entries / 100);
    buf[1] = '0' + ((pml4_entries / 10) % 10);
    buf[2] = '0' + (pml4_entries % 10);
    buf[3] = '\n';
    buf[4] = 0;
    print_func(buf);
}
