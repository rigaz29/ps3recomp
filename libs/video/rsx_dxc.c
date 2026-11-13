/*
 * ps3recomp - guest shaders through DXC, and under Wine the port's own (rsx_compile_own, below)
 *
 * A guest program met for the first time costs D3DCompile 126 ms on average,
 * up to a second, and the draws that use it wait for it: nearly all of a new
 * area's first minute. DXC takes the same programs in 47 ms, at most 320 ms.
 *
 * dxcompiler.dll and dxil.dll come with the Windows SDK and are looked for
 * beside the exe only. dxil.dll validates and signs the result, and D3D12
 * refuses a program that is not signed, so both are needed.
 *
 * Two of the options change what a program does:
 *
 * -Gis keeps floats exact. By default DXC marks every float operation as fast,
 * which lets it assume no value is a NaN, and `x == x`, the fragment
 * decompiler's NaN test on a program's outputs, folds to true and the test is
 * gone. D3DCompile keeps it. -Gis costs no compile time.
 *
 * -HV 2018 is the language D3DCompile takes. In 2021 `?:` no longer works on
 * vectors, and the decompilers use it that way.
 *
 * And one change to the text, see dxc_split_samplers.
 *
 * dxcapi.h is C++ only. The interfaces are declared here as far as they are
 * used. IDxcBlob is by definition an alias of ID3DBlob.
 */
#ifdef _WIN32

#include <windows.h>
#include <d3dcompiler.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "rsx_dxc.h"

typedef struct { LPCVOID ptr; SIZE_T size; UINT encoding; } DxcBuffer;
#define DXC_CP_UTF8 65001u

typedef struct DxcResult DxcResult;
struct DxcResult {
    const struct {
        HRESULT (STDMETHODCALLTYPE* QueryInterface)(DxcResult*, REFIID, void**);
        ULONG   (STDMETHODCALLTYPE* AddRef)(DxcResult*);
        ULONG   (STDMETHODCALLTYPE* Release)(DxcResult*);
        HRESULT (STDMETHODCALLTYPE* GetStatus)(DxcResult*, HRESULT*);
        HRESULT (STDMETHODCALLTYPE* GetResult)(DxcResult*, ID3DBlob**);
        HRESULT (STDMETHODCALLTYPE* GetErrorBuffer)(DxcResult*, ID3DBlob**);
    }* lpVtbl;
};

typedef struct DxcCompiler DxcCompiler;
struct DxcCompiler {
    const struct {
        HRESULT (STDMETHODCALLTYPE* QueryInterface)(DxcCompiler*, REFIID, void**);
        ULONG   (STDMETHODCALLTYPE* AddRef)(DxcCompiler*);
        ULONG   (STDMETHODCALLTYPE* Release)(DxcCompiler*);
        HRESULT (STDMETHODCALLTYPE* Compile)(DxcCompiler*, const DxcBuffer*, LPCWSTR*, UINT32, void*,
                                             REFIID, void**);
    }* lpVtbl;
};

static const GUID s_clsid_compiler =
    { 0x73e22d93, 0xe6ce, 0x47f3, { 0xb5, 0xbf, 0xf0, 0x66, 0x4f, 0x39, 0xc1, 0xb0 } };
static const GUID s_iid_compiler3 =
    { 0x228b4687, 0x5a6a, 0x4730, { 0x90, 0x0c, 0x97, 0x02, 0xb2, 0x20, 0x3f, 0x54 } };
static const GUID s_iid_result =
    { 0x58346cda, 0xdde7, 0x4497, { 0x94, 0x61, 0x6f, 0x87, 0xaf, 0x5e, 0x06, 0x59 } };

typedef HRESULT (WINAPI* DxcCreateInstanceFn)(REFCLSID, REFIID, void**);
static DxcCreateInstanceFn s_create;

int rsx_dxc_available(void)
{
    static int tried;
    if (tried) return s_create != NULL;
    tried = 1;
    wchar_t p[MAX_PATH];
    DWORD n = GetModuleFileNameW(NULL, p, MAX_PATH);
    while (n && p[n - 1] != L'\\' && p[n - 1] != L'/') n--;
    if (!n || n + 16 >= MAX_PATH) return 0;
    wcscpy(p + n, L"dxil.dll");
    if (!LoadLibraryW(p)) return 0;
    wcscpy(p + n, L"dxcompiler.dll");
    HMODULE m = LoadLibraryW(p);
    if (m) s_create = (DxcCreateInstanceFn)GetProcAddress(m, "DxcCreateInstance");
    return s_create != NULL;
}

/* A D3DCreateBlob copy: the blobs the backend keeps and hands around are all
 * of that kind, and a DXC blob goes back with its result. */
static ID3DBlob* dxc_copy(ID3DBlob* b, SIZE_T extra)
{
    ID3DBlob* c = NULL;
    const SIZE_T n = b->lpVtbl->GetBufferSize(b);
    if (FAILED(D3DCreateBlob(n + extra, &c)) || !c) return NULL;
    char* to = (char*)c->lpVtbl->GetBufferPointer(c);
    memcpy(to, b->lpVtbl->GetBufferPointer(b), n);
    if (extra) to[n] = 0;
    return c;
}

/* The backend's root signature has a table of its own for each sampler, filled
 * per draw. D3DCompile keeps of rsx_samp[16] only the samplers a program uses,
 * one register each, and that fits. To DXC it is what it says, a range of 16,
 * which D3D12 wants inside one table: every pipeline that sampled a texture was
 * refused, and the screen stayed black. The decompiler only indexes the array
 * with constants, so DXC gets sixteen names. The text, or NULL to compile src. */
static char* dxc_split_samplers(const char* src, size_t len, size_t* out_len)
{
    static const char decl[] = "SamplerState rsx_samp[16] : register(s0);";
    static const char use[]  = "rsx_samp[";
    const char* at = strstr(src, decl);
    if (!at) return NULL;
    char* o = (char*)malloc(len + 16 * 48);
    if (!o) return NULL;
    size_t n = (size_t)(at - src);
    memcpy(o, src, n);
    for (int i = 0; i < 16; i++)
        n += (size_t)sprintf(o + n, "SamplerState rsx_samp_%d : register(s%d);\n", i, i);
    const char* p = at + sizeof decl - 1;
    for (;;) {
        const char* u = strstr(p, use);
        const size_t head = (size_t)((u ? u : src + len) - p);
        memcpy(o + n, p, head);
        n += head;
        if (!u) break;
        memcpy(o + n, "rsx_samp_", 9);
        n += 9;
        p = u + sizeof use - 1;
        while (*p >= '0' && *p <= '9') o[n++] = *p++;
        if (*p == ']') p++;
    }
    *out_len = n;
    return o;
}

/* entry and target as D3DCompile takes them: the target's stage (vs_5_0's vs) at shader model 6.0. */
static HRESULT dxc_compile(const char* src, size_t len, const char* entry, const char* target, const wchar_t* level,
                           ID3DBlob** out, ID3DBlob** err)
{
    static __declspec(thread) DxcCompiler* cc;   /* an instance takes one compile at a time */
    *out = NULL;
    if (err) *err = NULL;
    if (!s_create) return E_FAIL;
    if (!cc) {
        const HRESULT hr = s_create(&s_clsid_compiler, &s_iid_compiler3, (void**)&cc);
        if (FAILED(hr)) { cc = NULL; return hr; }
    }
    wchar_t went[64], wtarget[8] = L"ps_6_0";
    size_t k = 0;
    for (; entry[k] && k + 1 < sizeof went / sizeof went[0]; k++) went[k] = (wchar_t)(unsigned char)entry[k];
    went[k] = 0;
    if (target[0] && target[1]) { wtarget[0] = (wchar_t)target[0]; wtarget[1] = (wchar_t)target[1]; }
    LPCWSTR args[] = {
        L"-T", wtarget, L"-E", went, level,
        L"-HV", L"2018", L"-Gis", L"-Qstrip_reflect", L"-Wno-everything",
    };
    size_t n = len;
    char* split = dxc_split_samplers(src, len, &n);
    const DxcBuffer in = { split ? split : src, n, DXC_CP_UTF8 };
    DxcResult* r = NULL;
    HRESULT hr = cc->lpVtbl->Compile(cc, &in, args, sizeof args / sizeof args[0], NULL, &s_iid_result, (void**)&r);
    free(split);
    if (FAILED(hr) || !r) return FAILED(hr) ? hr : E_FAIL;
    HRESULT st = E_FAIL;
    r->lpVtbl->GetStatus(r, &st);
    ID3DBlob* b = NULL;
    if (SUCCEEDED(st)) {
        r->lpVtbl->GetResult(r, &b);
        if (b) { *out = dxc_copy(b, 0); b->lpVtbl->Release(b); }
        if (!*out) st = E_OUTOFMEMORY;
    } else if (err) {
        r->lpVtbl->GetErrorBuffer(r, &b);
        if (b) { *err = dxc_copy(b, 1); b->lpVtbl->Release(b); }
    }
    r->lpVtbl->Release(r);
    return st;
}

HRESULT rsx_dxc_compile(const char* src, size_t len, const char* target, const wchar_t* level,
                        ID3DBlob** out, ID3DBlob** err)
{
    return dxc_compile(src, len, "main", target, level, out, err);
}

/* ---- the port's own programs -------------------------------------------------------------------
 * Under Wine, D3DCompile is Wine's own, on vkd3d-shader. It did not finish the vertex pass's program
 * in a minute (issue 12 of Ascension-R), and what it makes of the rest is not looked at here. There
 * the backend has DXC compile them (rsx_own_dxc_set), as it does the guest programs. */
static int s_own_dxc;

void rsx_own_dxc_set(int on) { s_own_dxc = on; }
int rsx_own_dxc(void) { return s_own_dxc; }

const char* rsx_wine_version(void)
{
    static int tried;
    static const char* v;
    if (!tried) {
        tried = 1;
        typedef const char* (CDECL* WineVersionFn)(void);
        HMODULE nt = GetModuleHandleW(L"ntdll.dll");
        WineVersionFn f = nt ? (WineVersionFn)(void*)GetProcAddress(nt, "wine_get_version") : NULL;
        v = f ? f() : NULL;
        if (f && !v) v = "?";
    }
    return v;
}

typedef struct { char* p; size_t n, cap; int bad; } DxcText;

static void dxc_text_add(DxcText* t, const char* s, size_t n)
{
    if (t->bad || !n) return;
    if (t->n + n + 1 > t->cap) {
        size_t cap = t->cap ? t->cap : 4096;
        while (cap < t->n + n + 1) cap *= 2;
        char* p = (char*)realloc(t->p, cap);
        if (!p) { t->bad = 1; return; }
        t->p = p;
        t->cap = cap;
    }
    memcpy(t->p + t->n, s, n);
    t->n += n;
    t->p[t->n] = 0;
}

/* src with each #include line that inc opens replaced by the file, so DXC gets one text, as
 * D3DCompile reads it. A line the handler does not open stays, and DXC says so. */
static void dxc_flatten(DxcText* t, const char* s, size_t n, ID3DInclude* inc, int depth)
{
    size_t i = 0;
    while (i < n) {
        size_t e = i;
        while (e < n && s[e] != '\n') e++;
        const size_t next = e < n ? e + 1 : e;
        const char* q = s + i;
        const char* end = s + e;
        int done = 0;
        while (q < end && (*q == ' ' || *q == '\t')) q++;
        if (inc && depth < 8 && q < end && *q == '#') {
            q++;
            while (q < end && (*q == ' ' || *q == '\t')) q++;
            if (end - q > 7 && !memcmp(q, "include", 7)) {
                q += 7;
                while (q < end && (*q == ' ' || *q == '\t')) q++;
                if (q < end && (*q == '"' || *q == '<')) {
                    const char close = *q == '"' ? '"' : '>';
                    const char* nm = ++q;
                    while (q < end && *q != close) q++;
                    char name[MAX_PATH];
                    LPCVOID data = NULL;
                    UINT bytes = 0;
                    if (q < end && (size_t)(q - nm) < sizeof name) {
                        memcpy(name, nm, (size_t)(q - nm));
                        name[q - nm] = 0;
                        if (SUCCEEDED(inc->lpVtbl->Open(inc, close == '"' ? D3D_INCLUDE_LOCAL : D3D_INCLUDE_SYSTEM,
                                                        name, NULL, &data, &bytes)) && data) {
                            dxc_flatten(t, (const char*)data, bytes, inc, depth + 1);
                            dxc_text_add(t, "\n", 1);
                            inc->lpVtbl->Close(inc, data);
                            done = 1;
                        }
                    }
                }
            }
        }
        if (!done) dxc_text_add(t, s + i, next - i);
        i = next;
    }
}

static HRESULT own_compile(LPCVOID src, SIZE_T len, LPCSTR name, const D3D_SHADER_MACRO* defines, ID3DInclude* include,
                           LPCSTR entry, LPCSTR target, UINT flags1, UINT flags2, ID3DBlob** out, ID3DBlob** err);

/* A program that took 100 ms or more is said, the first 24: where the time of a start went. */
HRESULT rsx_compile_own(LPCVOID src, SIZE_T len, LPCSTR name, const D3D_SHADER_MACRO* defines, ID3DInclude* include,
                        LPCSTR entry, LPCSTR target, UINT flags1, UINT flags2, ID3DBlob** out, ID3DBlob** err)
{
    LARGE_INTEGER f, a, b;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&a);
    const HRESULT hr = own_compile(src, len, name, defines, include, entry, target, flags1, flags2, out, err);
    QueryPerformanceCounter(&b);
    const double ms = (double)(b.QuadPart - a.QuadPart) * 1000.0 / (double)f.QuadPart;
    static volatile LONG said;
    if (ms >= 100.0 && InterlockedIncrement(&said) <= 24)
        printf("[d3d12] slow own shader compile: %s %s (%s, %s) took %.0f ms%c", name ? name : "?", entry, target,
               s_own_dxc ? "DXC" : "D3DCompile", ms, 10);
    return hr;
}

static HRESULT own_compile(LPCVOID src, SIZE_T len, LPCSTR name, const D3D_SHADER_MACRO* defines, ID3DInclude* include,
                           LPCSTR entry, LPCSTR target, UINT flags1, UINT flags2, ID3DBlob** out, ID3DBlob** err)
{
    if (!s_own_dxc)
        return D3DCompile(src, len, name, defines, include, entry, target, flags1, flags2, out, err);
    DxcText t = { NULL, 0, 0, 0 };
    for (const D3D_SHADER_MACRO* d = defines; d && d->Name; d++) {
        dxc_text_add(&t, "#define ", 8);
        dxc_text_add(&t, d->Name, strlen(d->Name));
        dxc_text_add(&t, " ", 1);
        if (d->Definition) dxc_text_add(&t, d->Definition, strlen(d->Definition));
        dxc_text_add(&t, "\n", 1);
    }
    dxc_flatten(&t, (const char*)src, len, include, 0);
    if (t.bad || !t.p) {
        free(t.p);
        *out = NULL;
        if (err) *err = NULL;
        return E_OUTOFMEMORY;
    }
    /* D3DCompile's optimisation levels: 0 is bit 14, 3 bit 15, 2 both, 1 neither */
    const UINT lv = flags1 & (D3DCOMPILE_OPTIMIZATION_LEVEL0 | D3DCOMPILE_OPTIMIZATION_LEVEL3);
    const wchar_t* level = (flags1 & D3DCOMPILE_SKIP_OPTIMIZATION) ? L"-Od"
                         : lv == D3DCOMPILE_OPTIMIZATION_LEVEL0   ? L"-O0"
                         : lv == D3DCOMPILE_OPTIMIZATION_LEVEL2   ? L"-O2"
                         : lv == D3DCOMPILE_OPTIMIZATION_LEVEL3   ? L"-O3" : L"-O1";
    const HRESULT hr = dxc_compile(t.p, t.n, entry, target, level, out, err);
    free(t.p);
    return hr;
}

#endif /* _WIN32 */
