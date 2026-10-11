/-
The MCP's HTTP requests it answers: this loopback host, no web origin but its own (DNS rebinding), and the
install's bearer token.

SPDX-License-Identifier: Apache-2.0 OR MIT
-/
namespace XrPilot

inductive Refusal where
  | unauthorized
  | foreignHost
  | foreignOrigin
deriving DecidableEq, Repr

/-- Equal strings, compared without stopping at the first difference. -/
def sameSecret (a b : String) : Bool :=
  a.length == b.length && (a.toList.zip b.toList).foldl (fun same (x, y) => same && x == y) true

def loopbackHosts (port : UInt16) : List String :=
  [s!"127.0.0.1:{port}", s!"localhost:{port}", s!"[::1]:{port}"]

def admit (token : String) (port : UInt16) (authorization host origin : Option String) : Option Refusal :=
  if !(loopbackHosts port).contains ((host.getD "").toLower) then some .foreignHost
  else if origin.any (fun o => !(loopbackHosts port).any (fun h => o.toLower == "http://" ++ h)) then
    some .foreignOrigin
  else if token.isEmpty || !sameSecret (authorization.getD "") ("Bearer " ++ token) then some .unauthorized
  else none

end XrPilot
