// mpbench: tiny deterministic Windows benchmark for comparing translation stacks
// (Rosetta vs FEX, wined3d vs DXVK vs DXMT vs D3DMetal, winevulkan vs native, ...).
//
// Usage: mpbench.exe [--out results.json] [--frames N] [--width W] [--height H]
//                    [--api d3d11|d3d12|vk|all[,...]] [--scenes PREFIX[,PREFIX...]]
//                    [--d3d12-shaders dxil|dxbc] [--note TEXT] [--no-gpu] [--no-cpu] [--list]
//
// --scenes takes scene names or prefixes ("cpu", "gpu12", "gpu12.features", "vk.fill").
// --api restricts the GPU APIs; CPU scenes are unaffected (use --no-cpu or --scenes).
//
// Scenes:
//   cpu.scalar           double-precision mandelbrot, single thread       (FP codegen)
//   cpu.vector           float SAXPY-ish loop the compiler vectorises      (SSE/NEON codegen)
//   cpu.memcpy           64 MiB memcpy bandwidth                           (memory)
//   cpu.atomic           4 threads hammering InterlockedIncrement          (locked RMW)
//   cpu.pingpong         2 threads passing a token via plain volatile ops  (TSO-sensitive)
//   gpu11.drawcalls      D3D11: 4000 draws/frame, Map/Unmap of a CB each   (API translation overhead)
//   gpu11.fill           D3D11: fullscreen ALU-heavy pixel shader          (shader translation / GPU)
//   gpu12.drawcalls      D3D12: 4000 draws/frame, root CBV into an upload ring each
//   gpu12.fill           D3D12: same fullscreen pixel shader
//   gpu12.features.gs    D3D12 feature probe: geometry shader point->quad expansion
//   gpu12.features.so    D3D12 feature probe: stream output (transform feedback)
//   gpu12.features.tess  D3D12 feature probe: hull/domain shader tessellation
//   vk.drawcalls         raw Vulkan via vulkan-1.dll: 4000 draws/frame, push constants each
//   vk.fill              raw Vulkan: same fullscreen pixel shader
// Each API also reports a <api>.device row (adapter / driver name).
//
// The feature probes render (or stream out) a known pattern, read it back and check it,
// then time 100 passes. They report "ok": true/false plus passes/s, so the benchmark
// doubles as a probe for features Metal lacks natively (KosmicKrisp, MoltenVK, DXMT and
// D3DMetal all emulate GS / SO / tessellation in different ways).
//
// Shaders (see bench/mpbench.hlsl and bench/gen_shaders.py):
//   D3D11 compiles HLSL at runtime with D3DCompile, as in v1.
//   D3D12 uses DXIL (SM 6.0) precompiled with dxc and embedded in mpbench_shaders.h. That
//   keeps the bytecode identical on every stack (it does not depend on the Wine version's
//   d3dcompiler), matches what modern D3D12 games ship, and is the native input of
//   Metal Shader Converter. --d3d12-shaders dxbc instead compiles DXBC at runtime with
//   D3DCompile, to exercise the DXBC path (vkd3d-shader in vkd3d-proton). It targets SM 5.0,
//   not 5.1: the shaders use nothing 5.1-specific (no register spaces or descriptor arrays),
//   D3D12 accepts 5.0 bytecode, and Wine 9's d3dcompiler cannot target 5.1 at all.
//   Vulkan uses SPIR-V 1.0 from the same HLSL (dxc -spirv), also embedded.
// Nothing beyond system DLLs is needed at runtime. vulkan-1.dll is loaded dynamically, so
// D3D-only stacks still run, and the Vulkan declarations below are a minimal subset
// copied from vulkan_core.h (build.sh checks them against the real header when present).

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <d3dcompiler.h>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include "mpbench_shaders.h"

static double now_ms() {
    static LARGE_INTEGER freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f; }();
    LARGE_INTEGER t; QueryPerformanceCounter(&t);
    return (double)t.QuadPart * 1000.0 / (double)freq.QuadPart;
}

static std::string fmt(const char *f, ...) {
    char buf[1024]; va_list ap; va_start(ap, f); vsnprintf(buf, sizeof buf, f, ap); va_end(ap); return buf;
}

static std::string jstr(const std::string &s) {
    std::string o = "\"";
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += (char)c; }
        else if (c < 0x20) o += fmt("\\u%04x", c);
        else o += (char)c;
    }
    return o + "\"";
}

// ---------------------------------------------------------------- results / scene selection

struct Result { std::string name, unit; double value; std::string extra; };
static std::vector<Result> g_results;
static int g_errors_total;     // hard errors (API unavailable, device lost, ...): exit code 1

static void report(const char *name, double value, const char *unit, std::string extra = "") {
    printf("%-20s %12.2f %-12s %s\n", name, value, unit, extra.c_str());
    fflush(stdout);
    g_results.push_back({name, unit, value, extra});
}

static void report_error(const char *name, const std::string &why) {
    g_errors_total++;
    report(name, 0, "error", "\"ok\": false, \"error\": " + jstr(why));
}

static const char *k_scenes[] = {
    "cpu.scalar", "cpu.vector", "cpu.memcpy", "cpu.atomic", "cpu.pingpong",
    "gpu11.drawcalls", "gpu11.fill",
    "gpu12.drawcalls", "gpu12.fill", "gpu12.features.gs", "gpu12.features.so", "gpu12.features.tess",
    "vk.drawcalls", "vk.fill",
};
enum { API_D3D11 = 1, API_D3D12 = 2, API_VK = 4, API_ALL = 7 };
static std::vector<std::string> g_filters;
static unsigned g_apis = API_ALL;
static bool g_cpu = true;

static bool want(const char *scene) {
    if (!strncmp(scene, "cpu.", 4) && !g_cpu) return false;
    if (!strncmp(scene, "gpu11.", 6) && !(g_apis & API_D3D11)) return false;
    if (!strncmp(scene, "gpu12.", 6) && !(g_apis & API_D3D12)) return false;
    if (!strncmp(scene, "vk.", 3) && !(g_apis & API_VK)) return false;
    if (g_filters.empty()) return true;
    for (auto &f : g_filters)
        if (f == "all" || !strncmp(scene, f.c_str(), f.size())) return true;
    return false;
}

static std::vector<std::string> split(const char *s) {
    std::vector<std::string> v; std::string cur;
    for (; ; s++) {
        if (!*s || *s == ',') { if (!cur.empty()) v.push_back(cur); cur.clear(); if (!*s) break; }
        else if (*s != ' ') cur += *s;
    }
    return v;
}

static std::string frame_stats(std::vector<double> &ft) {
    std::sort(ft.begin(), ft.end());
    auto pct = [&](double p) { return ft[(size_t)std::min<double>(ft.size() - 1, p * ft.size())]; };
    return fmt("\"p50_ms\": %.3f, \"p99_ms\": %.3f, \"max_ms\": %.3f", pct(0.5), pct(0.99), ft.back());
}

// ---------------------------------------------------------------- CPU scenes

static volatile double g_sink;

static void cpu_scalar() {
    double t0 = now_ms(); long iters = 0;
    for (int py = 0; py < 600; py++)
        for (int px = 0; px < 800; px++) {
            double cr = px / 400.0 - 1.5, ci = py / 300.0 - 1.0, zr = 0, zi = 0; int i = 0;
            for (; i < 256 && zr * zr + zi * zi < 4.0; i++) { double t = zr * zr - zi * zi + cr; zi = 2 * zr * zi + ci; zr = t; }
            iters += i;
        }
    double dt = now_ms() - t0; g_sink = (double)iters;
    report("cpu.scalar", iters / dt / 1000.0, "Miter/s");
}

static void cpu_vector() {
    const int n = 1 << 20; std::vector<float> x(n, 1.0f), y(n, 2.0f);
    double t0 = now_ms();
    for (int r = 0; r < 200; r++) { float a = 1.0001f + r * 1e-6f; for (int i = 0; i < n; i++) y[i] = a * x[i] + y[i] * 0.5f; }
    double dt = now_ms() - t0; g_sink = y[n / 2];
    report("cpu.vector", 200.0 * n / dt / 1e6, "GFLOP/s(x2)");
}

static void cpu_memcpy() {
    const size_t n = 64u << 20; char *a = (char *)malloc(n), *b = (char *)malloc(n);
    memset(a, 1, n); memset(b, 2, n);
    double t0 = now_ms();
    for (int r = 0; r < 20; r++) { memcpy(r & 1 ? a : b, r & 1 ? b : a, n); }
    double dt = now_ms() - t0; g_sink = a[123] + b[456]; free(a); free(b);
    report("cpu.memcpy", 20.0 * n / dt / 1e6, "GB/s");
}

static volatile LONG g_counter;
static DWORD WINAPI atomic_worker(void *) { for (int i = 0; i < 2000000; i++) InterlockedIncrement(&g_counter); return 0; }

static void cpu_atomic() {
    HANDLE th[4]; g_counter = 0; double t0 = now_ms();
    for (auto &h : th) h = CreateThread(nullptr, 0, atomic_worker, nullptr, 0, nullptr);
    WaitForMultipleObjects(4, th, TRUE, INFINITE);
    double dt = now_ms() - t0; for (auto h : th) CloseHandle(h);
    report("cpu.atomic", g_counter / dt / 1000.0, "Mops/s", g_counter == 8000000 ? "" : "\"WRONG_COUNT\": true");
}

// Message passing with plain stores/loads: correct on x86 because of TSO. If the
// translator gets memory ordering wrong, payload checks fail.
static volatile int g_flag, g_payload; static volatile int g_errors;
static const int PP_ROUNDS = 200000;
static DWORD WINAPI pp_consumer(void *) {
    for (int i = 1; i <= PP_ROUNDS; i++) {
        while (g_flag != i) YieldProcessor();
        if (g_payload != i * 7) g_errors = g_errors + 1;
        g_flag = -i;
    }
    return 0;
}

static void cpu_pingpong() {
    g_flag = 0; g_errors = 0;
    HANDLE th = CreateThread(nullptr, 0, pp_consumer, nullptr, 0, nullptr);
    double t0 = now_ms();
    for (int i = 1; i <= PP_ROUNDS; i++) {
        g_payload = i * 7;   // store payload ...
        g_flag = i;          // ... then publish (ordering guaranteed by x86 TSO)
        while (g_flag != -i) YieldProcessor();
    }
    WaitForSingleObject(th, INFINITE); CloseHandle(th);
    double dt = now_ms() - t0;
    report("cpu.pingpong", PP_ROUNDS / dt, "Krt/s", fmt("\"ordering_errors\": %d", (int)g_errors));
}

// ---------------------------------------------------------------- shared GPU helpers

struct Opts { int frames = 600, width = 1280, height = 720; bool dxil = true; };
static const int WARM_FRAMES = 30, DRAWS_PER_FRAME = 4000;

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_CLOSE) { PostQuitMessage(0); return 0; }
    return DefWindowProcA(h, m, w, l);
}

static bool pump() { MSG msg; while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) { if (msg.message == WM_QUIT) return false; TranslateMessage(&msg); DispatchMessageA(&msg); } return true; }

static HWND make_window(const char *title, int width, int height) {
    static bool registered = false;
    WNDCLASSA wc = {}; wc.lpfnWndProc = wndproc; wc.hInstance = GetModuleHandleA(nullptr); wc.lpszClassName = "mpbench";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    if (!registered) { RegisterClassA(&wc); registered = true; }
    RECT r = {0, 0, width, height}; AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    return CreateWindowA("mpbench", title, WS_OVERLAPPEDWINDOW | WS_VISIBLE, 100, 100,
                         r.right - r.left, r.bottom - r.top, nullptr, nullptr, wc.hInstance, nullptr);
}

// Per-draw constants for the drawcalls scene: same pattern for every API.
static void draw_consts(int i, int f, float out[8]) {
    float x = -1.0f + (i % 80) * 0.025f, y = 1.0f - (i / 80) * 0.04f;
    float d[8] = {x, y, 0.01f, -0.015f, (i % 7) / 7.0f, ((i + f) % 5) / 5.0f, (i % 3) / 3.0f, 1};
    memcpy(out, d, sizeof d);
}

// Shared frame loop: record+submit one frame, keep at most 2 in flight, collect frame times.
template <class Frame> static bool frame_loop(const char *name, int frames, Frame &&frame, std::string &err) {
    std::vector<double> ft; ft.reserve(frames);
    double start = 0;
    for (int f = 0; f < frames + WARM_FRAMES; f++) {
        if (!pump()) { err = "window closed"; return false; }
        double t0 = now_ms();
        if (f == WARM_FRAMES) start = t0;
        if (!frame(f)) return false;
        if (f >= WARM_FRAMES) ft.push_back(now_ms() - t0);
    }
    if (!frame(-1)) return false;   // -1: drain the GPU
    double total = now_ms() - start;
    report(name, frames * 1000.0 / total, "fps", frame_stats(ft));
    return true;
}

static std::string g_err;
#define CHECK(x) do { HRESULT hr_ = (x); if (FAILED(hr_)) { g_err = fmt("%s: 0x%08lx", #x, (unsigned long)hr_); return false; } } while (0)

// The feature-probe shaders sit behind per-feature macros (see mpbench.hlsl).
static const char *feature_macro(const char *entry) {
    static const char *map[][2] = {{"vs_point", "MP_FEAT_GS"}, {"gs_quad", "MP_FEAT_GS"}, {"ps_col", "MP_FEAT_COMMON"},
                                   {"vs_so", "MP_FEAT_SO"}, {"vs_patch", "MP_FEAT_TESS"}, {"hs_main", "MP_FEAT_TESS"},
                                   {"ds_disk", "MP_FEAT_TESS"}};
    for (auto &m : map) if (!strcmp(m[0], entry)) return m[1];
    return nullptr;
}

static bool compile(const char *entry, const char *target, ID3DBlob **out) {
    ID3DBlob *err = nullptr;
    D3D_SHADER_MACRO defs[] = {{feature_macro(entry), "1"}, {nullptr, nullptr}};
    HRESULT hr = D3DCompile(k_hlsl, strlen(k_hlsl), "mpbench.hlsl", defs[0].Name ? defs : nullptr, nullptr, entry, target, 0, 0, out, &err);
    if (FAILED(hr)) {
        std::string msg = err ? std::string((const char *)err->GetBufferPointer(), err->GetBufferSize()) : "";
        msg = msg.substr(0, msg.find('\0'));
        while (!msg.empty() && (msg.back() == '\n' || msg.back() == '\r')) msg.pop_back();
        g_err = fmt("D3DCompile %s (%s): 0x%08lx %s", entry, target, (unsigned long)hr, msg.c_str());
        if (err) err->Release();
        return false;
    }
    return true;
}

static std::string wide_to_utf8(const WCHAR *w) {
    char buf[256]; WideCharToMultiByte(CP_UTF8, 0, w, -1, buf, sizeof buf, nullptr, nullptr); return buf;
}

template <class T> struct Com {
    T *p = nullptr;
    Com() = default; Com(const Com &) = delete; Com &operator=(const Com &) = delete;
    ~Com() { if (p) p->Release(); }
    T *operator->() const { return p; }
    T **operator&() { return &p; }
    operator T *() const { return p; }
};

// ---------------------------------------------------------------- D3D11

static bool d3d11_run(const Opts &o) {
    int width = o.width, height = o.height, frames = o.frames;
    HWND hwnd = make_window("mpbench d3d11", width, height);
    struct WndGuard { HWND h; ~WndGuard() { DestroyWindow(h); } } wg{hwnd};

    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 2; sd.BufferDesc.Width = width; sd.BufferDesc.Height = height;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd; sd.SampleDesc.Count = 1; sd.Windowed = TRUE; sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

    Com<ID3D11Device> dev; Com<ID3D11DeviceContext> ctx; Com<IDXGISwapChain> sc; D3D_FEATURE_LEVEL fl;
    CHECK(D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
                                        &sd, &sc, &dev, &fl, &ctx));

    // Adapter name tells us which translation layer/driver we landed on.
    Com<IDXGIDevice> dxgidev; Com<IDXGIAdapter> adapter; DXGI_ADAPTER_DESC ad;
    CHECK(dev->QueryInterface(__uuidof(IDXGIDevice), (void **)&dxgidev)); CHECK(dxgidev->GetAdapter(&adapter)); CHECK(adapter->GetDesc(&ad));
    std::string aname = wide_to_utf8(ad.Description);
    std::string ainfo = fmt("\"adapter\": %s, \"feature_level\": \"0x%x\"", jstr(aname).c_str(), (unsigned)fl);
    if (fl < D3D_FEATURE_LEVEL_11_0) {
        // e.g. stock Wine on macOS: wined3d over Apple's GL 4.1 tops out at 9_3.
        report("gpu11.device", 0, "fl<11_0", ainfo);
        return true;
    }
    report("gpu11.device", 1, "ok", ainfo);

    Com<ID3D11Texture2D> bb; Com<ID3D11RenderTargetView> rtv;
    CHECK(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void **)&bb)); CHECK(dev->CreateRenderTargetView(bb, nullptr, &rtv));

    Com<ID3DBlob> b_vsq, b_vsf, b_psf, b_psh;
    if (!compile("vs_quad", "vs_5_0", &b_vsq) || !compile("vs_full", "vs_5_0", &b_vsf) ||
        !compile("ps_flat", "ps_5_0", &b_psf) || !compile("ps_heavy", "ps_5_0", &b_psh)) return false;
    Com<ID3D11VertexShader> vsq, vsf; Com<ID3D11PixelShader> psf, psh;
    CHECK(dev->CreateVertexShader(b_vsq->GetBufferPointer(), b_vsq->GetBufferSize(), nullptr, &vsq));
    CHECK(dev->CreateVertexShader(b_vsf->GetBufferPointer(), b_vsf->GetBufferSize(), nullptr, &vsf));
    CHECK(dev->CreatePixelShader(b_psf->GetBufferPointer(), b_psf->GetBufferSize(), nullptr, &psf));
    CHECK(dev->CreatePixelShader(b_psh->GetBufferPointer(), b_psh->GetBufferSize(), nullptr, &psh));

    D3D11_BUFFER_DESC cbd = {}; cbd.ByteWidth = 32; cbd.Usage = D3D11_USAGE_DYNAMIC;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER; cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    Com<ID3D11Buffer> cb; CHECK(dev->CreateBuffer(&cbd, nullptr, &cb));

    D3D11_QUERY_DESC qd = {D3D11_QUERY_EVENT, 0}; Com<ID3D11Query> q[3];
    for (auto &qq : q) CHECK(dev->CreateQuery(&qd, &qq));

    D3D11_VIEWPORT vp = {0, 0, (float)width, (float)height, 0, 1};
    auto set_cb = [&](const float d[8]) {
        D3D11_MAPPED_SUBRESOURCE m; ctx->Map(cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m);
        memcpy(m.pData, d, 32); ctx->Unmap(cb, 0);
    };

    auto run_scene = [&](const char *name, auto &&draw) -> bool {
        return frame_loop(name, frames, [&](int f) -> bool {
            if (f < 0) { ctx->End(q[0]); while (ctx->GetData(q[0], nullptr, 0, 0) == S_FALSE) YieldProcessor(); return true; }
            ID3D11RenderTargetView *rt = rtv; ID3D11Buffer *cbp = cb;
            ctx->OMSetRenderTargets(1, &rt, nullptr); ctx->RSSetViewports(1, &vp);
            ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            ctx->VSSetConstantBuffers(0, 1, &cbp); ctx->PSSetConstantBuffers(0, 1, &cbp);
            float clear[4] = {0.1f, 0.1f, 0.15f, 1}; ctx->ClearRenderTargetView(rtv, clear);
            draw(f);
            ctx->End(q[f % 3]);
            sc->Present(0, 0);
            // Keep at most 2 frames in flight so CPU frame times reflect GPU work.
            if (f >= 2) while (ctx->GetData(q[(f - 2) % 3], nullptr, 0, 0) == S_FALSE) YieldProcessor();
            return true;
        }, g_err);
    };

    bool ok = true;
    if (want("gpu11.drawcalls") && !run_scene("gpu11.drawcalls", [&](int f) {
            ctx->VSSetShader(vsq, nullptr, 0); ctx->PSSetShader(psf, nullptr, 0);
            for (int i = 0; i < DRAWS_PER_FRAME; i++) { float d[8]; draw_consts(i, f, d); set_cb(d); ctx->Draw(3, 0); }
        })) { report_error("gpu11.drawcalls", g_err); ok = false; }
    if (ok && want("gpu11.fill") && !run_scene("gpu11.fill", [&](int f) {
            float d[8] = {0, 0, 0, 0, (float)f, (float)f, 0, 1}; set_cb(d);
            ctx->VSSetShader(vsf, nullptr, 0); ctx->PSSetShader(psh, nullptr, 0);
            ctx->Draw(3, 0);
        })) report_error("gpu11.fill", g_err);
    ctx->ClearState(); ctx->Flush();
    return true;
}

// ---------------------------------------------------------------- D3D12

struct D12 {
    Com<ID3D12Device> dev; Com<ID3D12CommandQueue> queue; Com<ID3D12Fence> fence; UINT64 fence_val = 0;
    HANDLE ev = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    Com<ID3D12CommandAllocator> alloc[2]; Com<ID3D12GraphicsCommandList> cl;
    bool dxil = true;
    std::vector<ID3DBlob *> blobs;
    ~D12() { flush(); for (auto b : blobs) b->Release(); CloseHandle(ev); }

    void wait(UINT64 v) { if (fence->GetCompletedValue() < v) { fence->SetEventOnCompletion(v, ev); WaitForSingleObject(ev, INFINITE); } }
    UINT64 signal() { queue->Signal(fence, ++fence_val); return fence_val; }
    void flush() { if (fence) wait(signal()); }
    void submit() { cl->Close(); ID3D12CommandList *l = cl; queue->ExecuteCommandLists(1, &l); }
    bool begin(int slot = 0) { CHECK(alloc[slot]->Reset()); CHECK(cl->Reset(alloc[slot], nullptr)); return true; }
    void barrier(ID3D12Resource *r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b) {
        D3D12_RESOURCE_BARRIER br = {}; br.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        br.Transition.pResource = r; br.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        br.Transition.StateBefore = a; br.Transition.StateAfter = b; cl->ResourceBarrier(1, &br);
    }
    bool removed() {
        HRESULT hr = dev->GetDeviceRemovedReason();
        if (hr != S_OK) { g_err = fmt("device removed: 0x%08lx", (unsigned long)hr); return true; }
        return false;
    }

    bool shader(const char *entry, const char *stage, D3D12_SHADER_BYTECODE &bc) {
        if (dxil) {
            for (const MpBlob *b = k_dxil; b->name; b++)
                if (!strcmp(b->name, entry)) { bc = {b->data, b->size}; return true; }
            g_err = fmt("no DXIL blob for %s", entry); return false;
        }
        ID3DBlob *b; if (!compile(entry, fmt("%s_5_0", stage).c_str(), &b)) return false;
        blobs.push_back(b); bc = {b->GetBufferPointer(), b->GetBufferSize()}; return true;
    }

    bool buffer(UINT64 size, D3D12_HEAP_TYPE ht, D3D12_RESOURCE_STATES st, ID3D12Resource **out) {
        D3D12_HEAP_PROPERTIES hp = {}; hp.Type = ht;
        D3D12_RESOURCE_DESC rd = {}; rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; rd.Width = size; rd.Height = 1;
        rd.DepthOrArraySize = 1; rd.MipLevels = 1; rd.SampleDesc.Count = 1; rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        CHECK(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, st, nullptr, __uuidof(ID3D12Resource), (void **)out));
        return true;
    }

    bool root_sig(D3D12_ROOT_SIGNATURE_FLAGS flags, ID3D12RootSignature **out) {
        D3D12_ROOT_PARAMETER rp = {}; rp.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        rp.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        D3D12_ROOT_SIGNATURE_DESC rs = {1, &rp, 0, nullptr, flags};
        Com<ID3DBlob> blob, err;
        HRESULT hr = D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err);
        if (FAILED(hr)) { g_err = fmt("D3D12SerializeRootSignature: 0x%08lx %s", (unsigned long)hr, err ? (char *)err->GetBufferPointer() : ""); return false; }
        CHECK(dev->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), __uuidof(ID3D12RootSignature), (void **)out));
        return true;
    }

    struct PsoDesc {
        ID3D12RootSignature *rs; const char *vs, *ps = nullptr, *gs = nullptr, *hs = nullptr, *ds = nullptr;
        D3D12_PRIMITIVE_TOPOLOGY_TYPE topo = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        const D3D12_STREAM_OUTPUT_DESC *so = nullptr;
    };
    bool pso(const PsoDesc &p, ID3D12PipelineState **out) {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC d = {};
        d.pRootSignature = p.rs;
        if (!shader(p.vs, "vs", d.VS)) return false;
        if (p.ps && !shader(p.ps, "ps", d.PS)) return false;
        if (p.gs && !shader(p.gs, "gs", d.GS)) return false;
        if (p.hs && !shader(p.hs, "hs", d.HS)) return false;
        if (p.ds && !shader(p.ds, "ds", d.DS)) return false;
        if (p.so) d.StreamOutput = *p.so;
        d.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        d.SampleMask = UINT_MAX;
        d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID; d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        d.RasterizerState.DepthClipEnable = TRUE;
        d.PrimitiveTopologyType = p.topo;
        if (p.ps) { d.NumRenderTargets = 1; d.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM; }
        d.SampleDesc.Count = 1;
        HRESULT hr = dev->CreateGraphicsPipelineState(&d, __uuidof(ID3D12PipelineState), (void **)out);
        if (FAILED(hr)) { g_err = fmt("CreateGraphicsPipelineState(%s%s%s%s%s): 0x%08lx", p.vs, p.gs ? "+gs" : "", p.hs ? "+hs" : "",
                                      p.ds ? "+ds" : "", p.so ? "+so" : "", (unsigned long)hr); return false; }
        return true;
    }
};

static const UINT CB_STRIDE = D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT;   // 256

static bool d3d12_present_scenes(D12 &d, const Opts &o, Com<IDXGIFactory4> &factory) {
    const int width = o.width, height = o.height;
    HWND hwnd = make_window("mpbench d3d12", width, height);
    struct WndGuard { HWND h; ~WndGuard() { DestroyWindow(h); } } wg{hwnd};

    DXGI_SWAP_CHAIN_DESC1 sd = {};
    sd.Width = width; sd.Height = height; sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM; sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; sd.BufferCount = 2; sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.Scaling = DXGI_SCALING_STRETCH;
    Com<IDXGISwapChain1> sc1; Com<IDXGISwapChain3> sc;
    CHECK(factory->CreateSwapChainForHwnd(d.queue, hwnd, &sd, nullptr, nullptr, &sc1));
    CHECK(sc1->QueryInterface(__uuidof(IDXGISwapChain3), (void **)&sc));

    D3D12_DESCRIPTOR_HEAP_DESC hd = {D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 2, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
    Com<ID3D12DescriptorHeap> rtv_heap; CHECK(d.dev->CreateDescriptorHeap(&hd, __uuidof(ID3D12DescriptorHeap), (void **)&rtv_heap));
    UINT rtv_inc = d.dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    Com<ID3D12Resource> bb[2]; D3D12_CPU_DESCRIPTOR_HANDLE rtv[2];
    for (int i = 0; i < 2; i++) {
        CHECK(sc->GetBuffer(i, __uuidof(ID3D12Resource), (void **)&bb[i]));
        rtv[i] = rtv_heap->GetCPUDescriptorHandleForHeapStart(); rtv[i].ptr += i * rtv_inc;
        d.dev->CreateRenderTargetView(bb[i], nullptr, rtv[i]);
    }

    Com<ID3D12RootSignature> rs; if (!d.root_sig(D3D12_ROOT_SIGNATURE_FLAG_NONE, &rs)) return false;
    Com<ID3D12PipelineState> pso_draw, pso_fill;
    if (!d.pso({rs, "vs_quad", "ps_flat"}, &pso_draw) || !d.pso({rs, "vs_full", "ps_heavy"}, &pso_fill)) return false;

    // Upload ring: one 256-byte CB slot per draw, one region per frame in flight. Mirrors
    // the D3D11 scene's Map(WRITE_DISCARD) per draw.
    const UINT64 ring_frame = (UINT64)DRAWS_PER_FRAME * CB_STRIDE;
    Com<ID3D12Resource> ring; if (!d.buffer(ring_frame * 2, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, &ring)) return false;
    unsigned char *ring_ptr; D3D12_RANGE nr = {0, 0}; CHECK(ring->Map(0, &nr, (void **)&ring_ptr));
    D3D12_GPU_VIRTUAL_ADDRESS ring_va = ring->GetGPUVirtualAddress();

    D3D12_VIEWPORT vp = {0, 0, (float)width, (float)height, 0, 1};
    D3D12_RECT scissor = {0, 0, width, height};
    UINT64 slot_fence[2] = {0, 0};

    auto run_scene = [&](const char *name, ID3D12PipelineState *pso, auto &&draw) -> bool {
        return frame_loop(name, o.frames, [&](int f) -> bool {
            if (f < 0) { d.flush(); return !d.removed(); }
            int slot = f & 1;
            d.wait(slot_fence[slot]);
            if (!d.begin(slot)) return false;
            UINT bbi = sc->GetCurrentBackBufferIndex();
            d.barrier(bb[bbi], D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
            d.cl->OMSetRenderTargets(1, &rtv[bbi], FALSE, nullptr);
            d.cl->RSSetViewports(1, &vp); d.cl->RSSetScissorRects(1, &scissor);
            float clear[4] = {0.1f, 0.1f, 0.15f, 1}; d.cl->ClearRenderTargetView(rtv[bbi], clear, 0, nullptr);
            d.cl->SetGraphicsRootSignature(rs); d.cl->SetPipelineState(pso);
            d.cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            draw(f, slot);
            d.barrier(bb[bbi], D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
            d.submit();
            CHECK(sc->Present(0, 0));
            slot_fence[slot] = d.signal();
            return true;
        }, g_err);
    };

    bool ok = true;
    if (want("gpu12.drawcalls") && !run_scene("gpu12.drawcalls", pso_draw, [&](int f, int slot) {
            for (int i = 0; i < DRAWS_PER_FRAME; i++) {
                UINT64 off = slot * ring_frame + (UINT64)i * CB_STRIDE;
                draw_consts(i, f, (float *)(ring_ptr + off));
                d.cl->SetGraphicsRootConstantBufferView(0, ring_va + off);
                d.cl->DrawInstanced(3, 1, 0, 0);
            }
        })) { report_error("gpu12.drawcalls", g_err); ok = false; }
    if (ok && want("gpu12.fill") && !run_scene("gpu12.fill", pso_fill, [&](int f, int slot) {
            UINT64 off = slot * ring_frame;
            float c[8] = {0, 0, 0, 0, (float)f, (float)f, 0, 1}; memcpy(ring_ptr + off, c, sizeof c);
            d.cl->SetGraphicsRootConstantBufferView(0, ring_va + off);
            d.cl->DrawInstanced(3, 1, 0, 0);
        })) { report_error("gpu12.fill", g_err); ok = false; }
    d.flush();
    return ok;
}

// Feature probes: GS, SO, tessellation. Each one: check a known result, then time it.
static void d3d12_features(D12 &d) {
    const int S = 128;   // offscreen RT size
    const UINT32 CLEAR_PX = 0xFF000000u, COLOR_PX = 0xFFFF00FFu;   // RGBA8 (0,0,0,1) and (1,0,1,1)
    const int PASSES_PER_BATCH = 20, BATCHES = 5;
    std::vector<const char *> todo;
    for (const char *n : {"gpu12.features.gs", "gpu12.features.so", "gpu12.features.tess"}) if (want(n)) todo.push_back(n);
    if (todo.empty()) return;
    auto fail_all = [&](const std::string &why) { for (auto n : todo) report(n, 0, "passes/s", "\"ok\": false, \"error\": " + jstr(why)); };

    Com<ID3D12RootSignature> rs;
    if (!d.root_sig(D3D12_ROOT_SIGNATURE_FLAG_ALLOW_STREAM_OUTPUT, &rs)) { fail_all(g_err); return; }

    // Shared resources: RT, readback, constants.
    D3D12_HEAP_PROPERTIES hp = {}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC td = {}; td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; td.Width = S; td.Height = S;
    td.DepthOrArraySize = 1; td.MipLevels = 1; td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1;
    td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_CLEAR_VALUE cv = {}; cv.Format = td.Format; cv.Color[3] = 1;
    Com<ID3D12Resource> rt, rb, consts;
    D3D12_DESCRIPTOR_HEAP_DESC hd = {D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
    Com<ID3D12DescriptorHeap> rtv_heap;
    const UINT64 RB_SIZE = 4096 * 16 + 256;
    if (FAILED(d.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_RENDER_TARGET, &cv,
                                              __uuidof(ID3D12Resource), (void **)&rt)) ||
        FAILED(d.dev->CreateDescriptorHeap(&hd, __uuidof(ID3D12DescriptorHeap), (void **)&rtv_heap)) ||
        !d.buffer(RB_SIZE, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST, &rb) ||
        !d.buffer(8 * CB_STRIDE, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, &consts)) {
        fail_all("resource creation failed " + g_err); return;
    }
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
    d.dev->CreateRenderTargetView(rt, nullptr, rtv);

    // Constant slots: 0 GS probe, 1 GS timed, 2 SO, 3 tess probe, 4 tess timed, 5 zeros (SO counter reset).
    enum { C_GS_PROBE, C_GS_TIME, C_SO, C_TESS_PROBE, C_TESS_TIME, C_ZERO };
    const int GS_PROBE_GRID = 8, GS_TIME_GRID = 256, SO_PROBE_VERTS = 4096, SO_TIME_VERTS = 1 << 20;
    // Timed tess pass: 4 draws x 64 patches (8 x 8 grid of disks), factor 16. Kept to < 64k domain
    // vertices per draw: lavapipe's tessellator is ~50x slower per vertex beyond that.
    const int TESS_PROBE_FACTOR = 16, TESS_TIME_FACTOR = 16, TESS_TIME_GRID = 8, TESS_TIME_DRAWS = 4;
    const int TESS_TIME_PATCHES = TESS_TIME_GRID * TESS_TIME_GRID;
    {
        unsigned char *p; D3D12_RANGE nr = {0, 0}; consts->Map(0, &nr, (void **)&p);
        memset(p, 0, 8 * CB_STRIDE);
        auto put = [&](int slot, float x, float y, float z = 0) { float c[8] = {x, y, z, 0, 1, 0, 1, 1}; memcpy(p + slot * CB_STRIDE, c, sizeof c); };
        put(C_GS_PROBE, (float)GS_PROBE_GRID, 0.5f / GS_PROBE_GRID);   // quad = half a cell
        put(C_GS_TIME, (float)GS_TIME_GRID, 0.5f / GS_TIME_GRID);
        put(C_SO, 0, 0); ((float *)(p + C_SO * CB_STRIDE))[4] = 42.0f;
        put(C_TESS_PROBE, (float)TESS_PROBE_FACTOR, 0.9f);
        put(C_TESS_TIME, (float)TESS_TIME_FACTOR, 0.9f, (float)TESS_TIME_GRID);   // grid of small disks
        consts->Unmap(0, nullptr);
    }
    D3D12_GPU_VIRTUAL_ADDRESS cva = consts->GetGPUVirtualAddress();
    D3D12_VIEWPORT vp = {0, 0, (float)S, (float)S, 0, 1}; D3D12_RECT sc = {0, 0, S, S};

    auto begin_pass = [&](ID3D12PipelineState *pso, int cslot, bool clear) {
        d.cl->SetGraphicsRootSignature(rs); d.cl->SetPipelineState(pso);
        d.cl->SetGraphicsRootConstantBufferView(0, cva + cslot * CB_STRIDE);
        d.cl->RSSetViewports(1, &vp); d.cl->RSSetScissorRects(1, &sc);
        d.cl->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        if (clear) { float c[4] = {0, 0, 0, 1}; d.cl->ClearRenderTargetView(rtv, c, 0, nullptr); }
    };
    std::vector<UINT32> pixels(S * S);
    auto read_rt = [&]() -> bool {
        d.barrier(rt, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION dst = {}, src = {};
        dst.pResource = rb; dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint.Footprint = {DXGI_FORMAT_R8G8B8A8_UNORM, (UINT)S, (UINT)S, 1, (UINT)S * 4};
        src.pResource = rt; src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; src.SubresourceIndex = 0;
        d.cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        d.barrier(rt, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        d.submit(); d.flush();
        if (d.removed()) return false;
        void *p; D3D12_RANGE r = {0, (SIZE_T)S * S * 4}; CHECK(rb->Map(0, &r, &p));
        memcpy(pixels.data(), p, S * S * 4); D3D12_RANGE nr = {0, 0}; rb->Unmap(0, &nr);
        return true;
    };
    auto px = [&](int x, int y) { return pixels[y * S + x]; };

    // Time PASSES_PER_BATCH x BATCHES passes (after one warm-up batch); returns ms per pass.
    auto time_passes = [&](auto &&record_pass, double &ms) -> bool {
        double t0 = 0;
        for (int b = 0; b <= BATCHES; b++) {
            if (b == 1) t0 = now_ms();
            if (!d.begin()) return false;
            for (int i = 0; i < PASSES_PER_BATCH; i++) record_pass();
            d.submit(); d.flush();
            if (d.removed()) return false;
        }
        ms = (now_ms() - t0) / (PASSES_PER_BATCH * BATCHES);
        return true;
    };
    auto finish = [&](const char *name, bool ran, bool ok, double ms, const std::string &detail) {
        if (!ran) { report(name, 0, "passes/s", "\"ok\": false, \"error\": " + jstr(g_err)); return; }
        printf("  %s: %s (%s)\n", name, ok ? "PASS" : "FAIL", detail.c_str());
        report(name, ok ? 1000.0 / ms : 0, "passes/s",
               fmt("\"ok\": %s, \"ms_per_pass\": %.4f, \"check\": %s", ok ? "true" : "false", ms, jstr(detail).c_str()));
    };

    // --- geometry shader: G x G points -> quads covering the middle half of each cell.
    if (want("gpu12.features.gs")) [&] {
        Com<ID3D12PipelineState> pso; bool ok = false; double ms = 0; std::string detail;
        bool ran = d.pso({rs, "vs_point", "ps_col", "gs_quad", nullptr, nullptr, D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT}, &pso) && [&] {
            if (!d.begin()) return false;
            begin_pass(pso, C_GS_PROBE, true);
            d.cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_POINTLIST);
            d.cl->DrawInstanced(GS_PROBE_GRID * GS_PROBE_GRID, 1, 0, 0);
            if (!read_rt()) return false;
            int cell = S / GS_PROBE_GRID, bad = 0;
            for (int j = 0; j < GS_PROBE_GRID; j++)
                for (int i = 0; i < GS_PROBE_GRID; i++) {
                    if (px(i * cell + cell / 2, j * cell + cell / 2) != COLOR_PX) bad++;
                    if (px(i * cell + 1, j * cell + 1) != CLEAR_PX) bad++;
                }
            ok = bad == 0;
            detail = fmt("%d/%d probe pixels wrong; timed: %d points -> quads", bad, 2 * GS_PROBE_GRID * GS_PROBE_GRID, GS_TIME_GRID * GS_TIME_GRID);
            return time_passes([&] {
                begin_pass(pso, C_GS_TIME, true);
                d.cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_POINTLIST);
                d.cl->DrawInstanced(GS_TIME_GRID * GS_TIME_GRID, 1, 0, 0);
            }, ms);
        }();
        finish("gpu12.features.gs", ran, ok, ms, detail);
    }();

    // --- stream output: VS writes float4(i, i/2, 1000-i, 42) per vertex; no rasterization.
    if (want("gpu12.features.so")) [&] {
        D3D12_SO_DECLARATION_ENTRY decl = {0, "TEXCOORD", 0, 0, 4, 0};
        UINT stride = 16;
        D3D12_STREAM_OUTPUT_DESC sod = {&decl, 1, &stride, 1, D3D12_SO_NO_RASTERIZED_STREAM};
        Com<ID3D12PipelineState> pso; Com<ID3D12Resource> so_buf, so_count, zero;
        bool ok = false; double ms = 0; std::string detail;
        bool ran = d.pso({rs, "vs_so", nullptr, nullptr, nullptr, nullptr, D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT, &sod}, &pso) &&
                   d.buffer((UINT64)SO_TIME_VERTS * 16, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_STREAM_OUT, &so_buf) &&
                   d.buffer(256, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST, &so_count) && [&] {
            ID3D12Resource *zr = consts;   // slot C_ZERO is zeros
            auto so_pass = [&](int verts) {
                d.cl->CopyBufferRegion(so_count, 0, zr, C_ZERO * CB_STRIDE, 8);
                d.barrier(so_count, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_STREAM_OUT);
                D3D12_STREAM_OUTPUT_BUFFER_VIEW v = {so_buf->GetGPUVirtualAddress(), (UINT64)SO_TIME_VERTS * 16, so_count->GetGPUVirtualAddress()};
                d.cl->SetGraphicsRootSignature(rs); d.cl->SetPipelineState(pso);
                d.cl->SetGraphicsRootConstantBufferView(0, cva + C_SO * CB_STRIDE);
                d.cl->RSSetViewports(1, &vp); d.cl->RSSetScissorRects(1, &sc);
                d.cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_POINTLIST);
                d.cl->SOSetTargets(0, 1, &v);
                d.cl->DrawInstanced(verts, 1, 0, 0);
                d.cl->SOSetTargets(0, 0, nullptr);
            };
            if (!d.begin()) return false;
            so_pass(SO_PROBE_VERTS);
            d.barrier(so_buf, D3D12_RESOURCE_STATE_STREAM_OUT, D3D12_RESOURCE_STATE_COPY_SOURCE);
            d.barrier(so_count, D3D12_RESOURCE_STATE_STREAM_OUT, D3D12_RESOURCE_STATE_COPY_SOURCE);
            d.cl->CopyBufferRegion(rb, 0, so_buf, 0, SO_PROBE_VERTS * 16);
            d.cl->CopyBufferRegion(rb, SO_PROBE_VERTS * 16, so_count, 0, 8);
            d.barrier(so_buf, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_STREAM_OUT);
            d.barrier(so_count, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
            d.submit(); d.flush();
            if (d.removed()) return false;
            unsigned char *p; D3D12_RANGE r = {0, (SIZE_T)RB_SIZE}; CHECK(rb->Map(0, &r, (void **)&p));
            UINT32 filled; memcpy(&filled, p + SO_PROBE_VERTS * 16, 4);
            int bad = 0;
            for (int i = 0; i < SO_PROBE_VERTS; i++) {
                float v[4]; memcpy(v, p + i * 16, 16);
                if (v[0] != (float)i || v[1] != i * 0.5f || v[2] != 1000.0f - i || v[3] != 42.0f) bad++;
            }
            D3D12_RANGE nr = {0, 0}; rb->Unmap(0, &nr);
            ok = bad == 0 && filled == SO_PROBE_VERTS * 16u;
            detail = fmt("filled_size %u (want %u), %d/%d vertices wrong; timed: %d vertices", filled,
                         SO_PROBE_VERTS * 16u, bad, SO_PROBE_VERTS, SO_TIME_VERTS);
            return time_passes([&] {
                so_pass(SO_TIME_VERTS);
                d.barrier(so_count, D3D12_RESOURCE_STATE_STREAM_OUT, D3D12_RESOURCE_STATE_COPY_DEST);
            }, ms);
        }();
        finish("gpu12.features.so", ran, ok, ms, detail);
    }();

    // --- tessellation: quad patch, uniform integer factor, domain wrapped into a disk.
    if (want("gpu12.features.tess")) [&] {
        Com<ID3D12PipelineState> pso; bool ok = false; double ms = 0; std::string detail;
        bool ran = d.pso({rs, "vs_patch", "ps_col", nullptr, "hs_main", "ds_disk", D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH}, &pso) && [&] {
            if (!d.begin()) return false;
            begin_pass(pso, C_TESS_PROBE, true);
            d.cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_4_CONTROL_POINT_PATCHLIST);
            d.cl->DrawInstanced(4, 1, 0, 0);
            if (!read_rt()) return false;
            // Disk radius 0.9 (57.6 px). Inside at r = 0.75 must be covered, corners must not.
            int bad = 0, n = 0;
            for (int k = 0; k < 8; k++, n++) {
                double a = (k * 45 + 10) * 3.14159265358979 / 180, r = 0.75 * S / 2;
                if (px((int)(S / 2 + r * cos(a)), (int)(S / 2 - r * sin(a))) != COLOR_PX) bad++;
            }
            for (int c = 0; c < 4; c++, n++) if (px(c & 1 ? S - 3 : 2, c & 2 ? S - 3 : 2) != CLEAR_PX) bad++;
            ok = bad == 0;
            detail = fmt("%d/%d probe pixels wrong; timed: %d draws x %d patches x factor %d = %d triangles", bad, n, TESS_TIME_DRAWS,
                         TESS_TIME_PATCHES, TESS_TIME_FACTOR, TESS_TIME_DRAWS * TESS_TIME_PATCHES * 2 * TESS_TIME_FACTOR * TESS_TIME_FACTOR);
            return time_passes([&] {
                begin_pass(pso, C_TESS_TIME, true);
                d.cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_4_CONTROL_POINT_PATCHLIST);
                for (int k = 0; k < TESS_TIME_DRAWS; k++) d.cl->DrawInstanced(4 * TESS_TIME_PATCHES, 1, 0, 0);
            }, ms);
        }();
        finish("gpu12.features.tess", ran, ok, ms, detail);
    }();
}

static bool d3d12_run(const Opts &o) {
    D12 d; d.dxil = o.dxil;
    Com<IDXGIFactory1> f1; Com<IDXGIFactory4> factory;
    CHECK(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void **)&f1));
    CHECK(f1->QueryInterface(__uuidof(IDXGIFactory4), (void **)&factory));
    CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), (void **)&d.dev));

    std::string aname = "?";
    Com<IDXGIAdapter1> adapter;
    if (SUCCEEDED(factory->EnumAdapterByLuid(d.dev->GetAdapterLuid(), __uuidof(IDXGIAdapter1), (void **)&adapter))) {
        DXGI_ADAPTER_DESC1 ad; if (SUCCEEDED(adapter->GetDesc1(&ad))) aname = wide_to_utf8(ad.Description);
    }
    D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_12_2, D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_0, D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    D3D12_FEATURE_DATA_FEATURE_LEVELS fls = {5, levels, D3D_FEATURE_LEVEL_11_0};
    d.dev->CheckFeatureSupport(D3D12_FEATURE_FEATURE_LEVELS, &fls, sizeof fls);
    D3D12_FEATURE_DATA_SHADER_MODEL sm = {D3D_SHADER_MODEL_6_6};
    while (FAILED(d.dev->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &sm, sizeof sm)) && sm.HighestShaderModel > D3D_SHADER_MODEL_5_1)
        sm.HighestShaderModel = (D3D_SHADER_MODEL)(sm.HighestShaderModel - 1);
    std::string hint = o.dxil && sm.HighestShaderModel < D3D_SHADER_MODEL_6_0
                           ? ", \"hint\": \"device reports SM < 6.0, DXIL pipelines will likely fail; try --d3d12-shaders dxbc\"" : "";
    report("gpu12.device", 1, "ok", fmt("\"adapter\": %s, \"feature_level\": \"0x%x\", \"shader_model\": \"%d.%d\", \"shaders\": \"%s\"%s",
                                        jstr(aname).c_str(), (unsigned)fls.MaxSupportedFeatureLevel, sm.HighestShaderModel >> 4,
                                        sm.HighestShaderModel & 15, o.dxil ? "dxil" : "dxbc", hint.c_str()));

    D3D12_COMMAND_QUEUE_DESC qd = {D3D12_COMMAND_LIST_TYPE_DIRECT, 0, D3D12_COMMAND_QUEUE_FLAG_NONE, 0};
    CHECK(d.dev->CreateCommandQueue(&qd, __uuidof(ID3D12CommandQueue), (void **)&d.queue));
    CHECK(d.dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), (void **)&d.fence));
    for (auto &a : d.alloc) CHECK(d.dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator), (void **)&a));
    CHECK(d.dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, d.alloc[0], nullptr, __uuidof(ID3D12GraphicsCommandList), (void **)&d.cl));
    d.cl->Close();

    if ((want("gpu12.drawcalls") || want("gpu12.fill")) && !d3d12_present_scenes(d, o, factory)) {
        if (!g_err.empty()) report_error("gpu12.present", g_err);
        g_err.clear();
        if (d.removed()) return false;
    }
    d3d12_features(d);
    return true;
}

// ---------------------------------------------------------------- Vulkan (via vulkan-1.dll)
//
// Minimal subset of vulkan_core.h / vulkan_win32.h, copied verbatim (struct bodies) so no
// Vulkan SDK is needed to build. Non-dispatchable handles are uint64_t on every arch.
// build.sh compiles once with -DMPBENCH_VK_CHECK against the real headers to verify layouts.

namespace mvk {
#define VKAPI_PTR __stdcall
typedef uint32_t VkFlags; typedef uint32_t VkBool32; typedef uint64_t VkDeviceSize; typedef uint32_t VkSampleMask;
typedef int32_t VkStructureType, VkResult, VkFormat, VkColorSpaceKHR, VkPresentModeKHR, VkSharingMode, VkImageViewType,
    VkComponentSwizzle, VkImageLayout, VkAttachmentLoadOp, VkAttachmentStoreOp, VkPipelineBindPoint, VkPrimitiveTopology,
    VkPolygonMode, VkFrontFace, VkBlendFactor, VkBlendOp, VkLogicOp, VkDynamicState, VkCommandBufferLevel,
    VkPhysicalDeviceType, VkSampleCountFlagBits, VkShaderStageFlagBits, VkSurfaceTransformFlagBitsKHR,
    VkCompositeAlphaFlagBitsKHR, VkSubpassContents;
typedef VkFlags VkInstanceCreateFlags, VkDeviceCreateFlags, VkDeviceQueueCreateFlags, VkQueueFlags, VkSurfaceTransformFlagsKHR,
    VkCompositeAlphaFlagsKHR, VkImageUsageFlags, VkSwapchainCreateFlagsKHR, VkImageViewCreateFlags, VkImageAspectFlags,
    VkAttachmentDescriptionFlags, VkSubpassDescriptionFlags, VkPipelineStageFlags, VkAccessFlags, VkDependencyFlags,
    VkRenderPassCreateFlags, VkFramebufferCreateFlags, VkShaderModuleCreateFlags, VkShaderStageFlags,
    VkPipelineLayoutCreateFlags, VkPipelineShaderStageCreateFlags, VkPipelineVertexInputStateCreateFlags,
    VkPipelineInputAssemblyStateCreateFlags, VkPipelineViewportStateCreateFlags, VkPipelineRasterizationStateCreateFlags,
    VkCullModeFlags, VkPipelineMultisampleStateCreateFlags, VkColorComponentFlags, VkPipelineColorBlendStateCreateFlags,
    VkPipelineDynamicStateCreateFlags, VkPipelineCreateFlags, VkCommandPoolCreateFlags, VkCommandBufferUsageFlags,
    VkFenceCreateFlags, VkSemaphoreCreateFlags, VkWin32SurfaceCreateFlagsKHR, VkCommandBufferResetFlags;
typedef struct VkInstance_T *VkInstance; typedef struct VkPhysicalDevice_T *VkPhysicalDevice; typedef struct VkDevice_T *VkDevice;
typedef struct VkQueue_T *VkQueue; typedef struct VkCommandBuffer_T *VkCommandBuffer;
typedef uint64_t VkSurfaceKHR, VkSwapchainKHR, VkImage, VkImageView, VkRenderPass, VkFramebuffer, VkShaderModule,
    VkPipelineLayout, VkPipeline, VkDescriptorSetLayout, VkCommandPool, VkFence, VkSemaphore, VkPipelineCache;
struct VkAllocationCallbacks; struct VkPhysicalDeviceFeatures; struct VkCommandBufferInheritanceInfo;
struct VkPipelineDepthStencilStateCreateInfo; struct VkPipelineTessellationStateCreateInfo;
struct VkVertexInputBindingDescription; struct VkVertexInputAttributeDescription; struct VkExtensionProperties;
typedef struct VkPhysicalDeviceLimits { uint64_t opaque[sizeof(void *) == 8 ? 63 : 62]; } VkPhysicalDeviceLimits;   // 504/496 bytes, unused
static const uint32_t VK_MAX_PHYSICAL_DEVICE_NAME_SIZE = 256u, VK_UUID_SIZE = 16u;
typedef void (VKAPI_PTR *PFN_vkVoidFunction)(void);
typedef struct VkApplicationInfo{ VkStructureType sType; const void* pNext; const char* pApplicationName; uint32_t applicationVersion; const char* pEngineName; uint32_t engineVersion; uint32_t apiVersion; } VkApplicationInfo;
typedef struct VkInstanceCreateInfo{ VkStructureType sType; const void* pNext; VkInstanceCreateFlags flags; const VkApplicationInfo* pApplicationInfo; uint32_t enabledLayerCount; const char* const* ppEnabledLayerNames; uint32_t enabledExtensionCount; const char* const* ppEnabledExtensionNames; } VkInstanceCreateInfo;
typedef struct VkDeviceQueueCreateInfo{ VkStructureType sType; const void* pNext; VkDeviceQueueCreateFlags flags; uint32_t queueFamilyIndex; uint32_t queueCount; const float* pQueuePriorities; } VkDeviceQueueCreateInfo;
typedef struct VkDeviceCreateInfo{ VkStructureType sType; const void* pNext; VkDeviceCreateFlags flags; uint32_t queueCreateInfoCount; const VkDeviceQueueCreateInfo* pQueueCreateInfos; uint32_t enabledLayerCount; const char* const* ppEnabledLayerNames; uint32_t enabledExtensionCount; const char* const* ppEnabledExtensionNames; const VkPhysicalDeviceFeatures* pEnabledFeatures; } VkDeviceCreateInfo;
typedef struct VkExtent2D{ uint32_t width; uint32_t height; } VkExtent2D;
typedef struct VkOffset2D{ int32_t x; int32_t y; } VkOffset2D;
typedef struct VkRect2D{ VkOffset2D offset; VkExtent2D extent; } VkRect2D;
typedef struct VkExtent3D{ uint32_t width; uint32_t height; uint32_t depth; } VkExtent3D;
typedef struct VkQueueFamilyProperties{ VkQueueFlags queueFlags; uint32_t queueCount; uint32_t timestampValidBits; VkExtent3D minImageTransferGranularity; } VkQueueFamilyProperties;
typedef struct VkSurfaceCapabilitiesKHR{ uint32_t minImageCount; uint32_t maxImageCount; VkExtent2D currentExtent; VkExtent2D minImageExtent; VkExtent2D maxImageExtent; uint32_t maxImageArrayLayers; VkSurfaceTransformFlagsKHR supportedTransforms; VkSurfaceTransformFlagBitsKHR currentTransform; VkCompositeAlphaFlagsKHR supportedCompositeAlpha; VkImageUsageFlags supportedUsageFlags; } VkSurfaceCapabilitiesKHR;
typedef struct VkSurfaceFormatKHR{ VkFormat format; VkColorSpaceKHR colorSpace; } VkSurfaceFormatKHR;
typedef struct VkSwapchainCreateInfoKHR{ VkStructureType sType; const void* pNext; VkSwapchainCreateFlagsKHR flags; VkSurfaceKHR surface; uint32_t minImageCount; VkFormat imageFormat; VkColorSpaceKHR imageColorSpace; VkExtent2D imageExtent; uint32_t imageArrayLayers; VkImageUsageFlags imageUsage; VkSharingMode imageSharingMode; uint32_t queueFamilyIndexCount; const uint32_t* pQueueFamilyIndices; VkSurfaceTransformFlagBitsKHR preTransform; VkCompositeAlphaFlagBitsKHR compositeAlpha; VkPresentModeKHR presentMode; VkBool32 clipped; VkSwapchainKHR oldSwapchain; } VkSwapchainCreateInfoKHR;
typedef struct VkComponentMapping{ VkComponentSwizzle r; VkComponentSwizzle g; VkComponentSwizzle b; VkComponentSwizzle a; } VkComponentMapping;
typedef struct VkImageSubresourceRange{ VkImageAspectFlags aspectMask; uint32_t baseMipLevel; uint32_t levelCount; uint32_t baseArrayLayer; uint32_t layerCount; } VkImageSubresourceRange;
typedef struct VkImageViewCreateInfo{ VkStructureType sType; const void* pNext; VkImageViewCreateFlags flags; VkImage image; VkImageViewType viewType; VkFormat format; VkComponentMapping components; VkImageSubresourceRange subresourceRange; } VkImageViewCreateInfo;
typedef struct VkAttachmentDescription{ VkAttachmentDescriptionFlags flags; VkFormat format; VkSampleCountFlagBits samples; VkAttachmentLoadOp loadOp; VkAttachmentStoreOp storeOp; VkAttachmentLoadOp stencilLoadOp; VkAttachmentStoreOp stencilStoreOp; VkImageLayout initialLayout; VkImageLayout finalLayout; } VkAttachmentDescription;
typedef struct VkAttachmentReference{ uint32_t attachment; VkImageLayout layout; } VkAttachmentReference;
typedef struct VkSubpassDescription{ VkSubpassDescriptionFlags flags; VkPipelineBindPoint pipelineBindPoint; uint32_t inputAttachmentCount; const VkAttachmentReference* pInputAttachments; uint32_t colorAttachmentCount; const VkAttachmentReference* pColorAttachments; const VkAttachmentReference* pResolveAttachments; const VkAttachmentReference* pDepthStencilAttachment; uint32_t preserveAttachmentCount; const uint32_t* pPreserveAttachments; } VkSubpassDescription;
typedef struct VkSubpassDependency{ uint32_t srcSubpass; uint32_t dstSubpass; VkPipelineStageFlags srcStageMask; VkPipelineStageFlags dstStageMask; VkAccessFlags srcAccessMask; VkAccessFlags dstAccessMask; VkDependencyFlags dependencyFlags; } VkSubpassDependency;
typedef struct VkRenderPassCreateInfo{ VkStructureType sType; const void* pNext; VkRenderPassCreateFlags flags; uint32_t attachmentCount; const VkAttachmentDescription* pAttachments; uint32_t subpassCount; const VkSubpassDescription* pSubpasses; uint32_t dependencyCount; const VkSubpassDependency* pDependencies; } VkRenderPassCreateInfo;
typedef struct VkFramebufferCreateInfo{ VkStructureType sType; const void* pNext; VkFramebufferCreateFlags flags; VkRenderPass renderPass; uint32_t attachmentCount; const VkImageView* pAttachments; uint32_t width; uint32_t height; uint32_t layers; } VkFramebufferCreateInfo;
typedef struct VkShaderModuleCreateInfo{ VkStructureType sType; const void* pNext; VkShaderModuleCreateFlags flags; size_t codeSize; const uint32_t* pCode; } VkShaderModuleCreateInfo;
typedef struct VkPushConstantRange{ VkShaderStageFlags stageFlags; uint32_t offset; uint32_t size; } VkPushConstantRange;
typedef struct VkPipelineLayoutCreateInfo{ VkStructureType sType; const void* pNext; VkPipelineLayoutCreateFlags flags; uint32_t setLayoutCount; const VkDescriptorSetLayout* pSetLayouts; uint32_t pushConstantRangeCount; const VkPushConstantRange* pPushConstantRanges; } VkPipelineLayoutCreateInfo;
typedef struct VkSpecializationMapEntry{ uint32_t constantID; uint32_t offset; size_t size; } VkSpecializationMapEntry;
typedef struct VkSpecializationInfo{ uint32_t mapEntryCount; const VkSpecializationMapEntry* pMapEntries; size_t dataSize; const void* pData; } VkSpecializationInfo;
typedef struct VkPipelineShaderStageCreateInfo{ VkStructureType sType; const void* pNext; VkPipelineShaderStageCreateFlags flags; VkShaderStageFlagBits stage; VkShaderModule module; const char* pName; const VkSpecializationInfo* pSpecializationInfo; } VkPipelineShaderStageCreateInfo;
typedef struct VkPipelineVertexInputStateCreateInfo{ VkStructureType sType; const void* pNext; VkPipelineVertexInputStateCreateFlags flags; uint32_t vertexBindingDescriptionCount; const VkVertexInputBindingDescription* pVertexBindingDescriptions; uint32_t vertexAttributeDescriptionCount; const VkVertexInputAttributeDescription* pVertexAttributeDescriptions; } VkPipelineVertexInputStateCreateInfo;
typedef struct VkPipelineInputAssemblyStateCreateInfo{ VkStructureType sType; const void* pNext; VkPipelineInputAssemblyStateCreateFlags flags; VkPrimitiveTopology topology; VkBool32 primitiveRestartEnable; } VkPipelineInputAssemblyStateCreateInfo;
typedef struct VkViewport{ float x; float y; float width; float height; float minDepth; float maxDepth; } VkViewport;
typedef struct VkPipelineViewportStateCreateInfo{ VkStructureType sType; const void* pNext; VkPipelineViewportStateCreateFlags flags; uint32_t viewportCount; const VkViewport* pViewports; uint32_t scissorCount; const VkRect2D* pScissors; } VkPipelineViewportStateCreateInfo;
typedef struct VkPipelineRasterizationStateCreateInfo{ VkStructureType sType; const void* pNext; VkPipelineRasterizationStateCreateFlags flags; VkBool32 depthClampEnable; VkBool32 rasterizerDiscardEnable; VkPolygonMode polygonMode; VkCullModeFlags cullMode; VkFrontFace frontFace; VkBool32 depthBiasEnable; float depthBiasConstantFactor; float depthBiasClamp; float depthBiasSlopeFactor; float lineWidth; } VkPipelineRasterizationStateCreateInfo;
typedef struct VkPipelineMultisampleStateCreateInfo{ VkStructureType sType; const void* pNext; VkPipelineMultisampleStateCreateFlags flags; VkSampleCountFlagBits rasterizationSamples; VkBool32 sampleShadingEnable; float minSampleShading; const VkSampleMask* pSampleMask; VkBool32 alphaToCoverageEnable; VkBool32 alphaToOneEnable; } VkPipelineMultisampleStateCreateInfo;
typedef struct VkPipelineColorBlendAttachmentState{ VkBool32 blendEnable; VkBlendFactor srcColorBlendFactor; VkBlendFactor dstColorBlendFactor; VkBlendOp colorBlendOp; VkBlendFactor srcAlphaBlendFactor; VkBlendFactor dstAlphaBlendFactor; VkBlendOp alphaBlendOp; VkColorComponentFlags colorWriteMask; } VkPipelineColorBlendAttachmentState;
typedef struct VkPipelineColorBlendStateCreateInfo{ VkStructureType sType; const void* pNext; VkPipelineColorBlendStateCreateFlags flags; VkBool32 logicOpEnable; VkLogicOp logicOp; uint32_t attachmentCount; const VkPipelineColorBlendAttachmentState* pAttachments; float blendConstants[4]; } VkPipelineColorBlendStateCreateInfo;
typedef struct VkPipelineDynamicStateCreateInfo{ VkStructureType sType; const void* pNext; VkPipelineDynamicStateCreateFlags flags; uint32_t dynamicStateCount; const VkDynamicState* pDynamicStates; } VkPipelineDynamicStateCreateInfo;
typedef struct VkGraphicsPipelineCreateInfo{ VkStructureType sType; const void* pNext; VkPipelineCreateFlags flags; uint32_t stageCount; const VkPipelineShaderStageCreateInfo* pStages; const VkPipelineVertexInputStateCreateInfo* pVertexInputState; const VkPipelineInputAssemblyStateCreateInfo* pInputAssemblyState; const VkPipelineTessellationStateCreateInfo* pTessellationState; const VkPipelineViewportStateCreateInfo* pViewportState; const VkPipelineRasterizationStateCreateInfo* pRasterizationState; const VkPipelineMultisampleStateCreateInfo* pMultisampleState; const VkPipelineDepthStencilStateCreateInfo* pDepthStencilState; const VkPipelineColorBlendStateCreateInfo* pColorBlendState; const VkPipelineDynamicStateCreateInfo* pDynamicState; VkPipelineLayout layout; VkRenderPass renderPass; uint32_t subpass; VkPipeline basePipelineHandle; int32_t basePipelineIndex; } VkGraphicsPipelineCreateInfo;
typedef struct VkCommandPoolCreateInfo{ VkStructureType sType; const void* pNext; VkCommandPoolCreateFlags flags; uint32_t queueFamilyIndex; } VkCommandPoolCreateInfo;
typedef struct VkCommandBufferAllocateInfo{ VkStructureType sType; const void* pNext; VkCommandPool commandPool; VkCommandBufferLevel level; uint32_t commandBufferCount; } VkCommandBufferAllocateInfo;
typedef struct VkCommandBufferBeginInfo{ VkStructureType sType; const void* pNext; VkCommandBufferUsageFlags flags; const VkCommandBufferInheritanceInfo* pInheritanceInfo; } VkCommandBufferBeginInfo;
typedef union VkClearColorValue{ float float32[4]; int32_t int32[4]; uint32_t uint32[4]; } VkClearColorValue;
typedef struct VkClearDepthStencilValue{ float depth; uint32_t stencil; } VkClearDepthStencilValue;
typedef union VkClearValue{ VkClearColorValue color; VkClearDepthStencilValue depthStencil; } VkClearValue;
typedef struct VkRenderPassBeginInfo{ VkStructureType sType; const void* pNext; VkRenderPass renderPass; VkFramebuffer framebuffer; VkRect2D renderArea; uint32_t clearValueCount; const VkClearValue* pClearValues; } VkRenderPassBeginInfo;
typedef struct VkSubmitInfo{ VkStructureType sType; const void* pNext; uint32_t waitSemaphoreCount; const VkSemaphore* pWaitSemaphores; const VkPipelineStageFlags* pWaitDstStageMask; uint32_t commandBufferCount; const VkCommandBuffer* pCommandBuffers; uint32_t signalSemaphoreCount; const VkSemaphore* pSignalSemaphores; } VkSubmitInfo;
typedef struct VkPresentInfoKHR{ VkStructureType sType; const void* pNext; uint32_t waitSemaphoreCount; const VkSemaphore* pWaitSemaphores; uint32_t swapchainCount; const VkSwapchainKHR* pSwapchains; const uint32_t* pImageIndices; VkResult* pResults; } VkPresentInfoKHR;
typedef struct VkFenceCreateInfo{ VkStructureType sType; const void* pNext; VkFenceCreateFlags flags; } VkFenceCreateInfo;
typedef struct VkSemaphoreCreateInfo{ VkStructureType sType; const void* pNext; VkSemaphoreCreateFlags flags; } VkSemaphoreCreateInfo;
typedef struct VkWin32SurfaceCreateInfoKHR{ VkStructureType sType; const void* pNext; VkWin32SurfaceCreateFlagsKHR flags; HINSTANCE hinstance; HWND hwnd; } VkWin32SurfaceCreateInfoKHR;
typedef struct VkPhysicalDeviceSparseProperties{ VkBool32 residencyStandard2DBlockShape; VkBool32 residencyStandard2DMultisampleBlockShape; VkBool32 residencyStandard3DBlockShape; VkBool32 residencyAlignedMipSize; VkBool32 residencyNonResidentStrict; } VkPhysicalDeviceSparseProperties;
typedef struct VkPhysicalDeviceProperties{ uint32_t apiVersion; uint32_t driverVersion; uint32_t vendorID; uint32_t deviceID; VkPhysicalDeviceType deviceType; char deviceName[VK_MAX_PHYSICAL_DEVICE_NAME_SIZE]; uint8_t pipelineCacheUUID[VK_UUID_SIZE]; VkPhysicalDeviceLimits limits; VkPhysicalDeviceSparseProperties sparseProperties; } VkPhysicalDeviceProperties;
static const int32_t VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT = 0x00000100;
static const int32_t VK_ATTACHMENT_LOAD_OP_CLEAR = 1;
static const int32_t VK_ATTACHMENT_LOAD_OP_DONT_CARE = 2;
static const int32_t VK_ATTACHMENT_STORE_OP_DONT_CARE = 1;
static const int32_t VK_ATTACHMENT_STORE_OP_STORE = 0;
static const int32_t VK_COLOR_COMPONENT_A_BIT = 0x00000008;
static const int32_t VK_COLOR_COMPONENT_B_BIT = 0x00000004;
static const int32_t VK_COLOR_COMPONENT_G_BIT = 0x00000002;
static const int32_t VK_COLOR_COMPONENT_R_BIT = 0x00000001;
static const int32_t VK_COMMAND_BUFFER_LEVEL_PRIMARY = 0;
static const int32_t VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT = 0x00000001;
static const int32_t VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT = 0x00000002;
static const int32_t VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR = 0x00000001;
static const int32_t VK_CULL_MODE_NONE = 0;
static const int32_t VK_DYNAMIC_STATE_SCISSOR = 1;
static const int32_t VK_DYNAMIC_STATE_VIEWPORT = 0;
static const int32_t VK_FENCE_CREATE_SIGNALED_BIT = 0x00000001;
static const int32_t VK_FORMAT_B8G8R8A8_UNORM = 44;
static const int32_t VK_FORMAT_R8G8B8A8_UNORM = 37;
static const int32_t VK_FRONT_FACE_CLOCKWISE = 1;
static const int32_t VK_IMAGE_ASPECT_COLOR_BIT = 0x00000001;
static const int32_t VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL = 2;
static const int32_t VK_IMAGE_LAYOUT_PRESENT_SRC_KHR = 1000001002;
static const int32_t VK_IMAGE_LAYOUT_UNDEFINED = 0;
static const int32_t VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT = 0x00000010;
static const int32_t VK_IMAGE_VIEW_TYPE_2D = 1;
#define VK_KHR_SURFACE_EXTENSION_NAME "VK_KHR_surface"
#define VK_KHR_SWAPCHAIN_EXTENSION_NAME "VK_KHR_swapchain"
#define VK_KHR_WIN32_SURFACE_EXTENSION_NAME "VK_KHR_win32_surface"
static const int32_t VK_PIPELINE_BIND_POINT_GRAPHICS = 0;
static const int32_t VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT = 0x00000400;
static const int32_t VK_POLYGON_MODE_FILL = 0;
static const int32_t VK_PRESENT_MODE_FIFO_KHR = 2;
static const int32_t VK_PRESENT_MODE_IMMEDIATE_KHR = 0;
static const int32_t VK_PRESENT_MODE_MAILBOX_KHR = 1;
static const int32_t VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST = 3;
static const int32_t VK_QUEUE_GRAPHICS_BIT = 0x00000001;
static const int32_t VK_SAMPLE_COUNT_1_BIT = 0x00000001;
static const int32_t VK_SHADER_STAGE_FRAGMENT_BIT = 0x00000010;
static const int32_t VK_SHADER_STAGE_VERTEX_BIT = 0x00000001;
static const int32_t VK_SHARING_MODE_EXCLUSIVE = 0;
static const int32_t VK_STRUCTURE_TYPE_APPLICATION_INFO = 0;
static const int32_t VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO = 40;
static const int32_t VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO = 42;
static const int32_t VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO = 39;
static const int32_t VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO = 3;
static const int32_t VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO = 2;
static const int32_t VK_STRUCTURE_TYPE_FENCE_CREATE_INFO = 8;
static const int32_t VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO = 37;
static const int32_t VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO = 28;
static const int32_t VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO = 15;
static const int32_t VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO = 1;
static const int32_t VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO = 26;
static const int32_t VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO = 27;
static const int32_t VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO = 20;
static const int32_t VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO = 30;
static const int32_t VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO = 24;
static const int32_t VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO = 23;
static const int32_t VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO = 18;
static const int32_t VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO = 19;
static const int32_t VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO = 22;
static const int32_t VK_STRUCTURE_TYPE_PRESENT_INFO_KHR = 1000001001;
static const int32_t VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO = 43;
static const int32_t VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO = 38;
static const int32_t VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO = 9;
static const int32_t VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO = 16;
static const int32_t VK_STRUCTURE_TYPE_SUBMIT_INFO = 4;
static const int32_t VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR = 1000001000;
static const int32_t VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR = 1000009000;
static const int32_t VK_SUBPASS_CONTENTS_INLINE = 0;
static const uint32_t VK_SUBPASS_EXTERNAL = ~0u;
typedef VkResult (VKAPI_PTR *PFN_vkAcquireNextImageKHR)(VkDevice device, VkSwapchainKHR swapchain, uint64_t timeout, VkSemaphore semaphore, VkFence fence, uint32_t* pImageIndex);
typedef VkResult (VKAPI_PTR *PFN_vkAllocateCommandBuffers)(VkDevice device, const VkCommandBufferAllocateInfo* pAllocateInfo, VkCommandBuffer* pCommandBuffers);
typedef VkResult (VKAPI_PTR *PFN_vkBeginCommandBuffer)(VkCommandBuffer commandBuffer, const VkCommandBufferBeginInfo* pBeginInfo);
typedef void (VKAPI_PTR *PFN_vkCmdBeginRenderPass)(VkCommandBuffer commandBuffer, const VkRenderPassBeginInfo* pRenderPassBegin, VkSubpassContents contents);
typedef void (VKAPI_PTR *PFN_vkCmdBindPipeline)(VkCommandBuffer commandBuffer, VkPipelineBindPoint pipelineBindPoint, VkPipeline pipeline);
typedef void (VKAPI_PTR *PFN_vkCmdDraw)(VkCommandBuffer commandBuffer, uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex, uint32_t firstInstance);
typedef void (VKAPI_PTR *PFN_vkCmdEndRenderPass)(VkCommandBuffer commandBuffer);
typedef void (VKAPI_PTR *PFN_vkCmdPushConstants)(VkCommandBuffer commandBuffer, VkPipelineLayout layout, VkShaderStageFlags stageFlags, uint32_t offset, uint32_t size, const void* pValues);
typedef void (VKAPI_PTR *PFN_vkCmdSetScissor)(VkCommandBuffer commandBuffer, uint32_t firstScissor, uint32_t scissorCount, const VkRect2D* pScissors);
typedef void (VKAPI_PTR *PFN_vkCmdSetViewport)(VkCommandBuffer commandBuffer, uint32_t firstViewport, uint32_t viewportCount, const VkViewport* pViewports);
typedef VkResult (VKAPI_PTR *PFN_vkCreateCommandPool)(VkDevice device, const VkCommandPoolCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkCommandPool* pCommandPool);
typedef VkResult (VKAPI_PTR *PFN_vkCreateDevice)(VkPhysicalDevice physicalDevice, const VkDeviceCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkDevice* pDevice);
typedef VkResult (VKAPI_PTR *PFN_vkCreateFence)(VkDevice device, const VkFenceCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkFence* pFence);
typedef VkResult (VKAPI_PTR *PFN_vkCreateFramebuffer)(VkDevice device, const VkFramebufferCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkFramebuffer* pFramebuffer);
typedef VkResult (VKAPI_PTR *PFN_vkCreateGraphicsPipelines)(VkDevice device, VkPipelineCache pipelineCache, uint32_t createInfoCount, const VkGraphicsPipelineCreateInfo* pCreateInfos, const VkAllocationCallbacks* pAllocator, VkPipeline* pPipelines);
typedef VkResult (VKAPI_PTR *PFN_vkCreateImageView)(VkDevice device, const VkImageViewCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkImageView* pView);
typedef VkResult (VKAPI_PTR *PFN_vkCreateInstance)(const VkInstanceCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkInstance* pInstance);
typedef VkResult (VKAPI_PTR *PFN_vkCreatePipelineLayout)(VkDevice device, const VkPipelineLayoutCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkPipelineLayout* pPipelineLayout);
typedef VkResult (VKAPI_PTR *PFN_vkCreateRenderPass)(VkDevice device, const VkRenderPassCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkRenderPass* pRenderPass);
typedef VkResult (VKAPI_PTR *PFN_vkCreateSemaphore)(VkDevice device, const VkSemaphoreCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkSemaphore* pSemaphore);
typedef VkResult (VKAPI_PTR *PFN_vkCreateShaderModule)(VkDevice device, const VkShaderModuleCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkShaderModule* pShaderModule);
typedef VkResult (VKAPI_PTR *PFN_vkCreateSwapchainKHR)(VkDevice device, const VkSwapchainCreateInfoKHR* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkSwapchainKHR* pSwapchain);
typedef VkResult (VKAPI_PTR *PFN_vkCreateWin32SurfaceKHR)(VkInstance instance, const VkWin32SurfaceCreateInfoKHR* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkSurfaceKHR* pSurface);
typedef void (VKAPI_PTR *PFN_vkDestroyCommandPool)(VkDevice device, VkCommandPool commandPool, const VkAllocationCallbacks* pAllocator);
typedef void (VKAPI_PTR *PFN_vkDestroyDevice)(VkDevice device, const VkAllocationCallbacks* pAllocator);
typedef void (VKAPI_PTR *PFN_vkDestroyFence)(VkDevice device, VkFence fence, const VkAllocationCallbacks* pAllocator);
typedef void (VKAPI_PTR *PFN_vkDestroyFramebuffer)(VkDevice device, VkFramebuffer framebuffer, const VkAllocationCallbacks* pAllocator);
typedef void (VKAPI_PTR *PFN_vkDestroyImageView)(VkDevice device, VkImageView imageView, const VkAllocationCallbacks* pAllocator);
typedef void (VKAPI_PTR *PFN_vkDestroyInstance)(VkInstance instance, const VkAllocationCallbacks* pAllocator);
typedef void (VKAPI_PTR *PFN_vkDestroyPipeline)(VkDevice device, VkPipeline pipeline, const VkAllocationCallbacks* pAllocator);
typedef void (VKAPI_PTR *PFN_vkDestroyPipelineLayout)(VkDevice device, VkPipelineLayout pipelineLayout, const VkAllocationCallbacks* pAllocator);
typedef void (VKAPI_PTR *PFN_vkDestroyRenderPass)(VkDevice device, VkRenderPass renderPass, const VkAllocationCallbacks* pAllocator);
typedef void (VKAPI_PTR *PFN_vkDestroySemaphore)(VkDevice device, VkSemaphore semaphore, const VkAllocationCallbacks* pAllocator);
typedef void (VKAPI_PTR *PFN_vkDestroyShaderModule)(VkDevice device, VkShaderModule shaderModule, const VkAllocationCallbacks* pAllocator);
typedef void (VKAPI_PTR *PFN_vkDestroySurfaceKHR)(VkInstance instance, VkSurfaceKHR surface, const VkAllocationCallbacks* pAllocator);
typedef void (VKAPI_PTR *PFN_vkDestroySwapchainKHR)(VkDevice device, VkSwapchainKHR swapchain, const VkAllocationCallbacks* pAllocator);
typedef VkResult (VKAPI_PTR *PFN_vkDeviceWaitIdle)(VkDevice device);
typedef VkResult (VKAPI_PTR *PFN_vkEndCommandBuffer)(VkCommandBuffer commandBuffer);
typedef VkResult (VKAPI_PTR *PFN_vkEnumeratePhysicalDevices)(VkInstance instance, uint32_t* pPhysicalDeviceCount, VkPhysicalDevice* pPhysicalDevices);
typedef PFN_vkVoidFunction (VKAPI_PTR *PFN_vkGetDeviceProcAddr)(VkDevice device, const char* pName);
typedef void (VKAPI_PTR *PFN_vkGetDeviceQueue)(VkDevice device, uint32_t queueFamilyIndex, uint32_t queueIndex, VkQueue* pQueue);
typedef PFN_vkVoidFunction (VKAPI_PTR *PFN_vkGetInstanceProcAddr)(VkInstance instance, const char* pName);
typedef void (VKAPI_PTR *PFN_vkGetPhysicalDeviceProperties)(VkPhysicalDevice physicalDevice, VkPhysicalDeviceProperties* pProperties);
typedef void (VKAPI_PTR *PFN_vkGetPhysicalDeviceQueueFamilyProperties)(VkPhysicalDevice physicalDevice, uint32_t* pQueueFamilyPropertyCount, VkQueueFamilyProperties* pQueueFamilyProperties);
typedef VkResult (VKAPI_PTR *PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR)(VkPhysicalDevice physicalDevice, VkSurfaceKHR surface, VkSurfaceCapabilitiesKHR* pSurfaceCapabilities);
typedef VkResult (VKAPI_PTR *PFN_vkGetPhysicalDeviceSurfaceFormatsKHR)(VkPhysicalDevice physicalDevice, VkSurfaceKHR surface, uint32_t* pSurfaceFormatCount, VkSurfaceFormatKHR* pSurfaceFormats);
typedef VkResult (VKAPI_PTR *PFN_vkGetPhysicalDeviceSurfacePresentModesKHR)(VkPhysicalDevice physicalDevice, VkSurfaceKHR surface, uint32_t* pPresentModeCount, VkPresentModeKHR* pPresentModes);
typedef VkResult (VKAPI_PTR *PFN_vkGetPhysicalDeviceSurfaceSupportKHR)(VkPhysicalDevice physicalDevice, uint32_t queueFamilyIndex, VkSurfaceKHR surface, VkBool32* pSupported);
typedef VkResult (VKAPI_PTR *PFN_vkGetSwapchainImagesKHR)(VkDevice device, VkSwapchainKHR swapchain, uint32_t* pSwapchainImageCount, VkImage* pSwapchainImages);
typedef VkResult (VKAPI_PTR *PFN_vkQueuePresentKHR)(VkQueue queue, const VkPresentInfoKHR* pPresentInfo);
typedef VkResult (VKAPI_PTR *PFN_vkQueueSubmit)(VkQueue queue, uint32_t submitCount, const VkSubmitInfo* pSubmits, VkFence fence);
typedef VkResult (VKAPI_PTR *PFN_vkResetCommandBuffer)(VkCommandBuffer commandBuffer, VkCommandBufferResetFlags flags);
typedef VkResult (VKAPI_PTR *PFN_vkResetFences)(VkDevice device, uint32_t fenceCount, const VkFence* pFences);
typedef VkResult (VKAPI_PTR *PFN_vkWaitForFences)(VkDevice device, uint32_t fenceCount, const VkFence* pFences, VkBool32 waitAll, uint64_t timeout);
} // namespace mvk

#ifdef MPBENCH_VK_CHECK
#include <vulkan/vulkan_core.h>
#include <vulkan/vulkan_win32.h>
#define VKC_SZ(T) static_assert(sizeof(mvk::T) == sizeof(::T) && alignof(mvk::T) == alignof(::T), #T);
VKC_SZ(VkApplicationInfo) VKC_SZ(VkInstanceCreateInfo) VKC_SZ(VkDeviceQueueCreateInfo) VKC_SZ(VkDeviceCreateInfo)
VKC_SZ(VkQueueFamilyProperties) VKC_SZ(VkSurfaceCapabilitiesKHR) VKC_SZ(VkSurfaceFormatKHR) VKC_SZ(VkSwapchainCreateInfoKHR)
VKC_SZ(VkImageViewCreateInfo) VKC_SZ(VkAttachmentDescription) VKC_SZ(VkSubpassDescription) VKC_SZ(VkSubpassDependency)
VKC_SZ(VkRenderPassCreateInfo) VKC_SZ(VkFramebufferCreateInfo) VKC_SZ(VkShaderModuleCreateInfo) VKC_SZ(VkPushConstantRange)
VKC_SZ(VkPipelineLayoutCreateInfo) VKC_SZ(VkPipelineShaderStageCreateInfo) VKC_SZ(VkPipelineVertexInputStateCreateInfo)
VKC_SZ(VkPipelineInputAssemblyStateCreateInfo) VKC_SZ(VkViewport) VKC_SZ(VkPipelineViewportStateCreateInfo)
VKC_SZ(VkPipelineRasterizationStateCreateInfo) VKC_SZ(VkPipelineMultisampleStateCreateInfo)
VKC_SZ(VkPipelineColorBlendAttachmentState) VKC_SZ(VkPipelineColorBlendStateCreateInfo) VKC_SZ(VkPipelineDynamicStateCreateInfo)
VKC_SZ(VkGraphicsPipelineCreateInfo) VKC_SZ(VkCommandPoolCreateInfo) VKC_SZ(VkCommandBufferAllocateInfo)
VKC_SZ(VkCommandBufferBeginInfo) VKC_SZ(VkClearValue) VKC_SZ(VkRenderPassBeginInfo) VKC_SZ(VkSubmitInfo) VKC_SZ(VkPresentInfoKHR)
VKC_SZ(VkFenceCreateInfo) VKC_SZ(VkSemaphoreCreateInfo) VKC_SZ(VkPhysicalDeviceProperties) VKC_SZ(VkPhysicalDeviceLimits)
static_assert(offsetof(mvk::VkGraphicsPipelineCreateInfo, layout) == offsetof(::VkGraphicsPipelineCreateInfo, layout), "layout");
static_assert(offsetof(mvk::VkSwapchainCreateInfoKHR, oldSwapchain) == offsetof(::VkSwapchainCreateInfoKHR, oldSwapchain), "old");
VKC_SZ(VkWin32SurfaceCreateInfoKHR)
#else   // the rest of the file is skipped in the header-check build

using namespace mvk;

#define VK_INSTANCE_FUNCS(X) X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceProperties) \
    X(vkGetPhysicalDeviceQueueFamilyProperties) X(vkCreateWin32SurfaceKHR) X(vkGetPhysicalDeviceSurfaceSupportKHR) \
    X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR) X(vkGetPhysicalDeviceSurfaceFormatsKHR) \
    X(vkGetPhysicalDeviceSurfacePresentModesKHR) X(vkCreateDevice) X(vkGetDeviceProcAddr) X(vkDestroySurfaceKHR) \
    X(vkDestroyInstance)
#define VK_DEVICE_FUNCS(X) X(vkGetDeviceQueue) X(vkCreateSwapchainKHR) X(vkDestroySwapchainKHR) X(vkGetSwapchainImagesKHR) \
    X(vkAcquireNextImageKHR) X(vkQueuePresentKHR) X(vkCreateImageView) X(vkDestroyImageView) X(vkCreateRenderPass) \
    X(vkDestroyRenderPass) X(vkCreateFramebuffer) X(vkDestroyFramebuffer) X(vkCreateShaderModule) X(vkDestroyShaderModule) \
    X(vkCreatePipelineLayout) X(vkDestroyPipelineLayout) X(vkCreateGraphicsPipelines) X(vkDestroyPipeline) \
    X(vkCreateCommandPool) X(vkDestroyCommandPool) X(vkAllocateCommandBuffers) X(vkBeginCommandBuffer) \
    X(vkEndCommandBuffer) X(vkResetCommandBuffer) X(vkCmdBeginRenderPass) X(vkCmdEndRenderPass) X(vkCmdBindPipeline) \
    X(vkCmdPushConstants) X(vkCmdDraw) X(vkCmdSetViewport) X(vkCmdSetScissor) X(vkCreateFence) X(vkDestroyFence) \
    X(vkWaitForFences) X(vkResetFences) X(vkCreateSemaphore) X(vkDestroySemaphore) X(vkQueueSubmit) X(vkDeviceWaitIdle) \
    X(vkDestroyDevice)

struct Vk {
    HMODULE lib = nullptr;
    PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr = nullptr;
#define X(f) PFN_##f f = nullptr;
    VK_INSTANCE_FUNCS(X) VK_DEVICE_FUNCS(X)
#undef X
    VkInstance inst = nullptr; VkPhysicalDevice phys = nullptr; VkDevice dev = nullptr; VkQueue queue = nullptr;
    VkSurfaceKHR surface = 0; VkSwapchainKHR swapchain = 0; VkRenderPass rp = 0; VkPipelineLayout layout = 0;
    VkCommandPool pool = 0; VkCommandBuffer cmd[2] = {};
    std::vector<VkImageView> views; std::vector<VkFramebuffer> fbs; std::vector<VkSemaphore> done;
    VkSemaphore acquired[2] = {}; VkFence fences[2] = {}; VkPipeline pipes[2] = {}; VkShaderModule mods[4] = {};
    HWND hwnd = nullptr;
    ~Vk() {
        if (dev) {
            vkDeviceWaitIdle(dev);
            for (auto p : pipes) if (p) vkDestroyPipeline(dev, p, nullptr);
            for (auto m : mods) if (m) vkDestroyShaderModule(dev, m, nullptr);
            for (int i = 0; i < 2; i++) {
                if (fences[i]) vkDestroyFence(dev, fences[i], nullptr);
                if (acquired[i]) vkDestroySemaphore(dev, acquired[i], nullptr);
            }
            for (auto s : done) vkDestroySemaphore(dev, s, nullptr);
            if (pool) vkDestroyCommandPool(dev, pool, nullptr);
            if (layout) vkDestroyPipelineLayout(dev, layout, nullptr);
            for (auto f : fbs) vkDestroyFramebuffer(dev, f, nullptr);
            for (auto v : views) vkDestroyImageView(dev, v, nullptr);
            if (rp) vkDestroyRenderPass(dev, rp, nullptr);
            if (swapchain) vkDestroySwapchainKHR(dev, swapchain, nullptr);
            vkDestroyDevice(dev, nullptr);
        }
        if (surface) vkDestroySurfaceKHR(inst, surface, nullptr);
        if (inst) vkDestroyInstance(inst, nullptr);
        if (hwnd) DestroyWindow(hwnd);
        if (lib) FreeLibrary(lib);
    }
};

#define VK_CHECK(x) do { VkResult r_ = (x); if (r_ < 0) { g_err = fmt("%s: VkResult %d", #x, (int)r_); return false; } } while (0)

static bool vk_shader(Vk &v, const char *entry, VkShaderModule &out) {
    for (const MpBlob *b = k_spirv; b->name; b++)
        if (!strcmp(b->name, entry)) {
            VkShaderModuleCreateInfo ci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, nullptr, 0, b->size, (const uint32_t *)b->data};
            VK_CHECK(v.vkCreateShaderModule(v.dev, &ci, nullptr, &out));
            return true;
        }
    g_err = fmt("no SPIR-V blob for %s", entry); return false;
}

static bool vk_pipeline(Vk &v, VkShaderModule vs, const char *vs_name, VkShaderModule fs, const char *fs_name, VkPipeline &out) {
    VkPipelineShaderStageCreateInfo st[2] = {
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, vs_name, nullptr},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs, fs_name, nullptr}};
    VkPipelineVertexInputStateCreateInfo vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, nullptr, 0, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, 0};
    VkPipelineViewportStateCreateInfo vp = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, nullptr, 0, 1, nullptr, 1, nullptr};
    VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE; rs.frontFace = VK_FRONT_FACE_CLOCKWISE; rs.lineWidth = 1;
    VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState cba = {};
    cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo cb = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1; cb.pAttachments = &cba;
    VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo ds = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, nullptr, 0, 2, dyn};
    VkGraphicsPipelineCreateInfo ci = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    ci.stageCount = 2; ci.pStages = st; ci.pVertexInputState = &vi; ci.pInputAssemblyState = &ia; ci.pViewportState = &vp;
    ci.pRasterizationState = &rs; ci.pMultisampleState = &ms; ci.pColorBlendState = &cb; ci.pDynamicState = &ds;
    ci.layout = v.layout; ci.renderPass = v.rp; ci.subpass = 0;
    VK_CHECK(v.vkCreateGraphicsPipelines(v.dev, 0, 1, &ci, nullptr, &out));
    return true;
}

static bool vk_run(const Opts &o) {
    Vk v;
    v.lib = LoadLibraryA("vulkan-1.dll");
    if (!v.lib) { g_err = "vulkan-1.dll not found (no Vulkan loader on this stack)"; return false; }
    v.vkGetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)(void *)GetProcAddress(v.lib, "vkGetInstanceProcAddr");
    if (!v.vkGetInstanceProcAddr) { g_err = "vkGetInstanceProcAddr missing"; return false; }
    auto vkCreateInstance = (PFN_vkCreateInstance)v.vkGetInstanceProcAddr(nullptr, "vkCreateInstance");

    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, nullptr, "mpbench", 2, "mpbench", 2, (1u << 22)};   // Vulkan 1.0
    const char *iext[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME};
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, nullptr, 0, &app, 0, nullptr, 2, iext};
    VK_CHECK(vkCreateInstance(&ici, nullptr, &v.inst));
#define X(f) if (!(v.f = (PFN_##f)v.vkGetInstanceProcAddr(v.inst, #f))) { g_err = "missing " #f; return false; }
    VK_INSTANCE_FUNCS(X)
#undef X

    uint32_t n = 0; VK_CHECK(v.vkEnumeratePhysicalDevices(v.inst, &n, nullptr));
    if (!n) { g_err = "no Vulkan physical devices"; return false; }
    std::vector<VkPhysicalDevice> pds(n); VK_CHECK(v.vkEnumeratePhysicalDevices(v.inst, &n, pds.data()));
    v.phys = pds[0];
    VkPhysicalDeviceProperties props; v.vkGetPhysicalDeviceProperties(v.phys, &props);

    v.hwnd = make_window("mpbench vulkan", o.width, o.height);
    VkWin32SurfaceCreateInfoKHR sci = {VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR, nullptr, 0, GetModuleHandleA(nullptr), v.hwnd};
    VK_CHECK(v.vkCreateWin32SurfaceKHR(v.inst, &sci, nullptr, &v.surface));

    uint32_t qn = 0; v.vkGetPhysicalDeviceQueueFamilyProperties(v.phys, &qn, nullptr);
    std::vector<VkQueueFamilyProperties> qf(qn); v.vkGetPhysicalDeviceQueueFamilyProperties(v.phys, &qn, qf.data());
    uint32_t qfi = UINT32_MAX;
    for (uint32_t i = 0; i < qn && qfi == UINT32_MAX; i++) {
        VkBool32 present = 0; v.vkGetPhysicalDeviceSurfaceSupportKHR(v.phys, i, v.surface, &present);
        if ((qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present) qfi = i;
    }
    if (qfi == UINT32_MAX) { g_err = "no graphics+present queue family"; return false; }

    float prio = 1;
    VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, nullptr, 0, qfi, 1, &prio};
    const char *dext[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, nullptr, 0, 1, &qci, 0, nullptr, 1, dext, nullptr};
    VK_CHECK(v.vkCreateDevice(v.phys, &dci, nullptr, &v.dev));
#define X(f) if (!(v.f = (PFN_##f)v.vkGetDeviceProcAddr(v.dev, #f))) { g_err = "missing " #f; return false; }
    VK_DEVICE_FUNCS(X)
#undef X
    v.vkGetDeviceQueue(v.dev, qfi, 0, &v.queue);

    // Swapchain: prefer an uncapped present mode, like Present(0, 0) in the D3D scenes.
    VkSurfaceCapabilitiesKHR caps; VK_CHECK(v.vkGetPhysicalDeviceSurfaceCapabilitiesKHR(v.phys, v.surface, &caps));
    uint32_t fn = 0; VK_CHECK(v.vkGetPhysicalDeviceSurfaceFormatsKHR(v.phys, v.surface, &fn, nullptr));
    std::vector<VkSurfaceFormatKHR> fmts(fn); VK_CHECK(v.vkGetPhysicalDeviceSurfaceFormatsKHR(v.phys, v.surface, &fn, fmts.data()));
    VkSurfaceFormatKHR sf = fmts[0];
    for (auto &f : fmts) if (f.format == VK_FORMAT_B8G8R8A8_UNORM || f.format == VK_FORMAT_R8G8B8A8_UNORM) { sf = f; break; }
    uint32_t pn = 0; VK_CHECK(v.vkGetPhysicalDeviceSurfacePresentModesKHR(v.phys, v.surface, &pn, nullptr));
    std::vector<VkPresentModeKHR> pms(pn); VK_CHECK(v.vkGetPhysicalDeviceSurfacePresentModesKHR(v.phys, v.surface, &pn, pms.data()));
    VkPresentModeKHR pm = VK_PRESENT_MODE_FIFO_KHR;
    for (VkPresentModeKHR want_pm : {VK_PRESENT_MODE_IMMEDIATE_KHR, VK_PRESENT_MODE_MAILBOX_KHR})
        if (pm == VK_PRESENT_MODE_FIFO_KHR && std::find(pms.begin(), pms.end(), want_pm) != pms.end()) pm = want_pm;
    VkExtent2D ext = caps.currentExtent;
    if (ext.width == UINT32_MAX) ext = {(uint32_t)o.width, (uint32_t)o.height};
    uint32_t nimg = std::max(caps.minImageCount, 2u);
    if (caps.maxImageCount && nimg > caps.maxImageCount) nimg = caps.maxImageCount;
    VkSwapchainCreateInfoKHR swci = {VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    swci.surface = v.surface; swci.minImageCount = nimg; swci.imageFormat = sf.format; swci.imageColorSpace = sf.colorSpace;
    swci.imageExtent = ext; swci.imageArrayLayers = 1; swci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    swci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE; swci.preTransform = caps.currentTransform;
    swci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR; swci.presentMode = pm; swci.clipped = 1;
    VK_CHECK(v.vkCreateSwapchainKHR(v.dev, &swci, nullptr, &v.swapchain));
    uint32_t in = 0; VK_CHECK(v.vkGetSwapchainImagesKHR(v.dev, v.swapchain, &in, nullptr));
    std::vector<VkImage> imgs(in); VK_CHECK(v.vkGetSwapchainImagesKHR(v.dev, v.swapchain, &in, imgs.data()));

    const char *pm_name = pm == VK_PRESENT_MODE_IMMEDIATE_KHR ? "immediate" : pm == VK_PRESENT_MODE_MAILBOX_KHR ? "mailbox" : "fifo";
    report("vk.device", 1, "ok", fmt("\"adapter\": %s, \"api_version\": \"%u.%u.%u\", \"driver_version\": \"0x%x\", "
                                     "\"vendor_id\": \"0x%04x\", \"present_mode\": \"%s\", \"swapchain_images\": %u",
                                     jstr(props.deviceName).c_str(), props.apiVersion >> 22, (props.apiVersion >> 12) & 0x3ff,
                                     props.apiVersion & 0xfff, props.driverVersion, props.vendorID, pm_name, in));

    VkAttachmentDescription att = {0, sf.format, VK_SAMPLE_COUNT_1_BIT, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE,
                                   VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE,
                                   VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR};
    VkAttachmentReference aref = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub = {0, VK_PIPELINE_BIND_POINT_GRAPHICS, 0, nullptr, 1, &aref, nullptr, nullptr, 0, nullptr};
    VkSubpassDependency dep = {VK_SUBPASS_EXTERNAL, 0, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                               VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, 0};
    VkRenderPassCreateInfo rpci = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, nullptr, 0, 1, &att, 1, &sub, 1, &dep};
    VK_CHECK(v.vkCreateRenderPass(v.dev, &rpci, nullptr, &v.rp));
    for (VkImage img : imgs) {
        VkImageViewCreateInfo vci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vci.image = img; vci.viewType = VK_IMAGE_VIEW_TYPE_2D; vci.format = sf.format;
        vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkImageView view; VK_CHECK(v.vkCreateImageView(v.dev, &vci, nullptr, &view)); v.views.push_back(view);
        VkFramebufferCreateInfo fci = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, nullptr, 0, v.rp, 1, &view, ext.width, ext.height, 1};
        VkFramebuffer fb; VK_CHECK(v.vkCreateFramebuffer(v.dev, &fci, nullptr, &fb)); v.fbs.push_back(fb);
        VkSemaphoreCreateInfo sci2 = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VkSemaphore s; VK_CHECK(v.vkCreateSemaphore(v.dev, &sci2, nullptr, &s)); v.done.push_back(s);
    }

    VkPushConstantRange pcr = {VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 32};
    VkPipelineLayoutCreateInfo plci = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, nullptr, 0, 0, nullptr, 1, &pcr};
    VK_CHECK(v.vkCreatePipelineLayout(v.dev, &plci, nullptr, &v.layout));
    if (!vk_shader(v, "vs_quad", v.mods[0]) || !vk_shader(v, "ps_flat", v.mods[1]) ||
        !vk_shader(v, "vs_full", v.mods[2]) || !vk_shader(v, "ps_heavy", v.mods[3])) return false;
    if (!vk_pipeline(v, v.mods[0], "vs_quad", v.mods[1], "ps_flat", v.pipes[0]) ||
        !vk_pipeline(v, v.mods[2], "vs_full", v.mods[3], "ps_heavy", v.pipes[1])) return false;

    VkCommandPoolCreateInfo cpci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, nullptr, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, qfi};
    VK_CHECK(v.vkCreateCommandPool(v.dev, &cpci, nullptr, &v.pool));
    VkCommandBufferAllocateInfo cbai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, nullptr, v.pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 2};
    VK_CHECK(v.vkAllocateCommandBuffers(v.dev, &cbai, v.cmd));
    for (int i = 0; i < 2; i++) {
        VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, nullptr, VK_FENCE_CREATE_SIGNALED_BIT};
        VK_CHECK(v.vkCreateFence(v.dev, &fci, nullptr, &v.fences[i]));
        VkSemaphoreCreateInfo sci2 = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VK_CHECK(v.vkCreateSemaphore(v.dev, &sci2, nullptr, &v.acquired[i]));
    }

    VkViewport vp = {0, 0, (float)ext.width, (float)ext.height, 0, 1};
    VkRect2D scissor = {{0, 0}, ext};
    auto run_scene = [&](const char *name, VkPipeline pipe, auto &&draw) -> bool {
        return frame_loop(name, o.frames, [&](int f) -> bool {
            if (f < 0) { VK_CHECK(v.vkDeviceWaitIdle(v.dev)); return true; }
            int slot = f & 1; VkCommandBuffer cb = v.cmd[slot];
            VK_CHECK(v.vkWaitForFences(v.dev, 1, &v.fences[slot], 1, UINT64_MAX));
            uint32_t idx;
            VK_CHECK(v.vkAcquireNextImageKHR(v.dev, v.swapchain, UINT64_MAX, v.acquired[slot], 0, &idx));
            VK_CHECK(v.vkResetFences(v.dev, 1, &v.fences[slot]));
            VK_CHECK(v.vkResetCommandBuffer(cb, 0));
            VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, nullptr, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
            VK_CHECK(v.vkBeginCommandBuffer(cb, &bi));
            VkClearValue clear = {}; clear.color.float32[0] = 0.1f; clear.color.float32[1] = 0.1f; clear.color.float32[2] = 0.15f; clear.color.float32[3] = 1;
            VkRenderPassBeginInfo rbi = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, nullptr, v.rp, v.fbs[idx], scissor, 1, &clear};
            v.vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
            v.vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
            v.vkCmdSetViewport(cb, 0, 1, &vp); v.vkCmdSetScissor(cb, 0, 1, &scissor);
            draw(cb, f);
            v.vkCmdEndRenderPass(cb);
            VK_CHECK(v.vkEndCommandBuffer(cb));
            VkPipelineStageFlags ws = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
            VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, nullptr, 1, &v.acquired[slot], &ws, 1, &cb, 1, &v.done[idx]};
            VK_CHECK(v.vkQueueSubmit(v.queue, 1, &si, v.fences[slot]));
            VkPresentInfoKHR pi = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, nullptr, 1, &v.done[idx], 1, &v.swapchain, &idx, nullptr};
            VK_CHECK(v.vkQueuePresentKHR(v.queue, &pi));
            return true;
        }, g_err);
    };
    const VkShaderStageFlags pcs = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    bool ok = true;
    if (want("vk.drawcalls") && !run_scene("vk.drawcalls", v.pipes[0], [&](VkCommandBuffer cb, int f) {
            for (int i = 0; i < DRAWS_PER_FRAME; i++) {
                float d[8]; draw_consts(i, f, d);
                v.vkCmdPushConstants(cb, v.layout, pcs, 0, 32, d);
                v.vkCmdDraw(cb, 3, 1, 0, 0);
            }
        })) { report_error("vk.drawcalls", g_err); ok = false; }
    if (ok && want("vk.fill") && !run_scene("vk.fill", v.pipes[1], [&](VkCommandBuffer cb, int f) {
            float d[8] = {0, 0, 0, 0, (float)f, (float)f, 0, 1};
            v.vkCmdPushConstants(cb, v.layout, pcs, 0, 32, d);
            v.vkCmdDraw(cb, 3, 1, 0, 0);
        })) report_error("vk.fill", g_err);
    return true;
}

// ---------------------------------------------------------------- main

static std::string runtime_info() {
    std::string s;
    HMODULE ntdll = GetModuleHandleA("ntdll.dll");
    auto wine_ver = (const char *(CDECL *)(void))(void *)GetProcAddress(ntdll, "wine_get_version");
    auto wine_host = (void (CDECL *)(const char **, const char **))(void *)GetProcAddress(ntdll, "wine_get_host_version");
    if (wine_ver) s += fmt(", \"wine\": %s", jstr(wine_ver()).c_str());
    if (wine_host) { const char *sys = "", *rel = ""; wine_host(&sys, &rel); s += fmt(", \"host\": %s", jstr(std::string(sys) + " " + rel).c_str()); }
    SYSTEM_INFO si; GetNativeSystemInfo(&si);
    s += fmt(", \"cpus\": %lu", (unsigned long)si.dwNumberOfProcessors);
    return s;
}

static void write_json(const char *path, const Opts &o, const std::string &note, const std::string &args) {
    FILE *f = fopen(path, "w"); if (!f) { printf("cannot write %s\n", path); return; }
#if defined(__x86_64__) && defined(__arm64ec__)
    const char *arch = "arm64ec";
#elif defined(__x86_64__)
    const char *arch = "x86_64";
#elif defined(__i386__)
    const char *arch = "i386";
#elif defined(__aarch64__)
    const char *arch = "aarch64";
#else
    const char *arch = "unknown";
#endif
    fprintf(f, "{\n  \"bench\": \"mpbench\", \"version\": 2, \"pe_arch\": \"%s\"%s,\n", arch, runtime_info().c_str());
    fprintf(f, "  \"frames\": %d, \"width\": %d, \"height\": %d, \"d3d12_shaders\": \"%s\",\n", o.frames, o.width, o.height, o.dxil ? "dxil" : "dxbc");
    fprintf(f, "  \"note\": %s,\n  \"args\": %s,\n  \"results\": [\n", jstr(note).c_str(), jstr(args).c_str());
    for (size_t i = 0; i < g_results.size(); i++) {
        auto &r = g_results[i];
        fprintf(f, "    {\"name\": \"%s\", \"value\": %.4f, \"unit\": \"%s\"%s%s}%s\n", r.name.c_str(), r.value, r.unit.c_str(),
                r.extra.empty() ? "" : ", ", r.extra.c_str(), i + 1 < g_results.size() ? "," : "");
    }
    fprintf(f, "  ]\n}\n"); fclose(f);
}

static int usage(int rc) {
    printf("usage: mpbench [--out FILE] [--frames N] [--width W] [--height H] [--api d3d11|d3d12|vk|all[,...]]\n"
           "               [--scenes NAME|PREFIX[,...]] [--d3d12-shaders dxil|dxbc] [--note TEXT] [--no-gpu] [--no-cpu] [--list]\nscenes:\n");
    for (auto s : k_scenes) printf("  %s\n", s);
    return rc;
}

int main(int argc, char **argv) {
    const char *out = "mpbench-results.json"; Opts o; std::string note, args;
    for (int i = 1; i < argc; i++) {   // recorded in the JSON, minus the note text and output path
        if ((!strcmp(argv[i], "--note") || !strcmp(argv[i], "--out")) && i + 1 < argc) { i++; continue; }
        if (!args.empty()) args += ' ';
        args += argv[i];
    }
    for (int i = 1; i < argc; i++) {
        auto arg = [&](const char *name) { return !strcmp(argv[i], name) && i + 1 < argc; };
        if (arg("--out")) out = argv[++i];
        else if (arg("--frames")) o.frames = std::max(1, atoi(argv[++i]));
        else if (arg("--width")) o.width = atoi(argv[++i]);
        else if (arg("--height")) o.height = atoi(argv[++i]);
        else if (arg("--note")) note = argv[++i];
        else if (arg("--scenes")) g_filters = split(argv[++i]);
        else if (arg("--d3d12-shaders")) {
            std::string s = argv[++i];
            if (s == "dxil") o.dxil = true; else if (s == "dxbc") o.dxil = false;
            else { printf("bad --d3d12-shaders %s\n", s.c_str()); return usage(2); }
        } else if (arg("--api")) {
            g_apis = 0;
            for (auto &a : split(argv[++i])) {
                if (a == "d3d11") g_apis |= API_D3D11; else if (a == "d3d12") g_apis |= API_D3D12;
                else if (a == "vk" || a == "vulkan") g_apis |= API_VK; else if (a == "all") g_apis |= API_ALL;
                else { printf("bad --api %s\n", a.c_str()); return usage(2); }
            }
        }
        else if (!strcmp(argv[i], "--no-gpu")) g_apis = 0;
        else if (!strcmp(argv[i], "--no-cpu")) g_cpu = false;
        else if (!strcmp(argv[i], "--list")) return usage(0);
        else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) return usage(0);
        else { printf("unknown argument %s\n", argv[i]); return usage(2); }
    }
    for (auto &f : g_filters) {
        bool any = f == "all";
        for (auto s : k_scenes) any |= !strncmp(s, f.c_str(), f.size());
        if (!any) { printf("--scenes: '%s' matches no scene\n", f.c_str()); return usage(2); }
    }

    if (want("cpu.scalar")) cpu_scalar();
    if (want("cpu.vector")) cpu_vector();
    if (want("cpu.memcpy")) cpu_memcpy();
    if (want("cpu.atomic")) cpu_atomic();
    if (want("cpu.pingpong")) cpu_pingpong();
    auto any = [](std::initializer_list<const char *> l) { for (auto s : l) if (want(s)) return true; return false; };
    if (any({"gpu11.drawcalls", "gpu11.fill"})) { g_err.clear(); if (!d3d11_run(o)) report_error("gpu11.device", g_err); }
    if (any({"gpu12.drawcalls", "gpu12.fill", "gpu12.features.gs", "gpu12.features.so", "gpu12.features.tess"})) {
        g_err.clear(); if (!d3d12_run(o)) report_error("gpu12.device", g_err);
    }
    if (any({"vk.drawcalls", "vk.fill"})) { g_err.clear(); if (!vk_run(o)) report_error("vk.device", g_err); }
    write_json(out, o, note, args);
    return g_errors_total ? 1 : 0;
}
#endif  // MPBENCH_VK_CHECK
