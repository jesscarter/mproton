// ladder.h: shared harness for the tests/ladder correctness programs.
//
// Every program prints one machine-readable line per check on stdout:
//
//   LADDER<TAB>STATUS<TAB>program.check<TAB>detail
//
// STATUS is PASS, FAIL, SKIP (could not run here: missing API, no device, wrong arch) or INFO
// (a measurement or fact that is neither: adapter names, weak-ordering counts on native ARM).
// The last line is
//
//   LADDER<TAB>DONE<TAB>program<TAB>pass=N fail=N skip=N info=N
//
// and the exit code is 0 only when there was no FAIL. A missing DONE line means the program
// crashed or hung; tests/ladder/run.sh records that as a failure of the whole program.
//
// Common arguments (every program):
//   --timeout SECS   watchdog: print FAIL program.watchdog and exit 3 after SECS (default 120,
//                    or LADDER_TIMEOUT from the environment)
//   --only PREFIX    run only checks whose name starts with PREFIX (programs that support it)
//
// Header-only on purpose: each program is a single translation unit plus this file, so
// build.sh stays a loop over clang invocations.
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__arm64ec__)
#define LADDER_ARCH "arm64ec"
#elif defined(__aarch64__)
#define LADDER_ARCH "aarch64"
#elif defined(__x86_64__)
#define LADDER_ARCH "x86_64"
#elif defined(__i386__)
#define LADDER_ARCH "i686"
#else
#error "unknown architecture"
#endif

// The code the CPU actually executes for this build: x86 code under FEX (x86_64, i686), or
// native ARM64 code (aarch64, and arm64ec, whose own code is ARM64 even though its ABI is x64's).
#if defined(__aarch64__) || defined(__arm64ec__)
#define LADDER_NATIVE_ARM 1
#else
#define LADDER_NATIVE_ARM 0
#endif

static const char *g_prog = "?";
static int g_pass, g_fail, g_skip, g_info;
static const char *g_only;
static CRITICAL_SECTION g_report_lock;

static void ladder_line(const char *status, const char *check, const char *fmt, va_list ap) {
    char detail[1024];
    vsnprintf(detail, sizeof detail, fmt, ap);
    for (char *p = detail; *p; p++)
        if (*p == '\t' || *p == '\n' || *p == '\r') *p = ' ';
    EnterCriticalSection(&g_report_lock);
    printf("LADDER\t%s\t%s.%s\t%s\n", status, g_prog, check, detail);
    fflush(stdout);
    LeaveCriticalSection(&g_report_lock);
}

#define LADDER_PRINTF(a, b) __attribute__((format(printf, a, b)))
LADDER_PRINTF(2, 3) static void pass(const char *check, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); ladder_line("PASS", check, fmt, ap); va_end(ap); InterlockedIncrement((LONG *)&g_pass);
}
LADDER_PRINTF(2, 3) static void fail(const char *check, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); ladder_line("FAIL", check, fmt, ap); va_end(ap); InterlockedIncrement((LONG *)&g_fail);
}
LADDER_PRINTF(2, 3) static void skip(const char *check, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); ladder_line("SKIP", check, fmt, ap); va_end(ap); InterlockedIncrement((LONG *)&g_skip);
}
LADDER_PRINTF(2, 3) static void info(const char *check, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); ladder_line("INFO", check, fmt, ap); va_end(ap); InterlockedIncrement((LONG *)&g_info);
}
// check(ok, name, fmt, ...): PASS or FAIL with the same detail.
LADDER_PRINTF(3, 4) static bool check(bool ok, const char *name, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); ladder_line(ok ? "PASS" : "FAIL", name, fmt, ap); va_end(ap);
    InterlockedIncrement(ok ? (LONG *)&g_pass : (LONG *)&g_fail);
    return ok;
}

static bool want(const char *check) { return !g_only || !strncmp(check, g_only, strlen(g_only)); }

static int ladder_done(void) {
    EnterCriticalSection(&g_report_lock);
    printf("LADDER\tDONE\t%s\tpass=%d fail=%d skip=%d info=%d\n", g_prog, g_pass, g_fail, g_skip, g_info);
    fflush(stdout);
    LeaveCriticalSection(&g_report_lock);
    return g_fail ? 1 : 0;
}

static DWORD g_watchdog_secs = 120;
static DWORD WINAPI ladder_watchdog(void *) {
    Sleep(g_watchdog_secs * 1000);
    EnterCriticalSection(&g_report_lock);   // never released: nothing else may print after this
    printf("LADDER\tFAIL\t%s.watchdog\tstill running after %lus\n", g_prog, (unsigned long)g_watchdog_secs);
    printf("LADDER\tDONE\t%s\tpass=%d fail=%d skip=%d info=%d\n", g_prog, g_pass, g_fail + 1, g_skip, g_info);
    fflush(stdout);
    TerminateProcess(GetCurrentProcess(), 3);
    return 0;
}

static const char *machine_name(USHORT m) {
    switch (m) {
    case IMAGE_FILE_MACHINE_UNKNOWN: return "none";
    case IMAGE_FILE_MACHINE_I386: return "i386";
    case IMAGE_FILE_MACHINE_AMD64: return "amd64";
    case IMAGE_FILE_MACHINE_ARM64: return "arm64";
    case 0xa641: return "arm64ec";
    default: return "other";
    }
}

// Value of "--name VALUE" or NULL.
static const char *arg_value(int argc, char **argv, const char *name) {
    for (int i = 1; i + 1 < argc; i++)
        if (!strcmp(argv[i], name)) return argv[i + 1];
    return NULL;
}

// ladder_begin: name the program, start the watchdog, print the environment as INFO lines.
static void ladder_begin(const char *prog, int argc, char **argv) {
    g_prog = prog;
    InitializeCriticalSection(&g_report_lock);
    setvbuf(stdout, NULL, _IOFBF, 1 << 16);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    char env[32];
    if (GetEnvironmentVariableA("LADDER_TIMEOUT", env, sizeof env)) g_watchdog_secs = (DWORD)atoi(env);
    if (const char *t = arg_value(argc, argv, "--timeout")) g_watchdog_secs = (DWORD)atoi(t);
    g_only = arg_value(argc, argv, "--only");
    if (g_watchdog_secs) CloseHandle(CreateThread(NULL, 0, ladder_watchdog, NULL, 0, NULL));

    USHORT proc_m = 0, native_m = 0;
    typedef BOOL(WINAPI * iwp2_t)(HANDLE, USHORT *, USHORT *);
    iwp2_t iwp2 = (iwp2_t)(void *)GetProcAddress(GetModuleHandleA("kernel32.dll"), "IsWow64Process2");
    if (!iwp2 || !iwp2(GetCurrentProcess(), &proc_m, &native_m)) native_m = 0;
    HMODULE ntdll = GetModuleHandleA("ntdll.dll");
    typedef const char *(CDECL * wine_ver_t)(void);
    typedef void(CDECL * wine_host_t)(const char **, const char **);
    wine_ver_t wv = (wine_ver_t)(void *)GetProcAddress(ntdll, "wine_get_version");
    wine_host_t wh = (wine_host_t)(void *)GetProcAddress(ntdll, "wine_get_host_version");
    const char *sys = "?", *rel = "?";
    if (wh) wh(&sys, &rel);
    info("env", "build=%s wow64_process=%s native=%s wine=%s host=%s %s", LADDER_ARCH, machine_name(proc_m),
         native_m ? machine_name(native_m) : "unknown", wv ? wv() : "none", sys, rel);
}

// ---- SEH -------------------------------------------------------------------------------------
// A small function-based SEH API so every check runs the same way on all four architectures:
//   x86_64, aarch64, arm64ec: the compiler's __try/__except/__finally (table-based SEH: the
//     dispatcher walks .pdata/.xdata unwind info, through x64 frames under FEX for x86_64)
//   i686: frame-based SEH done by hand, the way MSVC-built 32-bit games do it: an
//     EXCEPTION_REGISTRATION_RECORD chained at fs:[0], RtlUnwind for the second pass, then a
//     jump back to the try site. llvm-mingw cannot compile __try for i686.
//
// seh_try(body, ctx, filter, fctx, &code): runs body(ctx). If an exception escapes it and
//   filter returns EXCEPTION_EXECUTE_HANDLER (a NULL filter takes everything), the stack is
//   unwound (running __finally handlers in between) and seh_try returns 1 with the code.
//   EXCEPTION_CONTINUE_EXECUTION resumes at the faulting point with the (possibly edited)
//   context. Returns 0 when body returns normally.
// seh_try_finally(body, ctx, fin, fctx): runs body, then fin(fctx, abnormal) on the way out,
//   whether body returned (abnormal=0) or an exception unwound through it (abnormal=1).
typedef LONG (*seh_filter_fn)(EXCEPTION_POINTERS *ep, void *fctx);
typedef void (*seh_body_fn)(void *ctx);
typedef void (*seh_fin_fn)(void *fctx, int abnormal);

#if defined(__i386__)
struct x86_reg {
    x86_reg *prev;   // the two fields Windows/Wine read: chain link and handler
    void *handler;
    int kind;        // 0 = __except, 1 = __finally
    seh_filter_fn filter; void *fctx;
    seh_fin_fn fin; void *finctx;
    DWORD code;
    void *jb[5];     // __builtin_setjmp buffer
};
static inline x86_reg *x86_seh_head(void) { x86_reg *p; __asm__ volatile("movl %%fs:0, %0" : "=r"(p)); return p; }
static inline void x86_seh_set(x86_reg *p) { __asm__ volatile("movl %0, %%fs:0" ::"r"(p) : "memory"); }

static EXCEPTION_DISPOSITION __cdecl x86_seh_handler(EXCEPTION_RECORD *rec, void *frame, CONTEXT *ctx, void *) {
    x86_reg *r = (x86_reg *)frame;
    if (rec->ExceptionFlags & (EXCEPTION_UNWINDING | EXCEPTION_EXIT_UNWIND)) {
        if (r->kind == 1 && r->fin) { seh_fin_fn f = r->fin; r->fin = NULL; f(r->finctx, 1); }
        return ExceptionContinueSearch;
    }
    if (r->kind == 1) return ExceptionContinueSearch;
    EXCEPTION_POINTERS ep = {rec, ctx};
    LONG v = r->filter ? r->filter(&ep, r->fctx) : EXCEPTION_EXECUTE_HANDLER;
    if (v == EXCEPTION_CONTINUE_EXECUTION) return ExceptionContinueExecution;
    if (v != EXCEPTION_EXECUTE_HANDLER) return ExceptionContinueSearch;
    r->code = rec->ExceptionCode;
    RtlUnwind(frame, NULL, rec, NULL);   // second pass: unwind handlers of the frames below us
    __builtin_longjmp(r->jb, 1);
}

__attribute__((noinline)) static int seh_try(seh_body_fn body, void *ctx, seh_filter_fn filter, void *fctx, DWORD *code) {
    x86_reg r = {};
    x86_reg *volatile rp = &r;
    r.prev = x86_seh_head(); r.handler = (void *)x86_seh_handler; r.kind = 0; r.filter = filter; r.fctx = fctx;
    if (__builtin_setjmp(r.jb)) {
        x86_seh_set(rp->prev);
        if (code) *code = rp->code;
        return 1;
    }
    x86_seh_set(&r);
    body(ctx);
    x86_seh_set(rp->prev);
    return 0;
}

__attribute__((noinline)) static void seh_try_finally(seh_body_fn body, void *ctx, seh_fin_fn fin, void *fctx) {
    x86_reg r = {};
    x86_reg *volatile rp = &r;
    r.prev = x86_seh_head(); r.handler = (void *)x86_seh_handler; r.kind = 1; r.fin = fin; r.finctx = fctx;
    x86_seh_set(&r);
    body(ctx);
    x86_seh_set(rp->prev);
    if (rp->fin) fin(fctx, 0);
}
#else
__attribute__((noinline)) static int seh_try(seh_body_fn body, void *ctx, seh_filter_fn filter, void *fctx, DWORD *code) {
    volatile DWORD c = 0;
    __try {
        body(ctx);
    } __except (c = GetExceptionCode(), filter ? filter(GetExceptionInformation(), fctx) : EXCEPTION_EXECUTE_HANDLER) {
        if (code) *code = c;
        return 1;
    }
    return 0;
}

__attribute__((noinline)) static void seh_try_finally(seh_body_fn body, void *ctx, seh_fin_fn fin, void *fctx) {
    __try {
        body(ctx);
    } __finally {
        fin(fctx, AbnormalTermination() ? 1 : 0);
    }
}
#endif

// Trap helpers, kept out of line so the fault happens in a callee of the __try body (clang
// only guarantees SEH coverage for calls, not for faulting instructions inlined into it).
__attribute__((noinline)) static int read_int(volatile int *p) { return *p; }
__attribute__((noinline)) static void write_int(volatile int *p, int v) { *p = v; }
__attribute__((noinline)) static unsigned char read_byte(volatile unsigned char *p) { return *p; }
__attribute__((noinline)) static void write_byte(volatile unsigned char *p, unsigned char v) { *p = v; }

// Integer divide by zero the way each architecture's compiled code does it: x86 has a
// faulting idiv; ARM64's sdiv returns 0, so MSVC emits a check and "brk #0xf003", which
// Windows (and Wine's signal_arm64.c) turns into STATUS_INTEGER_DIVIDE_BY_ZERO.
__attribute__((noinline)) static int divide(int a, int b) {
#if LADDER_NATIVE_ARM
    if (b == 0) __asm__ volatile("brk #0xf003");
    return a / (b ? b : 1);
#else
    int q;
    __asm__ volatile("cltd\n\tidivl %2" : "=a"(q) : "a"(a), "r"(b) : "edx", "cc");
    return q;
#endif
}

// Simple spin helpers for the multi-threaded checks.
static inline void cpu_relax(void) {
#if LADDER_NATIVE_ARM
    __asm__ volatile("yield" ::: "memory");
#else
    __asm__ volatile("pause" ::: "memory");
#endif
}

static double now_ms(void) {
    static LARGE_INTEGER f;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    LARGE_INTEGER t; QueryPerformanceCounter(&t);
    return (double)t.QuadPart * 1000.0 / (double)f.QuadPart;
}
