#!/usr/bin/env python3
"""Mirrors the current ONVIF XSD/WSDL set from onvif.org into
third_party/onvif-schemas/ (PLAN.md note 2, IMPLEMENTATION_PLAN.md §8).

The directory layout mirrors the URL paths (third_party/onvif-schemas/ver10/...)
so the relative schemaLocation imports inside the WSDLs resolve unchanged — the
schema-conformance lane validates captured request envelopes against these files.

usage: python tools/fetch_onvif_schemas.py [--root third_party/onvif-schemas]
"""
import argparse
import os
import urllib.request

# In-scope ONVIF service contracts (no Event/Recording/etc. — out of scope).
# Note: the Media2 service is the ver20/media WSDL (it reuses the classic
# operation names in the ver20 namespace; there is no media2.wsdl on onvif.org).
FILES = [
    "ver10/schema/onvif.xsd",
    "ver10/device/wsdl/devicemgmt.wsdl",
    "ver10/media/wsdl/media.wsdl",
    "ver20/media/wsdl/media.wsdl",
    "ver20/ptz/wsdl/ptz.wsdl",
    "ver20/imaging/wsdl/imaging.wsdl",
    "ver20/display/wsdl/display.wsdl",
]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default="third_party/onvif-schemas")
    ap.add_argument("--base", default="https://www.onvif.org")
    args = ap.parse_args()

    for rel in FILES:
        dest = os.path.join(args.root, rel.replace("/", os.sep))
        os.makedirs(os.path.dirname(dest), exist_ok=True)
        url = args.base.rstrip("/") + "/" + rel
        print(f"fetching {url}")
        with urllib.request.urlopen(url, timeout=120) as resp:
            data = resp.read()
        with open(dest, "wb") as fh:
            fh.write(data)
        print(f"  -> {dest} ({len(data)} bytes)")
    print("OK: onvif schemas mirrored")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
