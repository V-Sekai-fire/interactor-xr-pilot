/-
xr_pilot_mcp: the MCP server an agent registers. It starts xr-pilot, the OXRSys client, and drives
it over that process's stdin and stdout.

  xr_pilot_mcp [--pilot <path to xr-pilot>] [--http <port> [--parent]] [--frames <dir>] [--record-stream <out.pwrec>]

Without --pilot it runs the xr-pilot next to itself. Without --http it speaks MCP over its stdin and
stdout; with it, MCP's streamable HTTP transport at http://127.0.0.1:<port>/mcp, answering only requests
bearing the token in XR_PILOT_MCP_TOKEN. With --parent the pilot is the process that started this one:
commands go out on stdout and replies come back on stdin, as the tray runs it. With --frames, every
left-eye frame a tool takes is also kept in <dir> as <sequence>_<monotonic ms>.png. With --record-stream,
the pilot keeps every streamed frame as it arrived; a restarted pilot writes <out>.1.pwrec, and so on.

SPDX-License-Identifier: Apache-2.0 OR MIT
-/
import XrPilot
open Lean XrPilot

structure McpOptions where
  pilot : Option String := none
  http : Option UInt16 := none
  frames : Option String := none
  recordStream : Option String := none
  parent : Bool := false

def parseOptions : List String → McpOptions → IO McpOptions
  | [], o => return o
  | "--pilot" :: path :: rest, o => parseOptions rest { o with pilot := some path }
  | "--http" :: port :: rest, o =>
    match port.toNat? with
    | some n => if n > 0 && n < 65536 then parseOptions rest { o with http := some n.toUInt16 }
                else throw (IO.userError s!"--http needs a port, got {port}")
    | none => throw (IO.userError s!"--http needs a port, got {port}")
  | "--frames" :: dir :: rest, o => parseOptions rest { o with frames := some dir }
  | "--record-stream" :: out :: rest, o => parseOptions rest { o with recordStream := some out }
  | "--parent" :: rest, o => parseOptions rest { o with parent := true }
  | _, _ => throw (IO.userError "usage: xr_pilot_mcp [--pilot <path to xr-pilot>] [--http <port> [--parent]] [--frames <dir>] [--record-stream <out.pwrec>]")

def pilotPath (options : McpOptions) : IO String := do
  match options.pilot with
  | some path => return path
  | none =>
    let dir := (← IO.appPath).parent.getD "."
    let exe := if System.Platform.isWindows then "xr-pilot.exe" else "xr-pilot"
    return (dir / exe).toString

abbrev PilotConfig : IO.Process.StdioConfig := { stdin := .piped, stdout := .piped, stderr := .inherit }

/-- The recording the n-th pilot writes: the path itself first, then <stem>.<n>.pwrec, so a restart never
overwrites what came before. -/
def recordingName (out : String) (n : Nat) : String :=
  if n == 0 then out else
  let stem := if out.endsWith ".pwrec" then (out.dropEnd 6).toString else out
  s!"{stem}.{n}.pwrec"

def spawnPilot (path : String) (record : Option String := none) : IO (IO.Process.Child PilotConfig) :=
  let extra := match record with | some out => #["--record-stream", out] | none => #[]
  IO.Process.spawn { cmd := path, args := #["--agent"] ++ extra, toStdioConfig := PilotConfig }

def ask (pilot : IO.Process.Child PilotConfig) (cmd : String) : IO (Option String) := do
  try
    pilot.stdin.putStrLn cmd
    pilot.stdin.flush
    let line ← pilot.stdout.getLine
    return if line.isEmpty then none else some line.trimAsciiEnd.toString
  catch _ => return none

/-- The frame file a kept screenshot becomes: a zero-padded sequence, then the monotonic time. -/
def keptFrameName (sequence ms : Nat) : String :=
  let n := toString sequence
  ("".pushn '0' (6 - n.length)) ++ n ++ "_" ++ toString ms ++ ".png"

/-- One command to the parent over stdout, its reply from stdin. -/
def askParent (cmd : String) : IO (Option String) := do
  let stdout ← IO.getStdout
  stdout.putStrLn cmd
  stdout.flush
  let line ← (← IO.getStdin).getLine
  return if line.isEmpty then none else some line.trimAsciiEnd.toString

def main (args : List String) : IO UInt32 := do
  let options ← parseOptions args {}
  if options.parent && options.http.isNone then
    IO.eprintln "xr_pilot_mcp: --parent needs --http, since stdin and stdout carry the pilot's commands"
    return 2
  let token := ((← IO.getEnv "XR_PILOT_MCP_TOKEN").getD "").trimAscii.toString
  if options.http.isSome && token.length < 32 then
    IO.eprintln "xr_pilot_mcp: --http needs a token of 32 or more characters in XR_PILOT_MCP_TOKEN"
    return 2
  let path ← pilotPath options
  let spawns ← IO.mkRef (0 : Nat)
  let spawn : IO (Option (IO.Process.Child PilotConfig)) := do
    if options.parent then return none
    let n ← spawns.modifyGet fun n => (n, n + 1)
    return some (← spawnPilot path (options.recordStream.map (recordingName · n)))
  let pilotRef ← IO.mkRef (← spawn)
  let askPilot (cmd : String) : IO (Option String) := do
    match ← pilotRef.get with
    | some pilot => ask pilot cmd
    | none => askParent cmd
  let temp := (← IO.getEnv "TEMP").getD ((← IO.getEnv "TMPDIR").getD "/tmp")
  if let some dir := options.frames then IO.FS.createDirAll dir
  let kept ← IO.mkRef (0 : Nat)
  let keep : String → Json → IO Unit := fun cmd reply => do
    if let some dir := options.frames then
      if cmd.startsWith "screenshot " && reply.getObjValD "ok" == Json.bool true then
        let n ← kept.modifyGet fun n => (n, n + 1)
        let png ← IO.FS.readBinFile (cmd.drop 11).toString
        IO.FS.writeBinFile (System.FilePath.mk dir / keptFrameName n (← IO.monoMsNow)) png
  let backend : Backend := {
    send := fun cmd => do
      -- A pilot that exited is started again once; its agent state starts from rest.
      let reply ← match ← askPilot cmd with
        | some line => pure (some line)
        | none => do
          if options.parent then pure none
          else
            pilotRef.set (← spawn)
            askPilot cmd
      match reply with
      | none => return Json.mkObj [("ok", false), ("error", "the pilot exited and did not restart")]
      | some line =>
        match Json.parse line with
        | .ok j => do keep cmd j; return j
        | .error _ => return Json.mkObj [("ok", false), ("error", s!"the pilot replied: {line}")]
    readFile := fun path => IO.FS.readBinFile path
    sleepMs := fun ms => IO.sleep ms.toUInt32
    screenshotPath := (System.FilePath.mk temp / s!"xr-pilot-{← IO.monoMsNow}.png").toString
    lastShot := ← IO.mkRef (0.0, 0.0) }
  if let some port := options.http then
    IO.eprintln s!"xr_pilot_mcp: MCP at http://127.0.0.1:{port}/mcp"
    serveMcpHttp port token (handleLine backend)
    return 0
  let stdin ← IO.getStdin
  let stdout ← IO.getStdout
  repeat
    let line ← stdin.getLine
    if line.isEmpty then break
    -- Some shells start the stream with a byte order mark.
    let line := (line.dropWhile (· == '﻿')).trimAscii.toString
    if line.isEmpty then continue
    if let some reply ← handleLine backend line then
      stdout.putStrLn reply.compress
      stdout.flush
  if let some pilot ← pilotRef.get then
    pilot.stdin.flush
    let (_, child) ← pilot.takeStdin
    discard <| child.wait
  return 0
