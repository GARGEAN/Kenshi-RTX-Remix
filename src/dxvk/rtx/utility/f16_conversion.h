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
#ifndef RTX_UTILITY_F16_CONVERSION_H_DX11V225
#define RTX_UTILITY_F16_CONVERSION_H_DX11V225 // DX11_V225_GUARD

// CPU-side half-float (IEEE 754 binary16) to float conversion.
// On the GPU, f16tof32 is a built-in intrinsic; this header provides
// the equivalent for C++ host code.

#ifdef __cplusplus
#include <cstdint>
#include <cstring>

inline float f16tof32(uint32_t h16) {
  const uint32_t sign     = (h16 & 0x8000u) << 16u;
  const uint32_t exponent = (h16 >> 10u) & 0x1fu;
  const uint32_t mantissa =  h16 & 0x3ffu;
  uint32_t result;
  if (exponent == 0u) {
    if (mantissa == 0u) {
      result = sign; // +-zero
    } else {
      // Denormalized half -> normalized float
      uint32_t shiftedMantissa = mantissa;
      uint32_t shiftCount = 0u;
      while (!(shiftedMantissa & 0x400u)) {
        shiftedMantissa <<= 1u;
        ++shiftCount;
      }
      shiftedMantissa &= 0x3ffu;
      result = sign | ((113u - shiftCount) << 23u) | (shiftedMantissa << 13u);
    }
  } else if (exponent == 31u) {
    result = sign | 0x7f800000u | (mantissa << 13u); // +-Inf or NaN
  } else {
    result = sign | ((exponent + 112u) << 23u) | (mantissa << 13u);
  }
  float f;
  std::memcpy(&f, &result, sizeof(f));
  return f;
}

// CPU-side float to half-float (IEEE 754 binary16) conversion, the inverse of
// f16tof32 above. On the GPU f32tof16 is a built-in intrinsic; host code needs
// this to pack values into the half-precision fields of GPU structs.
//
// Rounds to nearest-even, and saturates to +-Inf rather than wrapping when the
// input exceeds the half range (65504). Denormals are produced for very small
// magnitudes instead of being flushed, so round-tripping a half through
// f16tof32/f32tof16 is exact.
inline uint32_t f32tof16(float f) {
  uint32_t bits;
  std::memcpy(&bits, &f, sizeof(bits));

  const uint32_t sign     = (bits >> 16u) & 0x8000u;
  const int32_t  exponent = int32_t((bits >> 23u) & 0xffu) - 127 + 15;
  const uint32_t mantissa =  bits & 0x7fffffu;

  // Inf / NaN pass through; a NaN must stay a NaN (non-zero mantissa).
  if (((bits >> 23u) & 0xffu) == 0xffu) {
    return sign | 0x7c00u | (mantissa != 0u ? 0x200u : 0u);
  }

  if (exponent >= 0x1f) {
    return sign | 0x7c00u; // overflow -> +-Inf
  }

  if (exponent <= 0) {
    // Subnormal half, or underflow to zero.
    if (exponent < -10) {
      return sign;
    }
    const uint32_t subnormalMantissa = mantissa | 0x800000u;
    const uint32_t shift = uint32_t(14 - exponent);
    const uint32_t rounded =
      (subnormalMantissa + (1u << (shift - 1u)) - 1u + ((subnormalMantissa >> shift) & 1u)) >> shift;
    return sign | rounded;
  }

  const uint32_t roundedMantissa =
    (mantissa + 0x00000fffu + ((mantissa >> 13u) & 1u)) >> 13u;

  // Rounding can carry into the exponent; the shifted value absorbs it.
  return sign | ((uint32_t(exponent) << 10u) + roundedMantissa);
}

#endif // __cplusplus

#endif // RTX_UTILITY_F16_CONVERSION_H_DX11V225
