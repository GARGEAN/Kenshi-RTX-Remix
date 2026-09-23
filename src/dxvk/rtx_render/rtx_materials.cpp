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

  // DX11_V488_KENSHI_NORMAL_MAPS: feed the game's own normal map into the
  // generic normal slot. A USD replacement that supplies its own normal map
  // still wins, because a replacement is not built through this conversion.
  if (kenshiNormalTexture.isValid()) {
    opaqueMat.setNormalTexture(kenshiNormalTexture);
    // DX11_V490: the ENCODING cannot be set here. OpaqueMaterialData's fields
    // are X-macro generated from the USD parameter list, so a non-USD field has
    // no home on it; the Kenshi flags all live on MaterialData instead and are
    // applied right after this conversion in rtx_scene_manager.cpp (~1469),
    // beside setKenshiTerrainBlend.
  }

  // DX11_V533_KENSHI_OBJECT_GLOSS: Kenshi's metal map goes straight into Remix's
  // metallic slot. The game writes its red channel into its own G-buffer metal
  // channel and both renderers then do the same lerp(0.04, albedo, metal) F0
  // split, so no curve is involved. Without it every Kenshi material sits on the
  // flat rtx.legacyMaterial.metallicConstant and no metal in the game reads as
  // metal.
  if (kenshiMetalTexture.isValid() && KenshiOptions::kenshiGameMetalness()) {
    opaqueMat.setMetallicTexture(kenshiMetalTexture);
  }
  // The gloss MULTIPLIER rides in roughnessConstant.
  //
  // DX11_V534: gated on the feature actually being ON. In V533 this ran
  // unconditionally, so every gloss-family material had its base roughness
  // replaced by its glossMult even at strength 0 - a material with
  // `glossMult = 0.1` became roughness 0.1, i.e. near-mirror, no matter where
  // the slider sat. That was the handful of props that came out glossy at every
  // setting. With the feature off the field keeps its ordinary meaning.
  if (kenshiGlossMult > 0.0f && KenshiOptions::kenshiObjectGlossMultiplier() > 0.0f) {
    opaqueMat.setRoughnessConstant(kenshiGlossMult);
  }

  // DX11_V585_KENSHI_WATER_ROUGHNESS_CHANNEL: a DIRECT perceptual roughness,
  // as opposed to the gloss multiplier above. Kenshi's water has no gloss in
  // its diffuse alpha, so the object-gloss path would read a meaningless alpha
  // and invert the value (see the note at the write site in d3d11_rtx.cpp).
  // `Power` is a fixed-function field this bridge never otherwise uses, and
  // going through it keeps `kenshiGlossMult` zero for water, which is what
  // keeps the gloss-in-alpha flag off.
  //
  // Deliberately NOT gated on kenshiObjectGlossMultiplier: water's roughness is
  // its own control and has nothing to do with the object-gloss feature.
  if (dx11Material.Power > 0.0f) {
    opaqueMat.setRoughnessConstant(std::clamp(dx11Material.Power, 0.0f, 1.0f));
  }

  // Indicate that we have an exact sampler to use on this material, directly from game
  if (getSampler().ptr()) {
    opaqueMat.setSamplerOverride(getSampler());
  }
  // Ignore colormap alpha of legacy texture if tagged as 'ignoreAlphaOnTextures' 
  // DX11_V588_KENSHI_WATER_OPACITY: watercolourmap.png is RGBA with alpha 0 in
  // every texel - the game never reads it, because the water pixel shader
  // computes its own alpha from Fresnel and scene depth. Taken as opacity it
  // makes any BLENDED water draw perfectly invisible, which is what happened to
  // the near-water patches the moment they were admitted. The whole-map grid was
  // unaffected only because it is not blended, so its alpha is ignored already.
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
