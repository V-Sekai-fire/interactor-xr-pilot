/-
emit_spark_cases: writes the sparkline runs the model works out.

  lake exe emit_spark_cases [path]     -- default ../core/tests/SparkModelCases.h

SPDX-License-Identifier: Apache-2.0 OR MIT
-/
import XrPilot.Spark

def main (args : List String) : IO UInt32 := do
  let path := args.headD "../core/tests/SparkModelCases.h"
  IO.FS.writeFile path XrPilot.Spark.emitCpp
  IO.println s!"wrote {path}"
  return 0
