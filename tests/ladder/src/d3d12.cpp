// d3d12: fences (CPU signal, queue signal, a queue waiting on a CPU signal, many small
// submissions), clear + copy to a readback buffer, two draws through root signatures and PSOs,
// and an upload on a COPY queue consumed by the DIRECT queue after a cross-queue fence wait.
// Every image is read back and compared pixel by pixel.
//
// d3d12.dll is loaded at run time: the wined3d tree of the Mac bundle has none, so there the
// program reports SKIP instead of failing to start. Shaders are DXBC (SM 5.0) from D3DCompile,
// which both vkd3d-proton and Wine's own vkd3d take.
#include "gfx.h"
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_4.h>

#define REL(p) do { if (p) { (p)->Release(); (p) = nullptr; } } while (0)
#define HR(x) do { HRESULT hr_ = (x); if (FAILED(hr_)) { snprintf(g_err, sizeof g_err, "%s: %08lx", #x, (unsigned long)hr_); return false; } } while (0)
static char g_err[256];

static ID3D12Device *dev;
static ID3D12CommandQueue *direct, *copyq;
static ID3D12CommandAllocator *alloc, *copy_alloc;
static ID3D12GraphicsCommandList *list, *copy_list;
static ID3D12Fence *fence;
static UINT64 fence_val;
static HANDLE ev;
static PFN_D3D12_SERIALIZE_ROOT_SIGNATURE serialize_rs;
static D3DCompile_t compile;

static bool wait_fence(ID3D12Fence *f, UINT64 v, DWORD ms = 10000) {
    if (f->GetCompletedValue() >= v) return true;
    f->SetEventOnCompletion(v, ev);
    return WaitForSingleObject(ev, ms) == WAIT_OBJECT_0 && f->GetCompletedValue() >= v;
}
static bool submit_and_wait(ID3D12CommandQueue *q, ID3D12GraphicsCommandList *l) {
    HR(l->Close());
    ID3D12CommandList *ls[] = {l};
    q->ExecuteCommandLists(1, ls);
    HR(q->Signal(fence, ++fence_val));
    if (!wait_fence(fence, fence_val)) { snprintf(g_err, sizeof g_err, "fence %llu never completed", (unsigned long long)fence_val); return false; }
    return true;
}
static bool reset_list() {
    HR(alloc->Reset());
    HR(list->Reset(alloc, nullptr));
    return true;
}

static ID3D12Resource *make_buffer(D3D12_HEAP_TYPE heap, UINT64 size, D3D12_RESOURCE_STATES state) {
    D3D12_HEAP_PROPERTIES hp = {heap, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 1, 1};
    D3D12_RESOURCE_DESC d = {D3D12_RESOURCE_DIMENSION_BUFFER, 0, size, 1, 1, 1, DXGI_FORMAT_UNKNOWN, {1, 0},
                             D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_FLAG_NONE};
    ID3D12Resource *r = nullptr;
    dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, state, nullptr, __uuidof(ID3D12Resource), (void **)&r);
    return r;
}
static ID3D12Resource *make_texture(D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state, const D3D12_CLEAR_VALUE *cv) {
    D3D12_HEAP_PROPERTIES hp = {D3D12_HEAP_TYPE_DEFAULT, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 1, 1};
    D3D12_RESOURCE_DESC d = {D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, W, H, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0},
                             D3D12_TEXTURE_LAYOUT_UNKNOWN, flags};
    ID3D12Resource *r = nullptr;
    dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, state, cv, __uuidof(ID3D12Resource), (void **)&r);
    return r;
}
static void barrier(ID3D12GraphicsCommandList *l, ID3D12Resource *r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b) {
    D3D12_RESOURCE_BARRIER rb = {};
    rb.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    rb.Transition.pResource = r; rb.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    rb.Transition.StateBefore = a; rb.Transition.StateAfter = b;
    l->ResourceBarrier(1, &rb);
}
// Texture <-> buffer with a 256-byte row pitch (64 * 4 = 256, already aligned).
static D3D12_TEXTURE_COPY_LOCATION buf_loc(ID3D12Resource *b) {
    D3D12_TEXTURE_COPY_LOCATION l = {};
    l.pResource = b; l.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    l.PlacedFootprint.Footprint = {DXGI_FORMAT_R8G8B8A8_UNORM, W, H, 1, W * 4};
    return l;
}
static D3D12_TEXTURE_COPY_LOCATION tex_loc(ID3D12Resource *t) {
    D3D12_TEXTURE_COPY_LOCATION l = {};
    l.pResource = t; l.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; l.SubresourceIndex = 0;
    return l;
}
// Map a readback buffer and compare.
template <class F> static void check_readback(const char *name, ID3D12Resource *rb, F expect, const char *what = "") {
    void *p = nullptr;
    D3D12_RANGE r = {0, (SIZE_T)W * H * 4};
    if (FAILED(rb->Map(0, &r, &p))) { fail(name, "Map(readback) failed"); return; }
    check_pixels(name, p, W * 4, expect, false, what);
    D3D12_RANGE none = {0, 0};
    rb->Unmap(0, &none);
}

// ---- fences ------------------------------------------------------------------------------------------
static void fence_checks() {
    ID3D12Fence *f = nullptr;
    dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), (void **)&f);
    if (!f) { fail("fence.create", "CreateFence failed"); return; }
    f->Signal(5);
    HANDLE e = CreateEventA(NULL, FALSE, FALSE, NULL);
    f->SetEventOnCompletion(5, e);
    DWORD w = WaitForSingleObject(e, 1000);
    check(f->GetCompletedValue() == 5 && w == WAIT_OBJECT_0, "fence.cpu_signal", "CPU Signal(5): completed value %llu, event %s",
          (unsigned long long)f->GetCompletedValue(), w == WAIT_OBJECT_0 ? "set" : "NOT set");

    direct->Signal(f, 10);
    bool ok = wait_fence(f, 10);
    check(ok, "fence.queue_signal", "queue Signal(10) reached %llu", (unsigned long long)f->GetCompletedValue());

    // The queue must stall on a value only the CPU will signal.
    ID3D12Fence *gate = nullptr;
    dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), (void **)&gate);
    direct->Wait(gate, 1);
    direct->Signal(f, 20);
    Sleep(200);
    UINT64 early = f->GetCompletedValue();
    gate->Signal(1);
    ok = wait_fence(f, 20, 5000);
    check(early < 20 && ok, "fence.queue_waits_cpu", "queue Wait on a CPU-signalled fence: %s before the signal, %s after",
          early < 20 ? "held" : "DID NOT WAIT", ok ? "released" : "STILL STUCK");

    // Many tiny submissions with increasing values, waited on out of order.
    for (UINT64 v = 101; v <= 300; v++) direct->Signal(f, v);
    bool mid = wait_fence(f, 150), last = wait_fence(f, 300);
    check(mid && last && f->GetCompletedValue() == 300, "fence.many_signals", "200 queued signals: completed %llu",
          (unsigned long long)f->GetCompletedValue());
    REL(gate); REL(f); CloseHandle(e);
}

// ---- clear + copy ------------------------------------------------------------------------------------
static ID3D12Resource *rt, *rb;
static ID3D12DescriptorHeap *rtv_heap;
static D3D12_CPU_DESCRIPTOR_HANDLE rtv;

static bool clear_test() {
    D3D12_CLEAR_VALUE cv = {DXGI_FORMAT_R8G8B8A8_UNORM, {}};
    memcpy(cv.Color, k_clear_color, sizeof cv.Color);
    rt = make_texture(D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, D3D12_RESOURCE_STATE_RENDER_TARGET, &cv);
    rb = make_buffer(D3D12_HEAP_TYPE_READBACK, W * H * 4, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_DESCRIPTOR_HEAP_DESC hd = {D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
    dev->CreateDescriptorHeap(&hd, __uuidof(ID3D12DescriptorHeap), (void **)&rtv_heap);
    if (!rt || !rb || !rtv_heap) { snprintf(g_err, sizeof g_err, "resource creation failed"); return false; }
    rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
    dev->CreateRenderTargetView(rt, nullptr, rtv);
    if (!reset_list()) return false;
    list->ClearRenderTargetView(rtv, k_clear_color, 0, nullptr);
    barrier(list, rt, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION dst = buf_loc(rb), src = tex_loc(rt);
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    barrier(list, rt, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    if (!submit_and_wait(direct, list)) return false;
    check_readback("clear", rb, [](int, int) { return k_clear; }, "ClearRenderTargetView -> CopyTextureRegion -> readback heap");
    return true;
}

// ---- draws --------------------------------------------------------------------------------------------
static ID3DBlob *build(const char *entry, const char *target) {
    ID3DBlob *code = nullptr, *err = nullptr;
    HRESULT hr = compile(k_hlsl, sizeof k_hlsl - 1, "ladder.hlsl", nullptr, nullptr, entry, target, 0, 0, &code, &err);
    if (FAILED(hr)) snprintf(g_err, sizeof g_err, "D3DCompile %s: %08lx %s", entry, (unsigned long)hr, err ? (const char *)err->GetBufferPointer() : "");
    REL(err);
    return code;
}

static bool draw_test() {
    ID3DBlob *vs1 = build("vs_full", "vs_5_0"), *ps1 = build("ps_quadrants", "ps_5_0"), *vs2 = build("vs_quad", "vs_5_0"),
             *ps2 = build("ps_color", "ps_5_0");
    if (!vs1 || !ps1 || !vs2 || !ps2) return false;

    D3D12_ROOT_PARAMETER rp = {};
    rp.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV; rp.Descriptor.ShaderRegister = 0;
    rp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC rsd = {1, &rp, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT};
    ID3DBlob *blob = nullptr, *err = nullptr;
    HR(serialize_rs(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err));
    ID3D12RootSignature *rs = nullptr;
    HR(dev->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), __uuidof(ID3D12RootSignature), (void **)&rs));
    REL(blob); REL(err);

    D3D12_INPUT_ELEMENT_DESC el = {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0};
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {};
    pd.pRootSignature = rs;
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pd.SampleMask = UINT_MAX;
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID; pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.RasterizerState.DepthClipEnable = TRUE;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 1; pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM; pd.SampleDesc.Count = 1;
    ID3D12PipelineState *pso1 = nullptr, *pso2 = nullptr;
    pd.VS = {vs1->GetBufferPointer(), vs1->GetBufferSize()}; pd.PS = {ps1->GetBufferPointer(), ps1->GetBufferSize()};
    HR(dev->CreateGraphicsPipelineState(&pd, __uuidof(ID3D12PipelineState), (void **)&pso1));
    pd.VS = {vs2->GetBufferPointer(), vs2->GetBufferSize()}; pd.PS = {ps2->GetBufferPointer(), ps2->GetBufferSize()};
    pd.InputLayout = {&el, 1};
    HR(dev->CreateGraphicsPipelineState(&pd, __uuidof(ID3D12PipelineState), (void **)&pso2));

    // upload heap: vertices at 0, constants at 256 (CBV alignment)
    ID3D12Resource *up = make_buffer(D3D12_HEAP_TYPE_UPLOAD, 512, D3D12_RESOURCE_STATE_GENERIC_READ);
    if (!up) { snprintf(g_err, sizeof g_err, "upload buffer creation failed"); return false; }
    uint8_t *p = nullptr;
    D3D12_RANGE none = {0, 0};
    HR(up->Map(0, &none, (void **)&p));
    memcpy(p, k_quad_verts, sizeof k_quad_verts);
    memcpy(p + 256, k_quad_color, 16);
    up->Unmap(0, nullptr);

    D3D12_VIEWPORT vp = {0, 0, (float)W, (float)H, 0, 1};
    D3D12_RECT sc = {0, 0, W, H};
    for (int pass = 0; pass < 2; pass++) {
        if (!reset_list()) return false;
        list->SetGraphicsRootSignature(rs);
        list->RSSetViewports(1, &vp); list->RSSetScissorRects(1, &sc);
        list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        list->SetGraphicsRootConstantBufferView(0, up->GetGPUVirtualAddress() + 256);
        if (pass == 0) {
            list->SetPipelineState(pso1);
            list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            list->DrawInstanced(3, 1, 0, 0);
        } else {
            list->SetPipelineState(pso2);
            list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
            D3D12_VERTEX_BUFFER_VIEW vbv = {up->GetGPUVirtualAddress(), sizeof k_quad_verts, 8};
            list->IASetVertexBuffers(0, 1, &vbv);
            list->DrawInstanced(4, 1, 0, 0);
        }
        barrier(list, rt, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION dst = buf_loc(rb), src = tex_loc(rt);
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        barrier(list, rt, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        if (!submit_and_wait(direct, list)) return false;
        if (pass == 0) check_readback("draw_fullscreen", rb, quadrants, "SV_VertexID triangle, PSO + root signature");
        else check_readback("draw_vb_cbv", rb, [](int x, int y) { return in_quad(x, y) ? k_quad : quadrants(x, y); },
                            "vertex-buffer quad, colour from a root CBV");
    }
    REL(up); REL(pso1); REL(pso2); REL(rs); REL(vs1); REL(ps1); REL(vs2); REL(ps2);
    return true;
}

// ---- COPY queue upload, DIRECT queue consumes after a fence wait --------------------------------------
static bool copy_queue_test() {
    ID3D12Resource *up = make_buffer(D3D12_HEAP_TYPE_UPLOAD, W * H * 4, D3D12_RESOURCE_STATE_GENERIC_READ);
    ID3D12Resource *tex = make_texture(D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COMMON, nullptr);
    ID3D12Resource *rb2 = make_buffer(D3D12_HEAP_TYPE_READBACK, W * H * 4, D3D12_RESOURCE_STATE_COPY_DEST);
    if (!up || !tex || !rb2) { snprintf(g_err, sizeof g_err, "resource creation failed"); return false; }
    uint8_t *p = nullptr;
    D3D12_RANGE none = {0, 0};
    HR(up->Map(0, &none, (void **)&p));
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) { RGBA c = pattern(x, y); memcpy(p + (y * W + x) * 4, &c, 4); }
    up->Unmap(0, nullptr);

    ID3D12Fence *xf = nullptr;
    HR(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), (void **)&xf));
    HR(copy_alloc->Reset());
    HR(copy_list->Reset(copy_alloc, nullptr));
    D3D12_TEXTURE_COPY_LOCATION src = buf_loc(up), dst = tex_loc(tex);
    copy_list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);   // COMMON promotes to COPY_DEST on a copy queue
    HR(copy_list->Close());

    // DIRECT: wait for the copy queue, then copy the texture out.
    if (!reset_list()) return false;
    D3D12_TEXTURE_COPY_LOCATION d2 = buf_loc(rb2), s2 = tex_loc(tex);
    list->CopyTextureRegion(&d2, 0, 0, 0, &s2, nullptr);
    HR(list->Close());

    direct->Wait(xf, 1);   // queued before the copy is even submitted
    ID3D12CommandList *dl[] = {list};
    direct->ExecuteCommandLists(1, dl);
    direct->Signal(fence, ++fence_val);
    Sleep(50);
    ID3D12CommandList *cl[] = {copy_list};
    copyq->ExecuteCommandLists(1, cl);
    copyq->Signal(xf, 1);
    if (!wait_fence(fence, fence_val)) { snprintf(g_err, sizeof g_err, "direct queue never finished after the cross-queue wait"); return false; }
    check_readback("copy_queue", rb2, pattern, "upload on the COPY queue, read on DIRECT after a cross-queue fence wait");

    // Buffer copy on the copy queue: upload -> default -> readback slice.
    ID3D12Resource *def = make_buffer(D3D12_HEAP_TYPE_DEFAULT, W * H * 4, D3D12_RESOURCE_STATE_COMMON);
    ID3D12Resource *rb3 = make_buffer(D3D12_HEAP_TYPE_READBACK, 4096, D3D12_RESOURCE_STATE_COPY_DEST);
    if (def && rb3) {
        HR(copy_alloc->Reset());
        HR(copy_list->Reset(copy_alloc, nullptr));
        copy_list->CopyBufferRegion(def, 0, up, 0, W * H * 4);
        HR(copy_list->Close());
        ID3D12CommandList *c2[] = {copy_list};
        copyq->ExecuteCommandLists(1, c2);
        copyq->Signal(xf, 2);
        if (!wait_fence(xf, 2)) { snprintf(g_err, sizeof g_err, "copy queue fence 2 never completed"); return false; }
        HR(copy_alloc->Reset());
        HR(copy_list->Reset(copy_alloc, nullptr));
        copy_list->CopyBufferRegion(rb3, 0, def, 8192, 4096);   // rows 32..35
        HR(copy_list->Close());
        copyq->ExecuteCommandLists(1, c2);
        copyq->Signal(xf, 3);
        if (!wait_fence(xf, 3)) { snprintf(g_err, sizeof g_err, "copy queue fence 3 never completed"); return false; }
        uint8_t *q = nullptr;
        D3D12_RANGE rr = {0, 4096};
        int bad = -1;
        if (SUCCEEDED(rb3->Map(0, &rr, (void **)&q))) {
            bad = 0;
            for (int i = 0; i < 1024; i++) {
                int pix = 2048 + i;
                RGBA c = pattern(pix % W, pix / W);
                bad += memcmp(q + i * 4, &c, 4) != 0;
            }
            rb3->Unmap(0, &none);
        }
        check(bad == 0, "copy_queue.buffers", "upload -> default -> readback buffer copies on the COPY queue: %d of 1024 texels wrong", bad);
    }
    REL(def); REL(rb3); REL(xf); REL(up); REL(tex); REL(rb2);
    return true;
}

int main(int argc, char **argv) {
    ladder_begin("d3d12", argc, argv);
    HMODULE lib = LoadLibraryA("d3d12.dll");
    auto create = lib ? (PFN_D3D12_CREATE_DEVICE)(void *)GetProcAddress(lib, "D3D12CreateDevice") : nullptr;
    serialize_rs = lib ? (PFN_D3D12_SERIALIZE_ROOT_SIGNATURE)(void *)GetProcAddress(lib, "D3D12SerializeRootSignature") : nullptr;
    if (!create || !serialize_rs) { skip("device", "no d3d12.dll in this stack (wined3d tree?)"); return ladder_done(); }
    HRESULT hr = create(nullptr, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), (void **)&dev);
    if (!check(SUCCEEDED(hr), "device", "D3D12CreateDevice(11_0) %08lx", (unsigned long)hr)) return ladder_done();

    char name[128] = "?";
    typedef HRESULT(WINAPI * CreateDXGIFactory1_t)(REFIID, void **);
    HMODULE dxgi = LoadLibraryA("dxgi.dll");
    auto cf = dxgi ? (CreateDXGIFactory1_t)(void *)GetProcAddress(dxgi, "CreateDXGIFactory1") : nullptr;
    IDXGIFactory4 *fac = nullptr; IDXGIAdapter1 *ad = nullptr;
    if (cf && SUCCEEDED(cf(__uuidof(IDXGIFactory4), (void **)&fac)) &&
        SUCCEEDED(fac->EnumAdapterByLuid(dev->GetAdapterLuid(), __uuidof(IDXGIAdapter1), (void **)&ad))) {
        DXGI_ADAPTER_DESC1 d = {}; ad->GetDesc1(&d); narrow(d.Description, name, sizeof name);
    }
    REL(ad); REL(fac);
    D3D12_FEATURE_DATA_FEATURE_LEVELS fls = {};
    D3D_FEATURE_LEVEL req[] = {D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_0, D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    fls.NumFeatureLevels = 4; fls.pFeatureLevelsRequested = req;
    dev->CheckFeatureSupport(D3D12_FEATURE_FEATURE_LEVELS, &fls, sizeof fls);
    info("adapter", "%s, max feature level %s", name, fl_name(fls.MaxSupportedFeatureLevel));

    D3D12_COMMAND_QUEUE_DESC qd = {D3D12_COMMAND_LIST_TYPE_DIRECT, 0, D3D12_COMMAND_QUEUE_FLAG_NONE, 0};
    dev->CreateCommandQueue(&qd, __uuidof(ID3D12CommandQueue), (void **)&direct);
    qd.Type = D3D12_COMMAND_LIST_TYPE_COPY;
    dev->CreateCommandQueue(&qd, __uuidof(ID3D12CommandQueue), (void **)&copyq);
    dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator), (void **)&alloc);
    dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COPY, __uuidof(ID3D12CommandAllocator), (void **)&copy_alloc);
    if (alloc) dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr, __uuidof(ID3D12GraphicsCommandList), (void **)&list);
    if (copy_alloc) dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COPY, copy_alloc, nullptr, __uuidof(ID3D12GraphicsCommandList), (void **)&copy_list);
    dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), (void **)&fence);
    ev = CreateEventA(NULL, FALSE, FALSE, NULL);
    if (!check(direct && copyq && list && copy_list && fence, "queues", "direct + copy queues, lists and a fence %s",
               direct && copyq && list && copy_list && fence ? "created" : "NOT all created"))
        return ladder_done();
    list->Close(); copy_list->Close();

    fence_checks();
    if (!clear_test()) fail("clear", "%s", g_err);
    else {
        const char *which;
        compile = load_d3dcompile(&which);
        if (!compile) skip("draw", "no d3dcompiler DLL");
        else if (!draw_test()) fail("draw", "%s", g_err);
    }
    if (!copy_queue_test()) fail("copy_queue", "%s", g_err);

    REL(rt); REL(rb); REL(rtv_heap); REL(list); REL(copy_list); REL(alloc); REL(copy_alloc); REL(fence);
    REL(direct); REL(copyq);
    ULONG refs = dev->Release();
    info("device_release", "final Release returned %lu", refs);
    return ladder_done();
}
