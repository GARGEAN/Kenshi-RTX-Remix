/*
* Copyright (c) 2022, NVIDIA CORPORATION. All rights reserved.
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

#include "rtx_materials.h"

#include <algorithm>

#include "rtx_options.h"
#include "rtx_kenshi_options.h"

namespace dxvk {

bool getEnableDiffuseLayerOverrideHack() {
  return TranslucentMaterialOptions::enableDiffuseLayerOverride();
}

float getEmissiveIntensity() {
  return RtxOptions::emissiveIntensity();
}

float getDisplacementFactor() {
  return RtxOptions::Displacement::displacementFactor();
}

float getDisplacementInFactor() {
  return RtxOptions::Displacement::displacementFactor() * RtxOptions::Displacement::displacementInFactor();
}

float getDisplacementOutFactor() {
  return RtxOptions::Displacement::displacementFactor() * RtxOptions::Displacement::displacementOutFactor();
}

dxvk::OpaqueMaterialData LegacyMaterialData::createDefault() {
  OpaqueMaterialData opaqueMat;
  opaqueMat.setAnisotropyConstant(LegacyMaterialDefaults::anisotropy());
  opaqueMat.setEmissiveIntensity(LegacyMaterialDefaults::emissiveIntensity());
  opaqueMat.setAlbedoConstant(LegacyMaterialDefaults::albedoConstant());
  opaqueMat.setOpacityConstant(LegacyMaterialDefaults::opacityConstant());
  opaqueMat.setRoughnessConstant(LegacyMaterialDefaults::roughnessConstant());
  opaqueMat.setMetallicConstant(LegacyMaterialDefaults::metallicConstant());
  opaqueMat.setEmissiveColorConstant(LegacyMaterialDefaults::emissiveColorConstant());
  opaqueMat.setEnableEmission(LegacyMaterialDefaults::enableEmissive());
  opaqueMat.setEnableThinFilm(LegacyMaterialDefaults::enableThinFilm());
  opaqueMat.setAlphaIsThinFilmThickness(LegacyMaterialDefaults::alphaIsThinFilmThickness());
  opaqueMat.setThinFilmThicknessConstant(LegacyMaterialDefaults::thinFilmThicknessConstant());
  return opaqueMat;
}

template<> OpaqueMaterialData LegacyMaterialData::as() const {
  // Legacy materials have parameters that can directly carry over onto the opaque material.
  const OpaqueMaterialData defaultLegacyOpaqueMaterial = createDefault();
  // Copy off the defaults, and make dynamic adjustments for the remaining params from this legacy material
  OpaqueMaterialData opaqueMat(defaultLegacyOpaqueMaterial);
  if (LegacyMaterialDefaults::useAlbedoTextureIfPresent()) {
    opaqueMat.setAlbedoOpacityTexture(getColorTexture());
  }
  // Constant-color materials carry their color in shader constant registers rather than a
  // texture; without this they render plain white. Opacity stays at the default - the
  // vector's w component rarely holds opacity.
  if (hasConstantAlbedo && !getColorTexture().isValid()) {
    const Vector3 clampedAlbedo(
      std::clamp(constantAlbedo.x, 0.0f, 1.0f),
      std::clamp(constantAlbedo.y, 0.0f, 1.0f),
      std::clamp(constantAlbedo.z, 0.0f, 1.0f));
    opaqueMat.setAlbedoConstant(clampedAlbedo);
  }
  if (getColorTexture2().isValid()) {
    opaqueMat.setSecondaryTexture(getColorTexture2());
  }

  // Feed the game's own normal map into the generic normal slot. A USD replacement with its own normal
  // map still wins; replacements are not built through this conversion.
  if (kenshiNormalTexture.isValid()) {
    opaqueMat.setNormalTexture(kenshiNormalTexture);
    // The encoding cannot be set here: OpaqueMaterialData's fields are X-macro generated from the USD
    // parameter list, so the Kenshi flags live on MaterialData and are applied right after this conversion
    // in rtx_scene_manager.cpp, beside setKenshiTerrainBlend.
  }

  // Kenshi's metal map goes straight into Remix's metallic slot: the game writes its red channel into
  // its G-buffer metal channel and both renderers do the same lerp(0.04, albedo, metal) F0 split.
  if (kenshiMetalTexture.isValid() && KenshiOptions::kenshiGameMetalness()) {
    opaqueMat.setMetallicTexture(kenshiMetalTexture);
  }
  // The gloss multiplier rides in roughnessConstant - only while the feature is on; otherwise the field
  // keeps its ordinary meaning (a glossMult of 0.1 would become a near-mirror roughness).
  if (kenshiGlossMult > 0.0f && KenshiOptions::kenshiObjectGlossMultiplier() > 0.0f) {
    opaqueMat.setRoughnessConstant(kenshiGlossMult);
  }

  // A direct perceptual roughness (water), as opposed to the gloss multiplier above. Power is a
  // fixed-function field the bridge otherwise never uses, which keeps kenshiGlossMult zero for water and
  // so the gloss-in-alpha flag off. Not gated on kenshiObjectGlossMultiplier: water's roughness is its
  // own control.
  if (dx11Material.Power > 0.0f) {
    opaqueMat.setRoughnessConstant(std::clamp(dx11Material.Power, 0.0f, 1.0f));
  }

  // Indicate that we have an exact sampler to use on this material, directly from game
  if (getSampler().ptr()) {
    opaqueMat.setSamplerOverride(getSampler());
  }
  // Ignore colormap alpha of legacy texture if tagged as 'ignoreAlphaOnTextures'.
  // Also for water (Power > 0): watercolourmap.png has zero alpha everywhere - the water pixel shader
  // computes its own alpha - and taken as opacity it would make a blended water draw invisible.
  bool ignoreAlphaChannel = LegacyMaterialDefaults::ignoreAlphaChannel()
    || dx11Material.Power > 0.0f;
  if (!ignoreAlphaChannel) {
    ignoreAlphaChannel = lookupHash(RtxOptions::ignoreAlphaOnTextures(), getHash());
  }
  opaqueMat.setIgnoreAlphaChannel(ignoreAlphaChannel);
  return opaqueMat;
}

template<> TranslucentMaterialData LegacyMaterialData::as() const {
  TranslucentMaterialData transluscentMat;
  if (getSampler().ptr()) {
    transluscentMat.setSamplerOverride(getSampler());
  }
  return transluscentMat;
}

template<> RayPortalMaterialData LegacyMaterialData::as() const {
  RayPortalMaterialData portalMat;
  portalMat.getMaskTexture() = getColorTexture();
  portalMat.getMaskTexture2() = getColorTexture2();
  portalMat.setEnableEmission(true);
  portalMat.setEmissiveIntensity(1.f);
  portalMat.setSpriteSheetCols(1);
  portalMat.setSpriteSheetRows(1);
  if (getSampler().ptr()) {
    portalMat.setSamplerOverride(getSampler());
  }
  return portalMat;
}


} // namespace dxvk
