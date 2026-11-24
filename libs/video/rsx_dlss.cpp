/*
 * ps3recomp - NVIDIA DLSS Super Resolution and DLAA through NGX (PS3RECOMP_DLSS; see rsx_dlss.h)
 *
 * NVIDIA's NGX SDK 310.9.16: its headers and the static library that finds the driver's NGX, which the build fetches
 * (CMakeLists.txt) and this repository does not keep. NGX comes with the NVIDIA driver. The DLSS DLL, NVIDIA's signed
 * release build of nvngx_dlss.dll, is looked for beside the exe and in the folder the caller names, where the
 * launcher puts it; NGX checks NVIDIA's signature on it before it loads it.
 *
 * The steps are those of NVIDIA's DLSS Programming Guide: NGX with a project id (5.2), its capability parameters (is
 * DLSS there, is the driver new enough; 5.2.6), each quality mode's optimal render size and the range of render sizes
 * it takes for the display (5.2.8, 3.2.2), the feature created at its mode's optimal size (5.3), one evaluate a frame
 * with the render size as the subrect (5.4).
 *
 * What a frame gives it is what FSR's upscaler takes (rsx_fsr3_prepare): the scene's color after the title's tone
 * mapping, 8 bits in sRGB, which is DLSS's LDR mode (3.1.2: IsHDR 0); depth as R32_FLOAT with the near plane at 0
 * (DepthInverted 0); motion at the render's size (MVLowRes) in UV units from a pixel to where it was, the direction
 * DLSS wants (3.6), made pixels by the motion scale, without the jitter (MVJittered 0). The jitter is Halton (2, 3)
 * over 8 * scale^2 phases, the pattern and count of 3.7.1, in render pixels with y down like the motion (3.7.3). The
 * inputs go to NON_PIXEL_SHADER_RESOURCE for the evaluate, as 3.4 asks, and back after.
 *
 * Two levers for a tester, as the guide's jitter debugging has in the SDK's own DLL (3.18.1.1):
 * D3D12_DLSS_JITTER=x,y and D3D12_DLSS_MV=x,y multiply what DLSS is told of the jitter and the motion (1,1 as they are,
 * -1 turns one round). D3D12_DLSS_LOG=on|verbose: NGX's own lines in the log, off by default (at on its start alone
 * is some 220 lines, its model cache's misses among them).
 */
#ifdef _WIN32

#include <windows.h>
#include <d3d11.h>
#include <d3d12.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "nvsdk_ngx_helpers.h"

#include "rsx_dlss.h"

/* The project's NGX id: a CUSTOM engine names its project by a GUID of its own (the guide's 5.2.1). */
static const char k_project_id[] = "1bc5ed29-b253-4b7c-b53d-105c30075d12";
static const char k_engine_version[] = "ps3recomp";

static ID3D12Device* s_dev;
static bool s_ngx;                      /* NGX is up on s_dev */
static NVSDK_NGX_Parameter* s_params;   /* its capability parameters, used for every call as NVIDIA's sample does */
static NVSDK_NGX_Handle* s_feature;
static unsigned s_rw, s_rh, s_dw, s_dh;
static int s_phases;
static unsigned s_preset;               /* NVSDK_NGX_DLSS_Hint_Render_Preset */
static float s_jmul[2] = { 1.0f, 1.0f }, s_mvmul[2] = { 1.0f, 1.0f };
static char s_name[64] = "DLSS";
static char s_dll[24];                  /* the DLL's file version, "310.9.16" */
static wchar_t s_dll_dir[MAX_PATH];     /* rsx_dlss_init's dll_dir */
static int s_fails;

static const struct {
    NVSDK_NGX_PerfQuality_Value value;
    const char* name;
    const char* preset;                 /* the parameter that hints its preset */
} k_modes[] = {
    { NVSDK_NGX_PerfQuality_Value_DLAA, "DLAA", NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA },
    { NVSDK_NGX_PerfQuality_Value_MaxQuality, "Quality", NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality },
    { NVSDK_NGX_PerfQuality_Value_Balanced, "Balanced", NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced },
    { NVSDK_NGX_PerfQuality_Value_MaxPerf, "Performance", NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance },
    { NVSDK_NGX_PerfQuality_Value_UltraPerformance, "Ultra Performance",
      NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance },
};

static const char* dlss_result(int r)
{
    switch ((NVSDK_NGX_Result)r) {
    case NVSDK_NGX_Result_Success: return "success";
    case NVSDK_NGX_Result_FAIL_FeatureNotSupported: return "feature not supported";
    case NVSDK_NGX_Result_FAIL_PlatformError: return "platform error";
    case NVSDK_NGX_Result_FAIL_FeatureAlreadyExists: return "feature already exists";
    case NVSDK_NGX_Result_FAIL_FeatureNotFound: return "feature not found";
    case NVSDK_NGX_Result_FAIL_InvalidParameter: return "invalid parameter";
    case NVSDK_NGX_Result_FAIL_ScratchBufferTooSmall: return "scratch buffer too small";
    case NVSDK_NGX_Result_FAIL_NotInitialized: return "not initialized";
    case NVSDK_NGX_Result_FAIL_UnsupportedInputFormat: return "unsupported input format";
    case NVSDK_NGX_Result_FAIL_RWFlagMissing: return "UAV flag missing";
    case NVSDK_NGX_Result_FAIL_MissingInput: return "missing input";
    case NVSDK_NGX_Result_FAIL_UnableToInitializeFeature: return "unable to initialize the feature";
    case NVSDK_NGX_Result_FAIL_OutOfDate: return "driver out of date";
    case NVSDK_NGX_Result_FAIL_OutOfGPUMemory: return "out of GPU memory";
    case NVSDK_NGX_Result_FAIL_UnsupportedFormat: return "unsupported format";
    case NVSDK_NGX_Result_FAIL_UnableToWriteToAppDataPath: return "cannot write to its data folder";
    case NVSDK_NGX_Result_FAIL_UnsupportedParameter: return "unsupported parameter";
    case NVSDK_NGX_Result_FAIL_Denied: return "denied";
    case NVSDK_NGX_Result_FAIL_NotImplemented: return "not implemented";
    default: break;
    }
    static char other[24];
    snprintf(other, sizeof other, "0x%08X", (unsigned)r);
    return other;
}

/* NGX's own lines, from any of its threads. Capped: a driver that logs every frame must not fill the log. */
static volatile LONG s_log_n;
static void NVSDK_CONV dlss_log(const char* message, NVSDK_NGX_Logging_Level level, NVSDK_NGX_Feature component)
{
    (void)level;
    (void)component;
    const LONG n = InterlockedIncrement(&s_log_n);
    if (n > 300) {
        if (n == 301) printf("[ngx] (no more of its lines)\n");
        return;
    }
    size_t len = message ? strlen(message) : 0;
    while (len && (message[len - 1] == '\n' || message[len - 1] == '\r')) len--;
    printf("[ngx] %.*s\n", (int)len, message ? message : "");
}

static void dlss_pair(const char* env, float out[2])
{
    const char* v = getenv(env);
    float x = 1.0f, y = 1.0f;
    if (!v || !*v) return;
    if (sscanf(v, "%f,%f", &x, &y) != 2 || x == 0.0f || y == 0.0f) {
        printf("[dlss] %s = %s is not two numbers such as 1,-1; 1,1\n", env, v);
        return;
    }
    out[0] = x;
    out[1] = y;
    printf("[dlss] %s: x %g, y %g\n", env, x, y);
}

static void dlss_fail(char* why, unsigned n, const char* fmt, ...)
{
    if (!why || !n) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(why, n, fmt, ap);
    va_end(ap);
}

static const char* dlss_preset_name(unsigned p)
{
    switch (p) {
    case NVSDK_NGX_DLSS_Hint_Render_Preset_J: return "J";
    case NVSDK_NGX_DLSS_Hint_Render_Preset_K: return "K";
    case NVSDK_NGX_DLSS_Hint_Render_Preset_L: return "L";
    case NVSDK_NGX_DLSS_Hint_Render_Preset_M: return "M";
    default: return "the DLL's own";
    }
}

int rsx_dlss_init(void* device, const wchar_t* dll_dir, const wchar_t* data_dir, char* why, unsigned why_size)
{
    if (s_ngx) return 1;
    if (why && why_size) why[0] = 0;
    s_dev = (ID3D12Device*)device;
    dlss_pair("D3D12_DLSS_JITTER", s_jmul);
    dlss_pair("D3D12_DLSS_MV", s_mvmul);

    const char* lv = getenv("D3D12_DLSS_LOG");
    NVSDK_NGX_Logging_Level level = NVSDK_NGX_LOGGING_LEVEL_OFF;
    if (lv && !_stricmp(lv, "on")) level = NVSDK_NGX_LOGGING_LEVEL_ON;
    else if (lv && !_stricmp(lv, "verbose")) level = NVSDK_NGX_LOGGING_LEVEL_VERBOSE;

    wchar_t* paths[1] = { s_dll_dir };
    NVSDK_NGX_FeatureCommonInfo info;
    memset(&info, 0, sizeof info);
    s_dll_dir[0] = 0;
    if (dll_dir && *dll_dir && wcslen(dll_dir) < MAX_PATH) {
        wcscpy_s(s_dll_dir, MAX_PATH, dll_dir);
        info.PathListInfo.Path = paths;
        info.PathListInfo.Length = 1;
    }
    info.LoggingInfo.LoggingCallback = dlss_log;
    info.LoggingInfo.MinimumLoggingLevel = level;
    info.LoggingInfo.DisableOtherLoggingSinks = level != NVSDK_NGX_LOGGING_LEVEL_OFF;

    NVSDK_NGX_Result r = NVSDK_NGX_D3D12_Init_with_ProjectID(k_project_id, NVSDK_NGX_ENGINE_TYPE_CUSTOM, k_engine_version,
                                                              data_dir && *data_dir ? data_dir : L".", s_dev, &info,
                                                              NVSDK_NGX_Version_API);
    if (NVSDK_NGX_FAILED(r)) {
        dlss_fail(why, why_size, "NGX did not start (%s): no NVIDIA driver with it here", dlss_result(r));
        return 0;
    }
    s_ngx = true;
    r = NVSDK_NGX_D3D12_GetCapabilityParameters(&s_params);
    if (NVSDK_NGX_FAILED(r) || !s_params) {
        dlss_fail(why, why_size, "NGX has no parameters for DLSS (%s)", dlss_result(r));
        rsx_dlss_shutdown();
        return 0;
    }
    int needs = 0;
    unsigned major = 0, minor = 0;
    if (NVSDK_NGX_SUCCEED(s_params->Get(NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver, &needs)) && needs) {
        s_params->Get(NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMajor, &major);
        s_params->Get(NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMinor, &minor);
        dlss_fail(why, why_size, "the NVIDIA driver is too old for DLSS: %u.%u or newer", major, minor);
        rsx_dlss_shutdown();
        return 0;
    }
    int available = 0;
    r = s_params->Get(NVSDK_NGX_Parameter_SuperSampling_Available, &available);
    if (NVSDK_NGX_FAILED(r) || !available) {
        int init = 0;
        s_params->Get(NVSDK_NGX_Parameter_SuperSampling_FeatureInitResult, &init);
        dlss_fail(why, why_size, "DLSS is not available on this GPU or driver (%s%s)",
                  dlss_result(init ? init : (int)r),
                  (NVSDK_NGX_Result)init == NVSDK_NGX_Result_FAIL_FeatureNotFound ? ": no nvngx_dlss.dll" : "");
        rsx_dlss_shutdown();
        return 0;
    }
    printf("[dlss] NGX up, DLSS available\n");
    return 1;
}

void rsx_dlss_preset(char letter)
{
    switch (letter) {
    case 'J': case 'j': s_preset = NVSDK_NGX_DLSS_Hint_Render_Preset_J; break;
    case 'K': case 'k': s_preset = NVSDK_NGX_DLSS_Hint_Render_Preset_K; break;
    case 'L': case 'l': s_preset = NVSDK_NGX_DLSS_Hint_Render_Preset_L; break;
    case 'M': case 'm': s_preset = NVSDK_NGX_DLSS_Hint_Render_Preset_M; break;
    default: s_preset = NVSDK_NGX_DLSS_Hint_Render_Preset_Default; break;
    }
}

/* The loaded DLL's file version, and where it came from, without the folder's path (a log may be posted). */
static void dlss_dll_version(void)
{
    s_dll[0] = 0;
    HMODULE m = GetModuleHandleW(L"nvngx_dlss.dll");
    wchar_t path[MAX_PATH], exe[MAX_PATH];
    if (!m || !GetModuleFileNameW(m, path, MAX_PATH)) {
        printf("[dlss] nvngx_dlss.dll: not among the loaded modules\n");
        return;
    }
    DWORD zero = 0;
    const DWORD n = GetFileVersionInfoSizeW(path, &zero);
    void* buf = n ? malloc(n) : NULL;
    VS_FIXEDFILEINFO* fi = NULL;
    UINT len = 0;
    if (buf && GetFileVersionInfoW(path, 0, n, buf) && VerQueryValueW(buf, L"\\", (void**)&fi, &len) && fi &&
        len >= sizeof *fi) {
        if (LOWORD(fi->dwFileVersionLS))
            snprintf(s_dll, sizeof s_dll, "%u.%u.%u.%u", HIWORD(fi->dwFileVersionMS), LOWORD(fi->dwFileVersionMS),
                     HIWORD(fi->dwFileVersionLS), LOWORD(fi->dwFileVersionLS));
        else
            snprintf(s_dll, sizeof s_dll, "%u.%u.%u", HIWORD(fi->dwFileVersionMS), LOWORD(fi->dwFileVersionMS),
                     HIWORD(fi->dwFileVersionLS));
    }
    free(buf);
    const wchar_t* from = L"another folder";
    wchar_t* slash = wcsrchr(path, L'\\');
    if (slash) *slash = 0;
    if (GetModuleFileNameW(NULL, exe, MAX_PATH)) {
        wchar_t* es = wcsrchr(exe, L'\\');
        if (es) *es = 0;
        if (!_wcsicmp(path, exe)) from = L"the exe's folder";
    }
    if (s_dll_dir[0] && !_wcsicmp(path, s_dll_dir)) from = L"the DLSS folder";
    printf("[dlss] nvngx_dlss.dll %s, from %ls\n", s_dll[0] ? s_dll : "(no version)", from);
}

int rsx_dlss_up_create(void* cmd_list, unsigned display_w, unsigned display_h, unsigned render_w, unsigned render_h,
                       char* why, unsigned why_size)
{
    if (why && why_size) why[0] = 0;
    if (!s_params) {
        dlss_fail(why, why_size, "NGX is not up");
        return 0;
    }
    rsx_dlss_up_destroy();
    /* Each mode's optimal render size and range for this display; the one that takes the render size and whose own
     * scale is nearest to it. */
    const double scale = (double)display_w / (double)render_w;
    int best = -1;
    double best_d = 1e30;
    unsigned ow = 0, oh = 0;
    for (int i = 0; i < (int)(sizeof k_modes / sizeof k_modes[0]); i++) {
        unsigned w = 0, h = 0, maxw = 0, maxh = 0, minw = 0, minh = 0;
        float sharp = 0.0f;
        const NVSDK_NGX_Result r = NGX_DLSS_GET_OPTIMAL_SETTINGS(s_params, display_w, display_h, k_modes[i].value, &w, &h,
                                                                  &maxw, &maxh, &minw, &minh, &sharp);
        if (NVSDK_NGX_FAILED(r) || !w || !h) {
            printf("[dlss] %s: none at %ux%u (%s)\n", k_modes[i].name, display_w, display_h,
                   NVSDK_NGX_FAILED(r) ? dlss_result(r) : "no size");
            continue;
        }
        const bool takes = render_w >= minw && render_w <= maxw && render_h >= minh && render_h <= maxh;
        printf("[dlss] %s: %ux%u, %ux%u to %ux%u%s\n", k_modes[i].name, w, h, minw, minh, maxw, maxh,
               takes ? "" : " (not the render's size)");
        if (!takes) continue;
        const double d = fabs(log(scale) - log((double)display_w / (double)w));
        if (d < best_d) {
            best = i;
            best_d = d;
            ow = w;
            oh = h;
        }
    }
    if (best < 0) {
        dlss_fail(why, why_size, "no DLSS mode takes %ux%u to %ux%u", render_w, render_h, display_w, display_h);
        return 0;
    }
    for (int i = 0; i < (int)(sizeof k_modes / sizeof k_modes[0]); i++)
        NVSDK_NGX_Parameter_SetUI(s_params, k_modes[i].preset, s_preset);

    NVSDK_NGX_DLSS_Create_Params cp;
    memset(&cp, 0, sizeof cp);
    cp.Feature.InWidth = ow;
    cp.Feature.InHeight = oh;
    cp.Feature.InTargetWidth = display_w;
    cp.Feature.InTargetHeight = display_h;
    cp.Feature.InPerfQualityValue = k_modes[best].value;
    cp.InFeatureCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_MVLowRes | NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;
    cp.InEnableOutputSubrects = false;
    const NVSDK_NGX_Result r = NGX_D3D12_CREATE_DLSS_EXT((ID3D12GraphicsCommandList*)cmd_list, 1, 1, &s_feature, s_params,
                                                         &cp);
    if (NVSDK_NGX_FAILED(r) || !s_feature) {
        s_feature = NULL;
        dlss_fail(why, why_size, "DLSS %s did not start (%s)", k_modes[best].name, dlss_result(r));
        return 0;
    }
    s_rw = render_w;
    s_rh = render_h;
    s_dw = display_w;
    s_dh = display_h;
    s_phases = (int)ceil(8.0 * scale * scale);
    if (s_phases < 8) s_phases = 8;
    s_fails = 0;
    dlss_dll_version();
    snprintf(s_name, sizeof s_name, "DLSS%s%s %s", s_dll[0] ? " " : "", s_dll, k_modes[best].name);
    unsigned long long vram = 0;
    char mb[32] = "";
    if (NVSDK_NGX_SUCCEED(NGX_DLSS_GET_STATS(s_params, &vram))) snprintf(mb, sizeof mb, ", %llu MB", vram >> 20);
    printf("[dlss] %s: %ux%u from %ux%u (made at %ux%u), preset %s, %d jitter phases%s\n", s_name, display_w, display_h,
           render_w, render_h, ow, oh, dlss_preset_name(s_preset), s_phases, mb);
    return 1;
}

void rsx_dlss_up_destroy(void)
{
    if (s_feature) {
        NVSDK_NGX_D3D12_ReleaseFeature(s_feature);
        s_feature = NULL;
    }
    s_phases = 0;
    snprintf(s_name, sizeof s_name, "DLSS");
}

static float dlss_halton(unsigned i, unsigned base)
{
    float f = 1.0f, r = 0.0f;
    while (i) {
        f /= (float)base;
        r += f * (float)(i % base);
        i /= base;
    }
    return r;
}

void rsx_dlss_up_jitter(unsigned frame, float* x, float* y)
{
    *x = *y = 0.0f;
    if (!s_feature || s_phases <= 0) return;
    const unsigned i = frame % (unsigned)s_phases + 1;
    *x = dlss_halton(i, 2) - 0.5f;
    *y = dlss_halton(i, 3) - 0.5f;
}

static void dlss_barriers(ID3D12GraphicsCommandList* cl, ID3D12Resource* const* r, const D3D12_RESOURCE_STATES* from,
                          D3D12_RESOURCE_STATES to, int back)
{
    D3D12_RESOURCE_BARRIER b[3];
    memset(b, 0, sizeof b);
    for (int i = 0; i < 3; i++) {
        b[i].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b[i].Transition.pResource = r[i];
        b[i].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b[i].Transition.StateBefore = back ? to : from[i];
        b[i].Transition.StateAfter = back ? from[i] : to;
    }
    cl->ResourceBarrier(3, b);
}

int rsx_dlss_up_frame(void* cmd_list, void* color, void* depth, void* motion, void* output, float jitter_x,
                      float jitter_y, int reset, float frame_ms)
{
    if (!s_feature) return 0;
    ID3D12GraphicsCommandList* cl = (ID3D12GraphicsCommandList*)cmd_list;
    ID3D12Resource* const in[3] = { (ID3D12Resource*)color, (ID3D12Resource*)depth, (ID3D12Resource*)motion };
    const D3D12_RESOURCE_STATES read = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    const D3D12_RESOURCE_STATES was[3] = { D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, read, read };
    dlss_barriers(cl, in, was, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, 0);

    NVSDK_NGX_D3D12_DLSS_Eval_Params e;
    memset(&e, 0, sizeof e);
    e.Feature.pInColor = in[0];
    e.Feature.pInOutput = (ID3D12Resource*)output;
    e.pInDepth = in[1];
    e.pInMotionVectors = in[2];
    e.InJitterOffsetX = jitter_x * s_jmul[0];
    e.InJitterOffsetY = jitter_y * s_jmul[1];
    e.InRenderSubrectDimensions.Width = s_rw;
    e.InRenderSubrectDimensions.Height = s_rh;
    e.InReset = reset ? 1 : 0;
    e.InMVScaleX = (float)s_rw * s_mvmul[0];
    e.InMVScaleY = (float)s_rh * s_mvmul[1];
    e.InPreExposure = 1.0f;
    e.InExposureScale = 1.0f;
    e.InFrameTimeDeltaInMsec = frame_ms;
    const NVSDK_NGX_Result r = NGX_D3D12_EVALUATE_DLSS_EXT(cl, s_feature, s_params, &e);

    dlss_barriers(cl, in, was, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, 1);
    if (NVSDK_NGX_FAILED(r)) {
        if (s_fails++ < 3) printf("[dlss] evaluate failed (%s)\n", dlss_result(r));
        return 0;
    }
    s_fails = 0;
    return 1;
}

const char* rsx_dlss_name(void) { return s_name; }

void rsx_dlss_shutdown(void)
{
    rsx_dlss_up_destroy();
    if (s_params) {
        NVSDK_NGX_D3D12_DestroyParameters(s_params);
        s_params = NULL;
    }
    if (s_ngx) {
        NVSDK_NGX_D3D12_Shutdown1(s_dev);
        s_ngx = false;
    }
}

#endif /* _WIN32 */
