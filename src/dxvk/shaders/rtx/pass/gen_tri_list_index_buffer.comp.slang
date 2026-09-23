/*
* Copyright (c) 2021-2024, NVIDIA CORPORATION. All rights reserved.
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
#include "gen_tri_list_index_buffer_indices.h"

// Borrowed from the vulkan_core.h file: 
#define VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST  3
#define VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP 4
#define VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN   5

#include "gen_tri_list_index_buffer.h"

layout(binding = GEN_TRILIST_BINDING_OUTPUT)
RWStructuredBuffer<uint16_t> dst;

layout(binding = GEN_TRILIST_BINDING_INPUT)
StructuredBuffer<uint16_t> src;

layout(binding = GEN_TRILIST_BINDING_KENSHI_PART_INPUT)
StructuredBuffer<uint16_t> kenshiPartData;

layout(push_constant)
ConstantBuffer<GenTriListArgs> cb;


[shader("compute")]
[numthreads(128, 1, 1)]
void main(uint idx : SV_DispatchThreadID)
{
  if (idx >= cb.primCount)
  {
    return;
  }

  generateIndices(idx, dst, src, cb);

  // skin.hlsl writes a per-vertex clip value from `(partData.y & hiddenMask)` and character.hlsl clips
  // every pixel where it is non-zero, so one masked vertex removes the triangle. PT has no raster clip
  // stage: make the same triangle degenerate, keeping index count and primitive ID.
  if (cb.kenshiHiddenMask != 0u)
  {
    uint tri[3];
    [unroll]
    for (uint corner = 0u; corner < 3u; ++corner)
    {
      const uint pos = idx * 3u + corner;
      tri[corner] = cb.useUint32 != 0u
        ? uint32_t(dst[pos * 2u]) | (uint32_t(dst[pos * 2u + 1u]) << 16u)
        : uint32_t(dst[pos]);
    }

    bool hidden = false;
    [unroll]
    for (uint corner = 0u; corner < 3u; ++corner)
    {
      // R16G16_SINT: y is the second 16-bit word. Sign-extend exactly as the
      // D3D11 vertex fetch does before the integer bitwise AND.
      const uint word = uint32_t(kenshiPartData[
        cb.kenshiPartOffset + tri[corner] * cb.kenshiPartStride + 1u]);
      const int partY = int(word << 16u) >> 16;
      hidden = hidden || ((uint(partY) & cb.kenshiHiddenMask) != 0u);
    }

    if (hidden)
    {
      genTriListStoreIndex(dst, idx * 3u + 0u, 0u, cb.useUint32);
      genTriListStoreIndex(dst, idx * 3u + 1u, 0u, cb.useUint32);
      genTriListStoreIndex(dst, idx * 3u + 2u, 0u, cb.useUint32);
    }
  }
}
