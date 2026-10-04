# interactor-xr-pilot
Lets an AI agent see and drive OpenXR apps through OXRSys: eye frames in, head, hands and buttons out, over MCP.

XR Pilot is a computer-use tool for VR. An agent takes a screenshot of what an OpenXR app shows the
left eye, then turns the head, walks, points a controller at a pixel, clicks with the trigger, or
presses buttons, the way a desktop computer-use driver moves a mouse. The app runs on the desktop OpenXR runtime, and XR Pilot connects
to it as a headset would, so neither the runtime nor the app needs changing.

## Pieces

- `xr_pilot_mcp` (Lean 4, `lean/`): the MCP server an agent registers. JSON-RPC 2.0 over stdio, and
  the pixel-to-ray maths (`XrPilot/Ray.lean`). It starts `xr-pilot` and drives it over that
  process's stdin and stdout.
- `xr-pilot` (C++20, `app/`, `core/`): the OXRSys client. It finds a runtime by its UDP announce,
  streams the agent's head and controllers at 90 Hz, and decodes the PyroWave video on the GPU:
  PyroWave decodes into planes and the Lean-authored `yuv420_to_rgbx` kernel from OXRSys packs RGBX,
  on the same Vulkan device the window presents with. Frames come back to the CPU only for a
  screenshot. The window, built with [panelspun](https://github.com/V-Sekai-fire/interactor-panelspun)
  (SDL3 and ThorVG), shows the left eye and the agent's state.

## Tools

| Tool | What it does |
|---|---|
| `screenshot` | The left eye as a PNG, with the head pose and field of view |
| `get_state` | Connection, eye size and field of view, head and hand poses, held inputs, frame counts |
| `look` | Turns the head to exactly one of a 3x3 rotation matrix, Euler angles in a named Tait-Bryan order, or a quaternion; absolute or relative to the head |
| `move` | Walks the head forward, right and up, in metres |
| `point_at` | Aims a controller through a screenshot pixel, from the eye, so its ray hits what the pixel shows |
| `click` | `point_at`, a 150 ms hover, then a trigger press and release |
| `press` | A button, trigger or grip, held for `hold_ms` (0 holds until `release_all`) |
| `thumbstick` | Pushes a thumbstick for `duration_ms` |
| `set_controllers` | Whether the app sees controllers |
| `release_all` | Lets go of every input; the head stays where it is |
| `wait` | Waits so the app can react |

Every rotation the tools report, and every one the pilot's line protocol carries, is a row-major 3x3 matrix taking head- or hand-local vectors to world space; its columns are the local +X, +Y and +Z axes. Euler angles (`XYZ`, `XZY`, `YXZ`, `YZX`, `ZXY`, `ZYX`, intrinsic, degrees) and quaternions (`[x, y, z, w]`) are inputs to `look`, converted to the matrix and never reported back. A matrix that is not orthonormal with determinant +1 is refused.

Pixel arguments refer to the last screenshot, so `point_at` and `click` refuse until one is taken.

## Build

    cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
    cmake --build build
    ctest --test-dir build --output-on-failure
    cd lean && lake build && lake build tests && .lake/build/bin/tests

On Windows run CMake from a Visual Studio 2022 x64 developer prompt. `-DXRPILOT_BUILD_APP=OFF`
builds and tests only the core, without a display or Vulkan.

## Run

Start an OpenXR app on OXRSys, then register the MCP server with your agent:

    xr_pilot_mcp --pilot <path to xr-pilot>

Without `--pilot` it runs the `xr-pilot` next to itself. Only one client streams from an OXRSys
runtime at a time, so close the OXRSys simulator or headset client first.

## Licence

Apache-2.0 OR MIT. `core/src/FrameAssembler.cpp`, `core/src/GpuDecoder.cpp` and their headers are
ported from OXRSys and stay under MPL-2.0, file by file. `third_party/panelspun` carries its own
licences.
