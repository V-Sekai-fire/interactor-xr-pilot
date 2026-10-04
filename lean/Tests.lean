/-
Tests for the ray maths and the MCP protocol, against a fake pilot. Each negative case asserts that
broken input or a broken implementation is caught.

SPDX-License-Identifier: Apache-2.0 OR MIT
-/
import XrPilot
open Lean XrPilot

def view : View :=
  { headX := 1.0, headY := 1.6, headZ := -2.0, yaw := 30.0, pitch := -10.0,
    halfFovH := 47.0, halfFovV := 50.0, ipd := 0.064, width := 1136.0, height := 1264.0 }

def near (a b tol : Float) : Bool := Float.abs (a - b) ≤ tol

/-- Where a ray of the given yaw and pitch crosses the image, by projecting it back through the eye. -/
def projectBack (v : View) (yaw pitch : Float) : Float × Float :=
  let p := degToRad pitch
  let y := degToRad yaw
  let d : Vec3 := { x := -Float.cos p * Float.sin y, y := Float.sin p, z := -Float.cos p * Float.cos y }
  -- Undo the head turn: yaw back, then pitch back.
  let hy := degToRad (-v.yaw)
  let x1 := d.x * Float.cos hy + d.z * Float.sin hy
  let z1 := -d.x * Float.sin hy + d.z * Float.cos hy
  let hp := degToRad (-v.pitch)
  let y2 := d.y * Float.cos hp - z1 * Float.sin hp
  let z2 := d.y * Float.sin hp + z1 * Float.cos hp
  let nx := x1 / (-z2) / Float.tan (degToRad v.halfFovH)
  let ny := y2 / (-z2) / Float.tan (degToRad v.halfFovV)
  ((nx + 1.0) * v.width / 2.0, (1.0 - ny) * v.height / 2.0)

structure Fake where
  sent : IO.Ref (Array String)
  backend : Backend

def stateReply : Json :=
  match Json.parse "{\"ok\":true,\"connected\":true,\"eye\":{\"width\":1512,\"height\":1680,\"half_fov_horizontal\":47,\"half_fov_vertical\":50,\"ipd\":0.064},\"head\":{\"position\":[0,1.6,0],\"yaw\":10,\"pitch\":0,\"roll\":0}}" with
  | .ok j => j
  | .error _ => Json.null

def fake : IO Fake := do
  let sent ← IO.mkRef #[]
  return { sent,
           backend := { send := fun c => do
                          sent.modify (·.push c)
                          if c == "state" then return stateReply
                          if c.startsWith "teleport" then return Json.mkObj [("ok", false), ("error", "unknown command")]
                          return Json.mkObj [("ok", true)]
                        readFile := fun _ => pure (ByteArray.mk #[1, 2, 3])
                        sleepMs := fun _ => pure ()
                        screenshotPath := "unused.png"
                        lastShot := ← IO.mkRef (1136.0, 1264.0) } }

def code (reply : Json) : Int :=
  match ((reply.getObjValD "error").getObjValD "code").getInt? with | .ok n => n | .error _ => 0

def isError (reply : Json) : Bool :=
  match ((reply.getObjValD "result").getObjValD "isError").getBool? with | .ok b => b | .error _ => true

def check (failures : IO.Ref Nat) (ok : Bool) (what : String) : IO Unit := do
  if !ok then
    IO.eprintln s!"FAIL {what}"
    failures.modify (· + 1)

def main : IO UInt32 := do
  let failures ← IO.mkRef 0
  let c := check failures

  -- The centre pixel looks where the head looks.
  let centre := aimThroughPixel view (view.width / 2.0) (view.height / 2.0)
  c (near centre.yaw 30.0 1e-6 && near centre.pitch (-10.0) 1e-6) "the centre pixel aims along the gaze"

  -- Every probed pixel's ray projects back onto that pixel, within 0.1 pixel.
  for (px, py) in [(0.0, 0.0), (1136.0, 0.0), (0.0, 1264.0), (1136.0, 1264.0), (200.0, 900.0), (1000.0, 100.0)] do
    let aim := aimThroughPixel view px py
    let (bx, by_) := projectBack view aim.yaw aim.pitch
    c (near bx px 0.1 && near by_ py 0.1) s!"pixel ({px}, {py}) round-trips, got ({bx}, {by_})"

  -- With the head level, the right edge is half the horizontal FOV to the right.
  let level := { view with yaw := 0.0, pitch := 0.0 }
  let edge := aimThroughPixel level level.width (level.height / 2.0)
  c (near edge.yaw (-47.0) 0.1 && near edge.pitch 0.0 0.1) s!"the right edge is at -47 degrees yaw, got {edge.yaw}"

  -- Control: horizontal and vertical FOV swapped must miss that edge.
  let swapped := aimThroughPixel { level with halfFovH := level.halfFovV, halfFovV := level.halfFovH } level.width (level.height / 2.0)
  c (!near swapped.yaw (-47.0) 0.1) "a swapped FOV is caught"

  -- The aim starts at the left eye, half an IPD left of the head.
  c (near centre.origin.y 1.6 1e-6 && near (Float.sqrt ((centre.origin.x - 1.0) ^ 2 + (centre.origin.z + 2.0) ^ 2)) 0.032 1e-6)
    "the aim starts at the left eye"

  -- Walking forward with yaw 90 goes toward -X.
  let w := walk 90.0 1.0 0.0 0.0
  c (near w.x (-1.0) 1e-6 && near w.z 0.0 1e-6) "forward at yaw 90 is -X"

  let b64 := base64 "Man is".toUTF8
  c (b64 == "TWFuIGlz") s!"base64 of 'Man is', got {b64}"
  c (base64 "Ma".toUTF8 == "TWE=" && base64 "M".toUTF8 == "TQ==") "base64 pads"

  -- Protocol.
  let f ← fake
  let call (line : String) : IO Json := do
    match ← handleLine f.backend line with
    | some j => return j
    | none => return Json.null
  let listed ← call "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/list\"}"
  let names := match ((listed.getObjValD "result").getObjValD "tools").getArr? with
    | .ok ts => ts.map (fun (t : Json) => match (t.getObjValD "name").getStr? with | .ok s => s | .error _ => "")
    | .error _ => #[]
  c (names.size == 11 && names.contains "click" && names.contains "screenshot") s!"tools/list lists 11 tools, got {names}"

  let unknown ← call "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/call\",\"params\":{\"name\":\"teleport\"}}"
  c (code unknown == -32602) "an unknown tool is a JSON-RPC error"

  let bad ← call "{not json"
  c (code bad == -32700) "malformed JSON is a parse error"

  let note ← handleLine f.backend "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}"
  c note.isNone "a notification gets no reply"

  f.sent.set #[]
  let clicked ← call "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"tools/call\",\"params\":{\"name\":\"click\",\"arguments\":{\"x\":568,\"y\":632}}}"
  let sent ← f.sent.get
  c (sent.size == 4 && sent[0]! == "state" && (sent[1]!).startsWith "hand right" && sent[2]! == "trigger right 1" && sent[3]! == "trigger right 0")
    s!"click aims, then presses and releases the trigger, sent {sent}"
  c (!isError clicked) "click succeeds"

  f.sent.set #[]
  let pressed ← call "{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"tools/call\",\"params\":{\"name\":\"press\",\"arguments\":{\"button\":\"a\"}}}"
  c ((← f.sent.get) == #["button a 1", "button a 0"]) "press holds and releases a button"
  c (!isError pressed) "press succeeds"

  f.backend.lastShot.set (0.0, 0.0)
  f.sent.set #[]
  let early ← call "{\"jsonrpc\":\"2.0\",\"id\":5,\"method\":\"tools/call\",\"params\":{\"name\":\"click\",\"arguments\":{\"x\":1,\"y\":1}}}"
  c (isError early && (← f.sent.get).isEmpty) "pixel tools refuse before any screenshot, sending nothing"

  let n ← failures.get
  if n == 0 then IO.println "all tests passed" else IO.eprintln s!"{n} failure(s)"
  return if n == 0 then 0 else 1
