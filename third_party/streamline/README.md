# NVIDIA Streamline headers

From [Streamline SDK 2.14.16](https://github.com/NVIDIA-RTX/Streamline/releases/tag/v2.14.16)
(`streamline-sdk-v2.14.16.zip`, SHA-256 `32041e51db0cab709a271b72a352281b690c2abbd669a827809401a5dd23956b`),
`include/`, unchanged: the eleven headers `sl.h`, `sl_reflex.h` and `sl_pcl.h` take. MIT License, Copyright (c) NVIDIA
Corporation: `license.txt`, and the text at the top of each file.

Only the headers are here. `libs/video/rsx_sl.cpp` loads Streamline at run time, from `sl.interposer.dll` beside the exe,
once Windows has checked NVIDIA's signature on it; Streamline then loads its plugins (`sl.common.dll`, `sl.reflex.dll`,
`sl.pcl.dll`) from the same folder and checks theirs. Those DLLs are the signed ones of the same zip's `bin/x64`; the
port's `gowa.py build` copies them beside the exe and its releases carry them.
