/*-
 * Copyright (c) 2018 Domagoj Stolfa
 * All rights reserved.
 *
 * This software was developed by BAE Systems, the University of Cambridge
 * Computer Laboratory, and Memorial University under DARPA/AFRL contract
 * FA8650-15-C-7558 ("CADETS"), as part of the DARPA Transparent Computing
 * (TC) research program.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <machine/vmparam.h>
#include <vm/vm_page.h>
#include <machine/pmap.h>
#include <vm/pmap.h>

/*
 * This was copy pasted from vmm_instruction_emul.c as it is useful here too.
 */
static int
dtrace_canonical_check(enum vm_cpu_mode cpu_mode, uint64_t gla)
{
	uint64_t mask;

	if (cpu_mode != CPU_MODE_64BIT)
		return (0);

	/*
	 * The value of the bit 47 in the 'gla' should be replicated in the
	 * most significant 16 bits.
	 */
	mask = ~((1UL << 48) - 1);
	if (gla & (1UL << 47))
		return ((gla & mask) != mask);
	else
		return ((gla & mask) != 0);
}

/* Return a pointer to the PML4 slot that corresponds to a VA */
static __inline pml4_entry_t *
dtrace_pml4e(pmap_t pmap, vm_offset_t va)
{

	return (&pmap->pm_pml4[pmap_pml4e_index(va)]);
}

static __inline boolean_t
dtrace_emulate_ad_bits(pmap_t pmap)
{

	return ((pmap->pm_flags & PMAP_EMULATE_AD_BITS) != 0);
}

static __inline pt_entry_t
dtrace_valid_bit(pmap_t pmap)
{
	pt_entry_t mask;

	switch (pmap->pm_type) {
	case PT_X86:
	case PT_RVI:
		mask = X86_PG_V;
		break;
	case PT_EPT:
		if (dtrace_emulate_ad_bits(pmap))
			mask = EPT_PG_EMUL_V;
		else
			mask = EPT_PG_READ;
		break;
	default:
		panic("dtrace_valid_bit: invalid pm_type %d", pmap->pm_type);
	}

	return (mask);
}


/* Return a pointer to the PDP slot that corresponds to a VA */
static __inline pdp_entry_t *
dtrace_pml4e_to_pdpe(pml4_entry_t *pml4e, vm_offset_t va)
{
	pdp_entry_t *pdpe;

	pdpe = (pdp_entry_t *)PHYS_TO_DMAP(*pml4e & PG_FRAME);
	return (&pdpe[pmap_pdpe_index(va)]);
}

/* Return a pointer to the PDP slot that corresponds to a VA */
static __inline pdp_entry_t *
dtrace_pdpe(pmap_t pmap, vm_offset_t va)
{
	pml4_entry_t *pml4e;
	pt_entry_t PGV;

	PGV = dtrace_valid_bit(pmap);
	pml4e = dtrace_pml4e(pmap, va);
	if ((*pml4e & PGV) == 0)
		return (NULL);
	return (dtrace_pml4e_to_pdpe(pml4e, va));
}


/* Return a pointer to the PD slot that corresponds to a VA */
static __inline pd_entry_t *
dtrace_pdpe_to_pde(pdp_entry_t *pdpe, vm_offset_t va)
{
	pd_entry_t *pde;

	pde = (pd_entry_t *)PHYS_TO_DMAP(*pdpe & PG_FRAME);
	return (&pde[pmap_pde_index(va)]);
}


/* Return a pointer to the PD slot that corresponds to a VA */
static __inline pd_entry_t *
dtrace_pde(pmap_t pmap, vm_offset_t va)
{
	pdp_entry_t *pdpe;
	pt_entry_t PGV;

	PGV = dtrace_valid_bit(pmap);
	pdpe = dtrace_pdpe(pmap, va);
	if (pdpe == NULL || (*pdpe & PGV) == 0)
		return (NULL);
	return (dtrace_pdpe_to_pde(pdpe, va));
}

/* Return a pointer to the PT slot that corresponds to a VA */
static __inline pt_entry_t *
dtrace_pde_to_pte(pd_entry_t *pde, vm_offset_t va)
{
	pt_entry_t *pte;

	pte = (pt_entry_t *)PHYS_TO_DMAP(*pde & PG_FRAME);
	return (&pte[pmap_pte_index(va)]);
}

static vm_page_t
DTRACE_PHYS_TO_VM_PAGE(vm_paddr_t pa)
{
#ifndef VM_PHYSSEG_DENSE
#error "Need dense paging."
#endif
	vm_page_t m;
	long pi;

	pi = atop(pa);
	m = &vm_page_array[pi - first_page];
	return (m);
}

static vm_page_t
dtrace_get_page(pmap_t pmap, vm_offset_t va)
{
	pd_entry_t pde, *pdep;
	pt_entry_t pte;
	vm_paddr_t pa;
	vm_page_t m;

	pa = 0;
	m = NULL;
	pdep = dtrace_pde(pmap, va);
	if (pdep != NULL && (pde = *pdep)) {
		if (pde & PG_PS) {
			m = DTRACE_PHYS_TO_VM_PAGE((pde & PG_PS_FRAME) |
			    (va & PDRMASK));
		} else {
			pte = *dtrace_pde_to_pte(pdep, va);
			m = DTRACE_PHYS_TO_VM_PAGE(pte & PG_FRAME);
		}
	}
	return (m);
}


int
dtrace_gla2hpa(struct vm_guest_paging *paging, uint64_t gla, uint64_t *hpa)
{
	const uint8_t shift = PAGE_SHIFT + 9;
	uint64_t *ptpbase, ptpphys, pte, pgsize, pde, thing_to_or;
	vm_page_t m;
	uint64_t index = 0, gpa = 0;
	int ptpshift, nlevels, ptpindex, pageoff;
	pdp_entry_t *pdp, *pdpe;
	pml4_entry_t *pml4e;
	pd_entry_t *pdep;
	pmap_t pmap;
	long pi;

	*hpa = 0;
	/* Make sure we have the paging */
	ASSERT(paging != NULL);

	if (paging->cpl == 3)
		return (EINVAL);

 restart:
	/* Page table root */
	ptpphys = paging->cr3;
	if (dtrace_canonical_check(paging->cpu_mode, gla))
	    return (EINVAL);

	if (paging->paging_mode == PAGING_MODE_FLAT)
		gpa = gla;
	else if (paging->paging_mode == PAGING_MODE_64) {
		nlevels = 4;
		pmap = paging->pmap;
		while (--nlevels >= 0) {
			ptpphys >>= 12;
			ptpphys <<= 24;
			ptpphys >>= 12;
			m = dtrace_get_page(pmap, trunc_page(ptpphys));
			pageoff = ptpphys & PAGE_MASK;
			ptpbase = (void *)(PHYS_TO_DMAP(VM_PAGE_TO_PHYS(m)) +
			    pageoff);

			ptpshift = PAGE_SHIFT + nlevels * 9;
			ptpindex = (gla >> ptpshift) & 0x1ff;
			pgsize = 1UL << ptpshift;

			pte = ptpbase[ptpindex];
			if ((pte & PG_A) == 0) {
				if (atomic_cmpset_64(&ptpbase[ptpindex],
				    pte, pte | PG_A) == 0) {
					goto restart;
				}
			}

			if (nlevels > 0 && (pte & PG_PS) != 0) {
				if (pgsize > 1 * 1024*1024*1024) {
					*hpa = 0;
					return (EINVAL);
				}
				break;
			}
			ptpphys = pte;
		}
		pte >>= ptpshift; pte <<= (ptpshift + 12); pte >>= 12;
		gpa = pte | (gla & (pgsize - 1));
	} else {
		return (EINVAL);
	}

	*hpa = gpa;
	return (0);
}
