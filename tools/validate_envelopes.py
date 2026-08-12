#!/usr/bin/env python3
"""Schema-conformance lane (IMPLEMENTATION_PLAN.md §8).

Fails the build on any drift between the request envelopes the plugin emits and
the current ONVIF schemas. Two checks:

  1. CROSS-CHECK — every operation the plugin emits must exist in the matching
     onvif.org WSDL (mirrored under third_party/onvif-schemas/). If we start
     emitting an operation the real spec doesn't define (a renamed element, a
     made-up op, a drifted namespace), this lane fails.
  2. ENVELOPE CHECK — every captured request envelope (dumped by the mock's
     --dump-envelopes mode) must parse, be in an in-scope ONVIF namespace, be a
     registered operation, and contain its required request elements.

The ver20 display (OSD) service has no WSDL published on a stable onvif.org
URL, so OSD envelopes get the namespace + operation + required-element checks
only. Full XSD validation of onvif.xsd is deliberately not attempted: libxml2
(lxml) cannot compile onvif.xsd because of its XSD 1.0 Unique-Particle-
Attribution violations, so the lane checks structure against the WSDL element
definitions instead.

usage: python tools/validate_envelopes.py --schema-dir third_party/onvif-schemas
       --envelopes <dir-of-*.xml | single.xml>
"""
import argparse
import glob
import os
import sys
import xml.etree.ElementTree as ET

SOAP_ENVELOPE = "http://www.w3.org/2003/05/soap-envelope"

# namespace -> WSDL file (relative to the schema dir). The ver20 display
# service has no published WSDL; it is validated structurally only.
NAMESPACES = {
    "http://www.onvif.org/ver10/device/wsdl": "ver10/device/wsdl/devicemgmt.wsdl",
    "http://www.onvif.org/ver10/media/wsdl": "ver10/media/wsdl/media.wsdl",
    "http://www.onvif.org/ver20/media/wsdl": "ver20/media/wsdl/media.wsdl",
    "http://www.onvif.org/ver20/ptz/wsdl": "ver20/ptz/wsdl/ptz.wsdl",
    "http://www.onvif.org/ver20/imaging/wsdl": "ver20/imaging/wsdl/imaging.wsdl",
}
DISPLAY_NS = "http://www.onvif.org/ver20/display/wsdl"

# Proprietary extensions the plugin emits that have no ONVIF-standard WSDL
# operation. Everything else must match the spec (a new op forces a manifest
# update here or the lane fails).
EXEMPT_OPERATIONS = {
    ("http://www.onvif.org/ver20/ptz/wsdl", "RenamePreset"),
}

# Operations the plugin emits, per service namespace, with the required direct
# children of each request element (local names, order-insensitive here — the
# WSDL cross-check catches name drift; this catches missing payload).
OPS = {
    "http://www.onvif.org/ver10/device/wsdl": {
        "GetCapabilities": [],
        "GetDeviceInformation": [],
        "GetNetworkInterfaces": [],
        "GetHostname": [],
        "SetNetworkInterfaces": ["InterfaceToken", "NetworkInterface"],
        "SetHostname": ["Name"],
        "SetNTP": ["FromDHCP"],
    },
    "http://www.onvif.org/ver10/media/wsdl": {
        "GetProfiles": [],
        "GetStreamUri": ["ProfileToken"],
        "GetVideoEncoderConfigurations": [],
        "GetVideoEncoderConfigurationOptions": [],
        "SetVideoEncoderConfiguration": ["Configuration"],
    },
    "http://www.onvif.org/ver20/media/wsdl": {
        "GetProfiles": [],
        "GetStreamUri": ["Protocol", "ProfileToken"],
        "GetVideoEncoderConfigurations": [],
        "GetVideoEncoderConfigurationOptions": [],
        "SetVideoEncoderConfiguration": ["Configuration", "ForcePersistence"],
    },
    "http://www.onvif.org/ver20/ptz/wsdl": {
        "GetPresets": ["ProfileToken"],
        "SetPreset": ["ProfileToken", "PresetName"],
        "RenamePreset": ["ProfileToken", "PresetToken", "NewName"],
        "RemovePreset": ["ProfileToken", "PresetToken"],
        "GotoPreset": ["ProfileToken", "PresetToken"],
        "ContinuousMove": ["ProfileToken", "Velocity"],
        "Stop": ["ProfileToken"],
        "AbsoluteMove": ["ProfileToken", "Position"],
        "RelativeMove": ["ProfileToken", "Translation"],
    },
    "http://www.onvif.org/ver20/imaging/wsdl": {
        "GetImagingSettings": ["VideoSourceToken"],
        "GetOptions": ["VideoSourceToken"],
        "SetImagingSettings": ["VideoSourceToken"],
    },
    DISPLAY_NS: {
        "GetOSDs": ["VideoSourceConfigurationToken"],
        "SetOSD": ["OSD"],
        "DeleteOSD": ["OSDToken"],
    },
}

XSD = "{http://www.w3.org/2001/XMLSchema}"


def _localname(tag):
    """Local name of a namespaced {uri}local element tag (ElementTree style)."""
    return tag[tag.rfind("}") + 1:] if "}" in tag else tag


def _wsdl_operation_names(wsdl_path):
    """Local names of the request elements declared by a WSDL's inline schema."""
    names = set()
    try:
        root = ET.parse(wsdl_path).getroot()
    except ET.ParseError:
        return names
    for el in root.iter():
        if el.tag == XSD + "schema":
            for child in el:
                if child.tag == XSD + "element" and child.get("name"):
                    names.add(child.get("name"))
    return names


def check_schemas(schema_dir):
    """Cross-check: every emitted op exists in the matching onvif WSDL."""
    failures = 0
    for ns, rel in NAMESPACES.items():
        path = os.path.join(schema_dir, *rel.split("/"))
        if not os.path.exists(path):
            print(f"SCHEMA MISSING {rel}")
            failures += 1
            continue
        declared = _wsdl_operation_names(path)
        for op in OPS[ns]:
            if op not in declared and (ns, op) not in EXEMPT_OPERATIONS:
                print(f"OP NOT IN SCHEMA {rel}: {op} (namespace {ns})")
                failures += 1
    return failures


def operation_element(root):
    """Returns the SOAP Body's first element child (the operation element)."""
    body = None
    for child in root:
        if _localname(child.tag) == "Body" and child.tag.startswith("{"):
            body = child
            break
    if body is None:
        return None
    for child in body:
        if isinstance(child.tag, str):
            return child
    return None


def check_envelope(path):
    try:
        root = ET.parse(path).getroot()
    except ET.ParseError as exc:
        print(f"PARSE FAIL {os.path.basename(path)}: {exc}")
        return 1
    op = operation_element(root)
    if op is None:
        print(f"NO OPERATION {os.path.basename(path)}")
        return 1
    # ElementTree tags are {uri}local; extract namespace and local name.
    if op.tag.startswith("{"):
        ns, local = op.tag[1:].split("}", 1)
    else:
        ns, local = "", op.tag
    spec = OPS.get(ns)
    if spec is None:
        print(f"UNKNOWN NAMESPACE {os.path.basename(path)}: {ns} {local}")
        return 1
    if local not in spec:
        print(f"UNKNOWN OPERATION {os.path.basename(path)}: {ns} {local}")
        return 1
    for required in spec[local]:
        if not any(_localname(c.tag) == required for c in op):
            print(f"MISSING ELEMENT {required} in {local} "
                  f"({os.path.basename(path)})")
            return 1
    if ns == DISPLAY_NS:
        print(f"OK (structural OSD) {os.path.basename(path)}: {local}")
    else:
        print(f"OK {os.path.basename(path)}: {local}")
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--schema-dir", required=True)
    ap.add_argument("--envelopes", required=True)
    args = ap.parse_args()

    failures = check_schemas(args.schema_dir)

    if os.path.isfile(args.envelopes):
        paths = [args.envelopes]
    else:
        paths = sorted(glob.glob(os.path.join(args.envelopes, "*.xml")))
    if not paths:
        print(f"no envelope files under {args.envelopes}")
        return 2
    for path in paths:
        failures += check_envelope(path)

    if failures:
        print(f"SCHEMA CONFORMANCE: {failures} failure(s)")
        return 1
    print("SCHEMA CONFORMANCE OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
