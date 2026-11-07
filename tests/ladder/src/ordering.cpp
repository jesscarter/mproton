// ordering: memory-ordering litmus tests on plain (volatile, unfenced) loads and stores.
//
// x86 code assumes TSO: stores become visible in program order, loads are not reordered with
// other loads, and every CPU sees stores in the same order. ARM64 promises none of that for
// plain accesses. FEX gives x86 code TSO either with the M-series hardware TSO bit
// (FEX_HARDWARE_TSO=1) or by emitting ordered loads/stores and barriers (=0). A gap in either
// shows up here as an outcome that TSO forbids.
//
// Tests (each iteration starts all threads together from a barrier, after a random delay):
//   mp         T0: data=1; flag=1          T1: r0=flag; r1=data        forbidden r0=1 r1=0
//   mp_stream  T0 writes data[i] then flag[i] for 1M i; T1 spins on flag[i], then reads data[i]
//   lb         T0: r0=x; y=1               T1: r1=y; x=1               forbidden r0=1 r1=1
//   iriw       T0: x=1  T1: y=1  T2: r0=x; r1=y  T3: r2=y; r3=x        forbidden 1,0,1,0
//   sb         T0: x=1; r0=y               T1: y=1; r1=x               r0=0 r1=0 is ALLOWED by TSO
//   corr       T0: x=1                     T1: r0=x; r1=x              forbidden r0=1 r1=0 (coherence; every arch)
//   lock       4 threads, spinlock taken with an interlocked exchange and released with a plain
//              store (correct on x86, the pattern a lot of game code uses), guarding a plain counter
//   atomic     4 threads of InterlockedIncrement
// Every test except corr/atomic also runs a "_fenced" variant with MemoryBarrier() between the
// two accesses of each thread (and an interlocked release for lock); fenced runs are forbidden on
// every architecture.
//
// Expected results:
//   x86 builds (i686, x86_64; run under FEX on the Mac): every forbidden outcome must be 0,
//     plain and fenced. sb plain is reported as INFO.
//   native ARM builds (aarch64, arm64ec): plain mp/mp_stream/lb/iriw/sb/lock are INFO: ARM may
//     reorder them, and seeing a non-zero count there shows the harness can catch reordering on
//     this machine. Fenced variants, corr and atomic must be 0.
//
// Arguments: --iters N (iterations per barrier test, default 500000; mp_stream uses 10x)
#include "ladder.h"

static long g_iters = 500000;

struct alignas(128) Line { volatile LONG v; char pad[128 - sizeof(LONG)]; };
struct Shared {
    Line x, y, data, flag;
    Line r[4];
    Line lock;
    Line counter;
};
static Shared g;

// ---- iteration barrier ----------------------------------------------------------------------
struct alignas(128) Barrier { volatile LONG count; char pad1[124]; volatile LONG gen; char pad2[124]; LONG n; };
static Barrier g_bar;
static void barrier_wait(Barrier *b) {
    LONG gen = b->gen;
    if (InterlockedIncrement(&b->count) == b->n) {
        b->count = 0;
        InterlockedIncrement(&b->gen);
    } else {
        int spins = 0;
        while (b->gen == gen)
            if (++spins & 1023) cpu_relax(); else SwitchToThread();   // oversubscribed hosts
    }
    MemoryBarrier();
}

static inline uint32_t rnd(uint32_t *s) { *s ^= *s << 13; *s ^= *s >> 17; *s ^= *s << 5; return *s; }
static inline void jitter(uint32_t *s) { for (volatile uint32_t n = rnd(s) & 31; n; n = n - 1) {} }
#define FENCE(f) do { if (f) MemoryBarrier(); } while (0)

// ---- tests ---------------------------------------------------------------------------------------
struct Test {
    const char *name;
    int threads;
    bool tso_forbidden_plain;   // plain run must be clean on x86
    bool forbidden_everywhere;  // plain run must be clean on ARM too
    void (*reset)(void);
    void (*body)(int tid, bool fenced);
    bool (*weak)(void);         // the forbidden (or, for sb, the interesting) outcome
};

static void reset_xy(void) { g.x.v = 0; g.y.v = 0; g.data.v = 0; g.flag.v = 0; for (auto &r : g.r) r.v = -1; }

static void mp_body(int tid, bool f) {
    if (tid == 0) { g.data.v = 1; FENCE(f); g.flag.v = 1; }
    else { g.r[0].v = g.flag.v; FENCE(f); g.r[1].v = g.data.v; }
}
static bool mp_weak(void) { return g.r[0].v == 1 && g.r[1].v == 0; }

static void lb_body(int tid, bool f) {
    if (tid == 0) { g.r[0].v = g.x.v; FENCE(f); g.y.v = 1; }
    else { g.r[1].v = g.y.v; FENCE(f); g.x.v = 1; }
}
static bool lb_weak(void) { return g.r[0].v == 1 && g.r[1].v == 1; }

static void sb_body(int tid, bool f) {
    if (tid == 0) { g.x.v = 1; FENCE(f); g.r[0].v = g.y.v; }
    else { g.y.v = 1; FENCE(f); g.r[1].v = g.x.v; }
}
static bool sb_weak(void) { return g.r[0].v == 0 && g.r[1].v == 0; }

static void iriw_body(int tid, bool f) {
    switch (tid) {
    case 0: g.x.v = 1; break;
    case 1: g.y.v = 1; break;
    case 2: g.r[0].v = g.x.v; FENCE(f); g.r[1].v = g.y.v; break;
    case 3: g.r[2].v = g.y.v; FENCE(f); g.r[3].v = g.x.v; break;
    }
}
static bool iriw_weak(void) { return g.r[0].v == 1 && g.r[1].v == 0 && g.r[2].v == 1 && g.r[3].v == 0; }

static void corr_body(int tid, bool) {
    if (tid == 0) g.x.v = 1;
    else { g.r[0].v = g.x.v; g.r[1].v = g.x.v; }
}
static bool corr_weak(void) { return g.r[0].v == 1 && g.r[1].v == 0; }

static const Test k_tests[] = {
    {"mp", 2, true, false, reset_xy, mp_body, mp_weak},
    {"lb", 2, true, false, reset_xy, lb_body, lb_weak},
    {"iriw", 4, true, false, reset_xy, iriw_body, iriw_weak},
    {"sb", 2, false, false, reset_xy, sb_body, sb_weak},
    {"corr", 2, true, true, reset_xy, corr_body, corr_weak},
};

struct Run { const Test *t; bool fenced; long iters; long weak; int tid; };
static Run g_run;

static DWORD WINAPI litmus_thread(void *p) {
    int tid = (int)(intptr_t)p;
    uint32_t seed = 0x9E3779B9u * (tid + 1);
    for (long i = 0; i < g_run.iters; i++) {
        if (tid == 0) g_run.t->reset();
        barrier_wait(&g_bar);
        jitter(&seed);
        g_run.t->body(tid, g_run.fenced);
        barrier_wait(&g_bar);
        if (tid == 0 && g_run.t->weak()) g_run.weak++;
    }
    return 0;
}

static long run_threads(int n, LPTHREAD_START_ROUTINE fn) {
    HANDLE h[4];
    for (int i = 0; i < n; i++) h[i] = CreateThread(NULL, 0, fn, (void *)(intptr_t)i, 0, NULL);
    WaitForMultipleObjects(n, h, TRUE, INFINITE);
    for (int i = 0; i < n; i++) CloseHandle(h[i]);
    return 0;
}

static long g_plain_weak_total;   // weak outcomes seen in plain runs that ARM allows
static long g_sb_weak = -1;       // store buffering seen in sb plain: proof the threads really overlap

static void report(const char *name, long weak, long iters, bool must_be_zero, double ms, const char *why_allowed) {
    if (must_be_zero)
        check(weak == 0, name, "forbidden outcome %ld/%ld times (%.0f ms)", weak, iters, ms);
    else
        info(name, "weak outcome %ld/%ld times (%.0f ms); %s", weak, iters, ms, why_allowed);
}

static void litmus(const Test &t, bool fenced) {
    char name[64];
    snprintf(name, sizeof name, "%s%s", t.name, fenced ? "_fenced" : "");
    if (!want(name)) return;
    if (fenced && (!strcmp(t.name, "corr"))) return;
    g_run = {&t, fenced, g_iters, 0, 0};
    g_bar.count = 0; g_bar.gen = 0; g_bar.n = t.threads;
    double t0 = now_ms();
    run_threads(t.threads, litmus_thread);
    double ms = now_ms() - t0;
    bool must = fenced || t.forbidden_everywhere || (!LADDER_NATIVE_ARM && t.tso_forbidden_plain);
    if (!must && !fenced && strcmp(t.name, "sb")) g_plain_weak_total += g_run.weak;
    if (!fenced && !strcmp(t.name, "sb")) g_sb_weak = g_run.weak;
    report(name, g_run.weak, g_run.iters, must, ms,
           !strcmp(t.name, "sb") ? "allowed by TSO and by ARM (store buffering)" : "allowed for native ARM code");
}

// ---- mp_stream: no per-iteration barrier, the reader chases the writer ---------------------------
static volatile LONG *g_sdata, *g_sflag;
static long g_stream_n, g_stream_bad;
static bool g_stream_fenced;
static DWORD WINAPI stream_thread(void *p) {
    int tid = (int)(intptr_t)p;
    barrier_wait(&g_bar);
    if (tid == 0) {
        for (long i = 0; i < g_stream_n; i++) {
            g_sdata[i] = (LONG)i + 1;
            FENCE(g_stream_fenced);
            g_sflag[i] = 1;
        }
    } else {
        long bad = 0;
        for (long i = 0; i < g_stream_n; i++) {
            int spins = 0;
            while (!g_sflag[i]) if (++spins & 1023) cpu_relax(); else SwitchToThread();
            FENCE(g_stream_fenced);
            if (g_sdata[i] != (LONG)i + 1) bad++;
        }
        g_stream_bad = bad;
    }
    return 0;
}
static void mp_stream(bool fenced) {
    const char *name = fenced ? "mp_stream_fenced" : "mp_stream";
    if (!want(name)) return;
    g_stream_n = g_iters * 10;
    // data and flag far apart so they sit in different lines and pages
    g_sdata = (volatile LONG *)VirtualAlloc(NULL, g_stream_n * sizeof(LONG), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    g_sflag = (volatile LONG *)VirtualAlloc(NULL, g_stream_n * sizeof(LONG), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    g_stream_fenced = fenced; g_stream_bad = 0;
    g_bar.count = 0; g_bar.gen = 0; g_bar.n = 2;
    double t0 = now_ms();
    run_threads(2, stream_thread);
    bool must = fenced || !LADDER_NATIVE_ARM;
    if (!must) g_plain_weak_total += g_stream_bad;
    report(name, g_stream_bad, g_stream_n, must, now_ms() - t0, "allowed for native ARM code");
    VirtualFree((void *)g_sdata, 0, MEM_RELEASE);
    VirtualFree((void *)g_sflag, 0, MEM_RELEASE);
}

// ---- lock with a plain-store release, and plain atomics ------------------------------------------
static bool g_lock_fenced;
static long g_lock_iters;
static DWORD WINAPI lock_thread(void *) {
    barrier_wait(&g_bar);
    for (long i = 0; i < g_lock_iters; i++) {
        while (InterlockedExchange(&g.lock.v, 1)) while (g.lock.v) cpu_relax();
        g.counter.v = g.counter.v + 1;                   // plain read-modify-write inside the lock
        if (g_lock_fenced) InterlockedExchange(&g.lock.v, 0);
        else g.lock.v = 0;                               // x86-style release: a plain store
    }
    return 0;
}
static DWORD WINAPI atomic_thread(void *) {
    barrier_wait(&g_bar);
    for (long i = 0; i < g_lock_iters; i++) InterlockedIncrement(&g.counter.v);
    return 0;
}
static void lock_tests() {
    g_lock_iters = g_iters;
    for (int variant = 0; variant < 3; variant++) {
        const char *name = variant == 0 ? "lock" : variant == 1 ? "lock_fenced" : "atomic";
        if (!want(name)) continue;
        g.counter.v = 0; g.lock.v = 0; g_lock_fenced = variant == 1;
        g_bar.count = 0; g_bar.gen = 0; g_bar.n = 4;
        double t0 = now_ms();
        run_threads(4, variant == 2 ? atomic_thread : lock_thread);
        long lost = 4 * g_lock_iters - g.counter.v;
        bool must = variant != 0 || !LADDER_NATIVE_ARM;
        if (!must) g_plain_weak_total += lost;
        if (must)
            check(lost == 0, name, "%ld of %ld increments lost (%.0f ms)", lost, 4 * g_lock_iters, now_ms() - t0);
        else
            info(name, "%ld of %ld increments lost (%.0f ms); a plain-store unlock is not a release on native ARM", lost,
                 4 * g_lock_iters, now_ms() - t0);
    }
}

int main(int argc, char **argv) {
    if (const char *n = arg_value(argc, argv, "--iters")) g_iters = atol(n);
    ladder_begin("ordering", argc, argv);
    char env[16];
    info("config", "iterations %ld, %s code, FEX_HARDWARE_TSO=%s", g_iters, LADDER_NATIVE_ARM ? "native ARM64" : "x86",
         GetEnvironmentVariableA("FEX_HARDWARE_TSO", env, sizeof env) ? env : "(unset)");
    for (const Test &t : k_tests) { litmus(t, false); litmus(t, true); }
    mp_stream(false);
    mp_stream(true);
    lock_tests();
    // Calibration: sb plain (store buffering, allowed everywhere) only shows up when the threads
    // really run their bodies at the same time, so a non-zero count says the barrier harness
    // creates the races the forbidden tests look for. On native ARM the plain weak counts also
    // show that this machine reorders the accesses the x86 runs must keep in order.
    if (g_sb_weak >= 0)
        info("calibration.overlap", "sb store-buffering outcomes %ld: %s", g_sb_weak,
             g_sb_weak ? "threads do overlap" : "NONE, so the threads may not overlap and clean results are weak evidence (more --iters?)");
    if (LADDER_NATIVE_ARM && !g_only)
        info("calibration.arm", "%ld weak outcomes in plain runs; %s", g_plain_weak_total,
             g_plain_weak_total ? "this machine does reorder them, so clean x86 runs under FEX mean something"
                                : "none seen, so a clean x86 run is weaker evidence on this machine (try more --iters)");
    return ladder_done();
}
