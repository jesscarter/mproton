// d3d9: adapter enumeration, identifier, display modes, HAL caps and format support, then a
// device on a hidden window: clear, a fixed-function pre-transformed quad, and
// GetRenderTargetData readback compared pixel by pixel. Also creates IDirect3D9Ex.
//
// d3d9.dll is loaded at run time. On the Mac it comes from wined3d (the default) or from DXVK
// when the bundle has a dxvk tree; DXMT has no D3D9.
#include "gfx.h"
#include <d3d9.h>

#define REL(p) do { if (p) { (p)->Release(); (p) = nullptr; } } while (0)

struct Vtx { float x, y, z, rhw; DWORD color; };

int main(int argc, char **argv) {
    ladder_begin("d3d9", argc, argv);
    HMODULE lib = LoadLibraryA("d3d9.dll");
    typedef IDirect3D9 *(WINAPI * create_t)(UINT);
    typedef HRESULT(WINAPI * create_ex_t)(UINT, IDirect3D9Ex **);
    auto create = lib ? (create_t)(void *)GetProcAddress(lib, "Direct3DCreate9") : nullptr;
    auto create_ex = lib ? (create_ex_t)(void *)GetProcAddress(lib, "Direct3DCreate9Ex") : nullptr;
    if (!create) { skip("create", "no d3d9.dll / Direct3DCreate9"); return ladder_done(); }
    IDirect3D9 *d3d = create(D3D_SDK_VERSION);
    if (!check(d3d != nullptr, "create", "Direct3DCreate9 %s", d3d ? "ok" : "returned NULL")) return ladder_done();

    // ---- adapters ----
    UINT n = d3d->GetAdapterCount();
    check(n >= 1, "adapters", "%u adapter(s)", n);
    for (UINT a = 0; a < n && a < 4; a++) {
        D3DADAPTER_IDENTIFIER9 id = {};
        d3d->GetAdapterIdentifier(a, 0, &id);
        UINT modes = d3d->GetAdapterModeCount(a, D3DFMT_X8R8G8B8);
        D3DDISPLAYMODE cur = {};
        d3d->GetAdapterDisplayMode(a, &cur);
        info(a == 0 ? "adapter0" : "adapter", "%u: %s (driver %s, vendor %04lx device %04lx), %u X8R8G8B8 modes, current %ux%u@%u", a,
             id.Description, id.Driver, id.VendorId, id.DeviceId, modes, cur.Width, cur.Height, cur.RefreshRate);
        if (a == 0) check(modes > 0 && cur.Width > 0, "display_modes", "%u X8R8G8B8 modes, current %ux%u", modes, cur.Width, cur.Height);
    }

    // ---- caps ----
    D3DCAPS9 caps = {};
    HRESULT hr = d3d->GetDeviceCaps(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, &caps);
    unsigned vs = (unsigned)(caps.VertexShaderVersion & 0xffff), ps = (unsigned)(caps.PixelShaderVersion & 0xffff);
    check(SUCCEEDED(hr) && vs >= 0x200 && ps >= 0x200 && caps.MaxTextureWidth >= 2048 && caps.MaxSimultaneousTextures >= 4 &&
              caps.MaxStreams >= 1,
          "caps", "HAL: VS %u.%u PS %u.%u, max texture %lux%lu, %lu texture stages, %lu streams, %lu RTs", vs >> 8, vs & 0xff, ps >> 8,
          ps & 0xff, caps.MaxTextureWidth, caps.MaxTextureHeight, caps.MaxSimultaneousTextures, caps.MaxStreams, caps.NumSimultaneousRTs);
    struct { const char *what; DWORD usage; D3DRESOURCETYPE type; D3DFORMAT fmt; } fmts[] = {
        {"A8R8G8B8 texture", 0, D3DRTYPE_TEXTURE, D3DFMT_A8R8G8B8},
        {"A8R8G8B8 render target", D3DUSAGE_RENDERTARGET, D3DRTYPE_TEXTURE, D3DFMT_A8R8G8B8},
        {"D24S8 depth", D3DUSAGE_DEPTHSTENCIL, D3DRTYPE_SURFACE, D3DFMT_D24S8},
        {"DXT1 texture", 0, D3DRTYPE_TEXTURE, D3DFMT_DXT1},
        {"DXT5 texture", 0, D3DRTYPE_TEXTURE, D3DFMT_DXT5},
    };
    char missing[256] = "";
    for (auto &f : fmts)
        if (FAILED(d3d->CheckDeviceFormat(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8, f.usage, f.type, f.fmt)))
            snprintf(missing + strlen(missing), sizeof missing - strlen(missing), "%s%s", *missing ? ", " : "", f.what);
    check(!*missing, "formats", "CheckDeviceFormat: %s", *missing ? missing : "all of A8R8G8B8 texture/RT, D24S8, DXT1, DXT5 supported");

    if (create_ex) {
        IDirect3D9Ex *ex = nullptr;
        hr = create_ex(D3D_SDK_VERSION, &ex);
        check(SUCCEEDED(hr) && ex && ex->GetAdapterCount() == n, "create_ex", "Direct3DCreate9Ex %08lx", (unsigned long)hr);
        REL(ex);
    } else skip("create_ex", "no Direct3DCreate9Ex");

    // ---- device, clear, draw, readback ----
    HWND wnd = gfx_window("ladder d3d9");
    D3DPRESENT_PARAMETERS pp = {};
    pp.BackBufferWidth = W; pp.BackBufferHeight = H; pp.BackBufferFormat = D3DFMT_X8R8G8B8; pp.BackBufferCount = 1;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD; pp.hDeviceWindow = wnd; pp.Windowed = TRUE;
    IDirect3DDevice9 *dev = nullptr;
    hr = d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, wnd, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &dev);
    if (FAILED(hr)) hr = d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, wnd, D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &dev);
    if (check(SUCCEEDED(hr), "device", "CreateDevice(HAL) %08lx", (unsigned long)hr)) {
        IDirect3DSurface9 *rt = nullptr, *sys = nullptr;
        dev->CreateRenderTarget(W, H, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &rt, nullptr);
        dev->CreateOffscreenPlainSurface(W, H, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &sys, nullptr);
        auto read = [&](const char *name, auto expect, const char *what) {
            HRESULT r = dev->GetRenderTargetData(rt, sys);
            D3DLOCKED_RECT lr = {};
            if (FAILED(r) || FAILED(sys->LockRect(&lr, nullptr, D3DLOCK_READONLY))) { fail(name, "GetRenderTargetData/LockRect %08lx", (unsigned long)r); return; }
            check_pixels(name, lr.pBits, lr.Pitch, expect, true, what);
            sys->UnlockRect();
        };
        if (rt && sys) {
            dev->SetRenderTarget(0, rt);
            dev->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_ARGB(255, 51, 102, 153), 1.0f, 0);
            read("clear", [](int, int) { return k_clear; }, "Clear -> GetRenderTargetData");

            // Pre-transformed quad; edges half a pixel off the centres, so it covers pixels 16..47.
            DWORD c = D3DCOLOR_ARGB(255, 204, 0, 102);
            Vtx v[4] = {{15.5f, 15.5f, 0.5f, 1, c}, {47.5f, 15.5f, 0.5f, 1, c}, {15.5f, 47.5f, 0.5f, 1, c}, {47.5f, 47.5f, 0.5f, 1, c}};
            dev->SetRenderState(D3DRS_LIGHTING, FALSE);
            dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
            dev->SetRenderState(D3DRS_ZENABLE, FALSE);
            dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
            dev->BeginScene();
            hr = dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, v, sizeof(Vtx));
            dev->EndScene();
            if (FAILED(hr)) fail("draw", "DrawPrimitiveUP %08lx", (unsigned long)hr);
            else read("draw", [](int x, int y) { return in_quad(x, y) ? k_quad : k_clear; }, "fixed-function XYZRHW quad");
        } else fail("render_target", "CreateRenderTarget/CreateOffscreenPlainSurface failed");
        REL(rt); REL(sys);
        info("device_release", "final Release returned %lu", dev->Release());
    }
    if (wnd) DestroyWindow(wnd);
    REL(d3d);
    return ladder_done();
}
