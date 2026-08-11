#!/usr/bin/env python3
"""Drives tests/unit/config_live_test.cpp against the mock server's camera
configuration endpoints (encoder / imaging / network / OSD) through the public
ABI, asserting each set operation round-trips through mock state.

usage: run_config_test.py <config_live_test binary>
"""
import os
import shutil
import subprocess
import sys
import tempfile
import threading

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mock_onvif_server as mock_mod  # noqa: E402


def main():
    if len(sys.argv) < 2 or not os.path.exists(sys.argv[1]):
        print("usage: run_config_test.py <config_live_test binary>",
              file=sys.stderr)
        return 2
    binary = sys.argv[1]
    cfgdir = tempfile.mkdtemp(prefix="obs-onvif-cfgtest-")
    rc = 1
    try:
        mock = mock_mod.OnvifMock(auth="digest", username="admin",
                                  password="pass")
        server = mock_mod.make_server(mock, host="127.0.0.1", port=0)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        port = server.server_address[1]

        r = subprocess.run([binary, cfgdir, str(port)],
                           capture_output=True, text=True, timeout=120)
        if r.stdout:
            sys.stdout.write(r.stdout)
        if r.stderr:
            sys.stderr.write(r.stderr)
        rc = r.returncode

        server.shutdown()
        server.server_close()
    finally:
        shutil.rmtree(cfgdir, ignore_errors=True)

    if rc == 0:
        print("ALL CONFIG LIVE SCENARIOS PASSED")
    return rc


if __name__ == "__main__":
    sys.exit(main())
