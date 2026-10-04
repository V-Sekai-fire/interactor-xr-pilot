/-
xr_pilot_mcp: the MCP server an agent registers. It starts xr-pilot, the OXRSys client, and drives
it over that process's stdin and stdout.

  xr_pilot_mcp [--pilot <path to xr-pilot>] [--http <port>]

Without --pilot it runs the xr-pilot next to itself. Without --http it speaks MCP over its stdin and
stdout; with it, MCP's streamable HTTP transport at http://127.0.0.1:<port>/mcp.

SPDX-License-Identifier: Apache-2.0 OR MIT
-/
import XrPilot
open Lean XrPilot

structure McpOptions where
  pilot : Option String := none
  http : Option UInt16 := none

def parseOptions : List String → McpOptions → IO McpOptions
  | [], o => return o
  | "--pilot" :: path :: rest, o => parseOptions rest { o with pilot := some path }
  | "--http" :: port :: rest, o =>
    match port.toNat? with
    | some n => if n > 0 && n < 65536 then parseOptions rest { o with http := some n.toUInt16 }
                else throw (IO.userError s!"--http needs a port, got {port}")
    | none => throw (IO.userError s!"--http needs a port, got {port}")
  | _, _ => throw (IO.userError "usage: xr_pilot_mcp [--pilot <path to xr-pilot>] [--http <port>]")

def pilotPath (options : McpOptions) : IO String := do
  match options.pilot with
  | some path => return path
  | none =>
    let dir := (← IO.appPath).parent.getD "."
    let exe := if System.Platform.isWindows then "xr-pilot.exe" else "xr-pilot"
    return (dir / exe).toString

abbrev PilotConfig : IO.Process.StdioConfig := { stdin := .piped, stdout := .piped, stderr := .inherit }

def spawnPilot (path : String) : IO (IO.Process.Child PilotConfig) :=
  IO.Process.spawn { cmd := path, args := #["--agent"], toStdioConfig := PilotConfig }

def ask (pilot : IO.Process.Child PilotConfig) (cmd : String) : IO (Option String) := do
  try
    pilot.stdin.putStrLn cmd
    pilot.stdin.flush
    let line ← pilot.stdout.getLine
    return if line.isEmpty then none else some line.trimAsciiEnd.toString
  catch _ => return none

def main (args : List String) : IO UInt32 := do
  let options ← parseOptions args {}
  let path ← pilotPath options
  let pilotRef ← IO.mkRef (← spawnPilot path)
  let temp := (← IO.getEnv "TEMP").getD ((← IO.getEnv "TMPDIR").getD "/tmp")
  let backend : Backend := {
    send := fun cmd => do
      -- A pilot that exited is started again once; its agent state starts from rest.
      let reply ← match ← ask (← pilotRef.get) cmd with
        | some line => pure (some line)
        | none => do
          pilotRef.set (← spawnPilot path)
          ask (← pilotRef.get) cmd
      match reply with
      | none => return Json.mkObj [("ok", false), ("error", "the pilot exited and did not restart")]
      | some line =>
        match Json.parse line with
        | .ok j => return j
        | .error _ => return Json.mkObj [("ok", false), ("error", s!"the pilot replied: {line}")]
    readFile := fun path => IO.FS.readBinFile path
    sleepMs := fun ms => IO.sleep ms.toUInt32
    screenshotPath := (System.FilePath.mk temp / s!"xr-pilot-{← IO.monoMsNow}.png").toString
    lastShot := ← IO.mkRef (0.0, 0.0) }
  if let some port := options.http then
    IO.eprintln s!"xr_pilot_mcp: MCP at http://127.0.0.1:{port}/mcp"
    serveMcpHttp port (handleLine backend)
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
  let pilot ← pilotRef.get
  pilot.stdin.flush
  let (_, child) ← pilot.takeStdin
  discard <| child.wait
  return 0
