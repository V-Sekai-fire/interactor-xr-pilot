/-
ui_check: replays the UI spans a pilot recorded against the model in XrPilot/Ui.lean.

  xrpilot-ui-trace traces.sqlite > run.jsonl
  lake exe ui_check run.jsonl

Each line is {"session": n, "span": "ui...", "ok": true}, in the order the spans started. A session
passes when some desk makes every span a step the UI offers there.

SPDX-License-Identifier: Apache-2.0 OR MIT
-/
import XrPilot.Ui

def main (args : List String) : IO UInt32 := do
  let some path := args.head? | IO.eprintln "usage: ui_check <spans.jsonl>"; return 2
  match XrPilot.Ui.checkJsonl (← IO.FS.readFile path) with
  | .ok n => IO.println s!"{n} session(s) follow the UI model"; return 0
  | .error e => IO.eprintln s!"ui_check: {e}"; return 1
