/*
 * ps3recomp - guest shaders through DXC, and under Wine the port's own (Windows only, see rsx_dxc.c)
 */
#ifndef RSX_DXC_H
#define RSX_DXC_H

#ifdef _WIN32
#include <stddef.h>
#include <d3dcommon.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 1 if dxcompiler.dll and dxil.dll load from the exe's folder. */
int rsx_dxc_available(void);

/* D3DCompile's job for entry "main": src ends in a NUL, target vs_5_0, ps_5_0 or
 * cs_5_0 asks for the same stage at shader model 6.0, level is "-O1" or the like. *out
 * and *err are D3DCreateBlob blobs for the caller to release; err may be NULL.
 * After rsx_dxc_available said yes. Any thread. */
HRESULT rsx_dxc_compile(const char* src, size_t len, const char* target, const wchar_t* level,
                        ID3DBlob** out, ID3DBlob** err);

/* The port's own programs (the presentation pass, FSR 1, SMAA, the copies, FSR 3's inputs), with
 * D3DCompile's arguments: D3DCompile, or DXC once rsx_own_dxc_set(1), the backend's choice, when it
 * has seen the device. For DXC, the include handler's files go in place of their #include lines and
 * the macros before the text. Any thread. */
HRESULT rsx_compile_own(LPCVOID src, SIZE_T len, LPCSTR name, const D3D_SHADER_MACRO* defines, ID3DInclude* include,
                        LPCSTR entry, LPCSTR target, UINT flags1, UINT flags2, ID3DBlob** out, ID3DBlob** err);
void rsx_own_dxc_set(int on);
int rsx_own_dxc(void);

/* Wine's version where this runs on Wine (its ntdll's wine_get_version), else NULL. */
const char* rsx_wine_version(void);

#ifdef __cplusplus
}
#endif
#endif /* _WIN32 */
#endif /* RSX_DXC_H */
