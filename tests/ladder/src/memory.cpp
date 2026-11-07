// memory: the Windows memory model on top of a host with 16K pages, and code that changes
// under a JIT.
//
// Apple Silicon has 16K pages; Windows programs assume 4K pages and 64K allocation granularity.
// Wine emulates 4K protection inside each 16K host page, which is where the bugs live: a
// VirtualProtect on one 4K page must not change its three neighbours, a guard page must fire
// once and only for its own 4K, commit/decommit and write-watch must work per 4K page.
// FEX caches translated x86 code, so code that is rewritten (with or without
// FlushInstructionCache), freed and reallocated, or rewritten on another thread must never run
// stale.
#include "ladder.h"

static const DWORD K4 = 4096;

// ---- fault probes --------------------------------------------------------------------------------
struct Probe { DWORD code; ULONG_PTR rw, addr; };
static LONG probe_filter(EXCEPTION_POINTERS *ep, void *p) {
    Probe *pr = (Probe *)p;
    pr->code = ep->ExceptionRecord->ExceptionCode;
    pr->rw = ep->ExceptionRecord->NumberParameters > 0 ? ep->ExceptionRecord->ExceptionInformation[0] : 99;
    pr->addr = ep->ExceptionRecord->NumberParameters > 1 ? ep->ExceptionRecord->ExceptionInformation[1] : 0;
    return EXCEPTION_EXECUTE_HANDLER;
}
static void do_read(void *p) { read_byte((volatile unsigned char *)p); }
static void do_write(void *p) { volatile unsigned char *b = (volatile unsigned char *)p; write_byte(b, read_byte(b)); }

// 0 = access worked, else the exception code (with *pr filled in).
static DWORD try_read(void *p, Probe *pr = nullptr) {
    Probe tmp = {}; Probe *x = pr ? pr : &tmp;
    *x = {};
    return seh_try(do_read, p, probe_filter, x, nullptr) ? x->code : 0;
}
static DWORD try_write(void *p, Probe *pr = nullptr) {
    Probe tmp = {}; Probe *x = pr ? pr : &tmp;
    *x = {};
    return seh_try(do_write, p, probe_filter, x, nullptr) ? x->code : 0;
}

static const char *prot_name(DWORD p) {
    switch (p & 0xff) {
    case PAGE_NOACCESS: return "NOACCESS"; case PAGE_READONLY: return "READONLY"; case PAGE_READWRITE: return "READWRITE";
    case PAGE_EXECUTE: return "EXECUTE"; case PAGE_EXECUTE_READ: return "EXECUTE_READ"; case PAGE_EXECUTE_READWRITE: return "EXECUTE_READWRITE";
    case 0: return "0";
    default: return "other";
    }
}

// ---- basics --------------------------------------------------------------------------------------
static void sysinfo_checks() {
    if (!want("sysinfo")) return;
    SYSTEM_INFO si; GetSystemInfo(&si);
    check(si.dwPageSize == K4 && si.dwAllocationGranularity == 65536, "sysinfo", "page size %lu, allocation granularity %lu",
          si.dwPageSize, si.dwAllocationGranularity);
    int misaligned = 0;
    void *a[20];
    for (int i = 0; i < 20; i++) {
        a[i] = VirtualAlloc(NULL, K4 * (1 + i), MEM_RESERVE, PAGE_NOACCESS);
        if (!a[i] || ((uintptr_t)a[i] & 0xffff)) misaligned++;
    }
    for (int i = 0; i < 20; i++) if (a[i]) VirtualFree(a[i], 0, MEM_RELEASE);
    check(misaligned == 0, "alloc_granularity", "%d of 20 reservations not 64K-aligned (or failed)", misaligned);
}

// One 64K block, four 4K pages with different protections inside the first 16K host page.
static void protect_checks() {
    if (!want("protect_4k")) return;
    unsigned char *b = (unsigned char *)VirtualAlloc(NULL, 65536, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!b) { fail("protect_4k", "VirtualAlloc failed: %lu", GetLastError()); return; }
    bool zero = true;
    for (DWORD i = 0; i < 65536; i += 61) zero = zero && b[i] == 0;
    check(zero, "commit_zeroed", "fresh commit reads %s", zero ? "zero" : "NON-ZERO");
    for (int pg = 0; pg < 16; pg++) memset(b + pg * K4, 0x40 + pg, K4);

    DWORD old;
    bool set = VirtualProtect(b + 1 * K4, K4, PAGE_NOACCESS, &old) && old == PAGE_READWRITE &&
               VirtualProtect(b + 2 * K4 + 100, 1, PAGE_READONLY, &old) &&   // unaligned: rounds to its 4K page
               VirtualProtect(b + 5 * K4, K4, PAGE_NOACCESS, &old);
    if (!check(set, "protect_4k.set", "VirtualProtect on single 4K pages %s (%lu)", set ? "ok" : "failed", GetLastError())) return;

    // Expected per page: 0 RW, 1 none, 2 RO, 3 RW, 4 RW, 5 none, 6.. RW
    struct { int pg; bool r, w; } want_[] = {{0, 1, 1}, {1, 0, 0}, {2, 1, 0}, {3, 1, 1}, {4, 1, 1}, {5, 0, 0}, {6, 1, 1}, {15, 1, 1}};
    char bad[256] = ""; int nbad = 0;
    for (auto &w : want_) {
        unsigned char *p = b + w.pg * K4 + 0x10 * (w.pg + 1);
        Probe pr;
        DWORD rc = try_read(p, &pr);
        bool r_ok = w.r ? rc == 0 : (rc == EXCEPTION_ACCESS_VIOLATION && pr.addr == (ULONG_PTR)p && pr.rw == 0);
        DWORD wc = try_write(p, &pr);
        bool w_ok = w.w ? wc == 0 : (wc == EXCEPTION_ACCESS_VIOLATION && pr.addr == (ULONG_PTR)p);
        if (!r_ok || !w_ok) { nbad++; snprintf(bad + strlen(bad), sizeof bad - strlen(bad), " page%d(read %08lx write %08lx)", w.pg, rc, wc); }
    }
    check(nbad == 0, "protect_4k.access", "each 4K page faults exactly per its own protection%s", nbad ? bad : "");

    nbad = 0; bad[0] = 0;
    DWORD want_prot[] = {PAGE_READWRITE, PAGE_NOACCESS, PAGE_READONLY, PAGE_READWRITE, PAGE_READWRITE, PAGE_NOACCESS, PAGE_READWRITE};
    SIZE_T want_size[] = {K4, K4, K4, 2 * K4, K4, K4, 10 * K4};   // from the queried page to the region's end
    for (int pg = 0; pg < 7; pg++) {
        MEMORY_BASIC_INFORMATION mbi = {};
        VirtualQuery(b + pg * K4, &mbi, sizeof mbi);
        unsigned char *want_base = b + pg * K4;   // BaseAddress is the queried page
        if (mbi.Protect != want_prot[pg] || mbi.BaseAddress != want_base || mbi.RegionSize != want_size[pg] || mbi.AllocationBase != b) {
            nbad++;
            snprintf(bad + strlen(bad), sizeof bad - strlen(bad), " page%d: %s base+%lx size %lx", pg, prot_name(mbi.Protect),
                     (unsigned long)((unsigned char *)mbi.BaseAddress - b), (unsigned long)mbi.RegionSize);
        }
    }
    check(nbad == 0, "protect_4k.query", "VirtualQuery reports 4K-granular regions%s", nbad ? bad : "");

    VirtualProtect(b + 1 * K4, K4, PAGE_READWRITE, &old);
    VirtualProtect(b + 2 * K4, K4, PAGE_READWRITE, &old);
    VirtualProtect(b + 5 * K4, K4, PAGE_READWRITE, &old);
    nbad = 0;
    for (int pg = 0; pg < 16; pg++)
        for (DWORD i = 0; i < K4; i += 97) nbad += b[pg * K4 + i] != (unsigned char)(0x40 + pg);
    check(nbad == 0, "protect_4k.contents", "after protect/unprotect every page kept its data (%d bytes wrong)", nbad);
    VirtualFree(b, 0, MEM_RELEASE);
}

static void guard_checks() {
    if (!want("guard_page")) return;
    unsigned char *b = (unsigned char *)VirtualAlloc(NULL, 4 * K4, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    DWORD old;
    VirtualProtect(b + K4, K4, PAGE_READWRITE | PAGE_GUARD, &old);
    VirtualProtect(b + 3 * K4, K4, PAGE_READWRITE | PAGE_GUARD, &old);
    Probe pr;
    DWORD n0 = try_read(b + 100), n2 = try_read(b + 2 * K4 + 100);   // neighbours: no guard
    DWORD first = try_read(b + K4 + 123, &pr);
    ULONG_PTR first_addr = pr.addr;
    DWORD second = try_read(b + K4 + 123);
    MEMORY_BASIC_INFORMATION mbi = {};
    VirtualQuery(b + K4, &mbi, sizeof mbi);
    check(n0 == 0 && n2 == 0 && first == STATUS_GUARD_PAGE_VIOLATION && first_addr == (ULONG_PTR)(b + K4 + 123) && second == 0 &&
              !(mbi.Protect & PAGE_GUARD),
          "guard_page.read", "neighbours %08lx/%08lx, first touch %08lx at +%lx (want 80000001 at +%lx), second touch %08lx, then %s",
          n0, n2, first, (unsigned long)(first_addr - (ULONG_PTR)b), (unsigned long)K4 + 123, second,
          mbi.Protect & PAGE_GUARD ? "STILL GUARDED" : "unguarded");
    DWORD wfirst = try_write(b + 3 * K4 + 8), wsecond = try_write(b + 3 * K4 + 8);
    check(wfirst == STATUS_GUARD_PAGE_VIOLATION && wsecond == 0, "guard_page.write", "write: first %08lx, second %08lx", wfirst, wsecond);
    VirtualFree(b, 0, MEM_RELEASE);
}

static void reserve_checks() {
    if (!want("reserve_commit")) return;
    unsigned char *b = (unsigned char *)VirtualAlloc(NULL, 1 << 20, MEM_RESERVE, PAGE_NOACCESS);
    DWORD before = try_read(b + 5 * K4);
    unsigned char *c = (unsigned char *)VirtualAlloc(b + 5 * K4, K4, MEM_COMMIT, PAGE_READWRITE);
    DWORD mid = try_write(b + 5 * K4 + 7), lo = try_read(b + 4 * K4 + 4095), hi = try_read(b + 6 * K4);
    MEMORY_BASIC_INFORMATION m4 = {}, m5 = {}, m6 = {};
    VirtualQuery(b + 4 * K4, &m4, sizeof m4); VirtualQuery(b + 5 * K4, &m5, sizeof m5); VirtualQuery(b + 6 * K4, &m6, sizeof m6);
    bool states = m4.State == MEM_RESERVE && m5.State == MEM_COMMIT && m5.RegionSize == K4 && m6.State == MEM_RESERVE;
    check(before == EXCEPTION_ACCESS_VIOLATION && c == b + 5 * K4 && mid == 0 && lo == EXCEPTION_ACCESS_VIOLATION &&
              hi == EXCEPTION_ACCESS_VIOLATION && states,
          "reserve_commit.single_page", "reserved %08lx, commit one 4K page %s, it %08lx, neighbours %08lx/%08lx, states %lx/%lx(size %lx)/%lx",
          before, c == b + 5 * K4 ? "ok" : "FAILED", mid, lo, hi, m4.State, m5.State, (unsigned long)m5.RegionSize, m6.State);

    b[5 * K4 + 7] = 0x99;
    VirtualFree(b + 5 * K4, K4, MEM_DECOMMIT);
    DWORD after = try_read(b + 5 * K4 + 7);
    VirtualAlloc(b + 5 * K4, K4, MEM_COMMIT, PAGE_READWRITE);
    check(after == EXCEPTION_ACCESS_VIOLATION && b[5 * K4 + 7] == 0, "reserve_commit.decommit",
          "decommitted page faults (%08lx) and recommits zeroed (%02x)", after, b[5 * K4 + 7]);
    VirtualFree(b, 0, MEM_RELEASE);
}

// GetWriteWatch must report exactly the 4K pages written, which GCs (.NET, Unity) rely on.
static void write_watch_checks() {
    if (!want("write_watch")) return;
    unsigned char *b = (unsigned char *)VirtualAlloc(NULL, 64 * K4, MEM_RESERVE | MEM_COMMIT | MEM_WRITE_WATCH, PAGE_READWRITE);
    if (!b) { fail("write_watch", "VirtualAlloc(MEM_WRITE_WATCH) failed: %lu", GetLastError()); return; }
    ResetWriteWatch(b, 64 * K4);
    int pages[] = {1, 2, 7, 13, 30, 63};
    for (int p : pages) b[p * K4 + 5] = 1;
    void *hits[64]; ULONG_PTR n = 64; ULONG gran = 0;
    UINT r = GetWriteWatch(WRITE_WATCH_FLAG_RESET, b, 64 * K4, hits, &n, &gran);
    char got[160] = ""; bool exact = r == 0 && gran == K4 && n == 6;
    for (ULONG_PTR i = 0; i < n && i < 64; i++) {
        int pg = (int)(((unsigned char *)hits[i] - b) / K4);
        snprintf(got + strlen(got), sizeof got - strlen(got), "%s%d", i ? "," : "", pg);
        if (i < 6 && pg != pages[i]) exact = false;
    }
    n = 64;
    GetWriteWatch(0, b, 64 * K4, hits, &n, &gran);
    check(exact && n == 0, "write_watch", "written pages 1,2,7,13,30,63; reported [%s] at granularity %lu; after reset %lu",
          got, gran, (unsigned long)n);
    VirtualFree(b, 0, MEM_RELEASE);
}

// ---- placeholders (VirtualAlloc2 / MapViewOfFile3) ------------------------------------------------
typedef PVOID(WINAPI *VirtualAlloc2_t)(HANDLE, PVOID, SIZE_T, ULONG, ULONG, MEM_EXTENDED_PARAMETER *, ULONG);
typedef PVOID(WINAPI *MapViewOfFile3_t)(HANDLE, HANDLE, PVOID, ULONG64, SIZE_T, ULONG, ULONG, MEM_EXTENDED_PARAMETER *, ULONG);
typedef BOOL(WINAPI *UnmapViewOfFile2_t)(HANDLE, PVOID, ULONG);

static void placeholder_checks() {
    if (!want("placeholder")) return;
    HMODULE kb = GetModuleHandleA("kernelbase.dll");
    auto va2 = (VirtualAlloc2_t)(void *)GetProcAddress(kb, "VirtualAlloc2");
    auto map3 = (MapViewOfFile3_t)(void *)GetProcAddress(kb, "MapViewOfFile3");
    auto unmap2 = (UnmapViewOfFile2_t)(void *)GetProcAddress(kb, "UnmapViewOfFile2");
    if (!va2 || !map3 || !unmap2) { skip("placeholder", "kernelbase has no VirtualAlloc2/MapViewOfFile3/UnmapViewOfFile2"); return; }
    const SIZE_T G = 65536;
    unsigned char *p = (unsigned char *)va2(NULL, NULL, 4 * G, MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, NULL, 0);
    if (!check(p != NULL, "placeholder.reserve", "VirtualAlloc2(MEM_RESERVE_PLACEHOLDER) %s (%lu)", p ? "ok" : "failed", GetLastError())) return;
    // Split off the first two 64K pieces.
    BOOL s1 = VirtualFree(p, G, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER);
    BOOL s2 = VirtualFree(p + G, G, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER);
    check(s1 && s2, "placeholder.split", "VirtualFree(MEM_PRESERVE_PLACEHOLDER) %s (%lu)", s1 && s2 ? "split it" : "failed", GetLastError());
    unsigned char *a = (unsigned char *)va2(NULL, p, G, MEM_RESERVE | MEM_COMMIT | MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, NULL, 0);
    bool a_ok = a == p;
    if (a_ok) { memset(a, 0x5c, G); a_ok = a[G - 1] == 0x5c; }
    check(a_ok, "placeholder.replace_alloc", "VirtualAlloc2(MEM_REPLACE_PLACEHOLDER) %s", a_ok ? "committed into the placeholder" : "failed");

    HANDLE sec = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, (DWORD)G, NULL);
    unsigned char *v = (unsigned char *)map3(sec, GetCurrentProcess(), p + G, 0, G, MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, NULL, 0);
    unsigned char *alias = (unsigned char *)MapViewOfFile(sec, FILE_MAP_ALL_ACCESS, 0, 0, G);
    bool v_ok = v == p + G && alias;
    if (v_ok) { v[123] = 0xA5; alias[G - 7] = 0x3C; v_ok = alias[123] == 0xA5 && v[G - 7] == 0x3C; }
    check(v_ok, "placeholder.map_view", "MapViewOfFile3 into the placeholder %s", v_ok ? "aliases a second view" : "failed");
    if (v == p + G) unmap2(GetCurrentProcess(), v, MEM_PRESERVE_PLACEHOLDER);
    if (alias) UnmapViewOfFile(alias);
    MEMORY_BASIC_INFORMATION mbi = {};
    VirtualQuery(p + G, &mbi, sizeof mbi);
    check(mbi.State == MEM_RESERVE, "placeholder.unmap_preserve", "after UnmapViewOfFile2(MEM_PRESERVE_PLACEHOLDER) the range is %s",
          mbi.State == MEM_RESERVE ? "a placeholder again" : "not reserved");
    CloseHandle(sec);
    VirtualFree(p, 0, MEM_RELEASE);
    VirtualFree(p + G, 0, MEM_RELEASE);
    VirtualFree(p + 2 * G, 0, MEM_RELEASE);
}

static void section_checks() {
    if (!want("section_alias")) return;
    HANDLE sec = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, 1 << 20, NULL);
    unsigned char *v1 = (unsigned char *)MapViewOfFile(sec, FILE_MAP_ALL_ACCESS, 0, 0, 0);
    unsigned char *v2 = (unsigned char *)MapViewOfFile(sec, FILE_MAP_ALL_ACCESS, 0, 65536, 65536);   // offset view
    int bad = 0;
    if (v1 && v2) {
        for (int i = 0; i < 65536; i += 512) v1[65536 + i] = (unsigned char)(i >> 9);
        for (int i = 0; i < 65536; i += 512) bad += v2[i] != (unsigned char)(i >> 9);
    }
    check(v1 && v2 && bad == 0, "section_alias", "two views of one section see each other's writes (%d mismatches)", bad);
    if (v1) UnmapViewOfFile(v1);
    if (v2) UnmapViewOfFile(v2);
    CloseHandle(sec);
}

// ---- self-modifying code ------------------------------------------------------------------------
// Code emitted is in the CPU's own instruction set: x86 for i686 and x86_64, and also x86_64
// for arm64ec, because a call to memory that isn't marked as ARM64EC code goes to the x64
// emulator (FEX), exactly as a game's own JIT output would. Native ARM64 for aarch64.
#if defined(__aarch64__) && !defined(__arm64ec__)
#define CODE_IS_X86 0
static size_t emit_ret(unsigned char *p, uint32_t v) {
    uint32_t ins[3] = {0x52800000u | ((v & 0xffff) << 5),          // movz w0, #lo
                       0x72a00000u | (((v >> 16) & 0xffff) << 5),  // movk w0, #hi, lsl #16
                       0xd65f03c0u};                                // ret
    memcpy(p, ins, sizeof ins);
    return sizeof ins;
}
#else
#define CODE_IS_X86 1
static size_t emit_ret(unsigned char *p, uint32_t v) {
    p[0] = 0xb8; memcpy(p + 1, &v, 4); p[5] = 0xc3;   // mov eax, imm32; ret
    return 6;
}
#endif
typedef uint32_t (*code_fn)(void);
static void flush(void *p, size_t n) { FlushInstructionCache(GetCurrentProcess(), p, n); }

static void smc_checks() {
    if (want("smc.wx_cycle")) {   // W^X: write as RW, flip to RX, run; repeat at the same address
        unsigned char *c = (unsigned char *)VirtualAlloc(NULL, K4, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        int bad = 0; DWORD old;
        for (uint32_t i = 0; i < 2000; i++) {
            VirtualProtect(c, K4, PAGE_READWRITE, &old);
            size_t n = emit_ret(c + (i % 4) * 16, 0x1000 + i);   // move around inside the page too
            VirtualProtect(c, K4, PAGE_EXECUTE_READ, &old);
            flush(c, K4);
            if (((code_fn)(c + (i % 4) * 16))() != 0x1000 + i) bad++;
            (void)n;
        }
        check(bad == 0, "smc.wx_cycle", "2000 rewrites through RW -> RX + FlushInstructionCache, %d stale results", bad);
        VirtualFree(c, 0, MEM_RELEASE);
    }
    if (want("smc.rwx_flush")) {   // RWX page rewritten in place, with flush
        unsigned char *c = (unsigned char *)VirtualAlloc(NULL, K4, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        int bad = 0;
        for (uint32_t i = 0; i < 5000; i++) {
            emit_ret(c, 0xA000 + i);
            flush(c, 16);
            if (((code_fn)c)() != 0xA000 + i) bad++;
        }
        check(bad == 0, "smc.rwx_flush", "5000 in-place rewrites of RWX code with FlushInstructionCache, %d stale results", bad);
        VirtualFree(c, 0, MEM_RELEASE);
    }
    if (want("smc.rwx_noflush")) {   // x86 semantics: stores to code are seen without a flush
        if (!CODE_IS_X86) skip("smc.rwx_noflush", "ARM64 code needs FlushInstructionCache by architecture");
        else {
            unsigned char *c = (unsigned char *)VirtualAlloc(NULL, K4, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
            emit_ret(c, 0);
            flush(c, 16);
            int bad = 0;
            for (uint32_t i = 1; i <= 5000; i++) {
                volatile uint32_t *imm = (volatile uint32_t *)(c + 1);   // the mov's immediate, unaligned on purpose
                *imm = i;
                if (((code_fn)c)() != i) bad++;
            }
            check(bad == 0, "smc.rwx_noflush", "5000 immediate patches without a flush (legal on x86), %d stale results", bad);
            VirtualFree(c, 0, MEM_RELEASE);
        }
    }
    if (want("smc.same_page_data")) {   // a hot data word in the same 4K page as running code
        unsigned char *c = (unsigned char *)VirtualAlloc(NULL, K4, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        emit_ret(c, 0x5151);
        flush(c, 16);
        volatile uint32_t *data = (volatile uint32_t *)(c + 2048);
        int bad = 0;
        for (uint32_t i = 0; i < 20000; i++) {
            *data = i;
            if (((code_fn)c)() != 0x5151 || *data != i) bad++;
        }
        check(bad == 0, "smc.same_page_data", "20000 data writes next to live code, %d wrong results", bad);
        VirtualFree(c, 0, MEM_RELEASE);
    }
    if (want("smc.realloc")) {   // free code and get new code at the same address
        int bad = 0, moved = 0;
        void *at = NULL;
        for (uint32_t i = 0; i < 200; i++) {
            unsigned char *c = (unsigned char *)VirtualAlloc(at, 65536, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
            if (!c) { c = (unsigned char *)VirtualAlloc(NULL, 65536, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE); moved++; }
            at = c;
            emit_ret(c + 64, 0xBEE0 + i);
            flush(c + 64, 16);
            if (((code_fn)(c + 64))() != 0xBEE0 + i) bad++;
            VirtualFree(c, 0, MEM_RELEASE);
        }
        check(bad == 0, "smc.realloc", "200 release/reallocate cycles at one address (%d moved), %d stale results", moved, bad);
    }
    if (want("smc.neighbor_page")) {   // code page RX, the 4K page after it RW and hot (same 16K host page)
        unsigned char *c = (unsigned char *)VirtualAlloc(NULL, 4 * K4, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        DWORD old;
        int bad = 0;
        for (uint32_t i = 0; i < 300; i++) {
            VirtualProtect(c, K4, PAGE_READWRITE, &old);
            emit_ret(c, 0xC0DE + i);
            VirtualProtect(c, K4, PAGE_EXECUTE_READ, &old);
            flush(c, 16);
            for (int j = 0; j < 64; j++) c[K4 + j * 64] = (unsigned char)(i + j);   // the RW neighbour stays writable
            if (((code_fn)c)() != 0xC0DE + i) bad++;
            if (c[K4 + 64] != (unsigned char)(i + 1)) bad++;
        }
        Probe pr;
        DWORD w = try_write(c + 8, &pr);   // the code page itself must still be read-only
        check(bad == 0 && w == EXCEPTION_ACCESS_VIOLATION, "smc.neighbor_page",
              "RX code page next to a hot RW page: %d wrong results, write to the RX page %08lx", bad, w);
        VirtualFree(c, 0, MEM_RELEASE);
    }
}

// Writer thread rewrites code, reader thread runs it: a JIT compiling on one thread and running
// on another (Java, V8). FEX must invalidate translations the other thread already has.
struct XThread { unsigned char *code; HANDLE wrote, ran; volatile LONG stop; int bad; uint32_t seen_last; };
static DWORD WINAPI smc_reader(void *p) {
    XThread *x = (XThread *)p;
    for (uint32_t i = 0; i < 500; i++) {
        if (WaitForSingleObject(x->wrote, 10000) != WAIT_OBJECT_0) { x->bad += 1000; break; }
        uint32_t v = ((code_fn)x->code)();
        if (v != 0x7000 + i) x->bad++;
        x->seen_last = v;
        SetEvent(x->ran);
    }
    return 0;
}
static void smc_thread_checks() {
    if (!want("smc.cross_thread")) return;
    XThread x = {};
    x.code = (unsigned char *)VirtualAlloc(NULL, K4, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    x.wrote = CreateEventA(NULL, FALSE, FALSE, NULL); x.ran = CreateEventA(NULL, FALSE, FALSE, NULL);
    HANDLE t = CreateThread(NULL, 0, smc_reader, &x, 0, NULL);
    for (uint32_t i = 0; i < 500; i++) {
        emit_ret(x.code, 0x7000 + i);
        flush(x.code, 16);
        SetEvent(x.wrote);
        if (WaitForSingleObject(x.ran, 10000) != WAIT_OBJECT_0) { x.bad += 1000; break; }
    }
    WaitForSingleObject(t, 10000); CloseHandle(t);
    check(x.bad == 0, "smc.cross_thread", "500 rewrites on one thread, run on another: %d stale results (last %x)", x.bad, x.seen_last);
    CloseHandle(x.wrote); CloseHandle(x.ran);
    VirtualFree(x.code, 0, MEM_RELEASE);
}

static void big_alloc_checks() {
    if (!want("big_alloc")) return;
    // Reserve 1 GB and commit and touch 256 MB at its end (512/128 MB for 32-bit, whose 2 GB
    // address space is already fragmented by Wine and the DLLs).
    const SIZE_T R = (SIZE_T)(sizeof(void *) == 8 ? 1024 : 512) << 20, C = R / 4;
    unsigned char *b = (unsigned char *)VirtualAlloc(NULL, R, MEM_RESERVE, PAGE_NOACCESS);
    unsigned char *c = b ? (unsigned char *)VirtualAlloc(b + R - C, C, MEM_COMMIT, PAGE_READWRITE) : NULL;
    int bad = 0;
    if (c) {
        for (SIZE_T i = 0; i < C; i += K4) c[i] = (unsigned char)(i >> 12);
        for (SIZE_T i = 0; i < C; i += K4) bad += c[i] != (unsigned char)(i >> 12);
    }
    check(c && bad == 0, "big_alloc", "reserve %u MB, commit and touch %u MB at its end: %s, %d bad pages", (unsigned)(R >> 20),
          (unsigned)(C >> 20), c ? "ok" : "allocation failed", bad);
    if (b) VirtualFree(b, 0, MEM_RELEASE);
}

int main(int argc, char **argv) {
    ladder_begin("memory", argc, argv);
    sysinfo_checks();
    protect_checks();
    guard_checks();
    reserve_checks();
    write_watch_checks();
    placeholder_checks();
    section_checks();
    smc_checks();
    smc_thread_checks();
    big_alloc_checks();
    return ladder_done();
}
