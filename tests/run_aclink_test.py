#!/usr/bin/env python3
"""ctest driver for tests/aclink against mock_onvif_server.py.

Seeds a store with a single online camera pointing at a fresh mock ONVIF
server, runs the C ABI consumer against it, and additionally verifies that the
built plugin module exports the obs_onvif_get_abi symbol (parses the MSVC
import/export .exp file).

usage: run_aclink_test.py <path/to/aclink> <path/to/build_dir>
"""
import json
import os
import shutil
import subprocess
import sys
import tempfile
import threading

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mock_onvif_server as mock_mod  # noqa: E402

CAMERA_ID = "sn:DS2CD2032I20170801AACH12345678"


def find_export_file(build_dir):
    if not build_dir or not os.path.isdir(build_dir):
        return None
    for root, _dirs, files in os.walk(build_dir):
        for f in files:
            if f == "obs-onvif.exp":
                return os.path.join(root, f)
    return None


def check_exports(build_dir):
    exp = find_export_file(build_dir)
    if not exp:
        print("EXPORTS: no obs-onvif.exp found under build dir")
        return False
    with open(exp, "r", errors="replace") as fh:
        text = fh.read()
    ok = "obs_onvif_get_abi" in text
    print(f"EXPORTS: obs-onvif.exp = {exp}; obs_onvif_get_abi exported: {ok}")
    return ok


def main():
    if len(sys.argv) < 2 or not os.path.exists(sys.argv[1]):
        print("usage: run_aclink_test.py <aclink binary> [build_dir]",
              file=sys.stderr)
        return 2
    binary = sys.argv[1]
    build_dir = sys.argv[2] if len(sys.argv) > 2 else ""

    cfgdir = tempfile.mkdtemp(prefix="obs-onvif-aclink-")
    mock = mock_mod.OnvifMock(auth="digest", username="admin", password="pass")
    server = mock_mod.make_server(mock, host="127.0.0.1", port=0)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()

    rc = 2
    try:
        port = server.server_address[1]
        base_url = f"http://127.0.0.1:{port}/onvif/device_service"
        cameras = {
            "cameras": [{
                "id": CAMERA_ID,
                "name": "ACLINK-CAM",
                "xaddr": base_url,
                "scope_mac": "40:d8:2e:12:34:56",
                "online": True,
                "last_seen": 0,
                "last_known_rtsp": {},
            }]
        }
        with open(os.path.join(cfgdir, "cameras.json"), "w") as fh:
            json.dump(cameras, fh, indent=2)

        proc = subprocess.run([binary, cfgdir, str(port)],
                              capture_output=True, text=True, timeout=120)
        if proc.stdout:
            sys.stdout.write(proc.stdout)
        if proc.stderr:
            sys.stderr.write(proc.stderr)
        sys.stdout.write(f"aclink rc={proc.returncode}\n")

        exports_ok = check_exports(build_dir)
        rc = 0 if (proc.returncode == 0 and exports_ok) else 1
    finally:
        server.shutdown()
        server.server_close()
        shutil.rmtree(cfgdir, ignore_errors=True)

    if rc == 0:
        print("ALL ACLINK SCENARIOS PASSED")
    return rc


if __name__ == "__main__":
    sys.exit(main())