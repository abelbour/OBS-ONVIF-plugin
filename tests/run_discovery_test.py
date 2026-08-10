#!/usr/bin/env python3
"""Drives tests/unit/ws_discovery_test.cpp's live UDP mode against the mock's
echo responder, proving a real loopback datagram round trip (probe in,
ProbeMatches out) independent of multicast.

Usage: python tests/run_discovery_test.py <ws_discovery_test_binary>
"""
import socket
import subprocess
import sys
import threading

from mock_onvif_server import PROBE_MATCH_REPLY, udp_echo_reply


def free_udp_port():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


def main():
    binary = sys.argv[1]
    port = free_udp_port()

    t = threading.Thread(target=udp_echo_reply,
                         args=(PROBE_MATCH_REPLY, port, 20.0),
                         daemon=True)
    t.start()

    r = subprocess.run([binary, "127.0.0.1", str(port)],
                       capture_output=True, text=True, timeout=90)
    if r.stdout:
        sys.stdout.write(r.stdout)
    if r.stderr:
        sys.stderr.write(r.stderr)
    sys.exit(r.returncode)


if __name__ == "__main__":
    main()