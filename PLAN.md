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
- ONVIF stack: **minimal hand-rolled C++ client** — WS-Discovery + SOAP/HTTP + WS-Security digest. No gSOAP generated code.
- UI: **native Qt widgets (Qt6)** — dock + Tools-menu settings dialog.
- Platform: **Windows only** (OBS 30–32). Dev via **local source editing; builds via GitHub Actions** (no local compiler initially).
- Network: **single LAN subnet only** — multicast WS-Discovery is the discovery mechanism; manual add-by-XAddr as a fallback when a vendor hides multicast. No multi-VLAN/IP-range scanning, no WAN/VPN discovery.
- Fleet scale: **1–4 cameras** typical — serial-ish heartbeat polling is fine; no concurrency tuning needed.
- Credentials: **shared default + per-camera override**; stored with OBS config obfuscation (acceptable, not DPAPI).
- Source mapping: **auto-suggest by parsing the Media Source RTSP URL host/IP → camera, with user confirmation**. Multiple sources pointing at one camera are each mapped and tracked separately (main/sub streams across scenes).
- Live-output policy: **when a camera is live during stream/record and its IP moves → ask the user** (dialog: Apply now / Defer / Ignore, with a "remember my choice" option). When no output is active, apply automatically without prompting.
- PTZ inputs: **hotkeys + keyboard/focus buttons + joystick/gamepad analog velocity** (Win32/DirectInput, no third-party dep, mirroring obs-ptz's joystick support).
- Imaging panel v1: render **everything `GetOptions` reports**, plus day/night + backlight where exposed, plus **video encoder settings** (bitrate/resolution/framerate/quality via Media service) — full camera-config panel. Edits are applied via an **explicit Apply button** (batches one SetImagingSettings/SetEncoderConfig), never auto-sent per widget.
- Scene→preset: **fire on scene activate** only (no transition timing, no multi-preset sequences, no preview-state triggers).
- Reconnect behavior: **leave the camera alone** — after an IP change / power cycle we only repair the source URL; we never push settings or move the camera by ourselves.
- Mapping storage: **per scene collection** — camera↔source and scene→preset bindings are namespaced by the active scene collection.
- Stream default: when auto-pairing, pick the **highest-resolution profile (main)**; user can override per source.
- URL auth: when rewriting an RTSP URL whose old value embedded `user:pass@`, **preserve those credentials** in the rewritten URL if `GetStreamUri` returns one without them.
- Hotkeys: **pre-bind presets 1–9** (Ctrl+Alt+1..9); user can rebind in OBS → Settings → Hotkeys.
- Distribution: **public repo, zip-only artifacts**, published to the OBS forum resources. **en-US only** locale (structure ready for later translation).
- Manual HW support: Hikvision, Dahua/OEM, and non-PT/2D fixed devices. UI degrades gracefully when a camera reports no imaging/PTZ capabilities.

## Non-goals (MVP)

- macOS/Linux builds, custom source type, event/alarm integrations, recording uploading, HTTPS/Onvif conformance (Profile G/T), multi-VLAN/remote discovery, querying analytics metadata, PTZ "auto tracking"/follow subject.

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
│  • Services: device / media / ptz / imaging
│  • Capabilities + imaging-options parsing
│  • UUID/serial/identity extraction
└─────────────────────────────────────────────────────────────────────┘
```

### ONVIF core (Phase 1 — OBS‑free, unit‑testable)

| File | Responsibility |
|---|---|
| `onvif/ws_discovery.{h,cpp}` | UDP socket on `239.255.255.250:3702` (SO_REUSEADDR + group join), send Probe, parse ProbeMatch/Hello/Bye; returns `DiscoveredDevice { xaddrs[], types[], scopes[], uuid, ttl }`. Background listener thread feeds `on_device_announced(xaddr)`. |
| `onvif/soap_client.{h,cpp}` | HTTP POST of SOAP envelopes; fault parsing; timeouts; http (and https if trivial). |
| `onvif/ws_security.{h,cpp}` | UsernameToken: nonce(rand)+created(ISO8601 UTC) + digest = Base64( SHA1( Base64Decode(nonce) ‖ created ‖ password ) ); exposes passwordText fallback for legacy devices. |
| `onvif/onvif_client.{h,cpp}` | Typed calls: `GetDeviceInformation`, `GetCapabilities` (mediaXaddr/ptzXaddr/imagingXaddr), `GetProfiles`, `GetStreamUri(profileToken)`, `ContinuousMove`, `AbsoluteMove/RelativeMove`, `Stop`, `GetPresets/SetPreset/GotoPreset`, `GetNetworkInterfaces` (MAC), `GetVideoEncoderConfigurations`/`SetVideoEncoderConfiguration` (bitrate/resolution/framerate/quality). |
| `onvif/imaging.{h,cpp}` | `GetImagingSettings`/`SetImagingSettings` (exposure mode + exposure time, iris, gain, white-balance mode/color/tint, brightness/sharpness where in imaging scope), `GetOptions` (valid ranges/enums for widget bounds), focus mode + one-touch refocus. |
| `onvif/capabilities.{h,cpp}` | Transform `GetCapabilities` into a per-camera `Capabilities { deviceXAddr, mediaXAddr, ptzXAddr, imagingXAddr, hasPTZ, hasImaging, hasEvent }` + cached `GetImagingOptions` ranges so the UI only renders real controls. |
| `onvif/identity.{h,cpp}` | Stable fingerprint: serialNumber ▶ scopes `MAC` ▶ hardwareId ▶ Endpoint uuid. |
| `onvif/xml.{h,cpp}` / `onvif/sha1.{h,cpp}` / `onvif/base64.{h,cpp}` | TinyXML2 (vendored, zlib), minimal SHA1, Base64 — no external deps. |

### Camera Registry (Phase-2)

- Persistent store (JSON in `%APPDATA%\obs-studio\plugin_config\obs-onvif\`); secrets obfuscated via OBS config helpers.
- On first successful contact with a camera: fetch `GetCapabilities` + `GetImagingOptions`, cache per service counts of profile/PTZ/imaging availability, and refresh it on reconnect or when the user presses "Re-detect". Store in the registry so image/controls UI is always capability-driven.
- Discovery loop (background thread):
  1. On-demand Probe (user "Scan") + one probe at startup.
  2. Periodic heartbeat Probe every `discovery_interval` (default 60 s).
  3. Continuous unicast-bound multicast listener for `Hello`/`Bye`.
  4. Ever the discovered device address → `GetDeviceInformation` → fingerprint → match by ID:

```
for each known camera C with fingerprint fp_C:
   new = discover(host) with fp_new
   if fp_new == fp_C:
       if new.xaddr != C.xaddr or new.streamUri != C.usedStreamUris:
           -> IP/move detected
           write C.xaddr = new.xaddr
           refresh profiles + stream URIs
           for each OBS source mapped to C:
             update Media Source "input" (UI thread)
             fire ffmpeg_source "restart"
```

- Live state: online/offline per camera; last-seen timestamps; UI refresh signal.

### OBS layer (Phase-3)

- **Source mapping**: enumerate `obs_frontend_get_sources()`; a source is considered a camera source if it is an `ffmpeg_source` (`media source`) whose settings `input` parses as `rtsp://…`. Auto-suggest (and manual override) mapping camera ↔ source ↔ profile, defaulting to the **highest-resolution profile (main)**. Persist mapping + `auto_apply_url` flag, **namespaced per scene collection**.
- **Applying a new URL**:
  1. `obs_source_get_settings(src)`
  2. `obs_data_set_string(settings, "input", newRTSP)` — if the old URL had `user:pass@` and the new one doesn't, splice the old credentials into it.
  3. `obs_source_update(src, settings)` **and** call the `restart` proc via `obs_source_proc_handler` → this recreates the ffmpeg media on the new address. (Verify in Phase-3 that `update` alone restarts; use `restart` proc if not.)
  4. All libobs object/source operations dispatched via the obs UI/main thread (`obs_add_main_thread_callback`), never from the background registry thread.
- **Live-output policy**: when a moved camera is active during streaming/recording, marshal to the UI thread and show the **Apply prompt** (Apply now / Defer / Ignore + "remember my choice", persisted as `apply_policy`); otherwise apply automatically with no prompt.
- **Scene→preset**: register `obs_frontend_add_event_callback`; on `OBS_FRONTEND_EVENT_SCENE_CHANGED` → scene config → fire `GotoPreset` for the bound camera, on scene-activate only. Mapping table lives in the per-scene-collection config.
- **Hotkeys + joystick**: OBS hotkeys **pre-bound to presets 1–9** (Ctrl+Alt+1..9) plus move keys; joystick/gamepad analog velocity via Win32/DirectInput (no third-party dep).
- **Docks/UI**: the dock extrapolates from obs-ptz: camera dropdown + online LED, PTZ pad (velocity buttons, stop-on-release), zoom +/−, focus, preset table (recall, save, rename, clear), "Set preset for current scene" button. Dock also has a **Config** panel: **Image** section (exposure, gain, iris, WB, focus, day-night/BLC where exposed, from `GetOptions`) and **Stream** section (bitrate/resolution/framerate/quality via Media service), widgets generated at runtime from cached capabilities, with an **Apply** button that batches the SOAP set. Settings dialog tabs: Cameras, Sources, Scenes, Config defaults, Discovery, Log/About.

## Concurrency & threading rules

- User+UI and all libobs grabs must happen on the top-level thread.
- ONVIF worker runs independent discovery/session threads; pushes results to UI via Qt signal/slot queued crossings; results passed by value (never shared pointers) to avoid data races.
- UDP socket member of a dedicated discovery gather thread; `Hello` listener socket bound once (SO_REUSEADDR) so it doesn't fight with other apps doing discovery.

## Data model

```
Camera {
  id: string                  // fingerprint
  name: string
  username, password          // obfuscated (shared-default + per-camera override)
  xaddr: string               // device service
  capabilities: { hasPTZ, hasImaging, media/profile tokens,
                  imagingOptions (ranges/enums, cached),
                  encoderConfigs (per profile) }
  lastKnownRTSP[profile]      // map<profileToken, rtsp url>
  online: bool, lastSeen
}
// The following are namespaced per scene collection (a "collection" key):
SourceMapping { collection, source_name, camera_id, profileToken, auto_apply: bool }
ScenePreset   { collection, scene_name, camera_id, preset_token }
AppConfig {
  discovery_interval_s, hello_listener_enabled,
  apply_policy(ask|always|defer),
  default_stream(high|low),   // high by default
  preset_hotkeys_prebound(true), restore_settings_on_reconnect(false)
}
```

## Build & CI (Phase-0, Windows)

- Repo: `abelbour/OBS-ONVIF-plugin` (public) — already initialized and pushed; Actions can run on it as-is.
- Source scaffold from `obs-plugintemplate` (vendored or submodule). Actions `windows-2022` runner.
- `obs-plugintemplate`'s `Build-Windows.ps1` pulls prebuilt OBS deps (versioned) → CMake/VS2022 → plugin zip (layout: `[win-x64]/obs-onvif/bin/64bit/*.dll`, data, locale) installed to `%APPDATA%\obs-studio\plugins`.
- CI on push/PR: build + unit-test the ONVIF core against `mock_onvif_server.py` (a small Python HTTP+UDP mock emulating Hikvision+Dahua identity/PLZ quirks); publish zip artifacts on tags for the **OBS forum resources page**.

### Milestones

| # | Timebox | Deliverable |
|---|---|---|
| 0 | Day 1–2 | Green no-op plugin on CI; installable zip; repo scaffolding; verified deps |
| 1 | ~1 wk | ONVIF core + tests vs mock server (discovery, digest auth, device/media/PTZ, capabilities + imaging + encoder) |
| 2 | ~0.5 wk | Registry + persistence + identity matching + URL change detection + apply-policy state |
| 3 | ~1.5 wk | OBS integration: mapping + confirmation, live updates + restart + output prompt, scene → preset, hotkeys, dock (PTZ + Config/Image/Stream) + settings dialog |
| 4 | Rolling | Hardening: joystick/gamepad, day-night/BLC quirks, DHCP-sack, packaging + README + user guide |

## Test plan

- Unit (`gtest`/CTest of core): digest math, XML parse of real probe responses, identity extraction, mapping `GetOptions` ranges → widget bounds for several profiles, encoder config round-trips.
- Mock server (python): Hikvision-grade + Dahua-grade + "IP changed" simulation (server re-binds on a host and sends Hello on the group; assert source state via harness in CI) — CI verifies registry rewrites the URL; imaging `Get/SetImagingSettings` round-trips; an "imaging disabled" camera degrades to no Image widgets; encoder `Set` round-trip; server emulates active-output so the Apply prompt path is exercised.
- Manual HW checklist: discovery on the local subnet, PTZ on Hikvision+Dahua, DHCP-leased camera that re-IPs, scene-preset firing, Apply-button image/encoder changes persisted on camera, pre-bound hotkeys 1–9, joystick, creds preserved through a URL rewrite, reconnect leaves camera untouched.

## Open questions for later

- Whether `input` change alone restarts ffmpeg_source in OBS 32 → verify against source; fallback to `restart` proc.
- HTTPS ONVIF on devices that require it (mostly optional on Hikvision).
- Imaging schema variance: some cameras omit `GetOptions` or nest exposure under Media2 — MVP uses classic Imaging model only; Media2 parsing is a follow-up.
- Joystick with no axes exposed (button-only gamepads) — map as digital pushes or disable axis movement.
- Scene-collection rename/delete: how migrations handle stale `collection` keys (stale mappings get dropped with a log warning).