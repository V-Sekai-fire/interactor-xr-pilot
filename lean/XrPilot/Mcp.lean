/-
The MCP server: JSON-RPC 2.0 over stdio, one message per line. Each tool turns into commands on the
pilot's line protocol (core/include/xrpilot/Commands.h) through a Backend, so the protocol logic is
tested against a fake pilot.

SPDX-License-Identifier: Apache-2.0 OR MIT
-/
import Lean.Data.Json
import XrPilot.Base64
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
    ("description", "Turns the head. Degrees; yaw positive turns left, pitch positive looks up. relative adds to the current pose."),
    ("inputSchema", schema [("yaw", prop "number" "Yaw in degrees"), ("pitch", prop "number" "Pitch in degrees"),
                            ("relative", prop "boolean" "Add to the current pose")] [])],
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
    ("inputSchema", schema [("ms", prop "integer" "Milliseconds")] ["ms"])]
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

private def viewFrom (state : Json) (shot : Float × Float) : View :=
  let head := state.getObjValD "head"
  let pos := head.getObjValD "position"
  let eye := state.getObjValD "eye"
  { headX := arrNum pos 0, headY := arrNum pos 1, headZ := arrNum pos 2
    yaw := num head "yaw" 0.0, pitch := num head "pitch" 0.0
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
  return .ok (s!"hand {hand} {fmt aim.origin.x} {fmt aim.origin.y} {fmt aim.origin.z} {fmt aim.yaw} {fmt aim.pitch}", aim)

def callTool (b : Backend) (name : String) (args : Json) : IO Json := do
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
    let head := state.getObjValD "head"
    let pos := head.getObjValD "position"
    let rel := bool args "relative" false
    let yaw := num args "yaw" 0.0 + (if rel then num head "yaw" 0.0 else 0.0)
    let pitch := num args "pitch" 0.0 + (if rel then num head "pitch" 0.0 else 0.0)
    let r ← b.send s!"head {fmt (arrNum pos 0)} {fmt (arrNum pos 1)} {fmt (arrNum pos 2)} {fmt yaw} {fmt pitch} 0"
    return toolResult #[text r.compress] (!ok? r)
  | "move" =>
    let state ← b.send "state"
    if !ok? state then return refused state
    let head := state.getObjValD "head"
    let pos := head.getObjValD "position"
    let d := walk (num head "yaw" 0.0) (num args "forward" 0.0) (num args "right" 0.0) (num args "up" 0.0)
    let r ← b.send s!"head {fmt (arrNum pos 0 + d.x)} {fmt (arrNum pos 1 + d.y)} {fmt (arrNum pos 2 + d.z)} {fmt (num head "yaw" 0.0)} {fmt (num head "pitch" 0.0)} 0"
    return toolResult #[text r.compress] (!ok? r)
  | "point_at" =>
    match ← pointCommand b args with
    | .error r => return refused r
    | .ok (cmd, aim) =>
      let r ← b.send cmd
      return toolResult #[text s!"aimed yaw {fmt aim.yaw} pitch {fmt aim.pitch}; {r.compress}"] (!ok? r)
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
      return toolResult #[text s!"clicked with the {hand} trigger at yaw {fmt aim.yaw} pitch {fmt aim.pitch}"]
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
  | other => throw (IO.userError s!"unknown tool: {other}")

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
