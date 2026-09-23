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

// Kenshi terrain parameter set. The game blends its ground from detail layers keyed by a splat
// map and multiplies the result by a low-frequency colour map; only that colour map reaches an
// ordinary Remix material. Sets are content-addressed and stable across frames, and a terrain
// material stores its index in secondaryTextureIndex. Several sets are live at once (ground,
// terrain-blended rocks, neighbouring biomes), hence BINDING_KENSHI_TERRAIN_BUFFER.
// The detail coordinate is derived from the interpolated overlay coordinate, since both are
// affine in the same object-space position:
//   objectPos.xz  = uv * rectSize + rectMin
//   detailUv      = objectPos.xz * 0.0002 * layerScale
//                 = uv * detailScale + detailOffset

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
  // Active blendMap channels, bits 0..3 for x..w. Zero on every record that is not a biome-boundary
  // primary, which keeps the selector inert for ordinary terrain and for non-terrain materials aliased
  // into this buffer.
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

  // Transfer-function exponent applied to the rasterized sky probe before it is used as radiance.
  // 1.0 = unchanged.
  // Must stay at the END of the struct and identical to src/dxvk/rtx/pass/raytrace_args.h: the C++ side
  // compiles that copy and the shaders this one, and inserting mid-struct in only one of them shifts the
  // layout of every following field.
  float skyProbeDecodeGamma;

  // Constant subtracted from every sky-probe channel before use, clamped at zero. The probe is unbounded
  // HDR (B10G11R11_UFLOAT), so a decode gamma only crushes values below 1.0 and expands those above it
  // (pow(10, 2.2) = 158), blowing up the sun disc. A black point removes the lifted floor (the star
  // texture's dark background) without amplifying anything.
  float skyProbeBlackPoint;

  // Independent gain on the sky probe's lighting contribution. rtx.skyBrightness scales both the visible
  // sky (the screen-space matte in composite.comp.slang) and the probe that lights the scene; this scales
  // the probe only, so appearance and illumination can be balanced separately.
  float skyProbeLightBrightness;

  // Floor under the sky probe's lighting contribution, so night never goes darker than a set level;
  // appearance is untouched. Kenshi's raster ambient comes from static irradiance/specularity cubes
  // scaled by
  //   envColour.w *= clamp(sunDirection.y * 5 + 0.2, 0.1, 1.0)
  // with sunDirection horizon-clamped, so raster night ambient is a flat 20% of daytime; deriving it
  // from a near-black sky probe gets the shape wrong, not just the scale.
  // A scalar deliberately: a vector can re-pad differently between this file and its C++ twin. Applied
  // as max() after skyBrightness and skyProbeLightBrightness, in final radiance units, so a daylit sky
  // is unaffected.
  float skyProbeLightFloor;

  // Kenshi's per-region ambient tint (ambientmap.png rgb). Multiplies the sky probe's lighting
  // contribution only, mirroring the game's `envLight.diffuse *= ambientMult.rgb`. Three scalars, not a
  // float3: a vector can re-pad differently between this file and its twin even if both declarations
  // are identical.
  float kenshiAmbientTintR;
  float kenshiAmbientTintG;
  float kenshiAmbientTintB;

  // Brightness multiplier for Kenshi's billboard particles, applied where the opacity-lighting
  // approximation reads the volumetric radiance cache.
  // Append-only. The two compiled copies of this struct end with extra sky fields, so diff the
  // declaration lists of all three copies before adding a field.
  float kenshiParticleLightIntensity;

  // Kenshi's weather wetness, pre-scaled by rtx.dx11.kenshiWetness on the CPU. Global state (rain),
  // read by name from whichever terrain or object draw last carried it.
  float kenshiWetness;
  // World-space water level, compared against the hit's world Y to give the
  // near-water term. Only meaningful when kenshiWetness is being applied.
  float kenshiWaterHeight;
  // Gain on Kenshi's character gloss, which lives in the diffuse alpha with no glossMult to scale it.
  // Appended after the true last field of both live copies (include/ is not compiled).
  float kenshiCharacterGloss;
  // Kenshi's biome dust overlay. Global like wetness: colour and amount vector are region properties
  // published each frame from whichever draw last carried them. The noise texture is per material
  // (tangentTextureIndex).
  float kenshiDustColourR;
  float kenshiDustColourG;
  float kenshiDustColourB;
  float kenshiDustAmountX;
  float kenshiDustAmountY;
  float kenshiDustAmountZ;
  float kenshiDustStrength;

  // Kenshi's water parameters, published last-draw-wins like wetness and dust (one water material in
  // the game, per-zone globals):
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
  // The scum layer's own tiling (`scale.zw`) and the amount the live ripple normal distorts it
  // (`distortion.y`). Published only by draws that declare a scum map (the near-water families).
  float kenshiWaterScumScaleX;
  float kenshiWaterScumScaleY;
  float kenshiWaterScumDistortion;
  // A negative value selects simulated depth and its magnitude scales the game's own depth falloff;
  // positive values select the translucent-water material route.
  float kenshiWaterTransparency;
  float kenshiWaterInvOpacity;
  float kenshiWaterPad0;
  float kenshiWaterPad1;
  float kenshiWaterPad2;
  float kenshiWaterGlow;
  // Kenshi's water diffuse is boosted far above physical, and that boost is the biome colour:
  // lightingFunctions.hlsl multiplies the sun diffuse by PI where a normalised BRDF divides by it, and
  // scales the irradiance probe by 4 (`irradianceCube.rgb * .a * 4.0f`). A correctly lit path-traced
  // water surface is about an order of magnitude less saturated; this reproduces the gain.
  float kenshiWaterColourGain;

  // Raster Basic_Coloured_Ambient_VP sun-height multiplier times the rain-only HDR-to-LDR calibration.
  float kenshiRainEmissionScale;

  // Building-interior cull volumes, one oriented box per active mask shell (four slots: several
  // buildings can be active at once), each encoded as a world-to-unit-box transform: a hit is inside
  // when all three transformed components are within [-1, 1].
  // Scalars only, never a mat4 or float4 array: a vector can re-pad differently between this file and
  // its twin. These are generated into both copies by script and diffed.
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
  // How many of the four slots are populated. 0 disables the test, which is the state whenever Kenshi
  // drew no mask shell (the only reliable "interior active" signal).
  uint kenshiInteriorClipCount;

  // Diagnostic: 1 = cull every interior-clip-eligible surface, ignoring the volumes. If terrain vanishes
  // inside a building only in this mode, the flag and constants arrive correctly and the fault is volume
  // placement; if not even here, the surface flag never arrived.
  uint kenshiInteriorClipDebugAll;

  // Append-only scalar layout, shared by CPU and GPU.
  uint shadowTerminatorEnableOffset;
  uint shadowTerminatorSoften;
  float shadowTerminatorMaxArea;
  float shadowTerminatorMaxLength;

  // NOTE: Add structs to the top section of RaytraceArgs, not the bottom.
  // NOTE: bool does not work in debug builds, use uint instead.
};

#endif // RTX_PASS_RAYTRACE_ARGS_H_DX11V225
