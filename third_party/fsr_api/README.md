# AMD FSR API headers

From [FSR SDK 2.4.0](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/releases/tag/v2.4.0), `Kits/FidelityFX`,
unchanged and in the same folders, because they include each other by relative path. MIT License, Copyright (C) Advanced
Micro Devices, Inc.: the text is at the top of each file.

Only the headers are here. `libs/video/rsx_fsr3.cpp` loads the API at run time, from
`amd_fidelityfx_loader_dx12.dll` beside the exe, and the port's `gowa.py build` copies the DLLs there from the SDK.
