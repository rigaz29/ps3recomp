#!/usr/bin/env python3
"""rsx_index_copy_be's SSE4.1 form converts the RSX's big-endian index arrays
exactly as the scalar one does (libs/video/tests/test_rsx_index_copy.c)."""
import os
from pathlib import Path
import platform
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
if os.name == 'nt':
    print('SKIP: builds with a POSIX cc')
    raise SystemExit(0)
if platform.machine().lower() not in ('x86_64', 'amd64'):
    print('SKIP: the SIMD form is x86-64 only')
    raise SystemExit(0)
with tempfile.TemporaryDirectory(prefix='rsx-index-copy-') as tmp:
    binary = Path(tmp) / 'test'
    subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu17', '-O2',
                    '-I' + str(root / 'libs/video'),
                    str(root / 'libs/video/tests/test_rsx_index_copy.c'),
                    '-o', str(binary)], check=True)
    result = subprocess.run([str(binary)], timeout=60, capture_output=True, text=True)
    print(result.stdout.strip())
    if result.returncode:
        raise SystemExit('test_rsx_index_copy failed')
