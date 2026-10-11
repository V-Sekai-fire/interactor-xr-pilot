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

When XR Pilot runs from the Start menu or the tray, it serves MCP itself at `http://127.0.0.1:9949/mcp` and drives its own pilot window, opening it if it is closed. Use this server and not a separate `xr_pilot_mcp`, because a second pilot cannot bind the OXRSys ports that the tray holds. The server listens on the loopback address only. It refuses each request that does not carry this desk's bearer token, and each request with an `Origin` that is not loopback. The token is 32 random bytes, which XR Pilot makes on its first start and keeps in `mcp-token`, a file in its per-user folder that only you can read. To get the client settings, click **Copy MCP settings** in the tray menu, or run `xr-pilot --mcp-config`:

```json
{"type":"http","url":"http://127.0.0.1:9949/mcp","headers":{"Authorization":"Bearer <token>"}}
```

`--mcp-port <port>` moves the server to a different port, and `--mcp-port 0` turns it off.

## Licence

Apache-2.0 OR MIT. `oxrsys/` and the files ported from it are MPL-2.0 (`oxrsys/LICENSE`), and `third_party/` projects carry their own licences.
