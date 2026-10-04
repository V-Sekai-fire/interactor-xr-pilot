/-
Screenshot pixel → world ray. The screenshot is the left eye: a pinhole looking down -Z with the
eye's half field of view, turned by the head's yaw (about +Y) then pitch (about +X), and sitting
half an IPD to the left of the head. A hand placed at the eye and aimed along this ray points at the
pixel at every distance.

SPDX-License-Identifier: Apache-2.0 OR MIT
-/
namespace XrPilot

def degToRad (d : Float) : Float := d * 3.141592653589793 / 180.0
def radToDeg (r : Float) : Float := r * 180.0 / 3.141592653589793

structure Vec3 where
  x : Float
  y : Float
  z : Float
  deriving Repr, Inhabited

/-- The view: head pose in degrees and metres, the eye's half FOVs in degrees, image size in pixels. -/
structure View where
  headX : Float
  headY : Float
  headZ : Float
  yaw : Float
  pitch : Float
  halfFovH : Float
  halfFovV : Float
  ipd : Float
  width : Float
  height : Float
  deriving Repr, Inhabited

/-- Rotates a head-space vector by yaw about +Y after pitch about +X, as the runtime composes them. -/
def rotate (yawDeg pitchDeg : Float) (v : Vec3) : Vec3 :=
  let p := degToRad pitchDeg
  let y := degToRad yawDeg
  let y1 := v.y * Float.cos p - v.z * Float.sin p
  let z1 := v.y * Float.sin p + v.z * Float.cos p
  { x := v.x * Float.cos y + z1 * Float.sin y, y := y1, z := -v.x * Float.sin y + z1 * Float.cos y }

/-- The aim through pixel (px, py), top-left origin: where it starts and its yaw and pitch in degrees. -/
structure Aim where
  origin : Vec3
  yaw : Float
  pitch : Float
  deriving Repr, Inhabited

def aimThroughPixel (view : View) (px py : Float) : Aim :=
  let nx := (2.0 * px / view.width - 1.0) * Float.tan (degToRad view.halfFovH)
  let ny := (1.0 - 2.0 * py / view.height) * Float.tan (degToRad view.halfFovV)
  let d := rotate view.yaw view.pitch { x := nx, y := ny, z := -1.0 }
  let len := Float.sqrt (d.x * d.x + d.y * d.y + d.z * d.z)
  let eye := rotate view.yaw view.pitch { x := -view.ipd / 2.0, y := 0.0, z := 0.0 }
  { origin := { x := view.headX + eye.x, y := view.headY + eye.y, z := view.headZ + eye.z }
    yaw := radToDeg (Float.atan2 (-d.x) (-d.z))
    pitch := radToDeg (Float.asin (d.y / len)) }

/-- The yaw-frame offset of a walk: forward along the view's yaw, right perpendicular to it. -/
def walk (yawDeg forward right up : Float) : Vec3 :=
  let y := degToRad yawDeg
  { x := -Float.sin y * forward + Float.cos y * right
    y := up
    z := -Float.cos y * forward - Float.sin y * right }

end XrPilot
