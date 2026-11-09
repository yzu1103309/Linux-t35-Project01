#include <linux/kernel.h>
#include <linux/syscalls.h>
#include <linux/mm.h>
#include <linux/sched.h>
#include <linux/pgtable.h>
#include <linux/spinlock.h>

SYSCALL_DEFINE1(get_pa, void __user *, uvaddr)
{
	unsigned long vaddr = (unsigned long)uvaddr;
	struct mm_struct *mm = current->mm;
	pgd_t *pgd;
	p4d_t *p4d;
	pud_t *pud, pudval;
	pmd_t *pmd, pmdval;
	pte_t *pte;
	spinlock_t *ptl;
	unsigned long pfn = 0;
	bool found = false;

	/* 1. 安全邊界:kernel thread 無 mm、不可走訪 kernel 位址 */
	if (!mm || vaddr >= TASK_SIZE)
		return 0;

	/* 2. killable 讀鎖;被中斷時依 hint 回傳 0 */
	if (mmap_read_lock_killable(mm))
		return 0;

	pgd = pgd_offset(mm, vaddr);
	if (pgd_none(*pgd) || pgd_bad(*pgd))
		goto out;

	p4d = p4d_offset(pgd, vaddr);
	if (p4d_none(*p4d) || p4d_bad(*p4d))
		goto out;

	pud = pud_offset(p4d, vaddr);
	pudval = READ_ONCE(*pud);
	if (pud_none(pudval))
		goto out;
	if (pud_leaf(pudval)) {			/* 1GB huge page,先 leaf 後 bad */
		if (pud_present(pudval)) {
			pfn = pud_pfn(pudval) +
			      ((vaddr & ~PUD_MASK) >> PAGE_SHIFT);
			found = true;
		}
		goto out;
	}
	if (pud_bad(pudval))
		goto out;

	pmd = pmd_offset(pud, vaddr);
	pmdval = READ_ONCE(*pmd);
	if (pmd_none(pmdval))
		goto out;

	if (pmd_leaf(pmdval)) {			/* 2MB THP */
		ptl = pmd_lock(mm, pmd);
		pmdval = READ_ONCE(*pmd);	/* 拿鎖後重查,防 split */
		if (pmd_present(pmdval) && pmd_leaf(pmdval)) {
			pfn = pmd_pfn(pmdval) +
			      ((vaddr & ~PMD_MASK) >> PAGE_SHIFT);
			found = true;
			spin_unlock(ptl);
			goto out;
		}
		spin_unlock(ptl);		/* 已被 split,落到 PTE 路徑 */
		pmdval = READ_ONCE(*pmd);
		if (pmd_none(pmdval))
			goto out;
	}
	if (pmd_bad(pmdval))
		goto out;

	pte = pte_offset_map_lock(mm, pmd, vaddr, &ptl);
	if (!pte)
		pte_unmap_unlock(pte, ptl);
		goto out;
	if (pte_present(*pte)) {
		pfn = pte_pfn(*pte);
		found = true;
	}
	pte_unmap_unlock(pte, ptl);

out:
	mmap_read_unlock(mm);

	/* 3. 只接受真正的 DRAM,排除 MMIO */
	if (!found || !pfn_valid(pfn))
		return 0;

	/* 4. 先轉 phys_addr_t 再 shift,避免溢位 */
	return (long)(((phys_addr_t)pfn << PAGE_SHIFT) | (vaddr & ~PAGE_MASK));
}
