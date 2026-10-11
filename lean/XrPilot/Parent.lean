/-
The pipe to the desk copy of xr-pilot that started this server: each command leaves as a line on
stdout and its reply comes back as a line on stdin.

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

/-- Reads reply lines from `inp` on a thread of its own. `onEnd` runs once the parent closes it, so a
server whose tray died does not outlive it even while no request arrives. -/
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

end XrPilot
