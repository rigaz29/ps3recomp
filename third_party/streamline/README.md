# NVIDIA Streamline headers

From [Streamline SDK 2.14.16](https://github.com/NVIDIA-RTX/Streamline/releases/tag/v2.14.16)
(`streamline-sdk-v2.14.16.zip`, SHA-256 `32041e51db0cab709a271b72a352281b690c2abbd669a827809401a5dd23956b`),
`include/`, unchanged: the twelve headers `sl.h`, `sl_reflex.h`, `sl_pcl.h` and `sl_dlss_g.h` take. MIT License,
Copyright (c) NVIDIA Corporation: `license.txt`, and the text at the top of each file. `3rd-party-licenses.md` is the
zip's own, the notices of the code in Streamline's DLLs, which releases carry beside them.

Only the headers are here. `libs/video/rsx_sl.cpp` loads Streamline at run time, from `sl.interposer.dll` beside the exe,
once Windows has checked NVIDIA's signature on it; Streamline then loads its plugins (`sl.common.dll`, `sl.reflex.dll`,
`sl.pcl.dll`, `sl.dlss_g.dll`) from the same folder and checks theirs. Those DLLs are the signed ones of the same zip's
`bin/x64`; the port's `gowa.py build` copies them beside the exe and its releases carry them. DLSS frame generation's
model, `nvngx_dlssg.dll`, is NVIDIA's under the RTX SDKs license and is not among them: the port's launcher fetches it
from NVIDIA's DLSS repository into `dlss\`, a folder `rsx_sl.cpp` gives Streamline to look in.
