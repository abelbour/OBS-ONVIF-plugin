# OBS ONVIF Plugin — User Guide

The **OBS ONVIF Plugin** discovers ONVIF cameras on your LAN, lets you control
PTZ/presets, configure image/stream/OSD/network settings, bind presets to
scenes, and automatically repairs Media Source URLs when a camera's DHCP
address changes.

Requirements: Windows 10+ (x64) and OBS Studio 32.x (Qt6).

---

## 1. Installation

1. Download the `obs-onvif-<version>-windows-x64.zip` release artifact.
2. Extract it so the contents land in OBS's plugin folder:
   `%APPDATA%\obs-studio\plugins\`. The zip contains an `obs-onvif` folder with
   `bin/64bit/obs-onvif.dll` and `data/locale/en-US.ini`.
3. (Re)start OBS. A **Tools → ONVIF Control** menu item appears.

Config data is stored under
`%APPDATA%\obs-studio\plugin_config\obs-onvif\`. Camera passwords are stored in
the Windows Credential Vault, never in plain-text files.

---

## 2. First run — discovery

Open **Tools → ONVIF Control**. The **Cameras** tab shows the live table. The
plugin's WS-Discovery listener runs continuously from startup (even with the
dock closed) — multicast Probes discover cameras automatically on the same
subnet, and a **Refresh** button forces a scan.

- Cameras are shown as **Online** / **Offline**. Offline entries come from the
  persisted registry and are flagged when they stop answering.
- If a camera hides multicast, you can add it manually (planned fallback; until
  then, discoverable ONVIF devices on a single subnet are the supported path).

---

## 3. Source Mapping

The **Source Mapping** tab lists every RTSP **Media Source** (`ffmpeg_source`)
in your scene tree. Assign each source to a camera and enable **Auto-apply**.

- **Auto-apply** is the DHCP IP-change auto-repair: when the plugin detects a
  camera's XAddr/stream URI changed (discovery re-contact, Hello on a new
  address, or a re-bound mock/camera), it rewrites the source's `input` URL —
  splicing the saved credentials back in — and restarts the source.
- The rewrite is governed by the **Apply Policy** (see §7): "Ask", "Always", or
  "Ignore".

---

## 4. PTZ control

**PTZ pad** (PTZ tab): press and hold a direction / zoom button to move, release
to stop. The center **Stop** button halts motion.

**Move hotkeys** (OBS **Settings → Hotkeys**): bind keys to *ONVIF Move* actions
(pan left/right, tilt up/down, zoom in/out, stop). Press = move, release = stop.

**Preset hotkeys**: *ONVIF Preset 1…9* hotkeys recall presets 1–9 on the first
online camera.

### PTZ transport settings (Settings → PTZ)

- **Reuse HTTP connections (keep-alive)** — reuse the camera's HTTP connection
  across SOAP calls instead of a fresh handshake per command (lower latency).
- **Cache negotiated authentication mode** — remember digest vs Basic per
  camera so repeated commands skip the 401 round-trip.
- **Move timeout** — `0` (default) moves until you release the button; a
  positive value bounds each move command and the plugin re-issues it while the
  button is held.
- **Stop mode** — *Immediate* aborts the in-flight move request so the camera
  stops within one round trip; *Queued* lets the current move finish first.
- **Minimum interval between moves** — a hard floor between movement commands
  (protects very weak camera firmware).

---

## 5. Presets and scene bindings

The **Presets** tab lists the selected camera's presets: **Go to**, **Save
current**, **Rename**, **Delete**. **Set preset for current scene** binds the
selected camera+preset to the active scene; **Clear scene binding** removes it.

When you switch to a bound scene, the plugin fires that camera's preset. Binding
is stored per scene collection.

---

## 6. Camera configuration

The **Config** tab (per camera) loads the current values and allowed ranges from
the camera, then applies your edits with one **Apply Changes** button:

- **Image** — brightness / color saturation / contrast / sharpness.
- **Stream** — resolution / frame rate / bitrate of the primary encoder.
- **Network** — DHCP or static IPv4 address (device-service network interface).
- **OSD** — text overlay enabled/disabled and content.

Services the camera does not expose show a "Not supported by this device"
label and their tab is skipped on Apply.

---

## 7. Apply Policy

Controls what happens when a mapped camera changes its address **while a source
is live** (streaming/recording):

- **Ask** — show a dialog: *Apply now*, *Defer* (auto-re-applied when outputs
  go idle, with a countdown), or *Ignore* ("remember my choice" persists a
  per-camera override).
- **Always** — rewrite + restart immediately.
- **Ignore** — never rewrite (logs only).

When no output is active, URL changes are applied automatically without a
prompt.

---

## 8. Settings

**Tools → ONVIF Control → Apply Policy tab → Settings...** opens the dialog:

- **General** — default stream quality (high/low auto-pairing), apply-prompt
  timeout, discovery interval, SOAP timeout.
- **Discovery** — continuous Hello/Bye listener toggle, probe timeout.
- **PTZ** — the transport/motor-control knobs from §4.
- **Log** — tail of the newest OBS session log.
- **About** — version + config paths.

---

## 9. Developer ABI

External OBS modules (e.g. Advanced Scene Switcher) can drive cameras through
the public C ABI. Link against the shipped `obs-onvif.h` and resolve
`obs_onvif_get_abi` via `obs_get_module_symbol(obs_get_module("obs-onvif"),
"obs_onvif_get_abi")`. The ABI covers camera list, PTZ move/stop, the full
preset lifecycle, scene bindings, and camera-config reads/writes. Move/stop are
non-blocking: they enqueue on the plugin's PTZ controller and return
immediately.

---

## 10. Troubleshooting

| Symptom | Likely cause / fix |
|---|---|
| No cameras in the dock | Camera on a different subnet / multicast blocked; check VLAN, firewall, and that the camera has ONVIF enabled. |
| PTZ works in the dock but not hotkeys | Hotkey bindings are assigned in OBS **Settings → Hotkeys** (the plugin registers the actions; it does not pre-bind keys). |
| "Not supported by this device" everywhere | Camera exposes no imaging/media/display services for that tab. |
| Credentials prompt / 401 | Password stored in the Credential Vault; re-add the camera's credentials, or clear the stale vault entry for `obs-onvif/<id>`. |
| Source keeps restarting | IP-change auto-repair is active; check the Apply Policy and the camera's DHCP lease. |

## 11. Limitations

- Single LAN subnet only (multicast discovery); no multi-VLAN scanning.
- Locales: en-US (source) and es-ES provided; OBS UI language is used.
- No Event/Recording/Analytics services — only device, media, PTZ, imaging,
  and display (text OSD).
