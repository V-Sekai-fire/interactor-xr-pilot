# interactor-panelspun
Docking panel UI on SDL3 windows and ThorVG canvases, Apache-2.0 OR MIT

panelspun is a small C++20 library for desktop tool windows built from docked panels. Panels are
leaves of a binary split tree; dragging a split resizes its neighbours within each panel's minimum
size, and the layout round-trips through a line-based text format. Every panel draws into one ThorVG
CPU canvas, and a panel can instead own a Vulkan region for content such as a decoded video preview.

## Pieces

- `SplitTree` (`include/panelspun/split_tree.h`): layout into pixel rects, split-handle hit tests and
  clamped drags, docking a panel left, right, above or below another, removal that collapses the
  parent (the last panel cannot be removed), and `serialize` / `deserialize`.
- `Window` (`include/panelspun/window.h`): an SDL3 window with a Vulkan swapchain, an event pump that
  routes pointer events to panels in panel-local pixels, and a frame loop that redraws on change.
- `Panel`, `Label`, `Button`, `Slider`, `WidgetPanel` (`include/panelspun/panel.h`, `widgets.h`).

## How the Vulkan region works

There is one swapchain, owned by the window. ThorVG draws the whole UI straight into a mapped,
host-visible staging buffer, which is copied into the acquired swapchain image. A panel whose
`usesVulkanRegion()` returns true is handed a window-owned image the size of its content rect, in
`TRANSFER_DST_OPTIMAL`, with the window's command buffer and device (`VulkanRegionFrame`); after it
records, the window copies that image over the panel's rect. `Window::vulkan()` exposes the instance,
device and queue so a consumer can create its own resources on the same device. A consumer that records
compute work, such as a GPU video decoder, sets `WindowConfig::vulkanAllFeatures`: the window then
creates a Vulkan 1.3 device with every supported core feature enabled, and `VulkanContext` carries the
instance and device create infos so a library that wraps an existing device can see what was enabled.

Two alternatives were set aside. A native child window per region is not portable across SDL3's
backends, and a second swapchain on the same surface is not allowed. The cost of this design is that
UI cannot yet be drawn over a Vulkan region; overlays on a video preview are drawn by the region's
own pass for now.

## Build

    cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
    cmake --build build
    ctest --test-dir build --output-on-failure

On Windows run these from a Visual Studio 2022 x64 developer prompt. On Linux SDL3 needs the X11 and
Wayland development headers listed in `.github/workflows/ci.yml`. The Vulkan loader is found at run
time through SDL; the headers come from SDL's Khronos copy unless `PANELSPUN_VULKAN_HEADERS_DIR`
points elsewhere.

`build/panelspun-demo` opens three docked panels. With a display and a Vulkan device,
`-DPANELSPUN_GPU_TESTS=ON` registers two more ctest cases: the demo reads back its presented frame
and checks that the Vulkan clear colour fills the video region and no other panel, and a control run
without the region must fail that check.

## Vendored code

| Path | Upstream | Version | Licence |
|---|---|---|---|
| `third_party/SDL` | libsdl-org/SDL | `release-3.4.18` (git subtree, squashed) | Zlib |
| `third_party/thorvg` | thorvg/thorvg | `v1.1.2` (git subtree, squashed) | MIT |
| `third_party/inter` | rsms/inter | `v4.1`, `Inter-Regular.ttf` | OFL-1.1 |

SDL3 is built static with video and Vulkan only (no audio, GPU, render, camera, joystick, haptic,
HIDAPI, power, sensor, dialog, tray or OpenGL). ThorVG is built from its sources by
`cmake/thorvg.cmake` with the CPU engine and the SVG and TTF loaders, single-threaded.

## Licence

Dual-licensed under either of [Apache-2.0](LICENSE-APACHE) or [MIT](LICENSE-MIT), at your option
(`SPDX-License-Identifier: Apache-2.0 OR MIT`). Vendored code keeps its own licence.
