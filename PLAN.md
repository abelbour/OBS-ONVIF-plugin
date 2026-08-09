# OBS ONVIF Plugin — Plan

## 1. Vision

A Windows-native OBS Studio plugin that:

1. **Discovers** ONVIF cameras on the LAN (WS-Discovery multicast + manual IP entry) and shows them in an OBS dock with online status.
2. **Controls** cameras: PTZ pan/tilt/zoom, zoom, presets (save/recall/rename), from a dockable Qt panel.
3. **Auto-repairs video sources**: when a camera's IP changes (DHCP), the plugin detects the camera by its stable hardware identity and **rewrites the `input` URL of the existing OBS **Media Source** associated with it**, then restarts the source so the feed recovers.
4. **Scene-to-preset actions**: assign a camera preset to an OBS scene; switching to that scene fires `GotoPreset`.

## Scope decisions (confirmed)

- Architecture: **manage existing standard OBS "Media Source" (ffmpeg_source)** — no custom source type. Keeps scenes/sources portable.
- ONVIF stack: **minimal hand-rolled C++ client** — WS-Discovery + SOAP/HTTP + WS-Security digest. No gSOAP generated code.
- UI: **native Qt widgets (Qt6)** — dock + Tools-menu settings dialog.
- Platform: **Windows only** (OBS 30–32). Dev via **local source editing; builds via GitHub Actions** (no local compiler initially).
- Test HW to support: Hikvision, Dahua/OEM, and non-PT/2D fixed devices (self-decl sensed as declined-PTZ).

## Non-goals (MVP)

- macOS/Linux builds, custom source type, event/alarm integrations (motion), recording uploading, HTTPS/Onvif Profile G/T, camera image (exposure/IR) controls.

---

## High-level architecture

```
┌─────────────────────────── OBS UI thread (Qt) ───────────────────────────┐
│  MainWindow (OBS)                                                        │
│    ├─ Dock: ONVIF Control (camera list, PTZ pad, zoom, presets)          │
│    └─ Menu ── Tools ── ONVIF Settings dialog (cameras, discovery,         │
│                      source mapping, scene→preset, general)              │
└─────────────────────────── libobs / obs-frontend-api ────────────────────┘
              │                    │                │
   source scan / apply        scene events       hotkeys (opt)
              │                    │                │
┌──────────────▼───────────────────▼────────────────▼───────────────┐
│                      Camera Registry (state)                       │
│  stable identity │ stored XAddr │ Online/offline │ credentials    │
│  source mapping (camera→OBS source + profile)                     │
│  scene→preset map │ discovery config                              │
└──────────────▲──────────────────────────────────────────────▲─────┘
               │ pump/dispatch (background thread)              │ ui push
┌──────────────┴──────────────────┐        ┌───────────────────┴──────┐
│ ONVIF Core (no OBS deps)        │        │ UI models + Qt signals    │
│  • WS-Discovery:                  │        └──────────────────────────┘
│     - multicast Probe (3702)      │
│     - ProbeMatch parse+xaddr        │
│     - continuous Hello/Bye listener │
│  • SOAP over HTTP client             │
│     - WS-Security PasswordDigest     │
│  • Services (mini): device/media/ptz  │
│  • UUID/serial identity extraction    │
└───────────────────────────────────────┘
```

### ONVIF core (Phase 1 — OBS‑free, unit‑testable)

| File | Responsibility |
|---|---|
| `onvif/ws_discovery.{h,cpp}` | UDP socket on `239.255.255.250:3702` (SO_REUSEADDR + group join), send Probe, parse ProbeMatch/Hello/Bye; returns `DiscoveredDevice { xaddrs[], types[], scopes[], uuid, ttl }`. Background listener thread feeds `on_device_announced(xaddr)`. |
| `onvif/soap_client.{h,cpp}` | HTTP POST of SOAP envelopes; fault parsing; timeouts; http (and https if trivial). |
| `onvif/ws_security.{h,cpp}` | UsernameToken: nonce(rand)+created(ISO8601 UTC) + digest = Base64( SHA1( Base64Decode(nonce) ‖ created ‖ password ) ); exposes passwordText fallback for legacy devices. |
| `onvif/onvif_client.{h,cpp}` | Typed calls: `GetDeviceInformation`, `GetCapabilities` (mediaXaddr/ptzXaddr), `GetProfiles`, `GetStreamUri(profileToken)`, `ContinuousMove`, `AbsoluteMove/RelativeMove`, `Stop`, `GetPresets/SetPreset/GotoPreset`, `GetNetworkInterfaces` (MAC). |
| `onvif/identity.{h,cpp}` | Stable fingerprint: serialNumber ▶ scopes `MAC` ▶ hardwareId ▶ Endpoint uuid. |
| `onvif/xml.{h,cpp}` / `onvif/sha1.{h,cpp}` / `onvif/base64.{h,cpp}` | TinyXML2 (vendored, zlib), minimal SHA1, Base64 — no external deps. |

### Camera Registry (Phase-2)

- Persistent store (JSON in `%APPDATA%\obs-studio\plugin_config\obs-onvif\`); secrets obfuscated via OBS config helpers.
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

- **Source mapping**: enumerate `obs_frontend_get_sources()`; a source is considered a camera source if it is an `ffmpeg_source` (`media source`) whose settings `input` parses as `rtsp://…`. Auto-suggest (and manual override) mapping camera ↔ source ↔ profile (main/sub). Persist mapping + `auto_apply_url` flag.
- **Applying a new URL**:
  1. `obs_source_get_settings(src)`
  2. `obs_data_set_string(settings, "input", newRTSP)`
  3. `obs_source_update(src, settings)` **and** call the `restart` proc via `obs_source_proc_handler` → this recreates the ffmpeg media on the new address. (Verify in Phase-3 that `update` alone restarts; use `restart` proc if not.)
  4. All libobs object/source operations dispatched via the obs UI/main thread (`obs_add_main_thread_callback`), never from the background registry thread.
- **Scene→preset**: register `obs_frontend_add_event_callback`; on `OBS_FRONTEND_EVENT_SCENE_CHANGED` → scene config → fire `GotoPreset` for the bound camera. Settings dialog keeps the mapping table.
- **Docks/UI**: the dock extrapolates from obs-ptz: camera dropdown + online LED, PTZ dot pad (velocity buttons, X-autosturn on press/release), zoom +/− , preset table (recall, save, rename, clear), "Set preset for current scene" button. Settings dialog tabs: Cameras, Sources, Scenes/Presets, Discovery, Log/About.

## Concurrency & threading rules

- User+UI and all libobs grabs must happen on the top-level thread.
- ONVIF worker runs independent discovery/session threads; pushes results to UI via Qt signal/slot queued crossings; results passed by value (never shared pointers) to avoid data races.
- UDP socket member of a dedicated discovery gather thread; `Hello` listener socket bound once (SO_REUSEADDR) so it doesn't fight with other apps doing discovery.

## Data model

```
Camera {
  id: string                  // fingerprint
  name: string
  username, password          // obfuscated
  xaddr: string               // device service
  lastKnownRTSP[profile]      // map<profileToken, rtsp url>
  online: bool, lastSeen
}
SourceMapping { source_name, camera_id, profileToken, auto_apply: bool }
ScenePreset { scene_name, camera_id, preset_token }
AppConfig { discovery_interval_s, hello_listener_enabled, apply_policy(always|on_live_switching) }
```

## Build & CI (Phase-0, Windows)

- Repo with `obs-plugintemplate` as base (vendored or submodule). Azure/Actions `windows-2022` runner.
- `obs-plugintemplate`'s `Build-Windows.ps1` pulls prebuilt OBS deps (versioned) → CMake/VS2022 → plugin zip (layout: `[win-x64]/obs-onvif/bin/64bit/*.dll`, data, locale) installed to `%APPDATA%\obs-studio\plugins`.
- CI on push/PR: build + unit-test the ONVIF core against `mock_onvif_server.py` (a small Python HTTP+UDP mock emulating Hikvision+Dahua identity/PLZ quirks), then publish artifacts on tags.
- **Requires a GitHub repo.** Next step: create `obs-onvif` repo, push, enable Actions for status:to validate toolchain.

### Milestones

| # | Timebox | Deliverable |
|---|---|---|
| 0 | Day 1–2 | Green no-op plugin on CI; installable zip; repo scaffolding; CRTDemo of deps |
| 1 | ~1 wk | ONVIF core + tests vs mock server (discovery, digest auth, device/media, PTZ) |
| 2 | ~0.5 wk | Registry + persistence + identity matching + URL change detection |
| 3 | ~1 wk | OBS integration: mapping, live updates+restart, scene→preset, dock + settings dialog |
| 4 | Rolling | Hardening: multi-VLAN, DHCP-sack, Hikvision/Dahua quirks, packaging + README + user guide |

## Test plan

- Unit (`gtest`/CTest of core): digest math, XML parse of real probe responses, identity extraction, debound logic.
- Mock server (python): Hikvision-grade + Dahua-grade + "IP changed" simulation (server re-binds on a host and sends Hello on the group; assert OBS-menu state via harness in CI) — CI verifies registry rewrites the URL.
- Manual HW checklist: discovery from multiple subnets, PTZ on Hikvision+Dahua, DHCP-leased camera that re-IPs, scene-preset firing, streams that remain while rewrites happen vs documented glitch.

## Open questions for later

- Whether `input` change alone restarts ffmpeg_source in OBS 32 → verify against source; fallback to `restart` proc.
- HTTPS ONVIF on devices that require it (mostly optional on Hikvision).
- Which stream profile (main/sub) is auto-associated by default (user-selectable).
- Apply policy default for "active output" scenarios (recommend: apply always, but surfaced in log + a toggle).
```