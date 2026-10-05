# interactor-xr-pilot

Lets an agent see and drive OpenXR apps through a Model Context Protocol server.

## What it is for

An agent screenshots what an OpenXR app shows, then moves the head, hands and controllers and presses buttons. The app runs on the OXRSys desktop runtime, which lives in this repository and which XR Pilot connects to as a headset would. The tool schemas list what it can do.

## Build and run

```sh
cmake -S . -B build
cmake --build build
```

`lake build` in `lean/` builds `xr_pilot_mcp` into `lean/.lake/build/bin`. It runs the `xr-pilot` next to itself, so pass `--pilot` with the path of the one CMake built. An app reaches OXRSys when `XR_RUNTIME_JSON` names the `oxrsys-runtime.json` the build writes beside the runtime library. On Windows, `scripts/windows_build.ps1` runs the build.

Start an OpenXR app on OXRSys, then register `xr_pilot_mcp` with your agent.

## Licence

Apache-2.0 OR MIT. `oxrsys/` and the files ported from it are MPL-2.0 (`oxrsys/LICENSE`), and `third_party/` projects carry their own licences.
