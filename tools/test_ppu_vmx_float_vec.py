#!/usr/bin/env python3
"""ppu_vmx.h: the vector forms of vaddfp/vsubfp/vmaddfp/vnmsubfp equal the scalar ones bit for bit.

Builds runtime/ppu/tests/test_ppu_vmx_float_vec.c twice: with FMA3 (x86-64 only), where the
vector forms are used, and without, where the public names call the scalar forms.
"""
import os
import platform
import subprocess
import tempfile
from pathlib import Path

if os.name == 'nt':
    print('SKIP: fixture currently requires the POSIX compiler')
    raise SystemExit(0)
root = Path(__file__).resolve().parents[1]
src = root / 'runtime/ppu/tests/test_ppu_vmx_float_vec.c'
builds = [('scalar', [])]
if platform.machine().lower() in ('x86_64', 'amd64'):
    builds.insert(0, ('vector', ['-mfma']))
with tempfile.TemporaryDirectory(prefix='ppu-vmx-float-vec-') as tmp:
    for name, flags in builds:
        binary = Path(tmp) / name
        subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu17', '-O2', '-ffp-contract=off', *flags,
                        '-I' + str(root / 'runtime/ppu'), str(src), '-lm', '-o', str(binary)], check=True)
        result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=120)
        print(result.stdout.strip())
        if result.returncode:
            raise SystemExit(f'FAILED ({name} forms)')
        if name == 'vector' and '(vector)' not in result.stdout:
            raise SystemExit('FAILED: the FMA3 build did not use the vector forms')
