#!/usr/bin/env python3
"""cellSaveDataAutoSave2 keeps the directory name wherever the guest's memory is.

AutoSave2 translated dirName from a guest address and handed the host pointer to
cellSaveDataAutoSave, which translated it again. On a PC that reserved the
guest's memory below 4 GB the host pointer read as a guest address, the name
came out empty, and every save went into the save root itself, where neither
the title nor a launcher found it again (Ascension-R issue #10).

libs/system/tests/test_savedata_autosave2.c maps the guest's memory at 0x10000
and checks the name the title's stat callback is given. A system that will not
map memory that low skips it. It needs a host C compiler (under CI=1 a missing
one fails instead of skipping).

Usage:
    python3 tools/test_savedata_autosave2.py
"""

import os
import shutil
import subprocess
import sys
import tempfile

TOOLS = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(TOOLS)
SOURCE = os.path.join(ROOT, "libs", "system", "tests", "test_savedata_autosave2.c")


def _cc():
    return (shutil.which(os.environ.get("CC", "cc")) or shutil.which("clang")
            or shutil.which("gcc"))


def main():
    cc = _cc()
    if cc is None:
        if os.environ.get("CI"):
            print("FAIL: no C compiler found")
            return 1
        print("skip test_savedata_autosave2.c: no C compiler")
        return 0
    with tempfile.TemporaryDirectory() as td:
        exe = os.path.join(td, "test_savedata_autosave2")
        cmd = [cc, "-std=gnu17", "-w", "-I" + os.path.join(ROOT, "include"), "-o", exe, SOURCE]
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode != 0:
            print("FAIL: fixture did not build")
            print((r.stdout + r.stderr)[-4000:])
            return 1
        scratch = os.path.join(td, "saves")
        os.mkdir(scratch)
        r = subprocess.run([exe, scratch], capture_output=True, text=True)
        lines = [l for l in r.stdout.splitlines() if not l.startswith(("[cellSaveData]", "    SP+"))]
        print("\n".join(lines))
        if r.returncode != 0:
            print("FAIL")
            return 1
    print("PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
