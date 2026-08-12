# OBS ONVIF Plugin

Windows-native [OBS Studio](https://obsproject.com) plugin for ONVIF cameras:
discovery, PTZ/preset control, capability-aware image / stream / OSD / network
configuration, scene→preset actions, and DHCP IP-change auto-repair of Media
Source URLs.

> **Status: feature-complete for the planned scope.** Milestones 0–3 (repo +
> ONVIF core + registry + OBS layer) and M4's PTZ transport/motor control
> (§6.8 of `IMPLEMENTATION_PLAN.md`) are implemented and CI-green on Windows
> x64. The remaining M4 items are hardware-gated field checks (latency/
> overshoot on real cameras) and release publishing. See
> [`PLAN.md`](PLAN.md) (decisions) and
> [`IMPLEMENTATION_PLAN.md`](IMPLEMENTATION_PLAN.md) (build order + code).

## Features

- **WS-Discovery** (multicast) with a always-on Hello/Bye listener + heartbeat
  Probes; manual add-by-IP fallback is planned.
- **Cameras** — live table, online/offline tracking, XAddr + stream-URI move
  detection that rewrites mapped Media Source URLs (DHCP IP-change auto-repair).
- **PTZ** — velocity pad and hotkeys through a coalesced, single-in-flight
  controller; presets (save/go/rename/delete); scene→preset bindings.
- **Config** — per-camera Image / Stream / OSD / Network tabs backed by the
  ONVIF imaging / media / display / device services.
- **Public C ABI** (`obs-onvif.h`, `obs_onvif_get_abi`) for external consumers
  (Advanced Scene Switcher and friends).

See [docs/USER_GUIDE.md](docs/USER_GUIDE.md) for installation and usage.

## Build status

| Lane | Status |
|---|---|
| Build & package (Windows x64) | [![build](https://github.com/abelbour/OBS-ONVIF-plugin/actions/workflows/build.yml/badge.svg)](https://github.com/abelbour/OBS-ONVIF-plugin/actions/workflows/build.yml) |
| Core tests (CTest, OBS-free) | `ctest --test-dir build_x64 -C <config> --output-on-failure` on every CI build |

The workflow runs on every push to `main`, on pull requests, on manual
`workflow_dispatch`, and once nightly (04:17 UTC) as a continuous dependency /
toolchain health check. Failed pull requests get an automatic comment pointing
at the failing run.

## Requirements

- Windows 10+ (x64)
- OBS Studio 32.x (Qt6). The vendored build pins OBS 32.2.1 deps (obs-studio
  32.2.1 + obs-deps/Qt6 2026-07-15).
- **Consumers of the public plugin ABI** link against `obs-onvif.h` (shipped in
  the release zip) and resolve `obs_onvif_get_abi` via `obs_get_module_symbol`.

## Build

Local (VS 2022 + CMake 3.28+):

```
cmake --preset windows-x64
cmake --build --preset windows-x64
cmake --install build_x64 --prefix <DESIRED_LOCATION> --config RelWithDebInfo
```

CI (`windows-2022`): `.github/workflows/build.yml` runs
`.github/scripts/Build-Windows.ps1` (downloads pinned deps into `.deps/`, caches
them between runs, builds obs-frontend-api from the pinned OBS source, compiles
the plugin with warnings-as-errors) then packages an installable zip that
includes `obs-onvif.h`, the license, third-party notices, and this guide.

## License

GPL-3.0 — see [`LICENSE`](LICENSE). Third-party attributions (obs-plugintemplate
GPL-2.0, ODM GPL-2.0, TinyXML2 zlib, nlohmann/json MIT, Qt6 LGPL-3.0) are in
[`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md).