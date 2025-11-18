/*! @file memory.c
    @brief Physical and virtual memory manager
    @copyright Copyright (c) 2024-2025 University of Illinois
    @license SPDX-License-identifier: NCSA

*/

#ifdef MEMORY_TRACE
#define TRACE
#endif

#ifdef MEMORY_DEBUG
#define DEBUG
#endif

#include "memory.h"

#include "conf.h"
#include "console.h"
#include "error.h"
#include "heap.h"
#include "misc.h"
#include "process.h"
#include "riscv.h"
#include "string.h"
#include "thread.h"

// COMPILE-TIME CONFIGURATION
//

// Minimum amount of memory in the initial heap block.

#ifndef HEAP_INIT_MIN
#define HEAP_INIT_MIN 256
#endif

// INTERNAL CONSTANT DEFINITIONS
//

#define MEGA_SIZE ((1UL << 9) * PAGE_SIZE)  // megapage size
#define GIGA_SIZE ((1UL << 9) * MEGA_SIZE)  // gigapage size

#define PTE_ORDER 3
#define PTE_CNT (1U << (PAGE_ORDER - PTE_ORDER)) // 2^(12-3) = 2^9 = 512 page table entries/ table page

#ifndef PAGING_MODE
#define PAGING_MODE RISCV_SATP_MODE_Sv39
#endif

#ifndef ROOT_LEVEL
#define ROOT_LEVEL 2
#endif

// IMPORTED GLOBAL SYMBOLS
//

// linker-provided (kernel.ld)
extern char _kimg_start[];
extern char _kimg_text_start[];
extern char _kimg_text_end[];
extern char _kimg_rodata_start[];
extern char _kimg_rodata_end[];
extern char _kimg_data_start[];
extern char _kimg_data_end[];
extern char _kimg_end[];

// EXPORTED GLOBAL VARIABLES
//

char memory_initialized = 0;

// INTERNAL TYPE DEFINITIONS
//

// We keep free physical pages in a linked list of _chunks_, where each chunk
// consists of several consecutive pages of memory. Initially, all free pages
// are in a single large chunk. To allocate a block of pages, we break up the
// smallest chunk on the list.

/**
 * @brief Section of consecutive physical pages. We keep free physical pages in a
 * linked list of chunks. Initially, all free pages are in a single large chunk. To
 * allocate a block of pages, we break up the smallest chunk in the list
 */
struct page_chunk {
    struct page_chunk *next;  ///< Next page in list
    unsigned long pagecnt;    ///< Number of pages in chunk
};

/**
 * @brief RISC-V PTE. RTDC (RISC-V docs) for what each of these fields means!
 */
struct pte {
    uint64_t flags : 8;
    uint64_t rsw : 2;
    uint64_t ppn : 44;
    uint64_t reserved : 7;
    uint64_t pbmt : 2;
    uint64_t n : 1;
};

// INTERNAL MACRO DEFINITIONS
//

#define VPN(vma) ((vma) / PAGE_SIZE)
#define VPN2(vma) ((VPN(vma) >> (2 * 9)) % PTE_CNT)
#define VPN1(vma) ((VPN(vma) >> (1 * 9)) % PTE_CNT)
#define VPN0(vma) ((VPN(vma) >> (0 * 9)) % PTE_CNT)

// The following macros test is a PTE is valid, global, or a leaf. The argument
// is a struct pte (*not* a pointer to a struct pte).

#define PTE_VALID(pte) (((pte).flags & PTE_V) != 0)
#define PTE_GLOBAL(pte) (((pte).flags & PTE_G) != 0)
#define PTE_LEAF(pte) (((pte).flags & (PTE_R | PTE_W | PTE_X)) != 0)

#define PT_INDEX(lvl, vpn) \
    (((vpn) & (0x1FF << (lvl * (PAGE_ORDER - PTE_ORDER)))) >> (lvl * (PAGE_ORDER - PTE_ORDER)))
// INTERNAL FUNCTION DECLARATIONS
//
/* UNUSED
static void ptab_reset(struct pte *ptab  // page table to reset
);

static struct pte *ptab_clone(struct pte *ptab  // page table to clone
);

static void ptab_discard(struct pte *ptab  // page table to discard
);
*/
static void ptab_insert(struct pte *ptab,   // page table to modify
                        unsigned long vpn,  // virtual page number to insert
                        void *pp,           // pointer to physical page to insert
                        int rwxug_flags     // flags for inserted mapping
);

static void *ptab_remove(struct pte *ptab, unsigned long vpn);

static void ptab_adjust(struct pte *ptab, unsigned long vpn, int rwxug_flags);

struct pte *ptab_fetch(struct pte *ptab, unsigned long vpn);

static inline mtag_t active_space_mtag(void);
static inline mtag_t ptab_to_mtag(struct pte *root, unsigned int asid);
static inline struct pte *mtag_to_ptab(mtag_t mtag);
static inline struct pte *active_space_ptab(void);

static inline void *pageptr(uintptr_t n);
static inline uintptr_t pagenum(const void *p);
static inline int wellformed(uintptr_t vma);

static inline struct pte leaf_pte(const void *pp, uint_fast8_t rwxug_flags);
static inline struct pte ptab_pte(const struct pte *pt, uint_fast8_t g_flag);
static inline struct pte null_pte(void);

// INTERNAL GLOBAL VARIABLES
//

static mtag_t main_mtag;

static struct pte main_pt2[PTE_CNT] __attribute__((section(".bss.pagetable"), aligned(4096)));

static struct pte main_pt1_0x80000[PTE_CNT]
    __attribute__((section(".bss.pagetable"), aligned(4096)));

static struct pte main_pt0_0x80000[PTE_CNT]
    __attribute__((section(".bss.pagetable"), aligned(4096)));

static struct page_chunk *free_chunk_list;

// EXPORTED FUNCTION DECLARATIONS
//

void memory_init(void) {
    const void *const text_start = _kimg_text_start;
    const void *const text_end = _kimg_text_end;
    const void *const rodata_start = _kimg_rodata_start;
    const void *const rodata_end = _kimg_rodata_end;
    const void *const data_start = _kimg_data_start;

    void *heap_start;
    void *heap_end;

    uintptr_t pma;
    const void *pp;

    trace("%s()", __func__);

    assert(RAM_START == _kimg_start);

    debug("           RAM: [%p,%p): %zu MB", RAM_START, RAM_END, RAM_SIZE / 1024 / 1024);
    debug("  Kernel image: [%p,%p)", _kimg_start, _kimg_end);

    // Kernel must fit inside 2MB megapage (one level 1 PTE)

    if (MEGA_SIZE < _kimg_end - _kimg_start) panic(NULL);

    // Initialize main page table with the following direct mapping:
    //
    //         0 to RAM_START:           RW gigapages (MMIO region)
    // RAM_START to _kimg_end:           RX/R/RW pages based on kernel image
    // _kimg_end to RAM_START+MEGA_SIZE: RW pages (heap and free page pool)
    // RAM_START+MEGA_SIZE to RAM_END:   RW megapages (free page pool)
    //
    // RAM_START = 0x80000000
    // MEGA_SIZE = 2 MB
    // GIGA_SIZE = 1 GB

    // Identity mapping of MMIO region as two gigapage mappings
    for (pma = 0; pma < RAM_START_PMA; pma += GIGA_SIZE)
        main_pt2[VPN2(pma)] = leaf_pte((void *)pma, PTE_R | PTE_W | PTE_G);

    // Third gigarange has a second-level subtable
    main_pt2[VPN2(RAM_START_PMA)] = ptab_pte(main_pt1_0x80000, PTE_G);

    // First physical megarange of RAM is mapped as individual pages with
    // permissions based on kernel image region.

    main_pt1_0x80000[VPN1(RAM_START_PMA)] = ptab_pte(main_pt0_0x80000, PTE_G);

    for (pp = text_start; pp < text_end; pp += PAGE_SIZE) {
        main_pt0_0x80000[VPN0((uintptr_t)pp)] = leaf_pte(pp, PTE_R | PTE_X | PTE_G);
    }

    for (pp = rodata_start; pp < rodata_end; pp += PAGE_SIZE) {
        main_pt0_0x80000[VPN0((uintptr_t)pp)] = leaf_pte(pp, PTE_R | PTE_G);
    }

    for (pp = data_start; pp < RAM_START + MEGA_SIZE; pp += PAGE_SIZE) {
        main_pt0_0x80000[VPN0((uintptr_t)pp)] = leaf_pte(pp, PTE_R | PTE_W | PTE_G);
    }

    // Remaining RAM mapped in 2MB megapages

    for (pp = RAM_START + MEGA_SIZE; pp < RAM_END; pp += MEGA_SIZE) {
        main_pt1_0x80000[VPN1((uintptr_t)pp)] = leaf_pte(pp, PTE_R | PTE_W | PTE_G);
    }

    // Enable paging; this part always makes me nervous.

    main_mtag = ptab_to_mtag(main_pt2, 0);
    csrw_satp(main_mtag);

    // Give the memory between the end of the kernel image and the next page
    // boundary to the heap allocator, but make sure it is at least
    // HEAP_INIT_MIN bytes.

    heap_start = _kimg_end;
    heap_end = (void *)ROUND_UP((uintptr_t)heap_start, PAGE_SIZE);

    if (heap_end - heap_start < HEAP_INIT_MIN) {
        heap_end += ROUND_UP(HEAP_INIT_MIN - (heap_end - heap_start), PAGE_SIZE);
    }

    if (RAM_END < heap_end) panic("out of memory");

    // Initialize heap memory manager

    heap_init(heap_start, heap_end);

    debug("Heap allocator: [%p,%p): %zu KB free", heap_start, heap_end,
          (heap_end - heap_start) / 1024);

    // FIXME: Initialize the free chunk list here
    free_chunk_list = (struct page_chunk *)heap_end;
    free_chunk_list->next = NULL;
    free_chunk_list->pagecnt = (uintptr_t)((char *)RAM_END - (char *)heap_end) / PAGE_SIZE;

    // Allow supervisor to access user memory. We could be more precise by only
    // enabling supervisor access to user memory when we are explicitly trying
    // to access user memory, and disable it at other times. This would catch
    // bugs that cause inadvertent access to user memory (due to bugs).

    csrs_sstatus(RISCV_SSTATUS_SUM);

    memory_initialized = 1;
}

mtag_t active_mspace(void) { return active_space_mtag(); }

mtag_t switch_mspace(mtag_t mtag) {
    mtag_t prev;

    prev = csrrw_satp(mtag);
    sfence_vma();
    return prev;
}

mtag_t clone_active_mspace(void) {
    // FIXME
    struct pte * old_root_table = active_space_ptab();
    struct pte * new_root_table = alloc_phys_page();
    
    // loop over and copy level 2 page table entries
    for (size_t i = 0; i < PTE_CNT; i++) {
        struct pte old_pte = old_root_table[i];

        // (1) invalid pte
        if (!PTE_VALID(old_pte)) {
            new_root_table[i] = null_pte();
            continue;
        }
        // (2) global pte, so share the same physical page numbers
        if (old_pte.flags & PTE_G) {
            new_root_table[i] = old_pte;
            continue;
        }
        // (3) gigapage pte (non-global)
        if (PTE_LEAF(old_pte)) {
            // clone physical pages
            void *old_page = pageptr(old_pte.ppn);
            void *new_page = alloc_phys_pages(GIGA_SIZE/PAGE_SIZE);
            memcpy(new_page, old_page, GIGA_SIZE);

            // create new level 2 pte
            new_root_table[i] = leaf_pte(new_page, old_pte.flags);
            continue;
        }
       
        // (4) level 1 page table (non-global)
        struct pte* old_level1_table = (struct pte *)pageptr(old_pte.ppn);
        struct pte* new_level1_table = alloc_phys_page();

        // loop over and copy level 1 page table entries
        for (size_t j = 0; j < PTE_CNT; j++) {
            struct pte old_pte_l1 = old_level1_table[j];

            // (a) invalid pte
            if (!PTE_VALID(old_pte_l1)) {
                new_level1_table[j] = null_pte();
                continue;
            }
            // (b) global pte
            if (old_pte_l1.flags & PTE_G) {
                new_level1_table[j] = old_pte_l1;
                continue;
            }
            // (c) megapage pte (non-global)
            if (PTE_LEAF(old_pte_l1)) {
                // clone physical pages
                void *old_page = pageptr(old_pte_l1.ppn);
                void *new_page = alloc_phys_pages(MEGA_SIZE/PAGE_SIZE);
                memcpy(new_page, old_page, MEGA_SIZE);

                // create new level 1 pte
                new_level1_table[j] = leaf_pte(new_page, old_pte_l1.flags);
                continue;
            }

            // (d) level 0 page table (non-global)
            struct pte* old_level0_table = (struct pte *)pageptr(old_pte_l1.ppn);
            struct pte* new_level0_table = alloc_phys_page();

            // loop over and copy level 0 page table entries
            for (size_t k = 0; k < PTE_CNT; k++) {
                struct pte old_pte_l0 = old_level0_table[k];

                // (i) invalid pte
                if (!PTE_VALID(old_pte_l0)) {
                    new_level0_table[k] = null_pte();
                    continue;
                }
                // (ii) global pte
                if (old_pte_l0.flags & PTE_G) {
                    new_level0_table[k] = old_pte_l0;
                    continue;
                }
                // (iii) page (non-global)
                void *old_page = pageptr(old_pte_l0.ppn);
                void *new_page = alloc_phys_page();
                memcpy(new_page, old_page, PAGE_SIZE);

                new_level0_table[k] = leaf_pte(new_page, old_pte_l0.flags);
            }

            new_level1_table[j] = ptab_pte(new_level0_table, old_pte_l1.flags & PTE_G);
        }

        new_root_table[i] = ptab_pte(new_level1_table, old_pte.flags & PTE_G);
    }

    return ptab_to_mtag(new_root_table, 0);
}

void reset_active_mspace(void) {
    // FIXME
    struct pte * root_table = active_space_ptab();
    size_t vpn = VPN2(0xC0000000); // = 3
    struct pte root_pte = root_table[vpn]; // get the pte that coresponds to the pages in the user range

    if (!PTE_VALID(root_pte)) return;
    
    // It's a gigapage
    if (PTE_LEAF(root_pte)) { 
        if (root_pte.flags & PTE_U) {
            void * pp = pageptr(root_pte.ppn);
            free_phys_pages(pp, GIGA_SIZE/PAGE_SIZE);
            root_table[vpn] = null_pte();
        }
        sfence_vma();
        return;
    } 

    // Get the level 1 table corresponding to the user space
    struct pte* level1_table = (struct pte *)pageptr(root_pte.ppn);
    // Umap and free the user space
    for (size_t i = 0; i < PTE_CNT; i++) {
        struct pte level1_pte = level1_table[i];

        // (1) skip invalid
        if (!PTE_VALID(level1_pte)) continue;

        // (2) megapage entry
        if (PTE_LEAF(level1_pte)) {
            if (level1_pte.flags & PTE_U) {
                void * pp = pageptr(level1_pte.ppn);
                free_phys_pages(pp, MEGA_SIZE/PAGE_SIZE);
                level1_table[i] = null_pte();
            }
            continue;
        } 
        // (3) not a megapage entry, walk through the level 0 table
        struct pte* level0_table = (struct pte *)pageptr(level1_pte.ppn);
        for (size_t j = 0; j < PTE_CNT; j++) {
            struct pte level0_pte = level0_table[j];

            // skip invalid
            if (!PTE_VALID(level0_pte)) continue;

            if (level0_pte.flags & PTE_U) {
                void * pp = pageptr(level0_pte.ppn);
                free_phys_page(pp);
                level0_table[j] = null_pte();
            }       
        }
        // assume that all level 0 pte are actually user-accessible and were freed and nullified,
        // so free the level 0 table and nullify the level 1 pte
        free_phys_page(level0_table);
        level1_table[i] = null_pte();
    }

    // assume that all level 1 pte are actually user-accessible and were freed and nullified
    // so free the level 1 table and nullify the level 2 pte
    free_phys_page(level1_table);
    root_table[vpn] = null_pte();

    sfence_vma();
    return;
}

mtag_t discard_active_mspace(void) {
    // FIXME

    // unmap and free all non-global pages
    struct pte * root_table = active_space_ptab();
    reset_active_mspace();

    // switch memory space to main
    switch_mspace(main_mtag);
    free_phys_page(root_table);

    return main_mtag;
}

// The map_page() function maps a single page into the active address space at
// the specified address. The map_range() function maps a range of contiguous
// pages into the active address space. Note that map_page() is a special case
// of map_range(), so it can be implemented by calling map_range(). Or
// map_range() can be implemented by calling map_page() for each page in the
// range. The current implementation does the latter.

// We currently map 4K pages only. At some point it may be disirable to support
// mapping megapages and gigapages.

static void ptab_insert(struct pte *ptab,   // page table to modify
    unsigned long vpn,  // virtual page number to insert
    void *pp,           // pointer to physical page to insert
    int rwxug_flags     // flags for inserted mapping
) {
    size_t vpn2 = PT_INDEX(2, vpn);
    size_t vpn1 = PT_INDEX(1, vpn);
    size_t vpn0 = PT_INDEX(0, vpn);

    // (1) go to level 1 page table

    // create the table if it hasn't been created yet
    if (!PTE_VALID(ptab[vpn2])) {
        struct pte *level1_table = alloc_phys_page();
        // initialize table entries to 0
        for (size_t i = 0; i < PTE_CNT; i++) {
            level1_table[i] = null_pte();
        }
        ptab[vpn2] = ptab_pte(level1_table, 0);
    } else if (PTE_LEAF(ptab[vpn2])) panic("ptab_insert: vpn2 entry is a gigapage");

    struct pte *level1_table = (struct pte *)pageptr(ptab[vpn2].ppn);

    // (2) go to level 0 page table

    // create the table if it hasn't been created yet
    if (!PTE_VALID(level1_table[vpn1])) {
        struct pte *level0_table = alloc_phys_page();
        for (size_t i = 0; i < PTE_CNT; i++) {
            level0_table[i] = null_pte();
        }
        level1_table[vpn1] = ptab_pte(level0_table, 0);
    } else if (PTE_LEAF(level1_table[vpn1])) panic("ptab_insert: vpn1 entry is a megapage");
    
    struct pte *level0_table = (struct pte *)pageptr(level1_table[vpn1].ppn);

    // (3) map page
    level0_table[vpn0] = leaf_pte(pp, rwxug_flags);

}

/*
    inputs: ptab        - root page table of the memory space
            vpn         - virtual page number
            rwxug_flags - page table entry flags
    description: modifies the page table entry corresponding to the vpn in the ptab so that they are rwxug_flags
*/
static void ptab_adjust(struct pte *ptab, unsigned long vpn, int rwxug_flags) {
    size_t vpn2 = PT_INDEX(2, vpn);
    size_t vpn1 = PT_INDEX(1, vpn);
    size_t vpn0 = PT_INDEX(0, vpn);

    // (1) go to level 1 page table
    if (!PTE_VALID(ptab[vpn2])) panic("ptab_adjust: vpn2 entry is invalid");
    else if (PTE_LEAF(ptab[vpn2])) panic("ptab_adjust: vpn2 entry is a gigapage");

    struct pte *level1_table = (struct pte *)pageptr(ptab[vpn2].ppn);

    // (2) go to level 0 page table
    if (!PTE_VALID(level1_table[vpn1])) panic("ptab_adjust: vpn1 entry is invalid");
    else if (PTE_LEAF(level1_table[vpn1])) panic("ptab_adjust: vpn1 entry is a megapage");

    struct pte *level0_table = (struct pte *)pageptr(level1_table[vpn1].ppn);

    // (3) adjust flags of entry
    if (!PTE_VALID(level0_table[vpn0])) panic("ptab_adjust: vpn0 entry is invalid");
    level0_table[vpn0].flags = rwxug_flags;
}
/*
    inputs: ptab        - root page table of the memory space
            vpn         - virtual page number
    output: pointer to the physical page so that it can be freed by the caller
    description: unmaps the physical page corresponding to vpn in ptab
*/
static void *ptab_remove(struct pte *ptab, unsigned long vpn) {
    size_t vpn2 = PT_INDEX(2, vpn);
    size_t vpn1 = PT_INDEX(1, vpn);
    size_t vpn0 = PT_INDEX(0, vpn);

    // (1) go to level 1 page table
    if (!PTE_VALID(ptab[vpn2])) return NULL;
    else if (PTE_LEAF(ptab[vpn2])) panic("ptab_remove: vpn2 entry is a gigapage");

    struct pte *level1_table = (struct pte *)pageptr(ptab[vpn2].ppn);

    // (2) go to level 0 page table
    if (!PTE_VALID(level1_table[vpn1])) return NULL;
    else if (PTE_LEAF(level1_table[vpn1])) panic("ptab_remove: vpn1 entry is a megapage");

    struct pte *level0_table = (struct pte *)pageptr(level1_table[vpn1].ppn);

    // (3) umap the physical page and return
    if (!PTE_VALID(level0_table[vpn0])) return NULL;
    
    void *pp = pageptr(level0_table[vpn0].ppn);

    level0_table[vpn0] = null_pte();

    return pp;
}

/*
    inputs: ptab        - root page table of the memory space
            vpn         - virtual page number
    output: pointer to the page table entry that corresponds to the vpn in ptab
*/
struct pte *ptab_fetch(struct pte *ptab, unsigned long vpn) {
    size_t vpn2 = PT_INDEX(2, vpn);
    size_t vpn1 = PT_INDEX(1, vpn);
    size_t vpn0 = PT_INDEX(0, vpn);

    // (1) go to level 1 page table
    if (!PTE_VALID(ptab[vpn2])) return NULL;
    else if (PTE_LEAF(ptab[vpn2])) return &ptab[vpn2]; // gigapage

    struct pte *level1_table = (struct pte *)pageptr(ptab[vpn2].ppn);

    // (2) go to level 0 page table
    if (!PTE_VALID(level1_table[vpn1])) return NULL;
    else if (PTE_LEAF(level1_table[vpn1])) return &level1_table[vpn1]; // megapage

    struct pte *level0_table = (struct pte *)pageptr(level1_table[vpn1].ppn);

    // (3) return the page table entry
    if (!PTE_VALID(level0_table[vpn0])) return NULL;

    return &level0_table[vpn0];
}


void *map_page(uintptr_t vma, void *pp, int rwxug_flags) {
    // FIXME
    if (vma % PAGE_SIZE != 0) panic("map_page: vma is not page-aligned");
    if ((uintptr_t)pp % PAGE_SIZE != 0) panic("map_page: pp is not page-aligned");
    if (!wellformed(vma)) panic("map_page: vma is not well-formed");

    map_range(vma, PAGE_SIZE, pp, rwxug_flags);

    return (void*)vma;
}

void *map_range(uintptr_t vma, size_t size, void *pp, int rwxug_flags) {
    // FIXME
    if (vma % PAGE_SIZE != 0) panic("map_range: vma is not page-aligned");
    if ((uintptr_t)pp % PAGE_SIZE != 0) panic("map_range: pp is not page-aligned");
    if (!wellformed(vma)) panic("map_range: vma is not well-formed");

    // round up size to be a multiple of PAGE_SIZE
    size = ROUND_UP(size, PAGE_SIZE);
    
    struct pte *root_table = active_space_ptab();

    for (size_t i = 0; i < size / PAGE_SIZE; i ++) {
        ptab_insert(root_table, VPN(vma) + i, (void*)((uintptr_t)pp + PAGE_SIZE * i), rwxug_flags);
    }
    sfence_vma();

    return (void*)vma;
}

void *alloc_and_map_range(uintptr_t vma, size_t size, int rwxug_flags) {
    // FIXME

    // round up size to be a multiple of PAGE_SIZE
    size = ROUND_UP(size, PAGE_SIZE);

    // allocate physical pages
    void * pp = alloc_phys_pages(size / PAGE_SIZE);
    void * vp = map_range(vma, size, pp, rwxug_flags);

    return vp;
}

void set_range_flags(const void *vp, size_t size, int rwxug_flags) {
    // FIXME
    uintptr_t vma = (uintptr_t) vp;
    if (vma % PAGE_SIZE != 0) panic("set_range_flags: vp is not page-aligned");
    if (!wellformed(vma)) panic("set_range_flags: vp is not well-formed");

    // round up size to be a multiple of PAGE_SIZE
    size = ROUND_UP(size, PAGE_SIZE);

    struct pte *root_table = active_space_ptab();

    for (size_t i = 0; i < size / PAGE_SIZE; i ++) {
        ptab_adjust(root_table, VPN(vma) + i, rwxug_flags);
    }
    sfence_vma();

    return;
}

void unmap_and_free_range(void *vp, size_t size) {
    // FIXME
    uintptr_t vma = (uintptr_t) vp;
    if (vma % PAGE_SIZE != 0) panic("unmap_and_free_range: vp is not page-aligned");
    if (!wellformed(vma)) panic("unmap_and_free_range: vp is not well-formed");

    // round up size to be a multiple of PAGE_SIZE
    size = ROUND_UP(size, PAGE_SIZE);

    struct pte *root_table = active_space_ptab();

    for (size_t i = 0; i < size / PAGE_SIZE; i ++) {
        void* pp = ptab_remove(root_table, VPN(vma) + i);
        if (pp != NULL) {
            free_phys_page(pp);
        }
    }
    sfence_vma();

    return;
}

int validate_vptr(const void *vp, size_t len, int rwxug_flags) {
    // FIXME

    // validate virtual memory address
    uintptr_t vma = (uintptr_t) vp;
    if (vma % PAGE_SIZE != 0) return -EINVAL;
    if (!wellformed(vma)) return -EINVAL;

    // len does not wrap around to zero
    if (vma + len < vma) return -EINVAL;

    // round up len to be a multiple of PAGE_SIZE
    size_t size = ROUND_UP(len, PAGE_SIZE);

    // iterates over pages in range
    struct pte *root_table = active_space_ptab();

    for (size_t i = 0; i < size / PAGE_SIZE; i ++) {
        struct pte * pte = ptab_fetch(root_table, VPN(vma) + i);

        // check if pages are mapped
        if (pte == NULL || !PTE_VALID(*pte)) return -ENOENT;

        // check if pages have flags set correctly
        if ((pte->flags & rwxug_flags) != rwxug_flags) return -EACCESS;
    }

    return 0;
}

int validate_vstr(const char *vs, int ug_flags) {
    // FIXME

    // validate virtual memory address
    uintptr_t vma = (uintptr_t) vs;
    if (!wellformed(vma)) return -EINVAL;

    // initialize values
    struct pte *root_table = active_space_ptab();
    uintptr_t current_page = VPN(vma);

    // get and check the first page
    struct pte *current_pte = ptab_fetch(root_table, current_page);

    if (current_pte == NULL || !PTE_VALID(*current_pte)) return -ENOENT;
    if ((current_pte->flags & ug_flags) != ug_flags) return -EACCESS;

    // loop through char
    for (size_t i = 0; 1; i++) {
        uintptr_t char_addr = vma + i;
        uintptr_t char_page = VPN(char_addr);

        // string spans multiple pages
        if (current_page != char_page) {
            current_pte = ptab_fetch(root_table, VPN(char_addr));
            current_page = char_page;

            if (current_pte == NULL || !PTE_VALID(*current_pte)) return -ENOENT;
            if ((current_pte->flags & ug_flags) != ug_flags) return -EACCESS;
        }

        // access and check char
        char c = vs[i];
        if (c == '\0') {
            return 0;
        }
    }


    return 0;
}

void *alloc_phys_page(void) {
    // FIXME
    return alloc_phys_pages(1);
}

void free_phys_page(void *pp) {
    // FIXME
    free_phys_pages(pp, 1);
}

void *alloc_phys_pages(unsigned int cnt) {
    // FIXME
    if (cnt == 0) return NULL;

    // initialize at the start of the chunk list
    struct page_chunk * curr = free_chunk_list;
    struct page_chunk * best = NULL;
    struct page_chunk ** prevs_next = &free_chunk_list;
    struct page_chunk ** best_prev = NULL;
    unsigned long best_pagecnt = 0;

    while (!(curr == NULL)) {
        // fits and is better, best_pagecnt == 0 means there's been no valid chunks yet
        if (curr->pagecnt >= cnt && (best_pagecnt == 0 || curr->pagecnt < best_pagecnt)) {
            best_pagecnt = curr->pagecnt;
            best = curr;
            best_prev = prevs_next;

            if (best_pagecnt == cnt) break;
        }
        prevs_next = &curr->next;
        curr = curr->next;
    }

    // no chunk found
    if (best == NULL) panic("alloc_phys_pages: out of memory");

    // perfectly matching chunk size
    if (best_pagecnt == cnt) {
        *best_prev = best->next;
    } else {
    // non-perfectly matching chunk size
        struct page_chunk * left_over = (struct page_chunk*)((char*)best + cnt * PAGE_SIZE);
        left_over->next = best->next;
        left_over->pagecnt = best->pagecnt - cnt;
        *best_prev = left_over;
    }

    return (void*)best;
}

void free_phys_pages(void *pp, unsigned int cnt) {
    // FIXME
    if ((uintptr_t)pp % PAGE_SIZE != 0) panic("free_phys_pages: pp is not page-aligned");
    if (cnt == 0) return;

    struct page_chunk * to_free = (struct page_chunk*)pp;
    to_free->pagecnt = cnt;

    // initialize at the start of the chunk list
    struct page_chunk * curr = free_chunk_list;
    struct page_chunk ** prevs_next = &free_chunk_list;

    // iterate until curr is the free chunk that should be after the chunk we're freeing
    while (curr < to_free && curr != NULL) {
        prevs_next = &curr->next;
        curr = curr->next;
    }

    to_free->next = curr;

    *prevs_next = to_free;
}

unsigned long free_phys_page_count(void) {
    // FIXME
    struct page_chunk * chunk = free_chunk_list;
    unsigned long num_free_pages = 0;
    while (!(chunk == NULL)) {
        num_free_pages += chunk->pagecnt;
        chunk = chunk->next;
    }
    return num_free_pages;
}

int handle_umode_page_fault(struct trap_frame *tfr, uintptr_t vma) {
    // FIXME
    if (!wellformed(vma)) panic("handle_umode_page_fault: vma is not well-formed");

    if (UMEM_START_VMA <= vma && vma < UMEM_END_VMA) {
        void * pp = alloc_phys_page();

        uintptr_t aligned_vma = (VPN(vma)) << PAGE_ORDER;

        map_range(aligned_vma, PAGE_SIZE, pp, PTE_W | PTE_R | PTE_U);
        memset(pp, 0, PAGE_SIZE);
        return 1;
    }
    return 0;  // no handled
}

/**
 * @brief Reads satp to retrieve tag for active memory space
 * @return Tag for active memory space
 */
mtag_t active_space_mtag(void) { return csrr_satp(); }

/**
 * @brief Constructs tag from page table address and address space identifier
 * @param ptab Pointer to page table to use in tag
 * @param asid Address space identifier to use in tag
 * @return Memory tag formed from paging mode, page table address, and ASID
 */
static inline mtag_t ptab_to_mtag(struct pte *ptab, unsigned int asid) {
    return (((unsigned long)PAGING_MODE << RISCV_SATP_MODE_shift) |
            ((unsigned long)asid << RISCV_SATP_ASID_shift) | pagenum(ptab) << RISCV_SATP_PPN_shift);
}

/**
 * @brief Retrives a page table address from a tag
 * @param mtag Tag to extract page table address from
 * @return Pointer to page table retrieved from tag
 */
static inline struct pte *mtag_to_ptab(mtag_t mtag) { return (struct pte *)((mtag << 20) >> 8); }

/**
 * @brief Returns the address of the page table corresponding to the active memory space
 * @return Pointer to page table extracted from active memory space tag
 */
static inline struct pte *active_space_ptab(void) { return mtag_to_ptab(active_space_mtag()); }

/**
 * @brief Constructs a physical pointer from a physical page number
 * @param n Physical page number to derive physical pointer from
 * @return Pointer to memory corresponding to physical page
 */
static inline void *pageptr(uintptr_t n) { return (void *)(n << PAGE_ORDER); }

/**
 * @brief Constructs a physical page number from a pointer
 * @param p Pointer to derive physical page number from
 * @return Physical page number corresponding to pointer
 */
static inline unsigned long pagenum(const void *p) { return (unsigned long)p >> PAGE_ORDER; }

/**
 * @brief Checks if bits 63:38 of passed virtual memory address are all 1 or all 0
 * @param vma Virtual memory address to check well-formedness of
 * @return 1 if pointer is well-formed, 0 otherwise
 */
static inline int wellformed(uintptr_t vma) {
    // Address bits 63:38 must be all 0 or all 1
    uintptr_t const bits = (intptr_t)vma >> 38;
    return (!bits || !(bits + 1));
}

/**
 * @brief Constructs a page table entry corresponding to a leaf
 * @details For our purposes, a leaf PTE has the A, D, and V flags set
 * @param pp Physical address to set physical page number of PTE from
 * @param rwxug_flags Flags to set on PTE
 * @return PTE initialized with proper flags and PPN
 */
static inline struct pte leaf_pte(const void *pp, uint_fast8_t rwxug_flags) {
    return (struct pte){.flags = rwxug_flags | PTE_A | PTE_D | PTE_V, .ppn = pagenum(pp)};
}

/**
 * @brief Constructs a page table entry corresponding to a page table
 * @param pt Physical address to set physical page number of PTE from
 * @param g_flag Flags to set on PTE (should either be G flag or nothing)
 * @return PTE initialized with proper flags and PPN
 */
static inline struct pte ptab_pte(const struct pte *pt, uint_fast8_t g_flag) {
    return (struct pte){.flags = g_flag | PTE_V, .ppn = pagenum(pt)};
}

/**
 * @brief Returns an empty pte
 * @return An empty pte
 */
static inline struct pte null_pte(void) { return (struct pte){}; }