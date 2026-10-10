/-
emit_ui_spans: writes the UI actions and their span names for the app.

  lake exe emit_ui_spans [path]     -- default ../core/include/xrpilot/UiSpans.h

SPDX-License-Identifier: Apache-2.0 OR MIT
-/
import XrPilot.Ui

def main (args : List String) : IO UInt32 := do
  let path := args.headD "../core/include/xrpilot/UiSpans.h"
  IO.FS.writeFile path XrPilot.Ui.emitCpp
  IO.println s!"wrote {path}"
  return 0
