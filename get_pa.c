#include <linux/kernel.h>
#include <linux/syscalls.h>
#include <linux/mm_types.h>
#include <linux/mm.h>
#include <linux/pgtable.h>

static bool va_exist(struct mm_struct *mm, unsigned long vaddr);

static bool page_exist(pgd_t *pgd);

static long get_phyaddr(struct mm_struct *mm, pmd_t *pmd, unsigned long vaddr);

SYSCALL_DEFINE1(get_pa, void __user *, vaddr_point)
{
	unsigned long vaddr = (unsigned long)vaddr_point;
	struct mm_struct *mm = current->mm;
	pgd_t *pgd;
	p4d_t *p4d;
	pud_t *pud;
	pmd_t *pmd;

	if (!va_exist(mm, vaddr))
		return 0;

	pgd = pgd_offset(mm, vaddr);
	
	if (!page_exist(pgd))
		return 0;

	p4d = p4d_offset(pgd, vaddr);
	pud = pud_offset(p4d, vaddr);
	pmd = pmd_offset(pud, vaddr);

	long phy_addr = get_phyaddr(mm, pmd, vaddr);

	if (!phy_addr)
		return 0;

	return phy_addr;
}

static bool va_exist(struct mm_struct *mm, unsigned long vaddr)
{
	struct vm_area_struct *vma = find_vma(mm, vaddr);

	if (!vma)
		return 0;
	return 1;
}

static bool page_exist(pgd_t *pgd)
{
	if (pgd_none(*pgd) || pgd_bad(*pgd))
		return 0;
	return 1;
}

static long get_phyaddr(struct mm_struct *mm, pmd_t *pmd, unsigned long vaddr)
{
	spinlock_t *spl;
	pte_t *pte_copy = pte_offset_map_lock(mm, pmd, vaddr, &spl);
	pte_t *pte = pte_copy;

	if (!pte_present(*pte)) {
		pte_unmap_unlock(pte_copy, spl);
		return 0;
	}
	phys_addr_t phy_addr = PFN_PHYS(pte_pfn(*pte)) | offset_in_page(vaddr);

	pte_unmap_unlock(pte_copy, spl);
	return (long)phy_addr;
}
