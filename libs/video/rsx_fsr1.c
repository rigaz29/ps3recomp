/*
 * ps3recomp - AMD FidelityFX FSR 1 for the D3D12 presentation pass
 *
 * EASU (the edge-adaptive upscale) and RCAS (the sharpening pass) come from
 * AMD's FSR 1 headers, unchanged (third_party/fidelityfx_fsr1, MIT). Their
 * constants are computed here with the headers' own CPU code, and their
 * shaders are compiled here, with the two headers served to D3DCompile from
 * the copies the build embeds in the library. The backend
 * (rsx_d3d12_backend.c) owns the pipelines and runs the passes.
 */
#ifdef _WIN32

#include <windows.h>
#include <d3dcompiler.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rsx_fsr1.h"

/* The headers' CPU half: FsrEasuCon and FsrRcasCon. */
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#elif defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#define A_CPU 1
#include "../../third_party/fidelityfx_fsr1/ffx_a.h"
#include "../../third_party/fidelityfx_fsr1/ffx_fsr1.h"
#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(_MSC_VER)
#pragma warning(pop)
#endif

/* cmake/EmbedFiles.cmake, from third_party/fidelityfx_fsr1 */
extern const unsigned char rsx_fsr1_ffx_a_h[];
extern const unsigned int rsx_fsr1_ffx_a_h_len;
extern const unsigned char rsx_fsr1_ffx_fsr1_h[];
extern const unsigned int rsx_fsr1_ffx_fsr1_h_len;

void rsx_fsr1_easu_con(uint32_t con[16], float in_w, float in_h, float out_w, float out_h)
{
    FsrEasuCon(con, con + 4, con + 8, con + 12, in_w, in_h, in_w, in_h, out_w, out_h);
}

void rsx_fsr1_rcas_con(uint32_t con[4], float stops)
{
    FsrRcasCon(con, stops);
}

/* #include "ffx_a.h" and "ffx_fsr1.h", from memory. */
static HRESULT STDMETHODCALLTYPE fsr1_open(ID3DInclude* self, D3D_INCLUDE_TYPE type, LPCSTR name,
                                           LPCVOID parent, LPCVOID* data, UINT* bytes)
{
    (void)self; (void)type; (void)parent;
    if (!strcmp(name, "ffx_a.h")) {
        *data = rsx_fsr1_ffx_a_h; *bytes = rsx_fsr1_ffx_a_h_len;
        return S_OK;
    }
    if (!strcmp(name, "ffx_fsr1.h")) {
        *data = rsx_fsr1_ffx_fsr1_h; *bytes = rsx_fsr1_ffx_fsr1_h_len;
        return S_OK;
    }
    return E_FAIL;
}
static HRESULT STDMETHODCALLTYPE fsr1_close(ID3DInclude* self, LPCVOID data)
{
    (void)self; (void)data;
    return S_OK;
}
static ID3DIncludeVtbl s_fsr1_include_vtbl = { fsr1_open, fsr1_close };
static ID3DInclude s_fsr1_include = { &s_fsr1_include_vtbl };

/* The 32-bit forms (FsrEasuF, FsrRcasF): ps_5_0 has no 16-bit types. */
static const char s_head[] =
    "#define A_GPU 1\n"
    "#define A_HLSL 1\n"
    "#include \"ffx_a.h\"\n"
    "cbuffer pc : register(b0) { uint4 con0; uint4 con1; uint4 con2; uint4 con3; };\n"
    "Texture2D<float4> src : register(t0);\n"
    "SamplerState lin : register(s0);\n"
    "struct V { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };\n";
static const char s_easu[] =
    "#define FSR_EASU_F 1\n"
    "AF4 FsrEasuRF(AF2 p) { return src.GatherRed(lin, p); }\n"
    "AF4 FsrEasuGF(AF2 p) { return src.GatherGreen(lin, p); }\n"
    "AF4 FsrEasuBF(AF2 p) { return src.GatherBlue(lin, p); }\n"
    "#include \"ffx_fsr1.h\"\n"
    "float4 main(V i) : SV_TARGET {\n"
    "  AF3 c; FsrEasuF(c, AU2(i.pos.xy), con0, con1, con2, con3); return float4(c, 1); }\n";
static const char s_rcas[] =
    "#define FSR_RCAS_F 1\n"
    "AF4 FsrRcasLoadF(ASU2 p) { return src.Load(int3(p, 0)); }\n"
    "void FsrRcasInputF(inout AF1 r, inout AF1 g, inout AF1 b) {}\n"
    "#include \"ffx_fsr1.h\"\n"
    "float4 main(V i) : SV_TARGET {\n"
    "  AF3 c; FsrRcasF(c.r, c.g, c.b, AU2(i.pos.xy), con0); return float4(c, 1); }\n";

void* rsx_fsr1_compile(int pass)
{
    const char* body = pass ? s_rcas : s_easu;
    const size_t n = sizeof s_head - 1 + strlen(body);
    char* src = (char*)malloc(n + 1);
    if (!src) return NULL;
    memcpy(src, s_head, sizeof s_head - 1);
    memcpy(src + sizeof s_head - 1, body, strlen(body) + 1);
    ID3DBlob *code = NULL, *err = NULL;
    const HRESULT hr = D3DCompile(src, n, pass ? "fsr1_rcas" : "fsr1_easu", NULL, &s_fsr1_include,
                                  "main", "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err);
    free(src);
    if (FAILED(hr)) {
        printf("[D3D12] FSR 1 %s: %s\n", pass ? "RCAS" : "EASU",
               err ? (const char*)err->lpVtbl->GetBufferPointer(err) : "compile failed");
        if (code) code->lpVtbl->Release(code);
        code = NULL;
    }
    if (err) err->lpVtbl->Release(err);
    return code;
}

#endif /* _WIN32 */
