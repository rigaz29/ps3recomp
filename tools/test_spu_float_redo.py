#!/usr/bin/env python3
"""The RZ float forms' four-lane redo (double precision under rounding toward zero) against the exact
lane-by-lane forms: runtime/spu/tests/test_spu_float_redo.c, built for AVX2 and FMA."""
import os
import subprocess
import tempfile
from pathlib import Path

if os.name == 'nt':
    print('SKIP: fixture currently requires the POSIX compiler')
    raise SystemExit(0)
root = Path(__file__).resolve().parents[1]
spu = root / 'runtime/spu'
with tempfile.TemporaryDirectory(prefix='spu-float-redo-') as tmp:
    binary = Path(tmp) / 'test'
    subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu17', '-O2', '-msse4.1', '-mavx2', '-mfma',
                    '-ffp-contract=off', '-DSPU_FLOAT_RZ_ONLY', '-I' + str(spu), '-I' + str(root / 'include'),
                    str(spu / 'tests/test_spu_float_redo.c'), '-lm', '-o', str(binary)], check=True)
    result = subprocess.run([str(binary), '3000000'], capture_output=True, text=True, timeout=300)
    print(result.stdout.strip())
    if result.returncode:
        raise SystemExit('FAILED')
