// mpbench shaders. One source, three consumers:
//   D3D11            compiled at runtime with D3DCompile (vs_5_0/ps_5_0), as before
//   D3D12 (default)  precompiled DXIL, SM 6.0, by dxc -> bench/mpbench_shaders.h
//   D3D12 --d3d12-shaders dxbc   compiled at runtime with D3DCompile, SM 5.0 (-D MP_FEAT_*)
//   Vulkan           precompiled SPIR-V 1.0 by dxc -spirv -D MP_VK -> bench/mpbench_shaders.h
// After editing, run bench/gen_shaders.py (needs dxc) and commit the regenerated header.

#ifdef MP_VK
struct PerDrawData { float4 xform_; float4 color_; };
[[vk::push_constant]] PerDrawData pd;
#define xform pd.xform_
#define color pd.color_
#else
cbuffer PerDraw : register(b0) { float4 xform; float4 color; };
#endif

struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; float4 col : COLOR0; };

// ---- drawcalls: small triangle, position/colour from the per-draw constants
VSOut vs_quad(uint id : SV_VertexID) {
    float2 c = float2((id << 1) & 2, id & 2);
    VSOut o; o.uv = c; o.pos = float4(c * xform.zw + xform.xy, 0, 1); o.col = color; return o;
}
float4 ps_flat(VSOut i) : SV_Target { return i.col; }

// ---- fill: fullscreen triangle, ALU-heavy pixel shader
VSOut vs_full(uint id : SV_VertexID) {
    float2 c = float2((id << 1) & 2, id & 2);
    VSOut o; o.uv = c; o.pos = float4(c * float2(2, -2) + float2(-1, 1), 0, 1); o.col = color; return o;
}
float4 ps_heavy(VSOut i) : SV_Target {
    float2 z = float2(0, 0); float2 c = i.uv * float2(1.5, 1.0) - float2(1.2, 0.5) + color.xy * 0.001;
    [loop] for (int k = 0; k < 96; k++) z = float2(z.x * z.x - z.y * z.y, 2 * z.x * z.y) + c;
    return float4(frac(z * 0.1), 0.5, 1);
}

// ---- feature probes (D3D12 only). Each sits behind its own MP_FEAT_* macro so a D3DCompile
//      that lacks e.g. GS syntax (Wine 9's) fails only that probe, not the whole file.
#if defined(MP_FEAT_GS) || defined(MP_FEAT_TESS) || defined(MP_FEAT_COMMON)
// Constants: xform.x = grid size / tess factor, xform.y = quad half-size / disk radius,
//            xform.z = tess patch grid,
//            color = output colour.
struct PtOut { float4 pos : SV_Position; float4 col : COLOR0; };
float4 ps_col(PtOut i) : SV_Target { return i.col; }
#endif

#ifdef MP_FEAT_GS
// Geometry shader: a G x G grid of points, each expanded to a quad.
struct GsIn { float2 center : TEXCOORD0; float4 col : COLOR0; };
GsIn vs_point(uint id : SV_VertexID) {
    uint g = uint(xform.x);
    GsIn o; o.center = (float2(id % g, id / g) + 0.5) / g * 2 - 1; o.col = color; return o;
}
[maxvertexcount(4)]
void gs_quad(point GsIn p[1], inout TriangleStream<PtOut> s) {
    float h = xform.y; PtOut o; o.col = p[0].col;
    o.pos = float4(p[0].center + float2(-h, -h), 0, 1); s.Append(o);
    o.pos = float4(p[0].center + float2(-h,  h), 0, 1); s.Append(o);
    o.pos = float4(p[0].center + float2( h, -h), 0, 1); s.Append(o);
    o.pos = float4(p[0].center + float2( h,  h), 0, 1); s.Append(o);
}
#endif

#ifdef MP_FEAT_SO
// Stream output: every vertex writes a deterministic float4 the CPU can check.
struct SoOut { float4 pos : SV_Position; float4 data : TEXCOORD0; };
SoOut vs_so(uint id : SV_VertexID) {
    float f = float(id); SoOut o; o.pos = float4(0, 0, 0, 1);
    o.data = float4(f, f * 0.5, 1000.0 - f, color.x); return o;
}
#endif

#ifdef MP_FEAT_TESS
// Tessellation: one quad-domain patch per 4 vertices, uniform integer factor;
// the domain shader wraps the (u, v) grid into a disk. Untessellated, it collapses
// to nothing, so coverage of the disk proves the tessellator ran.
struct CP { float2 uv : TEXCOORD0; };
CP vs_patch(uint id : SV_VertexID) { CP o; o.uv = float2(id & 1, (id >> 1) & 1); return o; }
struct HSC { float e[4] : SV_TessFactor; float i[2] : SV_InsideTessFactor; };
HSC hs_const(InputPatch<CP, 4> p) {
    HSC o; float t = xform.x;
    o.e[0] = t; o.e[1] = t; o.e[2] = t; o.e[3] = t; o.i[0] = t; o.i[1] = t; return o;
}
[domain("quad")] [partitioning("integer")] [outputtopology("triangle_cw")]
[outputcontrolpoints(4)] [patchconstantfunc("hs_const")] [maxtessfactor(64.0)]
CP hs_main(InputPatch<CP, 4> p, uint i : SV_OutputControlPointID) { return p[i]; }
// xform.z = 0: one disk centred on the target; xform.z = G: patch i is a small disk in cell i of a G x G grid.
[domain("quad")]
PtOut ds_disk(HSC c, float2 uv : SV_DomainLocation, const OutputPatch<CP, 4> p, uint pid : SV_PrimitiveID) {
    float2 center = float2(0, 0); float radius = xform.y;
    if (xform.z > 0) {
        uint g = uint(xform.z);
        center = (float2(pid % g, pid / g) + 0.5) / g * 2 - 1; radius = xform.y / g;
    }
    float a = uv.x * 6.2831853; float r = uv.y * radius;
    PtOut o; o.pos = float4(center + r * float2(cos(a), sin(a)), 0, 1); o.col = color; return o;
}
#endif
