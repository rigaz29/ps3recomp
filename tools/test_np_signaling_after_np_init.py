#!/usr/bin/env python3
"""sceNpSignaling contexts can be created after sceNpInit alone.

The firmware exports no sceNpSignalingInit: titles call sceNpInit and then
sceNpSignalingCreateCtx. The runtime checked only its own init flag, so God
of War: Ascension's SAM transport stopped at its first context with
SCE_NP_SIGNALING_ERROR_NOT_INITIALIZED (0x80022A01). This builds
libs/network/tests/test_np_signaling_after_np_init.c with its second file
(sceNp.c, shared with test_np_basic_after_np_init) and runs it.

It needs a host C compiler (under CI=1 a missing one fails instead of
skipping).

Usage:
    python3 tools/test_np_signaling_after_np_init.py
"""

import os
import shutil
import subprocess
import sys
import tempfile

TOOLS = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(TOOLS)
TESTS = os.path.join(ROOT, "libs", "network", "tests")
SOURCES = [os.path.join(TESTS, "test_np_signaling_after_np_init.c"),
           os.path.join(TESTS, "test_np_basic_after_np_init_np.c")]


def _cc():
    return (shutil.which(os.environ.get("CC", "cc")) or shutil.which("clang")
            or shutil.which("gcc"))


def main():
    cc = _cc()
    if cc is None:
        if os.environ.get("CI"):
            print("FAIL: no C compiler found")
            return 1
        print("skip test_np_signaling_after_np_init: no C compiler")
        return 0
    with tempfile.TemporaryDirectory() as td:
        exe = os.path.join(td, "test_np_signaling_after_np_init")
        cmd = [cc, "-std=gnu17", "-w", "-I" + os.path.join(ROOT, "include"),
               "-o", exe] + SOURCES
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode != 0:
            print("FAIL: fixture did not build")
            print((r.stdout + r.stderr)[-4000:])
            return 1
        r = subprocess.run([exe], capture_output=True, text=True)
        sys.stdout.write(r.stdout)
        return r.returncode


if __name__ == "__main__":
    sys.exit(main())
