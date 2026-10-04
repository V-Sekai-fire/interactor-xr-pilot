-- SPDX-License-Identifier: Apache-2.0 OR MIT
import Lake
open Lake DSL

package XrPilot where
  leanOptions := #[⟨`autoImplicit, false⟩]

lean_lib XrPilot

-- The MCP server agents register; it drives xr-pilot over its stdio line protocol.
@[default_target]
lean_exe xr_pilot_mcp where
  root := `Main

lean_exe tests where
  root := `Tests
