#include "kenshi_sign_overlay.h"
#include "rtx/utility/color.slangh"

layout(rgba16f, binding = 0) RWTexture2D<float4> Output;
layout(binding = 1) Texture2D<float4> SignColor;
layout(binding = 2) Texture2D<float> SignDepth;
layout(binding = 3) Texture2D<float> PrimaryViewZ;
layout(push_constant) ConstantBuffer<KenshiSignOverlayArgs> cb;

float projectedDepth(float viewZ) {
  float z = abs(viewZ) * (cb.projection.z < 0.0 ? -1.0 : 1.0);
  return (cb.projection.x * z + cb.projection.y)
       / (cb.projection.z * z + cb.projection.w);
}

float signDepthAt(int2 pixel) {
  return (SignDepth[pixel] - cb.viewportDepth.x) * cb.viewportDepth.y;
}

float depthSlope(int2 pixel, int2 axis, float center) {
  int2 lo = pixel - axis;
  int2 hi = pixel + axis;
  bool haveLo = all(lo >= 0) && SignColor[max(lo, int2(0))].a > 0.0;
  bool haveHi = all(hi < int2(cb.signExtent))
             && SignColor[min(hi, int2(cb.signExtent) - 1)].a > 0.0;
  float a = haveLo ? center - signDepthAt(lo) : 0.0;
  float b = haveHi ? signDepthAt(hi) - center : 0.0;
  // Prefer the same face at a silhouette or a crease in the sign mesh.
  return haveLo && haveHi ? (abs(a) < abs(b) ? a : b) : (haveLo ? a : b);
}

[shader("compute")]
[numthreads(16, 16, 1)]
void main(uint2 pixel : SV_DispatchThreadID) {
  if (any(pixel >= cb.outputExtent)) return;
  float2 uv = (float2(pixel) + 0.5) / float2(cb.outputExtent);
  int2 signPixel = min(int2(uv * float2(cb.signExtent)), int2(cb.signExtent) - 1);
  float4 overlay = SignColor[signPixel];
  if (overlay.a == 0.0) return;

  float signDepth = signDepthAt(signPixel);
  float2 slope = float2(depthSlope(signPixel, int2(1, 0), signDepth),
                        depthSlope(signPixel, int2(0, 1), signDepth));
  // Compare at the PT samples' actual positions, including camera jitter and
  // resolution scaling. Projected depth is affine across a triangle. Moving its
  // plane to each sample prevents a sloping sign from occluding its own overlay.
  // Blend visibility, never foreground/background depths, at an occlusion edge.
  float2 depthPixel = uv * float2(cb.depthExtent) - 0.5 + cb.depthPixelShift;
  int2 base = int2(floor(depthPixel));
  float2 f = frac(depthPixel);
  float visibility = 0.0;
  for (uint i = 0; i < 4; ++i) {
    int2 q = clamp(base + int2(i & 1, i >> 1), int2(0), int2(cb.depthExtent) - 1);
    float z = PrimaryViewZ[q];
    bool miss = z == cb.missViewZ || !isfinite(z) || z == 0.0;
    float2 samplePosition = (float2(q) + 0.5 - cb.depthPixelShift)
                         / float2(cb.depthExtent) * float2(cb.signExtent) - 0.5;
    float planeDepth = signDepth + dot(slope, samplePosition - float2(signPixel));
    float weight = ((i & 1) != 0 ? f.x : 1.0 - f.x) * ((i & 2) != 0 ? f.y : 1.0 - f.y);
    // Native constant+slope bias is already in SignDepth. The epsilon covers
    // a few D32 ULPs between independently transformed raster and RT positions.
    if (miss || planeDepth <= projectedDepth(z) + 8.0 * 1.192092896e-7)
      visibility += weight;
  }
  if (visibility == 0.0) return;
  float4 color = Output[pixel];
  // Only the exact (1, 0.9, 0.4, 0.6) SignHighlight material enters this pass. The capture supplies
  // coverage and its native 0.9..1 shading; state the authored colour and opacity explicitly at final
  // composition, keeping 40% of the displayed sign's contrast even at full visibility.
  float shade = clamp(overlay.r, 0.9, 1.0);
  float3 yellow = float3(1.0, 0.9, 0.4) * shade;
  float3 displayColor = cb.outputIsGammaEncoded != 0 ? color.rgb : linearToGamma(max(color.rgb, 0.0));
  displayColor = lerp(displayColor, yellow, 0.6 * saturate(visibility));
  // Capture output is linear display RGB; normal output may already be gamma
  // encoded. Use the tone mapper's conversion so both show the same overlay.
  color.rgb = cb.outputIsGammaEncoded != 0 ? displayColor : gammaToLinear(max(displayColor, 0.0));
  Output[pixel] = color;
}
