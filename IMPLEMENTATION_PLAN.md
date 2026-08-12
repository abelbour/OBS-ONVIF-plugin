# OBS ONVIF Plugin — Implementation Plan

> Companion to `PLAN.md` (the authoritative decisions/spec doc). This file turns those decisions into an ordered, buildable, code-level plan. Where this file and `PLAN.md` disagree, `PLAN.md` wins; if you change a decision, update both.

Table of contents:
1. [How to drive development with this plan](#1-how-to-drive-development-with-this-plan)
2. [Repository layout](#2-repository-layout)
3. [Milestone 0 — repo scaffolding + green CI](#3-milestone-0--repo-scaffolding--green-ci)
4. [Milestone 1 — ONVIF core](#4-milestone-1--onvif-core)
5. [Milestone 2 — Registry + persistence](#5-milestone-2--registry--persistence)
6. [Milestone 3 — OBS layer](#6-milestone-3--obs-layer)
7. [Milestone 4 — hardening + packaging](#7-milestone-4--hardening--packaging)
8. [Schema-conformance CI](#8-schema-conformance-ci)
9. [Mock ONVIF server](#9-mock-onvif-server)
10. [Risk register](#10-risk-register)

---

## 1. How to drive development with this plan

- **No local compiler initially** — every milestone is gated by GitHub Actions CI on `windows-2022`. Rule: *nothing merges unless the CI job for its lane is green*.
- Each milestone section lists **files to create**, **acceptance checks**, and **code sketches**. Sketches are compile-oriented pseudocode with real API calls; they are seed code meant to be grown, not final implementations.
- Chain rule (from `PLAN.md` §Dependency graph): core (M1) → registry (M2) → OBS layer (M3). `obs/abi` is a leaf over M2 + scene-presets store.
- Compat promised in the sketches: **OBS 32.x, Qt6, Windows 10+ (v1809+), MSVC 2022 (v143), C++17, GPLv3.**

---

## 2. Repository layout

```
OBS-ONVIF-plugin/
├─ PLAN.md / IMPLEMENTATION_PLAN.md
├─ CMakeLists.txt                      # from vendored obs-plugintemplate
├─ .github/workflows/ci.yml            # build+test+zip, schema conformance
├─ Build-Windows.ps1                   # vendored from obs-plugintemplate
├─ obs-onvif/                          # OBS-facing module code
│  ├─ plugin.{h,cpp}                   # module entry, lifecycle, bridges
│  ├─ obs_mapping.{h,cpp}              # source↔camera↔profile mapping
│  ├─ obs_apply.{h,cpp}                # URL rewrite + restart + output prompt
│  ├─ scene_presets.{h,cpp}            # scene→preset firing + dialog
│  ├─ hotkeys.{h,cpp}                  # preset 1–9 + move hotkeys
│  ├─ abi.{h,cpp}                      # public ABI impl
│  ├─ dock.{h,cpp}                     # ONVIF Control dock (Qt6)
│  ├─ settings_dialog.{h,cpp}          # Tools → ONVIF Settings
│  └─ obs-onvif.h                      # PUBLIC ABI header (installed)
├─ onvif/                              # OBS-free core (unit-tested in CI)
│  ├─ xml.{h,cpp} / base64.{h,cpp} / sha1.{h,cpp} / json.{h,cpp}
│  ├─ ws_discovery.{h,cpp}
│  ├─ soap_client.{h,cpp}
│  ├─ ws_security.{h,cpp}
│  ├─ onvif_client.{h,cpp}
│  ├─ imaging.{h,cpp}
│  ├─ display.{h,cpp}
│  ├─ capabilities.{h,cpp}
│  └─ identity.{h,cpp}
├─ registry/
│  ├─ camera.h
│  ├─ store.{h,cpp}                    # JSON + wincred, per collection
│  ├─ registry.{h,cpp}                 # in-memory state + discovery loop
│  └─ apply.{h,cpp}                    # live-output policy state machine
├─ third_party/
│  ├─ (vendored obs-plugintemplate tree)
│  ├─ onvif-schemas/                   # current onvif.org XSD/WSDL mirror
│  └─ nlohmann/json.hpp                # MIT single header (JSON store)
├─ tests/
│  ├─ unit/*.cpp                       # gtest targets for the onvif/ core
│  ├─ mock_onvif_server.py             # Hikvision/Dahua/Media2/HTTPS mock
│  └─ aclink/                           # ABI consumer smoke (C harness)
├─ tools/
│  ├─ validate_envelopes.py            # schema conformance (lxml/libxml2)
│  ├─ fetch_onvif_schemas.py           # mirrors onvif.org XSD/WSDL set
│  └─ make_selfsigned_cert.py          # HTTPS mock cert (CI)
└─ data/locale/en-US.ini               # en-US only (structure for l10n)
```

---

## 3. Milestone 0 — repo scaffolding + green CI

**Goal:** a no-op plugin that builds, produces an installable zip, runs a trivial unit test, and proves the OBS 32 toolchain is reachable from Actions.

### 3.1 Vendor obs-plugintemplate

Copy the latest obs-plugintemplate **into the repo** (not a submodule), renaming template symbols `obs-plugintemplate`/`sample-source` → `obs-onvif`. Keep `Build-Windows.ps1`, `CI/before_script.sh`, `CMakeLists.txt`, and the Windows runner workflow.

### 3.2 Minimal no-op module

`obs-onvif/plugin.cpp` skeleton (Qt-free so it links in M0):

```cpp
#include <obs-module.h>
#include <obs-frontend-api.h>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-onvif", "en-US")

MODULE_EXPORT const char *obs_module_name(void) { return "obs-onvif"; }
MODULE_EXPORT const char *obs_module_description(void)
{
    return "ONVIF camera discovery, PTZ/preset control, capability-aware "
           "image/stream/OSD/network config, and DHCP IP auto-repair for "
           "Media Sources.";
}

static void frontend_event(enum obs_frontend_event event, void *priv)
{
    UNUSED_PARAMETER(event); UNUSED_PARAMETER(priv);
    // Populated in M3.
}

bool obs_module_load(void)
{
    blog(LOG_INFO, "[obs-onvif] loading");
    obs_frontend_add_event_callback(frontend_event, nullptr);
    return true;
}

void obs_module_unload(void) { obs_frontend_remove_event_callback(frontend_event, nullptr); }
```

### 3.3 CI workflow (`.github/workflows/ci.yml`)

```yaml
name: ci
on: [push, pull_request]
jobs:
  windows:
    runs-on: windows-2022
    steps:
      - uses: actions/checkout@v4
      - name: Build & test
        run: .\Build-Windows.ps1 -SkipBoost -SkipVLC  # per template flags
      - name: Unit tests (core)
        run: ctest --test-dir build -C Release --output-on-failure
      - name: Schema conformance
        run: python tools/validate_envelopes.py --schema-dir third_party/onvif-schemas
      - name: Upload artifact (zip)
        uses: actions/upload-artifact@v4
        with:
          name: obs-onvif-win64
          path: release/**
```

`Build-Windows.ps1` from the template downloads pinned OBS deps and produces a zip laid out for `%APPDATA%\obs-studio\plugins`:

```
[win-x64]/obs-onvif/bin/64bit/obs-onvif.dll
[win-x64]/obs-onvif/data/locale/en-US.ini
[win-x64]/obs-onvif/obs-onvif.h            # ABI header for consumers
```

**M0 acceptance:** CI green; zip artifact downloadable; the trivial core test passes.

---

## 4. Milestone 1 — ONVIF core

All files under `onvif/` are **OBS-free** (link against nothing but the Windows API + vendored headers) so `ctest` runs them headless.

> **Status (2026-08):** implemented and CI-green (Windows x64). `onvif/base64`, `sha1`, `xml`
> (TinyXML2 wrapper), `ws_security` (UsernameToken digest, BCrypt nonce),
> `soap_client` (WinHTTP POST, SOAP fault parse, cert toggle, digest→basic auth
> fallback), `ws_discovery` (probe build + ProbeMatches/Hello/Bye parse + UDP
> surface), `identity` (scope-MAC + fingerprint), and `onvif_client` (typed
> GetCapabilities/GetDeviceInformation/GetProfiles/GetStreamUri/GotoPreset).
> Tests run headless: unit suites plus live round-trips against
> `tests/mock_onvif_server.py` (SOAP + UDP over 127.0.0.1) including HTTPS with
> the committed self-signed fixture. Open follow-ups (Midea-accurate, not
> required for M1): Media2 path and the §8 schema-conformance lane.

### 4.1 Plumbing primitives (P1a, parallelizable)

`onvif/base64.{h,cpp}`, `onvif/sha1.{h,cpp}`, `onvif/xml.{h,cpp}` (TinyXML2, vendored, zlib-licensed), `onvif/json.{h,cpp}` (thin wrapper over the vendored nlohmann single-header, so the core stays testable without OBS).

Private win32 helpers in `onvif/ws_security.cpp` using `BCryptGenRandom` for nonces:

```cpp
#include <bcrypt.h>
#pragma comment(lib, "bcrypt.lib")

static std::array<uint8_t, 16> random_bytes()
{
    std::array<uint8_t, 16> out{};
    BCRYPT_ALG_HANDLE alg = nullptr;
    if (BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_RNG_ALGORITHM,
                                                  nullptr, 0))) {
        BCryptGenRandom(alg, out.data(), (ULONG)out.size(), 0);
        BCryptCloseAlgorithmProvider(alg, 0);
    } else {
        unsigned int r = 0;
        rand_s(&r); // fallback path per byte loop
    }
    return out;
}
```

### 4.2 WS-Security digest (`onvif/ws_security.{h,cpp}`)

`PasswordDigest = base64( SHA1( base64decode(nonce) ‖ created ‖ password ) )`, per ONVIF spec. ODM’s `PasswordHelper.fs` is **excluded** (it computes a Kipod HMAC, not the ONVIF digest — see PLAN.md note 1).

```cpp
struct UsernameToken {
    std::string username, passwordText; // passwordText = legacy plaintext fallback
    std::string nonceBase64, created;   // ISO8601 UTC '2026-08-10T12:34:56Z'
    std::string passwordDigest;         // populated for digest mode
};

static std::string created_now()
{
    SYSTEMTIME st;
    GetSystemTime(&st);
    char buf[32];
    snprintf(buf, sizeof buf, "%04u-%02u-%02uT%02u:%02u:%02uZ",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return buf;
}

void BuildUsernameToken(UsernameToken &t)
{
    t.created = created_now();
    t.nonceBase64 = base64_encode(random_bytes().data(), 16);
    auto rawNonce = base64_decode(t.nonceBase64);
    std::string pre = rawNonce + t.created + t.passwordText;
    t.passwordDigest = base64_encode(sha1(pre.data(), pre.size()).data(), 20);
}
```

### 4.3 SOAP over HTTP(S) — `onvif/soap_client.{h,cpp}`

Uses **WinHTTP** (goes through Schannel for TLS 1.2/1.3 automatically; no OpenSSL). Cert-validation off by default, per-camera toggle. Timeout default 5 s. Basic-auth fallback is a second request with `Authorization: Basic`.

```cpp
#pragma comment(lib, "winhttp.lib")

struct SoapRequest {
    std::string  url;               // full XAddr
    std::string  soapEnvelope;      // body with SOAPAction header
    std::string  action;            // e.g. "http://www.onvif.org/ver10/device/wsdl/GetDeviceInformation"
    int          authMode;          // 0 = WS-Security in envelope, 1 = HTTP Basic
    std::string  basicCred;         // "user:pass" base64'd (authMode 1)
    bool         validateCert;      // per-camera
};

struct SoapResult {
    int    httpStatus;              // 200, 401, 500, ...
    bool   transportOk;
    std::string body;               // response envelope or fault detail
    std::string faultReason;        // parsed ws fault
};

bool SoapClient::Send(const SoapRequest &req, SoapResult &out)
{
    HINTERNET hSession = WinHttpOpen(L"obs-onvif/0.x (WinHTTP)",
                                     WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, nullptr,
                                     nullptr, 0);
    // parse scheme/host/port/path from req.url (rtsp-style parsing reused here)
    HINTERNET hConn = WinHttpConnect(hSession, hostW, port, 0);
    WINHTTP_STATUS_CALLBACK cb = WinHttpSetStatusCallback(...); // retry loop not shown
    DWORD flags = WINHTTP_FLAG_SECURE; // when scheme == https
    HINTERNET hReq = WinHttpOpenRequest(hConn, L"POST", pathW, nullptr,
                                        WINHTTP_NO_REFERER,
                                        WINHTTP_DEFAULT_ACCEPT_TYPES, flags);

    // TLS: accept self-signed unless the user validated the cert
    if (scheme == "https" && !req.validateCert) {
        DWORD secFlags = SECURITY_FLAG_IGNORE_UNKNOWN_CA |
                         SECURITY_FLAG_IGNORE_CERT_DATE_INVALID |
                         SECURITY_FLAG_IGNORE_CERT_CN_INVALID;
        WinHttpSetOption(hReq, WINHTTP_OPTION_SECURITY_FLAGS, &secFlags, sizeof secFlags);
    }

    if (req.authMode == 1)
        WinHttpAddRequestHeaders(hReq, L"Authorization: Basic <b64>", -1,
                                 WINHTTP_ADDREQ_FLAG_REPLACE);

    WinHttpSendRequest(hReq, wide(action), (DWORD)a.len, body.data(),
                       (DWORD)body.size(), (DWORD)body.size(), 0);
    WinHttpReceiveResponse(hReq, nullptr);
    // read body, parse HTTP status, extract fault if 4xx/5xx
    return CandidateHeadersWere201; // illustrative only
}
```

**Digest→Basic fallback logic** lives in a wrapper used by every typed call:

```
result = send(envelopeA, auth=WS-Security)
if result.httpStatus == 401 or transportOk==false:
    result = send(envelopeB, auth=Basic)      # B = A with header instead of WT
    if ok: remember(Basic mode) for this camera
else:
    remember(digest) for this camera
```

### 4.4 WS-Discovery — `onvif/ws_discovery.{h,cpp}`

Probe in **v1 / April-2005** form for broad compatibility; accept v1 and v1.1 responses. Sends **both** `NetworkVideoTransmitter` **and** `Device` types. Multicast socket joined on `239.255.255.250:3702` with `SO_REUSEADDR`; a reply-guard ensures we never re-emit multicast replies (ODM’s multicast-binding lesson).

```cpp
#pragma comment(lib, "ws2_32.lib")

constexpr uint16_t kDiscoveryPort = 3702;
const char *kDiscoveryGroup = "239.255.255.250";

std::string BuildProbe(const std::string &messageId /*uuid*/)
{
    return
    R"(<?xml version="1.0" encoding="UTF-8"?>
<soap:Envelope xmlns:soap="http://www.w3.org/2003/05/soap-envelope"
               xmlns:wsa="http://schemas.xmlsoap.org/ws/2004/08/addressing"
               xmlns:d="http://schemas.xmlsoap.org/ws/2005/04/discovery"
               xmlns:dn="http://www.onvif.org/ver10/network/wsdl">
  <soap:Header>
    <wsa:MessageID>)" + messageId + R"(</wsa:MessageID>
    <wsa:To>urn:schemas-xmlsoap-org:ws:2005:04:discovery</wsa:To>
    <wsa:Action>http://schemas.xmlsoap.org/ws/2005/04/discovery/Probe</wsa:Action>
  </soap:Header>
  <soap:Body>
    <d:Probe>
      <d:Types>dn:NetworkVideoTransmitter d:Device</d:Types>
    </d:Probe>
  </soap:Body>
</soap:Envelope>)";
}
```

Parsing `ProbeMatch`/`Hello`/`Bye` (TinyXML2): for each `d:ProbeMatch` read `wsa:RelatesTo`, `d:Types`, `d:Scopes`, `d:XAddrs`, `d:MetadataVersion`. Normalize the XAddrs into the first reachable endpoint via `identity.resolvedXAddr`. Dedup by `ListenUris`/endpoint uuid, keep TTL bookkeeping to expire offline nodes.

```cpp
struct DiscoveredDevice {
    std::vector<std::string> xaddrs;   // ProbeMatch XAddrs
    std::vector<std::string> types;    // contract types
    std::string              scopes;   // raw scopes string (for MAC uuid parse)
    std::string              uuid;     // discovered endpoint uuid
    uint64_t                 lastSeen; // monotonic clock ms
};
```

### 4.5 Typed sessions — `onvif/onvif_client.{h,cpp}` + friends

`CameraSession` wraps `onvif_client` calls so each operation is a typed small function. All use the shared `SoapClient::Call` wrapper (digest→basic) and the `5 s` timeout (retry-once for media ops).

```cpp
struct Capabilities {
    std::string deviceXAddr, mediaXAddr, media2XAddr, ptzXAddr,
                imagingXAddr, displayXAddr;
    bool hasPTZ=false, hasImaging=false, hasDisplay=false, hasMedia2=false;
};

struct DeviceInformation {
    std::string manufacturer, model, firmwareVersion, serialNumber, hardwareId;
};

// CameraSession (per camera, owns creds + authMode + capability cache)
struct CameraSession {
    Capabilities caps;
    DeviceInformation info;
    std::string authMode; // "digest" | "basic"

    bool   FetchCapabilities();               // GetCapabilities
    bool   GetDeviceInformation(DeviceInformation &out);
    std::vector<Profile> GetProfiles();       // Media2 first, classic fallback
    std::string GetStreamUri(const std::string &profileToken); // rtsp url
    // PTZ
    bool ContinuousMove(Rotate dir, double pan, double tilt, double zoom);
    bool Stop();                              // StopPanTiltZoom
    std::vector<Preset> GetPresets();
    std::string SetPreset(const std::string &name);
    bool GotoPreset(const std::string &token);
    void RenamePreset(const std::string &token, const std::string &name);
    void RemovePreset(const std::string &token);
    // imaging / display / network — thin wrappers to onvif/imaging|display + device
};
```

Profiles & preset calls are already `onvif/imaging.{h,cpp}`, `onvif/display.{h,cpp}` responsibilities (page 12 `PLAN.md`). All are thin `GetImagesSettings/SetImagingSettings/GetOptions/SetOSDOptions/SetNetworkInterface/SetNTP/SetHostname` wrappers using the shared `SoapClient` and returning typed C++ structs (no raw XML loans).

### 4.6 Identity — `onvif/identity.{h,cpp}`

```cpp
// Priority: serialNumber ▶ scopes MAC ▶ hardwareId ▶ endpoint uuid
std::string BuildFingerprint(const DeviceInformation &info,
                             const DiscoveredDevice &disc)
{
    if (!info.serialNumber.empty())
        return "serial:" + info.serialNumber;
    auto mac = ParseScopeMac(disc.scopes);   // scopes "onvif://www.onvif.org/mac/AA:BB:.."
    if (!mac.empty())
        return "mac:" + mac;
    if (!info.hardwareId.empty())
        return "hw:" + info.hardwareId;
    return "uuid:" + disc.uuid;
}
```

**M1 acceptance (ctest):** unit tests for base64/sha1/digest math, XML probe parsing of real transcript payloads; mock-server-driven tests for GetProfiles/GetStreamUri/GotoPreset round-trips, digest↔basic fallback, HTTPS via generated cert, Media2 path, and schema-conformance lane (see §8).

---

## 5. Milestone 2 — Registry + persistence

> **Status (2026-08):** implemented in `registry/` and CI-green (Windows x64).
> `camera.h` carries the data model; `store.{h,cpp}` persists cameras /
> collections / config / policies as JSON under
> `%APPDATA%\obs-studio\plugin_config\obs-onvif\` (dir injectable for tests)
> and stores secrets in the Windows Credential Vault via `CredWrite`/`CredRead`
> (never in JSON); `apply.{h,cpp}` is the deterministic live-output policy
> state machine (ask/always/ignore, 30 s auto-defer → re-offer on idle) plus
> `RewriteSourceUrl` (credential splicing); `registry.{h,cpp}` is the in-memory
> camera table with fingerprint matching and IP-change detection producing
> `SourceRewrite` records. Tests: `store_test` (round-trips + wincred on the CI
> runner), `apply_test` (all state transitions + URL helpers), `registry_test`
> (unit: first-seen / no-change / move / live-prompt / persist-restore), and a
> live `registry_live` ctest that drives the mock end-to-end — a discovery
> round-trip registers a camera, the mock re-binds to a new loopback host
> (127.0.0.2), and the registry detects the move and rewrites the mapped source
> URL with credentials spliced. Open follow-ups: continuous
> Hello/Bye listener + heartbeat loop threading and per-camera capabilities
> cache (M2→M3 bridge). The OBS-side dispatch is implemented in M3b
> (`obs/obs_apply.cpp`: output-activity → `OnOutputsIdle`, URL rewrite +
> `restart` proc).

### 5.1 Data model (mirrors PLAN.md §Data model)

`registry/camera.h`:

```cpp
struct Camera {
    std::string id;                 // fingerprint from identity.cpp
    std::string name;               // display name (editable)
    std::string username, password; // mirrored from wincred at load; keyed by id
    std::string xaddr;              // device service XAddr (resolved)
    Capabilities capabilities;      // incl. cached imagingOptions/encoderConfigs/osdOptions/networkConfig
    std::map<std::string, std::string> lastKnownRTSP; // profileToken -> url
    bool online = false;
    uint64_t lastSeen = 0;          // ms
};

struct SourceMapping {
    std::string collection_uuid;    // stable per-scene-collection key
    std::string source_name;        // OBS source name (media source)
    std::string camera_id;
    std::string profileToken;
    bool auto_apply = true;         // rewrite URL + restart on IP change
};

struct ScenePreset {
    std::string collection_uuid, scene_name, camera_id, preset_token;
};

struct AppConfig {
    int discovery_interval_s = 60;
    bool hello_listener_enabled = true;
    int soap_timeout_s = 5;
    bool soap_retry_media = true;
    int discovery_probe_timeout_s = 3;
    int prompt_timeout_s = 30;
    std::string apply_policy = "ask";       // ask | always | ignore
    std::string default_stream = "high";    // high | low
    bool restore_settings_on_reconnect = false;
    // PTZ transport/motor control (M4, §6.8) — all default-on:
    bool soap_keepalive = true;            // reuse WinHTTP connection across calls
    bool ptz_auth_cache = true;            // cache negotiated auth mode per camera
    int ptz_move_timeout_s = 0;            // 0 = omit Timeout (move until Stop)
    std::string ptz_stop_mode = "immediate"; // immediate (abort) | queued
    int ptz_min_interval_ms = 75;          // hard minimum between movement requests
};
```

### 5.2 Store — `registry/store.{h,cpp}`

- Directory: `%APPDATA%\obs-studio\plugin_config\obs-onvif\`.
- Secrets via **wincred**: `CredWrite` (CRED_TYPE_GENERIC, CRED_PERSIST_LOCAL_MACHINE, target `obs-onvif/{camera_id}` and `obs-onvif/default`).

```cpp
bool WriteCredential(const std::string &target, const std::string &secret)
{
    CREDENTIAL cred{};
    cred.Type = CRED_TYPE_GENERIC;
    std::wstring targetW = Utf8ToWide(target);
    cred.TargetName = const_cast<wchar_t*>(targetW.c_str());
    cred.CredentialBlobSize = (DWORD)secret.size();
    cred.CredentialBlob = (LPBYTE)secret.data();
    cred.Persist = CRED_PERSIST_LOCAL_MACHINE;
    return !!CredWrite(&cred, 0);
}

bool ReadCredential(const std::string &target, std::string &secret, bool &found)
{
    PCREDENTIAL cred = nullptr;
    std::wstring targetW = Utf8ToWide(target);
    if (!CredRead(targetW.c_str(), CRED_TYPE_GENERIC, 0, &cred)) {
        found = false; return true;
    }
    secret.assign((char*)cred->CredentialBlob, cred->CredentialBlobSize);
    CredFree(cred); found = true; return true;
}
```

- Config files (per collection) use nlohmann::json (`third_party/nlohmann/json.hpp`):

```cpp
// store.cpp
void Store::SaveCollection(const CollectionState &cs)
{
    json j;
    j["uuid"] = cs.uuid;
    j["display_name"] = cs.displayName;
    j["camera_mappings"] = json::array();
    for (auto &m : cs.mappings)
        j["camera_mappings"].push_back({{"source", m.sourceName},
                                        {"camera_id", m.cameraId},
                                        {"profile", m.profileToken},
                                        {"auto_apply", m.autoApply}});
    std::filesystem::path p = cfgDir_ / ("collection_" + cs.uuid + ".json");
    std::ofstream f(p); f << j.dump(2);
}
```

- Migration rules: on `SCENE_COLLECTION_RENAMED` — keep uuid, update display_name; on `SCENE_COLLECTION_ADDED` — the UUID is synthesized if absent; on `SCENE_COLLECTION_REMOVED` — delete `collection_<uuid>.json` + log a warning. Lookups resolve `collection_uuid` by display name at startup.

### 5.3 Discovery loop + IP-change detection — `registry/registry.{h,cpp}`

Worker thread (owns the socket) is the *only* thing touching `DiscoveredDevice` state; results are snapshot-copied to the UI via Qt queued signals.

```cpp
void Registry::RunDiscoveryLoop()
{
    while (!stop_) {
        // 1. one on-demand Probe (from "Scan") + one at startup
        // 2. periodic heartbeat Probe every discovery_interval_s
        // 3. continuous Hello/Bye listener (unicast-bound multicast socket, SO_REUSEADDR)
        // 4. for each ProbeMatch:
        //      resolve XAddr -> CameraSession::FetchCapabilities()
        //      fingerprint = BuildFingerprint(...)
        //      match against known cameras
        //      if matched && (newXAddr != cam.xaddr || streamUri changed for used profile):
        //          registerIpChange(cam, newStreamUri)   // cross to Apply
    }
}
```

IP-change consumption (`registerIpChange`) is pushed onto `apply` for policy handling and onto the UI thread for status updates.

### 5.4 Apply policy — `registry/apply.{h,cpp}`

Deterministic state machine (mirrors PLAN.md live-output decisions):

```
state = idle
on ipChange(cam, newUri):
    if policy[cam] == ignore: drop (log only)
    elif anyOutputActive():
        if policy[cam] == always: -> applyNow()
        else: showPrompt()                       # ask
              if answer == apply: -> applyNow()
              elif answer == ignore: policy[cam] = ignore (persist if "remember")
              elif no answer in prompt_timeout_s: -> defer (per-incident)
                   re-offer when outputs go inactive (auto-apply then)
    else: -> applyNow()                          # no output active: no prompt

applyNow():
    for each SourceMapping(cam): dispatch to UI thread -> obs_apply::RewriteSource(m)
```

**M2 acceptance (ctest, against mock server):** with the mock re-binding to a new IP and sending Hello, the registry detects the change, produces the new stream URI, and the observed `SourceMapping` rewrite (URL + credentials splice) matches the harness expectation; persistence round-trips; wincred read/write works on the CI runner; apply policy transitions are unit-tested (order of states, 30 s auto-defer path simulated).

---

## 6. Milestone 3 — OBS layer

All libobs/UI work happens on the OBS main thread; long SOAP dispatches are marshaled to the worker and results come back by value.

> **Status (2026-08):** M3b (libobs glue, OBS 32.2.1, Windows x64) implemented and CI-green. Lives in `obs/`:
> `glue.{h,cpp}` wires the module (ABI config via `obs_onvif_abi_init`, frontend event dispatch, hotkey + scene→preset registration);
> `scene_presets.{h,cpp}` fires the bound camera preset on `OBS_FRONTEND_EVENT_SCENE_CHANGED` (dispatched to a worker thread, never the UI thread);
> `hotkeys.{h,cpp}` registers preset (1–9) and PTZ move/stop hotkeys through the public `obs_hotkey_register_frontend` API (blocks on the worker via templated `FireAsync`);
> `obs_mapping.{h,cpp}` discovers RTSP **Media Sources** (`ffmpeg_source` with `rtsp://` input) across the scene tree;
> `obs_apply.{h,cpp}` owns the module's `registry::ApplyPolicy`, rewrites `input` + splices credentials via `registry::RewriteSourceUrl`, fires the ffmpeg `restart` proc, and turns output STARTED/STOPPED events into `OnOutputsIdle` re-offers on the last output going idle; `OnCameraMoved` is the worker-safe dispatch seam (policy → UI prompt / rewrite on the main thread) that the M2→M3 discovery bridge feeds; `SetStoreContext`/`ReseedApplyState` keep the policy mapping table in sync with the current scene collection;
> `obs-onvif.h` + `abi.{h,cpp}` export the public C ABI (`obs_onvif_get_abi`) and `obs_onvif_abi_init` (the plugin wires Store + wincred providers);
> `apply_prompt.{h,cpp}` shows the Apply/Defer/Ignore dialog (30 s auto-defer countdown, "remember my choice"; resolves through the policy, rewrites applied on the main thread);
> `dock.{h,cpp}` adds the ONVIF Control dock (`obs_frontend_add_dock_by_id` + Tools-menu toggle) with Cameras / Source Mapping / Presets / PTZ / Apply Policy tabs, and reads/writes the per-collection Store.
>
> **OBS 32.2.1 compatibility notes:** the `OBS*AutoRelease` RAII wrappers and `obs_current_module()` are **not** exported by libobs 32.2.1 — the code uses explicit `obs_data_release`/`obs_source_release`, and `obs_module_config_path` (a macro over `obs_current_module()`) is replaced by `obs_frontend_get_current_profile_path()` for the store root. The ffmpeg source's restart proc is `void restart()` via `proc_handler_call`. `obs_frontend_add_dock_by_id` wraps the content widget in its own `OBSDock`.
>
> **Qt on CI:** Qt6 needs no extra provisioning — `buildspec` already pulls pre-built obs-deps Qt6 into `.deps/` and `obs-build.yml`'s configure appends it to `CMAKE_PREFIX_PATH`. M3c turned `ENABLE_QT` on for the `windows-x64` preset; the module then links `Qt6::Core`/`Qt6::Widgets`, sets `AUTOMOC`, and compiles `apply_prompt.cpp`/`dock.cpp` under the `ENABLE_QT` definition.
>
> **M3 status:** done and CI-green. **M3c** shipped the Qt control dock + Apply/Defer/Ignore prompt; **M3f** completed the remaining dock follow-up and the M2→M3 discovery bridge:
> `discovery.{h,cpp}` runs a background loop (multicast Hello/Bye listener bound to the discovery group + periodic heartbeat Probe) that resolves every contact through `GetDeviceInformation` (stable fingerprint) and `GetProfiles`/`GetStreamUri`, keeps a live camera table (seeded from the persisted store, stale cameras marked offline), and reports IP/stream moves through the `onMoved` callback — which the OBS glue marshals onto the main thread via `obs_queue_task` and feeds to `obs_apply::OnCameraMoved` (apply policy + rewrite/prompt). The ABI `get_camera_list` now reads the live discovery snapshot. `ProbeOnce` shares the contact-resolution path for the loopback CI test (`discovery_bridge_live`).
> The dock gained a **PTZ pad** tab (velocity buttons that re-issue `move` while held and `stop` on release, online-camera selector), **scene-preset binding** on the Presets tab (`set_binding`/`clear_binding` for the current scene, fired by `scene_presets` on scene activation), and a **Config** tab with Image / Stream / OSD / Network sub-tabs.
> The Config tab's backend is full-stack: `onvif_client.{h,cpp}` gained encoder-configuration (read/options/set), imaging (settings/options/set), network-interface (get/set), and OSD (get/set/delete) operations over the media/imaging/device/display services; `registry/worker.{h,cpp}` wraps each behind the first-profile resolution; `obs/abi.{h,cpp}` exposes them as appended `obs_cast_abi_t` functions (fixed-buffer single values, array+release lists; `api_version` stays 1). The dock loads all four categories per camera on a worker thread (widget ranges from the cached options, "unsupported" labels for absent services) and one Apply button batches the SOAP sets. `config_live` round-trips the whole backend against the mock.
> **Not built (deferred, marked follow-up):** the full standalone settings dialog (Cameras / Sources / Scenes / Config defaults / Discovery / Log / About).

> **M3 status (final):** **complete and CI-green.** All M3 phases shipped: M3a (obs-free ABI core), M3b (libobs glue + apply-policy dispatch), M3c (Qt control dock + Apply/Defer/Ignore prompt), and M3f (M2→M3 discovery bridge, PTZ pad, scene-preset binding, camera Config panel, lean settings dialog). The dock covers per-camera work (Cameras / Source Mapping / Presets / PTZ / Config / Apply Policy); the lean settings dialog covers global application settings (General config defaults, Discovery toggles, OBS log tail, About). The remaining §6.7 "full standalone settings dialog" was replaced by the lean dialog (the dock already covers the other tabs) and is closed.

### 6.1 Source mapping — `obs/obs_mapping.{h,cpp}`

Enumerate Media Sources, detect RTSP inputs, auto-suggest camera↔source↔profile (high-profile default):

```cpp
static bool IsMediaSource(obs_source_t *s)
{
    if (obs_source_get_type(s) != OBS_SOURCE_TYPE_INPUT) return false;
    return strcmp(obs_source_get_id(s), "ffmpeg_source") == 0;
}

static bool IsRtspInput(obs_source_t *s)
{
    // OBS 32.2.1: no OBS*AutoRelease wrapper — use obs_source_get_settings +
    // explicit obs_data_release (see obs_apply::SourceInputUrl).
    OBSDataAutoRelease d = obs_source_get_settings(s);
    const char *input = obs_data_get_string(d, "input");
    return input && strncmp(input, "rtsp://", 7) == 0;
}

void DiscoverMappings()
{
    struct obs_frontend_source_list list{};
    obs_frontend_get_sources(&list);
    for (size_t i = 0; i < list.num; ++i) {
        auto *s = list.array[i];
        if (!IsMediaSource(s) || !IsRtspInput(s)) continue;
        // parse host:port from input URL, match against known cameras by IP (IP move
        // matches use fingerprint instead); if ambiguous, leave as 'suggestion' state
        // pending user confirmation in the dialog.
    }
    obs_frontend_source_list_free(&list);
}
```

### 6.2 URL rewrite + restart — `obs/obs_apply.{h,cpp}`

Credential splices + OBS 32 `restart` proc (deterministic restart, not relying on `update`-alone):

```cpp
static std::string SpliceOldCreds(const char *oldUrl,
                                  const std::string &newUrl)
{
    std::string old = oldUrl ? oldUrl : "";
    auto scheme = old.find("://");
    if (scheme == std::string::npos) return newUrl;
    auto at   = old.find('@', scheme + 3);
    auto hl   = old.find('/', scheme + 3);
    size_t he = hl == std::string::npos ? old.size() : hl;
    if (at != std::string::npos && at < he) {
        std::string creds = old.substr(scheme + 3, at - scheme - 3);
        auto ns = newUrl.find("://");
        std::string u = newUrl;
        u.insert(ns + 3, creds + "@");
        return u;                     // preserve user:pass@ from old URL
    }
    return newUrl;
}

static void proc_task(void *param) { // runs on UI thread via obs_queue_task
    auto *src = (obs_source_t*)param;
    OBSDataAutoRelease settings = obs_source_get_settings(src);
    const char *newUrl = obs_data_get_string(settings, "input");
    // (settings already contain the rewritten "input" at this point)
    obs_source_update(src, settings);
    obs_proc_handler_t *handler = obs_source_get_proc_handler(src);
    if (handler) {
        obs_proc_t restart = obs_proc_handler_call(handler, "restart");
        if (restart)
            restart(src, nullptr);
    }
}

void ApplyUrlToSource(obs_source_t *src, const std::string &newUrl)
{
    OBSDataAutoRelease settings = obs_source_get_settings(src);
    const char *old = obs_data_get_string(settings, "input");
    std::string url = SpliceOldCreds(old, newUrl);
    obs_data_set_string(settings, "input", url.c_str());
    obs_apply_t *ctx = /* allocated */;
    ctx->src = src; ctx->settings = obs_data_get_ref?; // keep refs until task runs
    obs_queue_task(OBS_TASK_UI, proc_task, ctx, false);
}
```

### 6.3 Output-prompt — `obs/obs_apply` + `registry/apply`

`registry::apply` raises the incident state; `obs/obs_apply` shows the Qt dialog (Apply now / Defer / Ignore + "remember my choice"), starts the 30 s staleness timer, and auto-defer→re-offer path is driven by `OBS_FRONTEND_EVENT_STREAMING_STARTED/STOPPED` and `RECORDING_*` events.

### 6.4 Scene→preset — `obs/scene_presets.{h,cpp}`

```cpp
static void OnFrontendEvent(enum obs_frontend_event event, void *priv)
{
    if (event == OBS_FRONTEND_EVENT_SCENE_CHANGED) {
        OBSSourceAutoRelease scene = obs_frontend_get_current_scene();
        if (!scene) return;
        const char *sceneName = obs_source_get_name(scene);
        // look up ScenePreset{collectionUuid, sceneName} in the per-collection store
        auto binding = BindingForCurrentScene(sceneName);   // returns {camera, token}
        if (binding) {
            // NEVER block the UI thread: post "goto preset" to the worker
            Worker::Post([b = *binding] {
                CameraSession s = Sessions::ForCamera(b.cameraId);
                s.GotoPreset(b.presetToken);
            });
        }
    }
}
```

The scene→preset dialog binds either an existing preset or calls `SetPreset(name)` first to capture the camera’s current position.

### 6.5 Hotkeys — `obs/hotkeys.{h,cpp}`

Implemented with the public `obs_hotkey_register_frontend` API only — no pre-binding and no `obs-internal.h`. Users assign keys in OBS Settings → Hotkeys; this avoids coupling to private OBS internals that can shift across minor release bumps. PTZ blocking calls run on a detached worker via a templated `FireAsync` (bare function pointers could not capture):

```cpp
// hotkeys.cpp — public API only (matches the shipped implementation)
void RegisterPresetHotkeys()
{
    for (int i = 1; i <= 9; ++i) {
        std::string id = "obs-onvif.preset_" + std::to_string(i);
        obs_hotkey_register_frontend(
            id.c_str(), ("ONVIF Preset " + std::to_string(i)).c_str(),
            OnPresetHotkey, (void*)(intptr_t)i);
    }
}

void OnPresetHotkey(void *data, obs_hotkey_id id, obs_hotkey_t *key, bool pressed)
{
    if (pressed) { int n = (int)(intptr_t)data; /* goto preset n on active camera */ }
}
```

Move hotkeys (W/A/S/D + spheres) are registered identically. **M4:** move/stop hotkeys (and the dock pad) route through `registry::PtzController` — single in-flight, latest-wins coalescing, Stop purge+priority, min interval — instead of fire-and-forget ABI calls (§6.8). **Done:** the ABI `move`/`stop` enqueue on the controller (non-blocking), the dock PTZ pad fires press→`move` / release→`stop` (no 300 ms re-fire loop), and move hotkeys call the ABI directly.

### 6.6 Public plugin ABI — `obs/obs-onvif.h` + `obs/abi.{h,cpp}`

The ABI is the seam for Advanced Scene Switcher and other consumers. Exported as a C symbol so a consumer can `obs_get_module_symbol(obs_get_module("obs-onvif"), "obs_onvif_get_abi")` — `get_sym_addr`-compatible.

```c
// obs-onvif.h  (installed; part of the shipped zip)
#pragma once
#ifdef __cplusplus
extern "C" {
#endif

typedef struct obs_cast_camera_info_s {
    const char *camera_id;
    const char *name;
    const char *xaddr;
    int  online;
} obs_cast_camera_info_t;

typedef struct obs_cast_abi_s {
    int api_version;   // 1

    // cameras
    int  (*get_camera_list)(obs_cast_camera_info_t **out, int *count);
    void (*release_camera_list)(obs_cast_camera_info_t *out);

    // PTZ (name-or-id addressing)
    int  (*move)(const char *cam, double pan, double tilt, double zoom);
    int  (*stop)(const char *cam);

    // presets
    int  (*goto_preset)(const char *cam, const char *preset_token);
    int  (*save_preset)(const char *cam, const char *name,
                        char *token_out, size_t token_cap);
    int  (*list_presets)(const char *cam,
                         const char **names[], const char **tokens[], int *count);
    void (*release_presets)(const char **names, const char **tokens, int count);
    int  (*rename_preset)(const char *cam, const char *preset_token,
                          const char *new_name);
    int  (*delete_preset)(const char *cam, const char *preset_token);
    int  (*get_current_preset)(const char *cam, char *token_out, size_t cap);

    // scene→preset bindings (per current scene collection), read+write
    int  (*get_bindings)(const char **scenes[], const char **cameras[],
                         const char **tokens[], int *count);
    void (*release_bindings)(const char **scenes, const char **cameras,
                             const char **tokens, int count);
    int  (*set_binding)(const char *scene_name, const char *cam,
                        const char *preset_token);
    int  (*clear_binding)(const char *scene_name);
} obs_cast_abi_t;

OBS_ONVIF_API obs_cast_abi_t *obs_onvif_get_abi(void);
#ifdef __cplusplus
}
#endif
```

`abi.cpp` implements each function by synchronously dispatching to the session worker and returning a small status enum (`0 = ok, -1 = not found, -2 = offline, -3 = SOAP error`). Long ops never block the caller beyond the session timeout.

**M3 acceptance:** build+smoke in CI; `tests/aclink/` is a tiny C consumer that walks the real plugin binary via the symbol and drives `get_camera_list`/`goto_preset` against the mock server; scene-prebuilt hotkey prebinding; dock + dialog compile against Qt6.

### 6.7 Dock + settings dialog — `obs/dock.{h,cpp}`, `obs/settings_dialog.{h,cpp}`

Qt6 widgets built under `obs/`. Dock = camera dropdown + LED, PTZ pad (velocity buttons, stop-on-release), zoom/focus, preset table, "Set preset for current scene", Config panel with Image/Stream/OSD/Network tabs (all widget ranges from cached `GetOptions`; single Apply button batches the SOAP set). Settings dialog tabs: Cameras / Sources / Scenes / Config defaults / Discovery / Log / About.

> **M3c/M3f implemented:** the control dock currently ships six tabs — **Cameras** (name / online / XAddr snapshot), **Source Mapping** (RTSP Media Sources in the scene tree ↔ camera assignment + auto-apply, persisted through `Store.SaveCollection` per collection and re-seeded into the apply policy), **Presets** (list / save / go-to / rename / delete on worker threads + "set preset for current scene" via `set_binding`/`clear_binding`), **PTZ** (velocity pad that fires `move` on press and `stop` on release through the PtzController, online-camera selector), **Config** (Image brightness/saturation/contrast/sharpness, Stream resolution/fps/bitrate from cached options, Network DHCP or static IPv4, OSD text overlay; one Apply batches the SOAP sets on a worker thread, with "unsupported" labels when a service is absent), and **Apply Policy** (default policy persisted to `config.json`, remembered per-camera overrides replayed). A **Settings** dialog (General / Discovery / PTZ / Log / About) is reachable from the Apply Policy tab; its General+Discovery tabs edit the persisted `AppConfig` (default stream quality, apply-prompt timeout, discovery interval, SOAP/probe timeouts, Hello-listener toggle), the **PTZ tab** edits the five M4 transport/motor-control knobs and pushes them to the worker + controller via `abi::ApplyAppConfig`, the Log tab shows the OBS session-log tail, and About shows version + config paths. **M4 done:** the PTZ pad drops its 300 ms re-fire loop for a single press→`move` / release→`stop` routed through `registry::PtzController` (§6.8), and the Settings dialog gained the PTZ tab.

---

### 6.8 PTZ command transport & motor control — `onvif/soap_client.{h,cpp}`, `registry/worker.{h,cpp}`, `registry/ptz_controller.{h,cpp}`, `obs/{dock,hotkeys,settings_dialog}.{h,cpp}` (M4)

> **Status (2026-08):** implemented and CI-green. `SoapPool` (WinHTTP connection pool keyed by scheme://host:port, keep-alive reuse, one stale-connection retry, per-service auth-mode cache) is shared per camera by the `Worker`; the profile/service cache makes a warm move cost exactly 1 HTTP request; `registry::PtzController` owns single-threaded PTZ dispatch (queue depth 1, latest-wins, Stop purge+priority, min interval, immediate-abort via `AbortHandle`, bounded re-fire); ABI `move`/`stop`, the dock PTZ pad (press→move / release→stop) and move hotkeys all route through it; Settings → PTZ exposes the five knobs. `ptz_controller` (unit) + `ptz_latency_live` (mock, HTTP/1.1 keep-alive with request/connection counters and an in-flight move delay) assert the §6.8 invariants.

Background and governing decisions in PLAN.md §PTZ command transport & motor control. Implements the mitigations for the **PTZ move/stop path only**:

- **Connection pool + keep-alive** (`soap_client`): cache `WinHttpConnect` handles keyed by `(scheme, host, port)` and reuse them across SOAP calls (`Connection: keep-alive`, HTTP/1.1); closed on `Shutdown`. Gated by `AppConfig::soap_keepalive` (off ⇒ per-call connection, today's behavior). The response-read loop must handle HTTP/1.1 chunked/keep-alive semantics. **Stale-connection recovery:** low-end cameras drop idle pooled connections, so a send/receive failure on a pooled connection (WinHTTP error, WSAECONNRESET) must close that connection and **retry the request once on a fresh connection** before surfacing a transport error.
- **Auth-mode cache** (`soap_client`): per-camera `{wsse, basic}` remembered from the first successful exchange; subsequent requests send the accepted mode first (no 401→retry on Basic-only cameras). Gated by `AppConfig::ptz_auth_cache`. WS-Security tokens remain freshly generated per request (client nonce+created — no reusable server nonce).
- **Profile/service cache** (`worker`): cache the camera's first `MediaProfile` (video-source/encoder/PTZ tokens) + resolved service URLs after `GetCapabilities`; `FirstProfile*`/config ops read the cache and refresh on SOAP error or TTL. Removes the 2 RTTs + 2 connections per command today's re-resolution costs. **Media2 fallback safety:** profile/stream-URI resolution is classic-Media-first today and stays v1-first even once Media2 is used; if a Media2 request faults or returns an incomplete stream URI for a profile, that profile resolves through classic Media and the cache must not treat the Media2 fault as a hard failure (PLAN.md §Profile-selection rule).
- **Template bodies + void-op skip** (`onvif_client`): `ContinuousMove`/`Stop` bodies built by `snprintf`-style fixed-buffer injection (velocity values only); void-op responses skip TinyXML2 body parsing (transport/fault handled by the HTTP layer).
- **`registry::PtzController`** (OBS-free, unit-testable): a single worker thread owns PTZ SOAP dispatch —
  - queue depth 1; a move arriving while one is in-flight only updates `pending_vector` (latest wins, intermediates discarded);
  - `Stop` purges `pending_vector` and dispatches before any queued move;
  - hard minimum interval (`ptz_min_interval_ms`) between dispatches;
  - `ptz_move_timeout_s`: 0 ⇒ `ContinuousMove` without `Timeout` (until Stop); >0 ⇒ bounded timeout with throttled re-fire while a control is held;
  - `ptz_stop_mode`: `immediate` (default) aborts the in-flight WinHTTP request and dispatches `Stop` on a fresh connection; `queued` waits for the current in-flight then dispatches `Stop`.
  The ABI `move`/`stop`, the dock pad, and the move hotkeys all route through it.
- **Dock/hotkey rework**: `PtzWidget` fires `controller.Move` on press and `controller.Stop` on release (no 300 ms loop); move hotkeys call the controller (coalesced).
- **Settings → PTZ tab**: the five user-facing knobs (`soap_keepalive`, `ptz_auth_cache`, `ptz_move_timeout_s`, `ptz_stop_mode`, `ptz_min_interval_ms`) persisted via `Store::SaveAppConfig`.
- **Mock + CI** (`ptz_latency_live`): upgrade `mock_onvif_server.py` to HTTP/1.1 keep-alive (parse `Content-Length`, keep the connection open; the handler currently forces HTTP/1.0 connection-close) and add a per-connection request counter. Assertions:
  1. a `move` costs exactly **1 HTTP request** after first contact (profile+service cached, no 401 retry on an auth-cached camera);
  2. **≥2 requests reuse one connection** when keep-alive is on;
  3. with a move artificially delayed in-flight, `Stop` dispatches within ~1 RTT of release (`immediate` mode);
  4. the mock never observes **overlapping movement requests** (queue depth ≤ 1).

## 7. Milestone 4 — hardening + packaging

- Day-night/BLC quirks cross-check against Hikvision + Dahua fixtures.
- DHCP-sack: Hello-listener reliability at scale (socket reuse, TTL bookkeeping) under repeated adds.
- ABI consumer field test: a small real consumer (or a staging build of Advanced Scene Switcher behavior) covering move/stop, full preset lifecycle, and scene-binding set/clear.
- Packaging: zip artifacts with `data/locale/en-US.ini`, ABI header, README, user guide; publish to OBS forum resources.
- GPLv3: add the required license text + attribution for ODM (GPLv2) and any vendored MIT headers (nlohmann, TinyXML2 zlib).
- **PTZ transport mitigations** (§6.8): WinHTTP connection pool + keep-alive, auth-mode cache, profile/service cache, `snprintf` template bodies, void-op response skip — user-facing knobs in Settings → PTZ. **Done:** implemented in `SoapPool`/`SoapClient`/`OnvifClient`/`Worker`; Settings → PTZ exposes the knobs and pushes them via `abi::ApplyAppConfig`.
- **PTZ motor control** (§6.8): `registry::PtzController` single-in-flight + latest-wins coalescing + Stop purge/priority + min interval; state-driven press→move / release→stop in dock + hotkeys; `ptz_stop_mode` immediate-abort vs queued. **Done:** `registry/ptz_controller.{h,cpp}` implements all of it; ABI `move`/`stop`, dock pad, and move hotkeys route through it.
- **PTZ latency/overshoot CI** (§6.8): HTTP/1.1 keep-alive mock + request counting; `ptz_latency_live` asserts 1-request moves, connection reuse, prompt Stop, and queue depth ≤ 1. **Done:** mock upgraded to HTTP/1.1 keep-alive with request/connection counters and an in-flight move delay; `ptz_controller` (unit) + `ptz_latency_live` (mock) are CI-green.
- **Manual latency/overshoot field check**: on Hikvision + Dahua over a throttled/WAN link, verify command-to-motion latency and absence of overshoot after Stop. **Remaining (needs real hardware).**

---

## 8. Schema-conformance CI

Mirror current onvif.org XSD/WSDL into `third_party/onvif-schemas/` via `tools/fetch_onvif_schemas.py`:

| Set (from onvif.org) | Files |
|---|---|
| `wsdl/devicemgmt.wsdl` + `xsd` | device service · network · ntp |
| `wsdl/media.wsdl` + `xsd` | profile/stream/encoder |
| `wsdl/media2.wsdl` + `xsd` | Media2Profile/Stream/Encoder2 |
| `wsdl/ptz.wsdl` + `xml` | PTZ/Presets |
| `wsdl/imaging.wsdl` | ImagingSettings |
| `wsdl/display.wsdl` | OSD/text |
| `wsdl/ws-discovery` | xaddr resolution (schemas only) |

Every generated envelope is validated against the XSD in CI, using **python `lxml`** (which is the `libxml2` binding, satisfying PLAN.md §Q11):

```python
#!/usr/bin/env python3
# tools/validate_envelopes.py
import sys, glob, json
from lxml import etree

schema_dir = sys.argv[sys.argv.index("--schema-dir") + 1]
points     = sys.argv[sys.argv.index("--envelopes") + 1]  # JSON manifest

fail = 0
for entry in json.load(open(points)):
    env  = etree.parse(entry["envelope"])
    xsd  = etree.XMLSchema(etree.parse(entry["schema"]))
    if not xsd.validate(env):
        fail += 1
        print(f"SCHEMA FAIL {entry['envelope']}: {xsd.error_log.last_error}")
sys.exit(fail)
```

Envelopes come from a `btest` phase that runs every typed call against the mock server and dumps request bodies for cross-validation. **This lane fails the build on any schema drift** — that’s the follow-recent-to-compat safety net for ONVIF releases.

---

## 9. Mock ONVIF server

`tests/mock_onvif_server.py` — stdlib `http.server` + `socket`/`threading`; no third-party deps to keep bootstrapping trivial.

```python
#!/usr/bin/env python3
# tests/mock_onvif_server.py  (fragment)
import http.server, socketserver, threading, socket, struct, urllib.parse

class OnvifHandler(http.server.BaseHTTPRequestHandler):
    def do_POST(self):
        n = int(self.headers.get("Content-Length", 0))
        body = self.rfile.read(n).decode()
        if "GetDeviceInformation" in body:
            xml = INFO_RESPONSE                 # manufacturer/model/fw/serial/hw
        elif "GetCapabilities" in body:
            xml = CAPS_RESPONSE                 # device/media/media2/ptz/imaging/display
        elif "GetProfiles" in body:
            xml = PROFILES_RESPONSE             # incl. media2 variant when requested
        elif "GetStreamUri" in body:
            xml = STREAMURI_RESPONSE            # rtsp://host:port/Streaming/Channels/101
        elif "GotoPreset" in body:
            xml = GOTO_RESPONSE
        else:
            xml = FAULT_RESPONSE
        self.send_response(200); self.send_header("Content-Type",'text/xml')
        self.send_header("Content-Length", str(len(xml))); self.end_headers()
        self.wfile.write(xml.encode())

config = {
  "flavor": "hikvision" | "dahua" | "media2" | "basic_auth"       # selectors
  "https": True, "change_ip_after": None, "use_media2": False,
}
```

Scenarios the mock must emulate for CI:

- **Hikvision-grade** identity/PTZ quirks (e.g., no `GetVideoEncoderConfigurationOptions` in some paths), digest auth only.
- **Dahua-grade** + Basic-only auth → exercises digest→basic fallback.
- **IP change**: mock observes `change_ip_after` seconds, then re-binds on a fresh loopback host, sends a `Hello` on the discovery group; the harness asserts the registry rewrote the source URL.
- **Media2**: `GetProfiles` returns `Media2Profile` tokens ideally; `GetStreamUri`/`SetVideoEncoderConfiguration2` paths.
- **Imaging disabled**: no imaging ops → UI must render zero Image widgets (drives graceful-degradation path).
- **HTTPS**: `--https` mode serves TLS via stdlib `ssl` with a CI-generated self-signed cert (`tools/make_selfsigned_cert.py`).

---

## 10. Risk register

| Risk | Impact | Mitigation |
|---|---|---|
| OBS releases after 32.x change `restart` proc or hotkey internals | Feed recovery / hotkeys break | Watch-list item; pin CI to 32.x; re-verify on each OBS minor bump |
| Media2 schema variance across models | Stream/encoder UI gaps on some cameras | Classic media fallback is v1 behavior; media2 only preferred when clean |
| Firmware that misreports `GetCapabilities` | Missing tabs / wrong XAddr | All capability checks are optional; UI degrades per-capability |
| WinHTTP vs gSOAP behavior on exotic firmware | Digest/Basic fallback paths differ | Kept in ODM’s proven two-pass shape; mock covers the split |
| Multicast blocked by VLAN / AP isolation | No discovery | Manual IP+port+creds add-IP fallback (see PLAN.md scope) |
| Large schema churn on onvif.org | CI red on schema lane | Lane is advisory-orange (blocking only on genuine drift, see §8) |

---

*Keep the two plan documents in sync — `PLAN.md` governs decisions; `IMPLEMENTATION_PLAN.md` governs the how.*