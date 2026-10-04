/-
Rotations as row-major 3x3 matrices taking pose-local vectors to world space; the columns are the
local +X, +Y and +Z axes. The matrix is the canonical form on the wire and in every reply. Euler
angles in one of the six Tait-Bryan orders, and quaternions, are inputs converted to it.

SPDX-License-Identifier: Apache-2.0 OR MIT
-/
import Lean.Data.Json

namespace XrPilot
open Lean

def degToRad (d : Float) : Float := d * 3.141592653589793 / 180.0
def radToDeg (r : Float) : Float := r * 180.0 / 3.141592653589793

structure Vec3 where
  x : Float
  y : Float
  z : Float
  deriving Repr, Inhabited

structure Mat3 where
  m : Array Float
  deriving Repr, Inhabited

namespace Mat3

def identity : Mat3 := ⟨#[1, 0, 0, 0, 1, 0, 0, 0, 1]⟩

def get (r : Mat3) (row col : Nat) : Float := r.m[row * 3 + col]!

def mul (a b : Mat3) : Mat3 :=
  ⟨Id.run do
    let mut out := #[]
    for row in [0:3] do
      for col in [0:3] do
        out := out.push (a.get row 0 * b.get 0 col + a.get row 1 * b.get 1 col + a.get row 2 * b.get 2 col)
    return out⟩

def apply (r : Mat3) (v : Vec3) : Vec3 :=
  { x := r.get 0 0 * v.x + r.get 0 1 * v.y + r.get 0 2 * v.z
    y := r.get 1 0 * v.x + r.get 1 1 * v.y + r.get 1 2 * v.z
    z := r.get 2 0 * v.x + r.get 2 1 * v.y + r.get 2 2 * v.z }

def transpose (r : Mat3) : Mat3 :=
  ⟨#[r.get 0 0, r.get 1 0, r.get 2 0, r.get 0 1, r.get 1 1, r.get 2 1, r.get 0 2, r.get 1 2, r.get 2 2]⟩

/-- Orthonormal with determinant +1 within tol, so scales, shears and mirrors are refused. -/
def isRotation (r : Mat3) (tol : Float := 1e-3) : Bool := Id.run do
  if r.m.size != 9 || r.m.any (fun v => v.isNaN || v.isInf) then return false
  let t := r.transpose.mul r
  for i in [0:3] do
    for j in [0:3] do
      if Float.abs (t.get i j - (if i == j then 1.0 else 0.0)) > tol then return false
  let det := r.get 0 0 * (r.get 1 1 * r.get 2 2 - r.get 1 2 * r.get 2 1)
           - r.get 0 1 * (r.get 1 0 * r.get 2 2 - r.get 1 2 * r.get 2 0)
           + r.get 0 2 * (r.get 1 0 * r.get 2 1 - r.get 1 1 * r.get 2 0)
  return Float.abs (det - 1.0) ≤ tol

/-- The rows as nested arrays. -/
def toJson (r : Mat3) : Json :=
  Json.arr #[Json.arr #[Lean.toJson (r.get 0 0), Lean.toJson (r.get 0 1), Lean.toJson (r.get 0 2)],
             Json.arr #[Lean.toJson (r.get 1 0), Lean.toJson (r.get 1 1), Lean.toJson (r.get 1 2)],
             Json.arr #[Lean.toJson (r.get 2 0), Lean.toJson (r.get 2 1), Lean.toJson (r.get 2 2)]]

/-- Three rows of three numbers, or none. -/
def ofJson? (j : Json) : Option Mat3 := do
  let rows ← j.getArr?.toOption
  if rows.size != 3 then none
  let mut out := #[]
  for row in rows do
    let vals ← row.getArr?.toOption
    if vals.size != 3 then none
    for v in vals do
      let n ← v.getNum?.toOption
      out := out.push n.toFloat
  return ⟨out⟩

/-- The nine entries row by row, as the pilot's line protocol takes them. -/
def args (r : Mat3) : String := " ".intercalate (r.m.toList.map toString)

end Mat3

private def axis (c : Char) (deg : Float) : Mat3 :=
  let co := Float.cos (degToRad deg)
  let s := Float.sin (degToRad deg)
  match c with
  | 'X' => ⟨#[1, 0, 0, 0, co, -s, 0, s, co]⟩
  | 'Y' => ⟨#[co, 0, s, 0, 1, 0, -s, 0, co]⟩
  | _ => ⟨#[co, -s, 0, s, co, 0, 0, 0, 1]⟩

def eulerOrders : List String := ["XYZ", "XZY", "YXZ", "YZX", "ZXY", "ZYX"]

/-- Intrinsic Tait-Bryan: order "YXZ" with degrees (a, b, c) is Ry(a) Rx(b) Rz(c). -/
def fromEuler (order : String) (a b c : Float) : Option Mat3 :=
  if eulerOrders.contains order then
    match order.toList with
    | [p, q, r] => some (((axis p a).mul (axis q b)).mul (axis r c))
    | _ => none
  else none

/-- A quaternion x, y, z, w, normalised first; none for one of zero length. -/
def fromQuaternion (x y z w : Float) : Option Mat3 :=
  let n := Float.sqrt (x * x + y * y + z * z + w * w)
  if n ≤ 1e-6 || n.isNaN then none
  else
    let x := x / n
    let y := y / n
    let z := z / n
    let w := w / n
    some ⟨#[1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w),
           2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w),
           2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)]⟩

/-- The rotation whose -Z axis points along d with no roll: yaw about +Y, then pitch about +X. -/
def facing (d : Vec3) : Mat3 :=
  let len := Float.sqrt (d.x * d.x + d.y * d.y + d.z * d.z)
  let yaw := radToDeg (Float.atan2 (-d.x) (-d.z))
  let pitch := radToDeg (Float.asin (d.y / len))
  match fromEuler "YXZ" yaw pitch 0.0 with
  | some r => r
  | none => Mat3.identity

end XrPilot
