struct EyeFitParams {
  uint width;
  uint eyeWidth;
  uint eyeHeight;
  uint chromaStride;
  uint chromaWidth;
  uint chromaHeight;
  uint fitWidth;
  uint fitHeight;
  uint offsetX;
  uint offsetY;
  uint canvasWidth;
  uint canvasHeight;
  uint taps;
};

[[vk::binding(0, 0)]]
ConstantBuffer<EyeFitParams> params;
[[vk::binding(1, 0)]]
StructuredBuffer<uint> luma;
[[vk::binding(2, 0)]]
StructuredBuffer<uint> cb;
[[vk::binding(3, 0)]]
StructuredBuffer<uint> cr;
[[vk::binding(4, 0)]]
StructuredBuffer<int> first;
[[vk::binding(5, 0)]]
StructuredBuffer<float> weight;
[[vk::binding(6, 0)]]
RWStructuredBuffer<uint> bgra;

[shader("compute")] [numthreads(64, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
  uint x = tid.x;
  uint y = tid.y;
  if (((x >= params.fitWidth) || (y >= params.fitHeight))) {
    return;
  }
  int fx = first[x];
  int fy = first[(params.fitWidth + y)];
  float r = 0.000000;
  float g = 0.000000;
  float b = 0.000000;
  for (uint ky = 0u; ky < params.taps; ++ky) {
    uint sy = uint(clamp((fy + int(ky)), 0, int((params.eyeHeight - 1u))));
    float wy = weight[(((params.fitWidth + y) * params.taps) + ky)];
    float fcy = clamp(((float(sy) * 0.500000) - 0.250000), 0.000000, float((params.chromaHeight - 1u)));
    uint cy0 = uint(fcy);
    uint cy1 = min((cy0 + 1u), (params.chromaHeight - 1u));
    float ty = (fcy - float(cy0));
    float hr = 0.000000;
    float hg = 0.000000;
    float hb = 0.000000;
    for (uint kx = 0u; kx < params.taps; ++kx) {
      uint sx = uint(clamp((fx + int(kx)), 0, int((params.eyeWidth - 1u))));
      float w = weight[((x * params.taps) + kx)];
      float yy = float(((luma[(((sy * params.width) + sx) >> 2u)] >> ((((sy * params.width) + sx) & 3u) * 8u)) & 255u));
      float fcx = clamp(((float(sx) * 0.500000) - 0.250000), 0.000000, float((params.chromaWidth - 1u)));
      uint cx0 = uint(fcx);
      uint cx1 = min((cx0 + 1u), (params.chromaWidth - 1u));
      float tx = (fcx - float(cx0));
      float pb = (((((float(((cb[(((cy0 * params.chromaStride) + cx0) >> 2u)] >> ((((cy0 * params.chromaStride) + cx0) & 3u) * 8u)) & 255u)) * (1.000000 - tx)) + (float(((cb[(((cy0 * params.chromaStride) + cx1) >> 2u)] >> ((((cy0 * params.chromaStride) + cx1) & 3u) * 8u)) & 255u)) * tx)) * (1.000000 - ty)) + (((float(((cb[(((cy1 * params.chromaStride) + cx0) >> 2u)] >> ((((cy1 * params.chromaStride) + cx0) & 3u) * 8u)) & 255u)) * (1.000000 - tx)) + (float(((cb[(((cy1 * params.chromaStride) + cx1) >> 2u)] >> ((((cy1 * params.chromaStride) + cx1) & 3u) * 8u)) & 255u)) * tx)) * ty)) - 128.000000);
      float pr = (((((float(((cr[(((cy0 * params.chromaStride) + cx0) >> 2u)] >> ((((cy0 * params.chromaStride) + cx0) & 3u) * 8u)) & 255u)) * (1.000000 - tx)) + (float(((cr[(((cy0 * params.chromaStride) + cx1) >> 2u)] >> ((((cy0 * params.chromaStride) + cx1) & 3u) * 8u)) & 255u)) * tx)) * (1.000000 - ty)) + (((float(((cr[(((cy1 * params.chromaStride) + cx0) >> 2u)] >> ((((cy1 * params.chromaStride) + cx0) & 3u) * 8u)) & 255u)) * (1.000000 - tx)) + (float(((cr[(((cy1 * params.chromaStride) + cx1) >> 2u)] >> ((((cy1 * params.chromaStride) + cx1) & 3u) * 8u)) & 255u)) * tx)) * ty)) - 128.000000);
      hr = (hr + (w * float(uint(min(max(((yy + (1.574800 * pr)) + 0.500000), 0.000000), 255.000000)))));
      hg = (hg + (w * float(uint(min(max((((yy - (0.187300 * pb)) - (0.468100 * pr)) + 0.500000), 0.000000), 255.000000)))));
      hb = (hb + (w * float(uint(min(max(((yy + (1.855600 * pb)) + 0.500000), 0.000000), 255.000000)))));
    }
    r = (r + (wy * hr));
    g = (g + (wy * hg));
    b = (b + (wy * hb));
  }
  uint o = ((((((params.canvasHeight - 1u) - params.offsetY) - y) * params.canvasWidth) + params.offsetX) + x);
  bgra[o] = (((uint(min(max((b + 0.500000), 0.000000), 255.000000)) | (uint(min(max((g + 0.500000), 0.000000), 255.000000)) << 8u)) | (uint(min(max((r + 0.500000), 0.000000), 255.000000)) << 16u)) | 4278190080u);
}
