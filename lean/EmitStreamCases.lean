/-
emit_stream_cases: writes the control routing and spectator list runs the stream model works out.

  lake exe emit_stream_cases [path]     -- default ../oxrsys/tests/StreamModelCases.h

SPDX-License-Identifier: Apache-2.0 OR MIT
-/
import XrPilot.Stream

def main (args : List String) : IO UInt32 := do
  let path := args.headD "../oxrsys/tests/StreamModelCases.h"
  IO.FS.writeFile path XrPilot.Stream.emitCpp
  IO.println s!"wrote {path}"
  return 0
