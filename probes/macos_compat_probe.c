// Probe for the macOS 26.x+ APIs ARM64 Wine needs. Looks symbols up at runtime so it
// builds with older SDKs, then tries to use them. Without the cross-architecture
// entitlement both are expected to fail; the error codes tell us how.
//
// Every finding is also printed as a "RESULT key=value" line, which tools/amfi-check.sh
// parses. Steps are reported separately, so a failure in one doesn't hide the others.
//
//   probe                          run every step; the 4K spawn re-runs this binary
//   probe --spawn-child PATH ...   spawn each PATH (built from this file) with the 4K attribute
//   probe --child                  (internal) report the page size and exit
//   probe --mp-iters N             iterations of the message-passing litmus (default 20M)
#include <dlfcn.h>
#include <errno.h>
#include <mach/mach.h>
#include <pthread.h>
#include <spawn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

typedef kern_return_t (*tso_fn)(thread_t, int);
typedef int (*set4k_fn)(posix_spawnattr_t *, int);

static tso_fn g_tso;

static void *sym(const char *n) {
    void *p = dlsym(RTLD_DEFAULT, n);
    printf("%-44s %s\n", n, p ? "present" : "MISSING");
    printf("RESULT sym.%s=%s\n", n, p ? "present" : "missing");
    return p;
}

// ---- message-passing litmus -----------------------------------------------------------
// The writer stores data=i then flag=i; the reader loads flag then data. Under TSO the
// reader can never see data < flag. Arm's weak ordering allows it (load-load reordering),
// so violations with TSO off are possible but not guaranteed; with TSO on they must be 0.

struct mp_shared {
    volatile uint64_t data; char pad0[120];
    volatile uint64_t flag; char pad1[120];
    volatile int go;
    int tso;          // request TSO on both threads
    uint64_t iters;
    int writer_kr;    // thread_set_x86_64_compat result in each thread (or -1 = not asked)
    int reader_kr;
    uint64_t violations;
};

#define COMPILER_BARRIER() __asm__ __volatile__("" ::: "memory")

static int set_tso(int on) {
    if (!g_tso) return -1;
    return (int)g_tso(mach_thread_self(), on);
}

static void *mp_writer(void *arg) {
    struct mp_shared *s = arg;
    s->writer_kr = s->tso ? set_tso(1) : -1;
    while (!s->go) {}
    for (uint64_t i = 1; i <= s->iters; i++) {
        s->data = i;
        COMPILER_BARRIER();
        s->flag = i;
    }
    if (s->tso) set_tso(0);
    return NULL;
}

static void *mp_reader(void *arg) {
    struct mp_shared *s = arg;
    s->reader_kr = s->tso ? set_tso(1) : -1;
    s->go = 1;
    uint64_t bad = 0, f;
    do {
        f = s->flag;
        COMPILER_BARRIER();
        uint64_t d = s->data;
        if (d < f) bad++;
    } while (f < s->iters);
    s->violations = bad;
    if (s->tso) set_tso(0);
    return NULL;
}

static void mp_litmus(const char *label, int tso, uint64_t iters) {
    static struct mp_shared s __attribute__((aligned(128)));
    memset((void *)&s, 0, sizeof s);
    s.tso = tso;
    s.iters = iters;
    pthread_t w, r;
    pthread_create(&r, NULL, mp_reader, &s);
    pthread_create(&w, NULL, mp_writer, &s);
    pthread_join(w, NULL);
    pthread_join(r, NULL);
    printf("message-passing litmus, TSO %s: %llu violations in %llu iterations",
           label, (unsigned long long)s.violations, (unsigned long long)iters);
    if (tso) printf(" (thread_set_x86_64_compat: writer %d, reader %d)", s.writer_kr, s.reader_kr);
    printf("\n");
    printf("RESULT mp_%s.violations=%llu\n", label, (unsigned long long)s.violations);
    if (tso) printf("RESULT mp_%s.thread_kr=%d,%d\n", label, s.writer_kr, s.reader_kr);
}

// ---- 4K-page spawn ----------------------------------------------------------------------

static const char *base(const char *p) {
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}

static void spawn_4k(set4k_fn set4k, const char *path) {
    const char *label = base(path);
    posix_spawnattr_t a;
    posix_spawnattr_init(&a);
    int r = set4k(&a, 1);
    printf("[%s] posix_spawnattr_set_4k_page_size_np(1) -> %d (%s)\n", label, r, strerror(r));
    printf("RESULT spawn4k.%s.attr=%d\n", label, r);

    int fds[2];
    if (pipe(fds)) { perror("pipe"); return; }
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, fds[1], STDOUT_FILENO);
    posix_spawn_file_actions_addclose(&fa, fds[0]);

    pid_t pid;
    char *args[] = {(char *)path, "--child", NULL};
    fflush(stdout);
    r = posix_spawn(&pid, path, &fa, &a, args, environ);
    close(fds[1]);
    printf("[%s] posix_spawn with 4K attr -> %d (%s)\n", label, r, strerror(r));
    printf("RESULT spawn4k.%s.rc=%d\n", label, r);
    char buf[512] = {0};
    if (!r) {
        size_t n = 0;
        ssize_t got;
        while (n < sizeof buf - 1 && (got = read(fds[0], buf + n, sizeof buf - 1 - n)) > 0) n += (size_t)got;
        int st = 0;
        waitpid(pid, &st, 0);
        int page = 0;
        const char *p = strstr(buf, "child_page_size=");
        if (p) page = atoi(p + strlen("child_page_size="));
        if (WIFSIGNALED(st)) {
            printf("[%s] child killed by signal %d\n", label, WTERMSIG(st));
            printf("RESULT spawn4k.%s.child_signal=%d\n", label, WTERMSIG(st));
        } else {
            printf("[%s] child exited %d, page size %d\n", label, WEXITSTATUS(st), page);
            printf("RESULT spawn4k.%s.child_exit=%d\n", label, WEXITSTATUS(st));
        }
        printf("RESULT spawn4k.%s.child_page_size=%d\n", label, page);
    }
    close(fds[0]);
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&a);
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (argc > 1 && !strcmp(argv[1], "--child")) {
        printf("child: getpagesize()=%d vm_page_size=%lu\n", getpagesize(), (unsigned long)vm_page_size);
        printf("RESULT child_page_size=%d\n", getpagesize());
        return 0;
    }
    const char *children[16];
    int nchildren = 0;
    uint64_t mp_iters = 20000000;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--spawn-child") && i + 1 < argc && nchildren < 16) children[nchildren++] = argv[++i];
        else if (!strcmp(argv[i], "--mp-iters") && i + 1 < argc) mp_iters = strtoull(argv[++i], NULL, 10);
        else { fprintf(stderr, "unknown argument %s\n", argv[i]); return 2; }
    }
    if (!nchildren) children[nchildren++] = argv[0];

    printf("RESULT launched=1\n");
    printf("parent: getpagesize()=%d\n", getpagesize());
    printf("RESULT page_size=%d\n", getpagesize());
    set4k_fn set4k = (set4k_fn)sym("posix_spawnattr_set_4k_page_size_np");
    sym("posix_spawnattr_get_4k_page_size_np");
    g_tso = (tso_fn)sym("thread_set_x86_64_compat");
    sym("thread_get_x86_64_compat");
    sym("os_arch_x18_custom_abi");

    int v = 0; size_t sz = sizeof v;
    if (!sysctlbyname("hw.optional.arm.FEAT_AFP", &v, &sz, NULL, 0)) printf("FEAT_AFP=%d\n", v);
    sz = sizeof v;
    if (!sysctlbyname("sysctl.proc_translated", &v, &sz, NULL, 0)) printf("proc_translated=%d\n", v);

    // Step 1: per-thread TSO.
    if (g_tso) {
        kern_return_t kr = g_tso(mach_thread_self(), 1);
        printf("thread_set_x86_64_compat(self, 1) -> %d (%s)\n", kr, mach_error_string(kr));
        printf("RESULT tso_set=%d\n", kr);
        if (kr == KERN_SUCCESS) {
            kr = g_tso(mach_thread_self(), 0);
            printf("thread_set_x86_64_compat(self, 0) -> %d (%s)\n", kr, mach_error_string(kr));
            printf("RESULT tso_clear=%d\n", kr);
        }
    }

    // Step 2: does TSO actually change ordering? Informational: a weakly ordered run can
    // still show 0 violations, but a TSO run must.
    if (mp_iters) {
        mp_litmus("off", 0, mp_iters);
        if (g_tso) mp_litmus("on", 1, mp_iters);
    }

    // Step 3: 4K-page spawn, once per child binary.
    if (set4k)
        for (int i = 0; i < nchildren; i++) spawn_4k(set4k, children[i]);

    printf("RESULT done=1\n");
    return 0;
}
