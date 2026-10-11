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

### The tray's MCP server

The installed tray runs `xr_pilot_mcp` itself, at `http://127.0.0.1:9950/mcp` (MCP's streamable HTTP transport). An agent drives the pilot you see: its first command opens the pilot window, and while the window is closed no command is answered. The server listens on the loopback address only. It refuses a request without the install's bearer token (401), and one whose `Host` is not this machine's loopback or whose `Origin` is a web page (403), so a page in a browser cannot reach it.

The token is made at the tray's first start, in `%APPDATA%\V-Sekai-fire\xr-pilot\mcp-token`, which only your account can read. Register the server with Claude Code by putting the token in its header:

```json
"xr-pilot": {
  "type": "http",
  "url": "http://127.0.0.1:9950/mcp",
  "headers": { "Authorization": "Bearer <the contents of mcp-token>" }
}
```

`claude mcp add --transport http xr-pilot http://127.0.0.1:9950/mcp --header "Authorization: Bearer <token>"` writes the same entry. The stdio registration above still works when the tray is not running.

## Licence

Apache-2.0 OR MIT. `oxrsys/` and the files ported from it are MPL-2.0 (`oxrsys/LICENSE`), and `third_party/` projects carry their own licences.
