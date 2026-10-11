/-
The Stream panel's sparklines as a queue and a state machine: each line keeps the last 40 per-tick
increments of a counter (10 s at 250 ms), sampled on its own clock whatever the window does.

* The window never holds more than 40 samples (`window_bounded`).
* A stream that should be sending and is not draws red: a sample of zero on a series that expects
  data while connected is a fault (`silence_while_connected_is_red`), and the same silence while
  disconnected is not (`silence_while_disconnected_is_not`). This is issue #52.
* Zero is drawn a unit above the base rule, never on it (`zero_clears_the_base`), and the second ticks
  move with every sample (`ticks_move_every_sample`), so a strip of zeros still visibly scrolls. This
  is issue #53.
* The pump samples once per period of elapsed time, frames or not (`pump_counts_time`).

`emitCpp` writes `core/tests/SparkModelCases.h`, which `core/tests/human.cpp` replays against
`core/src/Sparkline.cpp`.

SPDX-License-Identifier: Apache-2.0 OR MIT
-/

namespace XrPilot.Spark

def samples : Nat := 40
/-- A tick mark every this many samples: once a second at 250 ms. -/
def tickEvery : Nat := 4

structure Sample where
  delta : Nat
  /-- Zero while the series expected data: the stream is down. -/
  starved : Bool
  deriving DecidableEq, Repr, Inhabited

structure Line where
  window : List Sample := []
  last : Nat := 0
  primed : Bool := false
  /-- Samples ever taken; places the tick marks. -/
  count : Nat := 0
  deriving DecidableEq, Repr, Inhabited

/-- The window drops its oldest sample past `samples`. -/
def trim (w : List Sample) : List Sample := if w.length > samples then w.drop 1 else w

theorem trim_bounded (w : List Sample) (h : w.length ≤ samples + 1) : (trim w).length ≤ samples := by
  unfold trim; split <;> simp_all <;> omega

theorem trim_last (w : List Sample) (x : Sample) : (trim (w ++ [x])).getLast? = some x := by
  cases w with
  | nil => simp [trim, samples]
  | cons a t =>
    unfold trim; split
    · simp
    · exact List.getLast?_concat

/-- `Sparkline::sample`: the increment since the last total (none for the first, or when the counter
went backwards), starved when it is zero on a series that expects data. -/
def Line.sample (l : Line) (total : Nat) (expecting : Bool) : Line :=
  let d := if l.primed && total ≥ l.last then total - l.last else 0
  { window := trim (l.window ++ [⟨d, expecting && d == 0⟩]), last := total, primed := true, count := l.count + 1 }

theorem window_bounded (l : Line) (total : Nat) (e : Bool) (h : l.window.length ≤ samples) :
    (l.sample total e).window.length ≤ samples := by
  exact trim_bounded _ (by simp; omega)

inductive Colour where
  | normal | fault | clipped
  deriving DecidableEq, Repr, Inhabited

/-- 1.5 × the 90th percentile, at least 1: a spike clips instead of flattening the rest. -/
def scale (w : List Sample) : Nat :=
  let sorted := (w.map (·.delta)).mergeSort (· ≤ ·)
  max 1 (sorted.getD ((sorted.length - 1) * 9 / 10) 0 * 3 / 2)

def colour (faultSeries : Bool) (sc : Nat) (s : Sample) : Colour :=
  if (faultSeries && s.delta > 0) || s.starved then .fault
  else if s.delta > sc then .clipped
  else .normal

/-- One colour per segment, each taking its end sample's colour. -/
def Line.colours (l : Line) (faultSeries : Bool) : List Colour :=
  if l.window.length < 2 then [] else (l.window.drop 1).map (colour faultSeries (scale l.window))

/-- A connected stream that stops: the next sample of a series expecting data is red. -/
theorem silence_while_connected_is_red (l : Line) (fs : Bool) (h : l.primed = true) :
    ((l.sample l.last true).window.getLast?.map (colour fs 0)) = some .fault := by
  simp [Line.sample, h, trim_last, colour]

/-- The same silence while disconnected is not a fault on a series that is not one. -/
theorem silence_while_disconnected_is_not (l : Line) (sc : Nat) (h : l.primed = true) :
    ((l.sample l.last false).window.getLast?.map (colour false sc)) = some .normal := by
  simp [Line.sample, h, trim_last, colour]

/-- Which row a sample sits on, y down in a box `height` rows tall, to the nearest row below: the scale
spans rows 0 to `height - 1`, and the base rule is drawn on row `height`. -/
def row (height sc d : Nat) : Nat := (height - 1) - (height - 1) * min d sc / sc

/-- Zero sits a row above the base rule, so a strip of zeros never vanishes into it. -/
theorem zero_clears_the_base (height sc : Nat) (h : 0 < height) : row height sc 0 + 1 = height := by
  simp [row]; omega

/-- Which window positions carry a tick, for a line that has taken `count` samples into a full window. -/
def ticksAt (count : Nat) : List Nat :=
  (List.range samples).filter fun i => (count + i) % tickEvery == 0

theorem ticks_periodic (c : Nat) : ticksAt (c + tickEvery) = ticksAt c := by
  simp [ticksAt, tickEvery, Nat.add_right_comm c 4, Nat.add_mod_right]

/-- Every sample moves the ticks, so even a window of zeros scrolls visibly. -/
theorem ticks_move_every_sample (c : Nat) : ticksAt (c + 1) ≠ ticksAt c := by
  have small : ∀ r < tickEvery, ticksAt (r + 1) ≠ ticksAt r := by decide
  have reduce : ∀ k, ticksAt k = ticksAt (k % tickEvery) := by
    intro k
    induction k using Nat.strongRecOn with
    | _ k ih =>
      by_cases hk : k < tickEvery
      · rw [Nat.mod_eq_of_lt hk]
      · have : k = (k - tickEvery) + tickEvery := by unfold tickEvery at *; omega
        rw [this, ticks_periodic, ih _ (by unfold tickEvery at *; omega), Nat.add_mod_right]
  rw [reduce (c + 1), reduce c]
  have hc : c % tickEvery < tickEvery := Nat.mod_lt _ (by decide)
  by_cases last : c % tickEvery + 1 < tickEvery
  · have : (c + 1) % tickEvery = c % tickEvery + 1 := by unfold tickEvery at *; omega
    rw [this]; exact small _ hc
  · have : (c + 1) % tickEvery = 0 := by unfold tickEvery at *; omega
    have three : c % tickEvery = 3 := by unfold tickEvery at *; omega
    rw [this, three]; decide

/-- The pump takes a sample each period of elapsed time; frames do not enter into it. -/
def pumped (periodNs startNs nowNs : Nat) : Nat := (nowNs - startNs) / periodNs

theorem pump_counts_time : pumped 250000000 0 2000000000 = 8 := by decide

/-! ## Cases for the C++ -/

def Colour.letter : Colour → Char
  | .normal => 'N' | .fault => 'F' | .clipped => 'C'

/-- Runs of (total, expecting) samples on a series, and whether it is a fault series. -/
def runs : List (String × Bool × List (Nat × Bool)) :=
  [("a connected stream that stops goes red", false,
     [(0, true), (90, true), (180, true), (180, true), (180, true), (270, true)]),
   ("control: the same silence while disconnected stays green", false,
     [(0, false), (90, false), (180, false), (180, false), (180, false), (270, false)]),
   ("a fault series is red only while it counts", true, [(0, false), (0, false), (2, false), (2, false)]),
   ("a spike clips", false, ((List.range 31).map fun i => (10 * i, true)) ++ [(1300, true)]),
   ("a counter that went backwards adds nothing", false, [(100, false), (40, false), (40, false)])]

def runLine (ops : List (Nat × Bool)) : Line := ops.foldl (fun l (t, e) => l.sample t e) {}

def emitCpp : String := Id.run do
  let mut out := "// SPDX-License-Identifier: MPL-2.0\n//\n"
  out := out ++ "// Generated by `lake exe emit_spark_cases` from lean/XrPilot/Spark.lean; do not edit.\n"
  out := out ++ "// What the model works out for the Stream panel's sparklines; core/tests/human.cpp replays\n"
  out := out ++ "// each against core/src/Sparkline.cpp.\n\n#pragma once\n\n#include <cstdint>\n#include <vector>\n\n"
  out := out ++ "namespace spark_model\n{\n\nstruct Op\n{\n    uint64_t total;\n    bool expecting;\n};\n\n"
  out := out ++ "struct Run\n{\n    const char* name;\n    bool faultSeries;\n    std::vector<Op> ops;\n"
  out := out ++ "    // One letter per segment: N normal, F fault, C clipped.\n    const char* colours;\n};\n\n"
  out := out ++ s!"constexpr int tickEvery = {tickEvery};\n\ninline const Run runs[] = \{\n"
  for (name, fs, ops) in runs do
    let ops' := ", ".intercalate (ops.map fun (t, e) => s!"\{{t}u, {e}}")
    let colours := String.ofList ((runLine ops).colours fs |>.map Colour.letter)
    out := out ++ s!"    \{\"{name}\", {fs}, \{{ops'}}, \"{colours}\"},\n"
  out := out ++ "};\n\n} // namespace spark_model\n"
  return out

end XrPilot.Spark
