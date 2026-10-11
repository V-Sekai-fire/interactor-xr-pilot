import OxrSys.Dsl

/-!
# `OxrSys.EyeFitBgra` — pyro2cfhd's CPU fit on the GPU, one thread per fitted pixel

Left-eye chroma widened bilinearly, full-range BT.709 to 8-bit RGB, then the host's separable taps
(`first`, `weight`: fit columns, then rows) into a BGRA canvas stored bottom row first.
-/

namespace OxrSys.EyeFitBgra

open LeanSlang
open OxrSys.Dsl

def byteAt (buf : String) (i : E) : E :=
  toF (band (shr (at_ buf (shr i (u 2))) (band i (u 3) * u 8)) (u 255))

def clampI (e hi : E) : E := toU (call "clamp" [e, .litInt 0, .cast iT hi])

def bilinear (plane : String) : E :=
  let at2 (cy cx : String) := byteAt plane (v cy * p "chromaStride" + v cx)
  (at2 "cy0" "cx0" * (fl 1.0 - v "tx") + at2 "cy0" "cx1" * v "tx") * (fl 1.0 - v "ty")
    + (at2 "cy1" "cx0" * (fl 1.0 - v "tx") + at2 "cy1" "cx1" * v "tx") * v "ty"

def channel (e : E) : E := toF (toU (fmin (fmax (e + fl 0.5) (fl 0.0)) (fl 255.0)))

def tap : List St :=
  [ let_ uT "sx" (clampI (v "fx" + .cast iT (v "kx")) (p "eyeWidth" - u 1))
  , let_ fT "w" (at_ "weight" (v "x" * p "taps" + v "kx"))
  , let_ fT "yy" (byteAt "luma" (v "sy" * p "width" + v "sx"))
  , let_ fT "fcx" (call "clamp" [toF (v "sx") * fl 0.5 - fl 0.25, fl 0.0, toF (p "chromaWidth" - u 1)])
  , let_ uT "cx0" (toU (v "fcx"))
  , let_ uT "cx1" (fmin (v "cx0" + u 1) (p "chromaWidth" - u 1))
  , let_ fT "tx" (v "fcx" - toF (v "cx0"))
  , let_ fT "pb" (bilinear "cb" - fl 128.0)
  , let_ fT "pr" (bilinear "cr" - fl 128.0)
  , setv "hr" (v "hr" + v "w" * channel (v "yy" + fl 1.5748 * v "pr"))
  , setv "hg" (v "hg" + v "w" * channel (v "yy" - fl 0.1873 * v "pb" - fl 0.4681 * v "pr"))
  , setv "hb" (v "hb" + v "w" * channel (v "yy" + fl 1.8556 * v "pb")) ]

def row : List St :=
  [ let_ uT "sy" (clampI (v "fy" + .cast iT (v "ky")) (p "eyeHeight" - u 1))
  , let_ fT "wy" (at_ "weight" ((p "fitWidth" + v "y") * p "taps" + v "ky"))
  , let_ fT "fcy" (call "clamp" [toF (v "sy") * fl 0.5 - fl 0.25, fl 0.0, toF (p "chromaHeight" - u 1)])
  , let_ uT "cy0" (toU (v "fcy"))
  , let_ uT "cy1" (fmin (v "cy0" + u 1) (p "chromaHeight" - u 1))
  , let_ fT "ty" (v "fcy" - toF (v "cy0"))
  , let_ fT "hr" (fl 0.0), let_ fT "hg" (fl 0.0), let_ fT "hb" (fl 0.0)
  , for_ "kx" (u 0) (p "taps") tap
  , setv "r" (v "r" + v "wy" * v "hr")
  , setv "g" (v "g" + v "wy" * v "hg")
  , setv "b" (v "b" + v "wy" * v "hb") ]

def byte (e : E) : E := toU (fmin (fmax (e + fl 0.5) (fl 0.0)) (fl 255.0))

def shader : SlangShaderModule :=
  { structs := [ { name := "EyeFitParams"
                 , fields := ["width", "eyeWidth", "eyeHeight", "chromaStride", "chromaWidth", "chromaHeight",
                              "fitWidth", "fitHeight", "offsetX", "offsetY", "canvasWidth", "canvasHeight", "taps"].map
                               (fun n => fld n uT) } ]
  , globals := [ paramsCB "EyeFitParams", roU "luma" 1, roU "cb" 2, roU "cr" 3, glob "first" (.roBuf iT) 4,
                 roF "weight" 5, rwU "bgra" 6 ]
  , functions :=
      [ entry 64 [dtid]
          ([ let_ uT "x" (.member (v "tid") "x")
           , let_ uT "y" (.member (v "tid") "y")
           , if_ (or_ (ge (v "x") (p "fitWidth")) (ge (v "y") (p "fitHeight"))) [ ret ]
           , let_ iT "fx" (at_ "first" (v "x"))
           , let_ iT "fy" (at_ "first" (p "fitWidth" + v "y"))
           , let_ fT "r" (fl 0.0), let_ fT "g" (fl 0.0), let_ fT "b" (fl 0.0)
           , for_ "ky" (u 0) (p "taps") row
           , let_ uT "o" ((p "canvasHeight" - u 1 - p "offsetY" - v "y") * p "canvasWidth" + p "offsetX" + v "x")
           , setAt "bgra" (v "o")
               (bor (bor (bor (byte (v "b")) (shl (byte (v "g")) (u 8))) (shl (byte (v "r")) (u 16)))
                 (u 4278190080)) ]) ] }

end OxrSys.EyeFitBgra
