/-
Taskweft's RECTGTN (relationship-enabled capability-temporal goal-task network) as an acting DSL.

  {"capabilities": ["look", "click", "wait"],
   "methods": {"enter": {"params": ["x", "y"], "alternatives": [
       {"name": "click_it", "check": [{"eval": {"type": "math/eq", "a": {"pointer_get": "/connected"}, "b": true}}],
        "subtasks": [["click", {"x": "{x}", "y": "{y}"}], ["wait", {"ms": 500}]]}]},
               "connected": {"params": ["want"], "alternatives": [{"name": "settle", "subtasks": [["wait", {"ms": 2000}]]}]}},
   "todo_list": [{"goal": [{"pointer": "/connected", "eq": true}]}, ["enter", 568, 632]]}

- Tasks: a method call [name, arg...], a tool call [tool, {arguments}] (the actions are the MCP tools),
  a goal {"goal": [{pointer, eq}]} or a multigoal {"multigoal": [{pointer, eq}...]} over the pilot's state.
- Methods: ordered alternatives, each with an optional `check` of taskweft eval guards and `subtasks`;
  "{param}" is substituted. A goal's unmet binding runs the method named after its variable (the first
  pointer segment) with the wanted value, then is checked again.
- Capabilities: when listed, an action outside them is refused, as taskweft's rebac guard refuses one.
- Temporal: every action records when it started and how long it took.

Unlike taskweft, which plans against a model and replans on request, this acts: an action runs for
real when reached, and a failure makes the enclosing method try its next alternative from the world as
it now is, which is taskweft's incremental replan applied to the world.

SPDX-License-Identifier: Apache-2.0 OR MIT
-/
import Lean.Data.Json

namespace XrPilot
open Lean

/-- How the plan reaches the world: run a tool, read the pilot's state, mark a span, the clock. -/
structure Actor where
  tool : String → Json → IO Json
  state : IO Json
  mark : String → IO Unit
  isTool : String → Bool
  nowMs : IO Nat := do return (← IO.monoMsNow)

structure PlanLimits where
  maxSteps : Nat := 256
  maxDepth : Nat := 16

/-- What happened: the actions run in order with their outcome and timing, and why the plan stopped. -/
structure PlanTrace where
  steps : Array Json := #[]
  failure : Option String := none
  startMs : Nat := 0

def jsonPointer (j : Json) (pointer : String) : Json :=
  (pointer.splitOn "/").drop 1 |>.foldl (init := j) fun cur key =>
    match key.toNat? with
    | some i => match cur.getArrVal? i with
      | .ok v => v
      | .error _ => cur.getObjValD key
    | none => cur.getObjValD key

/-- Replaces "{p}" in strings: a string that is exactly "{p}" becomes the bound value itself. -/
partial def substitute (bindings : List (String × Json)) : Json → Json
  | .str s =>
    match bindings.find? (fun (p, _) => s == "{" ++ p ++ "}") with
    | some (_, v) => v
    | none => .str (bindings.foldl (fun acc (p, v) =>
        acc.replace ("{" ++ p ++ "}") (match v with | .str t => t | other => other.compress)) s)
  | .arr items => .arr (items.map (substitute bindings))
  | .obj kvs => Json.mkObj (kvs.foldl (fun acc k v => (k, substitute bindings v) :: acc) [] |>.reverse)
  | other => other

private def operand (state : Json) (j : Json) : Json :=
  match (j.getObjValD "pointer_get").getStr? with
  | .ok p => jsonPointer state p
  | .error _ => j

private def numberOf (j : Json) : Option Float :=
  match j.getNum? with
  | .ok n => some n.toFloat
  | .error _ => none

private def same (a b : Json) : Bool :=
  a == b || match numberOf a, numberOf b with
    | some x, some y => x == y
    | _, _ => false

/-- One taskweft `eval` guard against the pilot's state; an unknown type is false. -/
def evalCheck (state : Json) (check : Json) : Bool :=
  let e := check.getObjValD "eval"
  let a := operand state (e.getObjValD "a")
  let b := operand state (e.getObjValD "b")
  let compare (f : Float → Float → Bool) : Bool :=
    match numberOf a, numberOf b with
    | some x, some y => f x y
    | _, _ => false
  match (e.getObjValD "type").getStr? with
  | .ok "math/eq" => same a b
  | .ok "math/ne" => !same a b
  | .ok "math/gt" => compare (· > ·)
  | .ok "math/ge" => compare (· ≥ ·)
  | .ok "math/lt" => compare (· < ·)
  | .ok "math/le" => compare (· ≤ ·)
  | _ => false

private def strOf (j : Json) (d : String := "") : String :=
  match j.getStr? with | .ok s => s | .error _ => d

private def arrOf (j : Json) : Array Json :=
  match j.getArr? with | .ok a => a | .error _ => #[]

private def isError (result : Json) : Bool :=
  match (result.getObjValD "isError").getBool? with
  | .ok b => b
  | .error _ => false

/-- The bindings a goal or multigoal names: (pointer, wanted value). -/
private def bindingsOf (task : Json) : Array (String × Json) :=
  let list := if !(task.getObjValD "goal").isNull then task.getObjValD "goal" else task.getObjValD "multigoal"
  (arrOf list).filterMap fun b => match (b.getObjValD "pointer").getStr? with
    | .ok p => some (p, b.getObjValD "eq")
    | .error _ => none

private def fail (trace : IO.Ref PlanTrace) (why : String) : IO Bool := do
  trace.modify fun t => { t with failure := t.failure <|> some why }
  return false

mutual

/-- Runs one task, depth-first; true when it succeeded. Steps and the first failure land in the trace. -/
partial def runTask (actor : Actor) (domain : Json) (limits : PlanLimits) (trace : IO.Ref PlanTrace)
    (depth : Nat) (task : Json) : IO Bool := do
  if (← trace.get).steps.size ≥ limits.maxSteps then
    return ← fail trace s!"out of fuel after {limits.maxSteps} actions"
  if depth > limits.maxDepth then
    return ← fail trace s!"deeper than {limits.maxDepth} methods"
  if task.getObj?.isOk then
    return ← runGoal actor domain limits trace depth task
  let items := arrOf task
  let name := strOf (items.getD 0 Json.null)
  let args := items.extract 1 items.size
  if actor.isTool name then
    let capabilities := arrOf (domain.getObjValD "capabilities")
    if capabilities.size > 0 && !capabilities.contains (Json.str name) then
      return ← fail trace s!"no capability for {name}"
    let toolArgs := args.getD 0 (Json.mkObj [])
    let started ← actor.nowMs
    let result ← actor.tool name toolArgs
    let finished ← actor.nowMs
    let ok := !isError result
    let base := (← trace.get).startMs
    trace.modify fun t => { t with steps := t.steps.push (Json.mkObj [
      ("action", name), ("args", toolArgs), ("ok", ok),
      ("start_ms", started - base), ("duration_ms", finished - started)]) }
    if ok then
      return true
    return ← fail trace s!"{name} failed"
  runMethod actor domain limits trace depth name args

/-- A goal or multigoal: each unmet binding runs the method named after its variable, then is rechecked. -/
partial def runGoal (actor : Actor) (domain : Json) (limits : PlanLimits) (trace : IO.Ref PlanTrace)
    (depth : Nat) (task : Json) : IO Bool := do
  let bindings := bindingsOf task
  if bindings.isEmpty then
    return ← fail trace s!"a goal with no bindings: {task.compress}"
  for (pointer, want) in bindings do
    if same (jsonPointer (← actor.state) pointer) want then
      continue
    let varName := ((pointer.splitOn "/").drop 1).headD ""
    if !(← runMethod actor domain limits trace (depth + 1) varName #[want]) then
      return ← fail trace s!"goal {pointer} = {want.compress} has no way to be reached"
    if !same (jsonPointer (← actor.state) pointer) want then
      return ← fail trace s!"goal {pointer} = {want.compress} still unmet after {varName}"
  return true

/-- A method: the first alternative whose checks hold and whose subtasks all succeed. -/
partial def runMethod (actor : Actor) (domain : Json) (limits : PlanLimits) (trace : IO.Ref PlanTrace)
    (depth : Nat) (name : String) (args : Array Json) : IO Bool := do
  let method := (domain.getObjValD "methods").getObjValD name
  if method.isNull then
    return ← fail trace s!"no action or method named {name}"
  let params := (arrOf (method.getObjValD "params")).toList.filterMap fun (p : Json) => p.getStr?.toOption
  let bindings := params.zip args.toList
  for alt in arrOf (method.getObjValD "alternatives") do
    let altName := strOf (alt.getObjValD "name") "alternative"
    let checks := arrOf (alt.getObjValD "check")
    if checks.size > 0 then
      let state ← actor.state
      unless checks.all (fun c => evalCheck state (substitute bindings c)) do
        continue
    let id := toString (← IO.monoNanosNow)
    actor.mark s!"span begin {id} {name}/{altName}"
    let mut ok := true
    for sub in arrOf (alt.getObjValD "subtasks") do
      if !(← runTask actor domain limits trace (depth + 1) (substitute bindings sub)) then
        ok := false
        break
    actor.mark s!"span end {id} {if ok then "ok" else "error"}"
    if ok then
      trace.modify fun t => { t with failure := none }
      return true
  fail trace s!"no alternative of {name} applies"

end

/-- Runs every task of the todo list in order, stopping at the first that fails. -/
def runPlan (actor : Actor) (domain : Json) (limits : PlanLimits := {}) : IO PlanTrace := do
  let trace ← IO.mkRef ({ startMs := ← actor.nowMs } : PlanTrace)
  for task in arrOf (domain.getObjValD "todo_list") do
    if !(← runTask actor domain limits trace 0 task) then
      break
  trace.get

end XrPilot
