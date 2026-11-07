/*
 * ps3recomp - AMD FSR 3.1 through the FSR API
 *
 * The API's three DLLs come with the FSR SDK and are looked for beside the exe:
 * amd_fidelityfx_loader_dx12.dll, which finds the effects' next to itself. On a
 * GPU that is not AMD's it selects Super Resolution 3.1.6, Frame Generation
 * 3.1.6 and its swapchain 3.1.7. Without them the backend stays as it is.
 *
 * C++, as the API's DX12 header is. Its headers are in third_party/fsr_api.
 */
#ifdef _WIN32

#include <windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_5.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "../../third_party/fsr_api/api/include/ffx_api.h"
#include "../../third_party/fsr_api/api/include/ffx_api_loader.h"
#include "../../third_party/fsr_api/api/include/dx12/ffx_api_dx12.h"
#include "../../third_party/fsr_api/framegeneration/include/ffx_framegeneration.h"
#include "../../third_party/fsr_api/framegeneration/include/dx12/ffx_api_framegeneration_dx12.h"
#include "../../third_party/fsr_api/upscalers/include/ffx_upscale.h"

#include "rsx_fsr3.h"

static ffxFunctions s_ffx;
static ffxContext s_swap_ctx;   /* the proxy's */
static ffxContext s_fg_ctx;     /* frame generation's */
static ffxContext s_up_ctx;     /* the upscaler's */
static float s_up_sharp;
static int s_up_phases;
static void* s_swap;
static int s_generate;
static unsigned s_dw, s_dh, s_rw, s_rh;
static uint64_t s_frame_id;
static ID3D12Resource* s_depth;     /* R32_FLOAT at the render's size */
static ID3D12Resource* s_motion;    /* R16G16_FLOAT, in UV units, from a pixel to where it was */

/* The inputs' pass: see fsr3_inputs. */
static ID3D12RootSignature* s_in_root;
static ID3D12PipelineState* s_in_pso;
static ID3D12DescriptorHeap* s_in_heap;
static ID3D12Resource* s_in_src;    /* what the heap's SRVs show */
static ID3D12Resource* s_in_obj;
static ID3D12Device* s_dev;

/* The camera, from the matrix a scene draws with. */
struct fsr3_camera {
    double m[4][4];                 /* row, column */
    double pos[3], right[3], up[3], fwd[3];
    double znear, zfar, fov;
};
/* A cut, in one frame. The title's camera at most turns 2.1 degrees and moves 1.6 units, near
 * being 0.25; its cuts move 7.7 and 502 units, the first with a third of the field of view. */
#define FSR3_CUT_COS  0.966      /* 15 degrees */
#define FSR3_CUT_MOVE 20.0       /* near planes: the one length a matrix gives */
#define FSR3_CUT_FOV  0.1
#define FSR3_FLOOR_BACK 2.0
static fsr3_camera s_prev;
static bool s_prev_ok;              /* the last frame was a scene */
static fsr3_camera s_cam;           /* this frame's, from rsx_fsr3_prepare */
static bool s_on, s_reset, s_prepared;
static bool s_fg_was;               /* generation ran on the last frame */
static double s_floor, s_rate_ms;   /* see fsr3_rate */
static int s_rate_n, s_rate_streak;
static bool s_under;

int rsx_fsr3_available(void)
{
    static int tried;
    if (tried) return s_ffx.CreateContext != nullptr;
    tried = 1;
    wchar_t p[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, p, MAX_PATH);
    while (n && p[n - 1] != L'\\' && p[n - 1] != L'/') n--;
    if (!n || n + 40 >= MAX_PATH) return 0;
    wcscpy(p + n, L"amd_fidelityfx_loader_dx12.dll");
    HMODULE m = LoadLibraryW(p);
    if (!m) return 0;
    ffxLoadFunctions(&s_ffx, m);
    return s_ffx.CreateContext != nullptr;
}

static void fsr3_message(uint32_t type, const wchar_t* message)
{
    fprintf(stderr, "[fsr3] %s: %ls\n", type == FFX_API_MESSAGE_TYPE_ERROR ? "error" : "warning", message);
}

static void fsr3_version(ffxContext* ctx, const char* what)
{
    ffxQueryGetProviderVersion q{};
    q.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
    if (s_ffx.Query(ctx, &q.header) == FFX_API_RETURN_OK && q.versionName)
        printf("[fsr3] %s: %s\n", what, q.versionName);
}

/* Left alone, the swapchain's two threads spin: 113% of a logical CPU at the gate.
 * Sleeping for all but two timer units and waiting on the fences takes that to 25%,
 * with the same pacing: 97.4% of the presents within 3 ms of 16.7, against 96.8%.
 * D3D12_FG_SPIN=0 leaves them spinning, another number spins that long. */
static void fsr3_pacing(void)
{
    const char* e = getenv("D3D12_FG_SPIN");
    const int spin = e ? atoi(e) : 2;
    if (!spin) return;
    FfxApiSwapchainFramePacingTuning t{};
    t.safetyMarginInMs = 0.1f;
    t.varianceFactor = 0.1f;
    t.allowHybridSpin = true;
    t.hybridSpinTime = (uint32_t)spin;
    t.allowWaitForSingleObjectOnFence = true;
    ffxConfigureDescFrameGenerationSwapChainKeyValueDX12 k{};
    k.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_KEYVALUE_DX12;
    k.key = FFX_API_CONFIGURE_FG_SWAPCHAIN_KEY_FRAMEPACINGTUNING;
    k.ptr = &t;
    if (s_ffx.Configure(&s_swap_ctx, &k.header) != FFX_API_RETURN_OK)
        printf("[fsr3] the pacing could not be tuned\n");
}

int rsx_fsr3_wrap(void** swap_chain, void* queue)
{
    if (!rsx_fsr3_available()) return 0;
    IDXGISwapChain3* sc3 = (IDXGISwapChain3*)*swap_chain;
    IDXGISwapChain4* sc = nullptr;
    if (FAILED(sc3->QueryInterface(IID_PPV_ARGS(&sc)))) return 0;
    sc3->Release();                         /* sc is the only reference now */

    ffxCreateContextDescFrameGenerationSwapChainVersionDX12 ver{};
    ver.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_VERSION_DX12;
    ver.version = FFX_FRAMEGENERATION_SWAPCHAIN_DX12_VERSION;
    ffxCreateContextDescFrameGenerationSwapChainWrapDX12 d{};
    d.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_WRAP_DX12;
    d.header.pNext = &ver.header;
    d.swapchain = &sc;
    d.gameQueue = (ID3D12CommandQueue*)queue;
    const ffxReturnCode_t rc = s_ffx.CreateContext(&s_swap_ctx, &d.header, nullptr);
    *swap_chain = sc;
    if (rc != FFX_API_RETURN_OK) {
        printf("[fsr3] the swapchain could not be wrapped (%u)\n", (unsigned)rc);
        s_swap_ctx = nullptr;
        return 0;
    }
    s_swap = sc;
    fsr3_version(&s_swap_ctx, "swapchain");
    fsr3_pacing();
    return 1;
}

/* The depth into the format the API takes, and the camera's motion at every pixel:
 * back to the world through this frame's matrix, forward through last frame's. That
 * is right for what stands still; what moves comes from the title, drawn into objects. */
static const char s_in_hlsl[] =
    "Texture2D<float> depth_in : register(t0);\n"
    "Texture2D<float2> objects : register(t1);\n"
    "RWTexture2D<float> depth_out : register(u0);\n"
    "RWTexture2D<float2> motion_out : register(u1);\n"
    "cbuffer C : register(b0) { float4 r0, r1, r2, r3; float4 k; float4 k2; };\n"
    "[numthreads(8, 8, 1)]\n"
    "void main(uint3 id : SV_DispatchThreadID) {\n"
    "    uint w, h;\n"
    "    depth_out.GetDimensions(w, h);\n"
    "    if (id.x >= w || id.y >= h) return;\n"
    "    float d = depth_in[id.xy];\n"
    "    depth_out[id.xy] = d;\n"
    "    float2 uv = (float2(id.xy) + 0.5) * k.zw;\n"
    "    float4 n = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, (d - k.y) / k.x, 1.0);\n"
    "    float4 p = float4(dot(r0, n), dot(r1, n), dot(r2, n), dot(r3, n));\n"
    "    float2 was = float2(p.x / p.w * 0.5 + 0.5, 0.5 - p.y / p.w * 0.5);\n"
    "    float2 m = p.w > 0.0 ? was - uv : float2(0.0, 0.0);\n"
    "    if (k2.x != 0.0) {\n"
    "        float2 o = objects[id.xy];\n"
    "        if (o.x < 30000.0) m = o;\n"
    "    }\n"
    "    motion_out[id.xy] = m;\n"
    "}\n";

static ID3D12Resource* fsr3_target(ID3D12Device* dev, DXGI_FORMAT fmt, unsigned w, unsigned h)
{
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = w;
    rd.Height = h;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.Format = fmt;
    rd.SampleDesc.Count = 1;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    ID3D12Resource* r = nullptr;
    dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                 D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                 nullptr, IID_PPV_ARGS(&r));
    return r;
}

static int fsr3_inputs_init(ID3D12Device* dev)
{
    if (s_in_pso) return 1;
    ID3DBlob* cs = nullptr;
    ID3DBlob* err = nullptr;
    if (FAILED(D3DCompile(s_in_hlsl, sizeof s_in_hlsl - 1, "fsr3_inputs", nullptr, nullptr, "main", "cs_5_0", 0, 0, &cs, &err))) {
        printf("[fsr3] inputs' pass: %s\n", err ? (const char*)err->GetBufferPointer() : "?");
        return 0;
    }
    D3D12_DESCRIPTOR_RANGE range[2]{};
    range[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range[0].NumDescriptors = 2;
    range[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    range[1].NumDescriptors = 2;
    D3D12_ROOT_PARAMETER rp[3]{};
    rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rp[0].DescriptorTable.NumDescriptorRanges = 1;
    rp[0].DescriptorTable.pDescriptorRanges = &range[0];
    rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rp[1].DescriptorTable.NumDescriptorRanges = 1;
    rp[1].DescriptorTable.pDescriptorRanges = &range[1];
    rp[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rp[2].Constants.Num32BitValues = 24;
    D3D12_ROOT_SIGNATURE_DESC rd{};
    rd.NumParameters = 3;
    rd.pParameters = rp;
    ID3DBlob* sig = nullptr;
    if (FAILED(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err)) ||
        FAILED(dev->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&s_in_root))))
        return 0;
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = s_in_root;
    pd.CS.pShaderBytecode = cs->GetBufferPointer();
    pd.CS.BytecodeLength = cs->GetBufferSize();
    if (FAILED(dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(&s_in_pso)))) return 0;
    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = 4;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&s_in_heap)))) return 0;
    const UINT inc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE h = s_in_heap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += inc;
    D3D12_SHADER_RESOURCE_VIEW_DESC none{};
    none.Format = DXGI_FORMAT_R16G16_FLOAT;
    none.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    none.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    none.Texture2D.MipLevels = 1;
    dev->CreateShaderResourceView(nullptr, &none, h);
    h.ptr += inc;
    dev->CreateUnorderedAccessView(s_depth, nullptr, nullptr, h);
    h.ptr += inc;
    dev->CreateUnorderedAccessView(s_motion, nullptr, nullptr, h);
    return 1;
}

static void fsr3_barrier(ID3D12GraphicsCommandList* cl, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to)
{
    D3D12_RESOURCE_BARRIER b[2]{};
    ID3D12Resource* const r[2] = { s_depth, s_motion };
    for (int i = 0; i < 2; i++) {
        b[i].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b[i].Transition.pResource = r[i];
        b[i].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b[i].Transition.StateBefore = from;
        b[i].Transition.StateAfter = to;
    }
    cl->ResourceBarrier(2, b);
}

/* reproject: last frame's matrix after the inverse of this frame's, by rows. */
static void fsr3_inputs(ID3D12GraphicsCommandList* cl, ID3D12Resource* depth, ID3D12Resource* objects,
                        const double reproject[4][4], float z_scale, float z_offset)
{
    if (depth != s_in_src) {
        D3D12_SHADER_RESOURCE_VIEW_DESC v{};
        v.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
        v.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        v.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        v.Texture2D.MipLevels = 1;
        s_dev->CreateShaderResourceView(depth, &v, s_in_heap->GetCPUDescriptorHandleForHeapStart());
        s_in_src = depth;
    }
    if (objects && objects != s_in_obj) {
        D3D12_CPU_DESCRIPTOR_HANDLE h = s_in_heap->GetCPUDescriptorHandleForHeapStart();
        h.ptr += s_dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        s_dev->CreateShaderResourceView(objects, nullptr, h);
        s_in_obj = objects;
    }
    float c[24] = {};
    for (int r = 0; r < 4; r++)
        for (int k = 0; k < 4; k++) c[r * 4 + k] = (float)reproject[r][k];
    c[16] = z_scale;
    c[17] = z_offset;
    c[18] = 1.0f / (float)s_rw;
    c[19] = 1.0f / (float)s_rh;
    c[20] = objects ? 1.0f : 0.0f;
    const D3D12_RESOURCE_STATES read = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    fsr3_barrier(cl, read, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    cl->SetDescriptorHeaps(1, &s_in_heap);
    cl->SetComputeRootSignature(s_in_root);
    cl->SetPipelineState(s_in_pso);
    D3D12_GPU_DESCRIPTOR_HANDLE g = s_in_heap->GetGPUDescriptorHandleForHeapStart();
    cl->SetComputeRootDescriptorTable(0, g);
    g.ptr += 2 * s_dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    cl->SetComputeRootDescriptorTable(1, g);
    cl->SetComputeRoot32BitConstants(2, 24, c, 0);
    cl->Dispatch((s_rw + 7) / 8, (s_rh + 7) / 8, 1);
    fsr3_barrier(cl, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, read);
}

static void v3_norm(double v[3])
{
    const double n = sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (n > 0.0) { v[0] /= n; v[1] /= n; v[2] /= n; }
}

/* 4x4 inverse by Gauss-Jordan, in doubles: a far pixel's motion is a small difference. */
static bool m4_inverse(const double a[4][4], double out[4][4])
{
    double t[4][8];
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) { t[r][c] = a[r][c]; t[r][c + 4] = r == c ? 1.0 : 0.0; }
    for (int c = 0; c < 4; c++) {
        int p = c;
        for (int r = c + 1; r < 4; r++) if (fabs(t[r][c]) > fabs(t[p][c])) p = r;
        if (fabs(t[p][c]) < 1e-20) return false;
        if (p != c) for (int k = 0; k < 8; k++) { const double x = t[p][k]; t[p][k] = t[c][k]; t[c][k] = x; }
        const double d = 1.0 / t[c][c];
        for (int k = 0; k < 8; k++) t[c][k] *= d;
        for (int r = 0; r < 4; r++) {
            if (r == c) continue;
            const double f = t[r][c];
            for (int k = 0; k < 8; k++) t[r][k] -= f * t[c][k];
        }
    }
    for (int r = 0; r < 4; r++) for (int c = 0; c < 4; c++) out[r][c] = t[r][c + 4];
    return true;
}

/* The pose out of a view-projection: its first, second and fourth rows are the planes
 * through the camera, and their normals its axes. The fourth row gives the view
 * depth. The third, against it, gives where depth 0 and 1 are. */
static bool fsr3_pose(const float cam[16], float z_scale, float z_offset, fsr3_camera* c)
{
    for (int r = 0; r < 4; r++)
        for (int k = 0; k < 4; k++) c->m[r][k] = cam[k * 4 + r];
    const double a[3][3] = { { c->m[0][0], c->m[0][1], c->m[0][2] },
                             { c->m[1][0], c->m[1][1], c->m[1][2] },
                             { c->m[3][0], c->m[3][1], c->m[3][2] } };
    const double b[3] = { -c->m[0][3], -c->m[1][3], -c->m[3][3] };
    const double det = a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1]) - a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0]) +
                       a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0]);
    if (fabs(det) < 1e-20) return false;
    for (int i = 0; i < 3; i++) {               /* Cramer */
        double m[3][3];
        memcpy(m, a, sizeof m);
        for (int r = 0; r < 3; r++) m[r][i] = b[r];
        c->pos[i] = (m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
                     m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0])) / det;
    }
    for (int i = 0; i < 3; i++) { c->right[i] = c->m[0][i]; c->up[i] = c->m[1][i]; c->fwd[i] = c->m[3][i]; }
    const double len_w = sqrt(c->fwd[0] * c->fwd[0] + c->fwd[1] * c->fwd[1] + c->fwd[2] * c->fwd[2]);
    const double len_y = sqrt(c->up[0] * c->up[0] + c->up[1] * c->up[1] + c->up[2] * c->up[2]);
    v3_norm(c->right);
    v3_norm(c->up);
    v3_norm(c->fwd);
    if (len_w <= 0.0 || len_y <= 0.0) return false;
    c->fov = 2.0 * atan(len_w / len_y);
    /* clip z along the view at depth t: A t + B, clip w: len_w t */
    const double A = c->m[2][0] * c->fwd[0] + c->m[2][1] * c->fwd[1] + c->m[2][2] * c->fwd[2];
    const double B = c->m[2][0] * c->pos[0] + c->m[2][1] * c->pos[1] + c->m[2][2] * c->pos[2] + c->m[2][3];
    const double S = z_scale, O = z_offset;
    c->znear = -B * S / (A * S + O * len_w);
    c->zfar  = -B * S / (A * S - (1.0 - O) * len_w);
    return true;
}

static int fsr3_targets(ID3D12Device* dev, unsigned render_w, unsigned render_h)
{
    s_dev = dev;
    s_rw = render_w; s_rh = render_h;
    if (!s_depth) s_depth = fsr3_target(dev, DXGI_FORMAT_R32_FLOAT, render_w, render_h);
    if (!s_motion) s_motion = fsr3_target(dev, DXGI_FORMAT_R16G16_FLOAT, render_w, render_h);
    return s_depth && s_motion && fsr3_inputs_init(dev);
}

int rsx_fsr3_up_create(void* device, unsigned display_w, unsigned display_h,
                       unsigned render_w, unsigned render_h, float sharpness)
{
    ID3D12Device* dev = (ID3D12Device*)device;
    if (!fsr3_targets(dev, render_w, render_h)) return 0;
    ffxCreateContextDescUpscaleVersion ver{};
    ver.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE_VERSION;
    ver.version = FFX_UPSCALER_VERSION;
    ffxCreateBackendDX12Desc be{};
    be.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
    be.header.pNext = &ver.header;
    be.device = dev;
    ffxCreateContextDescUpscale d{};
    d.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    d.header.pNext = &be.header;
    /* The title's picture is finished: display-encoded, tone mapped, 8 bits. */
    d.flags = FFX_UPSCALE_ENABLE_AUTO_EXPOSURE | FFX_UPSCALE_ENABLE_NON_LINEAR_COLORSPACE;
    if (getenv("D3D12_FG_CHECK")) d.flags |= FFX_UPSCALE_ENABLE_DEBUG_CHECKING;
    d.maxRenderSize.width = render_w;
    d.maxRenderSize.height = render_h;
    d.maxUpscaleSize.width = display_w;
    d.maxUpscaleSize.height = display_h;
    d.fpMessage = fsr3_message;
    const ffxReturnCode_t rc = s_ffx.CreateContext(&s_up_ctx, &d.header, nullptr);
    if (rc != FFX_API_RETURN_OK) {
        printf("[fsr3] upscaler: no context (%u)\n", (unsigned)rc);
        s_up_ctx = nullptr;
        return 0;
    }
    fsr3_version(&s_up_ctx, "upscaler");
    ffxQueryDescUpscaleGetJitterPhaseCount q{};
    q.header.type = FFX_API_QUERY_DESC_TYPE_UPSCALE_GETJITTERPHASECOUNT;
    q.renderWidth = render_w;
    q.displayWidth = display_w;
    int32_t n = 0;
    q.pOutPhaseCount = &n;
    s_ffx.Query(&s_up_ctx, &q.header);
    s_up_phases = n;
    s_dw = display_w; s_dh = display_h;
    s_up_sharp = sharpness;
    s_prev_ok = false;
    return 1;
}

void rsx_fsr3_up_destroy(void)
{
    if (!s_up_ctx) return;
    s_ffx.DestroyContext(&s_up_ctx, nullptr);
    s_up_ctx = nullptr;
}

void rsx_fsr3_up_jitter(unsigned frame, float* x, float* y)
{
    *x = *y = 0.0f;
    if (!s_up_ctx || s_up_phases <= 0) return;
    ffxQueryDescUpscaleGetJitterOffset q{};
    q.header.type = FFX_API_QUERY_DESC_TYPE_UPSCALE_GETJITTEROFFSET;
    q.index = (int32_t)(frame % (unsigned)s_up_phases);
    q.phaseCount = s_up_phases;
    q.pOutX = x;
    q.pOutY = y;
    s_ffx.Query(&s_up_ctx, &q.header);
}

int rsx_fsr3_fg_create(void* device, void* swap_chain, unsigned display_w, unsigned display_h,
                       unsigned render_w, unsigned render_h, int generate)
{
    if (!s_swap_ctx) return 0;
    ID3D12Device* dev = (ID3D12Device*)device;
    if (!fsr3_targets(dev, render_w, render_h)) return 0;

    ffxCreateContextDescFrameGenerationVersion ver{};
    ver.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION_VERSION;
    ver.version = FFX_FRAMEGENERATION_VERSION;
    ffxCreateBackendDX12Desc be{};
    be.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
    be.header.pNext = &ver.header;
    be.device = dev;
    ffxCreateContextDescFrameGeneration d{};
    d.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
    d.header.pNext = &be.header;
    d.displaySize.width = display_w;
    d.displaySize.height = display_h;
    d.maxRenderSize.width = render_w;
    d.maxRenderSize.height = render_h;
    d.backBufferFormat = FFX_API_SURFACE_FORMAT_R8G8B8A8_UNORM;
    if (getenv("D3D12_FG_CHECK")) d.flags |= FFX_FRAMEGENERATION_ENABLE_DEBUG_CHECKING;
    const ffxReturnCode_t rc = s_ffx.CreateContext(&s_fg_ctx, &d.header, nullptr);
    if (rc != FFX_API_RETURN_OK) {
        printf("[fsr3] frame generation: no context (%u)\n", (unsigned)rc);
        s_fg_ctx = nullptr;
        return 0;
    }
    ffxConfigureDescGlobalDebug dbg{};
    dbg.header.type = FFX_API_CONFIGURE_DESC_TYPE_GLOBALDEBUG;
    dbg.effectId = FFX_API_EFFECT_ID_FRAMEGENERATION;
    dbg.fpMessage = fsr3_message;
    dbg.debugLevel = FFX_API_CONFIGURE_GLOBALDEBUG_LEVEL_WARNINGS;
    s_ffx.Configure(&s_fg_ctx, &dbg.header);
    fsr3_version(&s_fg_ctx, "frame generation");
    s_swap = swap_chain;
    s_generate = generate;
    s_dw = display_w; s_dh = display_h;
    s_prev_ok = false;
    s_fg_was = false;
    return 1;
}

static void fsr3_configure(bool enabled, void* hudless)
{
    ffxConfigureDescFrameGeneration c{};
    c.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
    c.swapChain = s_swap;
    c.frameGenerationCallback = [](ffxDispatchDescFrameGeneration* params, void* ctx) -> ffxReturnCode_t {
        return s_ffx.Dispatch((ffxContext*)ctx, &params->header);
    };
    c.frameGenerationCallbackUserContext = &s_fg_ctx;
    c.frameGenerationEnabled = enabled;
    if (hudless) c.HUDLessColor = ffxApiGetResourceDX12((ID3D12Resource*)hudless, FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
    c.generationRect.width = (int32_t)s_dw;
    c.generationRect.height = (int32_t)s_dh;
    c.frameID = s_frame_id;
    s_ffx.Configure(&s_fg_ctx, &c.header);
}

void rsx_fsr3_fg_destroy(void)
{
    if (!s_fg_ctx) return;
    fsr3_configure(false, nullptr);
    s_ffx.DestroyContext(&s_fg_ctx, nullptr);
    s_fg_ctx = nullptr;
}

/* A frame rate over 1.5 s, which a single hitch does not bring under the floor, and two of
 * those in a row to change: over the floor by a margin to come back, as generation costs some
 * of the rate it is judged on. */
void rsx_fsr3_fg_floor(double fps) { s_floor = fps; }

static void fsr3_rate(float frame_ms)
{
    if (s_floor <= 0.0) return;
    s_rate_ms += frame_ms;
    s_rate_n++;
    if (s_rate_ms < 1500.0) return;
    const double rate = s_rate_n * 1000.0 / s_rate_ms;
    s_rate_ms = 0.0;
    s_rate_n = 0;
    const bool under = rate < (s_under ? s_floor + FSR3_FLOOR_BACK : s_floor);
    s_rate_streak = under != s_under ? s_rate_streak + 1 : 0;
    if (s_rate_streak >= 2) {
        s_under = under;
        s_rate_streak = 0;
        printf("[fsr3] %.1f frames a second: generation %s\n", rate, under ? "stands aside" : "is back");
    }
}

/* The motion across a cut means nothing. */
static bool fsr3_cut(const fsr3_camera& a, const fsr3_camera& b)
{
    const double turn = a.fwd[0] * b.fwd[0] + a.fwd[1] * b.fwd[1] + a.fwd[2] * b.fwd[2];
    double d[3] = { a.pos[0] - b.pos[0], a.pos[1] - b.pos[1], a.pos[2] - b.pos[2] };
    const double moved = sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    return turn < FSR3_CUT_COS || moved > FSR3_CUT_MOVE * a.znear || fabs(a.fov - b.fov) > FSR3_CUT_FOV * a.fov;
}

int rsx_fsr3_prepare(void* cmd_list, const rsx_fsr3_frame* f)
{
    fsr3_camera cam;
    bool on = f->scene && f->depth && fsr3_pose(f->cam, f->z_scale, f->z_offset, &cam);
    double inv[4][4];
    if (on && !m4_inverse(cam.m, inv)) on = false;
    const bool reset = on && (!s_prev_ok || fsr3_cut(cam, s_prev));
    if (on) {
        double rep[4][4];
        for (int r = 0; r < 4; r++)
            for (int k = 0; k < 4; k++) {
                double v = 0.0;
                if (reset) v = r == k ? 1.0 : 0.0;
                else for (int i = 0; i < 4; i++) v += s_prev.m[r][i] * inv[i][k];
                rep[r][k] = v;
            }
        /* From clip space to clip space, and the pass gives it NDC: the same point, as a
         * projective map does not mind the scale. */
        fsr3_inputs((ID3D12GraphicsCommandList*)cmd_list, (ID3D12Resource*)f->depth,
                    reset ? nullptr : (ID3D12Resource*)f->objects, rep, f->z_scale, f->z_offset);
        if (!s_prev_ok) {
            static int told;
            if (!told++)
                printf("[fsr3] camera: near %.3f far %.1f fov %.1f at (%.1f %.1f %.1f)\n", cam.znear, cam.zfar,
                       cam.fov * 57.29578, cam.pos[0], cam.pos[1], cam.pos[2]);
        }
        s_prev = cam;
        s_cam = cam;
    }
    s_on = on;
    s_reset = reset;
    s_prev_ok = on;
    s_prepared = true;
    return on;
}

void rsx_fsr3_up_frame(void* cmd_list, const rsx_fsr3_frame* f, void* color, void* output, float frame_ms)
{
    if (!s_up_ctx || !s_on) return;
    ffxDispatchDescUpscale d{};
    d.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
    d.commandList = cmd_list;
    d.color = ffxApiGetResourceDX12((ID3D12Resource*)color, FFX_API_RESOURCE_STATE_PIXEL_READ);
    d.depth = ffxApiGetResourceDX12(s_depth, FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
    d.motionVectors = ffxApiGetResourceDX12(s_motion, FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
    d.output = ffxApiGetResourceDX12((ID3D12Resource*)output, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
    d.jitterOffset.x = f->jitter_x;
    d.jitterOffset.y = f->jitter_y;
    d.motionVectorScale.x = (float)s_rw;
    d.motionVectorScale.y = (float)s_rh;
    d.renderSize.width = s_rw;
    d.renderSize.height = s_rh;
    d.upscaleSize.width = s_dw;
    d.upscaleSize.height = s_dh;
    d.enableSharpening = s_up_sharp >= 0.0f;
    d.sharpness = s_up_sharp >= 0.0f ? s_up_sharp : 0.0f;
    d.frameTimeDelta = frame_ms;
    d.preExposure = 1.0f;
    d.reset = s_reset;
    d.cameraNear = (float)s_cam.znear;
    d.cameraFar = (float)s_cam.zfar;
    d.cameraFovAngleVertical = (float)s_cam.fov;
    d.viewSpaceToMetersFactor = 1.0f;
    d.flags = FFX_UPSCALE_FLAG_NON_LINEAR_COLOR_SRGB;
    const ffxReturnCode_t rc = s_ffx.Dispatch(&s_up_ctx, &d.header);
    static int said;
    if (rc != FFX_API_RETURN_OK && !said++)
        fprintf(stderr, "[fsr3] upscale failed (%u)\n", (unsigned)rc);
}

void rsx_fsr3_fg_frame(void* cmd_list, const rsx_fsr3_frame* f, float frame_ms)
{
    if (!s_fg_ctx) return;
    fsr3_rate(frame_ms);
    if (s_generate && !s_prepared) rsx_fsr3_prepare(cmd_list, f);
    s_prepared = false;
    const bool on = s_generate && !s_under && s_on && f->hudless;
    const bool reset = on && (s_reset || !s_fg_was);
    fsr3_configure(on, on ? f->hudless : nullptr);
    if (on) {
        ffxDispatchDescFrameGenerationPrepareV2 p{};
        p.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_V2;
        p.frameID = s_frame_id;
        p.commandList = cmd_list;
        p.renderSize.width = s_rw;
        p.renderSize.height = s_rh;
        p.jitterOffset.x = f->jitter_x;
        p.jitterOffset.y = f->jitter_y;
        p.motionVectorScale.x = (float)s_rw;
        p.motionVectorScale.y = (float)s_rh;
        p.frameTimeDelta = frame_ms;
        p.reset = reset;
        p.cameraNear = (float)s_cam.znear;
        p.cameraFar = (float)s_cam.zfar;
        p.cameraFovAngleVertical = (float)s_cam.fov;
        p.depth = ffxApiGetResourceDX12(s_depth, FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
        p.motionVectors = ffxApiGetResourceDX12(s_motion, FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
        for (int i = 0; i < 3; i++) {
            p.cameraPosition[i] = (float)s_cam.pos[i];
            p.cameraUp[i] = (float)s_cam.up[i];
            p.cameraRight[i] = (float)s_cam.right[i];
            p.cameraForward[i] = (float)s_cam.fwd[i];
        }
        const ffxReturnCode_t rc = s_ffx.Dispatch(&s_fg_ctx, &p.header);
        static int said;
        if (rc != FFX_API_RETURN_OK && !said++)
            fprintf(stderr, "[fsr3] prepare failed (%u)\n", (unsigned)rc);
    }
    s_fg_was = on;
    s_frame_id++;
}

#endif /* _WIN32 */
