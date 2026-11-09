#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef SYS_MY_GET_PHYSICAL
#define SYS_MY_GET_PHYSICAL 449
#endif

static void * test_syscall(void *vaddr)
{
    errno = 0;
    long ret = syscall(SYS_MY_GET_PHYSICAL, vaddr);
    if (ret == -1 && errno != 0) {
        int e = errno;
        fprintf(stderr, "Fatal Error: syscall %d failed (errno=%d: %s)%s\n",
                SYS_MY_GET_PHYSICAL, e, strerror(e),
                e == ENOSYS ? " -- kernel does not implement it" : "");
        exit(EXIT_FAILURE);
    }
    return (void *)ret;
}

int main() {
    int num = 10;
    void *ret = test_syscall(&num);
    printf("Syscall Returned: %p (Physical Address)\n", ret);
}