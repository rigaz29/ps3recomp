#!/usr/bin/env python3
"""Exercise the production Taskset2 exit-code entry writer against an SPU creator's check."""
import os
import re
import subprocess
import tempfile
from pathlib import Path

if os.name == 'nt':
    print('SKIP: fixture currently requires a POSIX C compiler')
    raise SystemExit(0)
root = Path(__file__).resolve().parents[1]
source = (root / 'runtime/spu/spu_workload.c').read_text()

def function(name):
    match = re.search(r'^.*\b' + re.escape(name) + r'\([^;\n]*\)\s*\{', source, re.M)
    if not match:
        raise AssertionError('Missing production function ' + name)
    brace = source.index('{', match.start())
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[match.start():end]

tables = re.search(r'^static uint32_t s_ts2\[16\];.*$', source, re.M)
if not tables:
    raise AssertionError('Missing production table s_ts2')
functions = '\n'.join(function(name) for name in
                      ['spu_taskset2_register', 'spu_taskset_is_v2', 'spu_taskset2_set_exit'])
fixture = (root / 'runtime/spu/tests/test_spu_taskset2_exit_slot.c').read_text()
fixture = fixture.replace('/* PRODUCTION_FUNCTIONS */', tables.group(0) + '\n' + functions)
with tempfile.TemporaryDirectory(prefix='spu-taskset2-exit-slot-') as tmp:
    c = Path(tmp) / 'test.c'
    binary = Path(tmp) / 'test'
    c.write_text(fixture)
    subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu17', '-O1', '-UNDEBUG',
                    '-Wall', '-Wextra', str(c), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True, timeout=5, cwd=tmp)
