#!/usr/bin/env python3
"""libs/video/tests/test_texture_layout.c: the RSX texture layout and its decode, the
4x4-tile deswizzle among them, built with a POSIX cc and run."""
import os
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
if os.name == 'nt':
    print('SKIP: builds with a POSIX cc')
    raise SystemExit(0)
with tempfile.TemporaryDirectory(prefix='rsx-texture-layout-') as tmp:
    binary = Path(tmp) / 'test'
    subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu17', '-O2',
                    '-I' + str(root / 'include'), '-I' + str(root / 'libs/video'),
                    str(root / 'libs/video/tests/test_texture_layout.c'),
                    str(root / 'libs/video/rsx_texture_layout.c'),
                    '-o', str(binary)], check=True)
    result = subprocess.run([str(binary)], timeout=120, capture_output=True, text=True)
    print(result.stdout.strip()[-2000:])
    if result.returncode:
        raise SystemExit('test_texture_layout failed')
