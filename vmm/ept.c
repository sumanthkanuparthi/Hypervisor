
#include <vmm/ept.h>

#include <inc/error.h>
#include <inc/memlayout.h>
#include <kern/pmap.h>
#include <inc/string.h>
#include <kern/env.h>


// Return the physical address of an ept entry
static inline uintptr_t epte_addr(epte_t epte)
{
	return (epte & EPTE_ADDR);
}

// Return the host kernel virtual address of an ept entry
static inline uintptr_t epte_page_vaddr(epte_t epte)
{
	return (uintptr_t) KADDR(epte_addr(epte));
}

// Return the flags from an ept entry
static inline epte_t epte_flags(epte_t epte)
{
	return (epte & EPTE_FLAGS);
}

// Return true if an ept entry's mapping is present
static inline int epte_present(epte_t epte)
{
	return (epte & __EPTE_FULL) > 0;
}

// Find the final ept entry for a given guest physical address,
// creating any missing intermediate extended page tables if create is non-zero.
//
// If epte_out is non-NULL, store the found epte_t* at this address.
//
// Return 0 on success.  
// 
// Error values:
//    -E_INVAL if eptrt is NULL
//    -E_NO_ENT if create == 0 and the intermediate page table entries are missing.
//    -E_NO_MEM if allocation of intermediate page table entries fails
//
// Hint: Set the permissions of intermediate ept entries to __EPTE_FULL.
//       The hardware ANDs the permissions at each level, so removing a permission
//       bit at the last level entry is sufficient (and the bookkeeping is much simpler).
static int ept_lookup_gpa(epte_t* eptrt, void *gpa, 
			  int create, epte_t **epte_out) {

       if(eptrt == NULL)
		return -E_INVAL;
    

       epte_t *pte = epml4e_walk(eptrt, (void*)gpa, create);

       
        if (pte == NULL) {
		*epte_out = NULL;
              if(create == 0)
			return -E_NO_ENT;
              else
			return -E_NO_MEM;
	}


	*epte_out = pte;

    return 0;

}


epte_t *
epml4e_walk(epte_t *pml4e, const void *va, int create)
{
	uintptr_t index_in_pml4t = PML4(va);
	pml4e_t *offsetd_ptr_in_pml4t = pml4e + index_in_pml4t;
	pdpe_t *pdpt_base = (pdpe_t*)(PTE_ADDR(*offsetd_ptr_in_pml4t));
	
	// Check if PDP does exists
	if (pdpt_base == 0) {
		if (!create) return NULL;
		else {
			struct Page *newPage = page_alloc(ALLOC_ZERO);

			if (newPage == NULL) return NULL; // Out of memory

			newPage->pp_ref++;
			pdpt_base = (pdpe_t*)page2pa(newPage);
			epte_t *pte = epdpe_walk((pdpe_t*)page2kva(newPage), va, create);

			if (pte == NULL)
				page_decref(newPage); // Free allocated page for PDPE
			else {
				*offsetd_ptr_in_pml4t = ((uint64_t)pdpt_base) | __EPTE_FULL;
				return pte;
			}
		}
	}
	else 
		return epdpe_walk(KADDR((uint64_t)pdpt_base), va, create); // PDP exists, so walk through it.

	return NULL;
}

// Given a pdpe i.e page directory pointer pdpe_walk returns the pointer to page table entry
// The programming logic in this function is similar to pml4e_walk.
// It calls the pgdir_walk which returns the page_table entry pointer.
// Hints are the same as in pml4e_walk
epte_t *
epdpe_walk(pdpe_t *pdpe,const void *va,int create){
        uintptr_t index_in_pdpt = PDPE(va);
        pdpe_t *offsetd_ptr_in_pdpt = pdpe + index_in_pdpt;
        pde_t *pgdir_base = (pde_t*) PTE_ADDR(*offsetd_ptr_in_pdpt);

	//Check if PD exists
        if (pgdir_base == 0)
        {
                if (create == 0) return NULL;
                else {
                        struct Page *newPage = page_alloc(ALLOC_ZERO);

                        if (newPage == NULL) return NULL;

                        newPage->pp_ref++;
                        pgdir_base = (pde_t*)page2pa(newPage);
                        epte_t *pte = pgdir_walk(page2kva(newPage), va, create);

                        if (pte == NULL) page_decref(newPage); // Free allocated page for PDE
                        else {
                                *offsetd_ptr_in_pdpt = ((uint64_t)pgdir_base) | __EPTE_FULL;
                                return pte;
                        }
               }
        }
        else
                return epgdir_walk(KADDR((uint64_t)pgdir_base), va, create); // PD is present, so walk through it

	return NULL;
}
// Given 'pgdir', a pointer to a page directory, pgdir_walk returns
// a pointer to the page table entry (PTE). 
// The programming logic and the hints are the same as pml4e_walk
// and pdpe_walk.

    epte_t *
epgdir_walk(pde_t *pgdir, const void *va, int create)
{
        uintptr_t index_in_pgdir = PDX(va);
        pde_t *offsetd_ptr_in_pgdir = pgdir + index_in_pgdir;
        epte_t *page_table_base = (epte_t*)(PTE_ADDR(*offsetd_ptr_in_pgdir));

	//Check if PT exists
        if (page_table_base == 0) {
                if (create == 0) return NULL;
                else {
                        struct Page *newPage = page_alloc(ALLOC_ZERO);

                        if (newPage == NULL) return NULL;

                        newPage->pp_ref++;
                        page_table_base = (epte_t*)page2pa(newPage);
			*offsetd_ptr_in_pgdir = ((uint64_t)page_table_base) | __EPTE_FULL;

			// Return PTE
		        uintptr_t index_in_page_table = PTX(va);
		        epte_t *offsetd_ptr_in_page_table = page_table_base + index_in_page_table;
			return (epte_t*)KADDR((uint64_t)offsetd_ptr_in_page_table);
		}
        }
        else {
		// PT exists, so return PTE
	        uintptr_t index_in_page_table = PTX(va);
        	epte_t *offsetd_ptr_in_page_table = page_table_base + index_in_page_table;
		return (epte_t*)KADDR((uint64_t)offsetd_ptr_in_page_table);
        }

	return NULL;
}


void ept_gpa2hva(epte_t* eptrt, void *gpa, void **hva) {
    epte_t* pte;
    int ret = ept_lookup_gpa(eptrt, gpa, 0, &pte);
    if(ret < 0) {
        *hva = NULL;
    } else {
        if(!epte_present(*pte)) {
           *hva = NULL;
        } else {
           *hva = KADDR(epte_addr(*pte));
        }
    }
}

static void free_ept_level(epte_t* eptrt, int level) {
    epte_t* dir = eptrt;
    int i;

    for(i=0; i<NPTENTRIES; ++i) {
        if(level != 0) {
            if(epte_present(dir[i])) {
                physaddr_t pa = epte_addr(dir[i]);
                free_ept_level((epte_t*) KADDR(pa), level-1);
                // free the table.
                page_decref(pa2page(pa));
            }
        } else {
            // Last level, free the guest physical page.
            if(epte_present(dir[i])) {
                physaddr_t pa = epte_addr(dir[i]);
                page_decref(pa2page(pa));
            }
        }
    }
    return;
}

// Free the EPT table entries and the EPT tables.
// NOTE: Does not deallocate EPT PML4 page.
void free_guest_mem(epte_t* eptrt) {
    free_ept_level(eptrt, EPT_LEVELS - 1);
}

// Add Page pp to a guest's EPT at guest physical address gpa
//  with permission perm.  eptrt is the EPT root.
// 
// Return 0 on success, <0 on failure.
//
int ept_page_insert(epte_t* eptrt, struct Page* pp, void* gpa, int perm) {
	

       if (ept_map_hva2gpa(eptrt, page2kva(pp), gpa, perm,1) < 0)
                        return -E_NO_MEM;
	pp->pp_ref++;
	return 0;

}

// Map host virtual address hva to guest physical address gpa,
// with permissions perm.  eptrt is a pointer to the extended
// page table root.
//
// Return 0 on success.
// 
// If the mapping already exists and overwrite is set to 0,
//  return -E_INVAL.
// 
// Hint: use ept_lookup_gpa to create the intermediate 
//       ept levels, and return the final epte_t pointer.
int ept_map_hva2gpa(epte_t* eptrt, void* hva, void* gpa, int perm, 
        int overwrite) {

    epte_t* pte;
    epte_t* phys;

    int ptr = PADDR(hva);

    struct Page *pp = pa2page(ptr);
    
    if (pp == NULL)
             return -E_INVAL;

  int ret = ept_lookup_gpa(eptrt, gpa, 1, &pte);

  if(overwrite == 0 && *pte != 0)
	return -E_INVAL;
 else if (overwrite  != 0 || *pte == 0)
{
	*pte = PTE_ADDR(ptr)| PTE_P |  perm | __EPTE_IPAT | __EPTE_TYPE(EPTE_TYPE_WB);
}
 else
	cprintf("Not ENTERING!");

    return 0;
}

int ept_alloc_static(epte_t *eptrt, struct VmxGuestInfo *ginfo) {
    physaddr_t i;
    
    for(i=0x0; i < 0xA0000; i+=PGSIZE) {
        struct Page *p = page_alloc(0);
        p->pp_ref += 1;
        int r = ept_map_hva2gpa(eptrt, page2kva(p), (void *)i, __EPTE_FULL, 0);
    }

    for(i=0x100000; i < ginfo->phys_sz; i+=PGSIZE) {
        struct Page *p = page_alloc(0);
        p->pp_ref += 1;
        int r = ept_map_hva2gpa(eptrt, page2kva(p), (void *)i, __EPTE_FULL, 0);
    }
    return 0;
}


#ifdef TEST_EPT_MAP
#include <kern/env.h>
#include <kern/syscall.h>
int _export_sys_ept_map(envid_t srcenvid, void *srcva,
	    envid_t guest, void* guest_pa, int perm);

int test_ept_map(void)
{
	struct Env *srcenv, *dstenv;
	struct Page *pp;
	epte_t *epte;
	int r;

	/* Initialize source env */
	if ((r = env_alloc(&srcenv, 0)) < 0)
		panic("Failed to allocate env (%d)\n", r);
	if (!(pp = page_alloc(ALLOC_ZERO)))
		panic("Failed to allocate page (%d)\n", r);
	if ((r = page_insert(srcenv->env_pml4e, pp, UTEMP, 0)) < 0)
		panic("Failed to insert page (%d)\n", r);
	curenv = srcenv;

	/* Check if sys_ept_map correctly verify the target env */
	if ((r = env_alloc(&dstenv, srcenv->env_id)) < 0)
		panic("Failed to allocate env (%d)\n", r);
	if ((r = _export_sys_ept_map(srcenv->env_id, UTEMP, dstenv->env_id, UTEMP, __EPTE_READ)) < 0)
		cprintf("EPT map to non-guest env failed as expected (%d).\n", r);
	else
		panic("sys_ept_map success on non-guest env.\n");

	/*env_destroy(dstenv);*/

	if ((r = env_guest_alloc(&dstenv, srcenv->env_id)) < 0)
		panic("Failed to allocate guest env (%d)\n", r);
	dstenv->env_vmxinfo.phys_sz = (uint64_t)UTEMP + PGSIZE;

	/* Check if sys_ept_map can verify srcva correctly */
	if ((r = _export_sys_ept_map(srcenv->env_id, (void *)UTOP, dstenv->env_id, UTEMP, __EPTE_READ)) < 0)
		cprintf("EPT map from above UTOP area failed as expected (%d).\n", r);
	else
		panic("sys_ept_map from above UTOP area success\n");
	if ((r = _export_sys_ept_map(srcenv->env_id, UTEMP+1, dstenv->env_id, UTEMP, __EPTE_READ)) < 0)
		cprintf("EPT map from unaligned srcva failed as expected (%d).\n", r);
	else
		panic("sys_ept_map from unaligned srcva success\n");

	/* Check if sys_ept_map can verify guest_pa correctly */
	if ((r = _export_sys_ept_map(srcenv->env_id, UTEMP, dstenv->env_id, UTEMP + PGSIZE, __EPTE_READ)) < 0)
		cprintf("EPT map to out-of-boundary area failed as expected (%d).\n", r);
	else
		panic("sys_ept_map success on out-of-boundary area\n");
	if ((r = _export_sys_ept_map(srcenv->env_id, UTEMP, dstenv->env_id, UTEMP-1, __EPTE_READ)) < 0)
		cprintf("EPT map to unaligned guest_pa failed as expected (%d).\n", r);
	else
		panic("sys_ept_map success on unaligned guest_pa\n");

	/* Check if the sys_ept_map can verify the permission correctly */
	if ((r = _export_sys_ept_map(srcenv->env_id, UTEMP, dstenv->env_id, UTEMP, 0)) < 0)
		cprintf("EPT map with empty perm parameter failed as expected (%d).\n", r);
	else
		panic("sys_ept_map success on empty perm\n");
	if ((r = _export_sys_ept_map(srcenv->env_id, UTEMP, dstenv->env_id, UTEMP, __EPTE_WRITE)) < 0)
		cprintf("EPT map with write perm parameter failed as expected (%d).\n", r);
	else
		panic("sys_ept_map success on write perm\n");

	/* Check if the sys_ept_map can succeed on correct setup */
	if ((r = _export_sys_ept_map(srcenv->env_id, UTEMP, dstenv->env_id, UTEMP, __EPTE_READ)) < 0)
		panic("Failed to do sys_ept_map (%d)\n", r);
	else
		cprintf("sys_ept_map finished normally.\n");

	/* Check if the mapping is valid */
	if ((r = ept_lookup_gpa(dstenv->env_pml4e, UTEMP, 0, &epte)) < 0)
		panic("Failed on ept_lookup_gpa (%d)\n", r);
	if (page2pa(pp) != (epte_addr(*epte)))
		panic("EPT mapping address mismatching (%x vs %x).\n",
				page2pa(pp), epte_addr(*epte));
	else
		cprintf("EPT mapping address looks good: %x vs %x.\n",
				page2pa(pp), epte_addr(*epte));

	/* stop running after test, as this is just a test run. */
	panic("Cheers! sys_ept_map seems to work correctly.\n");

	return 0;
}
#endif

