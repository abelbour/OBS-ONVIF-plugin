# OBS ONVIF Plugin

Windows-native [OBS Studio](https://obsproject.com) plugin for ONVIF cameras:
discovery, PTZ/preset control, capability-aware image / stream / OSD / network
configuration, scene→preset actions, and DHCP IP-change auto-repair of Media
Source URLs.

> **Status: scaffolding (Milestone 0).** The repo builds a green no-op plugin on
> Windows CI. Feature work is tracked in [`PLAN.md`](PLAN.md) (decisions) and
> [`IMPLEMENTATION_PLAN.md`](IMPLEMENTATION_PLAN.md) (build order + code).

## Build status

| Lane | Status |
|---|---|
| Build & package (Windows x64) | [![build](https://github.com/abelbour/OBS-ONVIF-plugin/actions/workflows/build.yml/badge.svg)](https://github.com/abelbour/OBS-ONVIF-plugin/actions/workflows/build.yml) |

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
the plugin with warnings-as-errors) then packages an installable zip.

## License

GPL-3.0. The vendored [obs-plugintemplate](https://github.com/obsproject/obs-plugintemplate)
scaffold is GPL-2.0, kept under [`LICENSE`](LICENSE); ODM protocol logic ported
from [ONVIF Device Manager](https://github.com/dxball/ONVIF-Device-Manager)
(GPL-2.0, compatible).