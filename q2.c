#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/vfs.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef SYS_MY_GET_PHYSICAL
#define SYS_MY_GET_PHYSICAL 449
#endif
#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

#define TEST_DIR      "/var/tmp"
#define TEST_TEMPLATE TEST_DIR "/ncu_cache_XXXXXX"
#define PAD_SIZE      (128UL * 1024 * 1024)
#define TMPFS_MAGIC   0x01021994
#define DROP_RETRIES  5
#define DROP_RETRY_US 20000

#define SEP_THICK "=================================================================================="
#define SEP_THIN  "----------------------------------------------------------------------------------"

static long page_size;
static char g_path[sizeof(TEST_TEMPLATE) + 8];
static int  g_fd = -1;
static int  g_fail, g_inconclusive, g_skip;

struct a_result {
    void *va_libm, *va_file, *pa_libm, *pa_file;
};
struct b_state {
    void *handle, *sym, *pa_libm_before;
    int libm_shifted, file_shifted;
};

typedef enum { RES_PASS, RES_FAIL, RES_SKIP, RES_INCONCLUSIVE } res_t;

static void print_res(const char *name, res_t r, const char *msg)
{
    const char *t = "????";
    if (r == RES_PASS) t = "PASS";
    else if (r == RES_FAIL) { t = "FAIL"; g_fail++; }
    else if (r == RES_SKIP) { t = "SKIP"; g_skip++; }
    else if (r == RES_INCONCLUSIVE) { t = "INCONCLUSIVE"; g_inconclusive++; }
    printf("  [%s] %-36s : %s\n", t, name, msg);
}

static void *get_pa(void *va)
{
    errno = 0;
    long ret = syscall(SYS_MY_GET_PHYSICAL, va);
    if (ret == -1 && errno) {
        fprintf(stderr, "syscall %d failed: %s\n", SYS_MY_GET_PHYSICAL, strerror(errno));
        exit(1);
    }
    return (void *)ret;
}

static void *xmmap(void *a, size_t n, int prot, int flags, int fd)
{
    void *p = mmap(a, n, prot, flags, fd, 0);
    if (p == MAP_FAILED) { perror("mmap"); exit(1); }
    return p;
}

static int write_exact(int fd, const void *buf, size_t n)
{
    size_t off = 0;
    while (off < n) {
        ssize_t k = write(fd, (const char *)buf + off, n - off);
        if (k < 0 && errno == EINTR) continue;
        if (k <= 0) return -1;
        off += (size_t)k;
    }
    return 0;
}

static int read_exact(int fd, void *buf, size_t n)
{
    size_t off = 0;
    while (off < n) {
        ssize_t k = read(fd, (char *)buf + off, n - off);
        if (k < 0 && errno == EINTR) continue;
        if (k <= 0) return -1;
        off += (size_t)k;
    }
    return 0;
}

static int mincore1(void *addr)
{
    unsigned char v = 0;
    if (mincore(addr, page_size, &v) < 0) { perror("mincore"); return -1; }
    return v & 1;
}

static int64_t kpagecount(void *pa)
{
    if (!pa) return -1;
    int fd = open("/proc/kpagecount", O_RDONLY);
    if (fd < 0) return -1;
    unsigned long pfn = (uintptr_t)pa / (uintptr_t)page_size;
    uint64_t c = 0;
    ssize_t n = pread(fd, &c, sizeof(c), (off_t)(pfn * sizeof(c)));
    close(fd);
    return n == (ssize_t)sizeof(c) ? (int64_t)c : -1;
}

static int drop_caches(void)
{
    sync();
    int fd = open("/proc/sys/vm/drop_caches", O_WRONLY);
    if (fd < 0) return -1;
    int rc = (write(fd, "3\n", 2) == 2) ? 0 : -1;
    close(fd);
    return rc;
}

static void warn_tmpfs(void)
{
    struct statfs s;
    if (statfs(TEST_DIR, &s) == 0 && s.f_type == TMPFS_MAGIC)
        printf("  [WARN] %s is tmpfs; reclaim may not work.\n", TEST_DIR);
}

static int wait_ok(pid_t pid)
{
    int st;
    for (;;) {
        if (waitpid(pid, &st, 0) < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        break;
    }
    return (WIFEXITED(st) && WEXITSTATUS(st) == 0) ? 0 : -1;
}

static void *page_down(void *p)
{
    return (void *)((uintptr_t)p & ~((uintptr_t)page_size - 1));
}

static void cleanup_dummy(void)
{
    if (g_fd >= 0) {
        close(g_fd);
        g_fd = -1;
    }
    if (g_path[0]) {
        unlink(g_path);
        g_path[0] = 0;
    }
}

static int make_dummy(void)
{
    memcpy(g_path, TEST_TEMPLATE, sizeof(TEST_TEMPLATE));
    g_fd = mkstemp(g_path);
    if (g_fd < 0)
        return -1;

    if (fchmod(g_fd, 0600) < 0)
        goto fail;
    if (write_exact(g_fd, "KernelCacheDataPayload", 22) < 0)
        goto fail;
    if (lseek(g_fd, 0, SEEK_SET) < 0)
        goto fail;
    return 0;

fail:
    cleanup_dummy();
    return -1;
}

/* ---------- Part 1 ---------- */
static void test_backing_types(void)
{
    printf("[Part 1] Anonymous vs File-backed (dummy.bin)\n");
    printf("%s\n", SEP_THIN);

    char *a1 = xmmap(NULL, page_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1);
    char *a2 = xmmap(NULL, page_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1);
    if (lseek(g_fd, 0, SEEK_SET) < 0) { perror("lseek"); exit(1); }
    char *f = xmmap(NULL, page_size, PROT_READ, MAP_SHARED, g_fd);

    void *p1 = get_pa(a1), *p2 = get_pa(a2), *pf = get_pa(f);
    printf("  Before access:\n");
    printf("    anon1  VA %p  PA %#lx\n", (void *)a1, (unsigned long)p1);
    printf("    anon2  VA %p  PA %#lx\n", (void *)a2, (unsigned long)p2);
    printf("    file   VA %p  PA %#lx\n", (void *)f, (unsigned long)pf);
    print_res("No present PTE before access",
              (p1 || p2 || pf) ? RES_FAIL : RES_PASS,
              "PA=0 means no present mapping from syscall, not 'no page cache'");

    (void)*(volatile char *)a1;
    (void)*(volatile char *)a2;
    (void)*(volatile char *)f;
    p1 = get_pa(a1); p2 = get_pa(a2); pf = get_pa(f);
    printf("  After read:\n");
    printf("    anon1  VA %p  PA %#lx\n", (void *)a1, (unsigned long)p1);
    printf("    anon2  VA %p  PA %#lx\n", (void *)a2, (unsigned long)p2);
    printf("    file   VA %p  PA %#lx\n", (void *)f, (unsigned long)pf);
    print_res("Zero page + file frame",
              (p1 && p1 == p2 && pf && pf != p1) ? RES_PASS : RES_FAIL,
              "anon share PA; file different PA");

    munmap(a1, page_size); munmap(a2, page_size); munmap(f, page_size);
}

/* ---------- Part 2 ---------- */
static void test_cow(void)
{
    printf("\n[Part 2] Copy-on-Write\n");
    printf("%s\n", SEP_THIN);

    char *m = xmmap(NULL, page_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1);
    m[0] = 'P';
    void *pp = get_pa(m);
    printf("  Parent     VA %p  PA %#lx  '%c'\n", (void *)m, (unsigned long)pp, m[0]);

    int pfd[2];
    if (pipe(pfd) < 0) { perror("pipe"); exit(1); }
    fflush(stdout);
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        close(pfd[0]);
        close(pfd[1]);
        exit(1);
    }

    if (pid == 0) {
        close(pfd[0]);
        if (m[0] != 'P') _exit(2);
        void *pr = get_pa(m);
        m[0] = 'C';
        if (m[0] != 'C') _exit(3);
        void *pw = get_pa(m);
        if (write_exact(pfd[1], &pr, sizeof(pr)) < 0) _exit(4);
        if (write_exact(pfd[1], &pw, sizeof(pw)) < 0) _exit(5);
        close(pfd[1]);
        _exit(0);
    }
    close(pfd[1]);
    void *cr = NULL, *cw = NULL;
    int io = read_exact(pfd[0], &cr, sizeof(cr)) == 0 &&
             read_exact(pfd[0], &cw, sizeof(cw)) == 0;
    close(pfd[0]);
    int child = wait_ok(pid) == 0;
    void *pa = get_pa(m);
    printf("  Child read VA %p  PA %#lx\n", (void *)m, (unsigned long)cr);
    printf("  Child write VA %p  PA %#lx\n", (void *)m, (unsigned long)cw);
    printf("  Parent     VA %p  PA %#lx  '%c'\n", (void *)m, (unsigned long)pa, m[0]);

    int ok = io && child && m[0] == 'P' && pp && cr && cw && pa &&
             cr == pp && cw != pp && pa == pp;
    print_res("CoW", ok ? RES_PASS : RES_FAIL, "share then split; parent still 'P'");
    munmap(m, page_size);
}

/* ---------- Part 3 ---------- */
static int run_a(struct a_result *out)
{
    int pfd[2];
    if (pipe(pfd) < 0) return -1;
    fflush(stdout);
    pid_t pid = fork();
    if (pid < 0) {
        close(pfd[0]);
        close(pfd[1]);
        return -1;
    }

    if (pid == 0) {
        close(pfd[0]);

        /* Optional VA layout padding; failure must not be ignored. */
        void *pad = mmap(NULL, PAD_SIZE, PROT_NONE,
                         MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
        if (pad == MAP_FAILED)
            _exit(1);

        void *h = dlopen("libm.so.6", RTLD_LAZY);
        if (!h) _exit(1);
        void *sym = dlsym(h, "cos");
        if (!sym) _exit(1);
        (void)*(volatile char *)sym;

        int fd = open(g_path, O_RDONLY);
        if (fd < 0) _exit(1);
        char *fm = mmap(NULL, page_size, PROT_READ, MAP_SHARED, fd, 0);
        if (fm == MAP_FAILED) _exit(1);
        (void)*(volatile char *)fm;

        struct a_result r = {
            .va_libm = sym, .va_file = fm,
            .pa_libm = get_pa(sym), .pa_file = get_pa(fm)
        };
        printf("  A | libm.so   VA %p  PA %#lx\n", r.va_libm, (unsigned long)r.pa_libm);
        printf("  A | dummy.bin VA %p  PA %#lx\n", r.va_file, (unsigned long)r.pa_file);
        printf("  A exits (baseline kept for comparison)\n");
        fflush(stdout);

        if (write_exact(pfd[1], &r, sizeof(r)) < 0) _exit(1);
        close(pfd[1]);
        munmap(fm, page_size);
        close(fd);
        dlclose(h);
        _exit(0);
    }

    close(pfd[1]);
    int ok = read_exact(pfd[0], out, sizeof(*out)) == 0;
    close(pfd[0]);
    return (wait_ok(pid) == 0 && ok) ? 0 : -1;
}

static void run_b_and_reclaim(const struct a_result *a, struct b_state *b)
{
    void *lp = page_down(a->va_libm);
    void *fp = page_down(a->va_file);

    void *bl = mmap(lp, page_size, PROT_NONE,
                    MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    int lb = (bl != MAP_FAILED && bl == lp);
    if (bl != MAP_FAILED && !lb) { munmap(bl, page_size); bl = MAP_FAILED; }

    void *bf = mmap(fp, page_size, PROT_NONE,
                    MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    int fb = (bf != MAP_FAILED && bf == fp);
    if (bf != MAP_FAILED && !fb) { munmap(bf, page_size); bf = MAP_FAILED; }

    b->handle = dlopen("libm.so.6", RTLD_LAZY);
    if (!b->handle) { fprintf(stderr, "dlopen\n"); exit(1); }
    b->sym = dlsym(b->handle, "cos");
    if (!b->sym) exit(1);
    (void)*(volatile char *)b->sym;
    b->pa_libm_before = get_pa(b->sym);
    b->libm_shifted = (b->sym != a->va_libm) && lb;

    int fd = open(g_path, O_RDONLY);
    if (fd < 0) { perror("open"); exit(1); }
    char *fm = xmmap(NULL, page_size, PROT_READ, MAP_SHARED, fd);
    (void)*(volatile char *)fm;
    void *pa_f = get_pa(fm);
    b->file_shifted = ((void *)fm != a->va_file) && fb;

    printf("\n  B | libm.so   VA %p  PA %#lx  (A %p / %#lx)\n",
           b->sym, (unsigned long)b->pa_libm_before,
           a->va_libm, (unsigned long)a->pa_libm);
    printf("  B | dummy.bin VA %p  PA %#lx  (A %p / %#lx)\n",
           (void *)fm, (unsigned long)pa_f,
           a->va_file, (unsigned long)a->pa_file);

    print_res("VA shifted (libm + dummy.bin)",
              (b->libm_shifted && b->file_shifted) ? RES_PASS : RES_INCONCLUSIVE,
              "reserved baseline VAs so B differs");
    int same_pa = b->pa_libm_before && b->pa_libm_before == a->pa_libm &&
                  pa_f && pa_f == a->pa_file;
    print_res("Same PA as A baseline",
              same_pa ? RES_PASS : RES_INCONCLUSIVE,
              "page cache can be shared across different VAs");

    munmap(fm, page_size);
    if (bl != MAP_FAILED) munmap(bl, page_size);
    if (bf != MAP_FAILED) munmap(bf, page_size);

    printf("\n  Reclaim dummy.bin: unmap → drop_caches → remap → touch\n");
    fm = xmmap(NULL, page_size, PROT_READ, MAP_SHARED, fd);
    (void)*(volatile char *)fm;
    int r0 = mincore1(fm);
    void *pa0 = get_pa(fm);
    munmap(fm, page_size);

    int drop_ok = 0;
    for (int i = 0; i < DROP_RETRIES; i++) {
        if (drop_caches() == 0) { drop_ok = 1; break; }
        usleep(DROP_RETRY_US);
    }

    fm = xmmap(NULL, page_size, PROT_READ, MAP_SHARED, fd);
    int r1 = mincore1(fm);
    void *pa1 = get_pa(fm);
    (void)*(volatile char *)fm;
    int r2 = mincore1(fm);
    void *pa2 = get_pa(fm);

    printf("  resident: %d → (drop) → %d → (read) → %d\n", r0, r1, r2);
    printf("  PA: %#lx → (after drop, no touch) %#lx → (after read) %#lx\n",
           (unsigned long)pa0, (unsigned long)pa1, (unsigned long)pa2);

    res_t cr;
    const char *cm;
    if (r0 != 1 || !drop_ok) { cr = RES_INCONCLUSIVE; cm = "setup/drop incomplete"; }
    else if (r1 == 0 && r2 == 1) { cr = RES_PASS; cm = "1→0→1 observed"; }
    else if (r1 == 1) { cr = RES_INCONCLUSIVE; cm = "still resident after drop"; }
    else { cr = RES_INCONCLUSIVE; cm = "unexpected pattern"; }
    print_res("dummy.bin reclaim", cr, cm);
    if (cr == RES_PASS && pa0 && pa2 && pa0 != pa2)
        print_res("PA changed after reload", RES_PASS, "new frame");
    else
        print_res("PA changed after reload", RES_INCONCLUSIVE, "skipped or same PA");

    void *pa_l = get_pa(b->sym);
    int64_t mc = kpagecount(pa_l);
    printf("  libm still mapped: PA %#lx (was %#lx) kpagecount=%ld\n",
           (unsigned long)pa_l, (unsigned long)b->pa_libm_before, (long)mc);
    print_res("libm PA stable while mapped",
              (pa_l && b->pa_libm_before && pa_l == b->pa_libm_before && mc > 0)
                  ? RES_PASS : RES_INCONCLUSIVE,
              "still-mapped library page not observed to move");

    munmap(fm, page_size);
    close(fd);
}

static int test_page_cache(void)
{
    printf("\n[Part 3] Page cache: libm.so vs dummy.bin\n");
    printf("%s\n", SEP_THIN);
    printf("  libm.so   = shared library\n");
    printf("  dummy.bin = private data file for this run\n\n");

    if (geteuid() != 0) {
        print_res("root", RES_SKIP, "need drop_caches / kpagecount");
        return 0;
    }
    warn_tmpfs();

    struct a_result a;
    struct b_state b = {0};
    if (run_a(&a) < 0) {
        print_res("Process A", RES_FAIL, "failed");
        return 1;
    }
    run_b_and_reclaim(&a, &b);
    dlclose(b.handle);
    return 0;
}

int main(void)
{
    page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0) page_size = 4096;

    printf("%s\n", SEP_THICK);
    printf(" Linux Memory Management Experiment\n");
    printf("%s\n\n", SEP_THICK);

    if (make_dummy() < 0) {
        fprintf(stderr, "mkstemp dummy.bin failed\n");
        return 1;
    }

    test_backing_types();
    test_cow();
    int rc = test_page_cache();
    cleanup_dummy();

    printf("\n%s\n", SEP_THIN);
    printf("Summary: %d FAIL, %d INCONCLUSIVE, %d SKIP\n", g_fail, g_inconclusive, g_skip);
    printf("%s\n", SEP_THICK);

    if (g_fail || rc) return 1;
    if (g_inconclusive) return 2;
    return 0;
}
