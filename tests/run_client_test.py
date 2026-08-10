#!/usr/bin/env python3
"""ctest driver for onvif_client_test against tests/mock_onvif_server.py.

Boots a fresh mock per scenario (digest-required and basic-required), runs the
compiled C++ client against it on 127.0.0.1, and aggregates exit codes.

usage: run_client_test.py <path/to/onvif_client_test>
"""
import os
import subprocess
import sys
import threading

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mock_onvif_server as mock_mod  # noqa: E402

SCENARIOS = [
    ("digest", dict(auth="digest", password="pass")),
    ("basic", dict(auth="basic", password="pass")),
]


def run_scenario(exe, mode, mock_kwargs):
    mock = mock_mod.OnvifMock(**mock_kwargs)
    server = mock_mod.make_server(mock, port=0)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        port = server.server_address[1]
        url = f"http://127.0.0.1:{port}/onvif/device_service"
        proc = subprocess.run([exe, url, mode])
        return proc.returncode
    finally:
        server.shutdown()
        server.server_close()


def main():
    if len(sys.argv) < 2 or not os.path.exists(sys.argv[1]):
        print("usage: run_client_test.py <onvif_client_test binary>",
              file=sys.stderr)
        return 2
    exe = sys.argv[1]

    failed = []
    for mode, mock_kwargs in SCENARIOS:
        rc = run_scenario(exe, mode, mock_kwargs)
        print(f"[{mode}] -> {'PASS' if rc == 0 else 'FAIL'} (rc={rc})")
        if rc != 0:
            failed.append(mode)

    if failed:
        print("FAILED scenarios: " + ", ".join(failed), file=sys.stderr)
        return 1
    print("ALL ONVIF CLIENT SCENARIOS PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main())