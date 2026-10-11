/-
MCP's streamable HTTP transport: each JSON-RPC message is a POST to /mcp, answered with its JSON
response, or 202 Accepted for a notification. The server sends nothing unprompted, so GET has no
event stream to open. It listens on the loopback address only, and trusts no caller: every request
carries `Authorization: Bearer <token>` (401 otherwise), and one with an Origin that is not loopback
is refused (403), so a web page cannot reach it by DNS rebinding.

SPDX-License-Identifier: Apache-2.0 OR MIT
-/
import Std.Http
import Lean.Data.Json

namespace XrPilot
open Std Async Http Server

/-- The port the desk copy serves MCP on: past OXRSys's 9943-9948. -/
def defaultMcpPort : UInt16 := 9949

structure McpHttp where
  handle : String → IO (Option Lean.Json)
  lock : Std.BaseMutex
  session : String
  token : String

/-- One message at a time: the pilot answers one command at a time over its pipe. -/
def McpHttp.call (self : McpHttp) (body : String) : IO (Option Lean.Json) := do
  self.lock.lock
  try
    self.handle body
  finally
    self.lock.unlock

/-- Whether an Origin names this machine: http or https to 127.0.0.1, localhost or [::1], any port. -/
def loopbackOrigin (origin : String) : Bool :=
  let rest := if origin.startsWith "http://" then some (origin.drop 7).toString
    else if origin.startsWith "https://" then some (origin.drop 8).toString else none
  match rest with
  | none => false
  | some rest =>
    ["127.0.0.1", "localhost", "[::1]"].any fun host =>
      rest == host || (rest.startsWith (host ++ ":") &&
        let port := (rest.drop (host.length + 1)).toString
        !port.isEmpty && port.all Char.isDigit)

/-- Compares every byte whatever the first difference, so the time taken says nothing of the token. -/
def sameSecret (a b : String) : Bool :=
  let x := a.toUTF8
  let y := b.toUTF8
  x.size == y.size && (List.range x.size).foldl (fun acc i => acc ||| (x[i]! ^^^ y[i]!)) 0 == 0

/-- Why a request is refused before it reaches MCP, or none when it may. -/
def refusal (token : String) (authorization origin : Option String) : Option (Status × String) :=
  if origin.any (!loopbackOrigin ·) then some (.forbidden, "origin not allowed")
  else if token.isEmpty || !authorization.any (sameSecret s!"Bearer {token}") then some (.unauthorized, "unauthorized")
  else none

instance : Handler McpHttp where
  onRequest self req := do
    let header (name : String) : Option String :=
      (req.line.headers.get? (Header.Name.ofString! name)).map (·.value)
    if let some (status, text) := refusal self.token (header "authorization") (header "origin") then
      let response := Response.new |>.status status
      return ← (if status == .unauthorized then response.header! "WWW-Authenticate" "Bearer" else response).text text
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

def McpHttp.new (token : String) (handle : String → IO (Option Lean.Json)) : IO McpHttp := do
  return { handle, token, lock := ← Std.BaseMutex.new, session := toString (← IO.monoNanosNow) }

/-- Serves MCP over HTTP on 127.0.0.1:port, to callers holding the token, until the process ends. -/
def serveMcpHttp (port : UInt16) (token : String) (handle : String → IO (Option Lean.Json)) : IO Unit := do
  let mcp ← McpHttp.new token handle
  Async.block do
    let addr : Net.SocketAddress := .v4 ⟨.ofParts 127 0 0 1, port⟩
    let server ← Server.serve addr mcp
    server.waitShutdown

end XrPilot
