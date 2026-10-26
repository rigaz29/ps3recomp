#!/usr/bin/env python3
"""Player settings: the file is read, the order of precedence holds, and
writing it back keeps what the player wrote.

This builds a small host program on runtime/settings/ps3_settings.c and
checks that:

- values come from the file, with comments and quotes handled;
- an environment variable wins over the file, a command-line override over
  both, and the caller's default applies when none has a value;
- on/off, hex and decimal numbers parse, and a bad value gives the default;
- a changed value is written in place with its comment, a new key lands at
  the end of its section, a new section at the end, and every other line
  (comments, keys this build does not know) is written back as it was;
- a missing file loads as "no file" and keeps its path for the first save.
"""
import os
import shutil
import subprocess
import tempfile
from pathlib import Path

if os.name == 'nt':
    print('SKIP: fixture currently requires a POSIX C compiler')
    raise SystemExit(0)

cc = os.environ.get('CC') or shutil.which('cc') or shutil.which('gcc')
if not cc:
    print('SKIP: no C compiler')
    raise SystemExit(0)

root = Path(__file__).resolve().parents[1]

INI = '''; player settings
[display]
mode = borderless   ; set by the launcher
vsync = off

[graphics]
async_shaders = 4
hex_key = 0x10
future_key = keep me
shader_cache = "C:/a b/cache;1.bin"

[input]
deadzone = 0.3
bad_bool = maybe
'''

HARNESS = r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ps3emu/ps3_settings.h"

static int bad;
#define CHECK(c) do { if (!(c)) { printf("[FAIL] line %d: %s\n", __LINE__, #c); bad++; } } while (0)

int main(int argc, char** argv)
{
    const char* in = argv[1];
    const char* out = argv[2];
    const char* missing = argv[3];
    (void)argc;

    CHECK(ps3_settings_load(in) == 0);
    CHECK(!strcmp(ps3_settings_path(), in));

    /* the file, with its comment and quotes */
    CHECK(!strcmp(ps3_settings_str("display", "mode", NULL, "windowed"), "borderless"));
    CHECK(!strcmp(ps3_settings_str("Graphics", "Shader_Cache", NULL, ""), "C:/a b/cache;1.bin"));
    CHECK(ps3_settings_bool("display", "vsync", "TEST_VSYNC_UNSET", 1) == 0);
    CHECK(ps3_settings_int("graphics", "hex_key", NULL, 0) == 16);
    CHECK(ps3_settings_num("input", "deadzone", NULL, 0.24) == 0.3);

    /* the environment wins over the file */
    CHECK(ps3_settings_int("graphics", "async_shaders", "TEST_ASYNC", 2) == 5);

    /* the default, when nobody has a value; a bad value gives it too */
    CHECK(ps3_settings_int("graphics", "nothing", "TEST_NOTHING_UNSET", 7) == 7);
    CHECK(!strcmp(ps3_settings_str("system", "language", NULL, "auto"), "auto"));
    CHECK(ps3_settings_bool("input", "bad_bool", NULL, 1) == 1);
    CHECK(ps3_settings_bool("input", "bad_bool", NULL, 0) == 0);

    /* a command-line override wins over the environment */
    ps3_settings_override("display", "mode", "windowed");
    CHECK(!strcmp(ps3_settings_str("display", "mode", "TEST_MODE", "x"), "windowed"));

    /* the launcher's edits, then the file written back */
    CHECK(ps3_settings_set("display", "mode", "borderless") == 0);
    CHECK(ps3_settings_set("display", "vsync", "on") == 0);
    CHECK(ps3_settings_set("graphics", "new_key", "a;b") == 0);
    CHECK(ps3_settings_set("system", "language", "fr") == 0);
    CHECK(ps3_settings_save(out) == 0);
    CHECK(!strcmp(ps3_settings_path(), out));

    CHECK(ps3_settings_load(out) == 0);
    CHECK(ps3_settings_bool("display", "vsync", NULL, 0) == 1);
    CHECK(!strcmp(ps3_settings_str("graphics", "new_key", NULL, ""), "a;b"));
    CHECK(!strcmp(ps3_settings_str("system", "language", NULL, "auto"), "fr"));
    CHECK(!strcmp(ps3_settings_str("graphics", "future_key", NULL, ""), "keep me"));

    /* no file yet: defaults, and the path is kept for the first save */
    CHECK(ps3_settings_load(missing) == 1);
    CHECK(!strcmp(ps3_settings_path(), missing));
    CHECK(ps3_settings_bool("display", "vsync", NULL, 1) == 1);
    CHECK(ps3_settings_set("display", "vsync", "off") == 0);
    CHECK(ps3_settings_save(NULL) == 0);

    printf("bad=%d\n", bad);
    return bad;
}
'''

EXPECTED_OUT = '''; player settings
[display]
mode = borderless ; set by the launcher
vsync = on

[graphics]
async_shaders = 4
hex_key = 0x10
future_key = keep me
shader_cache = "C:/a b/cache;1.bin"
new_key = "a;b"

[input]
deadzone = 0.3
bad_bool = maybe

[system]
language = fr
'''

EXPECTED_NEW = '''[display]
vsync = off
'''

with tempfile.TemporaryDirectory() as tmp:
    t = Path(tmp)
    (t / 'in.ini').write_text(INI)
    c_path = t / 'settings.c'
    exe = t / 'settings'
    c_path.write_text(HARNESS)
    cmd = [cc, '-std=gnu17', '-O1', '-Wall', '-Wextra', '-Werror',
           '-I', str(root / 'include'), str(c_path),
           str(root / 'runtime/settings/ps3_settings.c'), '-o', str(exe)]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        print(r.stdout + r.stderr)
        raise SystemExit('FAIL: harness did not compile')

    env = {k: v for k, v in os.environ.items() if not k.startswith('TEST_')}
    env.update({'TEST_ASYNC': '5', 'TEST_MODE': 'from-env'})
    r = subprocess.run([str(exe), str(t / 'in.ini'), str(t / 'out.ini'), str(t / 'new.ini')],
                       capture_output=True, text=True, env=env)
    print(r.stdout, end='')
    if r.returncode:
        raise SystemExit('FAIL: harness')

    log = r.stdout
    for line in ('[settings] display.mode = borderless (file)',
                 '[settings] graphics.async_shaders = 5 (TEST_ASYNC)',
                 '[settings] graphics.nothing = 7 (default)',
                 '[settings] display.mode = windowed (command line)'):
        if line not in log:
            raise SystemExit('FAIL: the log has no line "%s"' % line)
    if log.count("is not on or off") != 1:
        raise SystemExit('FAIL: a bad value should be reported once')

    got = (t / 'out.ini').read_text()
    if got != EXPECTED_OUT:
        print('--- written\n' + got + '--- expected\n' + EXPECTED_OUT)
        raise SystemExit('FAIL: the file written back differs')
    got = (t / 'new.ini').read_text()
    if got != EXPECTED_NEW:
        print('--- written\n' + got + '--- expected\n' + EXPECTED_NEW)
        raise SystemExit('FAIL: the new file differs')
    print('PASS: settings read in order of precedence and written back as the player wrote them')
