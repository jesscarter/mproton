// Probe for the macOS 26.x+ APIs ARM64 Wine needs. Looks symbols up at runtime so it
// builds with older SDKs, then tries to use them (expected to fail without the
// cross-architecture entitlement; the error codes tell us how).
#include <dlfcn.h>
#include <errno.h>
#include <mach/mach.h>
#include <spawn.h>
#include <stdio.h>
#include <string.h>
#include <sys/sysctl.h>
#include <unistd.h>

extern char **environ;

static void *sym(const char *n) {
    void *p = dlsym(RTLD_DEFAULT, n);
    printf("%-44s %s\n", n, p ? "present" : "MISSING");
    return p;
}

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "--child")) {
        printf("child: getpagesize()=%d vm_page_size=%lu\n", getpagesize(), (unsigned long)vm_page_size);
        return 0;
    }
    printf("parent: getpagesize()=%d\n", getpagesize());
    int (*set4k)(posix_spawnattr_t *, int) = sym("posix_spawnattr_set_4k_page_size_np");
    sym("posix_spawnattr_get_4k_page_size_np");
    kern_return_t (*tso)(thread_t, int) = sym("thread_set_x86_64_compat");
    sym("thread_get_x86_64_compat");
    sym("os_arch_x18_custom_abi");

    int v = 0; size_t sz = sizeof v;
    if (!sysctlbyname("hw.optional.arm.FEAT_AFP", &v, &sz, NULL, 0)) printf("FEAT_AFP=%d\n", v);
    sz = sizeof v;
    if (!sysctlbyname("sysctl.proc_translated", &v, &sz, NULL, 0)) printf("proc_translated=%d\n", v);

    if (tso) {
        kern_return_t kr = tso(mach_thread_self(), 1);
        printf("thread_set_x86_64_compat(self, 1) -> %d (%s)\n", kr, mach_error_string(kr));
    }
    if (set4k) {
        posix_spawnattr_t a; posix_spawnattr_init(&a);
        int r = set4k(&a, 1);
        printf("posix_spawnattr_set_4k_page_size_np(1) -> %d (%s)\n", r, strerror(r));
        pid_t pid; char *args[] = {argv[0], "--child", NULL};
        r = posix_spawn(&pid, argv[0], NULL, &a, args, environ);
        printf("posix_spawn with 4K attr -> %d (%s)\n", r, strerror(r));
        if (!r) { int st; waitpid(pid, &st, 0); }
    }
    return 0;
}
