#!/usr/bin/env python3
"""ctest driver for soap_live_test against tests/mock_onvif_server.py.

Boots a fresh mock per scenario (plain HTTP and HTTPS with the committed
self-signed fixture), runs the compiled C++ binary against it on 127.0.0.1,
and aggregates exit codes.

usage: run_soap_live_test.py <path/to/soap_live_test>
"""
import os
import subprocess
import sys
import threading

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mock_onvif_server as mock_mod  # noqa: E402

FIXTURES = os.path.join(os.path.dirname(os.path.abspath(__file__)), "fixtures")

# mode -> (mock kwargs, https)  -- mode names must match soap_live_test.cpp
SCENARIOS = [
    ("digest", dict(auth="digest", password="pass"), False),
    ("digest_invalid", dict(auth="digest", password="other-pass"), False),
    ("basic", dict(auth="basic", password="pass"), False),
    ("fallback", dict(auth="basic", password="pass"), False),
    ("open_fault", dict(auth="open", password="pass"), False),
    ("https_open", dict(auth="open", password="pass"), True),
    ("https_strict", dict(auth="open", password="pass"), True),
]


def run_scenario(exe, mode, mock_kwargs, https):
    mock = mock_mod.OnvifMock(**mock_kwargs)
    server = mock_mod.make_server(
        mock,
        port=0,
        https=https,
        cert=os.path.join(FIXTURES, "server.crt"),
        key=os.path.join(FIXTURES, "server.key"),
    )
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        port = server.server_address[1]
        scheme = "https" if https else "http"
        url = f"{scheme}://127.0.0.1:{port}/onvif/device_service"
        proc = subprocess.run([exe, url, mode, "admin", "pass"])
        return proc.returncode
    finally:
        server.shutdown()
        server.server_close()


def main():
    if len(sys.argv) < 2 or not os.path.exists(sys.argv[1]):
        print("usage: run_soap_live_test.py <soap_live_test binary>",
              file=sys.stderr)
        return 2
    exe = sys.argv[1]

    failed = []
    for mode, mock_kwargs, https in SCENARIOS:
        rc = run_scenario(exe, mode, mock_kwargs, https)
        print(f"[{mode}] -> {'PASS' if rc == 0 else 'FAIL'} (rc={rc})")
        if rc != 0:
            failed.append(mode)

    if failed:
        print("FAILED scenarios: " + ", ".join(failed), file=sys.stderr)
        return 1
    print("ALL SOAP LIVE SCENARIOS PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main())