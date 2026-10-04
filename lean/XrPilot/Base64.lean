/-
Standard base64 (RFC 4648, with padding), for image content in MCP replies.

SPDX-License-Identifier: Apache-2.0 OR MIT
-/
namespace XrPilot

private def alphabet : String := "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"

def base64 (bytes : ByteArray) : String := Id.run do
  let table := alphabet.toList.toArray
  let at_ (i : Nat) : Char := table.getD (i % 64) 'A'
  let mut out := ""
  let n := bytes.size
  let mut i := 0
  while i + 2 < n do
    let v := (bytes.get! i).toNat * 65536 + (bytes.get! (i + 1)).toNat * 256 + (bytes.get! (i + 2)).toNat
    out := out.push (at_ (v / 262144)) |>.push (at_ (v / 4096)) |>.push (at_ (v / 64)) |>.push (at_ v)
    i := i + 3
  if n - i == 1 then
    let v := (bytes.get! i).toNat * 65536
    out := out.push (at_ (v / 262144)) |>.push (at_ (v / 4096)) |>.push '=' |>.push '='
  else if n - i == 2 then
    let v := (bytes.get! i).toNat * 65536 + (bytes.get! (i + 1)).toNat * 256
    out := out.push (at_ (v / 262144)) |>.push (at_ (v / 4096)) |>.push (at_ (v / 64)) |>.push '='
  return out

end XrPilot
