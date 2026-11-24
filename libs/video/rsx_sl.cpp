/*
 * ps3recomp - NVIDIA Reflex through NVIDIA Streamline (graphics.reflex): see rsx_sl.h.
 *
 * Streamline's own functions are taken from sl.interposer.dll with GetProcAddress and the features' with its
 * slGetFeatureFunction, so nothing links against it and a PC without it, or a GPU that is not NVIDIA's, never loads
 * it. It runs with manual hooking (the backend's device, queues and lists stay its own), no updates over the network
 * and no plugin but Reflex and PCL (PC latency stats) with Streamline's common one.
 */
#ifdef _WIN32

#include <windows.h>
#include <dxgi1_4.h>
#include <softpub.h>
#include <wincrypt.h>
#include <wintrust.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "sl.h"
#include "sl_pcl.h"
#include "sl_reflex.h"

#include "rsx_sl.h"

#include "ps3emu/ps3_settings.h"

/* The project's id for NVIDIA's libraries, the one rsx_dlss.cpp gives NGX. */
static const char k_project_id[] = "1bc5ed29-b253-4b7c-b53d-105c30075d12";

static HMODULE s_mod;
static PFun_slInit* p_init;
static PFun_slShutdown* p_shutdown;
static PFun_slIsFeatureSupported* p_supported;
static PFun_slUpgradeInterface* p_upgrade;
static PFun_slGetNativeInterface* p_native;
static PFun_slGetFeatureFunction* p_feature;
static PFun_slGetNewFrameToken* p_token;
static PFun_slSetD3DDevice* p_device;
static PFun_slReflexSleep* p_sleep;
static PFun_slReflexSetOptions* p_options;
static PFun_slReflexGetState* p_rstate;
static PFun_slPCLSetMarker* p_marker;
static PFun_slPCLGetState* p_pstate;
static PFun_slPCLSetOptions* p_poptions;

static int s_on;
static sl::ReflexMode s_mode;
static char s_label[32];
static UINT s_ping_msg;          /* PCL's latency ping, a thread message */
/* The frames, by Streamline's index: the one the title builds (its simulation started at a flip's release), whether
 * the drain has reached its flip, the one the backend presents next and the one it presents now (0: none). The walk
 * thread sets them; the render thread reads s_present, which is set before it is asked to present. */
static uint32_t s_sim;
static int s_sim_ended;
static uint32_t s_shown;
static uint32_t s_marked;
static volatile uint32_t s_present;

static const char* result_name(sl::Result r)
{
    switch (r) {
    case sl::Result::eOk: return "ok";
    case sl::Result::eErrorDriverOutOfDate: return "the driver is too old";
    case sl::Result::eErrorOSOutOfDate: return "Windows is too old";
    case sl::Result::eErrorNoSupportedAdapterFound: return "not supported on this GPU";
    case sl::Result::eErrorFeatureMissing: return "its plugin is missing";
    case sl::Result::eErrorFeatureMissingDependency: return "a plugin it needs is missing";
    default: return "";
    }
}

static void say_result(const char* what, sl::Result r)
{
    const char* n = result_name(r);
    printf("[D3D12] reflex: off, %s failed (Streamline result %d%s%s)\n", what, (int)r, *n ? ": " : "", n);
}

/* Streamline's own messages: warnings and errors, and the rest with D3D12_SL_VERBOSE. */
static void sl_log(sl::LogType type, const char* msg)
{
    static const int verbose = getenv("D3D12_SL_VERBOSE") != nullptr;
    if (!msg || (type == sl::LogType::eInfo && !verbose)) return;
    size_t n = strlen(msg);
    while (n && (msg[n - 1] == '\n' || msg[n - 1] == '\r')) n--;
    fprintf(stderr, "[sl] %.*s\n", (int)n, msg);
}

/* Who signed the file, by Authenticode, into who: 1 when the signature checks out against Windows' trusted roots,
 * offline (no revocation lookups: a game start must not wait on the network). As rsx_anr.cpp checks ANR's DLL. */
static int sl_signer(const wchar_t* path, wchar_t* who, DWORD n)
{
    who[0] = 0;
    WINTRUST_FILE_INFO fi{};
    fi.cbStruct = sizeof fi;
    fi.pcwszFilePath = path;
    GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    WINTRUST_DATA wd{};
    wd.cbStruct = sizeof wd;
    wd.dwUIChoice = WTD_UI_NONE;
    wd.fdwRevocationChecks = WTD_REVOKE_NONE;
    wd.dwUnionChoice = WTD_CHOICE_FILE;
    wd.pFile = &fi;
    wd.dwStateAction = WTD_STATEACTION_VERIFY;
    wd.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL | WTD_REVOCATION_CHECK_NONE;
    const LONG r = WinVerifyTrust((HWND)INVALID_HANDLE_VALUE, &action, &wd);
    if (r == ERROR_SUCCESS) {
        CRYPT_PROVIDER_DATA* pd = WTHelperProvDataFromStateData(wd.hWVTStateData);
        CRYPT_PROVIDER_SGNR* sg = pd ? WTHelperGetProvSignerFromChain(pd, 0, FALSE, 0) : nullptr;
        CRYPT_PROVIDER_CERT* pc = sg ? WTHelperGetProvCertFromChain(sg, 0) : nullptr;
        if (pc && pc->pCert) CertGetNameStringW(pc->pCert, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, nullptr, who, n);
    }
    wd.dwStateAction = WTD_STATEACTION_CLOSE;
    WinVerifyTrust((HWND)INVALID_HANDLE_VALUE, &action, &wd);
    return r == ERROR_SUCCESS && who[0];
}

static sl::FrameToken* token(uint32_t index)
{
    sl::FrameToken* t = nullptr;
    return p_token && p_token(t, &index) == sl::Result::eOk ? t : nullptr;
}

static void mark(sl::PCLMarker m, uint32_t frame)
{
    if (!p_marker || !frame) return;
    if (sl::FrameToken* t = token(frame)) p_marker(m, *t);
}

static void sl_unload(void)
{
    if (p_shutdown) p_shutdown();
    if (s_mod) FreeLibrary(s_mod);
    s_mod = nullptr;
    p_init = nullptr; p_shutdown = nullptr; p_supported = nullptr; p_upgrade = nullptr; p_native = nullptr;
    p_feature = nullptr; p_token = nullptr; p_device = nullptr; p_sleep = nullptr; p_options = nullptr;
    p_rstate = nullptr; p_marker = nullptr; p_pstate = nullptr; p_poptions = nullptr;
    s_on = 0;
}

template <class T>
static bool feature_fn(sl::Feature f, const char* name, T** out)
{
    void* fn = nullptr;
    *out = p_feature(f, name, fn) == sl::Result::eOk ? (T*)fn : nullptr;
    return *out != nullptr;
}

extern "C" int rsx_sl_start(void* device, const void* luid, unsigned vendor, const char* mode)
{
    if (s_on) return 1;
    if (!device || !mode || ps3_settings_ieq(mode, "off")) return 0;
    s_mode = ps3_settings_ieq(mode, "boost") ? sl::ReflexMode::eLowLatencyWithBoost : sl::ReflexMode::eLowLatency;
    /* D3D12_REFLEX_ANY_GPU: Streamline on any GPU, PCL's markers alone where Reflex is not supported (a test). */
    const bool any = getenv("D3D12_REFLEX_ANY_GPU") != nullptr;
    if (vendor != 0x10DE && !any) {
        printf("[D3D12] reflex: off, not an NVIDIA GPU\n");
        return 0;
    }
    wchar_t dir[MAX_PATH], path[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, dir, MAX_PATH);
    while (n && dir[n - 1] != L'\\' && dir[n - 1] != L'/') n--;
    dir[n] = 0;
    if (!n || _snwprintf(path, MAX_PATH, L"%lssl.interposer.dll", dir) < 0 ||
        GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) {
        printf("[D3D12] reflex: off, no sl.interposer.dll beside the exe\n");
        return 0;
    }
    /* NVIDIA's signed build only: a file put there by anyone else is not loaded. */
    wchar_t who[160];
    if (!sl_signer(path, who, 160) || wcscmp(who, L"NVIDIA Corporation")) {
        printf("[D3D12] reflex: off, sl.interposer.dll is not signed by NVIDIA (%ls)\n", who[0] ? who : L"no valid signature");
        return 0;
    }
    s_mod = LoadLibraryExW(path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!s_mod) {
        printf("[D3D12] reflex: off, sl.interposer.dll did not load (error %lu)\n", GetLastError());
        return 0;
    }
#define SL_GET(p, name) (p = (decltype(p))(void*)GetProcAddress(s_mod, name))
    if (!SL_GET(p_init, "slInit") || !SL_GET(p_shutdown, "slShutdown") || !SL_GET(p_supported, "slIsFeatureSupported") ||
        !SL_GET(p_upgrade, "slUpgradeInterface") || !SL_GET(p_native, "slGetNativeInterface") ||
        !SL_GET(p_feature, "slGetFeatureFunction") || !SL_GET(p_token, "slGetNewFrameToken") ||
        !SL_GET(p_device, "slSetD3DDevice")) {
#undef SL_GET
        printf("[D3D12] reflex: off, sl.interposer.dll lacks a function\n");
        p_shutdown = nullptr;
        sl_unload();
        return 0;
    }
#undef SL_GET

    static const sl::Feature features[] = { sl::kFeatureReflex, sl::kFeaturePCL };
    const wchar_t* plugins[] = { dir };
    sl::Preferences pref{};
    pref.showConsole = false;
    pref.logLevel = getenv("D3D12_SL_VERBOSE") ? sl::LogLevel::eVerbose : sl::LogLevel::eDefault;
    pref.pathsToPlugins = plugins;
    pref.numPathsToPlugins = 1;
    pref.pathToLogsAndData = nullptr;
    pref.logMessageCallback = sl_log;
    /* No updates over the network and no plugins but the ones beside the exe. */
    pref.flags = sl::PreferenceFlags::eDisableCLStateTracking | sl::PreferenceFlags::eDisableDebugText |
                 sl::PreferenceFlags::eUseManualHooking;
    pref.featuresToLoad = features;
    pref.numFeaturesToLoad = (uint32_t)(sizeof features / sizeof features[0]);
    pref.engine = sl::EngineType::eCustom;
    pref.engineVersion = "ps3recomp";
    pref.projectId = k_project_id;
    pref.renderAPI = sl::RenderAPI::eD3D12;
    sl::Result r = p_init(pref, sl::kSDKVersion);
    if (r != sl::Result::eOk) {
        say_result("Streamline's start", r);
        p_shutdown = nullptr;
        sl_unload();
        return 0;
    }
    r = p_device(device);
    if (r != sl::Result::eOk) {
        say_result("Streamline's device", r);
        sl_unload();
        return 0;
    }
    sl::AdapterInfo ai{};
    ai.deviceLUID = (uint8_t*)luid;
    ai.deviceLUIDSizeInBytes = sizeof(LUID);
    const sl::Result rr = luid ? p_supported(sl::kFeatureReflex, ai) : sl::Result::eErrorNoSupportedAdapterFound;
    const sl::Result pr = luid ? p_supported(sl::kFeaturePCL, ai) : sl::Result::eErrorNoSupportedAdapterFound;
    if (rr != sl::Result::eOk && !any) {
        say_result("Reflex on this GPU", rr);
        sl_unload();
        return 0;
    }
    bool low = false;
    if (rr == sl::Result::eOk && feature_fn(sl::kFeatureReflex, "slReflexSleep", &p_sleep) &&
        feature_fn(sl::kFeatureReflex, "slReflexSetOptions", &p_options) &&
        feature_fn(sl::kFeatureReflex, "slReflexGetState", &p_rstate)) {
        sl::ReflexOptions o{};
        o.mode = s_mode;
        r = p_options(o);
        if (r != sl::Result::eOk) {
            say_result("Reflex's mode", r);
            sl_unload();
            return 0;
        }
        static sl::ReflexState st{};
        if (p_rstate(st) == sl::Result::eOk) low = st.lowLatencyAvailable;
    } else {
        p_sleep = nullptr;
        p_options = nullptr;
        p_rstate = nullptr;
    }
    if (pr == sl::Result::eOk && feature_fn(sl::kFeaturePCL, "slPCLSetMarker", &p_marker)) {
        /* The ping comes to this thread, the window's, as a thread message (rsx_sl_window_message). */
        if (feature_fn(sl::kFeaturePCL, "slPCLSetOptions", &p_poptions)) {
            sl::PCLOptions po{};
            po.idThread = GetCurrentThreadId();
            p_poptions(po);
        }
        sl::PCLState ps{};
        if (feature_fn(sl::kFeaturePCL, "slPCLGetState", &p_pstate) && p_pstate(ps) == sl::Result::eOk)
            s_ping_msg = ps.statsWindowMessage;
    }
    s_on = 1;
    /* Without low latency (a GPU that is not NVIDIA's, D3D12_REFLEX_ANY_GPU) Reflex's sleep returns at once and the
     * markers feed PC latency stats alone. */
    snprintf(s_label, sizeof s_label, "%s", !p_sleep || !low ? "Reflex: latency stats only"
             : s_mode == sl::ReflexMode::eLowLatencyWithBoost ? "Reflex On + Boost" : "Reflex On");
    printf("[D3D12] reflex: %s (Streamline %d.%d.%d, signed by NVIDIA Corporation; low latency %s, PC latency stats %s)\n",
           !p_sleep || !low ? "latency stats only" : s_mode == sl::ReflexMode::eLowLatencyWithBoost ? "on + boost" : "on",
           SL_VERSION_MAJOR, SL_VERSION_MINOR, SL_VERSION_PATCH, low ? "available" : "not available",
           p_marker ? "on" : "off");
    return 1;
}

extern "C" void* rsx_sl_factory(void* factory)
{
    if (!s_on || !factory || !p_upgrade) return nullptr;
    void* f = factory;
    const sl::Result r = p_upgrade(&f);
    if (r != sl::Result::eOk || !f) {
        say_result("Streamline's factory", r);
        sl_unload();
        return nullptr;
    }
    return f;
}

extern "C" int rsx_sl_on(void) { return s_on; }

extern "C" int rsx_sl_presents_through(void* swap_chain)
{
    if (!s_on || !p_native || !swap_chain) return 0;
    void* native = nullptr;
    if (p_native(swap_chain, &native) != sl::Result::eOk || !native) return 0;
    const int proxy = native != swap_chain;
    if (proxy) ((IUnknown*)native)->Release();
    return proxy;
}

extern "C" void rsx_sl_flip_release(int after)
{
    if (!s_on) return;
    if (!after) {
        /* The frame whose flip completes now is the one the backend presents next. */
        s_shown = s_sim && s_sim_ended ? s_sim : 0;
        if (p_sleep)
            if (sl::FrameToken* t = token(s_sim + 1)) p_sleep(*t);
        return;
    }
    s_sim++;
    s_sim_ended = 0;
    mark(sl::PCLMarker::eSimulationStart, s_sim);
    /* Asked again once frames have gone through it, in case the driver said no before the first. */
    if (s_sim == 120 && p_rstate) {
        static sl::ReflexState st{};
        const bool low = p_rstate(st) == sl::Result::eOk && st.lowLatencyAvailable;
        const char* was = s_label;
        char now[32];
        snprintf(now, sizeof now, "%s", !low ? "Reflex: latency stats only"
                 : s_mode == sl::ReflexMode::eLowLatencyWithBoost ? "Reflex On + Boost" : "Reflex On");
        if (strcmp(was, now)) {
            printf("[D3D12] reflex: low latency %s after 120 frames\n", low ? "available" : "not available");
            snprintf(s_label, sizeof s_label, "%s", now);
        }
    }
}

extern "C" void rsx_sl_flip_reached(void)
{
    if (!s_on || !s_sim || s_sim_ended) return;
    s_sim_ended = 1;
    mark(sl::PCLMarker::eSimulationEnd, s_sim);
}

extern "C" void rsx_sl_marker(int which)
{
    if (!s_on) return;
    if (which == RSX_SL_SUBMIT_START) {
        /* Once a frame: a frame presented again (a display clear's render) has had its markers. */
        s_present = s_shown && s_shown != s_marked ? s_shown : 0;
        if (s_present) s_marked = s_present;
    }
    static const sl::PCLMarker k[] = { sl::PCLMarker::eRenderSubmitStart, sl::PCLMarker::eRenderSubmitEnd,
                                        sl::PCLMarker::ePresentStart, sl::PCLMarker::ePresentEnd };
    if (which >= 0 && which < 4) mark(k[which], s_present);
}

extern "C" void rsx_sl_window_message(unsigned msg)
{
    if (s_on && s_ping_msg && msg == s_ping_msg) mark(sl::PCLMarker::ePCLatencyPing, s_sim);
}

extern "C" const char* rsx_sl_label(void) { return s_on ? s_label : ""; }

extern "C" void rsx_sl_shutdown(void)
{
    if (s_on) sl_unload();
}

#endif /* _WIN32 */
