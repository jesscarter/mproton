// threads: thread creation and join, static TLS (__declspec(thread) / thread_local, which the
// loader sets up from the image's TLS directory), dynamic TLS (TlsAlloc, including slots past
// the 64 in the TEB), fiber-local storage with its destructor callback, TLS callbacks, locks
// and condition variables under contention, and APCs (QueueUserAPC with alertable and
// non-alertable waits, FIFO order, waitable-timer completion routines).
#include "ladder.h"

// ---- static TLS --------------------------------------------------------------------------------
static thread_local int t_value = 7;                 // initialised: must read 7 in every new thread
static thread_local char t_block[256] = {1, 2, 3};   // bigger initialised block
static thread_local int t_zero;                      // .tbss: must read 0

// The callback marks the attaching thread (other threads Wine starts in the process see it too,
// so count per thread rather than globally).
static thread_local int t_cb_seen;
static void NTAPI tls_callback(void *, DWORD reason, void *) {
    if (reason == DLL_THREAD_ATTACH) t_cb_seen = 1;
}
// Register a TLS callback in the image's TLS directory (the CRT's .CRT$XL? array).
extern "C" __attribute__((section(".CRT$XLY"), used)) PIMAGE_TLS_CALLBACK ladder_tls_cb = tls_callback;

struct ThreadArgs { int index; DWORD tls_slot; DWORD extra_slot; DWORD fls_slot; int ok; int cb_seen; char why[96]; };

static volatile LONG g_fls_dtor;
static void WINAPI fls_dtor(void *p) { if (p == (void *)0xF15) InterlockedIncrement(&g_fls_dtor); }

static DWORD WINAPI tls_thread(void *p) {
    ThreadArgs *a = (ThreadArgs *)p;
    a->ok = 0;
    a->cb_seen = t_cb_seen;
    if (t_value != 7 || t_block[2] != 3 || t_block[200] != 0 || t_zero != 0) {
        snprintf(a->why, sizeof a->why, "initial static TLS %d %d %d %d", t_value, t_block[2], t_block[200], t_zero);
        return 1;
    }
    if (TlsGetValue(a->tls_slot) != nullptr || TlsGetValue(a->extra_slot) != nullptr) {
        snprintf(a->why, sizeof a->why, "dynamic TLS not empty in a new thread");
        return 1;
    }
    t_value = 1000 + a->index; t_zero = a->index; t_block[200] = (char)a->index;
    TlsSetValue(a->tls_slot, (void *)(uintptr_t)(2000 + a->index));
    TlsSetValue(a->extra_slot, (void *)(uintptr_t)(3000 + a->index));
    FlsSetValue(a->fls_slot, (void *)0xF15);
    for (int i = 0; i < 200; i++) {
        SwitchToThread();
        if (t_value != 1000 + a->index || t_zero != a->index || t_block[200] != (char)a->index ||
            TlsGetValue(a->tls_slot) != (void *)(uintptr_t)(2000 + a->index) ||
            TlsGetValue(a->extra_slot) != (void *)(uintptr_t)(3000 + a->index)) {
            snprintf(a->why, sizeof a->why, "value changed under us at iteration %d", i);
            return 1;
        }
    }
    a->ok = 1;
    return 0;
}

static void tls_checks() {
    if (!want("tls")) return;
    enum { N = 32 };
    DWORD slot = TlsAlloc();
    // Use up the 64 TEB slots so the next one lives in TlsExpansionSlots.
    DWORD filler[80]; int nfill = 0;
    DWORD extra = TLS_OUT_OF_INDEXES;
    while (nfill < 80) {
        DWORD s = TlsAlloc();
        if (s == TLS_OUT_OF_INDEXES) break;
        if (s >= 64) { extra = s; break; }
        filler[nfill++] = s;
    }
    DWORD fls = FlsAlloc(fls_dtor);
    ThreadArgs args[N];
    HANDLE h[N];
    for (int i = 0; i < N; i++) {
        args[i] = {i, slot, extra, fls, 0, 0, ""};
        h[i] = CreateThread(NULL, 0, tls_thread, &args[i], 0, NULL);
    }
    DWORD w = WaitForMultipleObjects(N, h, TRUE, 30000);
    int good = 0, cb = 0; const char *why = "";
    for (int i = 0; i < N; i++) {
        cb += args[i].cb_seen;
        if (args[i].ok) good++; else if (!*why) why = args[i].why;
        CloseHandle(h[i]);
    }
    check(w < WAIT_OBJECT_0 + N && good == N && t_value == 7, "tls.static_and_dynamic",
          "%d/%d threads kept their own static and TlsAlloc values (expansion slot %lu)%s%s", good, N, extra, *why ? ": " : "", why);
    check(extra != TLS_OUT_OF_INDEXES, "tls.expansion_slots", "TlsAlloc beyond 64 slots gave index %lu", extra);
    check(g_fls_dtor == N, "tls.fls_callback", "FLS destructor ran %ld times for %d exited threads", g_fls_dtor, N);
    check(cb == N, "tls.image_callback", "TLS directory callback ran DLL_THREAD_ATTACH in %d of %d new threads", cb, N);
    for (int i = 0; i < nfill; i++) TlsFree(filler[i]);
    if (extra != TLS_OUT_OF_INDEXES) TlsFree(extra);
    TlsFree(slot);
    FlsFree(fls);
}

// ---- locks --------------------------------------------------------------------------------------
static CRITICAL_SECTION g_cs;
static SRWLOCK g_srw = SRWLOCK_INIT;
static CONDITION_VARIABLE g_cv = CONDITION_VARIABLE_INIT;
static volatile long long g_counter_cs, g_counter_srw;
static volatile LONG g_counter_atomic;
static LONG g_queue[1024]; static int g_qhead, g_qtail; static int g_produced_done;
enum { LOCK_ITERS = 100000 };

static DWORD WINAPI lock_thread(void *) {
    for (int i = 0; i < LOCK_ITERS; i++) {
        EnterCriticalSection(&g_cs); g_counter_cs = g_counter_cs + 1; LeaveCriticalSection(&g_cs);
        AcquireSRWLockExclusive(&g_srw); g_counter_srw = g_counter_srw + 1; ReleaseSRWLockExclusive(&g_srw);
        InterlockedIncrement(&g_counter_atomic);
    }
    return 0;
}

static DWORD WINAPI producer(void *) {
    for (LONG i = 1; i <= 20000; i++) {
        AcquireSRWLockExclusive(&g_srw);
        while (g_qtail - g_qhead == 1024) SleepConditionVariableSRW(&g_cv, &g_srw, INFINITE, 0);
        g_queue[g_qtail++ % 1024] = i;
        ReleaseSRWLockExclusive(&g_srw);
        WakeAllConditionVariable(&g_cv);
    }
    AcquireSRWLockExclusive(&g_srw); g_produced_done = 1; ReleaseSRWLockExclusive(&g_srw);
    WakeAllConditionVariable(&g_cv);
    return 0;
}

static void lock_checks() {
    if (want("locks.counters")) {
        enum { N = 8 };
        InitializeCriticalSection(&g_cs);
        HANDLE h[N];
        for (int i = 0; i < N; i++) h[i] = CreateThread(NULL, 0, lock_thread, NULL, 0, NULL);
        WaitForMultipleObjects(N, h, TRUE, 60000);
        for (int i = 0; i < N; i++) CloseHandle(h[i]);
        long long want_n = (long long)N * LOCK_ITERS;
        check(g_counter_cs == want_n && g_counter_srw == want_n && g_counter_atomic == want_n, "locks.counters",
              "critical section %lld, SRW %lld, Interlocked %ld (want %lld each)", g_counter_cs, g_counter_srw,
              g_counter_atomic, want_n);
    }
    if (want("locks.condvar")) {
        HANDLE p = CreateThread(NULL, 0, producer, NULL, 0, NULL);
        long long sum = 0; LONG expect = 1; bool in_order = true;
        for (;;) {
            AcquireSRWLockExclusive(&g_srw);
            while (g_qhead == g_qtail && !g_produced_done) SleepConditionVariableSRW(&g_cv, &g_srw, INFINITE, 0);
            if (g_qhead == g_qtail && g_produced_done) { ReleaseSRWLockExclusive(&g_srw); break; }
            LONG v = g_queue[g_qhead++ % 1024];
            ReleaseSRWLockExclusive(&g_srw);
            WakeAllConditionVariable(&g_cv);
            if (v != expect++) in_order = false;
            sum += v;
        }
        WaitForSingleObject(p, 30000); CloseHandle(p);
        check(in_order && sum == 20000LL * 20001 / 2, "locks.condvar", "consumed sum %lld (want %lld), in order %d", sum,
              20000LL * 20001 / 2, in_order);
    }
}

// ---- APCs -----------------------------------------------------------------------------------------
static volatile LONG g_apc_count;
static DWORD g_apc_thread;
static char g_apc_order[32]; static volatile LONG g_apc_pos;
static void CALLBACK apc_fn(ULONG_PTR p) {
    g_apc_thread = GetCurrentThreadId();
    LONG i = InterlockedIncrement(&g_apc_pos) - 1;
    if (i < 31) g_apc_order[i] = (char)p;
    InterlockedIncrement(&g_apc_count);
}

// Modes 0 and 1: the target blocks in an alertable wait and one APC must wake it, run on it,
// and make the wait return WAIT_IO_COMPLETION. Mode 2: the target blocks in a NON-alertable
// wait while five APCs are queued (none may run), then enters an alertable wait, which must run
// all five in FIFO order before returning. Every step is ordered by events, not by timing, so
// the check holds on a loaded machine.
struct ApcTarget { HANDLE ready, go; DWORD result; DWORD tid; int mode; };
static DWORD WINAPI apc_target(void *p) {
    ApcTarget *t = (ApcTarget *)p;
    t->tid = GetCurrentThreadId();
    SetEvent(t->ready);
    if (t->mode == 0) t->result = SleepEx(10000, TRUE);
    else if (t->mode == 1) t->result = WaitForSingleObjectEx(t->go, 10000, TRUE);   // nobody sets go
    else {
        WaitForSingleObject(t->go, 10000);   // non-alertable
        t->result = SleepEx(10000, TRUE);
    }
    return 0;
}

static void apc_one(int mode, const char *name) {
    g_apc_count = 0; g_apc_pos = 0; memset(g_apc_order, 0, sizeof g_apc_order); g_apc_thread = 0;
    ApcTarget t = {CreateEventA(NULL, FALSE, FALSE, NULL), CreateEventA(NULL, TRUE, FALSE, NULL), 0, 0, mode};
    HANDLE h = CreateThread(NULL, 0, apc_target, &t, 0, NULL);
    WaitForSingleObject(t.ready, 10000);
    Sleep(50);   // let it block (if it isn't blocked yet the APC is simply pending when it gets there)
    LONG during = 0;
    const char *want_order = mode == 2 ? "abcde" : "a";
    if (mode == 2) {
        for (char c = 'a'; c <= 'e'; c++) QueueUserAPC(apc_fn, h, (ULONG_PTR)c);
        Sleep(100);
        during = g_apc_count;   // the target is still in its non-alertable wait
        SetEvent(t.go);
    } else {
        QueueUserAPC(apc_fn, h, 'a');
    }
    DWORD w = WaitForSingleObject(h, 15000);
    LONG want_n = (LONG)strlen(want_order);
    bool ok = w == WAIT_OBJECT_0 && t.result == WAIT_IO_COMPLETION && g_apc_count == want_n && g_apc_thread == t.tid &&
              !strcmp(g_apc_order, want_order) && during == 0;
    check(ok, name, "wait returned %lu (want WAIT_IO_COMPLETION=%lu), %ld APCs ran on %s thread in order '%s' (want '%s')%s", t.result,
          (DWORD)WAIT_IO_COMPLETION, g_apc_count, g_apc_thread == t.tid ? "the target" : "the WRONG", g_apc_order, want_order,
          mode == 2 ? (during == 0 ? ", none during the non-alertable wait" : ", some ran during a NON-alertable wait") : "");
    CloseHandle(h); CloseHandle(t.ready); CloseHandle(t.go);
}

static volatile LONG g_timer_apc;
static void CALLBACK timer_apc(void *arg, DWORD, DWORD) { if (arg == (void *)0x77) InterlockedIncrement(&g_timer_apc); }

static void apc_checks() {
    if (want("apc.sleepex")) apc_one(0, "apc.sleepex");
    if (want("apc.wait_ex")) apc_one(1, "apc.wait_ex");
    if (want("apc.nonalertable")) apc_one(2, "apc.nonalertable");
    if (want("apc.self")) {
        g_apc_count = 0; g_apc_pos = 0; memset(g_apc_order, 0, sizeof g_apc_order);
        QueueUserAPC(apc_fn, GetCurrentThread(), 'x');
        LONG before = g_apc_count;
        DWORD r = SleepEx(0, TRUE);
        check(before == 0 && r == WAIT_IO_COMPLETION && g_apc_count == 1, "apc.self",
              "ran before the alertable wait=%ld, SleepEx(0, TRUE)=%lu, ran=%ld", before, r, g_apc_count);
    }
    if (want("apc.timer")) {
        HANDLE tm = CreateWaitableTimerA(NULL, FALSE, NULL);
        LARGE_INTEGER due; due.QuadPart = -20 * 10000LL;   // 20 ms
        g_timer_apc = 0;
        SetWaitableTimer(tm, &due, 10, timer_apc, (void *)0x77, FALSE);   // then every 10 ms
        double t0 = now_ms();
        while (g_timer_apc < 3 && now_ms() - t0 < 5000) SleepEx(1000, TRUE);
        CancelWaitableTimer(tm); CloseHandle(tm);
        SleepEx(0, TRUE);
        check(g_timer_apc >= 3, "apc.timer", "periodic waitable-timer completion routine ran %ld times in %.0f ms", g_timer_apc,
              now_ms() - t0);
    }
}

// ---- many threads ----------------------------------------------------------------------------------
static DWORD WINAPI short_thread(void *p) { return (DWORD)(uintptr_t)p * 3; }
static void spawn_checks() {
    if (!want("spawn")) return;
    int bad = 0; double t0 = now_ms();
    for (int round = 0; round < 10; round++) {
        HANDLE h[50];
        for (int i = 0; i < 50; i++) h[i] = CreateThread(NULL, 64 * 1024, short_thread, (void *)(uintptr_t)(round * 50 + i), 0, NULL);
        WaitForMultipleObjects(50, h, TRUE, 30000);
        for (int i = 0; i < 50; i++) {
            DWORD code = 0;
            GetExitCodeThread(h[i], &code);
            if (code != (DWORD)(round * 50 + i) * 3) bad++;
            CloseHandle(h[i]);
        }
    }
    check(bad == 0, "spawn", "500 short threads, %d wrong exit codes, %.0f ms", bad, now_ms() - t0);
}

int main(int argc, char **argv) {
    ladder_begin("threads", argc, argv);
    tls_checks();
    lock_checks();
    apc_checks();
    spawn_checks();
    return ladder_done();
}
