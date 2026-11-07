// mpbench: tiny deterministic Windows benchmark for comparing translation stacks
// (Rosetta vs FEX, wined3d vs DXVK vs DXMT vs D3DMetal, ...).
//
// Usage: mpbench.exe [--out results.json] [--frames N] [--width W] [--height H]
//                    [--no-gpu] [--no-cpu]
//
// Scenes:
//   cpu.scalar    double-precision mandelbrot, single thread       (FP codegen)
//   cpu.vector    float SAXPY-ish loop the compiler vectorises      (SSE/NEON codegen)
//   cpu.memcpy    64 MiB memcpy bandwidth                           (memory)
//   cpu.atomic    4 threads hammering InterlockedIncrement          (locked RMW)
//   cpu.pingpong  2 threads passing a token via plain volatile ops  (TSO-sensitive)
//   gpu.drawcalls 4000 draws/frame, each with a Map/Unmap of a CB   (API translation overhead)
//   gpu.fill      fullscreen ALU-heavy pixel shader                 (shader translation / GPU)

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

static double now_ms() {
    static LARGE_INTEGER freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f; }();
    LARGE_INTEGER t; QueryPerformanceCounter(&t);
    return (double)t.QuadPart * 1000.0 / (double)freq.QuadPart;
}

struct Result { std::string name, unit; double value; std::string extra; };
static std::vector<Result> g_results;

static void report(const char *name, double value, const char *unit, std::string extra = "") {
    printf("%-14s %12.2f %s %s\n", name, value, unit, extra.c_str());
    fflush(stdout);
    g_results.push_back({name, unit, value, extra});
}

static std::string frame_stats(std::vector<double> &ft) {
    std::sort(ft.begin(), ft.end());
    auto pct = [&](double p) { return ft[(size_t)std::min<double>(ft.size() - 1, p * ft.size())]; };
    char buf[160];
    snprintf(buf, sizeof buf, "\"p50_ms\": %.3f, \"p99_ms\": %.3f, \"max_ms\": %.3f", pct(0.5), pct(0.99), ft.back());
    return buf;
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
    char extra[64]; snprintf(extra, sizeof extra, "\"ordering_errors\": %d", (int)g_errors);
    report("cpu.pingpong", PP_ROUNDS / dt, "Krt/s", extra);
}

// ---------------------------------------------------------------- GPU scenes

static const char *k_shaders = R"(
cbuffer PerDraw : register(b0) { float4 xform; float4 color; };
struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; float4 col : COLOR0; };

VSOut vs_quad(uint id : SV_VertexID) {
    float2 c = float2((id << 1) & 2, id & 2);
    VSOut o; o.uv = c; o.pos = float4(c * xform.zw + xform.xy, 0, 1); o.col = color; return o;
}
float4 ps_flat(VSOut i) : SV_Target { return i.col; }

VSOut vs_full(uint id : SV_VertexID) {
    float2 c = float2((id << 1) & 2, id & 2);
    VSOut o; o.uv = c; o.pos = float4(c * float2(2, -2) + float2(-1, 1), 0, 1); o.col = color; return o;
}
float4 ps_heavy(VSOut i) : SV_Target {
    float2 z = float2(0, 0); float2 c = i.uv * float2(1.5, 1.0) - float2(1.2, 0.5) + color.xy * 0.001;
    [loop] for (int k = 0; k < 96; k++) z = float2(z.x * z.x - z.y * z.y, 2 * z.x * z.y) + c;
    return float4(frac(z * 0.1), 0.5, 1);
}
)";

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_CLOSE) { PostQuitMessage(0); return 0; }
    return DefWindowProcA(h, m, w, l);
}

static bool pump() { MSG msg; while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) { if (msg.message == WM_QUIT) return false; TranslateMessage(&msg); DispatchMessageA(&msg); } return true; }

#define CHECK(x) do { HRESULT hr_ = (x); if (FAILED(hr_)) { printf("FAILED %s: 0x%08lx\n", #x, (unsigned long)hr_); return false; } } while (0)

static bool compile(const char *entry, const char *target, ID3DBlob **out) {
    ID3DBlob *err = nullptr;
    HRESULT hr = D3DCompile(k_shaders, strlen(k_shaders), "mpbench", nullptr, nullptr, entry, target, 0, 0, out, &err);
    if (FAILED(hr)) { printf("shader %s: %s\n", entry, err ? (char *)err->GetBufferPointer() : "?"); return false; }
    return true;
}

static bool gpu_bench(int frames, int width, int height) {
    WNDCLASSA wc = {}; wc.lpfnWndProc = wndproc; wc.hInstance = GetModuleHandleA(nullptr); wc.lpszClassName = "mpbench";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW); RegisterClassA(&wc);
    RECT r = {0, 0, width, height}; AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = CreateWindowA("mpbench", "mpbench", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 100, 100,
                              r.right - r.left, r.bottom - r.top, nullptr, nullptr, wc.hInstance, nullptr);

    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 2; sd.BufferDesc.Width = width; sd.BufferDesc.Height = height;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd; sd.SampleDesc.Count = 1; sd.Windowed = TRUE; sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

    ID3D11Device *dev; ID3D11DeviceContext *ctx; IDXGISwapChain *sc; D3D_FEATURE_LEVEL fl;
    CHECK(D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
                                        &sd, &sc, &dev, &fl, &ctx));

    // Adapter name tells us which translation layer/driver we landed on.
    IDXGIDevice *dxgidev; IDXGIAdapter *adapter; DXGI_ADAPTER_DESC ad;
    CHECK(dev->QueryInterface(__uuidof(IDXGIDevice), (void **)&dxgidev)); CHECK(dxgidev->GetAdapter(&adapter)); CHECK(adapter->GetDesc(&ad));
    char aname[128]; WideCharToMultiByte(CP_UTF8, 0, ad.Description, -1, aname, sizeof aname, nullptr, nullptr);
    printf("adapter: %s  feature level: 0x%x\n", aname, (unsigned)fl);
    char ainfo[256]; snprintf(ainfo, sizeof ainfo, "\"adapter\": \"%s\", \"feature_level\": \"0x%x\"", aname, (unsigned)fl);
    if (fl < D3D_FEATURE_LEVEL_11_0) {
        // e.g. stock Wine on macOS: wined3d over Apple's GL 4.1 tops out at 9_3.
        report("gpu.device", 0, "fl<11_0", ainfo);
        DestroyWindow(hwnd);
        return true;
    }
    report("gpu.device", 1, "ok", ainfo);

    ID3D11Texture2D *bb; ID3D11RenderTargetView *rtv;
    CHECK(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void **)&bb)); CHECK(dev->CreateRenderTargetView(bb, nullptr, &rtv));

    ID3DBlob *b_vsq, *b_vsf, *b_psf, *b_psh;
    if (!compile("vs_quad", "vs_5_0", &b_vsq) || !compile("vs_full", "vs_5_0", &b_vsf) ||
        !compile("ps_flat", "ps_5_0", &b_psf) || !compile("ps_heavy", "ps_5_0", &b_psh)) return false;
    ID3D11VertexShader *vsq, *vsf; ID3D11PixelShader *psf, *psh;
    CHECK(dev->CreateVertexShader(b_vsq->GetBufferPointer(), b_vsq->GetBufferSize(), nullptr, &vsq));
    CHECK(dev->CreateVertexShader(b_vsf->GetBufferPointer(), b_vsf->GetBufferSize(), nullptr, &vsf));
    CHECK(dev->CreatePixelShader(b_psf->GetBufferPointer(), b_psf->GetBufferSize(), nullptr, &psf));
    CHECK(dev->CreatePixelShader(b_psh->GetBufferPointer(), b_psh->GetBufferSize(), nullptr, &psh));

    D3D11_BUFFER_DESC cbd = {}; cbd.ByteWidth = 32; cbd.Usage = D3D11_USAGE_DYNAMIC;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER; cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ID3D11Buffer *cb; CHECK(dev->CreateBuffer(&cbd, nullptr, &cb));

    D3D11_QUERY_DESC qd = {D3D11_QUERY_EVENT, 0}; ID3D11Query *q[3];
    for (auto &qq : q) CHECK(dev->CreateQuery(&qd, &qq));

    D3D11_VIEWPORT vp = {0, 0, (float)width, (float)height, 0, 1};
    auto set_cb = [&](float x, float y, float sx, float sy, float r_, float g, float b) {
        D3D11_MAPPED_SUBRESOURCE m; ctx->Map(cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m);
        float d[8] = {x, y, sx, sy, r_, g, b, 1}; memcpy(m.pData, d, sizeof d); ctx->Unmap(cb, 0);
    };

    auto run_scene = [&](const char *name, auto &&draw) -> bool {
        std::vector<double> ft; ft.reserve(frames);
        int warm = 30;
        double start = 0;
        for (int f = 0; f < frames + warm; f++) {
            if (!pump()) return false;
            double t0 = now_ms();
            if (f == warm) start = t0;
            ctx->OMSetRenderTargets(1, &rtv, nullptr); ctx->RSSetViewports(1, &vp);
            ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            ctx->VSSetConstantBuffers(0, 1, &cb); ctx->PSSetConstantBuffers(0, 1, &cb);
            float clear[4] = {0.1f, 0.1f, 0.15f, 1}; ctx->ClearRenderTargetView(rtv, clear);
            draw(f);
            ctx->End(q[f % 3]);
            sc->Present(0, 0);
            // Keep at most 2 frames in flight so CPU frame times reflect GPU work.
            if (f >= 2) while (ctx->GetData(q[(f - 2) % 3], nullptr, 0, 0) == S_FALSE) YieldProcessor();
            if (f >= warm) ft.push_back(now_ms() - t0);
        }
        ctx->End(q[0]); while (ctx->GetData(q[0], nullptr, 0, 0) == S_FALSE) YieldProcessor();
        double total = now_ms() - start;
        report(name, frames * 1000.0 / total, "fps", frame_stats(ft));
        return true;
    };

    bool ok = run_scene("gpu.drawcalls", [&](int f) {
        ctx->VSSetShader(vsq, nullptr, 0); ctx->PSSetShader(psf, nullptr, 0);
        for (int i = 0; i < 4000; i++) {
            float x = -1.0f + (i % 80) * 0.025f, y = 1.0f - (i / 80) * 0.04f;
            set_cb(x, y, 0.01f, -0.015f, (i % 7) / 7.0f, ((i + f) % 5) / 5.0f, (i % 3) / 3.0f);
            ctx->Draw(3, 0);
        }
    }) && run_scene("gpu.fill", [&](int f) {
        set_cb(0, 0, 0, 0, (float)f, (float)f, 0);
        ctx->VSSetShader(vsf, nullptr, 0); ctx->PSSetShader(psh, nullptr, 0);
        ctx->Draw(3, 0);
    });

    DestroyWindow(hwnd);
    return ok;
}

// ---------------------------------------------------------------- main

static void write_json(const char *path, int frames, int w, int h) {
    FILE *f = fopen(path, "w"); if (!f) { printf("cannot write %s\n", path); return; }
#if defined(__x86_64__)
    const char *arch = "x86_64";
#elif defined(__i386__)
    const char *arch = "i386";
#elif defined(__aarch64__)
    const char *arch = "aarch64";
#else
    const char *arch = "unknown";
#endif
    fprintf(f, "{\n  \"bench\": \"mpbench\", \"version\": 1, \"pe_arch\": \"%s\",\n", arch);
    fprintf(f, "  \"frames\": %d, \"width\": %d, \"height\": %d,\n  \"results\": [\n", frames, w, h);
    for (size_t i = 0; i < g_results.size(); i++) {
        auto &r = g_results[i];
        fprintf(f, "    {\"name\": \"%s\", \"value\": %.4f, \"unit\": \"%s\"%s%s}%s\n", r.name.c_str(), r.value, r.unit.c_str(),
                r.extra.empty() ? "" : ", ", r.extra.c_str(), i + 1 < g_results.size() ? "," : "");
    }
    fprintf(f, "  ]\n}\n"); fclose(f);
}

int main(int argc, char **argv) {
    const char *out = "mpbench-results.json"; int frames = 600, w = 1280, h = 720; bool gpu = true, cpu = true;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--out") && i + 1 < argc) out = argv[++i];
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--width") && i + 1 < argc) w = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--height") && i + 1 < argc) h = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--no-gpu")) gpu = false;
        else if (!strcmp(argv[i], "--no-cpu")) cpu = false;
    }
    if (cpu) { cpu_scalar(); cpu_vector(); cpu_memcpy(); cpu_atomic(); cpu_pingpong(); }
    bool ok = !gpu || gpu_bench(frames, w, h);
    write_json(out, frames, w, h);
    return ok ? 0 : 1;
}
