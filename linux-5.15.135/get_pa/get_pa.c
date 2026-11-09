#include <linux/kernel.h>
#include <linux/syscalls.h>
#include <linux/sched.h>
#include <linux/mm.h>

SYSCALL_DEFINE1(get_pa, void __user *, vaddr)
{
	pr_info("Syscall 449 gotten address input %px\n", vaddr);

	struct mm_struct * mm = current->mm;

	// Check if mm is NULL or vaddr exceeds user space limit
	if (!mm || (unsigned long) vaddr >= TASK_SIZE) {
		pr_err("Not a valid user process or address to lookup\n");
		// usually "return -EFAULT;"
		// But project requires returning 0 for lookup errors
		return 0;
	}

	// Acquire read lock of mm
	if (mmap_read_lock_killable(mm)) {
		pr_err("Failed to obtain the lock\n");
		// usually "return -EINTR;"
		// But project requires returning 0 for interrupted lock acquisition
		return 0;
	}

	// Travers through PGD->P4D->PUD->PMD->PTE
	// Use parts of vaddr as table index to find the next level table

	// Get PGD address and check if valid
	pgd_t * PGD = pgd_offset(mm, (unsigned long) vaddr);
	if (pgd_none(*PGD) || pgd_bad(*PGD)) {
		pr_info("PGD not present or invalid\n");
		mmap_read_unlock(mm);
		return 0;
	}

	// Get P4D address and check if valid
	// No need to check huge page because source code always return 0
	p4d_t * P4D = p4d_offset(PGD, (unsigned long) vaddr);
	if (p4d_none(*P4D) || p4d_bad(*P4D)) {
		pr_info("P4D not present or invalid\n");
		mmap_read_unlock(mm);
		return 0;
	}

	// Get PUD address and check if valid
	pud_t * PUD = pud_offset(P4D, (unsigned long) vaddr);
	if (pud_none(*PUD)) {
		pr_info("PUD not present\n");
		mmap_read_unlock(mm);
		return 0;
	}

	// Check if PUD points to leaf (huge page, usually 1GB)
	if (pud_leaf(*PUD)) {
		unsigned long pfn = pud_pfn(*PUD);
		// Physical frame number of this huge page
		if (!pfn_valid(pfn)) {
			pr_info("Page frame number not valid\n");
			mmap_read_unlock(mm);
			return 0;
		}
		pr_info("PUD points to huge page\n");
		// calculate physical frame address (by shifting pfn bits by 12)
		phys_addr_t pf_base_addr = (phys_addr_t) pfn << PAGE_SHIFT;
		// calculate address offset (mask high bits) in this page (by frame size)
		phys_addr_t offset = (unsigned long) vaddr & (pud_leaf_size(*PUD) - 1);
		phys_addr_t paddr = pf_base_addr + offset; // can also use "or" other than "add"
		mmap_read_unlock(mm);
		return (long) paddr;
	}

	// Check if PUD bad after checking leaf (leaf also triggers bad)
	if (pud_bad(*PUD)) {
		pr_info("PUD invalid\n");
		mmap_read_unlock(mm);
		return 0;
	}

	// Get PMD address and check if valid
	pmd_t * PMD = pmd_offset(PUD, (unsigned long) vaddr);
	if (pmd_none(*PMD)) {
		pr_info("PMD not present or invalid\n");
		mmap_read_unlock(mm);
		return 0;
	}

	if (pmd_leaf(*PMD)) {
		// Acquire lock of PMD and check again
		// (huge page might be split by other threads during check)
		spinlock_t *ptl = pmd_lock(mm, PMD);
		// Retrieve latest PMD entry
		pmd_t pmdval = READ_ONCE(*PMD);
		// check if PMD present (still in memory) & still a huge page
		if (pmd_present(pmdval) && pmd_leaf(pmdval)) {
			unsigned long pfn = pmd_pfn(pmdval);
			if (!pfn_valid(pfn)) {
				pr_info("Page frame number not valid\n");
				spin_unlock(ptl);
				mmap_read_unlock(mm);
				return 0; // not a valid frame
			}
			phys_addr_t pf_base_addr = (phys_addr_t) pfn << PAGE_SHIFT;
			phys_addr_t offset = (unsigned long) vaddr & (pmd_leaf_size(pmdval) - 1);
			phys_addr_t paddr = pf_base_addr + offset;
			// unlock before return
			spin_unlock(ptl);
			mmap_read_unlock(mm);
			return (long) paddr;
		}
		// Already been split or not present anymore
		// Continue to later checks
		spin_unlock(ptl);
	}

	if (pmd_bad(*PMD)) {
		pr_info("PMD invalid\n");
		mmap_read_unlock(mm);
		return 0;
	}


	// Get PTE address and check if valid
	spinlock_t *ptl;
	// pass spinlock by reference
	// Acquire temporary kernel mapping to PTE (need unmap later)
	pte_t * PTE = pte_offset_map_lock(mm, PMD, (unsigned long) vaddr, &ptl);
	// Check if PTE present (has valid physical mapping)
	if (!pte_present(*PTE)) {
		pr_info("PTE not valid\n");
		pte_unmap_unlock(PTE, ptl);
		mmap_read_unlock(mm);
		return 0;
	}

	unsigned long pfn = pte_pfn(*PTE);
	if (!pfn_valid(pfn)) {
		pr_info("Page frame number not valid\n");
		pte_unmap_unlock(PTE, ptl);
		mmap_read_unlock(mm);
		return 0;
	}

	phys_addr_t pf_base_addr = (phys_addr_t) pfn << PAGE_SHIFT;
	phys_addr_t offset = (unsigned long) vaddr & ~PAGE_MASK;
	phys_addr_t paddr = pf_base_addr + offset;

	// Unmap & unlock
	pte_unmap_unlock(PTE, ptl);
	mmap_read_unlock(mm);
	return (long) paddr;
}
