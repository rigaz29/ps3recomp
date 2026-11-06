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
#include <dxgi1_5.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "../../third_party/fsr_api/api/include/ffx_api.h"
#include "../../third_party/fsr_api/api/include/ffx_api_loader.h"
#include "../../third_party/fsr_api/api/include/dx12/ffx_api_dx12.h"
#include "../../third_party/fsr_api/framegeneration/include/ffx_framegeneration.h"
#include "../../third_party/fsr_api/framegeneration/include/dx12/ffx_api_framegeneration_dx12.h"

#include "rsx_fsr3.h"

static ffxFunctions s_ffx;
static ffxContext s_swap_ctx;   /* the proxy's */
static ffxContext s_fg_ctx;     /* frame generation's */
static void* s_swap;
static int s_generate;
static unsigned s_dw, s_dh, s_rw, s_rh;
static uint64_t s_frame_id;
static ID3D12Resource* s_depth;
static ID3D12Resource* s_motion;

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

/* What phase 2 gives it: nothing moves, one depth. */
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
    ID3D12Resource* r = nullptr;
    dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                 D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                 nullptr, IID_PPV_ARGS(&r));
    return r;
}

int rsx_fsr3_fg_create(void* device, void* swap_chain, unsigned display_w, unsigned display_h,
                       unsigned render_w, unsigned render_h, int generate)
{
    if (!s_swap_ctx) return 0;
    ID3D12Device* dev = (ID3D12Device*)device;
    if (!s_depth) s_depth = fsr3_target(dev, DXGI_FORMAT_R32_FLOAT, render_w, render_h);
    if (!s_motion) s_motion = fsr3_target(dev, DXGI_FORMAT_R16G16_FLOAT, render_w, render_h);
    if (!s_depth || !s_motion) return 0;

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
    s_rw = render_w; s_rh = render_h;
    return 1;
}

static void fsr3_configure(bool enabled)
{
    ffxConfigureDescFrameGeneration c{};
    c.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
    c.swapChain = s_swap;
    c.frameGenerationCallback = [](ffxDispatchDescFrameGeneration* params, void* ctx) -> ffxReturnCode_t {
        return s_ffx.Dispatch((ffxContext*)ctx, &params->header);
    };
    c.frameGenerationCallbackUserContext = &s_fg_ctx;
    c.frameGenerationEnabled = enabled;
    c.generationRect.width = (int32_t)s_dw;
    c.generationRect.height = (int32_t)s_dh;
    c.frameID = s_frame_id;
    s_ffx.Configure(&s_fg_ctx, &c.header);
}

void rsx_fsr3_fg_destroy(void)
{
    if (!s_fg_ctx) return;
    fsr3_configure(false);
    s_ffx.DestroyContext(&s_fg_ctx, nullptr);
    s_fg_ctx = nullptr;
}

void rsx_fsr3_fg_frame(void* cmd_list, float frame_ms)
{
    if (!s_fg_ctx) return;
    fsr3_configure(s_generate != 0);
    if (s_generate) {
        ffxDispatchDescFrameGenerationPrepareV2 p{};
        p.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_V2;
        p.frameID = s_frame_id;
        p.commandList = cmd_list;
        p.renderSize.width = s_rw;
        p.renderSize.height = s_rh;
        p.motionVectorScale.x = (float)s_rw;
        p.motionVectorScale.y = (float)s_rh;
        p.frameTimeDelta = frame_ms;
        p.cameraNear = 0.1f;
        p.cameraFar = 1000.0f;
        p.cameraFovAngleVertical = 1.0f;
        p.depth = ffxApiGetResourceDX12(s_depth, FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
        p.motionVectors = ffxApiGetResourceDX12(s_motion, FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
        p.cameraUp[1] = 1.0f;
        p.cameraRight[0] = 1.0f;
        p.cameraForward[2] = 1.0f;
        const ffxReturnCode_t rc = s_ffx.Dispatch(&s_fg_ctx, &p.header);
        static int said;
        if (rc != FFX_API_RETURN_OK && !said++)
            fprintf(stderr, "[fsr3] prepare failed (%u)\n", (unsigned)rc);
    }
    s_frame_id++;
}

#endif /* _WIN32 */
