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
#include "kenshi_heat_haze_depth.h"

layout(r32f, binding = KENSHI_HEAT_HAZE_DEPTH_BINDING_OUTPUT)
RWTexture2D<float> Output;

layout(r32f, binding = KENSHI_HEAT_HAZE_DEPTH_BINDING_HIT_DISTANCE_INPUT)
Texture2D<float> PrimaryHitDistance;

layout(push_constant)
ConstantBuffer<KenshiHeatHazeDepthArgs> cb;

[shader("compute")]
[numthreads(16, 16, 1)]
void main(uint2 pixel : SV_DispatchThreadID) {
  if (any(pixel >= cb.extent)) {
    return;
  }

  // Radial distance from the camera along the primary ray, in world units -
  // the same quantity Kenshi's deferred shaders write as
  // length(worldPos - cameraPos). geometryResolverOutputMiss stores -1 for a
  // ray that hit nothing.
  const float hitDistance = PrimaryHitDistance[pixel];

  // A miss must land on exactly 0, not on a small positive value: the consumer
  // tests `depth == 0` and treats it as sky, which is what raster did with the
  // untouched clear value. saturate() also keeps a hit from ever reaching 0
  // through the far clip, so the two cases stay distinguishable.
  Output[pixel] = (hitDistance > 0.0f) ? saturate(hitDistance * cb.invFarClip) : 0.0f;
}
