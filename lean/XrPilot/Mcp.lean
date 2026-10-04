/-
The MCP server: JSON-RPC 2.0 over stdio, one message per line. Each tool turns into commands on the
pilot's line protocol (core/include/xrpilot/Commands.h) through a Backend, so the protocol logic is
tested against a fake pilot.

SPDX-License-Identifier: Apache-2.0 OR MIT
-/
import Lean.Data.Json
import XrPilot.Base64
import XrPilot.Plan
import XrPilot.Ray

namespace XrPilot
open Lean

structure Backend where
  /-- Sends one pilot command and returns its JSON reply. -/
  send : String → IO Json
  readFile : String → IO ByteArray
  sleepMs : Nat → IO Unit
  screenshotPath : String
  /-- The size of the last screenshot, which pixel arguments refer to. -/
  lastShot : IO.Ref (Float × Float)

def protocolVersion : String := "2025-06-18"

private def num (j : Json) (k : String) (d : Float) : Float :=
  match (j.getObjValD k).getNum? with
  | .ok n => n.toFloat
  | .error _ => d

private def str (j : Json) (k : String) (d : String) : String :=
  match (j.getObjValD k).getStr? with
  | .ok s => s
  | .error _ => d

private def bool (j : Json) (k : String) (d : Bool) : Bool :=
  match (j.getObjValD k).getBool? with
  | .ok b => b
  | .error _ => d

private def arrNum (j : Json) (i : Nat) : Float :=
  match j.getArrVal? i with
  | .ok v => match v.getNum? with | .ok n => n.toFloat | .error _ => 0.0
  | .error _ => 0.0

private def fmt (f : Float) : String := toString f

private def prop (type description : String) : Json :=
  Json.mkObj [("type", type), ("description", description)]

private def schema (props : List (String × Json)) (required : List String) : Json :=
  Json.mkObj [("type", "object"), ("properties", Json.mkObj props),
              ("required", Json.arr (required.map Json.str).toArray)]

private def rotationProp : Json :=
  Json.mkObj [("type", "array"), ("minItems", 3), ("maxItems", 3),
              ("items", Json.mkObj [("type", "array"), ("minItems", 3), ("maxItems", 3), ("items", Json.mkObj [("type", "number")])]),
              ("description", "A 3x3 rotation matrix, row by row, taking head-local vectors to world; the canonical form")]

private def eulerProp : Json :=
  Json.mkObj [("type", "object"),
              ("properties", Json.mkObj [
                ("order", Json.mkObj [("type", "string"), ("enum", Json.arr (eulerOrders.map Json.str).toArray),
                                      ("description", "Intrinsic Tait-Bryan order: YXZ with degrees [a, b, c] is Ry(a) Rx(b) Rz(c)")]),
                ("degrees", Json.mkObj [("type", "array"), ("minItems", 3), ("maxItems", 3), ("items", Json.mkObj [("type", "number")])])]),
              ("required", Json.arr #["order", "degrees"]),
              ("description", "Euler angles, an input converted to the matrix; YXZ is yaw (left positive), pitch (up positive), roll")]

private def quaternionProp : Json :=
  Json.mkObj [("type", "array"), ("minItems", 4), ("maxItems", 4), ("items", Json.mkObj [("type", "number")]),
              ("description", "A quaternion [x, y, z, w], an input converted to the matrix")]

private def handProp : Json :=
  Json.mkObj [("type", "string"), ("enum", Json.arr #["left", "right"]),
              ("description", "Which controller; right by default")]

def tools : Array Json := #[
  Json.mkObj [("name", "screenshot"),
    ("description", "The left eye of the OpenXR app as a PNG, with the head pose. Pixel arguments of point_at and click refer to this image."),
    ("inputSchema", schema [] [])],
  Json.mkObj [("name", "get_state"),
    ("description", "Connection, eye size and field of view, head and hand poses, held inputs and frame counts."),
    ("inputSchema", schema [] [])],
  Json.mkObj [("name", "look"),
    ("description", "Turns the head to a rotation given as exactly one of: a 3x3 matrix (canonical), Euler angles in a named Tait-Bryan order, or a quaternion. relative applies it in the head's own frame. Replies with the resulting matrix."),
    ("inputSchema", schema [("rotation", rotationProp), ("euler", eulerProp), ("quaternion", quaternionProp),
                            ("relative", prop "boolean" "Turn from the current rotation, in the head's frame")] [])],
  Json.mkObj [("name", "move"),
    ("description", "Walks the head in metres: forward along where it faces, right, and up."),
    ("inputSchema", schema [("forward", prop "number" "Metres forward"), ("right", prop "number" "Metres right"),
                            ("up", prop "number" "Metres up")] [])],
  Json.mkObj [("name", "point_at"),
    ("description", "Aims a controller through a screenshot pixel, from the eye, so its ray hits what that pixel shows."),
    ("inputSchema", schema [("x", prop "number" "Pixel column"), ("y", prop "number" "Pixel row"), ("hand", handProp)] ["x", "y"])],
  Json.mkObj [("name", "click"),
    ("description", "point_at, hover, then press and release the trigger: a click on what the pixel shows."),
    ("inputSchema", schema [("x", prop "number" "Pixel column"), ("y", prop "number" "Pixel row"), ("hand", handProp),
                            ("hold_ms", prop "integer" "How long the trigger stays pressed; 100 by default"),
                            ("hover_ms", prop "integer" "How long the aim settles before the press; 150 by default")] ["x", "y"])],
  Json.mkObj [("name", "press"),
    ("description", "Presses a button or pulls trigger or grip, holding it for hold_ms (0 holds until release_all)."),
    ("inputSchema", schema [("button", Json.mkObj [("type", "string"),
        ("enum", Json.arr #["trigger", "grip", "a", "b", "x", "y", "menu", "left_thumbstick", "right_thumbstick", "headset"])]),
      ("hand", handProp), ("hold_ms", prop "integer" "Milliseconds; 100 by default")] ["button"])],
  Json.mkObj [("name", "thumbstick"),
    ("description", "Pushes a thumbstick to x, y in -1..1 for duration_ms, then centres it (0 keeps it pushed)."),
    ("inputSchema", schema [("x", prop "number" "Left -1 to right 1"), ("y", prop "number" "Down -1 to up 1"),
                            ("hand", handProp), ("duration_ms", prop "integer" "Milliseconds; 300 by default")] ["x", "y"])],
  Json.mkObj [("name", "set_controllers"),
    ("description", "Whether the app sees controllers. Without them, apps that support it fall back to gaze and the headset button."),
    ("inputSchema", schema [("present", prop "boolean" "Controllers connected")] ["present"])],
  Json.mkObj [("name", "release_all"),
    ("description", "Lets go of every button, trigger, grip and stick and returns the hands to the sides; the head stays."),
    ("inputSchema", schema [] [])],
  Json.mkObj [("name", "wait"),
    ("description", "Waits so the app can react before the next screenshot."),
    ("inputSchema", schema [("ms", prop "integer" "Milliseconds")] ["ms"])],
  Json.mkObj [("name", "reach"),
    ("description", "The body's arm reaches for what a screenshot pixel shows, depth metres from the eye: the hand goes where the arm (ANNY proportions, two-bone elbow) can reach on the line to it, pointing along the reach with the palm level. Replies whether it is within reach."),
    ("inputSchema", schema [("x", prop "number" "Pixel column"), ("y", prop "number" "Pixel row"),
                            ("depth", prop "number" "Metres from the eye along the pixel's ray; 0.5 by default"), ("hand", handProp)] ["x", "y"])],
  Json.mkObj [("name", "grab"),
    ("description", "Picks up what a screenshot pixel shows, depth metres away: the hand reaches to 12 cm short of it, closes in, grips, and lifts. hold keeps the grip; otherwise it lets go after the lift."),
    ("inputSchema", schema [("x", prop "number" "Pixel column"), ("y", prop "number" "Pixel row"),
                            ("depth", prop "number" "Metres from the eye; 0.5 by default"), ("hand", handProp),
                            ("lift", prop "number" "Metres to lift after gripping; 0.15 by default"),
                            ("hold", prop "boolean" "Keep holding; true by default")] ["x", "y"])],
  Json.mkObj [("name", "locomote"),
    ("description", "Moves the player the ways the workspace's locomotion research names for controllers: smooth (the left stick held toward forward, back, left or right for a distance), snap_turn (the right stick flicked left or right, count times), or teleport (the left stick pushed forward to aim the arc, then released to land)."),
    ("inputSchema", schema [("mode", Json.mkObj [("type", "string"), ("enum", Json.arr #["smooth", "snap_turn", "teleport"])]),
                            ("direction", Json.mkObj [("type", "string"), ("enum", Json.arr #["forward", "back", "left", "right"]),
                                                      ("description", "smooth: where to move; snap_turn: left or right")]),
                            ("meters", prop "number" "smooth: how far, at 1.5 m/s; 1 by default"),
                            ("count", prop "integer" "snap_turn: how many turns; teleport: how many hops; 1 by default")] ["mode"])],
  Json.mkObj [("name", "plan"),
    ("description", "Runs a taskweft-style task network in one call; the last screenshot a step takes comes back with the reply. methods: {name: {params, alternatives: [{name, check, subtasks}]}}; a subtask is a method call [name, arg...] or a tool call [tool, {arguments}], with \"{param}\" substituted. check is a list of taskweft eval guards ({eval: {type: math/eq|ne|gt|ge|lt|le, a, b}}, with {pointer_get: \"/connected\"} reading get_state). Actions run for real; when one fails the method tries its next alternative from the world as it is. Replies with each action's outcome and the reason it stopped. Every method and tool shows as a span in the pilot's trace."),
    ("inputSchema", schema [("methods", Json.mkObj [("type", "object"), ("description", "Method name to {params, alternatives}")]),
                            ("todo_list", Json.mkObj [("type", "array"), ("description", "Tasks to run in order, e.g. [[\"open_door\"], [\"click\", {\"x\": 568, \"y\": 632}]]")]),
                            ("capabilities", Json.mkObj [("type", "array"), ("items", Json.mkObj [("type", "string")]),
                              ("description", "The tools this plan may use; any other is refused. All tools when absent")]),
                            ("max_steps", prop "integer" "Most actions to run; 256 by default")] ["todo_list"])]
]

private def text (s : String) : Json := Json.mkObj [("type", "text"), ("text", s)]

private def toolResult (content : Array Json) (isError : Bool := false) : Json :=
  Json.mkObj [("content", Json.arr content), ("isError", isError)]

private def ok? (reply : Json) : Bool := bool reply "ok" false

/-- Runs pilot commands in order, stopping at the first refusal. -/
private def sendAll (b : Backend) (cmds : List String) : IO (Option Json) := do
  for c in cmds do
    let r ← b.send c
    if !ok? r then return some r
  return none

private def headRotation (state : Json) : Mat3 :=
  (Mat3.ofJson? ((state.getObjValD "head").getObjValD "rotation")).getD Mat3.identity

/-- The rotation a look asks for: exactly one of matrix, Euler angles or quaternion. -/
def rotationInput (args : Json) : Except String Mat3 := do
  let given := ["rotation", "euler", "quaternion"].filter fun k => !(args.getObjValD k).isNull
  if given.length != 1 then
    throw "give exactly one of rotation (a 3x3 matrix), euler or quaternion"
  let r ← match given.head! with
    | "rotation" =>
      match Mat3.ofJson? (args.getObjValD "rotation") with
      | some r => pure r
      | none => throw "rotation must be three rows of three numbers"
    | "euler" =>
      let e := args.getObjValD "euler"
      let d := e.getObjValD "degrees"
      match fromEuler (str e "order" "") (arrNum d 0) (arrNum d 1) (arrNum d 2) with
      | some r => pure r
      | none => throw s!"euler order must be one of {eulerOrders}"
    | _ =>
      let q := args.getObjValD "quaternion"
      match fromQuaternion (arrNum q 0) (arrNum q 1) (arrNum q 2) (arrNum q 3) with
      | some r => pure r
      | none => throw "quaternion must be four numbers, not all zero"
  if !r.isRotation then throw "not a rotation: the matrix must be orthonormal with determinant +1"
  return r

private def viewFrom (state : Json) (shot : Float × Float) : View :=
  let head := state.getObjValD "head"
  let pos := head.getObjValD "position"
  let eye := state.getObjValD "eye"
  { headX := arrNum pos 0, headY := arrNum pos 1, headZ := arrNum pos 2
    rotation := headRotation state
    halfFovH := num eye "half_fov_horizontal" 50.0, halfFovV := num eye "half_fov_vertical" 50.0
    ipd := num eye "ipd" 0.064
    width := if shot.1 > 0.0 then shot.1 else num eye "width" 1.0
    height := if shot.2 > 0.0 then shot.2 else num eye "height" 1.0 }

private def pointCommand (b : Backend) (args : Json) : IO (Except Json (String × Aim)) := do
  if (← b.lastShot.get).1 <= 0.0 then
    return .error (Json.mkObj [("ok", false), ("error", "take a screenshot first: pixel coordinates refer to it")])
  let state ← b.send "state"
  if !ok? state then return .error state
  let hand := str args "hand" "right"
  let aim := aimThroughPixel (viewFrom state (← b.lastShot.get)) (num args "x" 0.0) (num args "y" 0.0)
  return .ok (s!"hand {hand} {fmt aim.origin.x} {fmt aim.origin.y} {fmt aim.origin.z} {aim.rotation.args}", aim)

/-- A tool call's arguments for its span: one line, at most 160 characters. -/
def spanDetail (args : Json) : String :=
  let shown := if args.isNull then "" else (args.compress.replace "\n" " ").replace "\r" " "
  if shown.length > 160 then (shown.take 157).toString ++ "..." else shown

private def runTool (b : Backend) (name : String) (args : Json) (call : String → Json → IO Json) : IO Json := do
  let refused (r : Json) : Json := toolResult #[text r.compress] true
  match name with
  | "screenshot" =>
    let r ← b.send s!"screenshot {b.screenshotPath}"
    if !ok? r then return refused r
    b.lastShot.set (num r "width" 0.0, num r "height" 0.0)
    let png ← b.readFile b.screenshotPath
    let state ← b.send "state"
    return toolResult #[Json.mkObj [("type", "image"), ("data", base64 png), ("mimeType", "image/png")],
                        text (Json.mkObj [("width", r.getObjValD "width"), ("height", r.getObjValD "height"),
                                          ("head", state.getObjValD "head"), ("eye", state.getObjValD "eye")]).compress]
  | "get_state" =>
    let r ← b.send "state"
    return toolResult #[text r.compress] (!ok? r)
  | "look" =>
    let state ← b.send "state"
    if !ok? state then return refused state
    let pos := (state.getObjValD "head").getObjValD "position"
    match rotationInput args with
    | .error e => return refused (Json.mkObj [("ok", false), ("error", e)])
    | .ok input =>
      let r := if bool args "relative" false then (headRotation state).mul input else input
      let reply ← b.send s!"head {fmt (arrNum pos 0)} {fmt (arrNum pos 1)} {fmt (arrNum pos 2)} {r.args}"
      if !ok? reply then return refused reply
      return toolResult #[text (Json.mkObj [("ok", true), ("rotation", r.toJson)]).compress]
  | "move" =>
    let state ← b.send "state"
    if !ok? state then return refused state
    let pos := (state.getObjValD "head").getObjValD "position"
    let rotation := headRotation state
    let d := walk rotation (num args "forward" 0.0) (num args "right" 0.0) (num args "up" 0.0)
    let r ← b.send s!"head {fmt (arrNum pos 0 + d.x)} {fmt (arrNum pos 1 + d.y)} {fmt (arrNum pos 2 + d.z)} {rotation.args}"
    return toolResult #[text r.compress] (!ok? r)
  | "point_at" =>
    match ← pointCommand b args with
    | .error r => return refused r
    | .ok (cmd, aim) =>
      let r ← b.send cmd
      return toolResult #[text (Json.mkObj [("ok", ok? r), ("rotation", aim.rotation.toJson)]).compress] (!ok? r)
  | "click" =>
    match ← pointCommand b args with
    | .error r => return refused r
    | .ok (cmd, aim) =>
      let hand := str args "hand" "right"
      if let some r ← sendAll b [cmd] then return refused r
      b.sleepMs (num args "hover_ms" 150.0).toUInt64.toNat
      if let some r ← sendAll b [s!"trigger {hand} 1"] then return refused r
      b.sleepMs (num args "hold_ms" 100.0).toUInt64.toNat
      if let some r ← sendAll b [s!"trigger {hand} 0"] then return refused r
      return toolResult #[text (Json.mkObj [("ok", true), ("hand", hand), ("rotation", aim.rotation.toJson)]).compress]
  | "press" =>
    let button := str args "button" ""
    let hand := str args "hand" "right"
    let (down, up) :=
      if button == "trigger" || button == "grip" then (s!"{button} {hand} 1", s!"{button} {hand} 0")
      else (s!"button {button} 1", s!"button {button} 0")
    if let some r ← sendAll b [down] then return refused r
    let hold := (num args "hold_ms" 100.0).toUInt64.toNat
    if hold > 0 then
      b.sleepMs hold
      if let some r ← sendAll b [up] then return refused r
    return toolResult #[text s!"pressed {button}"]
  | "thumbstick" =>
    let hand := str args "hand" "right"
    if let some r ← sendAll b [s!"stick {hand} {fmt (num args "x" 0.0)} {fmt (num args "y" 0.0)}"] then return refused r
    let duration := (num args "duration_ms" 300.0).toUInt64.toNat
    if duration > 0 then
      b.sleepMs duration
      if let some r ← sendAll b [s!"stick {hand} 0 0"] then return refused r
    return toolResult #[text "moved the thumbstick"]
  | "set_controllers" =>
    let v := if bool args "present" true then "1" else "0"
    if let some r ← sendAll b [s!"present left {v}", s!"present right {v}"] then return refused r
    return toolResult #[text s!"controllers present: {v == "1"}"]
  | "release_all" =>
    let r ← b.send "release"
    return toolResult #[text r.compress] (!ok? r)
  | "wait" =>
    b.sleepMs (num args "ms" 0.0).toUInt64.toNat
    return toolResult #[text "waited"]
  | "reach" | "grab" =>
    if (← b.lastShot.get).1 <= 0.0 then
      return refused (Json.mkObj [("ok", false), ("error", "take a screenshot first: pixel coordinates refer to it")])
    let state ← b.send "state"
    if !ok? state then return refused state
    let hand := str args "hand" "right"
    let aim := aimThroughPixel (viewFrom state (← b.lastShot.get)) (num args "x" 0.0) (num args "y" 0.0)
    let depth := max 0.1 (num args "depth" 0.5)
    let toward (d up : Float) : String :=
      s!"reach {hand} {fmt (aim.origin.x + aim.direction.x * d)} {fmt (aim.origin.y + aim.direction.y * d + up)} {fmt (aim.origin.z + aim.direction.z * d)}"
    if name == "reach" then
      let r ← b.send (toward depth 0.0)
      return toolResult #[text r.compress] (!ok? r)
    let lift := num args "lift" 0.15
    let pre ← b.send (toward (max 0.1 (depth - 0.12)) 0.0)
    if !ok? pre then return refused pre
    b.sleepMs 250
    let reached ← b.send (toward depth 0.0)
    if !ok? reached then return refused reached
    b.sleepMs 200
    if let some r ← sendAll b [s!"grip {hand} 1"] then return refused r
    b.sleepMs 300
    let lifted ← b.send (toward depth lift)
    if !bool args "hold" true then
      b.sleepMs 200
      if let some r ← sendAll b [s!"grip {hand} 0"] then return refused r
    return toolResult #[text (Json.mkObj [("ok", true), ("reachable", reached.getObjValD "reachable"),
                                          ("hand", hand), ("lifted", lifted.getObjValD "hand")]).compress]
  | "locomote" =>
    let count := (num args "count" 1.0).toUInt64.toNat
    match str args "mode" "" with
    | "smooth" =>
      let (x, y) := match str args "direction" "forward" with
        | "back" => (0.0, -1.0) | "left" => (-1.0, 0.0) | "right" => (1.0, 0.0) | _ => (0.0, 1.0)
      if let some r ← sendAll b [s!"stick left {fmt x} {fmt y}"] then return refused r
      b.sleepMs ((num args "meters" 1.0) / 1.5 * 1000.0).toUInt64.toNat
      if let some r ← sendAll b ["stick left 0 0"] then return refused r
      return toolResult #[text s!"moved {fmt (num args "meters" 1.0)} m {str args "direction" "forward"}"]
    | "snap_turn" =>
      let x := if str args "direction" "right" == "left" then "-1" else "1"
      for _ in [0:count] do
        if let some r ← sendAll b [s!"stick right {x} 0"] then return refused r
        b.sleepMs 150
        if let some r ← sendAll b ["stick right 0 0"] then return refused r
        b.sleepMs 250
      return toolResult #[text s!"snap turned {str args "direction" "right"} x{count}"]
    | "teleport" =>
      for _ in [0:count] do
        if let some r ← sendAll b ["stick left 0 1"] then return refused r
        b.sleepMs 800
        if let some r ← sendAll b ["stick left 0 0"] then return refused r
        b.sleepMs 600
      return toolResult #[text s!"teleported x{count}"]
    | other => return refused (Json.mkObj [("ok", false), ("error", s!"mode must be smooth, snap_turn or teleport, got {other}")])
  | "plan" =>
    -- The last image a step returned comes back with the plan's reply, so a plan can end on a screenshot.
    let lastImage ← IO.mkRef (none : Option Json)
    let keepImage := fun (name : String) (stepArgs : Json) => do
      let result ← call name stepArgs
      match result.getObjValD "content" with
      | .arr items =>
        for item in items do
          if str item "type" "" == "image" then lastImage.set (some item)
      | _ => pure ()
      return result
    let actor : Actor := {
      tool := keepImage
      state := b.send "state"
      mark := fun line => do let _ ← b.send line
      isTool := fun n => n != "plan" && tools.any (fun t => str t "name" "" == n) }
    let limits : PlanLimits := { maxSteps := (num args "max_steps" 256.0).toUInt64.toNat }
    let trace ← runPlan actor args limits
    let reply := Json.mkObj [("ok", trace.failure.isNone), ("steps", Json.arr trace.steps),
                             ("failure", match trace.failure with | some f => Json.str f | none => Json.null)]
    let content := match ← lastImage.get with
      | some image => #[text reply.compress, image]
      | none => #[text reply.compress]
    return toolResult content trace.failure.isSome
  | other => throw (IO.userError s!"unknown tool: {other}")

/-- Runs a tool inside a span, so the pilot's trace shows the call, its commands, its time and its outcome. -/
partial def callTool (b : Backend) (name : String) (args : Json) : IO Json := do
  let id := toString (← IO.monoNanosNow)
  let _ ← b.send s!"span begin {id} {name} {spanDetail args}"
  try
    let result ← runTool b name args (callTool b)
    let failed := bool result "isError" false
    let _ ← b.send s!"span end {id} {if failed then "error" else "ok"}"
    return result
  catch e =>
    let _ ← b.send s!"span end {id} error"
    throw e

private def response (id : Json) (result : Json) : Json :=
  Json.mkObj [("jsonrpc", "2.0"), ("id", id), ("result", result)]

private def errorResponse (id : Json) (code : Int) (message : String) : Json :=
  Json.mkObj [("jsonrpc", "2.0"), ("id", id), ("error", Json.mkObj [("code", code), ("message", message)])]

/-- Handles one line; none for a notification, which gets no reply. -/
def handleLine (b : Backend) (line : String) : IO (Option Json) := do
  match Json.parse line with
  | .error e => return some (errorResponse Json.null (-32700) s!"parse error: {e}")
  | .ok msg =>
    let id := msg.getObjValD "id"
    let method := str msg "method" ""
    let params := msg.getObjValD "params"
    if id.isNull then return none
    match method with
    | "initialize" =>
      return some (response id (Json.mkObj [
        ("protocolVersion", protocolVersion),
        ("capabilities", Json.mkObj [("tools", Json.mkObj [])]),
        ("serverInfo", Json.mkObj [("name", "xr-pilot"), ("version", "0.1.0")])]))
    | "ping" => return some (response id (Json.mkObj []))
    | "tools/list" => return some (response id (Json.mkObj [("tools", Json.arr tools)]))
    | "tools/call" =>
      let name := str params "name" ""
      if !(tools.any fun t => str t "name" "" == name) then
        return some (errorResponse id (-32602) s!"unknown tool: {name}")
      try
        return some (response id (← callTool b name (params.getObjValD "arguments")))
      catch e =>
        return some (response id (toolResult #[text (toString e)] true))
    | other => return some (errorResponse id (-32601) s!"method not found: {other}")

end XrPilot
