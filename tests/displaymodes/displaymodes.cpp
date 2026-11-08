// displaymodes: print what a Windows game sees of the display.
//   GDI:  EnumDisplayDevicesW (adapters + monitors), EnumDisplaySettingsW (current,
//         registry, every mode), monitor rects (EnumDisplayMonitors/GetMonitorInfoW)
//   DXGI: adapters, outputs (GetDesc), GetDisplayModeList for R8G8B8A8_UNORM, _SRGB,
//         B8G8R8A8_UNORM, _SRGB, R10G10B10A2_UNORM, R16G16B16A16_FLOAT, also through
//         IDXGIOutput1::GetDisplayModeList1 and after a D3D11 device exists (the order
//         Unity uses: device first, then the output of that adapter)
// Build: tests/displaymodes/build.sh   Run: tests/displaymodes/run.sh
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dxgi1_2.h>
#include <d3d11.h>
#include <stdio.h>
#include <wingdi.h>
#include <initializer_list>

static const struct { DXGI_FORMAT f; const char *name; } formats[] = {
    {DXGI_FORMAT_R8G8B8A8_UNORM, "R8G8B8A8_UNORM"},
    {DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, "R8G8B8A8_UNORM_SRGB"},
    {DXGI_FORMAT_B8G8R8A8_UNORM, "B8G8R8A8_UNORM"},
    {DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, "B8G8R8A8_UNORM_SRGB"},
    {DXGI_FORMAT_R10G10B10A2_UNORM, "R10G10B10A2_UNORM"},
    {DXGI_FORMAT_R16G16B16A16_FLOAT, "R16G16B16A16_FLOAT"},
};

static void print_devmode(const char *what, const DEVMODEW *dm)
{
    printf("  %-8s %5lux%-5lu %2lubpp %3luHz pos=(%ld,%ld) orient=%lu flags=0x%lx fields=0x%lx\n", what,
           dm->dmPelsWidth, dm->dmPelsHeight, dm->dmBitsPerPel, dm->dmDisplayFrequency,
           dm->dmPosition.x, dm->dmPosition.y, dm->dmDisplayOrientation, dm->dmDisplayFlags,
           dm->dmFields);
}

static void gdi(void)
{
    printf("== GDI ==\n");
    DISPLAY_DEVICEW dd;
    for (DWORD i = 0;; i++) {
        dd.cb = sizeof(dd);
        if (!EnumDisplayDevicesW(NULL, i, &dd, 0)) break;
        printf("adapter %lu: %ls \"%ls\" state=0x%lx id=%ls\n", i, dd.DeviceName, dd.DeviceString,
               dd.StateFlags, dd.DeviceID);
        WCHAR adapter[32];
        lstrcpynW(adapter, dd.DeviceName, 32);
        DISPLAY_DEVICEW md;
        for (DWORD j = 0;; j++) {
            md.cb = sizeof(md);
            if (!EnumDisplayDevicesW(adapter, j, &md, 0)) break;
            printf("  monitor %lu: %ls \"%ls\" state=0x%lx\n", j, md.DeviceName, md.DeviceString, md.StateFlags);
        }
        if (!(dd.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP)) continue;
        DEVMODEW dm;
        memset(&dm, 0, sizeof(dm)); dm.dmSize = sizeof(dm);
        if (EnumDisplaySettingsW(adapter, ENUM_CURRENT_SETTINGS, &dm)) print_devmode("current", &dm);
        else printf("  current  FAILED %lu\n", GetLastError());
        memset(&dm, 0, sizeof(dm)); dm.dmSize = sizeof(dm);
        if (EnumDisplaySettingsW(adapter, ENUM_REGISTRY_SETTINGS, &dm)) print_devmode("registry", &dm);
        else printf("  registry FAILED %lu\n", GetLastError());
        DWORD n = 0;
        for (;; n++) {
            memset(&dm, 0, sizeof(dm)); dm.dmSize = sizeof(dm);
            if (!EnumDisplaySettingsW(adapter, n, &dm)) break;
            char tag[16]; snprintf(tag, sizeof(tag), "mode %lu", n);
            print_devmode(tag, &dm);
        }
        printf("  %lu modes (EnumDisplaySettingsW)\n", n);
        DWORD nraw = 0;
        for (;; nraw++) {
            memset(&dm, 0, sizeof(dm)); dm.dmSize = sizeof(dm);
            if (!EnumDisplaySettingsExW(adapter, nraw, &dm, EDS_RAWMODE)) break;
        }
        printf("  %lu modes (EnumDisplaySettingsExW EDS_RAWMODE)\n", nraw);
    }
}

// QueryDisplayConfig: the CCD API. Engines that want exact (rational) refresh rates or
// monitor names use it instead of EnumDisplaySettings.
static void ccd(void)
{
    printf("== QueryDisplayConfig ==\n");
    for (UINT32 qflags : {(UINT32)QDC_ONLY_ACTIVE_PATHS, (UINT32)QDC_ALL_PATHS}) {
        UINT32 np = 0, nm = 0;
        LONG r = GetDisplayConfigBufferSizes(qflags, &np, &nm);
        printf("flags=0x%x GetDisplayConfigBufferSizes=%ld paths=%u modes=%u\n", qflags, r, np, nm);
        if (r || !np) continue;
        DISPLAYCONFIG_PATH_INFO *paths = new DISPLAYCONFIG_PATH_INFO[np];
        DISPLAYCONFIG_MODE_INFO *modes = new DISPLAYCONFIG_MODE_INFO[nm];
        r = QueryDisplayConfig(qflags, &np, paths, &nm, modes, NULL);
        printf("  QueryDisplayConfig=%ld paths=%u modes=%u\n", r, np, nm);
        for (UINT32 i = 0; !r && i < np; i++) {
            auto &p = paths[i];
            printf("  path %u: flags=0x%x src(adapter=%lx:%lx id=%u mode=%u) tgt(id=%u mode=%u avail=%d tech=%d rot=%d scaling=%d refresh=%u/%u scan=%d)\n",
                   i, p.flags, p.sourceInfo.adapterId.HighPart, p.sourceInfo.adapterId.LowPart, p.sourceInfo.id,
                   p.sourceInfo.modeInfoIdx, p.targetInfo.id, p.targetInfo.modeInfoIdx, p.targetInfo.targetAvailable,
                   p.targetInfo.outputTechnology, p.targetInfo.rotation, p.targetInfo.scaling,
                   p.targetInfo.refreshRate.Numerator, p.targetInfo.refreshRate.Denominator, p.targetInfo.scanLineOrdering);
            if (i > 3) break;
            DISPLAYCONFIG_SOURCE_DEVICE_NAME sn = {};
            sn.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME; sn.header.size = sizeof(sn);
            sn.header.adapterId = p.sourceInfo.adapterId; sn.header.id = p.sourceInfo.id;
            LONG r2 = DisplayConfigGetDeviceInfo(&sn.header);
            printf("    source name=%ld %ls\n", r2, sn.viewGdiDeviceName);
            DISPLAYCONFIG_TARGET_DEVICE_NAME tn = {};
            tn.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME; tn.header.size = sizeof(tn);
            tn.header.adapterId = p.targetInfo.adapterId; tn.header.id = p.targetInfo.id;
            r2 = DisplayConfigGetDeviceInfo(&tn.header);
            printf("    target name=%ld \"%ls\" path=%ls tech=%d edid=%04x:%04x\n", r2, tn.monitorFriendlyDeviceName,
                   tn.monitorDevicePath, tn.outputTechnology, tn.edidManufactureId, tn.edidProductCodeId);
        }
        for (UINT32 i = 0; !r && i < nm; i++) {
            auto &m = modes[i];
            if (m.infoType == DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE)
                printf("  mode %u source id=%u %ux%u fmt=%d pos=(%ld,%ld)\n", i, m.id, m.sourceMode.width,
                       m.sourceMode.height, m.sourceMode.pixelFormat, m.sourceMode.position.x, m.sourceMode.position.y);
            else if (m.infoType == DISPLAYCONFIG_MODE_INFO_TYPE_TARGET) {
                auto &v = m.targetMode.targetVideoSignalInfo;
                printf("  mode %u target id=%u active=%ux%u total=%ux%u vsync=%u/%u hsync=%u/%u pixclk=%llu scan=%d\n", i, m.id,
                       v.activeSize.cx, v.activeSize.cy, v.totalSize.cx, v.totalSize.cy, v.vSyncFreq.Numerator,
                       v.vSyncFreq.Denominator, v.hSyncFreq.Numerator, v.hSyncFreq.Denominator,
                       (unsigned long long)v.pixelRate, v.scanLineOrdering);
            } else
                printf("  mode %u type=%d id=%u\n", i, m.infoType, m.id);
        }
        delete[] paths; delete[] modes;
    }
}

static BOOL CALLBACK monitor_cb(HMONITOR mon, HDC, LPRECT, LPARAM)
{
    MONITORINFOEXW mi; mi.cbSize = sizeof(mi);
    GetMonitorInfoW(mon, &mi);
    printf("monitor %p %ls rect=(%ld,%ld)-(%ld,%ld) work=(%ld,%ld)-(%ld,%ld) primary=%d\n", (void *)mon,
           mi.szDevice, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right, mi.rcMonitor.bottom,
           mi.rcWork.left, mi.rcWork.top, mi.rcWork.right, mi.rcWork.bottom, !!(mi.dwFlags & MONITORINFOF_PRIMARY));
    return TRUE;
}

static void dump_modes(IDXGIOutput *out, bool verbose)
{
    IDXGIOutput1 *out1 = NULL;
    out->QueryInterface(__uuidof(IDXGIOutput1), (void **)&out1);
    for (auto &fmt : formats) {
        for (UINT flags : {0u, (UINT)DXGI_ENUM_MODES_SCALING}) {
            UINT n = 0;
            HRESULT hr = out->GetDisplayModeList(fmt.f, flags, &n, NULL);
            printf("    %-20s flags=%u: hr=0x%08lx count=%u", fmt.name, flags, (unsigned long)hr, n);
            if (FAILED(hr) || !n) { printf("\n"); continue; }
            DXGI_MODE_DESC *m = new DXGI_MODE_DESC[n];
            hr = out->GetDisplayModeList(fmt.f, flags, &n, m);
            printf(" (second call hr=0x%08lx count=%u)\n", (unsigned long)hr, n);
            if (verbose || fmt.f == DXGI_FORMAT_R8G8B8A8_UNORM)
                for (UINT i = 0; i < n; i++)
                    printf("      %5ux%-5u %u/%u Hz (%.3f) scanline=%d scaling=%d\n", m[i].Width, m[i].Height,
                           m[i].RefreshRate.Numerator, m[i].RefreshRate.Denominator,
                           m[i].RefreshRate.Denominator ? (double)m[i].RefreshRate.Numerator / m[i].RefreshRate.Denominator : 0.0,
                           m[i].ScanlineOrdering, m[i].Scaling);
            delete[] m;
        }
        if (out1) {
            UINT n = 0;
            HRESULT hr = out1->GetDisplayModeList1(fmt.f, 0, &n, NULL);
            printf("    %-20s GetDisplayModeList1: hr=0x%08lx count=%u\n", fmt.name, (unsigned long)hr, n);
        }
    }
    // FindClosestMatchingMode for the current desktop size, as engines do for fullscreen
    DXGI_OUTPUT_DESC od; out->GetDesc(&od);
    DXGI_MODE_DESC want = {}, got = {};
    want.Width = od.DesktopCoordinates.right - od.DesktopCoordinates.left;
    want.Height = od.DesktopCoordinates.bottom - od.DesktopCoordinates.top;
    want.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    HRESULT hr = out->FindClosestMatchingMode(&want, &got, NULL);
    printf("    FindClosestMatchingMode(%ux%u): hr=0x%08lx -> %ux%u %u/%u\n", want.Width, want.Height,
           (unsigned long)hr, got.Width, got.Height, got.RefreshRate.Numerator, got.RefreshRate.Denominator);
    if (out1) out1->Release();
}

static void dxgi_outputs(IDXGIAdapter *ad, bool verbose)
{
    DXGI_ADAPTER_DESC ade; ad->GetDesc(&ade);
    printf("adapter \"%ls\" vendor=0x%x device=0x%x vram=%zu MB\n", ade.Description, ade.VendorId, ade.DeviceId,
           (size_t)(ade.DedicatedVideoMemory >> 20));
    IDXGIOutput *out;
    UINT o = 0;
    for (; ad->EnumOutputs(o, &out) == S_OK; o++) {
        DXGI_OUTPUT_DESC od; out->GetDesc(&od);
        printf("  output %u: %ls attached=%d rect=(%ld,%ld)-(%ld,%ld) rot=%d mon=%p\n", o, od.DeviceName,
               od.AttachedToDesktop, od.DesktopCoordinates.left, od.DesktopCoordinates.top,
               od.DesktopCoordinates.right, od.DesktopCoordinates.bottom, od.Rotation, (void *)od.Monitor);
        dump_modes(out, verbose);
        out->Release();
    }
    printf("  %u outputs\n", o);
}

static void dxgi(bool verbose)
{
    printf("== DXGI (CreateDXGIFactory1) ==\n");
    IDXGIFactory1 *f;
    HRESULT hr = CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void **)&f);
    if (FAILED(hr)) { printf("CreateDXGIFactory1 failed 0x%08lx\n", (unsigned long)hr); return; }
    IDXGIAdapter1 *ad;
    for (UINT a = 0; f->EnumAdapters1(a, &ad) == S_OK; a++) {
        dxgi_outputs(ad, verbose);
        ad->Release();
    }
    f->Release();

    printf("== DXGI via D3D11 device (D3D11CreateDevice -> IDXGIDevice -> adapter) ==\n");
    ID3D11Device *dev = NULL;
    D3D_FEATURE_LEVEL fl;
    hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0, D3D11_SDK_VERSION, &dev, &fl, NULL);
    if (FAILED(hr)) { printf("D3D11CreateDevice failed 0x%08lx\n", (unsigned long)hr); return; }
    printf("feature level 0x%x\n", fl);
    IDXGIDevice *dd; IDXGIAdapter *a2;
    if (SUCCEEDED(dev->QueryInterface(__uuidof(IDXGIDevice), (void **)&dd)) && SUCCEEDED(dd->GetAdapter(&a2))) {
        dxgi_outputs(a2, false);
        a2->Release(); dd->Release();
    }
    dev->Release();
}

int main(int argc, char **argv)
{
    bool verbose = false, dpi = true;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-v")) verbose = true;
        else if (!strcmp(argv[i], "-nodpi")) dpi = false;
        // the launcher does not pass stdout through, so run.sh asks for a file
        else if (!strcmp(argv[i], "-o") && i + 1 < argc) freopen(argv[++i], "w", stdout);
    }
    printf("displaymodes (%s)\n", sizeof(void *) == 8 ? "64-bit" : "32-bit");
    // Unity players are per-monitor DPI aware (their manifest); match that.
    HMODULE u32 = GetModuleHandleW(L"user32.dll");
    auto set_ctx = (BOOL(WINAPI *)(HANDLE))GetProcAddress(u32, "SetProcessDpiAwarenessContext");
    if (set_ctx && dpi)
        printf("SetProcessDpiAwarenessContext(PER_MONITOR_AWARE_V2) = %d\n", set_ctx((HANDLE)-4));
    printf("GetSystemMetrics: screen=%dx%d virtual=%dx%d monitors=%d\n", GetSystemMetrics(SM_CXSCREEN),
           GetSystemMetrics(SM_CYSCREEN), GetSystemMetrics(SM_CXVIRTUALSCREEN), GetSystemMetrics(SM_CYVIRTUALSCREEN),
           GetSystemMetrics(SM_CMONITORS));
    EnumDisplayMonitors(NULL, NULL, monitor_cb, 0);
    gdi();
    ccd();
    dxgi(verbose);
    fflush(stdout);
    return 0;
}
