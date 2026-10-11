// SPDX-License-Identifier: MPL-2.0
// The local shell's lines and triangles, with a GL-style projection whose depth is moved into Vulkan's 0..1.
#version 450

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec4 aColor;
layout(location = 0) out vec4 vColor;

layout(push_constant) uniform Shell
{
    mat4 mvp;
} pc;

void main()
{
    vColor = aColor;
    gl_Position = pc.mvp * vec4(aPos, 1.0);
    gl_Position.z = (gl_Position.z + gl_Position.w) * 0.5;
}
