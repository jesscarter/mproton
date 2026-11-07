// gfx.h: helpers shared by the graphics programs (d3d9, d3d11, d3d12, opengl, vulkan).
// All targets are 64x64 RGBA8 (bytes R, G, B, A in memory), and every check is exact: the
// colours are multiples of 1/255 (51/255 = 0.2 and so on), so any correct float -> UNORM
// conversion gives the same byte.
#pragma once
#include "ladder.h"
#include <initializer_list>

static const int W = 64, H = 64;

struct RGBA { uint8_t r, g, b, a; };
static inline bool same(RGBA x, RGBA y) { return x.r == y.r && x.g == y.g && x.b == y.b && x.a == y.a; }

// Compare a readback against expect(x, y). bgra: the data is B, G, R, A (D3D9 X8R8G8B8 /
// A8R8G8B8 surfaces). Reports one PASS or FAIL line named `name`.
template <class F>
static bool check_pixels(const char *name, const void *data, size_t pitch, F expect, bool bgra = false, const char *what = "") {
    int bad = 0, fx = -1, fy = -1;
    RGBA got0 = {}, want0 = {};
    for (int y = 0; y < H; y++) {
        const uint8_t *row = (const uint8_t *)data + y * pitch;
        for (int x = 0; x < W; x++) {
            const uint8_t *p = row + x * 4;
            RGBA got = bgra ? RGBA{p[2], p[1], p[0], p[3]} : RGBA{p[0], p[1], p[2], p[3]};
            RGBA want_ = expect(x, y);
            if (!same(got, want_)) {
                if (!bad) { fx = x; fy = y; got0 = got; want0 = want_; }
                bad++;
            }
        }
    }
    if (!bad) return check(true, name, "%dx%d pixels exact%s%s", W, H, *what ? ", " : "", what);
    return check(false, name, "%d of %d pixels wrong%s%s; first at (%d,%d): got %d,%d,%d,%d want %d,%d,%d,%d", bad, W * H,
                 *what ? ", " : "", what, fx, fy, got0.r, got0.g, got0.b, got0.a, want0.r, want0.g, want0.b, want0.a);
}

// The reference images.
static const RGBA k_clear = {51, 102, 153, 255};
static const RGBA k_quad = {204, 0, 102, 255};
// Fullscreen pass: red where x < 32, green where y < 32, blue 0.2, alpha 1.
static inline RGBA quadrants(int x, int y) { return RGBA{(uint8_t)(x < 32 ? 255 : 0), (uint8_t)(y < 32 ? 255 : 0), 51, 255}; }
// The centre quad covers pixels [16, 48) in both directions (NDC -0.5 .. 0.5 on a 64x64 target).
static inline bool in_quad(int x, int y) { return x >= 16 && x < 48 && y >= 16 && y < 48; }
// A CPU-side pattern for upload/copy checks.
static inline RGBA pattern(int x, int y) { return RGBA{(uint8_t)(x * 4), (uint8_t)(y * 4), (uint8_t)((x ^ y) * 3), (uint8_t)(255 - x)}; }

// HLSL shared by D3D11 and D3D12 (compiled at run time with D3DCompile, SM 4.0 / 5.0).
static const char k_hlsl[] =
    "cbuffer C : register(b0) { float4 quad_color; };\n"
    "float4 vs_full(uint id : SV_VertexID) : SV_Position {\n"
    "    float2 uv = float2((id << 1) & 2, id & 2);\n"
    "    return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);\n"
    "}\n"
    "float4 ps_quadrants(float4 pos : SV_Position) : SV_Target {\n"
    "    return float4(pos.x < 32 ? 1 : 0, pos.y < 32 ? 1 : 0, 0.2, 1);\n"
    "}\n"
    "float4 vs_quad(float2 p : POSITION) : SV_Position { return float4(p, 0, 1); }\n"
    "float4 ps_color() : SV_Target { return quad_color; }\n";
// Triangle-strip quad, NDC -0.5 .. 0.5.
static const float k_quad_verts[] = {-0.5f, 0.5f, 0.5f, 0.5f, -0.5f, -0.5f, 0.5f, -0.5f};
static const float k_quad_color[4] = {204 / 255.0f, 0, 102 / 255.0f, 1};
static const float k_clear_color[4] = {51 / 255.0f, 102 / 255.0f, 153 / 255.0f, 1};

// D3DCompile from whichever d3dcompiler the prefix has.
typedef HRESULT(WINAPI *D3DCompile_t)(const void *, SIZE_T, const char *, const void *, void *, const char *, const char *, UINT,
                                      UINT, struct ID3D10Blob **, struct ID3D10Blob **);
static D3DCompile_t load_d3dcompile(const char **which) {
    for (const char *n : {"d3dcompiler_47.dll", "d3dcompiler_46.dll", "d3dcompiler_43.dll"})
        if (HMODULE m = LoadLibraryA(n))
            if (auto f = (D3DCompile_t)(void *)GetProcAddress(m, "D3DCompile")) { *which = n; return f; }
    *which = "none";
    return nullptr;
}

// A hidden window for APIs that need one (D3D9, WGL).
static HWND gfx_window(const char *title, bool visible = false) {
    WNDCLASSA wc = {};
    wc.style = CS_OWNDC; wc.lpfnWndProc = DefWindowProcA; wc.hInstance = GetModuleHandleA(NULL); wc.lpszClassName = "LadderGfx";
    RegisterClassA(&wc);
    RECT r = {0, 0, W, H};
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    HWND h = CreateWindowA("LadderGfx", title, WS_OVERLAPPEDWINDOW, 0, 0, r.right - r.left, r.bottom - r.top, NULL, NULL, wc.hInstance, NULL);
    if (h && visible) { ShowWindow(h, SW_SHOWNOACTIVATE); UpdateWindow(h); }
    return h;
}

// D3D_FEATURE_LEVEL (0xb100) as "11_1".
static const char *fl_name(unsigned fl) {
    static char buf[4][8]; static int i;
    char *b = buf[i++ & 3];
    snprintf(b, 8, "%u_%u", fl >> 12, (fl >> 8) & 0xf);
    return b;
}

// Narrow a wide adapter description for the INFO lines.
static void narrow(const WCHAR *w, char *out, size_t n) {
    WideCharToMultiByte(CP_UTF8, 0, w, -1, out, (int)n, NULL, NULL);
}
