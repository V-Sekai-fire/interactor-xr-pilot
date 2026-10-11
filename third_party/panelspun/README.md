# interactor-panelspun

A C++ library for desktop tool windows built from docked panels, drawn on a software canvas in SDL3 windows.

## What it is for

Panels are the leaves of a split tree that resizes, docks and saves to a text format, and every panel draws into one CPU canvas. A panel can instead own a GPU region of the window, for content such as a decoded video preview. The build also makes a demo window of docked panels.

## Build and run

    cmake -S . -B build
    cmake --build build
    ctest --test-dir build

## Licence

Apache-2.0 OR MIT, at your option; see [`LICENSE-APACHE`](LICENSE-APACHE) and [`LICENSE-MIT`](LICENSE-MIT). Vendored projects under `third_party/` carry their own licences.
