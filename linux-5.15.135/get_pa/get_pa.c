#include<linux/kernel.h>
#include<linux/syscalls.h>


SYSCALL_DEFINE1(get_pa, void *vaddr) {
    printk("Hello world\n");
    return 0;
}