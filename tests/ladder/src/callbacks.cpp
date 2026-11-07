// callbacks: Wine calling back into the program's own code. Window procedures (direct,
// cross-thread SendMessage, subclassing), EnumWindows / EnumChildWindows, SetTimer with a
// TIMERPROC, window hooks, thread-pool callbacks (timer queue, work items, registered waits),
// the CRT's qsort comparator and GDI's font enumeration.
//
// Under ARM64EC Wine every one of these is a native ARM64 caller invoking x86_64 code through
// FEX (or i386 code through WoW64), so argument passing, return values and the stack must
// survive the transition. On a native aarch64 build it checks the same paths without FEX.
#include "ladder.h"

static const char *k_class = "LadderCallbacks";
static volatile LONG g_create_seen, g_custom_seen;
static DWORD g_wndproc_tid;

static LRESULT CALLBACK wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: InterlockedIncrement(&g_create_seen); return 0;
    case WM_APP + 1:
        InterlockedIncrement(&g_custom_seen);
        g_wndproc_tid = GetCurrentThreadId();
        return (LRESULT)(wp * 3 + lp);   // a value the caller can check
    }
    return DefWindowProcA(h, msg, wp, lp);
}

static WNDPROC g_orig_proc;
static LRESULT CALLBACK subclass_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_APP + 1) return CallWindowProcA(g_orig_proc, h, msg, wp + 1, lp) + 1000;
    return CallWindowProcA(g_orig_proc, h, msg, wp, lp);
}

static void pump(DWORD ms) {
    double end = now_ms() + ms;
    MSG m;
    while (now_ms() < end) {
        MsgWaitForMultipleObjects(0, NULL, FALSE, 5, QS_ALLINPUT);
        while (PeekMessageA(&m, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageA(&m); }
    }
}

// ---- window procedures -------------------------------------------------------------------------
static HWND g_main;
static volatile LRESULT g_xthread_result = -1;
static DWORD WINAPI sender_thread(void *) {
    DWORD_PTR r = 0;
    if (SendMessageTimeoutA(g_main, WM_APP + 1, 5, 2, SMTO_NORMAL, 10000, &r)) g_xthread_result = (LRESULT)r;
    return 0;
}

static void wndproc_checks() {
    WNDCLASSA wc = {};
    wc.lpfnWndProc = wndproc; wc.hInstance = GetModuleHandleA(NULL); wc.lpszClassName = k_class;
    if (!RegisterClassA(&wc)) { fail("wndproc.create", "RegisterClass failed: %lu", GetLastError()); return; }
    g_main = CreateWindowExA(0, k_class, "ladder main", WS_OVERLAPPEDWINDOW, 0, 0, 200, 100, NULL, NULL, wc.hInstance, NULL);
    if (!check(g_main && g_create_seen == 1, "wndproc.create", "CreateWindow %s, WM_CREATE seen %ld times",
               g_main ? "ok" : "failed", g_create_seen)) return;
    LRESULT r = SendMessageA(g_main, WM_APP + 1, 10, 7);
    check(r == 37 && g_wndproc_tid == GetCurrentThreadId(), "wndproc.send", "SendMessage returned %ld (want 37)", (long)r);

    // Another thread sends; the window's thread must run the procedure while it pumps.
    g_wndproc_tid = 0;
    HANDLE t = CreateThread(NULL, 0, sender_thread, NULL, 0, NULL);
    double t0 = now_ms();
    while (WaitForSingleObject(t, 0) == WAIT_TIMEOUT && now_ms() - t0 < 10000) pump(10);
    CloseHandle(t);
    check(g_xthread_result == 17 && g_wndproc_tid == GetCurrentThreadId(), "wndproc.cross_thread",
          "cross-thread SendMessage returned %ld (want 17), procedure ran on the %s thread", (long)g_xthread_result,
          g_wndproc_tid == GetCurrentThreadId() ? "window's" : "WRONG");

    g_orig_proc = (WNDPROC)SetWindowLongPtrA(g_main, GWLP_WNDPROC, (LONG_PTR)subclass_proc);
    r = SendMessageA(g_main, WM_APP + 1, 10, 7);
    SetWindowLongPtrA(g_main, GWLP_WNDPROC, (LONG_PTR)g_orig_proc);
    check(r == 1040, "wndproc.subclass", "subclassed SendMessage returned %ld (want 1040)", (long)r);
}

// ---- enumeration -------------------------------------------------------------------------------
struct EnumState { HWND want[4]; int found[4]; int calls; int stop_after; };
static BOOL CALLBACK enum_proc(HWND h, LPARAM lp) {
    EnumState *s = (EnumState *)lp;
    s->calls++;
    for (int i = 0; i < 4; i++) if (s->want[i] == h) s->found[i]++;
    return s->stop_after == 0 || s->calls < s->stop_after;
}

static void enum_checks() {
    if (!g_main) { skip("enum_windows", "no window"); return; }
    EnumState s = {};
    HINSTANCE inst = GetModuleHandleA(NULL);
    for (int i = 0; i < 3; i++) s.want[i] = CreateWindowExA(0, k_class, "ladder enum", WS_OVERLAPPED, 0, 0, 50, 50, NULL, NULL, inst, NULL);
    s.want[3] = g_main;
    BOOL ok = EnumWindows(enum_proc, (LPARAM)&s);
    bool all = ok;
    for (int i = 0; i < 4; i++) all = all && s.found[i] == 1;
    check(all, "enum_windows", "EnumWindows returned %d after %d callbacks; our 4 windows seen %d/%d/%d/%d times", ok, s.calls,
          s.found[0], s.found[1], s.found[2], s.found[3]);

    EnumState stop = {}; stop.stop_after = 2;
    ok = EnumWindows(enum_proc, (LPARAM)&stop);
    check(!ok && stop.calls == 2, "enum_windows_stop", "callback returning FALSE: EnumWindows returned %d after %d calls (want 0 after 2)",
          ok, stop.calls);

    EnumState kids = {};
    for (int i = 0; i < 4; i++) kids.want[i] = CreateWindowExA(0, "STATIC", "kid", WS_CHILD, i * 10, 0, 10, 10, g_main, NULL, inst, NULL);
    ok = EnumChildWindows(g_main, enum_proc, (LPARAM)&kids);
    all = kids.calls == 4;
    for (int i = 0; i < 4; i++) all = all && kids.found[i] == 1;
    check(all, "enum_child_windows", "EnumChildWindows saw %d children, ours %d/%d/%d/%d", kids.calls, kids.found[0],
          kids.found[1], kids.found[2], kids.found[3]);
    for (int i = 0; i < 3; i++) DestroyWindow(s.want[i]);
}

// ---- timers and hooks --------------------------------------------------------------------------
static volatile LONG g_timer_calls, g_timer_bad;
static void CALLBACK timer_proc(HWND h, UINT msg, UINT_PTR id, DWORD) {
    if (msg != WM_TIMER || id != 0x1234 || h != g_main) InterlockedIncrement(&g_timer_bad);
    InterlockedIncrement(&g_timer_calls);
}
static volatile LONG g_hook_calls;
static LRESULT CALLBACK hook_proc(int code, WPARAM wp, LPARAM lp) {
    CWPSTRUCT *c = (CWPSTRUCT *)lp;
    if (code == HC_ACTION && c->message == WM_APP + 2) InterlockedIncrement(&g_hook_calls);
    return CallNextHookEx(NULL, code, wp, lp);
}

static void timer_checks() {
    if (!g_main) { skip("set_timer", "no window"); return; }
    SetTimer(g_main, 0x1234, 10, timer_proc);
    double t0 = now_ms();
    while (g_timer_calls < 5 && now_ms() - t0 < 5000) pump(20);
    KillTimer(g_main, 0x1234);
    check(g_timer_calls >= 5 && g_timer_bad == 0, "set_timer", "TIMERPROC ran %ld times in %.0f ms, %ld with wrong arguments",
          g_timer_calls, now_ms() - t0, g_timer_bad);

    HHOOK hk = SetWindowsHookExA(WH_CALLWNDPROC, hook_proc, NULL, GetCurrentThreadId());
    for (int i = 0; i < 3; i++) SendMessageA(g_main, WM_APP + 2, 0, 0);
    if (hk) UnhookWindowsHookEx(hk);
    check(hk && g_hook_calls == 3, "hook", "WH_CALLWNDPROC hook saw %ld of 3 messages", g_hook_calls);
}

// ---- thread pool -------------------------------------------------------------------------------
static volatile LONG g_tq_calls, g_tq_bad, g_work_calls, g_wait_calls;
static void CALLBACK tq_proc(void *p, BOOLEAN fired) {
    if (p != (void *)0xABC || !fired) InterlockedIncrement(&g_tq_bad);
    InterlockedIncrement(&g_tq_calls);
}
static void CALLBACK work_proc(PTP_CALLBACK_INSTANCE, void *p) { InterlockedExchangeAdd(&g_work_calls, (LONG)(uintptr_t)p); }
static void CALLBACK wait_proc(void *p, BOOLEAN timed_out) { if (p == (void *)0xD0D && !timed_out) InterlockedIncrement(&g_wait_calls); }

static void pool_checks() {
    HANDLE q = CreateTimerQueue(), timer = NULL;
    CreateTimerQueueTimer(&timer, q, tq_proc, (void *)0xABC, 10, 10, WT_EXECUTEDEFAULT);
    double t0 = now_ms();
    while (g_tq_calls < 5 && now_ms() - t0 < 5000) Sleep(10);
    DeleteTimerQueueEx(q, INVALID_HANDLE_VALUE);
    check(g_tq_calls >= 5 && g_tq_bad == 0, "timer_queue", "timer-queue callback ran %ld times, %ld with wrong arguments", g_tq_calls, g_tq_bad);

    for (int i = 1; i <= 100; i++) TrySubmitThreadpoolCallback(work_proc, (void *)(uintptr_t)i, NULL);
    t0 = now_ms();
    while (g_work_calls != 5050 && now_ms() - t0 < 5000) Sleep(5);
    check(g_work_calls == 5050, "threadpool_work", "100 work items summed to %ld (want 5050)", g_work_calls);

    HANDLE ev = CreateEventA(NULL, FALSE, FALSE, NULL), wait = NULL;
    RegisterWaitForSingleObject(&wait, ev, wait_proc, (void *)0xD0D, 10000, WT_EXECUTEDEFAULT);
    for (int i = 0; i < 3; i++) {
        SetEvent(ev);
        t0 = now_ms();
        while (g_wait_calls <= i && now_ms() - t0 < 5000) Sleep(5);
    }
    UnregisterWaitEx(wait, INVALID_HANDLE_VALUE);
    CloseHandle(ev);
    check(g_wait_calls == 3, "registered_wait", "RegisterWaitForSingleObject callback ran %ld of 3 times", g_wait_calls);
}

// ---- CRT and GDI -------------------------------------------------------------------------------
static int cmp_int(const void *a, const void *b) { int x = *(const int *)a, y = *(const int *)b; return (x > y) - (x < y); }
struct FontState { int count; int bad; };
static int CALLBACK font_proc(const LOGFONTA *lf, const TEXTMETRICA *tm, DWORD type, LPARAM lp) {
    FontState *s = (FontState *)lp;
    if (!lf || !tm || !lf->lfFaceName[0]) s->bad++;
    s->count++;
    return 1;
}

static void misc_checks() {
    int v[1000];
    for (int i = 0; i < 1000; i++) v[i] = (i * 7919) % 1000;
    qsort(v, 1000, sizeof v[0], cmp_int);
    bool sorted = true;
    for (int i = 0; i < 1000; i++) sorted = sorted && v[i] == i;
    check(sorted, "qsort", "CRT qsort with a program comparator %s", sorted ? "sorted correctly" : "gave the wrong order");

    HDC dc = GetDC(NULL);
    LOGFONTA lf = {}; lf.lfCharSet = DEFAULT_CHARSET;
    FontState fs = {};
    EnumFontFamiliesExA(dc, &lf, font_proc, (LPARAM)&fs, 0);
    ReleaseDC(NULL, dc);
    check(fs.count > 0 && fs.bad == 0, "enum_fonts", "EnumFontFamiliesEx callback ran %d times, %d with bad arguments", fs.count, fs.bad);
}

int main(int argc, char **argv) {
    ladder_begin("callbacks", argc, argv);
    wndproc_checks();
    enum_checks();
    timer_checks();
    pool_checks();
    misc_checks();
    if (g_main) DestroyWindow(g_main);
    return ladder_done();
}
