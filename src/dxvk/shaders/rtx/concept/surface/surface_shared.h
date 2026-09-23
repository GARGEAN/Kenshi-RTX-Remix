#ifndef DXVK_REMIX_SHARED_SURFACE_SHARED_H
#define DXVK_REMIX_SHARED_SURFACE_SHARED_H
/*
* Copyright (c) 2022-2023, NVIDIA CORPORATION. All rights reserved.
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
#ifndef RTX_CONCEPT_SURFACE_SURFACE_SHARED_H_DX11V225
#define RTX_CONCEPT_SURFACE_SURFACE_SHARED_H_DX11V225 // DX11_V225_GUARD

enum class BlendType : uint8_t {
  kAlpha = 0,
  kAlphaEmissive = 1,
  kReverseAlphaEmissive = 2,
  kColor = 3,
  kColorEmissive = 4,
  kReverseColorEmissive = 5,
  kEmissive = 6,
  kMultiplicative = 7,
  kDoubleMultiplicative = 8,
  kReverseAlpha = 9,
  kReverseColor = 10,

  kMinValue = 0, // kAlpha
  kMaxValue = 10, // kReverseColor
};

static const uint32_t surfaceBlendTypeMask = 0xfu;

// Note: Use caution when changing this enum, must match the values defined on the MDL side of things
// as well as matching Vulkan's alpha compare operations due to conversions from these values at some points
// in the code.
enum class AlphaTestType : uint8_t {
  kNever = 0,
  kLess = 1,
  kEqual = 2,
  kLessOrEqual = 3,
  kGreater = 4,
  kNotEqual = 5,
  kGreaterOrEqual = 6,
  kAlways = 7,

  kMinValue = 0, // kNever
  kMaxValue = 7, // kAlways
};

static const uint32_t alphaTestTypeMask = 0x7u;

// Where a surface's colour or alpha channel gets its value.
//
// D3D11 has no fixed-function texture stages: the game's pixel shader already
// produced the final colour, and the capture layer's job is only to record which
// input it came from. This replaces the previous two-argument/eight-operation
// combiner, whose full generality was never used - the DX11 capture path only
// ever emitted "the texture", "the texture modulated by vertex colour", and
// "a constant".
enum class D3D11ColorSource : uint8_t {
  Texture = 0,    // The selected albedo / emissive texture.
  VertexColor,    // Interpolated vertex colour (rgb, or a for the alpha channel).
  BlendConstant,  // The OMSetBlendState blend factor carried on the surface.

  Count
};

enum class TexGenMode : uint8_t {
  None = 0,
  ViewPositions,
  CascadedViewPositions,
  ViewNormals,
  // DX11_V392_WORLD_PROJECTED_UV: texture coordinates generated from the hit's
  // WORLD position rather than its view position, so the mapping is stationary
  // as the camera moves. Games that project a map over the world compute this
  // UV inside the pixel shader from an interpolated world position, which is
  // not a vertex attribute and so cannot be captured as one; the surface's
  // textureTransform carries the affine the shader applied.
  WorldPositions,
  KenshiTriplanar,
  KenshiTerrainFeature,

  Count
};

enum class DisplacementMode : uint32_t {
  Off = 0,
  RaymarchPOM = 1,
  QuadtreePOM = 2
};

enum class RussianRouletteMode : uint32_t {
  ThroughputBased = 0,
  SpecularBased = 1
};
#endif // DXVK_REMIX_SHARED_SURFACE_SHARED_H

#endif // RTX_CONCEPT_SURFACE_SURFACE_SHARED_H_DX11V225
