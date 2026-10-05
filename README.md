# interactor-xr-pilot
Lets an AI agent see and drive OpenXR apps.

XR Pilot is a computer-use tool for VR. An agent screenshots what an OpenXR app shows, then moves
the head, hands and controllers and presses buttons through an MCP server. The app runs on the
OXRSys desktop runtime, which XR Pilot connects to as a headset would. `xr_pilot_mcp --help` and
the tool schemas list what it can do.

`oxrsys/` is the OXRSys runtime, its PC VR driver and headset clients, brought in with its history
from `V-Sekai-fire/interactor-oxrsys`, which this repository replaces.

## Build

    cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
    cmake --build build
    ctest --test-dir build --output-on-failure

On Windows, `scripts/windows_build.ps1` builds with the toolchain from `pixi.toml`.

## Run

Start an OpenXR app on OXRSys, then register `xr_pilot_mcp` with your agent.

## Licence

Apache-2.0 OR MIT. `oxrsys/` and the files ported from it are MPL-2.0 (`oxrsys/LICENSE`), and
`third_party/` projects carry their own licences.
