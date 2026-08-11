#!/usr/bin/env python3
"""Drives tests/unit/registry_test.cpp's live modes against the mock server.

Covers the M2 acceptance path end-to-end without multicast:
  seed    -- mock HTTP on 127.0.0.1; discovery round-trip registers the
             camera and persists it (cameras.json in a temp config dir).
  rehome  -- mock re-bound to a new loopback host (127.0.0.2) whose
             GetStreamUri replies on that host; the registry restores the
             camera, detects the xaddr move, and rewrites the mapped source
             URL with credentials spliced to the new host.

usage: run_registry_test.py <registry_live_test binary>
"""
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import threading

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mock_onvif_server as mock_mod  # noqa: E402


def free_udp_port():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


def start_http(host, rtsp_host):
    mock = mock_mod.OnvifMock(auth="digest", username="admin", password="pass",
                              rtsp_host=rtsp_host)
    server = mock_mod.make_server(mock, host=host, port=0)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    return server, server.server_address[1]


def probe_responder(xaddr, port):
    """Replies to one discovery datagram with a ProbeMatch on `xaddr`."""
    payload = mock_mod.PROBE_MATCH_REPLY.replace(
        "http://127.0.0.1/onvif/device_service", xaddr)
    return threading.Thread(target=mock_mod.udp_echo_reply,
                            args=(payload, port, 30.0), daemon=True)


def run(binary, phase, http_host, http_port, udp_port, cfgdir):
    r = subprocess.run([binary, phase, http_host, str(http_port),
                        str(udp_port), cfgdir],
                       capture_output=True, text=True, timeout=120)
    sys.stdout.write(f"=== {phase} ===\n")
    if r.stdout:
        sys.stdout.write(r.stdout)
    if r.stderr:
        sys.stderr.write(r.stderr)
    sys.stdout.write(f"=== {phase}: "
                     f"{'PASS' if r.returncode == 0 else 'FAIL'} "
                     f"(rc={r.returncode}) ===\n")
    return r.returncode


def main():
    if len(sys.argv) < 2 or not os.path.exists(sys.argv[1]):
        print("usage: run_registry_test.py <registry_live_test binary>",
              file=sys.stderr)
        return 2
    binary = sys.argv[1]
    cfgdir = tempfile.mkdtemp(prefix="obs-onvif-regtest-")
    rc = 0
    try:
        # Phase 1: seed on 127.0.0.1.
        server_a, port_a = start_http("127.0.0.1", "127.0.0.1")
        try:
            udp_a = free_udp_port()
            xaddr_a = f"http://127.0.0.1:{port_a}/onvif/device_service"
            probe_responder(xaddr_a, udp_a).start()
            rc = run(binary, "seed", "127.0.0.1", port_a, udp_a, cfgdir)
        finally:
            server_a.shutdown()
            server_a.server_close()

        if rc == 0:
            # Phase 2: camera moved to a new loopback host.
            server_b, port_b = start_http("127.0.0.2", "127.0.0.2")
            try:
                udp_b = free_udp_port()
                xaddr_b = f"http://127.0.0.2:{port_b}/onvif/device_service"
                probe_responder(xaddr_b, udp_b).start()
                rc = run(binary, "rehome", "127.0.0.2", port_b, udp_b, cfgdir)
            finally:
                server_b.shutdown()
                server_b.server_close()
    finally:
        shutil.rmtree(cfgdir, ignore_errors=True)

    if rc == 0:
        print("ALL REGISTRY LIVE SCENARIOS PASSED")
    return rc


if __name__ == "__main__":
    sys.exit(main())
