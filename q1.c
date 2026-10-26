#define _GNU_SOURCE

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#ifndef SYS_MY_GET_PHYSICAL
#define SYS_MY_GET_PHYSICAL 449
#endif

static long page_size;
static uintptr_t page_mask;

static void *my_get_physical_addresses(void *vaddr)
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

static void *xmmap(void *addr, size_t len, int prot, int flags)
{
    void *p = mmap(addr, len, prot, flags, -1, 0);
    if (p == MAP_FAILED) {
        perror("mmap");
        exit(EXIT_FAILURE);
    }
    return p;
}

static void print_res(const char *name, int pass, const char *msg)
{
    printf("  [%s] %-30s : %s\n", pass ? "PASS" : "FAIL", name, msg);
}

static void print_info(const char *name, const char *msg)
{
    printf("  [INFO] %-30s : %s\n", name, msg);
}

static void print_va_pa_row(const char *label, void *va, void *pa)
{
    uintptr_t v_val = (uintptr_t)va;
    uintptr_t p_val = (uintptr_t)pa;
    uintptr_t offset_mask = (uintptr_t)page_size - 1;

    if (pa == NULL) {
        printf("  %-12s VA: %p [Offset]=0x%03lx -> PA: 0 (Unmapped)\n",
               label, va, (unsigned long)(v_val & offset_mask));
    } else {
        printf("  %-12s VA: %p [Offset]=0x%03lx -> PA: %#lx [Offset]=0x%03lx\n",
               label, va, (unsigned long)(v_val & offset_mask),
               (unsigned long)pa, (unsigned long)(p_val & offset_mask));
    }
}

static void test_state_machine(void)
{
    printf("====================================================================\n");
    printf("[Part A] The Memory State Machine & Offset Preservation\n");
    printf("====================================================================\n");

    char *pages = xmmap(NULL, page_size * 2, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS);

    /* State 1: 未存取 */
    printf("\n--- State 1: Virtual Mapping Established, Physical Page Not Populated ---\n");
    void *pa0_s1   = my_get_physical_addresses(&pages[0]);
    void *pa100_s1 = my_get_physical_addresses(&pages[100]);
    void *pa4k_s1  = my_get_physical_addresses(&pages[page_size]);

    print_va_pa_row("Byte 0", &pages[0], pa0_s1);
    print_va_pa_row("Byte 100", &pages[100], pa100_s1);
    print_va_pa_row("Byte 4096", &pages[page_size], pa4k_s1);
    print_res("Lazy Allocation", (pa0_s1 == NULL && pa100_s1 == NULL && pa4k_s1 == NULL),
              "Unaccessed virtual pages return 0 (no present physical mapping).");

    /* State 2: 唯讀存取 */
    printf("\n--- State 2: Read Fault maps to Global Zero Page ---\n");
    volatile char r0   = *(volatile char *)&pages[0];
    volatile char r100 = *(volatile char *)&pages[100];
    volatile char r4k  = *(volatile char *)&pages[page_size];
    (void)r0; (void)r100; (void)r4k;

    void *pa0_s2   = my_get_physical_addresses(&pages[0]);
    void *pa100_s2 = my_get_physical_addresses(&pages[100]);
    void *pa4k_s2  = my_get_physical_addresses(&pages[page_size]);

    print_va_pa_row("Byte 0", &pages[0], pa0_s2);
    print_va_pa_row("Byte 100", &pages[100], pa100_s2);
    print_va_pa_row("Byte 4096", &pages[page_size], pa4k_s2);

    uintptr_t offset_mask = (uintptr_t)page_size - 1;
    int offset_check = (((uintptr_t)&pages[100] & offset_mask) ==
                        ((uintptr_t)pa100_s2 & offset_mask));
    print_res("Page Offset Preservation", offset_check,
              "Page-table translation preserves lower offset bits (VA offset == PA offset).");

    int zero_page_check = (pa0_s2 != NULL) &&
                          ((uintptr_t)pa4k_s2 == (uintptr_t)pa0_s2) &&
                          (((uintptr_t)pa100_s2 & page_mask) == (uintptr_t)pa0_s2);
    print_res("Global Zero Page Collapse", zero_page_check,
              "Independent virtual pages share the same Global Zero Page on read fault.");

    /* State 3: 寫入配置私有分頁 */
    printf("\n--- State 3: Write Protection Fault allocates Private Page ---\n");
    *(volatile char *)&pages[0] = 'X';

    void *pa0_s3   = my_get_physical_addresses(&pages[0]);
    void *pa100_s3 = my_get_physical_addresses(&pages[100]);
    void *pa4k_s3  = my_get_physical_addresses(&pages[page_size]);

    print_va_pa_row("Byte 0", &pages[0], pa0_s3);
    print_va_pa_row("Byte 100", &pages[100], pa100_s3);
    print_va_pa_row("Byte 4096", &pages[page_size], pa4k_s3);

    int s3_check = (pa0_s3 != pa0_s2) &&
                   ((uintptr_t)pa100_s3 == (uintptr_t)pa0_s3 + 100) &&
                   (pa4k_s3 == pa4k_s2) &&
                   (pa4k_s3 != NULL);
    print_res("Private Page Allocation", s3_check,
              "Byte 0 and 100 share the new private page; Byte 4096 remains at Global Zero Page.");

    munmap(pages, page_size * 2);
}

static void test_fragmentation_and_paging(void)
{
    printf("\n====================================================================\n");
    printf("[Part B] Page Granularity & Discontinuity Masking\n");
    printf("====================================================================\n");

    printf("\n--- Section 1: Page-Level Internal Fragmentation ---\n");
    size_t req = 120;

    char *guard = xmmap(NULL, page_size * 2, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS);
    char *p = xmmap(guard, req, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED);

    *(volatile char *)&p[0] = 'M';

    void *pa_0    = my_get_physical_addresses(&p[0]);
    void *pa_last = my_get_physical_addresses(&p[page_size - 1]);
    void *pa_out  = my_get_physical_addresses(&p[page_size]);

    print_va_pa_row("Byte 0 (Req)", &p[0], pa_0);
    print_va_pa_row("Byte 4095", &p[page_size - 1], pa_last);
    print_va_pa_row("Byte 4096", &p[page_size], pa_out);

    long unused = page_size - (long)req;
    printf("  Actual Requested Size  : %zu Bytes via mmap\n", req);
    printf("  Kernel Allocation Unit : %ld Bytes (Kernel enforces PAGE_ALIGN on VMA)\n", page_size);
    printf("  Unused Page Capacity   : %ld Bytes (%.2f%% of frame capacity outside request)\n",
           unused, ((double)unused / (double)page_size) * 100.0);

    int int_frag_check = (pa_0 != NULL) &&
                         (((uintptr_t)pa_last & page_mask) == ((uintptr_t)pa_0 & page_mask)) &&
                         (pa_out == NULL);
    print_res("Page Granularity Proof", int_frag_check,
              "Requested 120 bytes forced 4KB page population; trailing capacity trapped within frame.");

    munmap(guard, page_size * 2);

    printf("\n--- Section 2: Virtual Contiguity vs. Physical Non-contiguity ---\n");
    enum { N = 5 };
    char *ext   = xmmap(NULL, N * page_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS);
    char *noise = xmmap(NULL, N * page_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS);

    /* 交錯 touch：讓 ext 的實體頁之間更可能夾著別的頁 */
    for (int i = 0; i < N; i++) {
        *(volatile char *)&ext[i * page_size]   = 'E';
        *(volatile char *)&noise[i * page_size] = 'N';
    }

    void *pas[N];
    int va_continuous = 1, pa_all_present = 1, pa_distinct = 1, pa_has_discontinuity = 0;

    for (int i = 0; i < N; i++) {
        void *cur_va = ext + i * page_size;
        pas[i] = my_get_physical_addresses(cur_va);
        if (pas[i] == NULL) pa_all_present = 0;

        printf("  Page %d | VA: %p -> PA: %#lx", i, cur_va, (unsigned long)pas[i]);
        if (i > 0) {
            uintptr_t prev_va = (uintptr_t)(ext + (i - 1) * page_size);
            if ((uintptr_t)cur_va - prev_va != (uintptr_t)page_size) va_continuous = 0;

            long stride = (long)((uintptr_t)pas[i] - (uintptr_t)pas[i - 1]);
            printf(" (Physical Stride: %+ld Bytes)", stride);
            if (stride != (long)page_size) pa_has_discontinuity = 1;
        }
        printf("\n");
    }
    for (int i = 0; i < N; i++)
        for (int j = i + 1; j < N; j++)
            if (pas[i] == pas[j]) pa_distinct = 0;

    print_res("VA Linear Continuity", va_continuous,
              "Virtual address increases strictly linearly by PAGE_SIZE.");
    print_res("Distinct Present Frames", pa_all_present && pa_distinct,
              "Every virtual page is backed by its own present physical frame.");
    /* 物理是否連續取決於 buddy allocator，不保證，因此僅作觀察 */
    print_info("Physical Discontinuity",
               pa_has_discontinuity
                   ? "Observed: contiguous VA maps to non-contiguous PA."
                   : "Not observed this run (allocator happened to be contiguous; not guaranteed).");

    munmap(ext, N * page_size);
    munmap(noise, N * page_size);
}

int main(void)
{
    page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0) page_size = 4096;
    page_mask = ~((uintptr_t)page_size - 1);

    test_state_machine();
    test_fragmentation_and_paging();
    return 0;
}
