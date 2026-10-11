/-
MCP's streamable HTTP transport: each JSON-RPC message is a POST to /mcp, answered with its JSON
response, or 202 Accepted for a notification. The server sends nothing unprompted, so GET has no
event stream to open. It listens on the loopback address only and answers only what Guard admits.

SPDX-License-Identifier: Apache-2.0 OR MIT
-/
import Std.Http
import Lean.Data.Json
import XrPilot.Guard

namespace XrPilot
open Std Async Http Server

structure McpHttp where
  handle : String → IO (Option Lean.Json)
  lock : Std.BaseMutex
  session : String
  token : String
  port : UInt16

/-- One message at a time: the pilot answers one command at a time over its pipe. -/
def McpHttp.call (self : McpHttp) (body : String) : IO (Option Lean.Json) := do
  self.lock.lock
  try
    self.handle body
  finally
    self.lock.unlock

instance : Handler McpHttp where
  onRequest self req := do
    let header (name : String) := (req.line.headers.get? (Header.Name.ofString! name)).map (·.value)
    match admit self.token self.port (header "authorization") (header "host") (header "origin") with
    | some .unauthorized =>
      return ← Response.new |>.status .unauthorized |>.header! "WWW-Authenticate" "Bearer" |>.text "unauthorized"
    | some _ => return ← Response.new |>.status .forbidden |>.text "forbidden"
    | none => pure ()
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

/-- Serves MCP over HTTP on 127.0.0.1:port until the process ends, to requests bearing `token`. -/
def serveMcpHttp (port : UInt16) (token : String) (handle : String → IO (Option Lean.Json)) : IO Unit := do
  let lock ← Std.BaseMutex.new
  let session := toString (← IO.monoNanosNow)
  Async.block do
    let addr : Net.SocketAddress := .v4 ⟨.ofParts 127 0 0 1, port⟩
    let server ← Server.serve addr ({ handle, lock, session, token, port } : McpHttp)
    server.waitShutdown

end XrPilot
