#include <linux/kernel.h>
#include <linux/syscalls.h>
#include <linux/mm.h>
#include <linux/sched.h>
#include <linux/pgtable.h> 

SYSCALL_DEFINE1(get_pa, void __user *, vaddr)
{
	unsigned long addr = (unsigned long)vaddr;
	struct mm_struct *mm = current->mm;
	pgd_t *pgd;
	p4d_t *p4d;
	pud_t *pud;
	pmd_t *pmd;
	pte_t *ptep, pte;
	unsigned long pa = 0;

	if (!mm)
		return -EFAULT;

	mmap_read_lock(mm);	/* 舊版 kernel (<5.8) 用 down_read(&mm->mmap_sem) */

	pgd = pgd_offset(mm, addr);
	if (pgd_none(*pgd) || pgd_bad(*pgd))
		goto out;

	p4d = p4d_offset(pgd, addr);
	if (p4d_none(*p4d) || p4d_bad(*p4d))
		goto out;

	pud = pud_offset(p4d, addr);
	if (pud_none(*pud))
		goto out;
	if (pud_leaf(*pud)) {			/* 1GB huge page */
		if (pud_present(*pud))
			pa = (pud_pfn(*pud) << PAGE_SHIFT) | (addr & ~PUD_MASK);
		goto out;
	}
	if (pud_bad(*pud))
		goto out;

	pmd = pmd_offset(pud, addr);
	if (pmd_none(*pmd))
		goto out;
	if (pmd_leaf(*pmd)) {			/* 2MB huge page (THP) */
		if (pmd_present(*pmd))
			pa = (pmd_pfn(*pmd) << PAGE_SHIFT) | (addr & ~PMD_MASK);
		goto out;
	}
	if (pmd_bad(*pmd))
		goto out;

	ptep = pte_offset_map(pmd, addr);
	if (!ptep)
		goto out;
	pte = *ptep;
	if (pte_present(pte))			/* Present = 1 才有實體頁 */
		pa = (pte_pfn(pte) << PAGE_SHIFT) | (addr & ~PAGE_MASK);
	pte_unmap(ptep);

out:
	mmap_read_unlock(mm);
	return (long)pa;			/* 未映射回傳 0 */
}
