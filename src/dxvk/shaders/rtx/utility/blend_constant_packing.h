#ifndef RTX_UTILITY_BLEND_CONSTANT_PACKING_H_DX11V225
#define RTX_UTILITY_BLEND_CONSTANT_PACKING_H_DX11V225 // DX11_V225_GUARD

// Packing for the surface blend constant (the D3D11 OMSetBlendState blend factor).
//
// The CPU writes the surface struct and the shaders read it, so the pack and
// unpack halves must agree exactly. Both live here, in one file, sharing the same
// constants - rather than relying on a host-side packer matching a separately
// written shader-side one.
//
// Encoding: rgb as R11G11B10 in a single 32-bit word, which is the slot the old
// packed D3DCOLOR texture factor occupied. That keeps the surface struct the same
// size while raising precision from 8/8/8 to 11/11/10. Alpha is carried
// separately (see the surface's texture-flag bits) because blend alpha is the
// channel that most often needs range beyond the rgb channels.
//
// UNORM semantics: quantize saturate(x) * maxValue with round-to-nearest, and
// dequantize by dividing by the same maxValue. Blend factors are weights, so the
// [0, 1] domain is the meaningful one.

#ifdef __cplusplus
#include <cstdint>
#include <algorithm>
#define BLEND_PACK_SATURATE(x) (std::min(1.0f, std::max(0.0f, (x))))
#define BLEND_PACK_ROUND(x)    (uint32_t((x) + 0.5f))
#else
#define BLEND_PACK_SATURATE(x) (saturate(x))
#define BLEND_PACK_ROUND(x)    (uint32_t((x) + 0.5f))
#endif

// Channel widths. Red and green get the extra bit because the blue channel is
// the least perceptually significant, matching the usual R11G11B10 convention.
#define BLEND_CONSTANT_R_BITS 11
#define BLEND_CONSTANT_G_BITS 11
#define BLEND_CONSTANT_B_BITS 10

#define BLEND_CONSTANT_R_MAX 2047.0f
#define BLEND_CONSTANT_G_MAX 2047.0f
#define BLEND_CONSTANT_B_MAX 1023.0f

#define BLEND_CONSTANT_R_MASK 0x7ffu
#define BLEND_CONSTANT_G_MASK 0x7ffu
#define BLEND_CONSTANT_B_MASK 0x3ffu

#ifdef __cplusplus
namespace dxvk {
#endif

  // Packs a [0, 1] rgb blend constant into a single 32-bit word.
  inline uint32_t packBlendConstantRGB(float r, float g, float b) {
    const uint32_t rq = BLEND_PACK_ROUND(BLEND_PACK_SATURATE(r) * BLEND_CONSTANT_R_MAX);
    const uint32_t gq = BLEND_PACK_ROUND(BLEND_PACK_SATURATE(g) * BLEND_CONSTANT_G_MAX);
    const uint32_t bq = BLEND_PACK_ROUND(BLEND_PACK_SATURATE(b) * BLEND_CONSTANT_B_MAX);

    return (rq & BLEND_CONSTANT_R_MASK)
         | ((gq & BLEND_CONSTANT_G_MASK) << BLEND_CONSTANT_R_BITS)
         | ((bq & BLEND_CONSTANT_B_MASK) << (BLEND_CONSTANT_R_BITS + BLEND_CONSTANT_G_BITS));
  }

#ifndef __cplusplus
  // Unpacks the word written by packBlendConstantRGB. Shader-side only; the host
  // never needs to read the value back out of the surface struct.
  vec3 unpackBlendConstantRGB(uint32_t packed) {
    return vec3(
      float( packed                                                    & BLEND_CONSTANT_R_MASK) / BLEND_CONSTANT_R_MAX,
      float((packed >> BLEND_CONSTANT_R_BITS)                          & BLEND_CONSTANT_G_MASK) / BLEND_CONSTANT_G_MAX,
      float((packed >> (BLEND_CONSTANT_R_BITS + BLEND_CONSTANT_G_BITS)) & BLEND_CONSTANT_B_MASK) / BLEND_CONSTANT_B_MAX);
  }
#endif

#ifdef __cplusplus
} // namespace dxvk
#endif

#endif // RTX_UTILITY_BLEND_CONSTANT_PACKING_H_DX11V225
