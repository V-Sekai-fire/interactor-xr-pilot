#!/usr/bin/env bash
# Compiles the Android client's shaders to SPIR-V for VulkanRenderer.cpp to embed.
#
#   src/main/cpp/shaders/gen.sh    # <name>.<stage>  ->  spirv/<name>_<stage>.inc   (committed)
#
# Each .inc is glslc's numeric output, the module's words for a uint32_t array initialiser.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
GLSLC="${GLSLC:-glslc}"
command -v "$GLSLC" >/dev/null 2>&1 || GLSLC="$HOME/scoop/apps/vulkan/current/Bin/glslc"
mkdir -p "$HERE/spirv"
for source in "$HERE"/*.vert "$HERE"/*.frag; do
	name="$(basename "$source")"
	"$GLSLC" --target-env=vulkan1.1 -O -mfmt=num -o "$HERE/spirv/${name%.*}_${name##*.}.inc" "$source"
done
echo "== $(ls "$HERE"/spirv/*.inc | wc -l) shader(s) =="
