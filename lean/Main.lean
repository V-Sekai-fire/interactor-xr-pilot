/-
xr_pilot_mcp: the MCP server an agent registers. It starts xr-pilot, the OXRSys client, and drives
it over that process's stdin and stdout.

  xr_pilot_mcp [--pilot <path to xr-pilot>] [--http <port> [--parent] [--token-file <path>]]
               [--frames <dir>] [--record-stream <out.pwrec>]

Without --pilot it runs the xr-pilot next to itself. Without --http it speaks MCP over its stdin and
stdout; with it, MCP's streamable HTTP transport at http://127.0.0.1:<port>/mcp, answering only
requests that carry the desk's bearer token (--token-file, by default the one the desk copy keeps in
its per-user folder). With --parent it starts no pilot: the desk copy that started it owns the
pilot, takes each command as a line on this server's stdout and answers on its stdin. With --frames, every
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
  tokenFile : Option String := none

def usage : String :=
  "usage: xr_pilot_mcp [--pilot <path to xr-pilot>] [--http <port> [--parent] [--token-file <path>]] [--frames <dir>] [--record-stream <out.pwrec>]"

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
  | "--token-file" :: path :: rest, o => parseOptions rest { o with tokenFile := some path }
  | _, _ => throw (IO.userError usage)

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

/-- A pilot this server starts (restarted once when it exits), or the desk copy that started it. -/
def pilotLink (options : McpOptions) : IO ((String → IO (Option String)) × IO Unit) := do
  if options.parent then
    let link ← ParentLink.start (← IO.getStdin) (← IO.getStdout) (IO.Process.exit 0)
    return (link.ask, pure ())
  let path ← pilotPath options
  let spawns ← IO.mkRef (0 : Nat)
  let spawn : IO (IO.Process.Child PilotConfig) := do
    let n ← spawns.modifyGet fun n => (n, n + 1)
    spawnPilot path (options.recordStream.map (recordingName · n))
  let pilotRef ← IO.mkRef (← spawn)
  let send cmd := do
    -- A pilot that exited is started again once; its agent state starts from rest.
    match ← ask (← pilotRef.get) cmd with
    | some line => pure (some line)
    | none => do
      pilotRef.set (← spawn)
      ask (← pilotRef.get) cmd
  let finish := do
    let pilot ← pilotRef.get
    pilot.stdin.flush
    let (_, child) ← pilot.takeStdin
    discard <| child.wait
  return (send, finish)

/-- The frame file a kept screenshot becomes: a zero-padded sequence, then the monotonic time. -/
def keptFrameName (sequence ms : Nat) : String :=
  let n := toString sequence
  ("".pushn '0' (6 - n.length)) ++ n ++ "_" ++ toString ms ++ ".png"

def main (args : List String) : IO UInt32 := do
  let options ← parseOptions args {}
  if options.parent && (options.http.isNone || options.recordStream.isSome || options.pilot.isSome) then
    throw (IO.userError "--parent serves over --http, and the desk copy owns the pilot and its recording")
  -- The token is read before any pilot starts, so a server that would refuse every caller never runs.
  let token ← match options.http with
    | some _ => readToken (← match options.tokenFile with | some p => pure (System.FilePath.mk p) | none => defaultTokenPath)
    | none => pure ""
  let (pilot, finish) ← pilotLink options
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
      match ← pilot cmd with
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
  finish
  return 0
