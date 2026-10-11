// SPDX-License-Identifier: MPL-2.0
// Converts one eye of the decoded Y, Cb, Cr planes to RGB.
// Foveated decompression follows ALVR's AADT inverse mapping (MIT licensed).
// The upscaling branch is an OXRSys edge-aware shader path using ALVR's public defaults.
#version 450

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 fragColor;

layout(set = 0, binding = 0) uniform sampler2D uTextureY;
layout(set = 0, binding = 1) uniform sampler2D uTextureCb;
layout(set = 0, binding = 2) uniform sampler2D uTextureCr;

// VulkanRenderer::BlitParams, member for member.
layout(push_constant) uniform Blit
{
    vec2 eyeSourceMin;
    vec2 eyeSourceMax;
    vec2 logicalTexelSize;
    vec2 foveationCenterSize;
    vec2 foveationCenterShift;
    vec2 foveationEdgeRatio;
    vec2 foveationEyeSizeRatio;
    vec2 reprojectionWarpOffset;
    float upscaleEdgeThreshold;
    float upscaleSharpness;
    int foveatedEncodingEnabled;
    int clientUpscalingEnabled;
    int reprojectionWarpEnabled;
    int passthroughAlphaEnabled;
} pc;

float compressAxis(float eyeUv, float centerSize, float centerShift, float edgeRatio)
{
    float c0 = (1.0 - centerSize) * 0.5;
    float c1 = (edgeRatio - 1.0) * c0 * (centerShift + 1.0) / edgeRatio;
    float c2 = (edgeRatio - 1.0) * centerSize + 1.0;
    float loBound = c0 * (centerShift + 1.0) / c2;
    float hiBound = c0 * (centerShift - 1.0) / c2 + 1.0;
    float center = eyeUv * c2 / edgeRatio + c1;
    float d2 = eyeUv * c2;
    float d3 = (eyeUv - 1.0) * c2 + 1.0;
    float g1 = loBound > 0.0 ? eyeUv / loBound : 1.0;
    float g2 = (1.0 - hiBound) > 0.0 ? (1.0 - eyeUv) / (1.0 - hiBound) : 1.0;
    float leftEdge = g1 * center + (1.0 - g1) * d2;
    float rightEdge = g2 * center + (1.0 - g2) * d3;
    if (eyeUv < loBound)
    {
        return leftEdge;
    }
    if (eyeUv > hiBound)
    {
        return rightEdge;
    }
    return center;
}

float decompressAxis(float targetUv, float centerSize, float centerShift, float edgeRatio)
{
    float lo = 0.0;
    float hi = 1.0;
    for (int i = 0; i < 10; ++i)
    {
        float mid = (lo + hi) * 0.5;
        float mapped = compressAxis(mid, centerSize, centerShift, edgeRatio);
        if (mapped < targetUv)
        {
            lo = mid;
        }
        else
        {
            hi = mid;
        }
    }
    return (lo + hi) * 0.5;
}

vec2 mapEyeUvToSource(vec2 eyeUv)
{
    vec2 corrected = eyeUv;
    if (pc.foveatedEncodingEnabled != 0)
    {
        corrected.x = decompressAxis(eyeUv.x, pc.foveationCenterSize.x, pc.foveationCenterShift.x,
                                     pc.foveationEdgeRatio.x) * pc.foveationEyeSizeRatio.x;
        corrected.y = decompressAxis(eyeUv.y, pc.foveationCenterSize.y, pc.foveationCenterShift.y,
                                     pc.foveationEdgeRatio.y) * pc.foveationEyeSizeRatio.y;
    }
    corrected = clamp(corrected, vec2(0.0), vec2(1.0));
    return mix(pc.eyeSourceMin, pc.eyeSourceMax, corrected);
}

// Full-range BT.709 with chroma centred on 128, as the runtime's PyroWave encode writes it.
vec3 sampleVideo(vec2 eyeUv)
{
    vec2 uv = mapEyeUvToSource(clamp(eyeUv, vec2(0.0), vec2(1.0)));
    float y = texture(uTextureY, uv).r;
    float cb = texture(uTextureCb, uv).r - 128.0 / 255.0;
    float cr = texture(uTextureCr, uv).r - 128.0 / 255.0;
    return clamp(vec3(y + 1.5748 * cr, y - 0.1873 * cb - 0.4681 * cr, y + 1.8556 * cb), vec3(0.0), vec3(1.0));
}

float luma(vec3 color)
{
    return dot(color, vec3(0.299, 0.587, 0.114));
}

void main()
{
    vec2 eyeUv = clamp(vUV, vec2(0.0), vec2(1.0));
    if (pc.reprojectionWarpEnabled != 0)
    {
        vec2 centered = eyeUv - vec2(0.5);
        float edgeWeight = 1.0 + dot(centered, centered);
        eyeUv = clamp(eyeUv + pc.reprojectionWarpOffset * edgeWeight, vec2(0.0), vec2(1.0));
    }
    vec3 color = sampleVideo(eyeUv);
    if (pc.clientUpscalingEnabled != 0)
    {
        vec2 stepUv = max(pc.logicalTexelSize, vec2(0.00001));
        vec3 left = sampleVideo(eyeUv + vec2(-stepUv.x, 0.0));
        vec3 right = sampleVideo(eyeUv + vec2(stepUv.x, 0.0));
        vec3 up = sampleVideo(eyeUv + vec2(0.0, -stepUv.y));
        vec3 down = sampleVideo(eyeUv + vec2(0.0, stepUv.y));
        float edgeVote = abs(luma(left) - luma(right)) + abs(luma(up) - luma(down));
        if (edgeVote > pc.upscaleEdgeThreshold)
        {
            vec3 detail = color * 4.0 - left - right - up - down;
            color = clamp(color + detail * (0.125 * pc.upscaleSharpness), vec3(0.0), vec3(1.0));
        }
    }
    float alpha = 1.0;
    if (pc.passthroughAlphaEnabled != 0)
    {
        float maxChannel = max(max(color.r, color.g), color.b);
        alpha = smoothstep(0.02, 0.10, maxChannel);
    }
    fragColor = vec4(color, alpha);
}
