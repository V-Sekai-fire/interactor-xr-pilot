// SPDX-License-Identifier: MPL-2.0
#version 450

layout(location = 0) in vec4 vColor;
layout(location = 0) out vec4 fragColor;

void main()
{
    fragColor = vColor;
}
