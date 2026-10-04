/-
Screenshot pixel → world ray. The screenshot is the left eye: a pinhole looking down -Z with the
eye's half field of view, turned by the head's rotation matrix, and sitting half an IPD to the left
of the head. A hand placed at the eye and aimed along this ray points at the pixel at every distance.

SPDX-License-Identifier: Apache-2.0 OR MIT
-/
import XrPilot.Rotation

namespace XrPilot

/-- The view: head position in metres and rotation, the eye's half FOVs in degrees, image size in pixels. -/
structure View where
  headX : Float
  headY : Float
  headZ : Float
  rotation : Mat3
  halfFovH : Float
  halfFovV : Float
  ipd : Float
  width : Float
  height : Float
  deriving Repr, Inhabited

/-- The aim through a pixel: where it starts, its unit direction, and the hand's rotation along it. -/
structure Aim where
  origin : Vec3
  direction : Vec3
  rotation : Mat3
  deriving Repr, Inhabited

/-- The aim through pixel (px, py), top-left origin. -/
def aimThroughPixel (view : View) (px py : Float) : Aim :=
  let nx := (2.0 * px / view.width - 1.0) * Float.tan (degToRad view.halfFovH)
  let ny := (1.0 - 2.0 * py / view.height) * Float.tan (degToRad view.halfFovV)
  let d := view.rotation.apply { x := nx, y := ny, z := -1.0 }
  let len := Float.sqrt (d.x * d.x + d.y * d.y + d.z * d.z)
  let unit : Vec3 := { x := d.x / len, y := d.y / len, z := d.z / len }
  let eye := view.rotation.apply { x := -view.ipd / 2.0, y := 0.0, z := 0.0 }
  { origin := { x := view.headX + eye.x, y := view.headY + eye.y, z := view.headZ + eye.z }
    direction := unit
    rotation := facing unit }

/-- The world offset of a walk: forward where the head faces across the floor, right of that, and up.
Looking straight down, forward is where the top of the view points. -/
def walk (rotation : Mat3) (forward right up : Float) : Vec3 :=
  let fx := -rotation.get 0 2
  let fz := -rotation.get 2 2
  let level := Float.sqrt (fx * fx + fz * fz)
  let (fx, fz, level) :=
    if level > 1e-3 then (fx, fz, level)
    else
      let ux := rotation.get 0 1
      let uz := rotation.get 2 1
      (ux, uz, Float.sqrt (ux * ux + uz * uz))
  let fx := fx / level
  let fz := fz / level
  { x := fx * forward - fz * right
    y := up
    z := fz * forward + fx * right }

end XrPilot
