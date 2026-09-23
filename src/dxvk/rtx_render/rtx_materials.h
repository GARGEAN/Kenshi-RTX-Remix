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
#pragma once
#include "rtx/dx11/dx11_material_fog_state.h"
#include <memory>
#include <variant>

#include "rtx_texture.h"
#include "rtx_option.h"
#include "../../util/util_color.h"
#include "../../util/util_macro.h"
#include "rtx/utility/shared_constants.h"
#include "rtx/utility/blend_constant_packing.h"
#include "rtx/concept/surface/surface_shared.h"
#include "rtx/pass/common_binding_indices.h"

#ifndef OPAQUE_SURFACE_MATERIAL_FLAG_USE_SECONDARY_TEXTURE_FOR_OPACITY
#define OPAQUE_SURFACE_MATERIAL_FLAG_USE_SECONDARY_TEXTURE_FOR_OPACITY (1 << COMMON_MATERIAL_FLAG_TYPE_OFFSET(5))
#endif
#include "rtx/pass/instance_definitions.h"
#include "../../d3d11/d3d11_resource_slot.h"
#include "rtx_material_data.h"
#include "../../lssusd/mdl_helpers.h"
#include "rtx/pass/particles/particle_system_common.h"
#include "dxvk_constant_state.h"

namespace dxvk {
// Surfaces

// Todo: Compute size directly from sizeof of GPU structure (by including it), for now computed by sum of members manually
constexpr std::size_t kSurfaceGPUSize = 16 * 4 * 4;

// Note: Use caution when changing this enum, must match the values defined on the MDL side of things.

static bool isBlendTypeEmissive(const BlendType type) {
  switch (type) {
  default:
    return false;
  case BlendType::kAlphaEmissive:
  case BlendType::kReverseAlphaEmissive:
  case BlendType::kColorEmissive:
  case BlendType::kReverseColorEmissive:
  case BlendType::kEmissive:
    return true;
  }
}

static BlendType tryConvertToEmissive(const BlendType type) {
  switch (type) {
  case BlendType::kAlpha:
    return BlendType::kAlphaEmissive;
  case BlendType::kColor:
    return BlendType::kColorEmissive;
  default:
    return type;
  }
}

static_assert((int)AlphaTestType::kNever == (int)VkCompareOp::VK_COMPARE_OP_NEVER);
static_assert((int)AlphaTestType::kLess == (int)VkCompareOp::VK_COMPARE_OP_LESS);
static_assert((int)AlphaTestType::kEqual == (int)VkCompareOp::VK_COMPARE_OP_EQUAL);
static_assert((int)AlphaTestType::kLessOrEqual == (int)VkCompareOp::VK_COMPARE_OP_LESS_OR_EQUAL);
static_assert((int)AlphaTestType::kGreater == (int)VkCompareOp::VK_COMPARE_OP_GREATER);
static_assert((int)AlphaTestType::kNotEqual == (int)VkCompareOp::VK_COMPARE_OP_NOT_EQUAL);
static_assert((int)AlphaTestType::kGreaterOrEqual == (int)VkCompareOp::VK_COMPARE_OP_GREATER_OR_EQUAL);
static_assert((int)AlphaTestType::kAlways == (int)VkCompareOp::VK_COMPARE_OP_ALWAYS);

// Note: "Temporary" hacks to get RtxOptions data from this header file as we cannot include rtx_options directly
// due to cyclic includes. This should be removed once the rtx_materials implementation is moved to a source file.
bool getEnableDiffuseLayerOverrideHack();
float getEmissiveIntensity();
float getDisplacementFactor();
float getDisplacementInFactor();
float getDisplacementOutFactor();

struct RtEyeParams {
  // origin of eyeball in world space
  // used to calculate eye normals
  Vector3 eyeballOrigin = Vector3{ 0, 0, 0 };
  // right/up vectors that define an eye orientation
  // NOTE: vectors can be unnormalized, and that scale denotes an iris size 
  Vector3 eyeRightU = Vector3{ 1, 0, 0 };
  Vector3 eyeUpV = Vector3{ 0, 1, 0 };
};

struct RtSurface {
  RtSurface() {
  }

  void writeGPUData(unsigned char* data, std::size_t& offset, size_t surfaceIndex = SIZE_MAX) const {
    [[maybe_unused]] const std::size_t oldOffset = offset;

    // Note: Position buffer and surface material index are required for proper
    // behavior of the Surface on the GPU.
    assert(positionBufferIndex != kSurfaceInvalidBufferIndex);

    writeGPUHelperExplicit<2>(data, offset, positionBufferIndex);
    writeGPUHelperExplicit<2>(data, offset, previousPositionBufferIndex);
    writeGPUHelperExplicit<2>(data, offset, normalBufferIndex);
    writeGPUHelperExplicit<2>(data, offset, texcoordBufferIndex);
    writeGPUHelperExplicit<2>(data, offset, indexBufferIndex);
    writeGPUHelperExplicit<2>(data, offset, color0BufferIndex);

    uint16_t flags0 = 0;
    flags0 |= normalFormat == VK_FORMAT_R32_UINT ? 1 : 0;
    flags0 |= isVertexColorBakedLighting ? (1 << 1) : 0;
    flags0 |= colorTextureIsSrgb ? (1 << 2) : 0;
    flags0 |= emissiveTextureIsSrgb ? (1 << 3) : 0;
    flags0 |= kenshiDistantTown ? (1 << 4) : 0;
    // Building under construction. On the surface, not the material: the opaque material's uint16 `flags`
    // is full (bits 2-15), and a flag past it is silently dropped. flags0 bits 6-15 remain free.
    flags0 |= kenshiConstruction ? (1 << 5) : 0;
    writeGPUHelper(data, offset, flags0);

    const uint16_t packedHash =
      (uint16_t) (associatedGeometryHash >> 48) ^
      (uint16_t) (associatedGeometryHash >> 32) ^
      (uint16_t) (associatedGeometryHash >> 16) ^
      (uint16_t) associatedGeometryHash;

    writeGPUHelper(data, offset, packedHash);

    writeGPUHelper(data, offset, positionOffset);
    writeGPUHelper(data, offset, normalOffset);
    writeGPUHelper(data, offset, texcoordOffset);
    writeGPUHelper(data, offset, color0Offset);
    writeGPUHelper(data, offset, objectPickingValue);

    writeGPUHelperExplicit<1>(data, offset, positionStride);
    writeGPUHelperExplicit<1>(data, offset, normalStride);
    writeGPUHelperExplicit<1>(data, offset, texcoordStride);
    writeGPUHelperExplicit<1>(data, offset, color0Stride);

    writeGPUHelperExplicit<3>(data, offset, firstIndex);
    writeGPUHelperExplicit<1>(data, offset, indexStride);

    // Note: Ensure alpha state values fit in the intended amount of bits allocated in the flags bitfield.
    assert(static_cast<uint32_t>(alphaState.alphaTestType) < (1 << 3));
    assert(static_cast<uint32_t>(alphaState.alphaTestReferenceValue) < (1 << 8));
    assert(static_cast<uint32_t>(alphaState.blendType) < (1 << 4));

    uint32_t flags1 = 0;

    flags1 |= isEmissive ? (1 << 0) : 0;
    flags1 |= alphaState.isFullyOpaque ? (1 << 1) : 0;
    flags1 |= isStatic ? (1 << 2) : 0;
    flags1 |= static_cast<uint32_t>(alphaState.alphaTestType) << 3;
    // Note: No mask needed as masking of this value to be 8 bit is done elsewhere.
    flags1 |= static_cast<uint32_t>(alphaState.alphaTestReferenceValue) << 6;
    flags1 |= static_cast<uint32_t>(alphaState.blendType) << 14;
    flags1 |= alphaState.invertedBlend ?      (1 << 18) : 0;
    flags1 |= alphaState.isBlendingDisabled ? (1 << 19) : 0;
    flags1 |= alphaState.emissiveBlend ?      (1 << 20) : 0;
    flags1 |= alphaState.isParticle ?         (1 << 21) : 0;
    flags1 |= alphaState.isDecal ?            (1 << 22) : 0;
    flags1 |= hasMaterialChanged ?            (1 << 23) : 0;
    flags1 |= isAnimatedWater ?               (1 << 24) : 0;
    flags1 |= isClipPlaneEnabled ?            (1 << 25) : 0;
    flags1 |= isMatte ?                       (1 << 26) : 0;
    flags1 |= isTextureFactorBlend ?          (1 << 27) : 0;
    flags1 |= isMotionBlurMaskOut ?           (1 << 28) : 0;
    flags1 |= skipSurfaceInteractionSpritesheetAdjustment ? (1 << 29) : 0;
    flags1 |= ignoreTransparencyLayer ?       (1 << 30) : 0;
    flags1 |= alphaState.isKenshiRain ?        (1u << 31) : 0;

    writeGPUHelper(data, offset, flags1);

    // Note: Matricies are stored on the cpu side in column-major order, the same as the GPU.

    Matrix4 instanceToWorld = objectToWorld;
    Matrix4 prevInstanceToWorld = prevObjectToWorld;
    Matrix3 normalInstanceToWorld = normalObjectToWorld;

    if (instancesToObject && surfaceIndexOfFirstInstance != SIZE_MAX && surfaceIndex != SIZE_MAX) {
      const size_t instanceIndex = surfaceIndex - surfaceIndexOfFirstInstance;
      if (instanceIndex >= instancesToObject->size()) {
        // Note: This should never happen.
        assert(false);
        Logger::err("Error: invalid instance index in RtSurface::WriteGPUData.");
      } else {
        instanceToWorld = objectToWorld * (*instancesToObject)[instanceIndex];
        prevInstanceToWorld = prevObjectToWorld * (*instancesToObject)[instanceIndex];
        normalInstanceToWorld = transpose(inverse(Matrix3(instanceToWorld)));
      }
    }

    // Note: Last row of object to world matrix not needed as it does not encode any useful information
    writeGPUHelper(data, offset, prevInstanceToWorld.data[0].x);
    writeGPUHelper(data, offset, prevInstanceToWorld.data[0].y);
    writeGPUHelper(data, offset, prevInstanceToWorld.data[0].z);
    writeGPUHelper(data, offset, prevInstanceToWorld.data[1].x);
    writeGPUHelper(data, offset, prevInstanceToWorld.data[1].y);
    writeGPUHelper(data, offset, prevInstanceToWorld.data[1].z);
    writeGPUHelper(data, offset, prevInstanceToWorld.data[2].x);
    writeGPUHelper(data, offset, prevInstanceToWorld.data[2].y);
    writeGPUHelper(data, offset, prevInstanceToWorld.data[2].z);
    writeGPUHelper(data, offset, prevInstanceToWorld.data[3].x);
    writeGPUHelper(data, offset, prevInstanceToWorld.data[3].y);
    writeGPUHelper(data, offset, prevInstanceToWorld.data[3].z);

    writeGPUHelper(data, offset, normalInstanceToWorld.data[0]);
    writeGPUHelper(data, offset, normalInstanceToWorld.data[1]);
    writeGPUHelper(data, offset, normalInstanceToWorld.data[2].x);
    writeGPUHelper(data, offset, normalInstanceToWorld.data[2].y);

    writeGPUHelper(data, offset, instanceToWorld.data[0].x);
    writeGPUHelper(data, offset, instanceToWorld.data[0].y);
    writeGPUHelper(data, offset, instanceToWorld.data[0].z);
    writeGPUHelper(data, offset, instanceToWorld.data[1].x);
    writeGPUHelper(data, offset, instanceToWorld.data[1].y);
    writeGPUHelper(data, offset, instanceToWorld.data[1].z);
    writeGPUHelper(data, offset, instanceToWorld.data[2].x);
    writeGPUHelper(data, offset, instanceToWorld.data[2].y);
    writeGPUHelper(data, offset, instanceToWorld.data[2].z);
    writeGPUHelper(data, offset, instanceToWorld.data[3].x);
    writeGPUHelper(data, offset, instanceToWorld.data[3].y);
    writeGPUHelper(data, offset, instanceToWorld.data[3].z);

    if (eyeParams) {
      // eye vectors are aliased with texture transform
      writeGPUHelper(data, offset, eyeParams->eyeRightU[0]);
      writeGPUHelper(data, offset, eyeParams->eyeRightU[1]);
      writeGPUHelper(data, offset, eyeParams->eyeRightU[2]);
      writeGPUHelper(data, offset, uint32_t{});
      writeGPUHelper(data, offset, eyeParams->eyeUpV[0]);
      writeGPUHelper(data, offset, eyeParams->eyeUpV[1]);
      writeGPUHelper(data, offset, eyeParams->eyeUpV[2]);
      writeGPUHelper(data, offset, uint32_t{});
    } else {
      // Note: Only 2 rows of texture transform written for now due to limit of 2 element restriction.
      writeGPUHelper(data, offset, textureTransform.data[0].x);
      writeGPUHelper(data, offset, textureTransform.data[1].x);
      writeGPUHelper(data, offset, textureTransform.data[2].x);
      writeGPUHelper(data, offset, textureTransform.data[3].x);
      writeGPUHelper(data, offset, textureTransform.data[0].y);
      writeGPUHelper(data, offset, textureTransform.data[1].y);
      writeGPUHelper(data, offset, textureTransform.data[2].y);
      writeGPUHelper(data, offset, textureTransform.data[3].y);
    }

    std::uint32_t textureSpritesheetData = 0;

    // Clamp rows and cols to at least 1, to avoid divide by 0 errors.
    textureSpritesheetData |= (static_cast<uint32_t>(std::max<uint8_t>(1, spriteSheetRows)) << 0);
    textureSpritesheetData |= (static_cast<uint32_t>(std::max<uint8_t>(1, spriteSheetCols)) << 8);
    textureSpritesheetData |= (static_cast<uint32_t>(spriteSheetFPS) << 16);
    // pack decalSortOrder into data13.x's last 8 bits.
    textureSpritesheetData |= (static_cast<uint32_t>(decalSortOrder) << 24);

    writeGPUHelper(data, offset, textureSpritesheetData);

    // Blend-constant rgb, in the slot the packed D3DCOLOR texture factor used to
    // occupy. R11G11B10 keeps the surface struct exactly the same size and layout
    // while raising precision from 8/8/8. Alpha rides in the freed texture-flag
    // bits below at 10 bits. See blend_constant_packing.h.
    writeGPUHelper(data, offset,
      packBlendConstantRGB(blendConstant.x, blendConstant.y, blendConstant.z));

    std::uint32_t textureFlags = 0;

    static_assert(static_cast<uint32_t>(D3D11ColorSource::Count) <= 4);

    textureFlags |= ((static_cast<uint32_t>(colorSource) & 0x3));
    textureFlags |= ((static_cast<uint32_t>(alphaSource) & 0x3) << 2);
    textureFlags |= modulateVertexColor ? (1u << 4) : 0u;
    textureFlags |= modulateVertexAlpha ? (1u << 5) : 0u;
    // Optional packed-texture scalar channel: bits 6-7 select RGBA and bit 8
    // enables it. Alpha remains independently sourced by alphaSource.
    assert(colorTextureChannel <= 4u);
    if (colorTextureChannel < 4u) {
      textureFlags |= (static_cast<uint32_t>(colorTextureChannel) & 0x3u) << 6;
      textureFlags |= 1u << 8;
    }
    // Optional packed-texture opacity channel: bits 9-10 select RGBA and bit
    // 11 enables it. With the selector disabled, opacity uses texture alpha.
    assert(opacityTextureChannel <= 4u);
    if (opacityTextureChannel < 4u) {
      textureFlags |= (static_cast<uint32_t>(opacityTextureChannel) & 0x3u) << 9;
      textureFlags |= 1u << 11;
    }
    // textureFlags bits 12-13 unused

    textureFlags |= eyeParams ? (1 << 14) : 0;
    // textureFlags bit 15 unused

    // 3 bits at 16, matching Surface::texcoordGenerationMode.
    static_assert(static_cast<uint32_t>(TexGenMode::Count) <= 8);
    textureFlags |= ((static_cast<uint32_t>(texgenMode) & 0x7) << 16);

    // Blend-constant alpha at 10 bits, in what were spare flag bits. This is the
    // channel most likely to carry a meaningful non-white value (a fade weight),
    // so it gets more precision than the 8 bits the old packed factor gave it.
    {
      const float saturatedAlpha = std::min(1.0f, std::max(0.0f, blendConstant.w));
      const uint32_t quantizedAlpha = static_cast<uint32_t>(saturatedAlpha * 1023.0f + 0.5f);
      textureFlags |= ((quantizedAlpha & 0x3ffu) << 19);
    }
    // textureFlags bits 29-30 unused

    writeGPUHelper(data, offset, textureFlags);

    // Note: This element of the normal object to world matrix is encoded to minimize padding
    writeGPUHelper(data, offset, normalInstanceToWorld.data[2].z);

    writeGPUHelper(data, offset, clipPlane);

    // eye origin
    if (eyeParams) {
      writeGPUHelper(data, offset, eyeParams->eyeballOrigin.x);
      writeGPUHelper(data, offset, eyeParams->eyeballOrigin.y);
      writeGPUHelper(data, offset, eyeParams->eyeballOrigin.z);
    } else {
      writeGPUHelper(data, offset, uint32_t{});
      writeGPUHelper(data, offset, uint32_t{});
      writeGPUHelper(data, offset, uint32_t{});
    }
    writeGPUHelper(data, offset, kenshiBloodMode);

    assert(offset - oldOffset == kSurfaceGPUSize);
  }

  void writeKenshiBloodGPUData(uint32_t* data) const {
    data[0] = kenshiBloodAmounts0;
    data[1] = kenshiBloodAmounts1;
    data[2] = kenshiBloodScale;
    data[3] = kenshiBloodColor;
    data[4] = kenshiBloodTextureIndex;
    data[5] = kenshiBloodSamplerIndex;
    data[6] = kenshiBloodProjectionBufferIndex;
    data[7] = kenshiBloodMode;
  }

  uint32_t positionBufferIndex = kSurfaceInvalidBufferIndex;
  uint32_t previousPositionBufferIndex = kSurfaceInvalidBufferIndex;
  uint32_t positionOffset = 0;
  uint32_t positionStride = 0;

  uint32_t normalBufferIndex = kSurfaceInvalidBufferIndex;
  uint32_t normalOffset = 0;
  uint32_t normalStride = 0;
  VkFormat normalFormat = VK_FORMAT_UNDEFINED;

  uint32_t texcoordBufferIndex = kSurfaceInvalidBufferIndex;
  uint32_t texcoordOffset = 0;
  uint32_t texcoordStride = 0;

  uint32_t indexBufferIndex = kSurfaceInvalidBufferIndex;
  uint32_t firstIndex = 0;
  uint32_t indexStride = 0;

  uint32_t color0BufferIndex = kSurfaceInvalidBufferIndex;
  uint32_t color0Offset = 0;
  uint32_t color0Stride = 0;

  uint32_t surfaceMaterialIndex = kSurfaceInvalidSurfaceMaterialIndex;

  // Runtime-only Kenshi blood state, uploaded in a parallel buffer indexed by
  // surfaceIndex. It does not change the fixed 256-byte Surface GPU ABI.
  uint32_t kenshiBloodAmounts0 = 0u;
  uint32_t kenshiBloodAmounts1 = 0u;
  uint32_t kenshiBloodScale = 0u;
  uint32_t kenshiBloodColor = 0u;
  uint32_t kenshiBloodTextureIndex = 0xFFFFu;
  uint32_t kenshiBloodSamplerIndex = 0xFFFFu;
  uint32_t kenshiBloodProjectionBufferIndex = kSurfaceInvalidBufferIndex;
  uint32_t kenshiBloodMode = 0u;

  bool isEmissive = false;
  bool isMatte = false;
  bool isStatic = false;
  bool hasMaterialChanged = false;
  bool isAnimatedWater = false;
  bool isClipPlaneEnabled = false;
  bool isTextureFactorBlend = false;
  bool isVertexColorBakedLighting = true;
  // Native distant-town colour/gloss contract; flags0 bit4, no GPU layout growth.
  bool kenshiDistantTown = false;
  // Under-construction scaffold contract; flags0 bit 5 (see the flags0 write above).
  bool kenshiConstruction = false;
  bool colorTextureIsSrgb = false;
  bool emissiveTextureIsSrgb = false;
  bool isMotionBlurMaskOut = false;
  bool skipSurfaceInteractionSpritesheetAdjustment = false;
  bool ignoreTransparencyLayer = false;

  // Where the surface's colour and alpha come from, and whether vertex colour
  // modulates the result. See D3D11ColorSource - this replaces the old
  // fixed-function texture-stage combiner, whose generality the DX11 capture
  // path never used.
  D3D11ColorSource colorSource = D3D11ColorSource::Texture;
  D3D11ColorSource alphaSource = D3D11ColorSource::Texture;
  bool modulateVertexColor = false;
  // Vertex alpha modulates separately: many DX11 vertex-colour streams carry
  // padding or zero in the alpha channel, so folding it into opacity by default
  // erases surfaces that only meant to tint their colour.
  bool modulateVertexAlpha = false;
  // 0-3 selects one sampled RGBA component for scalar colour; 4 disables it.
  uint8_t colorTextureChannel = 4u;
  // 0-3 selects one sampled RGBA component for opacity; 4 uses texture alpha.
  uint8_t opacityTextureChannel = 4u;
  // The D3D11 OMSetBlendState blend factor, kept as real floats on the CPU and
  // packed for the GPU by writeGPUData. Opaque white is the neutral default.
  Vector4 blendConstant = Vector4(1.0f, 1.0f, 1.0f, 1.0f);
  TexGenMode texgenMode = TexGenMode::None;
  std::optional<RtEyeParams> eyeParams = {};

  bool doBuffersMatch(const RtSurface& surface) {
    return positionBufferIndex == surface.positionBufferIndex
        && positionOffset == surface.positionOffset
        && previousPositionBufferIndex == surface.previousPositionBufferIndex
        && normalBufferIndex == surface.normalBufferIndex
        && normalOffset == surface.normalOffset
        && texcoordBufferIndex == surface.texcoordBufferIndex
        && texcoordOffset == surface.texcoordOffset
        && color0BufferIndex == surface.color0BufferIndex
        && color0Offset == surface.color0Offset
        && firstIndex == surface.firstIndex;
  }

  void printDebugInfo(const char* name = "") const {
#ifdef REMIX_DEVELOPMENT
    Logger::warn(str::format(
      "RtSurface ", name, "\n",
      "  address: ", this, "\n",
      "  surfaceMaterialIndex: ", surfaceMaterialIndex, "\n",
      "  associatedGeometryHash: 0x", std::hex, associatedGeometryHash, std::dec, "\n",
      "  objectPickingValue: ", objectPickingValue, "\n",
      "  decalSortOrder: ", decalSortOrder));
    
    // Print buffer info
    Logger::warn("=== Buffer Info ===");
    Logger::warn(str::format(
      "  positionBufferIndex: ", positionBufferIndex, "\n",
      "  positionOffset: ", positionOffset, "\n",
      "  positionStride: ", positionStride, "\n",
      "  previousPositionBufferIndex: ", previousPositionBufferIndex, "\n",
      "  normalBufferIndex: ", normalBufferIndex, "\n",
      "  normalOffset: ", normalOffset, "\n",
      "  normalStride: ", normalStride, "\n",
      "  normalFormat: ", static_cast<int>(normalFormat), "\n",
      "  texcoordBufferIndex: ", texcoordBufferIndex, "\n",
      "  texcoordOffset: ", texcoordOffset, "\n",
      "  texcoordStride: ", texcoordStride, "\n",
      "  indexBufferIndex: ", indexBufferIndex, "\n",
      "  firstIndex: ", firstIndex, "\n",
      "  indexStride: ", indexStride, "\n",
      "  color0BufferIndex: ", color0BufferIndex, "\n",
      "  color0Offset: ", color0Offset, "\n",
      "  color0Stride: ", color0Stride));
    
    // Print boolean flags
    Logger::warn("=== Boolean Flags ===");
    Logger::warn(str::format(
      "  isEmissive: ", isEmissive, "\n",
      "  isMatte: ", isMatte, "\n",
      "  isStatic: ", isStatic, "\n",
      "  hasMaterialChanged: ", hasMaterialChanged, "\n",
      "  isAnimatedWater: ", isAnimatedWater, "\n",
      "  isClipPlaneEnabled: ", isClipPlaneEnabled, "\n",
      "  isTextureFactorBlend: ", isTextureFactorBlend, "\n",
      "  isMotionBlurMaskOut: ", isMotionBlurMaskOut, "\n",
      "  skipSurfaceInteractionSpritesheetAdjustment: ", skipSurfaceInteractionSpritesheetAdjustment, "\n",
      "  ignoreTransparencyLayer: ", ignoreTransparencyLayer));
    
    // Print alpha state
    Logger::warn("=== Alpha State ===");
    Logger::warn(str::format(
      "  isBlendingDisabled: ", alphaState.isBlendingDisabled, "\n",
      "  isFullyOpaque: ", alphaState.isFullyOpaque, "\n",
      "  alphaTestType: ", static_cast<int>(alphaState.alphaTestType), "\n",
      "  alphaTestReferenceValue: ", static_cast<int>(alphaState.alphaTestReferenceValue), "\n",
      "  blendType: ", static_cast<int>(alphaState.blendType), "\n",
      "  invertedBlend: ", alphaState.invertedBlend, "\n",
      "  emissiveBlend: ", alphaState.emissiveBlend, "\n",
      "  isParticle: ", alphaState.isParticle, "\n",
      "  isDecal: ", alphaState.isDecal));
    
    // Print texture operations
    Logger::warn("=== Texture Operations ===");
    Logger::warn(str::format(
      "  textureColorArg1Source: ", static_cast<int>(textureColorArg1Source), "\n",
      "  textureColorArg2Source: ", static_cast<int>(textureColorArg2Source), "\n",
      "  textureColorOperation: ", static_cast<int>(textureColorOperation), "\n",
      "  textureAlphaArg1Source: ", static_cast<int>(textureAlphaArg1Source), "\n",
      "  textureAlphaArg2Source: ", static_cast<int>(textureAlphaArg2Source), "\n",
      "  textureAlphaOperation: ", static_cast<int>(textureAlphaOperation), "\n",
      "  texgenMode: ", static_cast<int>(texgenMode), "\n",
      "  tFactor: 0x", std::hex, tFactor, std::dec));
    
    // Print spritesheet info
    Logger::warn("=== Spritesheet Info ===");
    Logger::warn(str::format(
      "  spriteSheetRows: ", static_cast<int>(spriteSheetRows), "\n",
      "  spriteSheetCols: ", static_cast<int>(spriteSheetCols), "\n",
      "  spriteSheetFPS: ", static_cast<int>(spriteSheetFPS)));
    
    // Print instance info
    Logger::warn("=== Instance Info ===");
    Logger::warn(str::format(
      "  instancesToObject: ", (instancesToObject != nullptr ? "valid" : "null"), "\n",
      "  surfaceIndexOfFirstInstance: ", surfaceIndexOfFirstInstance));
#endif
  }

  // Used for calculating hashes, keep the members padded and default initialized
  struct AlphaState {
    bool isBlendingDisabled = true;
    bool isFullyOpaque = false;
    AlphaTestType alphaTestType = AlphaTestType::kAlways;
    uint8_t alphaTestReferenceValue = 0;
    BlendType blendType = BlendType::kAlpha;
    bool invertedBlend = false;
    bool emissiveBlend = false;
    bool isParticle = false;
    bool isDecal : 1;
    bool isKenshiRain : 1;
  } alphaState = {};

  // Original draw call state
  DxvkBlendMode blendModeState;

  // Static validation to detect any changes that require an alignment re-check
  static_assert(sizeof(AlphaState) == 9);

  Matrix4 objectToWorld;
  Matrix4 prevObjectToWorld;
  Matrix3 normalObjectToWorld;
  Matrix4 textureTransform;
  Vector4 clipPlane;

  uint8_t spriteSheetRows = 1;
  uint8_t spriteSheetCols = 1;
  uint8_t spriteSheetFPS = 0;

  XXH64_hash_t associatedGeometryHash; // NOTE: This is used for the debug view
  uint32_t objectPickingValue = 0; // NOTE: a value to fill GBUFFER_BINDING_PRIMARY_OBJECT_PICKING_OUTPUT
  uint32_t decalSortOrder = 0; // see: InstanceManager::m_decalSortOrderCounter

  // PointInstancer support - this surface may represent multiple instances, one for each transform in instancesToObject.
  // Some API-provided instance transform arrays are not owned by an AssetReplacement and may be destroyed before the
  // next full scene clear, so surfaces retain shared ownership of the transform data they reference.
  std::shared_ptr<const std::vector<Matrix4>> instancesToObject;
  // on the GPU, multiple copies of this surface with different transforms will exist.  They will be in a continuous block, starting at surfaceIndexOfFirstInstance.
  size_t surfaceIndexOfFirstInstance = SIZE_MAX;
};

// Shared Material Defaults/Limits

struct LegacyMaterialDefaults {
  friend class ImGUI;
  RTX_OPTION("rtx.legacyMaterial", float, anisotropy, 0.f, "The default roughness anisotropy to use for non-replaced \"legacy\" materials. Should be in the range -1 to 1, where 0 is isotropic.");
  RTX_OPTION("rtx.legacyMaterial", float, emissiveIntensity, 0.f, "The default emissive intensity to use for non-replaced \"legacy\" materials.");
  RTX_OPTION("rtx.legacyMaterial", bool, useAlbedoTextureIfPresent, true, "A flag to determine if an \"albedo\" texture (a qualifying color texture) from the original application should be used if present on non-replaced \"legacy\" materials.");
  RTX_OPTION("rtx.legacyMaterial", Vector3, albedoConstant, Vector3(1.0f, 1.0f, 1.0f), "The default albedo constant to use for non-replaced \"legacy\" materials. Should be a color in sRGB colorspace with gamma encoding.");
  RTX_OPTION("rtx.legacyMaterial", float, opacityConstant, 1.f, "The default opacity constant to use for non-replaced \"legacy\" materials. Should be in the range 0 to 1.");
  RTX_OPTION_ENV("rtx.legacyMaterial", float, roughnessConstant, 0.7f, "DXVK_LEGACY_MATERIAL_DEFAULT_ROUGHNESS", "The default perceptual roughness constant to use for non-replaced \"legacy\" materials. Should be in the range 0 to 1.");
  RTX_OPTION("rtx.legacyMaterial", float, metallicConstant, 0.1f, "The default metallic constant to use for non-replaced \"legacy\" materials. Should be in the range 0 to 1.");
  RTX_OPTION("rtx.legacyMaterial", Vector3, emissiveColorConstant, Vector3(0.0f, 0.0f, 0.0f), "The default emissive color constant to use for non-replaced \"legacy\" materials. Should be a color in sRGB colorspace with gamma encoding.");
  RTX_OPTION("rtx.legacyMaterial", bool, enableEmissive, false, "A flag to determine if emission should be used on non-replaced \"legacy\" materials.");
  RTX_OPTION("rtx.legacyMaterial", bool, ignoreAlphaChannel, false, "A flag to determine if the albedo alpha channel should be ignored on non-replaced \"legacy\" materials.");
  RTX_OPTION("rtx.legacyMaterial", bool, enableThinFilm, false, "A flag to determine if a thin-film layer should be used on non-replaced \"legacy\" materials.");
  RTX_OPTION("rtx.legacyMaterial", bool, alphaIsThinFilmThickness, false, "A flag to determine if the alpha channel from the albedo source should be treated as thin film thickness on non-replaced \"legacy\" materials.");
  // Note: Should be something non-zero as 0 is an invalid thickness to have (even if this is just unused).
  RTX_OPTION("rtx.legacyMaterial", float, thinFilmThicknessConstant, 200.f,
             "The thickness (in nanometers) of the thin-film layer assuming it is enabled on non-replaced \"legacy\" materials.\n"
             "Should be any value larger than 0, typically within the wavelength of light, but must be less than or equal to OPAQUE_SURFACE_MATERIAL_THIN_FILM_MAX_THICKNESS (" STRINGIFY(OPAQUE_SURFACE_MATERIAL_THIN_FILM_MAX_THICKNESS) " nm).");
};

// Surface Materials

// Todo: Compute size directly from sizeof of GPU structure (by including it), for now computed by sum of members manually.
// Blocked on float16 support on the c++ side.
constexpr std::size_t kSurfaceMaterialGPUSize = 4 * 4 * 4;
// Note: 0xFFFF used for inactive texture index to indicate to the GPU that no texture is in use for a specific variable
// (as some are optional). Also used for debugging to provide wildly out of range values in case one is not set.
constexpr uint32_t kSurfaceMaterialInvalidTextureIndex = 0xFFFFu;
// Note: These defaults are used in places where no value is available for the constructor of various Surface Materials, just to
// keep things consistent across the codebase.

enum class RtSurfaceMaterialType {
  // Todo: Legacy SurfaceMaterialType in the future
  Opaque = 0,
  Translucent,
  RayPortal,

  // Extensions
  Subsurface,

  Count
};

// Todo: Legacy SurfaceMaterial in the future

struct RtOpaqueSurfaceMaterial {
  RtOpaqueSurfaceMaterial(
    uint32_t albedoOpacityTextureIndex, uint32_t normalTextureIndex,
    uint32_t tangentTextureIndex, uint32_t heightTextureIndex, uint32_t roughnessTextureIndex,
    uint32_t metallicTextureIndex, uint32_t emissiveColorTextureIndex,
    float anisotropy, float emissiveIntensity,
    const Vector4& albedoOpacityConstant,
    float roughnessConstant, float metallicConstant,
    const Vector3& emissiveColorConstant, bool enableEmission,
    bool ignoreAlphaChannel, bool enableThinFilm, bool alphaIsThinFilmThickness, float thinFilmThicknessConstant,
    uint32_t samplerIndex, float displaceIn, float displaceOut,
    uint32_t subsurfaceMaterialIndex, bool isRaytracedRenderTarget,
    uint16_t samplerFeedbackStamp,
    uint32_t secondaryTextureIndex = 0,
    bool useSecondaryTextureForOpacity = false,
    bool kenshiTerrainBlend = false,
    bool kenshiCharacterHead = false,
    uint32_t kenshiCharacterBodyMaskTextureIndex = 0,
    uint32_t kenshiCharacterHeadMaskTextureIndex = 0,
    uint32_t kenshiCharacterHairTextureIndex = 0,
    uint32_t kenshiCharacterBeardTextureIndex = 0,
    uint16_t kenshiCharacterHairChannels = 0,
    // 0 = octahedral (Remix default), 1 = Kenshi RGB, 2 = DXT5nm.
    uint32_t kenshiNormalEncoding = 0,
    // The diffuse alpha is gloss, scaled by roughnessConstant.
    bool kenshiGlossInAlpha = false,
    // secondaryTextureIndex is this material's second albedo, blended with the first by the vertex
    // colour's alpha.
    bool kenshiDualTextureSet = false,
    // secondaryTextureIndex is a two-channel recolour mask; albedoOpacityConstant / emissiveColorConstant
    // carry the two colours.
    bool kenshiColorMask = false,
    // This character material carries a worn-clothing layer.
    bool kenshiCharacterVest = false
  ) :
    m_albedoOpacityTextureIndex{ albedoOpacityTextureIndex }, m_secondaryTextureIndex{secondaryTextureIndex}, m_normalTextureIndex{ normalTextureIndex },
    m_tangentTextureIndex { tangentTextureIndex }, m_heightTextureIndex { heightTextureIndex }, m_roughnessTextureIndex{ roughnessTextureIndex },
    m_metallicTextureIndex{ metallicTextureIndex }, m_emissiveColorTextureIndex{ emissiveColorTextureIndex },
    m_anisotropy{ anisotropy }, m_emissiveIntensity{ emissiveIntensity },
    m_albedoOpacityConstant{ albedoOpacityConstant },
    m_roughnessConstant{ roughnessConstant }, m_metallicConstant{ metallicConstant },
    m_emissiveColorConstant{ emissiveColorConstant }, m_enableEmission{ enableEmission },
    m_ignoreAlphaChannel { ignoreAlphaChannel }, m_useSecondaryTextureForOpacity { useSecondaryTextureForOpacity }, m_kenshiTerrainBlend { kenshiTerrainBlend }, m_kenshiCharacterHead { kenshiCharacterHead }, m_enableThinFilm { enableThinFilm }, m_alphaIsThinFilmThickness { alphaIsThinFilmThickness },
    m_thinFilmThicknessConstant { thinFilmThicknessConstant }, m_samplerIndex{ samplerIndex }, m_displaceIn{ displaceIn },
    m_displaceOut{ displaceOut }, m_subsurfaceMaterialIndex(subsurfaceMaterialIndex), m_isRaytracedRenderTarget(isRaytracedRenderTarget),
    m_samplerFeedbackStamp{ samplerFeedbackStamp },
    m_kenshiCharacterHairChannels { kenshiCharacterHairChannels },
    m_kenshiCharacterBodyMaskTextureIndex { kenshiCharacterBodyMaskTextureIndex },
    m_kenshiCharacterHeadMaskTextureIndex { kenshiCharacterHeadMaskTextureIndex },
    m_kenshiCharacterHairTextureIndex { kenshiCharacterHairTextureIndex },
    m_kenshiCharacterBeardTextureIndex { kenshiCharacterBeardTextureIndex },
    m_kenshiNormalEncoding { kenshiNormalEncoding },
    m_kenshiGlossInAlpha { kenshiGlossInAlpha },
    m_kenshiDualTextureSet { kenshiDualTextureSet },
    m_kenshiColorMask { kenshiColorMask },
    m_kenshiCharacterVest { kenshiCharacterVest }
  {
    updateCachedData();
    updateCachedHash();
  }

  void writeGPUData(unsigned char* data, std::size_t& offset) const {
    [[maybe_unused]] const std::size_t oldOffset = offset;
    uint16_t flags = surfaceMaterialTypeOpaque;

    // For decode process, see surface_material.h
    // this data is accessed from uint16_t data[32], so data[n] refers to a pair of bytes.

    if (m_enableThinFilm) {
      flags |= OPAQUE_SURFACE_MATERIAL_FLAG_USE_THIN_FILM_LAYER;

      // Note: Only consider setting alpha as thin film thickness flag if the thin film is enabled, GPU relies on
      // this logical ordering.
      if (m_alphaIsThinFilmThickness) {
        flags |= OPAQUE_SURFACE_MATERIAL_FLAG_ALPHA_IS_THIN_FILM_THICKNESS;
      }
    }

    if (m_ignoreAlphaChannel) {
      flags |= OPAQUE_SURFACE_MATERIAL_FLAG_IGNORE_ALPHA_CHANNEL;
    }
    if (m_useSecondaryTextureForOpacity) {
      flags |= OPAQUE_SURFACE_MATERIAL_FLAG_USE_SECONDARY_TEXTURE_FOR_OPACITY;
    }
    if (m_kenshiTerrainBlend) {
      flags |= OPAQUE_SURFACE_MATERIAL_FLAG_KENSHI_TERRAIN_BLEND;
    }
    if (m_kenshiCharacterHead) {
      flags |= OPAQUE_SURFACE_MATERIAL_FLAG_KENSHI_CHARACTER_HEAD;
    }
    // 0 = octahedral (Remix's own), so a USD replacement decodes exactly as before.
    if (m_kenshiNormalEncoding == 1u) {
      flags |= OPAQUE_SURFACE_MATERIAL_FLAG_KENSHI_NORMAL_RGB;
    } else if (m_kenshiNormalEncoding == 2u) {
      flags |= OPAQUE_SURFACE_MATERIAL_FLAG_KENSHI_NORMAL_DXT5NM;
    }
    if (m_kenshiDualTextureSet) {
      flags |= OPAQUE_SURFACE_MATERIAL_FLAG_KENSHI_DUAL_TEXTURE_SET;
    }
    if (m_kenshiColorMask) {
      flags |= OPAQUE_SURFACE_MATERIAL_FLAG_KENSHI_COLOR_MASK;
    }
    if (m_kenshiCharacterVest) {
      flags |= OPAQUE_SURFACE_MATERIAL_FLAG_KENSHI_CHARACTER_VEST;
    }
    if (m_kenshiGlossInAlpha) {
      flags |= OPAQUE_SURFACE_MATERIAL_FLAG_KENSHI_GLOSS_IN_ALPHA;
    }
    // NOTE: We keep the most commonly used elements in the material close together near the beginning
    //       This hopefully reduces loads for cases like opacity detection.

    if (m_isRaytracedRenderTarget) {
      flags |= OPAQUE_SURFACE_MATERIAL_FLAG_IS_RAYTRACED_RENDER_TARGET;
    }

    float displaceIn = m_displaceIn * getDisplacementInFactor();
    float displaceOut = m_displaceOut * getDisplacementOutFactor();
    uint32_t heightTextureIndex = m_heightTextureIndex;
    float thinFilmThickness = m_cachedThinFilmNormalizedThicknessConstant;
    float emissiveIntensity = m_cachedEmissiveIntensity;
    // The muscle-blend normal map rides in heightTextureIndex. hasValidDisplacement() returns false for
    // these materials, so the no-POM branch below runs: it zeroes the displacement scalars but must be
    // exempted from wiping heightTextureIndex, or the muscle map would arrive invalid.
    const bool kenshiMuscleBlend = kenshiHasMuscleBlendMap();
    if (hasValidDisplacement()) {
      flags |= OPAQUE_SURFACE_MATERIAL_FLAG_HAS_DISPLACEMENT;
    } else {
      // If any POM attribute would disable POM, just disable all POM attributes.
      displaceIn = 0.f;
      displaceOut = 0.f;
      if (!kenshiMuscleBlend) {
        heightTextureIndex = BINDING_INDEX_INVALID;
      }
    }
    assert(displaceIn <= FLOAT16_MAX);
    assert(displaceOut <= FLOAT16_MAX);

    assert(m_subsurfaceMaterialIndex <= SURFACE_INDEX_MAX_VALUE);

    // data[0 - 3]
    writeGPUHelper(data, offset, flags);
    writeGPUHelperExplicit<2>(data, offset, m_samplerIndex);
    writeGPUHelperExplicit<2>(data, offset, m_albedoOpacityTextureIndex);
    writeGPUHelperExplicit<2>(data, offset, m_secondaryTextureIndex);

    // data[4 - 7]
    writeGPUHelper(data, offset, glm::packHalf1x16(m_albedoOpacityConstant.x));
    writeGPUHelper(data, offset, glm::packHalf1x16(m_albedoOpacityConstant.y));
    writeGPUHelper(data, offset, glm::packHalf1x16(m_albedoOpacityConstant.z));
    writeGPUHelper(data, offset, glm::packHalf1x16(m_albedoOpacityConstant.w));

    // data[8 - 11]
    writeGPUHelper(data, offset, glm::packHalf1x16(displaceIn));
    writeGPUHelper(data, offset, glm::packHalf1x16(displaceOut));
    writeGPUHelperExplicit<2>(data, offset, heightTextureIndex);
    writeGPUHelper(data, offset, glm::packHalf1x16(thinFilmThickness));

    // data[12 - 15]
    writeGPUHelperExplicit<2>(data, offset, m_emissiveColorTextureIndex);
    writeGPUHelperExplicit<2>(data, offset, m_roughnessTextureIndex);
    writeGPUHelperExplicit<2>(data, offset, m_metallicTextureIndex);
    writeGPUHelperExplicit<2>(data, offset, m_normalTextureIndex);

    // data[16 - 19]
    writeGPUHelper(data, offset, glm::packHalf1x16(m_emissiveColorConstant.x));
    writeGPUHelper(data, offset, glm::packHalf1x16(m_emissiveColorConstant.y));
    writeGPUHelper(data, offset, glm::packHalf1x16(m_emissiveColorConstant.z));
    assert(m_cachedEmissiveIntensity <= FLOAT16_MAX);
    writeGPUHelper(data, offset, glm::packHalf1x16(emissiveIntensity));

    // data[20 - 23]
    writeGPUHelper(data, offset, glm::packHalf1x16(m_roughnessConstant));
    writeGPUHelper(data, offset, glm::packHalf1x16(m_metallicConstant));
    writeGPUHelper(data, offset, glm::packHalf1x16(m_anisotropy));
    writeGPUHelperExplicit<2>(data, offset, m_tangentTextureIndex);

    // data[24-25]
    writeGPUHelper(data, offset, m_subsurfaceMaterialIndex);

    // data[26]
    writeGPUHelperExplicit<2>(data, offset, m_samplerFeedbackStamp);

    writeGPUHelperExplicit<2>(data, offset, m_kenshiCharacterBodyMaskTextureIndex);
    writeGPUHelperExplicit<2>(data, offset, m_kenshiCharacterHeadMaskTextureIndex);
    writeGPUHelperExplicit<2>(data, offset, m_kenshiCharacterHairTextureIndex);
    writeGPUHelperExplicit<2>(data, offset, m_kenshiCharacterBeardTextureIndex);
    writeGPUHelper(data, offset, m_kenshiCharacterHairChannels);
    assert(offset - oldOffset == kSurfaceMaterialGPUSize);
  }

  bool validate() const {
    const bool hasTexture = m_albedoOpacityTextureIndex != kSurfaceMaterialInvalidTextureIndex ||
                            m_normalTextureIndex != kSurfaceMaterialInvalidTextureIndex ||
                            m_tangentTextureIndex != kSurfaceMaterialInvalidTextureIndex ||
                            m_heightTextureIndex != kSurfaceMaterialInvalidTextureIndex ||
                            m_roughnessTextureIndex != kSurfaceMaterialInvalidTextureIndex ||
                            m_metallicTextureIndex != kSurfaceMaterialInvalidTextureIndex ||
                            m_emissiveColorTextureIndex != kSurfaceMaterialInvalidTextureIndex ||
                            m_kenshiCharacterBodyMaskTextureIndex != kSurfaceMaterialInvalidTextureIndex ||
                            m_kenshiCharacterHeadMaskTextureIndex != kSurfaceMaterialInvalidTextureIndex ||
                            m_kenshiCharacterHairTextureIndex != kSurfaceMaterialInvalidTextureIndex ||
                            m_kenshiCharacterBeardTextureIndex != kSurfaceMaterialInvalidTextureIndex;

    return !hasTexture || m_samplerIndex != kSurfaceMaterialInvalidTextureIndex;
  }

  // Bits 10-15 of the hair-channel word carry the muscle blend; a non-zero amount proves
  // heightTextureIndex holds the muscle normal map, not a height map.
  bool kenshiHasMuscleBlendMap() const {
    return (m_kenshiCharacterHairChannels >> 10u) != 0u;
  }

  // A material carrying the muscle map must never be displaced. `DisplaceIn` defaults to 0.05 on every
  // OpaqueMaterialData (rtx_material_data.h) and rtx.displacement.mode defaults to QuadtreePOM, so the
  // only thing keeping POM off in this game is heightTextureIndex being invalid - aliasing the muscle map
  // there would switch POM on. opaqueSurfaceMaterialInteractionCreate runs POM before every texture read
  // and overwrites the texture coordinates, so everything on the material would sample at displaced
  // coordinates. Aliasing a field can change behaviour by being set, not only by being read.
  bool hasValidDisplacement() const {
    if (kenshiHasMuscleBlendMap()) {
      return false;
    }
    return (m_displaceIn > 0.f || m_displaceOut > 0.f) && m_heightTextureIndex != BINDING_INDEX_INVALID;
  }

  bool operator==(const RtOpaqueSurfaceMaterial& r) const {
    return m_cachedHash == r.m_cachedHash;
  }

  XXH64_hash_t getHash() const {
    return m_cachedHash;
  }

  uint32_t getSamplerIndex() const {
    return m_samplerIndex;
  }

  uint32_t getAlbedoOpacityTextureIndex() const {
    return m_albedoOpacityTextureIndex;
  }

  bool isKenshiTerrain() const { return m_kenshiTerrainBlend; }
  uint32_t getKenshiTerrainSetIndex() const { return m_secondaryTextureIndex; }

  // Kenshi cutouts can store opacity in normal-map alpha, independently of
  // the primary colour texture's gloss alpha. Do not expose other secondary
  // textures (terrain, recolour, etc.) as opacity inputs.
  uint32_t getSecondaryOpacityTextureIndex() const {
    return m_useSecondaryTextureForOpacity ? m_secondaryTextureIndex : kSurfaceMaterialInvalidTextureIndex;
  }

  uint32_t getNormalTextureIndex() const {
    return m_normalTextureIndex;
  }

  uint32_t getTangentTextureIndex() const {
    return m_tangentTextureIndex;
  }

  uint32_t getHeightTextureIndex() const {
    return m_heightTextureIndex;
  }

  uint32_t getRoughnessTextureIndex() const {
    return m_roughnessTextureIndex;
  }

  uint32_t getMetallicTextureIndex() const {
    return m_metallicTextureIndex;
  }

  uint32_t getEmissiveColorTextureIndex() const {
    return m_emissiveColorTextureIndex;
  }

  float getAnisotropy() const {
    return m_anisotropy;
  }

  float getEmissiveIntensity() const {
    return m_emissiveIntensity;
  }

  Vector4 getAlbedoOpacityConstant() const {
    return m_albedoOpacityConstant;
  }

  float getRoughnessConstant() const {
    return m_roughnessConstant;
  }

  float getMetallicConstant() const {
    return m_metallicConstant;
  }

  Vector3 getEmissiveColorConstant() const {
    return m_emissiveColorConstant;
  }

  bool getEnableEmission() const {
    return m_enableEmission;
  }

  uint32_t getSubsurfaceMaterialIndex() const {
    return m_subsurfaceMaterialIndex;
  }

  uint32_t getIsRaytracedRenderTarget() const {
    return m_isRaytracedRenderTarget;
  }

  template<typename Fn>
  void forEachTextureIndex(Fn&& fn) const {
    fn(m_albedoOpacityTextureIndex);
    // Terrain aliases this field to a parameter-record index, not a texture.
    if (!m_kenshiTerrainBlend) fn(m_secondaryTextureIndex);
    fn(m_normalTextureIndex);
    fn(m_tangentTextureIndex);
    fn(m_heightTextureIndex);
    fn(m_roughnessTextureIndex);
    fn(m_metallicTextureIndex);
    fn(m_emissiveColorTextureIndex);
    fn(m_kenshiCharacterBodyMaskTextureIndex);
    fn(m_kenshiCharacterHeadMaskTextureIndex);
    fn(m_kenshiCharacterHairTextureIndex);
    fn(m_kenshiCharacterBeardTextureIndex);
  }

private:
  void updateCachedHash() {
    static_assert(
      sizeof(*this) == 152,
      "add new member for hashing if needed: add a MEMBER into the struct + add a VALUE into the list-init"
    );
    struct HashStruct {
      uint32_t albedoOpacityTextureIndex;
      uint32_t normalTextureIndex;
      uint32_t tangentTextureIndex;
      uint32_t heightTextureIndex;
      uint32_t roughnessTextureIndex;
      uint32_t metallicTextureIndex;
      uint32_t emissiveColorTextureIndex;
      float anisotropy;
      float emissiveIntensity;
      Vector4 albedoOpacityConstant;
      float roughnessConstant;
      float metallicConstant;
      Vector3 emissiveColorConstant;
      uint32_t enableEmission;            // NOTE: uint32_t to avoid padding
      uint32_t ignoreAlphaChannel;        // NOTE: uint32_t to avoid padding
      uint32_t useSecondaryTextureForOpacity; // NOTE: uint32_t to avoid padding
      uint32_t kenshiTerrainBlend;           // NOTE: uint32_t to avoid padding
      uint32_t kenshiCharacterHead;          // NOTE: uint32_t to avoid padding
      uint32_t enableThinFilm;            // NOTE: uint32_t to avoid padding
      uint32_t alphaIsThinFilmThickness;  // NOTE: uint32_t to avoid padding
      float thinFilmThicknessConstant;
      uint32_t samplerIndex;
      float displaceIn;
      float displaceOut;
      uint32_t subsurfaceMaterialIndex;
      uint32_t isRaytracedRenderTarget;   // NOTE: uint32_t to avoid padding
      uint32_t samplerFeedbackStamp;      // NOTE: uint32_t to avoid padding
      uint32_t secondaryTextureIndex;
      uint32_t kenshiCharacterBodyMaskTextureIndex;
      uint32_t kenshiCharacterHeadMaskTextureIndex;
      uint32_t kenshiCharacterHairTextureIndex;
      uint32_t kenshiCharacterBeardTextureIndex;
      uint32_t kenshiCharacterHairChannels;
      // Must participate in the hash: materials that differ only in normal decode are different materials.
      uint32_t kenshiNormalEncoding;
      // Materials differing only in whether their alpha is gloss are different materials.
      uint32_t kenshiGlossInAlpha;    // NOTE: uint32_t to avoid padding
      // Materials differing only in whether their secondary texture is a second albedo are different
      // materials.
      uint32_t kenshiDualTextureSet;  // NOTE: uint32_t to avoid padding
      // Materials differing only in whether their secondary texture is a recolour mask are different
      // materials.
      uint32_t kenshiColorMask;       // NOTE: uint32_t to avoid padding
      // A body with clothing and the same body without are different materials.
      uint32_t kenshiCharacterVest;   // NOTE: uint32_t to avoid padding
      // NOTE: There must be NO padding between members, as the struct is used for hashing
    };
    static_assert(alignof(HashStruct) == 4 && sizeof(HashStruct) % 4 == 0);
    HashStruct hashData = HashStruct{
      m_albedoOpacityTextureIndex,
      m_normalTextureIndex,
      m_tangentTextureIndex,
      m_heightTextureIndex,
      m_roughnessTextureIndex,
      m_metallicTextureIndex,
      m_emissiveColorTextureIndex,
      m_anisotropy,
      m_emissiveIntensity,
      m_albedoOpacityConstant,
      m_roughnessConstant,
      m_metallicConstant,
      m_emissiveColorConstant,
      m_enableEmission,
      m_ignoreAlphaChannel,
      m_useSecondaryTextureForOpacity,
      m_kenshiTerrainBlend,
      m_kenshiCharacterHead,
      m_enableThinFilm,
      m_alphaIsThinFilmThickness,
      m_thinFilmThicknessConstant,
      m_samplerIndex,
      m_displaceIn,
      m_displaceOut,
      m_subsurfaceMaterialIndex,
      m_isRaytracedRenderTarget,
      m_samplerFeedbackStamp,
      m_secondaryTextureIndex,
      m_kenshiCharacterBodyMaskTextureIndex,
      m_kenshiCharacterHeadMaskTextureIndex,
      m_kenshiCharacterHairTextureIndex,
      m_kenshiCharacterBeardTextureIndex,
      m_kenshiCharacterHairChannels,
      m_kenshiNormalEncoding,
      m_kenshiGlossInAlpha,
      m_kenshiDualTextureSet,
      m_kenshiColorMask,
      m_kenshiCharacterVest,
    };
    m_cachedHash = XXH3_64bits(&hashData, sizeof(hashData));
  }

  void updateCachedData() {
    // Note: Ensure the thin film thickness constant is within the expected range for normalization.
    assert(m_thinFilmThicknessConstant <= OPAQUE_SURFACE_MATERIAL_THIN_FILM_MAX_THICKNESS);

    // Note: Opaque material does not take an emissive radiance directly, so zeroing out the intensity works
    // fine as a way to disable it (in case a texture is in use).
    m_cachedEmissiveIntensity = std::min(m_enableEmission ? m_emissiveIntensity : 0.0f, FLOAT16_MAX);
    // Note: Pre-normalize thickness constant so that it does not need to be done on the GPU.
    m_cachedThinFilmNormalizedThicknessConstant = m_thinFilmThicknessConstant / OPAQUE_SURFACE_MATERIAL_THIN_FILM_MAX_THICKNESS;
  }

  uint32_t m_albedoOpacityTextureIndex;
  uint32_t m_secondaryTextureIndex;
  uint32_t m_normalTextureIndex;
  uint32_t m_tangentTextureIndex;
  uint32_t m_heightTextureIndex;
  uint32_t m_roughnessTextureIndex;
  uint32_t m_metallicTextureIndex;
  uint32_t m_emissiveColorTextureIndex;
  uint32_t m_samplerIndex;

  float m_anisotropy;
  float m_emissiveIntensity;

  Vector4 m_albedoOpacityConstant;
  float m_roughnessConstant;
  float m_metallicConstant;
  Vector3 m_emissiveColorConstant;

  bool m_enableEmission;

  bool m_ignoreAlphaChannel;
  bool m_useSecondaryTextureForOpacity;
  bool m_kenshiTerrainBlend;
  bool m_kenshiCharacterHead;
  bool m_enableThinFilm;
  bool m_alphaIsThinFilmThickness;
  float m_thinFilmThicknessConstant;

  // How far inwards a height_texture value of 0 maps to.
  float m_displaceIn;
  // How far outwards a height_texture value of 1 maps to.
  float m_displaceOut;

  uint32_t m_subsurfaceMaterialIndex;

  bool m_isRaytracedRenderTarget;

  uint16_t m_samplerFeedbackStamp;

  uint16_t m_kenshiCharacterHairChannels;
  uint32_t m_kenshiCharacterBodyMaskTextureIndex;
  uint32_t m_kenshiCharacterHeadMaskTextureIndex;
  uint32_t m_kenshiCharacterHairTextureIndex;
  uint32_t m_kenshiCharacterBeardTextureIndex;
  // 0 = octahedral (Remix default), 1 = Kenshi RGB, 2 = DXT5nm.
  uint32_t m_kenshiNormalEncoding = 0u;
  // See OPAQUE_SURFACE_MATERIAL_FLAG_KENSHI_GLOSS_IN_ALPHA.
  bool m_kenshiGlossInAlpha = false;
  // See OPAQUE_SURFACE_MATERIAL_FLAG_KENSHI_DUAL_TEXTURE_SET.
  bool m_kenshiDualTextureSet = false;
  // See OPAQUE_SURFACE_MATERIAL_FLAG_KENSHI_COLOR_MASK.
  bool m_kenshiColorMask = false;
  // See OPAQUE_SURFACE_MATERIAL_FLAG_KENSHI_CHARACTER_VEST.
  bool m_kenshiCharacterVest = false;

  XXH64_hash_t m_cachedHash;

  // Note: Cached values are not involved in the hash as they are derived from the input data
  float m_cachedEmissiveIntensity;
  float m_cachedThinFilmNormalizedThicknessConstant;
};

struct RtTranslucentSurfaceMaterial {
  RtTranslucentSurfaceMaterial(
    uint32_t normalTextureIndex,
    uint32_t transmittanceTextureIndex,
    uint32_t emissiveColorTextureIndex,
    float refractiveIndex,
    float transmittanceMeasurementDistance, const Vector3& transmittanceColor,
    bool enableEmission, float emissiveIntensity, const Vector3& emissiveColorConstant,
    bool isThinWalled, float thinWallThickness, bool useDiffuseLayer, uint32_t samplerIndex) :
    m_normalTextureIndex(normalTextureIndex),
    m_transmittanceTextureIndex(transmittanceTextureIndex),
    m_emissiveColorTextureIndex(emissiveColorTextureIndex),
    m_refractiveIndex(refractiveIndex),
    m_transmittanceMeasurementDistance(transmittanceMeasurementDistance), m_transmittanceColor(transmittanceColor),
    m_enableEmission(enableEmission), m_emissiveIntensity(emissiveIntensity), m_emissiveColorConstant(emissiveColorConstant),
    m_isThinWalled(isThinWalled), m_thinWallThickness(thinWallThickness), m_useDiffuseLayer(useDiffuseLayer), m_samplerIndex(samplerIndex)
  {
    updateCachedData();
    updateCachedHash();
  }

  void writeGPUData(unsigned char* data, std::size_t& offset, uint32_t surfaceIndex) const {
    [[maybe_unused]] const std::size_t oldOffset = offset;

    // For decode process, see surface_material.h
    // this data is accessed from uint16_t data[32], so data[n] refers to a pair of bytes.

    uint16_t flags = surfaceMaterialTypeTranslucent;

    // Note: Respect override flag here to let the GPU do less work in determining if the diffuse layer should be used or not.
    if (m_useDiffuseLayer || getEnableDiffuseLayerOverrideHack()) {
      flags |= TRANSLUCENT_SURFACE_MATERIAL_FLAG_USE_DIFFUSE_LAYER;
    }

    // data[0- 1]
    writeGPUHelper(data, offset, flags);
    writeGPUHelper(data, offset, glm::packHalf1x16(m_cachedBaseReflectivity));
    // data[2 - 4]
    writeGPUHelper(data, offset, glm::packHalf1x16(m_transmittanceColor.x));
    writeGPUHelper(data, offset, glm::packHalf1x16(m_transmittanceColor.y));
    writeGPUHelper(data, offset, glm::packHalf1x16(m_transmittanceColor.z));
    // data[5 - 9]
    writeGPUHelperExplicit<2>(data, offset, m_samplerIndex);
    writeGPUHelperExplicit<2>(data, offset, m_transmittanceTextureIndex);
    writeGPUHelper(data, offset, glm::packHalf1x16(m_cachedTransmittanceMeasurementDistanceOrThickness));
    writeGPUHelperExplicit<2>(data, offset, m_normalTextureIndex);
    writeGPUHelperExplicit<2>(data, offset, m_emissiveColorTextureIndex);

    // data[10]
    assert(m_cachedEmissiveIntensity <= FLOAT16_MAX);
    writeGPUHelper(data, offset, glm::packHalf1x16(m_cachedEmissiveIntensity));

    // data[11]
    // Note: Ensure IoR falls in the range expected by the encoding/decoding logic for the GPU (this should also be
    // enforced in the MDL and relevant content pipeline to prevent this assert from being triggered).
    assert(m_refractiveIndex >= 1.0f && m_refractiveIndex <= 3.0f);
    writeGPUHelper(data, offset, glm::packHalf1x16(m_refractiveIndex));

    // data[12-13]: sourceSurfaceMaterialIndex
    assert(surfaceIndex <= SURFACE_INDEX_MAX_VALUE && "Surface index exceeds SURFACE_INDEX_MAX_VALUE for TranslucentSurfaceMaterial");
    writeGPUHelperExplicit<4>(data, offset, static_cast<uint32_t>(surfaceIndex));

    // data[14-16]: emissiveColorConstant
    writeGPUHelper(data, offset, glm::packHalf1x16(m_emissiveColorConstant.x));
    writeGPUHelper(data, offset, glm::packHalf1x16(m_emissiveColorConstant.y));
    writeGPUHelper(data, offset, glm::packHalf1x16(m_emissiveColorConstant.z));
    
    // data[17 - 31]
    writeGPUPadding<30>(data, offset);

    assert(offset - oldOffset == kSurfaceMaterialGPUSize);
  }

  bool validate() const {
    const bool hasTexture = m_normalTextureIndex != kSurfaceMaterialInvalidTextureIndex ||
                            m_transmittanceTextureIndex != kSurfaceMaterialInvalidTextureIndex ||
                            m_emissiveColorTextureIndex != kSurfaceMaterialInvalidTextureIndex;

    return !hasTexture || m_samplerIndex != kSurfaceMaterialInvalidTextureIndex;
  }

  bool operator==(const RtTranslucentSurfaceMaterial& r) const {
    return m_cachedHash == r.m_cachedHash;
  }

  XXH64_hash_t getHash() const {
    return m_cachedHash;
  }

  template<typename Fn>
  void forEachTextureIndex(Fn&& fn) const {
    fn(m_normalTextureIndex);
    fn(m_transmittanceTextureIndex);
    fn(m_emissiveColorTextureIndex);
  }

private:
  void updateCachedHash() {
    static_assert(
      sizeof(*this) == 96,
      "add new member for hashing if needed: add a MEMBER into the struct + add a VALUE into the list-init"
    );
    struct HashStruct {
      uint32_t normalTextureIndex;
      uint32_t transmittanceTextureIndex;
      uint32_t emissiveColorTextureIndex;
      float refractiveIndex;
      Vector3 transmittanceColor;
      float transmittanceMeasurementDistance;
      uint32_t enableEmission;  // NOTE: uint32_t to avoid padding
      float emissiveIntensity;
      Vector3 emissiveColorConstant;
      uint32_t isThinWalled;    // NOTE: uint32_t to avoid padding
      float thinWallThickness;
      uint32_t useDiffuseLayer; // NOTE: uint32_t to avoid padding
      uint32_t samplerIndex;
      // NOTE: There must be NO padding between members, as the struct is used for hashing
    };
    static_assert(alignof(HashStruct) == 4 && sizeof(HashStruct) % 4 == 0);
    HashStruct hashData = HashStruct{
      m_normalTextureIndex,
      m_transmittanceTextureIndex,
      m_emissiveColorTextureIndex,
      m_refractiveIndex,
      m_transmittanceColor,
      m_transmittanceMeasurementDistance,
      m_enableEmission,
      m_emissiveIntensity,
      m_emissiveColorConstant,
      m_isThinWalled,
      m_thinWallThickness,
      m_useDiffuseLayer,
      m_samplerIndex,
    };
    m_cachedHash = XXH3_64bits(&hashData, sizeof(hashData));
  }

  void updateCachedData() {
    // Note: Based on the Fresnel Equations with the assumption of a vacuum (nearly air
    // as the surrounding medium always) and an IoR of always >=1 (implicitly ensured by encoding
    // logic assertions later):
    // https://en.wikipedia.org/wiki/Fresnel_equations#Special_cases
    const float x = (1.0f - m_refractiveIndex) / (1.0f + m_refractiveIndex);

    m_cachedBaseReflectivity = x * x;
    m_cachedTransmittanceMeasurementDistanceOrThickness =
      m_isThinWalled ? -m_thinWallThickness : m_transmittanceMeasurementDistance;

    // Note: Translucent material does not take an emissive radiance directly, so zeroing out the intensity works
    // fine as a way to disable it (in case a texture is in use).
    m_cachedEmissiveIntensity = std::min(m_enableEmission ? m_emissiveIntensity : 0.0f, FLOAT16_MAX);

    // Note: Ensure the transmittance measurement distance or thickness was encoded properly by ensuring
    // it is not 0. This is because we currently do not actually check the sign bit but just use a less than
    // comparison to check the sign bit as neither of these values should be 0 in valid materials.
    assert(m_cachedTransmittanceMeasurementDistanceOrThickness != 0.0f);
  }

  uint32_t m_normalTextureIndex;
  uint32_t m_transmittanceTextureIndex;
  uint32_t m_emissiveColorTextureIndex;
  uint32_t m_samplerIndex;

  float m_refractiveIndex;
  Vector3 m_transmittanceColor;
  float m_transmittanceMeasurementDistance;
  bool m_enableEmission;
  float m_emissiveIntensity;
  Vector3 m_emissiveColorConstant;
  bool m_isThinWalled;
  float m_thinWallThickness;
  bool m_useDiffuseLayer;

  XXH64_hash_t m_cachedHash;

  // Note: Cached values are not involved in the hash as they are derived from the input data
  float m_cachedBaseReflectivity;
  float m_cachedTransmittanceMeasurementDistanceOrThickness;
  float m_cachedEmissiveIntensity;
};

struct RtRayPortalSurfaceMaterial {
  RtRayPortalSurfaceMaterial(
    uint32_t maskTextureIndex, uint32_t maskTextureIndex2, uint8_t rayPortalIndex,
    float rotationSpeed, bool enableEmission, float emissiveIntensity, uint32_t samplerIndex, uint32_t samplerIndex2) :
    m_maskTextureIndex{ maskTextureIndex }, m_maskTextureIndex2 { maskTextureIndex2 }, m_rayPortalIndex{ rayPortalIndex },
    m_rotationSpeed { rotationSpeed }, m_enableEmission(enableEmission), m_emissiveIntensity(emissiveIntensity), m_samplerIndex(samplerIndex), m_samplerIndex2(samplerIndex2) {
    updateCachedHash();
  }

  void writeGPUData(unsigned char* data, std::size_t& offset) const {
    [[maybe_unused]] const std::size_t oldOffset = offset;

    // For decode process, see surface_material.h
    // this data is accessed from uint16_t data[32], so data[n] refers to a pair of bytes.

    uint16_t flags = surfaceMaterialTypeRayPortal;
    // data[0]
    writeGPUHelper(data, offset, flags);

    // data[1]
    writeGPUHelper(data, offset, uint16_t(m_rayPortalIndex));

    // data[2 - 3]
    writeGPUHelperExplicit<2>(data, offset, m_maskTextureIndex);
    writeGPUHelperExplicit<2>(data, offset, m_maskTextureIndex2);

    // data[4 - 5]
    assert(m_rotationSpeed < FLOAT16_MAX);
    writeGPUHelper(data, offset, glm::packHalf1x16(m_rotationSpeed));
    float emissiveIntensity = m_enableEmission ? m_emissiveIntensity : 1.0f;
    writeGPUHelper(data, offset, glm::packHalf1x16(emissiveIntensity));

    // data[6 - 7]
    writeGPUHelperExplicit<2>(data, offset, m_samplerIndex);
    writeGPUHelperExplicit<2>(data, offset, m_samplerIndex2);

    // data[8 - 31]
    writeGPUPadding<48>(data, offset); // Note: Padding for unused space
    assert(offset - oldOffset == kSurfaceMaterialGPUSize);
  }

  bool validate() const {
    if (m_maskTextureIndex != kSurfaceMaterialInvalidTextureIndex && m_samplerIndex == kSurfaceMaterialInvalidTextureIndex) {
      return false;
    }

    if (m_maskTextureIndex2 != kSurfaceMaterialInvalidTextureIndex && m_samplerIndex2 == kSurfaceMaterialInvalidTextureIndex) {
      return false;
    }

    return true;
  }

  bool operator==(const RtRayPortalSurfaceMaterial& r) const {
    return m_cachedHash == r.m_cachedHash;
  }

  XXH64_hash_t getHash() const {
    return m_cachedHash;
  }

  uint32_t getMaskTextureIndex() const {
    return m_maskTextureIndex;
  }

  uint32_t getMaskTextureIndex2() const {
    return m_maskTextureIndex2;
  }

  uint32_t getSamplerIndex() const {
    return m_samplerIndex;
  }

  uint32_t getSamplerIndex2() const {
    return m_samplerIndex2;
  }

  uint8_t getRayPortalIndex() const {
    return m_rayPortalIndex;
  }

  float getRotationSpeed() const {
    return m_rotationSpeed;
  }

  bool getEnableEmission() const {
    return m_enableEmission;
  }

  float getEmissiveIntensity() const {
    return m_emissiveIntensity;
  }

  template<typename Fn>
  void forEachTextureIndex(Fn&& fn) const {
    fn(m_maskTextureIndex);
    fn(m_maskTextureIndex2);
  }

private:
  void updateCachedHash() {
    static_assert(
      sizeof(*this) == 40,
      "add new member for hashing if needed: add a MEMBER into the struct + add a VALUE into the list-init"
    );
    struct HashStruct {
      uint32_t maskTextureIndex;
      uint32_t maskTextureIndex2;
      uint32_t rayPortalIndex;  // NOTE: uint32_t to avoid padding
      float rotationSpeed;
      uint32_t enableEmission;  // NOTE: uint32_t to avoid padding
      float emissiveIntensity;
      uint32_t samplerIndex;
      uint32_t samplerIndex2;
      // NOTE: There must be NO padding between members, as the struct is used for hashing
    };
    static_assert(alignof(HashStruct) == 4 && sizeof(HashStruct) % 4 == 0);
    HashStruct hashData = HashStruct{
      m_maskTextureIndex,
      m_maskTextureIndex2,
      m_rayPortalIndex,
      m_rotationSpeed,
      m_enableEmission,
      m_emissiveIntensity,
      m_samplerIndex,
      m_samplerIndex2,
    };
    m_cachedHash = XXH3_64bits(&hashData, sizeof(hashData));
  }

  uint32_t m_maskTextureIndex;
  uint32_t m_maskTextureIndex2;
  uint32_t m_samplerIndex;
  uint32_t m_samplerIndex2;

  uint8_t m_rayPortalIndex;
  float m_rotationSpeed;
  bool m_enableEmission;
  float m_emissiveIntensity;

  XXH64_hash_t m_cachedHash;
};

// Extension of the three basic types of materials.
// Don't use material types below standalone. Instead, attach them to the materials above as side load data.

// Subsurface Material
struct RtSubsurfaceMaterial {
  RtSubsurfaceMaterial(
    const uint32_t subsurfaceTransmittanceTextureIndex,
    const uint32_t subsurfaceThicknessTextureIndex,
    const uint32_t subsurfaceSingleScatteringAlbedoTextureIndex,
    const Vector3& subsurfaceTransmittanceColor,
    const float subsurfaceMeasurementDistance,
    const Vector3& subsurfaceSingleScatteringAlbedo,
    const float subsurfaceVolumetricAnisotropy,
    const float subsurfaceRadiusScale,
    const float subsurfaceMaxSampleRadius)
    :
    m_subsurfaceTransmittanceTextureIndex(subsurfaceTransmittanceTextureIndex),
    m_subsurfaceThicknessTextureIndex(subsurfaceThicknessTextureIndex),
    m_subsurfaceSingleScatteringAlbedoTextureIndex(subsurfaceSingleScatteringAlbedoTextureIndex),
    m_subsurfaceTransmittanceColor { subsurfaceTransmittanceColor },
    m_subsurfaceMeasurementDistance { subsurfaceMeasurementDistance },
    m_subsurfaceSingleScatteringAlbedo { subsurfaceSingleScatteringAlbedo },
    m_subsurfaceVolumetricAnisotropy { subsurfaceVolumetricAnisotropy },
    // Because we do log on the transmittance color when mapping to attenuation coefficient, we need to clamp to a small epsilon value to avoid NaN issue.
    m_subsurfaceVolumetricAttenuationCoefficient {
      Vector3(-log(std::max(subsurfaceTransmittanceColor.x, FLT_EPSILON)),
              -log(std::max(subsurfaceTransmittanceColor.y, FLT_EPSILON)),
              -log(std::max(subsurfaceTransmittanceColor.z, FLT_EPSILON))) / std::max(subsurfaceMeasurementDistance, FLT_EPSILON) },
    m_subsurfaceRadiusScale { subsurfaceRadiusScale },
    m_subsurfaceMaxSampleRadius { subsurfaceMaxSampleRadius }
  {
    updateCachedHash();
  }

  void writeGPUData(unsigned char* data, std::size_t& offset) const {
    [[maybe_unused]] const std::size_t oldOffset = offset;

    // For decode process, see surface_material.h
    // this data is accessed from uint16_t data[32], so data[n] refers to a pair of bytes.

    // Write an empty flags to stay consistent with the other materials.
    uint16_t flags = 0;

    // data[0]
    writeGPUHelperExplicit<2>(data, offset, flags);

    // data[1]
    writeGPUHelperExplicit<2>(data, offset, m_subsurfaceTransmittanceTextureIndex);

    // data[2]
    writeGPUHelperExplicit<2>(data, offset, m_subsurfaceThicknessTextureIndex);

    // data[3]
    writeGPUHelperExplicit<2>(data, offset, m_subsurfaceSingleScatteringAlbedoTextureIndex);

    // data[4]
    writeGPUHelper(data, offset, glm::packHalf1x16(m_subsurfaceVolumetricAnisotropy));

    // data[5-8]
    if (m_subsurfaceRadiusScale < 0.0f) { // Thin Opaque
      writeGPUHelper(data, offset, glm::packHalf1x16(m_subsurfaceVolumetricAttenuationCoefficient.x));
      writeGPUHelper(data, offset, glm::packHalf1x16(m_subsurfaceVolumetricAttenuationCoefficient.y));
      writeGPUHelper(data, offset, glm::packHalf1x16(m_subsurfaceVolumetricAttenuationCoefficient.z));

      assert(m_subsurfaceMeasurementDistance <= FLOAT16_MAX);
      writeGPUHelper(data, offset, glm::packHalf1x16(m_subsurfaceMeasurementDistance));
    } else { // SSS
      writeGPUHelper(data, offset, glm::packHalf1x16(m_subsurfaceTransmittanceColor.x));
      writeGPUHelper(data, offset, glm::packHalf1x16(m_subsurfaceTransmittanceColor.y));
      writeGPUHelper(data, offset, glm::packHalf1x16(m_subsurfaceTransmittanceColor.z));

      assert(m_subsurfaceRadiusScale <= FLOAT16_MAX);
      writeGPUHelper(data, offset, glm::packHalf1x16(m_subsurfaceRadiusScale));
    }

    // data[9-11]
    writeGPUHelper(data, offset, glm::packHalf1x16(m_subsurfaceSingleScatteringAlbedo.x));
    writeGPUHelper(data, offset, glm::packHalf1x16(m_subsurfaceSingleScatteringAlbedo.y));
    writeGPUHelper(data, offset, glm::packHalf1x16(m_subsurfaceSingleScatteringAlbedo.z));

    // data[12]
    writeGPUHelper(data, offset, glm::packHalf1x16(m_subsurfaceMaxSampleRadius));

    // data[13-31]
    writeGPUPadding<38>(data, offset);
  }

  bool operator==(const RtSubsurfaceMaterial& r) const {
    return m_cachedHash == r.m_cachedHash;
  }

  bool validate() const {
    return true;
  }

  XXH64_hash_t getHash() const {
    return m_cachedHash;
  }

  uint32_t getSubsurfaceTransmittanceTextureIndex() const {
    return m_subsurfaceTransmittanceTextureIndex;
  }

  uint32_t getSubsurfaceThicknessTextureIndex() const {
    return m_subsurfaceThicknessTextureIndex;
  }

  uint32_t getSubsurfaceSingleScatteringAlbedoTextureIndex() const {
    return m_subsurfaceSingleScatteringAlbedoTextureIndex;
  }

  float getSubsurfaceMeasurementDistance() const {
    return m_subsurfaceMeasurementDistance;
  }

  const Vector3& getSubsurfaceVolumetricScatteringAlbedo() const {
    return m_subsurfaceSingleScatteringAlbedo;
  }

  float getSubsurfaceVolumetricAnisotropy() const {
    return m_subsurfaceVolumetricAnisotropy;
  }

  const Vector3& getSubsurfaceVolumetricAttenuationCoefficient() const {
    return m_subsurfaceVolumetricAttenuationCoefficient;
  }

  float getSubsurfaceRadiusScale() const {
    return m_subsurfaceRadiusScale;
  }

  float getSubsurfaceMaxRadius() const {
    return m_subsurfaceMaxSampleRadius;
  }

  template<typename Fn>
  void forEachTextureIndex(Fn&& fn) const {
    fn(m_subsurfaceTransmittanceTextureIndex);
    fn(m_subsurfaceThicknessTextureIndex);
    fn(m_subsurfaceSingleScatteringAlbedoTextureIndex);
  }

private:

  void updateCachedHash() {
    static_assert(
      sizeof(*this) == 72,
      "add new member for hashing if needed: add a MEMBER into the struct + add a VALUE into the list-init"
    );
    struct HashStruct {
      uint32_t m_subsurfaceTransmittanceTextureIndex;
      uint32_t m_subsurfaceThicknessTextureIndex;
      uint32_t m_subsurfaceSingleScatteringAlbedoTextureIndex;
      Vector3 m_subsurfaceTransmittanceColor;
      float m_subsurfaceMeasurementDistance;
      Vector3 m_subsurfaceSingleScatteringAlbedo;
      float m_subsurfaceVolumetricAnisotropy;
      Vector3 m_subsurfaceVolumetricAttenuationCoefficient;
      float m_subsurfaceRadiusScale;
      float m_subsurfaceMaxSampleRadius;
      // NOTE: There must be NO padding between members, as the struct is used for hashing
    };
    static_assert(alignof(HashStruct) == 4 && sizeof(HashStruct) % 4 == 0);
    HashStruct hashData = HashStruct{
      m_subsurfaceTransmittanceTextureIndex,
      m_subsurfaceThicknessTextureIndex,
      m_subsurfaceSingleScatteringAlbedoTextureIndex,
      m_subsurfaceTransmittanceColor,
      m_subsurfaceMeasurementDistance,
      m_subsurfaceSingleScatteringAlbedo,
      m_subsurfaceVolumetricAnisotropy,
      m_subsurfaceVolumetricAttenuationCoefficient,
      m_subsurfaceRadiusScale,
      m_subsurfaceMaxSampleRadius,
    };
    m_cachedHash = XXH3_64bits(&hashData, sizeof(hashData));
  }

  // Thin Opaque Textures Index (Shared with SSS)
  uint32_t m_subsurfaceTransmittanceTextureIndex;
  uint32_t m_subsurfaceThicknessTextureIndex;
  uint32_t m_subsurfaceSingleScatteringAlbedoTextureIndex;

  // Thin Opaque Properties (Shared with SSS)
  Vector3 m_subsurfaceTransmittanceColor;
  float m_subsurfaceMeasurementDistance;
  Vector3 m_subsurfaceSingleScatteringAlbedo; // scatteringCoefficient / attenuationCoefficient
  float m_subsurfaceVolumetricAnisotropy;

  // Cache Volumetric Properties
  Vector3 m_subsurfaceVolumetricAttenuationCoefficient; // scatteringCoefficient + absorptionCoefficient
  // Currently no need to cache scattering and absorption coefficient for single scattering simulation

  // SSS properties using Diffusion Profile
  float m_subsurfaceRadiusScale;
  float m_subsurfaceMaxSampleRadius;

  XXH64_hash_t m_cachedHash;
};

struct RtSurfaceMaterial {
  RtSurfaceMaterial(const RtOpaqueSurfaceMaterial& opaqueSurfaceMaterial) :
    m_type{ RtSurfaceMaterialType::Opaque },
    m_opaqueSurfaceMaterial{ opaqueSurfaceMaterial } {}

  RtSurfaceMaterial(const RtTranslucentSurfaceMaterial& translucentSurfaceMaterial) :
    m_type{ RtSurfaceMaterialType::Translucent },
    m_translucentSurfaceMaterial{ translucentSurfaceMaterial } {}

  RtSurfaceMaterial(const RtRayPortalSurfaceMaterial& rayPortalSurfaceMaterial) :
    m_type{ RtSurfaceMaterialType::RayPortal },
    m_rayPortalSurfaceMaterial{ rayPortalSurfaceMaterial } {}

  RtSurfaceMaterial(const RtSubsurfaceMaterial& subsurfaceMaterial) :
    m_type { RtSurfaceMaterialType::Subsurface },
    m_subsurfaceMaterial { subsurfaceMaterial } {}

  RtSurfaceMaterial(const RtSurfaceMaterial& surfaceMaterial) :
    m_type{ surfaceMaterial.m_type } {
    switch (m_type) {
    default:
      assert(false);

      [[fallthrough]];
    case RtSurfaceMaterialType::Opaque:
      new (&m_opaqueSurfaceMaterial) RtOpaqueSurfaceMaterial{ surfaceMaterial.m_opaqueSurfaceMaterial };
      break;
    case RtSurfaceMaterialType::Translucent:
      new (&m_translucentSurfaceMaterial) RtTranslucentSurfaceMaterial{ surfaceMaterial.m_translucentSurfaceMaterial };
      break;
    case RtSurfaceMaterialType::RayPortal:
      new (&m_rayPortalSurfaceMaterial) RtRayPortalSurfaceMaterial{ surfaceMaterial.m_rayPortalSurfaceMaterial };
      break;
    case RtSurfaceMaterialType::Subsurface:
      new (&m_subsurfaceMaterial) RtSubsurfaceMaterial { surfaceMaterial.m_subsurfaceMaterial };
      break;
    }
  }

  ~RtSurfaceMaterial() {
    switch (m_type) {
    default:
      assert(false);

      [[fallthrough]];
    case RtSurfaceMaterialType::Opaque:
      m_opaqueSurfaceMaterial.~RtOpaqueSurfaceMaterial();
      break;
    case RtSurfaceMaterialType::Translucent:
      m_translucentSurfaceMaterial.~RtTranslucentSurfaceMaterial();
      break;
    case RtSurfaceMaterialType::RayPortal:
      m_rayPortalSurfaceMaterial.~RtRayPortalSurfaceMaterial();
      break;
    case RtSurfaceMaterialType::Subsurface:
      m_subsurfaceMaterial.~RtSubsurfaceMaterial();
      break;
    }
  }

  void writeGPUData(unsigned char* data, std::size_t& offset, uint32_t surfaceIndex) const {
    switch (m_type) {
    default:
      assert(false);

      [[fallthrough]];
    case RtSurfaceMaterialType::Opaque:
      m_opaqueSurfaceMaterial.writeGPUData(data, offset);
      break;
    case RtSurfaceMaterialType::Translucent:
      m_translucentSurfaceMaterial.writeGPUData(data, offset, surfaceIndex);
      break;
    case RtSurfaceMaterialType::RayPortal:
      m_rayPortalSurfaceMaterial.writeGPUData(data, offset);
      break;
    case RtSurfaceMaterialType::Subsurface:
      m_subsurfaceMaterial.writeGPUData(data, offset);
      break;
    }
  }

  bool validate() const {
    switch (m_type) {
    default:
      assert(false);

      [[fallthrough]];
    case RtSurfaceMaterialType::Opaque:
      return m_opaqueSurfaceMaterial.validate();
    case RtSurfaceMaterialType::Translucent:
      return m_translucentSurfaceMaterial.validate();
    case RtSurfaceMaterialType::RayPortal:
      return m_rayPortalSurfaceMaterial.validate();
    case RtSurfaceMaterialType::Subsurface:
      return m_subsurfaceMaterial.validate();
    }

    return false;
  }

  RtSurfaceMaterial& operator=(const RtSurfaceMaterial& rtSurfaceMaterial) {
    if (this != &rtSurfaceMaterial) {
      m_type = rtSurfaceMaterial.m_type;

      switch (rtSurfaceMaterial.m_type) {
      default:
        assert(false);

        [[fallthrough]];
      case RtSurfaceMaterialType::Opaque:
        m_opaqueSurfaceMaterial = rtSurfaceMaterial.m_opaqueSurfaceMaterial;
        break;
      case RtSurfaceMaterialType::Translucent:
        m_translucentSurfaceMaterial = rtSurfaceMaterial.m_translucentSurfaceMaterial;
        break;
      case RtSurfaceMaterialType::RayPortal:
        m_rayPortalSurfaceMaterial = rtSurfaceMaterial.m_rayPortalSurfaceMaterial;
        break;
      case RtSurfaceMaterialType::Subsurface:
        m_subsurfaceMaterial = rtSurfaceMaterial.m_subsurfaceMaterial;
        break;
      }
    }

    return *this;
  }

  bool operator==(const RtSurfaceMaterial& rhs) const {
    // Note: Different Surface Material types are not the same Surface Material so comparison can return false
    if (m_type != rhs.m_type) {
      return false;
    }

    switch (m_type) {
    default:
      assert(false);

      [[fallthrough]];
    case RtSurfaceMaterialType::Opaque:
      return m_opaqueSurfaceMaterial == rhs.m_opaqueSurfaceMaterial;
    case RtSurfaceMaterialType::Translucent:
      return m_translucentSurfaceMaterial == rhs.m_translucentSurfaceMaterial;
    case RtSurfaceMaterialType::RayPortal:
      return m_rayPortalSurfaceMaterial == rhs.m_rayPortalSurfaceMaterial;
    case RtSurfaceMaterialType::Subsurface:
      return m_subsurfaceMaterial == rhs.m_subsurfaceMaterial;
    }
  }

  XXH64_hash_t getHash() const {
    switch (m_type) {
    default:
      assert(false);

      [[fallthrough]];
    case RtSurfaceMaterialType::Opaque:
      return m_opaqueSurfaceMaterial.getHash();
    case RtSurfaceMaterialType::Translucent:
      return m_translucentSurfaceMaterial.getHash();
    case RtSurfaceMaterialType::RayPortal:
      return m_rayPortalSurfaceMaterial.getHash();
    case RtSurfaceMaterialType::Subsurface:
      return m_subsurfaceMaterial.getHash();
    }
  }

  RtSurfaceMaterialType getType() const {
    return m_type;
  }

  const RtOpaqueSurfaceMaterial& getOpaqueSurfaceMaterial() const {
    assert(m_type == RtSurfaceMaterialType::Opaque);

    return m_opaqueSurfaceMaterial;
  }

  const RtTranslucentSurfaceMaterial& getTranslucentSurfaceMaterial() const {
    assert(m_type == RtSurfaceMaterialType::Translucent);

    return m_translucentSurfaceMaterial;
  }

  const RtRayPortalSurfaceMaterial& getRayPortalSurfaceMaterial() const {
    assert(m_type == RtSurfaceMaterialType::RayPortal);

    return m_rayPortalSurfaceMaterial;
  }

  template<typename Fn>
  void forEachTextureIndex(Fn&& fn) const {
    switch (m_type) {
    default:
      assert(false);

      [[fallthrough]];
    case RtSurfaceMaterialType::Opaque:
      m_opaqueSurfaceMaterial.forEachTextureIndex(fn);
      break;
    case RtSurfaceMaterialType::Translucent:
      m_translucentSurfaceMaterial.forEachTextureIndex(fn);
      break;
    case RtSurfaceMaterialType::RayPortal:
      m_rayPortalSurfaceMaterial.forEachTextureIndex(fn);
      break;
    case RtSurfaceMaterialType::Subsurface:
      m_subsurfaceMaterial.forEachTextureIndex(fn);
      break;
    }
  }

private:
  // Type-specific Surface Material Information

  RtSurfaceMaterialType m_type;
  union {
    RtOpaqueSurfaceMaterial m_opaqueSurfaceMaterial;
    RtTranslucentSurfaceMaterial m_translucentSurfaceMaterial;
    RtRayPortalSurfaceMaterial m_rayPortalSurfaceMaterial;
    RtSubsurfaceMaterial m_subsurfaceMaterial;
  };
};

// Volume Materials

// Todo: Compute size directly from sizeof of GPU structure (by including it), for now computed by sum of members manually
constexpr std::size_t kVolumeMaterialGPUSize = 4;

struct RtVolumeMaterial
{
  RtVolumeMaterial() {
    updateCachedHash();
  }

  void writeGPUData(unsigned char* data, std::size_t& offset) const {
    [[maybe_unused]] const std::size_t oldOffset = offset;

    writeGPUPadding<4>(data, offset);

    assert(offset - oldOffset == kVolumeMaterialGPUSize);
  }

  bool operator==(const RtVolumeMaterial& r) const {
    assert(false);

    return m_cachedHash == r.m_cachedHash;
  }

  XXH64_hash_t getHash() const {
    assert(false);

    return m_cachedHash;
  }
private:
  void updateCachedHash() {
    const XXH64_hash_t h = 0;

    m_cachedHash = h;
  }

  XXH64_hash_t m_cachedHash;
};

enum class MaterialDataType {
  Opaque,
  Translucent,
  RayPortal,
  Count,
  Invalid
};

// Note: For use with "Legacy" D3D11 material information
struct LegacyMaterialData {
  static OpaqueMaterialData createDefault();

  LegacyMaterialData()
  { }

  LegacyMaterialData(const TextureRef& colorTexture, const TextureRef& colorTexture2, const Dx11RuntimeMaterial material)
    : dx11Material{ material }
  {
    // Note: Texture required to be populated for hashing to function
    assert(!colorTexture.isImageEmpty());

    updateCachedHash();
  }

  const XXH64_hash_t getHash() const {
    return m_cachedHash;
  }

  const TextureRef& getColorTexture() const {
    return colorTextures[0];
  }

  const TextureRef& getColorTexture2() const {
    return colorTextures[1];
  }

  const Rc<DxvkSampler>& getSampler() const {
    return samplers[0];
  }

  const Rc<DxvkSampler>& getSampler2() const {
    return samplers[1];
  }

  const Dx11RuntimeMaterial& getLegacyMaterial() const {
    return dx11Material;
  }

  inline const bool usesTexture() const {
    return ((getColorTexture().isValid()  && !getColorTexture().isImageEmpty()) ||
            (getColorTexture2().isValid() && !getColorTexture2().isImageEmpty()));
  }

  // A single place to define and handle conversions between legacy and raytraced materials
  template<typename T>
  T as() const;

  const void printDebugInfo(const char* name = "") const {
#ifdef REMIX_DEVELOPMENT
    Logger::warn(str::format(
      "LegacyMaterialData ", name, "\n",
      "  address: ", this, "\n",
      "  alphaTestEnabled: ", alphaTestEnabled, "\n",
      "  alphaTestReferenceValue: ", alphaTestReferenceValue, "\n",
      "  alphaTestCompareOp: ", alphaTestCompareOp, "\n",
      "  alphaBlendEnabled: ", blendMode.enableBlending, "\n",
      "  colorSrcFactor: ", blendMode.colorSrcFactor, "\n",
      "  colorDstFactor: ", blendMode.colorDstFactor, "\n",
      "  colorBlendOp: ", blendMode.colorBlendOp, "\n",
      "  alphaSrcFactor: ", blendMode.alphaSrcFactor, "\n",
      "  alphaDstFactor: ", blendMode.alphaDstFactor, "\n",
      "  alphaBlendOp: ", blendMode.alphaBlendOp, "\n",
      "  writeMask: ", blendMode.writeMask, "\n",
      "  textureColorArg1Source: ", static_cast<int>(textureColorArg1Source), "\n",
      "  textureColorArg2Source: ", static_cast<int>(textureColorArg2Source), "\n",
      "  textureColorOperation: ", static_cast<int>(textureColorOperation), "\n",
      "  textureAlphaArg1Source: ", static_cast<int>(textureAlphaArg1Source), "\n",
      "  textureAlphaArg2Source: ", static_cast<int>(textureAlphaArg2Source), "\n",
      "  textureAlphaOperation: ", static_cast<int>(textureAlphaOperation), "\n",
      "  tFactor: ", tFactor, "\n",
      // "  m_dx11Material.Diffuse: ", m_dx11Material.Diffuse, "\n",
      // "  m_dx11Material.Ambient: ", m_dx11Material.Ambient, "\n",
      // "  m_dx11Material.Specular: ", m_dx11Material.Specular, "\n",
      // "  m_dx11Material.Emissive: ", m_dx11Material.Emissive, "\n",
      // "  m_dx11Material.Power: ", m_dx11Material.Power, "\n",
      std::hex, "  m_colorTexture: 0x", colorTextures[0].getImageHash(), "\n",
      "  m_colorTexture2: 0x", colorTextures[1].getImageHash(), "\n",
      "  m_cachedHash: 0x", m_cachedHash, std::dec));
#endif
  }

  uint32_t getColorTextureSlot(uint32_t slot) const {
    return colorTextureSlot[slot];
  }

  bool alphaTestEnabled = false;
  uint8_t alphaTestReferenceValue = 0;
  VkCompareOp alphaTestCompareOp = VkCompareOp::VK_COMPARE_OP_ALWAYS;

  DxvkBlendMode blendMode;

  // Capture-side counterpart of the RtSurface fields; see D3D11ColorSource.
  D3D11ColorSource colorSource = D3D11ColorSource::Texture;
  D3D11ColorSource alphaSource = D3D11ColorSource::Texture;
  // Set only when the pixel-shader bytecode proves that its cutout mask is
  // sampled from colorTextures[1], rather than from the albedo texture.
  bool useSecondaryTextureForOpacity = false;
  // Set when this draw is Kenshi's terrain. The vectors turn the surface's interpolated overlay
  // coordinate into the base tiled detail coordinate; see KenshiTerrainArgs.
  uint8_t kenshiTerrainBlend = 0; // 1 ground, 2 NO_ROADS feature; same storage
  // Exact capture-side rain-family marker. Per draw and excluded from material identity: the four
  // textures already have distinct hashes, and this state belongs to the surface blend path.
  bool kenshiRain = false;
  // FS_0284 character draws add the head atlas at UV.y + 1 for negative UVs.
  bool kenshiCharacterHead = false;
  // This draw's pixel shader carries two full texture sets and blends them with COLOR0.a.
  // colorTextures[1] holds the second albedo (diffuseMap2 / base_map2).
  bool kenshiDualTextureSet = false;
  // The second set's normal and metal maps, carried in two of the character-mask texture indices, which
  // no dual-set material uses.
  TextureRef kenshiDualNormalTexture = {};
  TextureRef kenshiDualMetalTexture = {};
  // Kenshi's two-colour recolour (armour, gear). colorTextures[1] holds the mask; the two colours travel
  // as constants.
  bool kenshiColorMask = false;
  Vector4 kenshiColor1 = Vector4(0.0f, 0.0f, 0.0f, 0.0f);
  Vector4 kenshiColor2 = Vector4(0.0f, 0.0f, 0.0f, 0.0f);
  // The worn-clothing layer. vestNormal's alpha is the coverage mask that decides skin versus clothing
  // per texel.
  bool kenshiCharacterVest = false;
  TextureRef kenshiVestDiffuseTexture = {};
  TextureRef kenshiVestNormalTexture = {};
  TextureRef kenshiVestMaskTexture = {};
  Vector4 kenshiVestColor = Vector4(1.0f, 1.0f, 1.0f, 1.0f);
  // A building under construction: the scaffold lattice texture (construction.dds) and the three scalars
  // the game's shader drives it with (see the construction capture in d3d11_rtx.cpp):
  //   x = constructionState.x, the build progress 0..1
  //   y = upperPos.x, the object-space height the progress is measured against
  //   z = scaffoldTiling, the UV multiplier for the lattice
  bool kenshiConstruction = false;
  TextureRef kenshiConstructionGridTexture = {};
  Vector4 kenshiConstructionParams = Vector4(0.0f, 0.0f, 0.0f, 0.0f);
  // The biome dust noise texture, sampled at world XZ. Travels in tangentTextureIndex (see the capture
  // site).
  TextureRef kenshiDustNoiseTexture = {};
  // Dust colour is per material, not a biome global. rgb = the raw display-space dustColour; w =
  // presence plus the shader's static amount selector (1 = dustAmount.x, 2 = .y). The frame-global amount
  // vector travels in RaytraceArgs. This rides in albedoOpacityConstant, unused on a textured material.
  Vector4 kenshiDustColour = Vector4(0.0f, 0.0f, 0.0f, 0.0f);
  TextureRef kenshiCharacterBodyMaskTexture = {};
  TextureRef kenshiCharacterHeadMaskTexture = {};
  TextureRef kenshiCharacterHairTexture = {};
  TextureRef kenshiCharacterBeardTexture = {};
  // `headNormalMap` (s6 in character.hlsl): the head lives in negative V of the same draw as the body and
  // has its own normal map. Travels to the GPU in the unused tangentTextureIndex slot (see the shader).
  TextureRef kenshiCharacterHeadNormalTexture = {};
  // Per-draw state: deliberately excluded from material hashing.
  uint32_t kenshiBloodMode = 0u; // 1: Y-axis regional, 2: X-axis severed limb
  TextureRef kenshiBloodTexture = {};
  Rc<DxvkSampler> kenshiBloodSampler = nullptr;
  std::array<float, 8> kenshiBloodAmounts = {};
  Vector2 kenshiBloodScale = Vector2(1.0f, 1.0f);
  Vector4 kenshiBloodColor = Vector4(0.0f, 0.0f, 0.0f, 0.0f);
  // The game's own tangent-space normal map, found by reflection name on the pixel shader (`base_normal`,
  // `normalMap`, `normalMap2`) rather than by slot. Remix's generic normalTextureIndex was only ever fed
  // from USD replacements; this carries a game texture into it (it lands in
  // LegacyMaterialData::as<OpaqueMaterialData>()). No tangent capture is needed: Remix derives a
  // UV-aligned tangent frame per hit (genTangSpace() in surface_interaction.slangh). The decode is
  // selected by kenshiNormalEncoding below.
  TextureRef kenshiNormalTexture = {};
  // 1 = standard RGB, 2 = DXT5nm; 0 leaves the material on Remix's octahedral decode. character.hlsl and
  // its severed_limb_fs write the DXT5nm expansion out longhand without `#ifdef DXT5NORMAL`, so character
  // bodies and heads are DXT5nm too.
  uint32_t kenshiNormalEncoding = 0u;
  // This shader family inverts the normal map's green channel: skin.hlsl before the decode
  // (`normalTex.g = 1.0f - normalTex.g`), creature.hlsl after it (`normalTex.g = -normalTex.g`);
  // objects, foliage and character.hlsl do not. Travels in bit 9 of kenshiCharacterHairChannels (the
  // material `flags` word is full).
  bool kenshiNormalFlipGreen = false;
  // character.hlsl's muscle-size layer, `bodyN.wy = lerp(bodyN.wy, blendNormalMap.wy, muscleBlend)` on
  // the raw DXT5nm pair before anything else; a character's build exists only in this blend
  // (<race>_body_normal_muscular.dds / _skinny.dds). Quantised to the 6 bits it travels in
  // (kenshiCharacterHairChannels bits 10-15); zero means the material binds flat.dds and the blend is a
  // no-op.
  float kenshiMuscleBlend = 0.0f;
  TextureRef kenshiCharacterBlendNormalTexture = {};
  // Kenshi's deferred lighting is a metallic workflow - `albedo * (1 - metal)` for diffuse,
  // `lerp(0.04, albedo, metal)` for F0, GGX with `alpha = 1 - 0.99 * gloss` - the same model as Remix,
  // so these are direct translations. Gloss lives in the diffuse texture's alpha, times this per-draw
  // scalar. Zero means the pixel shader does not declare glossMult and the alpha is ordinary coverage:
  // do not read it.
  float kenshiGlossMult = 0.0f;
  // `metal_map`, whose RED channel the game writes straight into its G-buffer's
  // metal channel. Remix's metallic slot wants exactly that.
  TextureRef kenshiMetalTexture = {};
  Vector3 kenshiCharacterHairColor = Vector3(0.0f, 0.0f, 0.0f);
  uint8_t kenshiCharacterHairColorChannel = 4u;
  uint8_t kenshiCharacterHairAlphaChannel = 4u;
  uint8_t kenshiCharacterBeardAlphaChannel = 4u;
  Vector2 kenshiTerrainDetailScale = Vector2(0.0f, 0.0f);
  Vector2 kenshiTerrainDetailOffset = Vector2(0.0f, 0.0f);
  static constexpr uint32_t kKenshiTerrainLayerCount = 6u;
  std::array<TextureRef, kKenshiTerrainLayerCount> kenshiTerrainLayers = {};
  std::array<Vector2, kKenshiTerrainLayerCount> kenshiTerrainLayerScales = {};
  TextureRef kenshiTerrainOverlay = {};
  Vector4 kenshiTerrainSlopeMin = Vector4(0.0f, 0.0f, 0.0f, 0.0f);
  Vector4 kenshiTerrainSlopeMax = Vector4(0.0f, 0.0f, 0.0f, 0.0f);
  Vector4 kenshiTerrainSlopeBlend = Vector4(0.0f, 0.0f, 0.0f, 0.0f);
  Vector4 kenshiTerrainOverlayMult = Vector4(0.0f, 0.0f, 0.0f, 0.0f);
  float kenshiTerrainBrightnessFix = 1.0f;
  float kenshiTerrainHeightOffset = 0.0f;
  // `distortion0.xy` from the terrain vertex shader: both terrain vertex shaders add
  // (cos(texturePos.x * .x) + cos(texturePos.z * .x)) * .y to the cliff layer's height coordinate, which
  // angles the rock strata. Zero leaves the coordinate unchanged.
  Vector2 kenshiTerrainHeightWarp = Vector2(0.0f, 0.0f);
  // A boundary tile's pixel shader carries one to three additional complete six-layer sets
  // (`diffuseMaps1..3`), each with its own scales, slope bands, overlayMult and brightnessFix, plus a
  // shared `blendMap` sampled at the biome UV. The per-chunk fields above (detail rect, overlay map,
  // height offset) are shared by every set.
  static constexpr uint32_t kKenshiTerrainBiomeSetCount = 3u;
  struct KenshiTerrainBiomeSet {
    std::array<TextureRef, kKenshiTerrainLayerCount> layers = {};
    std::array<TextureRef, kKenshiTerrainLayerCount> normals = {};
    std::array<Vector2, kKenshiTerrainLayerCount> layerScales = {};
    Vector4 slopeMin = Vector4(0.0f, 0.0f, 0.0f, 0.0f);
    Vector4 slopeMax = Vector4(0.0f, 0.0f, 0.0f, 0.0f);
    Vector4 slopeBlend = Vector4(0.0f, 0.0f, 0.0f, 0.0f);
    Vector4 overlayMult = Vector4(0.0f, 0.0f, 0.0f, 0.0f);
    float brightnessFix = 1.0f;
    Vector4 textureFade = Vector4(0.0f, 0.0f, 0.0f, 0.0f);
  };
  // CPU-only shared ownership follows prepared draws, queued commands and BlasEntry input materials. The
  // lookup tables keep weak references. Copying a draw copies two handles, not the large biome payload;
  // no GPU layout change.
  struct KenshiTerrainNormalSet {
    std::array<TextureRef, kKenshiTerrainLayerCount> textures = {};
    Vector4 textureFade = Vector4(0.0f);
  };
  using KenshiTerrainBiomeData = std::array<KenshiTerrainBiomeSet, kKenshiTerrainBiomeSetCount>;
  std::shared_ptr<KenshiTerrainNormalSet> kenshiTerrainNormals;
  std::shared_ptr<KenshiTerrainBiomeData> kenshiTerrainBiomes;
  uint64_t kenshiTerrainBiomeKey = 0u;
  TextureRef kenshiTerrainBiomeBlend = {};
  // detailBase -> blendMap UV; derived from the vertex shader's `biomeData`
  // rect exactly as the detail affine is derived from `overlayData`.
  Vector2 kenshiTerrainBiomeScale = Vector2(0.0f, 0.0f);
  Vector2 kenshiTerrainBiomeOffset = Vector2(0.0f, 0.0f);
  // Active blendMap channels, bits 0..3 = x..w. The numbered sets consume them
  // in descending w,z,y,x order and the base set takes 1 - sum(active).
  uint32_t kenshiTerrainBlendChannelMask = 0u;
  bool modulateVertexColor = false;
  bool modulateVertexAlpha = false;
  // 0-3 selects one sampled RGBA component for scalar colour; 4 disables it.
  uint8_t colorTextureChannel = 4u;
  // 0-3 selects one sampled RGBA component for opacity; 4 uses texture alpha.
  uint8_t opacityTextureChannel = 4u;
  // The D3D11 OMSetBlendState blend factor, in real floats.
  Vector4 blendConstant = Vector4(1.0f, 1.0f, 1.0f, 1.0f);
  Dx11RuntimeMaterial dx11Material = {};
  bool isTextureFactorBlend = false;
  bool isVertexColorBakedLighting = true;
  bool colorTextureIsSrgb = false;
  // The native distant-town PS receives IA COLOR0 through TEXCOORD1.
  bool kenshiDistantTown = false;
  // Constant-color materials: shaders with no material texture samplers carry their
  // color in shader constant registers. Captured at draw time so the legacy->opaque
  // conversion can use it as the albedo constant instead of rendering white.
  Vector4 constantAlbedo = Vector4(1.0f, 1.0f, 1.0f, 1.0f);
  bool hasConstantAlbedo = false;

  void setHashOverride(XXH64_hash_t hash) {
    m_cachedHash = hash;
  }

  // Material-instance compat - when set, material hashes include shader identity
  // in addition to the material texture set.
  void setPixelShaderHashForMaterialInstance(XXH64_hash_t hash) {
    m_pixelShaderHashForMaterialInstance = hash;
  }

  // Hash over the ordered (sampler index, image hash) set of all textures bound to the pixel
  // shader's material samplers (identified by name in the DXBC resource-definition chunk).
  // Differentiates material instances that override texture parameters in any material
  // sampler, not just the primary color texture.
  void setMaterialTextureSetHashForMaterialInstance(XXH64_hash_t hash) {
    m_materialTextureSetHash = hash;
  }

  // Hash over the material constant-buffer variables (identified by name in the DXBC
  // resource-definition chunk). Differentiates material instances that override vector/scalar
  // parameters on an identical texture set. kEmptyHash for shaders listed in
  // rtx.d3d11.materialInstanceIdentityExcludedShaders (frame-varying constants) or for
  // shaders without reflected constant ranges.
  void setPixelShaderConstantsHashForMaterialInstance(XXH64_hash_t hash) {
    m_pixelShaderConstantsHashForMaterialInstance = hash;
  }

  // Intermediate identity tier: PS bytecode + material texture set, without constants.
  // Shared by all material-instance siblings that only differ via constants, and stable
  // even for shaders with frame-varying constant registers. kEmptyHash unless the
  // material-instance identity path is active.
  XXH64_hash_t getTextureSetAndShaderHash() const {
    return m_textureSetShaderHash;
  }

private:
  friend class RtxContext;
  friend struct D3D11Rtx;
  friend class TerrainBaker;
  friend class SceneManager;
  friend struct RemixAPIPrivateAccessor;

  void updateCachedHash() {
    // Note: by default this is based on the color texture's data hash. This is not the same as
    // the plain data hash used by the RtSurfaceMaterial for storage in map-like data structures,
    // but rather one used to identify a material and compare to user-provided hashes.
    //
    // For material-instance compat the identity is a deterministic seed chain:
    //   PS bytecode hash -> material texture set -> material constants
    // Every input is a pure function of the current draw, so the same material instance always
    // produces the same hash.
    const XXH64_hash_t textureHash = colorTextures[0].getImageHash();
    if (m_pixelShaderHashForMaterialInstance != kEmptyHash) {
      // When the shader reflection exposes no material samplers, fall back to the primary
      // color texture so the identity keeps the texture+shader structure.
      const XXH64_hash_t textureSetHash =
        (m_materialTextureSetHash != kEmptyHash) ? m_materialTextureSetHash : textureHash;
      XXH64_hash_t hash = XXH3_64bits_withSeed(&textureSetHash, sizeof(textureSetHash), m_pixelShaderHashForMaterialInstance);
      m_textureSetShaderHash = hash;
      if (m_pixelShaderConstantsHashForMaterialInstance != kEmptyHash) {
        hash = XXH3_64bits_withSeed(
          &m_pixelShaderConstantsHashForMaterialInstance,
          sizeof(m_pixelShaderConstantsHashForMaterialInstance),
          hash);
      }
      m_cachedHash = hash;
    } else {
      m_textureSetShaderHash = kEmptyHash;
      m_cachedHash = textureHash;
    }
  }

  const static uint32_t kMaxSupportedTextures = 2;
  TextureRef colorTextures[kMaxSupportedTextures] = {};
  Rc<DxvkSampler> samplers[kMaxSupportedTextures] = {};
  static_assert(kInvalidResourceSlot == 0 && "Below initialization of all array members is only valid for a value of 0.");
  uint32_t colorTextureSlot[kMaxSupportedTextures] = { kInvalidResourceSlot };

  XXH64_hash_t m_cachedHash = kEmptyHash;
  XXH64_hash_t m_pixelShaderHashForMaterialInstance = kEmptyHash;
  XXH64_hash_t m_materialTextureSetHash = kEmptyHash;
  XXH64_hash_t m_pixelShaderConstantsHashForMaterialInstance = kEmptyHash;
  // Derived in updateCachedHash: PS bytecode + material texture set (no constants).
  XXH64_hash_t m_textureSetShaderHash = kEmptyHash;
};

struct MaterialData {
  bool m_ignored = false;
  // Runtime-only bridge state. This never appears in authored material data.
  bool m_useSecondaryTextureForOpacity = false;
  bool m_kenshiTerrainBlend = false;
  bool m_kenshiCharacterHead = false;
  TextureRef m_kenshiCharacterBodyMaskTexture = {};
  TextureRef m_kenshiCharacterHeadMaskTexture = {};
  TextureRef m_kenshiCharacterHairTexture = {};
  TextureRef m_kenshiCharacterBeardTexture = {};
  // Kenshi's separate head normal map (character.hlsl s6).
  TextureRef m_kenshiCharacterHeadNormalTexture = {};
  // Kenshi's muscle-blend normal map (character.hlsl s4). Travels to the GPU in heightTextureIndex (see
  // writeGPUData).
  TextureRef m_kenshiMuscleBlendTexture = {};
  // Bits 0-2 hair colour channel, 3-5 hair alpha, 6-8 beard alpha, bit 9 the green flip, bits 10-15 the
  // muscle blend in 1/63 steps.
  uint16_t m_kenshiCharacterHairChannels = 0u;
  uint32_t m_kenshiTerrainSetIndex = 0u;
  // 0 = octahedral (Remix default), 1 = Kenshi RGB, 2 = DXT5nm.
  uint32_t m_kenshiNormalEncoding = 0u;
  // See OPAQUE_SURFACE_MATERIAL_FLAG_KENSHI_GLOSS_IN_ALPHA.
  bool m_kenshiGlossInAlpha = false;
  // See OPAQUE_SURFACE_MATERIAL_FLAG_KENSHI_DUAL_TEXTURE_SET.
  bool m_kenshiDualTextureSet = false;
  // See LegacyMaterialData::kenshiDualNormalTexture.
  TextureRef m_kenshiDualNormalTexture = {};
  TextureRef m_kenshiDualMetalTexture = {};
  // See OPAQUE_SURFACE_MATERIAL_FLAG_KENSHI_COLOR_MASK.
  bool m_kenshiColorMask = false;
  Vector4 m_kenshiColor1 = Vector4(0.0f, 0.0f, 0.0f, 0.0f);
  Vector4 m_kenshiColor2 = Vector4(0.0f, 0.0f, 0.0f, 0.0f);
  TextureRef m_kenshiColorMaskTexture = {};
  // See OPAQUE_SURFACE_MATERIAL_FLAG_KENSHI_CHARACTER_VEST.
  bool m_kenshiCharacterVest = false;
  TextureRef m_kenshiVestDiffuseTexture = {};
  TextureRef m_kenshiVestNormalTexture = {};
  TextureRef m_kenshiVestMaskTexture = {};
  Vector4 m_kenshiVestColor = Vector4(1.0f, 1.0f, 1.0f, 1.0f);
  // The under-construction scaffold payload. Whether a draw is under construction is a surface flag
  // (flags0 bit 5); only the texture and the three scalars ride on the material.
  bool m_kenshiConstruction = false;
  TextureRef m_kenshiConstructionGridTexture = {};
  Vector4 m_kenshiConstructionParams = Vector4(0.0f, 0.0f, 0.0f, 0.0f);
  // See LegacyMaterialData::kenshiDustNoiseTexture.
  TextureRef m_kenshiDustNoiseTexture = {};
  // See LegacyMaterialData::kenshiDustColour.
  Vector4 m_kenshiDustColour = Vector4(0.0f, 0.0f, 0.0f, 0.0f);

  using MaterialVariant = std::variant<
    OpaqueMaterialData,
    TranslucentMaterialData,
    RayPortalMaterialData
  >;

  // Using variants rather than a union here, due to the MaterialData containing nested members of Rc pointers.
  MaterialVariant m_data;

  std::optional<RtxParticleSystemDesc> m_particleSystem;

  // Verify that the variant and enum stay in sync
  static_assert(std::variant_size_v<MaterialVariant> == (size_t)MaterialDataType::Count, "Enum is out of sync, please check your change.");
  static_assert(std::is_same_v<std::variant_alternative_t<(size_t)MaterialDataType::Opaque,      MaterialVariant>, OpaqueMaterialData>,      "MaterialVariant[Opaque] must be OpaqueMaterialData, please check your change.");
  static_assert(std::is_same_v<std::variant_alternative_t<(size_t)MaterialDataType::Translucent, MaterialVariant>, TranslucentMaterialData>, "MaterialVariant[Translucent] must be TranslucentMaterialData, please check your change.");
  static_assert(std::is_same_v<std::variant_alternative_t<(size_t)MaterialDataType::RayPortal,   MaterialVariant>, RayPortalMaterialData>,   "MaterialVariant[RayPortal] must be RayPortalMaterialData, please check your change.");

  MaterialData(const OpaqueMaterialData& opaque, std::optional<RtxParticleSystemDesc> particleSystem = std::nullopt, bool ignored = false)
    : m_ignored { ignored }, m_data { opaque }, m_particleSystem { particleSystem } {}

  MaterialData(const TranslucentMaterialData& translucent, std::optional<RtxParticleSystemDesc> particleSystem = std::nullopt, bool ignored = false)
    : m_ignored { ignored }, m_data { translucent }, m_particleSystem { particleSystem } {}

  MaterialData(const RayPortalMaterialData& portal, std::optional<RtxParticleSystemDesc> particleSystem = std::nullopt)
    : m_data { portal }, m_particleSystem { particleSystem } { }

  bool getIgnored() const {
    return m_ignored;
  }

  bool getUseSecondaryTextureForOpacity() const {
    return m_useSecondaryTextureForOpacity;
  }

  bool getKenshiTerrainBlend() const {
    return m_kenshiTerrainBlend;
  }

  bool getKenshiCharacterHead() const {
    return m_kenshiCharacterHead;
  }

  const TextureRef& getKenshiCharacterBodyMaskTexture() const {
    return m_kenshiCharacterBodyMaskTexture;
  }

  const TextureRef& getKenshiCharacterHeadMaskTexture() const {
    return m_kenshiCharacterHeadMaskTexture;
  }

  const TextureRef& getKenshiCharacterHairTexture() const {
    return m_kenshiCharacterHairTexture;
  }

  const TextureRef& getKenshiCharacterBeardTexture() const {
    return m_kenshiCharacterBeardTexture;
  }

  // Kenshi's separate head normal map.
  const TextureRef& getKenshiCharacterHeadNormalTexture() const {
    return m_kenshiCharacterHeadNormalTexture;
  }

  void setKenshiCharacterHeadNormalTexture(const TextureRef& value) {
    m_kenshiCharacterHeadNormalTexture = value;
  }

  // Kenshi's muscle-blend normal map.
  const TextureRef& getKenshiMuscleBlendTexture() const {
    return m_kenshiMuscleBlendTexture;
  }

  void setKenshiMuscleBlendTexture(const TextureRef& value) {
    m_kenshiMuscleBlendTexture = value;
  }

  uint16_t getKenshiCharacterHairChannels() const {
    return m_kenshiCharacterHairChannels;
  }

  // 0 = octahedral (Remix default), 1 = Kenshi RGB, 2 = DXT5nm.
  uint32_t getKenshiNormalEncoding() const { return m_kenshiNormalEncoding; }
  bool getKenshiGlossInAlpha() const { return m_kenshiGlossInAlpha; }
  bool getKenshiDualTextureSet() const { return m_kenshiDualTextureSet; }
  const TextureRef& getKenshiDualNormalTexture() const { return m_kenshiDualNormalTexture; }
  const TextureRef& getKenshiDualMetalTexture() const { return m_kenshiDualMetalTexture; }
  bool getKenshiColorMask() const { return m_kenshiColorMask; }
  const Vector4& getKenshiColor1() const { return m_kenshiColor1; }
  const Vector4& getKenshiColor2() const { return m_kenshiColor2; }
  const TextureRef& getKenshiColorMaskTexture() const { return m_kenshiColorMaskTexture; }
  bool getKenshiCharacterVest() const { return m_kenshiCharacterVest; }
  const TextureRef& getKenshiVestDiffuseTexture() const { return m_kenshiVestDiffuseTexture; }
  const TextureRef& getKenshiVestNormalTexture() const { return m_kenshiVestNormalTexture; }
  const TextureRef& getKenshiVestMaskTexture() const { return m_kenshiVestMaskTexture; }
  const Vector4& getKenshiVestColor() const { return m_kenshiVestColor; }
  bool getKenshiConstruction() const { return m_kenshiConstruction; }
  const TextureRef& getKenshiConstructionGridTexture() const { return m_kenshiConstructionGridTexture; }
  const Vector4& getKenshiConstructionParams() const { return m_kenshiConstructionParams; }
  const TextureRef& getKenshiDustNoiseTexture() const { return m_kenshiDustNoiseTexture; }
  const Vector4& getKenshiDustColour() const { return m_kenshiDustColour; }
  void setKenshiTerrainBlend(bool value) {
    m_kenshiTerrainBlend = value;
  }

  void setKenshiCharacterHead(bool value) {
    m_kenshiCharacterHead = value;
  }

  void setKenshiCharacterBodyMaskTexture(const TextureRef& value) {
    m_kenshiCharacterBodyMaskTexture = value;
  }

  void setKenshiCharacterHeadMaskTexture(const TextureRef& value) {
    m_kenshiCharacterHeadMaskTexture = value;
  }

  void setKenshiCharacterHairTexture(const TextureRef& value) {
    m_kenshiCharacterHairTexture = value;
  }

  void setKenshiCharacterBeardTexture(const TextureRef& value) {
    m_kenshiCharacterBeardTexture = value;
  }

  void setKenshiCharacterHairChannels(uint16_t value) {
    m_kenshiCharacterHairChannels = value;
  }

  void setKenshiNormalEncoding(uint32_t value) { m_kenshiNormalEncoding = value; }
  void setKenshiGlossInAlpha(bool value) { m_kenshiGlossInAlpha = value; }
  void setKenshiDualTextureSet(bool value) { m_kenshiDualTextureSet = value; }
  void setKenshiDualNormalTexture(const TextureRef& t) { m_kenshiDualNormalTexture = t; }
  void setKenshiDualMetalTexture(const TextureRef& t) { m_kenshiDualMetalTexture = t; }
  void setKenshiColorMask(bool value) { m_kenshiColorMask = value; }
  void setKenshiColor1(const Vector4& v) { m_kenshiColor1 = v; }
  void setKenshiColor2(const Vector4& v) { m_kenshiColor2 = v; }
  void setKenshiColorMaskTexture(const TextureRef& t) { m_kenshiColorMaskTexture = t; }
  void setKenshiCharacterVest(bool v) { m_kenshiCharacterVest = v; }
  void setKenshiVestDiffuseTexture(const TextureRef& t) { m_kenshiVestDiffuseTexture = t; }
  void setKenshiVestNormalTexture(const TextureRef& t) { m_kenshiVestNormalTexture = t; }
  void setKenshiVestMaskTexture(const TextureRef& t) { m_kenshiVestMaskTexture = t; }
  void setKenshiVestColor(const Vector4& v) { m_kenshiVestColor = v; }
  void setKenshiConstruction(bool v) { m_kenshiConstruction = v; }
  void setKenshiConstructionGridTexture(const TextureRef& t) { m_kenshiConstructionGridTexture = t; }
  void setKenshiConstructionParams(const Vector4& v) { m_kenshiConstructionParams = v; }
  void setKenshiDustNoiseTexture(const TextureRef& t) { m_kenshiDustNoiseTexture = t; }
  void setKenshiDustColour(const Vector4& v) { m_kenshiDustColour = v; }
  uint32_t getKenshiTerrainSetIndex() const {
    return m_kenshiTerrainSetIndex;
  }

  void setKenshiTerrainSetIndex(uint32_t value) {
    m_kenshiTerrainSetIndex = value;
  }

  void setUseSecondaryTextureForOpacity(bool value) {
    m_useSecondaryTextureForOpacity = value;
  }

  MaterialDataType getType() const {
    // NOTE: relies on the variant index matching MaterialDataType
    return static_cast<MaterialDataType>(m_data.index());
  }

  XXH64_hash_t getHash() const {
    return std::visit([](auto const& mat) { return mat.getHash(); }, m_data);
  }

  template<typename F>
  void forEachTexture(F&& fn) const {
    std::visit([&](auto const& mat) { mat.forEachTexture(fn); }, m_data);
  }

  const Rc<DxvkSampler>& getSamplerOverride() const {
    return std::visit([](auto const& mat) -> const Rc<DxvkSampler>& { return mat.getSamplerOverride(); }, m_data);
  }

  const OpaqueMaterialData& getOpaqueMaterialData() const {
    assert(std::holds_alternative<OpaqueMaterialData>(m_data));
    return std::get<OpaqueMaterialData>(m_data);
  }

  OpaqueMaterialData& getOpaqueMaterialData() {
    assert(std::holds_alternative<OpaqueMaterialData>(m_data));
    return std::get<OpaqueMaterialData>(m_data);
  }

  const TranslucentMaterialData& getTranslucentMaterialData() const {
    assert(std::holds_alternative<TranslucentMaterialData>(m_data));
    return std::get<TranslucentMaterialData>(m_data);
  }

  TranslucentMaterialData& getTranslucentMaterialData() {
    assert(std::holds_alternative<TranslucentMaterialData>(m_data));
    return std::get<TranslucentMaterialData>(m_data);
  }

  const RayPortalMaterialData& getRayPortalMaterialData() const {
    assert(std::holds_alternative<RayPortalMaterialData>(m_data));
    return std::get<RayPortalMaterialData>(m_data);
  }

  RayPortalMaterialData& getRayPortalMaterialData() {
    assert(std::holds_alternative<RayPortalMaterialData>(m_data));
    return std::get<RayPortalMaterialData>(m_data);
  }

  const RtxParticleSystemDesc* getParticleSystemDesc() const {
    return m_particleSystem.has_value() ? &m_particleSystem.value() : nullptr;
  }
  
  void getSpriteSheetData(uint8_t& spriteSheetRows, uint8_t& spriteSheetCols, uint8_t& spriteSheetFPS) const {
    // Note: Extract spritesheet information from the associated material data as it ends up stored in the Surface
    // not in the Surface Material like most material information.
    switch (getType()) {
    case MaterialDataType::Opaque:
      spriteSheetRows = getOpaqueMaterialData().getSpriteSheetRows();
      spriteSheetCols = getOpaqueMaterialData().getSpriteSheetCols();
      spriteSheetFPS = getOpaqueMaterialData().getSpriteSheetFPS();

      break;
    case MaterialDataType::Translucent:
      spriteSheetRows = getTranslucentMaterialData().getSpriteSheetRows();
      spriteSheetCols = getTranslucentMaterialData().getSpriteSheetCols();
      spriteSheetFPS = getTranslucentMaterialData().getSpriteSheetFPS();

      break;
    case MaterialDataType::RayPortal:
      spriteSheetRows = getRayPortalMaterialData().getSpriteSheetRows();
      spriteSheetCols = getRayPortalMaterialData().getSpriteSheetCols();
      spriteSheetFPS = getRayPortalMaterialData().getSpriteSheetFPS();

      break;
    case MaterialDataType::Count:
    case MaterialDataType::Invalid:
      assert(0);
      break;
    }
  }
  
  void setSpriteSheetData(uint8_t spriteSheetRows, uint8_t spriteSheetCols, uint8_t spriteSheetFPS) {
    switch (getType()) {
    case MaterialDataType::Opaque:
       getOpaqueMaterialData().setSpriteSheetRows(spriteSheetRows);
       getOpaqueMaterialData().setSpriteSheetCols(spriteSheetCols);
       getOpaqueMaterialData().setSpriteSheetFPS(spriteSheetFPS);

      break;
    case MaterialDataType::Translucent:
      getTranslucentMaterialData().setSpriteSheetRows(spriteSheetRows);
      getTranslucentMaterialData().setSpriteSheetCols(spriteSheetCols);
      getTranslucentMaterialData().setSpriteSheetFPS(spriteSheetFPS);

      break;
    case MaterialDataType::RayPortal:
      getRayPortalMaterialData().setSpriteSheetRows(spriteSheetRows);
      getRayPortalMaterialData().setSpriteSheetCols(spriteSheetCols);
      getRayPortalMaterialData().setSpriteSheetFPS(spriteSheetFPS);

      break;
    case MaterialDataType::Count:
    case MaterialDataType::Invalid:
      assert(0);
      break;
    }
  }

  void mergeLegacyMaterial(const LegacyMaterialData& input) {
    std::visit([&](auto& mat) {
      using T = std::decay_t<decltype(mat)>;
      if constexpr (std::is_same_v<T, OpaqueMaterialData>) {
        OpaqueMaterialData tmp;
        tmp.getAlbedoOpacityTexture() = input.getColorTexture();
        if (auto s = input.getSampler().ptr()) {
          tmp.getFilterMode() = lss::Mdl::Filter::vkToMdl(s->info().magFilter);
          tmp.getWrapModeU() = lss::Mdl::WrapMode::vkToMdl(s->info().addressModeU);
          tmp.getWrapModeV() = lss::Mdl::WrapMode::vkToMdl(s->info().addressModeV);
        }
        mat.merge(tmp);
      } else if constexpr (std::is_same_v<T, TranslucentMaterialData>) {
        TranslucentMaterialData tmp;
        if (auto s = input.getSampler().ptr()) {
          tmp.getFilterMode() = lss::Mdl::Filter::vkToMdl(s->info().magFilter);
          tmp.getWrapModeU() = lss::Mdl::WrapMode::vkToMdl(s->info().addressModeU);
          tmp.getWrapModeV() = lss::Mdl::WrapMode::vkToMdl(s->info().addressModeV);
        }
        mat.merge(tmp);
      } else { 
        RayPortalMaterialData tmp;
        tmp.getMaskTexture() = input.getColorTexture();
        tmp.getMaskTexture2() = input.getColorTexture2();
        if (auto s = input.getSampler().ptr()) {
          tmp.getFilterMode() = lss::Mdl::Filter::vkToMdl(s->info().magFilter);
          tmp.getWrapModeU() = lss::Mdl::WrapMode::vkToMdl(s->info().addressModeU);
          tmp.getWrapModeV() = lss::Mdl::WrapMode::vkToMdl(s->info().addressModeV);
        }
        mat.merge(tmp);
      }
    }, m_data);
  }

#define POPULATE_SAMPLER_INFO(info, material) \
  info.magFilter = \
    lss::Mdl::Filter::mdlToVk(material.getFilterMode()); \
  info.minFilter = \
    lss::Mdl::Filter::mdlToVk(material.getFilterMode()); \
  info.addressModeU = \
    lss::Mdl::WrapMode::mdlToVk(material.getWrapModeU(), &info.borderColor); \
  info.addressModeV = \
    lss::Mdl::WrapMode::mdlToVk(material.getWrapModeV(), &info.borderColor);

  void populateSamplerInfo(DxvkSamplerCreateInfo& toPopulate) const {
    std::visit([&](auto const& mat) { POPULATE_SAMPLER_INFO(toPopulate, mat); }, m_data);
  }
#undef POPULATE_SAMPLER_INFO
};

} // namespace dxvk
