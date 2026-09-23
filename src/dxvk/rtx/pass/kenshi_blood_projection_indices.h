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
#ifndef RTX_PASS_KENSHI_BLOOD_PROJECTION_INDICES_H_DX11V524
#define RTX_PASS_KENSHI_BLOOD_PROJECTION_INDICES_H_DX11V524

#include "rtx/utility/shader_types.h"

// Bakes Kenshi's bind-pose cylindrical blood projection on the GPU, straight from the vertex buffers
// the draw binds (the CPU vertex shadow is missing for some buffers, e.g. partData, and can be stale).
// All offsets/strides are in ELEMENTS of the bound view, not bytes: position/normal index a
// StructuredBuffer<float>, partData a StructuredBuffer<uint32_t>.
struct KenshiBloodProjectionArgs {
  uint32_t vertexCount;
  uint32_t mode;              // 1 = regional Y-cylinder, 2 = severed-limb X-cylinder
  uint32_t hasPartData;
  uint32_t positionOffset;

  uint32_t positionStride;
  uint32_t positionFormat;
  uint32_t normalOffset;
  uint32_t normalStride;

  uint32_t normalFormat;
  uint32_t partOffset;
  uint32_t partStride;
  uint32_t pad0;
};

#ifdef __cplusplus
static_assert(sizeof(KenshiBloodProjectionArgs) == 48,
              "KenshiBloodProjectionArgs must stay within DXVK's 128-byte push-constant bank");
#endif

#define KENSHI_BLOOD_PROJECTION_BINDING_OUTPUT          0
#define KENSHI_BLOOD_PROJECTION_BINDING_POSITION_INPUT  1
#define KENSHI_BLOOD_PROJECTION_BINDING_NORMAL_INPUT    2
#define KENSHI_BLOOD_PROJECTION_BINDING_PART_INPUT      3

#endif // RTX_PASS_KENSHI_BLOOD_PROJECTION_INDICES_H_DX11V524
