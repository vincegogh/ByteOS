#include <stdint.h>
#include "libk.h"
#include "mm.h"
#include "asm.h"
#include "util.h"
#include "smp.h"
#include "drivers/pit.h"

#define P4_ADDR_SHIFT 39
#define P3_ADDR_SHIFT 30
#define P2_ADDR_SHIFT 21
#define P1_ADDR_SHIFT 12
#define P1_ADDR_MASK (0x1FFUL << P1_ADDR_SHIFT)
#define P4_ADDR_MASK (0x1FFUL << P4_ADDR_SHIFT)
#define P3_ADDR_MASK (0x1FFUL << P3_ADDR_SHIFT)
#define P2_ADDR_MASK (0x1FFUL << P2_ADDR_SHIFT)
#define PAGE_OFFSET_MASK 0xFFFF
#define PTE_ADDR_MASK (~(0xFFF00000000001FFUL))

static void dump_page_tables_p2(struct page_table *, uintptr_t);
static void dump_page_tables_p1(struct page_table *, uintptr_t);
static void dump_page_tables_p0(struct page_table *, uintptr_t);

extern struct page_table p4_table; // Initial kernel p4 table

struct page_table *kernel_p4;

void vmm_init(void)
{
	kernel_p4 = phys_to_kern((physaddr_t)&p4_table);
	klog("vmm", "Kernel P4 at %p\n", kernel_p4);
//	vmm_dump_tables();
}

void vmm_map_all(struct mmap *mmap)
{
	// First map all regions marked "available", updating highest_mapped
	for (size_t i = 0; i < mmap->available.count; i++) {
		physaddr_t start = ALIGNUP(mmap->available.regions[i].base, PAGE_SIZE);
		physaddr_t end = mmap->available.regions[i].base + mmap->available.regions[i].len;
		for (physaddr_t j = start; j <= (end - PAGE_SIZE); j += PAGE_SIZE) {
			vmm_map_page(kernel_p4, j, phys_to_virt(j), VMM_ALLOC_MMAP | PAGE_GLOBAL | PAGE_WRITABLE);
			mmap->highest_mapped = MAX(j, mmap->highest_mapped);
		}
	}
}

static inline struct page_table *pgtab_extract_virt_addr(struct page_table *pgtab, uint16_t index)
{
	pte_t entry = pgtab->pages[index];
	if ((entry & PAGE_PRESENT) == 0)
		return NULL;
	return phys_to_virt((entry & PTE_ADDR_MASK));
}

static inline pte_t alloc_pgtab(unsigned int alloc_flags)
{
	uint64_t flags = PAGE_PRESENT | PAGE_WRITABLE | (alloc_flags & (PAGE_USER_ACCESSIBLE));
	if (alloc_flags & VMM_ALLOC_MMAP) {
		// Mmap low allocs are guaranteed to be mapped
		physaddr_t pgtab_phys = mmap_alloc_low(PAGE_SIZE, MMAP_ALLOC_PA).base;
		kassert_dbg((pgtab_phys & 0xFFF) == 0); // Must be aligned
		return (pgtab_phys & PTE_ADDR_MASK) | flags;
	} else {
		struct page *p = pmm_alloc_order(0, GFP_NONE);
		kassert_dbg(p != NULL);
		physaddr_t pgtab_phys = page_to_phys(p);
		return (pgtab_phys & PTE_ADDR_MASK) | flags;
	}
}

void vmm_dump_tables(void)
{
	for (size_t i = 0; i < 512; i++) {
		struct page_table *pgtab = pgtab_extract_virt_addr(kernel_p4, i);
		if (pgtab == NULL)
			continue;
		kprintf("P3: %lx\n", kernel_p4->pages[i] & ~PTE_ADDR_MASK);
		dump_page_tables_p2(pgtab, 0xFFFF000000000000 | (i << 39));
	}
}

static void dump_page_tables_p2(struct page_table *p3, uintptr_t addr_bits)
{
	for (size_t i = 0; i < 512; i++) {
		struct page_table *pgtab = pgtab_extract_virt_addr(p3, i);
		if (pgtab == NULL)
			continue;
		kprintf("\tP2: %lx\n", p3->pages[i] & ~PTE_ADDR_MASK);
		dump_page_tables_p1(pgtab, addr_bits | (i << 30));
	}
}

static void dump_page_tables_p1(struct page_table *p2, uintptr_t addr_bits)
{
	for (size_t i = 0; i < 512; i++) {
		struct page_table *pgtab = pgtab_extract_virt_addr(p2, i);
		if (pgtab == NULL)
			continue;
		virtaddr_t first_virt = (virtaddr_t)(addr_bits | (i << 21));
		physaddr_t first_phys = pgtab->pages[0] & PTE_ADDR_MASK;
		kprintf("\t\tP1: %p -> %p, %lx\n", first_virt, (void *)first_phys, p2->pages[i] & ~PTE_ADDR_MASK);
		//dump_page_tables_p0(pgtab, addr_bits | (i << 21));
		//pit_sleep_ms(300);
	}
}

static void __attribute__((unused)) dump_page_tables_p0(struct page_table *p0, uintptr_t addr_bits)
{
	for (size_t i = 0; i < 512; i++) {
		physaddr_t addr = p0->pages[i] & PTE_ADDR_MASK;
		if (addr == 0)
			continue;
		kprintf("\t\t\tPage: %p -> %p, %lx\n",
				(virtaddr_t)(addr_bits | (i << 12)),
				(void *)(p0->pages[i] & PTE_ADDR_MASK),
				p0->pages[i] & ~PTE_ADDR_MASK);
	}	
}

pte_t vmm_get_pte(struct page_table *p4, void *addr)
{
	const uintptr_t va = (uintptr_t)addr;
	const uint16_t p4_index = (va & P4_ADDR_MASK) >> P4_ADDR_SHIFT;
	const uint16_t p3_index = (va & P3_ADDR_MASK) >> P3_ADDR_SHIFT;
	const uint16_t p2_index = (va & P2_ADDR_MASK) >> P2_ADDR_SHIFT;
	const uint16_t p1_index = (va & P1_ADDR_MASK) >> P1_ADDR_SHIFT;

	struct page_table *p3_table = pgtab_extract_virt_addr(p4, p4_index);
	if (p3_table == NULL)
		return 0;

	struct page_table *p2_table = pgtab_extract_virt_addr(p3_table, p3_index);
	if (p2_table == NULL)
		return 0;

	struct page_table *p1_table = pgtab_extract_virt_addr(p2_table, p2_index);
	if (p1_table == NULL)
		return 0;

	return p1_table->pages[p1_index];
}

bool vmm_has_flags(struct page_table *p4, void *addr, uint64_t flags)
{
	kassert_dbg(addr != NULL);
	return (vmm_get_pte(p4, addr) & flags) != 0;
}

void vmm_map_page(struct page_table *p4, physaddr_t phys, virtaddr_t virt, unsigned long flags)
{
	const uintptr_t va = (uintptr_t)virt;
	const uint16_t p4_index = (va & P4_ADDR_MASK) >> P4_ADDR_SHIFT;
	const uint16_t p3_index = (va & P3_ADDR_MASK) >> P3_ADDR_SHIFT;
	const uint16_t p2_index = (va & P2_ADDR_MASK) >> P2_ADDR_SHIFT;
	const uint16_t p1_index = (va & P1_ADDR_MASK) >> P1_ADDR_SHIFT;

	// When the bit is on, execution is disabled, so we need to toggle the bit
	flags ^= PAGE_EXECUTABLE;

	struct page_table *p3_table = pgtab_extract_virt_addr(p4, p4_index);
	if (p3_table == NULL) {
		p4->pages[p4_index] = alloc_pgtab(flags);
		p3_table = pgtab_extract_virt_addr(p4, p4_index);
		kassert_dbg(p3_table != NULL);
		memset(p3_table, 0, sizeof(struct page_table));
	}

	struct page_table *p2_table = pgtab_extract_virt_addr(p3_table, p3_index);
	if (p2_table == NULL) {
		p3_table->pages[p3_index] = alloc_pgtab(flags);
		p2_table = pgtab_extract_virt_addr(p3_table, p3_index);
		kassert_dbg(p2_table != NULL);
		memset(p2_table, 0, sizeof(struct page_table));
	}

	struct page_table *p1_table = pgtab_extract_virt_addr(p2_table, p2_index);
	if (p1_table == NULL) {
		p2_table->pages[p2_index] = alloc_pgtab(flags);
		p1_table = pgtab_extract_virt_addr(p2_table, p2_index);
		kassert_dbg(p1_table != NULL);
		memset(p1_table, 0, sizeof(struct page_table));
	}

#ifdef DEBUG
	// We should really be doing a TLB shootdown in this case.
	if (smp_nr_cpus() > 1) {
		kassert((p1_table->pages[p1_index] & PAGE_GLOBAL) == 0);
		kassert((flags & PAGE_GLOBAL) == 0);
	}
#endif

	flags &= ~VMM_ALLOC_MMAP; // Don't care about this flag anymore
	p1_table->pages[p1_index] = (phys & PTE_ADDR_MASK) | PAGE_PRESENT | flags;
	invlpg((uintptr_t)virt);
}

void vmm_destroy_low_mappings(struct page_table *p4)
{
	// Only loop over the userspace mappings
	for (size_t p4_index = 0; p4_index < (1 << 7); p4_index++) {
		struct page_table *p3 = pgtab_extract_virt_addr(p4, p4_index);
		if (p3 == NULL)
			continue;
		for (size_t p3_index = 0; p3_index < 512; p3_index++) {
			struct page_table *p2 = pgtab_extract_virt_addr(p3, p3_index);
			if (p2 == NULL)
				continue;
			for (size_t p2_index = 0; p2_index < 512; p2_index++) {
				struct page_table *p1 = pgtab_extract_virt_addr(p2, p2_index);
				if (p1 != NULL) {
					kassert_dbg((p2->pages[p2_index] & PAGE_GLOBAL) == 0);
					pmm_free_order(virt_to_page(p1), 0);
					p2->pages[p2_index] = 0;
				}
			}
			pmm_free_order(virt_to_page(p2), 0);
			p3->pages[p3_index] = 0;
		}
		pmm_free_order(virt_to_page(p3), 0);
		p4->pages[p4_index] = 0;
	}
	reload_cr3();
	// TODO: Might need a TLB shootdown here.
}

physaddr_t vmm_get_phys_addr(struct page_table *p4, void *virt)
{
	kassert_dbg(virt >= (void *)0x1000); // Doesn't work below this address
	uint16_t page_offset = (uintptr_t)virt & PAGE_OFFSET_MASK;
	physaddr_t addr = (physaddr_t)(vmm_get_pte(p4, virt) & PTE_ADDR_MASK);
	return (addr == 0) ? 0 : addr + page_offset;
}