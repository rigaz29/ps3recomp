/*
 * ps3recomp - ANR FG through its DLSS-G interface (beta, PS3RECOMP_ANR): see rsx_anr.h.
 *
 * The DLL's own NGX exports, in the order a DLSS-G host calls them: Init_Ext, its capabilities
 * into our parameter map, CreateFeature, then one EvaluateFeature per generated frame on the
 * command list the frame goes out on. The DLL runs its model on a Vulkan device of its own and
 * keeps the two APIs in step itself (its log: "queue hooks ready"). The textures go in the states
 * DLSS-G hosts give them: the backbuffer NON_PIXEL_SHADER_RESOURCE; the picture without its HUD,
 * depth and motion COPY_DEST; the outputs UNORDERED_ACCESS. NGX's types and parameter names come
 * from rsx_ngx.h, our own declarations of the interface.
 */
#ifdef _WIN32

#include <windows.h>
#include <d3d12.h>
#include <bcrypt.h>
#include <softpub.h>
#include <wincrypt.h>
#include <wintrust.h>
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include <string>
#include <unordered_map>

#include "rsx_anr.h"
#include "rsx_ngx.h"

namespace {

/* A host's parameter map. NGX's values carry no type: a number reads back as whichever type is
 * asked for, a resource as itself or as a void*. */
class anr_params final : public ngx_parameter {
public:
    void Set(const char* k, unsigned long long v) override { number(k, (double)v); }
    void Set(const char* k, float v) override { number(k, v); }
    void Set(const char* k, double v) override { number(k, v); }
    void Set(const char* k, unsigned int v) override { number(k, v); }
    void Set(const char* k, int v) override { number(k, v); }
    void Set(const char* k, ID3D11Resource* v) override { pointer(k, v, K_D3D11); }
    void Set(const char* k, ID3D12Resource* v) override { pointer(k, v, K_D3D12); }
    void Set(const char* k, void* v) override { pointer(k, v, K_ANY); }
    ngx_result Get(const char* k, unsigned long long* v) const override { return read(k, v); }
    ngx_result Get(const char* k, float* v) const override { return read(k, v); }
    ngx_result Get(const char* k, double* v) const override { return read(k, v); }
    ngx_result Get(const char* k, unsigned int* v) const override { return read(k, v); }
    ngx_result Get(const char* k, int* v) const override { return read(k, v); }
    ngx_result Get(const char* k, ID3D11Resource** v) const override { return read_pointer(k, (void**)v, K_D3D11); }
    ngx_result Get(const char* k, ID3D12Resource** v) const override { return read_pointer(k, (void**)v, K_D3D12); }
    ngx_result Get(const char* k, void** v) const override { return read_pointer(k, v, K_ANY); }
    void Reset() override { values.clear(); }

private:
    enum kind { K_NUMBER, K_ANY, K_D3D11, K_D3D12 };
    struct value {
        kind what;
        double n;
        void* p;
    };
    std::unordered_map<std::string, value> values;

    void number(const char* k, double n) { values[k] = value{K_NUMBER, n, nullptr}; }
    void pointer(const char* k, void* p, kind what) { values[k] = value{what, 0.0, p}; }
    const value* find(const char* k) const
    {
        const auto it = values.find(k);
        return it == values.end() ? nullptr : &it->second;
    }
    static void as(double n, unsigned long long* o) { *o = n < 0.0 ? (unsigned long long)(long long)n : (unsigned long long)n; }
    static void as(double n, unsigned int* o) { *o = n < 0.0 ? (unsigned int)(int)n : (unsigned int)n; }
    static void as(double n, int* o) { *o = (int)n; }
    static void as(double n, float* o) { *o = (float)n; }
    static void as(double n, double* o) { *o = n; }
    template <class T>
    ngx_result read(const char* k, T* out) const
    {
        const value* v = find(k);
        if (!out || !v || v->what != K_NUMBER) return ngx_fail;
        as(v->n, out);
        return ngx_ok;
    }
    ngx_result read_pointer(const char* k, void** out, kind what) const
    {
        const value* v = find(k);
        if (!out || !v || v->what == K_NUMBER || (what != K_ANY && v->what != K_ANY && v->what != what))
            return ngx_fail;
        *out = v->p;
        return ngx_ok;
    }
};

}  // namespace

static HMODULE s_dll;
static ngx_init_fn s_init;
static ngx_populate_fn s_populate;
static ngx_create_fn s_create;
static ngx_evaluate_fn s_evaluate;
static ngx_release_fn s_release;
static anr_params s_p;
static ngx_handle* s_feature;
static bool s_create_failed;                /* not tried again until a release */
static unsigned s_fail_run;                 /* evaluations failed in a row */
static ID3D12Device* s_dev;
static ID3D12Resource* s_real;              /* OutputReal: the DLL copies the real frame in; nothing reads it */
static DXGI_FORMAT s_format;
static unsigned s_w, s_h, s_rw, s_rh;
static bool s_still_camera;                 /* D3D12_ANR_CAMERA=0: a camera that never moves, as OptiScaler gave it */
/* The matrices go by pointer, and may be read after EvaluateFeature returns: a few frames' worth. */
static float s_mats[4][2][16];
static unsigned s_mat_i;
static float s_none[16];                    /* FLT_MAX throughout: a matrix not given */
static unsigned long long s_made, s_twice, s_failed;

/* The DLL's full path: as given, or from the exe's folder. */
static bool anr_path(const char* dll, wchar_t* out)
{
    wchar_t w[MAX_PATH];
    if (!dll || !*dll || !MultiByteToWideChar(CP_UTF8, 0, dll, -1, w, MAX_PATH)) return false;
    if ((w[0] && w[1] == L':') || (w[0] == L'\\' && w[1] == L'\\')) {
        wcscpy(out, w);
        return true;
    }
    DWORD n = GetModuleFileNameW(nullptr, out, MAX_PATH);
    while (n && out[n - 1] != L'\\' && out[n - 1] != L'/') n--;
    if (!n || n + wcslen(w) >= MAX_PATH) return false;
    wcscpy(out + n, w);
    return true;
}

/* Who signed the file, by Authenticode, into who (its certificate's simple name): 1 when the signature checks out
 * against Windows' trusted roots, offline (no revocation lookups: a game start must not wait on the network). */
static int anr_signer(const wchar_t* path, wchar_t* who, DWORD n)
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

/* The file's size and the start of its SHA-256 (16 hex digits), which names the build in a log. */
static bool anr_hash(const wchar_t* path, unsigned long long* size, char* hex)
{
    hex[0] = 0;
    *size = 0;
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hh = nullptr;
    bool ok = BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0)) &&
              BCRYPT_SUCCESS(BCryptCreateHash(alg, &hh, nullptr, 0, nullptr, 0, 0));
    static unsigned char buf[1 << 16];
    DWORD got = 0;
    while (ok && ReadFile(h, buf, sizeof buf, &got, nullptr) && got) {
        ok = BCRYPT_SUCCESS(BCryptHashData(hh, buf, got, 0));
        *size += got;
    }
    unsigned char d[32];
    ok = ok && BCRYPT_SUCCESS(BCryptFinishHash(hh, d, sizeof d, 0));
    if (hh) BCryptDestroyHash(hh);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    CloseHandle(h);
    if (ok)
        for (int i = 0; i < 8; i++) snprintf(hex + 2 * i, 3, "%02x", d[i]);
    return ok;
}

int rsx_anr_open(ID3D12Device* device, const char* dll)
{
    if (s_dll) return s_dev == device;
    wchar_t path[MAX_PATH];
    if (!anr_path(dll, path)) {
        printf("[anr] no DLL to load (graphics.anr_dll)\n");
        return 0;
    }
    /* Signed by Apex Compute since FG V.B16; a file put there by anyone else is not loaded. */
    wchar_t who[160];
    const int signed_ok = anr_signer(path, who, 160);
    const char* any = getenv("D3D12_ANR_UNSIGNED");
    const bool apex = signed_ok && !wcscmp(who, L"Apex Compute, Inc.");
    unsigned long long size = 0;
    char hex[17];
    anr_hash(path, &size, hex);
    if (!apex && !(any && !strcmp(any, "1"))) {
        printf("[anr] %ls: %s%ls; not loaded (D3D12_ANR_UNSIGNED=1 loads it anyway)\n", path,
               signed_ok ? "signed by " : "not signed by Apex Compute, Inc.", signed_ok ? who : L"");
        return 0;
    }
    printf("[anr] %ls: %s%ls, %llu bytes, SHA-256 %s...\n", path, signed_ok ? "signed by " : "unsigned, loaded as asked",
           signed_ok ? who : L"", size, hex);
    HMODULE m = LoadLibraryExW(path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!m) {
        printf("[anr] %ls: not loaded (error %lu)\n", path, GetLastError());
        return 0;
    }
    s_init = (ngx_init_fn)(void*)GetProcAddress(m, "NVSDK_NGX_D3D12_Init_Ext");
    s_populate = (ngx_populate_fn)(void*)GetProcAddress(m, "NVSDK_NGX_D3D12_PopulateParameters_Impl");
    s_create = (ngx_create_fn)(void*)GetProcAddress(m, "NVSDK_NGX_D3D12_CreateFeature");
    s_evaluate = (ngx_evaluate_fn)(void*)GetProcAddress(m, "NVSDK_NGX_D3D12_EvaluateFeature");
    s_release = (ngx_release_fn)(void*)GetProcAddress(m, "NVSDK_NGX_D3D12_ReleaseFeature");
    if (!s_init || !s_create || !s_evaluate || !s_release) {
        printf("[anr] %ls: no DLSS-G entry points\n", path);
        FreeLibrary(m);
        return 0;
    }
    wchar_t temp[MAX_PATH];
    if (!GetTempPathW(MAX_PATH, temp)) temp[0] = 0;
    /* Application 1: there is no NVIDIA project to name, and nothing of NVIDIA's runs. Left loaded
     * when this fails: it may have hooked the device's queues by then. */
    const ngx_result r = s_init(1, temp, device, ngx_version_api, nullptr);
    if (ngx_failed(r)) {
        printf("[anr] %ls: Init_Ext failed (0x%08X)\n", path, (unsigned)r);
        return 0;
    }
    if (s_populate) s_populate(&s_p);
    unsigned most = 0;
    s_p.Get(ngx_p::multi_frame_count_max, &most);
    for (float& v : s_none) v = FLT_MAX;
    const char* cam = getenv("D3D12_ANR_CAMERA");
    s_still_camera = cam && !strcmp(cam, "0");
    s_dll = m;
    s_dev = device;
    printf("[anr] %ls: up to %u generated frames to a real one%s\n", path, most,
           s_still_camera ? ", given a camera that never moves" : "");
    return 1;
}

static void anr_transition(ID3D12GraphicsCommandList* cl, ID3D12Resource* const* r, const D3D12_RESOURCE_STATES* from,
                           const D3D12_RESOURCE_STATES* to, int n)
{
    D3D12_RESOURCE_BARRIER b[8];
    UINT k = 0;
    for (int i = 0; i < n && k < 8; i++) {
        if (!r[i] || from[i] == to[i]) continue;
        b[k] = {};
        b[k].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b[k].Transition.pResource = r[i];
        b[k].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b[k].Transition.StateBefore = from[i];
        b[k].Transition.StateAfter = to[i];
        k++;
    }
    if (k) cl->ResourceBarrier(k, b);
}

/* The real frame into the output, shown twice where there is no generated frame to show. */
static void anr_copy_real(ID3D12GraphicsCommandList* cl, const rsx_anr_frame* f)
{
    if (!f->backbuffer || !f->output) return;
    ID3D12Resource* const r[2] = { f->backbuffer, f->output };
    const D3D12_RESOURCE_STATES was[2] = { f->backbuffer_state, f->output_state };
    const D3D12_RESOURCE_STATES copy[2] = { D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST };
    anr_transition(cl, r, was, copy, 2);
    cl->CopyResource(f->output, f->backbuffer);
    anr_transition(cl, r, copy, was, 2);
}

static bool anr_create(ID3D12GraphicsCommandList* cl, const rsx_anr_frame* f)
{
    const D3D12_RESOURCE_DESC bd = f->backbuffer->GetDesc();
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = f->display_w;
    rd.Height = f->display_h;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.Format = bd.Format;
    rd.SampleDesc.Count = 1;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    if (FAILED(s_dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                              IID_PPV_ARGS(&s_real)))) {
        printf("[anr] no texture for the real frame\n");
        s_real = nullptr;
        return false;
    }
    s_p.Set(ngx_p::creation_node_mask, 1u);
    s_p.Set(ngx_p::visibility_node_mask, 1u);
    s_p.Set(ngx_p::ngx_width, f->display_w);
    s_p.Set(ngx_p::ngx_height, f->display_h);
    s_p.Set(ngx_p::width, f->display_w);
    s_p.Set(ngx_p::height, f->display_h);
    s_p.Set(ngx_p::backbuffer_format, (unsigned)bd.Format);
    s_p.Set(ngx_p::internal_width, f->render_w);
    s_p.Set(ngx_p::internal_height, f->render_h);
    s_p.Set(ngx_p::dynamic_resolution, 0u);
    const ngx_result r = s_create(cl, ngx_feature_frame_generation, &s_p, &s_feature);
    if (ngx_failed(r) || !s_feature) {
        printf("[anr] CreateFeature failed (0x%08X)\n", (unsigned)r);
        s_feature = nullptr;
        s_real->Release();
        s_real = nullptr;
        return false;
    }
    s_format = bd.Format;
    s_w = f->display_w;
    s_h = f->display_h;
    s_rw = f->render_w;
    s_rh = f->render_h;
    printf("[anr] frame generation: %ux%u from %ux%u, DXGI format %u\n", s_w, s_h, s_rw, s_rh, (unsigned)s_format);
    return true;
}

static void anr_rect(const char* x, const char* y, const char* w, const char* h, unsigned width, unsigned height)
{
    s_p.Set(x, 0u);
    s_p.Set(y, 0u);
    s_p.Set(w, width);
    s_p.Set(h, height);
}

/* One frame's parameters, as a DLSS-G host sets them each frame. */
static void anr_frame_params(const rsx_anr_frame* f, bool reset)
{
    ngx_parameter& p = s_p;
    p.Set(ngx_p::backbuffer, f->backbuffer);
    p.Set(ngx_p::hud_less, f->hudless);
    p.Set(ngx_p::ui, (ID3D12Resource*)nullptr);
    p.Set(ngx_p::ui_alpha, (ID3D12Resource*)nullptr);
    p.Set(ngx_p::depth, f->depth);
    p.Set(ngx_p::mvecs, f->motion);
    p.Set(ngx_p::output_interpolated, f->output);
    p.Set(ngx_p::output_real, s_real);
    p.Set(ngx_p::width, s_w);
    p.Set(ngx_p::height, s_h);
    p.Set(ngx_p::backbuffer_format, (unsigned)s_format);
    p.Set(ngx_p::multi_frame_count, 1u);
    p.Set(ngx_p::multi_frame_index, 1u);
    p.Set(ngx_p::reset, reset ? 1u : 0u);
    p.Set(ngx_p::not_rendering_game_frames, 0u);
    p.Set(ngx_p::eval_flags, 0u);
    p.Set(ngx_p::user_interface_recomposition_enabled, 0u);
    p.Set(ngx_p::menu_detection_enabled, 0u);
    p.Set(ngx_p::color_buffers_hdr, 0u);
    p.Set(ngx_p::depth_inverted, 0u);
    p.Set(ngx_p::ortho_projection, 0u);
    p.Set(ngx_p::camera_motion_included, 1u);
    p.Set(ngx_p::mvec_dilated, 0u);
    p.Set(ngx_p::mvec_jittered, 0u);
    /* No motion marks itself unknown: unset, the DLL reads 0, which is what a still pixel holds. */
    p.Set(ngx_p::mvec_invalid_value, FLT_MAX);
    /* The motion is in UV units, as FSR takes it: this many pixels to one. */
    p.Set(ngx_p::mvec_scale_x, (float)s_rw);
    p.Set(ngx_p::mvec_scale_y, (float)s_rh);
    p.Set(ngx_p::jitter_offset_x, f->jitter_x);
    p.Set(ngx_p::jitter_offset_y, f->jitter_y);
    p.Set(ngx_p::min_relative_linear_depth_object_separation, 40.0f);
    anr_rect(ngx_p::mvecs_subrect_base_x, ngx_p::mvecs_subrect_base_y,
             ngx_p::mvecs_subrect_width, ngx_p::mvecs_subrect_height, s_rw, s_rh);
    anr_rect(ngx_p::depth_subrect_base_x, ngx_p::depth_subrect_base_y,
             ngx_p::depth_subrect_width, ngx_p::depth_subrect_height, s_rw, s_rh);
    anr_rect(ngx_p::hud_less_subrect_base_x, ngx_p::hud_less_subrect_base_y,
             ngx_p::hud_less_subrect_width, ngx_p::hud_less_subrect_height, s_w, s_h);
    anr_rect(ngx_p::backbuffer_subrect_base_x, ngx_p::backbuffer_subrect_base_y,
             ngx_p::backbuffer_subrect_width, ngx_p::backbuffer_subrect_height, s_w, s_h);

    const float aspect = (float)s_w / (float)s_h;
    p.Set(ngx_p::camera_near, f->znear);
    p.Set(ngx_p::camera_far, f->zfar);
    p.Set(ngx_p::camera_fov, f->fov);
    p.Set(ngx_p::camera_aspect_ratio, aspect);
    p.Set(ngx_p::camera_pinhole_offset_x, 0.0f);
    p.Set(ngx_p::camera_pinhole_offset_y, 0.0f);
    static const float still_pos[3] = { 0.0f, 0.0f, 0.0f }, still_up[3] = { 0.0f, 1.0f, 0.0f };
    static const float still_right[3] = { 1.0f, 0.0f, 0.0f }, still_fwd[3] = { 0.0f, 0.0f, -1.0f };
    const float* pos = s_still_camera ? still_pos : f->pos;
    const float* up = s_still_camera ? still_up : f->up;
    const float* right = s_still_camera ? still_right : f->right;
    const float* fwd = s_still_camera ? still_fwd : f->fwd;
    p.Set(ngx_p::camera_pos_x, pos[0]);
    p.Set(ngx_p::camera_pos_y, pos[1]);
    p.Set(ngx_p::camera_pos_z, pos[2]);
    p.Set(ngx_p::camera_up_x, up[0]);
    p.Set(ngx_p::camera_up_y, up[1]);
    p.Set(ngx_p::camera_up_z, up[2]);
    p.Set(ngx_p::camera_right_x, right[0]);
    p.Set(ngx_p::camera_right_y, right[1]);
    p.Set(ngx_p::camera_right_z, right[2]);
    p.Set(ngx_p::camera_fwd_x, fwd[0]);
    p.Set(ngx_p::camera_fwd_y, fwd[1]);
    p.Set(ngx_p::camera_fwd_z, fwd[2]);

    /* The projection from the planes and the field of view, right-handed, by rows with the point
     * as a row, and its inverse; the clip-to-clip ones not given. */
    float (*m)[16] = s_mats[s_mat_i++ & 3];
    const bool lens = f->fov > 0.0f && f->znear > 0.0f && f->zfar > f->znear;
    if (lens) {
        const double y = 1.0 / tan(f->fov * 0.5), x = y / aspect, n = f->znear, fz = f->zfar;
        const float proj[16] = { (float)x, 0, 0, 0, 0, (float)y, 0, 0, 0, 0, (float)(fz / (n - fz)), -1.0f,
                                 0, 0, (float)(n * fz / (n - fz)), 0 };
        const float unproj[16] = { (float)(1.0 / x), 0, 0, 0, 0, (float)(1.0 / y), 0, 0, 0, 0, 0, (float)((n - fz) / (n * fz)),
                                   0, 0, -1.0f, (float)(1.0 / n) };
        memcpy(m[0], proj, sizeof proj);
        memcpy(m[1], unproj, sizeof unproj);
    }
    p.Set(ngx_p::camera_view_to_clip, (void*)(lens ? m[0] : s_none));
    p.Set(ngx_p::clip_to_camera_view, (void*)(lens ? m[1] : s_none));
    p.Set(ngx_p::clip_to_lens_clip, (void*)s_none);
    p.Set(ngx_p::clip_to_prev_clip, (void*)s_none);
    p.Set(ngx_p::prev_clip_to_clip, (void*)s_none);
}

int rsx_anr_generate(ID3D12GraphicsCommandList* cl, const rsx_anr_frame* f)
{
    if (!s_dll || s_create_failed || !f->backbuffer || !f->output || !f->depth || !f->motion) {
        anr_copy_real(cl, f);
        return 0;
    }
    if (s_feature && (f->display_w != s_w || f->display_h != s_h || f->render_w != s_rw || f->render_h != s_rh)) {
        static int told;
        if (!told++)
            printf("[anr] %ux%u from %ux%u, the feature's %ux%u from %ux%u: not generating\n", f->display_w, f->display_h,
                   f->render_w, f->render_h, s_w, s_h, s_rw, s_rh);
        anr_copy_real(cl, f);
        return 0;
    }
    bool reset = f->reset != 0;
    if (!s_feature) {
        if (!anr_create(cl, f)) {
            s_create_failed = true;
            anr_copy_real(cl, f);
            return 0;
        }
        reset = true;
    }
    const D3D12_RESOURCE_STATES read = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    ID3D12Resource* const r[5] = { f->backbuffer, f->output, f->hudless, f->depth, f->motion };
    const D3D12_RESOURCE_STATES was[5] = { f->backbuffer_state, f->output_state, read, read, read };
    const D3D12_RESOURCE_STATES host[5] = { D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                            D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_DEST,
                                            D3D12_RESOURCE_STATE_COPY_DEST };
    anr_frame_params(f, reset);
    anr_transition(cl, r, was, host, 5);
    const ngx_result res = s_evaluate(cl, s_feature, &s_p, nullptr);
    anr_transition(cl, r, host, was, 5);
    const bool ok = !ngx_failed(res);
    if (!ok && !s_failed++) printf("[anr] EvaluateFeature failed (0x%08X)\n", (unsigned)res);
    s_fail_run = ok ? 0 : s_fail_run + 1;
    if (!ok || reset) {
        if (ok) s_twice++;
        anr_copy_real(cl, f);
        return 0;
    }
    if (!s_made++) printf("[anr] first generated frame\n");
    return 1;
}

int rsx_anr_usable(void)
{
    return s_dll && !s_create_failed && s_fail_run < 10;
}

void rsx_anr_release(void)
{
    if (s_feature) {
        s_release(s_feature);
        s_feature = nullptr;
        printf("[anr] released: %llu frames generated, %llu resets shown as the real frame, %llu failed\n", s_made, s_twice,
               s_failed);
    }
    if (s_real) {
        s_real->Release();
        s_real = nullptr;
    }
    s_create_failed = false;
    s_fail_run = 0;
}

#endif /* _WIN32 */
