#!/usr/bin/env python3
"""ctest driver for tests/unit/ptz_latency_live_test.cpp against the mock.

Asserts the M4 §6.8 PTZ transport/motor-control invariants at the HTTP layer:

  1. after first contact (cache + auth warm), a move costs exactly 1 request;
  2. consecutive moves reuse one keep-alive connection (no new TCP handshake);
  3. with a move artificially delayed in-flight, an immediate Stop lands well
     before the delay elapses (abort path, not wait-out);
  4. the mock never observes overlapping movement requests (queue depth <= 1).

usage: run_ptz_latency_test.py <ptz_latency_live_test binary>
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


def main():
    if len(sys.argv) < 2 or not os.path.exists(sys.argv[1]):
        print("usage: run_ptz_latency_test.py <ptz_latency_live_test binary>",
              file=sys.stderr)
        return 2
    binary = sys.argv[1]

    cfgdir = tempfile.mkdtemp(prefix="obs-onvif-ptz-")
    mock = mock_mod.OnvifMock(auth="digest", username="admin", password="pass")
    server = mock_mod.make_server(mock, host="127.0.0.1", port=0)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    proc = None
    rc = 2
    try:
        port = server.server_address[1]
        base_url = f"http://127.0.0.1:{port}/onvif/device_service"
        cameras = {
            "cameras": [{
                "id": CAMERA_ID,
                "name": "PTZ-CAM",
                "xaddr": base_url,
                "scope_mac": "40:d8:2e:12:34:56",
                "online": True,
                "last_seen": 0,
                "last_known_rtsp": {},
            }]
        }
        with open(os.path.join(cfgdir, "cameras.json"), "w") as fh:
            json.dump(cameras, fh, indent=2)

        proc = subprocess.Popen(
            [binary, cfgdir, str(port)], stdin=subprocess.PIPE,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)

        def read_line():
            line = proc.stdout.readline()
            if not line:
                raise RuntimeError("ptz_latency_live_test closed stdout early")
            return line.strip()

        def send(line):
            proc.stdin.write(line + "\n")
            proc.stdin.flush()

        assert read_line() == "READY"
        send("WARM")
        assert read_line() == "WARM_DONE"
        r0 = mock.request_count
        c0 = mock.connection_count

        send("MOVES")
        assert read_line() == "MOVES_DONE"
        r1 = mock.request_count
        c1 = mock.connection_count
        # Assertion 1: each cached move costs exactly 1 HTTP request.
        assert r1 - r0 == 2, f"expected 2 requests during MOVES, saw {r1 - r0}"
        # Assertion 2: the two moves reused the warm keep-alive connection.
        assert c1 == c0, f"keep-alive connection not reused: {c0} -> {c1}"

        # Assertion 3: delay ContinuousMove in-flight, then Stop must abort.
        mock.move_delay_seconds = 2.0
        send("LATENCY")
        latency = read_line()
        assert latency == "LATENCY_OK", f"stop latency line: {latency}"
        assert mock.stop_requests >= 1, "mock observed no Stop request"
        # Assertion 4: dispatches never overlapped.
        assert mock.max_concurrent_moves == 1, (
            f"overlapping movement requests: {mock.max_concurrent_moves}")

        mock.move_delay_seconds = 0.0
        send("DONE")
        out, err = proc.communicate(timeout=120)
        if out:
            sys.stdout.write(out)
        if err:
            sys.stderr.write(err)
        rc = proc.returncode
        assert rc == 0, f"ptz_latency_live_test exited {rc}"
    finally:
        if proc and proc.poll() is None:
            proc.kill()
        server.shutdown()
        server.server_close()
        shutil.rmtree(cfgdir, ignore_errors=True)

    if rc == 0:
        print("ALL PTZ LATENCY SCENARIOS PASSED")
    return rc


if __name__ == "__main__":
    sys.exit(main())
