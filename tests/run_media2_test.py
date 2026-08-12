#!/usr/bin/env python3
"""ctest driver for tests/unit/media2_live_test.cpp against the mock.

Runs the binary twice against a Media2-advertising mock:
  media2    -- the Media2 endpoint answers (GetProfiles2/GetStreamUri2/encoder2)
  fallback  -- GetProfiles2 faults; the client must fall back to classic Media

usage: run_media2_test.py <media2_live_test binary>
"""
import os
import shutil
import subprocess
import sys
import tempfile
import threading

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mock_onvif_server as mock_mod  # noqa: E402


def run_case(binary, mode):
    mock = mock_mod.OnvifMock(auth="digest", username="admin", password="pass",
                              use_media2=True,
                              media2_faults=(mode == "fallback"))
    server = mock_mod.make_server(mock, host="127.0.0.1", port=0)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    rc = 2
    try:
        port = server.server_address[1]
        r = subprocess.run([binary, str(port), mode],
                           capture_output=True, text=True, timeout=120)
        sys.stdout.write(f"=== {mode} ===\n")
        if r.stdout:
            sys.stdout.write(r.stdout)
        if r.stderr:
            sys.stderr.write(r.stderr)
        rc = r.returncode
    finally:
        server.shutdown()
        server.server_close()
    return rc


def main():
    if len(sys.argv) < 2 or not os.path.exists(sys.argv[1]):
        print("usage: run_media2_test.py <media2_live_test binary>",
              file=sys.stderr)
        return 2
    binary = sys.argv[1]
    cfgdir = tempfile.mkdtemp(prefix="obs-onvif-m2-")
    rc = 0
    try:
        for mode in ("media2", "fallback"):
            rc = run_case(binary, mode)
            if rc != 0:
                break
    finally:
        shutil.rmtree(cfgdir, ignore_errors=True)

    if rc == 0:
        print("ALL MEDIA2 SCENARIOS PASSED")
    return rc


if __name__ == "__main__":
    sys.exit(main())
