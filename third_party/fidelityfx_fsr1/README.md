# AMD FidelityFX Super Resolution 1 (FSR 1)

`ffx_a.h` and `ffx_fsr1.h`, unchanged, from
[GPUOpen-Effects/FidelityFX-FSR](https://github.com/GPUOpen-Effects/FidelityFX-FSR)
(`ffx-fsr/`, commit `a21ffb8f6c13233ba336352bdff293894c706575`, version 1.20210629).
MIT license, Copyright (c) 2021 Advanced Micro Devices, Inc.; the full text is at the top of
both files.

Used by the D3D12 backend's presentation pass (`libs/video/rsx_fsr1.c`):

- EASU, the edge-adaptive spatial upscale, and RCAS, the sharpening pass, as pixel shaders
  compiled at run time with D3DCompile;
- `FsrEasuCon` and `FsrRcasCon`, the headers' CPU code, for the passes' constants.

The build embeds both files in the runtime library (`cmake/EmbedFiles.cmake`), so the
executable needs no copy of them.
