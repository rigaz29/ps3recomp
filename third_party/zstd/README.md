# Zstandard

The D3D12 backend keeps its shader cache in one zstd frame (`libs/video/rsx_d3d12_backend.c`, "Compiled guest shaders
on disk").

Taken unchanged from the [Zstandard 1.5.7 release](https://github.com/facebook/zstd/releases/tag/v1.5.7),
`zstd-1.5.7.tar.gz` (SHA-256 `eb33e51f49a15e023950cd7825ca74a4a2b43db8354825ac24fc1b7ee09e6fa3`):

- `LICENSE`
- `lib/zstd.h`, `lib/zstd_errors.h`
- the `.c` and `.h` files of `lib/common`, `lib/compress` and `lib/decompress`

Left out: the assembly decoder (`huf_decompress_amd64.S`, which clang-cl does not build; `ZSTD_DISABLE_ASM`), the
legacy formats, the dictionary builder and the deprecated API. Multithreading is off (`ZSTD_MULTITHREAD` is not
defined).

Zstandard is dual-licensed, BSD or GPLv2 at the user's choice; it is used here under the BSD license in `LICENSE`.
Copyright (c) Meta Platforms, Inc. and affiliates.
