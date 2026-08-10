#!/usr/bin/env python3
"""ONVIF HTTP(S) mock server used by the CI test suite.

Intentional deviations from a real camera keep the mock small but faithful for
the paths the suite exercises: WS-Security digest validation (the server
recomputes the expected digest), HTTP Basic-only devices, 401 rejection, and
envelope routing for Device/Media/PTZ calls.

Dependencies: stdlib only.  Usage (standalone is optional; ctest normally
drives it via tests/run_soap_live_test.py):

    python tests/mock_onvif_server.py --port 0 --mode digest
"""
import argparse
import base64
import hashlib
import http.server
import re
import socketserver
import ssl
import sys

ENV = "http://schemas.xmlsoap.org/soap/envelope/"
TDS = "http://www.onvif.org/ver10/device/wsdl"
TRT = "http://www.onvif.org/ver10/media/wsdl"
TT = "http://www.onvif.org/ver10/schema"

DEVICE_INFO_RESPONSE = f"""<?xml version="1.0" encoding="UTF-8"?>
<env:Envelope xmlns:env="{ENV}" xmlns:tds="{TDS}" xmlns:tt="{TT}">
  <env:Body>
    <tds:GetDeviceInformationResponse>
      <tds:Manufacturer>HIKVISION</tds:Manufacturer>
      <tds:Model>DS-2CD2032-I</tds:Model>
      <tds:FirmwareVersion>V5.6.8 build 220902</tds:FirmwareVersion>
      <tds:SerialNumber>DS2CD2032I20170801AACH12345678</tds:SerialNumber>
      <tds:HardwareId>0</tds:HardwareId>
    </tds:GetDeviceInformationResponse>
  </env:Body>
</env:Envelope>"""

CAPABILITIES_RESPONSE = f"""<?xml version="1.0" encoding="UTF-8"?>
<env:Envelope xmlns:env="{ENV}" xmlns:tds="{TDS}" xmlns:tt="{TT}">
  <env:Body>
    <tds:GetCapabilitiesResponse>
      <tds:Capabilities>
        <tds:Device>
          <tds:XAddr>http://127.0.0.1/onvif/device_service</tds:XAddr>
        </tds:Device>
        <tds:Media>
          <tds:XAddr>http://127.0.0.1/onvif/media_service</tds:XAddr>
        </tds:Media>
        <tds:PTZ>
          <tds:XAddr>http://127.0.0.1/onvif/ptz_service</tds:XAddr>
        </tds:PTZ>
      </tds:Capabilities>
    </tds:GetCapabilitiesResponse>
  </env:Body>
</env:Envelope>"""

PROFILES_RESPONSE = f"""<?xml version="1.0" encoding="UTF-8"?>
<env:Envelope xmlns:env="{ENV}" xmlns:trt="{TRT}" xmlns:tt="{TT}">
  <env:Body>
    <trt:GetProfilesResponse>
      <trt:Profiles token="profile1" fixed="true">
        <tt:Name>main</tt:Name>
        <tt:VideoSourceConfiguration token="vs0"/>
        <tt:VideoEncoderConfiguration token="enc1"/>
        <tt:PTZConfiguration token="ptz1"/>
      </trt:Profiles>
      <trt:Profiles token="profile2" fixed="false">
        <tt:Name>sub</tt:Name>
        <tt:VideoSourceConfiguration token="vs0"/>
        <tt:VideoEncoderConfiguration token="enc2"/>
        <tt:PTZConfiguration token="ptz1"/>
      </trt:Profiles>
    </trt:GetProfilesResponse>
  </env:Body>
</env:Envelope>"""

STREAM_URI_RESPONSE = f"""<?xml version="1.0" encoding="UTF-8"?>
<env:Envelope xmlns:env="{ENV}" xmlns:trt="{TRT}" xmlns:tt="{TT}">
  <env:Body>
    <trt:GetStreamUriResponse>
      <trt:MediaUri>
        <tt:Uri>rtsp://127.0.0.1:554/Streaming/Channels/101</tt:Uri>
        <tt:InvalidAfterConnect>false</tt:InvalidAfterConnect>
        <tt:InvalidAfterReboot>false</tt:InvalidAfterReboot>
        <tt:Timeout>PT30S</tt:Timeout>
      </trt:MediaUri>
    </trt:GetStreamUriResponse>
  </env:Body>
</env:Envelope>"""

GOTO_PRESET_RESPONSE = f"""<?xml version="1.0" encoding="UTF-8"?>
<env:Envelope xmlns:env="{ENV}" xmlns:trt="{TRT}">
  <env:Body>
    <trt:GotoPresetResponse/>
  </env:Body>
</env:Envelope>"""

FAULT_RESPONSE = f"""<?xml version="1.0" encoding="UTF-8"?>
<env:Envelope xmlns:env="{ENV}">
  <env:Body>
    <env:Fault>
      <faultcode>env:Server</faultcode>
      <faultstring>Requested operation not implemented</faultstring>
    </env:Fault>
  </env:Body>
</env:Envelope>"""

UNAUTHORIZED_RESPONSE = f"""<?xml version="1.0" encoding="UTF-8"?>
<env:Envelope xmlns:env="{ENV}">
  <env:Body>
    <env:Fault>
      <faultcode>env:Client</faultcode>
      <faultstring>Unauthorized: authentication required</faultstring>
    </env:Fault>
  </env:Body>
</env:Envelope>"""

ROUTES = {
    "GetDeviceInformation": DEVICE_INFO_RESPONSE,
    "GetCapabilities": CAPABILITIES_RESPONSE,
    "GetProfiles": PROFILES_RESPONSE,
    "GetStreamUri": STREAM_URI_RESPONSE,
    "GotoPreset": GOTO_PRESET_RESPONSE,
}


def _grab_tag(body, tag):
    # Require a boundary after the tag name so `wsse:Username` does not match
    # inside `<wsse:UsernameToken>`.
    m = re.search(r"<%s(?:\s[^>]*)?>(.*?)</%s>" % (tag, tag), body, re.S)
    return m.group(1) if m else ""


def _verify_digest(body, username, password):
    user = _grab_tag(body, "wsse:Username")
    nonce_b64 = _grab_tag(body, "wsse:Nonce")
    created = _grab_tag(body, "wsu:Created")
    pw = _grab_tag(body, "wsse:Password")
    if user != username or not nonce_b64 or not created:
        return False, "missing/invalid wsse fields"
    try:
        nonce = base64.b64decode(nonce_b64)
    except Exception:
        return False, "bad nonce base64"
    if not nonce:
        return False, "empty nonce"
    expected = base64.b64encode(
        hashlib.sha1(nonce + created.encode("utf-8") + password.encode("utf-8")).digest()
    ).decode("ascii")
    return (expected == pw), "digest mismatch"


class OnvifMock:
    """Stateless request router; shared by the handler and the ctest launcher."""

    def __init__(self, auth="digest", username="admin", password="pass",
                 basic_required=False):
        self.auth = auth            # "digest" | "basic" | "open"
        self.username = username
        self.password = password
        self.basic_required = basic_required  # digest-off when True

    def handle(self, body, headers):
        # 1. Authentication gate -------------------------------------------------
        if self.auth == "digest" and not self.basic_required:
            ok, _ = _verify_digest(body, self.username, self.password)
            if not ok:
                return (401, UNAUTHORIZED_RESPONSE)
        elif self.auth == "basic" or self.basic_required:
            header = headers.get("Authorization", "")
            expected = "Basic " + base64.b64encode(
                ("%s:%s" % (self.username, self.password)).encode("utf-8")
            ).decode("ascii")
            if header != expected:
                return (401, UNAUTHORIZED_RESPONSE)

        # 2. Route by operation marker -------------------------------------------
        for marker, response in ROUTES.items():
            if marker in body:
                return (200, response)
        return (500, FAULT_RESPONSE)


class OnvifHandler(http.server.BaseHTTPRequestHandler):
    # HTTP/1.0 closes each connection, so the client (WinHTTP) does not hold
    # the socket open and the handler never sees ConnectionResetError noise.
    protocol_version = "HTTP/1.0"

    def do_POST(self):
        n = int(self.headers.get("Content-Length", 0))
        body = self.rfile.read(n).decode("utf-8", errors="replace")
        status, xml_body = self.server.mock.handle(body, self.headers)
        data = xml_body.encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "text/xml; charset=utf-8")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def log_message(self, *args):
        pass


class OnvifServer(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True

    def __init__(self, addr, mock):
        self.mock = mock
        super().__init__(addr, OnvifHandler)


def make_server(mock, host="127.0.0.1", port=0, https=False,
                cert=None, key=None):
    server = OnvifServer((host, port), mock)
    if https:
        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        ctx.load_cert_chain(cert, key)
        server.socket = ctx.wrap_socket(server.socket, server_side=True)
    return server


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=0)
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--mode", choices=["digest", "basic", "open"], default="digest")
    ap.add_argument("--username", default="admin")
    ap.add_argument("--password", default="pass")
    ap.add_argument("--https", action="store_true")
    ap.add_argument("--cert", default="tests/fixtures/server.crt")
    ap.add_argument("--key", default="tests/fixtures/server.key")
    ap.add_argument("--port-file", default=None)
    ap.add_argument("--quit-after", type=float, default=None)
    args = ap.parse_args()

    mock = OnvifMock(auth=args.mode, username=args.username,
                     password=args.password)
    server = make_server(mock, host=args.host, port=args.port,
                         https=args.https, cert=args.cert, key=args.key)
    if args.port_file:
        with open(args.port_file, "w") as f:
            f.write(str(server.server_address[1]))
    try:
        server.serve_forever(timeout=args.quit_after)
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()
    return 0


if __name__ == "__main__":
    sys.exit(main())