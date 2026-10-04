/-
MCP's streamable HTTP transport: each JSON-RPC message is a POST to /mcp, answered with its JSON
response, or 202 Accepted for a notification. The server sends nothing unprompted, so GET has no
event stream to open. It listens on the loopback address only.

SPDX-License-Identifier: Apache-2.0 OR MIT
-/
import Std.Http
import Lean.Data.Json

namespace XrPilot
open Std Async Http Server

structure McpHttp where
  handle : String → IO (Option Lean.Json)
  lock : Std.BaseMutex
  session : String

/-- One message at a time: the pilot answers one command at a time over its pipe. -/
def McpHttp.call (self : McpHttp) (body : String) : IO (Option Lean.Json) := do
  self.lock.lock
  try
    self.handle body
  finally
    self.lock.unlock

instance : Handler McpHttp where
  onRequest self req := do
    let target := toString req.line.uri
    unless target == "/mcp" || target.startsWith "/mcp?" do
      return ← Response.new |>.status .notFound |>.text "not found"
    match req.line.method with
    | Method.post =>
      let body : String ← req.body.readAll (maximumSize := some 16777216)
      match ← self.call body.trimAscii.toString with
      | some reply => Response.ok |>.header! "Mcp-Session-Id" self.session |>.json reply.compress
      | none => Response.new |>.status .accepted |>.header! "Mcp-Session-Id" self.session |>.text ""
    | Method.delete => Response.ok |>.text ""
    | _ => Response.new |>.status .methodNotAllowed |>.header! "Allow" "POST, DELETE" |>.text ""

/-- Serves MCP over HTTP on 127.0.0.1:port until the process ends. -/
def serveMcpHttp (port : UInt16) (handle : String → IO (Option Lean.Json)) : IO Unit := do
  let lock ← Std.BaseMutex.new
  let session := toString (← IO.monoNanosNow)
  Async.block do
    let addr : Net.SocketAddress := .v4 ⟨.ofParts 127 0 0 1, port⟩
    let server ← Server.serve addr ({ handle, lock, session } : McpHttp)
    server.waitShutdown

end XrPilot
