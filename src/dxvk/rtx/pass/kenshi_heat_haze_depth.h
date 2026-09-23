/*
* Copyright (c) 2026, NVIDIA CORPORATION. All rights reserved.
*
* Permission is hereby granted, free of charge, to any person obtaining a
* copy of this software and associated documentation files (the "Software"),
* to deal in the Software without restriction, including without limitation
* the rights to use, copy, modify, merge, publish, distribute, sublicense,
* and/or sell copies of the Software, and to permit persons to whom the
* Software is furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included in
* all copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
* THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
* FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
* DEALINGS IN THE SOFTWARE.
*/
#ifndef RTX_PASS_KENSHI_HEAT_HAZE_DEPTH_H_DX11V759
#define RTX_PASS_KENSHI_HEAT_HAZE_DEPTH_H_DX11V759

#include "rtx/utility/shader_types.h"

// Re-encodes Remix's primary hit distance into the contract Kenshi's heat-haze post-process expects
// from its deferred G-buffer. HeatHaze (data/materials/post/heathaze.hlsl) samples global_gbuffer
// target 2 (R32_FLOAT, `length(worldPos - cameraPos) / farClip`) and scales the distortion by it:
//   float depth = tex2D(depthMap, uv).r;
//   if (depth == 0.0) depth = 1.0;        // nothing drawn here (sky) -> FULL
//   normal *= saturate(depth * 6) * 0.002 * heatHaze;
// Under path tracing the draws that write that target never reach raster, so it stays 0 and the haze
// runs at full amplitude everywhere. Remix writes -1 for a miss; the target wants 0 there (sky gets
// full haze, as in raster).
struct KenshiHeatHazeDepthArgs {
  uint2 extent;      // render (downscaled) extent - the hit distance resolution
  float invFarClip;  // 1 / (farClip * distanceScale)
  float pad0;
};

#ifdef __cplusplus
static_assert(sizeof(KenshiHeatHazeDepthArgs) == 16,
              "KenshiHeatHazeDepthArgs must stay within DXVK's 128-byte push-constant bank");
#endif

#define KENSHI_HEAT_HAZE_DEPTH_BINDING_OUTPUT             0
#define KENSHI_HEAT_HAZE_DEPTH_BINDING_HIT_DISTANCE_INPUT 1

#endif // RTX_PASS_KENSHI_HEAT_HAZE_DEPTH_H_DX11V759
