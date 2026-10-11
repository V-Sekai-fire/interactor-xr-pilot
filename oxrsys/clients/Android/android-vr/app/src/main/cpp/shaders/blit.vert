// SPDX-License-Identifier: MPL-2.0
// One triangle covering the viewport; with the renderer's flipped viewport, v = 1 is the frame's bottom row.
#version 450

layout(location = 0) out vec2 vUV;

void main()
{
    const vec2 position = vec2(gl_VertexIndex == 1 ? 3.0 : -1.0, gl_VertexIndex == 2 ? 3.0 : -1.0);
    vUV = vec2(position.x + 1.0, 1.0 - position.y) * 0.5;
    gl_Position = vec4(position, 0.0, 1.0);
}
