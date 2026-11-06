/*
 * ps3recomp - guest shaders through DXC (Windows only, see rsx_dxc.c)
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

/* D3DCompile's job for entry "main": src ends in a NUL, target vs_5_0 or ps_5_0
 * asks for the same stage at shader model 6.0, level is "-O1" or the like. *out
 * and *err are D3DCreateBlob blobs for the caller to release; err may be NULL.
 * After rsx_dxc_available said yes. Any thread. */
HRESULT rsx_dxc_compile(const char* src, size_t len, const char* target, const wchar_t* level,
                        ID3DBlob** out, ID3DBlob** err);

#ifdef __cplusplus
}
#endif
#endif /* _WIN32 */
#endif /* RSX_DXC_H */
