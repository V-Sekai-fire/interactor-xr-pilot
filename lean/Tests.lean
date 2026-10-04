/-
Tests for the ray maths and the MCP protocol, against a fake pilot. Each negative case asserts that
broken input or a broken implementation is caught.

SPDX-License-Identifier: Apache-2.0 OR MIT
-/
import XrPilot
open Lean XrPilot

def near (a b tol : Float) : Bool := Float.abs (a - b) ≤ tol

def yawPitch (yaw pitch : Float) : Mat3 := (fromEuler "YXZ" yaw pitch 0.0).getD Mat3.identity

def view : View :=
  { headX := 1.0, headY := 1.6, headZ := -2.0, rotation := yawPitch 30.0 (-10.0),
    halfFovH := 47.0, halfFovV := 50.0, ipd := 0.064, width := 1136.0, height := 1264.0 }

def matNear (a b : Mat3) (tol : Float) : Bool := (List.range 9).all fun i => near a.m[i]! b.m[i]! tol

/-- Where a world direction crosses the image, by undoing the head's rotation and projecting. -/
def projectBack (v : View) (d : Vec3) : Float × Float :=
  let l := v.rotation.transpose.apply d
  let nx := l.x / (-l.z) / Float.tan (degToRad v.halfFovH)
  let ny := l.y / (-l.z) / Float.tan (degToRad v.halfFovV)
  ((nx + 1.0) * v.width / 2.0, (1.0 - ny) * v.height / 2.0)

/-- The hand's pointing direction: the -Z column of its rotation. -/
def pointing (r : Mat3) : Vec3 := { x := -r.get 0 2, y := -r.get 1 2, z := -r.get 2 2 }

structure Fake where
  sent : IO.Ref (Array String)
  marks : IO.Ref (Array String)
  backend : Backend

def stateReply : Json :=
  match Json.parse "{\"ok\":true,\"connected\":true,\"eye\":{\"width\":1512,\"height\":1680,\"half_fov_horizontal\":47,\"half_fov_vertical\":50,\"ipd\":0.064},\"head\":{\"position\":[0,1.6,0],\"rotation\":[[1,0,0],[0,1,0],[0,0,1]]}}" with
  | .ok j => j
  | .error _ => Json.null

def fake : IO Fake := do
  let sent ← IO.mkRef #[]
  let marks ← IO.mkRef #[]
  return { sent, marks,
           backend := { send := fun c => do
                          if c.startsWith "span " then
                            marks.modify (·.push c)
                            return Json.mkObj [("ok", true)]
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

  -- The centre pixel looks where the head looks, and the hand's rotation matrix is the head's.
  let centre := aimThroughPixel view (view.width / 2.0) (view.height / 2.0)
  c (matNear centre.rotation view.rotation 1e-5) "the centre pixel aims along the gaze"
  c (centre.rotation.isRotation) "an aim is a rotation"

  -- Every probed pixel's ray, read back from the hand's matrix, projects onto that pixel within 0.1 pixel.
  for (px, py) in [(0.0, 0.0), (1136.0, 0.0), (0.0, 1264.0), (1136.0, 1264.0), (200.0, 900.0), (1000.0, 100.0)] do
    let aim := aimThroughPixel view px py
    let (bx, by_) := projectBack view (pointing aim.rotation)
    c (near bx px 0.1 && near by_ py 0.1) s!"pixel ({px}, {py}) round-trips, got ({bx}, {by_})"

  -- With the head level, the right edge is half the horizontal FOV to the right.
  let level := { view with rotation := Mat3.identity }
  let edge := pointing (aimThroughPixel level level.width (level.height / 2.0)).rotation
  let edgeYaw := radToDeg (Float.atan2 (-edge.x) (-edge.z))
  c (near edgeYaw (-47.0) 0.1 && near edge.y 0.0 1e-5) s!"the right edge is at -47 degrees yaw, got {edgeYaw}"

  -- Control: horizontal and vertical FOV swapped must miss that edge.
  let swapped := pointing (aimThroughPixel { level with halfFovH := level.halfFovV, halfFovV := level.halfFovH } level.width (level.height / 2.0)).rotation
  c (!near (radToDeg (Float.atan2 (-swapped.x) (-swapped.z))) (-47.0) 0.1) "a swapped FOV is caught"

  -- Every Tait-Bryan order gives a rotation; the same angles in two orders differ; others are refused.
  c (eulerOrders.all fun o => match fromEuler o 33.0 (-71.0) 12.0 with | some r => r.isRotation | none => false)
    "all six Tait-Bryan orders give rotations"
  c (!matNear ((fromEuler "YXZ" 40.0 (-25.0) 10.0).getD Mat3.identity) ((fromEuler "ZXY" 10.0 (-25.0) 40.0).getD Mat3.identity) 1e-3)
    "YXZ and ZXY with the same angles differ"
  c ((fromEuler "XYX" 1.0 2.0 3.0).isNone && (fromEuler "yxz" 1.0 2.0 3.0).isNone) "proper Euler and lower case are refused"
  -- A quaternion of a turn about +Y matches the YXZ yaw of the same angle.
  let half := degToRad 35.0 / 2.0
  c (matNear ((fromQuaternion 0.0 (Float.sin half) 0.0 (Float.cos half)).getD Mat3.identity) (yawPitch 35.0 0.0) 1e-5)
    "a quaternion converts to the matching matrix"
  c (!(⟨#[1, 0, 0, 0, 1, 0, 0, 0, -1]⟩ : Mat3).isRotation && !(⟨#[2, 0, 0, 0, 1, 0, 0, 0, 1]⟩ : Mat3).isRotation)
    "a mirror and a scale are not rotations"

  -- The aim starts at the left eye, half an IPD left of the head.
  c (near centre.origin.y 1.6 1e-6 && near (Float.sqrt ((centre.origin.x - 1.0) ^ 2 + (centre.origin.z + 2.0) ^ 2)) 0.032 1e-6)
    "the aim starts at the left eye"

  -- Walking forward with yaw 90 goes toward -X; looking straight down, forward is the top of the view.
  let w := walk (yawPitch 90.0 0.0) 1.0 0.0 0.0
  c (near w.x (-1.0) 1e-6 && near w.z 0.0 1e-6) "forward at yaw 90 is -X"
  let down := walk (yawPitch 0.0 (-90.0)) 1.0 0.0 0.0
  c (near down.z (-1.0) 1e-5 && near down.x 0.0 1e-5) "forward looking down is still -Z"

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
  c (names.size == 15 && names.contains "reach" && names.contains "grab" && names.contains "locomote")
    s!"tools/list lists 15 tools, got {names}"
  -- A client sends only the properties a tool declares, so every field plan reads must be in its schema.
  let planSchema := match ((listed.getObjValD "result").getObjValD "tools").getArr? with
    | .ok ts => (ts.find? (fun (t : Json) => (t.getObjValD "name").getStr?.toOption == some "plan")).getD Json.null
    | .error _ => Json.null
  let declared := (planSchema.getObjValD "inputSchema").getObjValD "properties"
  c (["methods", "todo_list", "capabilities", "max_steps"].all fun k => !(declared.getObjValD k).isNull)
    "plan's schema declares every field it reads"

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

  -- look sends the matrix, from any one input, and refuses everything else before sending.
  let lookWith (args : String) : IO (Json × Array String) := do
    f.sent.set #[]
    let r ← call s!"\{\"jsonrpc\":\"2.0\",\"id\":6,\"method\":\"tools/call\",\"params\":\{\"name\":\"look\",\"arguments\":{args}}}"
    return (r, ← f.sent.get)
  let yaw90 := (yawPitch 90.0 0.0).args
  let (r1, s1) ← lookWith "{\"euler\":{\"order\":\"YXZ\",\"degrees\":[90,0,0]}}"
  let expected := s!"head {(0.0 : Float)} {(1.6 : Float)} {(0.0 : Float)} {yaw90}"
  c (!isError r1 && s1.size == 2 && s1[1]! == expected) s!"euler YXZ sends the matrix, sent {s1}"
  let q := Float.sqrt 0.5
  let (r2, s2) ← lookWith s!"\{\"quaternion\":[0,{q},0,{q}]}"
  c (!isError r2 && s2.size == 2 && s2[1]! == expected) s!"a quaternion about +Y sends the same matrix as yaw 90, sent {s2}"
  let (r3, _) ← lookWith "{\"rotation\":[[0,0,1],[0,1,0],[-1,0,0]]}"
  c (!isError r3) "a rotation matrix is accepted"
  for bad in ["{}", "{\"rotation\":[[1,0,0],[0,1,0],[0,0,-1]]}", "{\"rotation\":[[2,0,0],[0,1,0],[0,0,1]]}",
              "{\"euler\":{\"order\":\"XYX\",\"degrees\":[1,2,3]}}", "{\"quaternion\":[0,0,0,0]}",
              "{\"euler\":{\"order\":\"YXZ\",\"degrees\":[1,2,3]},\"quaternion\":[0,0,0,1]}"] do
    let (r, sent) ← lookWith bad
    c (isError r && sent.size == 1) s!"look refuses {bad} and sends no head, sent {sent}"

  -- Each tool call is one span: a begin naming the tool, then an end with its outcome.
  f.marks.set #[]
  let _ ← call "{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"tools/call\",\"params\":{\"name\":\"press\",\"arguments\":{\"button\":\"b\"}}}"
  let marks ← f.marks.get
  c (marks.size == 2 && (marks[0]!).startsWith "span begin " && ((marks[0]!).splitOn " ")[3]! == "press" &&
      (marks[1]!).startsWith "span end " && (marks[1]!).endsWith " ok")
    s!"a tool call opens and closes one span, marked {marks}"
  c ((spanDetail (Json.str (String.ofList (List.replicate 400 (Char.ofNat 120))))).length ≤ 160) "a long argument list is cut to one short line"

  -- plan: a method falls through to its next alternative when an action fails, and a check gates one.
  let planCall (body : String) : IO Json := call s!"\{\"jsonrpc\":\"2.0\",\"id\":8,\"method\":\"tools/call\",\"params\":\{\"name\":\"plan\",\"arguments\":{body}}}"
  f.sent.set #[]
  let fallthrough ← planCall "{\"methods\":{\"poke\":{\"params\":[\"b\"],\"alternatives\":[{\"name\":\"broken\",\"subtasks\":[[\"teleport\",{}]]},{\"name\":\"press_it\",\"subtasks\":[[\"press\",{\"button\":\"{b}\"}]]}]}},\"todo_list\":[[\"poke\",\"a\"]]}"
  c (!isError fallthrough && (← f.sent.get) == #["button a 1", "button a 0"])
    s!"plan falls through to the alternative that works and substitutes its parameter, sent {← f.sent.get}"
  f.sent.set #[]
  let gated ← planCall "{\"methods\":{\"poke\":{\"alternatives\":[{\"name\":\"offline\",\"check\":[{\"eval\":{\"type\":\"math/eq\",\"a\":{\"pointer_get\":\"/connected\"},\"b\":false}}],\"subtasks\":[[\"press\",{\"button\":\"x\"}]]},{\"name\":\"online\",\"check\":[{\"eval\":{\"type\":\"math/eq\",\"a\":{\"pointer_get\":\"/connected\"},\"b\":true}}],\"subtasks\":[[\"press\",{\"button\":\"y\"}]]}]}},\"todo_list\":[[\"poke\"]]}"
  c (!isError gated && ((← f.sent.get).filter (·.startsWith "button")) == #["button y 1", "button y 0"])
    s!"a false check skips its alternative and a true one runs, sent {← f.sent.get}"
  let none ← planCall "{\"methods\":{},\"todo_list\":[[\"nothing_here\"]]}"
  c (isError none) "control: a task that is neither a tool nor a method fails the plan"
  let fuel ← planCall "{\"methods\":{\"loop\":{\"alternatives\":[{\"name\":\"again\",\"subtasks\":[[\"wait\",{\"ms\":0}],[\"loop\"]]}]}},\"todo_list\":[[\"loop\"]],\"max_steps\":5}"
  c (isError fuel) "control: a plan that never ends runs out of fuel instead of hanging"
  c (evalCheck (Json.mkObj [("frames", Json.mkObj [("decoded", (12 : Nat))])])
      (Json.mkObj [("eval", Json.mkObj [("type", "math/gt"), ("a", Json.mkObj [("pointer_get", "/frames/decoded")]), ("b", (10 : Nat))])]))
    "a pointer reads a nested number for a comparison"
  f.sent.set #[]
  let met ← planCall "{\"methods\":{},\"todo_list\":[{\"goal\":[{\"pointer\":\"/connected\",\"eq\":true}]}]}"
  c (!isError met && ((← f.sent.get).filter (· != "state")).isEmpty) "a goal already met runs nothing"
  let unreachable ← planCall "{\"methods\":{\"connected\":{\"params\":[\"want\"],\"alternatives\":[{\"name\":\"wait_for_it\",\"subtasks\":[[\"wait\",{\"ms\":0}]]}]}},\"todo_list\":[{\"goal\":[{\"pointer\":\"/connected\",\"eq\":false}]}]}"
  c (isError unreachable) "control: a goal its method does not reach fails the plan"
  f.sent.set #[]
  let refusedCap ← planCall "{\"capabilities\":[\"wait\"],\"methods\":{},\"todo_list\":[[\"press\",{\"button\":\"a\"}]]}"
  c (isError refusedCap && ((← f.sent.get).filter (·.startsWith "button")).isEmpty)
    "control: an action outside the capabilities is refused before it runs"
  let timed ← planCall "{\"capabilities\":[\"wait\"],\"methods\":{},\"todo_list\":[[\"wait\",{\"ms\":0}]]}"
  let firstContent := (((timed.getObjValD "result").getObjValD "content").getArrVal? 0).toOption.getD Json.null
  let timedText := ((firstContent.getObjValD "text").getStr?).toOption.getD ""
  c (!isError timed && (timedText.splitOn "duration_ms").length > 1 && (timedText.splitOn "start_ms").length > 1)
    s!"each action records when it started and how long it took, got {timedText}"

  -- grab: reach short, reach in, grip, lift; locomote: the stick sequences of each mode.
  f.sent.set #[]
  let grabbed ← call "{\"jsonrpc\":\"2.0\",\"id\":9,\"method\":\"tools/call\",\"params\":{\"name\":\"grab\",\"arguments\":{\"x\":568,\"y\":632,\"depth\":0.5,\"hold\":false}}}"
  let g := (← f.sent.get).filter (· != "state")
  c (!isError grabbed && g.size == 5 && (g[0]!).startsWith "reach right" && (g[1]!).startsWith "reach right" &&
      g[2]! == "grip right 1" && (g[3]!).startsWith "reach right" && g[4]! == "grip right 0")
    s!"grab reaches short, reaches in, grips, lifts and lets go, sent {g}"
  f.sent.set #[]
  let turned ← call "{\"jsonrpc\":\"2.0\",\"id\":10,\"method\":\"tools/call\",\"params\":{\"name\":\"locomote\",\"arguments\":{\"mode\":\"snap_turn\",\"direction\":\"left\",\"count\":2}}}"
  c (!isError turned && (← f.sent.get) == #["stick right -1 0", "stick right 0 0", "stick right -1 0", "stick right 0 0"])
    s!"two snap turns left flick the right stick twice, sent {← f.sent.get}"
  f.sent.set #[]
  let hop ← call "{\"jsonrpc\":\"2.0\",\"id\":11,\"method\":\"tools/call\",\"params\":{\"name\":\"locomote\",\"arguments\":{\"mode\":\"teleport\"}}}"
  c (!isError hop && (← f.sent.get) == #["stick left 0 1", "stick left 0 0"]) "a teleport pushes the left stick and releases it"
  let badMode ← call "{\"jsonrpc\":\"2.0\",\"id\":12,\"method\":\"tools/call\",\"params\":{\"name\":\"locomote\",\"arguments\":{\"mode\":\"fly\"}}}"
  c (isError badMode) "control: an unknown locomotion mode is refused"

  f.backend.lastShot.set (0.0, 0.0)
  f.sent.set #[]
  let early ← call "{\"jsonrpc\":\"2.0\",\"id\":5,\"method\":\"tools/call\",\"params\":{\"name\":\"click\",\"arguments\":{\"x\":1,\"y\":1}}}"
  c (isError early && (← f.sent.get).isEmpty) "pixel tools refuse before any screenshot, sending nothing"
  let endMark := (← f.marks.get).back!
  c (endMark.endsWith " error") s!"control: a refused tool's span ends in error, got {endMark}"

  let n ← failures.get
  if n == 0 then IO.println "all tests passed" else IO.eprintln s!"{n} failure(s)"
  return if n == 0 then 0 else 1
