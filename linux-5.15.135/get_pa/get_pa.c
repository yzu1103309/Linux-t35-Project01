#include<linux/kernel.h>
#include<linux/syscalls.h>

SYSCALL_DEFINE1(get_pa, void __user *, vaddr)
{
	// printk(KERN_INFO "Hello world\n");
	pr_info("Hello world");
	return 0;
}
