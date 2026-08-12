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
import socket
import socketserver
import ssl
import sys
import threading
import time

ENV = "http://schemas.xmlsoap.org/soap/envelope/"
TDS = "http://www.onvif.org/ver10/device/wsdl"
TRT = "http://www.onvif.org/ver10/media/wsdl"
TT = "http://www.onvif.org/ver10/schema"
TTPTZ = "http://www.onvif.org/ver20/ptz/wsdl"

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
        <tds:Imaging>
          <tds:XAddr>http://127.0.0.1/onvif/imaging_service</tds:XAddr>
        </tds:Imaging>
        <tds:Display>
          <tds:XAddr>http://127.0.0.1/onvif/display_service</tds:XAddr>
        </tds:Display>
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

STREAM_URI_RESPONSE = """<?xml version="1.0" encoding="UTF-8"?>
<env:Envelope xmlns:env="{ENV}" xmlns:trt="{TRT}" xmlns:tt="{TT}">
  <env:Body>
    <trt:GetStreamUriResponse>
      <trt:MediaUri>
        <tt:Uri>rtsp://{rtsp_host}:554/Streaming/Channels/101</tt:Uri>
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


def _ptz_empty_response(op_name):
    return ('<?xml version="1.0" encoding="UTF-8"?>\n'
            f'<env:Envelope xmlns:env="{ENV}" xmlns:ttptz="{TTPTZ}">'
            f"<env:Body><ttptz:{op_name}/></env:Body></env:Envelope>")


def _ptz_presets_response(presets):
    rows = "\n".join(
        f'<ttptz:PTZPreset token="{p["token"]}">'
        f'<tt:Name>{p["name"]}</tt:Name></ttptz:PTZPreset>'
        for p in presets
    )
    return ('<?xml version="1.0" encoding="UTF-8"?>\n'
            f'<env:Envelope xmlns:env="{ENV}" xmlns:ttptz="{TTPTZ}" '
            f'xmlns:tt="{TT}"><env:Body><ttptz:GetPresetsResponse>{rows}'
            "</ttptz:GetPresetsResponse></env:Body></env:Envelope>")


def _ptz_set_preset_response(preset_token):
    return ('<?xml version="1.0" encoding="UTF-8"?>\n'
            f'<env:Envelope xmlns:env="{ENV}" xmlns:ttptz="{TTPTZ}">'
            "<env:Body><ttptz:SetPresetResponse>"
            f"<ttptz:PresetToken>{preset_token}</ttptz:PresetToken>"
            "</ttptz:SetPresetResponse></env:Body></env:Envelope>")


# -- camera configuration (config-panel backend) ------------------------------
# Encoding namespaces.
TRT = "http://www.onvif.org/ver10/media/wsdl"
TIMG = "http://www.onvif.org/ver20/imaging/wsdl"
TDISP = "http://www.onvif.org/ver20/display/wsdl"


def _num(v):
    # Tolerate ints/floats/strings; emit without a trailing ".0" for ints.
    f = float(v)
    return str(int(f)) if f == int(f) else repr(f)


def _encoder_configs_response(configs):
    rows = []
    for token, cfg in configs.items():
        rows.append(
            f'<trt:Configurations token="{token}">'
            f'<tt:Name>{cfg["name"]}</tt:Name>'
            f'<tt:Encoding>{cfg["encoding"]}</tt:Encoding>'
            f'<tt:Resolution><tt:Width>{_num(cfg["width"])}</tt:Width>'
            f'<tt:Height>{_num(cfg["height"])}</tt:Height></tt:Resolution>'
            f'<tt:RateControl><tt:FrameRateLimit>{_num(cfg["frame_rate"])}'
            f"</tt:FrameRateLimit>"
            f'<tt:BitrateLimit>{_num(cfg["bitrate"])}</tt:BitrateLimit>'
            f"</tt:RateControl></trt:Configurations>")
    return ('<?xml version="1.0" encoding="UTF-8"?>\n'
            f'<env:Envelope xmlns:env="{ENV}" xmlns:trt="{TRT}" '
            f'xmlns:tt="{TT}"><env:Body>'
            "<trt:GetVideoEncoderConfigurationsResponse>"
            + "".join(rows)
            + "</trt:GetVideoEncoderConfigurationsResponse></env:Body></env:Envelope>")


def _encoder_options_response():
    return ('<?xml version="1.0" encoding="UTF-8"?>\n'
            f'<env:Envelope xmlns:env="{ENV}" xmlns:trt="{TRT}" '
            f'xmlns:tt="{TT}"><env:Body>'
            "<trt:GetVideoEncoderConfigurationOptionsResponse>"
            "<trt:Options><tt:FrameRateRange><tt:Min>1</tt:Min>"
            "<tt:Max>30</tt:Max></tt:FrameRateRange>"
            "<tt:BitrateRange><tt:Min>32</tt:Min><tt:Max>8192</tt:Max>"
            "</tt:BitrateRange><tt:H264>"
            "<tt:ResolutionAvailable><tt:Width>1920</tt:Width>"
            "<tt:Height>1080</tt:Height></tt:ResolutionAvailable>"
            "<tt:ResolutionAvailable><tt:Width>1280</tt:Width>"
            "<tt:Height>720</tt:Height></tt:ResolutionAvailable>"
            "<tt:ResolutionAvailable><tt:Width>640</tt:Width>"
            "<tt:Height>480</tt:Height></tt:ResolutionAvailable>"
            "</tt:H264></trt:Options>"
            "</trt:GetVideoEncoderConfigurationOptionsResponse>"
            "</env:Body></env:Envelope>")


def _set_encoder_response():
    return ('<?xml version="1.0" encoding="UTF-8"?>\n'
            f'<env:Envelope xmlns:env="{ENV}" xmlns:trt="{TRT}">'
            "<env:Body><trt:SetVideoEncoderConfigurationResponse/>"
            "</env:Body></env:Envelope>")


def _imaging_settings_response(im):
    return ('<?xml version="1.0" encoding="UTF-8"?>\n'
            f'<env:Envelope xmlns:env="{ENV}" xmlns:timg="{TIMG}" '
            f'xmlns:tt="{TT}"><env:Body>'
            "<timg:GetImagingSettingsResponse><tt:ImagingSettings>"
            f'<tt:Brightness>{_num(im["brightness"])}</tt:Brightness>'
            f'<tt:ColorSaturation>{_num(im["color_saturation"])}'
            "</tt:ColorSaturation>"
            f'<tt:Contrast>{_num(im["contrast"])}</tt:Contrast>'
            f'<tt:Sharpness>{_num(im["sharpness"])}</tt:Sharpness>'
            "</tt:ImagingSettings></timg:GetImagingSettingsResponse>"
            "</env:Body></env:Envelope>")


def _imaging_options_response():
    return ('<?xml version="1.0" encoding="UTF-8"?>\n'
            f'<env:Envelope xmlns:env="{ENV}" xmlns:timg="{TIMG}" '
            f'xmlns:tt="{TT}"><env:Body>'
            "<timg:GetImagingOptionsResponse><tt:ImagingOptions>"
            "<tt:Brightness><tt:Min>0</tt:Min><tt:Max>100</tt:Max>"
            "</tt:Brightness>"
            "<tt:ColorSaturation><tt:Min>0</tt:Min><tt:Max>100</tt:Max>"
            "</tt:ColorSaturation>"
            "<tt:Contrast><tt:Min>0</tt:Min><tt:Max>100</tt:Max></tt:Contrast>"
            "<tt:Sharpness><tt:Min>0</tt:Min><tt:Max>100</tt:Max></tt:Sharpness>"
            "</tt:ImagingOptions></timg:GetImagingOptionsResponse>"
            "</env:Body></env:Envelope>")


def _set_imaging_response():
    return ('<?xml version="1.0" encoding="UTF-8"?>\n'
            f'<env:Envelope xmlns:env="{ENV}" xmlns:timg="{TIMG}">'
            "<env:Body><timg:SetImagingSettingsResponse/>"
            "</env:Body></env:Envelope>")


def _network_interfaces_response(netifs):
    rows = []
    for n in netifs:
        if n["dhcp"]:
            ipv4 = "<tt:DHCP>true</tt:DHCP>"
        else:
            ipv4 = (f'<tt:Manual><tt:Address>{n["address"]}</tt:Address>'
                    f'<tt:PrefixLength>{_num(n["prefix_length"])}</tt:PrefixLength>'
                    "</tt:Manual>")
        rows.append(
            f'<tds:NetworkInterfaces token="{n["token"]}">'
            f'<tt:Enabled>{"true" if n["enabled"] else "false"}</tt:Enabled>'
            f'<tt:Info><tt:Name>{n["name"]}</tt:Name>'
            f'<tt:HwAddress>00:11:22:33:44:55</tt:HwAddress></tt:Info>'
            f'<tt:IPv4><tt:Enabled>true</tt:Enabled>{ipv4}</tt:IPv4>'
            "</tds:NetworkInterfaces>")
    return ('<?xml version="1.0" encoding="UTF-8"?>\n'
            f'<env:Envelope xmlns:env="{ENV}" xmlns:tds="{TDS}" '
            f'xmlns:tt="{TT}"><env:Body>'
            "<tds:GetNetworkInterfacesResponse>"
            + "".join(rows)
            + "</tds:GetNetworkInterfacesResponse></env:Body></env:Envelope>")


def _set_network_response():
    return ('<?xml version="1.0" encoding="UTF-8"?>\n'
            f'<env:Envelope xmlns:env="{ENV}" xmlns:tds="{TDS}">'
            "<env:Body><tds:SetNetworkInterfacesResponse>"
            "<tds:RebootNeeded>false</tds:RebootNeeded>"
            "</tds:SetNetworkInterfacesResponse></env:Body></env:Envelope>")


def _osds_response(osds):
    rows = []
    for o in osds:
        rows.append(
            f'<tdisp:OSDs token="{o["token"]}"><tt:Type>Text</tt:Type>'
            "<tt:Position><tt:X>0</tt:X><tt:Y>0</tt:Y></tt:Position>"
            '<tt:TextString type="Plain">'
            f'<tt:PlainText>{o["text"]}</tt:PlainText>'
            "</tt:TextString></tdisp:OSDs>")
    return ('<?xml version="1.0" encoding="UTF-8"?>\n'
            f'<env:Envelope xmlns:env="{ENV}" xmlns:tdisp="{TDISP}" '
            f'xmlns:tt="{TT}"><env:Body><tdisp:GetOSDsResponse>'
            + "".join(rows)
            + "</tdisp:GetOSDsResponse></env:Body></env:Envelope>")


def _set_osd_response():
    return ('<?xml version="1.0" encoding="UTF-8"?>\n'
            f'<env:Envelope xmlns:env="{ENV}" xmlns:tdisp="{TDISP}">'
            "<env:Body><tdisp:SetOSDResponse/>"
            "</env:Body></env:Envelope>")


def _delete_osd_response():
    return ('<?xml version="1.0" encoding="UTF-8"?>\n'
            f'<env:Envelope xmlns:env="{ENV}" xmlns:tdisp="{TDISP}">'
            "<env:Body><tdisp:DeleteOSDResponse/>"
            "</env:Body></env:Envelope>")


def _get_hostname_response(name):
    return ('<?xml version="1.0" encoding="UTF-8"?>\n'
            f'<env:Envelope xmlns:env="{ENV}" xmlns:tds="{TDS}">'
            f"<env:Body><tds:GetHostnameResponse><tds:Name>{name}</tds:Name>"
            "</tds:GetHostnameResponse></env:Body></env:Envelope>")


def _set_hostname_response():
    return ('<?xml version="1.0" encoding="UTF-8"?>\n'
            f'<env:Envelope xmlns:env="{ENV}" xmlns:tds="{TDS}">'
            "<env:Body><tds:SetHostnameResponse/>"
            "</env:Body></env:Envelope>")


def _set_ntp_response():
    return ('<?xml version="1.0" encoding="UTF-8"?>\n'
            f'<env:Envelope xmlns:env="{ENV}" xmlns:tds="{TDS}">'
            "<env:Body><tds:SetNTPResponse/>"
            "</env:Body></env:Envelope>")

# -- Media2 flavor (M5a: ver20/media/wsdl) ------------------------------------
TRT2 = "http://www.onvif.org/ver20/media/wsdl"


def _caps_response(use_media2):
    media2 = ""
    if use_media2:
        media2 = ('<tds:Media2><tds:XAddr>http://127.0.0.1/onvif/media2_service'
                  '</tds:XAddr></tds:Media2>\n')
    return (f'<?xml version="1.0" encoding="UTF-8"?>\n'
            f'<env:Envelope xmlns:env="{ENV}" xmlns:tds="{TDS}" '
            f'xmlns:tt="{TT}">\n'
            f'  <env:Body>\n'
            f'    <tds:GetCapabilitiesResponse>\n'
            f'      <tds:Capabilities>\n'
            f'        <tds:Device><tds:XAddr>http://127.0.0.1/onvif/'
            f'device_service</tds:XAddr></tds:Device>\n'
            f'        <tds:Media><tds:XAddr>http://127.0.0.1/onvif/'
            f'media_service</tds:XAddr></tds:Media>\n'
            f'        {media2}'
            f'        <tds:PTZ><tds:XAddr>http://127.0.0.1/onvif/'
            f'ptz_service</tds:XAddr></tds:PTZ>\n'
            f'        <tds:Imaging><tds:XAddr>http://127.0.0.1/onvif/'
            f'imaging_service</tds:XAddr></tds:Imaging>\n'
            f'        <tds:Display><tds:XAddr>http://127.0.0.1/onvif/'
            f'display_service</tds:XAddr></tds:Display>\n'
            f'      </tds:Capabilities>\n'
            f'    </tds:GetCapabilitiesResponse>\n'
            f'  </env:Body>\n'
            f'</env:Envelope>')


def _profiles2_response():
    return ('<?xml version="1.0" encoding="UTF-8"?>\n'
            f'<env:Envelope xmlns:env="{ENV}" xmlns:trt2="{TRT2}" '
            f'xmlns:tt="{TT}">'
            "<env:Body><trt2:GetProfiles2Response>"
            '<trt2:Profiles token="mp1"><tt:Name>main2</tt:Name>'
            '<tt:VideoSourceConfiguration token="vs0"/>'
            '<tt:VideoEncoderConfiguration token="enc1"/>'
            '<tt:PTZConfiguration token="ptz1"/></trt2:Profiles>'
            '<trt2:Profiles token="mp2"><tt:Name>sub2</tt:Name>'
            '<tt:VideoSourceConfiguration token="vs0"/>'
            '<tt:VideoEncoderConfiguration token="enc2"/>'
            '<tt:PTZConfiguration token="ptz1"/></trt2:Profiles>'
            "</trt2:GetProfiles2Response></env:Body></env:Envelope>")


def _stream_uri2_response(rtsp_host):
    return ('<?xml version="1.0" encoding="UTF-8"?>\n'
            f'<env:Envelope xmlns:env="{ENV}" xmlns:trt2="{TRT2}" '
            f'xmlns:tt="{TT}"><env:Body><trt2:GetStreamUri2Response>'
            "<trt2:MediaUri>"
            f"<tt:Uri>rtsp://{rtsp_host}:554/Streaming/Channels/101</tt:Uri>"
            "<tt:InvalidAfterConnect>false</tt:InvalidAfterConnect>"
            "<tt:InvalidAfterReboot>false</tt:InvalidAfterReboot>"
            "<tt:Timeout>PT30S</tt:Timeout>"
            "</trt2:MediaUri></trt2:GetStreamUri2Response>"
            "</env:Body></env:Envelope>")


def _encoder2_configs_response(configs):
    rows = []
    for token, cfg in configs.items():
        rows.append(
            f'<trt2:Configurations token="{token}">'
            f'<tt:Name>{cfg["name"]}</tt:Name>'
            f'<tt:Encoding>{cfg["encoding"]}</tt:Encoding>'
            f'<tt:{cfg["encoding"]}><tt:Resolution><tt:Width>'
            f'{_num(cfg["width"])}</tt:Width><tt:Height>'
            f'{_num(cfg["height"])}</tt:Height></tt:Resolution>'
            f'<tt:RateControl><tt:FrameRateLimit>{_num(cfg["frame_rate"])}'
            f'</tt:FrameRateLimit><tt:EncodingInterval>1</tt:EncodingInterval>'
            f'<tt:BitrateLimit>{_num(cfg["bitrate"])}</tt:BitrateLimit>'
            f'</tt:RateControl></tt:{cfg["encoding"]}>'
            f'</trt2:Configurations>')
    return ('<?xml version="1.0" encoding="UTF-8"?>\n'
            f'<env:Envelope xmlns:env="{ENV}" xmlns:trt2="{TRT2}" '
            f'xmlns:tt="{TT}"><env:Body>'
            "<trt2:GetVideoEncoderConfigurations2Response>"
            + "".join(rows)
            + "</trt2:GetVideoEncoderConfigurations2Response></env:Body>"
            "</env:Envelope>")


def _encoder2_options_response():
    return ('<?xml version="1.0" encoding="UTF-8"?>\n'
            f'<env:Envelope xmlns:env="{ENV}" xmlns:trt2="{TRT2}" '
            f'xmlns:tt="{TT}"><env:Body>'
            "<trt2:GetVideoEncoderConfigurationOptions2Response>"
            "<trt2:Options><tt:H264>"
            "<tt:FrameRateRange><tt:Min>1</tt:Min><tt:Max>30</tt:Max>"
            "</tt:FrameRateRange>"
            "<tt:BitrateRange><tt:Min>32</tt:Min><tt:Max>8192</tt:Max>"
            "</tt:BitrateRange>"
            "<tt:ResolutionAvailable><tt:Width>1920</tt:Width>"
            "<tt:Height>1080</tt:Height></tt:ResolutionAvailable>"
            "<tt:ResolutionAvailable><tt:Width>1280</tt:Width>"
            "<tt:Height>720</tt:Height></tt:ResolutionAvailable>"
            "<tt:ResolutionAvailable><tt:Width>640</tt:Width>"
            "<tt:Height>480</tt:Height></tt:ResolutionAvailable>"
            "</tt:H264></trt2:Options>"
            "</trt2:GetVideoEncoderConfigurationOptions2Response>"
            "</env:Body></env:Envelope>")


def _set_encoder2_response():
    return ('<?xml version="1.0" encoding="UTF-8"?>\n'
            f'<env:Envelope xmlns:env="{ENV}" xmlns:trt2="{TRT2}">'
            "<env:Body><trt2:SetVideoEncoderConfiguration2Response/>"
            "</env:Body></env:Envelope>")



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

def _routes(rtsp_host, use_media2):
    return {
        "GetDeviceInformation": DEVICE_INFO_RESPONSE,
        "GetCapabilities": _caps_response(use_media2),
        "GetProfiles": PROFILES_RESPONSE,
        "GetStreamUri": STREAM_URI_RESPONSE.format(rtsp_host=rtsp_host,
                                               ENV=ENV, TRT=TRT, TT=TT),
        "GotoPreset": GOTO_PRESET_RESPONSE,
    }


def _grab_tag(body, tag):
    # Require a boundary after the tag name so `wsse:Username` does not match
    # inside `<wsse:UsernameToken>`. The optional `(?:[^:>]*:)?` prefix lets a
    # bare local name match its namespace-qualified form in the SOAP body
    # (`<trt:PresetName>` matches a lookup for "PresetName").
    m = re.search(r"<(?:[^:>]*:)?%s(?:\s[^>]*)?>(.*?)</(?:[^:>]*:)?%s>"
                  % (tag, tag), body, re.S)
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
    """Stateful request router; shared by the handler and the ctest launcher.

    PTZ presets live in `self.presets` so the lifecycle (save/list/rename/
    delete) round-trips against a real state instead of canned responses.
    """

    def __init__(self, auth="digest", username="admin", password="pass",
                 basic_required=False, rtsp_host="127.0.0.1",
                 use_media2=False, media2_faults=False):
        self.auth = auth            # "digest" | "basic" | "open"
        self.username = username
        self.password = password
        self.basic_required = basic_required  # digest-off when True
        self.rtsp_host = rtsp_host  # host in GetStreamUri replied URIs
        # M5a Media2 flavor: advertise a Media2 endpoint and answer the
        # ver20/media operations. `media2_faults` makes GetProfiles2 fault so
        # tests can assert the classic-Media fallback.
        self.use_media2 = use_media2
        self.media2_faults = media2_faults
        self.presets = [{"token": "preset1", "name": "Home"}]
        # Camera-config state (config-panel backend round-trips).
        self.encoder = {
            "enc1": {"name": "main", "encoding": "H264", "width": 1920,
                     "height": 1080, "frame_rate": 25.0, "bitrate": 4096},
            "enc2": {"name": "sub", "encoding": "H264", "width": 640,
                     "height": 480, "frame_rate": 12.0, "bitrate": 1024},
        }
        self.imaging = {
            "vs0": {"brightness": 50.0, "color_saturation": 50.0,
                    "contrast": 50.0, "sharpness": 50.0},
        }
        self.osds = [{"token": "osd1", "text": "CAM-01"}]
        self.netifs = [
            {"token": "eth0", "name": "eth0", "enabled": True, "dhcp": True,
             "address": "", "prefix_length": 0},
            {"token": "eth1", "name": "eth1", "enabled": True, "dhcp": False,
             "address": "192.168.1.10", "prefix_length": 24},
        ]
        # M5b device hostname + NTP state.
        self.hostname = "mock-cam"
        self.ntp = {"dhcp": True, "servers": []}
        # M4 §6.8 latency/keep-alive instrumentation (ptz_latency_live).
        self._lock = threading.Lock()
        self.request_count = 0        # total HTTP requests served
        self.connection_count = 0     # distinct TCP connections accepted
        self.move_requests = 0        # ContinuousMove requests served
        self.stop_requests = 0        # Stop requests served
        self.max_concurrent_moves = 0 # peak in-flight ContinuousMoves
        self._current_moves = 0
        self.last_stop_monotonic = None
        self.move_delay_seconds = 0.0  # delay ContinuousMove responses by this

    def _record_ptz_stats(self, body):
        with self._lock:
            self.move_requests += 1
            self._current_moves += 1
            self.max_concurrent_moves = max(self.max_concurrent_moves,
                                            self._current_moves)

    def _handle_config(self, body):
        if "GetVideoEncoderConfigurations" in body:
            return (200, _encoder_configs_response(self.encoder))
        if "GetVideoEncoderConfigurationOptions" in body:
            return (200, _encoder_options_response())
        if "SetVideoEncoderConfiguration" in body:
            first = next(iter(self.encoder.values()))
            name = _grab_tag(body, "Name")
            encoding = _grab_tag(body, "Encoding")
            width = _grab_tag(body, "Width")
            height = _grab_tag(body, "Height")
            frame_rate = _grab_tag(body, "FrameRateLimit")
            bitrate = _grab_tag(body, "BitrateLimit")
            if name:
                first["name"] = name
            if encoding:
                first["encoding"] = encoding
            if width:
                first["width"] = int(float(width))
            if height:
                first["height"] = int(float(height))
            if frame_rate:
                first["frame_rate"] = float(frame_rate)
            if bitrate:
                first["bitrate"] = int(float(bitrate))
            return (200, _set_encoder_response())
        if "GetImagingSettings" in body:
            return (200, _imaging_settings_response(self.imaging["vs0"]))
        if "GetImagingOptions" in body:
            return (200, _imaging_options_response())
        if "SetImagingSettings" in body:
            im = self.imaging["vs0"]
            for key, tag in [("brightness", "Brightness"),
                             ("color_saturation", "ColorSaturation"),
                             ("contrast", "Contrast"),
                             ("sharpness", "Sharpness")]:
                v = _grab_tag(body, tag)
                if v:
                    im[key] = float(v)
            return (200, _set_imaging_response())
        if "GetNetworkInterfaces" in body:
            return (200, _network_interfaces_response(self.netifs))
        if "SetNetworkInterfaces" in body:
            first = self.netifs[0]
            first["dhcp"] = "<tt:DHCP>true</tt:DHCP>" in body
            addr = _grab_tag(body, "Address")
            prefix = _grab_tag(body, "PrefixLength")
            if addr:
                first["address"] = addr
            if prefix:
                first["prefix_length"] = int(prefix)
            return (200, _set_network_response())
        if "GetHostname" in body:
            return (200, _get_hostname_response(self.hostname))
        if "SetHostname" in body:
            name = _grab_tag(body, "Name")
            if name:
                self.hostname = name
            return (200, _set_hostname_response())
        if "SetNTP" in body:
            dhcp = _grab_tag(body, "FromDHCP")
            self.ntp["dhcp"] = (dhcp == "true")
            addr = _grab_tag(body, "Address")
            self.ntp["servers"] = [addr] if addr else []
            return (200, _set_ntp_response())
        if "GetOSDs" in body:
            return (200, _osds_response(self.osds))
        if "SetOSD" in body:
            text = _grab_tag(body, "PlainText")
            if text and self.osds:
                self.osds[0]["text"] = text
            elif text:
                self.osds.append({"token": "osd1", "text": text})
            return (200, _set_osd_response())
        if "DeleteOSD" in body:
            token = _grab_tag(body, "OSDToken")
            self.osds = [o for o in self.osds if o["token"] != token]
            return (200, _delete_osd_response())
        return None

    def _handle_ptz(self, body):
        if "GetPresets" in body:
            return (200, _ptz_presets_response(self.presets))
        if "SetPreset" in body:
            name = _grab_tag(body, "PresetName") or "preset"
            token = "preset%d" % (len(self.presets) + 1)
            self.presets.append({"token": token, "name": name})
            return (200, _ptz_set_preset_response(token))
        if "RenamePreset" in body:
            token = _grab_tag(body, "PresetToken")
            new_name = _grab_tag(body, "NewName")
            for p in self.presets:
                if p["token"] == token:
                    p["name"] = new_name
            return (200, _ptz_empty_response("RenamePresetResponse"))
        if "DeletePreset" in body:
            token = _grab_tag(body, "PresetToken")
            self.presets = [p for p in self.presets if p["token"] != token]
            return (200, _ptz_empty_response("DeletePresetResponse"))
        if "ContinuousMove" in body:
            self._record_ptz_stats(body)
            try:
                if self.move_delay_seconds > 0:
                    time.sleep(self.move_delay_seconds)
            finally:
                with self._lock:
                    self._current_moves -= 1
            return (200, _ptz_empty_response("ContinuousMoveResponse"))
        if "AbsoluteMove" in body:
            return (200, _ptz_empty_response("AbsoluteMoveResponse"))
        if "RelativeMove" in body:
            return (200, _ptz_empty_response("RelativeMoveResponse"))
        if "Stop" in body:
            with self._lock:
                self.stop_requests += 1
                self.last_stop_monotonic = time.monotonic()
            return (200, _ptz_empty_response("StopResponse"))
        return None

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

        # 2. Stateful Media2 operations (ver20) -------------------------------
        # Checked before the classic handlers: every *2 operation name is a
        # superset of its classic counterpart, so it must win the match.
        m2 = self._handle_media2(body)
        if m2 is not None:
            return m2

        # 3. Stateful PTZ operations -------------------------------------------
        ptz = self._handle_ptz(body)
        if ptz is not None:
            return ptz

        # 4. Stateful camera-config operations --------------------------------
        cfg = self._handle_config(body)
        if cfg is not None:
            return cfg

        # 5. Stateless route by operation marker ------------------------------
        for marker, response in _routes(self.rtsp_host, self.use_media2).items():
            if marker in body:
                return (200, response)
        return (500, FAULT_RESPONSE)

    def _handle_media2(self, body):
        if not self.use_media2:
            return None
        if "GetProfiles2" in body:
            if self.media2_faults:
                return (500, FAULT_RESPONSE)
            return (200, _profiles2_response())
        if "GetStreamUri2" in body:
            return (200, _stream_uri2_response(self.rtsp_host))
        if "GetVideoEncoderConfigurations2" in body:
            return (200, _encoder2_configs_response(self.encoder))
        if "GetVideoEncoderConfigurationOptions2" in body:
            return (200, _encoder2_options_response())
        if "SetVideoEncoderConfiguration2" in body:
            first = next(iter(self.encoder.values()))
            name = _grab_tag(body, "Name")
            encoding = _grab_tag(body, "Encoding")
            width = _grab_tag(body, "Width")
            height = _grab_tag(body, "Height")
            frame_rate = _grab_tag(body, "FrameRateLimit")
            bitrate = _grab_tag(body, "BitrateLimit")
            if name:
                first["name"] = name
            if encoding:
                first["encoding"] = encoding
            if width:
                first["width"] = int(float(width))
            if height:
                first["height"] = int(float(height))
            if frame_rate:
                first["frame_rate"] = float(frame_rate)
            if bitrate:
                first["bitrate"] = int(float(bitrate))
            return (200, _set_encoder2_response())
        return None


class OnvifHandler(http.server.BaseHTTPRequestHandler):
    # HTTP/1.1 keep-alive: the client reuses one connection for consecutive
    # SOAP calls (M4 §6.8). Content-Length is always sent so the handler knows
    # when a response ends and can stay open for the next request.
    protocol_version = "HTTP/1.1"

    def setup(self):
        super().setup()
        mock = self.server.mock
        with mock._lock:
            mock.connection_count += 1

    def handle(self):
        # A keep-alive client may close the connection between requests
        # (per-call WinHTTP); that surfaces as a reset on the next read and is
        # not an error worth tracing.
        try:
            super().handle()
        except (ConnectionResetError, BrokenPipeError, OSError):
            self.close_connection = True

    def do_POST(self):
        n = int(self.headers.get("Content-Length", 0))
        body = self.rfile.read(n).decode("utf-8", errors="replace")
        mock = self.server.mock
        status, xml_body = mock.handle(body, self.headers)
        with mock._lock:
            mock.request_count += 1
        data = xml_body.encode("utf-8")
        try:
            self.send_response(status)
            self.send_header("Content-Type", "text/xml; charset=utf-8")
            self.send_header("Content-Length", str(len(data)))
            if self.headers.get("Connection", "").lower() == "close":
                self.close_connection = True
            self.end_headers()
            self.wfile.write(data)
            self.wfile.flush()
        except (ConnectionResetError, BrokenPipeError, OSError):
            # The client may abort an in-flight request (immediate stop).
            self.close_connection = True

    def log_message(self, *args):
        pass


class OnvifServer(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True

    def __init__(self, addr, mock):
        self.mock = mock
        super().__init__(addr, OnvifHandler)


PROBE_MATCH_REPLY = """<?xml version="1.0" encoding="UTF-8"?>
<soap:Envelope xmlns:soap="http://www.w3.org/2003/05/soap-envelope"
               xmlns:wsa="http://schemas.xmlsoap.org/ws/2004/08/addressing"
               xmlns:d="http://schemas.xmlsoap.org/ws/2005/04/discovery"
               xmlns:dn="http://www.onvif.org/ver10/network/wsdl">
  <soap:Header>
    <wsa:MessageID>urn:uuid:0d8c9f97-8b2f-4a1b-8b5f-6f9b2d0a1e3c</wsa:MessageID>
    <wsa:Action>http://schemas.xmlsoap.org/ws/2005/04/discovery/ProbeMatches</wsa:Action>
  </soap:Header>
  <soap:Body>
    <d:ProbeMatches>
      <d:ProbeMatch>
        <wsa:Address>urn:uuid:bd43994a-1e5f-4e8a-9fc5-2e8b1c3d5f7a</wsa:Address>
        <d:Types>dn:NetworkVideoTransmitter d:Device</d:Types>
        <d:Scopes>onvif://www.onvif.org/name/DS-2CD2032-I onvif://www.onvif.org/hardware/DS-2CD2032-I onvif://www.onvif.org/mac/40:d8:2e:12:34:56 onvif://www.onvif.org/type/video_encoder</d:Scopes>
        <d:XAddrs>http://127.0.0.1/onvif/device_service</d:XAddrs>
        <d:MetadataVersion>1</d:MetadataVersion>
      </d:ProbeMatch>
    </d:ProbeMatches>
  </soap:Body>
</soap:Envelope>"""


def udp_echo_reply(reply_payload, port, timeout=15.0, justify_probe=None):
    """Listens on 127.0.0.1:port, replies to ONE discovery datagram with
    `reply_payload`. Immediately returns the raw probe datagram received, so a
    caller can assert on it (or pass `justify_probe` to have that asserted
    here). Raises/signals failure via the exception on timeout."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(("127.0.0.1", port))
    s.settimeout(timeout)
    try:
        data, addr = s.recvfrom(65535)
        if justify_probe is not None:
            assert justify_probe in data, "probe did not contain expected text"
        s.sendto(reply_payload.encode("utf-8"), addr)
        return data
    finally:
        s.close()


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