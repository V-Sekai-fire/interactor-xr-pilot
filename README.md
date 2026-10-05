# interactor-xr-pilot
Lets an AI agent see and drive OpenXR apps.

XR Pilot is a computer-use tool for VR. An agent takes a screenshot of what an OpenXR app shows the
left eye, then turns the head, walks, points a controller at a pixel, clicks with the trigger, or
presses buttons, the way a desktop computer-use driver moves a mouse. The app runs on the desktop OpenXR runtime, and XR Pilot connects
to it as a headset would, so neither the runtime nor the app needs changing.

## Tools

| Tool | What it does |
|---|---|
| `screenshot` | The left eye as a PNG, with the head pose and field of view |
| `record` | A short clip as a sequence of PyroWave-decoded left-eye frames, returned as images a vision model reads in order, each with its head pose |
| `get_state` | Connection, eye size and field of view, head and hand poses, held inputs, frame counts |
| `health` | Connection, the server streamed from, and the assembled, decoded and dropped frame counts with the dropped fraction |
| `look` | Turns the head to exactly one of a 3x3 rotation matrix, Euler angles in a named Tait-Bryan order, or a quaternion; absolute or relative to the head |
| `look_at` | Turns the head so a screenshot pixel is at the centre of the view |
| `set_fov` | Sets the vertical field of view the pilot submits, in degrees |
| `move` | Walks the head forward, right and up, in metres |
| `move_to` | Places the head at an absolute world position, keeping its rotation |
| `point_at` | Aims a controller through a screenshot pixel, from the eye, so its ray hits what the pixel shows |
| `probe` | Reports the world ray a screenshot pixel points along, without moving a hand |
| `click` | `point_at`, a 150 ms hover, then a trigger press and release |
| `double_click` | `point_at`, then two trigger presses in quick succession |
| `hover` | Aims through a pixel and holds there without pressing |
| `drag` | Points at one pixel, presses the trigger, points at a second, releases |
| `scroll` | Aims at a pixel, holds the trigger, pushes a thumbstick to scroll, releases |
| `press` | A button, trigger or grip, held for `hold_ms` (0 holds until `release_all`) |
| `thumbstick` | Pushes a thumbstick for `duration_ms` |
| `reach` | The body's arm reaches for what a pixel shows, a depth from the eye |
| `grab` | Reaches in, grips and lifts what a pixel shows |
| `set_hand` | Places a hand at an absolute world pose |
| `gesture` | Shapes a hand into `open`, `fist`, `point` or `pinch` |
| `set_controllers` | Whether the app sees controllers |
| `release_all` | Lets go of every input; the head stays where it is |
| `locomote` | Smooth movement, snap turns or teleport, the ways the locomotion research names |
| `wait` | Waits so the app can react |
| `wait_for` | Waits until a `get_state` pointer reaches a value, or times out |
| `plan` | Runs a taskweft task network over the other tools in one call |

Every rotation the tools report, and every one the pilot's line protocol carries, is a row-major 3x3 matrix taking head- or hand-local vectors to world space; its columns are the local +X, +Y and +Z axes. Euler angles (`XYZ`, `XZY`, `YXZ`, `YZX`, `ZXY`, `ZYX`, intrinsic, degrees) and quaternions (`[x, y, z, w]`) are inputs to `look`, converted to the matrix and never reported back. A matrix that is not orthonormal with determinant +1 is refused.

Pixel arguments refer to the last screenshot, so `point_at` and `click` refuse until one is taken.

## Build

    cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
    cmake --build build
    ctest --test-dir build --output-on-failure
    cd lean && lake build && lake build tests && .lake/build/bin/tests

On Windows run CMake from a Visual Studio 2022 x64 developer prompt. `-DXRPILOT_BUILD_APP=OFF`
builds and tests only the core, without a display or Vulkan.

The same build compiles the OXRSys runtime and PC VR driver from `oxrsys/` into `build/oxrsys/`,
which is where the tray installs them from; `-DXRPILOT_BUILD_OXRSYS=OFF` leaves them out.
`scripts/windows_build.ps1` does the whole Windows build with CMake, Ninja and the Vulkan loader from
`pixi.toml`. `cpack -G WIX` in that build tree packs XR Pilot, the runtime and the driver into one
per-user MSI with WiX Toolset v4 (`dotnet tool install --global wix --version 4.0.6`, then
`wix extension add -g WixToolset.UI.wixext/4.0.6`). It installs under
`%LOCALAPPDATA%\Programs\XR Pilot` with no UAC prompt and adds an `XR Pilot` Start-menu shortcut;
`scripts/windows_check_msi.ps1` installs, checks and uninstalls one.

The MSI writes nothing outside the user's profile. On first launch the tray copies the runtime and the
driver to `%LOCALAPPDATA%\OXRSys` and registers the driver with SteamVR, both per-user. Making OXRSys
the default OpenXR runtime stays a tray action with one UAC prompt: the Khronos loader (OpenXR-SDK
1.1.63, `src/loader/manifest_file.cpp`) reads `ActiveRuntime` from `HKEY_LOCAL_MACHINE` only, and reads
`HKEY_CURRENT_USER` for API layers, not runtimes. Without it, `XR_RUNTIME_JSON` pointed at
`runtime\oxrsys-runtime.json` selects OXRSys for one unelevated process.

## OXRSys

`oxrsys/` is the OXRSys OpenXR runtime, its PC VR driver and its Android and Apple headset clients,
brought in with its history from `V-Sekai-fire/interactor-oxrsys`, which this repository replaces. `oxrsys/README.md` and `oxrsys/docs/` describe it. XR Pilot's tray replaces the
Qt Home app, which is gone with the rest of the Qt clients.

## Run

Start an OpenXR app on OXRSys, then register the MCP server with your agent:

    xr_pilot_mcp --pilot <path to xr-pilot>

Without `--pilot` it runs the `xr-pilot` next to itself. Only one client streams from an OXRSys
runtime at a time, so close the OXRSys simulator or headset client first.

## Licence

Apache-2.0 OR MIT. `oxrsys/` is MPL-2.0 (`oxrsys/LICENSE`), forked from `demonixis/oxrsys`, unless
a file there states otherwise; the Windows build script came
from it and stay MPL-2.0. `core/src/FrameAssembler.cpp`, `core/src/GpuDecoder.cpp`, the tray and their
headers are ported from OXRSys and stay under MPL-2.0, file by file. `third_party/panelspun` carries
its own licences. `third_party/witness-cpp` is the header-only property-testing ladder
(MIT, `V-Sekai-fire/plausible-witness-dag`'s C++ shape), used only by the tests.
