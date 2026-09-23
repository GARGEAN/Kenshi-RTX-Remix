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
#include "kenshi_blood_projection_indices.h"
// interleave_geometry.h uses InterleaveGeometryArgs and the INTERLEAVE_GEOMETRY_*
// flags, which live in its indices header - it does not include them itself, so
// the order here matters exactly as it does in interleave_geometry.comp.slang.
#include "interleave_geometry_indices.h"
#include "interleave_geometry.h"

layout(binding = KENSHI_BLOOD_PROJECTION_BINDING_OUTPUT)
RWStructuredBuffer<uint32_t> dst;

layout(binding = KENSHI_BLOOD_PROJECTION_BINDING_POSITION_INPUT)
StructuredBuffer<float> srcPosition;
layout(binding = KENSHI_BLOOD_PROJECTION_BINDING_NORMAL_INPUT)
StructuredBuffer<float> srcNormal;
layout(binding = KENSHI_BLOOD_PROJECTION_BINDING_PART_INPUT)
StructuredBuffer<uint32_t> srcPart;

layout(push_constant)
ConstantBuffer<KenshiBloodProjectionArgs> cb;

// 1 / (2 * pi). The game writes atan2(..) / pi * 0.5, which is the same thing.
static const float kInvTwoPi = 0.15915494309189535f;

[shader("compute")]
[numthreads(128, 1, 1)]
void main(uint idx : SV_DispatchThreadID)
{
  if (idx >= cb.vertexCount)
  {
    return;
  }

  // Reuse the interleaver's format decoders so this pass supports every vertex
  // format the rest of the geometry path already does, rather than the four the
  // old CPU bake hand-rolled.
  const float3 position = interleaver::convert(
    cb.positionFormat, srcPosition, idx * cb.positionStride + cb.positionOffset);
  const float3 normal = interleaver::convert(
    cb.normalFormat, srcNormal, idx * cb.normalStride + cb.normalOffset);

  // skin.hlsl main_vs (BLOOD): a cylinder about Y for skinned bodies, and
  // character.hlsl severed_limb_vs: a cylinder about X, taking atan2 over a
  // different pair of normal axes. Both read the BIND-POSE attributes, which is
  // why this is a static per-mesh bake at all.
  const float axis = (cb.mode == 1u) ? position.y : position.x;
  const float angle = (cb.mode == 1u)
    ? atan2(normal.x, normal.z) * kInvTwoPi
    : atan2(normal.y, normal.z) * kInvTwoPi;

  // oWet.y = bloodAmount[partData.x]. The region is a per-vertex index into an
  // eight-float constant; the amount itself is per draw and stays on the surface.
  uint region = 0u;
  if (cb.hasPartData != 0u)
  {
    // R16G16_SINT: partData.x is the low 16 bits, signed.
    const uint word = srcPart[idx * cb.partStride + cb.partOffset];
    const int raw = int(word << 16) >> 16;
    region = uint(clamp(raw, 0, 7));
  }

  // Two words per vertex: [axis|angle] as half2, then [normal.z as half | region].
  // normal.z is kept rather than derived from the angle because the game's own
  // seam test is `if (blood.w < 0)` on the interpolated normal.z, and bloodScale.x
  // can scale the angle away from that relationship.
  dst[idx * 2u + 0u] = (f32tof16(axis) & 0xFFFFu) | (f32tof16(angle) << 16u);
  dst[idx * 2u + 1u] = (f32tof16(normal.z) & 0xFFFFu) | ((region & 0xFFu) << 16u);
}
