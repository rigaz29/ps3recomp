# SMAA (Enhanced Subpixel Morphological Antialiasing)

`SMAA.hlsl`, `AreaTex.h`, `SearchTex.h` and `LICENSE.txt`, unchanged, from
[iryoku/smaa](https://github.com/iryoku/smaa) (commit
`71c806a838bdd7d517df19192a20f0c61b3ca29d`; `AreaTex.h` and `SearchTex.h` are its `Textures/`).
MIT license with a clarification that binaries need not carry the notice; see `LICENSE.txt`.
Copyright (C) 2013 Jorge Jimenez, Jose I. Echevarria, Belen Masia, Fernando Navarro and
Diego Gutierrez.

Used by the D3D12 backend's presentation pass (`libs/video/rsx_smaa.c`) for SMAA 1x on the
title's frame before it is scaled: luma edge detection, blending weights, neighbourhood
blending. `SMAA.hlsl` is compiled at run time with D3DCompile, through `SMAA_CUSTOM_SL` so its
samplers get fixed registers; the build embeds it (`cmake/EmbedFiles.cmake`). `AreaTex.h` and
`SearchTex.h` are compiled into the library and uploaded once.

The files keep upstream's CRLF line endings (`.gitattributes`), so their blobs match upstream's.
