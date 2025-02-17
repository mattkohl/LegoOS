/*
 * Copyright (c) 2016-2019 Wuklab, Purdue University. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

/*
 * Virtual Memory Map support
 *
 * Virtual memory maps allow VM primitives pfn_to_page, page_to_pfn,
 * virt_to_page, page_address() to be implemented as a base offset
 * calculation without memory access.
 *
 * However, virtual mappings need a page table and TLBs. Many Linux
 * architectures already map their physical space using 1-1 mappings
 * via TLBs. For those arches the virtual memory map is essentially
 * for free if we use the same page size as the 1-1 mappings. In that
 * case the overhead consists of a few additional pages that are
 * allocated to create a view of memory for vmemmap.
 *
 * The architecture is expected to provide a vmemmap_populate() function
 * to instantiate the mapping.
 */

#include <lego/mm.h>
#include <lego/kernel.h>
#include <lego/memblock.h>

#include <asm/dma.h>
#include <asm/pgtable.h>
#include <asm/pgalloc.h>

static void *vmemmap_buf;
static void *vmemmap_buf_end;

static void * __earlyonly_bootmem_alloc(int node,
				unsigned long size,
				unsigned long align,
				unsigned long goal)
{
	void *p = memblock_virt_alloc_try_nid_nopanic(size, align, goal,
					    BOOTMEM_ALLOC_ACCESSIBLE, node);
	BUG_ON(!p);
	return p;
}

static void * __init vmemmap_alloc_block(unsigned long size, int node)
{
	return __earlyonly_bootmem_alloc(node, size, size,
			__pa(MAX_DMA_ADDRESS));
}

/* need to make sure size is all the same during early stage */
void * __init __vmemmap_alloc_block_buf(unsigned long size, int node)
{
	void *ptr;

	if (!vmemmap_buf)
		return vmemmap_alloc_block(size, node);

	/* take the from buf */
	ptr = (void *)ALIGN((unsigned long)vmemmap_buf, size);
	if (ptr + size > vmemmap_buf_end)
		return vmemmap_alloc_block(size, node);

	vmemmap_buf = ptr + size;

	return ptr;
}

void __init vmemmap_verify(pte_t *pte, int node,
			   unsigned long start, unsigned long end)
{
#if 0
	unsigned long pfn = pte_pfn(*pte);
	int actual_node = early_pfn_to_nid(pfn);

	if (node_distance(actual_node, node) > LOCAL_DISTANCE)
		pr_warn("[%lx-%lx] potential offnode page_structs\n",
			start, end - 1);
#endif
}

pte_t * __init vmemmap_pte_populate(pmd_t *pmd, unsigned long addr, int node)
{
	pte_t *pte = pte_offset_kernel(pmd, addr);
	if (pte_none(*pte)) {
		pte_t entry;
		void *p = __vmemmap_alloc_block_buf(PAGE_SIZE, node);
		if (!p)
			return NULL;
		entry = pfn_pte(__pa(p) >> PAGE_SHIFT, PAGE_KERNEL);
		pte_set(pte, entry);
	}
	return pte;
}

pmd_t * __init vmemmap_pmd_populate(pud_t *pud, unsigned long addr, int node)
{
	pmd_t *pmd = pmd_offset(pud, addr);
	if (pmd_none(*pmd)) {
		void *p = vmemmap_alloc_block(PAGE_SIZE, node);
		if (!p)
			return NULL;
		pmd_populate_kernel(&init_mm, pmd, p);
	}
	return pmd;
}

pud_t * __init vmemmap_pud_populate(pgd_t *pgd, unsigned long addr, int node)
{
	pud_t *pud = pud_offset(pgd, addr);
	if (pud_none(*pud)) {
		void *p = vmemmap_alloc_block(PAGE_SIZE, node);
		if (!p)
			return NULL;
		pud_populate(&init_mm, pud, p);
	}
	return pud;
}

pgd_t * __init vmemmap_pgd_populate(unsigned long addr, int node)
{
	pgd_t *pgd = pgd_offset_k(addr);
	if (pgd_none(*pgd)) {
		void *p = vmemmap_alloc_block(PAGE_SIZE, node);
		if (!p)
			return NULL;
		pgd_populate(&init_mm, pgd, p);
	}
	return pgd;
}

int __init vmemmap_populate_basepages(unsigned long start,
				      unsigned long end, int node)
{
	unsigned long addr = start;
	pgd_t *pgd;
	pud_t *pud;
	pmd_t *pmd;
	pte_t *pte;

	for (; addr < end; addr += PAGE_SIZE) {
		pgd = vmemmap_pgd_populate(addr, node);
		if (!pgd)
			return -ENOMEM;
		pud = vmemmap_pud_populate(pgd, addr, node);
		if (!pud)
			return -ENOMEM;
		pmd = vmemmap_pmd_populate(pud, addr, node);
		if (!pmd)
			return -ENOMEM;
		pte = vmemmap_pte_populate(pmd, addr, node);
		if (!pte)
			return -ENOMEM;
		vmemmap_verify(pte, node, addr, addr + PAGE_SIZE);
	}

	return 0;
}

/*
 * Create the virtual mem_map mapping
 */
struct page * __init sparse_mem_map_populate(unsigned long pnum, int nid)
{
	unsigned long start;
	unsigned long end;
	struct page *map;

	/* No touch, just address :) */
	map = pfn_to_page(pnum * PAGES_PER_SECTION);
	start = (unsigned long)map;
	end = (unsigned long)(map + PAGES_PER_SECTION);

	if (vmemmap_populate(start, end, nid))
		return NULL;

	return map;
}

void __init sparse_mem_maps_populate_node(struct page **map_map,
					  unsigned long pnum_begin,
					  unsigned long pnum_end,
					  unsigned long map_count, int nodeid)
{
	unsigned long pnum;
	unsigned long size = sizeof(struct page) * PAGES_PER_SECTION;
	void *vmemmap_buf_start;

	size = ALIGN(size, PMD_SIZE);
	vmemmap_buf_start = __earlyonly_bootmem_alloc(nodeid, size * map_count,
			 PMD_SIZE, __pa(MAX_DMA_ADDRESS));

	if (vmemmap_buf_start) {
		vmemmap_buf = vmemmap_buf_start;
		vmemmap_buf_end = vmemmap_buf_start + size * map_count;
	}

	for (pnum = pnum_begin; pnum < pnum_end; pnum++) {
		struct mem_section *ms;

		if (!present_section_nr(pnum))
			continue;

		map_map[pnum] = sparse_mem_map_populate(pnum, nodeid);
		if (map_map[pnum])
			continue;
		ms = __nr_to_section(pnum);
		pr_err("%s: sparsemem memory map backing failed some memory will not be available\n",
		       __func__);
		ms->section_mem_map = 0;
	}

	if (vmemmap_buf_start) {
		/* need to free left buf */
		memblock_free_early(__pa(vmemmap_buf),
				    vmemmap_buf_end - vmemmap_buf);
		vmemmap_buf = NULL;
		vmemmap_buf_end = NULL;
	}
}
