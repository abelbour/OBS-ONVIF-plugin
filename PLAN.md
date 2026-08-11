# OBS ONVIF Plugin — Plan

## 1. Vision

A Windows-native OBS Studio plugin that:

1. **Discovers** ONVIF cameras on the LAN (WS-Discovery multicast + manual IP entry) and shows them in an OBS dock with online status.
2. **Controls** cameras: PTZ pan/tilt/zoom, zoom, presets (save/recall/rename), from a dockable Qt panel.
3. **Auto-repairs video sources**: when a camera's IP changes (DHCP), the plugin detects the camera by its stable hardware identity and **rewrites the `input` URL of the existing OBS **Media Source** associated with it**, then restarts the source so the feed recovers.
4. **Scene-to-preset actions**: assign a camera preset to an OBS scene; switching that scene fires `GotoPreset`.
5. **Capability-aware image settings**: probes each camera's ONVIF capabilities, then exposes *only the controls that camera actually supports* — exposure, gain, iris, white balance, focus, day/night, and similar imaging parameters — editable from the dock.

## Scope decisions (confirmed)

- Architecture: **manage existing standard OBS "Media Source" (ffmpeg_source)** — no custom source type. Keeps scenes/sources portable.
- ONVIF stack: **C++ client ported from the ODM source** (see "ODM as the ONVIF reference" below) — WS-Discovery + SOAP/HTTP(S) + WS-Security digest. No gSOAP generated code, no .NET/WCF. **ODM is the behavior/session reference** (service operations, discovery semantics, session shape); **current ONVIF schemas are the envelope authority**. ODM's protocol logic is ported to C++; **plugin is GPLv3** (ODM is GPLv2-compatible with v3; OBS is GPLv2+).
- UI: **native Qt widgets (Qt6)** — dock + Tools-menu settings dialog.
- Platform: **Windows only**, targeting **current OBS stable (32.x / Qt6)** via latest obs-plugintemplate (**vendored copy**, not submodule). Dev via **local source editing; builds via GitHub Actions** (no local compiler initially).
- Network: **single LAN subnet only** — multicast WS-Discovery is the discovery mechanism (**version-tolerant**: probe in v1/April-2005 form for broad compatibility; accept v1 and v1.1 responses; probes both `NetworkVideoTransmitter` and `Device` types). Manual add-by-IP as a fallback form (IP + ONVIF port + creds, with a "Test connection" validation) when a vendor hides multicast. No multi-VLAN/IP-range scanning, no WAN/VPN discovery. Discovery heartbeat + Hello listener run **always-on from plugin load**, even with the dock closed (reliable IP-change detection while streaming/live).
- Fleet scale: **1–4 cameras** typical — serial-ish heartbeat polling is fine; no concurrency tuning needed.
- Credentials: **shared default + per-camera override**; stored in the **Windows Credential Vault via wincred `CredWrite`/`CredRead`** (reverses the earlier "not DPAPI" note; strongest option within reach, OBS already uses wincred paths for other features).
- Source mapping: **auto-suggest by parsing the Media Source RTSP URL host/IP → camera, with user confirmation**. Multiple sources pointing at one camera are each mapped and tracked separately (main/sub streams across scenes).
- Live-output policy: **when a camera is live during stream/record and its IP moves → ask the user** (dialog: Apply now / Defer / Ignore, with a "remember my choice" option). When no output is active, apply automatically without prompting.
- PTZ inputs: **hotkeys + keyboard/focus buttons**. No joystick inside our plugin — external control (joysticks, other plugins) is exposed via a **public plugin ABI**: `obs-onvif.h` exporting a function-pointer struct, `get_sym_addr`-compatible, addressable by camera ID or name. It exposes **PTZ moves** (`move/stop`), the **full preset set** (`goto_preset`, `save_preset` (capture-as-new), `list_presets`, `rename_preset`, `delete_preset`, `get_current_preset`), and **scene→preset bindings read+write** (`get_bindings`, `set_binding`, `clear_binding`). We export the ABI only; never link to other plugins (Advanced Scene Switcher can drive PTZ and manage scene-preset bindings from macros).
- ONVIF services in scope (v1): **Device** (identity, capabilities, network config), **Media + Media2** (profiles, stream URIs, encoder config), **PTZ** (moves, presets, home), **Imaging** (image panel), **Display** (text OSD: enable + overlay content). **Profile-selection rule:** prefer **Media2** (`Media2Profile`) when a camera exposes one; fall back to classic Media (`Profile`). Media2 is only "preferred when clean": hybrid firmware sometimes advertises a Media2 endpoint but faults or returns incomplete stream URIs for specific profiles — such a profile resolves through classic Media, and a Media2 fault is **never** a hard failure. StreamUri/encoder config for a given source always comes from the same service that owns its profile token. Explicitly **skipped**: Event, Receiver, Recording/Replay/Search, DeviceIO, Analytics/AnalyticsDevice/ActionEngine, Display metadata/PiP.
- Imaging panel v1: render **everything `GetOptions` reports**, plus day/night + backlight where exposed, plus **video encoder settings** (bitrate/resolution/framerate/quality via Media service) — full camera-config panel. Edits are applied via an **explicit Apply button** (batches one SetImagingSettings/SetEncoderConfig), never auto-sent per widget.
- Config panel beyond image/stream: **Network tab** (full IPv4/DHCP + subnet/gateway + DNS + NTP + hostname via Device service) and **OSD tab** (date/time + text overlays via Display service), batched into one `SetNetworkInterface/SetNTP/SetHostname` (or `SetSystemDateAndTime`) + `SetOSDOptions` Apply. **Camera-destabilizing warning:** changing IP/gateway can drop the feed mid-use — the Apply confirmation must warn, and the auto-repair flow (identity-based re-discovery) is what brings it back.
- Transport: **HTTPS supported via Win32 Schannel** (bundled OS, no OpenSSL dep), TLS 1.2+; certificate validation off-by-default with a per-camera warning toggle (cameras' self-signed certs standard). HTTP remains the default; XAddr scheme from `GetCapabilities` is honored.
- Auth mode: **auto-fallback per camera** — try WS-Security digest first; on a 401/fault, retry the same call once over HTTP Basic; the successful mode is remembered per camera in the registry. (Older OEM firmware that only speaks Basic is handled with zero config.)
- Timeouts (defaults, overridable in settings): **SOAP call 5 s** (retry once for media ops), **WS-Discovery probe accept window 3 s**, **live-output prompt 30 s**.
- Scene→preset: **fire on scene activate** only (no transition timing, no multi-preset sequences, no preview-state triggers). Binding supports picking an existing preset **or** capturing the camera's current position as a new preset (`SetPreset`) at bind time.
- Live-output prompt staleness: if the Apply/Defer/Ignore prompt is unanswered for **30 s**, auto-defer; the rewrite is re-offered when the output becomes inactive (auto-applies then).
- Reconnect behavior: **leave the camera alone** — after an IP change / power cycle we only repair the source URL; we never push settings or move the camera by ourselves.
- Error surfacing: **dock status (online/offline LED + per-camera error line) and OBS log only** — no popups, except the live-output prompt.
- Mapping storage: **per scene collection, keyed by a stable internal UUID** assigned at first run for each collection — the UUID survives collection renames, resolved by display name (`SCENE_COLLECTION_CHANGED`); on collection delete, stale keys are dropped with a log warning.
- Stream default: when auto-pairing, pick the **highest-resolution profile (main)**; user can override per source.
- URL auth: when rewriting an RTSP URL whose old value embedded `user:pass@`, **preserve those credentials** in the rewritten URL if `GetStreamUri` returns one without them.
- Hotkeys: **pre-bind presets 1–9** (Ctrl+Alt+1..9); user can rebind in OBS → Settings → Hotkeys.
- Distribution: **public repo, GPLv3, zip-only artifacts**, published to the OBS forum resources. **en-US only** locale (structure ready for later translation).
- Manual HW support: Hikvision, Dahua/OEM, and non-PT/2D fixed devices. UI degrades gracefully when a camera reports no imaging/PTZ capabilities.

## Non-goals (MVP)

- macOS/Linux builds, custom source type, event/alarm subscriptions, recording uploading, HTTPS/Onvif conformance (Profile G/T), multi-VLAN/remote discovery, querying analytics metadata, PTZ "auto tracking"/follow subject, Display metadata/PiP configuration, remote firmware upgrade or config backup/restore.

---

## High-level architecture

```
┌─────────────────────────── OBS UI thread (Qt) ───────────────────────────┐
│  MainWindow (OBS)                                                        │
│    ├─ Dock: ONVIF Control (camera list, PTZ pad, zoom, presets,          │
│    │                     Image settings pane)                            │
│    └─ Menu ── Tools ── ONVIF Settings dialog (cameras, discovery,         │
│                      source mapping, scene→preset, general)              │
└─────────────────────────── libobs / obs-frontend-api ────────────────────┘
              │                    │                │
   source scan / apply        scene events       hotkeys (opt)
              │                    │                │
┌──────────────▼───────────────────▼────────────────▼───────────────┐
│                      Camera Registry (state)                       │
│  stable identity │ stored XAddr │ Online/offline │ credentials    │
│  capabilities + imaging options │ source mapping / profile        │
│  scene→preset map │ discovery config                              │
└──────────────▲──────────────────────────────────────────────▲─────┘
               │ pump/dispatch (background thread)              │ ui push
┌──────────────┴───────────────────────────────────────────────▼──────┐
│ ONVIF Core (no OBS deps)                │  UI models + Qt signals   │
│  • WS-Discovery:                        └────────────────────────────┘
│     - multicast Probe (3702)
│     - ProbeMatch parse+xaddr
│     - continuous Hello/Bye listener
│  • SOAP over HTTP client
│     - WS-Security PasswordDigest
│  • Services: device / media / media2 / ptz / imaging / display
│  • Capabilities + imaging-options parsing
│  • UUID/serial/identity extraction
└─────────────────────────────────────────────────────────────────────┘
```

### ODM as the ONVIF reference

The ONVIF protocol implementation is ported from **ODM (ONVIF Device Manager)** —
read-only GitHub mirror `dxball/ONVIF-Device-Manager`, branch **`v2.2.208`** (canonical
reference; SourceForge fresh-trunk mirrors at `https://sourceforge.net/p/onvifdm/code/HEAD/tree/`
are the same lineage). It is **C#/F#/.NET**, licensed **GPLv2**; we do **not** copy its
.NET layer — we port the F#/C# protocol logic to hand-rolled C++ (keeping the "no gSOAP"
decision). **Role split:** ODM defines *behavior* (which operations, how discovery and
sessions work); *schema* authority is the **current ONVIF spec** (see note 2). Key files:

| ODM file (in `onvif/...`) | What we take from it |
|---|---|
| `onvif.discovery/NvtDiscovery.fs` | WS-Discovery engine: probe for (A1) **both** `NetworkVideoTransmitter` and `Device` contract types, `ProbeMatch` parse + dedup by ListenUris, online/offline node bookkeeping. → our `ws_discovery` |
| `onvif.discovery/DiscoveryLookupAsync.fs` | Async multicast probe/match over UDP. → `ws_discovery` |
| `onvif.session/NvtSession.fs` + `Services/*Async.fs` | Per-camera session: resolve service XAddrs from `GetCapabilities`, typed per-service wrappers. → our `CameraSession` in `onvif_client` (**only** files we need: `DeviceAsync` incl. network, `MediaAsync`/`Media2`, `PtzAsync`, `ImagingAsync`, `DisplayAsync` for OSD text) |
| `onvif.services/OnvifFactory.cs`, `MulticastCapabilitiesBindingElement.cs` | Endpoint/binding patterns; multicast-binding element marks multicast sends so we never re-emit multicast replies (probe-loop guard). → `ws_discovery` socket opts + reply-guard |
| `utils/common/UtfBasicAuthenticationModule.cs` | Basic-auth fallback for cameras that reject digest. → `soap_client` auth modes |
| `odm/odm.ui.views/views/SectionNVT/` (Ptz, LiveVideo, Imaging) | WPF views only — UI inspiration for the dock; **not** ported as-is |

Notes: (1) `PasswordHelper.fs` is **excluded** — it computes a Kipod "password
equivalence" HMAC, *not* the ONVIF UsernameToken digest (ODM delegates that to WCF);
we implement PasswordDigest ourselves per the ONVIF spec. (2) **Schemas:** we parse and
mirror the **current ONVIF** `devicemgmt/media`+`media2`/ptz/imaging/display + `ws-discovery`
XSD/WSDL (from onvif.org), not ODM's stale copies — ODM only guides which operations to
use (no Event/Recording/etc. WSDL since those services are out of scope). (3) Discovery is **version-tolerant**: send WS-Discovery **v1 (April 2005)** style
per ODM for broad compatibility, and accept both v1 and v1.1 headers in responses.
(4) ODM is GPLv2 → plugin is GPLv3 (compatible), decided.
(5) ODM uses WCF's `UdpDiscoveryEndpoint`; we replicate multicast manually (no WCF).

### ONVIF core (Phase 1 — OBS-free, unit-testable)

| File | Responsibility |
|---|---|
| `onvif/ws_discovery.{h,cpp}` | UDP socket on `239.255.255.250:3702` (SO_REUSEADDR + group join), send Probe for **both** `NetworkVideoTransmitter` and `Device` types, parse ProbeMatch/Hello/Bye; returns `DiscoveredDevice { xaddrs[], types[], scopes[], uuid, ttl }`. Version-tolerant (v1 April-2005 send, accept v1+v1.1). Background listener thread feeds `on_device_announced(xaddr)`. |
| `onvif/soap_client.{h,cpp}` | HTTP(S) POST of SOAP envelopes; fault parsing; timeouts; **https via Win32 Schannel** (TLS 1.2+, cert-validation toggle); basic-auth mode fallback. |
| `onvif/ws_security.{h,cpp}` | UsernameToken: nonce(rand)+created(ISO8601 UTC) + digest = Base64( SHA1( Base64Decode(nonce) ‖ created ‖ password ) ); exposes passwordText fallback for legacy devices. |
| `onvif/onvif_client.{h,cpp}` | Typed calls: `GetDeviceInformation`, `GetCapabilities` (device/media/media2/ptz/imaging/display XAddrs), `GetProfiles` (classic) + `GetProfiles` (Media2), `GetStreamUri(profileToken)` (media or media2 per profile token), `ContinuousMove`, `AbsoluteMove/RelativeMove`, `Stop`, `GetPresets/SetPreset/GotoPreset`, `GetNetworkInterfaces` (MAC) + `SetNetworkInterface/SetNTP/SetHostname`, `GetVideoEncoderConfigurations`/`SetVideoEncoderConfiguration` + `GetVideoEncoderConfigurationOptions` (bitrate/resolution/framerate/quality constraints). |
| `onvif/imaging.{h,cpp}` | `GetImagingSettings`/`SetImagingSettings` (exposure mode + exposure time, iris, gain, white-balance mode/color/tint, brightness/sharpness where in imaging scope), `GetOptions` (valid ranges/enums for widget bounds), focus mode + one-touch refocus. |
| `onvif/display.{h,cpp}` | `GetOSDOptions`/`SetOSDOptions` + text overlay enable/content (date/time + custom text) via Display service; also `GetDisplayConfiguration` where needed for the OSD tab. |
| `onvif/capabilities.{h,cpp}` | Transform `GetCapabilities` into a per-camera `Capabilities { deviceXAddr, mediaXAddr, media2XAddr, displayXAddr, ptzXAddr, imagingXAddr, hasPTZ, hasImaging, hasDisplay }` + cached `GetImagingOptions` + `GetVideoEncoderConfigurationOptions` ranges so the UI only renders real controls. |
| `onvif/identity.{h,cpp}` | Stable fingerprint: serialNumber ▶ scopes `MAC` ▶ hardwareId ▶ Endpoint uuid. |
| `onvif/xml.{h,cpp}` / `onvif/sha1.{h,cpp}` / `onvif/base64.{h,cpp}` | TinyXML2 (vendored, zlib), minimal SHA1, Base64 — no external deps. |

### Camera Registry (Phase-2)

- Persistent store (JSON in `%APPDATA%\obs-studio\plugin_config\obs-onvif\`); secrets stored via **wincred `CredWrite`/`CredRead`** (per-camera credential blobs keyed by camera ID; shared-default stored once under `obs-onvif/default`).
- On first successful contact with a camera: fetch `GetCapabilities` + `GetImagingOptions` + `GetVideoEncoderConfigurationOptions` + (where exposed) `GetOSDOptions` + network/interface info, cache per-service availability of profile/PTZ/imaging/display and the widget ranges; refresh on reconnect or user "Re-detect". Store in the registry so the Image/Stream/OSD/Network UI is always capability-driven.
- Discovery loop (background thread):
  1. On-demand Probe (user "Scan") + one probe at startup.
  2. Periodic heartbeat Probe every `discovery_interval` (default 60 s).
  3. Continuous unicast-bound multicast listener for `Hello`/`Bye`.
  4. Ever the discovered device address → `GetDeviceInformation` → fingerprint → match by ID:

```
for each known camera C with fingerprint fp_C:
   new = discover(host) with fp_new
   if fp_new == fp_C:
       if new.xaddr != C.xaddr or new.streamUri != C.lastKnownRTSP[usedProfile]:
           -> IP/move detected
           write C.xaddr = new.xaddr
           refresh profiles + stream URIs
           for each OBS source mapped to C:
             update Media Source "input" (UI thread)
             fire ffmpeg_source "restart"
```

- Live state: online/offline per camera; last-seen timestamps; UI refresh signal.

| File | Responsibility |
|---|---|
| `registry/camera.h` | `Camera` struct (id/name/xaddr/capabilities/lastKnownRTSP/online, mirrors data model). |
| `registry/store.{h,cpp}` | JSON persistence in `%APPDATA%\obs-studio\plugin_config\obs-onvif\`; wincred read/write; per-scene-collection namespacing; migration on collection rename/delete. |
| `registry/registry.{h,cpp}` | In-memory camera state + mapping tables; the discovery loop above; fingerprint matching + IP-change detection; live-state updates. |
| `registry/apply.{h,cpp}` | Applies a rewritten URL to mapped OBS sources (UI-thread dispatch); live-output policy state machine (ask/always/ignore + 30s auto-defer). |

### OBS layer (Phase-3)

- **Source mapping**: enumerate `obs_frontend_get_sources()`; a source is considered a camera source if it is an `ffmpeg_source` (`media source`) whose settings `input` parses as `rtsp://…`. Auto-suggest (and manual override) mapping camera ↔ source ↔ profile, defaulting to the **highest-resolution profile (main)**. Persist mapping + `auto_apply_url` flag, **namespaced per scene collection**.
- **Applying a new URL**:
  1. `obs_source_get_settings(src)`
  2. `obs_data_set_string(settings, "input", newRTSP)` — if the old URL had `user:pass@` and the new one doesn't, splice the old credentials into it.
  3. `obs_source_update(src, settings)` **and always** call the `restart` proc via `obs_source_proc_handler` → recreates the ffmpeg media on the new address (deterministic across OBS 32; not dependent on `update`-alone restarting).
  4. All libobs object/source operations dispatched via the obs UI/main thread (`obs_add_main_thread_callback`), never from the background registry thread.
- **Public plugin ABI** (`obs-onvif.h`): exported struct of function pointers — PTZ `move`, `stop`; presets `goto_preset`, `save_preset`, `list_presets`, `rename_preset`, `delete_preset`, `get_current_preset`; scene-binding `get_bindings`, `set_binding`, `clear_binding`; camera enumeration `get_camera_list` — all addressable by camera ID or name, registered so other modules can `get_sym_addr` it (ADVANCED-SCENE-SWITCHER-compatible). Scene-binding ABI ops use the same per-collection binding store as the built-in scene→preset feature. Thread-safe; long SOAP ops marshaled to our worker.
- **Live-output policy**: when a moved camera is active during streaming/recording, marshal to the UI thread and show the **Apply prompt** (Apply now / Defer / Ignore, with a "remember my choice" option persisting `apply_policy`); unanswered for 30 s → auto-defer, re-offer once output is inactive (then auto-apply). When no output is active, apply automatically with no prompt. **Semantics:** `ask` = default (prompt when live; auto-apply when inactive); `always` = auto-apply immediately even while live (no prompt); `ignore` = never apply or re-offer for this camera. Defer is a **per-incident** action (retry later), not a persisted policy.
- **Scene→preset**: register `obs_frontend_add_event_callback`; on `OBS_FRONTEND_EVENT_SCENE_CHANGED` → scene config → fire `GotoPreset` for the bound camera, on scene-activate only. The scene-preset mapping dialog offers "select existing preset" **and** "capture current position as new preset (`SetPreset`) then bind". Mapping table lives in the per-scene-collection config.
- **Hotkeys**: OBS hotkeys **pre-bound to presets 1–9** (Ctrl+Alt+1..9) plus move keys. No built-in joystick — external joystick/gamepad control goes through the public ABI.
- **Docks/UI**: the dock extrapolates from obs-ptz: camera dropdown + online LED, PTZ pad (velocity buttons, stop-on-release), zoom +/−, focus, preset table (recall, save, rename, clear), "Set preset for current scene" button. Dock also has a **Config** panel: **Image** section (exposure, gain, iris, WB, focus, day-night/BLC where exposed, from `GetOptions`), **Stream** section (bitrate/resolution/framerate/quality via Media service), **OSD** section (date/time + text overlays via Display service), and **Network** section (IPv4/DHCP, gateway, DNS, NTP, hostname via Device service, with a live-feed warning on Apply), widgets generated at runtime from cached capabilities, with an **Apply** button that batches the SOAP set. Settings dialog tabs: Cameras, Sources, Scenes, Config defaults, Discovery, Log/About.

| File | Responsibility |
|---|---|
| `obs/obs_mapping.{h,cpp}` | Enumerate `obs_frontend_get_sources()`, detect rtsp `ffmpeg_source`s, auto-suggest camera↔source↔profile mappings, persist per scene collection keyed by stable UUID; collection rename/delete migration. |
| `obs/obs_apply.{h,cpp}` | Wraps `registry/apply` for the OBS side: edit `input`+splice creds, `obs_source_update` + `restart` proc, output-activity prompt dialog (Apply/Defer/Ignore + remember), 30s staleness timer. |
| `obs/scene_presets.{h,cpp}` | `OBS_FRONTEND_EVENT_SCENE_CHANGED` handler → fire `GotoPreset`; scene→preset mapping dialog (existing preset or capture-as-new via `SetPreset`). |
| `obs/hotkeys.{h,cpp}` | Pre-bound preset hotkeys Ctrl+Alt+1..9 + move keys. |
| `obs/abi.{h,cpp}` | **Public plugin ABI**: `obs-onvif.h` function-pointer struct (PTZ move/stop; presets goto/save/list/rename/delete/get-current; scene-binding get/set/clear; camera list), registration symbol + `get_sym_addr` lookup story; thread-safe dispatch of external calls. |
| `obs/dock.{h,cpp}` | The ONVIF Control dock: camera list + online LED, PTZ pad, zoom/focus, preset table, Config panel (Image/Stream/OSD/Network tabs) with batched Apply + network-change warning. |
| `obs/plugin.{h,cpp}` | Plugin entry: module registration, Tools→ONVIF Settings dialog wiring, main-thread callback bridge, lifecycle. |
| `obs/settings_dialog.{h,cpp}` | The Tools-menu dialog (Cameras / Sources / Scenes / Config defaults / Discovery / Log / About). |

## Concurrency & threading rules

- User+UI and all libobs grabs must happen on the top-level thread.
- ONVIF worker runs independent discovery/session threads; pushes results to UI via Qt signal/slot queued crossings; results passed by value (never shared pointers) to avoid data races.
- UDP socket member of a dedicated discovery gather thread; `Hello` listener socket bound once (SO_REUSEADDR) so it doesn't fight with other apps doing discovery.

## PTZ command transport & motor control (M4)

High command latency on WAN/loaded links and motor overshooting come from
per-command round-trips (fresh TCP/TLS handshake + capability/profile
re-resolution + optional 401→Basic retry per call) and from flooding the
camera's embedded command queue. Mitigations are **default-on and
user-configurable** (Settings → PTZ tab) and apply to the **PTZ move/stop
command path only**:

- **Profile/service caching** (internal, always-on): after first contact, cache
  the camera's first `MediaProfile` (video-source/encoder/PTZ tokens) and the
  resolved device/media/ptz/imaging/display service URLs; refresh on SOAP error
  or TTL. This removes the 2 RTTs + 2 fresh connections per command that
  capability+profile re-resolution currently costs — the dominant latency lever.
- **HTTP keep-alive / connection reuse** (`soap_keepalive`): reuse a WinHTTP
  connection keyed by (scheme, host, port) across SOAP calls instead of a fresh
  TCP/TLS handshake per request. Off ⇒ legacy per-call connection. Low-end
  camera servers routinely drop idle persistent connections, so the transport
  must detect a stale pooled connection (send/receive failure, WSAECONNRESET)
  and **retry the request once on a fresh connection** before surfacing a
  transport error.
- **Auth-mode cache** (`ptz_auth_cache`): WS-Security UsernameToken stays
  per-request (a fresh client nonce+created is mandatory for replay protection —
  WS-Security has no reusable server nonce; HTTP Digest is out of scope). Cache
  which mode a camera accepted (WS-Security vs Basic fallback) so Basic-only
  cameras stop paying a 401→retry round trip on every command.
- **State-driven ContinuousMove** (internal): one `ContinuousMove` on press,
  one `Stop` on release — no per-tick re-fire. `ptz_move_timeout_s` controls
  motion lifetime: `0` omits Timeout (move until Stop), `>0` is a bounded
  timeout with throttled re-fire while held.
- **PTZ command controller** (internal): queue depth 1, latest pending vector
  wins (intermediate vectors discarded), `Stop` purges the queue and dispatches
  ahead of any pending move, and a hard minimum interval (`ptz_min_interval_ms`)
  prevents flooding the camera's request queue.
- **Stop mode** (`ptz_stop_mode`): `immediate` (default) aborts the in-flight
  movement request and dispatches `Stop` on a fresh connection; `queued` purges
  pending and dispatches `Stop` once the current in-flight resolves (~1 RTT
  worst case). "Instant" is bounded by one network RTT either way.
- **Template request bodies + void-op response skip** (internal): PTZ move/stop
  bodies are template-injected into a fixed buffer (no XML DOM generation; the
  code already builds requests by string concat — make it `snprintf`-based);
  void-op responses skip TinyXML2 parsing (transport/fault already handled by
  the HTTP layer).
- **Non-blocking dispatch** (internal): a single controller thread owns all PTZ
  SOAP; the UI thread only records intent and never blocks on the network.

All five user-facing knobs live in `AppConfig` (see Data model) and the
Settings → PTZ tab; profile cache, template bodies, single in-flight and
non-blocking dispatch are internal and always-on.

## Data model

```
Camera {
  id: string                  // fingerprint
  name: string
  username, password          // obfuscated (shared-default + per-camera override)
  xaddr: string               // device service
  capabilities: { hasPTZ, hasImaging, hasDisplay, media/media2/profile tokens,
                  imagingOptions (ranges/enums, cached),
                  encoderConfigs (per profile), osdOptions (cached),
                  networkConfig (ifaces/ip/gateway/dns/ntp/hostname, cached) }
  lastKnownRTSP[profile]      // map<profileToken, rtsp url>
  online: bool, lastSeen
}
// The following are namespaced per scene collection:
//   key = stable internal UUID (collection_uuid), resolved by display name.
SourceMapping { collection_uuid, source_name, camera_id, profileToken, auto_apply: bool }
ScenePreset   { collection_uuid, scene_name, camera_id, preset_token }
CollectionMap { uuid ↔ display_name }
AppConfig {
  discovery_interval_s(60), hello_listener_enabled(always-on),
  soap_timeout_s(5), soap_retry_media(once), discovery_probe_timeout_s(3),
  apply_policy(ask|always|ignore), prompt_timeout_s(30),
  default_stream(high|low),   // high by default
  restore_settings_on_reconnect(false),
  // PTZ transport/motor control (M4) — all default-on:
  soap_keepalive(true),           // reuse WinHTTP connection across SOAP calls
  ptz_auth_cache(true),           // cache negotiated auth mode per camera
  ptz_move_timeout_s(0),          // 0 = omit Timeout (move until Stop)
  ptz_stop_mode("immediate"),     // immediate (abort in-flight) | queued
  ptz_min_interval_ms(75)         // hard minimum between movement requests
}
```

## Build & CI (Phase-0, Windows)

- Repo: `abelbour/OBS-ONVIF-plugin` (public) — already initialized and pushed; Actions can run on it as-is.
- Source scaffold from `obs-plugintemplate` **vendored copy**. Actions `windows-2022` runner.
- `obs-plugintemplate`'s `Build-Windows.ps1` pulls prebuilt OBS deps (versioned) → CMake/VS2022 → plugin zip (layout: `[win-x64]/obs-onvif/bin/64bit/*.dll`, data, locale) installed to `%APPDATA%\obs-studio\plugins`.
- CI on push/PR: build + unit-test the ONVIF core against `mock_onvif_server.py` (a small Python HTTP+UDP mock emulating Hikvision+Dahua identity/PTZ quirks); publish zip artifacts on tags for the **OBS forum resources page**.

### Milestones

| # | Timebox | Deliverable |
|---|---|---|
| 0 | Day 1–2 | Green no-op plugin on CI; installable zip; repo scaffolding; verified deps |
| 1 | ~1 wk | ONVIF core + tests vs mock server + schema-conformance CI (discovery, digest auth, device/media/PTZ/display, capabilities + imaging + encoder, OSD + network) |
| 2 | ~0.5 wk | Registry + persistence + identity matching + URL change detection + apply-policy state |
| 3 | ~1.5 wk | OBS integration: mapping + confirmation, live updates + restart + output prompt, scene → preset, hotkeys, **public plugin ABI (obs-onvif.h)**, dock (PTZ + Config/Image/Stream/OSD/Network) + settings dialog |
| 4 | Rolling | Hardening: day-night/BLC quirks, DHCP-sack, ABI field-testing with a consumer plugin, packaging + README + user guide |

### Dependency graph & task order

Parallelizable work highlighted; CI is the gate for everything (no local compiler).

```
M0 (green CI + vendored template + dep download)
  │
  ├─ P1a  onvif/xml+sha1+base64 ──────────────┐
  ├─ P1b  ws_discovery ───────────────────────┤  all core units developed
  ├─ P1c  ws_security ───────────────────────┤  in ANY order (no deps)
  ├─ P1d  gutter: onvif/xml feeds everything   │
  └─ P1e  soap_client ────────────────────────┤
                                              ▼
  P1f  capabilities (needs soap client) ──► P1g  onvif_client/imaging/display
                                              │       (depend on capabilities)
                                              ▼
  P1h  schema-conformance CI on mock server (validate every envelope vs XSD)
                                              ▼
M2  registry/store (+wincred) ──► registry/registry ──► registry/apply
                                              ▼
M3a  obs/plugin shell (docks+mapping skeleton)
   ├─ M3b  obs_mapping ───────────► obs_apply (restart proc + prompts)
   ├─ M3c  scene_presets (built-in scene→preset)
   ├─ M3d  hotkeys
   ├─ M3e  abi (needs registry+scene_presets; independent of dock graphics)
   └─ M3f  dock + settings_dialog (UI; depends on mapping/settings data)
                                              ▼
M4  hardening + ABI consumer test + packaging + docs
```

**Key ordering rules**
- All libobs-touching code (M3) lands after the core (M1) and registry (M2) are test-
  green **in CI** — the mock server keeps the core seedable long before real HW.
- `obs/abi` is a leaf: it only needs registry + scene-preset stores, so it can be
  built and tested (via a tiny C harness / test consumer) without the dock or dialog.
- CI gates per milestone: M0 = builds+installs ZIP; M1 = ctest green + XSD validation;
  M2 = registry tests vs mock (URL rewrite asserted); M3 = build + smoke with fake
  obs-frontend hooks; M4 = packaging + ABI consumer smoke.
- Timeboxing: M0 is Day 1–2; M1 ~1 wk; M2 ~0.5 wk; M3 ~1.5 wk; M4 rolling. Total
  to a testable v1 ≈ 3.5–4 weeks of effort, CI-lagged not wall-clock-lagged.

## Test plan

- Unit (`gtest`/CTest of core): digest math, XML parse of real probe responses, identity extraction, mapping `GetOptions` ranges → widget bounds for several profiles, encoder config round-trips.
- **Schema conformance (CI, per Q11 decision):** mirror the **current onvif.org** XSD/WSDL set in `third_party/onvif-schemas`; every generated SOAP envelope is validated against the matching XSD (libxml2) in a CTest. No ODM golden-transcript/behavior-oracle step — ODM is used only for reading/porting, not as a live test dependency.
- Mock server (python): Hikvision-grade + Dahua-grade + "IP changed" simulation (server re-binds on a host and sends Hello on the group; assert source state via harness in CI) — CI verifies registry rewrites the URL; imaging `Get/SetImagingSettings` round-trips; an "imaging disabled" camera degrades to no Image widgets; encoder `Set` round-trip; OSD (`Get/SetOSDOptions`) and network (`SetNetworkInterface/SetNTP`) round-trips; **HTTPS XAddr via Schannel**, **digest→basic auto-fallback**, and **Media2-profile stream URI/encoder** paths; server emulates active-output so the Apply prompt path is exercised.
- Manual HW checklist: discovery on the local subnet, PTZ on Hikvision+Dahua, DHCP-leased camera that re-IPs, scene-preset firing, Apply-button image/encoder/OSD/network changes persisted on camera, pre-bound hotkeys 1–9, creds preserved through a URL rewrite, reconnect leaves camera untouched, network-IP change via Config tab recovers via auto-repair, HTTPS-only camera reached and controlled, ABI consumed from an external plugin (e.g. Advanced Scene Switcher) covering move/stop, full preset lifecycle (goto/save/list/rename/delete/get-current), and scene-binding set/clear.

## Open questions for later

- None blocking. Watch-list (parked until the relevant phase, not blocking): Media2 imaging-style variance (classic model used first), OBS releases newer than 32 (re-verify template + restart proc semantics then).