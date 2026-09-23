#pragma once

#include <cstdint>

namespace dxvk {

  // API-agnostic specialization constant IDs for the RTX pipeline.
  // These map 1:1 to Vulkan spec constant slots regardless of the
  // frontend API (D3D11 or Remix API).
  enum RtxSpecConstantId : uint32_t {
    AlphaCompareOp  = 0,
    SamplerType     = 1,
    FogEnabled      = 2,
    VertexFogMode   = 3,
    PixelFogMode    = 4,

    PointMode       = 5,
    ProjectionType  = 6,

    VertexShaderBools = 7,
    PixelShaderBools  = 8,
    Fetch4            = 9,

    SamplerDepthMode  = 10,

    CustomVertexTransformEnabled = 11,
    ReplacementTextureCategory = 12,
    ClipSpaceJitterEnabled = 13,

    Count
  };

}
