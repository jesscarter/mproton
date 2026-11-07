// d3d11: device creation, clear, two draws (a fullscreen SV_VertexID pass and a vertex-buffer
// quad coloured from a constant buffer), UpdateSubresource, CopyResource,
// CopySubresourceRegion and buffer copies, each read back through a staging resource and
// compared pixel by pixel. Offscreen only: no window or swapchain.
//
// d3d11.dll and dxgi.dll are loaded at run time so the program also starts on a stack without
// them (and reports SKIP). Which implementation answers (wined3d, DXMT, DXVK) is picked by the
// prefix/compatdb, and printed as INFO from the adapter description.
#include "gfx.h"
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>

#define REL(p) do { if (p) { (p)->Release(); (p) = nullptr; } } while (0)

static ID3D11Device *dev;
static ID3D11DeviceContext *ctx;
static D3DCompile_t compile;

static ID3DBlob *build(const char *entry, const char *target) {
    ID3DBlob *code = nullptr, *err = nullptr;
    HRESULT hr = compile(k_hlsl, sizeof k_hlsl - 1, "ladder.hlsl", nullptr, nullptr, entry, target, 0, 0, &code, &err);
    if (FAILED(hr)) {
        fail("compile", "D3DCompile %s %s: %08lx %s", entry, target, (unsigned long)hr, err ? (const char *)err->GetBufferPointer() : "");
        REL(err);
        return nullptr;
    }
    REL(err);
    return code;
}

static ID3D11Texture2D *make_tex(UINT bind, D3D11_USAGE usage = D3D11_USAGE_DEFAULT, UINT cpu = 0, const void *init = nullptr) {
    D3D11_TEXTURE2D_DESC d = {};
    d.Width = W; d.Height = H; d.MipLevels = 1; d.ArraySize = 1; d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.SampleDesc.Count = 1; d.Usage = usage; d.BindFlags = bind; d.CPUAccessFlags = cpu;
    D3D11_SUBRESOURCE_DATA sd = {init, W * 4, 0};
    ID3D11Texture2D *t = nullptr;
    dev->CreateTexture2D(&d, init ? &sd : nullptr, &t);
    return t;
}

// Read back a texture into `out` (W*H*4 bytes). Returns false (and FAILs `name`) if mapping failed.
static bool readback(ID3D11Texture2D *src, const char *name, uint8_t *out) {
    ID3D11Texture2D *st = make_tex(0, D3D11_USAGE_STAGING, D3D11_CPU_ACCESS_READ);
    if (!st) { fail(name, "staging texture creation failed"); return false; }
    ctx->CopyResource(st, src);
    D3D11_MAPPED_SUBRESOURCE m = {};
    HRESULT hr = ctx->Map(st, 0, D3D11_MAP_READ, 0, &m);
    if (FAILED(hr)) { fail(name, "Map(staging) %08lx", (unsigned long)hr); REL(st); return false; }
    for (int y = 0; y < H; y++) memcpy(out + y * W * 4, (uint8_t *)m.pData + y * m.RowPitch, W * 4);
    ctx->Unmap(st, 0);
    REL(st);
    return true;
}

int main(int argc, char **argv) {
    ladder_begin("d3d11", argc, argv);
    HMODULE lib = LoadLibraryA("d3d11.dll");
    auto create = lib ? (PFN_D3D11_CREATE_DEVICE)(void *)GetProcAddress(lib, "D3D11CreateDevice") : nullptr;
    if (!create) { skip("device", "no d3d11.dll / D3D11CreateDevice"); return ladder_done(); }

    D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
    D3D_FEATURE_LEVEL fl = {};
    HRESULT hr = create(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 4, D3D11_SDK_VERSION, &dev, &fl, &ctx);
    if (hr == E_INVALIDARG)   // pre-11.1 runtimes reject 11_1 in the list
        hr = create(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels + 1, 3, D3D11_SDK_VERSION, &dev, &fl, &ctx);
    if (!check(SUCCEEDED(hr), "device", "D3D11CreateDevice %08lx, feature level %s", (unsigned long)hr, fl_name(fl))) return ladder_done();

    IDXGIDevice *dxgi_dev = nullptr; IDXGIAdapter *ad = nullptr;
    DXGI_ADAPTER_DESC desc = {};
    if (SUCCEEDED(dev->QueryInterface(__uuidof(IDXGIDevice), (void **)&dxgi_dev)) && SUCCEEDED(dxgi_dev->GetAdapter(&ad))) ad->GetDesc(&desc);
    char name[128]; narrow(desc.Description, name, sizeof name);
    info("adapter", "%s (vendor %04x device %04x), feature level %s", name, desc.VendorId, desc.DeviceId, fl_name(fl));
    REL(ad); REL(dxgi_dev);

    const char *which;
    compile = load_d3dcompile(&which);
    uint8_t px[W * H * 4];

    // ---- clear ----
    ID3D11Texture2D *rt = make_tex(D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
    ID3D11RenderTargetView *rtv = nullptr;
    if (rt) dev->CreateRenderTargetView(rt, nullptr, &rtv);
    if (!check(rt && rtv, "render_target", "64x64 RGBA8 render target %s", rt && rtv ? "created" : "FAILED")) return ladder_done();
    ctx->ClearRenderTargetView(rtv, k_clear_color);
    if (readback(rt, "clear", px)) check_pixels("clear", px, W * 4, [](int, int) { return k_clear; });

    // ---- draws ----
    ID3DBlob *vs_full = nullptr, *ps_q = nullptr, *vs_quad = nullptr, *ps_col = nullptr;
    if (!compile) skip("draw", "no d3dcompiler DLL");
    else {
        vs_full = build("vs_full", "vs_4_0"); ps_q = build("ps_quadrants", "ps_4_0");
        vs_quad = build("vs_quad", "vs_4_0"); ps_col = build("ps_color", "ps_4_0");
    }
    if (vs_full && ps_q && vs_quad && ps_col) {
        ID3D11VertexShader *vs1 = nullptr, *vs2 = nullptr; ID3D11PixelShader *ps1 = nullptr, *ps2 = nullptr;
        dev->CreateVertexShader(vs_full->GetBufferPointer(), vs_full->GetBufferSize(), nullptr, &vs1);
        dev->CreatePixelShader(ps_q->GetBufferPointer(), ps_q->GetBufferSize(), nullptr, &ps1);
        dev->CreateVertexShader(vs_quad->GetBufferPointer(), vs_quad->GetBufferSize(), nullptr, &vs2);
        dev->CreatePixelShader(ps_col->GetBufferPointer(), ps_col->GetBufferSize(), nullptr, &ps2);
        D3D11_RASTERIZER_DESC rd = {}; rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE; rd.DepthClipEnable = TRUE;
        ID3D11RasterizerState *rs = nullptr; dev->CreateRasterizerState(&rd, &rs);
        D3D11_VIEWPORT vp = {0, 0, (float)W, (float)H, 0, 1};
        ctx->OMSetRenderTargets(1, &rtv, nullptr);
        ctx->RSSetViewports(1, &vp);
        ctx->RSSetState(rs);

        // fullscreen triangle, no vertex buffer
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->VSSetShader(vs1, nullptr, 0); ctx->PSSetShader(ps1, nullptr, 0);
        ctx->Draw(3, 0);
        if (readback(rt, "draw_fullscreen", px)) check_pixels("draw_fullscreen", px, W * 4, quadrants, false, "SV_VertexID triangle + SV_Position shading");

        // vertex buffer quad, colour from a constant buffer
        D3D11_BUFFER_DESC bd = {sizeof k_quad_verts, D3D11_USAGE_IMMUTABLE, D3D11_BIND_VERTEX_BUFFER, 0, 0, 0};
        D3D11_SUBRESOURCE_DATA vd = {k_quad_verts, 0, 0};
        ID3D11Buffer *vb = nullptr; dev->CreateBuffer(&bd, &vd, &vb);
        D3D11_BUFFER_DESC cd = {16, D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0};
        ID3D11Buffer *cb = nullptr; dev->CreateBuffer(&cd, nullptr, &cb);
        D3D11_MAPPED_SUBRESOURCE m = {};
        if (cb && SUCCEEDED(ctx->Map(cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) { memcpy(m.pData, k_quad_color, 16); ctx->Unmap(cb, 0); }
        D3D11_INPUT_ELEMENT_DESC el = {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0};
        ID3D11InputLayout *il = nullptr;
        dev->CreateInputLayout(&el, 1, vs_quad->GetBufferPointer(), vs_quad->GetBufferSize(), &il);
        UINT stride = 8, off = 0;
        ctx->IASetInputLayout(il);
        ctx->IASetVertexBuffers(0, 1, &vb, &stride, &off);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        ctx->VSSetShader(vs2, nullptr, 0); ctx->PSSetShader(ps2, nullptr, 0);
        ctx->PSSetConstantBuffers(0, 1, &cb);
        ctx->Draw(4, 0);
        if (readback(rt, "draw_vb_cb", px))
            check_pixels("draw_vb_cb", px, W * 4, [](int x, int y) { return in_quad(x, y) ? k_quad : quadrants(x, y); }, false,
                         "vertex-buffer quad over the fullscreen pass");
        REL(il); REL(cb); REL(vb); REL(rs); REL(vs1); REL(vs2); REL(ps1); REL(ps2);
    }
    REL(vs_full); REL(ps_q); REL(vs_quad); REL(ps_col);

    // ---- uploads and copies ----
    static uint8_t pat[W * H * 4];
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) { RGBA c = pattern(x, y); memcpy(pat + (y * W + x) * 4, &c, 4); }
    ID3D11Texture2D *up = make_tex(D3D11_BIND_SHADER_RESOURCE);
    if (up) {
        ctx->UpdateSubresource(up, 0, nullptr, pat, W * 4, 0);
        if (readback(up, "update_subresource", px)) check_pixels("update_subresource", px, W * 4, pattern);
        ID3D11Texture2D *dst = make_tex(D3D11_BIND_SHADER_RESOURCE, D3D11_USAGE_DEFAULT, 0, pat);
        // copy the 16x16 top-left of the render target (red+green quadrant, blue 51) to (40, 8)
        D3D11_BOX box = {0, 0, 0, 16, 16, 1};
        ctx->CopySubresourceRegion(dst, 0, 40, 8, 0, rt, 0, &box);
        if (readback(dst, "copy_region", px))
            check_pixels("copy_region", px, W * 4, [](int x, int y) {
                return x >= 40 && x < 56 && y >= 8 && y < 24 ? quadrants(x - 40, y - 8) : pattern(x, y);
            }, false, "16x16 box from the render target into an initialised texture");
        ID3D11Texture2D *full = make_tex(0);
        if (full) {
            ctx->CopyResource(full, up);
            if (readback(full, "copy_resource", px)) check_pixels("copy_resource", px, W * 4, pattern);
        }
        REL(full); REL(dst);
    }
    REL(up);

    // buffer: initialised default buffer -> CopySubresourceRegion slice -> staging
    {
        uint32_t init[1024], got[1024] = {};
        for (int i = 0; i < 1024; i++) init[i] = 0x9E3779B9u * (i + 1);
        D3D11_BUFFER_DESC bd = {sizeof init, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, 0, 0};
        D3D11_SUBRESOURCE_DATA sd = {init, 0, 0};
        ID3D11Buffer *src = nullptr, *st = nullptr;
        dev->CreateBuffer(&bd, &sd, &src);
        D3D11_BUFFER_DESC sdsc = {sizeof init, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 0, 0};
        dev->CreateBuffer(&sdsc, nullptr, &st);
        int bad = -1;
        if (src && st) {
            D3D11_BOX b = {256 * 4, 0, 0, 768 * 4, 1, 1};
            ctx->CopySubresourceRegion(st, 0, 0, 0, 0, src, 0, &b);   // elements 256..767 -> 0..511
            D3D11_MAPPED_SUBRESOURCE m = {};
            if (SUCCEEDED(ctx->Map(st, 0, D3D11_MAP_READ, 0, &m))) {
                memcpy(got, m.pData, sizeof got);
                ctx->Unmap(st, 0);
                bad = 0;
                for (int i = 0; i < 512; i++) bad += got[i] != init[256 + i];
            }
        }
        check(bad == 0, "buffer_copy", "1 KiB..3 KiB slice of an initialised buffer copied to staging: %d words wrong%s", bad,
              bad < 0 ? " (buffer creation or Map failed)" : "");
        REL(src); REL(st);
    }

    REL(rtv); REL(rt);
    ctx->ClearState(); ctx->Flush();
    REL(ctx);
    ULONG refs = dev->Release();
    info("device_release", "final Release returned %lu", refs);
    return ladder_done();
}
