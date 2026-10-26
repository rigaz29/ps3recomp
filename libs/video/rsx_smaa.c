/*
 * ps3recomp - SMAA 1x for the D3D12 presentation pass
 *
 * SMAA (third_party/smaa, MIT), unchanged: SMAA.hlsl is compiled here with
 * D3DCompile, served from the copy the build embeds in the library, and
 * AreaTex.h and SearchTex.h, its lookup textures, are compiled in. SMAA.hlsl
 * is used through SMAA_CUSTOM_SL, so its two samplers can be given the
 * registers of the presentation pass's static samplers. The backend
 * (rsx_d3d12_backend.c) owns the targets and the pipelines and runs the three
 * passes.
 */
#ifdef _WIN32

#include <windows.h>
#include <d3dcompiler.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rsx_smaa.h"

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#elif defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#include "../../third_party/smaa/AreaTex.h"
#include "../../third_party/smaa/SearchTex.h"
#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(_MSC_VER)
#pragma warning(pop)
#endif

/* cmake/EmbedFiles.cmake, from third_party/smaa */
extern const unsigned char rsx_smaa_hlsl[];
extern const unsigned int rsx_smaa_hlsl_len;

const unsigned char* rsx_smaa_area(unsigned* w, unsigned* h, unsigned* bpp)
{
    *w = AREATEX_WIDTH; *h = AREATEX_HEIGHT; *bpp = 2;
    return areaTexBytes;
}

const unsigned char* rsx_smaa_search(unsigned* w, unsigned* h, unsigned* bpp)
{
    *w = SEARCHTEX_WIDTH; *h = SEARCHTEX_HEIGHT; *bpp = 1;
    return searchTexBytes;
}

static HRESULT STDMETHODCALLTYPE smaa_open(ID3DInclude* self, D3D_INCLUDE_TYPE type, LPCSTR name,
                                           LPCVOID parent, LPCVOID* data, UINT* bytes)
{
    (void)self; (void)type; (void)parent;
    if (strcmp(name, "SMAA.hlsl")) return E_FAIL;
    *data = rsx_smaa_hlsl;
    *bytes = rsx_smaa_hlsl_len;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE smaa_close(ID3DInclude* self, LPCVOID data)
{
    (void)self; (void)data;
    return S_OK;
}
static ID3DIncludeVtbl s_smaa_include_vtbl = { smaa_open, smaa_close };
static ID3DInclude s_smaa_include = { &s_smaa_include_vtbl };

/* SMAA's porting macros for HLSL 5 with the pass's samplers (s0 linear, s1
 * point, both clamped), then the three passes. Every vertex output SMAA
 * computes is affine in the texture coordinate, so one triangle over the
 * target carries them exactly. */
static const char s_src[] =
    "cbuffer pc : register(b0) { float4 rt; float4 p1; float4 p2; float4 p3; };\n"
    "#define SMAA_RT_METRICS rt\n"
    "#define SMAA_PRESET_HIGH 1\n"
    "#define SMAA_CUSTOM_SL 1\n"
    "SamplerState LinearSampler : register(s0);\n"
    "SamplerState PointSampler : register(s1);\n"
    "#define SMAATexture2D(tex) Texture2D tex\n"
    "#define SMAATexturePass2D(tex) tex\n"
    "#define SMAASampleLevelZero(tex, coord) tex.SampleLevel(LinearSampler, coord, 0)\n"
    "#define SMAASampleLevelZeroPoint(tex, coord) tex.SampleLevel(PointSampler, coord, 0)\n"
    "#define SMAASampleLevelZeroOffset(tex, coord, offset) tex.SampleLevel(LinearSampler, coord, 0, offset)\n"
    "#define SMAASample(tex, coord) tex.Sample(LinearSampler, coord)\n"
    "#define SMAASamplePoint(tex, coord) tex.Sample(PointSampler, coord)\n"
    "#define SMAASampleOffset(tex, coord, offset) tex.Sample(LinearSampler, coord, offset)\n"
    "#define SMAA_FLATTEN [flatten]\n"
    "#define SMAA_BRANCH [branch]\n"
    "#define SMAAGather(tex, coord) tex.Gather(LinearSampler, coord, 0)\n"
    "#include \"SMAA.hlsl\"\n"
    "Texture2D t0 : register(t0);\n"
    "Texture2D t1 : register(t1);\n"
    "Texture2D t2 : register(t2);\n"
    "float2 tri(uint id, out float4 pos) {\n"
    "  float2 t = float2((id << 1) & 2, id & 2);\n"
    "  pos = float4(t * float2(2, -2) + float2(-1, 1), 0, 1); return t; }\n"
    "struct VE { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; float4 off[3] : TEXCOORD1; };\n"
    "VE vs_edge(uint id : SV_VertexID) { VE o; o.uv = tri(id, o.pos); SMAAEdgeDetectionVS(o.uv, o.off); return o; }\n"
    "float4 ps_edge(VE i) : SV_TARGET { return float4(SMAALumaEdgeDetectionPS(i.uv, i.off, t0), 0, 0); }\n"
    "struct VW { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; float2 pix : TEXCOORD1; float4 off[3] : TEXCOORD2; };\n"
    "VW vs_weight(uint id : SV_VertexID) { VW o; o.uv = tri(id, o.pos); SMAABlendingWeightCalculationVS(o.uv, o.pix, o.off); return o; }\n"
    "float4 ps_weight(VW i) : SV_TARGET { return SMAABlendingWeightCalculationPS(i.uv, i.pix, i.off, t0, t1, t2, float4(0, 0, 0, 0)); }\n"
    "struct VN { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; float4 off : TEXCOORD1; };\n"
    "VN vs_blend(uint id : SV_VertexID) { VN o; o.uv = tri(id, o.pos); SMAANeighborhoodBlendingVS(o.uv, o.off); return o; }\n"
    "float4 ps_blend(VN i) : SV_TARGET { return SMAANeighborhoodBlendingPS(i.uv, i.off, t0, t1); }\n";

void* rsx_smaa_compile(int pass, int vs)
{
    static const char* const vs_entry[3] = { "vs_edge", "vs_weight", "vs_blend" };
    static const char* const ps_entry[3] = { "ps_edge", "ps_weight", "ps_blend" };
    if (pass < 0 || pass > 2) return NULL;
    const char* entry = vs ? vs_entry[pass] : ps_entry[pass];
    ID3DBlob *code = NULL, *err = NULL;
    const HRESULT hr = D3DCompile(s_src, sizeof s_src - 1, "smaa", NULL, &s_smaa_include, entry,
                                  vs ? "vs_5_0" : "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err);
    if (FAILED(hr)) {
        printf("[D3D12] SMAA %s: %s\n", entry, err ? (const char*)err->lpVtbl->GetBufferPointer(err) : "compile failed");
        if (code) code->lpVtbl->Release(code);
        code = NULL;
    }
    if (err) err->lpVtbl->Release(err);
    return code;
}

#endif /* _WIN32 */
