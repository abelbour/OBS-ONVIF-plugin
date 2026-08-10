# Vendored third-party libraries

All vendored code is header/source-only and built from source (no binary
blobs). Licensing notes for GPLv3 compliance per `PLAN.md` §M4.

| Library | Version | Dir | License | Notes |
|---|---|---|---|---|
| TinyXML2 | 10.0.0 | `third_party/tinyxml2/` | zlib — see header text in `tinyxml2.h` | XML DOM used by `onvif/xml` |
| nlohmann/json | 3.11.3 | `third_party/nlohmann/json.hpp` | MIT | JSON store used by the registry (M2) |

Both are compatible with the plugin's GPLv3 license (zlib and MIT are
permissive). Do not upgrade in place without re-recording the pinned version.