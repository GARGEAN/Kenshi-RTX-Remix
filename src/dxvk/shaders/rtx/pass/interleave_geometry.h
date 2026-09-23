/*
* Copyright (c) 2021-2026, NVIDIA CORPORATION. All rights reserved.
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
#ifndef RTX_PASS_INTERLEAVE_GEOMETRY_H_DX11V225
#define RTX_PASS_INTERLEAVE_GEOMETRY_H_DX11V225 // DX11_V225_GUARD

#include "../utility/packing_helpers.h"

// This function can be executed on the CPU or GPU!!
#ifdef __cplusplus
#define asfloat(x) *reinterpret_cast<const float*>(&x)
#define asuint(x) *reinterpret_cast<const uint32_t*>(&x)
#define WriteBuffer(T) T*
#define ReadBuffer(T) const T*

// CPU-side half-float to float conversion (GPU uses the f16tof32 intrinsic built-in)
#include "../utility/f16_conversion.h"

#else
#define WriteBuffer(T) RWStructuredBuffer<T>
#define ReadBuffer(T) StructuredBuffer<T>
#endif

namespace interleaver {

  enum SupportedVkFormats : uint32_t {
    VK_FORMAT_UNDEFINED = 0,
    VK_FORMAT_R8G8B8A8_UNORM = 37,
    VK_FORMAT_A2B10G10R10_SNORM_PACK32 = 65,

    // Passthrough format mapping
    VK_FORMAT_B8G8R8A8_UNORM = 44,
    VK_FORMAT_R16G16_SFLOAT = 83,
    // DX11_V286_HALF4_POSITIONS: half-float positions (Skyrim SE stores nearly
    // all static world geometry this way in DEVICE-LOCAL buffers, so CPU-side
    // conversion is impossible and rejecting dropped the entire world).
    VK_FORMAT_R16G16B16A16_SFLOAT = 97,
    VK_FORMAT_R32G32_SFLOAT = 103,
    VK_FORMAT_R32G32B32_SFLOAT = 106,
    VK_FORMAT_R32G32B32A32_SFLOAT = 109,
  };

  static inline int signExtend10(uint x) {
    // keep lower 10 bits and sign extend to 32-bit integer
    return (int(x << 22)) >> 22;
  }

  static inline float snorm10ToF32_packed(uint x) {
    const int v = signExtend10(x);
    float f = float(v) / 511.0f;
    // clamp to [-1, 1] (since -512 / 511 < -1)
    f = (f < -1.0f) ? -1.0f : ((f > 1.0f) ? 1.0f : f);
    return f;
  }

  bool formatConversionFloatSupported(uint32_t format) {
    switch (format) {
    case SupportedVkFormats::VK_FORMAT_R16G16_SFLOAT:
    case SupportedVkFormats::VK_FORMAT_R16G16B16A16_SFLOAT:
    case SupportedVkFormats::VK_FORMAT_R32G32_SFLOAT:
    case SupportedVkFormats::VK_FORMAT_R32G32B32_SFLOAT:
    case SupportedVkFormats::VK_FORMAT_R32G32B32A32_SFLOAT:
    case SupportedVkFormats::VK_FORMAT_R8G8B8A8_UNORM:
    case SupportedVkFormats::VK_FORMAT_A2B10G10R10_SNORM_PACK32:
      return true;
    default:
      return false;
    }
  }

  bool formatConversionUintSupported(uint32_t format) {
    switch (format) {
    case SupportedVkFormats::VK_FORMAT_B8G8R8A8_UNORM:
    // DX11_V548_RGBA8_VERTEX_COLOR: RGBA8 vertex colours, decoded here rather
    // than converted on the CPU. Kenshi declares COLOR0 this way on every
    // object, in DEVICE-LOCAL buffers, so the CPU path could never map them and
    // dropped the channel outright - the same failure V286 fixed for Skyrim
    // SE's half-float positions, and for the same reason.
    case SupportedVkFormats::VK_FORMAT_R8G8B8A8_UNORM:
      return true;
    default:
      return false;
    }
  }

  float3 convert(uint32_t format, ReadBuffer(float) input, uint32_t index) {
    switch (format) {
    case SupportedVkFormats::VK_FORMAT_R16G16_SFLOAT:
    {
      // Two half-floats packed into one 32-bit word: [G(31:16) | R(15:0)] in memory
      uint data = asuint(input[index]);
      float r = f16tof32(data & 0xFFFFu);
      float g = f16tof32((data >> 16u) & 0xFFFFu);
      return float3(r, g, 0);
    }
    case SupportedVkFormats::VK_FORMAT_R16G16B16A16_SFLOAT:
    {
      // Four half-floats in two 32-bit words: [G(31:16)|R(15:0)] [A(31:16)|B(15:0)]; xyz used.
      uint d0 = asuint(input[index + 0]);
      uint d1 = asuint(input[index + 1]);
      return float3(f16tof32(d0 & 0xFFFFu), f16tof32((d0 >> 16u) & 0xFFFFu), f16tof32(d1 & 0xFFFFu));
    }
    case SupportedVkFormats::VK_FORMAT_R32G32_SFLOAT:
      return float3(input[index + 0], input[index + 1], 0);
    case SupportedVkFormats::VK_FORMAT_R32G32B32_SFLOAT:
    case SupportedVkFormats::VK_FORMAT_R32G32B32A32_SFLOAT:
      return float3(input[index + 0], input[index + 1], input[index + 2]);
    case SupportedVkFormats::VK_FORMAT_R8G8B8A8_UNORM:
    {
      uint data = asuint(input[index]);
      float b = unorm8ToF32(uint8_t((data >> 16) & 0xFF));
      float g = unorm8ToF32(uint8_t((data >> 8) & 0xFF));
      float r = unorm8ToF32(uint8_t((data >> 0) & 0xFF));
      return float3(r, g, b) * 2.f - 1.f;
    }
    case SupportedVkFormats::VK_FORMAT_A2B10G10R10_SNORM_PACK32:
    {
      uint data = asuint(input[index]);
      float b = snorm10ToF32_packed(data >> 20);
      float g = snorm10ToF32_packed(data >> 10);
      float r = snorm10ToF32_packed(data >> 0);
      return float3(r, g, b);
    }
    }
    return float3(1, 1, 1);
  }

  uint3 convert(uint32_t format, ReadBuffer(uint32_t) input, uint32_t index) {
    switch (format) {
    case SupportedVkFormats::VK_FORMAT_B8G8R8A8_UNORM:
      // Passthrough format we support in other places
      return uint3(input[index], 0, 0);
    // DX11_V548_RGBA8_VERTEX_COLOR: same 32-bit word, R and B exchanged.
    // In memory RGBA8 is [R,G,B,A] and BGRA8 is [B,G,R,A], so little-endian
    // this is a swap of bits 0..7 with bits 16..23; G and A keep their places.
    // The consumer (surface_interaction.slangh) unpacks BGRA unconditionally,
    // so normalising here keeps that single decode correct for both formats.
    case SupportedVkFormats::VK_FORMAT_R8G8B8A8_UNORM:
    {
      const uint32_t data = input[index];
      const uint32_t swapped = (data & 0xFF00FF00u)
                             | ((data >> 16u) & 0x000000FFu)
                             | ((data & 0x000000FFu) << 16u);
      return uint3(swapped, 0, 0);
    }
    }
    return uint3(1,1,1);
  }

  void interleave(const uint32_t idx, WriteBuffer(float) dst, ReadBuffer(float) srcPosition, ReadBuffer(float) srcNormal, ReadBuffer(float) srcTexcoord, ReadBuffer(uint32_t) srcColor0, const InterleaveGeometryArgs cb) {
    const uint32_t srcVertexIndex = idx + cb.minVertexIndex;

    uint32_t writeOffset = 0;

    const uint32_t srcPositionIndex =
      srcVertexIndex * cb.positionStride + cb.positionOffset;
    float3 position = convert(cb.positionFormat, srcPosition, srcPositionIndex);
    if ((cb.attributeFlags & INTERLEAVE_GEOMETRY_FLAG_HOMOGENEOUS_CLIP) != 0) {
#ifdef __cplusplus
      const vec4 clip(srcPosition[srcPositionIndex + 0],
                      srcPosition[srcPositionIndex + 1],
                      srcPosition[srcPositionIndex + 2],
                      srcPosition[srcPositionIndex + 3]);
      const mat4& m = cb.clipToPosition;
      if ((cb.attributeFlags & INTERLEAVE_GEOMETRY_FLAG_CLIP_W_DEPTH) != 0) {
        const float invXScale = m.m[0].x;
        const float invYScale = m.m[1].y;
        if (std::isfinite(clip.x) && std::isfinite(clip.y)
         && std::isfinite(clip.w) && std::isfinite(invXScale)
         && std::isfinite(invYScale) && std::abs(clip.w) > 1.0e-20f) {
          position = float3(clip.x * invXScale,
                            clip.y * invYScale,
                            clip.w);
        } else {
          position = float3(0.0f, 0.0f, 0.0f);
        }
      } else {
        const vec4 p = m.m[0] * clip.x + m.m[1] * clip.y
                     + m.m[2] * clip.z + m.m[3] * clip.w;
        if (std::isfinite(p.x) && std::isfinite(p.y)
         && std::isfinite(p.z) && std::isfinite(p.w)
         && std::abs(p.w) > 1.0e-20f) {
          const float invW = 1.0f / p.w;
          position = float3(p.x * invW, p.y * invW, p.z * invW);
        } else {
          position = float3(0.0f, 0.0f, 0.0f);
        }
      }
#else
      const float4 clip = float4(srcPosition[srcPositionIndex + 0],
                                 srcPosition[srcPositionIndex + 1],
                                 srcPosition[srcPositionIndex + 2],
                                 srcPosition[srcPositionIndex + 3]);
      if ((cb.attributeFlags & INTERLEAVE_GEOMETRY_FLAG_CLIP_W_DEPTH) != 0) {
        const float invXScale = cb.clipToPosition[0][0];
        const float invYScale = cb.clipToPosition[1][1];
        if (all(isfinite(float4(clip.x, clip.y, clip.w, invXScale)))
         && isfinite(invYScale) && abs(clip.w) > 1.0e-20f)
          position = float3(clip.x * invXScale,
                            clip.y * invYScale,
                            clip.w);
        else
          position = float3(0.0f, 0.0f, 0.0f);
      } else {
        const float4 p = mul(cb.clipToPosition, clip);
        if (all(isfinite(p)) && abs(p.w) > 1.0e-20f)
          position = p.xyz / p.w;
        else
          position = float3(0.0f, 0.0f, 0.0f);
      }
#endif
    }
    dst[idx * cb.outputStride + writeOffset++] = position.x;
    dst[idx * cb.outputStride + writeOffset++] = position.y;
    dst[idx * cb.outputStride + writeOffset++] = position.z;

    if ((cb.attributeFlags & INTERLEAVE_GEOMETRY_FLAG_HAS_NORMALS) != 0) {
      float3 normals = convert(cb.normalFormat, srcNormal, srcVertexIndex * cb.normalStride + cb.normalOffset);
      dst[idx * cb.outputStride + writeOffset++] = normals.x;
      dst[idx * cb.outputStride + writeOffset++] = normals.y;
      dst[idx * cb.outputStride + writeOffset++] = normals.z;
    } else if ((cb.attributeFlags & INTERLEAVE_GEOMETRY_FLAG_FORCE_NORMALS) != 0) {
      // Reserve normal space with zeros; will be filled by smooth normals pass
      dst[idx * cb.outputStride + writeOffset++] = 0.0f;
      dst[idx * cb.outputStride + writeOffset++] = 0.0f;
      dst[idx * cb.outputStride + writeOffset++] = 0.0f;
    }

    if ((cb.attributeFlags & INTERLEAVE_GEOMETRY_FLAG_HAS_TEXCOORD) != 0) {
      float3 texcoords = convert(cb.texcoordFormat, srcTexcoord, srcVertexIndex * cb.texcoordStride + cb.texcoordOffset);
      dst[idx * cb.outputStride + writeOffset++] = texcoords.x;
      dst[idx * cb.outputStride + writeOffset++] = texcoords.y;
    }

    if ((cb.attributeFlags & INTERLEAVE_GEOMETRY_FLAG_HAS_COLOR0) != 0) {
      const uint32_t srcColorIndex = srcVertexIndex * cb.color0Stride + cb.color0Offset;
      uint3 color0 = convert(cb.color0Format, srcColor0, srcColorIndex);
      dst[idx * cb.outputStride + writeOffset++] = asfloat(color0.x);
    }
  }
}

#ifdef __cplusplus
#undef WriteBuffer
#undef ReadBuffer

#undef asfloat
#undef asuint
#endif

#endif // RTX_PASS_INTERLEAVE_GEOMETRY_H_DX11V225
