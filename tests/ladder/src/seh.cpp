// seh: structured exception handling. Hardware faults (access violation on read, write and
// execute, integer divide by zero, breakpoint), the fault details the dispatcher hands over
// (code, address, read/write flag, context), nested handlers and their call order,
// RaiseException through ten frames with __finally blocks between, continuable exceptions,
// vectored handlers that fix the fault and continue, C++ throw/catch, and stack overflow.
//
// Under FEX every one of these crosses the emulator: the fault happens in translated x86 code
// and the context the handler sees has to be rebuilt in x86 terms.
#include "ladder.h"
#include <exception>
#include <string>

// ---- fault details captured by a filter -------------------------------------------------------
struct Seen {
    DWORD code = 0;
    DWORD nparams = 0;
    ULONG_PTR params[EXCEPTION_MAXIMUM_PARAMETERS] = {};
    void *address = nullptr;   // ExceptionAddress
    ULONG_PTR ctx_pc = 0, ctx_sp = 0;
    int calls = 0;
};

static ULONG_PTR context_pc(const CONTEXT *c) {
#if defined(__i386__)
    return c->Eip;
#elif defined(__x86_64__)   // also arm64ec: its CONTEXT is the x64 one
    return c->Rip;
#else
    return c->Pc;
#endif
}
static ULONG_PTR context_sp(const CONTEXT *c) {
#if defined(__i386__)
    return c->Esp;
#elif defined(__x86_64__)
    return c->Rsp;
#else
    return c->Sp;
#endif
}

static LONG record_filter(EXCEPTION_POINTERS *ep, void *fctx) {
    Seen *s = (Seen *)fctx;
    s->calls++;
    s->code = ep->ExceptionRecord->ExceptionCode;
    s->nparams = ep->ExceptionRecord->NumberParameters;
    for (DWORD i = 0; i < s->nparams && i < EXCEPTION_MAXIMUM_PARAMETERS; i++) s->params[i] = ep->ExceptionRecord->ExceptionInformation[i];
    s->address = ep->ExceptionRecord->ExceptionAddress;
    s->ctx_pc = context_pc(ep->ContextRecord);
    s->ctx_sp = context_sp(ep->ContextRecord);
    return EXCEPTION_EXECUTE_HANDLER;
}

static bool sp_on_stack(ULONG_PTR sp) {
    NT_TIB *tib = (NT_TIB *)NtCurrentTeb();
    return sp >= (ULONG_PTR)tib->StackLimit && sp < (ULONG_PTR)tib->StackBase;
}

// ---- access violations -------------------------------------------------------------------------
static unsigned char *g_noaccess;   // one committed PAGE_NOACCESS page

static void body_read(void *p) { read_byte((volatile unsigned char *)p); }
static void body_write(void *p) { write_byte((volatile unsigned char *)p, 0x5a); }
static void body_exec(void *p) { ((void (*)(void))p)(); }

static void av_checks() {
    g_noaccess = (unsigned char *)VirtualAlloc(NULL, 0x10000, MEM_COMMIT | MEM_RESERVE, PAGE_NOACCESS);
    if (!g_noaccess) { fail("av", "VirtualAlloc failed: %lu", GetLastError()); return; }

    struct { const char *name; seh_body_fn body; ULONG_PTR rw; size_t off; } cases[] = {
        {"av_read", body_read, 0, 0x123},
        {"av_write", body_write, 1, 0x456},
        {"av_null_read", body_read, 0, (size_t)-1},
        {"av_exec_noaccess", body_exec, 8, 0x200},
    };
    for (auto &c : cases) {
        if (!want(c.name)) continue;
        Seen s;
        unsigned char *addr = c.off == (size_t)-1 ? nullptr : g_noaccess + c.off;
        DWORD code = 0;
        int caught = seh_try(c.body, addr, record_filter, &s, &code);
        if (!caught) { fail(c.name, "no exception"); continue; }
        bool ok = s.code == EXCEPTION_ACCESS_VIOLATION && code == s.code && s.nparams == 2 &&
                  s.params[1] == (ULONG_PTR)addr && s.params[0] == c.rw;
        // For the execute case the faulting instruction is the target itself.
        bool pc_ok = s.ctx_pc == (ULONG_PTR)s.address && (c.rw != 8 || s.address == addr);
        check(ok && pc_ok && sp_on_stack(s.ctx_sp), c.name,
              "code=%08lx info=[%lu, %p] want [%lu, %p], ExceptionAddress=%p context pc=%p sp=%p%s",
              s.code, (unsigned long)s.params[0], (void *)s.params[1], (unsigned long)c.rw, addr, s.address,
              (void *)s.ctx_pc, (void *)s.ctx_sp, sp_on_stack(s.ctx_sp) ? "" : " (sp not on this thread's stack)");
    }
}

// ---- divide by zero, breakpoint -----------------------------------------------------------------
static volatile int g_zero = 0;
static void body_div0(void *) { volatile int r = divide(7, g_zero); (void)r; }
static void body_break(void *) { DebugBreak(); }

static void trap_checks() {
    if (want("div0")) {
        Seen s; DWORD code = 0;
        int caught = seh_try(body_div0, nullptr, record_filter, &s, &code);
        check(caught && code == EXCEPTION_INT_DIVIDE_BY_ZERO && s.ctx_pc == (ULONG_PTR)s.address, "div0",
              "caught=%d code=%08lx (want %08lx) address=%p pc=%p", caught, code, (DWORD)EXCEPTION_INT_DIVIDE_BY_ZERO,
              s.address, (void *)s.ctx_pc);
    }
    if (want("breakpoint")) {
        Seen s; DWORD code = 0;
        int caught = seh_try(body_break, nullptr, record_filter, &s, &code);
        check(caught && code == EXCEPTION_BREAKPOINT, "breakpoint", "caught=%d code=%08lx", caught, code);
    }
}

// ---- nested handlers ------------------------------------------------------------------------------
static std::string g_log;
static LONG inner_search(EXCEPTION_POINTERS *, void *) { g_log += "inner-filter,"; return EXCEPTION_CONTINUE_SEARCH; }
static LONG outer_take(EXCEPTION_POINTERS *ep, void *) {
    g_log += "outer-filter,";
    return ep->ExceptionRecord->ExceptionCode == 0xE0000001 ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH;
}
static void fin_log(void *, int abnormal) { g_log += abnormal ? "finally-abnormal," : "finally-normal,"; }
static void raise1(void *) { RaiseException(0xE0000001, 0, 0, nullptr); }
static void inner_with_finally(void *) { seh_try_finally(raise1, nullptr, fin_log, nullptr); }
static void nested_inner(void *) { seh_try(inner_with_finally, nullptr, inner_search, nullptr, nullptr); g_log += "WRONG-inner-returned,"; }

static void inner_catches(void *) {
    DWORD code = 0;
    int c = seh_try(raise1, nullptr, nullptr, nullptr, &code);
    g_log += c && code == 0xE0000001 ? "inner-caught," : "inner-missed,";
    // The chain must be intact after the inner catch: raise again for the outer handler.
    RaiseException(0xE0000001, 0, 0, nullptr);
}

static void nested_checks() {
    if (want("nested_search")) {
        g_log.clear();
        DWORD code = 0;
        int caught = seh_try(nested_inner, nullptr, outer_take, nullptr, &code);
        if (caught) g_log += "outer-handler";
        const char *want_log = "inner-filter,outer-filter,finally-abnormal,outer-handler";
        check(g_log == want_log, "nested_search", "order %s (want %s)", g_log.c_str(), want_log);
    }
    if (want("nested_rethrow")) {
        g_log.clear();
        int caught = seh_try(inner_catches, nullptr, outer_take, nullptr, nullptr);
        if (caught) g_log += "outer-handler";
        const char *want_log = "inner-caught,outer-filter,outer-handler";
        check(g_log == want_log, "nested_rethrow", "order %s (want %s)", g_log.c_str(), want_log);
    }
    if (want("finally_normal")) {
        g_log.clear();
        seh_try_finally([](void *) { g_log += "body,"; }, nullptr, fin_log, nullptr);
        check(g_log == "body,finally-normal,", "finally_normal", "order %s", g_log.c_str());
    }
}

// ---- RaiseException across frames ----------------------------------------------------------------
struct Depth { int depth; std::string *order; };
static const ULONG_PTR k_params[3] = {0x1234, (ULONG_PTR)-1, 0xC0FFEE};

__attribute__((noinline)) static void recurse(void *p);
static void fin_depth(void *p, int abnormal) {
    Depth *d = (Depth *)p;
    *d->order += std::to_string(d->depth) + (abnormal ? "a," : "n,");
}
__attribute__((noinline)) static void recurse(void *p) {
    Depth *d = (Depth *)p;
    volatile char pad[128];
    pad[d->depth % 64] = (char)d->depth;   // touch a depth-dependent slot
    Depth next = {d->depth + 1, d->order};
    if (d->depth == 10) RaiseException(0xE0C0FFEE, 0, 3, k_params);
    else if (d->depth == 3 || d->depth == 5 || d->depth == 7) seh_try_finally(recurse, &next, fin_depth, &next);
    else recurse(&next);
    pad[1] = pad[0];   // keep the frame alive across the call
}

static void raise_checks() {
    if (want("raise_frames")) {
        std::string order;
        Depth d = {0, &order};
        Seen s; DWORD code = 0;
        int caught = seh_try(recurse, &d, record_filter, &s, &code);
        bool params_ok = s.nparams == 3 && s.params[0] == k_params[0] && s.params[1] == k_params[1] && s.params[2] == k_params[2];
        check(caught && code == 0xE0C0FFEE && params_ok && order == "8a,6a,4a,", "raise_frames",
              "caught=%d code=%08lx params=%lu [%lx %lx %lx] finally order %s (want 8a,6a,4a,)", caught, code,
              s.nparams, (unsigned long)s.params[0], (unsigned long)s.params[1], (unsigned long)s.params[2], order.c_str());
    }
    if (want("raise_continue")) {
        static int after;
        after = 0;
        auto body = [](void *) { RaiseException(0x60000001, 0, 0, nullptr); after = 1; };
        int caught = seh_try(body, nullptr, [](EXCEPTION_POINTERS *, void *) -> LONG { return EXCEPTION_CONTINUE_EXECUTION; },
                             nullptr, nullptr);
        check(!caught && after == 1, "raise_continue", "caught=%d resumed=%d", caught, after);
    }
}

// ---- vectored handlers -----------------------------------------------------------------------------
static unsigned char *g_ro;
static volatile LONG g_veh_calls, g_veh_order;
static LONG CALLBACK veh_fix(EXCEPTION_POINTERS *ep) {
    EXCEPTION_RECORD *r = ep->ExceptionRecord;
    if (r->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || r->NumberParameters < 2) return EXCEPTION_CONTINUE_SEARCH;
    unsigned char *a = (unsigned char *)r->ExceptionInformation[1];
    if (a < g_ro || a >= g_ro + 4096) return EXCEPTION_CONTINUE_SEARCH;
    InterlockedIncrement(&g_veh_calls);
    DWORD old;
    VirtualProtect(g_ro, 4096, PAGE_READWRITE, &old);
    return EXCEPTION_CONTINUE_EXECUTION;   // retry the faulting store
}
static LONG CALLBACK veh_first(EXCEPTION_POINTERS *ep) {
    if (ep->ExceptionRecord->ExceptionCode == 0xE0000777) InterlockedCompareExchange(&g_veh_order, 1, 0);
    return EXCEPTION_CONTINUE_SEARCH;
}
static LONG CALLBACK veh_last(EXCEPTION_POINTERS *ep) {
    if (ep->ExceptionRecord->ExceptionCode == 0xE0000777) InterlockedCompareExchange(&g_veh_order, 2, 0);
    return EXCEPTION_CONTINUE_SEARCH;
}

static void veh_checks() {
    if (want("veh_fixup")) {
        g_ro = (unsigned char *)VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READONLY);
        void *h = AddVectoredExceptionHandler(1, veh_fix);
        int caught = seh_try([](void *) { for (int i = 0; i < 4096; i += 512) write_byte(g_ro + i, (unsigned char)(i / 512 + 1)); },
                             nullptr, nullptr, nullptr, nullptr);
        RemoveVectoredExceptionHandler(h);
        bool data_ok = true;
        for (int i = 0; i < 4096; i += 512) data_ok &= g_ro[i] == (unsigned char)(i / 512 + 1);
        check(!caught && g_veh_calls == 1 && data_ok, "veh_fixup",
              "frame handler reached=%d, handler calls=%ld (want 1), stores landed=%d", caught, g_veh_calls, data_ok);
        VirtualFree(g_ro, 0, MEM_RELEASE);
    }
    if (want("veh_order")) {
        void *h2 = AddVectoredExceptionHandler(0, veh_last);
        void *h1 = AddVectoredExceptionHandler(1, veh_first);
        DWORD code = 0;
        int caught = seh_try([](void *) { RaiseException(0xE0000777, 0, 0, nullptr); }, nullptr, nullptr, nullptr, &code);
        RemoveVectoredExceptionHandler(h1);
        RemoveVectoredExceptionHandler(h2);
        check(caught && code == 0xE0000777 && g_veh_order == 1, "veh_order",
              "first-registered handler ran first=%d, then frame handler caught=%d", g_veh_order == 1, caught);
    }
}

// ---- C++ exceptions ---------------------------------------------------------------------------------
struct Tracker { int *n; ~Tracker() { (*n)++; } };
struct LadderError : std::exception { int v; explicit LadderError(int x) : v(x) {} };
__attribute__((noinline)) static void cxx_deep(int d, int *dtors) {
    Tracker t{dtors};
    if (d == 0) throw LadderError(42);
    cxx_deep(d - 1, dtors);
}

static void cxx_checks() {
    if (!want("cxx_throw")) return;
    int dtors = 0, v = 0;
    try { cxx_deep(8, &dtors); } catch (const LadderError &e) { v = e.v; } catch (...) { v = -1; }
    check(v == 42 && dtors == 9, "cxx_throw", "caught value %d (want 42), destructors run %d (want 9)", v, dtors);
}

// ---- stack overflow --------------------------------------------------------------------------------
static volatile int g_sink;
__attribute__((noinline)) static void eat_stack(void *p) {
    volatile char buf[1024];
    buf[0] = (char)(uintptr_t)p;
    if (g_sink == 12345) return;   // never true; keeps the recursion from being proven infinite
    eat_stack((void *)buf);
    g_sink = buf[1];
}
static DWORD WINAPI overflow_thread(void *out) {
    ULONG guarantee = 64 * 1024;   // room for the dispatcher once the guard page is gone, as the CLR asks for
    SetThreadStackGuarantee(&guarantee);
    DWORD code = 0;
    Seen s; int caught = seh_try(eat_stack, nullptr, record_filter, &s, &code);
    if (caught && !code) code = 0xBAD00000 | (s.code & 0xffff);
    *(DWORD *)out = caught ? code : 1;
    return 0;
}
static void stack_checks() {
    if (!want("stack_overflow")) return;
    DWORD code = 0;
    HANDLE t = CreateThread(NULL, 256 * 1024, overflow_thread, &code, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
    DWORD w = WaitForSingleObject(t, 30000);
    DWORD exit_code = 0;
    GetExitCodeThread(t, &exit_code);
    CloseHandle(t);
    check(w == WAIT_OBJECT_0 && code == EXCEPTION_STACK_OVERFLOW, "stack_overflow",
          "caught code=%08lx (want %08lx), thread wait=%lu exit=%08lx", code, (DWORD)EXCEPTION_STACK_OVERFLOW, w, exit_code);
}

int main(int argc, char **argv) {
    ladder_begin("seh", argc, argv);
    av_checks();
    trap_checks();
    nested_checks();
    raise_checks();
    veh_checks();
    cxx_checks();
    stack_checks();
    return ladder_done();
}
