/-
The parent pipe to the desk copy of xr-pilot that started this server: each command leaves as a line
on stdout and its reply comes back as a line on stdin.

SPDX-License-Identifier: Apache-2.0 OR MIT
-/
import Std.Sync.Channel

namespace XrPilot

structure ParentLink where
  out : IO.FS.Stream
  replies : Std.CloseableChannel String

private partial def pumpReplies (inp : IO.FS.Stream) (replies : Std.CloseableChannel String) (onEnd : IO Unit) :
    IO Unit := do
  let line ← inp.getLine
  if line.isEmpty then
    discard <| EIO.toBaseIO replies.close
    onEnd
  else
    discard <| replies.send line.trimAscii.toString
    pumpReplies inp replies onEnd

/-- Reads reply lines from `inp` on a thread of its own; `onEnd` runs once the parent closes it, so a
server whose parent went away does not outlive it. -/
def ParentLink.start (inp out : IO.FS.Stream) (onEnd : IO Unit) : IO ParentLink := do
  let replies ← Std.CloseableChannel.new
  discard <| IO.asTask (prio := .dedicated) (pumpReplies inp replies onEnd)
  return { out, replies }

def ParentLink.ask (self : ParentLink) (cmd : String) : IO (Option String) := do
  try
    self.out.putStrLn cmd
    self.out.flush
  catch _ => return none
  IO.wait (← self.replies.recv)

/-- Where the desk copy keeps the token: SDL's per-user folder for V-Sekai-fire/xr-pilot. -/
def defaultTokenPath : IO System.FilePath := do
  let env (name : String) : IO System.FilePath := return ⟨(← IO.getEnv name).getD ""⟩
  let home ← env "HOME"
  let xdg ← env "XDG_DATA_HOME"
  let base ←
    if System.Platform.isWindows then env "APPDATA"
    else if System.Platform.isOSX then pure (home / "Library" / "Application Support")
    else pure (if xdg.toString.isEmpty then home / ".local" / "share" else xdg)
  return base / "V-Sekai-fire" / "xr-pilot" / "mcp-token"

/-- A token is 32 random bytes as 64 lowercase hex digits. -/
def validToken (token : String) : Bool :=
  token.length == 64 && token.all fun c => c.isDigit || ('a' ≤ c && c ≤ 'f')

def readToken (path : System.FilePath) : IO String := do
  let token ← try IO.FS.readFile path catch _ => pure ""
  let token := token.trimAscii.toString
  unless validToken token do
    throw (IO.userError s!"no MCP token in {path}: start XR Pilot once, or run xr-pilot --mcp-config")
  return token

end XrPilot
