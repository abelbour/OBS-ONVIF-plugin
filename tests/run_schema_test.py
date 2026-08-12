#!/usr/bin/env python3
"""ctest driver for the schema-conformance lane (IMPLEMENTATION_PLAN.md §8).

Runs the typed-client, config-panel, and Media2 live tests against dump-mode
mocks so every request envelope the plugin emits is captured to disk, then
validates them against the mirrored ONVIF schemas
(tools/validate_envelopes.py). Any drift fails the lane.

usage: run_schema_test.py <onvif_client_test> <config_live_test>
       <media2_live_test> [repo_root]
"""
import os
import shutil
import subprocess
import sys
import tempfile
import threading

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mock_onvif_server as mock_mod  # noqa: E402


def run_case(binary, dumpdir, media2=False, cfgdir=None):
    mock = mock_mod.OnvifMock(auth="digest", username="admin", password="pass",
                              use_media2=media2, dump_dir=dumpdir)
    server = mock_mod.make_server(mock, host="127.0.0.1", port=0)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    rc = 2
    try:
        port = server.server_address[1]
        url = f"http://127.0.0.1:{port}/onvif/device_service"
        if "config" in os.path.basename(binary):
            cmd = [binary, cfgdir, str(port)]
        elif "media2" in os.path.basename(binary):
            cmd = [binary, str(port), "media2"]
        else:
            cmd = [binary, url, "digest"]
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=180)
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
    if len(sys.argv) < 4 or not os.path.exists(sys.argv[1]):
        print("usage: run_schema_test.py <onvif_client_test> <config_live_test> "
              "<media2_live_test> [repo_root]", file=sys.stderr)
        return 2
    client_bin = sys.argv[1]
    config_bin = sys.argv[2]
    media2_bin = sys.argv[3]
    repo = os.path.abspath(
        sys.argv[4] if len(sys.argv) > 4
        else os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))

    dumpdir = tempfile.mkdtemp(prefix="obs-onvif-schema-")
    cfgdir = tempfile.mkdtemp(prefix="obs-onvif-schemacfg-")
    rc = 0
    cases = [
        ("client", client_bin, False),
        ("config", config_bin, False),
        ("media2", media2_bin, True),
    ]
    dump_dirs = []
    try:
        for name, binary, media2 in cases:
            if rc != 0:
                break
            case_dump = os.path.join(dumpdir, name)
            os.makedirs(case_dump)
            dump_dirs.append(case_dump)
            rc = run_case(binary, case_dump, media2=media2, cfgdir=cfgdir)
        for case_dump in dump_dirs:
            if rc != 0:
                break
            v = subprocess.run(
                [sys.executable,
                 os.path.join(repo, "tools", "validate_envelopes.py"),
                 "--schema-dir", os.path.join(repo, "third_party/onvif-schemas"),
                 "--envelopes", case_dump],
                capture_output=True, text=True)
            if v.stdout:
                sys.stdout.write(v.stdout)
            if v.stderr:
                sys.stderr.write(v.stderr)
            rc = v.returncode
    finally:
        shutil.rmtree(dumpdir, ignore_errors=True)
        shutil.rmtree(cfgdir, ignore_errors=True)

    if rc == 0:
        print("ALL SCHEMA CONFORMANCE SCENARIOS PASSED")
    return rc


if __name__ == "__main__":
    sys.exit(main())
