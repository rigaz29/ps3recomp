#!/usr/bin/env python3
"""sceNpSignaling between two players, through tools/np_signaling_server.py.

Builds libs/network/tests/test_np_signaling.c with the signaling client, starts the server on a free port, and runs
the test twice at once, as PlayerA (P2P port 3658) and PlayerB (3659): each activates a connection to the other,
both see ESTABLISHED and EXT_MUTUAL_ACTIVATED with the other's address and port, and A sees B leave (DEAD,
TERMINATED_BY_PEER). A third run with no server ends its activation DEAD (PEER_UNREACHABLE).

It needs a host C compiler (under CI=1 a missing one fails instead of skipping).

Usage:
    python3 tools/test_np_signaling.py
"""

import os
import shutil
import subprocess
import sys
import tempfile

TOOLS = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(TOOLS)
sys.path.insert(0, TOOLS)
import np_signaling_server  # noqa: E402

SOURCES = [os.path.join(ROOT, "libs", "network", "tests", "test_np_signaling.c"),
           os.path.join(ROOT, "libs", "network", "np_signal_client.c")]


def _cc():
    return (shutil.which(os.environ.get("CC", "cc")) or shutil.which("clang")
            or shutil.which("gcc"))


def main():
    cc = _cc()
    if cc is None:
        if os.environ.get("CI"):
            print("FAIL: no C compiler found")
            return 1
        print("skip test_np_signaling: no C compiler")
        return 0
    with tempfile.TemporaryDirectory() as td:
        exe = os.path.join(td, "test_np_signaling" + (".exe" if os.name == "nt" else ""))
        cmd = [cc, "-std=gnu17", "-w", "-I" + os.path.join(ROOT, "include"), "-o", exe] + SOURCES
        if os.name == "nt":
            cmd.append("-lws2_32")
        else:
            cmd.append("-lpthread")
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode != 0:
            print("FAIL: fixture did not build")
            print((r.stdout + r.stderr)[-4000:])
            return 1

        log = []
        srv = np_signaling_server.start(port=0, log=log.append)
        try:
            base = dict(os.environ, PS3_NP_SIGNALING_SERVER="127.0.0.1:%d" % srv.port, PS3_NP_SIGNALING_DELAY_MS="100")
            runs = {
                "a": dict(base, PS3_NP_ONLINE_ID="PlayerA", PS3_NET_P2P_PORT="3658", PEER_NAME="PlayerB",
                          PEER_PORT="3659"),
                "b": dict(base, PS3_NP_ONLINE_ID="PlayerB", PS3_NET_P2P_PORT="3659", PEER_NAME="PlayerA",
                          PEER_PORT="3658"),
            }
            procs = {k: subprocess.Popen([exe, k], env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                         text=True) for k, env in runs.items()}
            rc = 0
            for k, p in procs.items():
                out, _ = p.communicate(timeout=60)
                print("--- %s ---" % k)
                sys.stdout.write(out)
                rc = rc or p.returncode
        finally:
            srv.stop()
        print("--- server ---")
        print("\n".join(log))
        env = {k: v for k, v in os.environ.items() if k != "PS3_NP_SIGNALING_SERVER"}
        env.update(PS3_NP_ONLINE_ID="PlayerA")
        p = subprocess.run([exe, "offline"], env=env, capture_output=True, text=True, timeout=60)
        print("--- offline ---")
        sys.stdout.write(p.stdout + p.stderr)
        rc = rc or p.returncode
        print("PASS" if rc == 0 else "FAIL")
        return 1 if rc else 0


if __name__ == "__main__":
    sys.exit(main())
