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
#ifndef RTX_PASS_RAYTRACE_ARGS_H_DX11V225
#define RTX_PASS_RAYTRACE_ARGS_H_DX11V225 // DX11_V225_GUARD

#include "rtx/utility/shader_types.h"
#ifdef __cplusplus
#include "rtx/concept/camera/camera.h"
#include "rtx/concept/ray_portal/ray_portal.h"
#else
#include "rtx/concept/camera/camera.slangh"
#include "rtx/concept/ray_portal/ray_portal.slangh"
#endif

#include "rtx/pass/nrd_args.h"
#include "rtx/pass/nrc_args.h"
#include "rtx/pass/volume_args.h"
#include "rtx/pass/material_args.h"
#include "rtx/pass/view_distance_args.h"
#include "rtx/pass/atmosphere/atmosphere_args.h"
#include "rtx/concept/light/light_types.h"
#include "rtx/concept/surface/surface_shared.h"
#include "rtx/algorithm/nee_cache_data.h"

struct LightRangeInfo {
  uint offset;
  uint count;
  uint16_t rtxdiSampleCount;
  uint16_t volumeRISSampleCount;
  uint16_t risSampleCount;
  uint16_t pad;
};

// Note: ensure 16B alignment
struct TerrainArgs {
  uint2 cascadeMapSize;    // Number of cascade tiles in each dimension
  float2 rcpCascadeMapSize;

  uint maxCascadeLevel;
  float lastCascadeScale;
  float displaceIn;
  uint pad0;
};

// DX11_V396_KENSHI_TERRAIN: Kenshi blends its ground from a stack of detail
// layers keyed by a splat map, then multiplies the result by a low-frequency
// colour map. Only that colour map can reach an ordinary Remix material, which
// is why terrain renders correctly tinted but with no grain.
//
// The parameters live here rather than in a per-material buffer because the
// game uses ONE terrain material set per zone: every terrain draw measured in a
// frame resolved the same colour map image and the same constants, and the
// per-chunk variation is carried entirely by the interpolated texture
// coordinate. `set` is 0 when no terrain draw has been seen, which is what the
// material flag is checked against.
//
// The detail coordinate is derived from the overlay coordinate the surface
// already interpolates, because both are affine in the same object-space
// position:
//
//   objectPos.xz  = uv * rectSize + rectMin
//   detailUv      = objectPos.xz * 0.0002 * layerScale
//                 = uv * detailScale + detailOffset
// DX11_V398_KENSHI_TERRAIN_PER_MATERIAL: Kenshi draws terrain with more than
// one parameter set at a time - the ground and the terrain-blended rock
// formations are separate families with their own rects, and neighbouring
// biomes bring their own sets again. A single global set made whichever drew
// last win, which showed up as the ground swapping texture sets with view
// angle and as the rocks sampling their detail through the ground's rect.
//
// Sets are content-addressed and stable across frames, so a surface material
// can store its index once (in secondaryTextureIndex, which terrain does not
// otherwise use) and keep it while it is cached. They live in their own
// structured buffer because the count is unbounded - see
// BINDING_KENSHI_TERRAIN_BUFFER.

struct KenshiTerrainArgs {
  // uv -> objectPos.xz * 0.0002, the base every layer scales from.
  float2 detailScale;
  float2 detailOffset;

  // Per-layer tiling (scalesB.xy, scalesA.xy, scalesA.zw, scalesB.zw,
  // scalesC.xy, scalesC.zw for layers 0..5). Layer 2 is the cliff layer and
  // uses its pair for both of its projections.
  float2 layerScale0;
  float2 layerScale1;
  float2 layerScale2;
  float2 layerScale3;
  float2 layerScale4;
  float2 layerScale5;

  // .xy only: band x drives layer 1, band y drives the cliff layer.
  float4 slopeMin;
  float4 slopeMax;
  float4 slopeBlend;

  // Per-layer strength of the colour map's tint, for layers 2,3,4,5.
  float4 overlayMult;

  uint layerTexture0;
  uint layerTexture1;
  uint layerTexture2;
  uint layerTexture3;

  uint layerTexture4;
  uint layerTexture5;
  uint overlayTexture;
  uint set;

  float brightnessFix;
  // objectPos.y = worldPos.y - objectHeightOffset, for the cliff projection.
  float objectHeightOffset;
  uint pad0;
  // DX11_V528_KENSHI_TERRAIN_BIOME_BLEND: active blendMap channels, bits 0..3
  // for x..w. Zero on every record that is not a biome-boundary primary, which
  // is what keeps the selector inert for ordinary terrain and for every
  // non-terrain material that lands in this buffer by index aliasing.
  uint blendMask;

  // detailBase -> blendMap UV, the affine raster builds from `biomeData`.
  float2 biomeScale;
  float2 biomeOffset;
  // blendMap bindless index, or kSurfaceMaterialInvalidTextureIndex.
  uint blendTexture;
  // Record indices of the neighbouring biome sets, in the descending w,z,y,x
  // channel order the numbered diffuseMapsN arrays follow.
  uint blendSet0;
  uint blendSet1;
  uint blendSet2;
  // Native per-biome distant RGB and distance scale. Append to preserve offsets.
  float4 textureFade;
};

struct NeeCacheArgs {
  uint enable;
  uint enableImportanceSampling;
  uint enableMIS;
  uint enableOnFirstBounce;

  uint enableAnalyticalLight;
  float specularFactor;
  float uniformSamplingProbability;
  float cullingThreshold;

  NeeEnableMode enableModeAfterFirstBounce;
  float ageCullingSpeed;
  float emissiveTextureSampleFootprintScale;
  uint approximateParticleLighting;

  float resolution;
  float minRange;
  float learningRate;
  uint clearCache;

  float triangleExplorationRangeRatio;
  uint  triangleExplorationMaxRange;
  float triangleExplorationProbability;
  float triangleExplorationAcceptRangeRatio;

  uint padding;
  uint enableReshuffleResilience;
  uint reshuffleMaxAge;
  uint enableSpatialReuse;
};

struct DomeLightArgs {
  mat4 worldToLightTransform;

  vec3 radiance;
  uint active;

  uint3 pad0;
  uint textureIndex;
};

struct SssArgs {
  uint enableThinOpaque;
  uint enableDiffusionProfile;
  float diffusionProfileScale;
  u16vec2 diffusionProfileDebuggingPixel;
};

struct EyeArgs {
  uint  enableEyes;
  float normalBendingEyeball;
  float normalBendingCornea;
  float whitesAlbedoScale;

  float irisRadius;
  float irisDepth;
  uint  pad0;
  uint  pad1;
};

#define OBJECT_PICKING_INVALID (cb.clearColorPicking)

// Constant buffer
struct RaytraceArgs {
  // NOTE: this class should be kept as all structs, then all non-structs.  This is because the padding rules are different between C++ and shaders.
  Camera camera;

  // Note: Primary combined variant used in place of the primary direct denoiser when seperated direct/indirect
  // lighting is not used.
  NrdArgs primaryDirectNrd;
  NrdArgs primaryIndirectNrd;
  NrdArgs secondaryCombinedNrd;

  // Note: Not tightly packed, meaning these indices will align with the Ray Portal Index in the
  // Surface Material. Do note however due to elements being potentially "empty" each Ray Portal Hit Info
  // must be checked to be empty or not before usage. Additionally both Ray Portals in a pair will match
  // in state, either being present or not.
  // The first `maxRayPortalCount` portals are for this frame, the second `maxRayPortalCount` are for the previous frame.
  RayPortalHitInfo rayPortalHitInfos[maxRayPortalCount * 2];

  VolumeArgs volumeArgs;
  OpaqueMaterialArgs opaqueMaterialArgs;
  TranslucentMaterialArgs translucentMaterialArgs;
  ViewDistanceArgs viewDistanceArgs;

  LightRangeInfo lightRanges[lightTypeCount];

  TerrainArgs terrainArgs;
  NeeCacheArgs neeCacheArgs;
  DomeLightArgs domeLightArgs;
  NrcArgs nrcArgs;
  SssArgs sssArgs;
  AtmosphereArgs atmosphereArgs;
  EyeArgs eyeArgs;

  Camera renderTargetCamera;

  // ------------------------- Structs above this line, non structs below this line -----------------------------------

  uint frameIdx;
  float ambientIntensity;
  uint16_t lightCount;
  uint16_t risTotalSampleCount;
  uint16_t volumeRISTotalSampleCount;
  uint16_t rtxdiTotalSampleCount;

  // The maximum probability of continuing a path when Russian Roulette is being used.
  RussianRouletteMode russianRouletteMode;
  float russianRouletteDistanceFactor;
  float russianRouletteDiffuseContinueProbability;
  float russianRouletteSpecularContinueProbability;

  float russianRouletteMaxContinueProbability;
  float russianRoulette1stBounceMinContinueProbability;
  float russianRoulette1stBounceMaxContinueProbability;
  float fireflyFilteringLuminanceThreshold;

  // The minimum number of indirect bounces the path must complete before Russian Roulette can be used. Must be < 16.
  uint8_t pathMinBounces;
  // The maximum number of indirect bounces the path will be allowed to complete. Must be < 16.
  uint8_t pathMaxBounces;
  // The number of samples to clamp temporal reservoirs to. Note this is not the same as RTXDI's history length as it is not scaled
  // by the number of samples the current reservoir performs (due to variability in how many actual current reservoir samples are done).
  uint16_t volumeTemporalReuseMaxSampleCount;
  // The maximum number of resolve interactions for primary (geometry resolver) rays.
  uint8_t primaryRayMaxInteractions;
  // The maximum number of resolve interactions for PSR (geometry resolver) rays.
  uint8_t psrRayMaxInteractions;
  // The maximum number of resolve interactions for secondary (integrator) rays.
  uint8_t secondaryRayMaxInteractions;
  // The number of active Ray Portals (Used for Ray Portal sampling). Always <= RAY_PORTAL_MAX_COUNT
  uint8_t numActiveRayPortals;
  float secondarySpecularFireflyFilteringThreshold;
  uint  outputParticleLayer;

  // Note: Packed as float16, uses uint16_t due to being shared on C++ side
  uint16_t emissiveBlendOverrideEmissiveIntensity;
  // The maximum number of bounces to evaluate reflection PSR over.
  uint8_t psrrMaxBounces;
  // The maximum number of bounces to evaluate transmission PSR over.
  uint8_t pstrMaxBounces;
  float viewModelRayTMax;
  uint16_t particleSoftnessFactor;
  uint16_t emissiveIntensity;
  uint8_t rtxdiSpatialSamples;
  uint8_t rtxdiDisocclusionSamples;
  uint8_t rtxdiMaxHistoryLength;
  uint8_t virtualInstancePortalIndex; // portal space for which virtual view model or player model instances were generated for

  float indirectRaySpreadAngleFactor;
  // Half the angle of the cone spawned by each pixel to use for ray cone texture filtering.
  float screenSpacePixelSpreadHalfAngle;
  uint debugView;
  float primaryDirectMissLinearViewZ;


  vec4 debugKnob;     // For temporary tuning in shaders, has a dedicated UI widget.

  // Values to use on a ray miss
  vec3 clearColorNormal;
  float clearColorDepth;

  float2 upscaleFactor;   // Displayed(upscaled) / RT resolution
  uint32_t clearColorPicking;

  uint enableDLSSRR;
  uint setLogValueForDisocclusionMaskForDLSSRR;

  // NOTE: Variables need to be in groups of 4x32 bits above this comment.

  uint uniformRandomNumber;
  uint16_t opaqueDiffuseLobeSamplingProbabilityZeroThreshold;
  uint16_t minOpaqueDiffuseLobeSamplingProbability;
  uint16_t opaqueSpecularLobeSamplingProbabilityZeroThreshold;
  uint16_t minOpaqueSpecularLobeSamplingProbability;
  uint16_t opaqueOpacityTransmissionLobeSamplingProbabilityZeroThreshold;
  uint16_t minOpaqueOpacityTransmissionLobeSamplingProbability;
  uint16_t opaqueDiffuseTransmissionLobeSamplingProbabilityZeroThreshold;
  uint16_t minOpaqueDiffuseTransmissionLobeSamplingProbability;

  uint16_t translucentSpecularLobeSamplingProbabilityZeroThreshold;
  uint16_t minTranslucentSpecularLobeSamplingProbability;
  uint16_t translucentTransmissionLobeSamplingProbabilityZeroThreshold;
  uint16_t minTranslucentTransmissionLobeSamplingProbability;
  float roughnessDemodulationOffset;
  float timeSinceStartSeconds;

  uint enableCalculateVirtualShadingNormals;
  uint enableDirectLighting;
  uint enableEmissiveBlendEmissiveOverride;
  uint enablePortalFadeInEffect;
  uint enableRussianRoulette;
  uint enableSecondaryBounces;
  uint enableSeparateUnorderedApproximations;
  uint enableStochasticAlphaBlend;
  uint16_t enableDirectTranslucentShadows;
  uint16_t enableDirectAlphaBlendShadows;
  uint16_t enableIndirectTranslucentShadows;
  uint16_t enableIndirectAlphaBlendShadows;
  uint enableFirstBounceLobeProbabilityDithering;
  uint enableUnorderedResolveInIndirectRays;
  uint enableProbabilisticUnorderedResolveInIndirectRays;
  uint enableUnorderedEmissiveParticlesInIndirectRays;
  uint enableTransmissionApproximationInIndirectRays;
  uint enableDecalMaterialBlending;
  uint enableLegacyRectLightConeShaping;
  uint enableRectLightConeShapingRatioScaling;
  uint enableBillboardOrientationCorrection;
  uint enablePlayerModelInPrimarySpace;
  uint enablePlayerModelPrimaryShadows;
  uint enablePreviousTLAS;
  uint useIntersectionBillboardsOnPrimaryRays;

  uint enableRtxdi;
  uint enableRtxdiPermutationSampling;
  uint enableRtxdiRayTracedBiasCorrection;
  uint enableRtxdiSampleStealing;
  uint enableRtxdiStealBoundaryPixelSamplesWhenOutsideOfScreen;
  uint enableRtxdiCrossPortalLight;
  uint enableRtxdiTemporalBiasCorrection;
  uint enableRtxdiInitialVisibility;
  uint enableRtxdiTemporalReuse;
  uint enableRtxdiSpatialReuse;
  uint enableRtxdiDiscardInvisibleSamples;
  uint enableRtxdiDiscardEnlargedPixels;
  uint enableDirectLightBoilingFilter;
  uint enableRtxdiBestLightSampling;
  float directLightBoilingThreshold;
  float rtxdiDisocclusionFrames;

  uint enableDemodulateRoughness;
  uint enableHitTFiltering;
  uint enableReplaceDirectSpecularHitTWithIndirectSpecularHitT;
  uint enableSeparatedDenoisers;

  uint enableViewModelVirtualInstances;

  uint enablePSRR;
  uint enablePSTR;
  uint enablePSTROutgoingSplitApproximation;
  uint enablePSTRSecondaryIncidentSplitApproximation;
  float psrrNormalDetailThreshold;
  float pstrNormalDetailThreshold;

  uint enableEnhanceBSDFDetail;
  uint enhanceBSDFIndirectMode;
  float enhanceBSDFDirectLightPower;
  float enhanceBSDFIndirectLightPower;
  float enhanceBSDFDirectLightMaxValue;
  float enhanceBSDFIndirectLightMaxValue;
  float enhanceBSDFIndirectLightMinRoughness;

  uint startInMediumMaterialIndex;
  uint enableReSTIRGI;
  uint enableReSTIRGIFinalVisibility;
  uint enableReSTIRGIReflectionReprojection;
  float restirGIReflectionMinParallax;
  uint enableReSTIRGIVirtualSample;
  float reSTIRGIVirtualSampleLuminanceThreshold;
  float reSTIRGIVirtualSampleRoughnessThreshold;
  float reSTIRGIVirtualSampleSpecularThreshold;
  float reSTIRGIVirtualSampleMaxDistanceRatio;
  uint reSTIRGIMISMode;
  float reSTIRGIMISModePairwiseMISCentralWeight;
  uint enableReSTIRGIPermutationSampling;
  uint enableReSTIRGIDLSSRRCompatibilityMode;
  float reSTIRGIDLSSRRTemporalRandomizationRadius;
  uint enableReSTIRGISampleStealing;
  float reSTIRGISampleStealingJitter;
  uint enableReSTIRGIStealBoundaryPixelSamplesWhenOutsideOfScreen;
  uint enableReSTIRGISpatialReuse;
  uint enableReSTIRGITemporalReuse;
  uint reSTIRGIBiasCorrectionMode;
  uint enableReSTIRGIBoilingFilter;
  float boilingFilterLowerThreshold;
  float boilingFilterHigherThreshold;
  float boilingFilterRemoveReservoirThreshold;
  uint temporalHistoryLength;
  uint permutationSamplingSize;
  uint enableReSTIRGITemporalBiasCorrection;
  uint enableReSTIRGIDiscardEnlargedPixels;
  float reSTIRGIHistoryDiscardStrength;
  uint enableReSTIRGITemporalJacobian;
  float reSTIRGIFireflyThreshold;
  float reSTIRGIRoughnessClamp;
  float reSTIRGIMISRoughness;
  float reSTIRGIMISParallaxAmount;
  uint enableReSTIRGIDemodulatedTargetFunction;
  uint enableReSTIRGILightingValidation;
  uint enableReSTIRGIVisibilityValidation;
  float reSTIRGISampleValidationThreshold;
  float reSTIRGIVisibilityValidationRange;

  uint surfaceCount;
  uint teleportationPortalIndex; // 0 means no teleportation, 1+ means portal 0+

  float resolveTransparencyThreshold;
  float resolveOpaquenessThreshold;
  float resolveStochasticAlphaBlendThreshold;
  float translucentDecalAlbedoFactor;

  uint enableHeuristicSingleScatteringTransmission;

  float skyBrightness;
  uint skyMode;  // 0 = skybox rasterization, 1 = physical atmosphere

  uint isLastCompositeOutputValid;
  uint isZUp; // Note: Indicates if the Z axis is the "up" axis in world space if true, otherwise the Y axis if false.
  uint enableCullingSecondaryRays;

  u16vec2 gpuPrintThreadIndex;
  uint gpuPrintElementIndex;
  uint enableObjectPicking;

  DisplacementMode pomMode;
  uint pomEnableDirectLighting;
  uint pomEnableIndirectLighting;
  uint pomEnableNEECache;
  uint pomEnableReSTIRGI;
  uint pomEnablePSR;
  uint pomMaxIterations;
  uint enableSssTransmission;
  uint enableSssTransmissionSingleScattering;
  uint sssTransmissionBsdfSampleCount;
  uint sssTransmissionSingleScatteringSampleCount;
  uint enableTransmissionDiffusionProfileCorrection;
  float totalMipBias;

  uint forceFirstHitInGBufferPass;

  uint enableRaytracedRenderTarget;
  // NRC enablement is controlled by global macros being defined.
  // When macros are not used (i.e. in some passes) this variable controls the NRC enablement.
  uint enableNrc;

  // Debug override to disallow NRC training when it is enabled in the first place,
  // hence why it is not named enableNrcTraining here
  uint allowNrcTraining;

  float vertexColorStrength;
  float alphaBlendSurfacePackMult; // for packing/unpacking hitT into Float16 in AlphaBlendSurface

  float wboitEnergyLossCompensation;
  float wboitDepthWeightTuning;
  uint wboitEnabled;

  // DX11_V458: transfer-function exponent applied to the rasterized sky probe
  // before it is used as radiance. 1.0 = unchanged.
  //
  // MUST stay at the END of the struct and MUST stay identical to the copy in
  // src/dxvk/rtx/pass/raytrace_args.h. These are two separate files: the C++
  // side compiles the latter, the shaders compile this one. V456 changed only
  // the C++ copy and inserted mid-struct, so the CPU wrote a shifted layout
  // while the GPU kept reading the old one - which corrupted every field after
  // the insertion point, including alphaBlendSurfacePackMult and the wboit*
  // group just above, and turned working alpha transparencies milky.
  float skyProbeDecodeGamma;

  // DX11_V459: constant subtracted from every sky-probe channel before use,
  // clamped at zero. This is the operator that actually matches the defect.
  //
  // The probe is UNBOUNDED HDR (VK_FORMAT_B10G11R11_UFLOAT_PACK32), so
  // skyProbeDecodeGamma above only crushes values BELOW 1.0 - above it, an
  // exponent EXPANDS: pow(5,2.2)=37, pow(10,2.2)=158. Applied to a sky
  // containing a sun disc that detonated the highlights, which read as violent
  // auto-exposure, environment-sampling fireflies and heavy glancing-angle
  // noise. A black point removes the lifted floor (the star texture's dark
  // background, which glows where raster is black) while leaving bright stars
  // essentially untouched, and cannot amplify anything.
  float skyProbeBlackPoint;

  // DX11_V460: independent gain on the sky probe's LIGHTING contribution.
  //
  // rtx.skyBrightness scales the visible sky (the screen-space matte, sampled
  // in composite.comp.slang) AND the probe (the cubemap that lights the scene)
  // together, so appearance and illumination could not be balanced against each
  // other. This multiplies the probe only: the sky can be left matching raster
  // while the light it casts is tuned separately.
  float skyProbeLightBrightness;

  // DX11_V482: floor under the sky probe's LIGHTING contribution, so night never
  // goes darker than a set level. Appearance is untouched.
  //
  // Kenshi's raster ambient does not come from the sky at all: it is the STATIC
  // mp_irradiance/mp_specularity cubes scaled by
  //   envColour.w *= clamp(sunDirection.y * 5 + 0.2, 0.1, 1.0)
  // and sunDirection is horizon-clamped to y >= 0, so that scalar is 1.0 at
  // midday and lands on exactly 0.2 at sunset and all night. Raster night ambient
  // is a FLAT 20% of daytime ambient that never reaches zero. This bridge derives
  // night ambient from a near-black sky probe instead, so the SHAPE is wrong, not
  // the scale - which is why no single gain suits both day and night.
  //
  // A SCALAR deliberately: HLSL constant-buffer packing will not let a vector
  // straddle a 16-byte boundary, so a float3 can re-pad differently between this
  // file and its C++ twin. That is exactly how V456 corrupted every field after
  // its insertion point. A float cannot straddle, so this is immune.
  //
  // Applied as max() AFTER skyBrightness and skyProbeLightBrightness, so it is
  // expressed in the same final radiance units as the value it floors, and a
  // daylit sky (already above it) is completely unaffected.
  float skyProbeLightFloor;

  // DX11_V483: Kenshi's per-region ambient tint, from ambientmap.png's rgb.
  // Multiplies the sky probe's LIGHTING contribution only, mirroring the game's
  // own `envLight.diffuse *= ambientMult.rgb`.
  //
  // THREE SCALARS, not a float3, for the same reason skyProbeLightFloor is a
  // scalar: HLSL will not let a vector straddle a 16-byte boundary while C++
  // packs it flat, so a float3 can re-pad differently between this file and its
  // twin EVEN IF both declarations are identical. Scalars cannot straddle.
  float kenshiAmbientTintR;
  float kenshiAmbientTintG;
  float kenshiAmbientTintB;

  // DX11_V510: brightness multiplier for Kenshi's billboard particles, applied
  // where the opacity-lighting approximation reads the volumetric radiance cache.
  //
  // AT THE END, and verified to be at the end of the LIVE copies rather than
  // trusted to be. The first attempt anchored on `wboitEnabled`, which is the
  // last field of include/rtx/pass/raytrace_args.h but is followed by seven sky
  // fields in the two copies that are actually compiled - so it landed
  // mid-struct, which is exactly the V456/V457 corruption. Compare the stripped
  // declaration lists of all three copies before believing any anchor here.
  float kenshiParticleLightIntensity;

  // DX11_V540_KENSHI_WETNESS. Kenshi's own weather wetness, already scaled by
  // rtx.dx11.kenshiWetness on the CPU so the shader needs no separate knob.
  // Genuinely global state (rain), read by reflected name off whichever terrain
  // or object draw last carried it.
  float kenshiWetness;
  // World-space water level, compared against the hit's world Y to give the
  // near-water term. Only meaningful when kenshiWetness is being applied.
  float kenshiWaterHeight;
  // DX11_V556: gain on Kenshi's own character gloss, which lives in the diffuse
  // alpha and has no `glossMult` to scale it. Appended after the true last
  // field of BOTH live copies; include/ is the dead one and is left alone.
  float kenshiCharacterGloss;
  // DX11_V557_KENSHI_DUST: Kenshi's biome dust overlay. Global, like wetness -
  // the colour and complete amount vector are properties of the region, published each
  // frame from whichever draw last carried them. The noise texture itself is
  // per-material and rides in the material's tangentTextureIndex.
  float kenshiDustColourR;
  float kenshiDustColourG;
  float kenshiDustColourB;
  float kenshiDustAmountX;
  float kenshiDustAmountY;
  float kenshiDustAmountZ;
  float kenshiDustStrength;

  // DX11_V591_KENSHI_WATER_RIPPLES. Kenshi's water parameters, published the
  // same last-draw-wins way wetness and dust are: one water material exists in
  // the whole game and its constants are per-zone globals.
  //   tile* : world XZ -> the tiling ripple coordinate  (VS `scale`)
  //   map*  : world XZ -> the 0-1 map coordinate        (VS `mapBounds`)
  //   speed : `scale.xy * 5000`, the flow speed multiplier
  //   distortion / invStrength / time : the PS constants of the same name
  // A zero tile scale means no water draw has been seen and the path is inert.
  float kenshiWaterTileScaleX;
  float kenshiWaterTileScaleY;
  float kenshiWaterTileOffsetX;
  float kenshiWaterTileOffsetY;
  float kenshiWaterMapScaleX;
  float kenshiWaterMapScaleY;
  float kenshiWaterMapOffsetX;
  float kenshiWaterMapOffsetY;
  float kenshiWaterSpeedX;
  float kenshiWaterSpeedY;
  float kenshiWaterDistortion;
  float kenshiWaterInvStrength;
  float kenshiWaterTime;
  float kenshiWaterRainAmount;
  // DX11_V599_KENSHI_WATER_SCUM: the scum layer's own tiling coordinate
  // (`scale.zw`, the FCS scum scale) and the amount the live ripple normal
  // distorts it (`distortion.y`). Published only by draws that declare a scum
  // map, i.e. the near-water families - the distant shader has no scum at all.
  float kenshiWaterScumScaleX;
  float kenshiWaterScumScaleY;
  float kenshiWaterScumDistortion;
  // DX11_V621/V627_KENSHI_WATER_TRANSPARENCY. A negative value selects
  // simulated depth and its magnitude scales the game's own depth falloff.
  // Positive values belong to the proper translucent-water material route.
  float kenshiWaterTransparency;
  float kenshiWaterInvOpacity;
  float kenshiWaterPad0;
  float kenshiWaterPad1;
  float kenshiWaterPad2;
  float kenshiWaterGlow;
  // DX11_V632_KENSHI_WATER_COLOUR_GAIN. Kenshi's water diffuse is boosted far
  // above physical and that boost IS the biome colour: lightingFunctions.hlsl
  // multiplies the sun diffuse by PI where a normalised BRDF divides by it, and
  // scales the irradiance probe by 4 (`irradianceCube.rgb * .a * 4.0f`). Since
  // waterColour only enters through `(sunLight.diffuse + envLight.diffuse) *
  // albedo`, a path-traced water surface lit correctly is about an order of
  // magnitude less saturated than the raster one. This reproduces that gain.
  float kenshiWaterColourGain;

  // DX11_V634_KENSHI_RAIN. Raster Basic_Coloured_Ambient_VP sun-height
  // multiplier times the rain-only HDR-to-LDR calibration.
  float kenshiRainEmissionScale;

  // DX11_V638_KENSHI_INTERIOR_CLIP. Kenshi's building-interior cull, generalised
  // from raster's screen-space depth slab to a world-space volume test - see
  // journal chapter 16. Each active mask shell is captured from the InteriorMask
  // compositor draws and published as an oriented box, encoded as a world-to-unit
  // -box transform: a hit is inside when all three components of the transformed
  // position are within [-1, 1]. isSurfaceClipped() rejects such hits.
  //
  // FOUR slots, because Kenshi renders an interior for every building that has a
  // character in it and a town can have several active at once - V637 measured
  // two shells alternating frame to frame, which a single slot cannot express.
  //
  // SCALARS ONLY, never a mat4 or a float4 array, for the reason spelled out on
  // skyProbeLightFloor and kenshiAmbientTint* above: HLSL constant-buffer packing
  // will not let a vector straddle a 16-byte boundary while C++ packs flat, so a
  // vector member can re-pad differently between this file and its twin EVEN IF
  // both declarations are identical. That is how V456 corrupted every field after
  // its insertion point. Scalars cannot straddle. These are generated into both
  // copies by script and diffed, so they cannot drift by transcription either.
  float kenshiInteriorB0R0X;
  float kenshiInteriorB0R0Y;
  float kenshiInteriorB0R0Z;
  float kenshiInteriorB0R0W;
  float kenshiInteriorB0R1X;
  float kenshiInteriorB0R1Y;
  float kenshiInteriorB0R1Z;
  float kenshiInteriorB0R1W;
  float kenshiInteriorB0R2X;
  float kenshiInteriorB0R2Y;
  float kenshiInteriorB0R2Z;
  float kenshiInteriorB0R2W;
  float kenshiInteriorB1R0X;
  float kenshiInteriorB1R0Y;
  float kenshiInteriorB1R0Z;
  float kenshiInteriorB1R0W;
  float kenshiInteriorB1R1X;
  float kenshiInteriorB1R1Y;
  float kenshiInteriorB1R1Z;
  float kenshiInteriorB1R1W;
  float kenshiInteriorB1R2X;
  float kenshiInteriorB1R2Y;
  float kenshiInteriorB1R2Z;
  float kenshiInteriorB1R2W;
  float kenshiInteriorB2R0X;
  float kenshiInteriorB2R0Y;
  float kenshiInteriorB2R0Z;
  float kenshiInteriorB2R0W;
  float kenshiInteriorB2R1X;
  float kenshiInteriorB2R1Y;
  float kenshiInteriorB2R1Z;
  float kenshiInteriorB2R1W;
  float kenshiInteriorB2R2X;
  float kenshiInteriorB2R2Y;
  float kenshiInteriorB2R2Z;
  float kenshiInteriorB2R2W;
  float kenshiInteriorB3R0X;
  float kenshiInteriorB3R0Y;
  float kenshiInteriorB3R0Z;
  float kenshiInteriorB3R0W;
  float kenshiInteriorB3R1X;
  float kenshiInteriorB3R1Y;
  float kenshiInteriorB3R1Z;
  float kenshiInteriorB3R1W;
  float kenshiInteriorB3R2X;
  float kenshiInteriorB3R2Y;
  float kenshiInteriorB3R2Z;
  float kenshiInteriorB3R2W;
  // How many of the four slots are populated. 0 disables the test entirely, and
  // is the state for every frame in which Kenshi drew no mask shell - the only
  // reliable "an interior is active" signal, since the mask TEXTURE is full-size
  // at all times (V635).
  uint kenshiInteriorClipCount;

  // DX11_V639. Diagnostic. 1 = cull EVERY surface marked as interior-clip
  // eligible, ignoring the volumes entirely. That separates the two halves of
  // this feature: if terrain vanishes inside a building under this mode but not
  // under the normal path, the flag and constants are reaching the shader
  // correctly and the fault is the volume placement. If it does not vanish even
  // here, the surface flag never arrived and the placement is irrelevant.
  uint kenshiInteriorClipDebugAll;

  // V794: append-only scalar layout, shared by CPU and GPU.
  uint shadowTerminatorEnableOffset;
  uint shadowTerminatorSoften;
  float shadowTerminatorMaxArea;
  float shadowTerminatorMaxLength;

  // NOTE: Add structs to the top section of RaytraceArgs, not the bottom.
  // NOTE: bool does not work in debug builds, use uint instead.
};

#endif // RTX_PASS_RAYTRACE_ARGS_H_DX11V225
