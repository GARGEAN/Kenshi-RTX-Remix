#pragma once
#include "rtx/utility/shader_types.h"

// V792. Private pass arguments; no shared scene/material layout changes.
struct KenshiSignOverlayArgs {
  uint2 outputExtent;
  uint2 signExtent;
  uint2 depthExtent;
  float2 depthPixelShift;
  float4 projection; // P[2][2], P[3][2], P[2][3], P[3][3]
  float2 viewportDepth; // minDepth, 1 / (maxDepth - minDepth)
  float missViewZ;
  uint outputIsGammaEncoded;
};

#ifdef __cplusplus
static_assert(sizeof(KenshiSignOverlayArgs) == 64);
#endif
